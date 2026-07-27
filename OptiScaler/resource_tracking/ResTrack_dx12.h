#pragma once

#include "SysUtils.h"

#include <hudfix/Hudfix_Dx12.h>
#include <framegen/IFGFeature_Dx12.h>

#include <ankerl/unordered_dense.h>

#include <new>
#include <mutex>
#include <atomic>
#include <shared_mutex>

// #define DEBUG_TRACKING

#ifdef DEBUG_TRACKING
static void TestResource(ResourceInfo* info)
{
    if (info == nullptr || info->buffer == nullptr)
        return;

    auto desc = info->buffer->GetDesc();

    if (desc.Width != info->width || desc.Height != info->height || desc.Format != info->format)
    {
        LOG_TRACK("Resource mismatch: {:X}, info: {:X}", (size_t) info->buffer, (size_t) info);

        // LOG_WARN("Resource mismatch: {:X}, info: {:X}", (size_t) info->buffer, (size_t) info);
        //__debugbreak();
    }
}
#endif

#define USE_SPINLOCK_MUTEX

#ifdef USE_SPINLOCK_MUTEX

// #define USE_PERF_SPINLOCK

#ifdef __cpp_lib_hardware_interference_size
constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
constexpr size_t CACHE_LINE_SIZE = 64;
#endif
#else
#ifdef __cpp_lib_hardware_interference_size
constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size * 2;
#else
constexpr size_t CACHE_LINE_SIZE = 128;
#endif
#endif

#ifdef USE_SPINLOCK_MUTEX
#ifdef USE_PERF_SPINLOCK
class SpinLock
{
    std::atomic<bool> _lock = { false };

  public:
    void lock()
    {
        int backoff = 1;

        while (true)
        {
            // 1. Optimistic Read (TTAS)
            // Using 'relaxed' because we don't need ordering until we actually acquire.
            if (!_lock.load(std::memory_order_relaxed))
            {

                // 2. Attempt Acquire
                // 'acquire' ensures no memory ops move before this lock
                if (!_lock.exchange(true, std::memory_order_acquire))
                {
                    return; // Success
                }
            }

            // 3. Pause instruction to help HT and branch prediction
            _mm_pause();
        }
    }

    void unlock()
    {
        // 'release' ensures all memory ops are finished before unlocking
        _lock.store(false, std::memory_order_release);
    }
};
#else
struct SpinLock
{
    std::atomic<bool> _lock = { false };

    __forceinline void lock()
    {
        // Fast path: try to grab immediately
        if (!_lock.exchange(true, std::memory_order_acquire))
            return;

        int backoff = 1;
        while (true)
        {
            while (_lock.load(std::memory_order_relaxed))
            {
                for (int i = 0; i < backoff; ++i)
                    _mm_pause();

                backoff = std::min(backoff * 2, 64);
            }

            if (!_lock.exchange(true, std::memory_order_acquire))
                return;
        }
    }

    __forceinline void unlock() { _lock.store(false, std::memory_order_release); }
};
#endif
#endif

static ankerl::unordered_dense::map<ID3D12Resource*, std::vector<ResourceInfo*>> _trackedResources;
#ifdef USE_SPINLOCK_MUTEX
static SpinLock _trackedResourcesMutex;
#else
static std::mutex _trackedResourcesMutex;
#endif

struct HeapInfo
{
    // mutable std::shared_mutex mutex;

    ID3D12DescriptorHeap* heap = nullptr;
    SIZE_T cpuStart = 0;
    SIZE_T cpuEnd = 0;
    SIZE_T gpuStart = 0;
    SIZE_T gpuEnd = 0;
    UINT numDescriptors = 0;
    UINT increment = 0;
    UINT type = 0;
    std::shared_ptr<ResourceInfo[]> info;
    UINT lastOffset = 0;
    bool active = true;
    std::atomic<uint64_t> version { 0 };

    HeapInfo(ID3D12DescriptorHeap* heap, SIZE_T cpuStart, SIZE_T cpuEnd, SIZE_T gpuStart, SIZE_T gpuEnd,
             UINT numResources, UINT increment, UINT type)
        : heap(heap), cpuStart(cpuStart), cpuEnd(cpuEnd), gpuStart(gpuStart), gpuEnd(gpuEnd),
          numDescriptors(numResources), increment(increment), type(type), info(new ResourceInfo[numResources])
    {
        static std::atomic<uint64_t> globalHeapVersion { 1 };
        version.store(globalHeapVersion.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);

        for (size_t i = 0; i < numDescriptors; i++)
        {
            info[i].buffer = nullptr;
        }
    }

