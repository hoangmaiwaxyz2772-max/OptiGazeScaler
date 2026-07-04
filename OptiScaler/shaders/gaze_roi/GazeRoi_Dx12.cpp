#include "pch.h"
#include "GazeRoi_Dx12.h"

static const char* gazeRoiShader = R"(
cbuffer Params : register(b0)
{
    int _SrcWidth;
    int _SrcHeight;
    int _DstWidth;
    int _DstHeight;
    int _RoiX;
    int _RoiY;
    int _RoiWidth;
    int _RoiHeight;
    int _FeatherPx;
    int _DebugBorderPx;
    int _PeripheralBlur;
    float _PeripheralBlurRadius;
    int _PeripheralJitterCancel;
    int _PeripheralJitterSign;
    float _JitterOffsetX;
    float _JitterOffsetY;
    int _PeripheralTemporal;
    float _PeripheralTemporalCurrentWeight;
    float _PeripheralTemporalReactiveScale;
    int _PeripheralTemporalInitialized;
};

Texture2D<float4> PeripheralColorTexture : register(t0);
Texture2D<float4> DlssRoiOutput : register(t1);
RWTexture2D<float4> FinalOutput : register(u0);
SamplerState LinearClampSampler : register(s0);

float RoiAlpha(int2 p)
{
    if (p.x < _RoiX || p.y < _RoiY || p.x >= _RoiX + _RoiWidth || p.y >= _RoiY + _RoiHeight)
        return 0.0f;

    if (_FeatherPx <= 0)
        return 1.0f;

    int left = p.x - _RoiX;
    int right = (_RoiX + _RoiWidth - 1) - p.x;
    int top = p.y - _RoiY;
    int bottom = (_RoiY + _RoiHeight - 1) - p.y;

    if (_RoiX <= 0)
        left = _FeatherPx;
    if (_RoiY <= 0)
        top = _FeatherPx;
    if (_RoiX + _RoiWidth >= _DstWidth)
        right = _FeatherPx;
    if (_RoiY + _RoiHeight >= _DstHeight)
        bottom = _FeatherPx;

    int edgeDistance = min(min(left, right), min(top, bottom));
    return saturate((float)edgeDistance / (float)_FeatherPx);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_DstWidth || p.y >= (uint)_DstHeight)
        return;

    float2 uv = (float2(p) + 0.5f) / float2((float)_DstWidth, (float)_DstHeight);
    float alpha = RoiAlpha((int2)p);
    float4 result = float4(0.0f, 0.0f, 0.0f, 1.0f);

    if (alpha >= 1.0f)
    {
        result = DlssRoiOutput.Load(int3(p, 0));
    }
    else
    {
        float4 peripheral = PeripheralColorTexture.SampleLevel(LinearClampSampler, uv, 0.0f);
        if (alpha > 0.0f)
        {
            float4 roi = DlssRoiOutput.Load(int3(p, 0));
            result = lerp(peripheral, roi, alpha);
        }
        else
        {
            result = peripheral;
        }
    }

    if (_DebugBorderPx > 0 && p.x >= (uint)_RoiX && p.y >= (uint)_RoiY &&
        p.x < (uint)(_RoiX + _RoiWidth) && p.y < (uint)(_RoiY + _RoiHeight))
    {
        int left = (int)p.x - _RoiX;
        int right = (_RoiX + _RoiWidth - 1) - (int)p.x;
        int top = (int)p.y - _RoiY;
        int bottom = (_RoiY + _RoiHeight - 1) - (int)p.y;
        int edgeDistance = min(min(left, right), min(top, bottom));
        if (edgeDistance < _DebugBorderPx)
            result = float4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    FinalOutput[p] = result;
}
)";

