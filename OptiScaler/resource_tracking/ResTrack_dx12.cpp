#include "pch.h"
#include <upscalers/dlssnr/DLSSNRLatePass.h>
#include <upscalers/dlssnr/DLSSNRCommandState.h>
#include <upscalers/dlssnr/DLSSNRMethodHooks.h>
#include <upscalers/dlssnr/DLSSNRPipelineSplit.h>
#include <hooks/D3D12_Hooks.h>
#include "ResTrack_dx12.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <menu/menu_overlay_dx.h>
#include <shaders/gaze_roi/GazeRoi_Dx12.h>

#include <algorithm>
#include <future>

#include <magic_enum_utility.hpp>
#include <include/d3dx/d3dx12.h>
#include <detours/detours.h>

namespace
{
std::string LateAddress(void* address)
{
    MEMORY_BASIC_INFORMATION memory {};
    char path[MAX_PATH] {};
    if (VirtualQuery(address, &memory, sizeof(memory)))
        GetModuleFileNameA(static_cast<HMODULE>(memory.AllocationBase), path, MAX_PATH);
    return std::format("0x{:X} {}+0x{:X}", reinterpret_cast<uintptr_t>(address), path,
        reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(memory.AllocationBase));
}

}

#ifndef STDMETHODCALLTYPE
#include <Unknwn.h> // or <objbase.h> to get STDMETHODCALLTYPE
#endif

// Device hooks for FG
typedef void(STDMETHODCALLTYPE* PFN_CreateRenderTargetView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                            D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CreateShaderResourceView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                              D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                                              D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CreateUnorderedAccessView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                               ID3D12Resource* pCounterResource,
                                                               D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                                               D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CreateDepthStencilView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                            const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CreateConstantBufferView)(ID3D12Device* This,
                                                              const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
                                                              D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

typedef void(STDMETHODCALLTYPE* PFN_CreateSampler)(ID3D12Device* This, const D3D12_SAMPLER_DESC* pDesc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDescriptorHeap)(ID3D12Device* This,
                                                             D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
                                                             REFIID riid, void** ppvHeap);
typedef ULONG(STDMETHODCALLTYPE* PFN_HeapRelease)(ID3D12DescriptorHeap* This);
typedef void(STDMETHODCALLTYPE* PFN_CopyDescriptors)(ID3D12Device* This, UINT NumDestDescriptorRanges,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                                     UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                                     UINT* pSrcDescriptorRangeSizes,
                                                     D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);
typedef void(STDMETHODCALLTYPE* PFN_CopyDescriptorsSimple)(ID3D12Device* This, UINT NumDescriptors,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                                           D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);

// Command list hooks for FG
typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D12GraphicsCommandList* This,
                                                        UINT NumRenderTargetDescriptors,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                                        BOOL RTsSingleHandleToDescriptorRange,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_SetGraphicsRootDescriptorTable)(ID3D12GraphicsCommandList* This,
                                                                    UINT RootParameterIndex,
                                                                    D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_SetComputeRootDescriptorTable)(ID3D12GraphicsCommandList* This,
                                                                   UINT RootParameterIndex,
                                                                   D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                                          UINT InstanceCount, UINT StartIndexLocation,
                                                          INT BaseVertexLocation, UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance,
                                                   UINT InstanceCount, UINT StartVertexLocation,
                                                   UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D12GraphicsCommandList* This, UINT ThreadGroupCountX,
                                              UINT ThreadGroupCountY, UINT ThreadGroupCountZ);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteBundle)(ID3D12GraphicsCommandList* This,
                                                   ID3D12GraphicsCommandList* pCommandList);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Close)(ID3D12GraphicsCommandList* This);

typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue* This, UINT NumCommandLists,
                                                         ID3D12CommandList* const* ppCommandLists);

typedef ULONG(STDMETHODCALLTYPE* PFN_Release)(ID3D12Resource* This);

using CaptureDrawInstanced = DLSSNRMethodHooks::MethodHook<12, PFN_DrawInstanced, true>;
using CaptureDrawIndexedInstanced = DLSSNRMethodHooks::MethodHook<13, PFN_DrawIndexedInstanced, true>;
using CaptureDispatch = DLSSNRMethodHooks::MethodHook<14, PFN_Dispatch, true>;
using CaptureSetComputeRootDescriptorTable = DLSSNRMethodHooks::MethodHook<31, PFN_SetComputeRootDescriptorTable, true>;
using CaptureSetGraphicsRootDescriptorTable = DLSSNRMethodHooks::MethodHook<32, PFN_SetGraphicsRootDescriptorTable, true>;

// Original method calls for device
static PFN_CreateRenderTargetView o_CreateRenderTargetView = nullptr;
static PFN_CreateShaderResourceView o_CreateShaderResourceView = nullptr;
static PFN_CreateUnorderedAccessView o_CreateUnorderedAccessView = nullptr;
static PFN_CreateDepthStencilView o_CreateDepthStencilView = nullptr;
static PFN_CreateConstantBufferView o_CreateConstantBufferView = nullptr;
static PFN_CreateSampler o_CreateSampler = nullptr;

static PFN_CreateDescriptorHeap o_CreateDescriptorHeap = nullptr;
static PFN_HeapRelease o_HeapRelease = nullptr;
static PFN_CopyDescriptors o_CopyDescriptors = nullptr;
static PFN_CopyDescriptorsSimple o_CopyDescriptorsSimple = nullptr;

// Original method calls for command list
static PFN_Dispatch o_Dispatch = nullptr;
static PFN_DrawInstanced o_DrawInstanced = nullptr;
static PFN_DrawIndexedInstanced o_DrawIndexedInstanced = nullptr;
using PFN_LateReset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_LateBeginRenderPass = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC*, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
using PFN_LateEndRenderPass = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*);
using CaptureReset = DLSSNRMethodHooks::MethodHook<10, PFN_LateReset, true>;
using CaptureClose = DLSSNRMethodHooks::MethodHook<9, PFN_Close, true>;
using CaptureBundle = DLSSNRMethodHooks::MethodHook<27, PFN_ExecuteBundle, true>;
using CaptureBeginRenderPass = DLSSNRMethodHooks::MethodHook<68, PFN_LateBeginRenderPass, true>;
using CaptureEndRenderPass = DLSSNRMethodHooks::MethodHook<69, PFN_LateEndRenderPass, true>;
static HRESULT STDMETHODCALLTYPE hkLateReset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator,
                                             ID3D12PipelineState* pipeline)
{
    HRESULT result = CaptureReset::Forward(list, allocator, pipeline);
    if (SUCCEEDED(result))
    {
        const bool ready = ResTrack_Dx12::ObserveCommandList(list);
        DLSSNRCommandState::Reset(list, pipeline, ready && DLSSNRCommandState::CaptureEnabled(DLSSNRLatePass::Enabled()));
    }
    return result;
}
static void STDMETHODCALLTYPE hkLateBeginRenderPass(ID3D12GraphicsCommandList4* list, UINT count,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth,
    D3D12_RENDER_PASS_FLAGS flags)
{
    if (DLSSNRLatePass::Enabled())
    {
        DLSSNRCommandState::RenderPass(list, true);
    }
    CaptureBeginRenderPass::Forward(list, count, targets, depth, flags);
}
static void STDMETHODCALLTYPE hkLateEndRenderPass(ID3D12GraphicsCommandList4* list)
{
    CaptureEndRenderPass::Forward(list);
    if (DLSSNRLatePass::Enabled()) DLSSNRCommandState::RenderPass(list, false);
}

static PFN_ExecuteCommandLists o_ExecuteCommandLists = nullptr;
static PFN_Release o_Release = nullptr;

static PFN_OMSetRenderTargets o_OMSetRenderTargets = nullptr;
static PFN_SetGraphicsRootDescriptorTable o_SetGraphicsRootDescriptorTable = nullptr;
static PFN_SetComputeRootDescriptorTable o_SetComputeRootDescriptorTable = nullptr;

static std::mutex _hudlessTrackMutex;
static ankerl::unordered_dense::map<ID3D12GraphicsCommandList*,
                                    ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo>>
    fgPossibleHudless[BUFFER_COUNT];

// heaps section

// #define USE_SPINLOCK_MUTEX_FOR_HEAP_CREATION

#ifdef USE_SPINLOCK_MUTEX_FOR_HEAP_CREATION
static SpinLock _heapCreationMutex;
#else
static std::mutex _heapCreationMutex;
#endif

static std::vector<std::unique_ptr<HeapInfo>> fgHeaps;

static std::set<void*> _notFoundCmdLists;
static std::unordered_map<FG_ResourceType, void*> _resCmdList[BUFFER_COUNT];

struct HeapCacheTLS
{
    unsigned genSeen = 0;
    HeapInfo* heapPtr = nullptr;
    uint64_t heapVersion = 0;
};

// Copies commonly alternate among several source and destination heaps.
// Keep the hot heaps per thread without changing the generation/version guard.
static thread_local std::array<HeapCacheTLS, 4> cacheCopySource;
static thread_local std::array<HeapCacheTLS, 4> cacheCopyDestination;
static thread_local HeapCacheTLS cacheRTV;
static thread_local HeapCacheTLS cacheCBV;
static thread_local HeapCacheTLS cacheSRV;
static thread_local HeapCacheTLS cacheUAV;
static std::atomic<unsigned> gHeapGeneration { 1 };

static thread_local HeapCacheTLS cacheGR;
static thread_local HeapCacheTLS cacheCR;

bool ResTrack_Dx12::CheckResource(ID3D12Resource* resource, ResourceInfo* outInfo)
{
    if (resource == nullptr || State::Instance().isShuttingDown)
        return false;

    const auto resDesc = resource->GetDesc();

    if (resDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        resDesc.DepthOrArraySize != 1 || resDesc.SampleDesc.Count != 1)
        return false;

    constexpr auto unsupportedFlags =
        D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
        D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
        D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY;
    if ((resDesc.Flags & unsupportedFlags) != 0)
        return false;

    // Descriptor metadata outlives the current swapchain dimensions. The game
    // may create its scene views before the swapchain or before a resize.
    // Apply size/format selection at Hudfix::CheckResource when using the view.
    if (!DLSSNRLatePass::TrackDescriptors())
    {
        auto& s = State::Instance();
        if (resDesc.Height != s.currentSwapchainDesc.BufferDesc.Height ||
            resDesc.Width != s.currentSwapchainDesc.BufferDesc.Width)
        {
            if (!(Config::Instance()->FGRelaxedResolutionCheck.value_or_default() &&
                  resDesc.Height >= s.currentSwapchainDesc.BufferDesc.Height - 32 &&
                  resDesc.Height <= s.currentSwapchainDesc.BufferDesc.Height + 32 &&
                  resDesc.Width >= s.currentSwapchainDesc.BufferDesc.Width - 32 &&
                  resDesc.Width <= s.currentSwapchainDesc.BufferDesc.Width + 32))
                return false;
        }
    }

    if (outInfo != nullptr)
    {
        outInfo->buffer = resource;
        outInfo->width = resDesc.Width;
        outInfo->height = resDesc.Height;
        outInfo->format = resDesc.Format;
        outInfo->flags = resDesc.Flags;
    }

    return true;
}

