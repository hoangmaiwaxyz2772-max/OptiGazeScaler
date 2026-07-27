#include <pch.h>

#include "RayReconstructionPeripheralDenoiser_Dx12.h"

#include <shaders/gaze_roi/GazeRoi_Dx12.h>

#include <algorithm>

namespace
{
constexpr UINT RAY_RECONSTRUCTION_DENOISER_CONSTANT_DWORDS = 31;

static const char* rayReconstructionTemporalShader = R"(
cbuffer Params : register(b0)
{
    uint _SourceWidth;
    uint _SourceHeight;
    uint _ColorBaseX;
    uint _ColorBaseY;
    uint _DepthBaseX;
    uint _DepthBaseY;
    uint _MotionVectorBaseX;
    uint _MotionVectorBaseY;
    uint _MotionVectorWidth;
    uint _MotionVectorHeight;
    uint _OutputWidth;
    uint _OutputHeight;
    uint _OutputBaseX;
    uint _OutputBaseY;
    float _MotionVectorScaleX;
    float _MotionVectorScaleY;
    float _JitterOffsetX;
    float _JitterOffsetY;
    float _PreviousJitterOffsetX;
    float _PreviousJitterOffsetY;
    uint _MotionVectorsLowResolution;
    uint _MotionVectorsJittered;
    uint _ResetHistory;
    uint _HistoryInitialized;
    uint _DebugView;
    uint _MaxHistory;
    uint _SpatialPasses;
    float _SpatialRadius;
    uint _SpatialPassIndex;
    uint _DenoiserWidth;
    uint _DenoiserHeight;
};

Texture2D<float4> CurrentColor : register(t0);
Texture2D<float> CurrentDepth : register(t1);
Texture2D<float4> MotionVectors : register(t2);
Texture2D<float4> PreviousRadiance : register(t3);
Texture2D<float2> PreviousMoments : register(t4);
Texture2D<float> PreviousDepth : register(t5);
RWTexture2D<float4> NextRadiance : register(u0);
RWTexture2D<float2> NextMoments : register(u1);
RWTexture2D<float> NextDepth : register(u2);
RWTexture2D<float4> DebugOutput : register(u3);
SamplerState LinearClampSampler : register(s0);

float Luminance(float3 c)
{
    return max(0.0f, dot(c, float3(0.2126f, 0.7152f, 0.0722f)));
}

float3 SafeColor(float3 color)
{
    return all(isfinite(color)) ? max(color, 0.0f) : float3(0.0f, 0.0f, 0.0f);
}

float3 ClampHistoryToCurrentNeighborhood(uint2 sourceCenter, float3 current, float3 history)
{
    const float currentLogLum = log2(1.0f + Luminance(current));
    float minLogLum = currentLogLum;
    float maxLogLum = currentLogLum;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            uint2 samplePixel = min(uint2(max(int2(sourceCenter) + int2(x, y), 0)),
                                    uint2(_SourceWidth - 1, _SourceHeight - 1));
            float3 sampleColor = SafeColor(CurrentColor.Load(int3(samplePixel + uint2(_ColorBaseX, _ColorBaseY), 0)).rgb);
            float logLum = log2(1.0f + Luminance(sampleColor));
            minLogLum = min(minLogLum, logLum);
            maxLogLum = max(maxLogLum, logLum);
        }
    }

    const float historyLum = Luminance(history);
    const float targetLum = exp2(clamp(log2(1.0f + historyLum), minLogLum, maxLogLum)) - 1.0f;
    return history * (targetLum / max(historyLum, 1e-5f));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    if (p.x >= _DenoiserWidth || p.y >= _DenoiserHeight)
        return;

    const float2 sourceSize = float2(_SourceWidth, _SourceHeight);
    const float2 denoiserSize = float2(_DenoiserWidth, _DenoiserHeight);
    const uint2 sourcePixel = min(uint2((float2(p) + 0.5f) * sourceSize / denoiserSize),
                                  uint2(_SourceWidth - 1, _SourceHeight - 1));
    const float3 current = SafeColor(CurrentColor.Load(int3(sourcePixel + uint2(_ColorBaseX, _ColorBaseY), 0)).rgb);
    const float currentDepth = CurrentDepth.Load(int3(sourcePixel + uint2(_DepthBaseX, _DepthBaseY), 0));

    const float2 mvUv = (float2(sourcePixel) + 0.5f) * float2(_MotionVectorWidth, _MotionVectorHeight) / sourceSize;
    const uint2 mvPixel = min(uint2(mvUv), uint2(_MotionVectorWidth - 1, _MotionVectorHeight - 1));
    float2 motion = MotionVectors.Load(int3(mvPixel + uint2(_MotionVectorBaseX, _MotionVectorBaseY), 0)).xy;
    motion *= float2(_MotionVectorScaleX, _MotionVectorScaleY);
    if (_MotionVectorsLowResolution == 0)
        motion *= sourceSize / float2(_MotionVectorWidth, _MotionVectorHeight);
    if (_MotionVectorsJittered != 0)
        motion += float2(_PreviousJitterOffsetX - _JitterOffsetX, _PreviousJitterOffsetY - _JitterOffsetY);
    motion *= denoiserSize / sourceSize;

    const float2 previousPixel = float2(p) + motion;
    const bool inBounds = all(previousPixel >= 0.0f) && all(previousPixel < denoiserSize);
    const uint2 previousDepthPixel = min(uint2(max(previousPixel + 0.5f, 0.0f)),
                                         uint2(_DenoiserWidth - 1, _DenoiserHeight - 1));
    const float previousDepth = PreviousDepth.Load(int3(previousDepthPixel, 0));

    const float depthGradient = max(abs(currentDepth - CurrentDepth.Load(int3(uint2(min(sourcePixel.x + 1, _SourceWidth - 1), sourcePixel.y) +
                                                                           uint2(_DepthBaseX, _DepthBaseY), 0))),
                                    abs(currentDepth - CurrentDepth.Load(int3(uint2(sourcePixel.x, min(sourcePixel.y + 1, _SourceHeight - 1)) +
                                                                           uint2(_DepthBaseX, _DepthBaseY), 0))));
    const float depthThreshold = max(0.001f, max(abs(currentDepth), abs(previousDepth)) * 0.02f + depthGradient * 2.0f);
    const bool depthMatches = isfinite(currentDepth) && isfinite(previousDepth) &&
                              abs(currentDepth - previousDepth) <= depthThreshold;
    const bool acceptHistory = _HistoryInitialized != 0 && _ResetHistory == 0 && inBounds && depthMatches;

    float4 reprojected = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float2 previousMoments = float2(0.0f, 0.0f);
    if (inBounds)
    {
        const float2 previousUv = (previousPixel + 0.5f) / denoiserSize;
        reprojected = PreviousRadiance.SampleLevel(LinearClampSampler, previousUv, 0.0f);
        previousMoments = PreviousMoments.Load(int3(previousDepthPixel, 0));
    }

    float3 filtered = current;
    float confidence = 1.0f;
    float2 moments = float2(Luminance(current), Luminance(current) * Luminance(current));
    if (acceptHistory)
    {
        const float3 clippedHistory =
            ClampHistoryToCurrentNeighborhood(sourcePixel, current, SafeColor(reprojected.rgb));
        confidence = min(reprojected.a + 1.0f, (float)max(_MaxHistory, 2u));
        const float currentWeight = 1.0f / confidence;
        filtered = lerp(clippedHistory, current, currentWeight);
        const float currentLum = Luminance(current);
        moments = lerp(previousMoments, float2(currentLum, currentLum * currentLum), currentWeight);
    }

    NextRadiance[p] = float4(filtered, confidence);
    NextMoments[p] = moments;
    NextDepth[p] = currentDepth;

    if (_DebugView != 0)
    {
        if (_DebugView == 1)
            DebugOutput[p] = float4(current, 1.0f);
        else if (_DebugView == 2)
            DebugOutput[p] = float4(acceptHistory ? SafeColor(reprojected.rgb) : float3(0.0f, 0.0f, 0.0f), 1.0f);
        else if (_DebugView == 3)
            DebugOutput[p] = acceptHistory ? float4(0.0f, 1.0f, 0.0f, 1.0f) : float4(1.0f, 0.0f, 0.0f, 1.0f);
        else
            DebugOutput[p] = float4(confidence / (float)max(_MaxHistory, 2u),
                                    confidence / (float)max(_MaxHistory, 2u),
                                    confidence / (float)max(_MaxHistory, 2u), 1.0f);
    }
}
)";

