#include <pch.h>
#include "DLSSNRPipelineTrace.h"
#include "DLSSNRPipelineCapture.h"
#include "DLSSNRMethodHooks.h"
#include <Config.h>
#include <State.h>
#include <Util.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <format>

namespace DLSSNRPipelineTrace
{
namespace
{
std::mutex controlMutex, eventMutex;
std::atomic<bool> recording {false};
Capture capture;
bool armed = false;
uint64_t session = 0;
std::chrono::steady_clock::time_point start;
std::unordered_set<ID3D12GraphicsCommandList*> afterNR;
using Microsoft::WRL::ComPtr;
constexpr size_t ObjectLimit = 128;
std::unordered_map<ID3D12CommandQueue*, ComPtr<ID3D12CommandQueue>> queues;
std::unordered_map<ID3D12Fence*, ComPtr<ID3D12Fence>> fences;
std::unordered_map<ID3D12Resource*, ComPtr<ID3D12Resource>> resources;
std::unordered_set<void*> describedQueues, describedFences;
uint64_t objectsLost = 0;
std::atomic<uint64_t> nextCall {0};

using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using FenceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
using ExecuteHook = DLSSNRMethodHooks::MethodHook<1010, ExecuteFn>;
using SignalHook = DLSSNRMethodHooks::MethodHook<1014, FenceFn>;
using WaitHook = DLSSNRMethodHooks::MethodHook<1015, FenceFn>;
using CpuSignalFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64);
using CpuSignalHook = DLSSNRMethodHooks::MethodHook<1110, CpuSignalFn>;
using EventFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64, HANDLE);
using EventHook = DLSSNRMethodHooks::MethodHook<1109, EventFn>;
using CreateQueueFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using CreateFenceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**);
using CreateQueueHook = DLSSNRMethodHooks::MethodHook<1208, CreateQueueFn>;
using CreateFenceHook = DLSSNRMethodHooks::MethodHook<1236, CreateFenceFn>;
using ResidentFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device3*, D3D12_RESIDENCY_FLAGS, UINT,
                                              ID3D12Pageable* const*, ID3D12Fence*, UINT64);
using ResidentHook = DLSSNRMethodHooks::MethodHook<1250, ResidentFn>;
using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using CopyFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using CopyTextureFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                               const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using BarrierHook = DLSSNRMethodHooks::MethodHook<1326, BarrierFn>;
using CopyHook = DLSSNRMethodHooks::MethodHook<1317, CopyFn>;
using CopyTextureHook = DLSSNRMethodHooks::MethodHook<1316, CopyTextureFn>;

// The system DXGI submission callback can wait on a fence whose producer does
// not call a public D3D12 Signal method. Correlate nested KMT operations with
// the actual D3D12 call, rather than guessing from equal numeric fence values.
struct FenceScope
{
    inline static thread_local ID3D12Fence* current = nullptr;
    inline static thread_local uint64_t currentCall = 0;
    ID3D12Fence* previous = current;
    uint64_t previousCall = currentCall;
    FenceScope(ID3D12Fence* fence, uint64_t call) { current = fence; currentCall = call; }
    ~FenceScope() { current = previous; currentCall = previousCall; }
};
const void* KmtHandle(UINT handle) { return reinterpret_cast<const void*>(uintptr_t(handle)); }
template<unsigned Id, class T> using KmtHook =
    DLSSNRMethodHooks::MethodHook<Id, NTSTATUS(APIENTRY*)(const T*)>;
using KmtCpuSignal = KmtHook<1400, D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU>;
using KmtCpuWait = KmtHook<1401, D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU>;
using KmtGpuWait = KmtHook<1402, D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU>;
using KmtGpuSignal = KmtHook<1403, D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU>;
using KmtGpuSignal2 = KmtHook<1404, D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2>;
using KmtSignal2 = KmtHook<1405, D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2>;
using KmtWait2 = KmtHook<1406, D3DKMT_WAITFORSYNCHRONIZATIONOBJECT2>;