inline static IID streamlineRiid {};
inline static std::once_flag streamlineRiidInitFlag;

bool ResTrack_Dx12::CheckForRealObject(const std::string functionName, IUnknown* pObject, IUnknown** ppRealObject)
{
    std::call_once(streamlineRiidInitFlag,
                   []() { IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}", &streamlineRiid); });

    auto qResult = pObject->QueryInterface(streamlineRiid, (void**) ppRealObject);

    if (qResult == S_OK && *ppRealObject != nullptr)
    {
        LOG_INFO("{} Streamline proxy found!", functionName);
        (*ppRealObject)->Release();
        return true;
    }

    return false;
}

#pragma region Resource methods

bool ResTrack_Dx12::CreateBufferResource(ID3D12Device* InDevice, ResourceInfo* InSource, D3D12_RESOURCE_STATES InState,
                                         ID3D12Resource** OutResource)
{
    if (InDevice == nullptr || InSource == nullptr || InSource->buffer == nullptr)
        return false;

    if (*OutResource != nullptr)
    {
        auto bufDesc = (*OutResource)->GetDesc();

        if (bufDesc.Width != (UINT64) (InSource->width) || bufDesc.Height != (UINT) (InSource->height) ||
            bufDesc.Format != InSource->format)
        {
            (*OutResource)->Release();
            (*OutResource) = nullptr;
        }
        else
            return true;
    }

    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_HEAP_FLAGS heapFlags;
    HRESULT hr = InSource->buffer->GetHeapProperties(&heapProperties, &heapFlags);

    if (hr != S_OK)
    {
        LOG_ERROR("GetHeapProperties result: {0:X}", (UINT64) hr);
        return false;
    }

    D3D12_RESOURCE_DESC texDesc = InSource->buffer->GetDesc();
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    hr = InDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &texDesc, InState, nullptr,
                                           IID_PPV_ARGS(OutResource));

    if (hr != S_OK)
    {
        LOG_ERROR("CreateCommittedResource result: {0:X}", (UINT64) hr);
        return false;
    }

    (*OutResource)->SetName(L"fgHudlessSCBufferCopy");
    return true;
}

void ResTrack_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
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

#pragma endregion

#pragma region Heap helpers

SIZE_T ResTrack_Dx12::GetGPUHandle(ID3D12Device* This, SIZE_T cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        auto val = fgHeaps[i].get();
        if (fgHeaps[i] != nullptr && val->active && val->cpuStart <= cpuHandle && val->cpuEnd > cpuHandle &&
            val->gpuStart != 0)
        {
            auto incSize = This->GetDescriptorHandleIncrementSize(type);
            auto addr = cpuHandle - val->cpuStart;
            auto index = addr / incSize;
            auto gpuAddr = val->gpuStart + (index * incSize);

            return gpuAddr;
        }
    }

    return NULL;
}

SIZE_T ResTrack_Dx12::GetCPUHandle(ID3D12Device* This, SIZE_T gpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        auto val = fgHeaps[i].get();
        if (fgHeaps[i] != nullptr && val->active && val->gpuStart <= gpuHandle && val->gpuEnd > gpuHandle &&
            val->cpuStart != 0)
        {
            auto incSize = This->GetDescriptorHandleIncrementSize(type);
            auto addr = gpuHandle - val->gpuStart;
            auto index = addr / incSize;
            auto cpuAddr = val->cpuStart + (index * incSize);

            return cpuAddr;
        }
    }

    return NULL;
}

HeapInfo* ResTrack_Dx12::GetHeapByCpuHandleCBV(SIZE_T cpuHandle)
{
    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheCBV.genSeen == currentGen && cacheCBV.heapPtr != nullptr &&
        cacheCBV.heapPtr->version == cacheCBV.heapVersion && cacheCBV.heapPtr->active &&
        cacheCBV.heapPtr->cpuStart <= cpuHandle && cpuHandle < cacheCBV.heapPtr->cpuEnd)
    {
        return cacheCBV.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->cpuStart <= cpuHandle &&
            cpuHandle < fgHeaps[i]->cpuEnd)
        {
            cacheCBV.genSeen = currentGen;
            cacheCBV.heapPtr = fgHeaps[i].get();
            cacheCBV.heapVersion = cacheCBV.heapPtr->version;
            return cacheCBV.heapPtr;
        }
    }

    cacheCBV.heapVersion = 0;
    cacheCBV.heapPtr = nullptr;
    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByCpuHandleRTV(SIZE_T cpuHandle)
{
    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheRTV.genSeen == currentGen && cacheRTV.heapPtr != nullptr &&
        cacheRTV.heapPtr->version == cacheRTV.heapVersion && cacheRTV.heapPtr->active &&
        cacheRTV.heapPtr->cpuStart <= cpuHandle && cpuHandle < cacheRTV.heapPtr->cpuEnd)
    {
        return cacheRTV.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->cpuStart <= cpuHandle &&
            cpuHandle < fgHeaps[i]->cpuEnd)
        {
            cacheRTV.genSeen = currentGen;
            cacheRTV.heapPtr = fgHeaps[i].get();
            cacheRTV.heapVersion = cacheRTV.heapPtr->version;
            return cacheRTV.heapPtr;
        }
    }

    cacheRTV.heapVersion = 0;
    cacheRTV.heapPtr = nullptr;
    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByCpuHandleSRV(SIZE_T cpuHandle)
{
    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheSRV.genSeen == currentGen && cacheSRV.heapPtr != nullptr &&
        cacheSRV.heapPtr->version == cacheSRV.heapVersion && cacheSRV.heapPtr->active &&
        cacheSRV.heapPtr->cpuStart <= cpuHandle && cpuHandle < cacheSRV.heapPtr->cpuEnd)
    {
        return cacheSRV.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->cpuStart <= cpuHandle &&
            cpuHandle < fgHeaps[i]->cpuEnd)
        {
            cacheSRV.genSeen = currentGen;
            cacheSRV.heapPtr = fgHeaps[i].get();
            cacheSRV.heapVersion = cacheSRV.heapPtr->version;
            return cacheSRV.heapPtr;
        }
    }

    cacheSRV.heapVersion = 0;
    cacheSRV.heapPtr = nullptr;
    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByCpuHandleUAV(SIZE_T cpuHandle)
{
    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheUAV.genSeen == currentGen && cacheUAV.heapPtr != nullptr &&
        cacheUAV.heapPtr->version == cacheUAV.heapVersion && cacheUAV.heapPtr->active &&
        cacheUAV.heapPtr->cpuStart <= cpuHandle && cpuHandle < cacheUAV.heapPtr->cpuEnd)
    {
        return cacheUAV.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->cpuStart <= cpuHandle &&
            cpuHandle < fgHeaps[i]->cpuEnd)
        {
            cacheUAV.genSeen = currentGen;
            cacheUAV.heapPtr = fgHeaps[i].get();
            cacheUAV.heapVersion = cacheUAV.heapPtr->version;
            return cacheUAV.heapPtr;
        }
    }

    cacheUAV.heapVersion = 0;
    cacheUAV.heapPtr = nullptr;
    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByCpuHandle(SIZE_T cpuHandle, DescriptorCopyRole role)
{
    // Source and destination have separate caches. Keep the most recent heap
    // first, then search the other recently used heaps before the heap table.
    auto& cache = role == DescriptorCopyRole::Source ? cacheCopySource : cacheCopyDestination;
    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    for (size_t slot = 0; slot < cache.size(); ++slot)
    {
        const auto entry = cache[slot];
        if (entry.genSeen != currentGen || entry.heapPtr == nullptr ||
            entry.heapPtr->version != entry.heapVersion || !entry.heapPtr->active ||
            entry.heapPtr->cpuStart > cpuHandle || cpuHandle >= entry.heapPtr->cpuEnd)
            continue;
        for (size_t i = slot; i > 0; --i)
            cache[i] = cache[i - 1];
        cache[0] = entry;
        return entry.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->cpuStart <= cpuHandle &&
            cpuHandle < fgHeaps[i]->cpuEnd)
        {
            for (size_t slot = cache.size() - 1; slot > 0; --slot)
                cache[slot] = cache[slot - 1];
            cache[0] = { currentGen, fgHeaps[i].get(), fgHeaps[i]->version.load(std::memory_order_relaxed) };
            return cache[0].heapPtr;
        }
    }

    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByGpuHandleGR(SIZE_T gpuHandle)
{
    if (gpuHandle == NULL)
        return nullptr;

    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheGR.genSeen == currentGen && cacheGR.heapPtr != nullptr &&
        cacheGR.heapPtr->version == cacheGR.heapVersion && cacheGR.heapPtr->active &&
        cacheGR.heapPtr->gpuStart <= gpuHandle && gpuHandle < cacheGR.heapPtr->gpuEnd)
    {
        return cacheGR.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->gpuStart <= gpuHandle &&
            gpuHandle < fgHeaps[i]->gpuEnd)
        {
            cacheGR.genSeen = currentGen;
            cacheGR.heapPtr = fgHeaps[i].get();
            cacheGR.heapVersion = cacheGR.heapPtr->version;
            return cacheGR.heapPtr;
        }
    }

    cacheGR.heapVersion = 0;
    cacheGR.heapPtr = nullptr;
    return nullptr;
}

HeapInfo* ResTrack_Dx12::GetHeapByGpuHandleCR(SIZE_T gpuHandle)
{
    if (gpuHandle == NULL)
        return nullptr;

    unsigned currentGen = gHeapGeneration.load(std::memory_order_acquire);
    if (cacheCR.genSeen == currentGen && cacheCR.heapPtr != nullptr &&
        cacheCR.heapPtr->version == cacheCR.heapVersion && cacheCR.heapPtr->active &&
        cacheCR.heapPtr->gpuStart <= gpuHandle && gpuHandle < cacheCR.heapPtr->gpuEnd)
    {
        return cacheCR.heapPtr;
    }

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        if (fgHeaps[i] != nullptr && fgHeaps[i]->active && fgHeaps[i]->gpuStart <= gpuHandle &&
            gpuHandle < fgHeaps[i]->gpuEnd)
        {
            cacheCR.genSeen = currentGen;
            cacheCR.heapPtr = fgHeaps[i].get();
            cacheCR.heapVersion = cacheCR.heapPtr->version;
            return cacheCR.heapPtr;
        }
    }

    cacheCR.heapVersion = 0;
    cacheCR.heapPtr = nullptr;
    return nullptr;
}

