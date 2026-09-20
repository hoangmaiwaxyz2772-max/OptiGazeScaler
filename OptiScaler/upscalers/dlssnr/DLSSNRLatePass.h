#pragma once
#include "DLSSNRFeature_Dx12.h"
#include <dxgi1_4.h>

namespace DLSSNRLatePass
{
bool Enabled();
// Descriptor metadata must be collected before the runtime late toggle: D3D12
// cannot recover the contents of heaps that the game already populated.
bool TrackDescriptors();
bool Pending();
bool Stage(DLSSNRFeatureDx12* owner, ID3D12Device* device, ID3D12GraphicsCommandList* commands,
           NVSDK_NGX_Parameter* parameters, UINT width, UINT height, bool inverted,
           const DLSSNRFeatureDx12::FoveatedRegion* region);
// Called at the existing HUDfix selection point, BEFORE copying the scene for FG.
// Writes the selected original resource and restores its incoming state.
bool Process(ID3D12GraphicsCommandList* commands, ID3D12Resource* scene,
             D3D12_RESOURCE_STATES state);
void Cancel(DLSSNRFeatureDx12* owner);
void EndFrame();
void SetColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace);
}