static const char* gazeRoiPeripheralShader = R"(
cbuffer Params : register(b0)
{
    int _SrcWidth;
    int _SrcHeight;
    int _DstWidth;
    int _DstHeight;
    int _RoiX;
    int _RoiY;
    int _RoiWidth;
    int _RoiHeight;
    int _FeatherPx;
    int _DebugBorderPx;
    int _PeripheralBlur;
    float _PeripheralBlurRadius;
    int _PeripheralJitterCancel;
    int _PeripheralJitterSign;
    float _JitterOffsetX;
    float _JitterOffsetY;
    int _PeripheralTemporal;
    float _PeripheralTemporalCurrentWeight;
    float _PeripheralTemporalReactiveScale;
    int _PeripheralTemporalInitialized;
};

Texture2D<float4> LowResColor : register(t0);
Texture2D<float4> PeripheralHistory : register(t1);
RWTexture2D<float4> PeripheralOutput : register(u0);
SamplerState LinearClampSampler : register(s0);

float4 SampleSource(float2 uv)
{
    if (_PeripheralJitterCancel != 0)
    {
        float sign = _PeripheralJitterSign < 0 ? -1.0f : 1.0f;
        uv += sign * float2(_JitterOffsetX, _JitterOffsetY) / float2((float)_SrcWidth, (float)_SrcHeight);
    }

    return LowResColor.SampleLevel(LinearClampSampler, uv, 0.0f);
}

float4 CurrentPeripheral(float2 uv)
{
    if (_PeripheralBlur == 0 || _PeripheralBlurRadius <= 0.0f)
        return SampleSource(uv);

    float2 stepUv = _PeripheralBlurRadius / float2((float)_SrcWidth, (float)_SrcHeight);

    float4 color = SampleSource(uv) * 4.0f;
    color += SampleSource(uv + stepUv * float2(-1.0f, 0.0f)) * 2.0f;
    color += SampleSource(uv + stepUv * float2(1.0f, 0.0f)) * 2.0f;
    color += SampleSource(uv + stepUv * float2(0.0f, -1.0f)) * 2.0f;
    color += SampleSource(uv + stepUv * float2(0.0f, 1.0f)) * 2.0f;
    color += SampleSource(uv + stepUv * float2(-1.0f, -1.0f));
    color += SampleSource(uv + stepUv * float2(1.0f, -1.0f));
    color += SampleSource(uv + stepUv * float2(-1.0f, 1.0f));
    color += SampleSource(uv + stepUv * float2(1.0f, 1.0f));
    return color * (1.0f / 16.0f);
}

void CurrentNeighborhood(float2 uv, out float3 minColor, out float3 maxColor)
{
    float2 stepUv = 1.0f / float2((float)_SrcWidth, (float)_SrcHeight);
    minColor = float3(1e20f, 1e20f, 1e20f);
    maxColor = float3(-1e20f, -1e20f, -1e20f);

    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            float3 c = SampleSource(uv + stepUv * float2((float)x, (float)y)).rgb;
            minColor = min(minColor, c);
            maxColor = max(maxColor, c);
        }
    }
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_SrcWidth || p.y >= (uint)_SrcHeight)
        return;

    float2 uv = (float2(p) + 0.5f) / float2((float)_SrcWidth, (float)_SrcHeight);
    float4 current = CurrentPeripheral(uv);

    if (_PeripheralTemporal == 0 || _PeripheralTemporalInitialized == 0)
    {
        PeripheralOutput[p] = current;
        return;
    }

    float3 minColor;
    float3 maxColor;
    CurrentNeighborhood(uv, minColor, maxColor);

    float4 history = PeripheralHistory.Load(int3(p, 0));
    float3 clippedHistory = clamp(history.rgb, minColor, maxColor);
    float baseWeight = saturate(_PeripheralTemporalCurrentWeight);
    float reactiveWeight = saturate(max(max(abs(current.r - clippedHistory.r), abs(current.g - clippedHistory.g)),
                                     abs(current.b - clippedHistory.b)) *
                                    _PeripheralTemporalReactiveScale);
    float currentWeight = max(baseWeight, reactiveWeight);
    PeripheralOutput[p] = float4(lerp(clippedHistory, current.rgb, currentWeight), current.a);
}
)";

static const char* gazeRoiMvShader = R"(
cbuffer Params : register(b0)
{
    int _Width;
    int _Height;
    float _RawOffsetX;
    float _RawOffsetY;
};