#pragma endregion

#pragma region Hudless methods

bool ResTrack_Dx12::IsHudFixActive()
{
    if (DLSSNRCommandState::suppress) return false;
    if (DLSSNRLatePass::Pending()) return Hudfix_Dx12::IsResourceCheckActive();
    if (!Config::Instance()->FGEnabled.value_or_default() || !Config::Instance()->FGHUDFix.value_or_default())
    {
        LOG_TRACK(
            "!Config::Instance()->FGEnabled.value_or_default() || !Config::Instance()->FGHUDFix.value_or_default()");
        return false;
    }

    if (State::Instance().currentFG == nullptr || State::Instance().currentFeature == nullptr ||
        State::Instance().fgChanged)
    {
        LOG_TRACK("State::Instance().currentFG == nullptr || State::Instance().currentFeature == nullptr || "
                  "State::Instance().fgChanged");
        return false;
    }

    if (!State::Instance().currentFG->IsActive())
    {
        LOG_TRACK("!State::Instance().currentFG->IsActive()");
        return false;
    }

    if (!_presentDone)
    {
        LOG_TRACK("!_presentDone");
        return false;
    }

    if (Hudfix_Dx12::SkipHudlessChecks())
    {
        LOG_TRACK("!Hudfix_Dx12::SkipHudlessChecks()");
        return false;
    }

    if (!Hudfix_Dx12::IsResourceCheckActive())
    {
        // LOG_TRACK("!Hudfix_Dx12::IsResourceCheckActive()");
        return false;
    }

    return true;
}

#pragma endregion

#pragma region Resource input hooks

void ResTrack_Dx12::hkCreateRenderTargetView(ID3D12Device* This, ID3D12Resource* pResource,
                                             D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                             D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    // force hdr for swapchain buffer
    if (pResource != nullptr && pDesc != nullptr && Config::Instance()->ForceHDR.value_or_default())
    {
        for (size_t i = 0; i < State::Instance().scBuffers.size(); i++)
        {
            if (State::Instance().scBuffers[i] == pResource)
            {
                if (Config::Instance()->UseHDR10.value_or_default())
                    pDesc->Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                else
                    pDesc->Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

                break;
            }
        }
    }

    o_CreateRenderTargetView(This, pResource, pDesc, DestDescriptor);

    if (Config::Instance()->FGHudfixDisableRTV.value_or_default())
        return;

    ResourceInfo resInfo {};
    if ((pDesc != nullptr && pDesc->ViewDimension != D3D12_RTV_DIMENSION_TEXTURE2D) ||
        !CheckResource(pResource, &resInfo))
    {
        auto heap = GetHeapByCpuHandleRTV(DestDescriptor.ptr);

        if (heap != nullptr)
            heap->ClearByCpuHandle(DestDescriptor.ptr);

        return;
    }

    auto heap = GetHeapByCpuHandleRTV(DestDescriptor.ptr);
    if (heap != nullptr)
    {
        resInfo.type = RTV;
        resInfo.captureInfo = CaptureInfo::CreateRTV;
        heap->SetByCpuHandle(DestDescriptor.ptr, resInfo);
    }
    // else
    //{
    //     LOG_TRACK("Heap not found for RTV: {:X}", DestDescriptor.ptr);
    // }
}

void ResTrack_Dx12::hkCreateShaderResourceView(ID3D12Device* This, ID3D12Resource* pResource,
                                               D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                               D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    // force hdr for swapchain buffer
    if (pResource != nullptr && pDesc != nullptr && Config::Instance()->ForceHDR.value_or_default())
    {
        for (size_t i = 0; i < State::Instance().scBuffers.size(); i++)
        {
            if (State::Instance().scBuffers[i] == pResource)
            {
                if (Config::Instance()->UseHDR10.value_or_default())
                    pDesc->Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                else
                    pDesc->Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

                break;
            }
        }
    }

    o_CreateShaderResourceView(This, pResource, pDesc, DestDescriptor);

    if (Config::Instance()->FGHudfixDisableSRV.value_or_default())
        return;

    ResourceInfo resInfo {};
    if ((pDesc != nullptr && pDesc->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D) ||
        !CheckResource(pResource, &resInfo))
    {
        auto heap = GetHeapByCpuHandleSRV(DestDescriptor.ptr);

        if (heap != nullptr)
            heap->ClearByCpuHandle(DestDescriptor.ptr);

        return;
    }

    auto heap = GetHeapByCpuHandleSRV(DestDescriptor.ptr);
    if (heap != nullptr)
    {
        resInfo.type = SRV;
        resInfo.captureInfo = CaptureInfo::CreateSRV;
        heap->SetByCpuHandle(DestDescriptor.ptr, resInfo);
    }
    // else
    //{
    //     LOG_TRACK("Heap not found for SRV: {:X}", DestDescriptor.ptr);
    // }
}

void ResTrack_Dx12::hkCreateUnorderedAccessView(ID3D12Device* This, ID3D12Resource* pResource,
                                                ID3D12Resource* pCounterResource,
                                                D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    if (pResource != nullptr && pDesc != nullptr && Config::Instance()->ForceHDR.value_or_default())
    {
        for (size_t i = 0; i < State::Instance().scBuffers.size(); i++)
        {
            if (State::Instance().scBuffers[i] == pResource)
            {
                if (Config::Instance()->UseHDR10.value_or_default())
                    pDesc->Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                else
                    pDesc->Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

                break;
            }
        }
    }

    o_CreateUnorderedAccessView(This, pResource, pCounterResource, pDesc, DestDescriptor);

    if (Config::Instance()->FGHudfixDisableUAV.value_or_default())
        return;

    ResourceInfo resInfo {};
    if ((pDesc != nullptr && pDesc->ViewDimension != D3D12_UAV_DIMENSION_TEXTURE2D) ||
        !CheckResource(pResource, &resInfo))
    {
        auto heap = GetHeapByCpuHandleUAV(DestDescriptor.ptr);

        if (heap != nullptr)
            heap->ClearByCpuHandle(DestDescriptor.ptr);

        return;
    }

    auto heap = GetHeapByCpuHandleUAV(DestDescriptor.ptr);
    if (heap != nullptr)
    {
        resInfo.type = UAV;
        resInfo.captureInfo = CaptureInfo::CreateUAV;
        heap->SetByCpuHandle(DestDescriptor.ptr, resInfo);
    }
    // else
    //{
    //     LOG_TRACK("Heap not found for UAV: {:X}", DestDescriptor.ptr);
    // }
}

#pragma endregion

void ResTrack_Dx12::hkExecuteCommandLists(ID3D12CommandQueue* This, UINT NumCommandLists,
                                          ID3D12CommandList* const* ppCommandLists)
{
    auto fg = State::Instance().currentFG;

    if (fg != nullptr && fg->IsActive() && !fg->IsPaused())
    {
        LOG_TRACK("NumCommandLists: {}", NumCommandLists);

        std::vector<FG_ResourceType> found;
        auto fIndex = fg->GetIndex();

        do
        {
            std::lock_guard<std::mutex> lock2(_resourceCommandListMutex);

            if (!_notFoundCmdLists.empty())
            {
                for (size_t i = 0; i < NumCommandLists; i++)
                {
                    if (_notFoundCmdLists.contains(ppCommandLists[i]))
                    {
                        LOG_WARN("Found last frames cmdList: {:X}", (size_t) ppCommandLists[i]);
                        _notFoundCmdLists.erase(ppCommandLists[i]);
                    }
                }
            }

            if (_resCmdList[fIndex].empty())
                break;

            for (size_t i = 0; i < NumCommandLists; i++)
            {
                LOG_TRACK("ppCommandLists[{}]: {:X}", i, (size_t) ppCommandLists[i]);

                for (const auto& pair : _resCmdList[fIndex])
                {
                    if (pair.second == ppCommandLists[i])
                    {
                        LOG_DEBUG("found {} cmdList: {:X}, queue: {:X}", (UINT) pair.first, (size_t) pair.second,
                                  (size_t) This);
                        fg->SetResourceReady(pair.first);
                        found.push_back(pair.first);
                    }
                }

                for (size_t i = 0; i < found.size(); i++)
                {
                    _resCmdList[fIndex].erase(found[i]);
                }

                if (_resCmdList[fIndex].empty())
                    break;
            }

        } while (false);

        if (!found.empty())
        {
            o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);
            GazeRoiFrameSync::OnExecuteCommandLists(This, NumCommandLists, ppCommandLists);

            for (size_t i = 0; i < found.size(); i++)
            {
                fg->SetCommandQueue(found[i], This);
            }

            return;
        }
    }

    LOG_TRACK("Done NumCommandLists: {}", NumCommandLists);

    o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);
    GazeRoiFrameSync::OnExecuteCommandLists(This, NumCommandLists, ppCommandLists);
}

#pragma region Heap hooks

static ULONG STDMETHODCALLTYPE hkHeapRelease(ID3D12DescriptorHeap* This)
{
    if (State::Instance().isShuttingDown)
        return o_HeapRelease(This);

    size_t count = fgHeaps.size();
    for (size_t i = 0; i < count; i++)
    {
        auto& up = fgHeaps[i];

        if (up == nullptr || up->heap != This || !up->active)
            continue;

        This->AddRef();
        if (o_HeapRelease(This) <= 1)
        {
#ifdef USE_SPINLOCK_MUTEX_FOR_HEAP_CREATION
            std::lock_guard<SpinLock> lock(_heapCreationMutex);
#else
            std::lock_guard<std::mutex> lock(_heapCreationMutex);
#endif

            up->active = false;

            LOG_INFO("Heap released: {:X}", (size_t) This);

            // Use the same indexed removal as descriptor overwrites. Heap
            // teardown must not reintroduce quadratic reverse-index scans.
            for (UINT j = 0; j < up->numDescriptors; ++j)
                up->ClearByCpuHandle(up->cpuStart + static_cast<SIZE_T>(j) * up->increment);

            gHeapGeneration.fetch_add(1, std::memory_order_release); // invalidate caches
        }

        break;
    }

    return o_HeapRelease(This);
}

