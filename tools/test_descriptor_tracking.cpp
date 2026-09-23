#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include <ankerl/unordered_dense.h>
#define LOG_TRACK(...) ((void)0)
#include "descriptor-heap-production.h"
static std::vector<std::unique_ptr<HeapInfo>> fgHeaps;
static thread_local unsigned heapScans = 0;
#include "descriptor-lookup-production.h"

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
uint64_t Tick() { LARGE_INTEGER value; QueryPerformanceCounter(&value); return value.QuadPart; }
double Milliseconds(uint64_t ticks)
{
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    return ticks * 1000.0 / frequency.QuadPart;
}
void ClearTracking()
{
    for (auto& shard : _resourceReferenceShards) { shard.resources.clear(); shard.lifetimes.clear(); }
}
size_t References(ID3D12Resource* resource)
{
    auto& map = ResourceReferences(resource).resources;
    auto it = map.find(resource);
    return it == map.end() ? 0 : it->second.size();
}
void Validate(const std::vector<HeapInfo*>& heaps)
{
    size_t actual = 0, expected = 0;
    for (auto& shard : _resourceReferenceShards)
        for (auto& [resource, refs] : shard.resources)
        {
            Require(!refs.empty() && resource != nullptr, "No empty or null resource entries");
            for (size_t i = 0; i < refs.size(); ++i)
            {
                Require(refs[i].info->buffer == resource, "Reverse entry resource matches slot");
                Require(*refs[i].position == i, "Moved slot position repaired");
                ++actual;
            }
        }
    for (auto* heap : heaps)
        for (UINT i = 0; i < heap->numDescriptors; ++i)
        {
            auto* resource = heap->info[i].buffer;
            auto index = heap->referencePositions[i];
            if (!resource || !heap->GetByIndex(i))
                Require(index == INVALID_RESOURCE_REFERENCE, "Empty or expired slot detached");
            else if (index != INVALID_RESOURCE_REFERENCE)
            {
                auto& refs = ResourceReferences(resource).resources.at(resource);
                Require(index < refs.size() && refs[index].info == &heap->info[i], "Forward slot membership");
                Require(refs[index].position == &heap->referencePositions[i], "Position storage belongs to slot");
                ++expected;
            }
        }
    Require(actual == expected, "No missing or duplicate reverse references");
}
void TestHeapLookup()
{
    using Role = ResTrack_Dx12::DescriptorCopyRole;
    auto lookup = ResTrack_Dx12::GetHeapByCpuHandle;
    constexpr UINT stride = 32, count = 64;
    for (UINT i = 0; i < 38; ++i)
    {
        const SIZE_T start = 0x100000ull + 0x10000ull*i;
        fgHeaps.push_back(std::make_unique<HeapInfo>(nullptr, start, start+stride*count, 0, 0, count, stride, 0));
    }
    gHeapGeneration.fetch_add(1);
    for (const auto& heap : fgHeaps)
        for (auto role : { Role::Source, Role::Destination })
        {
            Require(lookup(heap->cpuStart, role) == heap.get(), "First descriptor lookup");
            Require(lookup(heap->cpuEnd-stride, role) == heap.get(), "Last descriptor lookup");
            Require(lookup(heap->cpuStart-1, role) == nullptr, "Lower boundary excludes preceding gap");
            Require(lookup(heap->cpuEnd, role) == nullptr, "Upper boundary is exclusive");
        }
    auto* source = fgHeaps[18].get();
    auto* destination = fgHeaps.back().get();
    gHeapGeneration.fetch_add(1);
    heapScans = 0;
    for (UINT i = 0; i < 1000; ++i)
    {
        const auto offset = (i % count)*stride;
        Require(lookup(source->cpuStart+offset, Role::Source) == source, "Repeated source copy lookup");
        Require(lookup(destination->cpuStart+offset, Role::Destination) == destination, "Repeated destination copy lookup");
    }
    Require(heapScans == 2, "Source and destination copies must not evict each other's cache");
    gHeapGeneration.fetch_add(1);
    heapScans = 0;
    for (UINT i = 0; i < 1000; ++i)
        for (UINT slot = 0; slot < 4; ++slot)
        {
            auto* heap = fgHeaps[18 + slot].get();
            Require(lookup(heap->cpuStart + (i % count)*stride, Role::Source) == heap,
                    "Alternating source heap lookup");
            Require(lookup(heap->cpuStart + (i % count)*stride, Role::Destination) == heap,
                    "Alternating destination heap lookup");
        }
    Require(heapScans == 8, "Four alternating heaps per role need one scan each");
    for (auto role : { Role::Source, Role::Destination })
    {
        lookup(source->cpuStart, role);
        source->version.fetch_add(1);
        heapScans = 0;
        Require(lookup(source->cpuStart, role) == source && heapScans == 1, "Version change invalidates cached slot");
        source->active = false;
        Require(lookup(source->cpuStart, role) == nullptr, "Retired heap cannot serve a cached lookup");
        source->active = true;
    }

    std::barrier phase(5);
    std::atomic<unsigned> failures { 0 };
    std::vector<std::thread> workers;
    for (UINT t = 0; t < 4; ++t) workers.emplace_back([&, t] {
        for (UINT pass = 0; pass < 2; ++pass)
        {
            phase.arrive_and_wait();
            auto* src = fgHeaps[2*t].get();
            auto* dst = fgHeaps[2*t+1].get();
            heapScans = 0;
            for (UINT i = 0; i < 10000; ++i)
            {
                const auto offset = (i % count)*stride;
                if (lookup(src->cpuStart+offset, Role::Source) != src ||
                    lookup(dst->cpuStart+offset, Role::Destination) != dst) ++failures;
            }
            if (heapScans != 2) ++failures;
            phase.arrive_and_wait();
        }
    });
    phase.arrive_and_wait();
    phase.arrive_and_wait();
    // Replace storage at the same CPU handle ranges between copy batches, as
    // during swapchain/HDR heap recreation. Workers retain their old TLS entries.
    for (UINT i = 0; i < 8; ++i)
    {
        const auto start = fgHeaps[i]->cpuStart;
        auto replacement = std::make_unique<HeapInfo>(nullptr, start, start+stride*count, 0, 0, count, stride, 0);
        fgHeaps[i] = std::move(replacement);
    }
    gHeapGeneration.fetch_add(1, std::memory_order_release);
    phase.arrive_and_wait();
    phase.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    Require(failures == 0, "Thread-local caches must survive heap retirement and same-range reuse");
    const auto removedStart = source->cpuStart;
    fgHeaps.clear();
    gHeapGeneration.fetch_add(1);
    Require(lookup(removedStart, Role::Source) == nullptr, "Removed heap lookup");
    std::cout << "Heap cache: 2000 alternating lookups need 2 scans; boundaries, retirement, version, and 4-thread reuse passed.\n";
}
void TestMembership()
{
    HeapInfo a(nullptr, 32, 32*1025, 65536, 65536+32*1024, 1024, 32, 0);
    HeapInfo b(nullptr, 131072, 131072+32*1024, 262144, 262144+32*1024, 1024, 32, 0);
    std::vector<HeapInfo*> heaps { &a, &b };
    ResourceInfo resources[64] {};
    for (UINT i = 0; i < 64; ++i)
    {
        resources[i].buffer = reinterpret_cast<ID3D12Resource*>(0x100000ull + 65536ull*i);
        resources[i].width = 1920 + i;
    }
    std::mt19937 rng(12345);
    for (UINT step = 0; step < 100000; ++step)
    {
        auto* heap = heaps[rng()%2];
        const UINT index = rng()%1024;
        const auto cpu = heap->cpuStart + 32ull*index;
        const auto gpu = heap->gpuStart + 32ull*index;
        switch (rng()%7)
        {
        case 0: heap->SetByCpuHandle(cpu, resources[rng()%64]); break;
        case 1: heap->SetByGpuHandle(gpu, resources[rng()%64]); break;
        case 2: heap->ClearByCpuHandle(cpu); break;
        case 3: heap->ClearByGpuHandle(gpu); break;
        case 4: heap->SetByCpuHandle(cpu, {}); break;
        case 5: // Copy views across heaps without reverse-table updates.
            heap->CopyByIndex(index, heaps[rng()%2], rng()%1024); break;
        case 6:
            if (auto* view = heap->GetByCpuHandle(cpu))
            {
                auto changed = *view;
                changed.width++;
                const auto position = heap->referencePositions[index];
                heap->SetByGpuHandle(gpu, changed);
                Require(heap->info[index].width == changed.width &&
                        (heap->referencePositions[index] == position ||
                         (position == INVALID_RESOURCE_REFERENCE &&
                          heap->referencePositions[index] != INVALID_RESOURCE_REFERENCE)),
                        "Direct view update keeps or creates reverse membership");
            }
            break;
        }
        if (step % 1000 == 0)
        {
            auto* resource = resources[rng()%64].buffer;
            auto& shard = ResourceReferences(resource);
            { std::scoped_lock lock(shard.mutex); shard.Invalidate(resource); }
            Require(References(resource) == 0, "Destroyed resource removed from every heap");
            Validate(heaps);
        }
    }
    Validate(heaps);
    for (auto* heap : heaps)
        for (UINT i = 0; i < heap->numDescriptors; ++i)
            heap->ClearByCpuHandle(heap->cpuStart + 32ull*i);
    Validate(heaps);
    for (auto& shard : _resourceReferenceShards) Require(shard.resources.empty(), "Heap teardown removes all references");
    // Reuse identical descriptor storage and opaque resource addresses after teardown.
    for (UINT i = 0; i < 1024; ++i) a.SetByCpuHandle(a.cpuStart + 32ull*i, resources[i%64]);
    Validate(heaps);
    for (auto& resource : resources)
    {
        auto& shard = ResourceReferences(resource.buffer);
        std::scoped_lock lock(shard.mutex);
        shard.Invalidate(resource.buffer);
    }
    Validate(heaps);
    std::cout << "Reverse index: 100000 randomized operations, cross-heap copies, release/heap teardown/reuse passed.\n";
}
void TestIndexPaths()
{
    for (const UINT stride : {16u, 32u, 40u})
    {
        constexpr UINT count = 64;
        constexpr SIZE_T cpu = 0x10000, gpu = 0x20000;
        HeapInfo source(nullptr, cpu, cpu+stride*count, gpu, gpu+stride*count, count, stride, 0);
        HeapInfo destination(nullptr, cpu+0x10000, cpu+0x10000+stride*count, 0, 0, count, stride, 0);
        ResourceInfo view {};
        view.buffer = reinterpret_cast<ID3D12Resource*>(0x40000);
        view.width = 1920;
        for (UINT i = 0; i < count; ++i)
        {
            UINT index = UINT_MAX;
            Require(source.GetCpuIndex(cpu+stride*i,index) && index==i, "Exact CPU handle index");
            if (i%3 != 0) source.SetByIndex(i,view);
            Require(source.GetByIndex(i)==source.GetByCpuHandle(cpu+stride*i) &&
                    source.GetByIndex(i)==source.GetByGpuHandle(gpu+stride*i), "Indexed lookup matches handle paths");
            destination.CopyByIndex(i, &source, i);
            Require(destination.referencePositions[i] == INVALID_RESOURCE_REFERENCE,
                    "Copied descriptor has no reverse membership");
        }
        Validate({&source,&destination});
        const auto position = destination.referencePositions[1];
        view.width = 2560;
        destination.SetByIndex(1,view);
        Require(destination.GetByIndex(1)->width==2560 &&
                position==INVALID_RESOURCE_REFERENCE &&
                destination.referencePositions[1]!=INVALID_RESOURCE_REFERENCE,
                "Direct overwrite of a copy restores reverse membership");
        UINT index = 123;
        for (const SIZE_T invalid : {cpu-1, cpu+1, cpu+stride*count, SIZE_MAX})
            Require(!source.GetCpuIndex(invalid,index), "Invalid or unaligned handle rejected");
        const auto references = References(view.buffer);
        destination.SetByIndex(count,view); destination.SetByIndex(SIZE_MAX,view);
        destination.ClearByIndex(count); destination.ClearByIndex(SIZE_MAX);
        Require(!destination.GetByIndex(count) && !destination.GetByIndex(SIZE_MAX) &&
                References(view.buffer)==references, "Out-of-range index cannot change metadata");
        {
            auto& shard = ResourceReferences(view.buffer);
            std::scoped_lock lock(shard.mutex);
            Require(shard.Invalidate(view.buffer), "Resource release invalidates lifetime");
        }
        Require(source.GetByIndex(1)==nullptr && destination.GetByIndex(2)==nullptr,
                "Resource release invalidates copied descriptors without reverse entries");
        source.SetByIndex(1,view);
        Require(source.GetByIndex(1)!=nullptr && destination.GetByIndex(2)==nullptr,
                "Reused resource address does not revive an old copied descriptor");
        source.active = false;
        Require(!source.GetCpuIndex(cpu,index), "Retired heap cannot admit a range");
        // Heap retirement must still remove the reverse references after marking inactive.
        for (UINT i=0; i<count; ++i) { source.ClearByIndex(i); destination.ClearByIndex(i); }
        Validate({&source,&destination});
        Require(References(view.buffer)==0, "Indexed teardown detaches every descriptor");
    }
    std::cout << "Descriptor indices: aligned/non-power-of-two strides, bounds, copies, overwrite and retirement passed.\n";
}
void TestConcurrentRewrites()
{
    HeapInfo heap(nullptr, 32, 32ull*32769, 0, 0, 32768, 32, 0);
    ResourceInfo resources[64] {};
    for (UINT i = 0; i < 64; ++i)
        resources[i].buffer = reinterpret_cast<ID3D12Resource*>(0x200000ull + 65536ull*i);
    for (UINT resourceCount : { 2u, 64u })
    {
        std::barrier go(5);
        std::vector<std::thread> workers;
        for (UINT t = 0; t < 4; ++t) workers.emplace_back([&, t] {
            std::mt19937 rng(42+t);
            go.arrive_and_wait();
            for (UINT i = 0; i < 250000; ++i)
            {
                const UINT slot = (rng()%8192)*4+t; // disjoint slots, shared resource vectors
                if (i % 11 == 0) heap.ClearByCpuHandle(32ull*(slot+1));
                else heap.SetByCpuHandle(32ull*(slot+1), resources[rng()%resourceCount]);
            }
        });
        const auto start = Tick();
        go.arrive_and_wait();
        for (auto& worker : workers) worker.join();
        const auto elapsed = Tick()-start;
        Validate({ &heap });
        std::cout << "Concurrent stress: 1000000 operations, resources=" << resourceCount
                  << " elapsed_ms=" << Milliseconds(elapsed) << " membership passed.\n";
    }
    for (UINT i = 0; i < 32768; ++i) heap.ClearByCpuHandle(32ull*(i+1));
    Validate({ &heap });
}
int main()
{
    TestHeapLookup();
    TestMembership();
    TestIndexPaths();
    TestConcurrentRewrites();
    // No GPU work or fake COM calls: these are opaque identities used solely by
    // the unmodified production HeapInfo reverse-reference bookkeeping.
    ResourceInfo a {}, b {};
    a.buffer = reinterpret_cast<ID3D12Resource*>(0x10000);
    b.buffer = reinterpret_cast<ID3D12Resource*>(0x20000);
    std::cout << "Synthetic production HeapInfo benchmark, not a game FPS measurement.\n";
    for (UINT count : {1024u, 4096u, 16384u, 32768u})
    {
        ClearTracking();
        HeapInfo heap(nullptr, 32, 32ull*(count+1), 0, 0, count, 32, 0);
        auto start = Tick();
        for (UINT i = 0; i < count; ++i) heap.SetByCpuHandle(32ull*(i+1), a);
        auto populate = Tick() - start;
        Require(References(a.buffer) == count, "Population membership");
        start = Tick();
        for (UINT i = 0; i < count; ++i) heap.SetByCpuHandle(32ull*(i+1), b);
        auto rewrite = Tick() - start;
        Require(References(a.buffer) == 0 && References(b.buffer) == count, "Rewrite membership");
        start = Tick();
        std::barrier go(5);
        std::vector<std::thread> workers;
        for (UINT t = 0; t < 4; ++t) workers.emplace_back([&, t] {
            go.arrive_and_wait();
            for (UINT i = t; i < count; i += 4) heap.SetByCpuHandle(32ull*(i+1), a);
        });
        go.arrive_and_wait();
        for (auto& worker : workers) worker.join();
        auto parallel = Tick() - start;
        Require(References(b.buffer) == 0 && References(a.buffer) == count, "Parallel membership");
        Validate({ &heap });
        std::cout << "slots=" << count << " populate_ms=" << Milliseconds(populate)
                  << " rewrite_ms=" << Milliseconds(rewrite) << " four_threads_ms=" << Milliseconds(parallel) << '\n';
        ClearTracking();
    }
}
