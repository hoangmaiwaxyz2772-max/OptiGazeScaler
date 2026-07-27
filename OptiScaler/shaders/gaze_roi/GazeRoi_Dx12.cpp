#include "pch.h"
#include "GazeRoi_Dx12.h"

#include <Config.h>

#include <array>
#include <mutex>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace
{
enum class GazeRoiSlotState
{
    Free,
    Recording,
    Submitted,
};

struct GazeRoiSlot
{
    GazeRoiSlotState state = GazeRoiSlotState::Free;
    ID3D12GraphicsCommandList* commandList = nullptr;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    uint64_t generation = 0;
    bool timingActive = false;
    bool timingResolved = false;
    UINT64 timestampFrequency = 0;
};

struct GazeRoiQueueFence
{
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 nextValue = 0;
};

struct GazeRoiDeferredCallback
{
    uint64_t retireAfterGeneration = 0;
    std::function<void()> callback;
};

std::mutex gazeRoiFrameSyncMutex;
std::array<GazeRoiSlot, GAZE_ROI_FRAME_SLOTS> gazeRoiSlots;
std::unordered_map<ID3D12CommandQueue*, GazeRoiQueueFence> gazeRoiQueueFences;
HANDLE gazeRoiFenceEvent = nullptr;
uint64_t gazeRoiNextGeneration = 0;
std::vector<GazeRoiDeferredCallback> gazeRoiDeferredCallbacks;

constexpr UINT GAZE_ROI_MV_CONSTANT_DWORDS = 6;
constexpr UINT GAZE_ROI_DEPTH_CONSTANT_DWORDS = 4;
constexpr UINT GAZE_ROI_COLOR_CONSTANT_DWORDS = 6;
constexpr UINT GAZE_ROI_COMPOSITE_CONSTANT_DWORDS = 45;
constexpr UINT GAZE_ROI_DEPTH_DEBUG_CONSTANT_DWORDS = 10;
constexpr UINT GAZE_ROI_TIMING_MARKERS = 6;
ComPtr<ID3D12QueryHeap> gazeRoiTimestampHeap;
ComPtr<ID3D12Resource> gazeRoiTimestampReadback;

bool EnsureGazeRoiTimingResourcesLocked(ID3D12Device* device)
{
    if (gazeRoiTimestampHeap != nullptr && gazeRoiTimestampReadback != nullptr)
        return true;
    if (device == nullptr)
        return false;

    D3D12_QUERY_HEAP_DESC queryDesc {};
    queryDesc.Count = GAZE_ROI_FRAME_SLOTS * GAZE_ROI_TIMING_MARKERS;
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&gazeRoiTimestampHeap))))
        return false;

    const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(
        static_cast<UINT64>(queryDesc.Count) * sizeof(UINT64));
    if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&gazeRoiTimestampReadback))))
    {
        gazeRoiTimestampHeap.Reset();
        return false;
    }

    gazeRoiTimestampHeap->SetName(L"GazeRoi_Timestamp_Heap");
    gazeRoiTimestampReadback->SetName(L"GazeRoi_Timestamp_Readback");
    return true;
}

void LogCompletedGazeRoiTimingLocked(uint32_t slotIndex, GazeRoiSlot& slot)
{
    if (!slot.timingResolved || slot.timestampFrequency == 0 || gazeRoiTimestampReadback == nullptr)
        return;

    const UINT64 byteOffset = static_cast<UINT64>(slotIndex) * GAZE_ROI_TIMING_MARKERS * sizeof(UINT64);
    const D3D12_RANGE readRange = { static_cast<SIZE_T>(byteOffset),
                                    static_cast<SIZE_T>(byteOffset + GAZE_ROI_TIMING_MARKERS * sizeof(UINT64)) };
    UINT8* mapped = nullptr;
    if (SUCCEEDED(gazeRoiTimestampReadback->Map(0, &readRange, reinterpret_cast<void**>(&mapped))))
    {
        const auto* timestamps = reinterpret_cast<const UINT64*>(mapped + byteOffset);
        const double millisecondsPerTick = 1000.0 / static_cast<double>(slot.timestampFrequency);
        LOG_INFO("[GROI_TIMING] generation={} slot={} mvMs={:.4f} dlssMs={:.4f} postMs={:.4f} totalMs={:.4f}",
                 slot.generation, slotIndex, (timestamps[1] - timestamps[0]) * millisecondsPerTick,
                 (timestamps[3] - timestamps[2]) * millisecondsPerTick,
                 (timestamps[5] - timestamps[4]) * millisecondsPerTick,
                 (timestamps[5] - timestamps[0]) * millisecondsPerTick);
        const D3D12_RANGE writeRange = { 0, 0 };
        gazeRoiTimestampReadback->Unmap(0, &writeRange);
    }

    slot.timingActive = false;
    slot.timingResolved = false;
    slot.timestampFrequency = 0;
}

bool HasActiveGazeRoiGenerationLocked(uint64_t retireAfterGeneration)
{
    for (const auto& slot : gazeRoiSlots)
    {
        if (slot.state != GazeRoiSlotState::Free && slot.generation <= retireAfterGeneration)
            return true;
    }

    return false;
}

std::vector<std::function<void()>> CollectReadyGazeRoiCallbacksLocked()
{
    std::vector<std::function<void()>> ready;
    for (auto it = gazeRoiDeferredCallbacks.begin(); it != gazeRoiDeferredCallbacks.end();)
    {
        if (HasActiveGazeRoiGenerationLocked(it->retireAfterGeneration))
        {
            ++it;
            continue;
        }

        ready.push_back(std::move(it->callback));
        it = gazeRoiDeferredCallbacks.erase(it);
    }

    return ready;
}

void RunGazeRoiCallbacks(std::vector<std::function<void()>>& callbacks)
{
    for (auto& callback : callbacks)
    {
        if (callback)
            callback();
    }
}

void ReclaimCompletedGazeRoiSlotsLocked()
{
    for (size_t slotIndex = 0; slotIndex < gazeRoiSlots.size(); ++slotIndex)
    {
        auto& slot = gazeRoiSlots[slotIndex];
        if (slot.state != GazeRoiSlotState::Submitted || slot.fence == nullptr || slot.fenceValue == 0)
            continue;

        if (slot.fence->GetCompletedValue() < slot.fenceValue)
            continue;

        LogCompletedGazeRoiTimingLocked(static_cast<uint32_t>(slotIndex), slot);

        slot.state = GazeRoiSlotState::Free;
        slot.commandList = nullptr;
        slot.fence.Reset();
        slot.fenceValue = 0;
    }
}

int FindFreeGazeRoiSlotLocked()
{
    for (size_t index = 0; index < gazeRoiSlots.size(); ++index)
    {
        if (gazeRoiSlots[index].state == GazeRoiSlotState::Free)
            return static_cast<int>(index);
    }

    return -1;
}

bool IsSubmittedCommandList(ID3D12GraphicsCommandList* commandList, UINT numCommandLists,
                            ID3D12CommandList* const* commandLists)
{
    if (commandList == nullptr || commandLists == nullptr)
        return false;

    for (UINT index = 0; index < numCommandLists; ++index)
    {
        if (commandLists[index] == commandList)
            return true;
    }

    return false;
}

} // namespace

bool GazeRoiFrameSync::Acquire(ID3D12GraphicsCommandList* commandList, uint32_t& frameSlot)
{
    frameSlot = 0;
    if (commandList == nullptr)
        return false;

    std::unique_lock lock(gazeRoiFrameSyncMutex);
    ReclaimCompletedGazeRoiSlotsLocked();

    int slotIndex = FindFreeGazeRoiSlotLocked();
    if (slotIndex < 0)
    {
        GazeRoiSlot* waitSlot = nullptr;
        for (auto& slot : gazeRoiSlots)
        {
            if (slot.state == GazeRoiSlotState::Submitted && slot.fence != nullptr && slot.fenceValue != 0)
            {
                waitSlot = &slot;
                break;
            }
        }

        if (waitSlot == nullptr)
        {
            LOG_ERROR("[GROI_SYNC] no reusable frame slot; all {} slots are recorded but not submitted",
                      GAZE_ROI_FRAME_SLOTS);
            return false;
        }

        if (gazeRoiFenceEvent == nullptr)
            gazeRoiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (gazeRoiFenceEvent == nullptr)
        {
            LOG_ERROR("[GROI_SYNC] CreateEvent failed: {}", GetLastError());
            return false;
        }

        ComPtr<ID3D12Fence> waitFence = waitSlot->fence;
        const UINT64 waitValue = waitSlot->fenceValue;
        const HRESULT eventResult = waitFence->SetEventOnCompletion(waitValue, gazeRoiFenceEvent);
        if (FAILED(eventResult))
        {
            LOG_ERROR("[GROI_SYNC] SetEventOnCompletion failed fence={} completed={} result={:X}", waitValue,
                      waitFence->GetCompletedValue(), static_cast<UINT>(eventResult));
            return false;
        }

        LOG_WARN("[GROI_SYNC] waiting for a frame slot fence={} completed={}", waitValue,
                 waitFence->GetCompletedValue());
        lock.unlock();
        const DWORD waitResult = WaitForSingleObject(gazeRoiFenceEvent, 5000);
        lock.lock();
        if (waitResult != WAIT_OBJECT_0)
        {
            LOG_ERROR("[GROI_SYNC] frame slot wait failed fence={} completed={} waitResult={}", waitValue,
                      waitFence->GetCompletedValue(), waitResult);
            return false;
        }

        ReclaimCompletedGazeRoiSlotsLocked();
        slotIndex = FindFreeGazeRoiSlotLocked();
        if (slotIndex < 0)
        {
            LOG_ERROR("[GROI_SYNC] fence completed but no frame slot became reusable");
            return false;
        }
    }

    auto& slot = gazeRoiSlots[slotIndex];
    slot.state = GazeRoiSlotState::Recording;
    slot.commandList = commandList;
    slot.fence.Reset();
    slot.fenceValue = 0;
    slot.generation = ++gazeRoiNextGeneration;
    slot.timingActive = false;
    slot.timingResolved = false;
    slot.timestampFrequency = 0;
    frameSlot = static_cast<uint32_t>(slotIndex);
    auto readyCallbacks = CollectReadyGazeRoiCallbacksLocked();
    LOG_DEBUG("[GROI_SYNC] acquire generation={} slot={} commandList={:X}", slot.generation, frameSlot,
              reinterpret_cast<size_t>(commandList));
    lock.unlock();
    RunGazeRoiCallbacks(readyCallbacks);
    return true;
}

