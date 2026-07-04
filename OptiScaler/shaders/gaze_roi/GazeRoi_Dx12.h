#pragma once

#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <cstdint>
#include <string>

#define GAZE_ROI_NUM_OF_HEAPS 2
#define GAZE_ROI_MV_NUM_OF_HEAPS 2

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
    int32_t dstWidth = 0;
    int32_t dstHeight = 0;
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
    float peripheralTemporalCurrentWeight = 0.2f;
    float peripheralTemporalReactiveScale = 4.0f;
    int32_t peripheralTemporalInitialized = 0;
};

struct alignas(256) GazeRoiMvConstants
{
    int32_t width = 0;
    int32_t height = 0;
    float rawOffsetX = 0.0f;
    float rawOffsetY = 0.0f;
};

class GazeRoiMvPatch_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[GAZE_ROI_MV_NUM_OF_HEAPS];

    ID3D12Resource* _patchedMotionVectors = nullptr;
    D3D12_RESOURCE_STATES _patchedMotionVectorsState = D3D12_RESOURCE_STATE_COMMON;

    uint32_t _numThreadsX = 16;
    uint32_t _numThreadsY = 16;

  public:
    bool CreatePatchedResource(ID3D12Device* device, ID3D12Resource* motionVectorTemplate,
                               D3D12_RESOURCE_STATES initialState);
    void SetPatchedState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state);
    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceMotionVectors,
                  const GazeRoiMvConstants& constants);

    ID3D12Resource* PatchedMotionVectors() const { return _patchedMotionVectors; }

    GazeRoiMvPatch_Dx12(std::string name, ID3D12Device* device);
    ~GazeRoiMvPatch_Dx12();
};

class GazeRoi_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[GAZE_ROI_NUM_OF_HEAPS];
    FrameDescriptorHeap _peripheralFrameHeaps[GAZE_ROI_NUM_OF_HEAPS];

    ID3D12Resource* _dlssOutput = nullptr;
    D3D12_RESOURCE_STATES _dlssOutputState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _peripheralOutput = nullptr;
    D3D12_RESOURCE_STATES _peripheralOutputState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _peripheralHistory[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _peripheralHistoryState[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
    uint32_t _peripheralHistoryIndex = 0;
    bool _peripheralHistoryInitialized = false;
    ID3D12PipelineState* _peripheralPipelineState = nullptr;
    ID3D12Resource* _peripheralConstantBuffer = nullptr;

    uint32_t _numThreadsX = 16;
    uint32_t _numThreadsY = 16;

  public:
    bool CreateDlssOutputResource(ID3D12Device* device, ID3D12Resource* outputTemplate,
                                  D3D12_RESOURCE_STATES initialState);
    bool CreatePeripheralResource(ID3D12Device* device, ID3D12Resource* colorTemplate,
                                  D3D12_RESOURCE_STATES initialState);
    void SetDlssOutputState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state);
    void SetPeripheralOutputState(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state);
    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* lowResColor,
                  ID3D12Resource* finalOutput, const GazeRoiConstants& constants);

    ID3D12Resource* DlssOutput() const { return _dlssOutput; }
    bool CanRender() const { return _init && _dlssOutput != nullptr; }

    GazeRoi_Dx12(std::string name, ID3D12Device* device);
    ~GazeRoi_Dx12();
};
