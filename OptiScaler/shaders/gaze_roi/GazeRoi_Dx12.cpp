#include "pch.h"
#include "GazeRoi_Dx12.h"

#include <Config.h>

#define A_CPU
#include <shaders/output_scaling/fsr1/ffx_fsr1.h>
#undef A_CPU

#include <array>
#include <bit>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <numeric>

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
    uint32_t timingKind = 0;
    bool timingPrimeIssued = false;
};

struct FsrFgTimingSample
{
    double values[8] {};
    bool prime = false;
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
constexpr UINT GAZE_ROI_COMPOSITE_CONSTANT_DWORDS = 63;
constexpr UINT GAZE_ROI_DEPTH_DEBUG_CONSTANT_DWORDS = 10;
static_assert(GAZE_ROI_COMPOSITE_CONSTANT_DWORDS + 1 <= 64);
static_assert(offsetof(GazeRoiConstants, peripheralResolveWidth) == 45 * sizeof(uint32_t));
static_assert(offsetof(GazeRoiConstants, easuConst0) == 47 * sizeof(uint32_t));
static_assert(offsetof(GazeRoiConstants, easuConst3) + sizeof(GazeRoiConstants::easuConst3) ==
              GAZE_ROI_COMPOSITE_CONSTANT_DWORDS * sizeof(uint32_t));
constexpr UINT GAZE_ROI_TIMING_MARKERS = 8;
constexpr size_t FSRFG_TIMING_WINDOW = 512;
ComPtr<ID3D12QueryHeap> gazeRoiTimestampHeap;
ComPtr<ID3D12Resource> gazeRoiTimestampReadback;
std::deque<FsrFgTimingSample> fsrFgTimingSamples;

GazeRoiFrameSync::TimingMetricStats BuildTimingMetricStats(const std::vector<double>& values)
{
    GazeRoiFrameSync::TimingMetricStats result {};
    if (values.empty())
        return result;
    result.count = static_cast<uint32_t>(values.size());
    result.meanMs = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double p) {
        const size_t index = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
        return sorted[index];
    };
    result.p50Ms = percentile(0.50);
    result.p95Ms = percentile(0.95);
    result.maxMs = sorted.back();
    return result;
}

GazeRoiFrameSync::TimingGroupStats BuildTimingGroupStats(const std::vector<FsrFgTimingSample>& samples)
{
    GazeRoiFrameSync::TimingGroupStats result {};
    result.count = static_cast<uint32_t>(samples.size());
    std::array<std::vector<double>, 8> values;
    for (const auto& sample : samples)
        for (size_t index = 0; index < values.size(); ++index)
            values[index].push_back(sample.values[index]);
    result.placeholder = BuildTimingMetricStats(values[0]);
    result.previousRoiCrop = BuildTimingMetricStats(values[1]);
    result.fullHistorySave = BuildTimingMetricStats(values[2]);
    result.colorCrop = BuildTimingMetricStats(values[3]);
    result.historyPrime = BuildTimingMetricStats(values[4]);
    result.provider = BuildTimingMetricStats(values[5]);
    result.composite = BuildTimingMetricStats(values[6]);
    result.total = BuildTimingMetricStats(values[7]);
    return result;
}

bool CreateGazeRoiRootSignature(ID3D12Device* device, UINT srvCount, UINT uavCount,
                                const D3D12_STATIC_SAMPLER_DESC& sampler,
                                ID3D12RootSignature** rootSignature)
{
    if (device == nullptr || rootSignature == nullptr)
        return false;

    CD3DX12_DESCRIPTOR_RANGE1 ranges[2] {};
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srvCount, 0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, uavCount, 0);
    CD3DX12_ROOT_PARAMETER1 parameters[2] {};
    parameters[0].InitAsDescriptorTable(_countof(ranges), ranges);
    parameters[1].InitAsConstants(GAZE_ROI_COMPOSITE_CONSTANT_DWORDS, 0);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
    desc.Init_1_1(_countof(parameters), parameters, 1, &sampler);
    ComPtr<ID3DBlob> signatureBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT result = D3D12SerializeVersionedRootSignature(&desc, &signatureBlob, &errorBlob);
    if (FAILED(result))
    {
        LOG_ERROR("[GazeRoi] Failed to serialize fused-detail root signature: {:X} {}", static_cast<UINT64>(result),
                  errorBlob != nullptr ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "");
        return false;
    }

    result = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
                                         IID_PPV_ARGS(rootSignature));
    if (FAILED(result))
    {
        LOG_ERROR("[GazeRoi] Failed to create fused-detail root signature: {:X}", static_cast<UINT64>(result));
        return false;
    }
    return true;
}

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
        if (slot.timingKind == 1)
        {
            const double milliseconds[8] = {
                (timestamps[1] - timestamps[0]) * millisecondsPerTick,
                (timestamps[2] - timestamps[1]) * millisecondsPerTick,
                (timestamps[3] - timestamps[2]) * millisecondsPerTick,
                (timestamps[4] - timestamps[3]) * millisecondsPerTick,
                (timestamps[5] - timestamps[4]) * millisecondsPerTick,
                (timestamps[6] - timestamps[5]) * millisecondsPerTick,
                (timestamps[7] - timestamps[6]) * millisecondsPerTick,
                (timestamps[7] - timestamps[0]) * millisecondsPerTick,
            };
            FsrFgTimingSample sample {};
            sample.values[0] = milliseconds[0]; // placeholder
            sample.values[1] = milliseconds[2]; // previous ROI crop
            sample.values[2] = milliseconds[3]; // complete history save
            sample.values[3] = milliseconds[1]; // current color crop
            sample.values[4] = milliseconds[4];
            sample.values[5] = milliseconds[5];
            sample.values[6] = milliseconds[6];
            sample.values[7] = milliseconds[7];
            sample.prime = slot.timingPrimeIssued;
            fsrFgTimingSamples.push_back(sample);
            if (fsrFgTimingSamples.size() > FSRFG_TIMING_WINDOW)
                fsrFgTimingSamples.pop_front();

            LOG_INFO("[FSRFG_ROI_TIMING] generation={} slot={} placeholderMs={:.4f} previousRoiCropMs={:.4f} "
                     "fullHistorySaveMs={:.4f} colorCropMs={:.4f} historyPrimeMs={:.4f} providerMs={:.4f} "
                     "roiCompositeMs={:.4f} totalMs={:.4f}",
                     slot.generation, slotIndex, sample.values[0], sample.values[1], sample.values[2], sample.values[3],
                     sample.values[4], sample.values[5], sample.values[6], sample.values[7]);
        }
        else
        {
            LOG_INFO("[GROI_TIMING] generation={} slot={} mvMs={:.4f} dlssMs={:.4f} peripheralMs={:.4f} "
                     "compositeMs={:.4f} postMs={:.4f} totalMs={:.4f}",
                     slot.generation, slotIndex, (timestamps[1] - timestamps[0]) * millisecondsPerTick,
                     (timestamps[3] - timestamps[2]) * millisecondsPerTick,
                     (timestamps[5] - timestamps[4]) * millisecondsPerTick,
                     (timestamps[6] - timestamps[5]) * millisecondsPerTick,
                     (timestamps[6] - timestamps[4]) * millisecondsPerTick,
                     (timestamps[6] - timestamps[0]) * millisecondsPerTick);
        }
        const D3D12_RANGE writeRange = { 0, 0 };
        gazeRoiTimestampReadback->Unmap(0, &writeRange);
    }

    slot.timingActive = false;
    slot.timingResolved = false;
    slot.timestampFrequency = 0;
    slot.timingKind = 0;
    slot.timingPrimeIssued = false;
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
                                      uint32_t frameSlot, uint32_t timingKind)
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
    slot.timingKind = timingKind;
    slot.timingPrimeIssued = false;
    return true;
}

void GazeRoiFrameSync::SetGpuTimingPrime(ID3D12GraphicsCommandList* commandList, uint32_t frameSlot, bool issued)
{
    if (commandList == nullptr || frameSlot >= GAZE_ROI_FRAME_SLOTS)
        return;
    std::lock_guard lock(gazeRoiFrameSyncMutex);
    auto& slot = gazeRoiSlots[frameSlot];
    if (slot.timingActive && slot.state == GazeRoiSlotState::Recording && slot.commandList == commandList)
        slot.timingPrimeIssued = issued;
}

GazeRoiFrameSync::FsrFgTimingSnapshot GazeRoiFrameSync::GetFsrFgTimingSnapshot()
{
    std::lock_guard lock(gazeRoiFrameSyncMutex);
    std::vector<FsrFgTimingSample> all(fsrFgTimingSamples.begin(), fsrFgTimingSamples.end());
    std::vector<FsrFgTimingSample> prime;
    std::vector<FsrFgTimingSample> nonPrime;
    for (const auto& sample : all)
        (sample.prime ? prime : nonPrime).push_back(sample);
    FsrFgTimingSnapshot result {};
    result.windowSize = static_cast<uint32_t>(all.size());
    result.all = BuildTimingGroupStats(all);
    result.prime = BuildTimingGroupStats(prime);
    result.nonPrime = BuildTimingGroupStats(nonPrime);
    return result;
}

void GazeRoiFrameSync::ClearFsrFgTimingSnapshot()
{
    std::lock_guard lock(gazeRoiFrameSyncMutex);
    fsrFgTimingSamples.clear();
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
    int _PeripheralResolveWidth;
    int _PeripheralResolveHeight;
    uint _DetailEnabled;
    uint _DetailStrengthBits;
    uint _DetailJointMode;
    uint _PeripheralPreExposureBits;
    uint _PreviousPeripheralPreExposureBits;
};

Texture2D<float4> PeripheralColorTexture : register(t0);
Texture2D<float4> DlssRoiOutput : register(t1);
Texture2D<float4> PeripheralDetailTexture : register(t2);
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

float4 ApplyPeripheralDetail(float4 color, float2 uv)
{
    if (_DetailEnabled == 0)
        return color;

    const float4 detailSample = PeripheralDetailTexture.SampleLevel(LinearClampSampler, uv, 0.0f);
    const float detail = _DetailJointMode != 0 ? detailSample.y : detailSample.x;
    const float strength = asfloat(_DetailStrengthBits);
    if (_DetailJointMode != 0)
    {
        const float preExposure = max(asfloat(_PeripheralPreExposureBits), 1.0e-6f);
        const float3 positiveColor = max(color.rgb, 0.0f);
        const float3 sceneColor = positiveColor / preExposure;
        const float3 compressed = sceneColor / (1.0f + sceneColor);
        const float perceptualLuma = dot(compressed, float3(0.25f, 0.5f, 0.25f));
        const float provenDelta = detail * min(strength, 2.0f);
        const float requestedExtra = detail * max(strength - 2.0f, 0.0f);
        const float extraKnee = max(abs(provenDelta) * 2.0f + perceptualLuma * 0.1f, 1.0e-4f);
        const float limitedExtra = requestedExtra / (1.0f + abs(requestedExtra) / extraKnee);
        const float relativeDetail = (provenDelta + limitedExtra) / max(perceptualLuma, 0.05f);
        // Do not invert a modified per-channel Reinhard value. Its derivative
        // diverges near 1 and turned small highlight residuals into exposure
        // spikes. Transfer perceptual detail as a bounded chroma-preserving
        // gain in linear RGB instead.
        const float highlightProtection = saturate((perceptualLuma - 0.5f) / 0.4f);
        const float linearGainLimit = lerp(0.35f, 0.10f, highlightProtection);
        const float gainShoulder = linearGainLimit;
        const float absoluteRelativeDetail = abs(relativeDetail);
        const float gainExcess = max(absoluteRelativeDetail - linearGainLimit, 0.0f);
        // Preserve ordinary detail exactly. Only gain beyond the safe linear
        // range enters the shoulder; the previous limiter compressed from zero
        // and made equal Strength values visibly softer than mode 1.
        const float limitedGainMagnitude =
            min(absoluteRelativeDetail, linearGainLimit) +
            gainExcess / (1.0f + gainExcess / max(gainShoulder, 1.0e-4f));
        const float limitedRelativeDetail =
            relativeDetail < 0.0f ? -limitedGainMagnitude : limitedGainMagnitude;
        color.rgb = positiveColor * max(1.0f + limitedRelativeDetail, 0.0f);
        return color;
    }

    const float luma = dot(color.rgb, float3(0.299f, 0.587f, 0.114f));
    const float provenStrength = min(strength, 2.0f);
    const float provenDelta = detail * provenStrength;
    const float requestedExtra = detail * max(strength - 2.0f, 0.0f);
    // Preserve the validated 0..2 response exactly. Above 2, compress only the
    // extra gain so experimental sharpening cannot grow without bound around
    // high-contrast edges or unstable residuals.
    const float extraKnee = max(abs(provenDelta) * 2.0f + max(luma, 0.0f) * 0.1f, 1.0e-4f);
    const float limitedExtra = requestedExtra / (1.0f + abs(requestedExtra) / extraKnee);
    const float targetLuma = max(luma + provenDelta + limitedExtra, 0.0f);
    color.rgb = luma > 1.0e-4f ? color.rgb * (targetLuma / luma) : targetLuma.xxx;
    return color;
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
        float4 peripheral = ApplyPeripheralDetail(SamplePeripheral(uv), uv);
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

// The EASU filter below is adapted from AMD FidelityFX Super Resolution 1.0
// (Copyright (c) 2021 Advanced Micro Devices, Inc.), distributed under the MIT
// license included in shaders/output_scaling/fsr1/ffx_fsr1.h. It is fused with
// the ROI composite so the feature does not allocate or write a 4K intermediate.
static const char* gazeRoiEasuCompositeShader = R"(
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
    int _PeripheralResolveWidth;
    int _PeripheralResolveHeight;
    // Keep these scalar so HLSL packs them directly after the resolve size. A uint4
    // would start at the next 16-byte register and desynchronize root constants.
    uint _EasuConst0X;
    uint _EasuConst0Y;
    uint _EasuConst0Z;
    uint _EasuConst0W;
    uint _EasuConst1X;
    uint _EasuConst1Y;
    uint _EasuConst1Z;
    uint _EasuConst1W;
    uint _EasuConst2X;
    uint _EasuConst2Y;
    uint _EasuConst2Z;
    uint _EasuConst2W;
    uint _EasuConst3X;
    uint _EasuConst3Y;
    uint _EasuConst3Z;
    uint _EasuConst3W;
};