void GazeRoiFrameSync::OnExecuteCommandLists(ID3D12CommandQueue* commandQueue, UINT numCommandLists,
                                              ID3D12CommandList* const* commandLists)
{
    if (commandQueue == nullptr || numCommandLists == 0 || commandLists == nullptr)
        return;

    std::unique_lock lock(gazeRoiFrameSyncMutex);
    bool containsGazeRoi = false;
    for (const auto& slot : gazeRoiSlots)
    {
        if (slot.state == GazeRoiSlotState::Recording &&
            IsSubmittedCommandList(slot.commandList, numCommandLists, commandLists))
        {
            containsGazeRoi = true;
            break;
        }
    }

    if (!containsGazeRoi)
        return;

    auto& queueFence = gazeRoiQueueFences[commandQueue];
    if (queueFence.fence == nullptr)
    {
        ComPtr<ID3D12Device> device;
        const HRESULT deviceResult = commandQueue->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(deviceResult) || device == nullptr)
        {
            LOG_ERROR("[GROI_SYNC] command queue GetDevice failed: {:X}", static_cast<UINT>(deviceResult));
            return;
        }

        const HRESULT fenceResult = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queueFence.fence));
        if (FAILED(fenceResult))
        {
            LOG_ERROR("[GROI_SYNC] CreateFence failed: {:X}", static_cast<UINT>(fenceResult));
            return;
        }
        queueFence.queue = commandQueue;
        LOG_INFO("[GROI_SYNC] initialized queue fence queue={:X} slots={}",
                 reinterpret_cast<size_t>(commandQueue), GAZE_ROI_FRAME_SLOTS);
    }

    const UINT64 signalValue = ++queueFence.nextValue;
    const HRESULT signalResult = commandQueue->Signal(queueFence.fence.Get(), signalValue);
    if (FAILED(signalResult))
    {
        LOG_ERROR("[GROI_SYNC] queue Signal failed fence={} result={:X}", signalValue,
                  static_cast<UINT>(signalResult));
        return;
    }

    for (auto& slot : gazeRoiSlots)
    {
        if (slot.state != GazeRoiSlotState::Recording ||
            !IsSubmittedCommandList(slot.commandList, numCommandLists, commandLists))
        {
            continue;
        }

        slot.state = GazeRoiSlotState::Submitted;
        slot.commandList = nullptr;
        slot.fence = queueFence.fence;
        slot.fenceValue = signalValue;
        if (slot.timingActive)
        {
            const HRESULT frequencyResult = commandQueue->GetTimestampFrequency(&slot.timestampFrequency);
            if (FAILED(frequencyResult))
            {
                slot.timestampFrequency = 0;
                LOG_WARN("[GROI_TIMING] GetTimestampFrequency failed result={:X}",
                         static_cast<UINT>(frequencyResult));
            }
        }
        LOG_DEBUG("[GROI_SYNC] submit generation={} slot fence={}", slot.generation, signalValue);
    }

    auto readyCallbacks = CollectReadyGazeRoiCallbacksLocked();
    lock.unlock();
    RunGazeRoiCallbacks(readyCallbacks);
}

void GazeRoiFrameSync::DeferRelease(IUnknown* object)
{
    if (object == nullptr)
        return;

    DeferCallback([object]() { object->Release(); });
}

void GazeRoiFrameSync::DeferCallback(std::function<void()> callback)
{
    if (!callback)
        return;

    std::unique_lock lock(gazeRoiFrameSyncMutex);
    gazeRoiDeferredCallbacks.push_back({ gazeRoiNextGeneration, std::move(callback) });
    auto readyCallbacks = CollectReadyGazeRoiCallbacksLocked();
    lock.unlock();
    RunGazeRoiCallbacks(readyCallbacks);
}

void GazeRoiFrameSync::FlushDeferred()
{
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard lock(gazeRoiFrameSyncMutex);
        callbacks.reserve(gazeRoiDeferredCallbacks.size());
        for (auto& deferred : gazeRoiDeferredCallbacks)
            callbacks.push_back(std::move(deferred.callback));
        gazeRoiDeferredCallbacks.clear();
    }

    RunGazeRoiCallbacks(callbacks);
}

bool GazeRoiFrameSync::BeginGpuTiming(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                                      uint32_t frameSlot)
{
    if (!Config::Instance()->GazeRoiGpuTiming.value_or_default() || device == nullptr || commandList == nullptr ||
        frameSlot >= GAZE_ROI_FRAME_SLOTS)
    {
        return false;
    }

    std::lock_guard lock(gazeRoiFrameSyncMutex);
    auto& slot = gazeRoiSlots[frameSlot];
    if (slot.state != GazeRoiSlotState::Recording || slot.commandList != commandList ||
        !EnsureGazeRoiTimingResourcesLocked(device))
    {
        return false;
    }

    slot.timingActive = true;
    slot.timingResolved = false;
    slot.timestampFrequency = 0;
    return true;
}

void GazeRoiFrameSync::WriteGpuTimestamp(ID3D12GraphicsCommandList* commandList, uint32_t frameSlot,
                                         uint32_t marker)
{
    if (commandList == nullptr || frameSlot >= GAZE_ROI_FRAME_SLOTS || marker >= GAZE_ROI_TIMING_MARKERS)
        return;

    std::lock_guard lock(gazeRoiFrameSyncMutex);
    const auto& slot = gazeRoiSlots[frameSlot];
    if (!slot.timingActive || slot.state != GazeRoiSlotState::Recording || slot.commandList != commandList ||
        gazeRoiTimestampHeap == nullptr)
    {
        return;
    }

    const UINT queryIndex = frameSlot * GAZE_ROI_TIMING_MARKERS + marker;
    commandList->EndQuery(gazeRoiTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
}

void GazeRoiFrameSync::ResolveGpuTiming(ID3D12GraphicsCommandList* commandList, uint32_t frameSlot)
{
    if (commandList == nullptr || frameSlot >= GAZE_ROI_FRAME_SLOTS)
        return;

    std::lock_guard lock(gazeRoiFrameSyncMutex);
    auto& slot = gazeRoiSlots[frameSlot];
    if (!slot.timingActive || slot.timingResolved || slot.state != GazeRoiSlotState::Recording ||
        slot.commandList != commandList || gazeRoiTimestampHeap == nullptr || gazeRoiTimestampReadback == nullptr)
    {
        return;
    }

    const UINT firstQuery = frameSlot * GAZE_ROI_TIMING_MARKERS;
    const UINT64 byteOffset = static_cast<UINT64>(firstQuery) * sizeof(UINT64);
    commandList->ResolveQueryData(gazeRoiTimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery,
                                  GAZE_ROI_TIMING_MARKERS, gazeRoiTimestampReadback.Get(), byteOffset);
    slot.timingResolved = true;
}

static const char* gazeRoiShader = R"(
cbuffer Params : register(b0)
{
    int _SrcWidth;
    int _SrcHeight;
    int _SrcTextureWidth;
    int _SrcTextureHeight;
    int _SrcBaseX;
    int _SrcBaseY;
    int _DstWidth;
    int _DstHeight;
    int _DstBaseX;
    int _DstBaseY;
    int _RoiX;
    int _RoiY;
    int _RoiWidth;
    int _RoiHeight;
    int _FeatherPx;
    int _DebugBorderPx;
    int _PeripheralBlur;
    float _PeripheralBlurRadius;
    int _PeripheralJitterCancel;
    int _PeripheralJitterSign;
    float _JitterOffsetX;
    float _JitterOffsetY;
    int _PeripheralTemporal;
    float _PeripheralTemporalHistoryWeight;
    float _PeripheralTemporalReactiveScale;
    int _PeripheralTemporalInitialized;
    int _PeripheralTemporalMotionReprojection;
    int _PeripheralTemporalHistoryReset;
    int _PeripheralDepthBaseX;
    int _PeripheralDepthBaseY;
    int _PeripheralMotionVectorBaseX;
    int _PeripheralMotionVectorBaseY;
    int _PeripheralMotionVectorWidth;
    int _PeripheralMotionVectorHeight;
    float _PeripheralMotionVectorScaleX;
    float _PeripheralMotionVectorScaleY;
    int _PeripheralMotionVectorsLowResolution;
    int _PeripheralMotionVectorsJittered;
    float _PreviousJitterOffsetX;
    float _PreviousJitterOffsetY;
    int _MotionVectorDebugView;
    int _MotionVectorWidth;
    int _MotionVectorHeight;
    float _MotionVectorScaleX;
    float _MotionVectorScaleY;
};

Texture2D<float4> PeripheralColorTexture : register(t0);
Texture2D<float4> DlssRoiOutput : register(t1);
RWTexture2D<float4> FinalOutput : register(u0);
SamplerState LinearClampSampler : register(s0);

float4 SamplePeripheral(float2 uv)
{
    if (_PeripheralJitterCancel != 0)
    {
        float sign = _PeripheralJitterSign < 0 ? -1.0f : 1.0f;
        uv += sign * float2(_JitterOffsetX, _JitterOffsetY) / float2((float)_SrcWidth, (float)_SrcHeight);
    }

    float2 sourcePixel = uv * float2((float)_SrcWidth, (float)_SrcHeight) + float2(_SrcBaseX, _SrcBaseY);
    float2 sourceUv = sourcePixel / float2((float)_SrcTextureWidth, (float)_SrcTextureHeight);
    return PeripheralColorTexture.SampleLevel(LinearClampSampler, sourceUv, 0.0f);
}

float RoiAlpha(int2 p)
{
    if (p.x < _RoiX || p.y < _RoiY || p.x >= _RoiX + _RoiWidth || p.y >= _RoiY + _RoiHeight)
        return 0.0f;

    if (_FeatherPx <= 0)
        return 1.0f;

    int left = p.x - _RoiX;
    int right = (_RoiX + _RoiWidth - 1) - p.x;
    int top = p.y - _RoiY;
    int bottom = (_RoiY + _RoiHeight - 1) - p.y;

    if (_RoiX <= 0)
        left = _FeatherPx;
    if (_RoiY <= 0)
        top = _FeatherPx;
    if (_RoiX + _RoiWidth >= _DstWidth)
        right = _FeatherPx;
    if (_RoiY + _RoiHeight >= _DstHeight)
        bottom = _FeatherPx;

    int edgeDistance = min(min(left, right), min(top, bottom));
    return saturate((float)edgeDistance / (float)_FeatherPx);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_DstWidth || p.y >= (uint)_DstHeight)
        return;

    float2 uv = (float2(p) + 0.5f) / float2((float)_DstWidth, (float)_DstHeight);
    float alpha = RoiAlpha((int2)p);
    float4 result = float4(0.0f, 0.0f, 0.0f, 1.0f);

    if (alpha >= 1.0f)
    {
        uint2 roiPixel = p - uint2((uint)_RoiX, (uint)_RoiY);
        result = DlssRoiOutput.Load(int3(roiPixel, 0));
    }
    else
    {
        float4 peripheral = SamplePeripheral(uv);
        if (alpha > 0.0f)
        {
            uint2 roiPixel = p - uint2((uint)_RoiX, (uint)_RoiY);
            float4 roi = DlssRoiOutput.Load(int3(roiPixel, 0));
            result = lerp(peripheral, roi, alpha);
        }
        else
        {
            result = peripheral;
        }
    }

    result.a = 1.0f;
    FinalOutput[p + uint2(_DstBaseX, _DstBaseY)] = result;
}
)";

