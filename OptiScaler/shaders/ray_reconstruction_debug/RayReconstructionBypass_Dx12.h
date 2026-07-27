#pragma once

#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#include <d3d12.h>
#include <d3dx/d3dx12.h>

#include <cstdint>
#include <string>

#define RAY_RECONSTRUCTION_BYPASS_FRAME_SLOTS 8

struct RayReconstructionBypassConstants
{
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t sourceBaseX = 0;
    uint32_t sourceBaseY = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t outputBaseX = 0;
    uint32_t outputBaseY = 0;
};

class RayReconstructionBypassDx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[RAY_RECONSTRUCTION_BYPASS_FRAME_SLOTS];
    uint32_t _numThreadsX = 16;
    uint32_t _numThreadsY = 16;

  public:
    bool Dispatch(ID3D12GraphicsCommandList* commandList, ID3D12Resource* sourceColor,
                  ID3D12Resource* output, const RayReconstructionBypassConstants& constants,
                  uint32_t frameSlot);

    RayReconstructionBypassDx12(std::string name, ID3D12Device* device);
    ~RayReconstructionBypassDx12();
};