void KmtObjects(uint64_t call, UINT context, UINT count, const D3DKMT_HANDLE* handles,
                const UINT64* values = nullptr)
{
    if (!call) return;
    Leave("kmt-parent-d3d12-call", call, FenceScope::current, nullptr, FenceScope::currentCall);
    if (handles) for (UINT i = 0; i < std::min(count, 64u); ++i)
    {
        Leave("kmt-sync-object", call, KmtHandle(context), KmtHandle(handles[i]), i);
        if (values) Leave("kmt-fence-value", call, KmtHandle(context), KmtHandle(handles[i]), values[i]);
    }
    if (count > 64) Leave("kmt-objects-truncated", call, KmtHandle(context), nullptr, count - 64);
}
NTSTATUS APIENTRY TraceKmtCpuSignal(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU* args)
{
    const auto call = args ? Enter("kmt-cpu-signal-enter", KmtHandle(args->hDevice), nullptr, args->ObjectCount) : 0;
    if (call)
    {
        KmtObjects(call, args->hDevice, args->ObjectCount, args->ObjectHandleArray, args->FenceValueArray);
        Leave("kmt-flags", call, nullptr, nullptr, args->Flags.Value);
    }
    const auto result = KmtCpuSignal::Forward(args);
    Leave("kmt-cpu-signal-return", call, nullptr, nullptr, 0, result);
    return result;
}
NTSTATUS APIENTRY TraceKmtCpuWait(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU* args)
{
    const auto call = args ? Enter("kmt-cpu-wait-enter", KmtHandle(args->hDevice), args->hAsyncEvent, args->ObjectCount) : 0;
    if (call) KmtObjects(call, args->hDevice, args->ObjectCount, args->ObjectHandleArray, args->FenceValueArray);
    const auto result = KmtCpuWait::Forward(args);
    Leave("kmt-cpu-wait-return", call, nullptr, nullptr, 0, result);
    return result;
}
NTSTATUS APIENTRY TraceKmtGpuWait(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU* args)
{
    const auto call = args ? Enter("kmt-gpu-wait-enter", KmtHandle(args->hContext), nullptr, args->ObjectCount) : 0;
    if (call)
    {
        KmtObjects(call, args->hContext, args->ObjectCount, args->ObjectHandleArray);
        // This union is a scalar for legacy fences and a pointer for monitored
        // fences. Without object-type metadata, never dereference it as an array.
        Leave("kmt-union-raw", call, nullptr, nullptr, args->Reserved[0]);
    }
    const auto result = KmtGpuWait::Forward(args);
    Leave("kmt-gpu-wait-return", call, nullptr, nullptr, 0, result);
    return result;
}
NTSTATUS APIENTRY TraceKmtGpuSignal(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU* args)
{
    const auto call = args ? Enter("kmt-gpu-signal-enter", KmtHandle(args->hContext), nullptr, args->ObjectCount) : 0;
    if (call) KmtObjects(call, args->hContext, args->ObjectCount, args->ObjectHandleArray, args->MonitoredFenceValueArray);
    const auto result = KmtGpuSignal::Forward(args);
    Leave("kmt-gpu-signal-return", call, nullptr, nullptr, 0, result);
    return result;
}
NTSTATUS APIENTRY TraceKmtGpuSignal2(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2* args)
{
    const auto call = args ? Enter("kmt-gpu-signal2-enter", nullptr, nullptr, args->ObjectCount) : 0;
    if (call)
    {
        KmtObjects(call, 0, args->ObjectCount, args->ObjectHandleArray);
        Leave("kmt-flags", call, nullptr, nullptr, args->Flags.Value);
        Leave("kmt-union-raw", call, nullptr, nullptr, args->Reserved[0]);
        if (args->BroadcastContextArray)
            for (UINT i = 0; i < std::min<UINT>(args->BroadcastContextCount, 64); ++i)
                Leave("kmt-broadcast-context", call, KmtHandle(args->BroadcastContextArray[i]), nullptr, i);
        if (args->BroadcastContextCount > 64)
            Leave("kmt-contexts-truncated", call, nullptr, nullptr, args->BroadcastContextCount - 64);
    }
    const auto result = KmtGpuSignal2::Forward(args);
    Leave("kmt-gpu-signal2-return", call, nullptr, nullptr, 0, result);
    return result;
}
NTSTATUS APIENTRY TraceKmtSignal2(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2* args)
{
    const auto call = args ? Enter("kmt-signal2-enter", KmtHandle(args->hContext), nullptr, args->ObjectCount) : 0;
    if (call)
    {
        KmtObjects(call, args->hContext, std::min<UINT>(args->ObjectCount, D3DDDI_MAX_OBJECT_SIGNALED), args->ObjectHandleArray);
        Leave("kmt-flags", call, nullptr, nullptr, args->Flags.Value);
        Leave(args->Flags.EnqueueCpuEvent ? "kmt-cpu-event-raw" : "kmt-scalar-fence-value",
              call, nullptr, nullptr, args->Reserved[0]);
    }
    const auto result = KmtSignal2::Forward(args);
    Leave("kmt-signal2-return", call, nullptr, nullptr, 0, result);
    return result;
}
NTSTATUS APIENTRY TraceKmtWait2(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECT2* args)
{
    const auto call = args ? Enter("kmt-wait2-enter", KmtHandle(args->hContext), nullptr, args->ObjectCount) : 0;
    if (call)
    {
        KmtObjects(call, args->hContext, std::min<UINT>(args->ObjectCount, D3DDDI_MAX_OBJECT_WAITED_ON), args->ObjectHandleArray);
        Leave("kmt-scalar-fence-value", call, nullptr, nullptr, args->Fence.FenceValue);
    }
    const auto result = KmtWait2::Forward(args);
    Leave("kmt-wait2-return", call, nullptr, nullptr, 0, result);
    return result;
}
template<class Hook> bool InstallKmtExport(const char* suffix, typename Hook::Function callback)
{
    bool found = false, complete = true;
    // Observe both the public thunk and syscall export. MethodHook deduplicates
    // nested forwarding of the same argument block within each API family.
    for (const auto& [moduleName, prefix] : {std::pair {L"gdi32.dll", "D3DKMT"},
                                           std::pair {L"win32u.dll", "NtGdiDdDDI"}})
    {
        const auto module = GetModuleHandleW(moduleName);
        const auto entry = module ? GetProcAddress(module, (std::string(prefix) + suffix).c_str()) : nullptr;
        if (!entry) continue;
        found = true;
        const auto status = Hook::Install(reinterpret_cast<void*>(entry), callback);
        Mark("kmt-hook-entry", module, reinterpret_cast<void*>(entry), status);
        if (status != NO_ERROR)
            LOG_WARN("[DLSSNR_PIPELINE] KMT hook failed {}{} status={}", prefix, suffix, status);
        complete = complete && status == NO_ERROR;
    }
    if (!found) LOG_WARN("[DLSSNR_PIPELINE] KMT export unavailable {}", suffix);
    return found && complete;
}
bool InstallKmt()
{
    bool complete = true;
    // Do not short-circuit installation when an optional export is absent.
    complete = InstallKmtExport<KmtCpuSignal>("SignalSynchronizationObjectFromCpu", TraceKmtCpuSignal) && complete;
    complete = InstallKmtExport<KmtCpuWait>("WaitForSynchronizationObjectFromCpu", TraceKmtCpuWait) && complete;
    complete = InstallKmtExport<KmtGpuWait>("WaitForSynchronizationObjectFromGpu", TraceKmtGpuWait) && complete;
    complete = InstallKmtExport<KmtGpuSignal>("SignalSynchronizationObjectFromGpu", TraceKmtGpuSignal) && complete;
    complete = InstallKmtExport<KmtGpuSignal2>("SignalSynchronizationObjectFromGpu2", TraceKmtGpuSignal2) && complete;
    complete = InstallKmtExport<KmtSignal2>("SignalSynchronizationObject2", TraceKmtSignal2) && complete;
    complete = InstallKmtExport<KmtWait2>("WaitForSynchronizationObject2", TraceKmtWait2) && complete;
    return complete;
}