Texture2D<float2> SourceMotionVectors : register(t0);
RWTexture2D<float2> PatchedMotionVectors : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_Width || p.y >= (uint)_Height)
        return;

    float2 mv = SourceMotionVectors.Load(int3(p, 0));
    mv += float2(_RawOffsetX, _RawOffsetY);
    PatchedMotionVectors[p] = mv;
}
)";

static bool IsSupportedMotionVectorFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS:
        return true;
    default:
        return false;
    }
}

bool GazeRoiMvPatch_Dx12::CreatePatchedResource(ID3D12Device* device, ID3D12Resource* motionVectorTemplate,
                                                D3D12_RESOURCE_STATES initialState)
{
    if (device == nullptr || motionVectorTemplate == nullptr)
        return false;

    auto desc = motionVectorTemplate->GetDesc();
    if (!IsSupportedMotionVectorFormat(desc.Format))
    {
        LOG_WARN("[{}] Unsupported MV format for ROI patch: {}", _name, static_cast<uint32_t>(desc.Format));
        return false;
    }

    desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    if (_patchedMotionVectors != nullptr)
    {
        auto oldDesc = _patchedMotionVectors->GetDesc();
        if (oldDesc.Width == desc.Width && oldDesc.Height == desc.Height && oldDesc.Format == desc.Format &&
            oldDesc.Flags == desc.Flags)
        {
            return true;
        }

        SAFE_RELEASE(_patchedMotionVectors);
    }

    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags {};
    HRESULT hr = motionVectorTemplate->GetHeapProperties(&heapProperties, &heapFlags);
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] GetHeapProperties result: {:X}", _name, (UINT64)hr);
        return false;
    }

    hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                         IID_PPV_ARGS(&_patchedMotionVectors));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource result: {:X}", _name, (UINT64)hr);
        return false;
    }

    _patchedMotionVectors->SetName(L"GazeRoi_Patched_MotionVectors");
    _patchedMotionVectorsState = initialState;
    LOG_INFO("[{}] Created patched MV resource: {}x{}", _name, desc.Width, desc.Height);
    return true;
}

void GazeRoiMvPatch_Dx12::SetPatchedState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state)
{
    Shader_Dx12::SetBufferState(commandList, state, _patchedMotionVectors, &_patchedMotionVectorsState);
}

