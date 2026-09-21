#pragma once
#include <map>

namespace DescriptorRegression
{
// Minimal host services around the extracted production copy hooks. No FG or
// pending capture exists while the game fills these real D3D12 descriptors.
struct Option { bool value=false; bool value_or_default() const { return value; } };
struct Config
{
    Option DLSSNREnabled, DLSSNRLateHudless, FGAlwaysTrackHeaps;
    Option FGRelaxedResolutionCheck, ForceHDR, UseHDR10;
    Option FGHudfixDisableRTV, FGHudfixDisableSRV, FGHudfixDisableUAV;
    static Config* Instance() { static Config config; return &config; }
};
struct State
{
    bool isShuttingDown=false;
    DXGI_SWAP_CHAIN_DESC currentSwapchainDesc {};
    std::vector<ID3D12Resource*> scBuffers;
    static State& Instance() { static State state; return state; }
};
enum ResourceType { RTV, SRV, UAV };
enum CaptureInfo { CreateRTV, CreateSRV, CreateUAV };
struct ResourceInfo
{
    ID3D12Resource* buffer=nullptr;
    UINT64 width=0;
    UINT height=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE;
    ResourceType type=SRV;
    CaptureInfo captureInfo=CreateSRV;
};
struct HeapInfo
{
    SIZE_T begin, end;
    std::map<SIZE_T, ResourceInfo> entries;
    ResourceInfo* GetByCpuHandle(SIZE_T key)
    { auto it=entries.find(key); return it==entries.end()?nullptr:&it->second; }
    void SetByCpuHandle(SIZE_T key, const ResourceInfo& value) { entries[key]=value; }
    void ClearByCpuHandle(SIZE_T key) { entries.erase(key); }
};
std::vector<HeapInfo> heaps;
struct ResTrack_Dx12
{
    static bool IsHudFixActive() { return false; }
    static bool CheckResource(ID3D12Resource*);
    static void FillResourceInfo(ID3D12Resource*, ResourceInfo*);
    static HeapInfo* GetHeapByCpuHandle(SIZE_T key)
    { for (auto& heap:heaps) if(key>=heap.begin && key<heap.end) return &heap; return nullptr; }
    static HeapInfo* GetHeapByCpuHandleRTV(SIZE_T key) { return GetHeapByCpuHandle(key); }
    static HeapInfo* GetHeapByCpuHandleSRV(SIZE_T key) { return GetHeapByCpuHandle(key); }
    static HeapInfo* GetHeapByCpuHandleUAV(SIZE_T key) { return GetHeapByCpuHandle(key); }
    static void hkCreateRenderTargetView(ID3D12Device*, ID3D12Resource*, D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    static void hkCreateShaderResourceView(ID3D12Device*, ID3D12Resource*, D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    static void hkCreateUnorderedAccessView(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    static void hkCopyDescriptors(ID3D12Device*,UINT,D3D12_CPU_DESCRIPTOR_HANDLE*,UINT*,
        UINT,D3D12_CPU_DESCRIPTOR_HANDLE*,UINT*,D3D12_DESCRIPTOR_HEAP_TYPE);
    static void hkCopyDescriptorsSimple(ID3D12Device*,UINT,D3D12_CPU_DESCRIPTOR_HANDLE,
        D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_DESCRIPTOR_HEAP_TYPE);
};
unsigned nativeCopyCalls = 0, nativeSimpleCalls = 0;
void o_CopyDescriptors(ID3D12Device* device,UINT dstCount,D3D12_CPU_DESCRIPTOR_HANDLE* dst,UINT* dstSizes,
    UINT srcCount,D3D12_CPU_DESCRIPTOR_HANDLE* src,UINT* srcSizes,D3D12_DESCRIPTOR_HEAP_TYPE type)
{ ++nativeCopyCalls; device->CopyDescriptors(dstCount,dst,dstSizes,srcCount,src,srcSizes,type); }
void o_CopyDescriptorsSimple(ID3D12Device* device,UINT count,D3D12_CPU_DESCRIPTOR_HANDLE dst,
    D3D12_CPU_DESCRIPTOR_HANDLE src,D3D12_DESCRIPTOR_HEAP_TYPE type)
{ ++nativeSimpleCalls; device->CopyDescriptorsSimple(count,dst,src,type); }
void o_CreateRenderTargetView(ID3D12Device* d,ID3D12Resource* r,D3D12_RENDER_TARGET_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h)
{ d->CreateRenderTargetView(r,v,h); }
void o_CreateShaderResourceView(ID3D12Device* d,ID3D12Resource* r,D3D12_SHADER_RESOURCE_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h)
{ d->CreateShaderResourceView(r,v,h); }
void o_CreateUnorderedAccessView(ID3D12Device* d,ID3D12Resource* r,ID3D12Resource* c,D3D12_UNORDERED_ACCESS_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h)
{ d->CreateUnorderedAccessView(r,c,v,h); }
void LateViewObserved(unsigned, bool) {}
void LateViewStored(unsigned) {}
#include "late-descriptors-production.h"

UINT Run(ID3D12Device* device)
{
    constexpr auto type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    D3D12_DESCRIPTOR_HEAP_DESC hd {}; hd.Type=type; hd.NumDescriptors=2;
    ComPtr<ID3D12DescriptorHeap> cpu,gpu;
    Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&cpu)));
    hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&gpu)));
    auto src=cpu->GetCPUDescriptorHandleForHeapStart(), dst=gpu->GetCPUDescriptorHandleForHeapStart();
    const auto stride=device->GetDescriptorHandleIncrementSize(type);
    heaps={{src.ptr,src.ptr+2*stride,{}},{dst.ptr,dst.ptr+2*stride,{}}};
    auto td=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM,16,16,1,1,1,0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto hp=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&texture)));
    D3D12_SHADER_RESOURCE_VIEW_DESC view {};view.Format=td.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(texture.Get(),&view,src);
    heaps[0].SetByCpuHandle(src.ptr,{texture.Get()});
    UINT checks=0;
    Config::Instance()->DLSSNREnabled.value=true;
    Config::Instance()->DLSSNRLateHudless.value=true;
    Require(DLSSNRLatePass::Enabled(), "Enabled late NR must admit capture");
    ++checks;
    Config::Instance()->DLSSNRLateHudless.value=false;
    // Real default views are valid with pDesc=nullptr. Metadata must survive
    // creation before any swapchain and creation at a different current size.
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvHeap)));
    auto rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart();
    heaps.push_back({rtv.ptr,rtv.ptr+2*device->GetDescriptorHandleIncrementSize(hd.Type),{}});
    for (UINT currentSize:{0u,8u,16u})
    {
        State::Instance().currentSwapchainDesc.BufferDesc.Width=currentSize;
        State::Instance().currentSwapchainDesc.BufferDesc.Height=currentSize;
        ResTrack_Dx12::hkCreateShaderResourceView(device,texture.Get(),nullptr,src);
        Require(heaps[0].GetByCpuHandle(src.ptr) && heaps[0].GetByCpuHandle(src.ptr)->type==SRV,
            "default SRV metadata lost before swapchain/resize"); ++checks;
        ResTrack_Dx12::hkCreateRenderTargetView(device,texture.Get(),nullptr,rtv);
        Require(heaps[2].GetByCpuHandle(rtv.ptr) && heaps[2].GetByCpuHandle(rtv.ptr)->type==RTV,
            "default RTV metadata lost before swapchain/resize"); ++checks;
        ResTrack_Dx12::hkCreateUnorderedAccessView(device,texture.Get(),nullptr,nullptr,src);
        Require(heaps[0].GetByCpuHandle(src.ptr) && heaps[0].GetByCpuHandle(src.ptr)->type==UAV,
            "default UAV metadata lost before swapchain/resize"); ++checks;
        ResTrack_Dx12::hkCreateShaderResourceView(device,texture.Get(),&view,src);
        Require(heaps[0].GetByCpuHandle(src.ptr) && heaps[0].GetByCpuHandle(src.ptr)->width==16,
            "explicit view metadata filtered by creation-time swapchain size"); ++checks;
    }
    ResTrack_Dx12::hkCreateShaderResourceView(device,nullptr,&view,src);
    Require(!heaps[0].GetByCpuHandle(src.ptr), "null resource retained stale view metadata"); ++checks;
    auto arrayDesc=td; arrayDesc.DepthOrArraySize=2;
    ComPtr<ID3D12Resource> array;
    Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&arrayDesc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&array)));
    ResTrack_Dx12::hkCreateShaderResourceView(device,array.Get(),nullptr,src);
    Require(!heaps[0].GetByCpuHandle(src.ptr), "default array view misclassified as Texture2D"); ++checks;
    Config::Instance()->DLSSNREnabled.value=false;
    State::Instance().currentSwapchainDesc.BufferDesc.Width=8;
    Require(!ResTrack_Dx12::CheckResource(texture.Get()), "ordinary FG size admission changed"); ++checks;
    Config::Instance()->DLSSNREnabled.value=true;
    State::Instance().isShuttingDown=true;
    Require(!ResTrack_Dx12::CheckResource(texture.Get()), "shutdown admitted new metadata"); ++checks;
    State::Instance().isShuttingDown=false;
    ResTrack_Dx12::hkCreateShaderResourceView(device,texture.Get(),&view,src);
    Config::Instance()->DLSSNREnabled.value=false;
    Config::Instance()->DLSSNRLateHudless.value=true;
    Require(!DLSSNRLatePass::Enabled() && DLSSNRLatePass::TrackDescriptors(), "NR off must retain descriptor tracking");
    ++checks;
    for (bool simple:{false,true})
    {
        heaps[1].entries.clear();
        if (simple) ResTrack_Dx12::hkCopyDescriptorsSimple(device,1,dst,src,type);
        else ResTrack_Dx12::hkCopyDescriptors(device,1,&dst,nullptr,1,&src,nullptr,type);
        auto* found=heaps[1].GetByCpuHandle(dst.ptr);
        Require(found && found->buffer==texture.Get(),"descriptor copied before late capture was lost with FG off");
        ++checks;
        // Overwrite a previously known scene view with a valid null descriptor:
        // stale scene metadata must not survive either production copy API.
        D3D12_CPU_DESCRIPTOR_HANDLE empty {src.ptr+stride};
        device->CreateShaderResourceView(nullptr,&view,empty);
        if (simple) ResTrack_Dx12::hkCopyDescriptorsSimple(device,1,dst,empty,type);
        else ResTrack_Dx12::hkCopyDescriptors(device,1,&dst,nullptr,1,&empty,nullptr,type);
        Require(!heaps[1].GetByCpuHandle(dst.ptr),"descriptor overwrite retained stale scene resource");
        ++checks;
    }
    Config::Instance()->DLSSNREnabled.value=true;
    Require(DLSSNRLatePass::Enabled(), "Runtime NR re-enable rejected");
    ++checks;
    Config::Instance()->DLSSNREnabled.value=false;
    Config::Instance()->DLSSNRLateHudless.value=false;
    heaps.clear();
    std::cout<<"FG-off descriptor metadata checks passed: "<<checks<<" (production copy hooks, late toggle off).\n";
    return checks;
}
}