bool Watched(ID3D12Resource* resource)
{
    std::lock_guard lock(eventMutex);
    return capture.active && resources.contains(resource);
}
void STDMETHODCALLTYPE Barrier(ID3D12GraphicsCommandList* commands, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    if (Active() && barriers) for (UINT i = 0; i < count; ++i)
    {
        const auto& b = barriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && Watched(b.Transition.pResource))
        {
            const auto call = Enter("watched-transition", commands, b.Transition.pResource,
                                     (uint64_t(b.Transition.StateBefore) << 32) | UINT(b.Transition.StateAfter));
            Leave("watched-transition-subresource-flags", call, commands, b.Transition.pResource,
                    (uint64_t(b.Transition.Subresource) << 32) | UINT(b.Flags));
        }
        else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV && b.UAV.pResource && Watched(b.UAV.pResource))
            Enter("watched-uav-barrier", commands, b.UAV.pResource);
        else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING &&
                 (Watched(b.Aliasing.pResourceBefore) || Watched(b.Aliasing.pResourceAfter)))
        {
            const auto call = Enter("watched-alias-before", commands, b.Aliasing.pResourceBefore);
            Leave("watched-alias-after", call, commands, b.Aliasing.pResourceAfter);
        }
    }
    BarrierHook::Forward(commands, count, barriers);
}
void TraceCopy(ID3D12GraphicsCommandList* commands, ID3D12Resource* destination, ID3D12Resource* source)
{
    if (!Active() || (!Watched(source) && !Watched(destination))) return;
    WatchResource(destination); // Track downstream full/partial scene copies as candidates.
    const auto call = Enter("watched-copy-source", commands, source);
    Leave("watched-copy-destination", call, commands, destination);
}
void STDMETHODCALLTYPE Copy(ID3D12GraphicsCommandList* commands, ID3D12Resource* destination, ID3D12Resource* source)
{
    TraceCopy(commands, destination, source);
    CopyHook::Forward(commands, destination, source);
}
void STDMETHODCALLTYPE CopyTexture(ID3D12GraphicsCommandList* commands, const D3D12_TEXTURE_COPY_LOCATION* destination,
                                     UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* box)
{
    if (destination && source) TraceCopy(commands, destination->pResource, source->pResource);
    CopyTextureHook::Forward(commands, destination, x, y, z, source, box);
}