static const char* rayReconstructionAtrousShader = R"(
cbuffer Params : register(b0)
{
    uint _SourceWidth;
    uint _SourceHeight;
    uint _ColorBaseX;
    uint _ColorBaseY;
    uint _DepthBaseX;
    uint _DepthBaseY;
    uint _MotionVectorBaseX;
    uint _MotionVectorBaseY;
    uint _MotionVectorWidth;
    uint _MotionVectorHeight;
    uint _OutputWidth;
    uint _OutputHeight;
    uint _OutputBaseX;
    uint _OutputBaseY;
    float _MotionVectorScaleX;
    float _MotionVectorScaleY;
    float _JitterOffsetX;
    float _JitterOffsetY;
    float _PreviousJitterOffsetX;
    float _PreviousJitterOffsetY;
    uint _MotionVectorsLowResolution;
    uint _MotionVectorsJittered;
    uint _ResetHistory;
    uint _HistoryInitialized;
    uint _DebugView;
    uint _MaxHistory;
    uint _SpatialPasses;
    float _SpatialRadius;
    uint _SpatialPassIndex;
    uint _DenoiserWidth;
    uint _DenoiserHeight;
};

Texture2D<float4> InputRadiance : register(t0);
Texture2D<float> CurrentDepth : register(t1);
Texture2D<float2> Moments : register(t2);
RWTexture2D<float4> OutputRadiance : register(u0);

