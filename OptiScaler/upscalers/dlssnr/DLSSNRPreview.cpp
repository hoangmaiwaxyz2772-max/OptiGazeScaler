#include <pch.h>
#include "DLSSNRPreview.h"
#include <Config.h>
#include <shaders/Shader_Common.h>
#include <shaders/gaze_roi/GazeRoi_Dx12.h>
#include <menu/menu_common.h>
#include <d3dx/d3dx12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <memory>
#include <mutex>

namespace DLSSNRPreview
{
namespace
{
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
std::atomic<bool> everCaptured { false };
constexpr GUID kColorSpaceKey { 0x7cf320e1, 0x69f8, 0x4873, { 0x90, 0x51, 0xc7, 0xad, 0xa1, 0x64, 0x7c, 0x15 } };
constexpr char kPreviewShader[] = R"(
Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);
cbuffer Display : register(b0) { uint Encoding; float WhiteNits; uint DisplayLinear; };
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID)
{
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float3 SRGBToLinear(float3 v)
{
    return float3(v.x <= .04045 ? v.x / 12.92 : pow((v.x + .055) / 1.055, 2.4),
                  v.y <= .04045 ? v.y / 12.92 : pow((v.y + .055) / 1.055, 2.4),
                  v.z <= .04045 ? v.z / 12.92 : pow((v.z + .055) / 1.055, 2.4));
}
float3 PQ(float3 nits)
{
    float3 p = pow(saturate(nits / 10000.0), 2610.0 / 16384.0);
    return pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) /
               (1.0 + (2392.0 / 128.0) * p), 2523.0 / 32.0);
}
float3 LinearToSRGB(float3 v)
{
    v = max(v, 0);
    return float3(v.x <= .0031308 ? 12.92*v.x : 1.055*pow(v.x, 1.0/2.4)-.055,
                  v.y <= .0031308 ? 12.92*v.y : 1.055*pow(v.y, 1.0/2.4)-.055,
                  v.z <= .0031308 ? 12.92*v.z : 1.055*pow(v.z, 1.0/2.4)-.055);
}
float4 PS(Vertex v) : SV_Target
{
    float3 rgb = Source.SampleLevel(LinearClamp, v.uv, 0).rgb;
    bool3 valid = (asuint(rgb) & 0x7f800000u) != 0x7f800000u;
    rgb = float3(valid.x ? rgb.x : 0, valid.y ? rgb.y : 0, valid.z ? rgb.z : 0);
    if (DisplayLinear != 0)
    {
        if (Encoding == 1) return float4(rgb, 1); // Preserve scRGB HDR and signed gamut.
        if (Encoding == 0) return float4(saturate(LinearToSRGB(rgb)), 1);
        if (Encoding == 3) return float4(saturate(rgb), 1);
        float3 rec2020 = mul(float3x3(.627404, .329283, .043313,
                                     .069097, .919540, .011362,
                                     .016391, .088013, .895595), rgb);
        return float4(PQ(rec2020 * 80.0), 1);
    }
    rgb = saturate(rgb);
    if (Encoding == 0) return float4(rgb, 1); // SDR UNORM: already sRGB encoded.
    rgb = SRGBToLinear(rgb);
    if (Encoding == 1) return float4(rgb * (WhiteNits / 80.0), 1); // scRGB
    if (Encoding == 3) return float4(rgb, 1); // sRGB RTV performs the encoding.
    float3 rec2020 = mul(float3x3(.627404, .329283, .043313,
                                  .069097, .919540, .011362,
                                  .016391, .088013, .895595), rgb);
    return float4(PQ(rec2020 * WhiteNits), 1);
}
)";

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
    list->ResourceBarrier(1, &barrier);
}

struct Image
{
    ComPtr<ID3D12Resource> texture;
    UINT width = 0, height = 0, mode = 0;
    bool displayLinear = false;
    UINT64 serial = 0;
    Clock::time_point captured;
};
struct DrawFrame
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12DescriptorHeap> srv, rtv;
    ComPtr<ID3D12Resource> backbuffer;
    std::shared_ptr<Image> image;
    UINT64 submitted = 0;
};
struct Renderer
{
    std::mutex mutex;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3DBlob> vs, ps;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    IDXGISwapChain* swapchain = nullptr; // identity only, never dereferenced
    UINT64 nextValue = 0;
    bool failed = false;
    std::array<DrawFrame, 3> frames;