void ObserveFence(ID3D12Fence* fence)
{
    if (!Active() || !fence) return;
    std::lock_guard lock(eventMutex);
    if (!capture.active || fences.contains(fence)) return;
    if (fences.size() == ObjectLimit) { ++objectsLost; return; }
    fences.emplace(fence, fence);
}

// Residency completion signals the supplied fence without calling either
// ID3D12CommandQueue::Signal or ID3D12Fence::Signal. Preserve the original
// asynchronous operation: its return is not a GPU/residency completion.
HRESULT STDMETHODCALLTYPE EnqueueResident(ID3D12Device3* device, D3D12_RESIDENCY_FLAGS flags, UINT count,
                                           ID3D12Pageable* const* objects, ID3D12Fence* fence, UINT64 value)
{
    ObserveFence(fence);
    const auto call = Enter("residency-signal-enter", device, fence, value);
    FenceScope scope(fence, call);
    Leave("residency-flags-count", call, device, fence, (uint64_t(UINT(flags)) << 32) | count);
    const auto result = ResidentHook::Forward(device, flags, count, objects, fence, value);
    Leave("residency-signal-return", call, device, fence, value, result);
    return result;
}

bool InstallResidency(ID3D12Device* device)
{
    ComPtr<ID3D12Device3> device3;
    const auto result = device->QueryInterface(IID_PPV_ARGS(&device3));
    if (FAILED(result))
    {
        Mark("residency-interface-unavailable", device, nullptr, UINT(result));
        return result == E_NOINTERFACE;
    }
    // ID3D12Device3 appends OpenExistingHeapFromAddress, FileMapping and
    // EnqueueMakeResident after ID3D12Device2::CreatePipelineState (slot 47).
    auto table = *reinterpret_cast<void***>(device3.Get());
    const LONG status = ResidentHook::Install(table[50], EnqueueResident);
    Mark("residency-signal-entry", device3.Get(), table[50], status);
    if (status != NO_ERROR)
        LOG_WARN("[DLSSNR_PIPELINE] residency discovery incomplete device=0x{:X} status={}",
                 (uintptr_t)device3.Get(), status);
    return status == NO_ERROR;
}

