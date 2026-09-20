#include <pch.h>
#include "DLSSNRPipelineSplit.h"
#include "DLSSNRCommandState.h"
#include "DLSSNRMethodHooks.h"
#include "DLSSNRPipelineTrace.h"
#include "DLSSNRPipelineAccess.h"
#include <NVNGX_Parameter.h>
#include <State.h>
#include <Util.h>
#include <wrl/client.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <tuple>

namespace DLSSNRPipelineSplit
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr size_t Capacity = 128;
std::recursive_mutex mutex;
bool initialized = false, queueReady = false, disabled = false;
std::atomic<bool> enabled {false};
thread_local unsigned internal = 0;
struct Internal
{
    DLSSNRCommandState::Suppress suppress;
    Internal() { ++internal; }
    ~Internal() { --internal; }
};
struct Segment
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> prefix;
    ComPtr<ID3D12Resource> scene;
    bool sealed = false, nrBoundary = false;
    unsigned sealSlot = 0;
    HRESULT closeResult = S_OK;
    UINT64 serial = 0;
};
struct Retired
{
    ComPtr<ID3D12Fence> fence;
    std::vector<std::shared_ptr<Segment>> segments;
    bool signaled = false;
};
std::unordered_map<ID3D12GraphicsCommandList*, std::shared_ptr<Segment>> recordings;
// Only a list that actually reached the game's selected late-NR boundary may
// be armed on a later Reset. Never segment the FG provider's private lists.
std::unordered_set<ID3D12GraphicsCommandList*> candidates;
std::unordered_set<ID3D12GraphicsCommandList*> opaqueLists;
std::unordered_map<void**, int> implementations;
std::vector<std::shared_ptr<Retired>> retired;
thread_local std::vector<Segment*> expandedHere;
UINT64 cuts = 0, submissions = 0, earlyBoundaries = 0, missingBoundaries = 0;

// Object-owned metadata avoids stale pointer keys when signatures are released
// and their addresses reused. No game/root-signature reference is retained.
constexpr GUID IndirectMetadataId {0xaec765c1, 0x5816, 0x46be, {0x91, 0xb3, 0xf4, 0x47, 0xca, 0x2d, 0x03, 0xd8}};
struct IndirectMetadata
{
    UINT version = 1, count = 0, compute = 0, supported = 0;
    D3D12_INDIRECT_ARGUMENT_DESC arguments[128] {};
};
IndirectMetadata DescribeIndirect(const D3D12_COMMAND_SIGNATURE_DESC* desc, ID3D12RootSignature* root)
{
    IndirectMetadata info;
    if (!desc || !desc->pArgumentDescs || !desc->NumArgumentDescs ||
        desc->NumArgumentDescs > std::size(info.arguments)) return info;
    info.count = desc->NumArgumentDescs;
    UINT workloads = 0;
    bool valid = true, roots = false;
    for (UINT i = 0; i < info.count; ++i)
    {
        const auto& arg = info.arguments[i] = desc->pArgumentDescs[i];
        switch (arg.Type)
        {
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:
            info.compute = 1;
            [[fallthrough]];
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
            ++workloads;
            valid &= i + 1 == info.count;
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
            valid &= arg.VertexBuffer.Slot < D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
            roots = true;
            valid &= arg.Constant.RootParameterIndex < 64 && arg.Constant.DestOffsetIn32BitValues < 64 &&
                     arg.Constant.Num32BitValuesToSet > 0 &&
                     arg.Constant.Num32BitValuesToSet <= 64 - arg.Constant.DestOffsetIn32BitValues;
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
            roots = true; valid &= arg.ConstantBufferView.RootParameterIndex < 64; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            roots = true; valid &= arg.ShaderResourceView.RootParameterIndex < 64; break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
            roots = true; valid &= arg.UnorderedAccessView.RootParameterIndex < 64; break;
        default: valid = false; break; // Includes incrementing constants/extensions.
        }
    }
    info.supported = valid && workloads == 1 && (!roots || root);
    return info;
}
bool ReadIndirect(ID3D12CommandSignature* signature, IndirectMetadata& info)
{
    if (!signature) return false;
    ID3D12CommandSignature* native = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, signature, reinterpret_cast<IUnknown**>(&native))) signature = native;
    UINT bytes = sizeof(info);
    return SUCCEEDED(signature->GetPrivateData(IndirectMetadataId, &bytes, &info)) &&
           bytes == sizeof(info) && info.version == 1 && info.supported &&
           info.count > 0 && info.count <= std::size(info.arguments);
}

// ExecuteIndirect resets only bindings named by its signature. When its work
// moves to the prefix, the logical tail needs those same resets explicitly.
// Always update the injection snapshot as well, including unsegmented lists.
void ResetIndirectState(ID3D12GraphicsCommandList* list, const IndirectMetadata& info, bool mirror)
{
    const bool cs = info.compute != 0;
    const UINT zeros[64] {};
    for (UINT i = 0; i < info.count; ++i)
    {
        const auto& arg = info.arguments[i];
        if (mirror)
        {
            Internal guard; // Do not route these resets back into the prefix.
            switch (arg.Type)
            {
            case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
            {
                const D3D12_VERTEX_BUFFER_VIEW view {};
                list->IASetVertexBuffers(arg.VertexBuffer.Slot, 1, &view);
                break;
            }
            case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: list->IASetIndexBuffer(nullptr); break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
                if (cs) list->SetComputeRoot32BitConstants(arg.Constant.RootParameterIndex,
                    arg.Constant.Num32BitValuesToSet, zeros, arg.Constant.DestOffsetIn32BitValues);
                else list->SetGraphicsRoot32BitConstants(arg.Constant.RootParameterIndex,
                    arg.Constant.Num32BitValuesToSet, zeros, arg.Constant.DestOffsetIn32BitValues);
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
                if (cs) list->SetComputeRootConstantBufferView(arg.ConstantBufferView.RootParameterIndex, 0);
                else list->SetGraphicsRootConstantBufferView(arg.ConstantBufferView.RootParameterIndex, 0);
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
                if (cs) list->SetComputeRootShaderResourceView(arg.ShaderResourceView.RootParameterIndex, 0);
                else list->SetGraphicsRootShaderResourceView(arg.ShaderResourceView.RootParameterIndex, 0);
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
                if (cs) list->SetComputeRootUnorderedAccessView(arg.UnorderedAccessView.RootParameterIndex, 0);
                else list->SetGraphicsRootUnorderedAccessView(arg.UnorderedAccessView.RootParameterIndex, 0);
                break;
            default: break;
            }
        }
        // Internal's state-capture suppression has ended here.
        switch (arg.Type)
        {
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
            DLSSNRCommandState::Constants(list, cs, arg.Constant.RootParameterIndex,
                arg.Constant.Num32BitValuesToSet, zeros, arg.Constant.DestOffsetIn32BitValues);
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
            DLSSNRCommandState::Value(list, cs, arg.ConstantBufferView.RootParameterIndex, DLSSNRCommandState::Kind::CBV, 0);
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            DLSSNRCommandState::Value(list, cs, arg.ShaderResourceView.RootParameterIndex, DLSSNRCommandState::Kind::SRV, 0);
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
            DLSSNRCommandState::Value(list, cs, arg.UnorderedAccessView.RootParameterIndex, DLSSNRCommandState::Kind::UAV, 0);
            break;
        default: break;
        }
    }
}

std::shared_ptr<Segment> Find(ID3D12GraphicsCommandList* list)
{
    std::lock_guard lock(mutex);
    auto it = recordings.find(list);
    return it == recordings.end() ? nullptr : it->second;
}
void Collect()
{
    // UINT64_MAX is device removal, never successful GPU completion.
    std::erase_if(retired, [](const auto& entry) {
        if (!entry->signaled) return false;
        const auto value = entry->fence->GetCompletedValue();
        return value != UINT64_MAX && value >= 1;
    });
}
HRESULT Seal(const std::shared_ptr<Segment>& segment)
{
    if (!segment || segment->sealed) return segment ? segment->closeResult : S_OK;
    Internal guard;
    segment->closeResult = segment->prefix->Close();
    segment->sealed = true;
    if (FAILED(segment->closeResult))
        LOG_ERROR("[DLSSNR_SPLIT] prefix Close failed hr=0x{:08X}; logical Close will fail", (UINT)segment->closeResult);
    return segment->closeResult;
}

