#define NOMINMAX
#include <windows.h>
#include <dxgi1_4.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <format>
#include <stdexcept>
#include <vector>
#include "DLSSNRLateColor.h"
#include "DLSSNRCommandState.h"
using Microsoft::WRL::ComPtr;
using DLSSNRLatePass::LateColor;
std::string guideFailure;
void Bypass(const std::string& reason) { guideFailure = reason; }
#define LOG_INFO(...) ((void)0)
#include "late-guide-production.h"
#undef LOG_INFO
void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D HRESULT " + std::to_string(hr)); }
void Require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
#include "test_dlssnr_late_descriptors.h"
#include "test_dlssnr_late_lifecycle.h"
struct Fixture
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 serial = 0;
    Fixture()
    {
        Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC q {}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
        Check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        Check(list->Close()); Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }
    void Begin() { Check(allocator->Reset()); Check(list->Reset(allocator.Get(), nullptr)); }
    void Submit()
    {
        Check(list->Close()); ID3D12CommandList* lists[] {list.Get()}; queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), ++serial));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(fence->SetEventOnCompletion(serial, event));
        DWORD wait = WaitForSingleObject(event, 10000); CloseHandle(event);
        Require(wait == WAIT_OBJECT_0, "GPU timeout");
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE kind)
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(kind); auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        ComPtr<ID3D12Resource> buffer;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
            kind == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&buffer))); return buffer;
    }
    ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format, UINT width, UINT height,
                                  const std::vector<std::byte>& data, UINT bytes, bool uav = false)
    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format,width,height,1,1,1,0,
            uav ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> texture;
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&texture)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {}; UINT64 total = 0;
        device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&total);
        auto upload = Buffer(total,D3D12_HEAP_TYPE_UPLOAD); void* mapped;
        Check(upload->Map(0,nullptr,&mapped));
        for (UINT y=0;y<height;++y) memcpy(static_cast<std::byte*>(mapped)+y*fp.Footprint.RowPitch,
            data.data()+y*width*bytes,width*bytes);
        upload->Unmap(0,nullptr);
        Begin(); D3D12_TEXTURE_COPY_LOCATION src {},dst {};
        src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
        dst.pResource=texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        LateColor::Barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
        Submit();return texture;
    }
    std::vector<std::byte> Read(ID3D12Resource* texture, D3D12_RESOURCE_STATES state, UINT bytes)
    {
        auto desc=texture->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};UINT64 total=0, rowBytes=0;
        device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,&rowBytes,&total);
        Require(rowBytes==desc.Width*bytes && fp.Offset+(desc.Height-1)*fp.Footprint.RowPitch+rowBytes<=total,
                "readback pixel size disagrees with the resource plane footprint");
        auto output=Buffer(total,D3D12_HEAP_TYPE_READBACK);Begin();
        LateColor::Barrier(list.Get(),texture,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src {},dst {};
        src.pResource=texture;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=output.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        LateColor::Barrier(list.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,state);Submit();
        void* mapped;Check(output->Map(0,nullptr,&mapped));std::vector<std::byte> result(desc.Width*desc.Height*bytes);
        for(UINT y=0;y<desc.Height;++y) memcpy(result.data()+y*desc.Width*bytes,
            static_cast<std::byte*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,desc.Width*bytes);
        output->Unmap(0,nullptr);return result;
    }
};


