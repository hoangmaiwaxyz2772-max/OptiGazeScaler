#pragma once

#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <cstdint>
#include <functional>
#include <string>

#define GAZE_ROI_FRAME_SLOTS 8
#define GAZE_ROI_NUM_OF_HEAPS GAZE_ROI_FRAME_SLOTS
#define GAZE_ROI_MV_NUM_OF_HEAPS GAZE_ROI_FRAME_SLOTS
#define GAZE_ROI_COLOR_NUM_OF_HEAPS GAZE_ROI_FRAME_SLOTS
#define GAZE_ROI_DEPTH_NUM_OF_HEAPS GAZE_ROI_FRAME_SLOTS

// A slot is acquired while an ROI-bearing command list is recorded, associated
// with the real queue submission by the ExecuteCommandLists hook, and recycled
// only after that queue's fence has completed. This protects both shader-visible
// descriptors and CPU-written upload constants from cross-frame overwrite.
class GazeRoiFrameSync
{
  public:
    static bool Acquire(ID3D12GraphicsCommandList* commandList, uint32_t& frameSlot);
    static void OnExecuteCommandLists(ID3D12CommandQueue* commandQueue, UINT numCommandLists,
                                      ID3D12CommandList* const* commandLists);
    static void DeferRelease(IUnknown* object);
    static void DeferCallback(std::function<void()> callback);
    static void FlushDeferred();
    static bool BeginGpuTiming(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, uint32_t frameSlot);
    static void WriteGpuTimestamp(ID3D12GraphicsCommandList* commandList, uint32_t frameSlot, uint32_t marker);
    static void ResolveGpuTiming(ID3D12GraphicsCommandList* commandList, uint32_t frameSlot);
};

struct GazeRoiRect
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct alignas(256) GazeRoiConstants
{
    int32_t srcWidth = 0;
    int32_t srcHeight = 0;
    int32_t srcTextureWidth = 0;
    int32_t srcTextureHeight = 0;
    int32_t srcBaseX = 0;
    int32_t srcBaseY = 0;
    int32_t dstWidth = 0;
    int32_t dstHeight = 0;
    int32_t dstBaseX = 0;
    int32_t dstBaseY = 0;
    int32_t roiX = 0;
    int32_t roiY = 0;
    int32_t roiWidth = 0;
    int32_t roiHeight = 0;
    int32_t featherPx = 96;
    int32_t debugBorderPx = 2;
    int32_t peripheralBlur = 1;
    float peripheralBlurRadius = 1.0f;
    int32_t peripheralJitterCancel = 1;
    int32_t peripheralJitterSign = 1;
    float jitterOffsetX = 0.0f;
    float jitterOffsetY = 0.0f;
    int32_t peripheralTemporal = 1;
    float peripheralTemporalHistoryWeight = 0.75f;
    float peripheralTemporalReactiveScale = 4.0f;
    int32_t peripheralTemporalInitialized = 0;
    int32_t peripheralTemporalMotionReprojection = 0;
    int32_t peripheralTemporalHistoryReset = 0;
    int32_t peripheralDepthBaseX = 0;
    int32_t peripheralDepthBaseY = 0;
    int32_t peripheralMotionVectorBaseX = 0;
    int32_t peripheralMotionVectorBaseY = 0;
    int32_t peripheralMotionVectorWidth = 0;
    int32_t peripheralMotionVectorHeight = 0;
    float peripheralMotionVectorScaleX = 1.0f;
    float peripheralMotionVectorScaleY = 1.0f;
    int32_t peripheralMotionVectorsLowResolution = 1;
    int32_t peripheralMotionVectorsJittered = 0;
    float previousJitterOffsetX = 0.0f;
    float previousJitterOffsetY = 0.0f;
    int32_t motionVectorDebugView = 0;
    int32_t motionVectorWidth = 0;
    int32_t motionVectorHeight = 0;
    float motionVectorScaleX = 1.0f;
    float motionVectorScaleY = 1.0f;
    int32_t peripheralResolveWidth = 0;
    int32_t peripheralResolveHeight = 0;
    uint32_t easuConst0[4] {};
    uint32_t easuConst1[4] {};
    uint32_t easuConst2[4] {};
    uint32_t easuConst3[4] {};
};

struct alignas(256) GazeRoiMvConstants
{
    int32_t width = 0;
    int32_t height = 0;
    int32_t sourceBaseX = 0;
    int32_t sourceBaseY = 0;
    float rawOffsetX = 0.0f;
    float rawOffsetY = 0.0f;
};

struct alignas(256) GazeRoiDepthConstants
{
    int32_t width = 0;
    int32_t height = 0;
    int32_t sourceBaseX = 0;
    int32_t sourceBaseY = 0;
};

// Separate constants for the RR-only depth inspection overlay. The overlay
// reads the original depth resource directly and writes a point-for-point
// grayscale view into the game's output active rect.
struct alignas(256) GazeRoiDepthDebugConstants
{
    int32_t depthWidth = 0;
    int32_t depthHeight = 0;
    int32_t depthTextureWidth = 0;
    int32_t depthTextureHeight = 0;
    int32_t depthBaseX = 0;
    int32_t depthBaseY = 0;
    int32_t dstWidth = 0;
    int32_t dstHeight = 0;
    int32_t dstBaseX = 0;
    int32_t dstBaseY = 0;
};