static const char* gazeRoiDebugOverlayShader = R"(
cbuffer Params : register(b0)
{
    int _SrcWidth;
    int _SrcHeight;
    int _SrcTextureWidth;
    int _SrcTextureHeight;
    int _SrcBaseX;
    int _SrcBaseY;
    int _DstWidth;
    int _DstHeight;
    int _DstBaseX;
    int _DstBaseY;
    int _RoiX;
    int _RoiY;
    int _RoiWidth;
    int _RoiHeight;
    int _FeatherPx;
    int _DebugBorderPx;
    int _PeripheralBlur;
    float _PeripheralBlurRadius;
    int _PeripheralJitterCancel;
    int _PeripheralJitterSign;
    float _JitterOffsetX;
    float _JitterOffsetY;
    int _PeripheralTemporal;
    float _PeripheralTemporalHistoryWeight;
    float _PeripheralTemporalReactiveScale;
    int _PeripheralTemporalInitialized;
    int _PeripheralTemporalMotionReprojection;
    int _PeripheralTemporalHistoryReset;
    int _PeripheralDepthBaseX;
    int _PeripheralDepthBaseY;
    int _PeripheralMotionVectorBaseX;
    int _PeripheralMotionVectorBaseY;
    int _PeripheralMotionVectorWidth;
    int _PeripheralMotionVectorHeight;
    float _PeripheralMotionVectorScaleX;
    float _PeripheralMotionVectorScaleY;
    int _PeripheralMotionVectorsLowResolution;
    int _PeripheralMotionVectorsJittered;
    float _PreviousJitterOffsetX;
    float _PreviousJitterOffsetY;
    int _MotionVectorDebugView;
    int _MotionVectorWidth;
    int _MotionVectorHeight;
    float _MotionVectorScaleX;
    float _MotionVectorScaleY;
};

Texture2D<float4> PatchedMotionVectors : register(t0);
RWTexture2D<float4> FinalOutput : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_DstWidth || p.y >= (uint)_DstHeight)
        return;

    if (_DebugBorderPx > 0 && p.x >= (uint)_RoiX && p.y >= (uint)_RoiY &&
        p.x < (uint)(_RoiX + _RoiWidth) && p.y < (uint)(_RoiY + _RoiHeight))
    {
        int left = (int)p.x - _RoiX;
        int right = (_RoiX + _RoiWidth - 1) - (int)p.x;
        int top = (int)p.y - _RoiY;
        int bottom = (_RoiY + _RoiHeight - 1) - (int)p.y;
        int edgeDistance = min(min(left, right), min(top, bottom));
        if (edgeDistance < _DebugBorderPx)
            FinalOutput[p + uint2(_DstBaseX, _DstBaseY)] = float4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    if (_MotionVectorDebugView != 0 && _MotionVectorWidth > 0 && _MotionVectorHeight > 0)
    {
        int debugWidth = min(_MotionVectorWidth, _DstWidth);
        int debugHeight = min(_MotionVectorHeight, _DstHeight);
        int debugLeft = _DstWidth - debugWidth;
        if ((int)p.x >= debugLeft && (int)p.y < debugHeight)
        {
            uint2 motionPixel = uint2((int)p.x - debugLeft, (int)p.y);
            float2 rawMotion = PatchedMotionVectors.Load(int3(motionPixel, 0)).xy;
            float2 pixelMotion = rawMotion * float2(_MotionVectorScaleX, _MotionVectorScaleY);
            const float visibleRangePixels = 32.0f;
            float2 signedMotion = saturate(0.5f + pixelMotion / (2.0f * visibleRangePixels));
            float magnitude = saturate(length(pixelMotion) / visibleRangePixels);
            FinalOutput[p + uint2(_DstBaseX, _DstBaseY)] =
                float4(signedMotion.x, signedMotion.y, magnitude, 1.0f);
        }
    }
}
)";

static const char* gazeRoiDepthDebugShader = R"(
cbuffer Params : register(b0)
{
    int _DepthWidth;
    int _DepthHeight;
    int _DepthTextureWidth;
    int _DepthTextureHeight;
    int _DepthBaseX;
    int _DepthBaseY;
    int _DstWidth;
    int _DstHeight;
    int _DstBaseX;
    int _DstBaseY;
};

Texture2D<float> DepthTexture : register(t0);
RWTexture2D<float4> FinalOutput : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_DepthWidth || p.y >= (uint)_DepthHeight ||
        p.x >= (uint)_DstWidth || p.y >= (uint)_DstHeight)
        return;

    uint2 sourcePixel = p + uint2((uint)_DepthBaseX, (uint)_DepthBaseY);
    if (sourcePixel.x >= (uint)_DepthTextureWidth || sourcePixel.y >= (uint)_DepthTextureHeight)
        return;
    float depth = DepthTexture.Load(int3(sourcePixel, 0));
    // Keep the source texel mapping exact; only clamp to the displayable
    // grayscale range because depth formats may contain values outside [0,1].
    depth = saturate(depth);
    FinalOutput[p + uint2((uint)_DstBaseX, (uint)_DstBaseY)] = float4(depth, depth, depth, 1.0f);
}
)";

static const char* gazeRoiPeripheralShader = R"(
cbuffer Params : register(b0)
{
    int _SrcWidth;
    int _SrcHeight;
    int _SrcTextureWidth;
    int _SrcTextureHeight;
    int _SrcBaseX;
    int _SrcBaseY;
    int _DstWidth;
    int _DstHeight;
    int _DstBaseX;
    int _DstBaseY;
    int _RoiX;
    int _RoiY;
    int _RoiWidth;
    int _RoiHeight;
    int _FeatherPx;
    int _DebugBorderPx;
    int _PeripheralBlur;
    float _PeripheralBlurRadius;
    int _PeripheralJitterCancel;
    int _PeripheralJitterSign;
    float _JitterOffsetX;
    float _JitterOffsetY;
    int _PeripheralTemporal;
    float _PeripheralTemporalHistoryWeight;
    float _PeripheralTemporalReactiveScale;
    int _PeripheralTemporalInitialized;
    int _PeripheralTemporalMotionReprojection;
    int _PeripheralTemporalHistoryReset;
    int _PeripheralDepthBaseX;
    int _PeripheralDepthBaseY;
    int _PeripheralMotionVectorBaseX;
    int _PeripheralMotionVectorBaseY;
    int _PeripheralMotionVectorWidth;
    int _PeripheralMotionVectorHeight;
    float _PeripheralMotionVectorScaleX;
    float _PeripheralMotionVectorScaleY;
    int _PeripheralMotionVectorsLowResolution;
    int _PeripheralMotionVectorsJittered;
    float _PreviousJitterOffsetX;
    float _PreviousJitterOffsetY;
    int _MotionVectorDebugView;
    int _MotionVectorWidth;
    int _MotionVectorHeight;
    float _MotionVectorScaleX;
    float _MotionVectorScaleY;
};

Texture2D<float4> LowResColor : register(t0);
Texture2D<float4> PeripheralHistory : register(t1);
Texture2D<float4> PeripheralMotionVectors : register(t2);
Texture2D<float> CurrentDepth : register(t3);
Texture2D<float> PreviousDepth : register(t4);
RWTexture2D<float4> PeripheralOutput : register(u0);
RWTexture2D<float> NextDepth : register(u1);
SamplerState LinearClampSampler : register(s0);

float4 SampleSource(float2 uv)
{
    if (_PeripheralJitterCancel != 0)
    {
        float sign = _PeripheralJitterSign < 0 ? -1.0f : 1.0f;
        uv += sign * float2(_JitterOffsetX, _JitterOffsetY) / float2((float)_SrcWidth, (float)_SrcHeight);
    }

    float2 sourcePixel = uv * float2((float)_SrcWidth, (float)_SrcHeight) + float2(_SrcBaseX, _SrcBaseY);
    float2 sourceUv = sourcePixel / float2((float)_SrcTextureWidth, (float)_SrcTextureHeight);
    return LowResColor.SampleLevel(LinearClampSampler, sourceUv, 0.0f);
}