    void Collect()
    {
        if (!fence || failed) return;
        const auto done = fence->GetCompletedValue();
        for (auto& frame : frames)
            if (frame.submitted && done >= frame.submitted)
            {
                frame.image.reset();
                frame.backbuffer.Reset();
                frame.submitted = 0;
            }
    }
    bool Idle() const
    {
        for (const auto& frame : frames) if (frame.submitted) return false;
        return true;
    }
    bool Setup(ID3D12Device* dev, ID3D12CommandQueue* cq, DXGI_FORMAT fmt)
    {
        if (failed) return false;
        if (device.Get() != dev || queue.Get() != cq || format != fmt)
        {
            if (!Idle()) return false;
            frames = {};
            pipeline.Reset(); root.Reset(); fence.Reset();
            device = dev; queue = cq; format = fmt; nextValue = 0;
        }
        if (pipeline) return true;
        if (!vs) vs.Attach(CompileShader(kPreviewShader, "VS", "vs_5_0"));
        if (!ps) ps.Attach(CompileShader(kPreviewShader, "PS", "ps_5_0"));
        if (!vs || !ps) return false;
        CD3DX12_DESCRIPTOR_RANGE range;
        range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(3, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC desc(2, params, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = root.Get();
        pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1; pso.RTVFormats[0] = fmt; pso.SampleDesc.Count = 1;
        return SUCCEEDED(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)));
    }
};

// Process lifetime: pending game command lists can outlive feature teardown.
// Image ownership is bounded; callbacks and draw frames retain it through both
// producer and consumer fences. No per-frame allocation once the pool is warm.
struct Shared
{
    std::mutex mutex;
    std::array<std::shared_ptr<Image>, 6> pool;
    std::shared_ptr<Image> latest;
    UINT64 serial = 0;
    Renderer renderer;
};
Shared& Data() { static auto* data = new Shared; return *data; }
}