float Luminance(float3 c)
{
    return max(0.0f, dot(c, float3(0.2126f, 0.7152f, 0.0722f)));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    if (p.x >= _DenoiserWidth || p.y >= _DenoiserHeight)
        return;

    const float4 center = InputRadiance.Load(int3(p, 0));
    const float centerDepth = CurrentDepth.Load(int3(p, 0));
    const float2 moments = Moments.Load(int3(p, 0));
    const float variance = max(0.0001f, moments.y - moments.x * moments.x);
    const int stepSize = max(1, (int)round(_SpatialRadius * (float)(1u << min(_SpatialPassIndex, 2u))));

    float3 sum = center.rgb * 4.0f;
    float totalWeight = 4.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;

            const int2 samplePoint = clamp(int2(p) + int2(x, y) * stepSize, int2(0, 0),
                                            int2(_DenoiserWidth - 1, _DenoiserHeight - 1));
            const float4 sampleColor = InputRadiance.Load(int3(samplePoint, 0));
            const float sampleDepth = CurrentDepth.Load(int3(samplePoint, 0));
            const float depthScale = max(0.002f, max(abs(centerDepth), abs(sampleDepth)) * 0.015f);
            const float depthWeight = isfinite(centerDepth) && isfinite(sampleDepth)
                                          ? exp(-abs(centerDepth - sampleDepth) / depthScale)
                                          : 0.0f;
            const float luminanceDelta = Luminance(sampleColor.rgb) - Luminance(center.rgb);
            const float colorWeight = exp(-(luminanceDelta * luminanceDelta) / (variance * 12.0f + 0.03f));
            const float kernelWeight = (x == 0 || y == 0) ? 2.0f : 1.0f;
            const float weight = kernelWeight * depthWeight * colorWeight;
            sum += sampleColor.rgb * weight;
            totalWeight += weight;
        }
    }

    OutputRadiance[p] = float4(sum / max(totalWeight, 1e-4f), center.a);
}
)";

static const char* rayReconstructionOutputShader = R"(
cbuffer Params : register(b0)
{
    uint _SourceWidth;
    uint _SourceHeight;
    uint _ColorBaseX;
    uint _ColorBaseY;
    uint _DepthBaseX;
    uint _DepthBaseY;
    uint _MotionVectorBaseX;
    uint _MotionVectorBaseY;
    uint _MotionVectorWidth;
    uint _MotionVectorHeight;
    uint _OutputWidth;
    uint _OutputHeight;
    uint _OutputBaseX;
    uint _OutputBaseY;
    float _MotionVectorScaleX;
    float _MotionVectorScaleY;
    float _JitterOffsetX;
    float _JitterOffsetY;
    float _PreviousJitterOffsetX;
    float _PreviousJitterOffsetY;
    uint _MotionVectorsLowResolution;
    uint _MotionVectorsJittered;
    uint _ResetHistory;
    uint _HistoryInitialized;
    uint _DebugView;
    uint _MaxHistory;
    uint _SpatialPasses;
    float _SpatialRadius;
    uint _SpatialPassIndex;
    uint _DenoiserWidth;
    uint _DenoiserHeight;
};