HRESULT STDMETHODCALLTYPE CpuSignal(ID3D12Fence* fence, UINT64 value)
{
    ObserveFence(fence);
    const auto call = Enter("cpu-signal-enter", fence, nullptr, value);
    FenceScope scope(fence, call);
    const auto result = CpuSignalHook::Forward(fence, value);
    Leave("cpu-signal-return", call, fence, nullptr, value, result);
    return result;
}
HRESULT STDMETHODCALLTYPE SetEvent(ID3D12Fence* fence, UINT64 value, HANDLE event)
{
    ObserveFence(fence);
    const auto call = Enter("fence-event-enter", fence, event, value);
    FenceScope scope(fence, call);
    const auto result = EventHook::Forward(fence, value, event);
    Leave("fence-event-return", call, fence, event, value, result);
    return result;
}
HRESULT STDMETHODCALLTYPE CreateQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc,
                                       REFIID iid, void** output)
{
    const auto call = Enter("create-queue-enter", device);
    const auto result = CreateQueueHook::Forward(device, desc, iid, output);
    Leave("create-queue-return", call, device, SUCCEEDED(result) && output ? *output : nullptr, 0, result);
    if (Active() && SUCCEEDED(result) && output && *output)
    {
        ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&queue)))) ObserveQueue(queue.Get());
    }
    return result;
}
HRESULT STDMETHODCALLTYPE CreateFence(ID3D12Device* device, UINT64 value, D3D12_FENCE_FLAGS flags,
                                       REFIID iid, void** output)
{
    const auto call = Enter("create-fence-enter", device, nullptr, value);
    const auto result = CreateFenceHook::Forward(device, value, flags, iid, output);
    Leave("create-fence-return", call, device, SUCCEEDED(result) && output ? *output : nullptr, value, result);
    if (Active() && SUCCEEDED(result) && output && *output)
    {
        ComPtr<ID3D12Fence> fence;
        if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&fence)))) ObserveFence(fence.Get());
    }
    return result;
}

void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    ObserveQueue(queue);
    const auto call = Enter("submit-enter", queue, nullptr, count);
    if (Active())
    {
        if (lists) for (UINT i = 0; i < count; ++i)
            Leave("submit-list", call, queue, lists[i], i);
    }
    ExecuteHook::Forward(queue, count, lists);
    Leave("submit-return", call, queue, nullptr, count);
}
HRESULT STDMETHODCALLTYPE Signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value)
{
    ObserveQueue(queue); ObserveFence(fence);
    const auto call = Enter("signal-enter", queue, fence, value);
    FenceScope scope(fence, call);
    const HRESULT result = SignalHook::Forward(queue, fence, value);
    Leave("signal-return", call, queue, fence, value, result);
    return result;
}
HRESULT STDMETHODCALLTYPE Wait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value)
{
    ObserveQueue(queue); ObserveFence(fence);
    const auto call = Enter("wait-enter", queue, fence, value);
    FenceScope scope(fence, call);
    if (call && fence) Leave("wait-completed-sample", call, queue, fence, fence->GetCompletedValue());
    const HRESULT result = WaitHook::Forward(queue, fence, value);
    Leave("wait-return", call, queue, fence, value, result);
    return result;
}