void Capture(ID3D12Device* device, ID3D12GraphicsCommandList* commands, ID3D12Resource* source,
             D3D12_RESOURCE_STATES state, UINT x, UINT y, UINT width, UINT height, UINT mode, bool displayLinear)
{
    if (mode == 0 || Config::Instance()->DLSSNRPresentPreview.value_or_default() != mode ||
        !device || !commands || !source || !width || !height) return;
    const auto sourceDesc = source->GetDesc();
    if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sourceDesc.SampleDesc.Count != 1 ||
        sourceDesc.DepthOrArraySize != 1 || x > sourceDesc.Width || width > sourceDesc.Width - x ||
        y > sourceDesc.Height || height > sourceDesc.Height - y) return;
    auto& data = Data();
    everCaptured.store(true, std::memory_order_relaxed);
    std::shared_ptr<Image> image;
    {
        std::lock_guard lock(data.mutex);
        for (auto& entry : data.pool)
        {
            if (!entry) entry = std::make_shared<Image>();
            if (entry.use_count() == 1) { image = entry; break; }
        }
        if (!image)
        {
            return;
        }
        ComPtr<ID3D12Device> owner;
        if (image->texture) image->texture->GetDevice(IID_PPV_ARGS(&owner));
        if (!image->texture || owner.Get() != device || image->width != width || image->height != height ||
            image->texture->GetDesc().Format != sourceDesc.Format)
        {
            image->texture.Reset();
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(sourceDesc.Format, width, height, 1, 1);
            auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&image->texture)))) return;
        }
        image->width = width; image->height = height; image->mode = mode;
        image->displayLinear = displayLinear;
        image->serial = ++data.serial; image->captured = Clock::now();
    }
    Barrier(commands, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(commands, image->texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    CD3DX12_TEXTURE_COPY_LOCATION src(source, 0), dst(image->texture.Get(), 0);
    D3D12_BOX box { x, y, 0, x + width, y + height, 1 };
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    Barrier(commands, image->texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Barrier(commands, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    GazeRoiFrameSync::DeferCallback([image]()
    {
        auto& data = Data();
        std::lock_guard lock(data.mutex);
        if (!data.latest || image->serial > data.latest->serial)
        {
            data.latest = image;
        }
    });
}

void SetColorSpace(IDXGISwapChain* swapchain, DXGI_COLOR_SPACE_TYPE colorSpace)
{
    if (swapchain) swapchain->SetPrivateData(kColorSpaceKey, sizeof(colorSpace), &colorSpace);
}

bool Present(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue)
{
    if (!swapchain || !queue || !everCaptured.load(std::memory_order_relaxed))
    {
        return false;
    }
    GazeRoiFrameSync::FlushDeferred();
    auto& data = Data();
    auto& r = data.renderer;
    std::lock_guard drawLock(r.mutex);
    r.Collect();
    const UINT mode = Config::Instance()->DLSSNRPresentPreview.value_or_default();
    if (mode == 0 || !Config::Instance()->DLSSNREnabled.value_or_default() ||
        (mode == 3 && !Config::Instance()->DLSSNRLateHudless.value_or_default()))
    {
        std::lock_guard lock(data.mutex);
        data.latest.reset();
        for (auto& image : data.pool) if (image && image.use_count() == 1) image.reset();
        return false;
    }
    // The legacy in-upscaler menu is part of the image we cover. Temporarily
    // reveal it while open, so preview can always be switched off in the UI.
    if (!Config::Instance()->OverlayMenu.value_or_default() && MenuCommon::IsVisible()) return false;
    std::shared_ptr<Image> image;
    {
        std::lock_guard lock(data.mutex);
        image = data.latest;
    }
    if (!image || image->mode != mode || Clock::now() - image->captured > std::chrono::milliseconds(500))
    {
        return false;
    }
    ComPtr<ID3D12Device> device, sourceDevice;
    ComPtr<IDXGISwapChain3> sc;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
        FAILED(image->texture->GetDevice(IID_PPV_ARGS(&sourceDevice))) || device.Get() != sourceDevice.Get() ||
        FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc))) || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;
    ComPtr<ID3D12Resource> backbuffer;
    if (FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) return false;
    const auto desc = backbuffer->GetDesc();
    DXGI_COLOR_SPACE_TYPE colorSpace = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ?
        DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT size = sizeof(colorSpace);
    swapchain->GetPrivateData(kColorSpaceKey, &size, &colorSpace);
    UINT encoding;
    if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) encoding = 1;
    else if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) encoding = 2;
    else if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) encoding = 0;
    else return false; // Do not silently show incorrect primaries/range/transfer.
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    {
        if (encoding != 0) return false;
        encoding = 3;
    }
    if (!r.Setup(device.Get(), queue, desc.Format)) return false;
    DrawFrame* frame = nullptr;
    for (auto& f : r.frames) if (!f.submitted) { frame = &f; break; }
    if (!frame) return false;
    auto& f = *frame;
    if (!f.allocator)
    {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator.Get(),
                nullptr, IID_PPV_ARGS(&f.commands)))) { f = {}; return false; }
        f.commands->Close();
        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = 1;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&f.srv)))) { f = {}; return false; }
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&f.rtv)))) { f = {}; return false; }
    }
    if (FAILED(f.allocator->Reset()) || FAILED(f.commands->Reset(f.allocator.Get(), r.pipeline.Get()))) return false;
    f.image = image; f.backbuffer = backbuffer;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = image->texture->GetDesc().Format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(image->texture.Get(), &srv, f.srv->GetCPUDescriptorHandleForHeapStart());
    device->CreateRenderTargetView(backbuffer.Get(), nullptr, f.rtv->GetCPUDescriptorHandleForHeapStart());
    auto* list = f.commands.Get();
    Barrier(list, backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Barrier(list, image->texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto rtv = f.rtv->GetCPUDescriptorHandleForHeapStart();
    const float black[4] { 0, 0, 0, 1 };
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    float scale = std::min(static_cast<float>(desc.Width) / image->width, static_cast<float>(desc.Height) / image->height);
    float width = image->width * scale, height = image->height * scale;
    D3D12_VIEWPORT viewport { (static_cast<float>(desc.Width) - width) * .5f,
        (static_cast<float>(desc.Height) - height) * .5f, width, height, 0, 1 };
    D3D12_RECT rect { 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &rect);
    ID3D12DescriptorHeap* heaps[] { f.srv.Get() }; list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(r.root.Get());
    list->SetGraphicsRootDescriptorTable(0, f.srv->GetGPUDescriptorHandleForHeapStart());
    float nits = Config::Instance()->DLSSNRPreviewWhiteNits.value_or_default();
    if (!std::isfinite(nits)) nits = 203.0f;
    struct { UINT encoding; float nits; UINT displayLinear; } constants {
        encoding, std::clamp(nits, 80.0f, 400.0f), image->displayLinear ? 1u : 0u };
    list->SetGraphicsRoot32BitConstants(1, 3, &constants, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    Barrier(list, image->texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    if (FAILED(list->Close())) { f.image.reset(); f.backbuffer.Reset(); return false; }
    ID3D12CommandList* lists[] { list };
    queue->ExecuteCommandLists(1, lists);
    f.submitted = ++r.nextValue;
    r.swapchain = swapchain;
    if (FAILED(queue->Signal(r.fence.Get(), f.submitted)))
    {
        r.failed = true; // Retain all referenced objects if completion cannot be established.
        LOG_ERROR("[DLSSNR_PREVIEW] presentation fence signal failed; preview disabled");
        return false;
    }
    return true;
}

void BeforeResize(IDXGISwapChain* swapchain)
{
    if (!swapchain || !everCaptured.load(std::memory_order_relaxed)) return;
    auto& r = Data().renderer;
    std::lock_guard lock(r.mutex);
    if (!r.fence) return;
    if (r.fence->GetCompletedValue() < r.nextValue)
    {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event)
        {
            if (SUCCEEDED(r.fence->SetEventOnCompletion(r.nextValue, event))) WaitForSingleObject(event, 2000);
            CloseHandle(event);
        }
    }
    r.Collect(); // Keep in-flight references on timeout; DXGI then rejects an unsafe resize.
    if (r.Idle())
    {
        r.frames = {};
        r.swapchain = nullptr;
    }
}
}