bool GazeRoiMvPatch_Dx12::Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceMotionVectors,
                                   const GazeRoiMvConstants& constants)
{
    if (!_init || _device == nullptr || commandList == nullptr || sourceMotionVectors == nullptr ||
        _patchedMotionVectors == nullptr)
    {
        return false;
    }

    _counter = (_counter + 1) % GAZE_ROI_MV_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    try
    {
        CreateShaderResourceView(_device, sourceMotionVectors, currentHeap.GetSrvCPU(0));
        CreateUnorderedAccessView(_device, _patchedMotionVectors, currentHeap.GetUavCPU(0), 0);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[{}] Failed to create MV patch descriptors: {}", _name, e.what());
        return false;
    }

    if (!CreateConstantsBuffer(_device, _constantBuffer, constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{}] Failed to create constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetComputeRootSignature(_rootSignature);
    commandList->SetPipelineState(_pipelineState);
    commandList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    const UINT dispatchWidth = (constants.width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (constants.height + _numThreadsY - 1) / _numThreadsY;
    commandList->Dispatch(dispatchWidth, dispatchHeight, 1);
    return true;
}

GazeRoiMvPatch_Dx12::GazeRoiMvPatch_Dx12(std::string name, ID3D12Device* device) : Shader_Dx12(name, device)
{
    if (device == nullptr)
    {
        LOG_ERROR("[{}] Device is nullptr", _name);
        return;
    }

    if (!SetupRootSignature(device, 1, 1, 1))
    {
        LOG_ERROR("[{}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(GazeRoiMvConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&_constantBuffer));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource constants result: {:X}", _name, (UINT64)hr);
        return;
    }

    ID3DBlob* shaderBlob = CompileShader(gazeRoiMvShader, "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
    {
        LOG_ERROR("[{}] CompileShader failed", _name);
        return;
    }

    bool psoCreated = CreateComputeShader(device, _rootSignature, &_pipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    if (!psoCreated)
    {
        LOG_ERROR("[{}] CreateComputeShader composite failed", _name);
        return;
    }

    _init = InitHeaps(device, _frameHeaps, GAZE_ROI_MV_NUM_OF_HEAPS);
}

GazeRoiMvPatch_Dx12::~GazeRoiMvPatch_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < GAZE_ROI_MV_NUM_OF_HEAPS; i++)
        _frameHeaps[i].ReleaseHeaps();

    SAFE_RELEASE(_patchedMotionVectors);
}

bool GazeRoi_Dx12::CreateDlssOutputResource(ID3D12Device* device, ID3D12Resource* outputTemplate,
                                            D3D12_RESOURCE_STATES initialState)
{
    if (device == nullptr || outputTemplate == nullptr)
        return false;

    auto desc = outputTemplate->GetDesc();
    desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    if (_dlssOutput != nullptr)
    {
        auto oldDesc = _dlssOutput->GetDesc();
        if (oldDesc.Width == desc.Width && oldDesc.Height == desc.Height && oldDesc.Format == desc.Format &&
            oldDesc.Flags == desc.Flags)
        {
            return true;
        }

        SAFE_RELEASE(_dlssOutput);
    }

    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags {};
    HRESULT hr = outputTemplate->GetHeapProperties(&heapProperties, &heapFlags);
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] GetHeapProperties result: {:X}", _name, (UINT64)hr);
        return false;
    }

    hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                         IID_PPV_ARGS(&_dlssOutput));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource result: {:X}", _name, (UINT64)hr);
        return false;
    }

    _dlssOutput->SetName(L"GazeRoi_DLSS_Output");
    _dlssOutputState = initialState;
    LOG_INFO("[{}] Created DLSS ROI output: {}x{}", _name, desc.Width, desc.Height);
    return true;
}

bool GazeRoi_Dx12::CreatePeripheralResource(ID3D12Device* device, ID3D12Resource* colorTemplate,
                                            D3D12_RESOURCE_STATES initialState)
{
    if (device == nullptr || colorTemplate == nullptr)
        return false;

    auto desc = colorTemplate->GetDesc();
    desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    const bool hasAnyResource =
        _peripheralOutput != nullptr || _peripheralHistory[0] != nullptr || _peripheralHistory[1] != nullptr;
    const bool hasAllResources = _peripheralOutput != nullptr && _peripheralHistory[0] != nullptr &&
                                 _peripheralHistory[1] != nullptr;
    if (hasAllResources)
    {
        auto oldDesc = _peripheralOutput->GetDesc();
        auto oldHistoryDesc = _peripheralHistory[0]->GetDesc();
        if (oldDesc.Width == desc.Width && oldDesc.Height == desc.Height && oldDesc.Format == desc.Format &&
            oldDesc.Flags == desc.Flags && oldHistoryDesc.Width == desc.Width && oldHistoryDesc.Height == desc.Height &&
            oldHistoryDesc.Format == desc.Format && oldHistoryDesc.Flags == desc.Flags)
        {
            return true;
        }
    }

    if (hasAnyResource)
    {
        SAFE_RELEASE(_peripheralOutput);
        SAFE_RELEASE(_peripheralHistory[0]);
        SAFE_RELEASE(_peripheralHistory[1]);
        _peripheralHistoryInitialized = false;
        _peripheralHistoryIndex = 0;
    }

    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags {};
    HRESULT hr = colorTemplate->GetHeapProperties(&heapProperties, &heapFlags);
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] GetHeapProperties peripheral result: {:X}", _name, (UINT64)hr);
        return false;
    }

    hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                         IID_PPV_ARGS(&_peripheralOutput));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource peripheral result: {:X}", _name, (UINT64)hr);
        return false;
    }

    _peripheralOutput->SetName(L"GazeRoi_Peripheral_Output");
    _peripheralOutputState = initialState;

    hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                         IID_PPV_ARGS(&_peripheralHistory[0]));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource peripheral history 0 result: {:X}", _name, (UINT64)hr);
        SAFE_RELEASE(_peripheralOutput);
        return false;
    }

    hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                         IID_PPV_ARGS(&_peripheralHistory[1]));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource peripheral history 1 result: {:X}", _name, (UINT64)hr);
        SAFE_RELEASE(_peripheralOutput);
        SAFE_RELEASE(_peripheralHistory[0]);
        return false;
    }

    _peripheralHistory[0]->SetName(L"GazeRoi_Peripheral_History0");
    _peripheralHistory[1]->SetName(L"GazeRoi_Peripheral_History1");
    _peripheralHistoryState[0] = initialState;
    _peripheralHistoryState[1] = initialState;
    _peripheralHistoryInitialized = false;
    _peripheralHistoryIndex = 0;
    LOG_INFO("[{}] Created peripheral output/history: {}x{}", _name, desc.Width, desc.Height);
    return true;
}

