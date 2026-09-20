#pragma once
#include <d3d12.h>
#include <memory>
#include <tuple>
#include <vector>

// Resource footprints for submission reordering. Missing coverage is an
// explicit dependency, never evidence that a command does not use memory.
namespace DLSSNRPipelineAccess
{
struct Footprint;
using Snapshot = std::shared_ptr<const Footprint>;
void Initialize(ID3D12Device* device);
bool Enabled();
bool RecordingEnabled();
void ObserveBackBuffer(ID3D12Resource* resource);
extern thread_local unsigned suppress;
void Domain(ID3D12GraphicsCommandList* list, const void* domain, const char* label = "private-domain");
struct PrivateWork
{
    PrivateWork() { ++suppress; }
    ~PrivateWork() { --suppress; }
};
void Reset(ID3D12GraphicsCommandList* list);
void Forget(ID3D12GraphicsCommandList* list);
void Cut(ID3D12GraphicsCommandList* list);
void Unknown(ID3D12GraphicsCommandList* list, const char* reason, UINT64 detail = 0);
Snapshot Capture(ID3D12GraphicsCommandList* list, bool prefix);
Snapshot Combine(const std::vector<Snapshot>& parts);
bool Complete(const Snapshot& footprint);
bool HasWork(const Snapshot& footprint);
bool ContainsNR(const Snapshot& footprint);
bool OrdersAllResources(const Snapshot& footprint);
const char* ConflictKind(const Snapshot& a, const Snapshot& b);
// DXGI buffers carry presentation ownership beyond ordinary memory hazards.
bool RequiresOriginalQueue(const Snapshot& footprint);
bool RetainEntryWaits(const Snapshot& footprint);
void TileTrackingReady(bool ready);
// Called under the submission scheduler lock. Retain a conservative union of
// every physical range ever mapped, and order mapping changes on their queue.
Snapshot UpdateTiles(ID3D12Resource* resource, UINT regions, const D3D12_TILE_REGION_SIZE* sizes,
                     ID3D12Heap* heap, UINT ranges, const D3D12_TILE_RANGE_FLAGS* flags,
                     const UINT* offsets, const UINT* counts);
Snapshot CopyTiles(ID3D12Resource* destination, ID3D12Resource* source);
void MarkNR(ID3D12GraphicsCommandList* list);
bool Conflicts(const Snapshot& a, const Snapshot& b);
const char* Reason(const Snapshot& footprint);
// Bounded diagnostics. Collection never treats an incomplete footprint as safe.
void AuditBoundary();
UINT64 AuditSession(const Snapshot& footprint);
void AuditConflicts(UINT64 session, const Snapshot& prefix, const Snapshot& prior,
                    ID3D12Fence* fence, UINT64 value);

void Resource(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, bool write);
void Address(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS address, bool write);
void Barriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers);
void Heaps(ID3D12GraphicsCommandList* list, UINT count, ID3D12DescriptorHeap* const* heaps);
void RootSignature(ID3D12GraphicsCommandList* list, bool compute, ID3D12RootSignature* signature);
void RootTable(ID3D12GraphicsCommandList* list, bool compute, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle);
void RootAddress(ID3D12GraphicsCommandList* list, bool compute, UINT index, UINT64 address, bool write);
void Targets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets,
             BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE* depth);
void VertexBuffers(ID3D12GraphicsCommandList* list, UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views);
void IndexBuffer(ID3D12GraphicsCommandList* list, const D3D12_INDEX_BUFFER_VIEW* view);
void StreamOutput(ID3D12GraphicsCommandList* list, UINT start, UINT count,
                  const D3D12_STREAM_OUTPUT_BUFFER_VIEW* views);
void QueryRange(ID3D12GraphicsCommandList* list, ID3D12QueryHeap* heap, UINT start, UINT count, bool write);
void Descriptor(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE handle, bool write);
void Shader(ID3D12GraphicsCommandList* list, bool compute, bool indexed = false, bool mesh = false);
void ClearState(ID3D12GraphicsCommandList* list);
void Work(ID3D12GraphicsCommandList* list);