// The original list already contains every state-setting operation, in order.
// This includes IA/RS/OM, predication, graphics roots and newer dynamic state;
// a compute-only snapshot is insufficient for a new graphics command list.
enum class Policy { Work, State, Seal, TailOnly, Indirect };
template<unsigned Slot, Policy P, auto Member, typename Signature = decltype(Member)> struct Route;
template<unsigned Slot, Policy P, auto Member, typename Object, typename... Args>
struct Route<Slot, P, Member, void(STDMETHODCALLTYPE Object::*)(Args...)>
{
    using Function = void(STDMETHODCALLTYPE*)(Object*, Args...);
    using Hook = DLSSNRMethodHooks::MethodHook<30000 + Slot, Function>;
    static void STDMETHODCALLTYPE Call(Object* list, Args... args)
    {
        if (internal) { Hook::Forward(list, args...); return; }
        if constexpr (P != Policy::Indirect) DLSSNRPipelineAccess::Record<Slot>(list, args...);
        auto segment = Find(list);
        if constexpr (P == Policy::Indirect)
        {
            IndirectMetadata info;
            const auto signature = std::get<0>(std::tuple(args...));
            const bool known = ReadIndirect(signature, info);
            if (DLSSNRPipelineAccess::Enabled() && !DLSSNRPipelineAccess::suppress)
            {
                const auto a = std::tuple(args...);
                DLSSNRPipelineAccess::Resource(list, std::get<2>(a), false);
                DLSSNRPipelineAccess::Resource(list, std::get<4>(a), false);
                if (known && info.count == 1 &&
                    (info.arguments[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW ||
                     info.arguments[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ||
                     info.arguments[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH ||
                     info.arguments[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH))
                    DLSSNRPipelineAccess::Shader(list, info.compute != 0,
                        info.arguments[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,
                        info.arguments[0].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH);
                else DLSSNRPipelineAccess::Unknown(list, "indirect-resource-addresses");
            }
            const bool routing = segment && !segment->sealed;
            if (routing && known)
            {
                {
                    Internal guard;
                    (static_cast<Object*>(segment->prefix.Get())->*Member)(args...);
                }
                ResetIndirectState(list, info, true);
                DLSSNRPipelineTrace::Mark("indirect-prefix-record", list, signature, info.count);
                static std::atomic<UINT64> routed {0};
                const auto count = ++routed;
                if (count <= 3 || count % 3000 == 0)
                    LOG_INFO("[DLSSNR_SPLIT] indirect-routed={} signature=0x{:X} arguments={} compute={} postState=mirrored",
                             count, (uintptr_t)signature, info.count, info.compute != 0);
            }
            else
            {
                if (routing)
                {
                    segment->sealSlot = Slot;
                    Seal(segment);
                    static std::atomic<UINT64> fallbacks {0};
                    const auto count = ++fallbacks;
                    if (count <= 3 || count % 300 == 0)
                        LOG_INFO("[DLSSNR_SPLIT] indirect-fallback={} signature=0x{:X} reason=missing-or-unsupported-signature",
                                 count, (uintptr_t)signature);
                }
                Hook::Forward(list, args...);
                if (known) ResetIndirectState(list, info, false);
                else if (!DLSSNRCommandState::suppress)
                {
                    // Do not let an NR injection restore pre-indirect bindings
                    // whose post-execution values we cannot reconstruct.
                    std::lock_guard stateLock(DLSSNRCommandState::mutex);
                    auto it = DLSSNRCommandState::states.find(list);
                    if (it != DLSSNRCommandState::states.end()) it->second.invalid = true;
                }
            }
            return;
        }
        if (!segment || segment->sealed) { Hook::Forward(list, args...); return; }
        if constexpr (P == Policy::TailOnly)
        {
            // Keep balanced PIX scopes on the logical list. No GPU work or
            // resource dependency is carried by BeginEvent/EndEvent.
            Hook::Forward(list, args...);
            return;
        }
        if constexpr (P == Policy::Seal)
        {
            if constexpr (Slot == 53)
            {
                // Timestamp EndQuery has no matching BeginQuery or scoped
                // state. Keep profiling in the segment that executes its work.
                if (std::get<1>(std::tuple(args...)) == D3D12_QUERY_TYPE_TIMESTAMP)
                {
                    Internal guard;
                    (static_cast<Object*>(segment->prefix.Get())->*Member)(args...);
                    return;
                }
            }
            // Queries/render passes, bundles, protected
            // sessions and opaque meta/work-graph operations stay wholly in the
            // original list. Never fabricate a resumable version of these.
            segment->sealSlot = Slot;
            Seal(segment);
            Hook::Forward(list, args...);
            return;
        }
        if constexpr (Slot == 26)
        {
            // Never move an outstanding split transition across the boundary.
            auto values = std::tuple(args...);
            const auto count = std::get<0>(values);
            const auto* barriers = std::get<1>(values);
            for (UINT i = 0; barriers && i < count; ++i)
                if (barriers[i].Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE)
                {
                    segment->sealSlot = Slot;
                    Seal(segment);
                    Hook::Forward(list, args...);
                    return;
                }
        }
        {
            // Invoke the PRIVATE object's own implementation, never transplant
            // a driver's or wrapper's trampoline onto a different COM object.
            Internal guard;
            (static_cast<Object*>(segment->prefix.Get())->*Member)(args...);
        }
        if constexpr (P == Policy::State) Hook::Forward(list, args...);
    }
    static bool Install(void* address)
    {
        const auto result = Hook::Install(address, Call);
        if (result != NO_ERROR)
            LOG_WARN("[DLSSNR_SPLIT] method install failed slot={} result={}", Slot, result);
        return result == NO_ERROR;
    }
};

using ResetFunction = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using ResetHook = DLSSNRMethodHooks::MethodHook<30010, ResetFunction>;
using CloseFunction = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using CloseHook = DLSSNRMethodHooks::MethodHook<30009, CloseFunction>;
using ReleaseFunction = ULONG(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using ReleaseHook = DLSSNRMethodHooks::MethodHook<30002, ReleaseFunction>;
using QueryFunction = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, REFIID, void**);
using QueryHook = DLSSNRMethodHooks::MethodHook<30000, QueryFunction>;

bool KnownInterface(REFIID iid)
{
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) ||
        iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12CommandList) ||
        iid == __uuidof(ID3D12GraphicsCommandList)) return true;
#define KNOWN_INTERFACE(N) if (iid == __uuidof(ID3D12GraphicsCommandList##N)) return true;
    KNOWN_INTERFACE(1) KNOWN_INTERFACE(2) KNOWN_INTERFACE(3) KNOWN_INTERFACE(4) KNOWN_INTERFACE(5)
    KNOWN_INTERFACE(6) KNOWN_INTERFACE(7) KNOWN_INTERFACE(8) KNOWN_INTERFACE(9) KNOWN_INTERFACE(10)
#undef KNOWN_INTERFACE
    return false;
}
HRESULT STDMETHODCALLTYPE Query(ID3D12GraphicsCommandList* list, REFIID iid, void** output)
{
    const auto result = QueryHook::Forward(list, iid, output);
    if (!internal && SUCCEEDED(result) && !KnownInterface(iid))
    {
        static std::atomic<UINT64> opaqueQueries {0};
        if (++opaqueQueries <= 16)
            LOG_INFO("[DLSSNR_ASYNC_INTERFACE] list=0x{:X} type={} iid={:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X} policy=retain-opaque-recording",
                uintptr_t(list), UINT(list->GetType()), iid.Data1, iid.Data2, iid.Data3,
                iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3], iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
        DLSSNRPipelineAccess::Unknown(list, "opaque-command-interface");
        {
            std::lock_guard lock(mutex);
            if (!implementations.contains(*reinterpret_cast<void***>(list))) return result;
        }
        // Extension interfaces may record work without any of the covered
        // methods. Stop routing before the caller can use the returned object.
        if (auto segment = Find(list)) { segment->sealSlot = 0; Seal(segment); }
        std::lock_guard lock(mutex);
        if (opaqueLists.size() < Capacity) opaqueLists.insert(list);
        else disabled = true;
        candidates.erase(list);
    }
    return result;
}

void Prepare(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline)
{
    std::lock_guard lock(mutex);
    recordings.erase(list); // Submitted generations remain owned by their fence.
    Collect();
    if (!enabled || disabled || !queueReady || State::Instance().isShuttingDown ||
        !Config::Instance()->DLSSNREnabled.value_or_default() ||
        !Config::Instance()->DLSSNRLateHudless.value_or_default() ||
        !candidates.contains(list) || recordings.size() >= Capacity || retired.size() >= Capacity ||
        list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    const auto it = implementations.find(*reinterpret_cast<void***>(list));
    if (it == implementations.end() || it->second < 0) return;
    Internal guard;
    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || device->GetNodeCount() != 1) return;
    auto segment = std::make_shared<Segment>();
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&segment->allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, segment->allocator.Get(), pipeline,
                                        IID_PPV_ARGS(&segment->prefix)))) return;
    // Only single-pointer COM interface layouts are admitted by Observe. Verify
    // the private object exposes every interface that the logical one can use.
    bool compatible = true;
#define CHECK_INTERFACE(N) \
    if (it->second >= N) { ComPtr<ID3D12GraphicsCommandList##N> version; \
        compatible &= SUCCEEDED(segment->prefix.As(&version)) && version.Get() == segment->prefix.Get(); }
    CHECK_INTERFACE(1) CHECK_INTERFACE(2) CHECK_INTERFACE(3) CHECK_INTERFACE(4) CHECK_INTERFACE(5)
    CHECK_INTERFACE(6) CHECK_INTERFACE(7) CHECK_INTERFACE(8) CHECK_INTERFACE(9) CHECK_INTERFACE(10)