void EvaluateCurrentPeripheral(float2 uv, out float4 current, out float3 minColor, out float3 maxColor)
{
    const bool blur = _PeripheralBlur != 0 && _PeripheralBlurRadius > 0.0f;
    const float2 pixelUv = 1.0f / float2((float)_SrcWidth, (float)_SrcHeight);
    const float2 blurUv = _PeripheralBlurRadius * pixelUv;
    minColor = float3(1e20f, 1e20f, 1e20f);
    maxColor = float3(-1e20f, -1e20f, -1e20f);
    float4 center = 0.0f;
    float4 blurSum = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            const float2 offset = float2((float)x, (float)y);
            const float4 sampleColor = SampleSource(uv + (blur ? blurUv : pixelUv) * offset);
            if (x == 0 && y == 0)
                center = sampleColor;
            minColor = min(minColor, sampleColor.rgb);
            maxColor = max(maxColor, sampleColor.rgb);
            const float weight = (x == 0 && y == 0) ? 4.0f : ((x == 0 || y == 0) ? 2.0f : 1.0f);
            blurSum += sampleColor * weight;
        }
    }

    current = blur ? blurSum * (1.0f / 16.0f) : center;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_SrcWidth || p.y >= (uint)_SrcHeight)
        return;

    float2 uv = (float2(p) + 0.5f) / float2((float)_SrcWidth, (float)_SrcHeight);
    float4 current;
    float3 minColor;
    float3 maxColor;
    EvaluateCurrentPeripheral(uv, current, minColor, maxColor);

    if (_PeripheralTemporal == 0 || _PeripheralTemporalInitialized == 0)
    {
        PeripheralOutput[p] = current;
        return;
    }

    float4 history = PeripheralHistory.Load(int3(p, 0));
    bool acceptHistory = true;
    if (_PeripheralTemporalMotionReprojection != 0)
    {
        float2 mvCoordinate = (float2(p) + 0.5f) *
                              float2((float)_PeripheralMotionVectorWidth, (float)_PeripheralMotionVectorHeight) /
                              float2((float)_SrcWidth, (float)_SrcHeight);
        uint2 mvPixel = min(uint2(mvCoordinate),
                            uint2(_PeripheralMotionVectorWidth - 1, _PeripheralMotionVectorHeight - 1));
        float2 motion = PeripheralMotionVectors.Load(int3(mvPixel +
            uint2(_PeripheralMotionVectorBaseX, _PeripheralMotionVectorBaseY), 0)).xy;
        motion *= float2(_PeripheralMotionVectorScaleX, _PeripheralMotionVectorScaleY);
        if (_PeripheralMotionVectorsLowResolution == 0)
            motion *= float2((float)_SrcWidth, (float)_SrcHeight) /
                      float2((float)_PeripheralMotionVectorWidth, (float)_PeripheralMotionVectorHeight);
        if (_PeripheralMotionVectorsJittered != 0)
            motion += float2(_PreviousJitterOffsetX - _JitterOffsetX,
                             _PreviousJitterOffsetY - _JitterOffsetY);

        float2 previousPixel = float2(p) + motion;
        bool inBounds = all(previousPixel >= 0.0f) &&
                        all(previousPixel < float2((float)_SrcWidth, (float)_SrcHeight));
        uint2 previousPoint = min(uint2(max(previousPixel + 0.5f, 0.0f)),
                                  uint2(_SrcWidth - 1, _SrcHeight - 1));
        float currentDepth = CurrentDepth.Load(int3(p + uint2(_PeripheralDepthBaseX, _PeripheralDepthBaseY), 0));
        float previousDepth = PreviousDepth.Load(int3(previousPoint, 0));
        float depthThreshold = max(0.001f, max(abs(currentDepth), abs(previousDepth)) * 0.02f);
        bool depthMatches = isfinite(currentDepth) && isfinite(previousDepth) &&
                            abs(currentDepth - previousDepth) <= depthThreshold;
        acceptHistory = inBounds && depthMatches && _PeripheralTemporalHistoryReset == 0;
        if (inBounds)
        {
            float2 previousUv = (previousPixel + 0.5f) / float2((float)_SrcWidth, (float)_SrcHeight);
            history = PeripheralHistory.SampleLevel(LinearClampSampler, previousUv, 0.0f);
        }
    }

    float3 clippedHistory = clamp(history.rgb, minColor, maxColor);
    float colorDelta = max(max(abs(current.r - clippedHistory.r), abs(current.g - clippedHistory.g)),
                           abs(current.b - clippedHistory.b));
    float reactiveWeight = saturate(colorDelta * _PeripheralTemporalReactiveScale);
    // The control is the fraction of validated history to retain. Reactive
    // changes reduce that fraction; they no longer replace it with a fixed
    // current-frame weight, which made the old slider effectively inert.
    float historyWeight = saturate(_PeripheralTemporalHistoryWeight) * (1.0f - reactiveWeight);
    float currentWeight = 1.0f - historyWeight;
    PeripheralOutput[p] = acceptHistory
                              ? float4(lerp(clippedHistory, current.rgb, currentWeight), current.a)
                              : current;
    if (_PeripheralTemporalMotionReprojection != 0)
        NextDepth[p] = CurrentDepth.Load(int3(p + uint2(_PeripheralDepthBaseX, _PeripheralDepthBaseY), 0));
}
)";

static const char* gazeRoiMvShader = R"(
cbuffer Params : register(b0)
{
    int _Width;
    int _Height;
    int _SourceBaseX;
    int _SourceBaseY;
    float _RawOffsetX;
    float _RawOffsetY;
};

Texture2D<float2> SourceMotionVectors : register(t0);
RWTexture2D<float2> PatchedMotionVectors : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_Width || p.y >= (uint)_Height)
        return;

    float2 mv = SourceMotionVectors.Load(int3(p + uint2(_SourceBaseX, _SourceBaseY), 0));
    mv += float2(_RawOffsetX, _RawOffsetY);
    PatchedMotionVectors[p] = mv;
}
)";

static const char* gazeRoiMvShaderFourComponent = R"(
cbuffer Params : register(b0)
{
    int _Width;
    int _Height;
    int _SourceBaseX;
    int _SourceBaseY;
    float _RawOffsetX;
    float _RawOffsetY;
};

Texture2D<float4> SourceMotionVectors : register(t0);
RWTexture2D<float4> PatchedMotionVectors : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_Width || p.y >= (uint)_Height)
        return;

    float4 mv = SourceMotionVectors.Load(int3(p + uint2(_SourceBaseX, _SourceBaseY), 0));
    mv.xy += float2(_RawOffsetX, _RawOffsetY);
    PatchedMotionVectors[p] = mv;
}
)";

static const char* gazeRoiDepthCropShader = R"(
cbuffer Params : register(b0)
{
    int _Width;
    int _Height;
    int _SourceBaseX;
    int _SourceBaseY;
};

Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> CroppedDepth : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_Width || p.y >= (uint)_Height)
        return;

    CroppedDepth[p] = SourceDepth.Load(int3(p + uint2(_SourceBaseX, _SourceBaseY), 0));
}
)";

static const char* gazeRoiColorCropShader = R"(
cbuffer Params : register(b0)
{
    int _SourceWidth;
    int _SourceHeight;
    int _SourceBaseX;
    int _SourceBaseY;
    int _OutputWidth;
    int _OutputHeight;
};

Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> CroppedColor : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_SourceWidth || p.y >= (uint)_SourceHeight)
        return;

    CroppedColor[p] = SourceColor.Load(int3(p + uint2(_SourceBaseX, _SourceBaseY), 0));
}
)";

static const char* gazeRoiColorPointBypassShader = R"(
cbuffer Params : register(b0)
{
    int _SourceWidth;
    int _SourceHeight;
    int _SourceBaseX;
    int _SourceBaseY;
    int _OutputWidth;
    int _OutputHeight;
};

Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> PrivateOutput : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (p.x >= (uint)_OutputWidth || p.y >= (uint)_OutputHeight ||
        _SourceWidth <= 0 || _SourceHeight <= 0)
        return;

    uint2 sourceSize = uint2(_SourceWidth, _SourceHeight);
    uint2 outputSize = uint2(_OutputWidth, _OutputHeight);
    uint2 sourcePixel = min(uint2((float2(p) + 0.5f) * float2(sourceSize) / float2(outputSize)),
                            sourceSize - 1);
    PrivateOutput[p] = SourceColor.Load(int3(sourcePixel + uint2(_SourceBaseX, _SourceBaseY), 0));
}
)";

static bool IsSupportedMotionVectorFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return true;
    default:
        return false;
    }
}

static bool IsFourComponentMotionVectorFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS;
}

bool GazeRoiMvPatch_Dx12::CreatePatchedResource(ID3D12Device* device, ID3D12Resource* motionVectorTemplate,
                                                 uint32_t width, uint32_t height,
                                                 D3D12_RESOURCE_STATES initialState)
{
    if (device == nullptr || motionVectorTemplate == nullptr || width == 0 || height == 0)
        return false;

    auto desc = motionVectorTemplate->GetDesc();
    if (!IsSupportedMotionVectorFormat(desc.Format))
    {
        LOG_WARN("[{}] Unsupported MV format for ROI patch: {}", _name, static_cast<uint32_t>(desc.Format));
        return false;
    }

    _motionVectorFormat = desc.Format;

    desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    desc.Width = width;
    desc.Height = height;

    if (_patchedMotionVectors != nullptr)
    {
        auto oldDesc = _patchedMotionVectors->GetDesc();
        if (oldDesc.Width == desc.Width && oldDesc.Height == desc.Height && oldDesc.Format == desc.Format &&
            oldDesc.Flags == desc.Flags)
        {
            return true;
        }

        GazeRoiFrameSync::DeferRelease(_patchedMotionVectors);
        _patchedMotionVectors = nullptr;
        std::fill(std::begin(_slotPatchedMotionVectors), std::end(_slotPatchedMotionVectors), nullptr);
    }

    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags {};
    HRESULT hr = motionVectorTemplate->GetHeapProperties(&heapProperties, &heapFlags);
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] GetHeapProperties result: {:X}", _name, (UINT64)hr);
        return false;
    }

    hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                         IID_PPV_ARGS(&_patchedMotionVectors));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource result: {:X}", _name, (UINT64)hr);
        return false;
    }

    _patchedMotionVectors->SetName(L"GazeRoi_Patched_MotionVectors");
    _patchedMotionVectorsState = initialState;
    LOG_INFO("[{}] Created patched MV resource: {}x{}", _name, desc.Width, desc.Height);
    return true;
}

void GazeRoiMvPatch_Dx12::SetPatchedState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state)
{
    Shader_Dx12::SetBufferState(commandList, state, _patchedMotionVectors, &_patchedMotionVectorsState);
}

