#pragma once
#include "DLSSDFeature.h"
#include <upscalers/IFeature_Dx12.h>
#include <shaders/rcas/RCAS_Dx12.h>
#include <shaders/ray_reconstruction_debug/RayReconstructionBypass_Dx12.h>
#include <shaders/ray_reconstruction_debug/RayReconstructionPeripheralDenoiser_Dx12.h>
#include <shaders/gaze_roi/GazeRoi_Dx12.h>
#include <memory>
#include <string>

class DLSSDFeatureDx12 : public DLSSDFeature, public IFeature_Dx12
{
  private:
    std::unique_ptr<RayReconstructionBypassDx12> _rawColorBypass;
    std::unique_ptr<RayReconstructionPeripheralDenoiserDx12> _peripheralDenoiser;
    bool _rawColorBypassWasEnabled = false;
    bool _peripheralDenoiserWasEnabled = false;
    float _previousJitterOffsetX = 0.0f;
    float _previousJitterOffsetY = 0.0f;
    std::unique_ptr<GazeRoi_Dx12> _gazeRoi;
    std::unique_ptr<GazeRoiMvPatch_Dx12> _gazeRoiMvPatch;
    NVSDK_NGX_Handle _gazeRoiRrHandle = {};
    NVSDK_NGX_Handle* _p_gazeRoiRrHandle = nullptr;
    GazeRoiRect _gazeRoiHandleInputRect = {};
    GazeRoiRect _gazeRoiHandleOutputRect = {};
    std::string _gazeRoiHandleSignature;
    GazeRoiRect _gazePreviousInputRect = {};
    GazeRoiRect _gazePreviousOutputRect = {};
    GazeRoiRect _gazePreviousMvRect = {};
    std::string _gazeRoiNativeResourceSignature;
    bool _gazeHasPreviousRect = false;
    bool _gazeRoiHandleWasCreated = false;
    bool _gazeRoiRawColorBypassWasEnabled = false;
    float _gazePointX = 0.5f;
    float _gazePointY = 0.5f;
    float _gazePreviousJitterOffsetX = 0.0f;
    float _gazePreviousJitterOffsetY = 0.0f;
    void UpdateGazePoint();
    bool BuildGazeRoiRects(GazeRoiRect& outputRect, GazeRoiRect& inputRect);
    bool EnsureGazeRoiRrHandle(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters,
                               const GazeRoiRect& outputRect, const GazeRoiRect& inputRect);
    void LogGazeRoiNativeResources(NVSDK_NGX_Parameter* parameters);
    bool TryEvaluateGazeRoi(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters,
                            NVSDK_NGX_Result& result);
  protected:
    bool InitDLSSD(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);

  public:
    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    feature_version Version() override { return DLSSDFeature::Version(); }
    Upscaler GetUpscalerType() const final { return DLSSDFeature::GetUpscalerType(); }

    bool IsWithDx12() override { return false; }

    DLSSDFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~DLSSDFeatureDx12();
};