Texture2D<float4> FilteredInput : register(t0);
RWTexture2D<float4> Output : register(u0);
SamplerState LinearClampSampler : register(s0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    if (p.x >= _OutputWidth || p.y >= _OutputHeight)
        return;
    const float2 uv = (float2(p) + 0.5f) / float2(_OutputWidth, _OutputHeight);
    Output[p + uint2(_OutputBaseX, _OutputBaseY)] = FilteredInput.SampleLevel(LinearClampSampler, uv, 0.0f);
}
)";

bool MatchesResource(ID3D12Resource* resource, uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    if (resource == nullptr)
        return false;
    const auto desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == width && desc.Height == height &&
           desc.Format == format;
}
} // namespace

RayReconstructionPeripheralDenoiserDx12::RayReconstructionPeripheralDenoiserDx12(std::string name,
                                                                                   ID3D12Device* device)
    : Shader_Dx12(std::move(name), device)
{
    if (device == nullptr)
        return;

    CD3DX12_STATIC_SAMPLER_DESC sampler(0);
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    if (!SetupRootSignatureWithConstants(device, 6, 4, RAY_RECONSTRUCTION_DENOISER_CONSTANT_DWORDS, 0, 1, &sampler))
        return;

    ID3DBlob* temporalBlob = CompileShader(rayReconstructionTemporalShader, "CSMain", "cs_5_0");
    ID3DBlob* outputBlob = CompileShader(rayReconstructionOutputShader, "CSMain", "cs_5_0");
    ID3DBlob* spatialBlob = CompileShader(rayReconstructionAtrousShader, "CSMain", "cs_5_0");
    if (temporalBlob == nullptr || outputBlob == nullptr || spatialBlob == nullptr ||
        !CreateComputeShader(device, _rootSignature, &_pipelineState, temporalBlob, {}) ||
        !CreateComputeShader(device, _rootSignature, &_outputPipelineState, outputBlob, {}) ||
        !CreateComputeShader(device, _rootSignature, &_spatialPipelineState, spatialBlob, {}))
    {
        SAFE_RELEASE(temporalBlob);
        SAFE_RELEASE(outputBlob);
        SAFE_RELEASE(spatialBlob);
        return;
    }
    SAFE_RELEASE(temporalBlob);
    SAFE_RELEASE(outputBlob);
    SAFE_RELEASE(spatialBlob);

    bool heapsCreated = true;
    for (uint32_t slot = 0; slot < RAY_RECONSTRUCTION_DENOISER_FRAME_SLOTS; ++slot)
    {
        heapsCreated = heapsCreated && _temporalFrameHeaps[slot].Initialize(device, 6, 4, 0);
        // The output pass reuses the temporal root signature. Its u0 begins after six SRVs,
        // so this heap must preserve the same descriptor-table layout even though the shader
        // only reads t0 and writes u0.
        heapsCreated = heapsCreated && _outputFrameHeaps[slot].Initialize(device, 6, 4, 0);
        for (uint32_t pass = 0; pass < 3; ++pass)
            heapsCreated = heapsCreated && _spatialFrameHeaps[pass][slot].Initialize(device, 6, 4, 0);
    }
    _init = heapsCreated;
}

RayReconstructionPeripheralDenoiserDx12::~RayReconstructionPeripheralDenoiserDx12()
{
    ReleaseHistoryResources();
    SAFE_RELEASE(_outputPipelineState);
    SAFE_RELEASE(_spatialPipelineState);
    if (State::Instance().isShuttingDown)
        return;
    for (auto& heap : _temporalFrameHeaps)
        heap.ReleaseHeaps();
    for (auto& heap : _outputFrameHeaps)
        heap.ReleaseHeaps();
    for (auto& passHeaps : _spatialFrameHeaps)
        for (auto& heap : passHeaps)
            heap.ReleaseHeaps();
}

