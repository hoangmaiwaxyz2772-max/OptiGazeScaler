#include <pch.h>
#include "DLSSNRPipelineAccess.h"
#include "DLSSNRMethodHooks.h"
#include <Config.h>
#include <wrl/client.h>
#include <map>
#include <unordered_map>
#include <array>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <type_traits>
#include <set>

namespace DLSSNRPipelineAccess
{
thread_local unsigned suppress = 0;
namespace
{
using Microsoft::WRL::ComPtr;
std::recursive_mutex mutex;
std::atomic<bool> enabled {false};
std::optional<bool> requested;
bool installationFailed = false;
bool tileTrackingReady = false;
constexpr size_t Limit = 65536;
struct Audit
{
    UINT64 id = 0, boundaries = 0, recordings = 0, snapshots = 0, examples = 0, omitted = 0;
    bool active = true;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::map<std::string, UINT64> reasons;
    std::map<std::string, UINT64> examplesPerReason;
    std::set<SIZE_T> descriptorExamples;
    std::set<std::tuple<UINT64, bool, UINT>> rootExamples;
};
std::shared_ptr<Audit> audit;
UINT64 auditSerial = 0, nrBoundaries = 0, missingObjectsOmitted = 0, missingObjectSerial = 0;
std::unordered_map<ID3D12Resource*, UINT64> missingObjects;
void EndAudit(const char* reason)
{
    if (!audit || !audit->active) return;
    audit->active = false;
    for (const auto& [key, count] : audit->reasons)
        spdlog::info("{} [DLSSNR_AUDIT_REASON] session={} reason={} occurrences={}", __FUNCTION__, audit->id, key, count);
    spdlog::info("{} [DLSSNR_AUDIT] session={} end={} recordings={} snapshots={} exampleOmitted={} objectDetailsOmitted={} coverage=observed-paths-only", __FUNCTION__,
             audit->id, reason, audit->recordings, audit->snapshots, audit->omitted, missingObjectsOmitted);
}
bool AuditActive(const std::shared_ptr<Audit>& a)
{
    if (!a || !a->active) return false;
    if (std::chrono::steady_clock::now() - a->start > std::chrono::seconds(5))
    {
        if (a == audit) EndAudit("time-limit");
        return false;
    }
    return true;
}
UINT64 DescribeMissingResource(ID3D12Resource* resource, const char* origin);
struct Identity
{
    std::atomic<bool> alive {true};
    std::atomic<bool> originalQueueOnly {false};
    const char* incomplete = nullptr;
    UINT64 id = 0;
    UINT64 object = 0;
    const char* kind = "private-domain";
    std::shared_ptr<Identity> allocation;
    UINT64 address = 0, bytes = 0;
    UINT64 allocationOffset = 0, allocationBytes = UINT64_MAX;
    bool reserved = false, shared = false;
    // Immutable range identities. Mapping edits replace/coalesce entries;
    // submitted footprints retain their original physical range snapshots.
    std::vector<std::shared_ptr<Identity>> backing;
};
UINT64 nextIdentity = 0;
struct View
{
    std::shared_ptr<Identity> resource, counter;
    bool known = false, write = false;
    const char* unknown = "untracked-descriptor";
    UINT64 missingResource = 0, missingCounter = 0;
    SIZE_T sourceHandle = 0;
    UINT64 address = 0;
};
struct Heap
{
    std::atomic<bool> alive {true};
    UINT64 object = 0;
    SIZE_T cpu = 0;
    UINT64 gpu = 0;
    UINT increment = 0, count = 0;
    D3D12_DESCRIPTOR_HEAP_TYPE type {};
    std::map<UINT, View> views;
};
struct Range { UINT offset = 0, count = 0; };
struct Root
{
    std::atomic<bool> alive {true};
    bool known = true, direct = false;
    const char* reason = "untracked-root-signature";
    std::array<std::vector<Range>, 64> tables;
    std::array<D3D12_ROOT_PARAMETER_TYPE, 64> kinds {};
    std::array<D3D12_SHADER_VISIBILITY, 64> visibility {};
    UINT count = 0;
};
// Snapshots keep metadata alive after COM object destruction. The object's Box
// explicitly expires registry eligibility; weak_ptr alone cannot detect reuse.
constexpr GUID MetadataId {0x0ad7f440,0xbc38,0x4cc9,{0x85,0x71,0x20,0xf5,0xaa,0xa7,0x63,0x14}};
constexpr GUID IdentityMetadataId {0x26d4bd66,0x3b13,0x4549,{0xaa,0xbc,0x1a,0xe5,0xde,0x0b,0x70,0x54}};
constexpr GUID CreationMetadataId {0x0ac3ba82,0x4c77,0x4f9f,{0xab,0x62,0x21,0x40,0x73,0x9d,0x43,0x3e}};
struct CreationMetadata
{
    UINT slot = 0;
    UINT64 heap = 0, heapIdentity = 0, offset = 0, bytes = UINT64_MAX;
    char result[64] {};
};
template<class T> struct Box final : IUnknown
{
    std::atomic<ULONG> references {1};
    std::shared_ptr<T> value;
    explicit Box(std::shared_ptr<T> v) : value(std::move(v)) {}
    ~Box() { value->alive.store(false, std::memory_order_release); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *output = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto left = --references;
        if (!left) delete this;
        return left;
    }
};
std::unordered_map<IUnknown*, std::weak_ptr<Identity>> identities;
std::unordered_map<ID3D12DescriptorHeap*, std::weak_ptr<Heap>> heaps;
std::map<SIZE_T, std::weak_ptr<Heap>> cpuHeaps;
std::unordered_map<ID3D12RootSignature*, std::weak_ptr<Root>> roots;
std::map<UINT64, std::weak_ptr<Identity>> addresses;
std::unordered_map<const void*, std::shared_ptr<Identity>> domains;
std::shared_ptr<Identity> sharedMemoryDomain;
void PruneMetadata()
{
    static UINT64 creations = 0;
    if (++creations % 256 != 0) return;
    const auto dead = [](const auto& item) {
        auto value = item.second.lock();
        return !value || !value->alive.load(std::memory_order_acquire);
    };
    std::erase_if(identities, dead); std::erase_if(heaps, dead);
    std::erase_if(cpuHeaps, dead); std::erase_if(roots, dead); std::erase_if(addresses, dead);
}
template<class T, class O> bool Attach(O* object, const std::shared_ptr<T>& value)
{
    auto* box = new Box<T>(value);
    const auto result = object->SetPrivateDataInterface(
        std::is_same_v<T, Identity> ? IdentityMetadataId : MetadataId, box);
    box->Release();
    return SUCCEEDED(result);
}
template<class O> ComPtr<O> NativeObject(O* object)
{
    ComPtr<O> result = object;
    if (!object) return result;
    // Streamline's public unwrap contract. Resource/heap wrappers need not
    // keep their own private-data store identical to the native object's.
    constexpr GUID realObjectId {0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
    ComPtr<IUnknown> native;
    if (SUCCEEDED(object->QueryInterface(realObjectId, &native)) && native)
    {
        ComPtr<O> typed;
        if (SUCCEEDED(native.As(&typed))) result = std::move(typed);
    }
    return result;
}
template<class T> std::shared_ptr<T> ReadMetadata(IUnknown* object)
{
    if (!object) return {};
    const auto native = NativeObject(object);
    ComPtr<ID3D12Object> owner;
    if (FAILED(native.As(&owner))) return {};
    Box<T>* box = nullptr; UINT bytes = sizeof(box);
    const auto key = std::is_same_v<T, Identity> ? IdentityMetadataId : MetadataId;
    if (FAILED(owner->GetPrivateData(key, &bytes, &box)) || !box) return {};
    const auto value = box->value;
    box->Release();
    return value->alive.load(std::memory_order_acquire) ? value : nullptr;
}
std::shared_ptr<Identity> IdentityOf(IUnknown* object)
{
    if (!object) return {};
    const auto it = identities.find(object);
    if (it != identities.end())
        if (auto identity = it->second.lock(); identity && identity->alive.load(std::memory_order_acquire)) return identity;
    // A view may be supplied a different COM interface/wrapper pointer from
    // CreateResource. Recover the existing object-owned identity; never invent
    // a committed allocation for a resource whose creation was not observed.
    const auto identity = ReadMetadata<Identity>(object);
    // Do not cache an arbitrary wrapper's address under the native object's
    // lifetime: the wrapper can die and be reused while the native survives.
    return identity;
}
std::shared_ptr<Identity> AddressOf(UINT64 address)
{
    if (!address) return {};
    auto it = addresses.upper_bound(address);
    if (it == addresses.begin()) return {};
    auto value = (--it)->second.lock();
    return value && value->alive.load(std::memory_order_acquire) &&
        address >= value->address && address - value->address < value->bytes ? value : nullptr;
}
std::shared_ptr<Heap> CpuHeap(SIZE_T handle)
{
    auto it = cpuHeaps.upper_bound(handle);
    if (it == cpuHeaps.begin()) return {};
    auto heap = (--it)->second.lock();
    return heap && heap->alive.load(std::memory_order_acquire) && handle >= heap->cpu && (handle - heap->cpu) / heap->increment < heap->count &&
        (handle - heap->cpu) % heap->increment == 0 ? heap : nullptr;
}
View CpuView(SIZE_T handle)
{
    auto heap = CpuHeap(handle);
    if (!heap) return {{}, {}, false, false, "untracked-descriptor-source-heap", 0, 0, handle};
    const auto it = heap->views.find(static_cast<UINT>((handle - heap->cpu) / heap->increment));
    return it == heap->views.end() ? View{{}, {}, false, false, "untracked-descriptor-source-slot", 0, 0, handle} : it->second;
}
void StoreView(SIZE_T handle, const View& view)
{
    auto heap = CpuHeap(handle);
    if (heap)
    {
        auto stored = view;
        if (!stored.sourceHandle) stored.sourceHandle = handle;
        heap->views[static_cast<UINT>((handle - heap->cpu) / heap->increment)] = stored;
    }
}
struct TableUse
{
    std::shared_ptr<Heap> heap;
    UINT offset = 0, count = 0;
};
}

struct Footprint
{
    const char* incomplete = nullptr;
    bool hasWork = false;
    bool containsNr = false;
    bool retainEntryWaits = false;
    // A null-endpoint alias/UAV barrier orders memory but does not introduce
    // hidden shader accesses. Retain ALL predecessor dependencies for it.
    bool ordersAllResources = false;
    UINT64 namedAliases = 0, wildcardAliases = 0, globalUavs = 0, splitTransitions = 0;
    std::shared_ptr<Audit> audit;
    UINT64 list = 0;
    bool tail = false;
    std::unordered_map<UINT64, bool> access;
    std::vector<std::shared_ptr<Identity>> lifetime;
    std::unordered_map<UINT64, std::vector<Identity*>> aliases;
    std::vector<TableUse> tables;
};
namespace
{
// Once a whole-list footprint orders every resource, enumerating more
// descriptors cannot make it independent. Keep full enumeration in audits.
bool Inspect(const Footprint& f) { return (!f.incomplete && !f.ordersAllResources) || AuditActive(f.audit); }
void Issue(Footprint& f, const char* reason, UINT64 object = 0, UINT64 detail = 0)
{
    if (!f.incomplete) f.incomplete = reason;
    if (!AuditActive(f.audit)) return;
    auto& a = *f.audit;
    const auto key = std::format("{}:{}", f.tail ? "tail" : "prefix", reason);
    ++a.reasons[key];
    if (++a.examplesPerReason[key] <= 8 && a.examples < 256)
    {
        ++a.examples;
        spdlog::info("{} [DLSSNR_AUDIT_ISSUE] session={} list=0x{:X} part={} reason={} object=0x{:X} detail=0x{:X}", __FUNCTION__,
                 a.id, f.list, f.tail ? "tail" : "prefix", reason, object, detail);
    }
    else ++a.omitted;
}
struct Bindings
{
    std::shared_ptr<Root> root;
    UINT64 rootObject = 0;
    std::array<UINT64, 64> tables {}, addresses {};
    std::array<bool, 64> writes {};
    const char* invalidation = "reset";
};
struct Recording
{
    Footprint prefix, tail;
    bool cut = false;
    bool nrSeen = false;
    std::shared_ptr<Heap> shaderHeap;
    // Heap identity is compared, not just its recyclable COM address.
    std::shared_ptr<Heap> samplerHeap;
    Bindings compute, graphics;
    std::array<UINT64, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertices {};
    UINT64 index = 0;
    std::array<D3D12_STREAM_OUTPUT_BUFFER_VIEW, D3D12_SO_BUFFER_SLOT_COUNT> streamOutput {};
    std::vector<View> targets;
    Footprint& Current() { return cut ? tail : prefix; }
};
std::unordered_map<ID3D12GraphicsCommandList*, Recording> recordings;
Recording* Find(ID3D12GraphicsCommandList* list)
{
    const auto it = recordings.find(list);
    return it == recordings.end() ? nullptr : &it->second;
}
void Add(Footprint& result, const std::shared_ptr<Identity>& resource, bool write)
{
    if (!Inspect(result)) return; // Outside audit, opaque remains an absorbing fast path.
    if (!resource) { Issue(result, "untracked-resource-or-address"); return; }
    if (resource->incomplete) Issue(result, resource->incomplete, resource->object);
    if (resource->allocation && resource->allocation->incomplete)
        Issue(result, resource->allocation->incomplete, resource->allocation->object);
    if (!Inspect(result)) return;
    if (result.access.size() >= Limit) { Issue(result, "resource-limit"); return; }
    const auto [it, inserted] = result.access.emplace(resource->id, write);
    it->second |= write;
    if (inserted)
    {
        result.lifetime.push_back(resource);
        if (resource->allocation) result.aliases[resource->allocation->id].push_back(resource.get());
    }
    if (resource->shared || (resource->allocation && resource->allocation->shared))
    {
        result.retainEntryWaits = true;
        if (!sharedMemoryDomain)
        {
            sharedMemoryDomain = std::make_shared<Identity>();
            sharedMemoryDomain->id = ++nextIdentity;
            sharedMemoryDomain->kind = "shared-memory-domain";
        }
        // All known shared allocations may alias through external handles.
        // Serialize them together AND retain the application's external waits.
        Add(result, sharedMemoryDomain, true);
    }
}
void ResolveBacking(Footprint& result)
{
    const auto resources = result.lifetime.size();
    for (size_t i = 0; i < resources && Inspect(result); ++i)
    {
        const auto resource = result.lifetime[i];
        if (!resource->reserved) continue;
        if (resource->incomplete) Issue(result, resource->incomplete, resource->object);
        const bool write = result.access.at(resource->id);
        for (const auto& range : resource->backing) Add(result, range, write);
    }
}
void AddView(Footprint& result, const View& view, bool write, UINT64 handle = 0)
{
    if (!Inspect(result)) return;
    if (!view.known)
    {
        Issue(result, view.unknown, handle, view.missingCounter ? view.missingCounter : view.missingResource);
        if (view.missingResource && view.missingCounter)
            Issue(result, "untracked-descriptor-resource", handle, view.missingResource);
        if (!AuditActive(result.audit)) return;
    }
    if (view.resource) Add(result, view.resource, write || view.write);
    if (view.counter) Add(result, view.counter, true);
}
struct ResolveStats { size_t ranges = 0, descriptors = 0; };
ResolveStats Resolve(Footprint& result)
{
    ResolveStats stats;
    if (!Inspect(result)) { result.tables.clear(); return stats; }
    // Resolve the union of ranges, not every differently sized/offset table.
    // Bindless draws commonly refer to overlapping suffixes of one large heap.
    // All resolution still uses submission-time descriptors under mutex.
    auto tables = std::move(result.tables);
    std::sort(tables.begin(), tables.end(), [](const auto& a, const auto& b) {
        if (a.heap.get() != b.heap.get()) return std::less<Heap*>{}(a.heap.get(), b.heap.get());
        return a.offset < b.offset;
    });
    Heap* previous = nullptr;
    UINT previousEnd = 0;
    for (const auto& table : tables)
    {
        if (!Inspect(result)) break;
        if (!table.heap || table.offset > table.heap->count || table.count > table.heap->count - table.offset)
        { Issue(result, "descriptor-range"); continue; }
        const UINT end = table.offset + table.count;
        const UINT begin = table.heap.get() == previous ? std::max(table.offset, previousEnd) : table.offset;
        previous = table.heap.get();
        previousEnd = std::max(begin, end);
        if (begin >= end) continue;
        ++stats.ranges;
        // A registered heap was created after all descriptor writers were
        // intercepted. Absent entries were never initialized and cannot be
        // legally dereferenced. Explicitly unknown/copied entries still fail.
        for (auto it = table.heap->views.lower_bound(begin); it != table.heap->views.end() && it->first < end; ++it)
        {
            ++stats.descriptors;
            const auto handle = table.heap->cpu + SIZE_T(it->first) * table.heap->increment;
            if (!it->second.known && AuditActive(result.audit))
            {
                auto& a = *result.audit;
                if (a.descriptorExamples.size() < 256)
                {
                    if (a.descriptorExamples.insert(handle).second)
                        spdlog::info("{} [DLSSNR_AUDIT_DESCRIPTOR] session={} list=0x{:X} part={} heap=0x{:X} slot={} cpu=0x{:X} gpu=0x{:X} sourceCpu=0x{:X} write={} missingObject={} missingCounter={} reason={} bufferAddress=0x{:X}", __FUNCTION__,
                            a.id, result.list, result.tail ? "tail" : "prefix", table.heap->object, it->first, handle,
                            table.heap->gpu + UINT64(it->first) * table.heap->increment, it->second.sourceHandle,
                            it->second.write, it->second.missingResource, it->second.missingCounter, it->second.unknown,
                            it->second.address);
                }
                else { ++a.omitted; ++a.reasons["descriptor-example-limit"]; }
            }
            AddView(result, it->second, false, handle);
            if (!Inspect(result)) break;
        }
    }
    return stats;
}
const char* RegisterResource(ID3D12Resource* resource, ID3D12Heap* heap, bool committed,
                             UINT64 offset = 0, UINT64 allocationBytes = UINT64_MAX)
{
    if (!resource) return "null-resource";
    if (IdentityOf(resource)) return "already-tracked"; // Forwarded/newer APIs or cached objects.
    const auto native = NativeObject(resource);
    resource = native.Get();
    auto record = std::make_shared<Identity>();
    record->id = ++nextIdentity;
    record->object = uintptr_t(resource); record->kind = "resource";
    if (committed)
    {
        D3D12_HEAP_FLAGS flags {}; D3D12_HEAP_PROPERTIES properties {};
        if (SUCCEEDED(resource->GetHeapProperties(&properties, &flags)) &&
            (flags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)))
            record->shared = true;
    }
    if (!committed)
    {
        record->reserved = !heap;
        if (record->reserved && !tileTrackingReady) record->incomplete = "tile-mapping-prehistory";
        record->allocation = IdentityOf(heap);
        if (heap && !record->allocation) return "heap-identity-missing";
    }
    const auto desc = resource->GetDesc();
    if (record->allocation)
    {
        record->allocationOffset = offset; // This is known even if size query fails.
        const auto heapBytes = heap->GetDesc().SizeInBytes;
        if (offset >= heapBytes) return "allocation-offset-out-of-heap";
        if (allocationBytes && allocationBytes != UINT64_MAX && allocationBytes <= heapBytes - offset)
            record->allocationBytes = allocationBytes;
        else
        {
            // A successful placed creation is contained in its heap. Keep the
            // suffix as a conservative bound, never invent a precise size.
            record->allocationBytes = heapBytes - offset;
        }
    }
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        record->address = resource->GetGPUVirtualAddress();
        record->bytes = desc.Width;
    }
    if (!Attach(resource, record)) return "resource-metadata-store-failed";
    identities[resource] = record;
    if (record->address) addresses[record->address] = record;
    return record->reserved ? "tracked-reserved" :
        allocationBytes == UINT64_MAX && heap ? "tracked-with-heap-suffix" : "tracked";
}

template<class Desc> D3D12_RESOURCE_DESC1 AllocationDesc(const Desc& desc)
{
    if constexpr (std::is_same_v<Desc, D3D12_RESOURCE_DESC1>) return desc;
    else return {desc.Dimension, desc.Alignment, desc.Width, desc.Height, desc.DepthOrArraySize,
                 desc.MipLevels, desc.Format, desc.SampleDesc, desc.Layout, desc.Flags, {}};
}
template<class Device, class Desc>
UINT64 AllocationSize(Device* device, const Desc* desc, UINT32 castCount = 0, const DXGI_FORMAT* casts = nullptr)
{
    if (!desc) return UINT64_MAX;
    const auto desc1 = AllocationDesc(*desc);
    ComPtr<ID3D12Device12> d12;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d12))))
        return d12->GetResourceAllocationInfo3(0, 1, &desc1, &castCount, &casts, nullptr).SizeInBytes;
    if (castCount) return UINT64_MAX;
    ComPtr<ID3D12Device8> d8;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d8))))
        return d8->GetResourceAllocationInfo2(0, 1, &desc1, nullptr).SizeInBytes;
    if constexpr (std::is_same_v<Desc, D3D12_RESOURCE_DESC>)
        return device->GetResourceAllocationInfo(0, 1, desc).SizeInBytes;
    return UINT64_MAX;
}
void RecordCreation(ID3D12Resource* resource, UINT slot, ID3D12Heap* heap,
                    UINT64 offset, UINT64 bytes, const char* result)
{
    const auto native = NativeObject(resource);
    resource = native.Get();
    CreationMetadata record; record.slot = slot; record.heap = uintptr_t(heap);
    if (const auto identity = IdentityOf(heap)) record.heapIdentity = identity->id;
    record.offset = offset; record.bytes = bytes;
    strncpy_s(record.result, result, _TRUNCATE);
    const auto stored = resource->SetPrivateData(CreationMetadataId, sizeof(record), &record);
    if (const auto identity = IdentityOf(resource); identity && identity->shared)
    {
        static UINT64 sharedSamples = 0;
        if (++sharedSamples <= 8 || sharedSamples % 3000 == 0)
        {
            const auto desc = resource->GetDesc();
            spdlog::info("{} [DLSSNR_ASYNC_SHARED] sample={} resource=0x{:X} id={} creationSlot={} dimension={} width={} height={} format={} policy=shared-domain-and-original-entry-waits", __FUNCTION__,
                sharedSamples, uintptr_t(resource), identity->id, slot, UINT(desc.Dimension), desc.Width, desc.Height, UINT(desc.Format));
        }
    }
    static std::map<std::pair<UINT, std::string>, UINT64> counters;
    const auto sample = ++counters[{slot, result}];
    if (sample <= 3 || sample % 3000 == 0)
        spdlog::info("{} [DLSSNR_ACCESS_CREATE] slot={} result={} sample={} resource=0x{:X} heap=0x{:X} heapId={} offset={} bytes={} metadataHr=0x{:08X}", __FUNCTION__,
            slot, result, sample, uintptr_t(resource), record.heap, record.heapIdentity, offset, bytes, UINT(stored));
}