Texture2D<float4> PeripheralColorTexture : register(t0);
Texture2D<float4> DlssRoiOutput : register(t1);
RWTexture2D<float4> FinalOutput : register(u0);
SamplerState LinearClampSampler : register(s0);

void EasuTap(inout float3 accumulatedColor, inout float accumulatedWeight, float2 offset,
             float2 direction, float2 anisotropicLength, float negativeLobe, float clippingPoint,
             float3 color)
{
    float2 v;
    v.x = offset.x * direction.x + offset.y * direction.y;
    v.y = offset.x * -direction.y + offset.y * direction.x;
    v *= anisotropicLength;
    float distanceSquared = min(dot(v, v), clippingPoint);
    float baseWeight = (2.0f / 5.0f) * distanceSquared - 1.0f;
    float windowWeight = negativeLobe * distanceSquared - 1.0f;
    baseWeight *= baseWeight;
    windowWeight *= windowWeight;
    baseWeight = (25.0f / 16.0f) * baseWeight - (25.0f / 16.0f - 1.0f);
    float weight = baseWeight * windowWeight;
    accumulatedColor += color * weight;
    accumulatedWeight += weight;
}

void EasuSet(inout float2 direction, inout float edgeLength, float2 fractionalPosition,
             bool upperLeft, bool upperRight, bool lowerLeft, bool lowerRight,
             float lumaA, float lumaB, float lumaC, float lumaD, float lumaE)
{
    float weight = 0.0f;
    if (upperLeft)
        weight = (1.0f - fractionalPosition.x) * (1.0f - fractionalPosition.y);
    if (upperRight)
        weight = fractionalPosition.x * (1.0f - fractionalPosition.y);
    if (lowerLeft)
        weight = (1.0f - fractionalPosition.x) * fractionalPosition.y;
    if (lowerRight)
        weight = fractionalPosition.x * fractionalPosition.y;

    float horizontalLeft = lumaD - lumaC;
    float horizontalRight = lumaC - lumaB;
    float horizontalRange = max(abs(horizontalLeft), abs(horizontalRight));
    float horizontalDirection = lumaD - lumaB;
    direction.x += horizontalDirection * weight;
    float horizontalLength = saturate(abs(horizontalDirection) / max(horizontalRange, 1.0e-6f));
    edgeLength += horizontalLength * horizontalLength * weight;

    float verticalDown = lumaE - lumaC;
    float verticalUp = lumaC - lumaA;
    float verticalRange = max(abs(verticalDown), abs(verticalUp));
    float verticalDirection = lumaE - lumaA;
    direction.y += verticalDirection * weight;
    float verticalLength = saturate(abs(verticalDirection) / max(verticalRange, 1.0e-6f));
    edgeLength += verticalLength * verticalLength * weight;
}

float3 Easu(uint2 outputPixel)
{
    float4 con0 = asfloat(uint4(_EasuConst0X, _EasuConst0Y, _EasuConst0Z, _EasuConst0W));
    float4 con1 = asfloat(uint4(_EasuConst1X, _EasuConst1Y, _EasuConst1Z, _EasuConst1W));
    float4 con2 = asfloat(uint4(_EasuConst2X, _EasuConst2Y, _EasuConst2Z, _EasuConst2W));
    float4 con3 = asfloat(uint4(_EasuConst3X, _EasuConst3Y, _EasuConst3Z, _EasuConst3W));

    float2 position = float2(outputPixel) * con0.xy + con0.zw;
    float2 sourceFloor = floor(position);
    float2 fractionalPosition = position - sourceFloor;
    float2 gather0 = sourceFloor * con1.xy + con1.zw;
    float2 gather1 = gather0 + con2.xy;
    float2 gather2 = gather0 + con2.zw;
    float2 gather3 = gather0 + con3.xy;

    float4 bczzR = PeripheralColorTexture.GatherRed(LinearClampSampler, gather0);
    float4 bczzG = PeripheralColorTexture.GatherGreen(LinearClampSampler, gather0);
    float4 bczzB = PeripheralColorTexture.GatherBlue(LinearClampSampler, gather0);
    float4 ijfeR = PeripheralColorTexture.GatherRed(LinearClampSampler, gather1);
    float4 ijfeG = PeripheralColorTexture.GatherGreen(LinearClampSampler, gather1);
    float4 ijfeB = PeripheralColorTexture.GatherBlue(LinearClampSampler, gather1);
    float4 klhgR = PeripheralColorTexture.GatherRed(LinearClampSampler, gather2);
    float4 klhgG = PeripheralColorTexture.GatherGreen(LinearClampSampler, gather2);
    float4 klhgB = PeripheralColorTexture.GatherBlue(LinearClampSampler, gather2);
    float4 zzonR = PeripheralColorTexture.GatherRed(LinearClampSampler, gather3);
    float4 zzonG = PeripheralColorTexture.GatherGreen(LinearClampSampler, gather3);
    float4 zzonB = PeripheralColorTexture.GatherBlue(LinearClampSampler, gather3);

    float4 bczzL = bczzB * 0.5f + (bczzR * 0.5f + bczzG);
    float4 ijfeL = ijfeB * 0.5f + (ijfeR * 0.5f + ijfeG);
    float4 klhgL = klhgB * 0.5f + (klhgR * 0.5f + klhgG);
    float4 zzonL = zzonB * 0.5f + (zzonR * 0.5f + zzonG);

    float2 direction = 0.0f;
    float edgeLength = 0.0f;
    EasuSet(direction, edgeLength, fractionalPosition, true, false, false, false,
            bczzL.x, ijfeL.w, ijfeL.z, klhgL.w, ijfeL.y);
    EasuSet(direction, edgeLength, fractionalPosition, false, true, false, false,
            bczzL.y, ijfeL.z, klhgL.w, klhgL.z, klhgL.x);
    EasuSet(direction, edgeLength, fractionalPosition, false, false, true, false,
            ijfeL.z, ijfeL.x, ijfeL.y, klhgL.x, zzonL.w);
    EasuSet(direction, edgeLength, fractionalPosition, false, false, false, true,
            klhgL.w, ijfeL.y, klhgL.x, klhgL.y, zzonL.z);

    float directionLengthSquared = dot(direction, direction);
    if (directionLengthSquared < (1.0f / 32768.0f))
        direction = float2(1.0f, 0.0f);
    else
        direction *= rsqrt(directionLengthSquared);

    edgeLength = edgeLength * 0.5f;
    edgeLength *= edgeLength;
    float stretch = dot(direction, direction) / max(max(abs(direction.x), abs(direction.y)), 1.0e-6f);
    float2 anisotropicLength = float2(1.0f + (stretch - 1.0f) * edgeLength,
                                     1.0f - 0.5f * edgeLength);
    float negativeLobe = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * edgeLength;
    float clippingPoint = 1.0f / negativeLobe;

    float3 f = float3(ijfeR.z, ijfeG.z, ijfeB.z);
    float3 g = float3(klhgR.w, klhgG.w, klhgB.w);
    float3 j = float3(ijfeR.y, ijfeG.y, ijfeB.y);
    float3 k = float3(klhgR.x, klhgG.x, klhgB.x);
    float3 minimumColor = min(min(f, g), min(j, k));
    float3 maximumColor = max(max(f, g), max(j, k));
    float3 accumulatedColor = 0.0f;
    float accumulatedWeight = 0.0f;
    EasuTap(accumulatedColor, accumulatedWeight, float2(0.0f, -1.0f) - fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint,
            float3(bczzR.x, bczzG.x, bczzB.x));
    EasuTap(accumulatedColor, accumulatedWeight, float2(1.0f, -1.0f) - fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint,
            float3(bczzR.y, bczzG.y, bczzB.y));
    EasuTap(accumulatedColor, accumulatedWeight, float2(-1.0f, 1.0f) - fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint,
            float3(ijfeR.x, ijfeG.x, ijfeB.x));
    EasuTap(accumulatedColor, accumulatedWeight, float2(0.0f, 1.0f) - fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint,
            float3(ijfeR.y, ijfeG.y, ijfeB.y));
    EasuTap(accumulatedColor, accumulatedWeight, -fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint, f);
    EasuTap(accumulatedColor, accumulatedWeight, float2(-1.0f, 0.0f) - fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint,
            float3(ijfeR.w, ijfeG.w, ijfeB.w));
    EasuTap(accumulatedColor, accumulatedWeight, float2(1.0f, 1.0f) - fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint, k);
    EasuTap(accumulatedColor, accumulatedWeight, float2(2.0f, 1.0f) - fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint,
            float3(klhgR.y, klhgG.y, klhgB.y));
    EasuTap(accumulatedColor, accumulatedWeight, float2(2.0f, 0.0f) - fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint,
            float3(klhgR.z, klhgG.z, klhgB.z));
    EasuTap(accumulatedColor, accumulatedWeight, float2(1.0f, 0.0f) - fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint, g);
    EasuTap(accumulatedColor, accumulatedWeight, float2(1.0f, 2.0f) - fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint,
            float3(zzonR.z, zzonG.z, zzonB.z));
    EasuTap(accumulatedColor, accumulatedWeight, float2(0.0f, 2.0f) - fractionalPosition,
            direction, anisotropicLength, negativeLobe, clippingPoint,
            float3(zzonR.w, zzonG.w, zzonB.w));

    float3 filtered = accumulatedColor / max(accumulatedWeight, 1.0e-6f);
    return min(maximumColor, max(minimumColor, filtered));
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
    return saturate((float)min(min(left, right), min(top, bottom)) / (float)_FeatherPx);
}