void RayReconstructionPeripheralDenoiserDx12::SetResourceState(ID3D12GraphicsCommandList* commandList,
                                                                ID3D12Resource* resource,
                                                                D3D12_RESOURCE_STATES* currentState,
                                                                D3D12_RESOURCE_STATES nextState)
{
    Shader_Dx12::SetBufferState(commandList, nextState, resource, currentState);
}

void RayReconstructionPeripheralDenoiserDx12::ReleaseHistoryResources()
{
    for (uint32_t index = 0; index < 2; ++index)
    {
        GazeRoiFrameSync::DeferRelease(_radianceHistory[index]);
        GazeRoiFrameSync::DeferRelease(_momentsHistory[index]);
        GazeRoiFrameSync::DeferRelease(_depthHistory[index]);
        _radianceHistory[index] = nullptr;
        _momentsHistory[index] = nullptr;
        _depthHistory[index] = nullptr;
    }
    GazeRoiFrameSync::DeferRelease(_debugOutput);
    _debugOutput = nullptr;
    for (auto& scratch : _spatialScratch)
    {
        GazeRoiFrameSync::DeferRelease(scratch);
        scratch = nullptr;
    }
    _historyInitialized = false;
    _filteredOutput = nullptr;
    _historyWidth = 0;
    _historyHeight = 0;
    _spatialScratchCount = 0;
}

