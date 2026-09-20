#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>

// Captures are recorded inside an acquired GazeRoiFrameSync generation. Only
// fence-completed copies are published to Present; the model resources are
// never sampled concurrently by the presentation queue.
namespace DLSSNRPreview
{
void Capture(ID3D12Device* device, ID3D12GraphicsCommandList* commands, ID3D12Resource* source,
             D3D12_RESOURCE_STATES state, UINT x, UINT y, UINT width, UINT height, UINT mode,
             bool displayLinear = false);
bool Present(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue);
void BeforeResize(IDXGISwapChain* swapchain);
void SetColorSpace(IDXGISwapChain* swapchain, DXGI_COLOR_SPACE_TYPE colorSpace);
}