#undef CHECK_INTERFACE
    if (!compatible) { segment->prefix->Close(); return; }
    recordings.emplace(list, std::move(segment));
}
HRESULT STDMETHODCALLTYPE Reset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator,
                                 ID3D12PipelineState* pipeline)
{
    const auto result = ResetHook::Forward(list, allocator, pipeline);
    if (!internal && SUCCEEDED(result))
    {
        std::lock_guard lock(mutex);
        const auto known = implementations.find(*reinterpret_cast<void***>(list));
        if (known != implementations.end() && known->second >= 0 && !opaqueLists.contains(list))
            DLSSNRPipelineAccess::Reset(list);
        else DLSSNRPipelineAccess::Forget(list);
        Prepare(list, pipeline);
    }
    return result;
}
HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* list)
{
    const auto prefixResult = internal ? S_OK : Seal(Find(list));
    const auto result = CloseHook::Forward(list);
    return FAILED(prefixResult) ? prefixResult : result;
}
ULONG STDMETHODCALLTYPE Release(ID3D12GraphicsCommandList* list)
{
    const auto result = ReleaseHook::Forward(list);
    if (!internal && !result)
    {
        DLSSNRPipelineAccess::Forget(list);
        std::lock_guard lock(mutex);
        recordings.erase(list); // Never dereference the destroyed logical list.
        candidates.erase(list);
        opaqueLists.erase(list);
    }
    return result;
}

using ExecuteFunction = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ExecuteHook = DLSSNRMethodHooks::MethodHook<31010, ExecuteFunction>;
using FenceFunction = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
using SignalHook = DLSSNRMethodHooks::MethodHook<31014, FenceFunction>;
using WaitHook = DLSSNRMethodHooks::MethodHook<31015, FenceFunction>;
using CpuSignalFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64);
using CpuSignalHook = DLSSNRMethodHooks::MethodHook<31108, CpuSignalFn>;
thread_local unsigned scheduling = 0;
struct Scheduling { Scheduling() { ++scheduling; } ~Scheduling() { --scheduling; } };
struct Dependency
{
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    DLSSNRPipelineAccess::Snapshot footprint;
};
struct FenceWait { ComPtr<ID3D12Fence> fence; UINT64 value = 0; };
struct QueueSchedule
{
    ComPtr<ID3D12CommandQueue> queue, prefixQueue;
    ComPtr<ID3D12Fence> fence, prefixFence;
    UINT64 value = 0, prefixValue = 0;
    std::vector<Dependency> prior;
    std::vector<FenceWait> waits;
    Dependency lastNrTail, precedingNrTail;
    std::vector<FenceWait> previousFrameWaits;
    bool failed = false;
    bool nrOwner = false;
    UINT64 decisions = 0;
    std::map<std::pair<std::string, std::string>, UINT64> outcomes;
    std::map<std::pair<std::string, std::string>, UINT64> pendingOutcomes;
    std::map<std::tuple<std::string, std::string, std::string>, UINT64> blockers;
    bool nrPendingAtDecision = false;
};
struct SignalScope
{
    std::vector<Dependency> prior;
    std::vector<FenceWait> waits;
};
struct FenceSignals
{
    ComPtr<ID3D12Fence> fence;
    ComPtr<IUnknown> producer;
    std::map<UINT64, SignalScope> values;
    UINT64 highest = 0;
    bool observedValue = false;
    bool opaque = false;
    const char* opaqueReason = "none";
};
std::recursive_mutex scheduleMutex;
std::unordered_map<ID3D12CommandQueue*, std::shared_ptr<QueueSchedule>> schedules;
std::unordered_map<ID3D12Fence*, FenceSignals> signals;
std::atomic<bool> schedulerReady {false};
UINT64 asyncSubmissions = 0, serialSubmissions = 0;

void OpaqueFence(ID3D12Fence* fence, const char* reason = "unobserved-cpu-signal")
{
    std::lock_guard lock(scheduleMutex);
    if (!signals.contains(fence) && signals.size() >= 512) return;
    auto& entry = signals[fence]; entry.fence = fence; entry.opaque = true;
    entry.opaqueReason = reason;
}

bool Finished(ID3D12Fence* fence, UINT64 value)
{
    const auto completed = fence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= value;
}
void Reclaim(QueueSchedule& s)
{
    std::erase_if(s.prior, [](const auto& d) { return Finished(d.fence.Get(), d.value); });
    std::erase_if(s.waits, [](const auto& d) { return Finished(d.fence.Get(), d.value); });
}
std::shared_ptr<QueueSchedule> Schedule(ID3D12CommandQueue* queue)
{
    if (const auto it = schedules.find(queue); it != schedules.end()) return it->second;
    if (schedules.size() >= Capacity) return {};
    auto result = std::make_shared<QueueSchedule>(); result->queue = queue;
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result->fence)))) return {};
    // The queue can predate interception. An empty registry does not prove it
    // has no preceding work; fence that unknown history before allowing bypass.
    result->value = 1;
    if (FAILED(queue->Signal(result->fence.Get(), result->value))) return {};
    result->prior.push_back({result->fence, result->value, {}});
    schedules.emplace(queue, result);
    return result;
}
void Fail(QueueSchedule& s, HRESULT result, const char* operation)
{
    s.failed = true;
    LOG_ERROR("[DLSSNR_ASYNC] {} failed hr=0x{:08X}; further migration disabled for queue=0x{:X}",
              operation, (UINT)result, (uintptr_t)s.queue.Get());
}
Dependency SubmitPoint(QueueSchedule& s, const DLSSNRPipelineAccess::Snapshot& footprint)
{
    Dependency point {s.fence, ++s.value, footprint};
    const auto result = s.queue->Signal(s.fence.Get(), point.value);
    if (FAILED(result)) Fail(s, result, "completion Signal");
    s.prior.push_back(point);
    if (s.prior.size() > Capacity)
    {
        // This signal follows all prior work on the source queue, including
        // joins from auxiliary work. Collapse conservatively, never drop it.
        s.prior.assign(1, Dependency {s.fence, point.value, {}});
    }
    return point;
}
void NrTail(QueueSchedule& s, const Dependency& point)
{
    s.precedingNrTail = s.lastNrTail;
    s.lastNrTail = point;
    s.previousFrameWaits = s.waits;
}
void ResolveWait(const FenceWait& wait, const DLSSNRPipelineAccess::Snapshot& prefix,
                 std::vector<FenceWait>& required, unsigned depth = 0)
{
    if (Finished(wait.fence.Get(), wait.value)) return;
    const auto found = signals.find(wait.fence.Get());
    if (depth >= 16 || found == signals.end() || found->second.opaque)
    { required.push_back(wait); return; }
    const auto scope = found->second.values.find(wait.value);
    if (scope == found->second.values.end()) { required.push_back(wait); return; }
    for (const auto& point : scope->second.prior)
        if (!Finished(point.fence.Get(), point.value) && DLSSNRPipelineAccess::Conflicts(prefix, point.footprint))
            required.push_back({point.fence, point.value});
    for (const auto& dependency : scope->second.waits) ResolveWait(dependency, prefix, required, depth + 1);
}
bool PrefixQueue(QueueSchedule& s)
{
    if (s.prefixQueue) return true;
    ComPtr<ID3D12Device> device;
    if (FAILED(s.queue->GetDevice(IID_PPV_ARGS(&device)))) return false;
    auto desc = s.queue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT || desc.NodeMask > 1 || device->GetNodeCount() != 1) return false;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&s.prefixQueue))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.prefixFence))))
    { s.prefixQueue.Reset(); return false; }
    s.prefixQueue->SetName(L"OptiScaler independent rendering prefix");
    LOG_INFO("[DLSSNR_ASYNC] source=0x{:X} prefixQueue=0x{:X} mode=independent-prefix nr-and-native-fg-ownership=preserved",
             (uintptr_t)s.queue.Get(), (uintptr_t)s.prefixQueue.Get());
    return true;
}
HRESULT STDMETHODCALLTYPE Signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value)
{
    if (scheduling || !schedulerReady) return SignalHook::Forward(queue, fence, value);
    std::lock_guard lock(scheduleMutex);
    Scheduling guard;
    const auto result = SignalHook::Forward(queue, fence, value);
    if (SUCCEEDED(result) && fence && (signals.size() < 512 || signals.contains(fence)))
    {
        auto& record = signals[fence]; record.fence = fence;
        ComPtr<IUnknown> producer;
        const auto producerResult = queue->QueryInterface(IID_PPV_ARGS(&producer));
        if (FAILED(producerResult) || (record.producer && record.producer != producer))
        { record.opaque = true; record.opaqueReason = "multiple-or-unidentified-queue-producers"; }
        else record.producer = std::move(producer);
        ComPtr<ID3D12Fence1> fence1;
        if (!CpuSignalHook::Contains((*reinterpret_cast<void***>(fence))[10]) ||
            FAILED(fence->QueryInterface(IID_PPV_ARGS(&fence1))) || fence1->GetCreationFlags() != D3D12_FENCE_FLAG_NONE)
        { record.opaque = true; record.opaqueReason = "cpu-hook-missing-or-shared-fence"; }
        std::erase_if(record.values, [&](const auto& item) { return Finished(fence, item.first); });
        // Signal(0) is legal as the FIRST observation. It must not poison a
        // fence permanently merely because highest was zero-initialized.
        const bool duplicate = record.observedValue && value == record.highest;
        if (record.observedValue && value < record.highest)
        {
            if (!record.opaque)
                LOG_INFO("[DLSSNR_ASYNC_FENCE] fence=0x{:X} reason=nonmonotonic-signal value={} previousHighest={} scopes={}",
                    uintptr_t(fence), value, record.highest, record.values.size());
            if (!record.opaque) record.opaqueReason = "nonmonotonic-signal";
            record.opaque = true;
        }
        record.observedValue = true;
        record.highest = std::max(record.highest, value);
        if (duplicate)
        {
            // An equal-value re-signal on the SAME queue cannot rewind a later
            // value. A Wait for this value could be satisfied by either signal,
            // so never substitute the newer scope (it can introduce a cycle).
            // Keep that original Wait; later, higher values remain trackable.
            record.values.erase(value);
            static UINT64 duplicates = 0;
            if (++duplicates <= 3 || duplicates % 3000 == 0)
                LOG_INFO("[DLSSNR_ASYNC_FENCE] fence=0x{:X} duplicate={} count={} policy=original-wait-for-this-value",
                    uintptr_t(fence), value, duplicates);
            return result;
        }
        // Missing an OLD scope retains that original Wait in ResolveWait.
        // Eviction must not disable every future scope on a monotonic fence.
        if (record.values.size() >= Capacity && !record.values.contains(value))
        {
            const auto evicted = record.values.begin()->first;
            record.values.erase(record.values.begin());
            static UINT64 evictions = 0;
            if (++evictions <= 3 || evictions % 3000 == 0)
                LOG_INFO("[DLSSNR_ASYNC_FENCE] fence=0x{:X} evicted={} count={} policy=retain-wait-for-missing-scope",
                    uintptr_t(fence), evicted, evictions);
        }
        if (auto source = Schedule(queue); source && !source->failed)
        {
            Reclaim(*source);
            record.values[value] = {source->prior, source->waits};
        }
        else { record.opaque = true; record.opaqueReason = "producer-schedule-unavailable"; }
    }
    return result;
}
HRESULT STDMETHODCALLTYPE Wait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value)
{
    if (scheduling || !schedulerReady) return WaitHook::Forward(queue, fence, value);
    std::lock_guard lock(scheduleMutex);
    Scheduling guard;
    const auto result = WaitHook::Forward(queue, fence, value);
    if (SUCCEEDED(result) && fence)
        if (auto source = Schedule(queue))
        {
            Reclaim(*source);
            if (source->waits.size() >= Capacity) source->failed = true;
            else source->waits.push_back({fence, value});
        }
    return result;
}