    void DetachFromOldResource(SIZE_T index) const
    {
        if (info[index].buffer == nullptr)
            return;

        std::scoped_lock lock(_trackedResourcesMutex);
        LOG_TRACK("Heap: {:X}, Index: {}, Resource: {:X}, Res: {}x{}, Format: {}", (size_t) this, index,
                  (size_t) info[index].buffer, info[index].width, info[index].height, (UINT) info[index].format);
        auto it = _trackedResources.find(info[index].buffer);
        if (it != _trackedResources.end())
        {
            auto& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), &info[index]), vec.end());
            if (vec.empty())
                _trackedResources.erase(it);
        }
    }

    void AttachToNewResource(SIZE_T index) const
    {
        std::scoped_lock lock(_trackedResourcesMutex);
        LOG_TRACK("Heap: {:X}, Index: {}, Resource: {:X}, Res: {}x{}, Format: {}", (size_t) this, index,
                  (size_t) info[index].buffer, info[index].width, info[index].height, (UINT) info[index].format);
        auto& vec = _trackedResources[info[index].buffer];
        if (std::find(vec.begin(), vec.end(), &info[index]) == vec.end())
            vec.push_back(&info[index]);
    }

    ResourceInfo* GetByCpuHandle(SIZE_T cpuHandle) const
    {
        auto index = (cpuHandle - cpuStart) / increment;

        if (index >= numDescriptors)
            return nullptr;

        // std::shared_lock<std::shared_mutex> lock(mutex);

        if (info[index].buffer == nullptr)
            return nullptr;

#ifdef DEBUG_TRACKING
        TestResource(&info[index]);
#endif

        return &info[index];
    }

    ResourceInfo* GetByGpuHandle(SIZE_T gpuHandle) const
    {
        auto index = (gpuHandle - gpuStart) / increment;

        if (index >= numDescriptors)
            return nullptr;

        // std::shared_lock<std::shared_mutex> lock(mutex);

        if (info[index].buffer == nullptr)
            return nullptr;

#ifdef DEBUG_TRACKING
        TestResource(&info[index]);
#endif

        return &info[index];
    }

    void SetByCpuHandle(SIZE_T cpuHandle, ResourceInfo setInfo) const
    {
        auto index = (cpuHandle - cpuStart) / increment;

        if (index >= numDescriptors)
            return;

        // std::unique_lock<std::shared_mutex> lock(mutex);

#ifdef DEBUG_TRACKING
        TestResource(&setInfo);
#endif
        if (info[index].buffer != setInfo.buffer)
        {
            DetachFromOldResource(index);
            info[index] = setInfo;
            AttachToNewResource(index);
        }
        else
        {
            info[index] = setInfo;
        }
    }

    void SetByGpuHandle(SIZE_T gpuHandle, ResourceInfo setInfo) const
    {
        auto index = (gpuHandle - gpuStart) / increment;

        if (index >= numDescriptors)
            return;

        // std::unique_lock<std::shared_mutex> lock(mutex);

#ifdef DEBUG_TRACKING
        TestResource(&setInfo);
#endif

        if (info[index].buffer != setInfo.buffer)
        {
            DetachFromOldResource(index);
            info[index] = setInfo;
            AttachToNewResource(index);
        }
        else
        {
            info[index] = setInfo;
        }
    }

    void ClearByCpuHandle(SIZE_T cpuHandle) const
    {
        auto index = (cpuHandle - cpuStart) / increment;

        if (index >= numDescriptors)
            return;

        // std::unique_lock<std::shared_mutex> lock(mutex);

        if (info[index].buffer != nullptr)
        {
            LOG_TRACK("Resource: {:X}, Res: {}x{}, Format: {}", (size_t) info[index].buffer, info[index].width,
                      info[index].height, (UINT) info[index].format);

            DetachFromOldResource(index);
        }

        info[index].buffer = nullptr;
        info[index].lastUsedFrame = 0;
    }

    void ClearByGpuHandle(SIZE_T gpuHandle) const
    {
        auto index = (gpuHandle - gpuStart) / increment;

        if (index >= numDescriptors)
            return;

        // std::unique_lock<std::shared_mutex> lock(mutex);

        if (info[index].buffer != nullptr)
        {
            LOG_TRACK("Resource: {:X}, Res: {}x{}, Format: {}", (size_t) info[index].buffer, info[index].width,
                      info[index].height, (UINT) info[index].format);

            DetachFromOldResource(index);
        }

        info[index].buffer = nullptr;
        info[index].lastUsedFrame = 0;
    }
};

struct ResourceHeapInfo
{
    SIZE_T cpuStart = NULL;
    SIZE_T gpuStart = NULL;
};

#ifdef USE_SPINLOCK_MUTEX
// Force each struct to start on a new cache line
struct alignas(CACHE_LINE_SIZE) CommandListShard
{
    SpinLock mutex;
    ankerl::unordered_dense::map<ID3D12GraphicsCommandList*,
                                 ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo>>
        map;

    char padding[CACHE_LINE_SIZE - ((sizeof(SpinLock) + sizeof(void*)) % CACHE_LINE_SIZE)] = {};
};
#else
struct alignas(CACHE_LINE_SIZE) CommandListShard
{
    std::mutex mutex;
    ankerl::unordered_dense::map<ID3D12GraphicsCommandList*,
                                 ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo>>
        map;