void GazeRoi_Dx12::SetDlssOutputState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state)
{
    Shader_Dx12::SetBufferState(commandList, state, _dlssOutput, &_dlssOutputState);
}

void GazeRoi_Dx12::SetPeripheralOutputState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state)
{
    Shader_Dx12::SetBufferState(commandList, state, _peripheralOutput, &_peripheralOutputState);
}

bool GazeRoi_Dx12::Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* lowResColor,
                            ID3D12Resource* finalOutput, const GazeRoiConstants& constants)
{
    if (!_init || _device == nullptr || commandList == nullptr || lowResColor == nullptr || finalOutput == nullptr ||
        _dlssOutput == nullptr || _peripheralOutput == nullptr || _peripheralHistory[0] == nullptr ||
        _peripheralHistory[1] == nullptr || _peripheralPipelineState == nullptr)
    {
        return false;
    }

    _counter = (_counter + 1) % GAZE_ROI_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];
    FrameDescriptorHeap& peripheralHeap = _peripheralFrameHeaps[_counter];

    GazeRoiConstants peripheralConstants = constants;
    ID3D12Resource* peripheralForComposite = _peripheralOutput;

    if (peripheralConstants.peripheralTemporal != 0)
    {
        const uint32_t nextHistoryIndex = (_peripheralHistoryIndex + 1) % 2;
        const uint32_t previousHistoryIndex = _peripheralHistoryIndex;

        Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    _peripheralHistory[previousHistoryIndex],
                                    &_peripheralHistoryState[previousHistoryIndex]);
        Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    _peripheralHistory[nextHistoryIndex], &_peripheralHistoryState[nextHistoryIndex]);

        peripheralConstants.peripheralTemporalInitialized = _peripheralHistoryInitialized ? 1 : 0;
        peripheralForComposite = _peripheralHistory[nextHistoryIndex];
    }
    else
    {
        _peripheralHistoryInitialized = false;
        Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    _peripheralHistory[_peripheralHistoryIndex],
                                    &_peripheralHistoryState[_peripheralHistoryIndex]);
        SetPeripheralOutputState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    try
    {
        CreateShaderResourceView(_device, lowResColor, peripheralHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, _peripheralHistory[_peripheralHistoryIndex], peripheralHeap.GetSrvCPU(1));
        CreateUnorderedAccessView(_device, peripheralForComposite, peripheralHeap.GetUavCPU(0), 0);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[{}] Failed to create peripheral descriptors: {}", _name, e.what());
        return false;
    }

    if (!CreateConstantsBuffer(_device, _peripheralConstantBuffer, peripheralConstants, peripheralHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{}] Failed to create constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* peripheralHeaps[] = { peripheralHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(peripheralHeaps), peripheralHeaps);
    commandList->SetComputeRootSignature(_rootSignature);
    commandList->SetPipelineState(_peripheralPipelineState);
    commandList->SetComputeRootDescriptorTable(0, peripheralHeap.GetTableGPUStart());

    const UINT peripheralDispatchWidth = (constants.srcWidth + _numThreadsX - 1) / _numThreadsX;
    const UINT peripheralDispatchHeight = (constants.srcHeight + _numThreadsY - 1) / _numThreadsY;
    commandList->Dispatch(peripheralDispatchWidth, peripheralDispatchHeight, 1);

    if (peripheralConstants.peripheralTemporal != 0)
    {
        const uint32_t nextHistoryIndex = (_peripheralHistoryIndex + 1) % 2;
        Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    _peripheralHistory[nextHistoryIndex], &_peripheralHistoryState[nextHistoryIndex]);
        _peripheralHistoryIndex = nextHistoryIndex;
        _peripheralHistoryInitialized = true;
        peripheralForComposite = _peripheralHistory[_peripheralHistoryIndex];
    }
    else
    {
        SetPeripheralOutputState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    try
    {
        CreateShaderResourceView(_device, peripheralForComposite, currentHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, _dlssOutput, currentHeap.GetSrvCPU(1));
        CreateUnorderedAccessView(_device, finalOutput, currentHeap.GetUavCPU(0), 0);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[{}] Failed to create ROI composite descriptors: {}", _name, e.what());
        return false;
    }

    if (!CreateConstantsBuffer(_device, _constantBuffer, constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{}] Failed to create composite constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* compositeHeaps[] = { currentHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(compositeHeaps), compositeHeaps);
    commandList->SetPipelineState(_pipelineState);
    commandList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    const UINT dispatchWidth = (constants.dstWidth + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (constants.dstHeight + _numThreadsY - 1) / _numThreadsY;
    commandList->Dispatch(dispatchWidth, dispatchHeight, 1);
    return true;
}

GazeRoi_Dx12::GazeRoi_Dx12(std::string name, ID3D12Device* device) : Shader_Dx12(name, device)
{
    if (device == nullptr)
    {
        LOG_ERROR("[{}] Device is nullptr", _name);
        return;
    }

    CD3DX12_STATIC_SAMPLER_DESC sampler(0);
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    if (!SetupRootSignature(device, 2, 1, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(GazeRoiConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&_constantBuffer));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource constants result: {:X}", _name, (UINT64)hr);
        return;
    }

    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&_peripheralConstantBuffer));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource peripheral constants result: {:X}", _name, (UINT64)hr);
        return;
    }

    ID3DBlob* shaderBlob = CompileShader(gazeRoiShader, "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
    {
        LOG_ERROR("[{}] CompileShader failed", _name);
        return;
    }

    bool psoCreated = CreateComputeShader(device, _rootSignature, &_pipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    if (!psoCreated)
    {
        LOG_ERROR("[{}] CreateComputeShader failed", _name);
        return;
    }

    shaderBlob = CompileShader(gazeRoiPeripheralShader, "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
    {
        LOG_ERROR("[{}] CompileShader peripheral failed", _name);
        return;
    }

    psoCreated = CreateComputeShader(device, _rootSignature, &_peripheralPipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    if (!psoCreated)
    {
        LOG_ERROR("[{}] CreateComputeShader peripheral failed", _name);
        return;
    }

    _init = InitHeaps(device, _frameHeaps, GAZE_ROI_NUM_OF_HEAPS) &&
            InitHeaps(device, _peripheralFrameHeaps, GAZE_ROI_NUM_OF_HEAPS);
}

GazeRoi_Dx12::~GazeRoi_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < GAZE_ROI_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
        _peripheralFrameHeaps[i].ReleaseHeaps();
    }

    SAFE_RELEASE(_dlssOutput);
    SAFE_RELEASE(_peripheralOutput);
    SAFE_RELEASE(_peripheralHistory[0]);
    SAFE_RELEASE(_peripheralHistory[1]);
    SAFE_RELEASE(_peripheralPipelineState);
    SAFE_RELEASE(_peripheralConstantBuffer);
}
