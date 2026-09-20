#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <proxies/NVNGX_Proxy.h>
#include <wrl/client.h>
#include <shaders/gaze_roi/GazeRoi_Dx12.h>

#include <memory>
#include <optional>
#include <string>
#include <deque>

class DLSSNRFeatureDx12
{
  public:
    enum class ColorDomain { Scene, DisplaySDR, DisplayLinearHDR };
    struct FoveatedRegion
    {
        unsigned int outputX = 0;
        unsigned int outputY = 0;
        unsigned int outputWidth = 0;
        unsigned int outputHeight = 0;
        unsigned int inputX = 0;
        unsigned int inputY = 0;
        unsigned int inputWidth = 0;
        unsigned int inputHeight = 0;
    };
    struct OriginalGuideDimensions
    {
        bool observed = false;
        unsigned int motionWidth = 0;
        unsigned int motionHeight = 0;
        unsigned int motionAllocationWidth = 0;
        unsigned int motionAllocationHeight = 0;
        unsigned int motionBaseX = 0;
        unsigned int motionBaseY = 0;
        unsigned int depthWidth = 0;
        unsigned int depthHeight = 0;
        unsigned int depthAllocationWidth = 0;
        unsigned int depthAllocationHeight = 0;
        unsigned int depthBaseX = 0;
        unsigned int depthBaseY = 0;
    };

    DLSSNRFeatureDx12();
    ~DLSSNRFeatureDx12();

    DLSSNRFeatureDx12(const DLSSNRFeatureDx12&) = delete;
    DLSSNRFeatureDx12& operator=(const DLSSNRFeatureDx12&) = delete;

    // Returns true when NR wrote its output, or the debug late route staged this frame.
    bool Evaluate(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                  NVSDK_NGX_Parameter* dlssParameters, unsigned int outputWidth, unsigned int outputHeight,
                  bool depthInverted, const FoveatedRegion* foveatedRegion = nullptr,
                  ColorDomain domain = ColorDomain::Scene,
                  std::optional<uint32_t> lifetimeSlot = std::nullopt,
                  ID3D12Resource** lateOutput = nullptr);
    void Shutdown(ID3D12Device* device);
    void InvalidateHistory() { _pendingReset = true; _hasPreviousFoveatedRegion = false; }
    static OriginalGuideDimensions GetLastOriginalGuideDimensions();

  private:
    struct GuidanceResources;

    bool EnsureInitialized(ID3D12Device* device);
    bool EnsureOutput(ID3D12Device* device, unsigned int width, unsigned int height);
    bool EnsureFeature(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters,
                       unsigned int inputWidth, unsigned int inputHeight,
                       unsigned int outputWidth, unsigned int outputHeight);
    void ReleaseFeature();
    void LogBypass(const std::string& reason);
    void LogStatistics(double evaluateCpuMs, double recordCpuMs, uint64_t copiedBytes);
    bool InstallCallerCompatibility();
    bool RestoreCallerCompatibility();
    void SetParameterUInt(NVSDK_NGX_Parameter* parameters, const char* key, unsigned int value);
    void SetParameterFloat(NVSDK_NGX_Parameter* parameters, const char* key, float value);
    void SetParameterResource(NVSDK_NGX_Parameter* parameters, const char* key, ID3D12Resource* value);

    HMODULE _module = nullptr;
    NVSDK_NGX_Handle* _handle = nullptr;
    NVSDK_NGX_Parameter* _parameters = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> _output;
    std::unique_ptr<GuidanceResources> _guidance;
    std::unique_ptr<GazeRoiMvPatch_Dx12> _gazeRoiMvPatch;
    std::wstring _modulePath;
    std::wstring _moduleDirectory;
    NVSDK_NGX_Feature _featureId = NVSDK_NGX_Feature_Reserved0;
    unsigned int _width = 0;
    unsigned int _height = 0;
    unsigned int _featureInputWidth = 0;
    unsigned int _featureInputHeight = 0;
    unsigned int _featureOutputWidth = 0;
    unsigned int _featureOutputHeight = 0;
    std::optional<int> _createStyle;
    std::optional<int> _createPreset;
    bool _initialized = false;
    bool _initializationAttempted = false;
    bool _pendingReset = true;
    bool _lastSDRToneMapping = false;
    ColorDomain _lastColorDomain = ColorDomain::Scene;
    float _lastHudlessHDRPaperWhiteNits = 203.0f;
    bool _lastFullResolutionGuidance = false;
    bool _debugInputViewCompositeDisabledLogged = false;
    bool _lastTemporalResidualReconstruction = false;
    bool _lastHighResolutionGuidedResidual = false;
    bool _lastLowResolutionOriginalMVec = false;
    bool _lastLowResolutionMVecScale = false;
    bool _lastCloneTypelessDepth = false;
    bool _lastZeroMotionInput = false;
    bool _lastZeroDepthInput = false;
    bool _lastDisableGazeRoiMotionInjection = false;
    bool _lastDebugGlobalDownsampleOutput = false;
    int _lastGazeRoiEdgeBlendSignature = -1;
    FoveatedRegion _previousFoveatedRegion {};
    bool _hasPreviousFoveatedRegion = false;
    bool _hasPreviousTemporalJitter = false;
    float _previousTemporalJitterX = 0.0f;
    float _previousTemporalJitterY = 0.0f;
    bool _callerHookInstalled = false;
    int _floatSetterSlot = -1;
    void** _getModuleFileNameIatSlot = nullptr;
    using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
    GetModuleFileNameWFn _originalGetModuleFileNameW = nullptr;
    std::string _lastBypassReason;
    uint64_t _attemptedEvaluations = 0;
    uint64_t _successfulEvaluations = 0;
    std::deque<double> _evaluateCpuMs;
    std::deque<double> _recordCpuMs;
    ID3D12Resource* _lastGameOutputResource = nullptr;
    D3D12_RESOURCE_DESC _lastGameOutputDesc {};
    bool _hasGameOutputContract = false;

    using InitExtFn = PFN_D3D12_Init_Ext;
    using Shutdown1Fn = decltype(&NVSDK_NGX_D3D12_Shutdown1);
    using CreateFeatureFn = decltype(&NVSDK_NGX_D3D12_CreateFeature);
    using EvaluateFeatureFn = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
    using ReleaseFeatureFn = decltype(&NVSDK_NGX_D3D12_ReleaseFeature);

    InitExtFn _initExt = nullptr;
    Shutdown1Fn _shutdown1 = nullptr;
    CreateFeatureFn _createFeature = nullptr;
    EvaluateFeatureFn _evaluateFeature = nullptr;
    ReleaseFeatureFn _releaseFeature = nullptr;
};
