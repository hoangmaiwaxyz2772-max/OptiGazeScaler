#include <pch.h>

#include "RayReconstructionBypass_Dx12.h"

namespace
{
constexpr UINT RAY_RECONSTRUCTION_BYPASS_CONSTANT_DWORDS = 8;

static const char* rayReconstructionBypassShader = R"(
cbuffer Params : register(b0)
{
    uint _SourceWidth;
    uint _SourceHeight;
    uint _SourceBaseX;
    uint _SourceBaseY;
    uint _OutputWidth;
    uint _OutputHeight;
    uint _OutputBaseX;
    uint _OutputBaseY;
};

Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 outputPixel = id.xy;
    if (outputPixel.x >= _OutputWidth || outputPixel.y >= _OutputHeight ||
        _SourceWidth == 0 || _SourceHeight == 0)
    {
        return;
    }

    uint2 sourceSize = uint2(_SourceWidth, _SourceHeight);
    uint2 outputSize = uint2(_OutputWidth, _OutputHeight);
    uint2 sourcePixel = min(
        uint2((float2(outputPixel) + 0.5f) * float2(sourceSize) / float2(outputSize)),
        sourceSize - 1);

    OutputColor[outputPixel + uint2(_OutputBaseX, _OutputBaseY)] =
        SourceColor.Load(int3(sourcePixel + uint2(_SourceBaseX, _SourceBaseY), 0));
}
)";
} // namespace

bool RayReconstructionBypassDx12::Dispatch(ID3D12GraphicsCommandList* commandList,
                                            ID3D12Resource* sourceColor, ID3D12Resource* output,
                                            const RayReconstructionBypassConstants& constants,
                                            uint32_t frameSlot)
{
    if (!_init || _device == nullptr || commandList == nullptr || sourceColor == nullptr || output == nullptr ||
        frameSlot >= RAY_RECONSTRUCTION_BYPASS_FRAME_SLOTS || constants.sourceWidth == 0 ||
        constants.sourceHeight == 0 || constants.outputWidth == 0 || constants.outputHeight == 0)
    {
        return false;
    }

    const auto sourceDesc = sourceColor->GetDesc();
    const auto outputDesc = output->GetDesc();
    if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        outputDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sourceDesc.SampleDesc.Count != 1 ||
        outputDesc.SampleDesc.Count != 1 ||
        (sourceDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 ||
        (outputDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0 ||
        static_cast<uint64_t>(constants.sourceBaseX) + constants.sourceWidth > sourceDesc.Width ||
        static_cast<uint64_t>(constants.sourceBaseY) + constants.sourceHeight > sourceDesc.Height ||
        static_cast<uint64_t>(constants.outputBaseX) + constants.outputWidth > outputDesc.Width ||
        static_cast<uint64_t>(constants.outputBaseY) + constants.outputHeight > outputDesc.Height)
    {
        LOG_ERROR("[DLSSD_DEBUG] RawColorBypass resource contract invalid: source={}x{} base={},{} active={}x{} "
                  "output={}x{} base={},{} active={}x{} sourceFlags={:X} outputFlags={:X}",
                  sourceDesc.Width, sourceDesc.Height, constants.sourceBaseX, constants.sourceBaseY,
                  constants.sourceWidth, constants.sourceHeight, outputDesc.Width, outputDesc.Height,
                  constants.outputBaseX, constants.outputBaseY, constants.outputWidth, constants.outputHeight,
                  static_cast<uint32_t>(sourceDesc.Flags), static_cast<uint32_t>(outputDesc.Flags));
        return false;
    }

    FrameDescriptorHeap& currentHeap = _frameHeaps[frameSlot];
    try
    {
        CreateShaderResourceView(_device, sourceColor, currentHeap.GetSrvCPU(0));
        CreateUnorderedAccessView(_device, output, currentHeap.GetUavCPU(0), 0);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[DLSSD_DEBUG] RawColorBypass descriptor creation failed: {}", e.what());
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetComputeRootSignature(_rootSignature);
    commandList->SetPipelineState(_pipelineState);
    commandList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());
    commandList->SetComputeRoot32BitConstants(1, RAY_RECONSTRUCTION_BYPASS_CONSTANT_DWORDS, &constants, 0);

    const UINT dispatchWidth = (constants.outputWidth + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (constants.outputHeight + _numThreadsY - 1) / _numThreadsY;
    commandList->Dispatch(dispatchWidth, dispatchHeight, 1);

    const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(output);
    commandList->ResourceBarrier(1, &uavBarrier);
    return true;
}

RayReconstructionBypassDx12::RayReconstructionBypassDx12(std::string name, ID3D12Device* device)
    : Shader_Dx12(std::move(name), device)
{
    if (device == nullptr)
    {
        LOG_ERROR("[DLSSD_DEBUG] RawColorBypass device is null");
        return;
    }

    if (!SetupRootSignatureWithConstants(device, 1, 1, RAY_RECONSTRUCTION_BYPASS_CONSTANT_DWORDS))
    {
        LOG_ERROR("[DLSSD_DEBUG] RawColorBypass root signature creation failed");
        return;
    }

    ID3DBlob* shaderBlob = CompileShader(rayReconstructionBypassShader, "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
    {
        LOG_ERROR("[DLSSD_DEBUG] RawColorBypass shader compilation failed");
        return;
    }

    const bool pipelineCreated = CreateComputeShader(device, _rootSignature, &_pipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    if (!pipelineCreated)
    {
        LOG_ERROR("[DLSSD_DEBUG] RawColorBypass pipeline creation failed");
        return;
    }

    _init = InitHeaps(device, _frameHeaps, RAY_RECONSTRUCTION_BYPASS_FRAME_SLOTS);
}

RayReconstructionBypassDx12::~RayReconstructionBypassDx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (auto& heap : _frameHeaps)
        heap.ReleaseHeaps();
}