// Device creation hooks use the actual interface's public call, and never
// transplant a runtime trampoline to an unrelated object implementation.
template<unsigned Slot, auto Member, typename Signature = decltype(Member)> struct Creation;
template<unsigned Slot, auto Member, class Object, class... Args>
struct Creation<Slot, Member, HRESULT(STDMETHODCALLTYPE Object::*)(Args...)>
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(Object*, Args...);
    using Hook = DLSSNRMethodHooks::MethodHook<34000 + Slot, Fn>;
    static HRESULT STDMETHODCALLTYPE Call(Object* device, Args... args)
    {
        const auto result = Hook::Forward(device, args...);
        if (FAILED(result)) return result;
        const auto a = std::tuple(args...);
        auto** output = std::get<sizeof...(Args) - 1>(a);
        if (!output || !*output) return result;
        std::lock_guard lock(mutex);
        // Initialization holds mutex while hooks are installed. Do not drop
        // a concurrent creation merely because enabled has not published yet.
        if (!enabled) return result;
        PruneMetadata();
        auto* object = static_cast<IUnknown*>(*output);
        if constexpr (Slot == 28 || Slot == 54 || Slot == 48 || Slot == 49 || Slot == 81)
        {
            ComPtr<ID3D12Heap> heap;
            if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&heap))))
            {
                heap = NativeObject(heap.Get());
                if (IdentityOf(heap.Get())) return result;
                auto identity = std::make_shared<Identity>(); identity->id = ++nextIdentity;
                identity->object = uintptr_t(heap.Get()); identity->kind = "heap";
                if constexpr (Slot == 48 || Slot == 49 || Slot == 81)
                    identity->incomplete = "imported-heap-aliasing";
                if (heap->GetDesc().Flags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER))
                    identity->shared = true;
                const bool stored = Attach(heap.Get(), identity);
                if (stored) identities[heap.Get()] = identity;
                static UINT64 creations = 0;
                // Template function names may contain braces on MSVC; pass
                // the name as data rather than part of the format string.
                if (++creations <= 3 || !stored)
                    spdlog::info("{} [DLSSNR_ACCESS_HEAP] slot={} heap=0x{:X} id={} bytes={} tracked={}",
                        __FUNCTION__, Slot, uintptr_t(heap.Get()), identity->id, heap->GetDesc().SizeInBytes, stored);
            }
        }
        else if constexpr (Slot == 14)
        {
            ComPtr<ID3D12DescriptorHeap> heap;
            if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&heap))))
            {
                heap = NativeObject(heap.Get());
                auto record = std::make_shared<Heap>();
                record->object = uintptr_t(heap.Get());
                const auto desc = heap->GetDesc();
                record->count = desc.NumDescriptors; record->type = desc.Type;
                record->increment = device->GetDescriptorHandleIncrementSize(desc.Type);
                record->cpu = heap->GetCPUDescriptorHandleForHeapStart().ptr;
                if (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
                    record->gpu = heap->GetGPUDescriptorHandleForHeapStart().ptr;
                if (record->increment && Attach(heap.Get(), record))
                { heaps[heap.Get()] = record; cpuHeaps[record->cpu] = record; }
            }
        }
        else if constexpr (Slot == 27 || Slot == 29 || Slot == 30 || Slot == 53 || Slot == 55 ||
                           Slot == 69 || Slot == 70 || Slot == 76 || Slot == 77 || Slot == 78)
        {
            ComPtr<ID3D12Resource> resource;
            if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&resource))))
            {
                if constexpr (Slot == 29 || Slot == 70 || Slot == 77)
                {
                    UINT64 bytes;
                    if constexpr (Slot == 77)
                        bytes = AllocationSize(device, std::get<2>(a), std::get<5>(a), std::get<6>(a));
                    else bytes = AllocationSize(device, std::get<2>(a));
                    const auto status = RegisterResource(resource.Get(), std::get<0>(a), false, std::get<1>(a), bytes);
                    RecordCreation(resource.Get(), Slot, std::get<0>(a), std::get<1>(a), bytes, status);
                }
                else if constexpr (Slot == 30 || Slot == 55 || Slot == 78)
                    RecordCreation(resource.Get(), Slot, nullptr, 0, UINT64_MAX,
                                   RegisterResource(resource.Get(), nullptr, false));
                else
                    RecordCreation(resource.Get(), Slot, nullptr, 0, UINT64_MAX,
                                   RegisterResource(resource.Get(), nullptr, true));
            }
            else spdlog::warn("{} [DLSSNR_ACCESS_CREATE] slot={} result=resource-interface-unavailable", __FUNCTION__, Slot);
        }
        return result;
    }
    static bool Install(Object* object)
    { return Hook::Install((*reinterpret_cast<void***>(object))[Slot], Call) == NO_ERROR; }
};