// CPU and residency producers do not describe shader resources. Their fences
// remain opaque even if a queue also happens to signal the same numeric value.
HRESULT STDMETHODCALLTYPE CpuSignal(ID3D12Fence* fence, UINT64 value)
{
    if (DLSSNRPipelineAccess::Enabled()) OpaqueFence(fence, "cpu-signal");
    return CpuSignalHook::Forward(fence, value);
}
using ResidentFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device3*, D3D12_RESIDENCY_FLAGS, UINT, ID3D12Pageable* const*, ID3D12Fence*, UINT64);
using ResidentHook = DLSSNRMethodHooks::MethodHook<31150, ResidentFn>;
HRESULT STDMETHODCALLTYPE Resident(ID3D12Device3* device, D3D12_RESIDENCY_FLAGS flags, UINT count,
                                    ID3D12Pageable* const* objects, ID3D12Fence* fence, UINT64 value)
{
    if (DLSSNRPipelineAccess::Enabled() && fence) OpaqueFence(fence, "residency-signal");
    return ResidentHook::Forward(device, flags, count, objects, fence, value);
}

void AuditWait(UINT64 session, const DLSSNRPipelineAccess::Snapshot& prefix,
               const FenceWait& wait, const char* role, unsigned depth, unsigned& budget)
{
    if (!budget) return;
    --budget;
    const auto completed = wait.fence->GetCompletedValue();
    const auto found = signals.find(wait.fence.Get());
    const bool observed = found != signals.end();
    const char* producer = !observed ? "unobserved-producer" : found->second.opaque ? found->second.opaqueReason : "queue-scopes";
    LOG_INFO("[DLSSNR_AUDIT_WAIT] session={} role={} depth={} fence=0x{:X} requested={} completed={} producer={}",
        session, role, depth, uintptr_t(wait.fence.Get()), wait.value, completed, producer);
    if (!observed || found->second.opaque) return;
    const auto scope = found->second.values.find(wait.value);
    if (scope == found->second.values.end())
    {
        LOG_INFO("[DLSSNR_AUDIT_WAIT] session={} fence=0x{:X} requested={} scope=missing-or-retired",
            session, uintptr_t(wait.fence.Get()), wait.value);
        return;
    }
    for (const auto& point : scope->second.prior)
    {
        if (!budget) return;
        --budget;
        DLSSNRPipelineAccess::AuditConflicts(session, prefix, point.footprint, point.fence.Get(), point.value);
    }
    if (depth >= 16)
    {
        LOG_INFO("[DLSSNR_AUDIT_WAIT] session={} truncated=wait-depth-limit", session);
        return;
    }
    for (const auto& dependency : scope->second.waits) AuditWait(session, prefix, dependency, "inherited", depth + 1, budget);
}
void AuditSchedule(QueueSchedule& source, UINT count, const std::vector<std::shared_ptr<Segment>>& segments,
                   const std::vector<DLSSNRPipelineAccess::Snapshot>& parts, bool exact)
{
    if (parts.empty() || segments.empty()) return;
    const auto session = DLSSNRPipelineAccess::AuditSession(parts[0]);
    if (!session) return;
    const auto desc = source.queue->GetDesc();
    ComPtr<ID3D12Device> device;
    const UINT deviceNodes = SUCCEEDED(source.queue->GetDevice(IID_PPV_ARGS(&device))) ? device->GetNodeCount() : 0;
    LOG_INFO("[DLSSNR_AUDIT_SUBMIT] session={} source=0x{:X} lists={} segments={} exactCut={} failed={} queueType={} nodeMask={} deviceNodes={} priorPoints={} entryWaits={} previousFrameWaits={}",
        session, uintptr_t(source.queue.Get()), count, segments.size(), exact, source.failed,
        UINT(desc.Type), desc.NodeMask, deviceNodes, source.prior.size(), source.waits.size(), source.previousFrameWaits.size());
    for (const auto& point : source.prior)
        DLSSNRPipelineAccess::AuditConflicts(session, parts[0], point.footprint, point.fence.Get(), point.value);
    unsigned budget = 256;
    for (const auto& wait : source.waits) AuditWait(session, parts[0], wait, "current-entry", 0, budget);
    for (const auto& wait : source.previousFrameWaits) AuditWait(session, parts[0], wait, "previous-frame-entry", 0, budget);
    if (source.precedingNrTail.fence)
        AuditWait(session, parts[0], {source.precedingNrTail.fence, source.precedingNrTail.value}, "prefix-lead-limit", 0, budget);
    if (!budget) LOG_INFO("[DLSSNR_AUDIT_SUBMIT] session={} truncated=wait-node-limit", session);
}