HRESULT ResTrack_Dx12::hkCreateDescriptorHeap(ID3D12Device* This, D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
                                              REFIID riid, void** ppvHeap)
{
    auto result = o_CreateDescriptorHeap(This, pDescriptorHeapDesc, riid, ppvHeap);

    if (State::Instance().skipHeapCapture)
        return result;

    // try to calculate handle ranges for heap
    if (result == S_OK && (pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
                           pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV))
    {
        auto heap = (ID3D12DescriptorHeap*) (*ppvHeap);

        if (!o_HeapRelease)
        {
            PVOID* vtbl = *(PVOID**) heap;
            o_HeapRelease = (PFN_HeapRelease) vtbl[2];
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&) o_HeapRelease, hkHeapRelease);
            auto detourResult = DetourTransactionCommit();
            if (detourResult != NO_ERROR)
            {
                LOG_ERROR("Failed to hook Heap Release: {:X}", detourResult);
                o_HeapRelease = nullptr;
            }
        }

        auto increment = This->GetDescriptorHandleIncrementSize(pDescriptorHeapDesc->Type);
        auto numDescriptors = pDescriptorHeapDesc->NumDescriptors;
        auto cpuStart = (SIZE_T) (heap->GetCPUDescriptorHandleForHeapStart().ptr);
        auto cpuEnd = cpuStart + (increment * numDescriptors);
        auto gpuStart = (SIZE_T) (heap->GetGPUDescriptorHandleForHeapStart().ptr);
        auto gpuEnd = gpuStart + (increment * numDescriptors);
        auto type = (UINT) pDescriptorHeapDesc->Type;

        LOG_TRACE("Heap: {:X}, Heap type: {}, Cpu: {}-{}, Gpu: {}-{}, Desc count: {}", (size_t) *ppvHeap, type,
                  cpuStart, cpuEnd, gpuStart, gpuEnd, numDescriptors);
        {
#ifdef USE_SPINLOCK_MUTEX_FOR_HEAP_CREATION
            std::lock_guard<SpinLock> lock(_heapCreationMutex);
#else
            std::lock_guard<std::mutex> lock(_heapCreationMutex);
#endif
            size_t count = fgHeaps.size();
            bool foundEmpty = false;
            for (size_t i = 0; i < count; i++)
            {
                if (fgHeaps[i] != nullptr && !fgHeaps[i]->active)
                {

                    fgHeaps[i].reset();
                    fgHeaps[i] = std::make_unique<HeapInfo>(heap, cpuStart, cpuEnd, gpuStart, gpuEnd, numDescriptors,
                                                            increment, type);

                    gHeapGeneration.fetch_add(1, std::memory_order_release);
                    foundEmpty = true;
                    LOG_DEBUG("Reusing empty heap slot: {}", i);
                    break;
                }
            }

            if (!foundEmpty)
            {
                // Reallocate vector if needed
                if (fgHeaps.capacity() == fgHeaps.size())
                    fgHeaps.reserve(fgHeaps.size() + 65536);

                fgHeaps.push_back(std::make_unique<HeapInfo>(heap, cpuStart, cpuEnd, gpuStart, gpuEnd, numDescriptors,
                                                             increment, type));

                gHeapGeneration.fetch_add(1, std::memory_order_release);
                LOG_DEBUG("Adding new heap slot: {}", fgHeaps.size() - 1);
            }
        }
    }
    else
    {
        if (ppvHeap != nullptr && *ppvHeap != nullptr)
        {
            auto heap = (ID3D12DescriptorHeap*) (*ppvHeap);
            LOG_TRACE("Skipping, Heap type: {}, Cpu: {}, Gpu: {}", (UINT) pDescriptorHeapDesc->Type,
                      heap->GetCPUDescriptorHandleForHeapStart().ptr, heap->GetGPUDescriptorHandleForHeapStart().ptr);
        }
    }

    return result;
}

ULONG ResTrack_Dx12::hkRelease(ID3D12Resource* This)
{
    if (State::Instance().isShuttingDown)
        return o_Release(This);

    // Serialize refcount probes and the existing captured-HUDless bookkeeping,
    // independently of the descriptor-copy locks.
    static std::mutex releaseMutex;
    {
        std::lock_guard lock(releaseMutex);

        This->AddRef();
        auto refCount = o_Release(This);

        if (refCount <= 1)
        {
            auto& shard = ResourceReferences(This);
            std::scoped_lock referencesLock(shard.mutex);
            if (shard.Invalidate(This))
                State::Instance().capturedHudlesses.erase(This);
        }
    }

    return o_Release(This);
}