std::string AuditLocation(const void* address)
{
    HMODULE module = nullptr;
    if (!address || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCSTR>(address), &module)) return "unknown";
    char path[MAX_PATH] {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const auto result = std::format("{}+0x{:X}", path, uintptr_t(address) - uintptr_t(module));
    FreeLibrary(module);
    return result;
}
template<unsigned Slot, auto Member, class T> void AuditCreator(UINT64 id, T* device)
{
    const auto entry = (*reinterpret_cast<void***>(device))[Slot];
    spdlog::info("{} [DLSSNR_AUDIT_CREATOR] objectRecord={} slot={} entry=0x{:X} hooked={} location={}", __FUNCTION__,
        id, Slot, uintptr_t(entry), Creation<Slot, Member>::Hook::Contains(entry), AuditLocation(entry));
}
UINT64 DescribeMissingResource(ID3D12Resource* resource, const char* origin)
{
    if (!resource) return 0;
    const auto native = NativeObject(resource);
    resource = native.Get();
    if (const auto it = missingObjects.find(resource); it != missingObjects.end()) return it->second;
    if (missingObjects.size() >= 128) { ++missingObjectsOmitted; return 0; }
    const UINT64 id = ++missingObjectSerial;
    missingObjects.emplace(resource, id);
    // Read while the API caller owns a live resource. Descriptors keep this
    // observation number, never a raw pointer for a later COM dereference.
    // Pointer deduplication is diagnostic only, NOT a resource lifetime ID.
    const auto desc = resource->GetDesc();
    D3D12_HEAP_PROPERTIES properties {};
    D3D12_HEAP_FLAGS flags {};
    const auto heapResult = resource->GetHeapProperties(&properties, &flags);
    CreationMetadata creation; UINT creationBytes = sizeof(creation);
    const auto creationResult = resource->GetPrivateData(CreationMetadataId, &creationBytes, &creation);
    spdlog::info("{} [DLSSNR_AUDIT_ORIGIN] record={} observed={} slot={} result={} heap=0x{:X} heapId={} offset={} bytes={}", __FUNCTION__,
        id, SUCCEEDED(creationResult), creation.slot, SUCCEEDED(creationResult) ? creation.result : "creation-not-observed",
        creation.heap, creation.heapIdentity, creation.offset, creation.bytes);
    spdlog::info("{} [DLSSNR_AUDIT_OBJECT] record={} resource=0x{:X} origin={} dimension={} width={} height={} depth={} format={} flags={} heapResult=0x{:X} heapType={} heapFlags={} vtable=0x{:X} pointer-is-not-lifetime-id", __FUNCTION__,
        id, uintptr_t(resource), origin, UINT(desc.Dimension), desc.Width, desc.Height, desc.DepthOrArraySize,
        UINT(desc.Format), UINT(desc.Flags), UINT(heapResult), UINT(properties.Type), UINT(flags),
        uintptr_t(*reinterpret_cast<void***>(resource)));
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&device))))
    {
        AuditCreator<27, &ID3D12Device::CreateCommittedResource>(id, device.Get());
        AuditCreator<29, &ID3D12Device::CreatePlacedResource>(id, device.Get());
        AuditCreator<30, &ID3D12Device::CreateReservedResource>(id, device.Get());
        AuditCreator<28, &ID3D12Device::CreateHeap>(id, device.Get());
        ComPtr<ID3D12Device3> d3;
        if (SUCCEEDED(device.As(&d3)))
        {
            AuditCreator<48, &ID3D12Device3::OpenExistingHeapFromAddress>(id, d3.Get());
            AuditCreator<49, &ID3D12Device3::OpenExistingHeapFromFileMapping>(id, d3.Get());
        }
        ComPtr<ID3D12Device4> d4;
        if (SUCCEEDED(device.As(&d4)))
        {
            AuditCreator<53, &ID3D12Device4::CreateCommittedResource1>(id, d4.Get());
            AuditCreator<54, &ID3D12Device4::CreateHeap1>(id, d4.Get());
            AuditCreator<55, &ID3D12Device4::CreateReservedResource1>(id, d4.Get());
        }
        ComPtr<ID3D12Device8> d8;
        if (SUCCEEDED(device.As(&d8)))
        {
            AuditCreator<69, &ID3D12Device8::CreateCommittedResource2>(id, d8.Get());
            AuditCreator<70, &ID3D12Device8::CreatePlacedResource1>(id, d8.Get());
        }
        ComPtr<ID3D12Device10> d10;
        if (SUCCEEDED(device.As(&d10)))
        {
            AuditCreator<76, &ID3D12Device10::CreateCommittedResource3>(id, d10.Get());
            AuditCreator<77, &ID3D12Device10::CreatePlacedResource2>(id, d10.Get());
            AuditCreator<78, &ID3D12Device10::CreateReservedResource2>(id, d10.Get());
        }
        ComPtr<ID3D12Device13> d13;
        if (SUCCEEDED(device.As(&d13)))
            AuditCreator<81, &ID3D12Device13::OpenExistingHeapFromAddress1>(id, d13.Get());
    }
    void* stack[8] {};
    const auto depth = CaptureStackBackTrace(1, UINT(std::size(stack)), stack, nullptr);
    for (USHORT i = 0; i < depth; ++i)
        spdlog::info("{} [DLSSNR_AUDIT_OBJECT_STACK] record={} depth={} location={}", __FUNCTION__, id, i, AuditLocation(stack[i]));
    return id;
}

using SrvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using UavFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using RtvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using DsvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CbvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using SrvHook = DLSSNRMethodHooks::MethodHook<34118, SrvFn>;
using UavHook = DLSSNRMethodHooks::MethodHook<34119, UavFn>;
using RtvHook = DLSSNRMethodHooks::MethodHook<34120, RtvFn>;
using DsvHook = DLSSNRMethodHooks::MethodHook<34121, DsvFn>;
using CbvHook = DLSSNRMethodHooks::MethodHook<34117, CbvFn>;
View ResourceView(ID3D12Resource* resource, bool write, const char* origin)
{
    auto identity = IdentityOf(resource);
    View view {identity, {}, !resource || identity != nullptr, write, "untracked-descriptor-resource"};
    if (!view.known) view.missingResource = DescribeMissingResource(resource, origin);
    return view;
}
void STDMETHODCALLTYPE Srv(ID3D12Device* d, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    SrvHook::Forward(d, r, desc, h);
    std::lock_guard lock(mutex);
    auto view = ResourceView(r, false, "SRV");
    if (desc && desc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)
    {
        view.known = false; // The BVH contains further unenumerated GPU addresses.
        view.unknown = "untracked-descriptor-raytracing-bvh";
    }
    StoreView(h.ptr, view);
}
void STDMETHODCALLTYPE Uav(ID3D12Device* d, ID3D12Resource* r, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    UavHook::Forward(d, r, counter, desc, h);
    std::lock_guard lock(mutex);
    auto view = ResourceView(r, true, "UAV"); view.counter = IdentityOf(counter);
    if (counter && !view.counter)
    {
        view.known = false; view.unknown = "untracked-descriptor-uav-counter";
        view.missingCounter = DescribeMissingResource(counter, "UAV-counter");
    }
    StoreView(h.ptr, view);
}
void STDMETHODCALLTYPE Rtv(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h)
{ RtvHook::Forward(d, r, desc, h); std::lock_guard lock(mutex); StoreView(h.ptr, ResourceView(r, true, "RTV")); }
void STDMETHODCALLTYPE Dsv(ID3D12Device* d, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h)
{ DsvHook::Forward(d, r, desc, h); std::lock_guard lock(mutex); StoreView(h.ptr, ResourceView(r, true, "DSV")); }
void STDMETHODCALLTYPE Cbv(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    CbvHook::Forward(d, desc, h); std::lock_guard lock(mutex);
    const auto resource = desc ? AddressOf(desc->BufferLocation) : nullptr;
    StoreView(h.ptr, {resource, {}, !desc || !desc->BufferLocation || resource != nullptr, false,
                     "untracked-cbv-address", 0, 0, h.ptr, desc ? desc->BufferLocation : 0});
}
using CopySimpleFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopySimpleHook = DLSSNRMethodHooks::MethodHook<34124, CopySimpleFn>;
using CopyHook = DLSSNRMethodHooks::MethodHook<34123, CopyFn>;
using FeedbackFn = void(STDMETHODCALLTYPE*)(ID3D12Device8*, ID3D12Resource*, ID3D12Resource*, D3D12_CPU_DESCRIPTOR_HANDLE);
using FeedbackHook = DLSSNRMethodHooks::MethodHook<34171, FeedbackFn>;
void STDMETHODCALLTYPE Feedback(ID3D12Device8* d, ID3D12Resource* target, ID3D12Resource* feedback,
                                 D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    FeedbackHook::Forward(d, target, feedback, handle);
    std::lock_guard lock(mutex);
    auto view = ResourceView(feedback, true, "sampler-feedback");
    view.counter = IdentityOf(target); // Both resources conservatively participate as writes.
    if (target && !view.counter)
    {
        view.known = false; view.unknown = "untracked-feedback-target";
        view.missingCounter = DescribeMissingResource(target, "sampler-feedback-target");
    }
    StoreView(handle.ptr, view);
}
void STDMETHODCALLTYPE CopySimple(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    CopySimpleHook::Forward(d, n, dst, src, type);
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) return; // Samplers do not access resource memory.
    std::lock_guard lock(mutex); const UINT step = d->GetDescriptorHandleIncrementSize(type);
    std::vector<View> values;
    for (UINT i = 0; i < n; ++i) values.push_back(CpuView(src.ptr + SIZE_T(i) * step));
    for (UINT i = 0; i < n; ++i) StoreView(dst.ptr + SIZE_T(i) * step, values[i]);
}
void STDMETHODCALLTYPE Copy(ID3D12Device* d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dst, const UINT* dn,
                             UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* sn, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    CopyHook::Forward(d, nd, dst, dn, ns, src, sn, type);
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) return;
    if (!dst || !src) return;
    std::lock_guard lock(mutex); const UINT step = d->GetDescriptorHandleIncrementSize(type);
    std::vector<View> values;
    for (UINT r = 0; r < ns; ++r) for (UINT i = 0; i < (sn ? sn[r] : 1); ++i)
        values.push_back(CpuView(src[r].ptr + SIZE_T(i) * step));
    size_t index = 0;
    for (UINT r = 0; r < nd; ++r) for (UINT i = 0; i < (dn ? dn[r] : 1); ++i)
        StoreView(dst[r].ptr + SIZE_T(i) * step, index < values.size() ? values[index++] : View{});
}
using RootFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
using RootHook = DLSSNRMethodHooks::MethodHook<34116, RootFn>;
HRESULT STDMETHODCALLTYPE CreateRoot(ID3D12Device* d, UINT node, const void* blob, SIZE_T size, REFIID iid, void** output)
{
    const auto result = RootHook::Forward(d, node, blob, size, iid, output);
    if (FAILED(result) || !output || !*output) return result;
    auto record = std::make_shared<Root>(); record->known = false;
    using DeserializeFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    const auto library = GetModuleHandleW(L"d3d12.dll");
    const auto deserialize = library ? reinterpret_cast<DeserializeFn>(GetProcAddress(library, "D3D12CreateVersionedRootSignatureDeserializer")) : nullptr;
    ComPtr<ID3D12VersionedRootSignatureDeserializer> decoder;
    if (deserialize && SUCCEEDED(deserialize(blob, size, IID_PPV_ARGS(&decoder))))
    {
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
        if (SUCCEEDED(decoder->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc)))
        {
            record->known = desc->Desc_1_1.NumParameters <= 64;
            record->count = std::min(desc->Desc_1_1.NumParameters, 64u);
            record->direct = (desc->Desc_1_1.Flags & D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) != 0;
            for (UINT p = 0; p < record->count; ++p)
            {
                const auto& parameter = desc->Desc_1_1.pParameters[p]; record->kinds[p] = parameter.ParameterType;
                record->visibility[p] = parameter.ShaderVisibility;
                if (parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
                UINT64 offset = 0;
                for (UINT r = 0; r < parameter.DescriptorTable.NumDescriptorRanges; ++r)
                {
                    const auto& range = parameter.DescriptorTable.pDescriptorRanges[r];
                    if (range.OffsetInDescriptorsFromTableStart != D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
                        offset = range.OffsetInDescriptorsFromTableStart;
                    if (offset > UINT_MAX) { record->known = false; break; }
                    if (range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
                        record->tables[p].push_back({static_cast<UINT>(offset), range.NumDescriptors});
                    offset += range.NumDescriptors;
                }
            }
        }
    }
    ComPtr<ID3D12RootSignature> root;
    if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&root))))
    {
        root = NativeObject(root.Get());
        std::lock_guard lock(mutex);
        if (const auto found = roots.find(root.Get()); found != roots.end())
            if (const auto existing = found->second.lock(); existing && existing->alive.load()) return result;
        if (Attach(root.Get(), record)) roots[root.Get()] = record;
    }
    return result;
}
}