void WriteComposite(uint2 p)
{
    if (p.x >= (uint)_DstWidth || p.y >= (uint)_DstHeight)
        return;

    float alpha = RoiAlpha((int2)p);
    float4 result;
    if (alpha >= 1.0f)
    {
        result = DlssRoiOutput.Load(int3(p - uint2((uint)_RoiX, (uint)_RoiY), 0));
    }
    else
    {
        float4 peripheral = float4(Easu(p), 1.0f);
        if (alpha > 0.0f)
        {
            float4 roi = DlssRoiOutput.Load(int3(p - uint2((uint)_RoiX, (uint)_RoiY), 0));
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

uint2 Remap8x8(uint lane)
{
    uint y = (lane >> 3u) & 7u;
    y = (y & ~1u) | (lane & 1u);
    return uint2((lane >> 1u) & 7u, y);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 localThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    uint2 p = Remap8x8(localThreadId.x) + (groupId.xy << 4u);
    WriteComposite(p);
    WriteComposite(p + uint2(8u, 0u));
    WriteComposite(p + uint2(8u, 8u));
    WriteComposite(p + uint2(0u, 8u));
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
    int _PeripheralResolveWidth;
    int _PeripheralResolveHeight;
    int _FusedDetailInitialized;
    uint _TaauDetailStrengthBits;
    uint _ReservedDetailConstant2;
    uint _PeripheralPreExposureBits;
    uint _PreviousPeripheralPreExposureBits;
};

Texture2D<float4> LowResColor : register(t0);
Texture2D<float4> PeripheralHistory : register(t1);
Texture2D<float4> PeripheralMotionVectors : register(t2);
Texture2D<float> CurrentDepth : register(t3);
#ifdef GAZE_ROI_JOINT_DETAIL
Texture2D<float2> PreviousDepthDetail : register(t4);
#else
Texture2D<float> PreviousDepth : register(t4);
#endif
RWTexture2D<float4> PeripheralOutput : register(u0);
#ifdef GAZE_ROI_JOINT_DETAIL
RWTexture2D<float2> NextDepthDetail : register(u1);
#else
RWTexture2D<float> NextDepth : register(u1);
#endif
#ifdef GAZE_ROI_FUSED_DETAIL
#ifndef GAZE_ROI_JOINT_DETAIL
Texture2D<float2> PreviousDetail : register(t5);
RWTexture2D<float2> NextDetail : register(u2);
#endif
#endif
SamplerState LinearClampSampler : register(s0);

float CurrentPeripheralPreExposure()
{
    const float value = asfloat(_PeripheralPreExposureBits);
    return isfinite(value) && value > 1.0e-6f ? value : 1.0f;
}

float PreviousPeripheralPreExposure()
{
    const float value = asfloat(_PreviousPeripheralPreExposureBits);
    return isfinite(value) && value > 1.0e-6f ? value : CurrentPeripheralPreExposure();
}

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

float4 SampleSourceAtPixel(float2 sourcePixel)
{
    const float2 sourceLimit = float2((float)_SrcWidth - 0.5f, (float)_SrcHeight - 0.5f);
    const int2 pixel = int2(clamp(floor(sourcePixel), 0.0f, sourceLimit));
    return LowResColor.Load(int3(pixel + int2(_SrcBaseX, _SrcBaseY), 0));
}

#if defined(GAZE_ROI_FUSED_DETAIL) && !defined(GAZE_ROI_JOINT_DETAIL)
float FusedDetailLuma(float3 color)
{
    return dot(max(color, 0.0f), float3(0.299f, 0.587f, 0.114f));
}

float3 SampleFusedDetailCurrent(float2 sourcePosition)
{
    sourcePosition = clamp(sourcePosition, 0.5f,
                           float2((float)_SrcWidth, (float)_SrcHeight) - 0.5f);
    const float2 sourcePixel = sourcePosition + float2((float)_SrcBaseX, (float)_SrcBaseY);
    const float2 sourceUv = sourcePixel / float2((float)_SrcTextureWidth, (float)_SrcTextureHeight);
    return LowResColor.SampleLevel(LinearClampSampler, sourceUv, 0.0f).rgb;
}

void GetFusedCurrentDetail(float2 sourcePosition, out float residual, out float localContrast)
{
    const float center = FusedDetailLuma(SampleFusedDetailCurrent(sourcePosition));
    const float left = FusedDetailLuma(SampleFusedDetailCurrent(sourcePosition + float2(-1.0f, 0.0f)));
    const float right = FusedDetailLuma(SampleFusedDetailCurrent(sourcePosition + float2(1.0f, 0.0f)));
    const float up = FusedDetailLuma(SampleFusedDetailCurrent(sourcePosition + float2(0.0f, -1.0f)));
    const float down = FusedDetailLuma(SampleFusedDetailCurrent(sourcePosition + float2(0.0f, 1.0f)));
    const float lowPass = center * 0.5f + (left + right + up + down) * 0.125f;
    residual = center - lowPass;
    const float neighborhoodMin = min(center, min(min(left, right), min(up, down)));
    const float neighborhoodMax = max(center, max(max(left, right), max(up, down)));
    localContrast = max(neighborhoodMax - neighborhoodMin, 0.002f);
}

void WriteFusedDetail(uint2 p, float currentResidual, float localContrast, float currentDepth, float2 motion)
{
    const float2 detailSize = float2((float)_PeripheralResolveWidth, (float)_PeripheralResolveHeight);
    const float2 previousPixel = float2(p) + motion;
    const bool inBounds = all(previousPixel >= 0.0f) && all(previousPixel < detailSize);
    float2 previous = float2(0.0f, currentDepth);
    if (inBounds && _FusedDetailInitialized != 0)
    {
        const float2 previousUv = (previousPixel + 0.5f) / detailSize;
        previous = PreviousDetail.SampleLevel(LinearClampSampler, previousUv, 0.0f);
        previous.x *= CurrentPeripheralPreExposure() / PreviousPeripheralPreExposure();
    }

    const float depthThreshold = max(0.002f, max(abs(currentDepth), abs(previous.y)) * 0.02f);
    const bool depthMatches = isfinite(currentDepth) && isfinite(previous.y) &&
                              abs(currentDepth - previous.y) <= depthThreshold;
    const bool acceptHistory = _FusedDetailInitialized != 0 &&
                               _PeripheralTemporalHistoryReset == 0 && inBounds && depthMatches;
    const float clippedHistory = clamp(previous.x, -localContrast, localContrast);
    const float normalizedChange = abs(currentResidual - clippedHistory) / localContrast;
    float reactive = saturate(max(normalizedChange - 0.5f, 0.0f) *
                              0.25f * _PeripheralTemporalReactiveScale);
    if (abs(currentResidual) < localContrast * 0.1f)
        reactive *= 0.25f;
    const float motionReactivity = saturate(length(motion) * 0.125f);
    const float historyWeight = saturate(_PeripheralTemporalHistoryWeight) *
                                (1.0f - reactive) * (1.0f - 0.75f * motionReactivity);
    const float resolvedResidual = acceptHistory
                                       ? lerp(currentResidual, clippedHistory, historyWeight)
                                       : currentResidual;
    NextDetail[p] = float2(clamp(resolvedResidual, -localContrast, localContrast), currentDepth);
}
#endif

float3 TemporalCompress(float3 color)
{
    color = max(color, 0.0f);
    return color / (1.0f + color);
}

float3 TemporalExpand(float3 color)
{
    color = clamp(color, 0.0f, 0.999f);
    return color / max(1.0f - color, 1.0e-3f);
}

float3 RgbToYCoCg(float3 color)
{
    return float3(0.25f * color.r + 0.5f * color.g + 0.25f * color.b,
                  0.5f * color.r - 0.5f * color.b,
                  -0.25f * color.r + 0.5f * color.g - 0.25f * color.b);
}

float3 YCoCgToRgb(float3 color)
{
    return float3(color.x + color.y - color.z,
                  color.x + color.z,
                  color.x - color.y - color.z);
}

#ifdef GAZE_ROI_JOINT_DETAIL
static const uint JOINT_GROUP_SIZE = 16;
static const uint JOINT_TILE_HALO = 1;
static const uint JOINT_TILE_SIZE = JOINT_GROUP_SIZE + JOINT_TILE_HALO * 2;
// RGB and Depth share one 16-byte tile entry. The Color alpha channel is not
// used by the temporal resolve, so packing Depth into W reduces LDS footprint
// and avoids a second group-shared array.
groupshared float4 JointColorDepthTile[JOINT_TILE_SIZE * JOINT_TILE_SIZE];

int2 JointTileOrigin(uint3 groupId)
{
    return int2(groupId.xy) * int(JOINT_GROUP_SIZE) - int(JOINT_TILE_HALO);
}

void LoadJointTiles(uint3 groupId, uint3 groupThreadId)
{
    const int2 tileOrigin = JointTileOrigin(groupId);
    const uint threadIndex = groupThreadId.y * JOINT_GROUP_SIZE + groupThreadId.x;
    const int2 sourceLimit = int2(_SrcWidth - 1, _SrcHeight - 1);
    [loop]
    for (uint tileIndex = threadIndex; tileIndex < JOINT_TILE_SIZE * JOINT_TILE_SIZE;
         tileIndex += JOINT_GROUP_SIZE * JOINT_GROUP_SIZE)
    {
        const int2 tilePosition = int2(tileIndex % JOINT_TILE_SIZE, tileIndex / JOINT_TILE_SIZE);
        const int2 sourcePosition = clamp(tileOrigin + tilePosition, int2(0, 0), sourceLimit);
        float4 colorDepth = LowResColor.Load(int3(sourcePosition + int2(_SrcBaseX, _SrcBaseY), 0));
        colorDepth.w = CurrentDepth.Load(
            int3(sourcePosition + int2(_PeripheralDepthBaseX, _PeripheralDepthBaseY), 0));
        JointColorDepthTile[tileIndex] = colorDepth;
    }
    GroupMemoryBarrierWithGroupSync();
}

float4 JointColorLoad(int2 sourcePosition, int2 tileOrigin)
{
    const int2 sourceLimit = int2(_SrcWidth - 1, _SrcHeight - 1);
    sourcePosition = clamp(sourcePosition, int2(0, 0), sourceLimit);
    const int2 tilePosition = sourcePosition - tileOrigin;
    if (all(tilePosition >= 0) && all(tilePosition < int(JOINT_TILE_SIZE)))
        return JointColorDepthTile[tilePosition.y * JOINT_TILE_SIZE + tilePosition.x];
    return LowResColor.Load(int3(sourcePosition + int2(_SrcBaseX, _SrcBaseY), 0));
}

float JointDepthLoad(int2 sourcePosition, int2 tileOrigin)
{
    const int2 sourceLimit = int2(_SrcWidth - 1, _SrcHeight - 1);
    sourcePosition = clamp(sourcePosition, int2(0, 0), sourceLimit);
    const int2 tilePosition = sourcePosition - tileOrigin;
    if (all(tilePosition >= 0) && all(tilePosition < int(JOINT_TILE_SIZE)))
        return JointColorDepthTile[tilePosition.y * JOINT_TILE_SIZE + tilePosition.x].w;
    return CurrentDepth.Load(int3(sourcePosition + int2(_PeripheralDepthBaseX, _PeripheralDepthBaseY), 0));
}

void EvaluateJointPeripheral(float2 sampleCenter, int2 tileOrigin, out float4 current,
                             out float3 neighborhoodMean, out float3 neighborhoodDeviation,
                             out float detailResidual, out float detailContrast)
{
    const int2 kernelPixel = int2(floor(sampleCenter));
    const float2 kernelCenter = float2(kernelPixel) + 0.5f;
    const float2 subpixelOffset = sampleCenter - kernelCenter;
    const float blurScale = _PeripheralBlur != 0 ? max(_PeripheralBlurRadius, 1.0f) : 1.0f;
    const float upscaleRatio = max((float)_DstWidth / max((float)_SrcWidth, 1.0f),
                                   (float)_DstHeight / max((float)_SrcHeight, 1.0f));
    const float sparseCoverage = saturate((upscaleRatio - 1.0f) * 0.5f);
    const float reconstructionSharpness = lerp(2.29f, 1.45f, sparseCoverage) / blurScale;
    float3 weightedColor = 0.0f;
    float centerPerceptualLuma = 0.0f;
    float axialPerceptualLuma = 0.0f;
    float3 weightedSum = 0.0f;
    float3 weightedSquareSum = 0.0f;
    float reconstructionTotal = 0.0f;
    float statisticsTotal = 0.0f;
    const float currentPreExposure = CurrentPeripheralPreExposure();
    const float3 reconstructionDistanceX = float3(-1.0f, 0.0f, 1.0f) - subpixelOffset.x;
    const float3 reconstructionDistanceY = float3(-1.0f, 0.0f, 1.0f) - subpixelOffset.y;
    const float3 reconstructionWeightX =
        exp(-reconstructionSharpness * reconstructionDistanceX * reconstructionDistanceX);
    const float3 reconstructionWeightY =
        exp(-reconstructionSharpness * reconstructionDistanceY * reconstructionDistanceY);
    const float statisticsEdgeWeight = 0.47236655f; // exp(-0.75)
    const float3 statisticsWeightAxis = float3(statisticsEdgeWeight, 1.0f, statisticsEdgeWeight);
    const float inversePreExposure = 1.0f / currentPreExposure;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2((float)x, (float)y);
            const float4 sampleColor = JointColorLoad(kernelPixel + int2(x, y), tileOrigin);
            const float reconstructionWeight = reconstructionWeightX[x + 1] * reconstructionWeightY[y + 1];
            const float statisticsWeight = statisticsWeightAxis[x + 1] * statisticsWeightAxis[y + 1];
            const float3 workingColor =
                RgbToYCoCg(TemporalCompress(sampleColor.rgb * inversePreExposure));
            weightedColor += sampleColor.rgb * reconstructionWeight;
            reconstructionTotal += reconstructionWeight;
            weightedSum += workingColor * statisticsWeight;
            weightedSquareSum += workingColor * workingColor * statisticsWeight;
            statisticsTotal += statisticsWeight;
            if (x == 0 && y == 0)
                centerPerceptualLuma = workingColor.x;
            else if (abs(x) + abs(y) == 1)
                axialPerceptualLuma += workingColor.x;
        }
    }
    neighborhoodMean = weightedSum / statisticsTotal;
    neighborhoodDeviation =
        sqrt(max(weightedSquareSum / statisticsTotal - neighborhoodMean * neighborhoodMean, 0.0f));
    current = float4(weightedColor / max(reconstructionTotal, 1.0e-5f), 1.0f);
    // Restore the validated five-point high-pass shape from samples already
    // present in the main 3x3 gather. This adds no texture or LDS reads.
    const float lowPassLuma = centerPerceptualLuma * 0.5f + axialPerceptualLuma * 0.125f;
    detailResidual = centerPerceptualLuma - lowPassLuma;
    detailContrast = max(neighborhoodDeviation.x * 2.0f, 0.002f);
}

float ResolveJointDetail(uint2 p, float currentResidual, float localContrast, float2 motion,
                         bool sharedHistoryValid, float baseHistoryConfidence)
{
    const float2 detailSize = float2((float)_PeripheralResolveWidth, (float)_PeripheralResolveHeight);
    const float2 previousPixel = float2(p) + motion;
    float previousResidual = 0.0f;
    if (sharedHistoryValid && _FusedDetailInitialized != 0)
    {
        const float2 previousUv = (previousPixel + 0.5f) / detailSize;
        previousResidual = PreviousDepthDetail.SampleLevel(LinearClampSampler, previousUv, 0.0f).y;
    }

    const float detailLimit = max(localContrast, 0.001f);
    const float clippedHistory = clamp(previousResidual, -detailLimit, detailLimit);
    const float magnitudeFloor = localContrast * 0.05f;
    const bool signAgrees = currentResidual * clippedHistory >= 0.0f ||
                            min(abs(currentResidual), abs(clippedHistory)) <= magnitudeFloor;
    const float normalizedChange = abs(currentResidual - clippedHistory) / localContrast;
    const float amplitudeConfidence = 1.0f - saturate((normalizedChange - 0.25f) * (4.0f / 3.0f));
    const float motionConfidence = 1.0f - saturate(length(motion) * 0.125f);
    const float detailConfidence = saturate(baseHistoryConfidence) * amplitudeConfidence * motionConfidence *
                                   (signAgrees ? 1.0f : 0.1f);
    const bool acceptHistory = _FusedDetailInitialized != 0 && sharedHistoryValid;
    const float historyWeight = saturate(_PeripheralTemporalHistoryWeight) * detailConfidence;
    const float resolvedResidual = acceptHistory
                                       ? lerp(currentResidual, clippedHistory, historyWeight)
                                       : currentResidual;
    return clamp(resolvedResidual, -detailLimit, detailLimit);
}
#endif

)" R"(

float3 ClipToAabb(float3 value, float3 center, float3 extent)
{
    const float3 offset = value - center;
    const float3 normalized = abs(offset) / max(extent, 1.0e-4f);
    const float largest = max(max(normalized.x, normalized.y), normalized.z);
    return largest > 1.0f ? center + offset / largest : value;
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

void EvaluateLightweightPeripheral(float2 sampleCenter, bool allowConfiguredBlur, out float4 current,
                                   out float3 neighborhoodMean, out float3 neighborhoodDeviation)
{
    // Reconstruct one stable-grid sample from the current jitter phase. Exact
    // texel loads avoid applying a second bilinear filter before accumulation.
    const float2 kernelCenter = floor(sampleCenter) + 0.5f;
    const float2 subpixelOffset = sampleCenter - kernelCenter;
    const float blurScale = allowConfiguredBlur && _PeripheralBlur != 0
                                ? max(_PeripheralBlurRadius, 1.0f)
                                : 1.0f;
    const float resolveRatio = max((float)_PeripheralResolveWidth / max((float)_SrcWidth, 1.0f),
                                   (float)_PeripheralResolveHeight / max((float)_SrcHeight, 1.0f));
    // Keep the validated wider Ultra Performance footprint when resolving at
    // render resolution. A larger temporal grid already supplies subpixel
    // positions, so its current reconstruction follows that grid's scale and
    // avoids inheriting the full display-ratio blur.
    const float upscaleRatio = resolveRatio > 1.001f
                                   ? resolveRatio
                                   : max((float)_DstWidth / max((float)_SrcWidth, 1.0f),
                                         (float)_DstHeight / max((float)_SrcHeight, 1.0f));
    // Sparse DLSS inputs need a slightly wider coverage reconstruction as one
    // render pixel covers more display pixels. Keep it bounded so this does
    // not become the legacy large spatial blur.
    const float sparseCoverage = saturate((upscaleRatio - 1.0f) * 0.5f);
    const float reconstructionSharpness = lerp(2.29f, 1.45f, sparseCoverage) / blurScale;
    float4 weightedColor = 0.0f;
    float3 weightedSum = 0.0f;
    float3 weightedSquareSum = 0.0f;
    float reconstructionTotal = 0.0f;
    float statisticsTotal = 0.0f;
    const float currentPreExposure = CurrentPeripheralPreExposure();
    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            const float2 offset = float2((float)x, (float)y);
            const float4 sampleColor = SampleSourceAtPixel(kernelCenter + offset);
            const float2 distanceFromSample = offset - subpixelOffset;
            const float reconstructionWeight =
                exp(-reconstructionSharpness * dot(distanceFromSample, distanceFromSample));
            // Statistics use the same nine Color loads, but operate in a
            // bounded luminance/chroma domain so HDR highlights cannot make
            // the history box or change rejection explode.
            const float statisticsWeight = exp(-0.75f * dot(offset, offset));
            const float3 workingColor = RgbToYCoCg(TemporalCompress(sampleColor.rgb / currentPreExposure));
            weightedColor += sampleColor * reconstructionWeight;
            reconstructionTotal += reconstructionWeight;
            weightedSum += workingColor * statisticsWeight;
            weightedSquareSum += workingColor * workingColor * statisticsWeight;
            statisticsTotal += statisticsWeight;
        }
    }
    neighborhoodMean = weightedSum / statisticsTotal;
    neighborhoodDeviation =
        sqrt(max(weightedSquareSum / statisticsTotal - neighborhoodMean * neighborhoodMean, 0.0f));
    current = weightedColor / max(reconstructionTotal, 1.0e-5f);
}

