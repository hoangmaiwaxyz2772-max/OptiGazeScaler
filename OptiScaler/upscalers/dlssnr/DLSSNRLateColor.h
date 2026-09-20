#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3dx/d3dx12.h>
#include <wrl/client.h>
#include <cstring>
#include <string>

namespace DLSSNRLatePass
{
// The output is packed into a copyable buffer. This works for BGRA/sRGB and
// game render targets without UAV flags, and never changes graphics state.
inline constexpr char kLateColorShader[] = R"(
Texture2D<float4> Scene : register(t0);
Texture2D<float4> Processed : register(t1);
RWTexture2D<float4> Linear : register(u0);
RWByteAddressBuffer Packed : register(u1);
#ifdef DLSSNR_LATE_TYPED_OUTPUT
RWTexture2D<float4> SceneOutput : register(u2);
#endif
cbuffer Params : register(b0)
{
    uint Width, Height, Encoding, Format;
    uint Pitch, BytesPerPixel, SRGBView, Padding;
    uint OriginX, OriginY;
};
float3 SRGBToLinear(float3 v)
{
    return float3(v.x <= .04045 ? v.x / 12.92 : pow((v.x + .055) / 1.055, 2.4),
                  v.y <= .04045 ? v.y / 12.92 : pow((v.y + .055) / 1.055, 2.4),
                  v.z <= .04045 ? v.z / 12.92 : pow((v.z + .055) / 1.055, 2.4));
}
float3 LinearToSRGB(float3 v)
{
    v = max(v, 0);
    return float3(v.x <= .0031308 ? v.x * 12.92 : 1.055 * pow(v.x, 1.0/2.4) - .055,
                  v.y <= .0031308 ? v.y * 12.92 : 1.055 * pow(v.y, 1.0/2.4) - .055,
                  v.z <= .0031308 ? v.z * 12.92 : 1.055 * pow(v.z, 1.0/2.4) - .055);
}
float3 PQDecode(float3 v)
{
    float3 p = pow(saturate(v), 32.0/2523.0);
    return 10000.0 * pow(max(p - 3424.0/4096.0, 0) /
        max(2413.0/128.0 - (2392.0/128.0)*p, 1e-6), 16384.0/2610.0);
}
float3 PQEncode(float3 v)
{
    float3 p = pow(saturate(v / 10000.0), 2610.0/16384.0);
    return pow((3424.0/4096.0 + (2413.0/128.0)*p) /
        (1 + (2392.0/128.0)*p), 2523.0/32.0);
}
float4 Raw(uint2 p)
{
#ifdef DLSSNR_LATE_TYPED_OUTPUT
    float4 c = SceneOutput[p];
#else
    float4 c = Scene.Load(int3(p,0));
#endif
    if (SRGBView) c.rgb = LinearToSRGB(c.rgb);
    return c;
}
float3 Decode(float3 c)
{
    if (Encoding == 2u)
        return mul(float3x3(1.660491,-.587641,-.072850,
                           -.124550,1.132900,-.008349,
                           -.018151,-.100579,1.118730), PQDecode(c)) / 80.0;
    return c; // SDR stays encoded; scRGB already uses 80 nits per linear unit.
}
float3 Encode(float3 c)
{
    if (Encoding == 2u)
        return PQEncode(mul(float3x3(.627404,.329283,.043313,
                                    .069097,.919540,.011362,
                                    .016391,.088013,.895595), c) * 80.0);
    return Encoding == 0u ? saturate(c) : c;
}
float3 Canonical(float3 c)
{
    c = Decode(c);
    c = float3(isfinite(c.x) ? c.x : 0, isfinite(c.y) ? c.y : 0, isfinite(c.z) ? c.z : 0);
    return clamp(c, -65504, 65504);
}
uint UFloat(float value, uint mantissaBits)
{
    // R11/R10 unsigned floats share FP16's exponent and have 6/5 mantissa bits.
    uint h = f32tof16(clamp(value, 0, 65024));
    uint shift = 10u - mantissaBits;
    uint rounded = h + ((1u << (shift-1u)) - 1u) + ((h >> shift) & 1u);
    return min(rounded >> shift, (31u << mantissaBits) - 1u);
}
[numthreads(8,8,1)]
void Input(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    id.xy += uint2(OriginX, OriginY);
    float4 c = Raw(id.xy);
    Linear[id.xy] = float4(Canonical(c.rgb), c.a);
}
[numthreads(8,8,1)]
void Output(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    id.xy += uint2(OriginX, OriginY);
    float4 original = Raw(id.xy);
    float3 result = Processed.Load(int3(id.xy,0)).rgb;
    float3 baseline = f16tof32(f32tof16(Canonical(original.rgb)));
    float4 c = original;
    // Preserve untouched pixels without an HDR encode/decode round trip.
    if (any(result != baseline) && all(isfinite(result))) c.rgb = Encode(result);
#ifdef DLSSNR_LATE_TYPED_OUTPUT
    // Match the packed fallback's explicit nearest-even quantization instead
    // of depending on the device's UNORM conversion at exact half steps.
    if (Format == 0u) c = round(saturate(c) * 255.0) / 255.0;
    else if (Format == 2u) c = round(saturate(c) * float4(1023,1023,1023,3)) / float4(1023,1023,1023,3);
    SceneOutput[id.xy] = c;
#else
    uint address = id.y * Pitch + id.x * BytesPerPixel;
    if (Format == 0u || Format == 1u)
    {
        uint4 b = (uint4)round(saturate(Format == 1u ? c.bgra : c) * 255.0);
        Packed.Store(address, b.x | (b.y<<8) | (b.z<<16) | (b.w<<24));
    }
    else if (Format == 2u)
    {
        uint4 b = (uint4)round(saturate(c) * float4(1023,1023,1023,3));
        Packed.Store(address, b.x | (b.y<<10) | (b.z<<20) | (b.w<<30));
    }
    else if (Format == 3u)
    {
        uint4 b = f32tof16(c);
        Packed.Store2(address, uint2(b.x | (b.y<<16), b.z | (b.w<<16)));
    }
    else if (Format == 4u) Packed.Store4(address, asuint(c));
    else Packed.Store(address, UFloat(c.r,6) | (UFloat(c.g,6)<<11) | (UFloat(c.b,5)<<22));
#endif
}
)";

