#include "pch.h"

#include "FSRFG_Dx12.h"
#include <State.h>

#include <hudfix/Hudfix_Dx12.h>
#include <menu/menu_overlay_dx.h>
#include <shaders/Shader_Dx12Utils.h>

#include <magic_enum.hpp>

#define FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_MAJOR 3
#define FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_MINOR 1
#define FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_PATCH 6

#define FFX_FRAMEGENERATION_SWAPCHAIN_DX12_MAKE_VERSION(major, minor, patch)                                           \
    (((major) << 22) | ((minor) << 12) | (patch))

#define FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION                                                                     \
    FFX_FRAMEGENERATION_SWAPCHAIN_DX12_MAKE_VERSION(FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_MAJOR,                  \
                                                    FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_MINOR,                  \
                                                    FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_PATCH)

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12 0x3000bu
struct ffxCreateContextDescFrameGenerationSwapChainVersionDX12
{
    ffxCreateContextDescHeader header; ///< Description header for frame generation swapchain version context creation.
    uint32_t version;                  ///< The API version the application was built against. This must be set to
                                       ///< FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION.
};

static D3D12_RESOURCE_STATES GetD3D12State(FfxApiResourceState state)
{
    switch (state)
    {
    case FFX_API_RESOURCE_STATE_COMMON:
        return D3D12_RESOURCE_STATE_COMMON;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_READ:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    case FFX_API_RESOURCE_STATE_COPY_SRC:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_API_RESOURCE_STATE_GENERIC_READ:
        return D3D12_RESOURCE_STATE_GENERIC_READ;
    case FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT:
        return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    default:
        return D3D12_RESOURCE_STATE_COMMON;
    }
}

inline static int GetFormatGroup(DXGI_FORMAT format)
{
    switch (format)
    {

    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 1;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 2;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
        return 3;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return 4;

    case DXGI_FORMAT_R11G11B10_FLOAT:
        return 5;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
        return 6;

    case DXGI_FORMAT_B5G6R5_UNORM:
        return 7;

    case DXGI_FORMAT_B5G5R5A1_UNORM:
        return 8;

    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return 9;

    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        return 10;

    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return 11;

    default:
        return -1;
    }
}

inline static bool CompareResourceFormats(DXGI_FORMAT sc, DXGI_FORMAT hudless)
{
    if (sc == hudless)
        return true;

    auto scGroup = GetFormatGroup(sc);
    auto hudlessGroup = GetFormatGroup(hudless);
    return scGroup == hudlessGroup;
}

static float GetHudDetectionThreshold()
{
    float threshold = 0.03f;
    const bool isCyberpunk = State::Instance().gameQuirks[GameQuirk::CyberpunkHudlessState];
    if (isCyberpunk && State::Instance().activeFgInput != FGInput::FSRFG)
        threshold = 0.01f;

    if (Config::Instance()->FGHudCutoff.value_or_default() > 0.0f)
        threshold = Config::Instance()->FGHudCutoff.value_or_default() / 10.0f;
    return threshold;
}