    char padding[CACHE_LINE_SIZE - ((sizeof(std::mutex) + sizeof(void*)) % CACHE_LINE_SIZE)] = {};
};
#endif

class ResTrack_Dx12
{
  private:
    inline static bool _presentDone = true;
    inline static std::mutex _drawMutex;
    inline static bool _useShards = false;

    inline static std::mutex _resourceCommandListMutex;
    inline static std::unordered_map<FG_ResourceType, ID3D12GraphicsCommandList*> _resourceCommandList[BUFFER_COUNT];

    inline static ULONG64 _lastHudlessFrame = 0;
    inline static std::mutex _hudlessMutex;
    inline static void* _hudlessMutexQueue = nullptr;

    static bool IsHudFixActive();

    // static bool IsFGCommandList(IUnknown* cmdList);

    static void hkCopyDescriptors(ID3D12Device* This, UINT NumDestDescriptorRanges,
                                  D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                  UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                  D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                  UINT* pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);
    static void hkCopyDescriptorsSimple(ID3D12Device* This, UINT NumDescriptors,
                                        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                        D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);

    static void hkSetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT RootParameterIndex,
                                                 D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);
    static void hkOMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors,
                                     D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                     BOOL RTsSingleHandleToDescriptorRange,
                                     D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor);
    static void hkSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT RootParameterIndex,
                                                D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);

    static void hkDrawInstanced(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance, UINT InstanceCount,
                                UINT StartVertexLocation, UINT StartInstanceLocation);
    static void hkDrawIndexedInstanced(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance, UINT InstanceCount,
                                       UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation);
    static void hkDispatch(ID3D12GraphicsCommandList* This, UINT ThreadGroupCountX, UINT ThreadGroupCountY,
                           UINT ThreadGroupCountZ);

    static void hkExecuteBundle(ID3D12GraphicsCommandList* This, ID3D12GraphicsCommandList* pCommandList);

    static HRESULT hkClose(ID3D12GraphicsCommandList* This);

    static void hkCreateRenderTargetView(ID3D12Device* This, ID3D12Resource* pResource,
                                         D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                         D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
    static void hkCreateShaderResourceView(ID3D12Device* This, ID3D12Resource* pResource,
                                           D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
    static void hkCreateUnorderedAccessView(ID3D12Device* This, ID3D12Resource* pResource,
                                            ID3D12Resource* pCounterResource, D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

    static void hkExecuteCommandLists(ID3D12CommandQueue* This, UINT NumCommandLists,
                                      ID3D12CommandList* const* ppCommandLists);

    static HRESULT hkCreateDescriptorHeap(ID3D12Device* This, D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
                                          REFIID riid, void** ppvHeap);

    static ULONG hkRelease(ID3D12Resource* This);

    static void HookCommandList(ID3D12Device* InDevice);
    static void HookToQueue(ID3D12Device* InDevice);
    static void HookResource(ID3D12Device* InDevice);

    static bool CheckResource(ID3D12Resource* resource);

    static bool CheckForRealObject(const std::string functionName, IUnknown* pObject, IUnknown** ppRealObject);

    static bool CreateBufferResource(ID3D12Device* InDevice, ResourceInfo* InSource, D3D12_RESOURCE_STATES InState,
                                     ID3D12Resource** OutResource);

    static void ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState);

    static SIZE_T GetGPUHandle(ID3D12Device* This, SIZE_T cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE type);
    static SIZE_T GetCPUHandle(ID3D12Device* This, SIZE_T gpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE type);

    static HeapInfo* GetHeapByCpuHandleCBV(SIZE_T cpuHandle);
    static HeapInfo* GetHeapByCpuHandleRTV(SIZE_T cpuHandle);
    static HeapInfo* GetHeapByCpuHandleSRV(SIZE_T cpuHandle);
    static HeapInfo* GetHeapByCpuHandleUAV(SIZE_T cpuHandle);
    static HeapInfo* GetHeapByCpuHandle(SIZE_T cpuHandle);
    static HeapInfo* GetHeapByGpuHandleGR(SIZE_T gpuHandle);
    static HeapInfo* GetHeapByGpuHandleCR(SIZE_T gpuHandle);

    static void FillResourceInfo(ID3D12Resource* resource, ResourceInfo* info);

    // Sharding
    inline static constexpr size_t SHARD_COUNT = 16;
    inline static CommandListShard _hudlessShards[BUFFER_COUNT][SHARD_COUNT];

    inline static size_t GetShardIndex(ID3D12GraphicsCommandList* ptr)
    {
        auto addr = (UINT64) ptr;
        return (addr >> 4) % SHARD_COUNT;
    }

  public:
    static void HookDevice(ID3D12Device* device);
    static void EnsureQueueHook(ID3D12Device* device);
    static void ReleaseHooks();
    static void ReleaseDeviceHooks();
    static void ClearPossibleHudless();
    static void SetResourceCmdList(FG_ResourceType type, ID3D12GraphicsCommandList* cmdList);
};