void ResTrack_Dx12::hkCopyDescriptors(ID3D12Device* This, UINT NumDestDescriptorRanges,
                                      D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                      UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                      D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                      UINT* pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    o_CopyDescriptors(This, NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                      NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);

    // Early exit conditions - consistent validation
    if (DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
        return;

    if (NumDestDescriptorRanges == 0 || pDestDescriptorRangeStarts == nullptr)
        return;

    // Copies may populate long-lived tables before upscaling or while late NR
    // is toggled off. Capture eligibility must not gate descriptor metadata.
    if (!DLSSNRLatePass::TrackDescriptors() &&
        !Config::Instance()->FGAlwaysTrackHeaps.value_or_default() && !IsHudFixActive())
        return;

    const UINT inc = This->GetDescriptorHandleIncrementSize(DescriptorHeapsType);

    // Validate that we have source descriptors to copy
    bool haveSources = (NumSrcDescriptorRanges > 0 && pSrcDescriptorRangeStarts != nullptr);

    // Track positions in both source and destination ranges
    UINT srcRangeIndex = 0;
    UINT srcOffsetInRange = 0;
    UINT destRangeIndex = 0;
    UINT destOffsetInRange = 0;

    // Cache heap and index state for each range (upstream 5666be0a).
    HeapInfo* cachedDestHeap = nullptr;
    SIZE_T cachedDestRangeStart = 0;
    UINT cachedDestRangeSize = 0;
    UINT cachedDestBaseIndex = 0;
    bool cachedDestRangeFits = false;
    HeapInfo* cachedSrcHeap = nullptr;
    SIZE_T cachedSrcRangeStart = 0;
    UINT cachedSrcRangeSize = 0;
    UINT cachedSrcBaseIndex = 0;
    bool cachedSrcRangeFits = false;

    // Process all destination descriptors
    while (destRangeIndex < NumDestDescriptorRanges)
    {
        // Update destination heap cache if we've moved to a new range
        if (destOffsetInRange == 0)
        {
            cachedDestRangeStart = pDestDescriptorRangeStarts[destRangeIndex].ptr;
            cachedDestRangeSize =
                (pDestDescriptorRangeSizes == nullptr) ? 1 : pDestDescriptorRangeSizes[destRangeIndex];
            if (cachedDestRangeSize == 0)
            {
                ++destRangeIndex;
                continue;
            }
            cachedDestHeap = GetHeapByCpuHandle(cachedDestRangeStart, DescriptorCopyRole::Destination);
            cachedDestRangeFits = cachedDestHeap != nullptr &&
                cachedDestHeap->GetCpuIndex(cachedDestRangeStart, cachedDestBaseIndex) &&
                cachedDestRangeSize <= cachedDestHeap->numDescriptors - cachedDestBaseIndex;
        }

        // Resolve source and destination indices once per range where possible.
        UINT srcIndex = 0;
        bool srcValid = false;
        // Zero-sized ranges consume no source or destination descriptors.
        while (haveSources && srcRangeIndex < NumSrcDescriptorRanges && srcOffsetInRange == 0 &&
               pSrcDescriptorRangeSizes != nullptr && pSrcDescriptorRangeSizes[srcRangeIndex] == 0)
            ++srcRangeIndex;
        if (haveSources && srcRangeIndex < NumSrcDescriptorRanges)
        {
            // Update source heap cache if we've moved to a new range
            if (srcOffsetInRange == 0)
            {
                cachedSrcRangeStart = pSrcDescriptorRangeStarts[srcRangeIndex].ptr;
                cachedSrcRangeSize =
                    (pSrcDescriptorRangeSizes == nullptr) ? 1 : pSrcDescriptorRangeSizes[srcRangeIndex];
                cachedSrcHeap = GetHeapByCpuHandle(cachedSrcRangeStart, DescriptorCopyRole::Source);
                cachedSrcRangeFits = cachedSrcHeap != nullptr &&
                    cachedSrcHeap->GetCpuIndex(cachedSrcRangeStart, cachedSrcBaseIndex) &&
                    cachedSrcRangeSize <= cachedSrcHeap->numDescriptors - cachedSrcBaseIndex;
            }

            if (cachedSrcHeap != nullptr)
            {
                srcValid = cachedSrcRangeFits;
                if (srcValid)
                    srcIndex = cachedSrcBaseIndex + srcOffsetInRange;
                else
                    srcValid = cachedSrcHeap->GetCpuIndex(
                        cachedSrcRangeStart + static_cast<SIZE_T>(srcOffsetInRange) * inc, srcIndex);
            }

            // Advance source position
            srcOffsetInRange++;
            if (srcOffsetInRange >= cachedSrcRangeSize)
            {
                srcOffsetInRange = 0;
                srcRangeIndex++;
            }
        }

        if (cachedDestHeap != nullptr)
        {
            if (cachedDestRangeFits)
            {
                const auto index = cachedDestBaseIndex + destOffsetInRange;
                cachedDestHeap->CopyByIndex(index, srcValid ? cachedSrcHeap : nullptr, srcIndex);
            }
            else
            {
                const auto handle = cachedDestRangeStart + static_cast<SIZE_T>(destOffsetInRange) * inc;
                UINT dstIndex = 0;
                if (cachedDestHeap->GetCpuIndex(handle, dstIndex))
                    cachedDestHeap->CopyByIndex(dstIndex, srcValid ? cachedSrcHeap : nullptr, srcIndex);
            }
        }

        // Advance destination position
        destOffsetInRange++;
        if (destOffsetInRange >= cachedDestRangeSize)
        {
            destOffsetInRange = 0;
            destRangeIndex++;
        }
    }
}

void ResTrack_Dx12::hkCopyDescriptorsSimple(ID3D12Device* This, UINT NumDescriptors,
                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                            D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                            D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    o_CopyDescriptorsSimple(This, NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart,
                            DescriptorHeapsType);

    if (DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
        return;

    if (NumDescriptors == 0)
        return;

    if (!DLSSNRLatePass::TrackDescriptors() &&
        !Config::Instance()->FGAlwaysTrackHeaps.value_or_default() && !IsHudFixActive())
        return;

    // Each valid CopyDescriptorsSimple range stays within one heap. Resolve
    // each range once; descriptor metadata still updates for every copied slot.
    auto dstHeap = GetHeapByCpuHandle(DestDescriptorRangeStart.ptr, DescriptorCopyRole::Destination);
    if (dstHeap == nullptr)
        return;

    auto srcHeap = SrcDescriptorRangeStart.ptr != 0
                       ? GetHeapByCpuHandle(SrcDescriptorRangeStart.ptr, DescriptorCopyRole::Source)
                       : nullptr;

    // Adapted from upstream a6713718: convert each valid range to indices once,
    // then reuse them for the whole copy. The heap already knows its stride;
    // the common path needs no additional device query or per-slot division.
    UINT srcBaseIndex = 0, dstBaseIndex = 0;
    const bool srcRangeFits = srcHeap == nullptr ||
        (srcHeap->GetCpuIndex(SrcDescriptorRangeStart.ptr, srcBaseIndex) &&
         NumDescriptors <= srcHeap->numDescriptors - srcBaseIndex);
    const bool dstRangeFits = dstHeap->GetCpuIndex(DestDescriptorRangeStart.ptr, dstBaseIndex) &&
        NumDescriptors <= dstHeap->numDescriptors - dstBaseIndex;
    if (srcRangeFits && dstRangeFits)
    {
        for (UINT i = 0; i < NumDescriptors; ++i)
        {
            dstHeap->CopyByIndex(dstBaseIndex + i, srcHeap, srcBaseIndex + i);
        }
        return;
    }

    // Keep bounded handle-based lookup for ranges not admitted by the index path.
    const auto size = This->GetDescriptorHandleIncrementSize(DescriptorHeapsType);

    for (size_t i = 0; i < NumDescriptors; i++)
    {
        const auto srcHandle = SrcDescriptorRangeStart.ptr + i * size;
        const auto destHandle = DestDescriptorRangeStart.ptr + i * size;

        UINT dstIndex = 0, srcIndex = 0;
        if (dstHeap->GetCpuIndex(destHandle, dstIndex))
            dstHeap->CopyByIndex(dstIndex,
                srcHeap != nullptr && srcHeap->GetCpuIndex(srcHandle, srcIndex) ? srcHeap : nullptr, srcIndex);
    }
}

#pragma endregion

#pragma region Shader input hooks

void ResTrack_Dx12::hkSetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT RootParameterIndex,
                                                     D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    // Consistent early exit - always call original function
    auto shouldTrack = !Config::Instance()->FGHudfixDisableSGR.value_or_default() && BaseDescriptor.ptr != 0 &&
                       IsHudFixActive() && !Hudfix_Dx12::SkipHudlessChecks() &&
                       This != MenuOverlayDx::MenuCommandList();

    if (!shouldTrack)
    {
        CaptureSetGraphicsRootDescriptorTable::Forward(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    auto heap = GetHeapByGpuHandleGR(BaseDescriptor.ptr);
    if (heap == nullptr)
    {
        LOG_DEBUG_ONLY("No heap for handle: {:X}", BaseDescriptor.ptr);
        CaptureSetGraphicsRootDescriptorTable::Forward(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    auto capturedBuffer = heap->GetByGpuHandle(BaseDescriptor.ptr);
    if (capturedBuffer == nullptr || capturedBuffer->buffer == nullptr)
    {
        LOG_DEBUG_ONLY("No resource at RootParameterIndex: {}, CommandList: {:X}, gpuHandle: {:X}", RootParameterIndex,
                       (SIZE_T) This, BaseDescriptor.ptr);
        CaptureSetGraphicsRootDescriptorTable::Forward(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    LOG_DEBUG_ONLY("CommandList: {:X}, Resource: {:X}", (size_t) This, (size_t) capturedBuffer->buffer);

    // Only proceed with tracking if we have a valid buffer
    capturedBuffer->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    capturedBuffer->captureInfo = CaptureInfo::SetGR;

    // Track the resource
    bool capturedImmediately = false;
    if (Config::Instance()->FGImmediateCapture.value_or_default() &&
        (!DLSSNRLatePass::Pending() || capturedBuffer->type == SRV))
    {
        capturedImmediately = Hudfix_Dx12::CheckForHudless(This, capturedBuffer, capturedBuffer->state);
    }

    if (!capturedImmediately)
    {
        auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

        if (!_useShards)
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (!fgPossibleHudless[fIndex].contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                fgPossibleHudless[fIndex].insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("Tracking Resource: {:X}, Desc: {:X}", (size_t) capturedBuffer->buffer, BaseDescriptor.ptr);
            fgPossibleHudless[fIndex][This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
        }
        else
        {
            size_t shardIdx = GetShardIndex(This);
            auto& shard = _hudlessShards[fIndex][shardIdx];

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            if (!shard.map.contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                shard.map.insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("CmdList: {:X}, Tracking Resource: {:X}, Desc: {:X}, Format: {}", (size_t) This,
                      (size_t) capturedBuffer->buffer, BaseDescriptor.ptr, (UINT) capturedBuffer->format);

            shard.map[This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
        }
    }

    CaptureSetGraphicsRootDescriptorTable::Forward(This, RootParameterIndex, BaseDescriptor);
}

#pragma endregion

#pragma region Shader output hooks

void ResTrack_Dx12::hkOMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors,
                                         D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                         BOOL RTsSingleHandleToDescriptorRange,
                                         D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    ObserveCommandList(This);
{
    }
    // Consistent early exit validation
    auto shouldTrack = !Config::Instance()->FGHudfixDisableOM.value_or_default() && NumRenderTargetDescriptors > 0 &&
                       pRenderTargetDescriptors != nullptr && IsHudFixActive() && !Hudfix_Dx12::SkipHudlessChecks() &&
                       This != MenuOverlayDx::MenuCommandList();

    if (!shouldTrack)
    {
        o_OMSetRenderTargets(This, NumRenderTargetDescriptors, pRenderTargetDescriptors,
                             RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
        return;
    }

    LOG_DEBUG_ONLY("NumRenderTargetDescriptors: {}", NumRenderTargetDescriptors);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    // Process render targets
    for (size_t i = 0; i < NumRenderTargetDescriptors; i++)
    {
        HeapInfo* heap = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE handle {};

        // Get the appropriate handle
        if (RTsSingleHandleToDescriptorRange)
        {
            heap = GetHeapByCpuHandleRTV(pRenderTargetDescriptors[0].ptr);
            if (heap == nullptr)
            {
                LOG_DEBUG_ONLY("No heap at index: {}", i);
                continue;
            }

            handle.ptr = pRenderTargetDescriptors[0].ptr + (i * heap->increment);
        }
        else
        {
            handle = pRenderTargetDescriptors[i];
            heap = GetHeapByCpuHandleRTV(handle.ptr);
            if (heap == nullptr)
            {
                LOG_DEBUG_ONLY("No heap at index: {}", i);
                continue;
            }
        }

        auto capturedBuffer = heap->GetByCpuHandle(handle.ptr);
        if (capturedBuffer == nullptr || capturedBuffer->buffer == nullptr)
        {
            LOG_DEBUG_ONLY("No resource at index: {}, cpu: {:X}", i, handle.ptr);
            continue;
        }

        // Valid resource found, update state
        capturedBuffer->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        capturedBuffer->captureInfo = CaptureInfo::OMSetRTV;

        // Check for immediate capture
        bool capturedImmediately = false;
        if (Config::Instance()->FGImmediateCapture.value_or_default() &&
        (!DLSSNRLatePass::Pending() || capturedBuffer->type == SRV))
        {
            capturedImmediately = Hudfix_Dx12::CheckForHudless(This, capturedBuffer, capturedBuffer->state);
            if (capturedImmediately)
                break; // Early exit if captured
        }

        // Track for later processing
        if (!capturedImmediately)
        {
            if (!_useShards)
            {
                std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

                if (!fgPossibleHudless[fIndex].contains(This))
                {
                    ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                    newMap.reserve(32);
                    fgPossibleHudless[fIndex].insert_or_assign(This, std::move(newMap));
                }

                LOG_TRACK("Tracking Resource: {:X}, Desc: {:X}", (size_t) capturedBuffer->buffer, handle.ptr);
                fgPossibleHudless[fIndex][This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
            }
            else
            {
                size_t shardIdx = GetShardIndex(This);
                auto& shard = _hudlessShards[fIndex][shardIdx];

#ifdef USE_SPINLOCK_MUTEX
                std::lock_guard<SpinLock> lock(shard.mutex);
#else
                std::lock_guard<std::mutex> lock(shard.mutex);
#endif

                if (!shard.map.contains(This))
                {
                    ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                    newMap.reserve(32);
                    shard.map.insert_or_assign(This, std::move(newMap));
                }

                LOG_TRACK("CmdList: {:X}, Tracking Resource: {:X}, Desc: {:X}, Format: {}", (size_t) This,
                          (size_t) capturedBuffer->buffer, handle.ptr, (UINT) capturedBuffer->format);

                shard.map[This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
            }
        }
    }

    o_OMSetRenderTargets(This, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange,
                         pDepthStencilDescriptor);
}

#pragma endregion

#pragma region Compute paramter hooks

void ResTrack_Dx12::hkSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT RootParameterIndex,
                                                    D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    // Consistent early exit - always call original function
    auto shouldTrack = !Config::Instance()->FGHudfixDisableSCR.value_or_default() && BaseDescriptor.ptr != 0 &&
                       IsHudFixActive() && !Hudfix_Dx12::SkipHudlessChecks() &&
                       This != MenuOverlayDx::MenuCommandList();

    if (!shouldTrack)
    {
        CaptureSetComputeRootDescriptorTable::Forward(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    auto heap = GetHeapByGpuHandleCR(BaseDescriptor.ptr);
    if (heap == nullptr)
    {
        LOG_DEBUG_ONLY("No heap for handle: {:X}", BaseDescriptor.ptr);
        CaptureSetComputeRootDescriptorTable::Forward(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    auto capturedBuffer = heap->GetByGpuHandle(BaseDescriptor.ptr);
    if (capturedBuffer == nullptr || capturedBuffer->buffer == nullptr)
    {
        LOG_DEBUG_ONLY("No resource at RootParameterIndex: {}, CommandList: {:X}, gpuHandle: {:X}", RootParameterIndex,
                       (SIZE_T) This, BaseDescriptor.ptr);
        CaptureSetComputeRootDescriptorTable::Forward(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    LOG_DEBUG_ONLY("CommandList: {:X}, Resource: {:X}", (size_t) This, (size_t) capturedBuffer->buffer);

    // Only proceed with tracking if we have a valid buffer
    if (capturedBuffer->type == UAV)
        capturedBuffer->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    else
        capturedBuffer->state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    capturedBuffer->captureInfo = CaptureInfo::SetCR;

    // Track the resource
    bool capturedImmediately = false;
    if (Config::Instance()->FGImmediateCapture.value_or_default() &&
        (!DLSSNRLatePass::Pending() || capturedBuffer->type == SRV))
    {
        capturedImmediately = Hudfix_Dx12::CheckForHudless(This, capturedBuffer, capturedBuffer->state);
    }

    if (!capturedImmediately)
    {
        auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

        if (!_useShards)
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (!fgPossibleHudless[fIndex].contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                fgPossibleHudless[fIndex].insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("Tracking Resource: {:X}, Desc: {:X}", (size_t) capturedBuffer->buffer, BaseDescriptor.ptr);
            fgPossibleHudless[fIndex][This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
        }
        else
        {
            size_t shardIdx = GetShardIndex(This);
            auto& shard = _hudlessShards[fIndex][shardIdx];

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            if (!shard.map.contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                shard.map.insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("CmdList: {:X}, Tracking Resource: {:X}, Desc: {:X}, Format: {}", (size_t) This,
                      (size_t) capturedBuffer->buffer, BaseDescriptor.ptr, (UINT) capturedBuffer->format);

            shard.map[This].insert_or_assign(capturedBuffer->buffer, *capturedBuffer);
        }
    }

    CaptureSetComputeRootDescriptorTable::Forward(This, RootParameterIndex, BaseDescriptor);
}

#pragma endregion

#pragma region Shader finalizer hooks

// HUDfix may capture shader INPUTS as well as render/UAV OUTPUTS. Late NR
// must modify inputs before their consuming draw/dispatch; outputs are ready
// only after their producing draw/dispatch. Both reuse the same candidate map.
void ResTrack_Dx12::CheckLateInputs(ID3D12GraphicsCommandList* commands, UINT captureKind)
{
    if (!DLSSNRLatePass::Pending() || !IsHudFixActive()) return;
    if ((captureKind == CaptureInfo::DrawInstanced && Config::Instance()->FGHudfixDisableDI.value_or_default()) ||
        (captureKind == CaptureInfo::DrawIndexedInstanced && Config::Instance()->FGHudfixDisableDII.value_or_default()) ||
        (captureKind == CaptureInfo::Dispatch && Config::Instance()->FGHudfixDisableDispatch.value_or_default())) return;
    const auto index = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;
    std::vector<ResourceInfo> inputs;
    if (!_useShards)
    {
        std::lock_guard lock(_hudlessTrackMutex);
        auto it = fgPossibleHudless[index].find(commands);
        if (it != fgPossibleHudless[index].end())
            for (const auto& [key, resource] : it->second)
                if (resource.type == SRV) inputs.push_back(resource);
    }
    else
    {
        auto& shard = _hudlessShards[index][GetShardIndex(commands)];
        std::lock_guard lock(shard.mutex);
        auto it = shard.map.find(commands);
        if (it != shard.map.end())
            for (const auto& [key, resource] : it->second)
                if (resource.type == SRV) inputs.push_back(resource);
    }
    for (auto& resource : inputs)
    {
        resource.captureInfo |= captureKind;
        if (Hudfix_Dx12::CheckForHudless(commands, &resource, resource.state)) break;
    }
}

// Capture if render target matches, wait for DrawIndexed
void ResTrack_Dx12::hkDrawInstanced(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance, UINT InstanceCount,
                                    UINT StartVertexLocation, UINT StartInstanceLocation)
{
    CheckLateInputs(This, CaptureInfo::DrawInstanced);
    CaptureDrawInstanced::Forward(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);

    if (!IsHudFixActive())
    {
        LOG_TRACK("Skipping {:X}", (size_t) This);
        return;
    }

    LOG_TRACK("CmdList: {:X}", (size_t) This);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        if (This == MenuOverlayDx::MenuCommandList())
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
            fgPossibleHudless[fIndex].erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (fgPossibleHudless[fIndex].size() == 0 || !fgPossibleHudless[fIndex].contains(This))
                return;

            val0 = std::move(fgPossibleHudless[fIndex][This]);
            fgPossibleHudless[fIndex].erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDI.value_or_default())
                break;

            std::lock_guard<std::mutex> lock(_drawMutex);
            for (auto& [key, val] : val0)
            {
                if (DLSSNRLatePass::Pending() && val.type == SRV) continue;
                val.captureInfo |= CaptureInfo::DrawInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
    else
    {
        size_t shardIdx = GetShardIndex(This);
        auto& shard = _hudlessShards[fIndex][shardIdx];

        if (This == MenuOverlayDx::MenuCommandList() && shard.map.contains(This))
        {
#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            shard.map.erase(This);
            return;
        }

        // if can't find output skip
        if (shard.map.size() == 0)
        {
            LOG_DEBUG_ONLY("Early exit");
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            if (!shard.map.contains(This))
                return;

            val0 = std::move(shard.map[This]);
            shard.map.erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDI.value_or_default())
                break;

            for (auto& [key, val] : val0)
            {
                std::lock_guard<std::mutex> lock(_drawMutex);

                if (DLSSNRLatePass::Pending() && val.type == SRV) continue;
                val.captureInfo |= CaptureInfo::DrawInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
}

void ResTrack_Dx12::hkDrawIndexedInstanced(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                           UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
                                           UINT StartInstanceLocation)
{
    CheckLateInputs(This, CaptureInfo::DrawIndexedInstanced);
    CaptureDrawIndexedInstanced::Forward(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                           StartInstanceLocation);

    if (!IsHudFixActive())
    {
        LOG_TRACK("Skipping CmdList: {:X}", (size_t) This);
        return;
    }

    LOG_TRACK("CmdList: {:X}", (size_t) This);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        if (This == MenuOverlayDx::MenuCommandList())
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
            fgPossibleHudless[fIndex].erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (fgPossibleHudless[fIndex].size() == 0 || !fgPossibleHudless[fIndex].contains(This))
                return;

            val0 = std::move(fgPossibleHudless[fIndex][This]);
            fgPossibleHudless[fIndex].erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDII.value_or_default())
                break;

            std::lock_guard<std::mutex> lock(_drawMutex);
            for (auto& [key, val] : val0)
            {
                if (DLSSNRLatePass::Pending() && val.type == SRV) continue;
                val.captureInfo |= CaptureInfo::DrawIndexedInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
    else
    {
        size_t shardIdx = GetShardIndex(This);
        auto& shard = _hudlessShards[fIndex][shardIdx];

        if (This == MenuOverlayDx::MenuCommandList() && shard.map.contains(This))
        {
#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            shard.map.erase(This);
            return;
        }

        // if can't find output skip
        if (shard.map.size() == 0)
        {
            LOG_DEBUG_ONLY("Early exit");
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            if (!shard.map.contains(This))
                return;

            val0 = std::move(shard.map[This]);
            shard.map.erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDII.value_or_default())
                break;

            for (auto& [key, val] : val0)
            {
                // LOG_DEBUG("Waiting _drawMutex {:X}", (size_t)val.buffer);
                std::lock_guard<std::mutex> lock(_drawMutex);

                if (DLSSNRLatePass::Pending() && val.type == SRV) continue;
                val.captureInfo |= CaptureInfo::DrawIndexedInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
}

void ResTrack_Dx12::hkExecuteBundle(ID3D12GraphicsCommandList* This, ID3D12GraphicsCommandList* pCommandList)
{
    LOG_FUNC();

    IFGFeature_Dx12* fg = State::Instance().currentFG;
    auto index = fg != nullptr ? fg->GetIndex() : 0;

    {
        std::lock_guard<std::mutex> lock(_resourceCommandListMutex);

        if (fg != nullptr && fg->IsActive() && (_resourceCommandList[index].size() > 0 || !_resCmdList[index].empty()))
        {
            if (_notFoundCmdLists.contains(pCommandList))
                LOG_WARN("Found last frames cmdList: {:X}", (size_t) This);

            auto& frameCmdList = _resourceCommandList[index];
            for (std::unordered_map<FG_ResourceType, ID3D12GraphicsCommandList*>::iterator it = frameCmdList.begin();
                 it != frameCmdList.end(); ++it)
            {
                if (it->second == pCommandList)
                    it->second = This;
            }

            for (std::unordered_map<FG_ResourceType, void*>::iterator it = _resCmdList[index].begin();
                 it != _resCmdList[index].end(); ++it)
            {
                if (it->second == pCommandList)
                    it->second = This;
            }
        }
    }

    if (DLSSNRLatePass::Enabled())
        DLSSNRCommandState::Invalidate(This); // bundle-inherited bindings are not reconstructible here
    CaptureBundle::Forward(This, pCommandList);
}

HRESULT ResTrack_Dx12::hkClose(ID3D12GraphicsCommandList* This)
{
    auto fg = State::Instance().currentFG;
    auto index = fg != nullptr ? fg->GetIndex() : 0;

    if (fg != nullptr && fg->IsActive() && !fg->IsPaused() && _resourceCommandList[index].size() > 0)
    {
        LOG_TRACK("CmdList: {:X}", (size_t) This);

        std::lock_guard<std::mutex> lock(_resourceCommandListMutex);

        if (_notFoundCmdLists.contains(This))
            LOG_WARN("Found last frames cmdList: {:X}", (size_t) This);

        std::vector<FG_ResourceType> found;

        for (const auto& pair : _resourceCommandList[index])
        {
            if (This == pair.second)
            {
                if (!fg->IsResourceReady(pair.first))
                {
                    LOG_DEBUG("{} cmdList: {:X}", (UINT) pair.first, (size_t) This);
                    _resCmdList[index][pair.first] = pair.second;
                    found.push_back(pair.first);
                }
            }
        }

        for (size_t i = 0; i < found.size(); i++)
        {
            _resourceCommandList[index].erase(found[i]);
        }
    }

    DLSSNRCommandState::Reset(This);
    const auto result = CaptureClose::Forward(This);
    return result;
}

void ResTrack_Dx12::hkDispatch(ID3D12GraphicsCommandList* This, UINT ThreadGroupCountX, UINT ThreadGroupCountY,
                               UINT ThreadGroupCountZ)
{
    CheckLateInputs(This, CaptureInfo::Dispatch);
    CaptureDispatch::Forward(This, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);

    if (!IsHudFixActive())
    {
        LOG_TRACK("Skipping {:X}", (size_t) This);
        return;
    }

    LOG_TRACK("CmdList: {:X}", (size_t) This);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        if (This == MenuOverlayDx::MenuCommandList())
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
            fgPossibleHudless[fIndex].erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (fgPossibleHudless[fIndex].size() == 0 || !fgPossibleHudless[fIndex].contains(This))
                return;

            val0 = std::move(fgPossibleHudless[fIndex][This]);
            fgPossibleHudless[fIndex].erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDispatch.value_or_default())
                break;

            std::lock_guard<std::mutex> lock(_drawMutex);
            for (auto& [key, val] : val0)
            {
                if (DLSSNRLatePass::Pending() && val.type == SRV) continue;
                val.captureInfo |= CaptureInfo::Dispatch;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }
        } while (false);
    }
    else
    {
        size_t shardIdx = GetShardIndex(This);
        auto& shard = _hudlessShards[fIndex][shardIdx];

        if (This == MenuOverlayDx::MenuCommandList() && shard.map.contains(This))
        {
#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            shard.map.erase(This);
            return;
        }

        // if can't find output skip
        if (shard.map.size() == 0)
        {
            LOG_DEBUG_ONLY("Early exit");
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            if (!shard.map.contains(This))
                return;

            val0 = std::move(shard.map[This]);
            shard.map.erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDispatch.value_or_default())
                break;

            for (auto& [key, val] : val0)
            {
                // LOG_DEBUG("Waiting _drawMutex {:X}", (size_t)val.buffer);
                std::lock_guard<std::mutex> lock(_drawMutex);

                if (DLSSNRLatePass::Pending() && val.type == SRV) continue;
                val.captureInfo |= CaptureInfo::Dispatch;
                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                {
                    break;
                }
            }
        } while (false);
    }
}

#pragma endregion

void ResTrack_Dx12::HookResource(ID3D12Device* InDevice)
{
    if (o_Release != nullptr)
        return;

    ID3D12Resource* tmp = nullptr;
    auto d = CD3DX12_RESOURCE_DESC::Buffer(4);
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    HRESULT hr = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &d,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tmp));

    if (hr == S_OK)
    {
        PVOID* pVTable = *(PVOID**) tmp;
        o_Release = (PFN_Release) pVTable[2];

        if (o_Release != nullptr)
        {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&) o_Release, hkRelease);
            auto detourResult = DetourTransactionCommit();

            if (detourResult != NO_ERROR)
            {
                LOG_ERROR("Failed to hook Heap Release: {:X}", detourResult);
                o_Release = nullptr;
                tmp->Release();
            }
            else
            {
                o_Release(tmp); // drop temp
            }
        }
        else
        {
            tmp->Release();
        }
    }
}

static std::atomic<uint64_t> commandHookGeneration {0};
template<class Hook>
static bool InstallLateLifecycleMethod(void* target, typename Hook::Function callback, const char* name)
{
    bool added = false;
    const auto result = Hook::Install(target, callback, &added);
    if (added) LOG_INFO("[DLSSNR_LATE] lifecycle hook={} target=[{}]", name, LateAddress(target));
    return result == NO_ERROR;
}
bool ResTrack_Dx12::ObserveCommandList(ID3D12GraphicsCommandList* commands)
{
    if (!commands || DLSSNRCommandState::suppress || State::Instance().isShuttingDown) return false;
    DLSSNRPipelineSplit::Observe(commands);
    bool ready = true;
    auto table = *reinterpret_cast<void***>(commands);
    // OM bindings and Reset repeatedly observe the same implementation. Cache
    // only complete installations, with teardown invalidating every thread.
    static thread_local uint64_t seenGeneration = ~uint64_t(0);
    static thread_local bool seenTracking = false;
    static thread_local std::unordered_set<void**> observed;
    const auto generation = commandHookGeneration.load(std::memory_order_acquire);
    const bool tracking = DLSSNRLatePass::TrackDescriptors();
    if (seenGeneration != generation || seenTracking != tracking)
    { observed.clear(); seenGeneration = generation; seenTracking = tracking; }
    if (observed.contains(table)) return true;
    ready &= InstallLateLifecycleMethod<CaptureReset>(table[10], hkLateReset, "Reset");
    ready &= InstallLateLifecycleMethod<CaptureClose>(table[9], hkClose, "Close");
    ready &= InstallLateLifecycleMethod<CaptureBundle>(table[27], hkExecuteBundle, "ExecuteBundle");
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> list4;
    if (SUCCEEDED(commands->QueryInterface(IID_PPV_ARGS(&list4))))
    {
        auto table4 = *reinterpret_cast<void***>(list4.Get());
        ready &= InstallLateLifecycleMethod<CaptureBeginRenderPass>(table4[68], hkLateBeginRenderPass, "BeginRenderPass");
        ready &= InstallLateLifecycleMethod<CaptureEndRenderPass>(table4[69], hkLateEndRenderPass, "EndRenderPass");
    }
    {
        bool added = false;
        const auto result = CaptureDrawInstanced::Install(table[12], hkDrawInstanced, &added);
        ready &= result == NO_ERROR;
    }
    {
        bool added = false;
        const auto result = CaptureDrawIndexedInstanced::Install(table[13], hkDrawIndexedInstanced, &added);
        ready &= result == NO_ERROR;
    }
    {
        bool added = false;
        const auto result = CaptureDispatch::Install(table[14], hkDispatch, &added);
        ready &= result == NO_ERROR;
    }
    {
        bool added = false;
        const auto result = CaptureSetComputeRootDescriptorTable::Install(table[31], hkSetComputeRootDescriptorTable, &added);
        ready &= result == NO_ERROR;
    }
    {
        bool added = false;
        const auto result = CaptureSetGraphicsRootDescriptorTable::Install(table[32], hkSetGraphicsRootDescriptorTable, &added);
        ready &= result == NO_ERROR;
    }
    ready &= D3D12Hooks::TrackLateCommandState(commands);
    if (!ready) DLSSNRCommandState::Invalidate(commands);
    else observed.insert(table);
    return ready;
}

void ResTrack_Dx12::HookCommandList(ID3D12Device* InDevice)
{

    if (o_OMSetRenderTargets != nullptr)
        return;

    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12CommandAllocator* commandAllocator = nullptr;

    if (InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)) == S_OK)
    {
        if (InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, nullptr,
                                        IID_PPV_ARGS(&commandList)) == S_OK)
        {
            ID3D12GraphicsCommandList* realCL = nullptr;
            if (!CheckForRealObject(__FUNCTION__, commandList, (IUnknown**) &realCL))
                realCL = commandList;

            // Get the vtable pointer
            PVOID* pVTable = *(PVOID**) realCL;
            // hudless shader
            o_OMSetRenderTargets = (PFN_OMSetRenderTargets) pVTable[46];
            o_SetGraphicsRootDescriptorTable = (PFN_SetGraphicsRootDescriptorTable) pVTable[32];

            o_DrawInstanced = (PFN_DrawInstanced) pVTable[12];
            o_DrawIndexedInstanced = (PFN_DrawIndexedInstanced) pVTable[13];
            o_Dispatch = (PFN_Dispatch) pVTable[14];

            // hudless compute
            o_SetComputeRootDescriptorTable = (PFN_SetComputeRootDescriptorTable) pVTable[31];

            ObserveCommandList(realCL);
            if (o_OMSetRenderTargets != nullptr)
            {
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                // Keep the existing capture hooks available for the runtime late-NR toggle.
                DetourAttach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook CommandList methods: {:X}", detourResult);
                    o_OMSetRenderTargets = nullptr;
                    o_SetGraphicsRootDescriptorTable = nullptr;
                    o_DrawInstanced = nullptr;
                    o_DrawIndexedInstanced = nullptr;
                    o_Dispatch = nullptr;
                    o_SetComputeRootDescriptorTable = nullptr;
                }
            }

            commandList->Close();
            commandList->Release();
        }

        commandAllocator->Reset();
        commandAllocator->Release();
    }
}

void ResTrack_Dx12::HookToQueue(ID3D12Device* InDevice)
{
    if (o_ExecuteCommandLists != nullptr)
        return;

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    auto hr = InDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));

    if (hr == S_OK)
    {
        ID3D12CommandQueue* realQueue = nullptr;
        if (!CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &realQueue))
            realQueue = queue;

        // Get the vtable pointer
        PVOID* pVTable = *(PVOID**) realQueue;

        o_ExecuteCommandLists = (PFN_ExecuteCommandLists) pVTable[10];

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_ExecuteCommandLists != nullptr)
            DetourAttach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook CommandList methods: {:X}", detourResult);
            o_ExecuteCommandLists = nullptr;
        }

        queue->Release();
    }
}

void ResTrack_Dx12::EnsureQueueHook(ID3D12Device* device)
{
    static std::mutex queueHookMutex;
    std::lock_guard lock(queueHookMutex);
    HookToQueue(device);
}



// Observe creation before the first game binding, including a supplied PSO.
using LateCreateListFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE,
    ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using CaptureCreateList = DLSSNRMethodHooks::MethodHook<4012, LateCreateListFn, true>;
using LateCreateList1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE,
    D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
using CaptureCreateList1 = DLSSNRMethodHooks::MethodHook<4051, LateCreateList1Fn, true>;
static void LateListCreated(HRESULT result, void** output, ID3D12PipelineState* pipeline, bool open)
{
    if (FAILED(result) || !output || !*output || DLSSNRCommandState::suppress) return;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&list)))) return;
    const bool ready = ResTrack_Dx12::ObserveCommandList(list.Get());
    DLSSNRCommandState::Reset(list.Get(), pipeline, open && ready && DLSSNRCommandState::CaptureEnabled(DLSSNRLatePass::Enabled()));
}
static HRESULT STDMETHODCALLTYPE hkLateCreateList(ID3D12Device* device, UINT node, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* pipeline, REFIID iid, void** output)
{
    const auto result = CaptureCreateList::Forward(device, node, type, allocator, pipeline, iid, output);
    LateListCreated(result, output, pipeline, true);
    return result;
}
static HRESULT STDMETHODCALLTYPE hkLateCreateList1(ID3D12Device4* device, UINT node, D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags, REFIID iid, void** output)
{
    const auto result = CaptureCreateList1::Forward(device, node, type, flags, iid, output);
    LateListCreated(result, output, nullptr, false);
    return result;
}
void ResTrack_Dx12::HookDevice(ID3D12Device* device)
{
    if (o_CreateDescriptorHeap != nullptr ||
        (State::Instance().activeFgInput == FGInput::NvngxFG && !DLSSNRLatePass::TrackDescriptors()))
        return;

    if (device == nullptr)
        return;

    if (fgHeaps.capacity() < 65536)
    {
        _useShards = Config::Instance()->FGUseShards.value_or_default();
        for (auto& shard : _resourceReferenceShards)
            shard.resources.reserve(32);
        fgHeaps.reserve(65536);
    }

    LOG_FUNC();

    ID3D12Device* realDevice = nullptr;
    if (!CheckForRealObject(__FUNCTION__, device, (IUnknown**) &realDevice))
        realDevice = device;

    // Get the vtable pointer
    PVOID* pVTable = *(PVOID**) realDevice;

    // Hudfix
    o_CreateDescriptorHeap = (PFN_CreateDescriptorHeap) pVTable[14];
    o_CreateShaderResourceView = (PFN_CreateShaderResourceView) pVTable[18];
    o_CreateUnorderedAccessView = (PFN_CreateUnorderedAccessView) pVTable[19];
    o_CreateRenderTargetView = (PFN_CreateRenderTargetView) pVTable[20];
    o_CreateSampler = (PFN_CreateSampler) pVTable[22];
    o_CopyDescriptors = (PFN_CopyDescriptors) pVTable[23];
    o_CopyDescriptorsSimple = (PFN_CopyDescriptorsSimple) pVTable[24];

    // o_CreateDepthStencilView = (PFN_CreateDepthStencilView) pVTable[21];
    // o_CreateConstantBufferView = (PFN_CreateConstantBufferView) pVTable[17];

    // Apply the detour

    if (o_CreateDescriptorHeap != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_CreateDescriptorHeap != nullptr)
            DetourAttach(&(PVOID&) o_CreateDescriptorHeap, hkCreateDescriptorHeap);

        if (o_CreateRenderTargetView != nullptr)
            DetourAttach(&(PVOID&) o_CreateRenderTargetView, hkCreateRenderTargetView);

        if (o_CreateShaderResourceView != nullptr)
            DetourAttach(&(PVOID&) o_CreateShaderResourceView, hkCreateShaderResourceView);

        if (o_CreateUnorderedAccessView != nullptr)
            DetourAttach(&(PVOID&) o_CreateUnorderedAccessView, hkCreateUnorderedAccessView);

        if (o_CopyDescriptors != nullptr)
            DetourAttach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);

        if (o_CopyDescriptorsSimple != nullptr)
            DetourAttach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook Descriptor methods: {:X}", detourResult);
            o_CreateDescriptorHeap = nullptr;
            o_CreateRenderTargetView = nullptr;
            o_CreateShaderResourceView = nullptr;
            o_CreateUnorderedAccessView = nullptr;
            o_CopyDescriptors = nullptr;
            o_CopyDescriptorsSimple = nullptr;
        }
    }

    HookToQueue(device);
    HookCommandList(device);
    if (const auto result = CaptureCreateList::Install(pVTable[12], hkLateCreateList); result != NO_ERROR)
        LOG_ERROR("Failed to hook CreateCommandList: {}", result);
    Microsoft::WRL::ComPtr<ID3D12Device4> device4;
    if (SUCCEEDED(realDevice->QueryInterface(IID_PPV_ARGS(&device4))))
        if (const auto result = CaptureCreateList1::Install(
                (*reinterpret_cast<void***>(device4.Get()))[51], hkLateCreateList1); result != NO_ERROR)
            LOG_ERROR("Failed to hook CreateCommandList1: {}", result);
    HookResource(device);
    if (DLSSNRLatePass::TrackDescriptors())
        LOG_INFO("[DLSSNR_LATE] descriptor tracking initialized before capture; FG-independent metadata tracking=1");
}

