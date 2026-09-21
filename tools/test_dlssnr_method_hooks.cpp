#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include "DLSSNRMethodHooks.h"
#include "DLSSNRCommandState.h"
#include "DLSSNRPipelineCapture.h"

unsigned checks = 0;
void Require(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
struct Object { unsigned runtime = 0, driver = 0, callbacks = 0; };
using Fn = void(STDMETHODCALLTYPE*)(Object*, UINT);
using Hook = DLSSNRMethodHooks::MethodHook<12, Fn>;
using Observer = DLSSNRMethodHooks::MethodHook<112, Fn>;
__declspec(noinline) void Runtime(Object* object, UINT value) { object->runtime += value; }
__declspec(noinline) void Driver(Object* object, UINT value)
{
    object->driver += value;
    Runtime(object, value);
}
__declspec(noinline) void OtherDriver(Object* object, UINT value) { object->driver += value * 3; }
void Callback(Object* object, UINT value)
{
    ++object->callbacks;
    Hook::Forward(object, value);
}
void Observe(Object* object, UINT value) { Observer::Forward(object, value); }

using ResultFn = HRESULT(STDMETHODCALLTYPE*)(Object*, UINT);
using ResultHook = DLSSNRMethodHooks::MethodHook<1014, ResultFn>;
__declspec(noinline) HRESULT FenceRuntime(Object* object, UINT value)
{ object->runtime += value; return value == 7 ? E_FAIL : S_OK; }
__declspec(noinline) HRESULT FenceDriver(Object* object, UINT value)
{ object->driver += value; return FenceRuntime(object, value); }
HRESULT FenceCallback(Object* object, UINT value)
{ ++object->callbacks; return ResultHook::Forward(object, value); }

void PipelineChecks()
{
    Require(ResultHook::Install(reinterpret_cast<void*>(FenceRuntime), FenceCallback) == NO_ERROR, "fence runtime hook");
    Require(ResultHook::Install(reinterpret_cast<void*>(FenceDriver), FenceCallback) == NO_ERROR, "fence driver hook");
    Object object;
    Require(FenceDriver(&object, 7) == E_FAIL, "fence failure result must survive nested forwarding");
    Require(object.callbacks == 1 && object.runtime == 7 && object.driver == 7, "fence observation duplicated call");
    Require(FenceRuntime(&object, 2) == S_OK && object.callbacks == 2, "fence success forwarding");
    Require(ResultHook::Remove() == NO_ERROR, "fence hook removal");
    using namespace DLSSNRPipelineTrace;
    Capture capture;
    capture.Add({"disabled"});
    Require(capture.events.empty(), "disabled capture recorded event");
    capture.Start();
    for (size_t i = 0; i < Capture::Capacity + 5; ++i) capture.Add({"event"});
    Require(capture.events.size() == Capture::Capacity && capture.lost == 5, "capture must bound storage and report overflow");
    for (unsigned i = 1; i < Capture::PresentLimit; ++i)
        Require(!capture.EndPresent(), "capture ended before present limit");
    Require(capture.EndPresent() && capture.active, "present limit must retain tail recording");
    Require(!capture.EndPresent(), "drain notification must occur only once");
    capture.Start();
    for (unsigned i = 0; i < Capture::PresentLimit; ++i) capture.EndPresent();
    capture.Add({"post-present-signal"});
    Require(capture.events.size() == 1 && capture.events[0].present == Capture::PresentLimit,
            "final Present truncated tail signal");
    capture.active = false;
    const auto last = capture.ordinal;
    capture.Add({"stopped"});
    Require(capture.ordinal == last, "stopped capture accepted event");
    capture.Start();
    capture.Add({"rearmed"});
    Require(capture.lost == 0 && capture.presents == 0 && capture.events.size() == 1 &&
            capture.events[0].ordinal == 1, "rearmed capture retained prior session");
}

// Execute the production state-hook installers/callbacks with the host services
// stubbed. The fake lists provide distinct runtime/driver method implementations.
struct Setting { bool value = true; bool value_or_default() const { return value; } };
struct Config
{
    Setting DLSSNRLateHudless;
    static Config* Instance() { static Config config; return &config; }
};
namespace DLSSNRLatePass {
    bool enabled = true;
    bool Enabled() { return enabled; }
    bool TrackDescriptors() { return true; }
}
static thread_local bool isUpscalerActive = false;
struct D3D12Hooks
{
    static bool TrackLateCommandState(ID3D12GraphicsCommandList*);
    static void ReleaseLateCommandStateHooks();
};
template<class T> struct rewrite_signature;
template<class R, class C, class... A> struct rewrite_signature<R(C::*)(A...)>
{ using type = R(STDMETHODCALLTYPE*)(C*, A...); };
#define LOG_ERROR(...) throw std::runtime_error("state hook detach failed")
#define DLSSNR_DIAG(...) ((void)0)
#include "late-state-hooks-production.h"
#undef LOG_ERROR
#undef DLSSNR_DIAG

struct FakeList
{
    void** table;
    unsigned calls = 0;
    ID3D12GraphicsCommandList* Get() { return reinterpret_cast<ID3D12GraphicsCommandList*>(this); }
};
template<unsigned Slot, class F, unsigned Variant> struct FakeMethod;
template<unsigned Slot, class... A, unsigned Variant>
struct FakeMethod<Slot, void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, A...), Variant>
{
    __declspec(noinline) static void STDMETHODCALLTYPE Invoke(ID3D12GraphicsCommandList* list, A...)
    { reinterpret_cast<FakeList*>(list)->calls += Slot + Variant; }
};
template<unsigned Variant> void FillTable(std::array<void*, 43>& table)
{
#define METHOD(Slot, Name) table[Slot] = reinterpret_cast<void*>(&FakeMethod<Slot, PFN_##Name, Variant>::Invoke)
    METHOD(25, SetPipelineState); METHOD(28, SetDescriptorHeaps);
    METHOD(29, SetComputeRootSignature); METHOD(30, SetGraphicsRootSignature);
    METHOD(31, SetComputeRootDescriptorTable); METHOD(32, SetGraphicsRootDescriptorTable);
    METHOD(33, SetComputeRoot32BitConstant); METHOD(34, SetGraphicsRoot32BitConstant);
    METHOD(35, SetComputeRoot32BitConstants); METHOD(36, SetGraphicsRoot32BitConstants);
    METHOD(37, SetComputeRootConstantBufferView); METHOD(38, SetGraphicsRootConstantBufferView);
    METHOD(39, SetComputeRootShaderResourceView); METHOD(40, SetGraphicsRootShaderResourceView);
    METHOD(41, SetComputeRootUnorderedAccessView); METHOD(42, SetGraphicsRootUnorderedAccessView);
#undef METHOD
}
void StateChecks()
{
    DLSSNRCommandState::CaptureEnabled(true);
    std::array<void*, 43> runtime {}, driver {};
    FillTable<0>(runtime); FillTable<100>(driver);
    FakeList lists[] {{runtime.data()}, {driver.data()}};
    for (auto& list : lists)
    {
        auto* cmd = list.Get();
        Require(D3D12Hooks::TrackLateCommandState(cmd), "state entry installation");
        Require(!DLSSNRCommandState::Snapshot(cmd), "incomplete recording must be rejected");
        DLSSNRCommandState::Reset(cmd, nullptr, true);
        auto* pipeline = reinterpret_cast<ID3D12PipelineState*>(0x1000);
        auto* signature = reinterpret_cast<ID3D12RootSignature*>(0x2000);
        auto* heap = reinterpret_cast<ID3D12DescriptorHeap*>(0x3000);
        cmd->SetPipelineState(pipeline); cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(signature); cmd->SetGraphicsRootSignature(signature);
        cmd->SetComputeRootDescriptorTable(0, {0x4000}); cmd->SetGraphicsRootDescriptorTable(0, {0x5000});
        UINT constants[] {1,2,3,4};
        cmd->SetComputeRoot32BitConstants(1, 4, constants, 0);
        cmd->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
        cmd->SetComputeRoot32BitConstant(1, 99, 2); cmd->SetGraphicsRoot32BitConstant(1, 88, 3);
        cmd->SetComputeRootConstantBufferView(2, 0x6000); cmd->SetGraphicsRootConstantBufferView(2, 0x7000);
        cmd->SetComputeRootShaderResourceView(3, 0x8000); cmd->SetGraphicsRootShaderResourceView(3, 0x9000);
        cmd->SetComputeRootUnorderedAccessView(4, 0xA000); cmd->SetGraphicsRootUnorderedAccessView(4, 0xB000);
        auto state = DLSSNRCommandState::Snapshot(cmd);
        Require(state.has_value(), "state missing after actual method calls");
        Require(state->pipeline == pipeline && state->heapCount == 1 && state->heaps[0] == heap, "pipeline/heaps lost");
        Require(state->compute.signature == signature && state->graphics.signature == signature, "signatures lost");
        Require(state->compute.roots[0].value == 0x4000 && state->graphics.roots[0].value == 0x5000, "tables lost");
        Require(state->compute.roots[1].constants.size() == 4 && state->compute.roots[1].constants[2] == 99 &&
                state->graphics.roots[1].constants[3] == 88, "partial constants lost");
        Require(state->compute.roots[2].value == 0x6000 && state->graphics.roots[2].value == 0x7000, "CBVs lost");
        Require(state->compute.roots[3].value == 0x8000 && state->graphics.roots[3].value == 0x9000, "SRVs lost");
        Require(state->compute.roots[4].value == 0xA000 && state->graphics.roots[4].value == 0xB000, "UAVs lost");
        Require(list.calls == (list.table == runtime.data() ? 550u : 2150u), "state forwarding used wrong entry");
        {
            DLSSNRCommandState::Suppress guard;
            cmd->SetGraphicsRootDescriptorTable(0, {0xDEAD});
        }
        isUpscalerActive = true;
        cmd->SetComputeRootDescriptorTable(0, {0xDEAD});
        isUpscalerActive = false;
        state = DLSSNRCommandState::Snapshot(cmd);
        Require(state->graphics.roots[0].value == 0x5000 && state->compute.roots[0].value == 0x4000,
                "injected/upscaler state contaminated snapshot");
        state->Restore(cmd);
        Require(DLSSNRCommandState::Snapshot(cmd)->graphics.roots[0].value == 0x5000, "restore contaminated snapshot");
        DLSSNRLatePass::enabled = false;
        cmd->SetGraphicsRootDescriptorTable(0, {0xDEAD});
        Require(DLSSNRCommandState::states.at(cmd).graphics.roots[0].value == 0x5000,
                "disabled model must not record root bindings");
        DLSSNRLatePass::enabled = true;
        cmd->SetGraphicsRootDescriptorTable(0, {0x5000});
        Require(!DLSSNRCommandState::Snapshot(cmd), "capture toggle must reject incomplete recording");
        DLSSNRCommandState::Reset(cmd);
    }
    D3D12Hooks::ReleaseLateCommandStateHooks();
}
// Execute the production Reset/render-pass/creation callbacks with distinct
// fake runtime and driver entries, including a driver forwarding to runtime.
struct ResTrack_Dx12 { static bool ObserveCommandList(ID3D12GraphicsCommandList*); };
namespace DLSSNRPipelineTrace { void ResetList(ID3D12GraphicsCommandList*) {} }
struct { std::atomic<unsigned> renderPasses {0}; } lateStats;
using PFN_Close = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PFN_ExecuteBundle = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
#include "late-lifecycle-hooks-production.h"
bool ResTrack_Dx12::ObserveCommandList(ID3D12GraphicsCommandList* list)
{
    auto table = *reinterpret_cast<void***>(list);
    return CaptureReset::Install(table[10], hkLateReset) == NO_ERROR;
}
struct LifecycleList
{
    void** table;
    unsigned refs = 1, queries = 0, resets = 0, driverResets = 0;
    ID3D12GraphicsCommandList* Get() { return reinterpret_cast<ID3D12GraphicsCommandList*>(this); }
    static HRESULT STDMETHODCALLTYPE Query(IUnknown* object, REFIID, void** output)
    {
        auto& list = *reinterpret_cast<LifecycleList*>(object);
        ++list.refs; ++list.queries; *output = object; return S_OK;
    }
    static ULONG STDMETHODCALLTYPE Release(IUnknown* object)
    { return --reinterpret_cast<LifecycleList*>(object)->refs; }
    __declspec(noinline) static HRESULT STDMETHODCALLTYPE RuntimeReset(ID3D12GraphicsCommandList* object,
        ID3D12CommandAllocator* allocator, ID3D12PipelineState*)
    {
        ++reinterpret_cast<LifecycleList*>(object)->resets;
        return allocator ? S_OK : E_FAIL;
    }
    __declspec(noinline) static HRESULT STDMETHODCALLTYPE DriverReset(ID3D12GraphicsCommandList* object,
        ID3D12CommandAllocator* allocator, ID3D12PipelineState* pipeline)
    {
        ++reinterpret_cast<LifecycleList*>(object)->driverResets;
        return RuntimeReset(object, allocator, pipeline);
    }
};
void LifecycleChecks()
{
    std::array<void*,11> runtime {}, driver {};
    runtime[0] = driver[0] = reinterpret_cast<void*>(&LifecycleList::Query);
    runtime[2] = driver[2] = reinterpret_cast<void*>(&LifecycleList::Release);
    runtime[10] = reinterpret_cast<void*>(&LifecycleList::RuntimeReset);
    driver[10] = reinterpret_cast<void*>(&LifecycleList::DriverReset);
    LifecycleList lists[] {{runtime.data()}, {driver.data()}};
    auto* pipeline = reinterpret_cast<ID3D12PipelineState*>(0x1234);
    auto* allocator = reinterpret_cast<ID3D12CommandAllocator*>(0x2345);
    for (auto& list : lists)
    {
        void* output = list.Get();
        LateListCreated(S_OK, &output, pipeline, true);
        auto& state = DLSSNRCommandState::states.at(list.Get());
        Require(state.recordingKnown && state.pipeline == pipeline,
                "CreateCommandList initial recording/PSO was lost");
        Require(list.refs == 1, "creation observation leaked COM reference");
        DLSSNRCommandState::Value(list.Get(), true, 3, DLSSNRCommandState::Kind::CBV, 123);
        Require(list.Get()->Reset(nullptr, nullptr) == E_FAIL, "failed reset HRESULT lost");
        Require(DLSSNRCommandState::states.at(list.Get()).compute.roots.size() == 1,
                "failed reset started a new recording");
        const auto queriesBefore = list.queries;
        Require(list.Get()->Reset(allocator, pipeline) == S_OK, "actual implementation Reset failed");
        Require(DLSSNRCommandState::states.at(list.Get()).recordingKnown &&
                DLSSNRCommandState::states.at(list.Get()).compute.roots.empty(),
                "actual Reset did not clear previous bindings and start recording");
        Require(list.resets == 2 && list.queries == queriesBefore, "nested Reset forwarded incorrectly");
        DLSSNRCommandState::Reset(list.Get());
        LateListCreated(S_OK, &output, nullptr, false);
        Require(!DLSSNRCommandState::states.contains(list.Get()), "CreateCommandList1 starts CLOSED");
        Require(list.Get()->Reset(allocator, pipeline) == S_OK &&
                DLSSNRCommandState::states.at(list.Get()).recordingKnown, "closed-created list Reset missed");
        DLSSNRLatePass::enabled = false;
        Require(list.Get()->Reset(allocator, pipeline) == S_OK &&
                !DLSSNRCommandState::states.contains(list.Get()), "disabled model started state capture");
        DLSSNRLatePass::enabled = true;
        LateListCreated(E_FAIL, &output, pipeline, true);
        Require(!DLSSNRCommandState::states.contains(list.Get()), "failed creation started capture");
    }
    Require(lists[1].driverResets == 4, "driver lifecycle trampoline was bypassed");
    Require(CaptureReset::Remove() == NO_ERROR, "lifecycle detach failed");
}
struct ConstantCapture
{
    void** table;
    UINT calls = 0;
    uint64_t mask = 0;
    std::array<UINT,64> values {};
    static void STDMETHODCALLTYPE Write(ID3D12GraphicsCommandList* cmd, UINT index, UINT count,
                                         const void* data, UINT offset)
    {
        auto& capture = *reinterpret_cast<ConstantCapture*>(cmd);
        Require(index == 7 && count && offset + count <= 64, "invalid constant restore span");
        ++capture.calls;
        for (UINT i=0;i<count;++i)
        { capture.values[offset+i]=static_cast<const UINT*>(data)[i]; capture.mask |= uint64_t(1)<<(offset+i); }
    }
};
void ConstantChecks()
{
    std::array<void*,43> table {};
    table[35]=table[36]=reinterpret_cast<void*>(&ConstantCapture::Write);
    for (bool compute : {true,false})
    {
        ConstantCapture capture {table.data()};
        DLSSNRCommandState::RootConstants constants;
        std::array<UINT,64> values;for(UINT i=0;i<64;++i)values[i]=100+i;
        constants.Set(0,2,values.data());constants.Set(4,3,values.data()+4);constants.Set(63,1,values.data()+63);
        constants.Restore(reinterpret_cast<ID3D12GraphicsCommandList*>(&capture),compute,7);
        Require(capture.calls==3 && capture.mask==((uint64_t(1)<<63)|0x73),"sparse constants lost holes/boundary");
        Require(capture.values[1]==101 && capture.values[5]==105 && capture.values[63]==163,"sparse values lost");
        constants.Set(0,64,values.data());capture.calls=0;capture.mask=0;
        constants.Restore(reinterpret_cast<ID3D12GraphicsCommandList*>(&capture),compute,7);
        Require(capture.calls==1 && capture.mask==~uint64_t(0) && capture.values==values,"64-DWORD restore failed");
    }
}
int main()
{
    try
    {
        ConstantChecks();
        Require(Hook::Install(nullptr, Callback) == ERROR_INVALID_PARAMETER, "null target accepted");
        bool added = false;
        Require(Hook::Install(reinterpret_cast<void*>(Runtime), Callback, &added) == NO_ERROR && added, "runtime hook");
        Require(Hook::Install(reinterpret_cast<void*>(Runtime), Callback, &added) == NO_ERROR && !added, "duplicate hook");
        Object object;
        Runtime(&object, 2);
        Require(object.runtime == 2 && object.callbacks == 1, "runtime forwarding");
        std::cerr << "Testing live installation...\n";
        std::atomic<bool> running {true}, liveGood {true};
        std::atomic<unsigned> started {0};
        std::vector<std::thread> liveThreads;
        for (unsigned n = 0; n < 4; ++n)
            liveThreads.emplace_back([&] {
                Object local;
                unsigned calls = 0;
                ++started;
                while (running.load()) { Driver(&local, 1); ++calls; }
                if (local.runtime != calls || local.driver != calls || local.callbacks != calls) liveGood = false;
            });
        while (started.load() != 4) std::this_thread::yield();
        const auto liveInstall = Hook::Install(reinterpret_cast<void*>(Driver), Callback);
        running = false;
        for (auto& thread : liveThreads) thread.join();
        Require(liveInstall == NO_ERROR, "driver hook while recording threads run");
        Require(liveGood, "concurrent installation lost/duplicated original calls");
        std::cerr << "Live installation passed.\n";
        Driver(&object, 3);
        Require(object.runtime == 5 && object.driver == 3 && object.callbacks == 2, "nested driver/runtime captured twice");
        Require(Hook::Install(reinterpret_cast<void*>(OtherDriver), Callback) == NO_ERROR, "second driver hook");
        OtherDriver(&object, 4);
        Require(object.driver == 15 && object.runtime == 5 && object.callbacks == 3, "wrong original implementation");
        std::atomic<bool> good {true};
        std::vector<std::thread> threads;
        for (unsigned n = 0; n < 4; ++n)
            threads.emplace_back([&] {
                Object local;
                for (unsigned i = 0; i < 10000; ++i) { Runtime(&local, 1); Driver(&local, 2); OtherDriver(&local, 3); }
                if (local.runtime != 30000 || local.driver != 110000 || local.callbacks != 30000) good = false;
            });
        for (auto& thread : threads) thread.join();
        Require(good, "thread-local original routing");
        std::cerr << "Testing production state hooks...\n";
        StateChecks();
        LifecycleChecks();
        PipelineChecks();
        std::cerr << "Testing chained removal...\n";
        Require(Observer::Install(reinterpret_cast<void*>(Runtime), Observe) == NO_ERROR, "coexisting state observer");
        Require(Hook::Remove() == ERROR_BUSY, "covered trampoline must stay alive");
        Require(Observer::Remove() == NO_ERROR, "detach coexisting observer");
        Require(Hook::Remove() == NO_ERROR, "detach");
        Runtime(&object, 1); Driver(&object, 1); OtherDriver(&object, 1);
        Require(object.callbacks == 3 && object.runtime == 7 && object.driver == 19, "detach changed originals");
        Require(Hook::Install(reinterpret_cast<void*>(Runtime), Callback) == NO_ERROR, "reinstall");
        Runtime(&object, 1);
        Require(object.callbacks == 4, "reinstall callback");
        Require(Hook::Remove() == NO_ERROR, "final detach");
        std::cout << "Method-entry/state checks passed: " << checks << "; 120000 concurrent calls.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