struct LateColor
{
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    Resource color, packed;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> inputPipeline, outputPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> typedOutputPipeline;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT width = 0, height = 0, formatCode = 0, bytes = 0;
    UINT regionX = 0, regionY = 0, regionWidth = 0, regionHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN, srvFormat = DXGI_FORMAT_UNKNOWN;
    bool srgb = false;
    bool typedOutput = false;
    D3D12_RESOURCE_STATES packedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        if (before == after) return;
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
        list->ResourceBarrier(1, &b);
    }
    bool Ensure(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc)
    {
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
            desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.Width > UINT_MAX ||
            (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) return false;
        const bool canWrite = CanWriteTyped(device, desc);
        if (color && packed && heap && root && inputPipeline && outputPipeline && width == desc.Width && height == desc.Height && format == desc.Format && typedOutput == canWrite) return true;
        typedOutput = canWrite;
        format = desc.Format; srvFormat = format; srgb = false;
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM; [[fallthrough]];
        case DXGI_FORMAT_R8G8B8A8_UNORM: formatCode = 0; bytes = 4; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: srgb = true; formatCode = 0; bytes = 4; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: srvFormat = DXGI_FORMAT_B8G8R8A8_UNORM; [[fallthrough]];
        case DXGI_FORMAT_B8G8R8A8_UNORM: formatCode = 1; bytes = 4; break;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: srgb = true; formatCode = 1; bytes = 4; break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: srvFormat = DXGI_FORMAT_R10G10B10A2_UNORM; [[fallthrough]];
        case DXGI_FORMAT_R10G10B10A2_UNORM: formatCode = 2; bytes = 4; break;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: srvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; [[fallthrough]];
        case DXGI_FORMAT_R16G16B16A16_FLOAT: formatCode = 3; bytes = 8; break;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: srvFormat = DXGI_FORMAT_R32G32B32A32_FLOAT; [[fallthrough]];
        case DXGI_FORMAT_R32G32B32A32_FLOAT: formatCode = 4; bytes = 16; break;
        case DXGI_FORMAT_R11G11B10_FLOAT: formatCode = 5; bytes = 4; break;
        default: return false;
        }
        color.Reset(); packed.Reset(); heap.Reset();
        width = static_cast<UINT>(desc.Width); height = desc.Height;
        SetRegion(0, 0, width, height);
        auto properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto colorDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &colorDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&color)))) return false;
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        if (!total || total > UINT_MAX) return false;
        auto buffer = CD3DX12_RESOURCE_DESC::Buffer(total, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&packed)))) return false;
        packedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_DESCRIPTOR_HEAP_DESC hd {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return false;
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        UINT stride = device->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv {};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1; sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        cpu.ptr += stride; device->CreateShaderResourceView(color.Get(), &sv, cpu);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv {};
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; uv.Format = sv.Format;
        cpu.ptr += stride; device->CreateUnorderedAccessView(color.Get(), nullptr, &uv, cpu);
        uv = {}; uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uv.Format = DXGI_FORMAT_R32_TYPELESS;
        uv.Buffer.NumElements = static_cast<UINT>(total / 4); uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        cpu.ptr += stride; device->CreateUnorderedAccessView(packed.Get(), nullptr, &uv, cpu);
        if (!root)
        {
            CD3DX12_DESCRIPTOR_RANGE ranges[2];
            ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
            ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0);
            CD3DX12_ROOT_PARAMETER parameters[2];
            parameters[0].InitAsDescriptorTable(2, ranges);
            parameters[1].InitAsConstants(10, 0);
            CD3DX12_ROOT_SIGNATURE_DESC rd(2, parameters);
            Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
            if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
                FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                    IID_PPV_ARGS(&root)))) return false;
        }
        for (int i = 0; i < 2; ++i)
        {
            auto& pipeline = i == 0 ? inputPipeline : outputPipeline;
            if (pipeline) continue;
            Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
            if (FAILED(D3DCompile(kLateColorShader, sizeof(kLateColorShader)-1, "DLSSNRLateColor", nullptr,
                nullptr, i == 0 ? "Input" : "Output", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                &blob, &errors))) return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
            pd.pRootSignature = root.Get(); pd.CS = {blob->GetBufferPointer(), blob->GetBufferSize()};
            if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)))) return false;
        }
        if (typedOutput && !typedOutputPipeline)
        {
            const std::string source = std::string("#define DLSSNR_LATE_TYPED_OUTPUT 1\n") + kLateColorShader;
            Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd {}; pd.pRootSignature = root.Get();
            if (FAILED(D3DCompile(source.data(), source.size(), "DLSSNRLateTypedOutput", nullptr, nullptr,
                                 "Output", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) return false;
            pd.CS = {blob->GetBufferPointer(), blob->GetBufferSize()};
            if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&typedOutputPipeline)))) return false;
        }
        color->SetName(L"OptiScaler_DLSSNR_LateScene");
        packed->SetName(L"OptiScaler_DLSSNR_LateWriteback");
        return true;
    }
    // Keep allocation coordinates unchanged: NR's ROI/guide contract and the
    // copy footprint still use the original top-left origin. Only the work
    // rectangle changes. Untouched pixels never undergo a format round trip.
    bool SetRegion(UINT x, UINT y, UINT w, UINT h)
    {
        if (!w || !h || UINT64(x) + w > width || UINT64(y) + h > height) return false;
        regionX = x; regionY = y; regionWidth = w; regionHeight = h;
        return true;
    }
    static bool CanWriteTyped(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc)
    {
        if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;
        // Keep sRGB, typeless and R11's explicit packing on the copy fallback.
        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
            desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && desc.Format != DXGI_FORMAT_R32G32B32A32_FLOAT) return false;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support {desc.Format};
        const auto needed = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
        return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
               (support.Support2 & needed) == needed;
    }
    void Bind(ID3D12GraphicsCommandList* list, UINT encoding, bool output, bool fullRead = false)
    {
        ID3D12DescriptorHeap* heaps[] {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root.Get());
        list->SetPipelineState(output ? (typedOutput ? typedOutputPipeline.Get() : outputPipeline.Get()) : inputPipeline.Get());
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        const UINT w = fullRead ? width : regionWidth, h = fullRead ? height : regionHeight;
        UINT constants[] {w, h, encoding, formatCode, footprint.Footprint.RowPitch, bytes, srgb ? 1u : 0u, 0,
                          fullRead ? 0u : regionX, fullRead ? 0u : regionY};
        list->SetComputeRoot32BitConstants(1, 10, constants, 0);
        list->Dispatch((w+7)/8, (h+7)/8, 1);
    }
    void Read(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* scene,
              D3D12_RESOURCE_STATES state, UINT encoding, bool fullRead = false)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv {};
        sv.Format = srvFormat; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(scene, &sv, heap->GetCPUDescriptorHandleForHeapStart());
        SetProcessed(device, color.Get());
        if (typedOutput)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uv {}; uv.Format = format; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            auto handle = heap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += 4 * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            device->CreateUnorderedAccessView(scene, nullptr, &uv, handle);
        }
        Barrier(list, scene, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Bind(list, encoding, false, fullRead);
        auto uav = CD3DX12_RESOURCE_BARRIER::UAV(color.Get()); list->ResourceBarrier(1, &uav);
        Barrier(list, scene, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, state);
    }
    void SetProcessed(ID3D12Device* device, ID3D12Resource* processed)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv {};
        sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        auto handle = heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CreateShaderResourceView(processed, &sv, handle);
    }
    void Write(ID3D12GraphicsCommandList* list, ID3D12Resource* scene,
               D3D12_RESOURCE_STATES state, UINT encoding, ID3D12Resource* processed = nullptr)
    {
        if (!processed) processed = color.Get();
        if (typedOutput)
        {
            Barrier(list, scene, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(list, processed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Bind(list, encoding, true);
            auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(scene); list->ResourceBarrier(1, &barrier);
            Barrier(list, scene, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, state);
            Barrier(list, processed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return;
        }
        Barrier(list, scene, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list, processed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list, packed.Get(), packedState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Bind(list, encoding, true);
        Barrier(list, packed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        packedState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        Barrier(list, scene, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION source {};
        source.pResource = packed.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION target {};
        target.pResource = scene; target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box {regionX, regionY, 0, regionX + regionWidth, regionY + regionHeight, 1};
        list->CopyTextureRegion(&target, regionX, regionY, 0, &source, &box);
        Barrier(list, scene, D3D12_RESOURCE_STATE_COPY_DEST, state);
        Barrier(list, processed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
};
}