bool GazeRoiMvPatch_Dx12::Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceMotionVectors,
                                   const GazeRoiMvConstants& constants, uint32_t frameSlot)
{
    if (!_init || _device == nullptr || commandList == nullptr || sourceMotionVectors == nullptr ||
        _patchedMotionVectors == nullptr || frameSlot >= GAZE_ROI_MV_NUM_OF_HEAPS)
    {
        return false;
    }

    FrameDescriptorHeap& currentHeap = _frameHeaps[frameSlot];

    try
    {
        CreateShaderResourceView(_device, sourceMotionVectors, currentHeap.GetSrvCPU(0));
        if (_slotPatchedMotionVectors[frameSlot] != _patchedMotionVectors)
        {
            CreateUnorderedAccessView(_device, _patchedMotionVectors, currentHeap.GetUavCPU(0), 0);
            _slotPatchedMotionVectors[frameSlot] = _patchedMotionVectors;
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[{}] Failed to create MV patch descriptors: {}", _name, e.what());
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetComputeRootSignature(_rootSignature);
    ID3D12PipelineState* pipelineState = IsFourComponentMotionVectorFormat(_motionVectorFormat)
                                             ? _fourComponentPipelineState
                                             : _pipelineState;
    if (pipelineState == nullptr)
    {
        LOG_ERROR("[{}] Missing MV patch pipeline for format {}", _name, static_cast<uint32_t>(_motionVectorFormat));
        return false;
    }
    commandList->SetPipelineState(pipelineState);
    commandList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());
    commandList->SetComputeRoot32BitConstants(1, GAZE_ROI_MV_CONSTANT_DWORDS, &constants, 0);

    const UINT dispatchWidth = (constants.width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (constants.height + _numThreadsY - 1) / _numThreadsY;
    commandList->Dispatch(dispatchWidth, dispatchHeight, 1);
    return true;
}

GazeRoiMvPatch_Dx12::GazeRoiMvPatch_Dx12(std::string name, ID3D12Device* device) : Shader_Dx12(name, device)
{
    if (device == nullptr)
    {
        LOG_ERROR("[{}] Device is nullptr", _name);
        return;
    }

    if (!SetupRootSignatureWithConstants(device, 1, 1, GAZE_ROI_MV_CONSTANT_DWORDS))
    {
        LOG_ERROR("[{}] Failed to setup root signature", _name);
        return;
    }


    ID3DBlob* shaderBlob = CompileShader(gazeRoiMvShader, "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
    {
        LOG_ERROR("[{}] CompileShader failed", _name);
        return;
    }

    bool psoCreated = CreateComputeShader(device, _rootSignature, &_pipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    if (!psoCreated)
    {
        LOG_ERROR("[{}] CreateComputeShader composite failed", _name);
        return;
    }

    shaderBlob = CompileShader(gazeRoiMvShaderFourComponent, "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
    {
        LOG_ERROR("[{}] CompileShader for four-component MV failed", _name);
        SAFE_RELEASE(_pipelineState);
        return;
    }

    psoCreated = CreateComputeShader(device, _rootSignature, &_fourComponentPipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    if (!psoCreated)
    {
        LOG_ERROR("[{}] CreateComputeShader for four-component MV failed", _name);
        SAFE_RELEASE(_pipelineState);
        return;
    }

    _init = InitHeaps(device, _frameHeaps, GAZE_ROI_MV_NUM_OF_HEAPS);
}

GazeRoiMvPatch_Dx12::~GazeRoiMvPatch_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < GAZE_ROI_MV_NUM_OF_HEAPS; i++)
        _frameHeaps[i].ReleaseHeaps();

    SAFE_RELEASE(_fourComponentPipelineState);
    SAFE_RELEASE(_patchedMotionVectors);
}

static bool IsSupportedDepthCropFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return true;
    default:
        return false;
    }
}

bool GazeRoiDepthCrop_Dx12::CreateCroppedResource(ID3D12Device* device, ID3D12Resource* depthTemplate,
                                                   uint32_t width, uint32_t height,
                                                   D3D12_RESOURCE_STATES initialState)
{
    if (device == nullptr || depthTemplate == nullptr || width == 0 || height == 0)
        return false;

    const auto sourceDesc = depthTemplate->GetDesc();
    if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sourceDesc.DepthOrArraySize != 1 ||
        sourceDesc.SampleDesc.Count != 1 || !IsSupportedDepthCropFormat(sourceDesc.Format) ||
        (sourceDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0)
    {
        LOG_ERROR("[{}] Unsupported depth crop resource: dimension={} size={}x{} format={} flags=0x{:X} "
                  "array={} samples={}",
                  _name, static_cast<uint32_t>(sourceDesc.Dimension), sourceDesc.Width, sourceDesc.Height,
                  static_cast<uint32_t>(sourceDesc.Format), static_cast<uint32_t>(sourceDesc.Flags),
                  sourceDesc.DepthOrArraySize, sourceDesc.SampleDesc.Count);
        return false;
    }

    const D3D12_RESOURCE_FLAGS flags =
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, width, height, 1, 1, 1, 0, flags);

    if (_croppedDepth != nullptr)
    {
        const auto oldDesc = _croppedDepth->GetDesc();
        if (oldDesc.Width == desc.Width && oldDesc.Height == desc.Height && oldDesc.Format == desc.Format &&
            oldDesc.Flags == desc.Flags)
        {
            return true;
        }

        GazeRoiFrameSync::DeferRelease(_croppedDepth);
        _croppedDepth = nullptr;
    }

    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags {};
    HRESULT hr = depthTemplate->GetHeapProperties(&heapProperties, &heapFlags);
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] GetHeapProperties depth result: {:X}", _name, static_cast<UINT64>(hr));
        return false;
    }

    hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                         IID_PPV_ARGS(&_croppedDepth));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource depth result: {:X}", _name, static_cast<UINT64>(hr));
        return false;
    }

    _croppedDepth->SetName(L"GazeRoi_Cropped_Depth");
    _croppedDepthState = initialState;
    LOG_INFO("[{}] Created cropped depth resource: {}x{} R32_FLOAT from source format {}", _name, width, height,
             static_cast<uint32_t>(sourceDesc.Format));
    return true;
}

void GazeRoiDepthCrop_Dx12::SetCroppedState(ID3D12GraphicsCommandList* commandList,
                                             D3D12_RESOURCE_STATES state)
{
    Shader_Dx12::SetBufferState(commandList, state, _croppedDepth, &_croppedDepthState);
}

bool GazeRoiDepthCrop_Dx12::Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceDepth,
                                     const GazeRoiDepthConstants& constants, uint32_t frameSlot)
{
    if (!_init || _device == nullptr || commandList == nullptr || sourceDepth == nullptr || _croppedDepth == nullptr ||
        frameSlot >= GAZE_ROI_DEPTH_NUM_OF_HEAPS)
        return false;

    FrameDescriptorHeap& currentHeap = _frameHeaps[frameSlot];

    try
    {
        CreateShaderResourceView(_device, sourceDepth, currentHeap.GetSrvCPU(0));
        CreateUnorderedAccessView(_device, _croppedDepth, currentHeap.GetUavCPU(0), 0);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[{}] Failed to create depth crop descriptors: {}", _name, e.what());
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetComputeRootSignature(_rootSignature);
    commandList->SetPipelineState(_pipelineState);
    commandList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());
    commandList->SetComputeRoot32BitConstants(1, GAZE_ROI_DEPTH_CONSTANT_DWORDS, &constants, 0);

    const UINT dispatchWidth = (constants.width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (constants.height + _numThreadsY - 1) / _numThreadsY;
    commandList->Dispatch(dispatchWidth, dispatchHeight, 1);
    return true;
}

GazeRoiDepthCrop_Dx12::GazeRoiDepthCrop_Dx12(std::string name, ID3D12Device* device)
    : Shader_Dx12(name, device)
{
    if (device == nullptr)
    {
        LOG_ERROR("[{}] Device is nullptr", _name);
        return;
    }

    if (!SetupRootSignatureWithConstants(device, 1, 1, GAZE_ROI_DEPTH_CONSTANT_DWORDS))
    {
        LOG_ERROR("[{}] Failed to setup depth crop root signature", _name);
        return;
    }


    ID3DBlob* shaderBlob = CompileShader(gazeRoiDepthCropShader, "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
    {
        LOG_ERROR("[{}] CompileShader depth crop failed", _name);
        return;
    }

    const bool psoCreated = CreateComputeShader(device, _rootSignature, &_pipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    if (!psoCreated)
    {
        LOG_ERROR("[{}] CreateComputeShader depth crop failed", _name);
        return;
    }

    _init = InitHeaps(device, _frameHeaps, GAZE_ROI_DEPTH_NUM_OF_HEAPS);
}

GazeRoiDepthCrop_Dx12::~GazeRoiDepthCrop_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < GAZE_ROI_DEPTH_NUM_OF_HEAPS; i++)
        _frameHeaps[i].ReleaseHeaps();

    SAFE_RELEASE(_croppedDepth);
}

static bool IsSupportedColorCropFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return true;
    default:
        return false;
    }
}

bool GazeRoiColorCrop_Dx12::CreateCroppedResource(ID3D12Device* device, ID3D12Resource* colorTemplate,
                                                   uint32_t width, uint32_t height,
                                                   D3D12_RESOURCE_STATES initialState)
{
    if (device == nullptr || colorTemplate == nullptr || width == 0 || height == 0)
        return false;

    const auto sourceDesc = colorTemplate->GetDesc();
    if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sourceDesc.DepthOrArraySize != 1 ||
        sourceDesc.SampleDesc.Count != 1 || !IsSupportedColorCropFormat(sourceDesc.Format) ||
        (sourceDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0)
    {
        LOG_ERROR("[{}] Unsupported color crop resource: dimension={} size={}x{} format={} flags=0x{:X} "
                  "array={} samples={}",
                  _name, static_cast<uint32_t>(sourceDesc.Dimension), sourceDesc.Width, sourceDesc.Height,
                  static_cast<uint32_t>(sourceDesc.Format), static_cast<uint32_t>(sourceDesc.Flags),
                  sourceDesc.DepthOrArraySize, sourceDesc.SampleDesc.Count);
        return false;
    }

    auto desc = sourceDesc;
    desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    if (_croppedColor != nullptr)
    {
        const auto oldDesc = _croppedColor->GetDesc();
        if (oldDesc.Width == desc.Width && oldDesc.Height == desc.Height && oldDesc.Format == desc.Format &&
            oldDesc.Flags == desc.Flags)
        {
            return true;
        }

        GazeRoiFrameSync::DeferRelease(_croppedColor);
        _croppedColor = nullptr;
    }

    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags {};
    HRESULT hr = colorTemplate->GetHeapProperties(&heapProperties, &heapFlags);
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] GetHeapProperties color result: {:X}", _name, static_cast<UINT64>(hr));
        return false;
    }

    hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                         IID_PPV_ARGS(&_croppedColor));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource color result: {:X}", _name, static_cast<UINT64>(hr));
        return false;
    }

    _croppedColor->SetName(L"GazeRoi_Cropped_Color");
    _croppedColorState = initialState;
    LOG_INFO("[{}] Created cropped color resource: {}x{} format {}", _name, width, height,
             static_cast<uint32_t>(desc.Format));
    return true;
}

void GazeRoiColorCrop_Dx12::SetCroppedState(ID3D12GraphicsCommandList* commandList,
                                             D3D12_RESOURCE_STATES state)
{
    Shader_Dx12::SetBufferState(commandList, state, _croppedColor, &_croppedColorState);
}