float TaauLanczos2ApproxSq(float distanceSquared)
{
    // FSR2's low-cost FSR1-derived Lanczos2 approximation. The 1.25x resolve
    // uses a sliced 3x3 footprint; the remaining jitter phases fill the kernel
    // temporally instead of paying for a full spatial Lanczos pass each frame.
    distanceSquared = min(distanceSquared, 4.0f);
    const float a = 0.4f * distanceSquared - 1.0f;
    const float b = 0.25f * distanceSquared - 1.0f;
    return (1.5625f * a * a - 0.5625f) * (b * b);
}

void EvaluateIntermediateTaau(float2 sourceOutputPosition, float2 sampleCenter,
                              out float4 current, out float3 neighborhoodMean,
                              out float3 neighborhoodDeviation, out float3 neighborhoodMin,
                              out float3 neighborhoodMax, out float currentSampleWeight,
                              out float thinFeatureLock)
{
    const int2 sourceLimit = int2(_SrcWidth - 1, _SrcHeight - 1);
    const int2 sourceBase = clamp(int2(floor(sourceOutputPosition)), int2(0, 0), sourceLimit);
    const float currentPreExposure = CurrentPeripheralPreExposure();
    const float resolveScale = max((float)_PeripheralResolveWidth / max((float)_SrcWidth, 1.0f),
                                   (float)_PeripheralResolveHeight / max((float)_SrcHeight, 1.0f));
    const float kernelBias = min(resolveScale, 1.99f);

    const float3 centerColor = SampleSourceAtPixel(float2(sourceBase) + 0.5f).rgb;
    const float centerLuma = dot(TemporalCompress(centerColor / currentPreExposure),
                                 float3(0.25f, 0.5f, 0.25f));
    uint similarMask = 1u << 4;
    bool hasDissimilarSample = false;
    float dissimilarLumaMin = 1.0e20f;
    float dissimilarLumaMax = -1.0e20f;

    float3 colorSum = 0.0f;
    float lanczosWeightSum = 0.0f;
    float3 statisticsSum = 0.0f;
    float3 statisticsSquareSum = 0.0f;
    float statisticsWeightSum = 0.0f;
    float3 currentRgbMin = float3(1.0e20f, 1.0e20f, 1.0e20f);
    float3 currentRgbMax = float3(-1.0e20f, -1.0e20f, -1.0e20f);
    float3 axialColorSum = 0.0f;
    float axialLumaMin = dot(max(centerColor, 0.0f), float3(0.299f, 0.587f, 0.114f));
    float axialLumaMax = axialLumaMin;
    neighborhoodMin = float3(1.0e20f, 1.0e20f, 1.0e20f);
    neighborhoodMax = float3(-1.0e20f, -1.0e20f, -1.0e20f);

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 samplePixel = clamp(sourceBase + int2(x, y), int2(0, 0), sourceLimit);
            const float3 sampleColor = (x == 0 && y == 0)
                                           ? centerColor
                                           : SampleSourceAtPixel(float2(samplePixel) + 0.5f).rgb;
            // A source texel center is displaced from the stable output grid
            // by the current jitter. sampleCenter already includes that phase.
            const float2 sampleOffset = float2(samplePixel) + 0.5f - sampleCenter;
            const float biasedDistanceSquared =
                dot(sampleOffset, sampleOffset) * kernelBias * kernelBias;
            const float lanczosWeight = TaauLanczos2ApproxSq(biasedDistanceSquared);
            colorSum += sampleColor * lanczosWeight;
            lanczosWeightSum += lanczosWeight;

            const float3 workingColor =
                RgbToYCoCg(TemporalCompress(sampleColor / currentPreExposure));
            const float statisticsWeight = exp(-2.0f * dot(sampleOffset, sampleOffset));
            statisticsSum += workingColor * statisticsWeight;
            statisticsSquareSum += workingColor * workingColor * statisticsWeight;
            statisticsWeightSum += statisticsWeight;
            neighborhoodMin = min(neighborhoodMin, workingColor);
            neighborhoodMax = max(neighborhoodMax, workingColor);
            currentRgbMin = min(currentRgbMin, sampleColor);
            currentRgbMax = max(currentRgbMax, sampleColor);
            if (abs(x) + abs(y) == 1)
            {
                axialColorSum += sampleColor;
                const float axialLuma = dot(max(sampleColor, 0.0f), float3(0.299f, 0.587f, 0.114f));
                axialLumaMin = min(axialLumaMin, axialLuma);
                axialLumaMax = max(axialLumaMax, axialLuma);
            }

            if (x != 0 || y != 0)
            {
                const float sampleLuma = workingColor.x;
                const float similarThreshold = 0.05f * max(max(abs(sampleLuma), abs(centerLuma)), 0.01f);
                const uint sampleBit = 1u << ((uint)(y + 1) * 3u + (uint)(x + 1));
                if (abs(sampleLuma - centerLuma) <= similarThreshold)
                {
                    similarMask |= sampleBit;
                }
                else
                {
                    hasDissimilarSample = true;
                    dissimilarLumaMin = min(dissimilarLumaMin, sampleLuma);
                    dissimilarLumaMax = max(dissimilarLumaMax, sampleLuma);
                }
            }
        }
    }

    neighborhoodMean = statisticsSum / max(statisticsWeightSum, 1.0e-5f);
    neighborhoodDeviation = sqrt(max(statisticsSquareSum / max(statisticsWeightSum, 1.0e-5f) -
                                         neighborhoodMean * neighborhoodMean,
                                     0.0f));
    const float safeLanczosWeight = max(lanczosWeightSum, 1.0e-3f);
    const float3 reconstructedColor = clamp(colorSum / safeLanczosWeight, currentRgbMin, currentRgbMax);
    const float detailStrength = clamp(asfloat(_TaauDetailStrengthBits), 0.0f, 4.0f);
    const float centerLinearLuma = dot(max(centerColor, 0.0f), float3(0.299f, 0.587f, 0.114f));
    const float lowPassLuma = dot(max(centerColor * 0.5f + axialColorSum * 0.125f, 0.0f),
                                  float3(0.299f, 0.587f, 0.114f));
    const float localContrast = max(axialLumaMax - axialLumaMin, 0.002f);
    const float detailResidual = clamp(centerLinearLuma - lowPassLuma, -localContrast, localContrast);
    const float reconstructedLuma = dot(max(reconstructedColor, 0.0f), float3(0.299f, 0.587f, 0.114f));
    const float provenDelta = detailResidual * min(detailStrength, 2.0f);
    const float requestedExtra = detailResidual * max(detailStrength - 2.0f, 0.0f);
    const float extraKnee = max(abs(provenDelta) * 2.0f + reconstructedLuma * 0.1f, 1.0e-4f);
    const float limitedExtra = requestedExtra / (1.0f + abs(requestedExtra) / extraKnee);
    const float targetLuma = max(reconstructedLuma + provenDelta + limitedExtra, 0.0f);
    const float3 detailedColor = reconstructedLuma > 1.0e-4f
                                     ? reconstructedColor * (targetLuma / reconstructedLuma)
                                     : targetLuma.xxx;
    current = float4(detailedColor, 1.0f);

    // The rectification box was built from the same unsharpened nine samples.
    // Include the ALU-only compensated result so valid sharpened history is not
    // immediately clipped back to the low-frequency footprint.
    const float3 reconstructedWorking =
        RgbToYCoCg(TemporalCompress(reconstructedColor / currentPreExposure));
    const float3 detailedWorking =
        RgbToYCoCg(TemporalCompress(detailedColor / currentPreExposure));
    const float3 detailWorkingDelta = abs(detailedWorking - reconstructedWorking);
    neighborhoodMin = min(neighborhoodMin, detailedWorking);
    neighborhoodMax = max(neighborhoodMax, detailedWorking);
    neighborhoodDeviation = max(neighborhoodDeviation, detailWorkingDelta * 0.5f);
    currentSampleWeight = max(safeLanczosWeight * (1.0f / 12.0f), 1.0f / 64.0f);

    const bool isRidge = hasDissimilarSample &&
                         (centerLuma > dissimilarLumaMax || centerLuma < dissimilarLumaMin);
    const uint upperLeft = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4);
    const uint upperRight = (1u << 1) | (1u << 2) | (1u << 4) | (1u << 5);
    const uint lowerLeft = (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7);
    const uint lowerRight = (1u << 4) | (1u << 5) | (1u << 7) | (1u << 8);
    const bool hasSolidQuadrant = (similarMask & upperLeft) == upperLeft ||
                                  (similarMask & upperRight) == upperRight ||
                                  (similarMask & lowerLeft) == lowerLeft ||
                                  (similarMask & lowerRight) == lowerRight;
    thinFeatureLock = isRidge && !hasSolidQuadrant ? 1.0f : 0.0f;
}

