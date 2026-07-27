#pragma once

#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#include <d3d12.h>

#include <cstdint>
#include <string>

#define RAY_RECONSTRUCTION_DENOISER_FRAME_SLOTS 8

enum class RayReconstructionDenoiserDebugView : uint32_t
{
    Filtered = 0,
    Current = 1,
    ReprojectedHistory = 2,
    HistoryRejection = 3,
    HistoryLength = 4,
};

struct RayReconstructionDenoiserConstants
{
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t colorBaseX = 0;
    uint32_t colorBaseY = 0;
    uint32_t depthBaseX = 0;
    uint32_t depthBaseY = 0;
    uint32_t motionVectorBaseX = 0;
    uint32_t motionVectorBaseY = 0;
    uint32_t motionVectorWidth = 0;
    uint32_t motionVectorHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t outputBaseX = 0;
    uint32_t outputBaseY = 0;
    float motionVectorScaleX = 1.0f;
    float motionVectorScaleY = 1.0f;
    float jitterOffsetX = 0.0f;
    float jitterOffsetY = 0.0f;
    float previousJitterOffsetX = 0.0f;
    float previousJitterOffsetY = 0.0f;
    uint32_t motionVectorsLowResolution = 1;
    uint32_t motionVectorsJittered = 0;
    uint32_t resetHistory = 0;
    uint32_t historyInitialized = 0;
    uint32_t debugView = 0;
    uint32_t maxHistory = 16;
    uint32_t spatialPasses = 2;
    float spatialRadius = 1.5f;
    uint32_t spatialPassIndex = 0;
    uint32_t denoiserWidth = 0;
    uint32_t denoiserHeight = 0;
};

class RayReconstructionPeripheralDenoiserDx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _temporalFrameHeaps[RAY_RECONSTRUCTION_DENOISER_FRAME_SLOTS];
    FrameDescriptorHeap _outputFrameHeaps[RAY_RECONSTRUCTION_DENOISER_FRAME_SLOTS];
    FrameDescriptorHeap _spatialFrameHeaps[3][RAY_RECONSTRUCTION_DENOISER_FRAME_SLOTS];

    ID3D12Resource* _radianceHistory[2] = { nullptr, nullptr };
    ID3D12Resource* _momentsHistory[2] = { nullptr, nullptr };
    ID3D12Resource* _depthHistory[2] = { nullptr, nullptr };
    ID3D12Resource* _debugOutput = nullptr;
    ID3D12Resource* _spatialScratch[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _radianceHistoryStates[2] = { D3D12_RESOURCE_STATE_COMMON,
                                                         D3D12_RESOURCE_STATE_COMMON };
    D3D12_RESOURCE_STATES _momentsHistoryStates[2] = { D3D12_RESOURCE_STATE_COMMON,
                                                        D3D12_RESOURCE_STATE_COMMON };
    D3D12_RESOURCE_STATES _depthHistoryStates[2] = { D3D12_RESOURCE_STATE_COMMON,
                                                      D3D12_RESOURCE_STATE_COMMON };
    D3D12_RESOURCE_STATES _debugOutputState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES _spatialScratchStates[2] = { D3D12_RESOURCE_STATE_COMMON,
                                                        D3D12_RESOURCE_STATE_COMMON };
    ID3D12PipelineState* _outputPipelineState = nullptr;
    ID3D12PipelineState* _spatialPipelineState = nullptr;
    uint32_t _historyIndex = 0;
    uint32_t _historyWidth = 0;
    uint32_t _historyHeight = 0;
    uint32_t _spatialScratchCount = 0;
    bool _historyInitialized = false;
    bool _debugOutputEnabled = false;
    ID3D12Resource* _filteredOutput = nullptr;
    uint32_t _numThreadsX = 16;
    uint32_t _numThreadsY = 16;

    bool CreateHistoryResources(uint32_t width, uint32_t height, bool debugOutput, uint32_t spatialPasses);
    void ReleaseHistoryResources();
    void SetResourceState(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                          D3D12_RESOURCE_STATES* currentState, D3D12_RESOURCE_STATES nextState);

  public:
    bool EnsureResources(uint32_t width, uint32_t height, bool debugOutput, uint32_t spatialPasses);
    void ResetHistory();
    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* color, ID3D12Resource* depth,
                  ID3D12Resource* motionVectors, ID3D12Resource* output,
                  const RayReconstructionDenoiserConstants& constants, uint32_t frameSlot);
    ID3D12Resource* FilteredOutput() const { return _filteredOutput; }

    RayReconstructionPeripheralDenoiserDx12(std::string name, ID3D12Device* device);
    ~RayReconstructionPeripheralDenoiserDx12();
};