bool GazeRoiColorCrop_Dx12::Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceColor,
                                     const GazeRoiColorConstants& constants, uint32_t frameSlot)
{
    if (!_init || _device == nullptr || commandList == nullptr || sourceColor == nullptr || _croppedColor == nullptr ||
        frameSlot >= GAZE_ROI_COLOR_NUM_OF_HEAPS)
        return false;

    FrameDescriptorHeap& currentHeap = _frameHeaps[frameSlot];

    try
    {
        CreateShaderResourceView(_device, sourceColor, currentHeap.GetSrvCPU(0));
        CreateUnorderedAccessView(_device, _croppedColor, currentHeap.GetUavCPU(0), 0);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[{}] Failed to create color crop descriptors: {}", _name, e.what());
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetComputeRootSignature(_rootSignature);
    commandList->SetPipelineState(_pipelineState);
    commandList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());
    commandList->SetComputeRoot32BitConstants(1, GAZE_ROI_COLOR_CONSTANT_DWORDS, &constants, 0);

    const UINT dispatchWidth = (constants.sourceWidth + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (constants.sourceHeight + _numThreadsY - 1) / _numThreadsY;
    commandList->Dispatch(dispatchWidth, dispatchHeight, 1);
    return true;
}

GazeRoiColorCrop_Dx12::GazeRoiColorCrop_Dx12(std::string name, ID3D12Device* device)
    : Shader_Dx12(name, device)
{
    if (device == nullptr)
    {
        LOG_ERROR("[{}] Device is nullptr", _name);
        return;
    }

    if (!SetupRootSignatureWithConstants(device, 1, 1, GAZE_ROI_COLOR_CONSTANT_DWORDS))
    {
        LOG_ERROR("[{}] Failed to setup color crop root signature", _name);
        return;
    }

    ID3DBlob* shaderBlob = CompileShader(gazeRoiColorCropShader, "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
    {
        LOG_ERROR("[{}] CompileShader color crop failed", _name);
        return;
    }

    const bool psoCreated = CreateComputeShader(device, _rootSignature, &_pipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    if (!psoCreated)
    {
        LOG_ERROR("[{}] CreateComputeShader color crop failed", _name);
        return;
    }

    _init = InitHeaps(device, _frameHeaps, GAZE_ROI_COLOR_NUM_OF_HEAPS);
}

GazeRoiColorCrop_Dx12::~GazeRoiColorCrop_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < GAZE_ROI_COLOR_NUM_OF_HEAPS; i++)
        _frameHeaps[i].ReleaseHeaps();

    SAFE_RELEASE(_croppedColor);
}

bool GazeRoi_Dx12::CreateDlssOutputResource(ID3D12Device* device, ID3D12Resource* outputTemplate, uint32_t width,
                                            uint32_t height, D3D12_RESOURCE_STATES initialState)
{
    if (device == nullptr || outputTemplate == nullptr || width == 0 || height == 0)
        return false;

    auto desc = outputTemplate->GetDesc();
    desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    desc.Width = width;
    desc.Height = height;

    if (_dlssOutput != nullptr)
    {
        auto oldDesc = _dlssOutput->GetDesc();
        if (oldDesc.Width == desc.Width && oldDesc.Height == desc.Height && oldDesc.Format == desc.Format &&
            oldDesc.Flags == desc.Flags)
        {
            return true;
        }

        GazeRoiFrameSync::DeferRelease(_dlssOutput);
        _dlssOutput = nullptr;
        std::fill(std::begin(_slotDlssOutputs), std::end(_slotDlssOutputs), nullptr);
    }

    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags {};
    HRESULT hr = outputTemplate->GetHeapProperties(&heapProperties, &heapFlags);
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] GetHeapProperties result: {:X}", _name, (UINT64)hr);
        return false;
    }

    hr = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                         IID_PPV_ARGS(&_dlssOutput));
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] CreateCommittedResource result: {:X}", _name, (UINT64)hr);
        return false;
    }

    _dlssOutput->SetName(L"GazeRoi_DLSS_Output");
    _dlssOutputState = initialState;
    LOG_INFO("[{}] Created DLSS ROI output: {}x{}", _name, desc.Width, desc.Height);
    return true;
}

bool GazeRoi_Dx12::EnsurePeripheralPipeline()
{
    if (_peripheralPipelineState == nullptr)
    {
        ID3DBlob* shaderBlob = CompileShader(gazeRoiPeripheralShader, "CSMain", "cs_5_0");
        if (shaderBlob == nullptr)
            return false;
        const bool created = CreateComputeShader(_device, _rootSignature, &_peripheralPipelineState, shaderBlob, {});
        SAFE_RELEASE(shaderBlob);
        if (!created)
            return false;
    }

    if (!_peripheralHeapsInitialized)
        _peripheralHeapsInitialized = InitHeaps(_device, _peripheralFrameHeaps, GAZE_ROI_NUM_OF_HEAPS);
    return _peripheralHeapsInitialized;
}

bool GazeRoi_Dx12::EnsureColorBypassPipeline()
{
    if (_colorBypassPipelineState == nullptr)
    {
        ID3DBlob* shaderBlob = CompileShader(gazeRoiColorPointBypassShader, "CSMain", "cs_5_0");
        if (shaderBlob == nullptr)
            return false;
        const bool created = CreateComputeShader(_device, _rootSignature, &_colorBypassPipelineState, shaderBlob, {});
        SAFE_RELEASE(shaderBlob);
        if (!created)
            return false;
    }

    if (!_colorBypassHeapsInitialized)
        _colorBypassHeapsInitialized = InitHeaps(_device, _colorBypassFrameHeaps, GAZE_ROI_NUM_OF_HEAPS);
    return _colorBypassHeapsInitialized;
}

bool GazeRoi_Dx12::EnsureOutputClearHeaps()
{
    if (_outputClearHeapsInitialized)
        return true;

    _outputClearHeapsInitialized = true;
    for (auto& heap : _outputClearFrameHeaps)
        _outputClearHeapsInitialized = _outputClearHeapsInitialized && heap.Initialize(_device, 0, 1, 0);
    return _outputClearHeapsInitialized;
}

bool GazeRoi_Dx12::EnsureDebugOverlayPipeline()
{
    if (_debugOverlayPipelineState == nullptr)
    {
        ID3DBlob* shaderBlob = CompileShader(gazeRoiDebugOverlayShader, "CSMain", "cs_5_0");
        if (shaderBlob == nullptr)
            return false;
        const bool created =
            CreateComputeShader(_device, _rootSignature, &_debugOverlayPipelineState, shaderBlob, {});
        SAFE_RELEASE(shaderBlob);
        if (!created)
            return false;
    }

    if (!_debugOverlayHeapsInitialized)
        _debugOverlayHeapsInitialized = InitHeaps(_device, _debugOverlayFrameHeaps, GAZE_ROI_NUM_OF_HEAPS);
    return _debugOverlayHeapsInitialized;
}

bool GazeRoi_Dx12::EnsureDepthDebugPipeline()
{
    if (_depthDebugPipelineState == nullptr)
    {
        ID3DBlob* shaderBlob = CompileShader(gazeRoiDepthDebugShader, "CSMain", "cs_5_0");
        if (shaderBlob == nullptr)
            return false;
        const bool created =
            CreateComputeShader(_device, _rootSignature, &_depthDebugPipelineState, shaderBlob, {});
        SAFE_RELEASE(shaderBlob);
        if (!created)
            return false;
    }

    if (!_depthDebugHeapsInitialized)
        _depthDebugHeapsInitialized = InitHeaps(_device, _depthDebugFrameHeaps, GAZE_ROI_NUM_OF_HEAPS);
    return _depthDebugHeapsInitialized;
}

bool GazeRoi_Dx12::CreatePeripheralResource(ID3D12Device* device, ID3D12Resource* colorTemplate, uint32_t width,
                                             uint32_t height, bool blurEnabled, bool temporalEnabled,
                                             bool motionReprojectionEnabled,
                                             D3D12_RESOURCE_STATES initialState)
{
    if (device == nullptr || colorTemplate == nullptr || width == 0 || height == 0)
        return false;

    auto desc = colorTemplate->GetDesc();
    desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    desc.Width = width;
    desc.Height = height;

    const bool needOutput = blurEnabled && !temporalEnabled;
    const bool needHistory = temporalEnabled;
    const bool needDepthHistory = temporalEnabled && motionReprojectionEnabled;
    const auto matches = [&desc](ID3D12Resource* resource)
    {
        if (resource == nullptr)
            return false;
        const auto oldDesc = resource->GetDesc();
        return oldDesc.Width == desc.Width && oldDesc.Height == desc.Height && oldDesc.Format == desc.Format &&
               oldDesc.Flags == desc.Flags;
    };
    const auto matchesDepth = [&desc](ID3D12Resource* resource)
    {
        if (resource == nullptr)
            return false;
        const auto oldDesc = resource->GetDesc();
        return oldDesc.Width == desc.Width && oldDesc.Height == desc.Height && oldDesc.Format == DXGI_FORMAT_R32_FLOAT &&
               oldDesc.Flags == desc.Flags;
    };

    const bool resourcesMatch = (!needOutput || matches(_peripheralOutput)) &&
                                 (needOutput || _peripheralOutput == nullptr) &&
                                 (!needHistory || (matches(_peripheralHistory[0]) && matches(_peripheralHistory[1]))) &&
                                 (needHistory || (_peripheralHistory[0] == nullptr && _peripheralHistory[1] == nullptr)) &&
                                 (!needDepthHistory ||
                                  (matchesDepth(_peripheralDepthHistory[0]) && matchesDepth(_peripheralDepthHistory[1]))) &&
                                 (needDepthHistory ||
                                  (_peripheralDepthHistory[0] == nullptr && _peripheralDepthHistory[1] == nullptr));
    if (resourcesMatch)
        return true;

    GazeRoiFrameSync::DeferRelease(_peripheralOutput);
    GazeRoiFrameSync::DeferRelease(_peripheralHistory[0]);
    GazeRoiFrameSync::DeferRelease(_peripheralHistory[1]);
    GazeRoiFrameSync::DeferRelease(_peripheralDepthHistory[0]);
    GazeRoiFrameSync::DeferRelease(_peripheralDepthHistory[1]);
    _peripheralOutput = nullptr;
    _peripheralHistory[0] = nullptr;
    _peripheralHistory[1] = nullptr;
    _peripheralDepthHistory[0] = nullptr;
    _peripheralDepthHistory[1] = nullptr;
    _peripheralHistoryInitialized = false;
    _peripheralHistoryIndex = 0;

    if (!needOutput && !needHistory && !needDepthHistory)
        return true;

    if (!EnsurePeripheralPipeline())
        return false;

    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags {};
    HRESULT hr = colorTemplate->GetHeapProperties(&heapProperties, &heapFlags);
    if (FAILED(hr))
    {
        LOG_ERROR("[{}] GetHeapProperties peripheral result: {:X}", _name, (UINT64)hr);
        return false;
    }

    const auto createSurface = [&](ID3D12Resource** resource, const wchar_t* name)
    {
        const HRESULT createResult = device->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(resource));
        if (FAILED(createResult))
        {
            LOG_ERROR("[{}] CreateCommittedResource peripheral result: {:X}", _name,
                      static_cast<UINT64>(createResult));
            return false;
        }
        (*resource)->SetName(name);
        return true;
    };

    if (needOutput)
    {
        if (!createSurface(&_peripheralOutput, L"GazeRoi_Peripheral_Output"))
            return false;
        _peripheralOutputState = initialState;
    }

    if (needHistory)
    {
        if (!createSurface(&_peripheralHistory[0], L"GazeRoi_Peripheral_History0") ||
            !createSurface(&_peripheralHistory[1], L"GazeRoi_Peripheral_History1"))
        {
            SAFE_RELEASE(_peripheralHistory[0]);
            SAFE_RELEASE(_peripheralHistory[1]);
            return false;
        }
        _peripheralHistoryState[0] = initialState;
        _peripheralHistoryState[1] = initialState;
    }

    if (needDepthHistory)
    {
        auto depthDesc = desc;
        depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
        const auto createDepthSurface = [&](ID3D12Resource** resource, const wchar_t* name)
        {
            const HRESULT createResult = device->CreateCommittedResource(
                &heapProperties, D3D12_HEAP_FLAG_NONE, &depthDesc, initialState, nullptr, IID_PPV_ARGS(resource));
            if (FAILED(createResult))
            {
                LOG_ERROR("[{}] CreateCommittedResource peripheral depth result: {:X}", _name,
                          static_cast<UINT64>(createResult));
                return false;
            }
            (*resource)->SetName(name);
            return true;
        };
        if (!createDepthSurface(&_peripheralDepthHistory[0], L"GazeRoi_Peripheral_DepthHistory0") ||
            !createDepthSurface(&_peripheralDepthHistory[1], L"GazeRoi_Peripheral_DepthHistory1"))
            return false;
        _peripheralDepthHistoryState[0] = initialState;
        _peripheralDepthHistoryState[1] = initialState;
    }

    LOG_INFO("[{}] Created peripheral resources mode={} size={}x{}", _name,
             needHistory ? "temporal-history-2" : "blur-output-1", desc.Width, desc.Height);
    return true;
}

