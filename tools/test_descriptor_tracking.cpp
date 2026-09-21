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
    for (auto& shard : _resourceReferenceShards) shard.resources.clear();
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
            if (!resource) Require(index == INVALID_RESOURCE_REFERENCE, "Cleared slot detached");
            else
            {
                auto& refs = ResourceReferences(resource).resources.at(resource);
                Require(index < refs.size() && refs[index].info == &heap->info[i], "Forward slot membership");
                Require(refs[index].position == &heap->referencePositions[i], "Position storage belongs to slot");
                ++expected;
            }
        }
    Require(actual == expected, "No missing or duplicate reverse references");
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
        case 5: // Copy views across heaps; reverse positions must not be copied.
            heap->SetByCpuHandle(cpu, heaps[rng()%2]->info[rng()%1024]); break;
        case 6:
            if (auto* view = heap->GetByCpuHandle(cpu))
            {
                auto changed = *view;
                changed.width++;
                const auto position = heap->referencePositions[index];
                heap->SetByGpuHandle(gpu, changed);
                Require(heap->info[index].width == changed.width &&
                        heap->referencePositions[index] == position, "Same resource updates metadata only");
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
    TestMembership();
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