float3 SampleHistoryCatmullRom(float2 uv, float2 texelSize)
{
    // Reduced MJP Catmull-Rom resolve from the Cauldron/MJP reference. The
    // middle two samples in each axis are combined through bilinear filtering,
    // keeping this in one pass while avoiding a point-sampled history step.
    float2 samplePos = uv / texelSize;
    float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
    float2 f = samplePos - texPos1;
    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / max(w12, 1e-5f);
    float2 texPos0 = (texPos1 - 1.0f) * texelSize;
    float2 texPos3 = (texPos1 + 2.0f) * texelSize;
    float2 texPos12 = (texPos1 + offset12) * texelSize;

    float3 result = 0.0f;
    result += PeripheralHistory.SampleLevel(LinearClampSampler, float2(texPos0.x, texPos0.y), 0.0f).rgb * w0.x * w0.y;
    result += PeripheralHistory.SampleLevel(LinearClampSampler, float2(texPos12.x, texPos0.y), 0.0f).rgb * w12.x * w0.y;
    result += PeripheralHistory.SampleLevel(LinearClampSampler, float2(texPos3.x, texPos0.y), 0.0f).rgb * w3.x * w0.y;
    result += PeripheralHistory.SampleLevel(LinearClampSampler, float2(texPos0.x, texPos12.y), 0.0f).rgb * w0.x * w12.y;
    result += PeripheralHistory.SampleLevel(LinearClampSampler, float2(texPos12.x, texPos12.y), 0.0f).rgb * w12.x * w12.y;
    result += PeripheralHistory.SampleLevel(LinearClampSampler, float2(texPos3.x, texPos12.y), 0.0f).rgb * w3.x * w12.y;
    result += PeripheralHistory.SampleLevel(LinearClampSampler, float2(texPos0.x, texPos3.y), 0.0f).rgb * w0.x * w3.y;
    result += PeripheralHistory.SampleLevel(LinearClampSampler, float2(texPos12.x, texPos3.y), 0.0f).rgb * w12.x * w3.y;
    result += PeripheralHistory.SampleLevel(LinearClampSampler, float2(texPos3.x, texPos3.y), 0.0f).rgb * w3.x * w3.y;
    return max(result, 0.0f);
}

)" R"(

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID,
            uint3 groupThreadId : SV_GroupThreadID)
{
#ifdef GAZE_ROI_JOINT_DETAIL
    LoadJointTiles(groupId, groupThreadId);
#endif
    uint2 p = id.xy;
    if (p.x >= (uint)_PeripheralResolveWidth || p.y >= (uint)_PeripheralResolveHeight)
        return;

    const float2 sourceSize = float2((float)_SrcWidth, (float)_SrcHeight);
    const float2 resolveSize = float2((float)_PeripheralResolveWidth, (float)_PeripheralResolveHeight);
    const int reconstructionMode = _PeripheralTemporal & 3;
    const bool depthInverted = (_PeripheralTemporal & 4) != 0;
    const bool currentOnlyMode = (_PeripheralTemporal & 8) != 0;
    const bool intermediateTaauMode = (_PeripheralTemporal & 16) != 0;
    const bool jointTaaMode = reconstructionMode == 3;
    const bool lightweightTaaMode = reconstructionMode == 2 || jointTaaMode;
#ifdef GAZE_ROI_JOINT_DETAIL
    const int2 jointTileOrigin = JointTileOrigin(groupId);
#endif
    float2 uv = (float2(p) + 0.5f) / resolveSize;
    float2 sourceCoordinate = (float2(p) + 0.5f) * sourceSize / resolveSize;
    float2 lightweightSampleCenter = sourceCoordinate;
    if ((currentOnlyMode || lightweightTaaMode) && _PeripheralJitterCancel != 0)
    {
        const float jitterSign = _PeripheralJitterSign < 0 ? -1.0f : 1.0f;
        lightweightSampleCenter += jitterSign * float2(_JitterOffsetX, _JitterOffsetY);
    }
#ifdef GAZE_ROI_JOINT_DETAIL
    float fusedCurrentResidual = 0.0f;
    float fusedLocalContrast = 0.002f;
#elif defined(GAZE_ROI_FUSED_DETAIL)
    float fusedCurrentResidual = 0.0f;
    float fusedLocalContrast = 0.002f;
    GetFusedCurrentDetail(lightweightSampleCenter, fusedCurrentResidual, fusedLocalContrast);
#endif
    float4 current;
    float3 neighborhoodMean;
    float3 neighborhoodDeviation;
    float3 legacyMinColor;
    float3 legacyMaxColor;
    float3 taauNeighborhoodMin = 0.0f;
    float3 taauNeighborhoodMax = 0.0f;
    float taauCurrentSampleWeight = 0.125f;
    float taauThinFeatureLock = 0.0f;
#ifdef GAZE_ROI_JOINT_DETAIL
    if (jointTaaMode)
        EvaluateJointPeripheral(lightweightSampleCenter, jointTileOrigin, current,
                                neighborhoodMean, neighborhoodDeviation,
                                fusedCurrentResidual, fusedLocalContrast);
    else
#endif
    if (intermediateTaauMode)
        EvaluateIntermediateTaau(sourceCoordinate, lightweightSampleCenter, current,
                                 neighborhoodMean, neighborhoodDeviation,
                                 taauNeighborhoodMin, taauNeighborhoodMax,
                                 taauCurrentSampleWeight, taauThinFeatureLock);
    else if (currentOnlyMode || lightweightTaaMode)
        EvaluateLightweightPeripheral(lightweightSampleCenter, lightweightTaaMode, current,
                                      neighborhoodMean, neighborhoodDeviation);
    else
    {
        EvaluateCurrentPeripheral(uv, current, legacyMinColor, legacyMaxColor);
        neighborhoodMean = 0.5f * (legacyMinColor + legacyMaxColor);
        neighborhoodDeviation = 0.5f * (legacyMaxColor - legacyMinColor);
    }

    uint2 sourcePoint = lightweightTaaMode
                            ? min(uint2(max(floor(lightweightSampleCenter), 0.0f)),
                                  uint2(_SrcWidth - 1, _SrcHeight - 1))
                            : min(uint2(sourceCoordinate), uint2(_SrcWidth - 1, _SrcHeight - 1));
    uint2 selectedSource = sourcePoint;
    float currentDepth = 0.0f;
    if (_PeripheralTemporalMotionReprojection != 0)
    {
#ifdef GAZE_ROI_JOINT_DETAIL
        currentDepth = JointDepthLoad(int2(sourcePoint), jointTileOrigin);
#else
        currentDepth = CurrentDepth.Load(int3(sourcePoint +
            uint2(_PeripheralDepthBaseX, _PeripheralDepthBaseY), 0));
#endif
        if (reconstructionMode > 1)
        {
            const int2 sourceLimit = int2(_SrcWidth - 1, _SrcHeight - 1);
            float closestDepth = depthInverted ? 0.0f : 1.0f;
            [unroll]
            for (int y = -1; y <= 1; ++y)
            {
                [unroll]
                for (int x = -1; x <= 1; ++x)
                {
                    const uint2 candidateSource =
                        uint2(clamp(int2(sourcePoint) + int2(x, y), int2(0, 0), sourceLimit));
#ifdef GAZE_ROI_JOINT_DETAIL
                    const float candidateDepth = JointDepthLoad(int2(candidateSource), jointTileOrigin);
#else
                    const float candidateDepth = CurrentDepth.Load(int3(candidateSource +
                        uint2(_PeripheralDepthBaseX, _PeripheralDepthBaseY), 0));
#endif
                    if (!isfinite(candidateDepth))
                        continue;
                    const bool candidateIsCloser = depthInverted ? candidateDepth > closestDepth
                                                                  : candidateDepth < closestDepth;
                    if (candidateIsCloser)
                    {
                        closestDepth = candidateDepth;
                        selectedSource = candidateSource;
                    }
                }
            }
            // Thin geometry can move in and out of the center depth texel over
            // the jitter cycle. Mode 1 stores the same closest-surface depth
            // whose MV it uses, keeping that history contract coherent.
            if (lightweightTaaMode && isfinite(closestDepth))
                currentDepth = closestDepth;
        }
#ifndef GAZE_ROI_JOINT_DETAIL
        if (reconstructionMode > 1)
            NextDepth[p] = currentDepth;
#endif
    }

    if (reconstructionMode == 0)
    {
        PeripheralOutput[p] = current;
        return;
    }

    if (_PeripheralTemporalInitialized == 0)
    {
        if (lightweightTaaMode)
            current.a = intermediateTaauMode ? taauCurrentSampleWeight : 0.125f;
#ifdef GAZE_ROI_JOINT_DETAIL
        NextDepthDetail[p] = float2(currentDepth,
                                    clamp(fusedCurrentResidual,
                                          -max(fusedLocalContrast, 0.001f),
                                          max(fusedLocalContrast, 0.001f)));
#elif defined(GAZE_ROI_FUSED_DETAIL)
        NextDetail[p] = float2(clamp(fusedCurrentResidual, -fusedLocalContrast, fusedLocalContrast), currentDepth);
#endif
        PeripheralOutput[p] = current;
        return;
    }

)" R"(

    float4 history = 0.0f;
    float historyConfidence = intermediateTaauMode ? 0.0f : 1.0f;
    if (!intermediateTaauMode)
    {
        history = PeripheralHistory.Load(int3(p, 0));
        if (lightweightTaaMode)
            historyConfidence = saturate(history.a);
    }
    bool acceptHistory = true;
    float2 motion = 0.0f;
    if (_PeripheralTemporalMotionReprojection != 0)
    {
        const int2 mvLimit = int2(_PeripheralMotionVectorWidth - 1, _PeripheralMotionVectorHeight - 1);
        const uint2 selectedMvPixel = _PeripheralMotionVectorsLowResolution != 0
                                          ? selectedSource
                                          : min(uint2((float2(selectedSource) + 0.5f) *
                                                      float2(_PeripheralMotionVectorWidth,
                                                             _PeripheralMotionVectorHeight) / sourceSize),
                                                uint2(mvLimit));
        motion = PeripheralMotionVectors.Load(int3(selectedMvPixel +
            uint2(_PeripheralMotionVectorBaseX, _PeripheralMotionVectorBaseY), 0)).xy;
        motion *= float2(_PeripheralMotionVectorScaleX, _PeripheralMotionVectorScaleY);
        const float2 motionDomain = _PeripheralMotionVectorsLowResolution != 0
                                        ? sourceSize
                                        : float2((float)_PeripheralMotionVectorWidth,
                                                 (float)_PeripheralMotionVectorHeight);
        motion *= resolveSize / motionDomain;
        if (_PeripheralMotionVectorsJittered != 0)
        {
            const float2 jitterDelta = float2(_PreviousJitterOffsetX - _JitterOffsetX,
                                              _PreviousJitterOffsetY - _JitterOffsetY) *
                                       resolveSize / sourceSize;
            // MVJittered means the game vector already contains the phase
            // delta. Mode 1 reconstructs a stable current sample, so remove
            // that embedded delta exactly once. Preserve the older modes'
            // established convention until they are validated separately.
            if (lightweightTaaMode && _PeripheralJitterCancel != 0)
                motion -= jitterDelta;
            else if (!lightweightTaaMode)
                motion += jitterDelta;
        }
        float2 previousPixel = float2(p) + motion;
        bool inBounds = all(previousPixel >= 0.0f) &&
                        all(previousPixel < resolveSize);
        const float2 previousUv = (previousPixel + 0.5f) / resolveSize;
#ifdef GAZE_ROI_JOINT_DETAIL
        const float4 previousDepthFootprint =
            PreviousDepthDetail.GatherRed(LinearClampSampler, previousUv);
#else
        const float4 previousDepthFootprint = PreviousDepth.GatherRed(LinearClampSampler, previousUv);
#endif
        // FSR-style conservative disocclusion coverage: compare against the
        // closest previous surface in the bilinear footprint, not one rounded
        // texel that can select background across a thin foreground edge.
        const float previousDepth = depthInverted
                                        ? max(max(previousDepthFootprint.x, previousDepthFootprint.y),
                                              max(previousDepthFootprint.z, previousDepthFootprint.w))
                                        : min(min(previousDepthFootprint.x, previousDepthFootprint.y),
                                              min(previousDepthFootprint.z, previousDepthFootprint.w));
        float depthThreshold = max(0.001f, max(abs(currentDepth), abs(previousDepth)) * 0.02f);
        bool depthMatches = isfinite(currentDepth) && isfinite(previousDepth) &&
                            abs(currentDepth - previousDepth) <= depthThreshold;
        acceptHistory = inBounds && depthMatches && _PeripheralTemporalHistoryReset == 0;
        if (inBounds)
        {
            const float4 bilinearHistory = PeripheralHistory.SampleLevel(LinearClampSampler, previousUv, 0.0f);
            if (lightweightTaaMode)
                historyConfidence = intermediateTaauMode
                                        ? (isfinite(bilinearHistory.a) ? max(bilinearHistory.a, 0.0f) : 0.0f)
                                        : saturate(bilinearHistory.a);
            const bool useHighQualityHistory = !intermediateTaauMode && reconstructionMode > 1 && acceptHistory &&
                                               historyConfidence >= 0.5f && length(motion) <= 0.75f;
            history = useHighQualityHistory
                          ? float4(SampleHistoryCatmullRom(previousUv, 1.0f / resolveSize), bilinearHistory.a)
                          : bilinearHistory;
        }
    }

#ifdef GAZE_ROI_JOINT_DETAIL
    const float resolvedJointDetail = ResolveJointDetail(
        p, fusedCurrentResidual, fusedLocalContrast, motion, acceptHistory, historyConfidence);
    NextDepthDetail[p] = float2(currentDepth, resolvedJointDetail);
#elif defined(GAZE_ROI_FUSED_DETAIL)
    WriteFusedDetail(p, fusedCurrentResidual, fusedLocalContrast, currentDepth, motion);
#endif

    const float motionPixels = length(motion);
    if (intermediateTaauMode)
    {
        const float motionFactor = saturate(motionPixels * 0.5f);
        const float currentPreExposure = CurrentPeripheralPreExposure();
        const float previousPreExposure = PreviousPeripheralPreExposure();
        const float3 currentWorking =
            RgbToYCoCg(TemporalCompress(current.rgb / currentPreExposure));
        const float3 historyWorking =
            RgbToYCoCg(TemporalCompress(history.rgb / previousPreExposure));

        const float scaleRatio = max(resolveSize.x / max(sourceSize.x, 1.0f),
                                     resolveSize.y / max(sourceSize.y, 1.0f));
        const float scaleRatioSquared = scaleRatio * scaleRatio;
        const float staticBoxScale = min(scaleRatioSquared * scaleRatioSquared * scaleRatioSquared, 20.0f);
        const float boxScale = lerp(staticBoxScale, 1.0f, motionFactor);
        const float3 varianceExtent = max(neighborhoodDeviation * boxScale,
                                          float3(0.006f, 0.012f, 0.012f));
        const float3 boxMin = max(neighborhoodMean - varianceExtent, taauNeighborhoodMin);
        const float3 boxMax = min(neighborhoodMean + varianceExtent, taauNeighborhoodMax);
        const float3 boxCenter = 0.5f * (boxMin + boxMax);
        const float3 boxExtent = max(0.5f * (boxMax - boxMin), 1.0e-4f);
        float3 rectifiedHistory = ClipToAabb(historyWorking, boxCenter, boxExtent);

        const float normalizedLumaDelta =
            abs(currentWorking.x - rectifiedHistory.x) /
            max(neighborhoodDeviation.x * 2.0f + 0.01f, 0.01f);
        float reactive = saturate(max(normalizedLumaDelta - 1.0f, 0.0f) *
                                  0.25f * _PeripheralTemporalReactiveScale);
        const float lockContribution = taauThinFeatureLock * (1.0f - motionFactor);
        reactive *= 1.0f - 0.75f * lockContribution;
        rectifiedHistory = lerp(rectifiedHistory, historyWorking,
                                0.5f * lockContribution * (1.0f - reactive));

        const float staticAccumulationCap =
            max(taauCurrentSampleWeight, saturate(_PeripheralTemporalHistoryWeight));
        const float motionAccumulationCap = max(taauCurrentSampleWeight, 0.25f);
        const float accumulationCap =
            lerp(staticAccumulationCap, min(staticAccumulationCap, motionAccumulationCap), motionFactor);
        float previousAccumulation = acceptHistory ? min(historyConfidence, accumulationCap) : 0.0f;
        previousAccumulation *= (1.0f - reactive) * (1.0f - reactive);
        previousAccumulation = min(previousAccumulation,
                                   max(accumulationCap - taauCurrentSampleWeight, 0.0f));
        const float totalAccumulation = previousAccumulation + taauCurrentSampleWeight;
        const float currentWeight = taauCurrentSampleWeight / max(totalAccumulation, 1.0e-5f);
        const float3 resolvedWorking = acceptHistory
                                           ? lerp(rectifiedHistory, currentWorking, currentWeight)
                                           : currentWorking;
        const float3 resolvedColor =
            TemporalExpand(YCoCgToRgb(resolvedWorking)) * currentPreExposure;
        PeripheralOutput[p] = float4(resolvedColor, totalAccumulation);
        return;
    }

    if (lightweightTaaMode)
    {
        const float motionConfidence = 1.0f - saturate(motionPixels * 0.5f);
        const float boxSize = lerp(0.5f, 2.5f, motionConfidence);
        const float3 boxExtent = max(neighborhoodDeviation * boxSize,
                                     float3(0.01f, 0.02f, 0.02f));
        const float currentPreExposure = CurrentPeripheralPreExposure();
        const float previousPreExposure = PreviousPeripheralPreExposure();
        const float3 currentWorking =
            RgbToYCoCg(TemporalCompress(current.rgb / currentPreExposure));
        const float3 historyWorking =
            RgbToYCoCg(TemporalCompress(history.rgb / previousPreExposure));
        float3 clippedHistoryWorking = ClipToAabb(historyWorking, neighborhoodMean, boxExtent);
        // A high-confidence static history is additional evidence for a thin
        // feature that may have zero coverage in this jitter phase. Relax the
        // current-frame AABB only in that case; motion or invalid depth still
        // removes the preservation immediately.
        const float subpixelHistoryPreservation = 0.75f * historyConfidence * motionConfidence;
        clippedHistoryWorking = lerp(clippedHistoryWorking, historyWorking, subpixelHistoryPreservation);

        // A jitter phase can radically change an aliased edge without changing
        // its depth or stable motion. Normalize the color signal by local
        // variance and strongly reduce it on stationary, depth-valid surfaces
        // instead of discarding exactly the history TAA needs to accumulate.
        const float normalizedLumaDelta =
            abs(currentWorking.x - clippedHistoryWorking.x) /
            max(neighborhoodDeviation.x * 2.0f + 0.02f, 0.02f);
        const float colorChange = saturate(max(normalizedLumaDelta - 1.5f, 0.0f) *
                                           0.25f * _PeripheralTemporalReactiveScale);
        const float motionReactivity = lerp(0.1f, 1.0f, saturate(motionPixels * 0.5f));
        const float reactiveWeight = colorChange * motionReactivity;
        const float maximumHistoryWeight =
            saturate(_PeripheralTemporalHistoryWeight) * (1.0f - reactiveWeight);
        const float historyWeight = min(maximumHistoryWeight, historyConfidence);
        const float3 resolvedWorking = acceptHistory
                                           ? lerp(currentWorking, clippedHistoryWorking, historyWeight)
                                           : currentWorking;
        const float3 resolvedColor = TemporalExpand(YCoCgToRgb(resolvedWorking)) * currentPreExposure;
        const float nextConfidence = acceptHistory ? min(historyConfidence + 0.125f, 1.0f) : 0.125f;
        PeripheralOutput[p] = float4(resolvedColor, nextConfidence);
        return;
    }

    const float motionConfidence = 1.0f - saturate(motionPixels * 0.5f);
    const float boxSize = lerp(0.5f, 2.0f, motionConfidence);
    const float3 neighborhoodMin = neighborhoodMean - neighborhoodDeviation * boxSize;
    const float3 neighborhoodMax = neighborhoodMean + neighborhoodDeviation * boxSize;
    const float3 clipMin = reconstructionMode > 1 ? neighborhoodMin : legacyMinColor;
    const float3 clipMax = reconstructionMode > 1 ? neighborhoodMax : legacyMaxColor;
    float3 clippedHistory = clamp(history.rgb, clipMin, clipMax);
    float colorDelta = max(max(abs(current.r - clippedHistory.r), abs(current.g - clippedHistory.g)),
                           abs(current.b - clippedHistory.b));
    float reactiveWeight = saturate(colorDelta * _PeripheralTemporalReactiveScale);
    // The control is the fraction of validated history to retain. Reactive
    // changes reduce that fraction; they no longer replace it with a fixed
    // current-frame weight, which made the old slider effectively inert.
    const float historyWeight = saturate(_PeripheralTemporalHistoryWeight) * (1.0f - reactiveWeight);
    float currentWeight = 1.0f - historyWeight;
    if (!acceptHistory)
        currentWeight = 1.0f;
    PeripheralOutput[p] = acceptHistory
                              ? float4(lerp(clippedHistory, current.rgb, currentWeight), current.a)
                              : current;
#ifndef GAZE_ROI_JOINT_DETAIL
    if (_PeripheralTemporalMotionReprojection != 0 && reconstructionMode <= 1)
        NextDepth[p] = currentDepth;
#endif
}
)";