std::vector<FenceWait> Dependencies(QueueSchedule& source, const DLSSNRPipelineAccess::Snapshot& work)
{
    std::vector<FenceWait> required;
    if (source.precedingNrTail.fence && !Finished(source.precedingNrTail.fence.Get(), source.precedingNrTail.value))
        required.push_back({source.precedingNrTail.fence, source.precedingNrTail.value});
    for (const auto& wait : source.previousFrameWaits)
        if (!Finished(wait.fence.Get(), wait.value)) required.push_back(wait);
    for (const auto& point : source.prior)
        if (DLSSNRPipelineAccess::Conflicts(work, point.footprint)) required.push_back({point.fence, point.value});
    for (const auto& wait : source.waits)
    {
        if (DLSSNRPipelineAccess::RetainEntryWaits(work))
        { if (!Finished(wait.fence.Get(), wait.value)) required.push_back(wait); }
        else ResolveWait(wait, work, required);
    }
    return required;
}
bool RetainsWholeTimeline(const QueueSchedule& source, const std::vector<FenceWait>& required)
{
    return std::any_of(required.begin(), required.end(), [&](const auto& d) {
        return d.fence.Get() == source.fence.Get() && d.value >= source.value;
    });
}
void DiagnoseBlock(QueueSchedule& source, const char* kind, const DLSSNRPipelineAccess::Snapshot& work)
{
    if (!source.nrPendingAtDecision) return;
    ++source.blockers[{kind, "nr-tail", DLSSNRPipelineAccess::ConflictKind(work, source.lastNrTail.footprint)}];
    for (auto it = source.prior.rbegin(); it != source.prior.rend(); ++it)
        if (it->fence == source.fence && it->value == source.value)
        {
            ++source.blockers[{kind, "latest-source", DLSSNRPipelineAccess::ConflictKind(work, it->footprint)}];
            break;
        }
}
void Decision(QueueSchedule& source, const char* kind, const char* reason)
{
    ++source.outcomes[{kind, reason}];
    if (source.nrPendingAtDecision) ++source.pendingOutcomes[{kind, reason}];
    if (++source.decisions != 1 && source.decisions % 600 != 0) return;
    for (const auto& [key, count] : source.outcomes)
        LOG_INFO("[DLSSNR_ASYNC_OUTCOME] source=0x{:X} decisions={} kind={} reason={} total={} pendingTotal={} counts=cumulative",
            uintptr_t(source.queue.Get()), source.decisions, key.first, key.second, count, source.pendingOutcomes[key]);
    for (const auto& [key, count] : source.blockers)
        LOG_INFO("[DLSSNR_ASYNC_BLOCK] source=0x{:X} kind={} against={} cause={} total={} counts=cumulative-pending-only",
            uintptr_t(source.queue.Get()), std::get<0>(key), std::get<1>(key), std::get<2>(key), count);
}

// The independent work can be in EARLIER submissions, not just the final list
// containing NR. Move complete batches only on the queue actually observed to
// own NR. Preserve each batch boundary, and join on that original queue before
// any subsequent game Signal/Wait/Present can observe completion. Native FG's
// own queue and NR-bearing batches are never candidates here.
bool AdvanceRenderBatch(QueueSchedule& source, const std::vector<ID3D12CommandList*>& expanded,
                         const DLSSNRPipelineAccess::Snapshot& work, bool& issued)
{
    issued = false;
    if (!source.nrOwner || source.failed || source.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;
    const char* reason = nullptr;
    auto required = std::vector<FenceWait>{};
    if (!source.nrPendingAtDecision) reason = "no-pending-nr";
    else if (!DLSSNRPipelineAccess::Complete(work)) reason = DLSSNRPipelineAccess::Reason(work);
    else if (DLSSNRPipelineAccess::OrdersAllResources(work)) reason = "global-resource-order";
    else if (DLSSNRPipelineAccess::RequiresOriginalQueue(work)) reason = "swapchain-original-queue";
    else if (!DLSSNRPipelineAccess::HasWork(work)) reason = "state-only-batch";
    else
    {
        required = Dependencies(source, work);
        if (RetainsWholeTimeline(source, required)) reason = "preceding-render-dependency";
        else if (!PrefixQueue(source)) reason = "prefix-queue-unavailable";
    }
    if (reason)
    {
        DiagnoseBlock(source, "render-batch", work);
        Decision(source, "render-batch", reason);
        static UINT64 serialBatches = 0;
        if (++serialBatches <= 3 || serialBatches % 600 == 0)
            LOG_INFO("[DLSSNR_ASYNC_BATCH] serial={} lists={} reason={} dependencies={} nrOwner=true",
                serialBatches, expanded.size(), reason, required.size());
        return false;
    }
    for (const auto& dependency : required)
    {
        const auto hr = source.prefixQueue->Wait(dependency.fence.Get(), dependency.value);
        if (FAILED(hr)) { Fail(source, hr, "render-batch dependency Wait"); return false; }
    }
    const bool pendingNr = source.lastNrTail.fence && !Finished(source.lastNrTail.fence.Get(), source.lastNrTail.value);
    source.prefixQueue->ExecuteCommandLists(static_cast<UINT>(expanded.size()), expanded.data());
    issued = true; // No failure below this line can replay the game batch.
    const UINT64 value = ++source.prefixValue;
    const auto signaled = source.prefixQueue->Signal(source.prefixFence.Get(), value);
    if (FAILED(signaled)) Fail(source, signaled, "render-batch Signal");
    const auto joined = source.queue->Wait(source.prefixFence.Get(), value);
    if (FAILED(joined)) { Fail(source, joined, "render-batch completion join"); return false; }
    if (FAILED(signaled)) return false; // Keep the source behind the unsatisfied join; never replay.
    // Use the REAL producer completion for subsequent resource conflicts. A
    // source-queue marker after the join would needlessly couple this batch
    // to every earlier unrelated source job (including the preceding NR).
    source.prior.push_back({source.prefixFence, value, work});
    if (source.prior.size() > Capacity)
    {
        // The source join covers every migrated generation. Collapse onto a
        // conservative completion point if a stalled GPU exhausts metadata.
        SubmitPoint(source, {});
        if (source.failed) return false;
    }
    ++asyncSubmissions;
    Decision(source, "render-batch", "advance");
    if (asyncSubmissions <= 3 || asyncSubmissions % 300 == 0)
        LOG_INFO("[DLSSNR_ASYNC] advance={} kind=render-batch source=0x{:X} prefix=0x{:X} lists={} dependencies={} previousNrPending={} join=original-queue cpu-submit-not-gpu-overlap-proof",
            asyncSubmissions, uintptr_t(source.queue.Get()), uintptr_t(source.prefixQueue.Get()), expanded.size(), required.size(), pendingNr);
    return true;
}

bool Submit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* original,
            const std::vector<ID3D12CommandList*>& expanded, const std::vector<std::shared_ptr<Segment>>& segments,
            const std::vector<DLSSNRPipelineAccess::Snapshot>& parts)
{
    if (!schedulerReady || !DLSSNRPipelineAccess::Enabled() || scheduling || parts.size() != size_t(count) * 2)
    { ExecuteHook::Forward(queue, static_cast<UINT>(expanded.size()), expanded.data()); return true; }
    std::lock_guard lock(scheduleMutex);
    Scheduling guard;
    auto source = Schedule(queue);
    if (!source) { ExecuteHook::Forward(queue, static_cast<UINT>(expanded.size()), expanded.data()); return true; }
    Reclaim(*source);
    // Preserve multi-list batches exactly until each inter-list boundary is
    // independently represented. The common one-list NR batch has an exact cut.
    const bool exact = count == 1 && segments.size() == 1 && segments.front()->nrBoundary;
    const bool containsNr = std::any_of(segments.begin(), segments.end(), [](const auto& s) { return s->nrBoundary; }) ||
        std::any_of(parts.begin(), parts.end(), DLSSNRPipelineAccess::ContainsNR);
    source->nrOwner |= containsNr;
    source->nrPendingAtDecision = source->lastNrTail.fence &&
        !Finished(source->lastNrTail.fence.Get(), source->lastNrTail.value);
    AuditSchedule(*source, count, segments, parts, exact);
    if (!exact || source->failed)
    {
        const auto work = DLSSNRPipelineAccess::Combine(parts);
        if (!containsNr && source->nrOwner && !source->failed &&
            std::all_of(original, original + count, [](auto* list) { return list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT; }))
        {
            bool issued = false;
            if (AdvanceRenderBatch(*source, expanded, work, issued)) return true;
            if (issued) return false;
        }
        ExecuteHook::Forward(queue, static_cast<UINT>(expanded.size()), expanded.data());
        const auto point = SubmitPoint(*source, work);
        if (containsNr) NrTail(*source, point);
        return !source->failed;
    }
    const auto prefix = parts[0], tail = parts[1];
    const char* prefixRestriction = !source->nrPendingAtDecision ? "no-pending-nr" :
        !DLSSNRPipelineAccess::Complete(prefix) ? DLSSNRPipelineAccess::Reason(prefix) :
        DLSSNRPipelineAccess::OrdersAllResources(prefix) ? "global-resource-order" :
        DLSSNRPipelineAccess::RequiresOriginalQueue(prefix) ? "swapchain-original-queue" :
        !DLSSNRPipelineAccess::HasWork(prefix) ? "state-only-prefix" : nullptr;
    if (prefixRestriction)
    {
        DiagnoseBlock(*source, "nr-prefix", prefix);
        Decision(*source, "nr-prefix", prefixRestriction);
        ExecuteHook::Forward(queue, static_cast<UINT>(expanded.size()), expanded.data());
        NrTail(*source, SubmitPoint(*source, DLSSNRPipelineAccess::Combine(parts)));
        ++serialSubmissions;
        if (serialSubmissions <= 3 || serialSubmissions % 300 == 0)
            LOG_INFO("[DLSSNR_ASYNC] serial={} frame={} reason={} tailReason={} mode=original-batch",
                     serialSubmissions, segments.front()->serial,
                     prefixRestriction,
                     DLSSNRPipelineAccess::Complete(tail) ? "complete" : DLSSNRPipelineAccess::Reason(tail));
        return !source->failed;
    }
    std::vector<FenceWait> required;
    const char* fallback = "preceding-tail-dependency";
    bool migrate = DLSSNRPipelineAccess::Complete(prefix);
    if (migrate)
    {
        // Bound GPU lead to one NR-bearing frame: prefix N+1 may overlap
        // tail N, but prefix N+2 cannot run until tail N has completed.
        required = Dependencies(*source, prefix);
        // If the entire preceding source timeline is required there is no
        // overlap to expose. Keep this frame on the original queue.
        if (RetainsWholeTimeline(*source, required)) migrate = false;
        if (migrate)
        {
            migrate = PrefixQueue(*source);
            if (!migrate) fallback = "prefix-queue-unavailable";
        }
    }
    ID3D12CommandList* head[] {segments.front()->prefix.Get()};
    ID3D12CommandList* body[] {original[0]};
    if (migrate)
    {
        for (const auto& dependency : required)
        {
            const auto result = source->prefixQueue->Wait(dependency.fence.Get(), dependency.value);
            if (FAILED(result)) { Fail(*source, result, "prefix dependency Wait"); fallback = "prefix-wait-failed"; migrate = false; break; }
        }
    }
    if (migrate)
    {
        const bool pendingNr = source->lastNrTail.fence && !Finished(source->lastNrTail.fence.Get(), source->lastNrTail.value);
        source->prefixQueue->ExecuteCommandLists(1, head);
        const UINT64 value = ++source->prefixValue;
        const auto signaled = source->prefixQueue->Signal(source->prefixFence.Get(), value);
        if (FAILED(signaled))
        {
            // Work has been issued and must never be replayed. Keep the
            // dependency unsatisfied rather than execute a tail on stale input.
            Fail(*source, signaled, "prefix Signal");
        }
        const auto joined = queue->Wait(source->prefixFence.Get(), value);
        if (FAILED(joined)) { Fail(*source, joined, "NR boundary Wait"); return false; }
        Decision(*source, "nr-prefix", "advance");
        source->prior.push_back({source->prefixFence, value, prefix});
        ++asyncSubmissions;
        if (asyncSubmissions <= 3 || asyncSubmissions % 300 == 0)
            LOG_INFO("[DLSSNR_ASYNC] advance={} kind=nr-prefix frame={} source=0x{:X} prefix=0x{:X} dependencies={} previousNrPending={} join=before-current-nr cpu-submit-not-gpu-overlap-proof",
                     asyncSubmissions, segments.front()->serial, (uintptr_t)queue, (uintptr_t)source->prefixQueue.Get(), required.size(), pendingNr);
    }
    else
    {
        DiagnoseBlock(*source, "nr-prefix", prefix);
        Decision(*source, "nr-prefix", fallback);
        ExecuteHook::Forward(queue, 1, head);
        SubmitPoint(*source, prefix);
        ++serialSubmissions;
        if (serialSubmissions <= 3 || serialSubmissions % 300 == 0)
            LOG_INFO("[DLSSNR_ASYNC] serial={} frame={} reason={} tailReason={} dependencies={}", serialSubmissions,
                     segments.front()->serial, fallback,
                     DLSSNRPipelineAccess::Complete(tail) ? "complete" : DLSSNRPipelineAccess::Reason(tail), required.size());
    }
    ExecuteHook::Forward(queue, 1, body);
    NrTail(*source, SubmitPoint(*source, tail));
    return !source->failed;
}
void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (scheduling) { ExecuteHook::Forward(queue, count, lists); return; }
    if (!lists || !count) { ExecuteHook::Forward(queue, count, lists); return; }
    // Page-table updates and footprint capture/submission form one ordered
    // transaction. A concurrent mapping cannot slip between capture and issue.
    std::lock_guard submissionLock(scheduleMutex);
    std::vector<ID3D12CommandList*> expanded;
    std::vector<DLSSNRPipelineAccess::Snapshot> accesses;
    auto lifetime = std::make_shared<Retired>();
    {
        std::lock_guard lock(mutex);
        for (UINT i = 0; i < count; ++i)
        {
            if (DLSSNRPipelineAccess::Enabled())
            {
                auto* list = static_cast<ID3D12GraphicsCommandList*>(lists[i]);
                accesses.push_back(DLSSNRPipelineAccess::Capture(list, true));
                accesses.push_back(DLSSNRPipelineAccess::Capture(list, false));
            }
            const auto it = recordings.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (it != recordings.end() &&
                std::find(expandedHere.begin(), expandedHere.end(), it->second.get()) == expandedHere.end())
            {
                const auto& segment = it->second;
                expanded.push_back(segment->prefix.Get());
                lifetime->segments.push_back(segment);
            }
            expanded.push_back(lists[i]);
        }
    }
    if (lifetime->segments.empty()) { Submit(queue, count, lists, expanded, lifetime->segments, accesses); return; }
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))))
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&lifetime->fence));
    {
        std::lock_guard lock(mutex);
        // Pin this generation before submission; Reset cannot free its prefix.
        retired.push_back(lifetime);
        if (!lifetime->fence) disabled = true;
    }
    const auto oldSize = expandedHere.size();
    for (const auto& segment : lifetime->segments) expandedHere.push_back(segment.get());
    struct Restore { size_t size; ~Restore() { expandedHere.resize(size); } } restore {oldSize};
    // With async off this remains one original batch. The async path admits
    // only exact legacy-barrier DIRECT cuts; unsupported scopes remain serial.
    const bool submitted = Submit(queue, count, lists, expanded, lifetime->segments, accesses);
    HRESULT signal;
    {
        Scheduling guard; // Private retirement fences are not application producers.
        signal = !submitted ? E_FAIL : lifetime->fence ? queue->Signal(lifetime->fence.Get(), 1) : E_OUTOFMEMORY;
    }
    {
        std::lock_guard lock(mutex);
        lifetime->signaled = SUCCEEDED(signal);
        if (FAILED(signal))
        {
            disabled = true; // Keep all affected objects; never guess completion.
            LOG_ERROR("[DLSSNR_SPLIT] retirement Signal failed hr=0x{:08X}; new recordings disabled", (UINT)signal);
        }
        ++submissions;
        if (submissions <= 3 || submissions % 300 == 0)
        {
            const auto nrSegments = std::count_if(lifetime->segments.begin(), lifetime->segments.end(),
                [](const auto& segment) { return segment->nrBoundary; });
            LOG_INFO("[DLSSNR_SPLIT] submit={} queue=0x{:X} inputLists={} expandedLists={} segments={} nrSegments={} mode={}",
                     submissions, (uintptr_t)queue, count, expanded.size(), lifetime->segments.size(), nrSegments,
                     DLSSNRPipelineAccess::Enabled() ? "dependency-scheduled" : "serial-same-batch");
        }
        Collect();
    }
}

using UpdateTilesFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Resource*, UINT,
    const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT,
    const D3D12_TILE_RANGE_FLAGS*, const UINT*, const UINT*, D3D12_TILE_MAPPING_FLAGS);
using UpdateTilesHook = DLSSNRMethodHooks::MethodHook<31008, UpdateTilesFn>;
using CopyTilesFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Resource*,
    const D3D12_TILED_RESOURCE_COORDINATE*, ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
    const D3D12_TILE_REGION_SIZE*, D3D12_TILE_MAPPING_FLAGS);
using CopyTilesHook = DLSSNRMethodHooks::MethodHook<31009, CopyTilesFn>;
void STDMETHODCALLTYPE UpdateTiles(ID3D12CommandQueue* queue, ID3D12Resource* resource, UINT regions,
    const D3D12_TILED_RESOURCE_COORDINATE* starts, const D3D12_TILE_REGION_SIZE* sizes,
    ID3D12Heap* heap, UINT ranges, const D3D12_TILE_RANGE_FLAGS* flags, const UINT* offsets,
    const UINT* counts, D3D12_TILE_MAPPING_FLAGS mappingFlags)
{
    if (scheduling || !DLSSNRPipelineAccess::Enabled())
    { UpdateTilesHook::Forward(queue, resource, regions, starts, sizes, heap, ranges, flags, offsets, counts, mappingFlags); return; }
    std::lock_guard lock(scheduleMutex);
    Scheduling guard;
    auto schedule = Schedule(queue);
    UpdateTilesHook::Forward(queue, resource, regions, starts, sizes, heap, ranges, flags, offsets, counts, mappingFlags);
    const auto footprint = DLSSNRPipelineAccess::UpdateTiles(resource, regions, sizes, heap, ranges, flags, offsets, counts);
    if (schedule) { Reclaim(*schedule); SubmitPoint(*schedule, footprint); }
    else schedulerReady = false;
}
void STDMETHODCALLTYPE CopyTiles(ID3D12CommandQueue* queue, ID3D12Resource* destination,
    const D3D12_TILED_RESOURCE_COORDINATE* dstStart, ID3D12Resource* source,
    const D3D12_TILED_RESOURCE_COORDINATE* srcStart, const D3D12_TILE_REGION_SIZE* size,
    D3D12_TILE_MAPPING_FLAGS flags)
{
    if (scheduling || !DLSSNRPipelineAccess::Enabled())
    { CopyTilesHook::Forward(queue, destination, dstStart, source, srcStart, size, flags); return; }
    std::lock_guard lock(scheduleMutex);
    Scheduling guard;
    auto schedule = Schedule(queue);
    CopyTilesHook::Forward(queue, destination, dstStart, source, srcStart, size, flags);
    const auto footprint = DLSSNRPipelineAccess::CopyTiles(destination, source);
    if (schedule) { Reclaim(*schedule); SubmitPoint(*schedule, footprint); }
    else schedulerReady = false;
}
bool InstallQueue(ID3D12CommandQueue* queue)
{
    ID3D12CommandQueue* native = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, queue, reinterpret_cast<IUnknown**>(&native))) native = queue;
    auto** table = *reinterpret_cast<void***>(native);
    bool ready = ExecuteHook::Install(table[10], Execute) == NO_ERROR;
    if (DLSSNRPipelineAccess::Enabled())
    {
        ready &= SignalHook::Install(table[14], Signal) == NO_ERROR;
        ready &= WaitHook::Install(table[15], Wait) == NO_ERROR;
        ready &= UpdateTilesHook::Install(table[8], UpdateTiles) == NO_ERROR;
        ready &= CopyTilesHook::Install(table[9], CopyTiles) == NO_ERROR;
        if (!ready) DLSSNRPipelineAccess::TileTrackingReady(false);
    }
    return ready;
}
using CreateQueueFunction = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using CreateQueueHook = DLSSNRMethodHooks::MethodHook<32008, CreateQueueFunction>;
using CreateQueue1Function = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device9*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, REFIID, void**);
using CreateQueue1Hook = DLSSNRMethodHooks::MethodHook<32080, CreateQueue1Function>;
using CreateSignatureFunction = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*,
                                                            ID3D12RootSignature*, REFIID, void**);
