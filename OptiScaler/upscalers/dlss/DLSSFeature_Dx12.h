#pragma once
#include "DLSSFeature.h"
#include <upscalers/IFeature_Dx12.h>
#include <shaders/rcas/RCAS_Dx12.h>
#include <shaders/gaze_roi/GazeRoi_Dx12.h>
#include <array>
#include <memory>
#include <string>

class DLSSFeatureDx12 : public DLSSFeature, public IFeature_Dx12
{
  private:
    std::unique_ptr<GazeRoi_Dx12> GazeRoi = nullptr;
    std::unique_ptr<GazeRoiMvPatch_Dx12> GazeRoiMvPatch = nullptr;
    std::unique_ptr<GazeRoiColorCrop_Dx12> GazeRoiColorCrop = nullptr;
    std::unique_ptr<GazeRoiDepthCrop_Dx12> GazeRoiDepthCrop = nullptr;
    NVSDK_NGX_Handle _gazeRoiDlssHandle = {};
    NVSDK_NGX_Handle* _p_gazeRoiDlssHandle = nullptr;
    GazeRoiRect _gazeRoiHandleInputRect = {};
    GazeRoiRect _gazeRoiHandleOutputRect = {};
    std::string _gazeRoiHandleCreateSignature = {};
    bool _gazeRoiHandleWasCreated = false;
    std::string _gazeRoiOptimalSignature = {};
    std::array<uint32_t, 6> _gazeRoiCreatePresetValues = {};
    bool _gazeRoiCreatePresetsCaptured = false;
    uint32_t _gazeRoiOptimalWidth = 0;
    uint32_t _gazeRoiOptimalHeight = 0;
    uint32_t _gazeRoiOptimalMinWidth = 0;
    uint32_t _gazeRoiOptimalMinHeight = 0;
    uint32_t _gazeRoiOptimalMaxWidth = 0;
    uint32_t _gazeRoiOptimalMaxHeight = 0;
    GazeRoiRect _gazePreviousInputRect = {};
    GazeRoiRect _gazePreviousOutputRect = {};
    bool _gazeHasPreviousInputRect = false;
    bool _gazeDiagnosticOptionsInitialized = false;
    bool _gazePreviousCurrentColorPointBypass = false;
    bool _gazePreviousColorCopy = false;
    bool _gazePreviousDepthCopy = true;
    bool _gazePreviousOmitBiasCurrentColorHint = false;
    bool _gazePreviousMinimalPrivateParameters = false;
    bool _gazePreviousResetOnMove = false;
    NVSDK_NGX_Parameter* _gazeMinimalParameters = nullptr;
    std::string _lastGazeContractSignature = {};
    std::string _lastGazeContractDecision = {};
    std::string _gazePreviousControl = {};
    bool _gazeExternalSourceStateInitialized = false;
    bool _gazeExternalSourceFresh = false;
    float _gazePreviousJitterOffsetX = 0.0f;
    float _gazePreviousJitterOffsetY = 0.0f;
    float _gazePointX = 0.5f;
    float _gazePointY = 0.5f;

    void LogGazeRoiContract(NVSDK_NGX_Parameter* InParameters, const std::string& mode);
    void LogGazeRoiDecision(const std::string& decision);
    void UpdateVirtualGazePoint();
    bool BuildGazeRoiRects(GazeRoiRect& outputRect, GazeRoiRect& inputRect);
    bool ResolveGazeRoiOptimalInput(NVSDK_NGX_Parameter* InParameters, GazeRoiRect& outputRect,
                                    GazeRoiRect& inputRect);
    void CaptureGazeRoiCreatePresets(NVSDK_NGX_Parameter* InParameters);
    uint32_t GazeRoiCreatePresetValue(NVSDK_NGX_Parameter* InParameters, size_t index) const;
    bool EnsureGazeRoiMinimalParameters();
    bool EnsureGazeRoiDlssHandle(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters,
                                 const GazeRoiRect& outputRect, const GazeRoiRect& inputRect);
    bool TryEvaluateGazeRoi(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters,
                            NVSDK_NGX_Result& nvResult);

  protected:
    bool InitDLSS(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);

  public:
    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    static void Shutdown(ID3D12Device* InDevice);

    feature_version Version() override { return DLSSFeature::Version(); }
    Upscaler GetUpscalerType() const final { return DLSSFeature::GetUpscalerType(); }

    bool IsWithDx12() override { return false; }

    DLSSFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~DLSSFeatureDx12();
};
