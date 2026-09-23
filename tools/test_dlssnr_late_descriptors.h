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
    UINT increment, numDescriptors;
    std::map<SIZE_T, ResourceInfo> entries;
    HeapInfo(SIZE_T first, SIZE_T last, UINT stride)
        : begin(first), end(last), increment(stride), numDescriptors(static_cast<UINT>((last-first)/stride)) {}
    bool GetCpuIndex(SIZE_T key, UINT& index) const
    {
        if (key<begin || key>=end || (key-begin)%increment) return false;
        index=static_cast<UINT>((key-begin)/increment); return true;
    }
    ResourceInfo* GetByIndex(SIZE_T index) { return GetByCpuHandle(begin+index*increment); }
    void SetByIndex(SIZE_T index, const ResourceInfo& value) { SetByCpuHandle(begin+index*increment,value); }
    void ClearByIndex(SIZE_T index) { ClearByCpuHandle(begin+index*increment); }
    void CopyByIndex(SIZE_T destination, HeapInfo* source, SIZE_T sourceIndex)
    {
        const auto* value=source ? source->GetByIndex(sourceIndex) : nullptr;
        if (value) SetByIndex(destination,*value); else ClearByIndex(destination);
    }
    ResourceInfo* GetByCpuHandle(SIZE_T key)
    { auto it=entries.find(key); return it==entries.end()?nullptr:&it->second; }
    void SetByCpuHandle(SIZE_T key, const ResourceInfo& value) { entries[key]=value; }
    void ClearByCpuHandle(SIZE_T key) { entries.erase(key); }
};
std::vector<HeapInfo> heaps;
struct ResTrack_Dx12
{
    enum class DescriptorCopyRole { Source, Destination };
    inline static unsigned sourceLookups = 0, destinationLookups = 0;
    static bool IsHudFixActive() { return false; }
    static bool CheckResource(ID3D12Resource*, ResourceInfo* outInfo = nullptr);
    static HeapInfo* GetHeapByCpuHandle(SIZE_T key, DescriptorCopyRole role = DescriptorCopyRole::Destination)
    {
        if (role == DescriptorCopyRole::Source) ++sourceLookups; else ++destinationLookups;
        for (auto& heap:heaps) if(key>=heap.begin && key<heap.end) return &heap;
        return nullptr;
    }
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
unsigned nativeViewCalls = 0;
void o_CopyDescriptors(ID3D12Device* device,UINT dstCount,D3D12_CPU_DESCRIPTOR_HANDLE* dst,UINT* dstSizes,
    UINT srcCount,D3D12_CPU_DESCRIPTOR_HANDLE* src,UINT* srcSizes,D3D12_DESCRIPTOR_HEAP_TYPE type)
{ ++nativeCopyCalls; device->CopyDescriptors(dstCount,dst,dstSizes,srcCount,src,srcSizes,type); }
void o_CopyDescriptorsSimple(ID3D12Device* device,UINT count,D3D12_CPU_DESCRIPTOR_HANDLE dst,
    D3D12_CPU_DESCRIPTOR_HANDLE src,D3D12_DESCRIPTOR_HEAP_TYPE type)
{ ++nativeSimpleCalls; device->CopyDescriptorsSimple(count,dst,src,type); }
void o_CreateRenderTargetView(ID3D12Device* d,ID3D12Resource* r,D3D12_RENDER_TARGET_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h)
{ ++nativeViewCalls; d->CreateRenderTargetView(r,v,h); }
void o_CreateShaderResourceView(ID3D12Device* d,ID3D12Resource* r,D3D12_SHADER_RESOURCE_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h)
{ ++nativeViewCalls; d->CreateShaderResourceView(r,v,h); }
void o_CreateUnorderedAccessView(ID3D12Device* d,ID3D12Resource* r,ID3D12Resource* c,D3D12_UNORDERED_ACCESS_VIEW_DESC* v,D3D12_CPU_DESCRIPTOR_HANDLE h)
{ ++nativeViewCalls; d->CreateUnorderedAccessView(r,c,v,h); }
void LateViewObserved(unsigned, bool) {}
void LateViewStored(unsigned) {}
#include "late-descriptors-production.h"

UINT Run(ID3D12Device* device)
{
    constexpr auto type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    constexpr UINT descriptorCount = 8;
    D3D12_DESCRIPTOR_HEAP_DESC hd {}; hd.Type=type; hd.NumDescriptors=descriptorCount;
    ComPtr<ID3D12DescriptorHeap> cpu,gpu;
    Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&cpu)));
    hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&gpu)));
    auto src=cpu->GetCPUDescriptorHandleForHeapStart(), dst=gpu->GetCPUDescriptorHandleForHeapStart();
    const auto stride=device->GetDescriptorHandleIncrementSize(type);
    heaps={{src.ptr,src.ptr+descriptorCount*stride,stride},{dst.ptr,dst.ptr+descriptorCount*stride,stride}};
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
    const auto rtvStride=device->GetDescriptorHandleIncrementSize(hd.Type);
    heaps.emplace_back(rtv.ptr,rtv.ptr+descriptorCount*rtvStride,rtvStride);
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
    Require(!ResTrack_Dx12::CheckResource(array.Get()), "texture arrays entered single-scene metadata"); ++checks;
    Require(!ResTrack_Dx12::CheckResource(nullptr), "null resource admitted for tracking"); ++checks;
    ResourceInfo admitted {};
    Require(ResTrack_Dx12::CheckResource(texture.Get(), &admitted) && admitted.buffer==texture.Get() &&
            admitted.width==td.Width && admitted.height==td.Height && admitted.format==td.Format &&
            admitted.flags==td.Flags, "resource admission did not return the observed metadata"); ++checks;
    auto depthDesc=td; depthDesc.Format=DXGI_FORMAT_R32_TYPELESS;
    depthDesc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    ComPtr<ID3D12Resource> depth;
    Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&depthDesc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&depth)));
    Require(!ResTrack_Dx12::CheckResource(depth.Get()), "depth-only resource admitted as a scene"); ++checks;
    auto multisampleDesc=td; multisampleDesc.SampleDesc.Count=2;
    multisampleDesc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> multisample;
    Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&multisampleDesc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&multisample)));
    Require(!ResTrack_Dx12::CheckResource(multisample.Get()), "multisampled resource admitted as a single-sample scene"); ++checks;
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
    // Bulk copies preserve valid/null slots while NR is off and only resolve
    // each heap once. Native calls still execute exactly once per API call.
    for (UINT i=0; i<descriptorCount; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle {src.ptr+i*stride};
        ResTrack_Dx12::hkCreateShaderResourceView(device,i%2 ? nullptr : texture.Get(),&view,handle);
    }
    for (bool simple : {false,true})
    {
        for (UINT i=0; i<descriptorCount; ++i) heaps[1].SetByCpuHandle(dst.ptr+i*stride,{texture.Get()});
        ResTrack_Dx12::sourceLookups = ResTrack_Dx12::destinationLookups = 0;
        const auto beforeNative = nativeCopyCalls + nativeSimpleCalls;
        UINT size=descriptorCount;
        if (simple) ResTrack_Dx12::hkCopyDescriptorsSimple(device,size,dst,src,type);
        else ResTrack_Dx12::hkCopyDescriptors(device,1,&dst,&size,1,&src,&size,type);
        Require(nativeCopyCalls+nativeSimpleCalls == beforeNative+1, "Bulk copy native forwarding changed"); ++checks;
        Require(ResTrack_Dx12::sourceLookups==1 && ResTrack_Dx12::destinationLookups==1,
            "Bulk copies must resolve each heap once"); ++checks;
        for (UINT i=0; i<descriptorCount; ++i)
        {
            auto* value=heaps[1].GetByCpuHandle(dst.ptr+i*stride);
            Require(i%2 ? value==nullptr : value && value->buffer==texture.Get(), "Bulk copy metadata/null overwrite");
            ++checks;
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE sameHeapDest {src.ptr+4*stride};
    ResTrack_Dx12::hkCopyDescriptorsSimple(device,4,sameHeapDest,src,type);
    for (UINT i=0; i<4; ++i)
    {
        auto* value=heaps[0].GetByCpuHandle(sameHeapDest.ptr+i*stride);
        Require(i%2 ? value==nullptr : value && value->buffer==texture.Get(), "Non-overlapping same-heap copy"); ++checks;
    }
    // Source ranges may change heaps inside one CopyDescriptors call.
    hd.Type=type; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> otherCpu;
    Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&otherCpu)));
    auto other=otherCpu->GetCPUDescriptorHandleForHeapStart();
    heaps.emplace_back(other.ptr,other.ptr+descriptorCount*stride,stride);
    for (UINT i=0; i<descriptorCount; ++i)
        ResTrack_Dx12::hkCreateShaderResourceView(device,texture.Get(),&view,{other.ptr+i*stride});
    D3D12_CPU_DESCRIPTOR_HANDLE sourceRanges[] {src,other,{src.ptr+4*stride}};
    UINT sourceSizes[] {2,4,2};
    D3D12_CPU_DESCRIPTOR_HANDLE destinationRanges[] {dst,{dst.ptr+3*stride}};
    UINT destinationSizes[] {3,5};
    ResTrack_Dx12::hkCopyDescriptors(device,2,destinationRanges,destinationSizes,3,sourceRanges,sourceSizes,type);
    for (UINT i=0; i<descriptorCount; ++i)
    {
        auto* value=heaps[1].GetByCpuHandle(dst.ptr+i*stride);
        Require(i==1 || i==7 ? value==nullptr : value && value->buffer==texture.Get(), "Multiple source/destination ranges");
        ++checks;
    }
    // Empty ranges must not consume a source slot or shift the copied views.
    D3D12_CPU_DESCRIPTOR_HANDLE zeroSources[] {other,src,other,other,{src.ptr+4*stride}};
    UINT zeroSourceSizes[] {0,2,0,4,2};
    D3D12_CPU_DESCRIPTOR_HANDLE zeroDestinations[] {dst,dst,{dst.ptr+3*stride},{dst.ptr+3*stride}};
    UINT zeroDestinationSizes[] {0,3,0,5};
    const auto beforeEmptyRanges = nativeCopyCalls;
    ResTrack_Dx12::hkCopyDescriptors(device,4,zeroDestinations,zeroDestinationSizes,5,zeroSources,zeroSourceSizes,type);
    Require(nativeCopyCalls==beforeEmptyRanges+1, "Empty ranges changed native forwarding"); ++checks;
    for (UINT i=0; i<descriptorCount; ++i)
    {
        auto* value=heaps[1].GetByCpuHandle(dst.ptr+i*stride);
        Require(i==1 || i==7 ? value==nullptr : value && value->buffer==texture.Get(), "Empty range shifted descriptor metadata");
        ++checks;
    }
    // A valid native source heap may have no OptiScaler metadata. Clear stale
    // destination records without skipping the real D3D12 operation.
    heaps.pop_back();
    ResTrack_Dx12::hkCopyDescriptorsSimple(device,descriptorCount,dst,other,type);
    Require(heaps[1].entries.empty(), "Untracked source heap must clear the entire destination range"); ++checks;
    const auto beforeZero = nativeSimpleCalls;
    ResTrack_Dx12::sourceLookups = ResTrack_Dx12::destinationLookups = 0;
    ResTrack_Dx12::hkCopyDescriptorsSimple(device,0,dst,src,type);
    Require(nativeSimpleCalls==beforeZero+1 && ResTrack_Dx12::sourceLookups==0 && ResTrack_Dx12::destinationLookups==0,
        "Zero count copy must forward without metadata work"); ++checks;
    ResTrack_Dx12::hkCopyDescriptorsSimple(device,descriptorCount,dst,src,type);
    Config::Instance()->DLSSNREnabled.value=true;
    Require(DLSSNRLatePass::Enabled() && heaps[1].GetByCpuHandle(dst.ptr) &&
        heaps[1].GetByCpuHandle(dst.ptr)->buffer==texture.Get(), "Runtime NR re-enable lost copied metadata");
    ++checks;
    Config::Instance()->DLSSNREnabled.value=false;
    Config::Instance()->DLSSNRLateHudless.value=false;
    heaps.clear();
    std::cout<<"FG-off descriptor metadata checks passed: "<<checks<<" (production copy hooks, late toggle off).\n";
    return checks;
}
}