using CreateSignatureHook = DLSSNRMethodHooks::MethodHook<32041, CreateSignatureFunction>;
using CreateFenceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**);
using CreateFenceHook = DLSSNRMethodHooks::MethodHook<32036, CreateFenceFn>;
HRESULT STDMETHODCALLTYPE CreateFence(ID3D12Device* device, UINT64 value, D3D12_FENCE_FLAGS flags, REFIID iid, void** output)
{
    const auto result = CreateFenceHook::Forward(device, value, flags, iid, output);
    if (SUCCEEDED(result) && output && *output)
    {
        ComPtr<ID3D12Fence> fence;
        if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&fence))))
        {
            const bool hooked = CpuSignalHook::Install((*reinterpret_cast<void***>(fence.Get()))[10], CpuSignal) == NO_ERROR;
            if (!hooked || flags != D3D12_FENCE_FLAG_NONE)
            {
                OpaqueFence(fence.Get());
            }
        }
    }
    return result;
}
using CreateListFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using CreateListHook = DLSSNRMethodHooks::MethodHook<32012, CreateListFn>;
using CreateList1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
using CreateList1Hook = DLSSNRMethodHooks::MethodHook<32051, CreateList1Fn>;
void ListCreated(HRESULT result, void** output, bool open)
{
    if (internal || FAILED(result) || !output || !*output) return;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&list))) && Observe(list.Get()) && open)
        DLSSNRPipelineAccess::Reset(list.Get());
}
HRESULT STDMETHODCALLTYPE CreateList(ID3D12Device* device, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                      ID3D12CommandAllocator* allocator, ID3D12PipelineState* state, REFIID iid, void** output)
{
    const auto result = CreateListHook::Forward(device, node, type, allocator, state, iid, output);
    ListCreated(result, output, true); return result;
}
HRESULT STDMETHODCALLTYPE CreateList1(ID3D12Device4* device, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                       D3D12_COMMAND_LIST_FLAGS flags, REFIID iid, void** output)
{
    const auto result = CreateList1Hook::Forward(device, node, type, flags, iid, output);
    ListCreated(result, output, false); return result;
}
HRESULT STDMETHODCALLTYPE CreateSignature(ID3D12Device* device, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                           ID3D12RootSignature* root, REFIID iid, void** output)
{
    const auto result = CreateSignatureHook::Forward(device, desc, root, iid, output);
    if (!enabled || internal || FAILED(result) || !output || !*output) return result;
    Internal guard;
    ComPtr<ID3D12CommandSignature> signature;
    if (FAILED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&signature)))) return result;
    ID3D12CommandSignature* native = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, signature.Get(), reinterpret_cast<IUnknown**>(&native)))
        native = signature.Get();
    const auto info = DescribeIndirect(desc, root);
    const auto stored = native->SetPrivateData(IndirectMetadataId, sizeof(info), &info);
    static std::atomic<UINT64> signatures {0};
    const auto count = ++signatures;
    if (count <= 8 || count % 300 == 0 || FAILED(stored))
        LOG_INFO("[DLSSNR_SPLIT] indirect-signature={} object=0x{:X} arguments={} compute={} supported={} metadataHr=0x{:08X}",
                 count, (uintptr_t)native, info.count, info.compute != 0, info.supported != 0, (UINT)stored);
    return result; // Never change the game's creation result or descriptor.
}
void QueueCreated(HRESULT result, void** output)
{
    if (internal || FAILED(result) || !output || !*output) return;
    Internal guard;
    ComPtr<ID3D12CommandQueue> queue;
    if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&queue))))
    {
        const bool installed = InstallQueue(queue.Get());
        if (!installed)
        {
            schedulerReady = false;
            std::lock_guard lock(mutex);
            disabled = true;
            LOG_ERROR("[DLSSNR_SPLIT] new queue hook unavailable; disabling future recordings");
        }
    }
}
HRESULT STDMETHODCALLTYPE CreateQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid, void** output)
{
    const auto result = CreateQueueHook::Forward(device, desc, iid, output);
    QueueCreated(result, output);
    return result;
}
HRESULT STDMETHODCALLTYPE CreateQueue1(ID3D12Device9* device, const D3D12_COMMAND_QUEUE_DESC* desc,
                                       REFIID creator, REFIID iid, void** output)
{
    const auto result = CreateQueue1Hook::Forward(device, desc, creator, iid, output);
    QueueCreated(result, output);
    return result;
}
} // namespace

NativeRecording::NativeRecording(ID3D12GraphicsCommandList* commands, const NVSDK_NGX_Parameter* parameters) : target(commands)
{
    if (commands && DLSSNRPipelineAccess::Enabled() && !DLSSNRPipelineAccess::suppress)
    {
        ID3D12GraphicsCommandList* native = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, commands, reinterpret_cast<IUnknown**>(&native))) native = commands;
        std::vector<ID3D12Resource*> resources;
        if (!NVNGX_Parameters::D3D12Resources(parameters, resources))
            DLSSNRPipelineAccess::Unknown(native, "opaque-native-parameters");
        for (auto* resource : resources) DLSSNRPipelineAccess::Resource(native, resource, true);
        static const char nativeDomain = 0;
        DLSSNRPipelineAccess::Domain(native, &nativeDomain, "native-sdk-private-domain");
        ++DLSSNRPipelineAccess::suppress;
        accessSuppressed = true;
    }
    if (!enabled || internal || !commands) return;
    ID3D12GraphicsCommandList* native = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, commands, reinterpret_cast<IUnknown**>(&native))) native = commands;
    const auto segment = Find(native);
    if (!segment || segment->sealed) return;
    target = segment->prefix.Get();
    redirected = true;
    ++internal;
    ++DLSSNRCommandState::suppress;
    static std::atomic<UINT64> calls {0};
    const auto count = ++calls;
    if (count <= 3 || count % 300 == 0)
        LOG_INFO("[DLSSNR_SPLIT] native-prefix={} logical=0x{:X} physical=0x{:X} scope=whole-sdk-call",
                 count, (uintptr_t)native, (uintptr_t)target);
    DLSSNRPipelineTrace::Mark("native-prefix-record", native, target, count);
}

NativeRecording::~NativeRecording()
{
    if (accessSuppressed) --DLSSNRPipelineAccess::suppress;
    if (!redirected) return;
    --DLSSNRCommandState::suppress;
    --internal;
}