// Discover runtime/driver implementations for all three queue types. The
// existing multi-entry hook owns a trampoline for every distinct entry point.
bool InstallQueue(ID3D12CommandQueue* object)
{
    auto table = *reinterpret_cast<void***>(object);
    const LONG execute = ExecuteHook::Install(table[10], Execute);
    const LONG signal = SignalHook::Install(table[14], Signal);
    const LONG wait = WaitHook::Install(table[15], Wait);
    const auto desc = object->GetDesc();
    Mark("queue-description", object, table, (uint64_t(desc.NodeMask) << 32) | UINT(desc.Type));
    Mark("queue-priority-flags", object, nullptr, (uint64_t(UINT(desc.Priority)) << 32) | UINT(desc.Flags));
    Mark("queue-execute-entry", object, table[10], execute);
    Mark("queue-signal-entry", object, table[14], signal);
    Mark("queue-wait-entry", object, table[15], wait);
    if (execute || signal || wait)
        LOG_WARN("[DLSSNR_PIPELINE] queue discovery incomplete object=0x{:X} execute={} signal={} wait={}",
                 (uintptr_t)object, execute, signal, wait);
    return execute == NO_ERROR && signal == NO_ERROR && wait == NO_ERROR;
}
bool InstallFence(ID3D12Fence* fence)
{
    ComPtr<ID3D12Fence1> fence1;
    if (SUCCEEDED(fence->QueryInterface(IID_PPV_ARGS(&fence1))))
        Mark("fence-creation-flags", fence, nullptr, UINT(fence1->GetCreationFlags()));
    auto table = *reinterpret_cast<void***>(fence);
    const LONG signal = CpuSignalHook::Install(table[10], CpuSignal);
    const LONG event = EventHook::Install(table[9], SetEvent);
    Mark("fence-cpu-signal-entry", fence, table[10], signal);
    Mark("fence-event-entry", fence, table[9], event);
    if (signal || event)
        LOG_WARN("[DLSSNR_PIPELINE] fence discovery incomplete object=0x{:X} signal={} event={}",
                 (uintptr_t)fence, signal, event);
    return signal == NO_ERROR && event == NO_ERROR;
}
bool Install(ID3D12Device* device)
{
    bool complete = InstallResidency(device);
    complete = InstallKmt() && complete;
    for (const auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                            D3D12_COMMAND_LIST_TYPE_COPY})
    {
        D3D12_COMMAND_QUEUE_DESC desc {};
        desc.Type = type;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        const auto result = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
        if (FAILED(result))
        {
            LOG_WARN("[DLSSNR_PIPELINE] queue discovery failed type={} HRESULT={:X}", (UINT)type, (UINT)result);
            complete = false;
            continue;
        }
        ID3D12CommandQueue* real = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, queue.Get(), (IUnknown**)&real)) real = queue.Get();
        complete = InstallQueue(real) && complete;
        if (real != queue.Get()) complete = InstallQueue(queue.Get()) && complete;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (SUCCEEDED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator))) &&
            SUCCEEDED(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        {
            ID3D12GraphicsCommandList* nativeList = nullptr;
            if (!Util::CheckForRealObject(__FUNCTION__, list.Get(), (IUnknown**)&nativeList)) nativeList = list.Get();
            auto entries = *reinterpret_cast<void***>(nativeList);
            complete = (BarrierHook::Install(entries[26], Barrier) == NO_ERROR) && complete;
            complete = (CopyHook::Install(entries[17], Copy) == NO_ERROR) && complete;
            complete = (CopyTextureHook::Install(entries[16], CopyTexture) == NO_ERROR) && complete;
            list->Close();
        }
        else complete = false;
    }
    ComPtr<ID3D12Fence> fence;
    if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        complete = InstallFence(fence.Get()) && complete;
    else complete = false;
    auto table = *reinterpret_cast<void***>(device);
    complete = (CreateQueueHook::Install(table[8], CreateQueue) == NO_ERROR) && complete;
    complete = (CreateFenceHook::Install(table[36], CreateFence) == NO_ERROR) && complete;
    return complete;
}

// Called only at Poll/Present boundaries, outside eventMutex and queue callbacks.
// Retained COM owners prevent pointer reuse until the capture is dumped.
void DiscoverAndSample(bool discover)
{
    std::vector<ComPtr<ID3D12CommandQueue>> pendingQueues;
    std::vector<ComPtr<ID3D12Fence>> pendingFences;
    {
        std::lock_guard lock(eventMutex);
        for (auto& [ptr, owner] : queues)
            if (discover && describedQueues.insert(ptr).second) pendingQueues.push_back(owner);
        for (auto& [ptr, owner] : fences) pendingFences.push_back(owner);
    }
    for (auto& queue : pendingQueues)
    {
        InstallQueue(queue.Get());
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))) InstallResidency(device.Get());
        ID3D12CommandQueue* real = nullptr;
        if (Util::CheckForRealObject(__FUNCTION__, queue.Get(), (IUnknown**)&real) && real != queue.Get())
        {
            Mark("queue-native-alias", queue.Get(), real);
            InstallQueue(real);
            device.Reset();
            if (SUCCEEDED(real->GetDevice(IID_PPV_ARGS(&device)))) InstallResidency(device.Get());
        }
    }
    for (auto& fence : pendingFences)
    {
        bool first = false;
        if (discover) { std::lock_guard lock(eventMutex); first = describedFences.insert(fence.Get()).second; }
        if (first)
        {
            InstallFence(fence.Get());
            ID3D12Fence* real = nullptr;
            if (Util::CheckForRealObject(__FUNCTION__, fence.Get(), (IUnknown**)&real) && real != fence.Get())
            {
                Mark("fence-native-alias", fence.Get(), real);
                InstallFence(real);
            }
        }
        Mark("fence-completed-sample", fence.Get(), nullptr, fence->GetCompletedValue());
    }
}

std::string AddressName(const void* address)
{
    HMODULE module = nullptr;
    if (!address || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                        reinterpret_cast<LPCSTR>(address), &module)) return "unknown";
    char path[MAX_PATH] {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const auto offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
    const char* name = strrchr(path, '\\');
    const auto description = std::format("{}+0x{:X} moduleBase=0x{:X} path=\"{}\"",
                                         name ? name + 1 : path, offset, (uintptr_t)module, path);
    FreeLibrary(module);
    return description;
}