void GazeRoi_Dx12::SetDlssOutputState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state)
{
    Shader_Dx12::SetBufferState(commandList, state, _dlssOutput, &_dlssOutputState);
}

bool GazeRoi_Dx12::DispatchCurrentColorPointBypass(ID3D12GraphicsCommandList* commandList,
                                                    ID3D12Resource* sourceColor,
                                                    const GazeRoiColorConstants& constants, uint32_t frameSlot)
{
    if (!_init || _device == nullptr || commandList == nullptr || sourceColor == nullptr || _dlssOutput == nullptr ||
        constants.sourceWidth <= 0 || constants.sourceHeight <= 0 ||
        constants.outputWidth <= 0 || constants.outputHeight <= 0 ||
        _dlssOutputState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS || frameSlot >= GAZE_ROI_NUM_OF_HEAPS)
    {
        return false;
    }

    if (!EnsureColorBypassPipeline())
        return false;

    FrameDescriptorHeap& currentHeap = _colorBypassFrameHeaps[frameSlot];

    try
    {
        // This shader reads only t0/u0; duplicate valid descriptors keep the shared root table complete.
        CreateShaderResourceView(_device, sourceColor, currentHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, sourceColor, currentHeap.GetSrvCPU(1));
        CreateShaderResourceView(_device, sourceColor, currentHeap.GetSrvCPU(2));
        CreateShaderResourceView(_device, sourceColor, currentHeap.GetSrvCPU(3));
        CreateShaderResourceView(_device, sourceColor, currentHeap.GetSrvCPU(4));
        CreateUnorderedAccessView(_device, _dlssOutput, currentHeap.GetUavCPU(0), 0);
        CreateUnorderedAccessView(_device, _dlssOutput, currentHeap.GetUavCPU(1), 0);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[{}] Failed to create current-color bypass descriptors: {}", _name, e.what());
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetComputeRootSignature(_rootSignature);
    commandList->SetPipelineState(_colorBypassPipelineState);
    commandList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());
    commandList->SetComputeRoot32BitConstants(1, GAZE_ROI_COLOR_CONSTANT_DWORDS, &constants, 0);

    const UINT dispatchWidth = (constants.outputWidth + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (constants.outputHeight + _numThreadsY - 1) / _numThreadsY;
    commandList->Dispatch(dispatchWidth, dispatchHeight, 1);

    const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(_dlssOutput);
    commandList->ResourceBarrier(1, &uavBarrier);
    return true;
}

bool GazeRoi_Dx12::ClearDlssOutputRectMagenta(ID3D12GraphicsCommandList* commandList, const GazeRoiRect& rect,
                                              uint32_t frameSlot)
{
    if (!_init || _device == nullptr || commandList == nullptr || _dlssOutput == nullptr || rect.width == 0 ||
        rect.height == 0 || _dlssOutputState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ||
        frameSlot >= GAZE_ROI_NUM_OF_HEAPS)
    {
        return false;
    }

    if (!EnsureOutputClearHeaps())
        return false;

    const auto desc = _dlssOutput->GetDesc();
    if (static_cast<uint64_t>(rect.x) + rect.width > desc.Width ||
        static_cast<uint64_t>(rect.y) + rect.height > desc.Height)
    {
        LOG_ERROR("[{}] Magenta pre-clear rect ({}, {}, {}x{}) exceeds private output {}x{}", _name, rect.x,
                  rect.y, rect.width, rect.height, desc.Width, desc.Height);
        return false;
    }

    FrameDescriptorHeap& clearHeap = _outputClearFrameHeaps[frameSlot];
    try
    {
        CreateUnorderedAccessView(_device, _dlssOutput, clearHeap.GetUavCPU(0), 0);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[{}] Failed to create private-output clear descriptor: {}", _name, e.what());
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { clearHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);

    const FLOAT clearColor[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
    const D3D12_RECT clearRect = { static_cast<LONG>(rect.x), static_cast<LONG>(rect.y),
                                   static_cast<LONG>(rect.x + rect.width),
                                   static_cast<LONG>(rect.y + rect.height) };
    commandList->ClearUnorderedAccessViewFloat(clearHeap.GetTableGPUStart(), clearHeap.GetUavCPU(0), _dlssOutput,
                                               clearColor, 1, &clearRect);

    const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(_dlssOutput);
    commandList->ResourceBarrier(1, &uavBarrier);
    return true;
}

void GazeRoi_Dx12::SetPeripheralOutputState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state)
{
    Shader_Dx12::SetBufferState(commandList, state, _peripheralOutput, &_peripheralOutputState);
}

bool GazeRoi_Dx12::Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* lowResColor,
                             ID3D12Resource* lowResDepth, ID3D12Resource* peripheralMotionVectors,
                             ID3D12Resource* patchedMotionVectors, ID3D12Resource* finalOutput,
                             const GazeRoiConstants& constants, uint32_t frameSlot)
{
    if (!_init || _device == nullptr || commandList == nullptr || lowResColor == nullptr || finalOutput == nullptr ||
        _dlssOutput == nullptr || frameSlot >= GAZE_ROI_NUM_OF_HEAPS ||
        (constants.peripheralTemporalMotionReprojection != 0 &&
         (lowResDepth == nullptr || peripheralMotionVectors == nullptr)))
    {
        return false;
    }

    GazeRoiConstants peripheralConstants = constants;
    GazeRoiConstants compositeConstants = constants;
    ID3D12Resource* peripheralForComposite = lowResColor;
    const bool usePeripheralEffects = peripheralConstants.peripheralBlur != 0 ||
                                      peripheralConstants.peripheralTemporal != 0;

    if (usePeripheralEffects)
    {
        if (!EnsurePeripheralPipeline())
            return false;

        FrameDescriptorHeap& peripheralHeap = _peripheralFrameHeaps[frameSlot];
        ID3D12Resource* historyForRead = lowResColor;
        ID3D12Resource* depthHistoryForRead = lowResColor;

        if (peripheralConstants.peripheralTemporal != 0)
        {
            if (_peripheralHistory[0] == nullptr || _peripheralHistory[1] == nullptr)
                return false;

            const uint32_t nextHistoryIndex = (_peripheralHistoryIndex + 1) % 2;
            const uint32_t previousHistoryIndex = _peripheralHistoryIndex;
            Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                        _peripheralHistory[previousHistoryIndex],
                                        &_peripheralHistoryState[previousHistoryIndex]);
            Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        _peripheralHistory[nextHistoryIndex],
                                        &_peripheralHistoryState[nextHistoryIndex]);
            peripheralConstants.peripheralTemporalInitialized = _peripheralHistoryInitialized ? 1 : 0;
            historyForRead = _peripheralHistory[previousHistoryIndex];
            peripheralForComposite = _peripheralHistory[nextHistoryIndex];
            if (peripheralConstants.peripheralTemporalMotionReprojection != 0)
            {
                if (_peripheralDepthHistory[0] == nullptr || _peripheralDepthHistory[1] == nullptr)
                    return false;
                Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                            _peripheralDepthHistory[previousHistoryIndex],
                                            &_peripheralDepthHistoryState[previousHistoryIndex]);
                Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            _peripheralDepthHistory[nextHistoryIndex],
                                            &_peripheralDepthHistoryState[nextHistoryIndex]);
                depthHistoryForRead = _peripheralDepthHistory[previousHistoryIndex];
            }
        }
        else
        {
            if (_peripheralOutput == nullptr)
                return false;
            _peripheralHistoryInitialized = false;
            SetPeripheralOutputState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            peripheralForComposite = _peripheralOutput;
        }

        try
        {
            CreateShaderResourceView(_device, lowResColor, peripheralHeap.GetSrvCPU(0));
            CreateShaderResourceView(_device, historyForRead, peripheralHeap.GetSrvCPU(1));
            CreateShaderResourceView(_device, peripheralMotionVectors != nullptr ? peripheralMotionVectors : lowResColor,
                                     peripheralHeap.GetSrvCPU(2));
            CreateShaderResourceView(_device, lowResDepth != nullptr ? lowResDepth : lowResColor,
                                     peripheralHeap.GetSrvCPU(3));
            CreateShaderResourceView(_device, depthHistoryForRead, peripheralHeap.GetSrvCPU(4));
            CreateUnorderedAccessView(_device, peripheralForComposite, peripheralHeap.GetUavCPU(0), 0);
            if (peripheralConstants.peripheralTemporalMotionReprojection != 0)
            {
                const uint32_t nextHistoryIndex = (_peripheralHistoryIndex + 1) % 2;
                CreateUnorderedAccessView(_device, _peripheralDepthHistory[nextHistoryIndex],
                                          peripheralHeap.GetUavCPU(1), 0);
            }
            else
            {
                // Keep the complete root descriptor table valid even though this shader variant does not write u1.
                CreateUnorderedAccessView(_device, peripheralForComposite, peripheralHeap.GetUavCPU(1), 0);
            }
        }
        catch (const std::exception& e)
        {
            LOG_WARN("[{}] Failed to create peripheral descriptors: {}", _name, e.what());
            return false;
        }

        ID3D12DescriptorHeap* peripheralHeaps[] = { peripheralHeap.GetHeapCSU() };
        commandList->SetDescriptorHeaps(_countof(peripheralHeaps), peripheralHeaps);
        commandList->SetComputeRootSignature(_rootSignature);
        commandList->SetPipelineState(_peripheralPipelineState);
        commandList->SetComputeRootDescriptorTable(0, peripheralHeap.GetTableGPUStart());
        commandList->SetComputeRoot32BitConstants(1, GAZE_ROI_COMPOSITE_CONSTANT_DWORDS,
                                                  &peripheralConstants, 0);

        const UINT peripheralDispatchWidth = (constants.srcWidth + _numThreadsX - 1) / _numThreadsX;
        const UINT peripheralDispatchHeight = (constants.srcHeight + _numThreadsY - 1) / _numThreadsY;
        commandList->Dispatch(peripheralDispatchWidth, peripheralDispatchHeight, 1);

        if (peripheralConstants.peripheralTemporal != 0)
        {
            const uint32_t nextHistoryIndex = (_peripheralHistoryIndex + 1) % 2;
            if (peripheralConstants.peripheralTemporalMotionReprojection != 0)
            {
                Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                            _peripheralDepthHistory[nextHistoryIndex],
                                            &_peripheralDepthHistoryState[nextHistoryIndex]);
            }
            Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                        _peripheralHistory[nextHistoryIndex],
                                        &_peripheralHistoryState[nextHistoryIndex]);
            _peripheralHistoryIndex = nextHistoryIndex;
            _peripheralHistoryInitialized = true;
            peripheralForComposite = _peripheralHistory[_peripheralHistoryIndex];
        }
        else
        {
            SetPeripheralOutputState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // The effect output is a compact render-resolution surface. Present it
        // to the same production composite as a zero-based, non-jittered source.
        compositeConstants.srcTextureWidth = constants.srcWidth;
        compositeConstants.srcTextureHeight = constants.srcHeight;
        compositeConstants.srcBaseX = 0;
        compositeConstants.srcBaseY = 0;
        compositeConstants.peripheralJitterCancel = 0;
    }

    return DispatchComposite(commandList, peripheralForComposite, patchedMotionVectors, finalOutput,
                             compositeConstants, frameSlot);
}