float PQ(float nits)
{
    float p=std::pow(nits/10000.f,2610.f/16384.f);
    return std::pow((3424.f/4096.f+(2413.f/128.f)*p)/(1+(2392.f/128.f)*p),2523.f/32.f);
}
int main()
{
    try
    {
        Fixture f; UINT checks=DescriptorRegression::Run(f.device.Get());
        checks += LifecycleRegression::Run(f.device.Get());
        // Execute the production guide allocation/copy helpers against actual
        // textures, including the packed depth family seen in the game log.
        for (auto format : {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT,
                            DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT})
        {
            const bool depth = format == DXGI_FORMAT_R32G8X24_TYPELESS || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format,2560,1440,1,1,1,0,
                depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE);
            auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            ComPtr<ID3D12Resource> source, copy;
            Check(f.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&source)));
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {}; UINT64 total = 0, rowBytes = 0;
            f.device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,&rowBytes,&total);
            // Depth/stencil plane zero is copied independently. Its footprint
            // can use four bytes per pixel even when the resource format uses eight.
            Require(rowBytes%desc.Width==0, "guide plane has a fractional pixel footprint");
            const UINT bytes = static_cast<UINT>(rowBytes/desc.Width);
            Require((bytes==4 || (depth && bytes==8)) &&
                    fp.Offset+(desc.Height-1)*fp.Footprint.RowPitch+rowBytes<=total,
                    "guide upload exceeds the plane's copyable footprint");
            auto upload = f.Buffer(total,D3D12_HEAP_TYPE_UPLOAD); void* mapped;
            Check(upload->Map(0,nullptr,&mapped));
            for (UINT y=0;y<desc.Height;++y)
                for (UINT x=0;x<desc.Width;++x)
                {
                    const UINT value[2] {0x3e800000u + (x+y)%256, 0};
                    memcpy(static_cast<std::byte*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch+x*bytes,value,bytes);
                }
            upload->Unmap(0,nullptr);
            Require(EnsureGuide(f.device.Get(),source.Get(),copy,depth?"depth":"motion"),"guide allocation failed");
            f.Begin(); D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
            src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
            dst.pResource=source.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            f.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            LateColor::Barrier(f.list.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CopyGuide(f.list.Get(),source.Get(),copy.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            f.Submit();
            const auto before=f.Read(source.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,bytes);
            const auto after=f.Read(copy.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,bytes);
            // Packed depth's unused bits need not survive driver copies.
            for (size_t i=0;i<before.size();i+=bytes)
                Require(memcmp(before.data()+i,after.data()+i,4)==0,"guide values changed during snapshot");
            auto* previous=copy.Get();
            Require(EnsureGuide(f.device.Get(),source.Get(),copy,"reuse") && copy.Get()==previous,"guide reuse failed");
            ++checks;
        }
        {
            auto desc=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16_FLOAT,16,16,2);
            auto hp=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            ComPtr<ID3D12Resource> source, copy;
            Check(f.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&source)));
            Require(!EnsureGuide(f.device.Get(),source.Get(),copy,"motion") &&
                guideFailure.find("motion guide unsupported")!=std::string::npos &&
                guideFailure.find("array=2")!=std::string::npos,"unsupported-guide diagnostic lost resource description");
            ++checks;
        }
        // Same production adapter, real render-target textures without UAV flags.
        for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,
             DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R10G10B10A2_UNORM,
             DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R11G11B10_FLOAT})
        {
            UINT bytes=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
            UINT encoding=format==DXGI_FORMAT_R16G16B16A16_FLOAT?1:format==DXGI_FORMAT_R10G10B10A2_UNORM?2:0;
            for (UINT size : {7u,13u}) for (UINT windowMode : {0u,1u,2u}) for (bool uav : {false,true})
            {
                if (uav && format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R10G10B10A2_UNORM &&
                    format != DXGI_FORMAT_R16G16B16A16_FLOAT) continue;
                std::vector<std::byte> pixels(size*9*bytes);
                for(UINT i=0;i<size*9;++i)
                {
                    if(bytes==8)
                    {
                        std::array<uint16_t,4> c {DirectX::PackedVector::XMConvertFloatToHalf(.5f),
                            DirectX::PackedVector::XMConvertFloatToHalf(2.5375f),
                            DirectX::PackedVector::XMConvertFloatToHalf(12.5f),
                            DirectX::PackedVector::XMConvertFloatToHalf(.25f)};
                        memcpy(pixels.data()+i*bytes,c.data(),bytes);
                    }
                    else
                    {
                        UINT c=0x803399bbu;
                        if(format==DXGI_FORMAT_R10G10B10A2_UNORM)
                        {
                            UINT v=static_cast<UINT>(std::round(PQ(203)*1023));c=v|(v<<10)|(v<<20)|(2u<<30);
                        }
                        if(format==DXGI_FORMAT_R11G11B10_FLOAT) c=0x781e03c0u; // finite representable channels
                        memcpy(pixels.data()+i*bytes,&c,bytes);
                    }
                }
                auto scene=f.Texture(format,size,9,pixels,bytes,uav);LateColor adapter;
                Require(adapter.Ensure(f.device.Get(),scene->GetDesc()),"late adapter initialization");
                Require(adapter.typedOutput==uav,"typed writeback path not selected");
                if (windowMode) Require(adapter.SetRegion(1,2,size-2,5),"window region rejected");
                Require(!adapter.SetRegion(size,0,1,1) && !adapter.SetRegion(0,0,0,9),"invalid region accepted");
                f.Begin();adapter.Read(f.device.Get(),f.list.Get(),scene.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,encoding,windowMode==2);
                adapter.Write(f.list.Get(),scene.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,encoding);f.Submit();
                Require(f.Read(scene.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,bytes)==pixels,"identity writeback changed original pixels");++checks;
                auto canonical=f.Read(adapter.color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,8);
                const auto* half=reinterpret_cast<const uint16_t*>(canonical.data())+(windowMode ? (2*size+1)*4 : 0);
                if(encoding==2) Require(std::abs(DirectX::PackedVector::XMConvertHalfToFloat(half[0])-2.5375f)<.025f,"HDR10 luminance decode");
                if(encoding==1) Require(half[0]==DirectX::PackedVector::XMConvertFloatToHalf(.5f),"scRGB linear decode");
                ++checks;
                // A stand-in model changes one pixel. The rest, and alpha, must
                // remain exactly original; the game consumes the same resource.
                f.Begin(); auto hp=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
                auto patch=f.Buffer(256,D3D12_HEAP_TYPE_UPLOAD);void* data;Check(patch->Map(0,nullptr,&data));
                const uint16_t value[4] {0x3400,0x3400,0x3400,0x3c00};memcpy(data,value,8);patch->Unmap(0,nullptr);
                LateColor::Barrier(f.list.Get(),adapter.color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION src {},dst {};src.pResource=patch.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,1,1,1,256};
                dst.pResource=adapter.color.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                f.list->CopyTextureRegion(&dst,2,3,0,&src,nullptr);
                LateColor::Barrier(f.list.Get(),adapter.color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                adapter.Write(f.list.Get(),scene.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,encoding);f.Submit();
                auto result=f.Read(scene.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,bytes);UINT changed=0;
                for(UINT i=0;i<size*9;++i)
                {
                    if(memcmp(result.data()+i*bytes,pixels.data()+i*bytes,bytes)!=0)
                    {Require(i==3*size+2,"writeback modified a pixel outside the model edit");++changed;}
                    if(bytes==8) Require(memcmp(result.data()+i*bytes+6,pixels.data()+i*bytes+6,2)==0,"FP16 alpha changed");
                    else if(format==DXGI_FORMAT_R10G10B10A2_UNORM)
                        Require((std::to_integer<UINT>(result[i*bytes+3])&0xc0)==(std::to_integer<UINT>(pixels[i*bytes+3])&0xc0),"HDR alpha changed");
                    else if(format!=DXGI_FORMAT_R11G11B10_FLOAT)
                        Require(result[i*bytes+3]==pixels[i*bytes+3],"SDR alpha changed");
                }
                Require(changed==1,"stand-in model edit did not reach original scene");++checks;
            }
        }
        // Restore both bind points and partial root constants, then run an
        // actual game compute consumer after the injected adapter changed them.
        auto scene=f.Texture(DXGI_FORMAT_R8G8B8A8_UNORM,7,9,std::vector<std::byte>(7*9*4,std::byte{64}),4);
        LateColor adapter;Require(adapter.Ensure(f.device.Get(),scene->GetDesc()),"state adapter");
        f.Begin();auto* list=f.list.Get();using namespace DLSSNRCommandState;
        Reset(list,nullptr,true);Pipeline(list,adapter.inputPipeline.Get());
        ID3D12DescriptorHeap* heaps[]{adapter.heap.Get()};Heaps(list,1,heaps);
        Signature(list,true,adapter.root.Get());Signature(list,false,adapter.root.Get());
        Value(list,true,0,Kind::Table,adapter.heap->GetGPUDescriptorHandleForHeapStart().ptr);
        Value(list,false,0,Kind::Table,adapter.heap->GetGPUDescriptorHandleForHeapStart().ptr);
        UINT constants[]{7,9,0,0,adapter.footprint.Footprint.RowPitch,4,0,0,0,0};
        Constants(list,true,1,10,constants,0);Constants(list,false,1,10,constants,0);
        UINT bad=123;Constants(list,true,1,1,&bad,4);Constants(list,true,1,1,constants+4,4);
        Signature(list,true,adapter.root.Get()); // same signature must preserve roots
        auto saved=Snapshot(list);Require(saved.has_value(),"complete state not captured");
        Require(saved->compute.roots[1].constants->size()==10,"partial constants lost earlier writes");++checks;
        adapter.Read(f.device.Get(),list,scene.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,0);
        saved->Restore(list); // input PSO and both root tables restored
        LateColor::Barrier(list,scene.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->Dispatch(1,2,1); // actual later game consumer
        LateColor::Barrier(list,scene.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        f.Submit();auto restored=f.Read(adapter.color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,8);
        Require(reinterpret_cast<const uint16_t*>(restored.data())[0]==DirectX::PackedVector::XMConvertFloatToHalf(64.f/255.f),"later consumer used injected bindings");++checks;
        RenderPass(list,true);Require(!Snapshot(list),"render pass accepted");RenderPass(list,false);
        Invalidate(list);Require(!Snapshot(list),"bundle state accepted");Reset(list);Require(!Snapshot(list),"closed state accepted");checks+=3;
        std::cout<<"Late scene GPU checks passed: "<<checks<<"; SDR/BGRA/sRGB/HDR10/scRGB/R11, original-resource writeback, alpha, state restoration.\n";
        std::cout<<"External NGX and game HUDfix selection are not executed by this fixture.\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