// Lightweight temporal detail reconstruction. The main peripheral TAA remains
// at render resolution; this pass stores only signed luminance residual and
// depth in an intermediate-resolution RG16 history.
static const char* gazeRoiPeripheralDetailShader = R"(
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
    int _PeripheralResolveWidth;
    int _PeripheralResolveHeight;
    uint _ReservedDetailConstant0;
    uint _ReservedDetailConstant1;
    uint _ReservedDetailConstant2;
    uint _PeripheralPreExposureBits;
    uint _PreviousPeripheralPreExposureBits;
};

Texture2D<float4> LowResColor : register(t0);
Texture2D<float4> StableBase : register(t1);
Texture2D<float4> MotionVectors : register(t2);
Texture2D<float> CurrentDepth : register(t3);
Texture2D<float2> PreviousDetail : register(t4);
RWTexture2D<float2> NextDetail : register(u0);
SamplerState LinearClampSampler : register(s0);

float DetailCurrentPreExposure()
{
    const float value = asfloat(_PeripheralPreExposureBits);
    return isfinite(value) && value > 1.0e-6f ? value : 1.0f;
}

float DetailPreviousPreExposure()
{
    const float value = asfloat(_PreviousPeripheralPreExposureBits);
    return isfinite(value) && value > 1.0e-6f ? value : DetailCurrentPreExposure();
}

float Luma(float3 color)
{
    return dot(max(color, 0.0f), float3(0.299f, 0.587f, 0.114f));
}