static void RemoveCaptureMethodHooks()
{
    commandHookGeneration.fetch_add(1, std::memory_order_release);
    D3D12Hooks::ReleaseLateCommandStateHooks();
    if (const auto result = CaptureCreateList::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove CaptureCreateList hooks: {}", result);
    if (const auto result = CaptureCreateList1::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove CaptureCreateList1 hooks: {}", result);
    if (const auto result = CaptureReset::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove CaptureReset hooks: {}", result);
    if (const auto result = CaptureClose::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove CaptureClose hooks: {}", result);
    if (const auto result = CaptureBundle::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove CaptureBundle hooks: {}", result);
    if (const auto result = CaptureBeginRenderPass::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove CaptureBeginRenderPass hooks: {}", result);
    if (const auto result = CaptureEndRenderPass::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove CaptureEndRenderPass hooks: {}", result);
    if (const auto result = CaptureDrawInstanced::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove DrawInstanced entry hooks: {}", result);
    if (const auto result = CaptureDrawIndexedInstanced::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove DrawIndexedInstanced entry hooks: {}", result);
    if (const auto result = CaptureDispatch::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove Dispatch entry hooks: {}", result);
    if (const auto result = CaptureSetComputeRootDescriptorTable::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove SetComputeRootDescriptorTable entry hooks: {}", result);
    if (const auto result = CaptureSetGraphicsRootDescriptorTable::Remove(); result != NO_ERROR)
        LOG_ERROR("Failed to remove SetGraphicsRootDescriptorTable entry hooks: {}", result);
}

void ResTrack_Dx12::ReleaseDeviceHooks()
{
    LOG_DEBUG("");


    RemoveCaptureMethodHooks();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_CreateDescriptorHeap != nullptr)
        DetourDetach(&(PVOID&) o_CreateDescriptorHeap, hkCreateDescriptorHeap);

    if (o_CreateRenderTargetView != nullptr)
        DetourDetach(&(PVOID&) o_CreateRenderTargetView, hkCreateRenderTargetView);

    if (o_CreateShaderResourceView != nullptr)
        DetourDetach(&(PVOID&) o_CreateShaderResourceView, hkCreateShaderResourceView);

    if (o_CreateUnorderedAccessView != nullptr)
        DetourDetach(&(PVOID&) o_CreateUnorderedAccessView, hkCreateUnorderedAccessView);

    if (o_CopyDescriptors != nullptr)
        DetourDetach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);

    if (o_CopyDescriptorsSimple != nullptr)
        DetourDetach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);

    // Queue
    if (o_ExecuteCommandLists != nullptr)
        DetourDetach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);

    // CommandList
    if (o_OMSetRenderTargets != nullptr)
        DetourDetach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);


    // Resource
    if (o_Release != nullptr)
        DetourDetach(&(PVOID&) o_Release, hkRelease);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook Resource methods: {:X}", detourResult);
    }
    else
    {
        // Device
        o_CreateDescriptorHeap = nullptr;
        o_CreateRenderTargetView = nullptr;
        o_CreateShaderResourceView = nullptr;
        o_CreateUnorderedAccessView = nullptr;
        o_CopyDescriptors = nullptr;
        o_CopyDescriptorsSimple = nullptr;

        // Queue
        o_ExecuteCommandLists = nullptr;

        // CommandList
        o_OMSetRenderTargets = nullptr;
        o_SetGraphicsRootDescriptorTable = nullptr;
        o_SetComputeRootDescriptorTable = nullptr;
        o_DrawIndexedInstanced = nullptr;
        o_DrawInstanced = nullptr;
        o_Dispatch = nullptr;

        // Resource
        o_Release = nullptr;
    }
}

void ResTrack_Dx12::ReleaseHooks()
{
    LOG_DEBUG("");


    RemoveCaptureMethodHooks();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    // if (o_CreateDescriptorHeap != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateDescriptorHeap, hkCreateDescriptorHeap);

    // if (o_CreateRenderTargetView != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateRenderTargetView, hkCreateRenderTargetView);

    // if (o_CreateShaderResourceView != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateShaderResourceView, hkCreateShaderResourceView);

    // if (o_CreateUnorderedAccessView != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateUnorderedAccessView, hkCreateUnorderedAccessView);

    // if (o_CopyDescriptors != nullptr)
    //     DetourDetach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);

    // if (o_CopyDescriptorsSimple != nullptr)
    //     DetourDetach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);

    // o_CreateDescriptorHeap = nullptr;
    // o_CreateRenderTargetView = nullptr;
    // o_CreateShaderResourceView = nullptr;
    // o_CreateUnorderedAccessView = nullptr;
    // o_CopyDescriptors = nullptr;
    // o_CopyDescriptorsSimple = nullptr;

    // if (o_ExecuteCommandLists != nullptr)
    //     DetourAttach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);

    // o_ExecuteCommandLists = nullptr;

    // if (o_Release != nullptr)
    //     DetourAttach(&(PVOID&) o_Release, hkRelease);

    // o_Release = nullptr;

    if (o_OMSetRenderTargets != nullptr)
        DetourDetach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);


    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook CommandList methods: {:X}", detourResult);
    }
    else
    {
        o_OMSetRenderTargets = nullptr;
        o_SetGraphicsRootDescriptorTable = nullptr;
        o_SetComputeRootDescriptorTable = nullptr;
        o_DrawIndexedInstanced = nullptr;
        o_DrawInstanced = nullptr;
        o_Dispatch = nullptr;
    }
}

void ResTrack_Dx12::ClearPossibleHudless()
{
    LOG_DEBUG("");

    auto hfIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
        fgPossibleHudless[hfIndex].clear();
    }
    else
    {
        for (size_t i = 0; i < SHARD_COUNT; i++)
        {
            auto& shard = _hudlessShards[hfIndex][i];

#ifdef USE_SPINLOCK_MUTEX
            std::lock_guard<SpinLock> lock(shard.mutex);
#else
            std::lock_guard<std::mutex> lock(shard.mutex);
#endif

            shard.map.clear();
        }
    }

    std::lock_guard<std::mutex> lock2(_resourceCommandListMutex);

    auto fg = State::Instance().currentFG;
    if (fg != nullptr)
    {
        auto fIndex = fg->GetIndex();

        if (_notFoundCmdLists.size() > 10)
            _notFoundCmdLists.clear();

        for (const auto& pair : _resourceCommandList[fIndex])
        {
            LOG_WARN("{} cmdList: {:X}, not closed!", (UINT) pair.first, (size_t) pair.second);
            _notFoundCmdLists.insert(pair.second);
        }

        _resourceCommandList[fIndex].clear();

        for (const auto& pair : _resCmdList[fIndex])
        {
            LOG_WARN("{} cmdList: {:X}, not executed!", (UINT) pair.first, (size_t) pair.second);
            _notFoundCmdLists.insert(pair.second);
        }

        _resCmdList[fIndex].clear();
    }
}

void ResTrack_Dx12::SetResourceCmdList(FG_ResourceType type, ID3D12GraphicsCommandList* cmdList)
{
    auto fg = State::Instance().currentFG;
    if (fg != nullptr && fg->IsActive())
    {
        auto index = fg->GetIndex();

        ID3D12GraphicsCommandList* realCmdList = nullptr;
        if (!CheckForRealObject(__FUNCTION__, cmdList, (IUnknown**) &realCmdList))
            realCmdList = cmdList;

        _resourceCommandList[index][type] = realCmdList;
        LOG_DEBUG("_resourceCommandList[{}][{}]: {:X}", index, magic_enum::enum_name(type), (size_t) realCmdList);
    }
}