static inline void ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                   D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState)
{
    if (InBeforeState == InAfterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = InResource;
    barrier.Transition.StateBefore = InBeforeState;
    barrier.Transition.StateAfter = InAfterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    InCommandList->ResourceBarrier(1, &barrier);
}

static const char* fsrFgBackgroundMvPyramidShader = R"(
cbuffer Params : register(b0)
{
    uint _FieldWidth;
    uint _FieldHeight;
    uint _DisplayWidth;
    uint _DisplayHeight;
    uint _MvWidth;
    uint _MvHeight;
    uint _DepthWidth;
    uint _DepthHeight;
    float _MvScaleX;
    float _MvScaleY;
    uint _MvLowResolution;
    float _DepthThreshold;
    uint _Mode;
    uint _Step;
};

Texture2D<float4> MotionVectors : register(t0);
Texture2D<float> CurrentDepth : register(t1);
Texture2D<float> PreviousDepth : register(t2);
Texture2D<float4> InputField : register(t3);
RWTexture2D<float4> OutputField : register(u0);

float2 DisplaySize()
{
    return float2((float)_DisplayWidth, (float)_DisplayHeight);
}

float2 FieldSize()
{
    return float2((float)_FieldWidth, (float)_FieldHeight);
}

float2 LoadMotion(float2 displayPosition)
{
    const float2 displaySize = DisplaySize();
    const float2 mvSize = float2(max(_MvWidth, 1u), max(_MvHeight, 1u));
    const uint2 pixel = _MvLowResolution != 0
                            ? min(uint2(displayPosition * mvSize / displaySize),
                                  uint2(_MvWidth - 1, _MvHeight - 1))
                            : min(uint2(displayPosition), uint2(_MvWidth - 1, _MvHeight - 1));
    float2 motion = MotionVectors.Load(int3(pixel, 0)).xy;
    motion *= float2(_MvScaleX, _MvScaleY);
    motion *= displaySize / (_MvLowResolution != 0 ? displaySize : mvSize);
    return motion;
}

float LoadDepth(Texture2D<float> depth, float2 displayPosition)
{
    const float2 displaySize = DisplaySize();
    const float2 depthSize = float2((float)max(_DepthWidth, 1u), (float)max(_DepthHeight, 1u));
    const uint2 pixel = min(uint2(displayPosition * depthSize / displaySize),
                            uint2(_DepthWidth - 1, _DepthHeight - 1));
    return depth.Load(int3(pixel, 0));
}

bool DepthCompatible(float a, float b)
{
    const float limit = max(_DepthThreshold, max(abs(a), abs(b)) * 0.02f);
    return all(isfinite(float2(a, b))) && abs(a - b) <= limit;
}

bool ValidMotion(float2 motion)
{
    return all(isfinite(motion)) && all(abs(motion) <= DisplaySize() * 0.5f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 cell = id.xy;
    if (cell.x >= _FieldWidth || cell.y >= _FieldHeight)
        return;

    const float2 displaySize = DisplaySize();
    const float2 fieldSize = FieldSize();
    const float2 displayPosition = (float2(cell) + 0.5f) * displaySize / fieldSize;

    if (_Mode == 0)
    {
        float4 best = float4(0.0f, 0.0f, -1.0f, -1.0f);
        float bestDistance = 1e30f;
        [unroll]
        for (uint sy = 0; sy < 4; ++sy)
        {
            [unroll]
            for (uint sx = 0; sx < 4; ++sx)
            {
                const float2 samplePosition = (float2(cell) + (float2(sx, sy) + 0.5f) * 0.25f) *
                                               displaySize / fieldSize;
                const float2 motion = LoadMotion(samplePosition);
                if (!ValidMotion(motion))
                    continue;

                const float currentDepth = LoadDepth(CurrentDepth, samplePosition);
                const float previousDepth = LoadDepth(PreviousDepth, samplePosition + motion);
                if (!DepthCompatible(currentDepth, previousDepth))
                    continue;

                const float distanceToCell = dot(samplePosition - displayPosition,
                                                 samplePosition - displayPosition);
                if (distanceToCell < bestDistance)
                {
                    best = float4(motion, float2(cell) + 0.5f);
                    bestDistance = distanceToCell;
                }
            }
        }
        OutputField[cell] = best;
        return;
    }

    const float2 targetMotion = LoadMotion(displayPosition);
    const float targetPreviousDepth = LoadDepth(PreviousDepth, displayPosition + targetMotion);
    float4 best = float4(0.0f, 0.0f, -1.0f, -1.0f);
    float bestDistance = 1e30f;
    const int step = max((int)_Step, 1);

    [unroll]
    for (int oy = -1; oy <= 1; ++oy)
    {
        [unroll]
        for (int ox = -1; ox <= 1; ++ox)
        {
            const int2 candidateCell = clamp(int2(cell) + int2(ox, oy) * step,
                                             int2(0, 0), int2((int)_FieldWidth - 1, (int)_FieldHeight - 1));
            const float4 candidate = InputField.Load(int3(candidateCell, 0));
            if (candidate.z < 0.0f || candidate.w < 0.0f || !ValidMotion(candidate.xy))
                continue;

            const float2 seedDisplayPosition = candidate.zw * displaySize / fieldSize;
            const float candidatePreviousDepth =
                LoadDepth(PreviousDepth, seedDisplayPosition + candidate.xy);
            if (!DepthCompatible(targetPreviousDepth, candidatePreviousDepth))
                continue;

            const float2 delta = candidate.zw - (float2(cell) + 0.5f);
            const float distanceToSeed = dot(delta, delta);
            if (distanceToSeed < bestDistance)
            {
                best = candidate;
                bestDistance = distanceToSeed;
            }
        }
    }
    OutputField[cell] = best;
}
)";

class FsrFgBackgroundMvPyramid final : public Shader_Dx12
{
    static constexpr UINT FieldScale = 8;
    static constexpr UINT MaxPasses = 16;

    struct Constants
    {
        UINT fieldWidth = 0;
        UINT fieldHeight = 0;
        UINT displayWidth = 0;
        UINT displayHeight = 0;
        UINT mvWidth = 0;
        UINT mvHeight = 0;
        UINT depthWidth = 0;
        UINT depthHeight = 0;
        float mvScaleX = 1.0f;
        float mvScaleY = 1.0f;
        UINT mvLowResolution = 0;
        float depthThreshold = 0.002f;
        UINT mode = 0;
        UINT step = 1;
    };

    static constexpr UINT ConstantDwords = sizeof(Constants) / sizeof(UINT);
    static_assert(sizeof(Constants) % sizeof(UINT) == 0);

    FrameDescriptorHeap _heaps[BUFFER_COUNT][MaxPasses];
    ID3D12Resource* _fields[BUFFER_COUNT][2] {};
    D3D12_RESOURCE_STATES _fieldStates[BUFFER_COUNT][2] {};

    static void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        if (commandList == nullptr || resource == nullptr || before == after)
            return;
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);
    }

    bool EnsureField(UINT slot, UINT index, ID3D12Resource* source, UINT width, UINT height)
    {
        if (source == nullptr || slot >= BUFFER_COUNT || index >= 2)
            return false;

        bool recreated = false;
        if (_fields[slot][index] != nullptr)
        {
            const auto desc = _fields[slot][index]->GetDesc();
            recreated = desc.Width != width || desc.Height != height || desc.Format != DXGI_FORMAT_R32G32B32A32_FLOAT;
            if (recreated)
            {
                SAFE_RELEASE(_fields[slot][index]);
                _fieldStates[slot][index] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
        }

        if (!CreateBufferResource(_device, source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  &_fields[slot][index], D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, width, height,
                                  DXGI_FORMAT_R32G32B32A32_FLOAT))
            return false;

        if (recreated || _fieldStates[slot][index] == D3D12_RESOURCE_STATE_COMMON)
            _fieldStates[slot][index] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return true;
    }

    bool DispatchPass(ID3D12GraphicsCommandList* commandList, UINT slot, UINT pass, ID3D12Resource* motionVectors,
                      ID3D12Resource* currentDepth, ID3D12Resource* previousDepth, ID3D12Resource* inputField,
                      ID3D12Resource* outputField, D3D12_RESOURCE_STATES motionState,
                      D3D12_RESOURCE_STATES currentDepthState, D3D12_RESOURCE_STATES previousDepthState,
                      UINT fieldWidth, UINT fieldHeight, UINT displayWidth, UINT displayHeight, UINT mvWidth,
                      UINT mvHeight, UINT depthWidth, UINT depthHeight, float mvScaleX, float mvScaleY,
                      bool lowResolution, float depthThreshold, UINT mode, UINT step)
    {
        if (commandList == nullptr || slot >= BUFFER_COUNT || pass >= MaxPasses || outputField == nullptr)
            return false;
        auto& heap = _heaps[slot][pass];
        if (heap.GetHeapCSU() == nullptr && !heap.Initialize(_device, 4, 1, 0))
            return false;

        try
        {
            CreateShaderResourceView(_device, motionVectors, heap.GetSrvCPU(0));
            CreateShaderResourceView(_device, currentDepth, heap.GetSrvCPU(1));
            CreateShaderResourceView(_device, previousDepth, heap.GetSrvCPU(2));
            CreateShaderResourceView(_device, inputField != nullptr ? inputField : currentDepth, heap.GetSrvCPU(3));
            CreateUnorderedAccessView(_device, outputField, heap.GetUavCPU(0), 0);
        }
        catch (const std::exception& error)
        {
            LOG_WARN("[FSRFG_BackgroundMvPyramid] Descriptor setup failed: {}", error.what());
            return false;
        }

        Constants constants {};
        constants.fieldWidth = fieldWidth;
        constants.fieldHeight = fieldHeight;
        constants.displayWidth = displayWidth;
        constants.displayHeight = displayHeight;
        constants.mvWidth = mvWidth;
        constants.mvHeight = mvHeight;
        constants.depthWidth = depthWidth;
        constants.depthHeight = depthHeight;
        constants.mvScaleX = std::isfinite(mvScaleX) ? mvScaleX : 1.0f;
        constants.mvScaleY = std::isfinite(mvScaleY) ? mvScaleY : 1.0f;
        constants.mvLowResolution = lowResolution ? 1u : 0u;
        constants.depthThreshold = std::isfinite(depthThreshold) && depthThreshold > 0.0f ? depthThreshold : 0.002f;
        constants.mode = mode;
        constants.step = step;

        Transition(commandList, motionVectors, motionState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, currentDepth, currentDepthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, previousDepth, previousDepthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, outputField, _fieldStates[slot][outputField == _fields[slot][0] ? 0 : 1],
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12DescriptorHeap* descriptorHeaps[] = { heap.GetHeapCSU() };
        commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
        commandList->SetComputeRootSignature(_rootSignature);
        commandList->SetPipelineState(_pipelineState);
        commandList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
        commandList->SetComputeRoot32BitConstants(1, ConstantDwords, &constants, 0);
        commandList->Dispatch((fieldWidth + 7) / 8, (fieldHeight + 7) / 8, 1);

        Transition(commandList, outputField, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        _fieldStates[slot][outputField == _fields[slot][0] ? 0 : 1] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Transition(commandList, motionVectors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, motionState);
        Transition(commandList, currentDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, currentDepthState);
        Transition(commandList, previousDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, previousDepthState);
        return true;
    }

  public:
    explicit FsrFgBackgroundMvPyramid(ID3D12Device* device) : Shader_Dx12("FSRFG_BackgroundMvPyramid", device)
    {
        if (device == nullptr || !SetupRootSignatureWithConstants(device, 4, 1, ConstantDwords))
            return;
        ID3DBlob* blob = CompileShader(fsrFgBackgroundMvPyramidShader, "CSMain", "cs_5_0");
        if (blob == nullptr || !CreateComputeShader(device, _rootSignature, &_pipelineState, blob,
                                                     D3D12_SHADER_BYTECODE {}))
        {
            SAFE_RELEASE(blob);
            return;
        }
        SAFE_RELEASE(blob);
        _init = true;
    }

    ~FsrFgBackgroundMvPyramid()
    {
        for (size_t slot = 0; slot < BUFFER_COUNT; ++slot)
            for (size_t field = 0; field < 2; ++field)
                SAFE_RELEASE(_fields[slot][field]);
    }

    ID3D12Resource* Build(ID3D12GraphicsCommandList* commandList, UINT slot, ID3D12Resource* motionVectors,
                          ID3D12Resource* currentDepth, ID3D12Resource* previousDepth,
                          D3D12_RESOURCE_STATES motionState, D3D12_RESOURCE_STATES currentDepthState,
                          D3D12_RESOURCE_STATES previousDepthState, UINT displayWidth, UINT displayHeight,
                          float mvScaleX, float mvScaleY, bool lowResolution, float depthThreshold, UINT* outputWidth,
                          UINT* outputHeight)
    {
        if (!_init || commandList == nullptr || slot >= BUFFER_COUNT || motionVectors == nullptr ||
            currentDepth == nullptr || previousDepth == nullptr || displayWidth == 0 || displayHeight == 0)
            return nullptr;

        const UINT fieldWidth = std::max<UINT>(1, (displayWidth + FieldScale - 1) / FieldScale);
        const UINT fieldHeight = std::max<UINT>(1, (displayHeight + FieldScale - 1) / FieldScale);
        if (!EnsureField(slot, 0, motionVectors, fieldWidth, fieldHeight) ||
            !EnsureField(slot, 1, motionVectors, fieldWidth, fieldHeight))
            return nullptr;

        const auto mvDesc = motionVectors->GetDesc();
        const auto depthDesc = currentDepth->GetDesc();
        const auto previousDepthDesc = previousDepth->GetDesc();
        if (depthDesc.Width != previousDepthDesc.Width || depthDesc.Height != previousDepthDesc.Height)
            return nullptr;

        if (!DispatchPass(commandList, slot, 0, motionVectors, currentDepth, previousDepth, nullptr,
                          _fields[slot][0], motionState, currentDepthState, previousDepthState, fieldWidth, fieldHeight,
                          displayWidth, displayHeight, static_cast<UINT>(mvDesc.Width), mvDesc.Height,
                          static_cast<UINT>(depthDesc.Width), depthDesc.Height, mvScaleX, mvScaleY, lowResolution,
                          depthThreshold, 0, 1))
            return nullptr;

        UINT maxDimension = std::max(fieldWidth, fieldHeight);
        UINT step = 1;
        while (step < maxDimension)
            step <<= 1;
        step >>= 1;

        UINT inputIndex = 0;
        UINT pass = 1;
        while (step != 0)
        {
            const UINT outputIndex = 1 - inputIndex;
            if (!DispatchPass(commandList, slot, pass++, motionVectors, currentDepth, previousDepth,
                              _fields[slot][inputIndex], _fields[slot][outputIndex], motionState, currentDepthState,
                              previousDepthState, fieldWidth, fieldHeight, displayWidth, displayHeight,
                              static_cast<UINT>(mvDesc.Width), mvDesc.Height, static_cast<UINT>(depthDesc.Width),
                              depthDesc.Height, mvScaleX, mvScaleY, lowResolution, depthThreshold, 1, step))
                return nullptr;
            inputIndex = outputIndex;
            step >>= 1;
        }

        if (outputWidth != nullptr)
            *outputWidth = fieldWidth;
        if (outputHeight != nullptr)
            *outputHeight = fieldHeight;
        return _fields[slot][inputIndex];
    }
};

// Minimal peripheral baseline: one current/previous color pair and one game MV
// sample per output pixel. The FSRFG real-frame ring supplies the previous source.
static const char* fsrFgPeripheralReprojectionShader = R"(
cbuffer Params : register(b0)
{
    uint _Width;
    uint _Height;
    uint _RoiLeft;
    uint _RoiTop;
    uint _RoiWidth;
    uint _RoiHeight;
    uint _MvWidth;
    uint _MvHeight;
    float _MvScaleX;
    float _MvScaleY;
    uint _MvLowResolution;
    float _HudThreshold;
    uint _DepthWidth;
    uint _DepthHeight;
    float _DepthThreshold;
    uint _DepthInverted;
    uint _ConservativeDepthFallback;
    uint _BackgroundFieldWidth;
    uint _BackgroundFieldHeight;
    uint _BackgroundFieldValid;
};

Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> CurrentHudless : register(t1);
Texture2D<float4> PreviousHudless : register(t2);
Texture2D<float2> MotionVectors : register(t3);
Texture2D<float> CurrentDepth : register(t4);
Texture2D<float> PreviousDepth : register(t5);
Texture2D<float4> BackgroundMotionField : register(t6);
RWTexture2D<float4> OutputColor : register(u0);

float MinDividedByMax(float v0, float v1)
{
    const float m = max(v0, v1);
    return m != 0.0f ? min(v0, v1) / m : 0.0f;
}

float CalculateStaticContentFactor(float3 currentInterpolationSource, float3 presentColor)
{
    const float3 factor = saturate(float3(
        saturate((1.0f - MinDividedByMax(currentInterpolationSource.r, presentColor.r)) / 0.1f),
        saturate((1.0f - MinDividedByMax(currentInterpolationSource.g, presentColor.g)) / 0.1f),
        saturate((1.0f - MinDividedByMax(currentInterpolationSource.b, presentColor.b)) / 0.1f)));
    return max(factor.x, max(factor.y, factor.z));
}

int2 ClampPixel(int2 p)
{
    return clamp(p, int2(0, 0), int2((int)_Width - 1, (int)_Height - 1));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    if (p.x >= _Width || p.y >= _Height)
        return;

    if (p.x >= _RoiLeft && p.y >= _RoiTop && p.x < _RoiLeft + _RoiWidth && p.y < _RoiTop + _RoiHeight)
        return;

    const float4 present = CurrentColor.Load(int3(p, 0));
    const float2 displaySize = float2((float)_Width, (float)_Height);
    const float2 mvSize = float2(max(_MvWidth, 1u), max(_MvHeight, 1u));
    const float2 fp = float2(p) + 0.5f;
    const uint2 mvPixel = _MvLowResolution != 0
                              ? min(uint2(fp * mvSize / displaySize), uint2(_MvWidth - 1, _MvHeight - 1))
                              : min(p, uint2(_MvWidth - 1, _MvHeight - 1));
    float2 motion = MotionVectors.Load(int3(mvPixel, 0)).xy;
    motion *= float2(_MvScaleX, _MvScaleY);
    motion *= displaySize / (_MvLowResolution != 0 ? displaySize : mvSize);

    if (!all(isfinite(motion)) || any(abs(motion) > displaySize * 0.5f))
    {
        OutputColor[p] = present;
        return;
    }

    const int2 currentPixel = ClampPixel(int2(floor(fp - motion * 0.5f)));
    const int2 previousPixel = ClampPixel(int2(floor(fp + motion * 0.5f)));
    const float2 depthSize = float2((float)max(_DepthWidth, 1u), (float)max(_DepthHeight, 1u));
    const float2 currentDepthPosition = float2(currentPixel) + 0.5f;
    const float2 previousDepthPosition = float2(previousPixel) + 0.5f;
    const uint2 currentDepthPixel = min(uint2(currentDepthPosition * depthSize / displaySize),
                                        uint2(_DepthWidth - 1, _DepthHeight - 1));
    const uint2 previousDepthPixel = min(uint2(previousDepthPosition * depthSize / displaySize),
                                         uint2(_DepthWidth - 1, _DepthHeight - 1));
    const float currentDepth = CurrentDepth.Load(int3(currentDepthPixel, 0));
    const float previousDepth = PreviousDepth.Load(int3(previousDepthPixel, 0));
    const float depthDelta = abs(currentDepth - previousDepth);
    const float depthLimit = max(_DepthThreshold,
                                 max(abs(currentDepth), abs(previousDepth)) * 0.02f);
    const bool depthValid = all(isfinite(float2(currentDepth, previousDepth))) && depthDelta <= depthLimit;
    const bool currentIsFront = _DepthInverted != 0 ? currentDepth > previousDepth : currentDepth < previousDepth;
    bool currentCoversOutput = false;
    if (!depthValid && _ConservativeDepthFallback == 0 && currentIsFront)
    {
        const uint2 outputDepthPixel = min(uint2(fp * depthSize / displaySize),
                                           uint2(_DepthWidth - 1, _DepthHeight - 1));
        const float outputCurrentDepth = CurrentDepth.Load(int3(outputDepthPixel, 0));
        const float coverageLimit = max(_DepthThreshold,
                                        max(abs(outputCurrentDepth), abs(currentDepth)) * 0.02f);
        currentCoversOutput = all(isfinite(float2(outputCurrentDepth, currentDepth))) &&
                              abs(outputCurrentDepth - currentDepth) <= coverageLimit;
    }

    float4 currentScene = 0.0f;
    float4 previousScene = 0.0f;
    if (depthValid || (_ConservativeDepthFallback == 0 && currentIsFront))
    {
        currentScene = CurrentHudless.Load(int3(currentPixel, 0));
        previousScene = PreviousHudless.Load(int3(previousPixel, 0));
    }
    if (!depthValid && _ConservativeDepthFallback == 0 && currentIsFront && !currentCoversOutput &&
        _BackgroundFieldValid != 0 && _BackgroundFieldWidth != 0 && _BackgroundFieldHeight != 0)
    {
        const uint2 fieldPixel = min(uint2(fp * float2(_BackgroundFieldWidth, _BackgroundFieldHeight) /
                                           displaySize),
                                     uint2(_BackgroundFieldWidth - 1, _BackgroundFieldHeight - 1));
        const float4 backgroundField = BackgroundMotionField.Load(int3(fieldPixel, 0));
        if (backgroundField.z >= 0.0f && backgroundField.w >= 0.0f &&
            all(isfinite(backgroundField.xy)) &&
            all(abs(backgroundField.xy) <= displaySize * 0.5f))
        {
            const int2 backgroundPreviousPixel = ClampPixel(int2(floor(fp + backgroundField.xy * 0.5f)));
            previousScene = PreviousHudless.Load(int3(backgroundPreviousPixel, 0));
        }
    }
    float4 reprojectedScene;
    if (depthValid)
    {
        reprojectedScene = 0.5f * (currentScene + previousScene);
    }
    else if (_ConservativeDepthFallback != 0)
    {
        reprojectedScene = CurrentHudless.Load(int3(p, 0));
    }
    else if (currentIsFront)
    {
        reprojectedScene = currentCoversOutput ? currentScene : previousScene;
    }
    else
    {
        reprojectedScene = CurrentHudless.Load(int3(p, 0));
    }

    const float4 hudless = CurrentHudless.Load(int3(p, 0));
    const float uiMask = CalculateStaticContentFactor(hudless.rgb, present.rgb);
    OutputColor[p] = lerp(reprojectedScene, present, uiMask);
}
)";

class FsrFgPeripheralReprojection final : public Shader_Dx12
{
    struct Constants
    {
        UINT width = 0;
        UINT height = 0;
        UINT roiLeft = 0;
        UINT roiTop = 0;
        UINT roiWidth = 0;
        UINT roiHeight = 0;
        UINT mvWidth = 0;
        UINT mvHeight = 0;
        float mvScaleX = 1.0f;
        float mvScaleY = 1.0f;
        UINT mvLowResolution = 0;
        float hudThreshold = 0.03f;
        UINT depthWidth = 0;
        UINT depthHeight = 0;
        float depthThreshold = 0.002f;
        UINT depthInverted = 0;
        UINT conservativeDepthFallback = 0;
        UINT backgroundFieldWidth = 0;
        UINT backgroundFieldHeight = 0;
        UINT backgroundFieldValid = 0;
    };

    static constexpr UINT ConstantDwords = sizeof(Constants) / sizeof(UINT);
    static_assert(sizeof(Constants) % sizeof(UINT) == 0);

    FrameDescriptorHeap _heaps[BUFFER_COUNT];

    static void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        if (commandList == nullptr || resource == nullptr || before == after)
            return;
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);
    }

  public:
    explicit FsrFgPeripheralReprojection(ID3D12Device* device) : Shader_Dx12("FSRFG_PeripheralReprojection", device)
    {
        if (device == nullptr || !SetupRootSignatureWithConstants(device, 7, 1, ConstantDwords))
            return;
        ID3DBlob* blob = CompileShader(fsrFgPeripheralReprojectionShader, "CSMain", "cs_5_0");
        if (blob == nullptr || !CreateComputeShader(device, _rootSignature, &_pipelineState, blob,
                                                     D3D12_SHADER_BYTECODE {}))
        {
            SAFE_RELEASE(blob);
            return;
        }
        SAFE_RELEASE(blob);
        for (auto& heap : _heaps)
        {
            if (!heap.Initialize(device, 7, 1, 0))
                return;
        }
        _init = true;
    }

    ~FsrFgPeripheralReprojection()
    {
        if (!_init || State::Instance().isShuttingDown)
            return;
        for (auto& heap : _heaps)
            heap.ReleaseHeaps();
    }

    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* current,
                  ID3D12Resource* currentHudless, ID3D12Resource* previousHudless,
                  ID3D12Resource* motionVectors, ID3D12Resource* currentDepth, ID3D12Resource* previousDepth,
                  ID3D12Resource* output, D3D12_RESOURCE_STATES currentState,
                  D3D12_RESOURCE_STATES currentHudlessState, D3D12_RESOURCE_STATES previousHudlessState,
                  D3D12_RESOURCE_STATES motionState, D3D12_RESOURCE_STATES currentDepthState,
                  D3D12_RESOURCE_STATES previousDepthState, D3D12_RESOURCE_STATES outputState,
                  const FsrFgRoiRect& roi, float mvScaleX, float mvScaleY, bool lowResolution, float hudThreshold,
                  float depthThreshold, bool invertedDepth, bool conservativeDepthFallback,
                  ID3D12Resource* backgroundMotionField, UINT backgroundFieldWidth, UINT backgroundFieldHeight,
                  D3D12_RESOURCE_STATES backgroundMotionFieldState)
    {
        if (!_init || commandList == nullptr || current == nullptr || currentHudless == nullptr ||
            previousHudless == nullptr || motionVectors == nullptr || currentDepth == nullptr ||
            previousDepth == nullptr || output == nullptr)
            return false;

        const auto currentDesc = current->GetDesc();
        const auto currentHudlessDesc = currentHudless->GetDesc();
        const auto previousHudlessDesc = previousHudless->GetDesc();
        const auto outputDesc = output->GetDesc();
        const auto mvDesc = motionVectors->GetDesc();
        const auto currentDepthDesc = currentDepth->GetDesc();
        const auto previousDepthDesc = previousDepth->GetDesc();
        if (currentDesc.Width == 0 || currentDesc.Height == 0 || currentHudlessDesc.Width != currentDesc.Width ||
            currentHudlessDesc.Height != currentDesc.Height || previousHudlessDesc.Width != currentDesc.Width ||
            previousHudlessDesc.Height != currentDesc.Height || outputDesc.Width != currentDesc.Width ||
            outputDesc.Height != currentDesc.Height || mvDesc.Width == 0 || mvDesc.Height == 0 ||
            currentDepthDesc.Width == 0 || currentDepthDesc.Height == 0 ||
            previousDepthDesc.Width != currentDepthDesc.Width || previousDepthDesc.Height != currentDepthDesc.Height)
            return false;

        const UINT slot = static_cast<UINT>(_counter++ % BUFFER_COUNT);
        auto& heap = _heaps[slot];
        try
        {
            CreateShaderResourceView(_device, current, heap.GetSrvCPU(0));
            CreateShaderResourceView(_device, currentHudless, heap.GetSrvCPU(1));
            CreateShaderResourceView(_device, previousHudless, heap.GetSrvCPU(2));
            CreateShaderResourceView(_device, motionVectors, heap.GetSrvCPU(3));
            CreateShaderResourceView(_device, currentDepth, heap.GetSrvCPU(4));
            CreateShaderResourceView(_device, previousDepth, heap.GetSrvCPU(5));
            CreateShaderResourceView(_device, backgroundMotionField != nullptr ? backgroundMotionField : currentDepth,
                                    heap.GetSrvCPU(6));
            CreateUnorderedAccessView(_device, output, heap.GetUavCPU(0), 0);
        }
        catch (const std::exception& error)
        {
            LOG_WARN("[{}] Descriptor setup failed: {}", _name, error.what());
            return false;
        }

        Constants constants {};
        constants.width = static_cast<UINT>(currentDesc.Width);
        constants.height = currentDesc.Height;
        constants.roiLeft = std::min(roi.left, constants.width);
        constants.roiTop = std::min(roi.top, constants.height);
        constants.roiWidth = std::min(roi.width, constants.width - constants.roiLeft);
        constants.roiHeight = std::min(roi.height, constants.height - constants.roiTop);
        constants.mvWidth = static_cast<UINT>(mvDesc.Width);
        constants.mvHeight = mvDesc.Height;
        constants.mvScaleX = std::isfinite(mvScaleX) ? mvScaleX : 1.0f;
        constants.mvScaleY = std::isfinite(mvScaleY) ? mvScaleY : 1.0f;
        constants.mvLowResolution = lowResolution ? 1u : 0u;
        constants.hudThreshold = std::isfinite(hudThreshold) && hudThreshold > 0.0f ? hudThreshold : 0.03f;
        constants.depthWidth = static_cast<UINT>(currentDepthDesc.Width);
        constants.depthHeight = currentDepthDesc.Height;
        constants.depthThreshold = std::isfinite(depthThreshold) && depthThreshold > 0.0f ? depthThreshold : 0.002f;
        constants.depthInverted = invertedDepth ? 1u : 0u;
        constants.conservativeDepthFallback = conservativeDepthFallback ? 1u : 0u;
        constants.backgroundFieldWidth = backgroundMotionField != nullptr ? backgroundFieldWidth : 0u;
        constants.backgroundFieldHeight = backgroundMotionField != nullptr ? backgroundFieldHeight : 0u;
        constants.backgroundFieldValid = backgroundMotionField != nullptr ? 1u : 0u;

        Transition(commandList, current, currentState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, currentHudless, currentHudlessState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, previousHudless, previousHudlessState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, motionVectors, motionState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, currentDepth, currentDepthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, previousDepth, previousDepthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (backgroundMotionField != nullptr)
            Transition(commandList, backgroundMotionField, backgroundMotionFieldState,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, output, outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12DescriptorHeap* descriptorHeaps[] = { heap.GetHeapCSU() };
        commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
        commandList->SetComputeRootSignature(_rootSignature);
        commandList->SetPipelineState(_pipelineState);
        commandList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
        commandList->SetComputeRoot32BitConstants(1, ConstantDwords, &constants, 0);
        commandList->Dispatch((constants.width + 7) / 8, (constants.height + 7) / 8, 1);

        Transition(commandList, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, outputState);
        Transition(commandList, current, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, currentState);
        Transition(commandList, currentHudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   currentHudlessState);
        Transition(commandList, previousHudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   previousHudlessState);
        Transition(commandList, motionVectors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, motionState);
        Transition(commandList, currentDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, currentDepthState);
        Transition(commandList, previousDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, previousDepthState);
        if (backgroundMotionField != nullptr)
            Transition(commandList, backgroundMotionField, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       backgroundMotionFieldState);
        return true;
    }
};

bool FSRFG_Dx12::HudlessFormatTransfer(int index, ID3D12Device* device, DXGI_FORMAT targetFormat,
                                       Dx12Resource* resource)
{
    if (_hudlessTransfer[index].get() == nullptr || !_hudlessTransfer[index].get()->IsFormatCompatible(targetFormat))
    {
        LOG_DEBUG("Format change, recreate the FormatTransfer");

        if (_hudlessTransfer[index].get() != nullptr)
            _hudlessTransfer[index].reset();

        _hudlessTransfer[index] = std::make_unique<FT_Dx12>("FormatTransfer", device, targetFormat);

        return false;
    }

    if (_hudlessTransfer[index].get() != nullptr &&
        _hudlessTransfer[index].get()->CreateBufferResource(device, resource->GetResource(),
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
        (resource->cmdList == nullptr ||
         CreateBufferResource(device, resource->GetResource(), D3D12_RESOURCE_STATE_COPY_DEST,
                              &_hudlessCopyResource[index])))
    {
        auto cmdList = GetUICommandList(index);

        if (resource->cmdList != nullptr && _hudlessCopyResource[index] != nullptr)
        {
            ResourceBarrier(resource->cmdList, resource->GetResource(), resource->state,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);

            resource->cmdList->CopyResource(_hudlessCopyResource[index], resource->GetResource());

            ResourceBarrier(resource->cmdList, resource->GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                            resource->state);

            ResourceBarrier(resource->cmdList, _hudlessCopyResource[index], D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            _hudlessTransfer[index].get()->Dispatch(cmdList, _hudlessCopyResource[index],
                                                    _hudlessTransfer[index].get()->Buffer());

            ResourceBarrier(cmdList, _hudlessCopyResource[index], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_DEST);
        }
        else
        {
            ResourceBarrier(cmdList, resource->GetResource(), resource->state,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            _hudlessTransfer[index].get()->Dispatch(cmdList, resource->GetResource(),
                                                    _hudlessTransfer[index].get()->Buffer());

            ResourceBarrier(cmdList, resource->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            resource->state);
        }

        resource->copy = _hudlessTransfer[index].get()->Buffer();
        resource->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        return true;
    }

    return false;
}

bool FSRFG_Dx12::UIFormatTransfer(int index, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                                  DXGI_FORMAT targetFormat, Dx12Resource* resource)
{
    if (_uiTransfer[index].get() == nullptr || !_uiTransfer[index].get()->IsFormatCompatible(targetFormat))
    {
        LOG_DEBUG("Format change, recreate the FormatTransfer");

        if (_uiTransfer[index].get() != nullptr)
            _uiTransfer[index].reset();

        _uiTransfer[index] = std::make_unique<FT_Dx12>("FormatTransfer", device, targetFormat);

        return false;
    }

    if (_uiTransfer[index].get() != nullptr &&
        _uiTransfer[index].get()->CreateBufferResource(device, resource->GetResource(),
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        ResourceBarrier(cmdList, resource->GetResource(), resource->state,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        _uiTransfer[index].get()->Dispatch(cmdList, resource->GetResource(), _uiTransfer[index].get()->Buffer());

        ResourceBarrier(cmdList, resource->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        resource->state);

        resource->copy = _uiTransfer[index].get()->Buffer();
        return true;
    }

    return false;
}

typedef struct FfxSwapchainFramePacingTuning
{
    float safetyMarginInMs;  // in Millisecond. Default is 0.1ms
    float varianceFactor;    // valid range [0.0,1.0]. Default is 0.1
    bool allowHybridSpin;    // Allows pacing spinlock to sleep. Default is false.
    uint32_t hybridSpinTime; // How long to spin if allowHybridSpin is true. Measured in timer resolution units. Not
                             // recommended to go below 2. Will result in frequent overshoots. Default is 2.
    bool allowWaitForSingleObjectOnFence; // Allows WaitForSingleObject instead of spinning for fence value. Default is
                                          // false.
} FfxSwapchainFramePacingTuning;

void FSRFG_Dx12::ConfigureFramePaceTuning()
{
    State::Instance().fsrfgFramePaceTuningChanged = false;

    if (_swapChainContext == nullptr || Version() < feature_version { 3, 1, 3 })
        return;

    FfxSwapchainFramePacingTuning fpt {};
    if (Config::Instance()->FGFramePacingTuning.value_or_default())
    {
        fpt.allowHybridSpin = Config::Instance()->FGFPTAllowHybridSpin.value_or_default();
        fpt.allowWaitForSingleObjectOnFence =
            Config::Instance()->FGFPTAllowWaitForSingleObjectOnFence.value_or_default();
        fpt.hybridSpinTime = Config::Instance()->FGFPTHybridSpinTime.value_or_default();
        fpt.safetyMarginInMs = Config::Instance()->FGFPTSafetyMarginInMs.value_or_default();
        fpt.varianceFactor = Config::Instance()->FGFPTVarianceFactor.value_or_default();

        ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 cfgDesc {};
        cfgDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12;
        cfgDesc.key = 2; // FfxSwapchainFramePacingTuning
        cfgDesc.ptr = &fpt;

        auto result = FfxApiProxy::D3D12_Configure(&_swapChainContext, &cfgDesc.header);
        LOG_DEBUG("HybridSpin D3D12_Configure result: {}", FfxApiProxy::ReturnCodeToString(result));
    }
}

feature_version FSRFG_Dx12::Version()
{

    if (_fgContext == nullptr && _version.major == 0)
    {
        if (!FfxApiProxy::IsFGReady())
            FfxApiProxy::InitFfxDx12();

        if (FfxApiProxy::IsFGReady())
            _version = FfxApiProxy::VersionDx12_FG();
    }

    return _version;
}

HWND FSRFG_Dx12::Hwnd() { return _hwnd; }

const char* FSRFG_Dx12::Name() { return "FSR-FG"; }

bool FSRFG_Dx12::IsLocalRoiContext() const
{
    return _roiContextActive;
}

bool FSRFG_Dx12::UseLocalRoiStagingBypass() const
{
    return _roiContextActive && Config::Instance()->FSRFGROIStagingBypass.value_or_default();
}

bool FSRFG_Dx12::IsLocalRoiHudlessActive() const
{
    return _roiContextActive && _roiHudlessActive;
}

uint32_t FSRFG_Dx12::GetFrameGenerationFlags() const
{
    const auto config = Config::Instance();
    uint32_t flags = 0;

    if (config->FGDebugView.value_or_default())
        flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;
    if (config->FGDebugTearLines.value_or_default())
        flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES;
    if (config->FGDebugResetLines.value_or_default())
        flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS;
    if (config->FGDebugPacingLines.value_or_default())
        flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES;

    return flags;
}

ffxReturnCode_t FSRFG_Dx12::FrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* userContext)
{
    if (userContext == nullptr)
        return FFX_API_RETURN_ERROR;

    return static_cast<FSRFG_Dx12*>(userContext)->DispatchCallback(params);
}

bool FSRFG_Dx12::ConfigureLocalHudless(ID3D12Resource* hudless, UINT64 frameId)
{
    if (_fgContext == nullptr)
        return false;

    ffxConfigureDescFrameGeneration config {};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.frameGenerationEnabled = true;
    config.flags = GetFrameGenerationFlags() | FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    config.frameGenerationCallback = &FSRFG_Dx12::FrameGenerationCallback;
    config.frameGenerationCallbackUserContext = this;
    config.onlyPresentGenerated = State::Instance().fgOnlyGenerated;
    config.swapChain = _swapChain;
    config.frameID = frameId;

    if (hudless != nullptr)
    {
        const auto desc = hudless->GetDesc();
        config.HUDLessColor = ffxApiGetResourceDX12(hudless, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        config.generationRect = { 0, 0, static_cast<int32_t>(desc.Width), static_cast<int32_t>(desc.Height) };
    }
    else
    {
        config.HUDLessColor = FfxApiResource({});
        config.generationRect = { 0, 0, static_cast<int32_t>(_roiRect[frameId % BUFFER_COUNT].width),
                                  static_cast<int32_t>(_roiRect[frameId % BUFFER_COUNT].height) };
    }

    const auto result = FfxApiProxy::D3D12_Configure(&_fgContext, &config.header);
    if (result != FFX_API_RETURN_OK)
    {
        LOG_WARN("FSR FG ROI HUD-less configure failed: frame={} resource={:X} result={:X}", frameId,
                 (size_t) hudless, (UINT) result);
        return false;
    }

    return true;
}

bool FSRFG_Dx12::RecordPrepare(int index, UINT64 frameId, ID3D12GraphicsCommandList* commandList, uint32_t flags,
                               const FsrFgRoiRect* lockedRoi)
{
    if (_fgContext == nullptr || commandList == nullptr || index < 0 || index >= BUFFER_COUNT)
        return false;

    ffxCreateBackendDX12Desc backendDesc {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device = _device;

    ffxDispatchDescFrameGenerationPrepareCameraInfo cameraData {};
    cameraData.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
    cameraData.header.pNext = &backendDesc.header;
    std::memcpy(cameraData.cameraPosition, _cameraPosition[index], 3 * sizeof(float));
    std::memcpy(cameraData.cameraUp, _cameraUp[index], 3 * sizeof(float));
    std::memcpy(cameraData.cameraRight, _cameraRight[index], 3 * sizeof(float));
    std::memcpy(cameraData.cameraForward, _cameraForward[index], 3 * sizeof(float));

    ffxDispatchDescFrameGenerationPrepare prepare {};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;

    const float* cameraDataRaw = cameraData.cameraPosition;
    const bool cameraDataIsZeroed =
        std::all_of(cameraDataRaw, cameraDataRaw + 12, [](float value) { return value == 0.0f; });
    prepare.header.pNext = cameraDataIsZeroed ? &backendDesc.header : &cameraData.header;
    prepare.commandList = commandList;
    prepare.frameID = frameId;
    prepare.flags = flags;

    FsrFgRoiRect localRoi {};
    if (_roiContextActive)
    {
        localRoi = lockedRoi != nullptr && lockedRoi->IsValid()
                       ? *lockedRoi
                       : (_roiRect[index].IsValid()
                              ? _roiRect[index]
                              : ResolveRoi(index, _physicalDisplayWidth, _physicalDisplayHeight));
        if (!localRoi.IsValid() ||
            !PrepareLocalInputResources(index, commandList, localRoi, _physicalDisplayWidth, _physicalDisplayHeight))
        {
            LOG_WARN("FSR FG local ROI input preparation failed for frame {}", frameId);
            return false;
        }
    }

    {
        auto velocity = GetResource(FG_ResourceType::Velocity, index);
        ID3D12Resource* velocityResource = nullptr;
        if (velocity)
            velocityResource = _roiContextActive ? _roiVelocity[index] : velocity->GetResource();
        if (!velocity || !IsResourceReady(FG_ResourceType::Velocity, index) || velocityResource == nullptr)
        {
            LOG_ERROR("Velocity is missing");
            return false;
        }
        prepare.motionVectors = ffxApiGetResourceDX12(
            velocityResource,
            _roiContextActive ? FFX_API_RESOURCE_STATE_COMPUTE_READ : GetFfxApiState(velocity->state));
    }

    auto& state = State::Instance();
    {
        auto depth = GetResource(FG_ResourceType::Depth, index);
        ID3D12Resource* depthResource = nullptr;
        if (depth)
            depthResource = _roiContextActive ? _roiDepth[index] : depth->GetResource();
        if (!depth || !IsResourceReady(FG_ResourceType::Depth, index) || depthResource == nullptr)
        {
            LOG_ERROR("Depth is missing");
            return false;
        }
        prepare.depth = ffxApiGetResourceDX12(
            depthResource, _roiContextActive ? FFX_API_RESOURCE_STATE_COMPUTE_READ : GetFfxApiState(depth->state));

        if (_roiContextActive && _roiDepth[index] != nullptr)
        {
            const auto localDepthDesc = _roiDepth[index]->GetDesc();
            prepare.renderSize = { static_cast<uint32_t>(localDepthDesc.Width), localDepthDesc.Height };
        }
        else if (state.currentFeature && state.activeFgInput == FGInput::Upscaler)
            prepare.renderSize = { state.currentFeature->RenderWidth(), state.currentFeature->RenderHeight() };
        else
            prepare.renderSize = { static_cast<uint32_t>(depth->width), depth->height };
    }

    prepare.jitterOffset.x = _jitterX[index];
    prepare.jitterOffset.y = _jitterY[index];
    prepare.motionVectorScale.x = _mvScaleX[index];
    prepare.motionVectorScale.y = _mvScaleY[index];
    prepare.cameraFar = _cameraFar[index];
    prepare.cameraNear = _cameraNear[index];
    prepare.cameraFovAngleVertical = _cameraVFov[index];
    if (_roiContextActive && localRoi.IsValid() && _physicalDisplayHeight != 0 &&
        std::isfinite(prepare.cameraFovAngleVertical) && prepare.cameraFovAngleVertical > 0.0f)
    {
        const float cropScaleY = static_cast<float>(localRoi.height) / static_cast<float>(_physicalDisplayHeight);
        prepare.cameraFovAngleVertical =
            2.0f * std::atan(std::tan(prepare.cameraFovAngleVertical * 0.5f) * cropScaleY);
    }

    const auto config = Config::Instance();
    prepare.frameTimeDelta = config->FTInput.value_or_default() == FrameTimeSource::Input
                                 ? static_cast<float>(_ftDelta[index])
                                 : static_cast<float>(state.lastFGFrameTime);
    prepare.viewSpaceToMetersFactor = _meterFactor[index];

    const auto result = FfxApiProxy::D3D12_Dispatch(&_fgContext, &prepare.header);
    LOG_DEBUG("FSR FG Prepare result: {}, frame: {}, fIndex: {}, commandList: {:X}", result, frameId, index,
              (size_t) commandList);
    return result == FFX_API_RETURN_OK;
}

FsrFgRoiRect FSRFG_Dx12::ResolveRoi(int index, UINT displayWidth, UINT displayHeight)
{
    FsrFgRoiRect roi {};
    if (!Config::Instance()->FSRFGROIEnabled.value_or_default() || displayWidth == 0 || displayHeight == 0)
        return roi;

    roi.width = std::min<UINT>(static_cast<UINT>(std::max(64, Config::Instance()->FSRFGROIWidthPx.value_or_default())),
                               displayWidth);
    roi.height = std::min<UINT>(static_cast<UINT>(std::max(64, Config::Instance()->FSRFGROIHeightPx.value_or_default())),
                                displayHeight);

    if (Config::Instance()->FSRFGROIUseGaze.value_or_default())
    {
        const auto sample = GazeRoiInput::Sample();
        const auto centerX = static_cast<int64_t>(sample.x * static_cast<float>(displayWidth));
        const auto centerY = static_cast<int64_t>(sample.y * static_cast<float>(displayHeight));
        roi.left = static_cast<UINT>(std::clamp<int64_t>(centerX - roi.width / 2, 0, displayWidth - roi.width));
        roi.top = static_cast<UINT>(std::clamp<int64_t>(centerY - roi.height / 2, 0, displayHeight - roi.height));
    }
    else
    {
        const auto left = Config::Instance()->FSRFGROIFixedLeft.value_or(
            static_cast<int>((displayWidth - roi.width) / 2));
        const auto top = Config::Instance()->FSRFGROIFixedTop.value_or(
            static_cast<int>((displayHeight - roi.height) / 2));
        roi.left = static_cast<UINT>(std::clamp(left, 0, static_cast<int>(displayWidth - roi.width)));
        roi.top = static_cast<UINT>(std::clamp(top, 0, static_cast<int>(displayHeight - roi.height)));
    }

    _roiRect[index] = roi;
    return roi;
}

bool FSRFG_Dx12::CreateRoiSurface(ID3D12Resource* source, UINT width, UINT height,
                                  D3D12_RESOURCE_STATES initialState, ID3D12Resource** target, bool allowUav)
{
    if (source == nullptr || target == nullptr || width == 0 || height == 0)
        return false;

    if (!CreateBufferResourceWithSize(_device, source, initialState, target, width, height, allowUav, false))
        return false;

    return *target != nullptr;
}

bool FSRFG_Dx12::CopyRoi(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source,
                         D3D12_RESOURCE_STATES sourceState, const FsrFgRoiRect& sourceRoi, ID3D12Resource* target,
                         UINT targetWidth, UINT targetHeight, D3D12_RESOURCE_STATES targetState)
{
    if (commandList == nullptr || source == nullptr || target == nullptr || sourceRoi.width == 0 ||
        sourceRoi.height == 0 || sourceRoi.width != targetWidth || sourceRoi.height != targetHeight)
        return false;

    const auto sourceDesc = source->GetDesc();
    if (sourceRoi.left + sourceRoi.width > sourceDesc.Width || sourceRoi.top + sourceRoi.height > sourceDesc.Height)
        return false;

    ResourceBarrier(commandList, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(commandList, target, targetState, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = target;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_BOX box { sourceRoi.left, sourceRoi.top, 0, sourceRoi.left + sourceRoi.width,
                   sourceRoi.top + sourceRoi.height, 1 };
    commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    ResourceBarrier(commandList, target, D3D12_RESOURCE_STATE_COPY_DEST, targetState);
    ResourceBarrier(commandList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    return true;
}

bool FSRFG_Dx12::CopyFull(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source,
                          D3D12_RESOURCE_STATES sourceState, ID3D12Resource* target,
                          D3D12_RESOURCE_STATES targetState)
{
    if (commandList == nullptr || source == nullptr || target == nullptr)
        return false;

    const auto sourceDesc = source->GetDesc();
    const auto targetDesc = target->GetDesc();
    if (sourceDesc.Width != targetDesc.Width || sourceDesc.Height != targetDesc.Height ||
        sourceDesc.Format != targetDesc.Format)
        return false;

    ResourceBarrier(commandList, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(commandList, target, targetState, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(target, source);
    ResourceBarrier(commandList, target, D3D12_RESOURCE_STATE_COPY_DEST, targetState);
    ResourceBarrier(commandList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    return true;
}

bool FSRFG_Dx12::CopyPeripheralRegions(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source,
                                       D3D12_RESOURCE_STATES sourceState, ID3D12Resource* target,
                                       D3D12_RESOURCE_STATES targetState, const FsrFgRoiRect& roi, UINT width,
                                       UINT height)
{
    if (commandList == nullptr || source == nullptr || target == nullptr || width == 0 || height == 0)
        return false;

    const auto sourceDesc = source->GetDesc();
    const auto targetDesc = target->GetDesc();
    if (sourceDesc.Width != width || sourceDesc.Height != height || targetDesc.Width != width ||
        targetDesc.Height != height || sourceDesc.Format != targetDesc.Format)
        return false;

    const UINT left = std::min(roi.left, width);
    const UINT top = std::min(roi.top, height);
    const UINT right = std::min(width, left + std::min(roi.width, width - left));
    const UINT bottom = std::min(height, top + std::min(roi.height, height - top));

    ResourceBarrier(commandList, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(commandList, target, targetState, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = target;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    auto copy = [&](UINT x, UINT y, UINT copyWidth, UINT copyHeight) {
        if (copyWidth == 0 || copyHeight == 0)
            return;
        D3D12_BOX box { x, y, 0, x + copyWidth, y + copyHeight, 1 };
        commandList->CopyTextureRegion(&dst, x, y, 0, &src, &box);
    };

    copy(0, 0, width, top);
    copy(0, bottom, width, height - bottom);
    copy(0, top, left, bottom - top);
    copy(right, top, width - right, bottom - top);

    ResourceBarrier(commandList, target, D3D12_RESOURCE_STATE_COPY_DEST, targetState);
    ResourceBarrier(commandList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    return true;
}

bool FSRFG_Dx12::PrepareLocalInputResources(int index, ID3D12GraphicsCommandList* commandList,
                                            const FsrFgRoiRect& roi, UINT displayWidth, UINT displayHeight)
{
    if (!_roiContextActive || !roi.IsValid() || commandList == nullptr || displayWidth == 0 || displayHeight == 0)
        return false;

    auto depth = GetResource(FG_ResourceType::Depth, index);
    auto velocity = GetResource(FG_ResourceType::Velocity, index);
    if (!depth || !velocity || depth->GetResource() == nullptr || velocity->GetResource() == nullptr)
        return false;

    auto mapRect = [&](UINT sourceWidth, UINT sourceHeight) {
        FsrFgRoiRect mapped {};
        mapped.left = std::min(sourceWidth, (roi.left * sourceWidth) / displayWidth);
        mapped.top = std::min(sourceHeight, (roi.top * sourceHeight) / displayHeight);
        mapped.width = std::max<UINT>(1, (roi.width * sourceWidth + displayWidth - 1) / displayWidth);
        mapped.height = std::max<UINT>(1, (roi.height * sourceHeight + displayHeight - 1) / displayHeight);
        mapped.width = std::min(mapped.width, sourceWidth - mapped.left);
        mapped.height = std::min(mapped.height, sourceHeight - mapped.top);
        return mapped;
    };

    const auto depthDesc = depth->GetResource()->GetDesc();
    const auto velocityDesc = velocity->GetResource()->GetDesc();
    const auto depthRoi = mapRect(static_cast<UINT>(depthDesc.Width), depthDesc.Height);
    const auto velocityRoi = mapRect(static_cast<UINT>(velocityDesc.Width), velocityDesc.Height);

    if (!CreateRoiSurface(depth->GetResource(), depthRoi.width, depthRoi.height,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &_roiDepth[index], false) ||
        !CreateRoiSurface(velocity->GetResource(), velocityRoi.width, velocityRoi.height,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &_roiVelocity[index], false))
        return false;

    if (!CopyRoi(commandList, depth->GetResource(), depth->state, depthRoi, _roiDepth[index], depthRoi.width,
                 depthRoi.height, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ||
        !CopyRoi(commandList, velocity->GetResource(), velocity->state, velocityRoi, _roiVelocity[index],
                 velocityRoi.width, velocityRoi.height, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
        return false;

    LOG_DEBUG("FSR FG local guides: frameSlot={} displayRoi=({},{} {}x{}) depthSource={:X} depthRoi=({},{} {}x{}) "
              "mvSource={:X} mvRoi=({},{} {}x{})",
              index, roi.left, roi.top, roi.width, roi.height, (size_t) depth->GetResource(), depthRoi.left,
              depthRoi.top, depthRoi.width, depthRoi.height, (size_t) velocity->GetResource(), velocityRoi.left,
              velocityRoi.top, velocityRoi.width, velocityRoi.height);

    _roiRect[index] = roi;
    return true;
}

void FSRFG_Dx12::ReleaseLocalResources()
{
    for (size_t i = 0; i < BUFFER_COUNT; ++i)
    {
        SAFE_RELEASE(_roiColor[i]);
        SAFE_RELEASE(_roiPreviousColor[i]);
        SAFE_RELEASE(_roiOutput[i]);
        SAFE_RELEASE(_roiDepth[i]);
        SAFE_RELEASE(_roiVelocity[i]);
        SAFE_RELEASE(_roiHudless[i]);
        SAFE_RELEASE(_roiPreviousHudless[i]);
        SAFE_RELEASE(_roiRealColorHistory[i]);
        SAFE_RELEASE(_roiRealHudlessHistory[i]);
        SAFE_RELEASE(_roiRealDepthHistory[i]);
        SAFE_RELEASE(_roiPeripheralOutput[i]);
        _roiRealColorHistoryFrameId[i] = 0;
        _roiRealColorHistoryValid[i] = false;
        _roiRealHudlessHistoryFrameId[i] = 0;
        _roiRealHudlessHistoryValid[i] = false;
        _roiRealDepthHistoryFrameId[i] = 0;
        _roiRealDepthHistoryValid[i] = false;
        _roiRealColorHistoryRect[i] = {};
        _roiRect[i] = {};
    }

    _roiProviderHistoryFrameId = 0;
    _roiProviderHistoryRect = {};
    _roiProviderHistoryValid = false;
    _roiProviderHistoryUsedHudless = false;
    _roiHudlessActivationLogged = false;
    _roiHudlessActive = false;
}

static void fgLogCallback(uint32_t type, const wchar_t* message)
{
    auto message_str = wstring_to_string(std::wstring(message));

    if (type == FFX_API_MESSAGE_TYPE_ERROR)
        spdlog::error("FFX FG Callback: {}", message_str);
    else if (type == FFX_API_MESSAGE_TYPE_WARNING)
        spdlog::warn("FFX FG Callback: {}", message_str);
}

bool FSRFG_Dx12::Dispatch(bool deferExecution)
{
    LOG_FUNC();

    if (_fgContext == nullptr)
    {
        LOG_DEBUG("No fg context");
        return false;
    }

    UINT64 willDispatchFrame = 0;
    auto fIndex = GetDispatchIndex(willDispatchFrame);
    if (fIndex < 0)
        return false;

    if (!IsActive() || IsPaused())
        return false;

    auto& state = State::Instance();
    auto config = Config::Instance();

    if (state.fsrfgFramePaceTuningChanged)
        ConfigureFramePaceTuning();

    LOG_DEBUG("_frameCount: {}, willDispatchFrame: {}, fIndex: {}", _frameCount, willDispatchFrame, fIndex);

    if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].contains(FG_ResourceType::Velocity) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Velocity))
    {
        LOG_WARN("Depth or Velocity is not ready, skipping");
        return false;
    }

    ffxConfigureDescFrameGeneration fgConfig = {};
    fgConfig.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;

    ffxConfigureDescFrameGenerationRegisterDistortionFieldResource distortionFieldDesc {};
    distortionFieldDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE;

    {
        auto distortion = GetResource(FG_ResourceType::Distortion, fIndex);
        if (!_roiContextActive && distortion && IsResourceReady(FG_ResourceType::Distortion, fIndex))
        {
            LOG_TRACE("Using Distortion Field: {:X}", (size_t) distortion->GetResource());

            distortionFieldDesc.distortionField =
                ffxApiGetResourceDX12(distortion->GetResource(), GetFfxApiState(distortion->state));

            distortionFieldDesc.header.pNext = fgConfig.header.pNext;
            fgConfig.header.pNext = &distortionFieldDesc.header;
        }
    }

    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiDesc {};
    uiDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    uiDesc.uiResource = FfxApiResource({});

    {
        auto ui = GetResource(FG_ResourceType::UIColor, fIndex);

        // UIColor belongs to the physical swapchain composition path, not the
        // local frame-generation context. Keeping it full-sized lets ROI mode
        // use the same UI contract as normal FSR frame generation.
        if (ui && IsResourceReady(FG_ResourceType::UIColor, fIndex) &&
            !config->FGDrawUIOverFG.value_or_default())
        {
            auto* uiResource = ui->GetResource();
            DXGI_SWAP_CHAIN_DESC swapChainDesc {};

            if (uiResource != nullptr && _swapChain != nullptr && _swapChain->GetDesc(&swapChainDesc) == S_OK)
            {
                const auto resourceDesc = uiResource->GetDesc();
                if (resourceDesc.Width == swapChainDesc.BufferDesc.Width &&
                    resourceDesc.Height == swapChainDesc.BufferDesc.Height)
                {
                    uiDesc.uiResource = ffxApiGetResourceDX12(uiResource, GetFfxApiState(ui->state));
                    uiDesc.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
                    if (config->FGUIPremultipliedAlpha.value_or_default())
                        uiDesc.flags |= FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA;

                    LOG_TRACE("Registering UI texture with FSR swapchain: {:X}, {}x{}, flags={:X}",
                              (size_t) uiResource, resourceDesc.Width, resourceDesc.Height, uiDesc.flags);
                }
                else
                {
                    LOG_WARN("Skipping FSR UI texture with non-swapchain dimensions: UI={}x{}, swapchain={}x{}",
                             resourceDesc.Width, resourceDesc.Height, swapChainDesc.BufferDesc.Width,
                             swapChainDesc.BufferDesc.Height);
                }
            }
        }
    }

    {
        auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);

        if (!_roiContextActive && hudless && IsResourceReady(FG_ResourceType::HudlessColor, fIndex))
        {
            LOG_TRACE("Using hudless: {:X}", (size_t) hudless->GetResource());

            fgConfig.HUDLessColor = ffxApiGetResourceDX12(hudless->GetResource(), GetFfxApiState(hudless->state));

            // Reset of _paramHudless[fIndex] happens in DispatchCallback
            // as we might use it in Preset to remove hud from swapchain
        }
        else
        {
            fgConfig.HUDLessColor = FfxApiResource({});
        }
    }

    const auto uiConfigureResult = FfxApiProxy::D3D12_Configure(&_swapChainContext, &uiDesc.header);
    if (uiConfigureResult != FFX_API_RETURN_OK)
        LOG_WARN("FSR swapchain UI registration failed: {:X}", (UINT) uiConfigureResult);

    if (fgConfig.HUDLessColor.resource != nullptr)
    {
        static auto localLastHudlessFormat = (FfxApiSurfaceFormat) fgConfig.HUDLessColor.description.format;
        _lastHudlessFormat = (FfxApiSurfaceFormat) fgConfig.HUDLessColor.description.format;

        if (localLastHudlessFormat != _lastHudlessFormat)
        {
            state.fgChanged = true;
            state.scChanged = true;
            LOG_DEBUG("HUDLESS format changed, triggering FG reinit");
        }

        localLastHudlessFormat = _lastHudlessFormat;
    }

    fgConfig.frameGenerationEnabled = _isActive;
    fgConfig.flags = GetFrameGenerationFlags();

    fgConfig.allowAsyncWorkloads = config->FGAsync.value_or_default();

    // if (state.activeFgInput != FGInput::Upscaler)
    {
        // use swapchain buffer info
        DXGI_SWAP_CHAIN_DESC scDesc1 {};
        bool hasSwapChainDesc = _swapChain->GetDesc(&scDesc1) == S_OK;

        int bufferWidth = hasSwapChainDesc ? scDesc1.BufferDesc.Width : 0;
        int bufferHeight = hasSwapChainDesc ? scDesc1.BufferDesc.Height : 0;

        int defaultLeft = 0;
        int defaultTop = 0;
        int defaultWidth = 0;
        int defaultHeight = 0;

        defaultLeft = static_cast<int>(hasSwapChainDesc ? (bufferWidth - _interpolationWidth[fIndex]) / 2 : 0);
        defaultTop = hasSwapChainDesc ? (bufferHeight - _interpolationHeight[fIndex]) / 2 : 0;
        defaultWidth = static_cast<int>(_interpolationWidth[fIndex]);
        defaultHeight = _interpolationHeight[fIndex];

        fgConfig.generationRect.left = config->FGRectLeft.value_or(_interpolationLeft[fIndex].value_or(defaultLeft));
        fgConfig.generationRect.top = config->FGRectTop.value_or(_interpolationTop[fIndex].value_or(defaultTop));
        fgConfig.generationRect.width = config->FGRectWidth.value_or(defaultWidth);
        fgConfig.generationRect.height = config->FGRectHeight.value_or(defaultHeight);

        if (_roiContextActive)
        {
            const auto roi = ResolveRoi(fIndex, static_cast<UINT>(bufferWidth), static_cast<UINT>(bufferHeight));
            _roiRect[fIndex] = roi;
            fgConfig.generationRect.left = roi.left;
            fgConfig.generationRect.top = roi.top;
            fgConfig.generationRect.width = roi.width;
            fgConfig.generationRect.height = roi.height;
        }
    }

    fgConfig.frameGenerationCallbackUserContext = this;
    fgConfig.frameGenerationCallback = &FSRFG_Dx12::FrameGenerationCallback;

    fgConfig.onlyPresentGenerated = state.fgOnlyGenerated;
    fgConfig.frameID = willDispatchFrame;
    fgConfig.swapChain = _swapChain;

    ffxReturnCode_t retCode = FfxApiProxy::D3D12_Configure(&_fgContext, &fgConfig.header);
    LOG_DEBUG("D3D12_Configure result: {0:X}, frame: {1}, fIndex: {2}", retCode, willDispatchFrame, fIndex);

    ffxConfigureDescGlobalDebug1 fgLogging = {};
    fgLogging.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
    fgLogging.fpMessage = &fgLogCallback;
    fgLogging.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
    ffxReturnCode_t loggingRetCode = FfxApiProxy::D3D12_Configure(&_fgContext, &fgLogging.header);

    bool dispatchResult = false;
    if (retCode == FFX_API_RETURN_OK && _isActive)
    {
        auto allocator = _fgCommandAllocator[fIndex];
        auto result = allocator->Reset();
        if (result != S_OK)
        {
            LOG_ERROR("allocator->Reset() error: {:X}", (UINT) result);
            return false;
        }

        result = _fgCommandList[fIndex]->Reset(allocator, nullptr);
        if (result != S_OK)
        {
            LOG_ERROR("_fgCommandList[fIndex]->Reset error: {:X}", (UINT) result);
            return false;
        }

        const auto lockedRoi = _roiContextActive ? &_roiRect[fIndex] : nullptr;
        if (!RecordPrepare(fIndex, willDispatchFrame, _fgCommandList[fIndex], fgConfig.flags, lockedRoi))
        {
            _fgCommandList[fIndex]->Close();
            return false;
        }

        _fgCommandList[fIndex]->Close();
        _waitingExecute[fIndex] = true;
        if (deferExecution)
        {
            _deferredPrepareIndex = fIndex;
            dispatchResult = true;
        }
        else
        {
            dispatchResult = ExecuteCommandList(fIndex);
        }
    }

    if (config->FGUseMutexForSwapchain.value_or_default() && Mutex.getOwner() == 1)
    {
        LOG_TRACE("Releasing FG->Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    };

    return dispatchResult;
}

ffxReturnCode_t FSRFG_Dx12::DispatchCallback(ffxDispatchDescFrameGeneration* params)
{
    const int fIndex = params->frameID % BUFFER_COUNT;

    auto& state = State::Instance();

    if (!Config::Instance()->FGSkipReset.value_or_default())
        params->reset = (_reset[fIndex] != 0);
    else
        params->reset = 0;

    LOG_DEBUG("frameID: {}, commandList: {:X}, numGeneratedFrames: {}", params->frameID, (size_t) params->commandList,
              params->numGeneratedFrames);

    // check for status
    if (!Config::Instance()->FGEnabled.value_or_default() || _fgContext == nullptr || state.scChanged)
    {
        LOG_WARN("Cancel async dispatch");
        params->numGeneratedFrames = 0;
    }

    // If fg is active but upscaling paused
    if ((state.currentFeature == nullptr && state.activeFgInput == FGInput::Upscaler) || state.fgChanged ||
        fIndex < 0 || !IsActive() || (state.currentFeature && state.currentFeature->FrameCount() == 0))
    {
        LOG_WARN("Upscaling paused! frameID: {}", params->frameID);
        params->numGeneratedFrames = 0;
    }

    static UINT64 _lastFrameId = 0;
    if (params->frameID == _lastFrameId)
    {
        LOG_WARN("Dispatched with the same frame id! frameID: {}", params->frameID);
        params->numGeneratedFrames = 0;
        return FFX_API_RETURN_OK;
    }

    if (_roiContextActive)
    {
        _roiHudlessActive = false;
        const auto roi = _roiRect[fIndex].IsValid()
                             ? _roiRect[fIndex]
                             : ResolveRoi(fIndex, _physicalDisplayWidth, _physicalDisplayHeight);
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
        auto* physicalOutput = static_cast<ID3D12Resource*>(params->outputs[0].resource);
        auto* presentColor = static_cast<ID3D12Resource*>(params->presentColor.resource);
        const auto presentState = GetD3D12State((FfxApiResourceState) params->presentColor.state);
        const auto outputState = GetD3D12State((FfxApiResourceState) params->outputs[0].state);
        uint32_t timingSlot = 0;
        const bool timingActive = Config::Instance()->GazeRoiGpuTiming.value_or_default() && commandList != nullptr &&
                                  GazeRoiFrameSync::Acquire(commandList, timingSlot) &&
                                  GazeRoiFrameSync::BeginGpuTiming(_device, commandList, timingSlot, 1);
        if (timingActive)
            GazeRoiFrameSync::WriteGpuTimestamp(commandList, timingSlot, 0);

        const bool placeholderReady = commandList != nullptr && physicalOutput != nullptr && presentColor != nullptr &&
                                      (physicalOutput == presentColor ||
                                       CopyFull(commandList, presentColor, presentState, physicalOutput, outputState));

        if (timingActive)
            GazeRoiFrameSync::WriteGpuTimestamp(commandList, timingSlot, 1);

        if (!roi.IsValid() || commandList == nullptr || physicalOutput == nullptr || presentColor == nullptr ||
            !placeholderReady ||
            !CreateRoiSurface(presentColor, roi.width, roi.height, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              &_roiColor[fIndex], false) ||
            !CreateRoiSurface(physicalOutput, roi.width, roi.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              &_roiOutput[fIndex], true))
        {
            LOG_WARN("FSR FG local ROI resources unavailable; using provider fallback for frame {}", params->frameID);
        }
        else
        {
            const auto presentDesc = presentColor->GetDesc();
            const auto outputDesc = physicalOutput->GetDesc();
            FsrFgRoiRect colorRoi = roi;
            colorRoi.left = std::min<UINT>(roi.left, static_cast<UINT>(presentDesc.Width));
            colorRoi.top = std::min<UINT>(roi.top, presentDesc.Height);
            colorRoi.width = std::min<UINT>(roi.width, static_cast<UINT>(presentDesc.Width) - colorRoi.left);
            colorRoi.height = std::min<UINT>(roi.height, presentDesc.Height - colorRoi.top);

            if (colorRoi.width != roi.width || colorRoi.height != roi.height ||
                outputDesc.Width != presentDesc.Width || outputDesc.Height != presentDesc.Height ||
                outputDesc.Format != presentDesc.Format)
            {
                LOG_WARN("FSR FG local ROI resource contract mismatch; disabling local dispatch for frame {}",
                         params->frameID);
            }
            else if (!CopyRoi(commandList, presentColor, presentState, colorRoi, _roiColor[fIndex], roi.width,
                              roi.height, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
            {
                LOG_WARN("FSR FG local color crop failed; disabling local dispatch for frame {}", params->frameID);
            }
            else
            {
                if (timingActive)
                    GazeRoiFrameSync::WriteGpuTimestamp(commandList, timingSlot, 2);

                auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);
                auto dedicatedUi = GetResource(FG_ResourceType::UIColor, fIndex);
                const bool dedicatedUiReady =
                    dedicatedUi && IsResourceReady(FG_ResourceType::UIColor, fIndex) &&
                    !Config::Instance()->FGDisableUI.value_or_default();
                const bool peripheralReprojectionEnabled =
                    Config::Instance()->FSRFGROIPeripheralReprojection.value_or_default();
                ID3D12Resource* hudlessResource = nullptr;
                D3D12_RESOURCE_STATES hudlessState = D3D12_RESOURCE_STATE_COMMON;
                bool currentHudlessReady = false;
                if ((!dedicatedUiReady || peripheralReprojectionEnabled) &&
                    !Config::Instance()->FGDisableHudless.value_or_default() && hudless &&
                    IsResourceReady(FG_ResourceType::HudlessColor, fIndex))
                {
                    hudlessResource = hudless->GetResource();
                    hudlessState = hudless->state;
                    if (hudlessResource != nullptr)
                    {
                        const auto hudlessDesc = hudlessResource->GetDesc();
                        if (hudlessDesc.Width == _physicalDisplayWidth && hudlessDesc.Height == _physicalDisplayHeight &&
                            CreateRoiSurface(hudlessResource, roi.width, roi.height,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             &_roiHudless[fIndex], false))
                        {
                            currentHudlessReady =
                                CopyRoi(commandList, hudlessResource, hudlessState, roi, _roiHudless[fIndex],
                                        roi.width, roi.height, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        }
                        else
                        {
                            LOG_WARN("FSR FG ROI HUD-less source has incompatible dimensions: source={}x{} physical={}x{}",
                                     hudlessDesc.Width, hudlessDesc.Height, _physicalDisplayWidth,
                                     _physicalDisplayHeight);
                        }
                    }
                }

                if (currentHudlessReady && !_roiHudlessActivationLogged)
                {
                    LOG_INFO("FSR FG ROI HUD-less active: source={:X}, local={}x{}", (size_t) hudlessResource,
                             roi.width, roi.height);
                    _roiHudlessActivationLogged = true;
                }

                int previousHistoryIndex = -1;
                int previousHudlessHistoryIndex = -1;
                int previousDepthHistoryIndex = -1;
                for (int i = 0; i < BUFFER_COUNT; ++i)
                {
                    if (_roiRealColorHistoryValid[i] &&
                        _roiRealColorHistoryFrameId[i] + 1 == params->frameID)
                    {
                        previousHistoryIndex = i;
                    }

                    if (_roiRealHudlessHistoryValid[i] &&
                        _roiRealHudlessHistoryFrameId[i] + 1 == params->frameID)
                        previousHudlessHistoryIndex = i;

                    if (_roiRealDepthHistoryValid[i] && _roiRealDepthHistoryFrameId[i] + 1 == params->frameID)
                        previousDepthHistoryIndex = i;
                }

                // UIColor is a separate provider composition input. Keep capturing HUD-less when both are
                // available so the peripheral path does not oscillate between temporal and placeholder output.
                bool providerHudlessReady = currentHudlessReady && !dedicatedUiReady;
                ID3D12Resource* reprojectionHudless = hudlessResource;
                D3D12_RESOURCE_STATES reprojectionHudlessState = hudlessState;
                bool reprojectionHudlessReady = currentHudlessReady;
                bool hudlessGraceUsed = false;
                if (!reprojectionHudlessReady && peripheralReprojectionEnabled &&
                    !Config::Instance()->FGDisableHudless.value_or_default() &&
                    previousHudlessHistoryIndex >= 0 &&
                    CreateRoiSurface(_roiRealHudlessHistory[previousHudlessHistoryIndex], roi.width, roi.height,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &_roiHudless[fIndex], false))
                {
                    reprojectionHudlessReady = CopyRoi(
                        commandList, _roiRealHudlessHistory[previousHudlessHistoryIndex],
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, roi, _roiHudless[fIndex], roi.width,
                        roi.height, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    if (reprojectionHudlessReady)
                    {
                        reprojectionHudless = _roiHudless[fIndex];
                        reprojectionHudlessState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        hudlessGraceUsed = true;
                        LOG_DEBUG("FSR FG ROI HUD-less one-frame grace: frame={} sourceFrame={}", params->frameID,
                                  _roiRealHudlessHistoryFrameId[previousHudlessHistoryIndex]);
                    }
                }

                const bool providerHistoryMatches =
                    _roiProviderHistoryValid && _roiProviderHistoryFrameId + 1 == params->frameID &&
                    _roiProviderHistoryRect.left == roi.left && _roiProviderHistoryRect.top == roi.top &&
                    _roiProviderHistoryRect.width == roi.width && _roiProviderHistoryRect.height == roi.height &&
                    _roiProviderHistoryUsedHudless == providerHudlessReady;

                bool previousCropReady = false;
                if (!providerHistoryMatches && previousHistoryIndex >= 0 &&
                    CreateRoiSurface(_roiRealColorHistory[previousHistoryIndex], roi.width, roi.height,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &_roiPreviousColor[fIndex],
                                     false))
                {
                    previousCropReady = CopyRoi(
                        commandList, _roiRealColorHistory[previousHistoryIndex],
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, roi, _roiPreviousColor[fIndex], roi.width,
                        roi.height, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }

                bool previousHudlessCropReady = false;
                if (!providerHistoryMatches && providerHudlessReady && previousHudlessHistoryIndex >= 0 &&
                    CreateRoiSurface(_roiRealHudlessHistory[previousHudlessHistoryIndex], roi.width, roi.height,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     &_roiPreviousHudless[fIndex], false))
                {
                    previousHudlessCropReady = CopyRoi(
                        commandList, _roiRealHudlessHistory[previousHudlessHistoryIndex],
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, roi, _roiPreviousHudless[fIndex], roi.width,
                        roi.height, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }

                if (timingActive)
                    GazeRoiFrameSync::WriteGpuTimestamp(commandList, timingSlot, 3);

                // Capture the complete real current frame before composition. presentColor can alias physicalOutput,
                // in which case the ROI is overwritten with the generated image later in this command list.
                const bool currentHistoryReady =
                    CreateRoiSurface(presentColor, static_cast<UINT>(presentDesc.Width), presentDesc.Height,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     &_roiRealColorHistory[fIndex], false) &&
                    CopyFull(commandList, presentColor, presentState, _roiRealColorHistory[fIndex],
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (currentHistoryReady)
                {
                    _roiRealColorHistoryFrameId[fIndex] = params->frameID;
                    _roiRealColorHistoryRect[fIndex] = roi;
                    _roiRealColorHistoryValid[fIndex] = true;
                }
                else
                {
                    _roiRealColorHistoryValid[fIndex] = false;
                    LOG_WARN("FSR FG failed to retain complete real frame {} for moving ROI history",
                             params->frameID);
                }

                bool currentHudlessHistoryReady = false;
                if (currentHudlessReady && (providerHudlessReady || peripheralReprojectionEnabled))
                {
                    const auto hudlessDesc = hudlessResource->GetDesc();
                    currentHudlessHistoryReady =
                        CreateRoiSurface(hudlessResource, static_cast<UINT>(hudlessDesc.Width), hudlessDesc.Height,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                         &_roiRealHudlessHistory[fIndex], false) &&
                        CopyFull(commandList, hudlessResource, hudlessState, _roiRealHudlessHistory[fIndex],
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }

                if (currentHudlessHistoryReady)
                {
                    _roiRealHudlessHistoryFrameId[fIndex] = params->frameID;
                    _roiRealHudlessHistoryValid[fIndex] = true;
                }
                else
                {
                    // Preserve the last real HUD-less frame for the bounded one-frame grace path. Do not advance
                    // its frame ID here, otherwise a missing capture could be chained indefinitely.
                    if (!currentHudlessReady)
                        LOG_DEBUG("FSR FG ROI HUD-less unavailable: frame={} grace={}", params->frameID,
                                  hudlessGraceUsed);
                }

                bool currentDepthHistoryReady = false;
                if (peripheralReprojectionEnabled)
                {
                    auto depth = GetResource(FG_ResourceType::Depth, fIndex);
                    auto* depthResource = depth ? depth->GetResource() : nullptr;
                    if (depthResource != nullptr && IsResourceReady(FG_ResourceType::Depth, fIndex))
                    {
                        const auto depthDesc = depthResource->GetDesc();
                        currentDepthHistoryReady =
                            CreateRoiSurface(depthResource, static_cast<UINT>(depthDesc.Width), depthDesc.Height,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             &_roiRealDepthHistory[fIndex], false) &&
                            CopyFull(commandList, depthResource, depth->state, _roiRealDepthHistory[fIndex],
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    }

                    if (currentDepthHistoryReady)
                    {
                        _roiRealDepthHistoryFrameId[fIndex] = params->frameID;
                        _roiRealDepthHistoryValid[fIndex] = true;
                    }
                    else
                    {
                        _roiRealDepthHistoryValid[fIndex] = false;
                    }
                }

                bool peripheralReprojectionReady = false;
                ID3D12Resource* backgroundMotionField = nullptr;
                UINT backgroundFieldWidth = 0;
                UINT backgroundFieldHeight = 0;
                if (peripheralReprojectionEnabled && reprojectionHudlessReady &&
                    previousHudlessHistoryIndex >= 0 && currentDepthHistoryReady && previousDepthHistoryIndex >= 0)
                {
                    auto velocity = GetResource(FG_ResourceType::Velocity, fIndex);
                    auto depth = GetResource(FG_ResourceType::Depth, fIndex);
                    auto* motionVectors = velocity ? velocity->GetResource() : nullptr;
                    auto* depthResource = depth ? depth->GetResource() : nullptr;
                    if (motionVectors != nullptr && depthResource != nullptr &&
                        IsResourceReady(FG_ResourceType::Velocity, fIndex) &&
                        IsResourceReady(FG_ResourceType::Depth, fIndex) &&
                        CreateRoiSurface(presentColor, static_cast<UINT>(presentDesc.Width), presentDesc.Height,
                                         D3D12_RESOURCE_STATE_COPY_DEST, &_roiPeripheralOutput[fIndex], true) &&
                        presentDesc.Width == static_cast<UINT>(_physicalDisplayWidth) &&
                        presentDesc.Height == _physicalDisplayHeight)
                    {
                        if (_peripheralReprojection == nullptr)
                            _peripheralReprojection = new FsrFgPeripheralReprojection(_device);

                        const bool conservativeDepthFallback =
                            Config::Instance()->FSRFGROIPeripheralDepthConservative.value_or_default();
                        const bool useBackgroundMvPyramid =
                            Config::Instance()->FSRFGROIPeripheralBackgroundMvPyramid.value_or_default() &&
                            !conservativeDepthFallback;
                        if (useBackgroundMvPyramid)
                        {
                            if (_backgroundMvPyramid == nullptr)
                                _backgroundMvPyramid = new FsrFgBackgroundMvPyramid(_device);

                            if (_backgroundMvPyramid != nullptr && _backgroundMvPyramid->IsInit())
                            {
                                backgroundMotionField = _backgroundMvPyramid->Build(
                                    commandList, static_cast<UINT>(fIndex), motionVectors, depthResource,
                                    _roiRealDepthHistory[previousDepthHistoryIndex], velocity->state, depth->state,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    static_cast<UINT>(presentDesc.Width), presentDesc.Height, _mvScaleX[fIndex],
                                    _mvScaleY[fIndex], IsLowResMV(), 0.002f, &backgroundFieldWidth,
                                    &backgroundFieldHeight);
                                if (backgroundMotionField == nullptr)
                                    LOG_DEBUG("FSR FG ROI background MV pyramid unavailable; using direct frame-0 "
                                              "background fallback: frame={}",
                                              params->frameID);
                            }
                        }

                        if (_peripheralReprojection != nullptr && _peripheralReprojection->IsInit())
                        {
                            peripheralReprojectionReady = _peripheralReprojection->Dispatch(
                                commandList, presentColor, reprojectionHudless,
                                _roiRealHudlessHistory[previousHudlessHistoryIndex],
                                motionVectors,
                                depthResource, _roiRealDepthHistory[previousDepthHistoryIndex],
                                _roiPeripheralOutput[fIndex], presentState, reprojectionHudlessState,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                velocity->state, depth->state,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_COPY_DEST, roi, _mvScaleX[fIndex], _mvScaleY[fIndex],
                                IsLowResMV(), GetHudDetectionThreshold(), 0.002f, IsInvertedDepth(),
                                conservativeDepthFallback, backgroundMotionField, backgroundFieldWidth,
                                backgroundFieldHeight, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        }

                        if (peripheralReprojectionReady)
                        {
                            const bool copiedPeripheral = CopyPeripheralRegions(
                                commandList, _roiPeripheralOutput[fIndex], D3D12_RESOURCE_STATE_COPY_DEST,
                                physicalOutput, outputState, roi, static_cast<UINT>(presentDesc.Width),
                                presentDesc.Height);
                            if (!copiedPeripheral)
                                LOG_WARN("FSR FG ROI peripheral region copy failed: frame={}", params->frameID);
                            LOG_DEBUG("FSR FG ROI UI-aware peripheral reprojection active: frame={} present={} "
                                      "hudless={} previousHudless={} mv={} depth={} previousDepth={} mvSize={}x{} "
                                      "depthSize={}x{} lowRes={} threshold={} depthThreshold={} "
                                      "depthReject=single-sample coverageCheck=current-depth depthInverted={} "
                                      "depthPolicy={} backgroundMvPyramid={} field={}x{} "
                                      "roi=({},{} {}x{})",
                                      params->frameID, (size_t) presentColor, (size_t) reprojectionHudless,
                                      (size_t) _roiRealHudlessHistory[previousHudlessHistoryIndex],
                                      (size_t) motionVectors, (size_t) depthResource,
                                      (size_t) _roiRealDepthHistory[previousDepthHistoryIndex],
                                      motionVectors->GetDesc().Width, motionVectors->GetDesc().Height,
                                      depthResource->GetDesc().Width, depthResource->GetDesc().Height, IsLowResMV(),
                                      GetHudDetectionThreshold(), 0.002f, IsInvertedDepth(),
                                      conservativeDepthFallback ? "conservative" : "front-preserve",
                                      backgroundMotionField != nullptr, backgroundFieldWidth, backgroundFieldHeight,
                                      roi.left, roi.top, roi.width, roi.height);
                        }
                    }
                }

                if (timingActive)
                    GazeRoiFrameSync::WriteGpuTimestamp(commandList, timingSlot, 4);

                // The physical output is deliberately initialized with the current complete image. The local
                // generated result overwrites only R_FG below; this is the phase-one peripheral placeholder.
                LOG_DEBUG("FSR FG local provider route=local-context frame={} display={}x{} "
                          "providerHistoryMatches={} previousRealFrame={} hudlessCurrent={} hudlessPrevious={} "
                          "periphery={}",
                          params->frameID, roi.width, roi.height, providerHistoryMatches, previousCropReady,
                          providerHudlessReady, previousHudlessCropReady,
                          peripheralReprojectionReady ? "mv-reprojection" : "external-current-image-placeholder");
                ffxDispatchDescFrameGeneration localParams = *params;
                localParams.presentColor =
                    ffxApiGetResourceDX12(_roiColor[fIndex], FFX_API_RESOURCE_STATE_COMPUTE_READ);
                localParams.outputs[0] =
                    ffxApiGetResourceDX12(_roiOutput[fIndex], FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
                localParams.generationRect = { 0, 0, static_cast<int32_t>(roi.width), static_cast<int32_t>(roi.height) };

                const bool previousSourceReady =
                    previousCropReady && (!providerHudlessReady || previousHudlessCropReady);
                bool providerHistoryReady = providerHistoryMatches;
                if (timingActive)
                    GazeRoiFrameSync::SetGpuTimingPrime(commandList, timingSlot,
                                                       !providerHistoryMatches && previousSourceReady);
                if (!providerHistoryMatches && previousSourceReady)
                {
                    ffxDispatchDescFrameGeneration historyParams = localParams;
                    historyParams.presentColor =
                        ffxApiGetResourceDX12(_roiPreviousColor[fIndex], FFX_API_RESOURCE_STATE_COMPUTE_READ);
                    historyParams.reset = 1;

                    const auto previousHudless = providerHudlessReady ? _roiPreviousHudless[fIndex] : nullptr;
                    const bool historyConfigured = ConfigureLocalHudless(previousHudless, params->frameID);
                    const auto historyResult = historyConfigured
                                                   ? FfxApiProxy::D3D12_Dispatch(&_fgContext, &historyParams.header)
                                                   : FFX_API_RETURN_ERROR;
                    providerHistoryReady = historyConfigured && historyResult == FFX_API_RETURN_OK;
                    LOG_DEBUG("FSR FG moving ROI history prime: frame={} previousFrame={} rect=({},{} {}x{}) result={:X}",
                              params->frameID, params->frameID - 1, roi.left, roi.top, roi.width, roi.height,
                              (UINT) historyResult);
                }

                if (!ConfigureLocalHudless(providerHudlessReady ? _roiHudless[fIndex] : nullptr, params->frameID))
                {
                    providerHudlessReady = false;
                    providerHistoryReady = false;
                    ConfigureLocalHudless(nullptr, params->frameID);
                }
                _roiHudlessActive = providerHudlessReady;

                if (!providerHistoryReady)
                {
                    localParams.reset = 1;
                    LOG_DEBUG("FSR FG local history unavailable; forcing current-frame fallback for frame {}",
                              params->frameID);
                }

                if (timingActive)
                    GazeRoiFrameSync::WriteGpuTimestamp(commandList, timingSlot, 5);

                const auto localResult = FfxApiProxy::D3D12_Dispatch(&_fgContext, &localParams.header);
                if (timingActive)
                    GazeRoiFrameSync::WriteGpuTimestamp(commandList, timingSlot, 6);
                if (localResult == FFX_API_RETURN_OK)
                {
                    _roiProviderHistoryFrameId = params->frameID;
                    _roiProviderHistoryRect = roi;
                    _roiProviderHistoryValid = true;
                    _roiProviderHistoryUsedHudless = providerHudlessReady;

                    ResourceBarrier(commandList, _roiOutput[fIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
                    ResourceBarrier(commandList, physicalOutput, outputState, D3D12_RESOURCE_STATE_COPY_DEST);

                    D3D12_TEXTURE_COPY_LOCATION src {};
                    src.pResource = _roiOutput[fIndex];
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    src.SubresourceIndex = 0;
                    D3D12_TEXTURE_COPY_LOCATION dst {};
                    dst.pResource = physicalOutput;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.SubresourceIndex = 0;
                    D3D12_BOX localBox { 0, 0, 0, roi.width, roi.height, 1 };
                    commandList->CopyTextureRegion(&dst, roi.left, roi.top, 0, &src, &localBox);

                    if (timingActive)
                        GazeRoiFrameSync::WriteGpuTimestamp(commandList, timingSlot, 7);
                    if (timingActive)
                        GazeRoiFrameSync::ResolveGpuTiming(commandList, timingSlot);

                    ResourceBarrier(commandList, physicalOutput, D3D12_RESOURCE_STATE_COPY_DEST, outputState);
                    ResourceBarrier(commandList, _roiOutput[fIndex], D3D12_RESOURCE_STATE_COPY_SOURCE,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    LOG_DEBUG("FSR FG local ROI dispatch: frame {}, rect=({},{} {}x{})", params->frameID,
                              roi.left, roi.top, roi.width, roi.height);
                    _lastFrameId = params->frameID;
                    return localResult;
                }

                LOG_WARN("FSR FG local dispatch failed ({:X}); returning provider error", (UINT) localResult);
                _roiProviderHistoryValid = false;
                _roiProviderHistoryUsedHudless = false;
                _roiHudlessActive = false;
                _lastFrameId = params->frameID;
                return localResult;
            }
        }
    }

    if (_roiContextActive)
    {
        // A local context cannot safely consume physical-display resources. Do
        // not fall through to the stock dispatch with mismatched dimensions.
        params->numGeneratedFrames = 0;
        _lastFrameId = params->frameID;
        return FFX_API_RETURN_OK;
    }

    auto scFormat = (FfxApiSurfaceFormat) params->presentColor.description.format;
    auto lhFormat = _lastHudlessFormat;
    auto uhFormat = _usingHudlessFormat;

    // if (_lastHudlessFormat != FFX_API_SURFACE_FORMAT_UNKNOWN && lhFormat != scFormat &&
    //     (_usingHudlessFormat == FFX_API_SURFACE_FORMAT_UNKNOWN || uhFormat != lhFormat))
    //{
    //     LOG_DEBUG("Hudless format doesn't match, hudless: {}, present: {}", (uint32_t) _lastHudlessFormat,
    //               params->presentColor.description.format);

    //    params->numGeneratedFrames = 0;
    //    _lastFrameId = params->frameID;

    //    state.fgChanged = true;
    //    state.scChanged = true;

    //    return FFX_API_RETURN_OK;
    //}

    static bool lastFGDisableHudless = Config::Instance()->FGDisableHudless.value_or_default();
    if (lastFGDisableHudless != Config::Instance()->FGDisableHudless.value_or_default())
    {
        lastFGDisableHudless = Config::Instance()->FGDisableHudless.value_or_default();

        // We can't just stop sending hudless if we have provided a hudless desc
        // Recreate FG if it was linked at creation
        if (_linkedHudlesDesc)
        {
            if (lastFGDisableHudless)
                _lastHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;

            params->numGeneratedFrames = 0;
            _lastFrameId = params->frameID;

            state.fgChanged = true;
            state.scChanged = true;

            return FFX_API_RETURN_OK;
        }
    }

    bool applyHudCutoff = Config::Instance()->FGHudCutoff.value_or_default() > 0.0f ||
                          State::Instance().gameQuirks & GameQuirk::FSRFGHudlessMismatchFixup;

    if ((applyHudCutoff || State::Instance().fgHudlessCompare) && !lastFGDisableHudless)
    {
        auto presentWithHud = (ID3D12Resource*) params->presentColor.resource;
        auto hudlessResource = _resourceCopy[fIndex][FG_ResourceType::HudlessColor];
        auto hudlessState = D3D12_RESOURCE_STATE_COPY_DEST;

        if (hudlessResource == nullptr)
        {
            auto hudless = _frameResources[fIndex][FG_ResourceType::HudlessColor];
            if (hudless.validity == FG_ResourceValidity::UntilPresent)
                hudlessResource = hudless.GetResource();

            // hudless.state only holds the state for the original resource, not the copy that we could get here
            if (hudlessResource && hudlessResource == hudless.resource)
                hudlessState = hudless.state;
        }

        if (presentWithHud && hudlessResource)
        {
            auto cmdList = (ID3D12GraphicsCommandList*) params->commandList;

            if (applyHudCutoff)
            {
                if (_hudCopy[fIndex].get() == nullptr)
                {
                    _hudCopy[fIndex] = std::make_unique<HudCopy_Dx12>("HudCopy", _device);
                }

                if (auto hudCopy = _hudCopy[fIndex].get(); hudCopy && hudCopy->IsInit())
                {
                    hudCopy->Dispatch(cmdList, hudlessResource, presentWithHud, hudlessState,
                                      GetD3D12State((FfxApiResourceState) params->presentColor.state),
                                      GetHudDetectionThreshold());
                }
            }

            if (State::Instance().fgHudlessCompare)
            {
                if (hudlessResource != nullptr)
                {
                    if (_hudlessCompareCompute[fIndex].get() == nullptr)
                    {
                        _hudlessCompareCompute[fIndex] = std::make_unique<HCC_Dx12>("HudlessCompareCompute", _device);
                    }

                    if (auto hudlessCompareCompute = _hudlessCompareCompute[fIndex].get();
                        hudlessCompareCompute && hudlessCompareCompute->IsInit())
                    {
                        hudlessCompareCompute->Dispatch(
                            cmdList, hudlessResource, presentWithHud, hudlessState,
                            GetD3D12State((FfxApiResourceState) params->presentColor.state));
                    }
                }
            }
        }
    }

    auto dispatchResult = FfxApiProxy::D3D12_Dispatch(&_fgContext, &params->header);
    LOG_DEBUG("D3D12_Dispatch result: {}, fIndex: {}", (UINT) dispatchResult, fIndex);

    _lastFrameId = params->frameID;

    return dispatchResult;
}

FSRFG_Dx12::~FSRFG_Dx12() { Shutdown(); }

bool FSRFG_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount) { return true; }

void* FSRFG_Dx12::FrameGenerationContext()
{
    LOG_DEBUG("");
    return (void*) _fgContext;
}

void* FSRFG_Dx12::SwapchainContext()
{
    LOG_DEBUG("");
    return _swapChainContext;
}

void FSRFG_Dx12::DestroyFGContext()
{
    _frameCount = 1;
    // _lastDispatchedFrame = 0;
    _version = {};

    LOG_DEBUG("");

    Deactivate();

    if (_fgContext != nullptr)
    {
        auto result = FfxApiProxy::D3D12_DestroyContext(&_fgContext, nullptr);

        if (!(State::Instance().isShuttingDown))
            LOG_INFO("D3D12_DestroyContext result: {0:X}", result);

        _fgContext = nullptr;
    }

    ReleaseObjects();
}

bool FSRFG_Dx12::Shutdown()
{
    Deactivate();

    if (_swapChainContext != nullptr)
    {
        if (ReleaseSwapchain(_hwnd))
            State::Instance().currentFGSwapchain = nullptr;
    }

    ReleaseObjects();

    return true;
}

bool FSRFG_Dx12::CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                 IDXGISwapChain** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == desc->OutputWindow)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->BufferDesc.Width, desc->BufferDesc.Height,
                              desc->BufferDesc.Format, desc->Flags) == S_OK;

            *swapChain = State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);

            // Not sure why but XeFG sometimes doesn't release the swapchain properly
            // maybe needed for FSR-FG too, so added it here
            // so we force release it here to be able to recreate swapchain for same hwnd
            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    ffxCreateContextDescFrameGenerationSwapChainNewDX12 createSwapChainDesc {};
    createSwapChainDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12;
    createSwapChainDesc.dxgiFactory = realFactory;
    createSwapChainDesc.gameQueue = realQueue;
    createSwapChainDesc.desc = desc;
    createSwapChainDesc.swapchain = (IDXGISwapChain4**) swapChain;

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 versionDesc {};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    versionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

    createSwapChainDesc.header.pNext = &versionDesc.header;

    auto result = FfxApiProxy::D3D12_CreateContext(&_swapChainContext, &createSwapChainDesc.header, nullptr);

    if (result == FFX_API_RETURN_OK)
    {
        ConfigureFramePaceTuning();

        _gameCommandQueue = realQueue;
        _swapChain = *swapChain;
        _hwnd = desc->OutputWindow;

        return true;
    }

    return false;
}

bool FSRFG_Dx12::CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                  DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                  IDXGISwapChain1** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == hwnd)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("XeFG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->Width, desc->Height, desc->Format, desc->Flags) == S_OK;

            *swapChain = (IDXGISwapChain1*) State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);

            // Not sure why but XeFG sometimes doesn't release the swapchain properly
            // maybe needed for FSR-FG too, so added it here
            // so we force release it here to be able to recreate swapchain for same hwnd
            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 createSwapChainDesc {};
    createSwapChainDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    createSwapChainDesc.fullscreenDesc = pFullscreenDesc;
    createSwapChainDesc.hwnd = hwnd;
    createSwapChainDesc.dxgiFactory = realFactory;
    createSwapChainDesc.gameQueue = realQueue;
    createSwapChainDesc.desc = desc;
    createSwapChainDesc.swapchain = (IDXGISwapChain4**) swapChain;

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 versionDesc {};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    versionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

    createSwapChainDesc.header.pNext = &versionDesc.header;

    auto result = FfxApiProxy::D3D12_CreateContext(&_swapChainContext, &createSwapChainDesc.header, nullptr);

    if (result == FFX_API_RETURN_OK)
    {
        ConfigureFramePaceTuning();

        _gameCommandQueue = realQueue;
        _swapChain = *swapChain;
        _hwnd = hwnd;

        return true;
    }

    return false;
}

bool FSRFG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    LOG_DEBUG("");

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        if (Mutex.getOwner() == 1)
        {
            LOG_WARN("Skipping Mutex we are already in ReleaseSwapchain");
            return true;
        }

        LOG_TRACE("Waiting Mutex 1, current: {}", Mutex.getOwner());
        Mutex.lock(1);
        LOG_TRACE("Accuired Mutex: {}", Mutex.getOwner());
    }

    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    if (_fgContext != nullptr)
        DestroyFGContext();

    if (!State::Instance().isShuttingDown)
    {
        if (_swapChainContext != nullptr)
        {
            auto result = FfxApiProxy::D3D12_DestroyContext(&_swapChainContext, nullptr);
            LOG_INFO("Destroy Ffx Swapchain Result: {}({})", result, FfxApiProxy::ReturnCodeToString(result));
        }

        _swapChainContext = nullptr;
        State::Instance().currentFGSwapchain = nullptr;
    }

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Releasing Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    }

    return true;
}

void FSRFG_Dx12::CreateContext(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_DEBUG("");

    CreateObjects(device);

    _constants = fgConstants;

    // Changing the format of the hudless resource requires a new context
    if (_fgContext != nullptr && (_lastHudlessFormat != _usingHudlessFormat))
    {
        auto result = FfxApiProxy::D3D12_DestroyContext(&_fgContext, nullptr);
        _fgContext = nullptr;
    }

    if (_fgContext != nullptr)
    {
        ffxConfigureDescFrameGeneration m_FrameGenerationConfig = {};
        m_FrameGenerationConfig.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        m_FrameGenerationConfig.frameGenerationEnabled = true;
        m_FrameGenerationConfig.swapChain = _swapChain;
        m_FrameGenerationConfig.presentCallback = nullptr;
        m_FrameGenerationConfig.HUDLessColor = FfxApiResource({});

        auto result = FfxApiProxy::D3D12_Configure(&_fgContext, &m_FrameGenerationConfig.header);

        _isActive = (result == FFX_API_RETURN_OK);

        LOG_DEBUG("Reactivate");

        return;
    }

    ffxQueryDescGetVersions versionQuery {};
    versionQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versionQuery.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    versionQuery.device = device; // only for DirectX 12 applications
    uint64_t versionCount = 0;
    versionQuery.outputCount = &versionCount;
    // get number of versions for allocation
    FfxApiProxy::D3D12_Query(nullptr, &versionQuery.header);

    State::Instance().ffxFGVersionIds.resize(versionCount);
    State::Instance().ffxFGVersionNames.resize(versionCount);
    versionQuery.versionIds = State::Instance().ffxFGVersionIds.data();
    versionQuery.versionNames = State::Instance().ffxFGVersionNames.data();
    // fill version ids and names arrays.
    FfxApiProxy::D3D12_Query(nullptr, &versionQuery.header);

    ffxCreateBackendDX12Desc backendDesc {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device = device;

    // Only gets linked if _lastHudlessFormat != FFX_API_SURFACE_FORMAT_UNKNOWN
    ffxCreateContextDescFrameGenerationHudless hudlessDesc {};
    hudlessDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudlessDesc.hudlessBackBufferFormat = _lastHudlessFormat;
    hudlessDesc.header.pNext = &backendDesc.header;

    ffxCreateContextDescFrameGeneration createFg {};
    createFg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;

    // use swapchain buffer info
    DXGI_SWAP_CHAIN_DESC desc {};
    if (State::Instance().currentSwapchain->GetDesc(&desc) == S_OK)
    {
        _physicalDisplayWidth = desc.BufferDesc.Width;
        _physicalDisplayHeight = desc.BufferDesc.Height;

        _roiContextActive = Config::Instance()->FSRFGROIEnabled.value_or_default();
        if (_roiContextActive)
        {
            const auto roi = ResolveRoi(0, _physicalDisplayWidth, _physicalDisplayHeight);
            createFg.displaySize = { roi.width, roi.height };
            const auto currentFeature = State::Instance().currentFeature;
            const auto renderWidth = currentFeature != nullptr && currentFeature->RenderWidth() != 0
                                         ? currentFeature->RenderWidth()
                                         : _physicalDisplayWidth;
            const auto renderHeight = currentFeature != nullptr && currentFeature->RenderHeight() != 0
                                          ? currentFeature->RenderHeight()
                                          : _physicalDisplayHeight;
            createFg.maxRenderSize = {
                std::max<UINT>(1, (roi.width * renderWidth + _physicalDisplayWidth - 1) / _physicalDisplayWidth),
                std::max<UINT>(1, (roi.height * renderHeight + _physicalDisplayHeight - 1) / _physicalDisplayHeight) };
            LOG_INFO("FSR FG local ROI context: {}x{} (physical {}x{})", roi.width, roi.height,
                     _physicalDisplayWidth, _physicalDisplayHeight);
        }
        else
        {
            createFg.displaySize = { _physicalDisplayWidth, _physicalDisplayHeight };

            if (fgConstants.displayWidth != 0 && fgConstants.displayHeight != 0)
                createFg.maxRenderSize = { fgConstants.displayWidth, fgConstants.displayHeight };
            else
                createFg.maxRenderSize = { _physicalDisplayWidth, _physicalDisplayHeight };
        }
    }
    else
    {
        // this might cause issues
        createFg.displaySize = { fgConstants.displayWidth, fgConstants.displayHeight };
        createFg.maxRenderSize = { fgConstants.displayWidth, fgConstants.displayHeight };
        _physicalDisplayWidth = createFg.displaySize.width;
        _physicalDisplayHeight = createFg.displaySize.height;
        _roiContextActive = false;
    }

    _maxRenderWidth = createFg.maxRenderSize.width;
    _maxRenderHeight = createFg.maxRenderSize.height;

    createFg.flags = 0;

    if (fgConstants.flags & FG_Flags::Hdr)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;

    if (fgConstants.flags & FG_Flags::InvertedDepth)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;

    if (fgConstants.flags & FG_Flags::JitteredMVs)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;

    if (fgConstants.flags & FG_Flags::DisplayResolutionMVs)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;

    if (fgConstants.flags & FG_Flags::Async)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;

    if (fgConstants.flags & FG_Flags::InfiniteDepth)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;

    if (spdlog::default_logger()->level() == SPDLOG_LEVEL_TRACE)
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;

    createFg.backBufferFormat = ffxApiGetSurfaceFormatDX12(desc.BufferDesc.Format);

    if (_lastHudlessFormat != FFX_API_SURFACE_FORMAT_UNKNOWN)
    {
        _usingHudlessFormat = _lastHudlessFormat;
        _lastHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
        createFg.header.pNext = &hudlessDesc.header;
        _linkedHudlesDesc = true;
    }
    else
    {
        _usingHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
        createFg.header.pNext = &backendDesc.header;
        _linkedHudlesDesc = false;
    }

    {
        ScopedSkipSpoofing skipSpoofing {};
        ScopedSkipHeapCapture skipHeapCapture {};

        // Currently 0 is non-ML FG and 1 is ML FG
        if (Config::Instance()->FfxFGIndex.value_or_default() < 0 ||
            Config::Instance()->FfxFGIndex.value_or_default() >= State::Instance().ffxFGVersionIds.size())
            Config::Instance()->FfxFGIndex.set_volatile_value(0);

        ffxOverrideVersion override = { 0 };
        override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        override.versionId = State::Instance().ffxFGVersionIds[Config::Instance()->FfxFGIndex.value_or_default()];
        backendDesc.header.pNext = &override.header;

        ParseVersion(State::Instance().ffxFGVersionNames[Config::Instance()->FfxFGIndex.value_or_default()], &_version);

        ffxReturnCode_t retCode = FfxApiProxy::D3D12_CreateContext(&_fgContext, &createFg.header, nullptr);

        LOG_INFO("D3D12_CreateContext result: {:X}", retCode);
        _isActive = (retCode == FFX_API_RETURN_OK);
        _lastDispatchedFrame = 0;
    }

    LOG_DEBUG("Create");
}

void FSRFG_Dx12::Activate()
{
    if (_fgContext != nullptr && _swapChain != nullptr && !_isActive)
    {
        ffxConfigureDescFrameGeneration fgConfig = {};
        fgConfig.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        fgConfig.frameGenerationEnabled = true;
        fgConfig.swapChain = _swapChain;
        fgConfig.presentCallback = nullptr;
        fgConfig.HUDLessColor = FfxApiResource({});

        auto result = FfxApiProxy::D3D12_Configure(&_fgContext, &fgConfig.header);

        if (result == FFX_API_RETURN_OK)
        {
            _isActive = true;
            _lastDispatchedFrame = 0;
        }

        LOG_INFO("D3D12_Configure Enabled: true, result: {} ({})", magic_enum::enum_name((FfxApiReturnCodes) result),
                 (UINT) result);
    }
}

void FSRFG_Dx12::Deactivate()
{
    if (_isActive)
    {
        auto fIndex = GetIndex();
        if (_uiCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _uiCommandList[fIndex][{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
            auto closeResult = _uiCommandList[fIndex]->Close();

            if (closeResult == S_OK)
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
            else
                LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);

            _uiCommandListResetted[fIndex] = false;
        }

        ffxReturnCode_t result = FFX_API_RETURN_ERROR;

        if (_fgContext != nullptr)
        {
            ffxConfigureDescFrameGeneration fgConfig = {};
            fgConfig.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            fgConfig.frameGenerationEnabled = false;
            fgConfig.swapChain = _swapChain;
            fgConfig.presentCallback = nullptr;
            fgConfig.HUDLessColor = FfxApiResource({});

            result = FfxApiProxy::D3D12_Configure(&_fgContext, &fgConfig.header);

            if (result == FFX_API_RETURN_OK)
                _isActive = false;
        }
        else
        {
            LOG_DEBUG("No context to deactivate, just set  active to false");
            _isActive = false;
        }

        // _lastDispatchedFrame = 0;

        LOG_INFO("D3D12_Configure Enabled: false, result: {} ({})", magic_enum::enum_name((FfxApiReturnCodes) result),
                 (UINT) result);
    }
}

void FSRFG_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_FUNC();

    OwnedLockGuard lock(Mutex, 555);

    _constants = fgConstants;

    if (!FfxApiProxy::IsFGReady())
        FfxApiProxy::InitFfxDx12();

    // If needed hooks are missing or XeFG proxy is not inited or FG swapchain is not created
    if (!FfxApiProxy::IsFGReady() || State::Instance().currentFGSwapchain == nullptr)
        return;

    if (State::Instance().isShuttingDown)
    {
        DestroyFGContext();
        return;
    }

    static bool lastInfiniteDepth = false;
    bool currentInfiniteDepth = static_cast<bool>(fgConstants.flags & FG_Flags::InfiniteDepth);
    if (lastInfiniteDepth != currentInfiniteDepth)
    {
        lastInfiniteDepth = currentInfiniteDepth;
        LOG_DEBUG("Infinite Depth changed: {}", currentInfiniteDepth);

        State::Instance().fgChanged = true;
        State::Instance().scChanged = true;
    }

    if (!_roiContextActive && _maxRenderWidth != 0 && _maxRenderHeight != 0 && IsActive() && !IsPaused() &&
        (fgConstants.displayWidth > _maxRenderWidth || fgConstants.displayHeight > _maxRenderHeight))

    {
        State::Instance().fgChanged = true;
        State::Instance().scChanged = true;
    }

    // If FG Enabled from menu
    if (Config::Instance()->FGEnabled.value_or_default())
    {
        // If FG context is nullptr
        if (_fgContext == nullptr)
        {
            // Create it again
            CreateContext(device, fgConstants);

            // Pause for 10 frames
            UpdateTarget();
        }
        // If there is a change deactivate it
        else if (State::Instance().fgChanged)
        {
            Deactivate();

            // Pause for 10 frames
            UpdateTarget();

            // Destroy if Swapchain has a change destroy FG Context too
            if (State::Instance().scChanged)
                DestroyFGContext();
        }

        if (_fgContext != nullptr && State::Instance().activeFgInput == FGInput::Upscaler && !IsPaused() && !IsActive())
            Activate();
    }
    else if (IsActive())
    {
        Deactivate();

        State::Instance().clearCapturedHudlesses = true;
        Hudfix_Dx12::ResetCounters();
    }

    if (State::Instance().fgChanged)
    {
        LOG_DEBUG("FGchanged");

        State::Instance().fgChanged = false;

        Hudfix_Dx12::ResetCounters();

        // Pause for 10 frames
        UpdateTarget();

        // Release FG mutex
        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }

    State::Instance().scChanged = false;
}

void FSRFG_Dx12::ReleaseObjects()
{
    ReleaseLocalResources();

    delete _peripheralReprojection;
    _peripheralReprojection = nullptr;
    delete _backgroundMvPyramid;
    _backgroundMvPyramid = nullptr;

    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_fgCommandAllocator[i]);
        SAFE_RELEASE(_fgCommandList[i]);
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);
    }

    _renderUI.reset();
    _hudlessCompare.reset();
    _mvFlip.reset();
    _depthFlip.reset();
}

bool FSRFG_Dx12::ExecuteCommandList(int index)
{
    if (_waitingExecute[index])
    {
        LOG_DEBUG("Executing FG cmdList: {:X}", (size_t) _fgCommandList[index]);
        _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_fgCommandList[index]);
        SetExecuted(index);
    }

    return true;
}

bool FSRFG_Dx12::SetResource(Dx12Resource* inputResource)
{
    if (inputResource == nullptr || inputResource->resource == nullptr ||
        (inputResource->type != FG_ResourceType::UIColor && (!IsActive() || IsPaused())))
    {
        return false;
    }

    // For late sent SL resources
    // we use provided frame index
    auto fIndex = inputResource->frameIndex;
    if (fIndex < 0)
        fIndex = GetIndex();

    auto& type = inputResource->type;

    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    if (_frameResources[fIndex].contains(type) &&
        _frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow)
    {
        return false;
    }

    if (type == FG_ResourceType::HudlessColor)
    {
        if (Config::Instance()->FGDisableHudless.value_or_default())
            return false;

        if (!_noHudless[fIndex] && Config::Instance()->FGOnlyAcceptFirstHudless.value_or_default() &&
            inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
        {
            return false;
        }
    }

    if (type == FG_ResourceType::UIColor && Config::Instance()->FGDisableUI.value_or_default())
        return false;

    if (inputResource->cmdList == nullptr && inputResource->validity == FG_ResourceValidity::ValidNow)
    {
        LOG_ERROR("{}, validity == ValidNow but cmdList is nullptr!", magic_enum::enum_name(type));
        return false;
    }

    _frameResources[fIndex][type] = {};
    auto fResource = &_frameResources[fIndex][type];
    fResource->type = type;
    fResource->state = inputResource->state;
    fResource->validity = inputResource->validity;
    fResource->resource = inputResource->resource;
    fResource->width = inputResource->width;
    fResource->height = inputResource->height;
    fResource->cmdList = inputResource->cmdList;

    auto willFlip = State::Instance().activeFgInput == FGInput::Upscaler &&
                    Config::Instance()->FGResourceFlip.value_or_default() &&
                    (fResource->type == FG_ResourceType::Velocity || fResource->type == FG_ResourceType::Depth);

    // Resource flipping
    if (willFlip && _device != nullptr)
    {
        FlipResource(fResource);
    }

    if (type == FG_ResourceType::UIColor)
    {
        // auto format = State::Instance().currentSwapchainDesc.BufferDesc.Format;

        // auto uiFormat = (FfxApiSurfaceFormat) ffxApiGetSurfaceFormatDX12(fResource->GetResource()->GetDesc().Format);
        // auto scFormat = (FfxApiSurfaceFormat) ffxApiGetSurfaceFormatDX12(format);

        // if (uiFormat == -1 || scFormat == -1 || uiFormat != scFormat)
        //{
        //     if (!UIFormatTransfer(fIndex, _device, GetUICommandList(fIndex), format, fResource))
        //     {
        //         LOG_WARN("Skipping UI resource due to format mismatch! UI: {}, swapchain: {}",
        //                  magic_enum::enum_name(uiFormat), magic_enum::enum_name(scFormat));

        //        _frameResources[fIndex][type] = {};
        //        return false;
        //    }
        //    else
        //    {
        //          fResource->validity = FG_ResourceValidity::UntilPresent;
        //    }
        //}

        // fResource->validity = FG_ResourceValidity::UntilPresent;
        _noUi[fIndex] = false;
    }
    else if (type == FG_ResourceType::Distortion)
    {
        _noDistortionField[fIndex] = false;
    }
    else if (type == FG_ResourceType::HudlessColor)
    {
        auto scFormat = State::Instance().currentSwapchainDesc.BufferDesc.Format;
        auto scFfxFormat =
            (FfxApiSurfaceFormat) ffxApiGetSurfaceFormatDX12(State::Instance().currentSwapchainDesc.BufferDesc.Format);

        auto resFormat = fResource->GetResource()->GetDesc().Format;
        _lastHudlessFormat = (FfxApiSurfaceFormat) ffxApiGetSurfaceFormatDX12(resFormat);

        if (_lastHudlessFormat != FFX_API_SURFACE_FORMAT_UNKNOWN && !CompareResourceFormats(resFormat, scFormat))
        {
            if (!HudlessFormatTransfer(fIndex, _device, scFormat, fResource))
            {
                LOG_WARN("Skipping hudless resource due to format mismatch! hudless: {}, swapchain: {}",
                         magic_enum::enum_name(_lastHudlessFormat), magic_enum::enum_name(scFfxFormat));

                _lastHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
                _frameResources[fIndex][type] = {};
                return false;
            }
            else
            {
                fResource->validity = FG_ResourceValidity::UntilPresent;
            }
        }

        _noHudless[fIndex] = false;
    }

    // For FSR FG we always copy ValidNow
    if (fResource->validity == FG_ResourceValidity::ValidButMakeCopy)
        fResource->validity = FG_ResourceValidity::ValidNow;

    fResource->validity = (fResource->validity != FG_ResourceValidity::ValidNow || willFlip)
                              ? FG_ResourceValidity::UntilPresent
                              : FG_ResourceValidity::ValidNow;

    // Copy ValidNow
    if (fResource->validity == FG_ResourceValidity::ValidNow)
    {
        ID3D12Resource* copyOutput = nullptr;

        if (_resourceCopy[fIndex].contains(type))
            copyOutput = _resourceCopy[fIndex].at(type);

        if (!CopyResource(inputResource->cmdList, inputResource->resource, &copyOutput, inputResource->state))
        {
            LOG_ERROR("{}, CopyResource error!", magic_enum::enum_name(type));
            return false;
        }

        copyOutput->SetName(std::format(L"_resourceCopy[{}][{}]", fIndex, (UINT) type).c_str());

        _resourceCopy[fIndex][type] = copyOutput;
        fResource->copy = copyOutput;
        fResource->state = D3D12_RESOURCE_STATE_COPY_DEST;
        LOG_TRACE("Made a copy: {:X} of input: {:X}", (size_t) fResource->copy, (size_t) fResource->resource);
    }

    SetResourceReady(type, fIndex);

    // if (inputResource->validity == FG_ResourceValidity::UntilPresent)
    //     SetResourceReady(type, fIndex);
    // else
    //     ResTrack_Dx12::SetResourceCmdList(type, inputResource->cmdList);

    LOG_TRACE("_frameResources[{}][{}]: {:X}", fIndex, magic_enum::enum_name(type), (size_t) fResource->GetResource());
    return true;
}

void FSRFG_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) { _gameCommandQueue = queue; }

void FSRFG_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    _device = InDevice;

    if (_fgCommandAllocator[0] != nullptr)
        return;

    LOG_DEBUG("");

    do
    {
        HRESULT result;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* cmdList = nullptr;
        ID3D12CommandQueue* cmdQueue = nullptr;

        // FG
        for (size_t i = 0; i < BUFFER_COUNT; i++)
        {
            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_fgCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _fgCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _fgCommandAllocator[i]->SetName(std::format(L"_fgCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _fgCommandAllocator[i], (IUnknown**) &allocator))
                _fgCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _fgCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_fgCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _fgCommandList[i]->SetName(std::format(L"_fgCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _fgCommandList[i], (IUnknown**) &cmdList))
                _fgCommandList[i] = cmdList;

            result = _fgCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_fgCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uiCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _uiCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _uiCommandAllocator[i]->SetName(std::format(L"_uiCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandAllocator[i], (IUnknown**) &allocator))
                _uiCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uiCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_uiCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _uiCommandList[i]->SetName(std::format(L"_uiCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandList[i], (IUnknown**) &cmdList))
                _uiCommandList[i] = cmdList;

            result = _uiCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_uiCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_uiFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_uiFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create UI fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_uiFenceEvent == nullptr)
            {
                _uiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_uiFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for UI fence failed");
                    break;
                }
            }

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_scCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _scCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _scCommandAllocator[i]->SetName(std::format(L"_scCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandAllocator[i], (IUnknown**) &allocator))
                _scCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _scCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_scCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _scCommandList[i]->SetName(std::format(L"_scCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandList[i], (IUnknown**) &cmdList))
                _scCommandList[i] = cmdList;

            result = _scCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_scCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_scFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_scFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create SC fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_scFenceEvent == nullptr)
            {
                _scFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_scFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for SC fence failed");
                    break;
                }
            }
        }

    } while (false);
}

bool FSRFG_Dx12::Present()
{
    auto fIndex = GetIndexWillBeDispatched();

    if (Config::Instance()->FGDrawUIOverFG.value_or_default())
    {
        auto ui = GetResource(FG_ResourceType::UIColor, fIndex);
        if (ui)
        {
            LOG_DEBUG("UI[{}] resource: {:X}, copy: {}", fIndex, (size_t) ui->resource, (size_t) ui->copy);
            if (_renderUI.get() == nullptr)
            {
                _renderUI = std::make_unique<RUI_Dx12>("RenderUI", _device,
                                                       Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
            }
            else
            {
                if (Config::Instance()->FGUIPremultipliedAlpha.value_or_default() != _renderUI->IsPreMultipliedAlpha())
                {
                    LOG_INFO("UI premultiplied alpha changed, recreating RenderUI");
                    _renderUI = std::make_unique<RUI_Dx12>(
                        "RenderUI", _device, Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
                }
                else if (_renderUI->IsInit())
                {
                    auto commandList = GetSCCommandList(fIndex);
                    _renderUI->Dispatch((IDXGISwapChain3*) _swapChain, commandList, ui->GetResource(), ui->state);
                }
            }
        }
        else if (!ui)
        {
            LOG_WARN("UI resource is nullptr");
        }
    }

    const bool shouldPause = (_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData;
    const bool batchPrepare = !shouldPause && _roiContextActive &&
                              Config::Instance()->FSRFGROIBatchPrepare.value_or_default();
    _deferredPrepareIndex = -1;
    const bool prepareResult = batchPrepare ? Dispatch(true) : false;

    // if (IsActive() && !IsPaused())
    {
        ID3D12CommandList* batchedLists[3] {};
        UINT batchedListCount = 0;
        bool signalUiFence = false;

        if (_uiCommandListResetted[fIndex])
        {
            LOG_DEBUG("Closing _uiCommandList[{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
            auto closeResult = _uiCommandList[fIndex]->Close();

            if (closeResult == S_OK)
            {
                if (batchPrepare && prepareResult)
                    batchedLists[batchedListCount++] = _uiCommandList[fIndex];
                else
                    _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
            }
            else
                LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            signalUiFence = true;
            _uiCommandListResetted[fIndex] = false;
        }

        if (_scCommandListResetted[fIndex])
        {
            LOG_DEBUG("Closing _scCommandList[{}]: {:X}", fIndex, (size_t) _scCommandList[fIndex]);
            auto closeResult = _scCommandList[fIndex]->Close();

            if (closeResult == S_OK)
            {
                if (batchPrepare && prepareResult)
                    batchedLists[batchedListCount++] = _scCommandList[fIndex];
                else
                    _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_scCommandList[fIndex]);
            }
            else
                LOG_ERROR("_scCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _scCommandListResetted[fIndex] = false;
        }

        if (batchPrepare && prepareResult && _deferredPrepareIndex >= 0 &&
            _deferredPrepareIndex < BUFFER_COUNT && _waitingExecute[_deferredPrepareIndex])
        {
            batchedLists[batchedListCount++] = _fgCommandList[_deferredPrepareIndex];
        }

        if (batchedListCount > 0)
        {
            LOG_DEBUG("Executing {} batched callback/UI/Prepare command lists", batchedListCount);
            _gameCommandQueue->ExecuteCommandLists(batchedListCount, batchedLists);
            if (_deferredPrepareIndex >= 0)
                SetExecuted(_deferredPrepareIndex);
        }

        if (signalUiFence)
            _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);
    }

    if (shouldPause)
    {
        LOG_DEBUG("Pausing FG");
        Deactivate();
        _waitingNewFrameData = true;
        return false;
    }

    _fgFramePresentId++;

    return batchPrepare ? prepareResult : Dispatch();
}