float3 SampleCurrent(float2 sourcePosition)
{
    sourcePosition = clamp(sourcePosition, 0.5f,
                           float2((float)_SrcWidth, (float)_SrcHeight) - 0.5f);
    const float2 sourcePixel = sourcePosition + float2((float)_SrcBaseX, (float)_SrcBaseY);
    const float2 sourceUv = sourcePixel / float2((float)_SrcTextureWidth, (float)_SrcTextureHeight);
    return LowResColor.SampleLevel(LinearClampSampler, sourceUv, 0.0f).rgb;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    if (p.x >= (uint)_PeripheralResolveWidth || p.y >= (uint)_PeripheralResolveHeight)
        return;

    const float2 sourceSize = float2((float)_SrcWidth, (float)_SrcHeight);
    const float2 detailSize = float2((float)_PeripheralResolveWidth, (float)_PeripheralResolveHeight);
    float2 sourcePosition = (float2(p) + 0.5f) * sourceSize / detailSize;
    if (_PeripheralJitterCancel != 0)
    {
        const float jitterSign = _PeripheralJitterSign < 0 ? -1.0f : 1.0f;
        sourcePosition += jitterSign * float2(_JitterOffsetX, _JitterOffsetY);
    }
    sourcePosition = clamp(sourcePosition, 0.5f, sourceSize - 0.5f);

    const float centerLuma = Luma(SampleCurrent(sourcePosition));
    const float leftLuma = Luma(SampleCurrent(sourcePosition + float2(-1.0f, 0.0f)));
    const float rightLuma = Luma(SampleCurrent(sourcePosition + float2(1.0f, 0.0f)));
    const float upLuma = Luma(SampleCurrent(sourcePosition + float2(0.0f, -1.0f)));
    const float downLuma = Luma(SampleCurrent(sourcePosition + float2(0.0f, 1.0f)));
    const float lowPassLuma = centerLuma * 0.5f +
                              (leftLuma + rightLuma + upLuma + downLuma) * 0.125f;
    const float currentResidual = centerLuma - lowPassLuma;
    const float neighborhoodMin = min(centerLuma, min(min(leftLuma, rightLuma), min(upLuma, downLuma)));
    const float neighborhoodMax = max(centerLuma, max(max(leftLuma, rightLuma), max(upLuma, downLuma)));
    const float localContrast = max(neighborhoodMax - neighborhoodMin, 0.002f);

    const int2 sourceLimit = int2(_SrcWidth - 1, _SrcHeight - 1);
    const int2 firstSource = clamp(int2(floor(sourcePosition - 0.5f)), int2(0, 0), sourceLimit);
    uint2 selectedSource = uint2(clamp(int2(floor(sourcePosition)), int2(0, 0), sourceLimit));
    const bool depthInverted = (_PeripheralTemporal & 4) != 0;
    float currentDepth = CurrentDepth.Load(int3(selectedSource +
        uint2(_PeripheralDepthBaseX, _PeripheralDepthBaseY), 0));
    float closestDepth = depthInverted ? 0.0f : 1.0f;
    bool foundDepth = false;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            const uint2 candidate = uint2(clamp(firstSource + int2(x, y), int2(0, 0), sourceLimit));
            const float candidateDepth = CurrentDepth.Load(int3(candidate +
                uint2(_PeripheralDepthBaseX, _PeripheralDepthBaseY), 0));
            if (!isfinite(candidateDepth))
                continue;
            const bool closer = !foundDepth || (depthInverted ? candidateDepth > closestDepth
                                                               : candidateDepth < closestDepth);
            if (closer)
            {
                foundDepth = true;
                closestDepth = candidateDepth;
                selectedSource = candidate;
            }
        }
    }
    if (foundDepth)
        currentDepth = closestDepth;

    const int2 mvLimit = int2(_PeripheralMotionVectorWidth - 1, _PeripheralMotionVectorHeight - 1);
    const uint2 selectedMvPixel = _PeripheralMotionVectorsLowResolution != 0
                                      ? selectedSource
                                      : min(uint2((float2(selectedSource) + 0.5f) *
                                                  float2(_PeripheralMotionVectorWidth,
                                                         _PeripheralMotionVectorHeight) / sourceSize),
                                            uint2(mvLimit));
    float2 motion = MotionVectors.Load(int3(selectedMvPixel +
        uint2(_PeripheralMotionVectorBaseX, _PeripheralMotionVectorBaseY), 0)).xy;
    motion *= float2(_PeripheralMotionVectorScaleX, _PeripheralMotionVectorScaleY);
    const float2 motionDomain = _PeripheralMotionVectorsLowResolution != 0
                                    ? sourceSize
                                    : float2(_PeripheralMotionVectorWidth, _PeripheralMotionVectorHeight);
    motion *= detailSize / motionDomain;
    if (_PeripheralMotionVectorsJittered != 0 && _PeripheralJitterCancel != 0)
    {
        const float2 jitterDelta = float2(_PreviousJitterOffsetX - _JitterOffsetX,
                                          _PreviousJitterOffsetY - _JitterOffsetY) *
                                   detailSize / sourceSize;
        motion -= jitterDelta;
    }

    const float2 previousPixel = float2(p) + motion;
    const bool inBounds = all(previousPixel >= 0.0f) && all(previousPixel < detailSize);
    float2 previous = float2(0.0f, currentDepth);
    if (inBounds && _PeripheralTemporalInitialized != 0)
    {
        const float2 previousUv = (previousPixel + 0.5f) / detailSize;
        previous = PreviousDetail.SampleLevel(LinearClampSampler, previousUv, 0.0f);
        previous.x *= DetailCurrentPreExposure() / DetailPreviousPreExposure();
    }
    const float depthThreshold = max(0.002f, max(abs(currentDepth), abs(previous.y)) * 0.02f);
    const bool depthMatches = isfinite(currentDepth) && isfinite(previous.y) &&
                              abs(currentDepth - previous.y) <= depthThreshold;
    const bool acceptHistory = _PeripheralTemporalInitialized != 0 &&
                               _PeripheralTemporalHistoryReset == 0 && inBounds && depthMatches;

    const float clippedHistory = clamp(previous.x, -localContrast, localContrast);
    const float normalizedChange = abs(currentResidual - clippedHistory) / localContrast;
    float reactive = saturate(max(normalizedChange - 0.5f, 0.0f) *
                              0.25f * _PeripheralTemporalReactiveScale);
    if (abs(currentResidual) < localContrast * 0.1f)
        reactive *= 0.25f;
    const float motionReactivity = saturate(length(motion) * 0.125f);
    const float historyWeight = saturate(_PeripheralTemporalHistoryWeight) *
                                (1.0f - reactive) * (1.0f - 0.75f * motionReactivity);
    const float resolvedResidual = acceptHistory
                                       ? lerp(currentResidual, clippedHistory, historyWeight)
                                       : currentResidual;
    NextDetail[p] = float2(clamp(resolvedResidual, -localContrast, localContrast), currentDepth);
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

bool GazeRoi_Dx12::EnsurePeripheralExtendedDetailResources()
{
    if (_peripheralFusedDetailRootSignature == nullptr)
    {
        CD3DX12_STATIC_SAMPLER_DESC sampler(0);
        sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        if (!CreateGazeRoiRootSignature(_device, 6, 3, sampler, &_peripheralFusedDetailRootSignature))
            return false;
    }

    if (!_peripheralFusedDetailHeapsInitialized)
    {
        bool initialized = true;
        for (auto& heap : _peripheralFusedDetailFrameHeaps)
        {
            if (!heap.Initialize(_device, 6, 3, 0))
            {
                initialized = false;
                break;
            }
        }
        if (!initialized)
        {
            for (auto& heap : _peripheralFusedDetailFrameHeaps)
                heap.ReleaseHeaps();
            return false;
        }
        _peripheralFusedDetailHeapsInitialized = true;
    }
    return true;
}

