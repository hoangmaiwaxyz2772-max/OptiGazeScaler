#pragma once

namespace LifecycleRegression
{
// Execute the production input callback, HUDfix counters and active gate.
// Only their host dependencies and NGX staging are replaced by fixture data.
struct Option
{
    bool value = false;
    bool value_or_default() const { return value; }
    int value_or(int fallback) const { return fallback; }
};
struct Config
{
    Option FGEnabled, FGHUDFix, OutputResourceBarrier;
    static Config* Instance() { static Config value; return &value; }
};
struct Feature { UINT64 FrameCount() const { return 1; } };
using IFeature_Dx12 = Feature;
struct FG { bool IsActive() const { return true; } };
enum class FGInput { Upscaler, Native };
struct State
{
    FG* currentFG = nullptr;
    Feature* currentFeature = nullptr;
    FGInput activeFgInput = FGInput::Upscaler;
    bool isShuttingDown = false, fgChanged = false;
    void* currentSwapchain = nullptr;
    double lastFGFrameTime = 16;
    static State& Instance() { static State value; return value; }
};
namespace DLSSNRLatePass
{
    bool enabled = false, pending = false;
    bool Enabled() { return enabled; }
    bool Pending() { return enabled && pending; }
}
constexpr int NVSDK_NGX_Parameter_Output = 0, NVSDK_NGX_Result_Success = 0;
struct NVSDK_NGX_Parameter
{
    ID3D12Resource* output = nullptr;
    template<class T> int Get(int, T* value) { *value = output; return NVSDK_NGX_Result_Success; }
};
enum CaptureInfo { Upscaler };
enum ResourceType { UAV };
struct ResourceInfo
{
    ID3D12Resource* buffer;
    UINT64 width;
    UINT height;
    DXGI_FORMAT format;
    D3D12_RESOURCE_FLAGS flags;
    ResourceType type;
    CaptureInfo captureInfo;
};
struct Hudfix_Dx12
{
    inline static UINT64 _upscaleCounter = 0, _fgCounter = 0;
    inline static int _captureCounter[4] = {};
    inline static double _frameTime = 0;
    inline static bool _skipTracking = false, _skipHudlessChecks = false;
    static unsigned GetIndex() { return static_cast<unsigned>(_upscaleCounter % 4); }
    static void SetSkipStatus(bool value) { _skipTracking = value; }
    static void UpscaleEnd(UINT64, double);
    static void PresentStart();
    static void PresentEnd();
    static UINT64 ActiveUpscaleFrame();
    static UINT64 ActivePresentFrame();
    static bool IsResourceCheckActive();
    static void CheckForHudless(ID3D12GraphicsCommandList*, ResourceInfo*, D3D12_RESOURCE_STATES, bool) {}
};
struct UpscalerInputsDx12
{
    inline static ID3D12Device* _device = nullptr;
    static void UpscaleEnd(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, IFeature_Dx12*);
};
#define LOG_DEBUG(...) ((void)0)
#include "late-lifecycle-production.h"
#undef LOG_DEBUG

UINT Run(ID3D12Device* device)
{
    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 1);
    auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> output;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output)));
    Feature feature;
    FG fg;
    NVSDK_NGX_Parameter parameters {output.Get()};
    UINT checks = 0;
    for (int mode = 0; mode < 3; ++mode) // FG off, native FG, custom upscaler FG
    {
        auto& state = State::Instance();
        state = {};
        state.currentFeature = &feature;
        state.currentFG = mode ? &fg : nullptr;
        state.activeFgInput = mode == 1 ? FGInput::Native : FGInput::Upscaler;
        state.currentSwapchain = &fg;
        Config::Instance()->FGEnabled.value = mode != 0;
        Config::Instance()->FGHUDFix.value = mode != 0;
        UpscalerInputsDx12::_device = mode ? device : nullptr;
        Hudfix_Dx12::_upscaleCounter = Hudfix_Dx12::_fgCounter = 0;
        DLSSNRLatePass::enabled = true;
        for (int frame = 0; frame < 3; ++frame)
        {
            DLSSNRLatePass::pending = false;
            Hudfix_Dx12::SetSkipStatus(true);
            // The real NVNGX wrapper calls this BEFORE feature->Evaluate/Stage.
            UpscalerInputsDx12::UpscaleEnd(nullptr, &parameters, &feature);
            Require(Hudfix_Dx12::ActiveUpscaleFrame() == UINT64(frame + 1),
                "late window never opened or advanced twice with FG");
            ++checks;
            if (mode == 0)
            {
                Require(!Hudfix_Dx12::IsResourceCheckActive(), "capture active without a staged request");
                ++checks;
            }
            DLSSNRLatePass::pending = true; // Evaluate -> successful Stage
            Require(Hudfix_Dx12::IsResourceCheckActive(), "staged frame cannot reach resource checks");
            ++checks;
            Hudfix_Dx12::PresentStart();
            Require(!Hudfix_Dx12::IsResourceCheckActive(), "capture window leaked past Present");
            ++checks;
        }
    }
    DLSSNRLatePass::enabled = DLSSNRLatePass::pending = false;
    const auto before = Hudfix_Dx12::ActiveUpscaleFrame();
    UpscalerInputsDx12::UpscaleEnd(nullptr, &parameters, &feature);
    Require(Hudfix_Dx12::ActiveUpscaleFrame() == before + 1 && Hudfix_Dx12::IsResourceCheckActive(),
        "ordinary custom-FG capture changed when late mode is disabled");
    ++checks;
    State::Instance().currentFG = nullptr;
    Hudfix_Dx12::PresentStart();
    UpscalerInputsDx12::UpscaleEnd(nullptr, &parameters, &feature);
    Require(Hudfix_Dx12::ActiveUpscaleFrame() == before + 1 && !Hudfix_Dx12::IsResourceCheckActive(),
        "disabled FG and late mode opened a window");
    ++checks;
    std::cout << "Late capture lifecycle checks passed: " << checks << " (production callbacks/gates, FG off/native/custom).\n";
    return checks;
}
}