bool Observe(ID3D12GraphicsCommandList* commands)
{
    if (!enabled || !commands || internal) return false;
    auto** table = *reinterpret_cast<void***>(commands);
    {
        std::lock_guard lock(mutex);
        const auto found = implementations.find(table);
        if (found != implementations.end()) return found->second >= 0;
    }
    Internal guard;
    int version = 0;
    // Interface compatibility belongs to the implementation, not the first
    // object's queue type. COMPUTE/COPY footprints are needed by producer
    // fences too; Prepare and submission still restrict migration to DIRECT.
    bool ready = true;
    ID3D12GraphicsCommandList* native = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, commands, reinterpret_cast<IUnknown**>(&native)) && native != commands)
    {
        // Route only native objects: mirroring a wrapper and its native list
        // would duplicate workloads and native queue expansion cannot accept
        // arbitrary wrapper pointers.
        std::lock_guard lock(mutex);
        implementations[table] = -1;
        return false;
    }
#define DISCOVER_INTERFACE(N) \
    { ComPtr<ID3D12GraphicsCommandList##N> value; \
      if (SUCCEEDED(commands->QueryInterface(IID_PPV_ARGS(&value)))) \
      { version = N; ready &= value.Get() == commands; } }
    DISCOVER_INTERFACE(1) DISCOVER_INTERFACE(2) DISCOVER_INTERFACE(3) DISCOVER_INTERFACE(4) DISCOVER_INTERFACE(5)
    DISCOVER_INTERFACE(6) DISCOVER_INTERFACE(7) DISCOVER_INTERFACE(8) DISCOVER_INTERFACE(9) DISCOVER_INTERFACE(10)
#undef DISCOVER_INTERFACE
    // Hook coverage and permission to segment are different decisions. Driver
    // DIRECT and COMPUTE lists can share method entry points. Capture observes
    // both kinds, so skipping routing installation for a COMPUTE list would let
    // its capture hooks install first. A later DIRECT list would then put
    // routing OUTSIDE capture: Work bypasses the logical-list observer and the
    // private prefix reaches it only under Internal's suppression.
    // Install base methods even for non-admitted lists; Find() keeps those lists
    // as pass-through. Only use extended slots on a compatible base layout.
    bool hooksReady = true;
    {
#define SPLIT_METHOD(SLOT, VERSION, INTERFACE, METHOD, POLICY) \
        if (VERSION == 0 || (ready && version >= VERSION)) \
            hooksReady &= Route<SLOT, Policy::POLICY, &INTERFACE::METHOD>::Install(table[SLOT]);
#include "DLSSNRSegmentMethods.inl"
#undef SPLIT_METHOD
        hooksReady &= CloseHook::Install(table[9], Close) == NO_ERROR;
        hooksReady &= ReleaseHook::Install(table[2], Release) == NO_ERROR;
        hooksReady &= QueryHook::Install(table[0], Query) == NO_ERROR;
        // Install Reset last: no recording can enter the route halfway through
        // method installation. Only subsequent successful resets can be armed.
        hooksReady &= ResetHook::Install(table[10], Reset) == NO_ERROR;
    }
    ready &= hooksReady;
    {
        std::lock_guard lock(mutex);
        implementations[table] = ready ? version : -1;
        // A failed lower-layer hook must not be retried after upper capture
        // hooks attach to the shared entry. Keep existing segments alive, but
        // prevent future routing with an unproven observer order.
        if (!hooksReady) { disabled = true; schedulerReady = false; }
    }
    LOG_INFO("[DLSSNR_SPLIT] command implementation=0x{:X} version={} ready={} routingHooks={} type={} observerOrder=logical-before-routing",
             (uintptr_t)table, version, ready, hooksReady, (UINT)commands->GetType());
    return ready;
}

void Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* probe)
{
    if (internal || !device || !probe) return;
    {
        std::lock_guard lock(mutex);
        if (!initialized)
        {
            enabled = Config::Instance()->DLSSNRPipelineSplit.value_or_default();
            initialized = true;
            LOG_INFO("[DLSSNR_SPLIT] startup enabled={} lateHudless={} trace={} restartRequired=true asyncRequested={}",
                     enabled.load(), Config::Instance()->DLSSNRLateHudless.value_or_default(),
                     Config::Instance()->DLSSNRPipelineDebug.value_or_default(),
                     Config::Instance()->DLSSNRPipelineAsync.value_or_default());
        }
    }
    if (!enabled) return;
    ID3D12Device* nativeDevice = nullptr;
    if (Util::CheckForRealObject(__FUNCTION__, device, reinterpret_cast<IUnknown**>(&nativeDevice))) device = nativeDevice;
    DLSSNRPipelineAccess::Initialize(device);
    {
        Internal guard;
        auto** deviceTable = *reinterpret_cast<void***>(device);
        const bool signaturesReady = CreateSignatureHook::Install(deviceTable[41], CreateSignature) == NO_ERROR;
        LOG_INFO("[DLSSNR_SPLIT] indirect-signature hook ready={} policy=signature-aware-post-state", signaturesReady);
        bool creatorsReady = CreateQueueHook::Install(deviceTable[8], CreateQueue) == NO_ERROR;
        ComPtr<ID3D12Device9> device9;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device9))))
            creatorsReady &= CreateQueue1Hook::Install((*reinterpret_cast<void***>(device9.Get()))[75], CreateQueue1) == NO_ERROR;
        if (DLSSNRPipelineAccess::Enabled())
        {
            creatorsReady &= CreateFenceHook::Install(deviceTable[36], CreateFence) == NO_ERROR;
            creatorsReady &= CreateListHook::Install(deviceTable[12], CreateList) == NO_ERROR;
            ComPtr<ID3D12Device3> device3;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device3))))
                creatorsReady &= ResidentHook::Install((*reinterpret_cast<void***>(device3.Get()))[50], Resident) == NO_ERROR;
            ComPtr<ID3D12Device4> device4;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4))))
                creatorsReady &= CreateList1Hook::Install((*reinterpret_cast<void***>(device4.Get()))[51], CreateList1) == NO_ERROR;
        }
        D3D12_COMMAND_QUEUE_DESC desc {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))))
        {
            const bool installed = InstallQueue(queue.Get());
            std::lock_guard lock(mutex);
            queueReady = installed && creatorsReady;
            DLSSNRPipelineAccess::TileTrackingReady(queueReady);
            LOG_INFO("[DLSSNR_SPLIT] queue hooks ready={} execute={} creators={}", queueReady, installed, creatorsReady);
        }
    }
    ID3D12GraphicsCommandList* nativeList = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, probe, reinterpret_cast<IUnknown**>(&nativeList))) nativeList = probe;
    Observe(nativeList);
    {
        std::lock_guard lock(mutex);
        schedulerReady = DLSSNRPipelineAccess::Enabled() && queueReady && !disabled;
        LOG_INFO("[DLSSNR_ASYNC] startup enabled={} split={} access={} queueHooks={} restartRequired=true",
                 schedulerReady.load(), enabled.load(), DLSSNRPipelineAccess::Enabled(), queueReady);
    }
}

bool BeforeNR(ID3D12GraphicsCommandList* commands, ID3D12Resource* scene, UINT64 serial)
{
    DLSSNRPipelineAccess::AuditBoundary();
    // Even an unarmed/early-sealed list contains NR. Whole-batch migration
    // must never relocate it merely because it has no exact-cut Segment.
    DLSSNRPipelineAccess::MarkNR(commands);
    {
        std::lock_guard lock(mutex);
        if (!enabled) return false;
        const auto it = implementations.find(*reinterpret_cast<void***>(commands));
        if (it != implementations.end() && it->second >= 0 && !opaqueLists.contains(commands) && candidates.size() < Capacity)
            candidates.insert(commands);
    }
    auto segment = Find(commands);
    if (!segment || segment->sealed)
    {
        std::lock_guard lock(mutex);
        if (enabled)
        {
            auto& count = segment ? earlyBoundaries : missingBoundaries;
            ++count;
            if (count <= 3 || count % 300 == 0)
                LOG_INFO("[DLSSNR_SPLIT] nr-boundary-unavailable frame={} cmd=0x{:X} reason={} earlierMethodSlot={} count={} queueReady={} disabled={} candidate={} opaque={} recordings={} retired={}",
                         serial, (uintptr_t)commands, segment ? "sealed-before-nr" : "recording-not-armed",
                         segment ? segment->sealSlot : 0, count, queueReady, disabled, candidates.contains(commands),
                         opaqueLists.contains(commands), recordings.size(), retired.size());
        }
        return false;
    }
    if (FAILED(Seal(segment))) return false;
    DLSSNRPipelineAccess::Cut(commands);
    segment->nrBoundary = true;
    segment->scene = scene;
    segment->serial = serial;
    {
        std::lock_guard lock(mutex);
        ++cuts;
        if (cuts <= 3 || cuts % 300 == 0)
            LOG_INFO("[DLSSNR_SPLIT] nr-boundary={} frame={} prefix=0x{:X} tail=0x{:X} scene=0x{:X}",
                     cuts, serial, (uintptr_t)segment->prefix.Get(), (uintptr_t)commands, (uintptr_t)scene);
    }
    DLSSNRPipelineTrace::Mark("nr-prefix-sealed", commands, segment->prefix.Get(), serial);
    return true;
}
} // namespace DLSSNRPipelineSplit