bool RayReconstructionPeripheralDenoiserDx12::CreateHistoryResources(uint32_t width, uint32_t height,
                                                                      bool debugOutput, uint32_t spatialPasses)
{
    const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto createTexture = [&](ID3D12Resource** resource, DXGI_FORMAT format, const wchar_t* name)
    {
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0,
                                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const HRESULT result = _device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                 IID_PPV_ARGS(resource));
        if (FAILED(result))
        {
            LOG_ERROR("[DLSSD_DENOISER] CreateCommittedResource {} failed {:X}", _name,
                      static_cast<uint64_t>(result));
            return false;
        }
        (*resource)->SetName(name);
        return true;
    };

    const uint32_t requiredScratchCount = std::min(std::clamp(spatialPasses, 0U, 3U), 2U);
    for (uint32_t index = 0; index < requiredScratchCount; ++index)
    {
        if (!createTexture(&_radianceHistory[index], DXGI_FORMAT_R16G16B16A16_FLOAT,
                           index == 0 ? L"RRDenoiser_Radiance0" : L"RRDenoiser_Radiance1") ||
            !createTexture(&_momentsHistory[index], DXGI_FORMAT_R16G16_FLOAT,
                           index == 0 ? L"RRDenoiser_Moments0" : L"RRDenoiser_Moments1") ||
            !createTexture(&_depthHistory[index], DXGI_FORMAT_R32_FLOAT,
                           index == 0 ? L"RRDenoiser_Depth0" : L"RRDenoiser_Depth1"))
        {
            ReleaseHistoryResources();
            return false;
        }
    }
    for (uint32_t index = 0; index < 2; ++index)
    {
        if (!createTexture(&_spatialScratch[index], DXGI_FORMAT_R16G16B16A16_FLOAT,
                           index == 0 ? L"RRDenoiser_Spatial0" : L"RRDenoiser_Spatial1"))
        {
            ReleaseHistoryResources();
            return false;
        }
    }
    if (debugOutput && !createTexture(&_debugOutput, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RRDenoiser_Debug"))
    {
        ReleaseHistoryResources();
        return false;
    }

    _historyWidth = width;
    _historyHeight = height;
    _spatialScratchCount = requiredScratchCount;
    _debugOutputEnabled = debugOutput;
    _historyIndex = 0;
    _radianceHistoryStates[0] = _radianceHistoryStates[1] = D3D12_RESOURCE_STATE_COMMON;
    _momentsHistoryStates[0] = _momentsHistoryStates[1] = D3D12_RESOURCE_STATE_COMMON;
    _depthHistoryStates[0] = _depthHistoryStates[1] = D3D12_RESOURCE_STATE_COMMON;
    _debugOutputState = D3D12_RESOURCE_STATE_COMMON;
    _spatialScratchStates[0] = _spatialScratchStates[1] = D3D12_RESOURCE_STATE_COMMON;
    LOG_INFO("[DLSSD_DENOISER] created history size={}x{} debugOutput={} spatialScratch={}", width, height,
             debugOutput, requiredScratchCount);
    return true;
}

bool RayReconstructionPeripheralDenoiserDx12::EnsureResources(uint32_t width, uint32_t height, bool debugOutput,
                                                               uint32_t spatialPasses)
{
    if (!_init || width == 0 || height == 0)
        return false;
    const uint32_t requiredScratchCount = std::min(std::clamp(spatialPasses, 0U, 3U), 2U);
    const bool matches = _historyWidth == width && _historyHeight == height &&
                         MatchesResource(_radianceHistory[0], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                         MatchesResource(_radianceHistory[1], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                         MatchesResource(_momentsHistory[0], width, height, DXGI_FORMAT_R16G16_FLOAT) &&
                         MatchesResource(_momentsHistory[1], width, height, DXGI_FORMAT_R16G16_FLOAT) &&
                         MatchesResource(_depthHistory[0], width, height, DXGI_FORMAT_R32_FLOAT) &&
                         MatchesResource(_depthHistory[1], width, height, DXGI_FORMAT_R32_FLOAT) &&
                          _spatialScratchCount == requiredScratchCount &&
                          (requiredScratchCount < 1 ||
                           MatchesResource(_spatialScratch[0], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT)) &&
                          (requiredScratchCount < 2 ||
                           MatchesResource(_spatialScratch[1], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT)) &&
                         _debugOutputEnabled == debugOutput &&
                         (!debugOutput || MatchesResource(_debugOutput, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT));
    if (matches)
        return true;

    ReleaseHistoryResources();
    return CreateHistoryResources(width, height, debugOutput, spatialPasses);
}

void RayReconstructionPeripheralDenoiserDx12::ResetHistory()
{
    _historyInitialized = false;
}

bool RayReconstructionPeripheralDenoiserDx12::Dispatch(ID3D12GraphicsCommandList* commandList,
                                                        ID3D12Resource* color, ID3D12Resource* depth,
                                                        ID3D12Resource* motionVectors, ID3D12Resource* output,
                                                        const RayReconstructionDenoiserConstants& constants,
                                                        uint32_t frameSlot)
{
    if (!_init || commandList == nullptr || color == nullptr || depth == nullptr || motionVectors == nullptr ||
        frameSlot >= RAY_RECONSTRUCTION_DENOISER_FRAME_SLOTS || constants.sourceWidth == 0 ||
        constants.sourceHeight == 0 || constants.motionVectorWidth == 0 || constants.motionVectorHeight == 0 ||
        constants.denoiserWidth == 0 || constants.denoiserHeight == 0 ||
        (output != nullptr && (constants.outputWidth == 0 || constants.outputHeight == 0)))
        return false;

    const auto fits = [](ID3D12Resource* resource, uint32_t baseX, uint32_t baseY, uint32_t width, uint32_t height)
    {
        const auto desc = resource->GetDesc();
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 &&
               (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 &&
               static_cast<uint64_t>(baseX) + width <= desc.Width &&
               static_cast<uint64_t>(baseY) + height <= desc.Height;
    };
    if (!fits(color, constants.colorBaseX, constants.colorBaseY, constants.sourceWidth, constants.sourceHeight) ||
        !fits(depth, constants.depthBaseX, constants.depthBaseY, constants.sourceWidth, constants.sourceHeight) ||
        !fits(motionVectors, constants.motionVectorBaseX, constants.motionVectorBaseY, constants.motionVectorWidth,
              constants.motionVectorHeight) ||
        (output != nullptr && (!fits(output, constants.outputBaseX, constants.outputBaseY, constants.outputWidth,
                                     constants.outputHeight) ||
                               (output->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)))
    {
        LOG_ERROR("[DLSSD_DENOISER] resource contract invalid");
        ResetHistory();
        return false;
    }

    RayReconstructionDenoiserConstants effectiveConstants = constants;
    effectiveConstants.historyInitialized = _historyInitialized ? 1U : 0U;
    effectiveConstants.maxHistory = std::clamp(effectiveConstants.maxHistory, 2U, 32U);
    effectiveConstants.spatialPasses = std::clamp(effectiveConstants.spatialPasses, 0U, 3U);
    effectiveConstants.spatialRadius = std::clamp(effectiveConstants.spatialRadius, 0.5f, 3.0f);

    const uint32_t previousIndex = _historyIndex;
    const uint32_t nextIndex = (previousIndex + 1) % 2;
    const bool showDebug =
        effectiveConstants.debugView != static_cast<uint32_t>(RayReconstructionDenoiserDebugView::Filtered);
    if (showDebug != _debugOutputEnabled || _radianceHistory[0] == nullptr || _debugOutputEnabled && _debugOutput == nullptr)
        return false;

    SetResourceState(commandList, _radianceHistory[previousIndex], &_radianceHistoryStates[previousIndex],
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    SetResourceState(commandList, _momentsHistory[previousIndex], &_momentsHistoryStates[previousIndex],
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    SetResourceState(commandList, _depthHistory[previousIndex], &_depthHistoryStates[previousIndex],
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    SetResourceState(commandList, _radianceHistory[nextIndex], &_radianceHistoryStates[nextIndex],
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SetResourceState(commandList, _momentsHistory[nextIndex], &_momentsHistoryStates[nextIndex],
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SetResourceState(commandList, _depthHistory[nextIndex], &_depthHistoryStates[nextIndex],
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (showDebug)
        SetResourceState(commandList, _debugOutput, &_debugOutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    FrameDescriptorHeap& temporalHeap = _temporalFrameHeaps[frameSlot];
    try
    {
        CreateShaderResourceView(_device, color, temporalHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, depth, temporalHeap.GetSrvCPU(1));
        CreateShaderResourceView(_device, motionVectors, temporalHeap.GetSrvCPU(2));
        CreateShaderResourceView(_device, _radianceHistory[previousIndex], temporalHeap.GetSrvCPU(3));
        CreateShaderResourceView(_device, _momentsHistory[previousIndex], temporalHeap.GetSrvCPU(4));
        CreateShaderResourceView(_device, _depthHistory[previousIndex], temporalHeap.GetSrvCPU(5));
        CreateUnorderedAccessView(_device, _radianceHistory[nextIndex], temporalHeap.GetUavCPU(0), 0);
        CreateUnorderedAccessView(_device, _momentsHistory[nextIndex], temporalHeap.GetUavCPU(1), 0);
        CreateUnorderedAccessView(_device, _depthHistory[nextIndex], temporalHeap.GetUavCPU(2), 0);
        CreateUnorderedAccessView(_device, showDebug ? _debugOutput : _radianceHistory[nextIndex],
                                  temporalHeap.GetUavCPU(3), 0);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[DLSSD_DENOISER] descriptor creation failed: {}", e.what());
        ResetHistory();
        return false;
    }

    ID3D12DescriptorHeap* temporalHeaps[] = { temporalHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(temporalHeaps), temporalHeaps);
    commandList->SetComputeRootSignature(_rootSignature);
    commandList->SetPipelineState(_pipelineState);
    commandList->SetComputeRootDescriptorTable(0, temporalHeap.GetTableGPUStart());
    commandList->SetComputeRoot32BitConstants(1, RAY_RECONSTRUCTION_DENOISER_CONSTANT_DWORDS, &effectiveConstants,
                                              0);
    commandList->Dispatch((effectiveConstants.denoiserWidth + _numThreadsX - 1) / _numThreadsX,
                          (effectiveConstants.denoiserHeight + _numThreadsY - 1) / _numThreadsY, 1);

    SetResourceState(commandList, _radianceHistory[nextIndex], &_radianceHistoryStates[nextIndex],
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* filteredForOutput = _radianceHistory[nextIndex];
    if (!showDebug && effectiveConstants.spatialPasses != 0)
    {
        SetResourceState(commandList, _momentsHistory[nextIndex], &_momentsHistoryStates[nextIndex],
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        SetResourceState(commandList, _depthHistory[nextIndex], &_depthHistoryStates[nextIndex],
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        for (uint32_t pass = 0; pass < effectiveConstants.spatialPasses; ++pass)
        {
            ID3D12Resource* target = _spatialScratch[pass % 2];
            SetResourceState(commandList, target, &_spatialScratchStates[pass % 2],
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            FrameDescriptorHeap& spatialHeap = _spatialFrameHeaps[pass][frameSlot];
            try
            {
                CreateShaderResourceView(_device, filteredForOutput, spatialHeap.GetSrvCPU(0));
                CreateShaderResourceView(_device, _depthHistory[nextIndex], spatialHeap.GetSrvCPU(1));
                CreateShaderResourceView(_device, _momentsHistory[nextIndex], spatialHeap.GetSrvCPU(2));
                CreateShaderResourceView(_device, filteredForOutput, spatialHeap.GetSrvCPU(3));
                CreateShaderResourceView(_device, filteredForOutput, spatialHeap.GetSrvCPU(4));
                CreateShaderResourceView(_device, filteredForOutput, spatialHeap.GetSrvCPU(5));
                CreateUnorderedAccessView(_device, target, spatialHeap.GetUavCPU(0), 0);
                CreateUnorderedAccessView(_device, target, spatialHeap.GetUavCPU(1), 0);
                CreateUnorderedAccessView(_device, target, spatialHeap.GetUavCPU(2), 0);
                CreateUnorderedAccessView(_device, target, spatialHeap.GetUavCPU(3), 0);
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("[DLSSD_DENOISER] spatial descriptor creation failed: {}", e.what());
                ResetHistory();
                return false;
            }

            ID3D12DescriptorHeap* spatialHeaps[] = { spatialHeap.GetHeapCSU() };
            effectiveConstants.spatialPassIndex = pass;
            commandList->SetDescriptorHeaps(_countof(spatialHeaps), spatialHeaps);
            commandList->SetComputeRootSignature(_rootSignature);
            commandList->SetPipelineState(_spatialPipelineState);
            commandList->SetComputeRootDescriptorTable(0, spatialHeap.GetTableGPUStart());
            commandList->SetComputeRoot32BitConstants(1, RAY_RECONSTRUCTION_DENOISER_CONSTANT_DWORDS,
                                                      &effectiveConstants, 0);
            commandList->Dispatch((effectiveConstants.denoiserWidth + _numThreadsX - 1) / _numThreadsX,
                                  (effectiveConstants.denoiserHeight + _numThreadsY - 1) / _numThreadsY, 1);
            SetResourceState(commandList, target, &_spatialScratchStates[pass % 2],
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            filteredForOutput = target;
        }
    }
    if (showDebug)
        SetResourceState(commandList, _debugOutput, &_debugOutputState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    _filteredOutput = showDebug ? _debugOutput : filteredForOutput;

    if (output == nullptr)
    {
        _historyIndex = nextIndex;
        _historyInitialized = true;
        return true;
    }

    FrameDescriptorHeap& outputHeap = _outputFrameHeaps[frameSlot];
    try
    {
        CreateShaderResourceView(_device, _filteredOutput, outputHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, _filteredOutput, outputHeap.GetSrvCPU(1));
        CreateShaderResourceView(_device, _filteredOutput, outputHeap.GetSrvCPU(2));
        CreateShaderResourceView(_device, _filteredOutput, outputHeap.GetSrvCPU(3));
        CreateShaderResourceView(_device, _filteredOutput, outputHeap.GetSrvCPU(4));
        CreateShaderResourceView(_device, _filteredOutput, outputHeap.GetSrvCPU(5));
        CreateUnorderedAccessView(_device, output, outputHeap.GetUavCPU(0), 0);
        CreateUnorderedAccessView(_device, output, outputHeap.GetUavCPU(1), 0);
        CreateUnorderedAccessView(_device, output, outputHeap.GetUavCPU(2), 0);
        CreateUnorderedAccessView(_device, output, outputHeap.GetUavCPU(3), 0);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[DLSSD_DENOISER] output descriptor creation failed: {}", e.what());
        ResetHistory();
        return false;
    }

    ID3D12DescriptorHeap* outputHeaps[] = { outputHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(outputHeaps), outputHeaps);
    commandList->SetComputeRootSignature(_rootSignature);
    commandList->SetPipelineState(_outputPipelineState);
    commandList->SetComputeRootDescriptorTable(0, outputHeap.GetTableGPUStart());
    commandList->SetComputeRoot32BitConstants(1, RAY_RECONSTRUCTION_DENOISER_CONSTANT_DWORDS, &effectiveConstants,
                                              0);
    commandList->Dispatch((effectiveConstants.outputWidth + _numThreadsX - 1) / _numThreadsX,
                          (effectiveConstants.outputHeight + _numThreadsY - 1) / _numThreadsY, 1);
    const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(output);
    commandList->ResourceBarrier(1, &barrier);

    _historyIndex = nextIndex;
    _historyInitialized = true;
    return true;
}