bool GazeRoi_Dx12::DispatchComposite(ID3D12GraphicsCommandList* commandList,
                                      ID3D12Resource* peripheralColor,
                                      ID3D12Resource* patchedMotionVectors,
                                      ID3D12Resource* finalOutput,
                                      const GazeRoiConstants& constants, uint32_t frameSlot,
                                      ID3D12Resource* depthDebugResource,
                                      const GazeRoiDepthDebugConstants& depthDebugConstants)
{
    if (!_init || _device == nullptr || commandList == nullptr || peripheralColor == nullptr ||
        finalOutput == nullptr || _dlssOutput == nullptr || frameSlot >= GAZE_ROI_NUM_OF_HEAPS)
        return false;

    FrameDescriptorHeap& currentHeap = _frameHeaps[frameSlot];
    try
    {
        CreateShaderResourceView(_device, peripheralColor, currentHeap.GetSrvCPU(0));
        if (_slotDlssOutputs[frameSlot] != _dlssOutput)
        {
            CreateShaderResourceView(_device, _dlssOutput, currentHeap.GetSrvCPU(1));
            _slotDlssOutputs[frameSlot] = _dlssOutput;
        }
        CreateShaderResourceView(_device, peripheralColor, currentHeap.GetSrvCPU(2));
        CreateShaderResourceView(_device, peripheralColor, currentHeap.GetSrvCPU(3));
        CreateShaderResourceView(_device, peripheralColor, currentHeap.GetSrvCPU(4));
        CreateUnorderedAccessView(_device, finalOutput, currentHeap.GetUavCPU(0), 0);
        CreateUnorderedAccessView(_device, finalOutput, currentHeap.GetUavCPU(1), 0);
    }
    catch (const std::exception& e)
    {
        LOG_WARN("[{}] Failed to create ROI composite descriptors: {}", _name, e.what());
        return false;
    }

    ID3D12DescriptorHeap* compositeHeaps[] = { currentHeap.GetHeapCSU() };
    commandList->SetDescriptorHeaps(_countof(compositeHeaps), compositeHeaps);
    commandList->SetComputeRootSignature(_rootSignature);
    commandList->SetPipelineState(_pipelineState);
    commandList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());
    commandList->SetComputeRoot32BitConstants(1, GAZE_ROI_COMPOSITE_CONSTANT_DWORDS, &constants, 0);

    const UINT dispatchWidth = (constants.dstWidth + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (constants.dstHeight + _numThreadsY - 1) / _numThreadsY;
    commandList->Dispatch(dispatchWidth, dispatchHeight, 1);

    const bool debugOverlay = constants.debugBorderPx > 0 || constants.motionVectorDebugView != 0;
    if (debugOverlay)
    {
        if (patchedMotionVectors == nullptr || !EnsureDebugOverlayPipeline())
            return false;

        const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(finalOutput);
        commandList->ResourceBarrier(1, &uavBarrier);
        FrameDescriptorHeap& debugHeap = _debugOverlayFrameHeaps[frameSlot];
        try
        {
            CreateShaderResourceView(_device, patchedMotionVectors, debugHeap.GetSrvCPU(0));
            CreateShaderResourceView(_device, patchedMotionVectors, debugHeap.GetSrvCPU(1));
            CreateShaderResourceView(_device, patchedMotionVectors, debugHeap.GetSrvCPU(2));
            CreateShaderResourceView(_device, patchedMotionVectors, debugHeap.GetSrvCPU(3));
            CreateShaderResourceView(_device, patchedMotionVectors, debugHeap.GetSrvCPU(4));
            CreateUnorderedAccessView(_device, finalOutput, debugHeap.GetUavCPU(0), 0);
            CreateUnorderedAccessView(_device, finalOutput, debugHeap.GetUavCPU(1), 0);
        }
        catch (const std::exception& e)
        {
            LOG_WARN("[{}] Failed to create debug-overlay descriptors: {}", _name, e.what());
            return false;
        }

        ID3D12DescriptorHeap* debugHeaps[] = { debugHeap.GetHeapCSU() };
        commandList->SetDescriptorHeaps(_countof(debugHeaps), debugHeaps);
        commandList->SetComputeRootSignature(_rootSignature);
        commandList->SetPipelineState(_debugOverlayPipelineState);
        commandList->SetComputeRootDescriptorTable(0, debugHeap.GetTableGPUStart());
        commandList->SetComputeRoot32BitConstants(1, GAZE_ROI_COMPOSITE_CONSTANT_DWORDS, &constants, 0);
        commandList->Dispatch(dispatchWidth, dispatchHeight, 1);
    }

    const bool depthDebug = depthDebugResource != nullptr && depthDebugConstants.depthWidth > 0 &&
                            depthDebugConstants.depthHeight > 0 && depthDebugConstants.dstWidth > 0 &&
                            depthDebugConstants.dstHeight > 0;
    if (depthDebug)
    {
        if (!EnsureDepthDebugPipeline())
            return false;

        const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(finalOutput);
        commandList->ResourceBarrier(1, &uavBarrier);
        FrameDescriptorHeap& depthDebugHeap = _depthDebugFrameHeaps[frameSlot];
        try
        {
            for (UINT i = 0; i < 5; ++i)
                CreateShaderResourceView(_device, depthDebugResource, depthDebugHeap.GetSrvCPU(i));
            CreateUnorderedAccessView(_device, finalOutput, depthDebugHeap.GetUavCPU(0), 0);
            CreateUnorderedAccessView(_device, finalOutput, depthDebugHeap.GetUavCPU(1), 0);
        }
        catch (const std::exception& e)
        {
            LOG_WARN("[{}] Failed to create RR depth debug descriptors: {}", _name, e.what());
            return false;
        }

        ID3D12DescriptorHeap* depthDebugHeaps[] = { depthDebugHeap.GetHeapCSU() };
        commandList->SetDescriptorHeaps(_countof(depthDebugHeaps), depthDebugHeaps);
        commandList->SetComputeRootSignature(_rootSignature);
        commandList->SetPipelineState(_depthDebugPipelineState);
        commandList->SetComputeRootDescriptorTable(0, depthDebugHeap.GetTableGPUStart());
        commandList->SetComputeRoot32BitConstants(1, GAZE_ROI_DEPTH_DEBUG_CONSTANT_DWORDS,
                                                  &depthDebugConstants, 0);
        commandList->Dispatch((static_cast<UINT>(depthDebugConstants.dstWidth) + _numThreadsX - 1) / _numThreadsX,
                              (static_cast<UINT>(depthDebugConstants.dstHeight) + _numThreadsY - 1) / _numThreadsY, 1);
    }

    return true;
}

GazeRoi_Dx12::GazeRoi_Dx12(std::string name, ID3D12Device* device) : Shader_Dx12(name, device)
{
    if (device == nullptr)
    {
        LOG_ERROR("[{}] Device is nullptr", _name);
        return;
    }

    CD3DX12_STATIC_SAMPLER_DESC sampler(0);
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    if (!SetupRootSignatureWithConstants(device, 5, 2, GAZE_ROI_COMPOSITE_CONSTANT_DWORDS, 0, 1, &sampler))
    {
        LOG_ERROR("[{}] Failed to setup root signature", _name);
        return;
    }

    ID3DBlob* shaderBlob = CompileShader(gazeRoiShader, "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
    {
        LOG_ERROR("[{}] CompileShader failed", _name);
        return;
    }

    bool psoCreated = CreateComputeShader(device, _rootSignature, &_pipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    if (!psoCreated)
    {
        LOG_ERROR("[{}] CreateComputeShader failed", _name);
        return;
    }

    _init = InitHeaps(device, _frameHeaps, GAZE_ROI_NUM_OF_HEAPS);
}

GazeRoi_Dx12::~GazeRoi_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < GAZE_ROI_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
        _peripheralFrameHeaps[i].ReleaseHeaps();
        _colorBypassFrameHeaps[i].ReleaseHeaps();
        _outputClearFrameHeaps[i].ReleaseHeaps();
        _debugOverlayFrameHeaps[i].ReleaseHeaps();
        _depthDebugFrameHeaps[i].ReleaseHeaps();
    }

    SAFE_RELEASE(_dlssOutput);
    SAFE_RELEASE(_peripheralOutput);
    SAFE_RELEASE(_peripheralHistory[0]);
    SAFE_RELEASE(_peripheralHistory[1]);
    SAFE_RELEASE(_peripheralDepthHistory[0]);
    SAFE_RELEASE(_peripheralDepthHistory[1]);
    SAFE_RELEASE(_peripheralPipelineState);
    SAFE_RELEASE(_colorBypassPipelineState);
    SAFE_RELEASE(_debugOverlayPipelineState);
    SAFE_RELEASE(_depthDebugPipelineState);
}