bool Enabled() { return enabled.load(std::memory_order_relaxed); }
bool RecordingEnabled()
{
    return Enabled() && Config::Instance()->DLSSNREnabled.value_or_default() &&
           Config::Instance()->DLSSNRLateHudless.value_or_default();
}
void ObserveBackBuffer(ID3D12Resource* resource)
{
    if (!Enabled() || !resource) return;
    std::lock_guard lock(mutex);
    // Swapchain-owned buffers do not pass through the application's resource
    // creation methods. DXGI owns their separate allocations and lifetime.
    RegisterResource(resource, nullptr, true);
    if (auto identity = IdentityOf(resource))
    {
        // A fence join does not transfer DXGI presentation ownership. Mark
        // the existing identity too: descriptors/snapshots may already hold it.
        if (!identity->originalQueueOnly.exchange(true))
            spdlog::info("{} [DLSSNR_ACCESS_QUEUE] resource=0x{:X} id={} reason=swapchain-backbuffer policy=original-queue", __FUNCTION__,
                     identity->object, identity->id);
    }
}
void Domain(ID3D12GraphicsCommandList* list, const void* domain, const char* label)
{
    if (!enabled) return;
    std::lock_guard lock(mutex);
    auto* recording = Find(list);
    if (!recording || !Inspect(recording->Current())) return;
    if (!domains.contains(domain) && domains.size() >= Limit)
    { Issue(recording->Current(), "domain-limit", uintptr_t(domain)); return; }
    auto& identity = domains[domain];
    if (!identity) { identity = std::make_shared<Identity>(); identity->id = ++nextIdentity; identity->object = uintptr_t(domain); identity->kind = label; }
    Add(recording->Current(), identity, true);
    recording->Current().hasWork = true;
}
void Initialize(ID3D12Device* device)
{
    std::lock_guard lock(mutex);
    if (!requested) requested = Config::Instance()->DLSSNRPipelineAsync.value_or_default();
    if (!*requested) return;
    auto** table = *reinterpret_cast<void***>(device);
    bool ready = true;
    ready &= Creation<14, &ID3D12Device::CreateDescriptorHeap>::Install(device);
    ready &= Creation<27, &ID3D12Device::CreateCommittedResource>::Install(device);
    ready &= Creation<28, &ID3D12Device::CreateHeap>::Install(device);
    ready &= Creation<29, &ID3D12Device::CreatePlacedResource>::Install(device);
    ready &= Creation<30, &ID3D12Device::CreateReservedResource>::Install(device);
    ComPtr<ID3D12Device3> d3;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3))))
    {
        ready &= Creation<48, &ID3D12Device3::OpenExistingHeapFromAddress>::Install(d3.Get());
        ready &= Creation<49, &ID3D12Device3::OpenExistingHeapFromFileMapping>::Install(d3.Get());
    }
    ComPtr<ID3D12Device4> d4;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d4))))
    {
        ready &= Creation<53, &ID3D12Device4::CreateCommittedResource1>::Install(d4.Get());
        ready &= Creation<54, &ID3D12Device4::CreateHeap1>::Install(d4.Get());
        ready &= Creation<55, &ID3D12Device4::CreateReservedResource1>::Install(d4.Get());
    }
    ComPtr<ID3D12Device8> d8;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d8))))
    {
        ready &= Creation<69, &ID3D12Device8::CreateCommittedResource2>::Install(d8.Get());
        ready &= Creation<70, &ID3D12Device8::CreatePlacedResource1>::Install(d8.Get());
        ready &= FeedbackHook::Install((*reinterpret_cast<void***>(d8.Get()))[71], Feedback) == NO_ERROR;
    }
    ComPtr<ID3D12Device10> d10;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d10))))
    {
        ready &= Creation<76, &ID3D12Device10::CreateCommittedResource3>::Install(d10.Get());
        ready &= Creation<77, &ID3D12Device10::CreatePlacedResource2>::Install(d10.Get());
        ready &= Creation<78, &ID3D12Device10::CreateReservedResource2>::Install(d10.Get());
    }
    ComPtr<ID3D12Device13> d13;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d13))))
        ready &= Creation<81, &ID3D12Device13::OpenExistingHeapFromAddress1>::Install(d13.Get());
    ready &= RootHook::Install(table[16], CreateRoot) == NO_ERROR;
    ready &= CbvHook::Install(table[17], Cbv) == NO_ERROR;
    ready &= SrvHook::Install(table[18], Srv) == NO_ERROR;
    ready &= UavHook::Install(table[19], Uav) == NO_ERROR;
    ready &= RtvHook::Install(table[20], Rtv) == NO_ERROR;
    ready &= DsvHook::Install(table[21], Dsv) == NO_ERROR;
    ready &= CopyHook::Install(table[23], Copy) == NO_ERROR;
    ready &= CopySimpleHook::Install(table[24], CopySimple) == NO_ERROR;
    installationFailed |= !ready;
    enabled = ready && !installationFailed;
    spdlog::info("{} [DLSSNR_ASYNC] access tracking ready={} unknown resources/descriptors remain dependencies", __FUNCTION__, enabled.load());
}
void Reset(ID3D12GraphicsCommandList* list)
{
    if (!enabled) return;
    std::lock_guard lock(mutex);
    if (RecordingEnabled())
    {
        auto& r = recordings[list] = {};
        r.prefix.list = r.tail.list = uintptr_t(list);
        r.tail.tail = true;
        if (AuditActive(audit))
        {
            if (++audit->recordings <= 512) r.prefix.audit = r.tail.audit = audit;
            else { ++audit->omitted; ++audit->reasons["recording-limit"]; }
        }
    }
    else recordings.erase(list);
}
void Forget(ID3D12GraphicsCommandList* list)
{ if (enabled) { std::lock_guard lock(mutex); recordings.erase(list); } }
void Cut(ID3D12GraphicsCommandList* list)
{ if (enabled) { std::lock_guard lock(mutex); if (auto* r = Find(list)) r->cut = true; } }
void Unknown(ID3D12GraphicsCommandList* list, const char* reason, UINT64 detail)
{
    std::lock_guard lock(mutex);
    if (auto* r = Find(list)) Issue(r->Current(), reason, detail);
}
Snapshot Capture(ID3D12GraphicsCommandList* list, bool prefix)
{
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex);
    const auto acquired = std::chrono::steady_clock::now();
    auto result = std::make_shared<Footprint>();
    size_t tables = 0;
    if (!RecordingEnabled()) result->incomplete = "nr-recording-disabled";
    else if (auto* r = Find(list))
    {
        const auto& part = prefix ? r->prefix : r->tail;
        tables = part.tables.size();
        // Unknown is an absorbing dependency. Copying its known subset cannot
        // improve admission and used to duplicate large maps on every submit.
        if ((part.incomplete || part.ordersAllResources) && !AuditActive(part.audit))
        {
            result->incomplete = part.incomplete;
            result->ordersAllResources = part.ordersAllResources;
            result->hasWork = part.hasWork;
            result->retainEntryWaits = part.retainEntryWaits;
            result->wildcardAliases = part.wildcardAliases;
            result->globalUavs = part.globalUavs;
        }
        else *result = part;
        result->containsNr = r->nrSeen;
    }
    else result->incomplete = "untracked-recording";
    const auto stats = Resolve(*result);
    ResolveBacking(*result);
    if (AuditActive(result->audit))
    {
        ++result->audit->snapshots;
        spdlog::info("{} [DLSSNR_AUDIT_SNAPSHOT] session={} list=0x{:X} part={} resources={} tables={} scanned={} globalBarrier={} namedAliases={} wildcardAliases={} globalUavs={} splitTransitions={} complete={} firstReason={} hasWork={} originalQueueOnly={}", __FUNCTION__,
            result->audit->id, uintptr_t(list), prefix ? "prefix" : "tail", result->access.size(), tables,
            stats.descriptors, result->ordersAllResources, result->namedAliases, result->wildcardAliases, result->globalUavs,
            result->splitTransitions, !result->incomplete, result->incomplete ? result->incomplete : "none", result->hasWork,
            RequiresOriginalQueue(result));
    }
    if (result->incomplete && !AuditActive(result->audit))
    {
        const auto reason = result->incomplete;
        const auto ordersAll = result->ordersAllResources;
        const auto containsNr = result->containsNr;
        *result = {}; // No resource COM lifetime is owned by footprint metadata.
        result->incomplete = reason;
        result->ordersAllResources = ordersAll;
        result->containsNr = containsNr;
    }
    static UINT64 captures[2] {};
    const auto sample = ++captures[prefix ? 0 : 1];
    lock.unlock();
    if (sample <= 3 || sample % 600 == 0)
    {
        const auto end = std::chrono::steady_clock::now();
        spdlog::info("{} [DLSSNR_ASYNC_CPU] capture={} prefix={} lockUs={} totalUs={} tableRefs={} ranges={} descriptors={} resources={} globalBarrier={} reason={}", __FUNCTION__,
                 sample, prefix,
                 std::chrono::duration_cast<std::chrono::microseconds>(acquired - start).count(),
                 std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(),
                 tables, stats.ranges, stats.descriptors, result->access.size(), result->ordersAllResources,
                 result->incomplete ? result->incomplete : "complete");
    }
    return result;
}
Snapshot Combine(const std::vector<Snapshot>& parts)
{
    std::lock_guard lock(mutex);
    // Incomplete already means conflict with everything. Keep the immutable
    // snapshot instead of merging data that Conflicts() will never inspect.
    auto result = std::make_shared<Footprint>();
    for (const auto& part : parts)
        if (part && AuditActive(part->audit)) { result->audit = part->audit; break; }
    if (!result->audit)
        for (const auto& part : parts)
            if (!part || part->incomplete) return part;
    for (const auto& part : parts)
    {
        if (!part) { if (!result->incomplete) result->incomplete = "missing-footprint"; continue; }
        if (part->incomplete && !result->incomplete) result->incomplete = part->incomplete;
        result->ordersAllResources |= part->ordersAllResources;
        result->hasWork |= part->hasWork;
        result->containsNr |= part->containsNr;
        result->retainEntryWaits |= part->retainEntryWaits;
        for (const auto& resource : part->lifetime) Add(*result, resource, part->access.at(resource->id));
    }
    return result;
}
bool Complete(const Snapshot& footprint) { return footprint && !footprint->incomplete; }
bool HasWork(const Snapshot& footprint) { return footprint && footprint->hasWork; }
bool ContainsNR(const Snapshot& footprint) { return footprint && footprint->containsNr; }
bool OrdersAllResources(const Snapshot& footprint) { return footprint && footprint->ordersAllResources; }
bool RequiresOriginalQueue(const Snapshot& footprint)
{
    // Read identity metadata at admission rather than caching it at recording
    // time, so late GetBuffer observations also constrain existing snapshots.
    return !Complete(footprint) || std::any_of(footprint->lifetime.begin(), footprint->lifetime.end(),
        [](const auto& resource) { return resource->originalQueueOnly.load(); });
}
bool RetainEntryWaits(const Snapshot& footprint) { return !footprint || footprint->retainEntryWaits; }
void TileTrackingReady(bool ready) { std::lock_guard lock(mutex); tileTrackingReady = ready; }
namespace
{
void IncludeBacking(const std::shared_ptr<Identity>& resource, const std::shared_ptr<Identity>& heap,
                    UINT64 offset, UINT64 bytes)
{
    if (!bytes) return;
    if (!heap) { resource->incomplete = "tile-heap-untracked"; return; }
    if (heap->incomplete) { resource->incomplete = heap->incomplete; return; }
    UINT64 end = offset + bytes;
    for (const auto& old : resource->backing)
        if (old->allocation == heap && old->allocationOffset <= offset &&
            old->allocationOffset + old->allocationBytes >= end) return;
    // Coalesce touching ranges without mutating identities already used by a
    // submitted footprint. Unmaps deliberately do not remove historical ranges.
    for (auto it = resource->backing.begin(); it != resource->backing.end();)
    {
        const auto& old = *it;
        if (old->allocation == heap && old->allocationOffset <= end &&
            offset <= old->allocationOffset + old->allocationBytes)
        {
            offset = std::min(offset, old->allocationOffset);
            end = std::max(end, old->allocationOffset + old->allocationBytes);
            it = resource->backing.erase(it);
            // A merged interval can now touch an earlier interval.
            it = resource->backing.begin();
        }
        else ++it;
    }
    if (resource->backing.size() >= 4096)
    { resource->incomplete = "tile-range-history-limit"; return; }
    auto range = std::make_shared<Identity>();
    range->id = ++nextIdentity; range->kind = "tile-backing-range";
    range->object = resource->object; range->allocation = heap;
    range->allocationOffset = offset; range->allocationBytes = end - offset;
    resource->backing.push_back(std::move(range));
}
Snapshot MappingPoint(const std::shared_ptr<Identity>& resource, const char* operation, UINT ranges)
{
    auto result = std::make_shared<Footprint>();
    result->hasWork = true;
    // Changing page tables is a queue operation. Never prune its completion
    // out of a producer fence, even if the subsequent shader uses another VA.
    result->ordersAllResources = true;
    if (!resource || !resource->reserved) result->incomplete = "tile-resource-untracked";
    else { Add(*result, resource, true); ResolveBacking(*result); }
    static std::map<std::pair<std::string, std::string>, UINT64> samples;
    const char* reason = result->incomplete ? result->incomplete : "complete";
    const auto sample = ++samples[{operation, reason}];
    if (sample <= 3 || sample % 3000 == 0)
        spdlog::info("{} [DLSSNR_ASYNC_TILES] operation={} sample={} resource=0x{:X} ranges={} history={} reason={} policy=historical-physical-ranges-and-queue-order", __FUNCTION__,
            operation, sample, resource ? resource->object : 0, ranges, resource ? resource->backing.size() : 0, reason);
    return result;
}
}
Snapshot UpdateTiles(ID3D12Resource* object, UINT regions, const D3D12_TILE_REGION_SIZE* sizes,
                     ID3D12Heap* heapObject, UINT ranges, const D3D12_TILE_RANGE_FLAGS* flags,
                     const UINT* offsets, const UINT* counts)
{
    std::lock_guard lock(mutex);
    const auto resource = IdentityOf(object);
    if (!resource || !resource->reserved) return MappingPoint(resource, "update", ranges);
    // Mapping coordinates affect which virtual tiles are used, but not this
    // conservative union of physical ranges. No unmapped region is guessed.
    if (heapObject && regions)
    {
        const auto heap = IdentityOf(heapObject);
        const UINT64 heapBytes = heapObject->GetDesc().SizeInBytes;
        if (!ranges || ranges > Limit) resource->incomplete = "tile-range-count";
        else for (UINT i = 0; i < ranges; ++i)
        {
            const auto flag = flags ? flags[i] : D3D12_TILE_RANGE_FLAG_NONE;
            if (flag == D3D12_TILE_RANGE_FLAG_NULL || flag == D3D12_TILE_RANGE_FLAG_SKIP) continue;
            if (flag != D3D12_TILE_RANGE_FLAG_NONE && flag != D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE)
            { resource->incomplete = "tile-range-flags"; continue; }
            const UINT64 offset = UINT64(offsets ? offsets[i] : 0) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
            // Omitted counts can cover an entire resource, not just one tile.
            // The remaining heap suffix is a safe bound for that shorthand.
            const UINT64 bytes = flag == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE ?
                (counts && !counts[i] ? 0 : D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES) :
                counts ? UINT64(counts[i]) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES :
                offset <= heapBytes ? heapBytes - offset : 0;
            if (offset > heapBytes || bytes > heapBytes - offset)
            { resource->incomplete = "tile-range-out-of-heap"; continue; }
            IncludeBacking(resource, heap, offset, bytes);
        }
    }
    return MappingPoint(resource, "update", ranges);
}
Snapshot CopyTiles(ID3D12Resource* destination, ID3D12Resource* source)
{
    std::lock_guard lock(mutex);
    const auto dst = IdentityOf(destination), src = IdentityOf(source);
    if (dst && dst->reserved)
    {
        if (!src || !src->reserved) dst->incomplete = "copy-tile-source-untracked";
        else if (src->incomplete) dst->incomplete = src->incomplete;
        else if (dst != src)
            for (const auto& range : src->backing)
                IncludeBacking(dst, range->allocation, range->allocationOffset, range->allocationBytes);
    }
    return MappingPoint(dst, "copy", src ? UINT(src->backing.size()) : 0);
}
void MarkNR(ID3D12GraphicsCommandList* list)
{
    std::lock_guard lock(mutex);
    if (auto* r = Find(list)) r->nrSeen = true;
}
const char* Reason(const Snapshot& footprint) { return footprint ? footprint->incomplete : "missing-footprint"; }
void AuditBoundary()
{
    if (!RecordingEnabled()) return;
    std::lock_guard lock(mutex);
    ++nrBoundaries;
    static bool debugWasEnabled = false;
    const bool debug = Config::Instance()->DLSSNRPipelineDebug.value_or_default();
    const bool manual = debug && !debugWasEnabled;
    debugWasEnabled = debug;
    if (AuditActive(audit) && ++audit->boundaries >= 8) EndAudit("nr-boundary-limit");
    // Automatic windows include warm-up and later frames in one game run.
    // The existing PipelineDebug off/on control can rearm at any scene.
    if (manual || nrBoundaries == 1 || nrBoundaries == 300 || nrBoundaries == 1200 || nrBoundaries == 3600)
    {
        EndAudit("rearmed");
        // Startup allocations must not exhaust all provenance details for
        // the later in-game captures. Observation numbers never repeat.
        missingObjects.clear();
        audit = std::make_shared<Audit>(); audit->id = ++auditSerial;
        spdlog::info("{} [DLSSNR_AUDIT] session={} start={} nrBoundary={} maxNrBoundaries=8 maxRecordings=512 maxSeconds=5 maxExamples=256 mode=observe-all-blockers admission=unchanged", __FUNCTION__,
                 audit->id, manual ? "PipelineDebug" : "automatic", nrBoundaries);
    }
}
UINT64 AuditSession(const Snapshot& footprint)
{
    std::lock_guard lock(mutex);
    return footprint && AuditActive(footprint->audit) ? footprint->audit->id : 0;
}
void AuditConflicts(UINT64 session, const Snapshot& prefix, const Snapshot& prior, ID3D12Fence* fence, UINT64 value)
{
    std::lock_guard lock(mutex);
    if (!session || !AuditActive(audit) || audit->id != session) return;
    UINT64 shared = 0, aliased = 0, emitted = 0, inspected = 0;
    constexpr UINT64 PairLimit = 65536;
    const auto emit = [&](const char* kind, const Identity& a, const Identity& b) {
        if (++emitted > 32) { ++audit->omitted; return; }
        spdlog::info("{} [DLSSNR_AUDIT_CONFLICT] session={} fence=0x{:X} value={} kind={} prefixId={} priorId={} prefixObject=0x{:X} priorObject=0x{:X} resourceKind={} allocation={} prefixOffset={} prefixBytes={} priorOffset={} priorBytes={}", __FUNCTION__,
            session, uintptr_t(fence), value, kind, a.id, b.id, a.object, b.object, a.kind,
            a.allocation ? a.allocation->id : 0, a.allocationOffset, a.allocationBytes, b.allocationOffset, b.allocationBytes);
    };
    if (prefix && prior)
    {
        for (const auto& resource : prefix->lifetime)
        {
            const auto found = prior->access.find(resource->id);
            if (found != prior->access.end() && (prefix->access.at(resource->id) || found->second))
            { ++shared; emit("same-resource", *resource, *resource); }
            if (!resource->allocation) continue;
            const auto aliases = prior->aliases.find(resource->allocation->id);
            if (aliases == prior->aliases.end()) continue;
            for (const auto* other : aliases->second)
            {
                if (++inspected > PairLimit) break;
                if (resource->id == other->id || (!prefix->access.at(resource->id) && !prior->access.at(other->id))) continue;
                if (resource->allocationBytes == UINT64_MAX || other->allocationBytes == UINT64_MAX ||
                    (resource->allocationOffset < other->allocationOffset + other->allocationBytes &&
                     other->allocationOffset < resource->allocationOffset + resource->allocationBytes))
                { ++aliased; emit("overlapping-allocation", *resource, *other); }
            }
            if (inspected > PairLimit) break;
        }
    }
    spdlog::info("{} [DLSSNR_AUDIT_DEPENDENCY] session={} fence=0x{:X} value={} prefixComplete={} priorComplete={} prefixReason={} priorReason={} prefixGlobalBarrier={} priorGlobalBarrier={} knownShared={} knownAlias={} conflictExamplesOmitted={} pairScanTruncated={} missing-coverage-is-still-a-dependency", __FUNCTION__,
        session, uintptr_t(fence), value, Complete(prefix), Complete(prior), Complete(prefix) ? "none" : Reason(prefix),
        Complete(prior) ? "none" : Reason(prior), prefix && prefix->ordersAllResources, prior && prior->ordersAllResources,
        shared, aliased, emitted > 32 ? emitted - 32 : 0, inspected > PairLimit);
}
const char* ConflictKind(const Snapshot& a, const Snapshot& b)
{
    if (!Complete(a)) return "candidate-incomplete";
    if (!Complete(b)) return "predecessor-incomplete";
    if (a->ordersAllResources) return "candidate-global-barrier";
    if (b->ordersAllResources) return "predecessor-global-barrier";
    for (const auto& [id, write] : a->access)
        if (const auto it = b->access.find(id); it != b->access.end() && (write || it->second)) return "same-resource";
    for (const auto& resource : a->lifetime)
    {
        if (!resource->allocation) continue;
        const auto found = b->aliases.find(resource->allocation->id);
        if (found == b->aliases.end()) continue;
        for (const auto* other : found->second)
        {
            if (!a->access.at(resource->id) && !b->access.at(other->id)) continue;
            if (resource->allocationBytes == UINT64_MAX || other->allocationBytes == UINT64_MAX ||
                (resource->allocationOffset < other->allocationOffset + other->allocationBytes &&
                 other->allocationOffset < resource->allocationOffset + resource->allocationBytes)) return "overlapping-allocation";
        }
    }
    return "independent";
}
bool Conflicts(const Snapshot& a, const Snapshot& b)
{ return std::string_view(ConflictKind(a, b)) != "independent"; }
namespace
{
void RecordResource(Footprint& f, ID3D12Resource* resource, bool write)
{
    if (!resource || !Inspect(f)) return;
    const auto identity = IdentityOf(resource);
    if (!identity && AuditActive(f.audit))
        Issue(f, "untracked-direct-resource", uintptr_t(resource), DescribeMissingResource(resource, "command"));
    Add(f, identity, write);
}
void RecordAddress(Footprint& f, UINT64 address, bool write)
{
    if (!address || !Inspect(f)) return;
    const auto identity = AddressOf(address);
    if (!identity) Issue(f, "untracked-gpu-address", address);
    Add(f, identity, write);
}
}
void Resource(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, bool write)
{
    if (!resource) return;
    std::lock_guard lock(mutex);
    if (auto* r = Find(list)) RecordResource(r->Current(), resource, write);
}
void Address(ID3D12GraphicsCommandList* list, UINT64 address, bool write)
{
    if (!address) return;
    std::lock_guard lock(mutex);
    if (auto* r = Find(list)) RecordAddress(r->Current(), address, write);
}
void Barriers(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    std::lock_guard lock(mutex); auto* r = Find(list); if (!r) return;
    if (!Inspect(r->Current())) return;
    for (UINT i = 0; i < count; ++i)
    {
        const auto& b = barriers[i];
        auto& part = r->Current();
        if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE || b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        {
            // Separate barrier kinds: the previous combined rejection label
            // concealed whether the cut or only access classification failed.
            const unsigned kind = b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ? 0 :
                b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING ?
                    (b.Aliasing.pResourceBefore && b.Aliasing.pResourceAfter ? 1 : 2) :
                b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV ? (b.UAV.pResource ? 3 : 4) : 5;
            static UINT64 samples[6] {};
            const auto sample = ++samples[kind];
            if (sample <= 3 || sample % 3000 == 0)
                spdlog::info("{} [DLSSNR_ASYNC] barrier={} kind={} flags={} prefix={} policy={}", __FUNCTION__, sample,
                    static_cast<unsigned>(b.Type), static_cast<unsigned>(b.Flags), !r->cut,
                    kind == 0 || kind == 5 ? "opaque" : kind == 2 || kind == 4 ? "retain-all-dependencies" : "resource-conflicts");
        }
        if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE)
        { ++part.splitTransitions; Issue(part, "split-transition-barrier", UINT(b.Flags)); continue; }
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
        {
            // Activation changes must conflict with either named resource and
            // every overlapping placed allocation, including read-only users.
            // A wildcard endpoint conservatively retains all memory ordering.
            if (!b.Aliasing.pResourceBefore || !b.Aliasing.pResourceAfter)
            { part.ordersAllResources = true; ++part.wildcardAliases; }
            else ++part.namedAliases;
            if (b.Aliasing.pResourceBefore) RecordResource(part, b.Aliasing.pResourceBefore, true);
            if (b.Aliasing.pResourceAfter) RecordResource(part, b.Aliasing.pResourceAfter, true);
        }
        else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
        {
            if (b.UAV.pResource) RecordResource(part, b.UAV.pResource, true);
            else { part.ordersAllResources = true; ++part.globalUavs; }
        }
        else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        {
            if (b.Transition.pResource) RecordResource(part, b.Transition.pResource, true);
            else Issue(part, "null-transition-resource");
        }
        else Issue(part, "unknown-barrier-type", UINT(b.Type));
        if (!Inspect(part)) break;
    }
}
void ClearState(ID3D12GraphicsCommandList* list)
{
    std::lock_guard lock(mutex); if (auto* r = Find(list))
    { r->compute = {}; r->graphics = {}; r->shaderHeap.reset(); r->samplerHeap.reset();
      r->vertices = {}; r->index = 0; r->streamOutput = {}; r->targets.clear(); }
}
void Work(ID3D12GraphicsCommandList* list)
{
    std::lock_guard lock(mutex);
    if (auto* r = Find(list)) r->Current().hasWork = true;
}
void Heaps(ID3D12GraphicsCommandList* list, UINT count, ID3D12DescriptorHeap* const* values)
{
    std::lock_guard lock(mutex); auto* r = Find(list); if (!r) return;
    std::shared_ptr<Heap> shaderHeap, samplerHeap;
    for (UINT i = 0; i < count; ++i)
    {
        const auto it = heaps.find(values[i]);
        auto heap = it == heaps.end() ? nullptr : it->second.lock();
        if (!heap || !heap->alive.load(std::memory_order_acquire)) heap = ReadMetadata<Heap>(values[i]);
        if (!heap || !heap->alive.load(std::memory_order_acquire))
        { Issue(r->Current(), "untracked-descriptor-heap", uintptr_t(values[i])); continue; }
        if (heap->type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) shaderHeap = heap;
        if (heap->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) samplerHeap = heap;
    }
    // Redundant SetDescriptorHeaps keeps root tables valid. The old tracker
    // invented a missing root table on every same-heap rebind.
    if (shaderHeap != r->shaderHeap || samplerHeap != r->samplerHeap)
    {
        r->compute.tables = {}; r->graphics.tables = {};
        r->compute.invalidation = r->graphics.invalidation = "descriptor-heap-change";
    }
    r->shaderHeap = std::move(shaderHeap); r->samplerHeap = std::move(samplerHeap);
}
void RootSignature(ID3D12GraphicsCommandList* list, bool compute, ID3D12RootSignature* signature)
{
    std::lock_guard lock(mutex); auto* r = Find(list); if (!r) return;
    const auto it = roots.find(signature); auto root = it == roots.end() ? nullptr : it->second.lock();
    if (!root || !root->alive.load(std::memory_order_acquire)) root = ReadMetadata<Root>(signature);
    auto& binding = compute ? r->compute : r->graphics;
    if (root != binding.root) { binding = {}; binding.root = root; binding.invalidation = "root-signature-change"; }
    binding.rootObject = uintptr_t(signature);
}
void RootTable(ID3D12GraphicsCommandList* list, bool compute, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle)
{
    std::lock_guard lock(mutex); if (auto* r = Find(list))
    { if (index < 64) (compute ? r->compute : r->graphics).tables[index] = handle.ptr; else Issue(r->Current(), "root-index", index); }
}
void RootAddress(ID3D12GraphicsCommandList* list, bool compute, UINT index, UINT64 address, bool write)
{
    std::lock_guard lock(mutex); if (auto* r = Find(list))
    {
        if (index >= 64) { Issue(r->Current(), "root-index", index); return; }
        auto& b = compute ? r->compute : r->graphics; b.addresses[index] = address; b.writes[index] = write;
    }
}
void Targets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
{
    std::lock_guard lock(mutex); auto* r = Find(list); if (!r) return;
    r->targets.clear();
    auto heap = count && targets ? CpuHeap(targets->ptr) : nullptr;
    for (UINT i = 0; i < count; ++i)
        r->targets.push_back(!targets || (contiguous && !heap) ? View{} : CpuView(contiguous ? targets->ptr + SIZE_T(i) * heap->increment : targets[i].ptr));
    if (depth) r->targets.push_back(CpuView(depth->ptr));
}
void VertexBuffers(ID3D12GraphicsCommandList* list, UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views)
{
    std::lock_guard lock(mutex); auto* r = Find(list); if (!r) return;
    if (start > r->vertices.size() || count > r->vertices.size() - start) { Issue(r->Current(), "vertex-range", start, count); return; }
    for (UINT i = 0; i < count; ++i) r->vertices[start + i] = views ? views[i].BufferLocation : 0;
}
void IndexBuffer(ID3D12GraphicsCommandList* list, const D3D12_INDEX_BUFFER_VIEW* view)
{ std::lock_guard lock(mutex); if (auto* r = Find(list)) r->index = view ? view->BufferLocation : 0; }
void StreamOutput(ID3D12GraphicsCommandList* list, UINT start, UINT count,
                  const D3D12_STREAM_OUTPUT_BUFFER_VIEW* views)
{
    std::lock_guard lock(mutex); auto* r = Find(list); if (!r) return;
    if (start > r->streamOutput.size() || count > r->streamOutput.size() - start)
    { Issue(r->Current(), "stream-output-range", start, count); return; }
    for (UINT i = 0; i < count; ++i)
        r->streamOutput[start + i] = views ? views[i] : D3D12_STREAM_OUTPUT_BUFFER_VIEW{};
}
void QueryRange(ID3D12GraphicsCommandList* list, ID3D12QueryHeap* heap, UINT start, UINT count, bool write)
{
    std::lock_guard lock(mutex); auto* r = Find(list);
    if (!r || !Inspect(r->Current()) || !count) return;
    if (!heap) { Issue(r->Current(), "null-query-heap"); return; }
    const auto native = NativeObject(heap); heap = native.Get();
    auto allocation = IdentityOf(heap);
    if (!allocation)
    {
        // Query heaps are not aliased resource allocations. An object-owned
        // identity is safe even for a heap created before interception.
        allocation = std::make_shared<Identity>(); allocation->id = ++nextIdentity;
        allocation->object = uintptr_t(heap); allocation->kind = "query-heap";
        if (!Attach(heap, allocation)) { Issue(r->Current(), "query-metadata-failed"); return; }
        identities[heap] = allocation;
    }
    auto range = std::make_shared<Identity>(); range->id = ++nextIdentity;
    range->object = uintptr_t(heap); range->kind = "query-range";
    range->allocation = std::move(allocation); range->allocationOffset = start; range->allocationBytes = count;
    Add(r->Current(), range, write);
}
void Descriptor(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE handle, bool write)
{ std::lock_guard lock(mutex); if (auto* r = Find(list); r && Inspect(r->Current())) AddView(r->Current(), CpuView(handle.ptr), write, handle.ptr); }
void Shader(ID3D12GraphicsCommandList* list, bool compute, bool indexed, bool mesh)
{
    std::lock_guard lock(mutex); auto* r = Find(list); if (!r) return;
    auto& f = r->Current(); const auto& b = compute ? r->compute : r->graphics;
    f.hasWork = true; // Also covers the supported ExecuteIndirect workload path.
    if (!Inspect(f)) return;
    if (!b.root || !b.root->known) Issue(f, b.root ? b.root->reason : "untracked-root-signature", b.rootObject);
    if (b.root && b.root->known && b.root->direct)
    {
        if (!r->shaderHeap) Issue(f, "untracked-direct-heap");
        else if (f.tables.size() < Limit) f.tables.push_back({r->shaderHeap, 0, r->shaderHeap->count});
        else Issue(f, "table-limit");
    }
    for (UINT i = 0; b.root && b.root->known && i < b.root->count; ++i)
    {
        const auto visibility = b.root->visibility[i];
        if (compute && visibility != D3D12_SHADER_VISIBILITY_ALL) continue;
        if (!compute && mesh && (visibility == D3D12_SHADER_VISIBILITY_VERTEX ||
            visibility == D3D12_SHADER_VISIBILITY_HULL || visibility == D3D12_SHADER_VISIBILITY_DOMAIN ||
            visibility == D3D12_SHADER_VISIBILITY_GEOMETRY)) continue;
        if (!compute && !mesh && (visibility == D3D12_SHADER_VISIBILITY_MESH ||
            visibility == D3D12_SHADER_VISIBILITY_AMPLIFICATION)) continue;
        const auto kind = b.root->kinds[i];
        if (kind == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) continue;
        if (kind != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        { if (b.addresses[i]) RecordAddress(f, b.addresses[i], b.writes[i]); continue; }
        for (const auto& range : b.root->tables[i])
        {
            const auto& heap = r->shaderHeap;
            if (!heap || !heap->gpu || b.tables[i] < heap->gpu || (b.tables[i] - heap->gpu) % heap->increment)
            {
                Issue(f, "untracked-root-table", b.tables[i], i);
                if (AuditActive(f.audit) && f.audit->rootExamples.size() < 64 &&
                    f.audit->rootExamples.emplace(uintptr_t(list), compute, i).second)
                    spdlog::info("{} [DLSSNR_AUDIT_ROOT] session={} list=0x{:X} compute={} mesh={} parameter={} visibility={} root=0x{:X} table=0x{:X} heap=0x{:X} heapGpu=0x{:X} rangeOffset={} rangeCount={} lastInvalidation={}", __FUNCTION__,
                        f.audit->id, uintptr_t(list), compute, mesh, i, UINT(visibility), b.rootObject,
                        b.tables[i], heap ? heap->object : 0, heap ? heap->gpu : 0, range.offset, range.count, b.invalidation);
                continue;
            }
            const UINT64 offset = (b.tables[i] - heap->gpu) / heap->increment + range.offset;
            const UINT64 count = range.count == UINT_MAX && offset <= heap->count ? heap->count - offset : range.count;
            if (offset > heap->count || count > heap->count - offset)
            { Issue(f, "invalid-table-range", offset, count); continue; }
            if (f.tables.size() == Limit) { Issue(f, "table-limit"); continue; }
            f.tables.push_back({heap, static_cast<UINT>(offset), static_cast<UINT>(count)});
        }
    }
    if (!compute)
    {
        if (!mesh)
        {
            for (auto address : r->vertices) if (address) RecordAddress(f, address, false);
            if (indexed && r->index) RecordAddress(f, r->index, false);
            for (const auto& output : r->streamOutput)
                if (output.BufferLocation)
                {
                    RecordAddress(f, output.BufferLocation, true);
                    RecordAddress(f, output.BufferFilledSizeLocation, true);
                }
        }
        for (const auto& target : r->targets) AddView(f, target, true);
    }
}
}