struct alignas(256) GazeRoiColorConstants
{
    int32_t sourceWidth = 0;
    int32_t sourceHeight = 0;
    int32_t sourceBaseX = 0;
    int32_t sourceBaseY = 0;
    int32_t outputWidth = 0;
    int32_t outputHeight = 0;
};

class GazeRoiMvPatch_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[GAZE_ROI_MV_NUM_OF_HEAPS];

    ID3D12Resource* _patchedMotionVectors = nullptr;
    ID3D12Resource* _slotPatchedMotionVectors[GAZE_ROI_MV_NUM_OF_HEAPS] {};
    D3D12_RESOURCE_STATES _patchedMotionVectorsState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12PipelineState* _fourComponentPipelineState = nullptr;
    DXGI_FORMAT _motionVectorFormat = DXGI_FORMAT_UNKNOWN;

    uint32_t _numThreadsX = 16;
    uint32_t _numThreadsY = 16;

  public:
    bool CreatePatchedResource(ID3D12Device* device, ID3D12Resource* motionVectorTemplate, uint32_t width,
                               uint32_t height, D3D12_RESOURCE_STATES initialState);
    void SetPatchedState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state);
    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceMotionVectors,
                  const GazeRoiMvConstants& constants, uint32_t frameSlot);

    ID3D12Resource* PatchedMotionVectors() const { return _patchedMotionVectors; }

    GazeRoiMvPatch_Dx12(std::string name, ID3D12Device* device);
    ~GazeRoiMvPatch_Dx12();
};

class GazeRoiDepthCrop_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[GAZE_ROI_DEPTH_NUM_OF_HEAPS];

    ID3D12Resource* _croppedDepth = nullptr;
    D3D12_RESOURCE_STATES _croppedDepthState = D3D12_RESOURCE_STATE_COMMON;

    uint32_t _numThreadsX = 16;
    uint32_t _numThreadsY = 16;

  public:
    bool CreateCroppedResource(ID3D12Device* device, ID3D12Resource* depthTemplate, uint32_t width,
                               uint32_t height, D3D12_RESOURCE_STATES initialState);
    void SetCroppedState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state);
    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceDepth,
                  const GazeRoiDepthConstants& constants, uint32_t frameSlot);

    ID3D12Resource* CroppedDepth() const { return _croppedDepth; }

    GazeRoiDepthCrop_Dx12(std::string name, ID3D12Device* device);
    ~GazeRoiDepthCrop_Dx12();
};

class GazeRoiColorCrop_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[GAZE_ROI_COLOR_NUM_OF_HEAPS];

    ID3D12Resource* _croppedColor = nullptr;
    D3D12_RESOURCE_STATES _croppedColorState = D3D12_RESOURCE_STATE_COMMON;

    uint32_t _numThreadsX = 16;
    uint32_t _numThreadsY = 16;

  public:
    bool CreateCroppedResource(ID3D12Device* device, ID3D12Resource* colorTemplate, uint32_t width,
                               uint32_t height, D3D12_RESOURCE_STATES initialState);
    void SetCroppedState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state);
    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceColor,
                  const GazeRoiColorConstants& constants, uint32_t frameSlot);

    ID3D12Resource* CroppedColor() const { return _croppedColor; }

    GazeRoiColorCrop_Dx12(std::string name, ID3D12Device* device);
    ~GazeRoiColorCrop_Dx12();
};