bool GazeRoi_Dx12::EnsurePeripheralFusedDetailPipeline()
{
    if (!EnsurePeripheralExtendedDetailResources())
        return false;
    if (_peripheralFusedDetailPipelineState != nullptr)
        return true;

    const std::string fusedSource = std::string("#define GAZE_ROI_FUSED_DETAIL 1\n") +
                                    gazeRoiPeripheralShader;
    ID3DBlob* shaderBlob = CompileShader(fusedSource.c_str(), "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
        return false;
    const bool created = CreateComputeShader(_device, _peripheralFusedDetailRootSignature,
                                             &_peripheralFusedDetailPipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    return created;
}

bool GazeRoi_Dx12::EnsurePeripheralJointDetailPipeline()
{
    if (!EnsurePeripheralExtendedDetailResources())
        return false;
    if (_peripheralJointDetailPipelineState != nullptr)
        return true;

    const std::string jointSource = std::string("#define GAZE_ROI_FUSED_DETAIL 1\n"
                                                "#define GAZE_ROI_JOINT_DETAIL 1\n") +
                                    gazeRoiPeripheralShader;
    ID3DBlob* shaderBlob = CompileShader(jointSource.c_str(), "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
        return false;
    const bool created = CreateComputeShader(_device, _peripheralFusedDetailRootSignature,
                                             &_peripheralJointDetailPipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    return created;
}

bool GazeRoi_Dx12::EnsurePeripheralDetailPipeline()
{
    if (_peripheralDetailPipelineState == nullptr)
    {
        ID3DBlob* shaderBlob = CompileShader(gazeRoiPeripheralDetailShader, "CSMain", "cs_5_0");
        if (shaderBlob == nullptr)
            return false;
        const bool created =
            CreateComputeShader(_device, _rootSignature, &_peripheralDetailPipelineState, shaderBlob, {});
        SAFE_RELEASE(shaderBlob);
        if (!created)
            return false;
    }

    if (!_peripheralDetailHeapsInitialized)
        _peripheralDetailHeapsInitialized =
            InitHeaps(_device, _peripheralDetailFrameHeaps, GAZE_ROI_NUM_OF_HEAPS);
    return _peripheralDetailHeapsInitialized;
}

bool GazeRoi_Dx12::EnsureEasuCompositePipeline()
{
    if (_easuCompositePipelineState != nullptr)
        return true;

    ID3DBlob* shaderBlob = CompileShader(gazeRoiEasuCompositeShader, "CSMain", "cs_5_0");
    if (shaderBlob == nullptr)
        return false;
    const bool created =
        CreateComputeShader(_device, _rootSignature, &_easuCompositePipelineState, shaderBlob, {});
    SAFE_RELEASE(shaderBlob);
    return created;
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
                                             bool motionReprojectionEnabled, bool jointDetailEnabled,
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
    const bool needDepthHistory = temporalEnabled && motionReprojectionEnabled && !jointDetailEnabled;
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

bool GazeRoi_Dx12::CreatePeripheralDetailResources(ID3D12Device* device, ID3D12Resource* colorTemplate,
                                                    uint32_t width, uint32_t height, bool enabled, bool jointMode,
                                                    D3D12_RESOURCE_STATES initialState)
{
    const auto releaseResources = [&]()
    {
        GazeRoiFrameSync::DeferRelease(_peripheralDetailHistory[0]);
        GazeRoiFrameSync::DeferRelease(_peripheralDetailHistory[1]);
        _peripheralDetailHistory[0] = nullptr;
        _peripheralDetailHistory[1] = nullptr;
        _peripheralDetailHistoryInitialized = false;
        _peripheralDetailHistoryIndex = 0;
        _peripheralDetailJointMode = -1;
    };

    if (!enabled)
    {
        if (_peripheralDetailHistory[0] != nullptr || _peripheralDetailHistory[1] != nullptr)
            releaseResources();
        return true;
    }
    if (device == nullptr || colorTemplate == nullptr || width == 0 || height == 0)
        return false;

    constexpr D3D12_RESOURCE_FLAGS detailFlags =
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    const DXGI_FORMAT detailFormat = DXGI_FORMAT_R16G16_FLOAT;
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(detailFormat, width, height, 1, 1, 1, 0,
                                                   detailFlags);
    const auto matches = [&desc](ID3D12Resource* resource)
    {
        if (resource == nullptr)
            return false;
        const auto oldDesc = resource->GetDesc();
        return oldDesc.Width == desc.Width && oldDesc.Height == desc.Height && oldDesc.Format == desc.Format &&
               oldDesc.Flags == desc.Flags;
    };
    if (_peripheralDetailJointMode == (jointMode ? 1 : 0) &&
        matches(_peripheralDetailHistory[0]) && matches(_peripheralDetailHistory[1]))
        return true;

    releaseResources();
    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags {};
    const HRESULT heapResult = colorTemplate->GetHeapProperties(&heapProperties, &heapFlags);
    if (FAILED(heapResult))
    {
        LOG_ERROR("[{}] GetHeapProperties detail history result: {:X}", _name,
                  static_cast<UINT64>(heapResult));
        return false;
    }

    const auto createHistory = [&](ID3D12Resource** resource, const wchar_t* name)
    {
        const HRESULT result = device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc,
                                                                initialState, nullptr, IID_PPV_ARGS(resource));
        if (FAILED(result))
        {
            LOG_ERROR("[{}] CreateCommittedResource detail history result: {:X}", _name,
                      static_cast<UINT64>(result));
            return false;
        }
        (*resource)->SetName(name);
        return true;
    };
    if (!createHistory(&_peripheralDetailHistory[0], L"GazeRoi_Peripheral_DetailHistory0") ||
        !createHistory(&_peripheralDetailHistory[1], L"GazeRoi_Peripheral_DetailHistory1"))
    {
        SAFE_RELEASE(_peripheralDetailHistory[0]);
        SAFE_RELEASE(_peripheralDetailHistory[1]);
        return false;
    }

    _peripheralDetailHistoryState[0] = initialState;
    _peripheralDetailHistoryState[1] = initialState;
    _peripheralDetailJointMode = jointMode ? 1 : 0;
    LOG_INFO("[{}] Created peripheral temporal detail history: {}x{} {}", _name, width, height,
             jointMode ? "RG16F depth/residual joint" : "RG16F residual/depth");
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
                             const GazeRoiConstants& constants, uint32_t frameSlot, bool peripheralDetailEnabled,
                             uint32_t peripheralDetailWidth, uint32_t peripheralDetailHeight,
                             float peripheralDetailStrength, bool peripheralEasu)
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
    const int32_t peripheralReconstructionMode = peripheralConstants.peripheralTemporal & 3;
    const int32_t peripheralHistoryMode = peripheralConstants.peripheralTemporal & (3 | 16);
    const bool usesPeripheralHistory = peripheralReconstructionMode != 0;
    const bool useFusedDetail = peripheralDetailEnabled && peripheralReconstructionMode == 2 &&
                                (peripheralConstants.peripheralTemporal & 16) == 0 &&
                                peripheralConstants.peripheralTemporalMotionReprojection != 0 &&
                                peripheralDetailWidth == static_cast<uint32_t>(peripheralConstants.peripheralResolveWidth) &&
                                peripheralDetailHeight == static_cast<uint32_t>(peripheralConstants.peripheralResolveHeight);
    const bool useJointDetail = peripheralDetailEnabled && peripheralReconstructionMode == 3 &&
                                peripheralConstants.peripheralTemporalMotionReprojection != 0 &&
                                peripheralDetailWidth == static_cast<uint32_t>(peripheralConstants.peripheralResolveWidth) &&
                                peripheralDetailHeight == static_cast<uint32_t>(peripheralConstants.peripheralResolveHeight);
    const bool useExtendedDetail = useFusedDetail || useJointDetail;
    ID3D12Resource* detailForComposite = nullptr;
    const uint32_t previousDetailIndex = _peripheralDetailHistoryIndex;
    const uint32_t nextDetailIndex = (_peripheralDetailHistoryIndex + 1) % 2;
    if (peripheralHistoryMode != _peripheralHistoryMode)
    {
        _peripheralHistoryInitialized = false;
        _peripheralHistoryIndex = 0;
        _peripheralHistoryMode = peripheralHistoryMode;
    }

    if (usePeripheralEffects)
    {
        const bool pipelineReady = useJointDetail
                                       ? EnsurePeripheralJointDetailPipeline()
                                       : (useFusedDetail ? EnsurePeripheralFusedDetailPipeline()
                                                         : EnsurePeripheralPipeline());
        if (!pipelineReady ||
            peripheralConstants.peripheralResolveWidth <= 0 ||
            peripheralConstants.peripheralResolveHeight <= 0)
            return false;

        FrameDescriptorHeap& peripheralHeap = useExtendedDetail
                                                  ? _peripheralFusedDetailFrameHeaps[frameSlot]
                                                  : _peripheralFrameHeaps[frameSlot];
        ID3D12Resource* historyForRead = lowResColor;
        ID3D12Resource* depthHistoryForRead = lowResColor;

        if (usesPeripheralHistory)
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
                if (!useJointDetail)
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
        }
        else
        {
            if (_peripheralOutput == nullptr)
                return false;
            _peripheralHistoryInitialized = false;
            SetPeripheralOutputState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            peripheralForComposite = _peripheralOutput;
        }

        if (useExtendedDetail)
        {
            if (_peripheralDetailHistory[0] == nullptr || _peripheralDetailHistory[1] == nullptr ||
                lowResDepth == nullptr || peripheralMotionVectors == nullptr)
                return false;
            Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                        _peripheralDetailHistory[previousDetailIndex],
                                        &_peripheralDetailHistoryState[previousDetailIndex]);
            Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        _peripheralDetailHistory[nextDetailIndex],
                                        &_peripheralDetailHistoryState[nextDetailIndex]);
            peripheralConstants.easuConst0[0] = _peripheralDetailHistoryInitialized ? 1u : 0u;
            if (useJointDetail)
                depthHistoryForRead = _peripheralDetailHistory[previousDetailIndex];
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
                CreateUnorderedAccessView(_device,
                                          useJointDetail ? _peripheralDetailHistory[nextDetailIndex]
                                                         : _peripheralDepthHistory[nextHistoryIndex],
                                          peripheralHeap.GetUavCPU(1), 0);
            }
            else
            {
                // Keep the complete root descriptor table valid even though this shader variant does not write u1.
                CreateUnorderedAccessView(_device, peripheralForComposite, peripheralHeap.GetUavCPU(1), 0);
            }
            if (useExtendedDetail)
            {
                CreateShaderResourceView(_device, _peripheralDetailHistory[previousDetailIndex],
                                         peripheralHeap.GetSrvCPU(5));
                CreateUnorderedAccessView(_device, _peripheralDetailHistory[nextDetailIndex],
                                          peripheralHeap.GetUavCPU(2), 0);
            }
        }
        catch (const std::exception& e)
        {
            LOG_WARN("[{}] Failed to create peripheral descriptors: {}", _name, e.what());
            return false;
        }

        ID3D12DescriptorHeap* peripheralHeaps[] = { peripheralHeap.GetHeapCSU() };
        commandList->SetDescriptorHeaps(_countof(peripheralHeaps), peripheralHeaps);
        commandList->SetComputeRootSignature(useExtendedDetail ? _peripheralFusedDetailRootSignature : _rootSignature);
        commandList->SetPipelineState(useJointDetail
                                          ? _peripheralJointDetailPipelineState
                                          : (useFusedDetail ? _peripheralFusedDetailPipelineState
                                                            : _peripheralPipelineState));
        commandList->SetComputeRootDescriptorTable(0, peripheralHeap.GetTableGPUStart());
        commandList->SetComputeRoot32BitConstants(1, GAZE_ROI_COMPOSITE_CONSTANT_DWORDS,
                                                  &peripheralConstants, 0);

        const UINT peripheralDispatchWidth =
            (static_cast<UINT>(peripheralConstants.peripheralResolveWidth) + _numThreadsX - 1) / _numThreadsX;
        const UINT peripheralDispatchHeight =
            (static_cast<UINT>(peripheralConstants.peripheralResolveHeight) + _numThreadsY - 1) / _numThreadsY;
        commandList->Dispatch(peripheralDispatchWidth, peripheralDispatchHeight, 1);

        if (useExtendedDetail)
        {
            Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                        _peripheralDetailHistory[nextDetailIndex],
                                        &_peripheralDetailHistoryState[nextDetailIndex]);
            _peripheralDetailHistoryIndex = nextDetailIndex;
            _peripheralDetailHistoryInitialized = true;
            detailForComposite = _peripheralDetailHistory[nextDetailIndex];
        }

        if (usesPeripheralHistory)
        {
            const uint32_t nextHistoryIndex = (_peripheralHistoryIndex + 1) % 2;
            if (peripheralConstants.peripheralTemporalMotionReprojection != 0 && !useJointDetail)
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

        // The temporal output is a zero-based, non-jittered source whose size
        // depends on the selected reconstruction mode.
        compositeConstants.srcWidth = peripheralConstants.peripheralResolveWidth;
        compositeConstants.srcHeight = peripheralConstants.peripheralResolveHeight;
        compositeConstants.srcTextureWidth = peripheralConstants.peripheralResolveWidth;
        compositeConstants.srcTextureHeight = peripheralConstants.peripheralResolveHeight;
        compositeConstants.srcBaseX = 0;
        compositeConstants.srcBaseY = 0;
        compositeConstants.peripheralJitterCancel = 0;
    }

    if (peripheralDetailEnabled && !useExtendedDetail)
    {
        if (!EnsurePeripheralDetailPipeline() || peripheralDetailWidth == 0 || peripheralDetailHeight == 0 ||
            _peripheralDetailHistory[0] == nullptr || _peripheralDetailHistory[1] == nullptr ||
            lowResDepth == nullptr || peripheralMotionVectors == nullptr)
        {
            return false;
        }

        Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    _peripheralDetailHistory[previousDetailIndex],
                                    &_peripheralDetailHistoryState[previousDetailIndex]);
        Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    _peripheralDetailHistory[nextDetailIndex],
                                    &_peripheralDetailHistoryState[nextDetailIndex]);

        GazeRoiConstants detailConstants = constants;
        detailConstants.peripheralResolveWidth = static_cast<int32_t>(peripheralDetailWidth);
        detailConstants.peripheralResolveHeight = static_cast<int32_t>(peripheralDetailHeight);
        detailConstants.peripheralTemporalInitialized = _peripheralDetailHistoryInitialized ? 1 : 0;

        FrameDescriptorHeap& detailHeap = _peripheralDetailFrameHeaps[frameSlot];
        try
        {
            CreateShaderResourceView(_device, lowResColor, detailHeap.GetSrvCPU(0));
            CreateShaderResourceView(_device, peripheralForComposite, detailHeap.GetSrvCPU(1));
            CreateShaderResourceView(_device, peripheralMotionVectors, detailHeap.GetSrvCPU(2));
            CreateShaderResourceView(_device, lowResDepth, detailHeap.GetSrvCPU(3));
            CreateShaderResourceView(_device, _peripheralDetailHistory[previousDetailIndex],
                                     detailHeap.GetSrvCPU(4));
            CreateUnorderedAccessView(_device, _peripheralDetailHistory[nextDetailIndex],
                                      detailHeap.GetUavCPU(0), 0);
            CreateUnorderedAccessView(_device, _peripheralDetailHistory[nextDetailIndex],
                                      detailHeap.GetUavCPU(1), 0);
        }
        catch (const std::exception& e)
        {
            LOG_WARN("[{}] Failed to create temporal detail descriptors: {}", _name, e.what());
            return false;
        }

        ID3D12DescriptorHeap* detailHeaps[] = { detailHeap.GetHeapCSU() };
        commandList->SetDescriptorHeaps(_countof(detailHeaps), detailHeaps);
        commandList->SetComputeRootSignature(_rootSignature);
        commandList->SetPipelineState(_peripheralDetailPipelineState);
        commandList->SetComputeRootDescriptorTable(0, detailHeap.GetTableGPUStart());
        commandList->SetComputeRoot32BitConstants(1, GAZE_ROI_COMPOSITE_CONSTANT_DWORDS,
                                                  &detailConstants, 0);
        const UINT detailDispatchWidth = (peripheralDetailWidth + _numThreadsX - 1) / _numThreadsX;
        const UINT detailDispatchHeight = (peripheralDetailHeight + _numThreadsY - 1) / _numThreadsY;
        commandList->Dispatch(detailDispatchWidth, detailDispatchHeight, 1);

        Shader_Dx12::SetBufferState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    _peripheralDetailHistory[nextDetailIndex],
                                    &_peripheralDetailHistoryState[nextDetailIndex]);
        _peripheralDetailHistoryIndex = nextDetailIndex;
        _peripheralDetailHistoryInitialized = true;
        detailForComposite = _peripheralDetailHistory[nextDetailIndex];
    }

    GazeRoiFrameSync::WriteGpuTimestamp(commandList, frameSlot, 5);
    return DispatchComposite(commandList, peripheralForComposite, patchedMotionVectors, finalOutput,
                             compositeConstants, frameSlot, nullptr, {}, detailForComposite,
                             peripheralDetailStrength, peripheralEasu);
}

bool GazeRoi_Dx12::DispatchComposite(ID3D12GraphicsCommandList* commandList,
                                      ID3D12Resource* peripheralColor,
                                      ID3D12Resource* patchedMotionVectors,
                                      ID3D12Resource* finalOutput,
                                        const GazeRoiConstants& constants, uint32_t frameSlot,
                                        ID3D12Resource* depthDebugResource,
                                        const GazeRoiDepthDebugConstants& depthDebugConstants,
                                        ID3D12Resource* peripheralDetail,
                                        float peripheralDetailStrength,
                                        bool peripheralEasu)
{
    if (!_init || _device == nullptr || commandList == nullptr || peripheralColor == nullptr ||
        finalOutput == nullptr || _dlssOutput == nullptr || frameSlot >= GAZE_ROI_NUM_OF_HEAPS)
        return false;

    GazeRoiConstants dispatchConstants = constants;
    dispatchConstants.easuConst0[0] = peripheralDetail != nullptr ? 1u : 0u;
    dispatchConstants.easuConst0[1] = std::bit_cast<uint32_t>(peripheralDetailStrength);
    dispatchConstants.easuConst0[2] =
        peripheralDetail != nullptr && (constants.peripheralTemporal & 3) == 3 ? 1u : 0u;
    if (peripheralEasu)
    {
        if (!EnsureEasuCompositePipeline() || constants.srcWidth <= 0 || constants.srcHeight <= 0 ||
            constants.srcTextureWidth <= 0 || constants.srcTextureHeight <= 0 ||
            constants.dstWidth <= 0 || constants.dstHeight <= 0)
        {
            return false;
        }

        const float jitterSign = constants.peripheralJitterSign < 0 ? -1.0f : 1.0f;
        const float inputOffsetX = static_cast<float>(constants.srcBaseX) +
                                   (constants.peripheralJitterCancel != 0
                                        ? jitterSign * constants.jitterOffsetX
                                        : 0.0f);
        const float inputOffsetY = static_cast<float>(constants.srcBaseY) +
                                   (constants.peripheralJitterCancel != 0
                                        ? jitterSign * constants.jitterOffsetY
                                        : 0.0f);
        FsrEasuConOffset(dispatchConstants.easuConst0, dispatchConstants.easuConst1,
                         dispatchConstants.easuConst2, dispatchConstants.easuConst3,
                         static_cast<float>(constants.srcWidth), static_cast<float>(constants.srcHeight),
                         static_cast<float>(constants.srcTextureWidth),
                         static_cast<float>(constants.srcTextureHeight),
                         static_cast<float>(constants.dstWidth), static_cast<float>(constants.dstHeight),
                         inputOffsetX, inputOffsetY);
    }

    FrameDescriptorHeap& currentHeap = _frameHeaps[frameSlot];
    try
    {
        CreateShaderResourceView(_device, peripheralColor, currentHeap.GetSrvCPU(0));
        if (_slotDlssOutputs[frameSlot] != _dlssOutput)
        {
            CreateShaderResourceView(_device, _dlssOutput, currentHeap.GetSrvCPU(1));
            _slotDlssOutputs[frameSlot] = _dlssOutput;
        }
        CreateShaderResourceView(_device, peripheralDetail != nullptr ? peripheralDetail : peripheralColor,
                                 currentHeap.GetSrvCPU(2));
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
    commandList->SetPipelineState(peripheralEasu ? _easuCompositePipelineState : _pipelineState);
    commandList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());
    commandList->SetComputeRoot32BitConstants(1, GAZE_ROI_COMPOSITE_CONSTANT_DWORDS, &dispatchConstants, 0);

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
        _peripheralFusedDetailFrameHeaps[i].ReleaseHeaps();
        _peripheralDetailFrameHeaps[i].ReleaseHeaps();
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
    SAFE_RELEASE(_peripheralDetailHistory[0]);
    SAFE_RELEASE(_peripheralDetailHistory[1]);
    SAFE_RELEASE(_easuCompositePipelineState);
    SAFE_RELEASE(_peripheralPipelineState);
    SAFE_RELEASE(_peripheralFusedDetailPipelineState);
    SAFE_RELEASE(_peripheralJointDetailPipelineState);
    SAFE_RELEASE(_peripheralFusedDetailRootSignature);
    SAFE_RELEASE(_peripheralDetailPipelineState);
    SAFE_RELEASE(_colorBypassPipelineState);
    SAFE_RELEASE(_debugOverlayPipelineState);
    SAFE_RELEASE(_depthDebugPipelineState);
}