void Dump(const char* reason)
{
    std::vector<Event> events;
    decltype(queues) retiredQueues;
    decltype(fences) retiredFences;
    decltype(resources) retiredResources;
    uint64_t lost, id, lostObjects;
    unsigned presents;
    {
        std::lock_guard lock(eventMutex);
        if (capture.events.empty()) return;
        recording.store(false, std::memory_order_release);
        capture.active = false;
        events.swap(capture.events);
        lost = capture.lost; presents = capture.presents; id = session;
        lostObjects = objectsLost;
        retiredQueues.swap(queues); retiredFences.swap(fences);
        retiredResources.swap(resources);
        describedQueues.clear(); describedFences.clear();
        afterNR.clear();
    }
    LOG_INFO("[DLSSNR_PIPELINE] capture={} reason={} events={} lost={} presents={} objectsLost={} schema=4 mode=observe-only CPU-order-not-GPU-timing",
             id, reason, events.size(), lost, presents, lostObjects);
    std::unordered_set<const void*> addresses;
    for (const auto& e : events)
    {
        LOG_INFO("[DLSSNR_PIPELINE] capture={} seq={} cpuUs={} thread={} present={} event={} object=0x{:X} resource=0x{:X} value={} call={} result=0x{:X}",
                 id, e.ordinal, e.cpuUs, e.thread, e.present, e.kind, e.object, e.resource, e.value, e.call, e.result);
        for (unsigned i = 0; i < e.stackSize; ++i)
        {
            LOG_INFO("[DLSSNR_PIPELINE_STACK] capture={} seq={} depth={} address=0x{:X}", id, e.ordinal, i, (uintptr_t)e.stack[i]);
            addresses.insert(e.stack[i]);
        }
        if (strstr(e.kind, "-entry")) addresses.insert(reinterpret_cast<void*>(e.resource));
    }
    for (auto address : addresses)
        LOG_INFO("[DLSSNR_PIPELINE_ADDRESS] capture={} address=0x{:X} location={}", id, (uintptr_t)address, AddressName(address));
    LOG_INFO("[DLSSNR_PIPELINE] capture={} end; toggle PipelineDebug off/on to capture again; missing events are not proof of independence", id);
}
}

bool Active() { return recording.load(std::memory_order_acquire); }

void WatchResource(ID3D12Resource* resource)
{
    if (!Active() || !resource) return;
    std::lock_guard lock(eventMutex);
    if (!capture.active || resources.contains(resource)) return;
    if (resources.size() == ObjectLimit) { ++objectsLost; return; }
    resources.emplace(resource, resource);
}

void ObserveQueue(ID3D12CommandQueue* queue)
{
    if (!Active() || !queue) return;
    std::lock_guard lock(eventMutex);
    if (!capture.active || queues.contains(queue)) return;
    if (queues.size() == ObjectLimit) { ++objectsLost; return; }
    queues.emplace(queue, queue);
}

uint64_t Enter(const char* kind, const void* object, const void* resource, uint64_t value)
{
    if (!Active()) return 0;
    Event event;
    event.stackSize = CaptureStackBackTrace(1, static_cast<DWORD>(event.stack.size()), event.stack.data(), nullptr);
    std::lock_guard lock(eventMutex);
    if (!capture.active) return 0;
    event.kind = kind; event.object = reinterpret_cast<uintptr_t>(object);
    event.resource = reinterpret_cast<uintptr_t>(resource); event.value = value;
    event.call = (session << 32) | (++nextCall & 0xFFFFFFFFull);
    event.cpuUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    event.thread = GetCurrentThreadId();
    capture.Add(event);
    return event.call;
}

void Leave(const char* kind, uint64_t call, const void* object, const void* resource, uint64_t value, HRESULT result)
{
    if (!call || !Active()) return;
    std::lock_guard lock(eventMutex);
    if (!capture.active || (call >> 32) != session) return;
    Event event;
    event.kind = kind; event.object = reinterpret_cast<uintptr_t>(object);
    event.resource = reinterpret_cast<uintptr_t>(resource); event.value = value;
    event.call = call; event.result = static_cast<UINT>(result);
    event.cpuUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    event.thread = GetCurrentThreadId();
    capture.Add(event);
}