class GazeRoi_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[GAZE_ROI_NUM_OF_HEAPS];
    FrameDescriptorHeap _peripheralFrameHeaps[GAZE_ROI_NUM_OF_HEAPS];
    FrameDescriptorHeap _peripheralFusedDetailFrameHeaps[GAZE_ROI_NUM_OF_HEAPS];
    FrameDescriptorHeap _peripheralDetailFrameHeaps[GAZE_ROI_NUM_OF_HEAPS];
    FrameDescriptorHeap _colorBypassFrameHeaps[GAZE_ROI_NUM_OF_HEAPS];
    FrameDescriptorHeap _outputClearFrameHeaps[GAZE_ROI_NUM_OF_HEAPS];
    FrameDescriptorHeap _debugOverlayFrameHeaps[GAZE_ROI_NUM_OF_HEAPS];
    FrameDescriptorHeap _depthDebugFrameHeaps[GAZE_ROI_NUM_OF_HEAPS];

    ID3D12Resource* _dlssOutput = nullptr;
    ID3D12Resource* _slotDlssOutputs[GAZE_ROI_NUM_OF_HEAPS] {};
    D3D12_RESOURCE_STATES _dlssOutputState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _peripheralOutput = nullptr;
    D3D12_RESOURCE_STATES _peripheralOutputState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _peripheralHistory[2] = { nullptr, nullptr };
    ID3D12Resource* _peripheralDepthHistory[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _peripheralHistoryState[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
    D3D12_RESOURCE_STATES _peripheralDepthHistoryState[2] = { D3D12_RESOURCE_STATE_COMMON,
                                                               D3D12_RESOURCE_STATE_COMMON };
    uint32_t _peripheralHistoryIndex = 0;
    bool _peripheralHistoryInitialized = false;
    int32_t _peripheralHistoryMode = -1;
    ID3D12Resource* _peripheralDetailHistory[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _peripheralDetailHistoryState[2] = { D3D12_RESOURCE_STATE_COMMON,
                                                               D3D12_RESOURCE_STATE_COMMON };
    uint32_t _peripheralDetailHistoryIndex = 0;
    bool _peripheralDetailHistoryInitialized = false;
    int32_t _peripheralDetailJointMode = -1;
    ID3D12PipelineState* _easuCompositePipelineState = nullptr;
    ID3D12PipelineState* _peripheralPipelineState = nullptr;
    ID3D12RootSignature* _peripheralFusedDetailRootSignature = nullptr;
    ID3D12PipelineState* _peripheralFusedDetailPipelineState = nullptr;
    ID3D12PipelineState* _peripheralJointDetailPipelineState = nullptr;
    ID3D12PipelineState* _peripheralDetailPipelineState = nullptr;
    ID3D12PipelineState* _colorBypassPipelineState = nullptr;
    ID3D12PipelineState* _debugOverlayPipelineState = nullptr;
    ID3D12PipelineState* _depthDebugPipelineState = nullptr;
    bool _peripheralHeapsInitialized = false;
    bool _peripheralFusedDetailHeapsInitialized = false;
    bool _peripheralDetailHeapsInitialized = false;
    bool _colorBypassHeapsInitialized = false;
    bool _outputClearHeapsInitialized = false;
    bool _debugOverlayHeapsInitialized = false;
    bool _depthDebugHeapsInitialized = false;
    uint32_t _numThreadsX = 16;
    uint32_t _numThreadsY = 16;

    bool EnsureEasuCompositePipeline();
    bool EnsurePeripheralPipeline();
    bool EnsurePeripheralExtendedDetailResources();
    bool EnsurePeripheralFusedDetailPipeline();
    bool EnsurePeripheralJointDetailPipeline();
    bool EnsurePeripheralDetailPipeline();
    bool EnsureColorBypassPipeline();
    bool EnsureOutputClearHeaps();
    bool EnsureDebugOverlayPipeline();
    bool EnsureDepthDebugPipeline();

  public:
    bool CreateDlssOutputResource(ID3D12Device* device, ID3D12Resource* outputTemplate, uint32_t width,
                                  uint32_t height, D3D12_RESOURCE_STATES initialState);
    bool CreatePeripheralResource(ID3D12Device* device, ID3D12Resource* colorTemplate, uint32_t width,
                                  uint32_t height, bool blurEnabled, bool temporalEnabled,
                                  bool motionReprojectionEnabled, bool jointDetailEnabled,
                                  D3D12_RESOURCE_STATES initialState);
    bool CreatePeripheralDetailResources(ID3D12Device* device, ID3D12Resource* colorTemplate, uint32_t width,
                                         uint32_t height, bool enabled, bool jointMode,
                                         D3D12_RESOURCE_STATES initialState);
    void SetDlssOutputState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state);
    bool DispatchCurrentColorPointBypass(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceColor,
                                         const GazeRoiColorConstants& constants, uint32_t frameSlot);
    bool ClearDlssOutputRectMagenta(ID3D12GraphicsCommandList* commandList, const GazeRoiRect& rect,
                                    uint32_t frameSlot);
    void SetPeripheralOutputState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state);
    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* lowResColor,
                  ID3D12Resource* lowResDepth, ID3D12Resource* peripheralMotionVectors,
                  ID3D12Resource* patchedMotionVectors, ID3D12Resource* finalOutput,
                  const GazeRoiConstants& constants, uint32_t frameSlot, bool peripheralDetailEnabled = false,
                  uint32_t peripheralDetailWidth = 0, uint32_t peripheralDetailHeight = 0,
                  float peripheralDetailStrength = 0.0f, bool peripheralEasu = false);
    bool DispatchComposite(ID3D12GraphicsCommandList* commandList, ID3D12Resource* peripheralColor,
                           ID3D12Resource* patchedMotionVectors, ID3D12Resource* finalOutput,
                           const GazeRoiConstants& constants, uint32_t frameSlot,
                           ID3D12Resource* depthDebugResource = nullptr,
                           const GazeRoiDepthDebugConstants& depthDebugConstants = {},
                           ID3D12Resource* peripheralDetail = nullptr,
                           float peripheralDetailStrength = 0.0f,
                           bool peripheralEasu = false);

    ID3D12Resource* DlssOutput() const { return _dlssOutput; }
    bool CanRender() const { return _init && _dlssOutput != nullptr; }

    GazeRoi_Dx12(std::string name, ID3D12Device* device);
    ~GazeRoi_Dx12();
};
