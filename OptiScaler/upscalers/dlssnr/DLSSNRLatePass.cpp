#include <pch.h>
#include "DLSSNRLatePass.h"
#include "DLSSNRDiagnostics.h"
#include "DLSSNRLateColor.h"
#include "DLSSNRCommandState.h"
#include "DLSSNRPreview.h"
#include "DLSSNRPipelineTrace.h"
#include "DLSSNRPipelineSplit.h"
#include "DLSSNRPipelineAccess.h"
#include <Config.h>
#include <State.h>
#include <NVNGX_Parameter.h>
#include <hudfix/Hudfix_Dx12.h>
#include <resource_tracking/ResTrack_dx12.h>
#include <array>
#include <atomic>
#include <mutex>
#include <algorithm>

namespace DLSSNRLatePass
{
namespace
{
using Microsoft::WRL::ComPtr;
struct Frame
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> motion, depth;
    NVNGX_Parameters parameters {"DLSSNR late scene snapshot", false};
    LateColor color;
    DLSSNRFeatureDx12* owner = nullptr;
    DLSSNRFeatureDx12::FoveatedRegion region {};
    UINT width = 0, height = 0;
    bool inverted = false, foveated = false;
    UINT64 serial = 0;
};
std::recursive_mutex mutex;
std::shared_ptr<Frame> pending;
std::array<std::shared_ptr<Frame>, GAZE_ROI_FRAME_SLOTS> pool;
std::atomic<bool> hasPending {false};
std::atomic<DXGI_COLOR_SPACE_TYPE> colorSpace {DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
UINT64 serial = 0, completed = 0, missed = 0;
std::string lastReason;

bool AcquireSlot(ID3D12GraphicsCommandList* commands, uint32_t& slot)
{
    if (GazeRoiFrameSync::TryAcquire(commands, slot)) return true;
    while (DLSSNRPipelineAccess::Enabled() && GazeRoiFrameSync::WaitForSubmittedWork())
        if (GazeRoiFrameSync::TryAcquire(commands, slot)) return true;
    return false;
}

void Bypass(const std::string& reason)
{
    if (lastReason != reason)
    {
        LOG_WARN("[DLSSNR_LATE] bypass: {}", reason);
        lastReason = reason;
    }
}
ID3D12Resource* Resource(NVSDK_NGX_Parameter* parameters, const char* key)
{
    ID3D12Resource* result = nullptr;
    if (parameters->Get(key, &result) != NVSDK_NGX_Result_Success)
        parameters->Get(key, reinterpret_cast<void**>(&result));
    return result;
}
template<class T> void CopyValue(NVSDK_NGX_Parameter* source, NVNGX_Parameters& target, const char* key)
{
    T value {};
    if (source->Get(key, &value) == NVSDK_NGX_Result_Success) target.Set(key, value);
}
std::string DescribeGuide(const D3D12_RESOURCE_DESC& desc)
{
    return std::format("dimension={} size={}x{} array={} mips={} format=0x{:X} samples={}:{} alignment={} layout={} flags=0x{:X}",
        static_cast<UINT>(desc.Dimension), desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels,
        static_cast<UINT>(desc.Format), desc.SampleDesc.Count, desc.SampleDesc.Quality, desc.Alignment,
        static_cast<UINT>(desc.Layout), static_cast<UINT>(desc.Flags));
}
bool EnsureGuide(ID3D12Device* device, ID3D12Resource* source, ComPtr<ID3D12Resource>& copy, const char* name)
{
    if (!source) { copy.Reset(); return true; }
    auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        desc.DepthOrArraySize != 1)
    {
        Bypass(std::format("{} guide unsupported: {}", name, DescribeGuide(desc)));
        return false;
    }
    const auto originalDesc = desc;
    // Retain the original allocation and subrect bases. Only the private copy
    // needs a shader-readable typeless depth format; copying does not resample.
    switch (desc.Format)
    {
    case DXGI_FORMAT_D32_FLOAT: desc.Format = DXGI_FORMAT_R32_TYPELESS; break;
    case DXGI_FORMAT_D16_UNORM: desc.Format = DXGI_FORMAT_R16_TYPELESS; break;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: desc.Format = DXGI_FORMAT_R24G8_TYPELESS; break;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS; break;
    default: break;
    }
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    // This is an independent committed allocation, not another placement in
    // the game's heap or tiled layout. Let D3D12 choose its alignment/layout.
    desc.Alignment = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (copy)
    {
        auto old = copy->GetDesc();
        if (old.Width == desc.Width && old.Height == desc.Height && old.MipLevels == desc.MipLevels &&
            old.Format == desc.Format) return true;
    }
    copy.Reset();
    auto properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto result = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&copy));
    if (FAILED(result))
    {
        Bypass(std::format("{} guide snapshot creation failed HRESULT=0x{:08X}; source [{}]; copy [{}]",
            name, static_cast<UINT>(result), DescribeGuide(originalDesc), DescribeGuide(desc)));
        return false;
    }
    LOG_INFO("[DLSSNR_LATE] {} guide snapshot created; source [{}]; copy [{}]",
        name, DescribeGuide(originalDesc), DescribeGuide(desc));
    return true;
}
void CopyGuide(ID3D12GraphicsCommandList* commands, ID3D12Resource* source, ID3D12Resource* copy,
               D3D12_RESOURCE_STATES state)
{
    if (!source || !copy) return;
    LateColor::Barrier(commands, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    LateColor::Barrier(commands, copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyResource(copy, source);
    LateColor::Barrier(commands, copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    LateColor::Barrier(commands, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
}
void DropPending(bool miss)
{
    if (pending && miss)
    {
        pending->owner->InvalidateHistory();
        ++missed;
        if (missed == 1 || missed % 300 == 0)
        {
            LOG_WARN("[DLSSNR_LATE] no usable HUDfix scene this frame; skipped={}, completed={}, checksActive={}, skipChecks={}, upscaleFrame={}, presentFrame={}",
                missed, completed, Hudfix_Dx12::IsResourceCheckActive(), Hudfix_Dx12::SkipHudlessChecks(),
                Hudfix_Dx12::ActiveUpscaleFrame(), Hudfix_Dx12::ActivePresentFrame());
            ResTrack_Dx12::LogLateCaptureDiagnostics();
        }
    }
    hasPending.store(false, std::memory_order_release);
    pending.reset();
}
}

bool Enabled()
{
    auto* config = Config::Instance();
    return config->DLSSNREnabled.value_or_default() && config->DLSSNRLateHudless.value_or_default() &&
        !State::Instance().isShuttingDown;
}
bool TrackDescriptors()
{
    const auto* config = Config::Instance();
    return (config->DLSSNREnabled.value_or_default() || config->DLSSNRLateHudless.value_or_default()) &&
        !State::Instance().isShuttingDown;
}
bool Pending() { return Enabled() && hasPending.load(std::memory_order_acquire); }
void SetColorSpace(DXGI_COLOR_SPACE_TYPE value) { colorSpace.store(value, std::memory_order_release); }
void Cancel(DLSSNRFeatureDx12* owner)
{
    std::lock_guard lock(mutex);
    if (pending && pending->owner == owner) DropPending(false);
    // In-flight frames remain owned by fence callbacks; idle caches may go.
    for (auto& frame : pool) if (frame && frame->owner == owner) frame.reset();
}
void EndFrame()
{
    if (!hasPending.load(std::memory_order_acquire) && !Enabled()) return;
    {
        std::lock_guard lock(mutex);
        DropPending(true);
    }
    if (Enabled() && (!Config::Instance()->FGEnabled.value_or_default() || State::Instance().currentFG == nullptr))
    {
        ResTrack_Dx12::ClearPossibleHudless();
        Hudfix_Dx12::PresentStart();
    }
}

bool Stage(DLSSNRFeatureDx12* owner, ID3D12Device* device, ID3D12GraphicsCommandList* commands,
           NVSDK_NGX_Parameter* parameters, UINT width, UINT height, bool inverted,
           const DLSSNRFeatureDx12::FoveatedRegion* region)
{
    std::lock_guard lock(mutex);
    DropPending(true);
    if (!Enabled() || !owner || !device || !commands || !parameters || !width || !height)
    {
        DLSSNR_DIAG("stage-reject", "enabled={} owner={} device={} cmd={} parameters={} size={}x{}",
            Enabled(), owner != nullptr, device != nullptr, commands != nullptr, parameters != nullptr, width, height);
        return false;
    }
    ResTrack_Dx12::HookDevice(device);
    ResTrack_Dx12::EnsureQueueHook(device);
    GazeRoiFrameSync::FlushDeferred();
    std::shared_ptr<Frame> frame;
    do
    {
        for (auto& entry : pool)
        {
            if (!entry) entry = std::make_shared<Frame>();
            if (entry.use_count() == 1)
            {
                if (entry->device && entry->device.Get() != device) entry = std::make_shared<Frame>();
                frame = entry; break;
            }
        }
    } while (!frame && DLSSNRPipelineAccess::Enabled() && GazeRoiFrameSync::WaitForSubmittedWork());
    if (!frame) { Bypass("guide snapshot pool is still in GPU use"); owner->InvalidateHistory(); return false; }
    auto* motion = Resource(parameters, NVSDK_NGX_Parameter_MotionVectors);
    auto* depth = Resource(parameters, NVSDK_NGX_Parameter_Depth);
    if (!Resource(parameters, NVSDK_NGX_Parameter_Output))
    { Bypass("completed DLSS output resource is null"); owner->InvalidateHistory(); return false; }
    if (!EnsureGuide(device, motion, frame->motion, "motion") ||
        !EnsureGuide(device, depth, frame->depth, "depth"))
    { owner->InvalidateHistory(); return false; }
    frame->device = device; frame->owner = owner; frame->width = width; frame->height = height;
    frame->inverted = inverted; frame->foveated = region != nullptr;
    frame->region = region ? *region : DLSSNRFeatureDx12::FoveatedRegion {};
    frame->serial = ++serial;
    DLSSNRPipelineTrace::Mark("guide-snapshot-record", commands, frame->motion.Get(), frame->serial);
    auto& snapshot = frame->parameters;
    snapshot.Reset();
    for (const char* key : {NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
         NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
         NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
         NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
         NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y})
        CopyValue<unsigned int>(parameters, snapshot, key);
    CopyValue<int>(parameters, snapshot, NVSDK_NGX_Parameter_Reset);
    for (const char* key : {NVSDK_NGX_Parameter_MV_Scale_X, NVSDK_NGX_Parameter_MV_Scale_Y,
         NVSDK_NGX_Parameter_Jitter_Offset_X, NVSDK_NGX_Parameter_Jitter_Offset_Y})
        CopyValue<float>(parameters, snapshot, key);
    snapshot.Set(NVSDK_NGX_Parameter_MotionVectors, frame->motion.Get());
    snapshot.Set(NVSDK_NGX_Parameter_Depth, frame->depth.Get());
    // The game exposure/LUT has already been applied at the late capture.
    snapshot.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    snapshot.Set(NVSDK_NGX_Parameter_ExposureTexture, static_cast<ID3D12Resource*>(nullptr));
    snapshot.Set("DLSSNR.OriginalOutputWidth", width);
    snapshot.Set("DLSSNR.OriginalOutputHeight", height);
    uint32_t slot = 0;
    if (!AcquireSlot(commands, slot))
    { Bypass("guide producer lifetime slot unavailable"); owner->InvalidateHistory(); return false; }
    CopyGuide(commands, motion, frame->motion.Get(), static_cast<D3D12_RESOURCE_STATES>(
        Config::Instance()->MVResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)));
    CopyGuide(commands, depth, frame->depth.Get(), static_cast<D3D12_RESOURCE_STATES>(
        Config::Instance()->DepthResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)));
    // Protect copies even if this frame never finds a late scene. The consumer
    // takes a second lifetime reference covering its own command-list fence.
    GazeRoiFrameSync::DeferCallback([frame]() {});
    pending = frame; hasPending.store(true, std::memory_order_release);
    DLSSNR_DIAG("staged", "frame={} producer=0x{:X} size={}x{} slot={} motion=0x{:X} depth=0x{:X} upscale={} present={}",
        frame->serial, (uintptr_t)commands, width, height, slot, (uintptr_t)motion, (uintptr_t)depth,
        Hudfix_Dx12::ActiveUpscaleFrame(), Hudfix_Dx12::ActivePresentFrame());
    return true;
}

bool Process(ID3D12GraphicsCommandList* commands, ID3D12Resource* scene, D3D12_RESOURCE_STATES state)
{
    if (!Pending() || DLSSNRCommandState::suppress) return false;
    std::lock_guard lock(mutex);
    auto frame = pending;
    if (!frame || !scene || !commands || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        DLSSNR_DIAG("process-reject", "reason=missing-context-or-nondirect frame={} scene={} cmd={} type={}",
            frame != nullptr, scene != nullptr, commands != nullptr, commands ? (int)commands->GetType() : -1);
        return false;
    }
    DLSSNR_DIAG("process-enter", "frame={} cmd=0x{:X} scene=0x{:X} state=0x{:X}",
        frame->serial, (uintptr_t)commands, (uintptr_t)scene, (UINT)state);
    const char* stateFailure = nullptr;
    auto saved = DLSSNRCommandState::Snapshot(commands, &stateFailure);
    if (!saved)
    {
        DLSSNR_DIAG("process-reject", "frame={} cmd=0x{:X} reason={}", frame->serial, (uintptr_t)commands, stateFailure);
        Bypass("candidate command state is incomplete, bundled, or inside a render pass"); return false;
    }
    auto desc = scene->GetDesc();
    DLSSNRPipelineTrace::Mark("hudless-nr-boundary", commands, scene, frame->serial);
    DLSSNRPipelineTrace::WatchResource(scene);
    const auto space = colorSpace.load(std::memory_order_acquire);
    UINT encoding = 0;
    if (space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) encoding = 1;
    else if (space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) encoding = 2;
    else if (space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
    { Bypass("unsupported swapchain color space"); return false; }
    DLSSNRCommandState::Suppress suppress;
    struct Restore
    {
        ID3D12GraphicsCommandList* commands;
        DLSSNRCommandState::State state;
        ~Restore() { state.Restore(commands); }
    } restore {commands, *saved};
    if (!frame->color.Ensure(frame->device.Get(), desc))
    { Bypass("candidate format cannot be converted and copied back"); return false; }
    uint32_t slot = 0;
    if (!AcquireSlot(commands, slot))
    { Bypass("late consumer lifetime slot unavailable"); return false; }
    GazeRoiFrameSync::DeferCallback([frame]() {});
    // Match HUDfix's top-left crop when the allocation is padded. A smaller scene uses
    // the original guide grid; only the output-space ROI is scaled to it.
    UINT width = std::min(frame->width, frame->color.width);
    UINT height = std::min(frame->height, frame->color.height);
    auto& snapshot = frame->parameters;
    snapshot.Set(NVSDK_NGX_Parameter_Output, frame->color.color.Get());
    snapshot.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
    snapshot.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
    auto region = frame->region;
    if (frame->foveated)
    {
        UINT right = static_cast<UINT>((UINT64(region.outputX + region.outputWidth) * width) / frame->width);
        UINT bottom = static_cast<UINT>((UINT64(region.outputY + region.outputHeight) * height) / frame->height);
        region.outputX = static_cast<UINT>((UINT64(region.outputX) * width) / frame->width);
        region.outputY = static_cast<UINT>((UINT64(region.outputY) * height) / frame->height);
        region.outputWidth = right - region.outputX; region.outputHeight = bottom - region.outputY;
    }
    const auto* config = Config::Instance();
    // The ordinary ROI path reads/writes only this rectangle. Exterior fitting
    // and the composited guide diagnostics can access the whole logical scene.
    // The full-scene preview needs a full read, but never a full writeback.
    const bool wholeScene = !frame->foveated || config->DLSSNRGazeRoiExtrapolation.value_or_default() ||
        (config->DLSSNRDebugInputView.value_or_default() && config->DLSSNRDebugInputViewComposite.value_or_default());
    if (!frame->color.SetRegion(wholeScene ? 0u : region.outputX, wholeScene ? 0u : region.outputY,
                               wholeScene ? width : region.outputWidth, wholeScene ? height : region.outputHeight))
    { Bypass("late work rectangle is empty or outside the scene"); return false; }
    DLSSNRPipelineSplit::BeforeNR(commands, scene, frame->serial);
    // Everything in this scope belongs to this NR instance and its retained
    // frame slot. The only game-owned image is scene. Guide snapshots are
    // explicitly included because their producer can be on the next prefix.
    DLSSNRPipelineAccess::Resource(commands, scene, true);
    DLSSNRPipelineAccess::Resource(commands, frame->motion.Get(), false);
    DLSSNRPipelineAccess::Resource(commands, frame->depth.Get(), false);
    DLSSNRPipelineAccess::Domain(commands, frame->owner, "nr-feature-private-domain");
    DLSSNRPipelineAccess::PrivateWork privateWork;
    frame->color.Read(frame->device.Get(), commands, scene, state, encoding,
                      config->DLSSNRPresentPreview.value_or_default() == 3);
    DLSSNR_DIAG("scene-copy-recorded", "frame={} scene=0x{:X} format={} size={}x{} work={}x{}+{},{} encoding={} slot={} typedWrite={}",
        frame->serial, (uintptr_t)scene, (UINT)desc.Format, desc.Width, desc.Height,
        frame->color.regionWidth, frame->color.regionHeight, frame->color.regionX, frame->color.regionY, encoding, slot, frame->color.typedOutput);
    DLSSNRPreview::Capture(frame->device.Get(), commands, frame->color.color.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 0, width, height, 3, encoding != 0);
    // Consume once. A failed model evaluation must not run again on another
    // candidate and must not leak a partially processed image into the game.
    DropPending(false);
    ID3D12Resource* processed = nullptr;
    const bool success = frame->owner->Evaluate(frame->device.Get(), commands, &snapshot, width, height,
        frame->inverted, frame->foveated ? &region : nullptr,
        encoding == 0 ? DLSSNRFeatureDx12::ColorDomain::DisplaySDR : DLSSNRFeatureDx12::ColorDomain::DisplayLinearHDR, slot, &processed);
    DLSSNR_DIAG("model-result", "frame={} success={}", frame->serial, success);
    if (!success)
    { Bypass("model evaluation failed; original scene retained"); frame->owner->InvalidateHistory(); return false; }
    if (processed) frame->color.SetProcessed(frame->device.Get(), processed);
    frame->color.Write(commands, scene, state, encoding, processed);
    DLSSNRPipelineTrace::AfterNR(commands);
    ++completed;
    if (completed == 1 || completed % 300 == 0 || !lastReason.empty())
        LOG_INFO("[DLSSNR_LATE] wrote original scene={:X} allocation={}x{} logical={}x{} format={} encoding={} frame={} completed={}",
            reinterpret_cast<UINT64>(scene), desc.Width, desc.Height, width, height,
            static_cast<UINT>(desc.Format), encoding, frame->serial, completed);
    lastReason.clear();
    return true;
}
}
