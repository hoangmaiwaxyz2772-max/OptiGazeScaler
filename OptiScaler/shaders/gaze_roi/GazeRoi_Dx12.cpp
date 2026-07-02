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
    int _Pad1;
    int _Pad2;
};

Texture2D<float4> LowResColor : register(t0);
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
    float4 peripheral = LowResColor.SampleLevel(LinearClampSampler, uv, 0.0f);
    float4 roi = DlssRoiOutput.Load(int3(p, 0));
    float alpha = RoiAlpha((int2)p);
    float4 result = lerp(peripheral, roi, alpha);

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
        LOG_ERROR("[{}] CreateComputeShader failed", _name);
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

void GazeRoi_Dx12::SetDlssOutputState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state)
{
    Shader_Dx12::SetBufferState(commandList, state, _dlssOutput, &_dlssOutputState);
}

bool GazeRoi_Dx12::Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* lowResColor,
                            ID3D12Resource* finalOutput, const GazeRoiConstants& constants)
{
    if (!_init || _device == nullptr || commandList == nullptr || lowResColor == nullptr || finalOutput == nullptr ||
        _dlssOutput == nullptr)
    {
        return false;
    }

    _counter = (_counter + 1) % GAZE_ROI_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    try
    {
        CreateShaderResourceView(_device, lowResColor, currentHeap.GetSrvCPU(0));
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
        LOG_ERROR("[{}] Failed to create constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetComputeRootSignature(_rootSignature);
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

    _init = InitHeaps(device, _frameHeaps, GAZE_ROI_NUM_OF_HEAPS);
}

GazeRoi_Dx12::~GazeRoi_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < GAZE_ROI_NUM_OF_HEAPS; i++)
        _frameHeaps[i].ReleaseHeaps();

    SAFE_RELEASE(_dlssOutput);
}
