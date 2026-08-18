#pragma once
#include "SysUtils.h"
#include <framegen/IFGFeature_Dx12.h>
#include <proxies/FfxApi_Proxy.h>
#include <shaders/format_transfer/FT_Dx12.h>
#include <shaders/hud_copy/HudCopy_Dx12.h>
#include <shaders/hudless_compare_compute/HCC_Dx12.h>

#include <ffx_framegeneration.h>
#include <gaze_roi/GazeRoiInput.h>
#include <shaders/gaze_roi/GazeRoi_Dx12.h>

struct FsrFgRoiRect
{
    UINT left = 0;
    UINT top = 0;
    UINT width = 0;
    UINT height = 0;

    bool IsValid() const { return width != 0 && height != 0; }
};

class FsrFgPeripheralReprojection;

class FSRFG_Dx12 : public virtual IFGFeature_Dx12
{
  private:
    ffxContext _swapChainContext = nullptr;
    ffxContext _fgContext = nullptr;
    FfxApiSurfaceFormat _lastHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
    FfxApiSurfaceFormat _usingHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
    feature_version _version { 0, 0, 0 };

    bool _linkedHudlesDesc = false;

    uint32_t _maxRenderWidth = 0;
    uint32_t _maxRenderHeight = 0;

    std::unique_ptr<HudCopy_Dx12> _hudCopy[BUFFER_COUNT];
    std::unique_ptr<HCC_Dx12> _hudlessCompareCompute[BUFFER_COUNT];

    std::unique_ptr<FT_Dx12> _hudlessTransfer[BUFFER_COUNT];
    ID3D12Resource* _hudlessCopyResource[BUFFER_COUNT] {};
    std::unique_ptr<FT_Dx12> _uiTransfer[BUFFER_COUNT];
    ID3D12Resource* _uiCopyResource[BUFFER_COUNT] {};

    ID3D12GraphicsCommandList* _fgCommandList[BUFFER_COUNT] {};
    ID3D12CommandAllocator* _fgCommandAllocator[BUFFER_COUNT] {};

    // Local ROI resources are opt-in. The physical output remains owned by
    // the swapchain provider; these surfaces only carry the local FI work.
    ID3D12Resource* _roiColor[BUFFER_COUNT] {};
    ID3D12Resource* _roiPreviousColor[BUFFER_COUNT] {};
    ID3D12Resource* _roiOutput[BUFFER_COUNT] {};
    ID3D12Resource* _roiDepth[BUFFER_COUNT] {};
    ID3D12Resource* _roiVelocity[BUFFER_COUNT] {};
    ID3D12Resource* _roiHudless[BUFFER_COUNT] {};
    ID3D12Resource* _roiPreviousHudless[BUFFER_COUNT] {};
    ID3D12Resource* _roiRealColorHistory[BUFFER_COUNT] {};
    ID3D12Resource* _roiRealHudlessHistory[BUFFER_COUNT] {};
    ID3D12Resource* _roiRealDepthHistory[BUFFER_COUNT] {};
    ID3D12Resource* _roiPeripheralOutput[BUFFER_COUNT] {};
    UINT64 _roiRealColorHistoryFrameId[BUFFER_COUNT] {};
    bool _roiRealColorHistoryValid[BUFFER_COUNT] {};
    UINT64 _roiRealHudlessHistoryFrameId[BUFFER_COUNT] {};
    bool _roiRealHudlessHistoryValid[BUFFER_COUNT] {};
    UINT64 _roiRealDepthHistoryFrameId[BUFFER_COUNT] {};
    bool _roiRealDepthHistoryValid[BUFFER_COUNT] {};
    FsrFgRoiRect _roiRealColorHistoryRect[BUFFER_COUNT] {};
    FsrFgRoiRect _roiRect[BUFFER_COUNT] {};
    UINT64 _roiProviderHistoryFrameId = 0;
    FsrFgRoiRect _roiProviderHistoryRect {};
    bool _roiProviderHistoryValid = false;
    bool _roiProviderHistoryUsedHudless = false;
    bool _roiHudlessActivationLogged = false;
    bool _roiHudlessActive = false;
    bool _roiContextActive = false;
    UINT _physicalDisplayWidth = 0;
    UINT _physicalDisplayHeight = 0;
    int _deferredPrepareIndex = -1;
    FsrFgPeripheralReprojection* _peripheralReprojection = nullptr;