void Mark(const char* kind, const void* object, const void* resource, uint64_t value)
{
    if (!Active()) return;
    std::lock_guard lock(eventMutex);
    if (!capture.active) return;
    Event event;
    event.kind = kind; event.object = reinterpret_cast<uintptr_t>(object);
    event.resource = reinterpret_cast<uintptr_t>(resource); event.value = value;
    event.cpuUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    event.thread = GetCurrentThreadId();
    capture.Add(event);
}

void Poll(ID3D12Device* device)
{
    std::lock_guard control(controlMutex);
    const bool enabled = Config::Instance()->DLSSNRPipelineDebug.value_or_default() && !State::Instance().isShuttingDown;
    if (!enabled)
    {
        if (Active()) DiscoverAndSample(false);
        if (armed) Dump("disabled");
        armed = false;
        return;
    }
    if (Active()) DiscoverAndSample(true);
    // Native FG can emit multiple Presents per real frame and signal its tail
    // only after the last Present returns. Keep observing until the next NR
    // polling boundary, instead of truncating that final signal inside Present.
    bool drained = false;
    if (device)
    {
        std::lock_guard lock(eventMutex);
        drained = capture.active && capture.presents >= Capture::PresentLimit;
    }
    if (drained)
    {
        Mark("capture-drain-nr-boundary", device);
        Dump("present-limit-at-next-nr");
    }
    if (Active() && std::chrono::steady_clock::now() - start > std::chrono::seconds(3))
        Dump("time-limit");
    if (armed || !device) return;
    armed = true;
    const bool hooksComplete = Install(device);
    {
        std::lock_guard lock(eventMutex);
        capture.Start(); afterNR.clear(); objectsLost = 0; ++session;
        start = std::chrono::steady_clock::now();
        recording.store(true, std::memory_order_release);
    }
    Mark("capture-start", device, nullptr, hooksComplete);
    Mark("fg-route", State::Instance().currentFG, nullptr,
         (uint64_t(State::Instance().activeFgInput) << 32) | uint64_t(State::Instance().activeFgOutput));
    Mark("fsr-async-config", nullptr, nullptr, Config::Instance()->FGAsync.value_or_default());
    InstallResidency(device); // Publish installed method identity inside this capture.
    InstallKmt();
    LOG_INFO("[DLSSNR_PIPELINE] capture={} started; six Present calls then drain to next NR, max {} events; observer hooks complete={}; serial NR unchanged",
             session, Capture::Capacity, hooksComplete);
}

void ResetList(ID3D12GraphicsCommandList* commands)
{
    if (!Active()) return;
    { std::lock_guard lock(eventMutex); afterNR.erase(commands); }
    Mark("list-reset", commands);
}
void AfterNR(ID3D12GraphicsCommandList* commands)
{
    if (!Active()) return;
    { std::lock_guard lock(eventMutex); if (afterNR.size() < Capture::Capacity) afterNR.insert(commands); }
    Mark("nr-writeback-recorded", commands);
}
void Work(ID3D12GraphicsCommandList* commands)
{
    if (!Active()) return;
    bool first;
    { std::lock_guard lock(eventMutex); first = afterNR.erase(commands) != 0; }
    if (first) Mark("same-list-work-after-nr", commands);
}
void EndPresent(HRESULT result)
{
    std::lock_guard control(controlMutex);
    if (Active()) DiscoverAndSample(false);
    Mark("present-return", nullptr, nullptr, static_cast<UINT>(result));
    bool done;
    {
        std::lock_guard lock(eventMutex);
        done = capture.EndPresent();
    }
    if (done) Mark("present-limit-draining");
}
void Shutdown()
{
    std::lock_guard control(controlMutex);
    Dump("shutdown");
    armed = false;
    for (LONG result : {CreateQueueHook::Remove(), CreateFenceHook::Remove(),
                       ExecuteHook::Remove(), SignalHook::Remove(), WaitHook::Remove(),
                       CpuSignalHook::Remove(), EventHook::Remove(), ResidentHook::Remove(),
                       KmtCpuSignal::Remove(), KmtCpuWait::Remove(), KmtGpuWait::Remove(),
                       KmtGpuSignal::Remove(), KmtGpuSignal2::Remove(), KmtSignal2::Remove(), KmtWait2::Remove(),
                       BarrierHook::Remove(), CopyHook::Remove(), CopyTextureHook::Remove()})
        if (result != NO_ERROR) LOG_WARN("[DLSSNR_PIPELINE] queue hook retained during teardown: {}", result);
}
}
