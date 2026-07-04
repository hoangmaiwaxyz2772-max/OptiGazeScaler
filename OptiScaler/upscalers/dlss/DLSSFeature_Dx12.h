#pragma once
#include "DLSSFeature.h"
#include <upscalers/IFeature_Dx12.h>
#include <shaders/rcas/RCAS_Dx12.h>
#include <shaders/gaze_roi/GazeRoi_Dx12.h>
#include <memory>
#include <string>

class DLSSFeatureDx12 : public DLSSFeature, public IFeature_Dx12
{
  private:
    std::unique_ptr<GazeRoi_Dx12> GazeRoi = nullptr;
    std::unique_ptr<GazeRoiMvPatch_Dx12> GazeRoiMvPatch = nullptr;
    NVSDK_NGX_Handle _gazeRoiDlssHandle = {};
    NVSDK_NGX_Handle* _p_gazeRoiDlssHandle = nullptr;
    GazeRoiRect _gazeRoiHandleInputRect = {};
    GazeRoiRect _gazeRoiHandleOutputRect = {};
    GazeRoiRect _gazePreviousInputRect = {};
    bool _gazeHasPreviousInputRect = false;
    float _gazePointX = 0.5f;
    float _gazePointY = 0.5f;

    void UpdateVirtualGazePoint();
    bool BuildGazeRoiRects(GazeRoiRect& outputRect, GazeRoiRect& inputRect);
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