    static FfxApiResourceState GetFfxApiState(D3D12_RESOURCE_STATES state)
    {
        switch (state)
        {
        case D3D12_RESOURCE_STATE_COMMON:
            return FFX_API_RESOURCE_STATE_COMMON;
        case D3D12_RESOURCE_STATE_UNORDERED_ACCESS:
            return FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
        case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
            return FFX_API_RESOURCE_STATE_COMPUTE_READ;
        case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
            return FFX_API_RESOURCE_STATE_PIXEL_READ;
        case (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE):
            return FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        case D3D12_RESOURCE_STATE_COPY_SOURCE:
            return FFX_API_RESOURCE_STATE_COPY_SRC;
        case D3D12_RESOURCE_STATE_COPY_DEST:
            return FFX_API_RESOURCE_STATE_COPY_DEST;
        case D3D12_RESOURCE_STATE_GENERIC_READ:
            return FFX_API_RESOURCE_STATE_GENERIC_READ;
        case D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT:
            return FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT;
        case D3D12_RESOURCE_STATE_RENDER_TARGET:
            return FFX_API_RESOURCE_STATE_RENDER_TARGET;
        default:
            return FFX_API_RESOURCE_STATE_COMMON;
        }
    }

    bool ExecuteCommandList(int index);
    bool Dispatch(bool deferExecution = false);
    static ffxReturnCode_t FrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* userContext);
    bool ConfigureLocalHudless(ID3D12Resource* hudless, UINT64 frameId);
    uint32_t GetFrameGenerationFlags() const;
    bool RecordPrepare(int index, UINT64 frameId, ID3D12GraphicsCommandList* commandList, uint32_t flags,
                       const FsrFgRoiRect* lockedRoi = nullptr);
    void ConfigureFramePaceTuning();
    bool HudlessFormatTransfer(int index, ID3D12Device* device, DXGI_FORMAT targetFormat, Dx12Resource* resource);
    bool UIFormatTransfer(int index, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DXGI_FORMAT targetFormat,
                          Dx12Resource* resource);

    FsrFgRoiRect ResolveRoi(int index, UINT displayWidth, UINT displayHeight);
    bool CreateRoiSurface(ID3D12Resource* source, UINT width, UINT height, D3D12_RESOURCE_STATES initialState,
                          ID3D12Resource** target, bool allowUav);
    bool CopyRoi(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState,
                 const FsrFgRoiRect& sourceRoi, ID3D12Resource* target, UINT targetWidth, UINT targetHeight,
                 D3D12_RESOURCE_STATES targetState);
    bool CopyFull(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState,
                  ID3D12Resource* target, D3D12_RESOURCE_STATES targetState);
    bool PrepareLocalInputResources(int index, ID3D12GraphicsCommandList* commandList, const FsrFgRoiRect& roi,
                                    UINT displayWidth, UINT displayHeight);
    void ReleaseLocalResources();

    void ParseVersion(const char* version_str, feature_version* _version)
    {
        const char* p = version_str;

        // Skip non-digits at front
        while (*p)
        {
            if (isdigit((unsigned char) p[0]))
            {
                if (sscanf(p, "%u.%u.%u", &_version->major, &_version->minor, &_version->patch) == 3)
                    return;
            }

            ++p;
        }

        LOG_WARN("can't parse {0}", version_str);
    }

  protected:
    void ReleaseObjects() override final;
    void CreateObjects(ID3D12Device* InDevice) override final;

  public:
    // IFGFeature
    const char* Name() override final;
    feature_version Version() override final;
    HWND Hwnd() override final;

    void* FrameGenerationContext() override final;
    void* SwapchainContext() override final;

    bool CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                         IDXGISwapChain** swapChain, bool readyToRelease) override final;
    bool CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd, DXGI_SWAP_CHAIN_DESC1* desc,
                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGISwapChain1** swapChain,
                          bool readyToRelease) override final;
    bool ReleaseSwapchain(HWND hwnd) override final;

    void CreateContext(ID3D12Device* device, FG_Constants& fgConstants) override final;
    void Activate() override final;
    void Deactivate() override final;
    void DestroyFGContext() override final;
    bool Shutdown() override final;

    void EvaluateState(ID3D12Device* device, FG_Constants& fgConstants) override final;

    bool Present() override final;

    bool SetResource(Dx12Resource* inputResource) override final;
    void SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) override final;
    bool IsLocalRoiContext() const override final;
    bool UseLocalRoiStagingBypass() const override final;
    bool IsLocalRoiHudlessActive() const override final;

    ffxReturnCode_t DispatchCallback(ffxDispatchDescFrameGeneration* params);

    FSRFG_Dx12() : IFGFeature_Dx12(), IFGFeature()
    {
        //
    }

    ~FSRFG_Dx12();

    // Inherited via IFGFeature_Dx12
    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override;
};
