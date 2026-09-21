#pragma once
#include <d3d12.h>
#include <array>
#include <atomic>
#include <bit>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>

// Opt-in state capture fed by the existing D3D12 hooks. Compute injection also
// changes descriptor heaps, which invalidates graphics descriptor tables, so
// both bind points must be restored, including partially updated root constants.
namespace DLSSNRCommandState
{
// A toggle during recording leaves a gap in the observed bindings. Reject
// that recording until the next successful creation/Reset, without taking the
// state mutex on every binding while the model is disabled.
inline std::atomic<uint64_t> captureEpoch {0};
inline bool CaptureEnabled(bool enabled)
{
    auto epoch = captureEpoch.load(std::memory_order_relaxed);
    while (bool(epoch & 1) != enabled &&
           !captureEpoch.compare_exchange_weak(epoch, epoch + 1, std::memory_order_relaxed)) {}
    return enabled;
}
inline thread_local unsigned suppress = 0;
struct Suppress
{
    Suppress() { ++suppress; }
    ~Suppress() { --suppress; }
};
enum class Kind { None, Table, CBV, SRV, UAV, Constants };
struct RootConstants
{
    // Root signatures contain at most 64 DWORDs. Avoid allocating a tree node
    // for every constant on the game's hot Set*Root32BitConstants path.
    std::array<UINT, 64> values {};
    uint64_t known = 0;
    size_t size() const { return std::popcount(known); }
    UINT operator[](UINT offset) const { return values[offset]; }
    void Set(UINT offset, UINT count, const UINT* data)
    {
        for (UINT i = 0; i < count; ++i) values[offset + i] = data[i];
        if (count) known |= (count == 64 ? ~uint64_t(0) : ((uint64_t(1) << count) - 1) << offset);
    }
    void Restore(ID3D12GraphicsCommandList* list, bool cs, UINT index) const
    {
        // Restore only observed values; combine adjacent writes into one call.
        for (UINT begin = 0; begin < 64;)
        {
            if (!(known & (uint64_t(1) << begin))) { ++begin; continue; }
            UINT end = begin + 1;
            while (end < 64 && (known & (uint64_t(1) << end))) ++end;
            if (cs) list->SetComputeRoot32BitConstants(index, end - begin, values.data() + begin, begin);
            else list->SetGraphicsRoot32BitConstants(index, end - begin, values.data() + begin, begin);
            begin = end;
        }
    }
};
struct Root
{
    Kind kind = Kind::None;
    UINT64 value = 0;
    RootConstants constants;
};
struct BindPoint
{
    ID3D12RootSignature* signature = nullptr;
    std::map<UINT, Root> roots;
};
struct State
{
    ID3D12PipelineState* pipeline = nullptr;
    std::array<ID3D12DescriptorHeap*, 2> heaps {};
    UINT heapCount = 0;
    bool heapsKnown = false;
    bool renderPass = false;
    bool invalid = false;
    bool recordingKnown = false;
    uint64_t epoch = 0;
    BindPoint compute, graphics;
    void Restore(ID3D12GraphicsCommandList* list) const
    {
        Suppress guard;
        list->SetDescriptorHeaps(heapCount, heaps.data());
        for (bool cs : { true, false })
        {
            const auto& point = cs ? compute : graphics;
            if (!point.signature) continue;
            if (cs) list->SetComputeRootSignature(point.signature);
            else list->SetGraphicsRootSignature(point.signature);
            for (const auto& [index, root] : point.roots)
            {
                switch (root.kind)
                {
                case Kind::Table:
                    if (cs) list->SetComputeRootDescriptorTable(index, {root.value});
                    else list->SetGraphicsRootDescriptorTable(index, {root.value});
                    break;
                case Kind::CBV:
                    if (cs) list->SetComputeRootConstantBufferView(index, root.value);
                    else list->SetGraphicsRootConstantBufferView(index, root.value);
                    break;
                case Kind::SRV:
                    if (cs) list->SetComputeRootShaderResourceView(index, root.value);
                    else list->SetGraphicsRootShaderResourceView(index, root.value);
                    break;
                case Kind::UAV:
                    if (cs) list->SetComputeRootUnorderedAccessView(index, root.value);
                    else list->SetGraphicsRootUnorderedAccessView(index, root.value);
                    break;
                case Kind::Constants:
                    root.constants.Restore(list, cs, index);
                    break;
                default: break;
                }
            }
        }
        list->SetPipelineState(pipeline);
    }
};
inline std::mutex mutex;
inline std::unordered_map<ID3D12GraphicsCommandList*, State> states;
inline void Reset(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline = nullptr, bool start = false)
{
    if (suppress) return;
    std::lock_guard lock(mutex);
    states.erase(list);
    if (start)
    {
        auto& state = states[list];
        state.pipeline = pipeline;
        state.recordingKnown = true;
        state.epoch = captureEpoch.load(std::memory_order_relaxed);
    }
}
inline void Pipeline(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline)
{
    if (suppress) return;
    std::lock_guard lock(mutex);
    states[list].pipeline = pipeline;
}
inline void Heaps(ID3D12GraphicsCommandList* list, UINT count, ID3D12DescriptorHeap* const* heaps)
{
    if (suppress) return;
    std::lock_guard lock(mutex);
    auto& s = states[list];
    if (count > 2 || (count && !heaps)) { s.invalid = true; return; }
    s.heapsKnown = true;
    s.heapCount = count;
    for (UINT i = 0; i < count; ++i) s.heaps[i] = heaps[i];
}
inline void Signature(ID3D12GraphicsCommandList* list, bool cs, ID3D12RootSignature* signature)
{
    if (suppress) return;
    std::lock_guard lock(mutex);
    auto& point = cs ? states[list].compute : states[list].graphics;
    if (point.signature != signature) point.roots.clear();
    point.signature = signature;
}
inline void Value(ID3D12GraphicsCommandList* list, bool cs, UINT index, Kind kind, UINT64 value)
{
    if (suppress) return;
    std::lock_guard lock(mutex);
    auto& point = cs ? states[list].compute : states[list].graphics;
    point.roots[index] = {kind, value, {}};
}
inline void Constants(ID3D12GraphicsCommandList* list, bool cs, UINT index, UINT count,
                      const void* data, UINT offset)
{
    if (suppress || !data || count > 64 || offset > 64 || count + offset > 64) return;
    std::lock_guard lock(mutex);
    auto& point = cs ? states[list].compute : states[list].graphics;
    auto& root = point.roots[index];
    if (root.kind != Kind::Constants) root = {};
    root.kind = Kind::Constants;
    root.constants.Set(offset, count, static_cast<const UINT*>(data));
}
inline void RenderPass(ID3D12GraphicsCommandList* list, bool active)
{
    if (suppress) return;
    std::lock_guard lock(mutex);
    states[list].renderPass = active;
}
inline void Invalidate(ID3D12GraphicsCommandList* list)
{
    if (suppress) return;
    std::lock_guard lock(mutex);
    states[list].invalid = true;
}
inline std::optional<State> Snapshot(ID3D12GraphicsCommandList* list, const char** reason = nullptr)
{
    std::lock_guard lock(mutex);
    auto it = states.find(list);
    const char* failure = it == states.end() ? "command-list-untracked" :
        it->second.invalid ? "invalid-or-bundle" : !it->second.recordingKnown ? "reset-not-observed" :
        it->second.epoch != captureEpoch.load(std::memory_order_relaxed) ? "capture-toggled-during-recording" :
        it->second.renderPass ? "inside-render-pass" : !it->second.pipeline ? "pipeline-unknown" :
        !it->second.heapsKnown ? "descriptor-heaps-unknown" :
        (!it->second.compute.signature && !it->second.graphics.signature) ? "root-signatures-unknown" : nullptr;
    if (reason) *reason = failure;
    if (failure) return {};
    return it->second;
}
}