template<unsigned Slot, class... Args> void Record(ID3D12GraphicsCommandList* list, Args... args)
{
    if (!Enabled() || suppress) return;
    const auto a = std::tuple(args...);
    if constexpr ((Slot >= 12 && Slot <= 19) || (Slot >= 47 && Slot <= 51) || Slot == 54 ||
                  Slot == 60 || Slot == 61 || Slot == 64 || Slot == 66 || (Slot >= 72 && Slot <= 76) || Slot == 79)
        Work(list);
    if constexpr (Slot == 11) ClearState(list);
    else if constexpr (Slot == 12) Shader(list, false);
    else if constexpr (Slot == 13) Shader(list, false, true);
    else if constexpr (Slot == 14) Shader(list, true);
    else if constexpr (Slot == 79) Shader(list, false, false, true);
    else if constexpr (Slot == 15 || Slot == 17 || Slot == 19)
    {
        Resource(list, std::get<0>(a), true);
        if constexpr (Slot == 17) Resource(list, std::get<1>(a), false);
        else Resource(list, std::get<2>(a), false);
    }
    else if constexpr (Slot == 16)
    {
        Resource(list, std::get<0>(a)->pResource, true);
        Resource(list, std::get<4>(a)->pResource, false);
    }
    else if constexpr (Slot == 18)
    {
        const bool toTiles = (std::get<5>(a) & D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE) != 0;
        Resource(list, std::get<0>(a), toTiles);
        Resource(list, std::get<3>(a), !toTiles);
    }
    else if constexpr (Slot == 26) Barriers(list, args...);
    else if constexpr (Slot == 28) Heaps(list, args...);
    else if constexpr (Slot == 29 || Slot == 30) RootSignature(list, Slot == 29, args...);
    else if constexpr (Slot == 31 || Slot == 32) RootTable(list, Slot == 31, args...);
    else if constexpr (Slot >= 37 && Slot <= 42)
        RootAddress(list, Slot % 2 != 0, std::get<0>(a), std::get<1>(a), Slot >= 41);
    else if constexpr (Slot == 43) IndexBuffer(list, args...);
    else if constexpr (Slot == 44) VertexBuffers(list, args...);
    else if constexpr (Slot == 45) StreamOutput(list, args...);
    else if constexpr (Slot == 46) Targets(list, args...);
    else if constexpr (Slot == 47 || Slot == 48) Descriptor(list, std::get<0>(a), true);
    else if constexpr (Slot == 49 || Slot == 50) Resource(list, std::get<2>(a), true);
    else if constexpr (Slot == 51) Resource(list, std::get<0>(a), true);
    else if constexpr (Slot == 53)
    {
        if (std::get<1>(a) == D3D12_QUERY_TYPE_TIMESTAMP)
            QueryRange(list, std::get<0>(a), std::get<2>(a), 1, true);
        else Unknown(list, "scoped-query", Slot);
    }
    else if constexpr (Slot == 54)
    { QueryRange(list, std::get<0>(a), std::get<2>(a), std::get<3>(a), false); Resource(list, std::get<4>(a), true); }
    else if constexpr (Slot == 78) Resource(list, std::get<0>(a), false);
    else if constexpr (Slot == 20 || Slot == 21 || Slot == 22 || Slot == 23 || Slot == 24 || Slot == 25 ||
                       (Slot >= 33 && Slot <= 36) || Slot == 56 || Slot == 57 || Slot == 58 || Slot == 62 ||
                       Slot == 65 || Slot == 77 || Slot == 81 || Slot == 82 || Slot == 83)
    {
        // Register-only state or balanced annotation. No resource access.
    }
    else Unknown(list, "uncovered-command", Slot);
}
}
