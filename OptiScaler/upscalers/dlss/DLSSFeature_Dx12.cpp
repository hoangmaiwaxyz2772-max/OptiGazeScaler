#include <pch.h>
#include "DLSSFeature_Dx12.h"
#include <dxgi1_4.h>
#include <Config.h>

namespace
{
template <typename T> struct SavedNgxParam
{
    const char* key = nullptr;
    T value {};
    bool hasValue = false;

    SavedNgxParam(NVSDK_NGX_Parameter* params, const char* inKey, T fallback = {})
        : key(inKey), value(fallback)
    {
        hasValue = params != nullptr && params->Get(key, &value) == NVSDK_NGX_Result_Success;
    }

    void Restore(NVSDK_NGX_Parameter* params) const
    {
        if (params == nullptr)
            return;

        params->Set(key, value);
    }
};

struct SavedNgxResourceParam
{
    const char* key = nullptr;
    ID3D12Resource* value = nullptr;
    bool hasValue = false;

    SavedNgxResourceParam(NVSDK_NGX_Parameter* params, const char* inKey) : key(inKey)
    {
        hasValue = params != nullptr && params->Get(key, &value) == NVSDK_NGX_Result_Success;
        if (!hasValue && params != nullptr)
            hasValue = params->Get(key, (void**)&value) == NVSDK_NGX_Result_Success;
    }

    void Restore(NVSDK_NGX_Parameter* params) const
    {
        if (params != nullptr && hasValue)
            params->Set(key, value);
    }
};

static uint32_t AlignDown(uint32_t value, uint32_t alignment)
{
    return alignment == 0 ? value : (value / alignment) * alignment;
}

static uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    return alignment == 0 ? value : ((value + alignment - 1) / alignment) * alignment;
}
} // namespace

bool DLSSFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;

    return InitDLSS(InCommandList, InParameters);
}

bool DLSSFeatureDx12::InitDLSS(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");
        return false;
    }

    if (!_dlssInited)
    {
        _dlssInited = NVNGXProxy::InitDx12(Device);

        if (!_dlssInited)
            return false;

        _moduleLoaded =
            (NVNGXProxy::D3D12_Init_ProjectID() != nullptr || NVNGXProxy::D3D12_Init_Ext() != nullptr) &&
            (NVNGXProxy::D3D12_Shutdown() != nullptr || NVNGXProxy::D3D12_Shutdown1() != nullptr) &&
            (NVNGXProxy::D3D12_GetParameters() != nullptr || NVNGXProxy::D3D12_AllocateParameters() != nullptr) &&
            NVNGXProxy::D3D12_DestroyParameters() != nullptr && NVNGXProxy::D3D12_CreateFeature() != nullptr &&
            NVNGXProxy::D3D12_ReleaseFeature() != nullptr && NVNGXProxy::D3D12_EvaluateFeature() != nullptr;

        // delay between init and create feature
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    LOG_INFO("Creating DLSS feature");

    if (NVNGXProxy::D3D12_CreateFeature() != nullptr)
    {
        ProcessInitParams(InParameters);

        _p_dlssHandle = &_dlssHandle;

        NVSDK_NGX_Result nvResult;
        {
            ScopedSkipHeapCapture skipHeapCapture {};

            nvResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                         &_p_dlssHandle);
        }

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_CreateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
        else
        {
            LOG_INFO("_CreateFeature result: NVSDK_NGX_Result_Success");
        }
    }
    else
    {
        LOG_ERROR("_CreateFeature is nullptr");
        return false;
    }

    ReadVersion();

    SetInit(true);
    return true;
}

bool DLSSFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    NVSDK_NGX_Result nvResult;

    if (NVNGXProxy::D3D12_EvaluateFeature() != nullptr)
    {
        ProcessEvaluateParams(InParameters);

        if (Config::Instance()->GazeRoiEnabled.value_or_default() &&
            TryEvaluateGazeRoi(InCommandList, InParameters, nvResult))
        {
            if (nvResult == NVSDK_NGX_Result_Success)
            {
                _frameCount++;
                return true;
            }

            LOG_WARN("Gaze ROI path failed ({:X}), falling back to full-frame DLSS",
                     static_cast<unsigned int>(nvResult));
        }

        nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _p_dlssHandle, InParameters, NULL);

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_EvaluateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
    }
    else
    {
        LOG_ERROR("_EvaluateFeature is nullptr");
        return false;
    }

    _frameCount++;

    return true;
}

void DLSSFeatureDx12::UpdateVirtualGazePoint()
{
    const std::string control = Config::Instance()->GazeRoiControl.value_or_default();

    if (control == "Mouse")
    {
        POINT cursor {};
        HWND foregroundWindow = GetForegroundWindow();
        RECT clientRect {};
        if (foregroundWindow != nullptr && GetCursorPos(&cursor) && ScreenToClient(foregroundWindow, &cursor) &&
            GetClientRect(foregroundWindow, &clientRect))
        {
            const int32_t width = clientRect.right - clientRect.left;
            const int32_t height = clientRect.bottom - clientRect.top;
            if (width > 0 && height > 0)
            {
                _gazePointX = static_cast<float>(cursor.x) / static_cast<float>(width);
                _gazePointY = static_cast<float>(cursor.y) / static_cast<float>(height);
            }
        }

        _gazePointX = std::clamp(_gazePointX, 0.0f, 1.0f);
        _gazePointY = std::clamp(_gazePointY, 0.0f, 1.0f);
        return;
    }

    if (control != "Keyboard")
        return;

    constexpr float defaultStep = 0.05f;
    constexpr float fineStep = 0.01f;
    const float step = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? fineStep : defaultStep;

    if (GetAsyncKeyState(VK_F5) & 0x0001)
        _gazePointX -= step;

    if (GetAsyncKeyState(VK_F6) & 0x0001)
        _gazePointX += step;

    if (GetAsyncKeyState(VK_F7) & 0x0001)
        _gazePointY -= step;

    if (GetAsyncKeyState(VK_F8) & 0x0001)
        _gazePointY += step;

    if (GetAsyncKeyState(VK_F9) & 0x0001)
    {
        _gazePointX = 0.5f;
        _gazePointY = 0.5f;
        _gazeHasPreviousInputRect = false;
    }

    _gazePointX = std::clamp(_gazePointX, 0.0f, 1.0f);
    _gazePointY = std::clamp(_gazePointY, 0.0f, 1.0f);
}

bool DLSSFeatureDx12::BuildGazeRoiRects(GazeRoiRect& outputRect, GazeRoiRect& inputRect)
{
    const uint32_t targetWidth = TargetWidth();
    const uint32_t targetHeight = TargetHeight();
    const uint32_t renderWidth = RenderWidth();
    const uint32_t renderHeight = RenderHeight();

    if (targetWidth == 0 || targetHeight == 0 || renderWidth == 0 || renderHeight == 0)
        return false;

    const float scaleX = static_cast<float>(renderWidth) / static_cast<float>(targetWidth);
    const float scaleY = static_cast<float>(renderHeight) / static_cast<float>(targetHeight);
    const float outputScaleX = static_cast<float>(targetWidth) / static_cast<float>(renderWidth);
    const float outputScaleY = static_cast<float>(targetHeight) / static_cast<float>(renderHeight);

    const uint32_t desiredOutputWidth =
        std::min(std::max<uint32_t>(16, static_cast<uint32_t>(Config::Instance()->GazeRoiWidthPx.value_or_default())),
                 targetWidth);
    const uint32_t desiredOutputHeight =
        std::min(std::max<uint32_t>(16, static_cast<uint32_t>(Config::Instance()->GazeRoiHeightPx.value_or_default())),
                 targetHeight);

    inputRect.width =
        std::max<uint32_t>(16, AlignUp(static_cast<uint32_t>(std::round(desiredOutputWidth * scaleX)), 8));
    inputRect.height =
        std::max<uint32_t>(16, AlignUp(static_cast<uint32_t>(std::round(desiredOutputHeight * scaleY)), 8));
    inputRect.width = std::min(inputRect.width, renderWidth);
    inputRect.height = std::min(inputRect.height, renderHeight);

    const int32_t centeredInputX =
        static_cast<int32_t>(_gazePointX * renderWidth) - static_cast<int32_t>(inputRect.width / 2);
    const int32_t centeredInputY =
        static_cast<int32_t>(_gazePointY * renderHeight) - static_cast<int32_t>(inputRect.height / 2);
    inputRect.x = static_cast<uint32_t>(
        std::clamp(centeredInputX, 0, static_cast<int32_t>(renderWidth - inputRect.width)));
    inputRect.y = static_cast<uint32_t>(
        std::clamp(centeredInputY, 0, static_cast<int32_t>(renderHeight - inputRect.height)));

    if (inputRect.x >= renderWidth || inputRect.y >= renderHeight)
        return false;

    inputRect.width = std::min(inputRect.width, renderWidth - inputRect.x);
    inputRect.height = std::min(inputRect.height, renderHeight - inputRect.y);

    outputRect.width =
        std::max<uint32_t>(1, static_cast<uint32_t>(std::round(inputRect.width * outputScaleX)));
    outputRect.height =
        std::max<uint32_t>(1, static_cast<uint32_t>(std::round(inputRect.height * outputScaleY)));
    outputRect.x = std::min(static_cast<uint32_t>(std::round(inputRect.x * outputScaleX)),
                            targetWidth - outputRect.width);
    outputRect.y = std::min(static_cast<uint32_t>(std::round(inputRect.y * outputScaleY)),
                            targetHeight - outputRect.height);

    if (outputRect.x >= targetWidth || outputRect.y >= targetHeight)
        return false;

    outputRect.width = std::min(outputRect.width, targetWidth - outputRect.x);
    outputRect.height = std::min(outputRect.height, targetHeight - outputRect.y);
    return inputRect.width > 0 && inputRect.height > 0 && outputRect.width > 0 && outputRect.height > 0;
}

bool DLSSFeatureDx12::EnsureGazeRoiDlssHandle(ID3D12GraphicsCommandList* InCommandList,
                                              NVSDK_NGX_Parameter* InParameters, const GazeRoiRect& outputRect,
                                              const GazeRoiRect& inputRect)
{
    if (InCommandList == nullptr || InParameters == nullptr || NVNGXProxy::D3D12_CreateFeature() == nullptr)
        return false;

    const bool handleMatches = _p_gazeRoiDlssHandle != nullptr &&
                               _gazeRoiHandleInputRect.width == inputRect.width &&
                               _gazeRoiHandleInputRect.height == inputRect.height &&
                               _gazeRoiHandleOutputRect.width == outputRect.width &&
                               _gazeRoiHandleOutputRect.height == outputRect.height;
    if (handleMatches)
        return true;

    if (_p_gazeRoiDlssHandle != nullptr && NVNGXProxy::D3D12_ReleaseFeature() != nullptr)
    {
        NVNGXProxy::D3D12_ReleaseFeature()(_p_gazeRoiDlssHandle);
        _p_gazeRoiDlssHandle = nullptr;
        _gazeRoiDlssHandle = {};
    }

    SavedNgxParam<unsigned int> savedWidth(InParameters, NVSDK_NGX_Parameter_Width, RenderWidth());
    SavedNgxParam<unsigned int> savedHeight(InParameters, NVSDK_NGX_Parameter_Height, RenderHeight());
    SavedNgxParam<unsigned int> savedOutWidth(InParameters, NVSDK_NGX_Parameter_OutWidth, TargetWidth());
    SavedNgxParam<unsigned int> savedOutHeight(InParameters, NVSDK_NGX_Parameter_OutHeight, TargetHeight());
    SavedNgxParam<int> savedOutputSubrects(InParameters, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    SavedNgxParam<int> savedFeatureFlags(InParameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                                         GetFeatureFlags());
    SavedNgxParam<int> savedPerfQuality(InParameters, NVSDK_NGX_Parameter_PerfQualityValue,
                                        static_cast<int>(PerfQualityValue()));

    InParameters->Set(NVSDK_NGX_Parameter_Width, inputRect.width);
    InParameters->Set(NVSDK_NGX_Parameter_Height, inputRect.height);
    InParameters->Set(NVSDK_NGX_Parameter_OutWidth, outputRect.width);
    InParameters->Set(NVSDK_NGX_Parameter_OutHeight, outputRect.height);
    InParameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(PerfQualityValue()));
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, GetFeatureFlags());
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);

    _p_gazeRoiDlssHandle = &_gazeRoiDlssHandle;

    NVSDK_NGX_Result createResult;
    {
        ScopedSkipHeapCapture skipHeapCapture {};
        createResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_SuperSampling, InParameters,
                                                        &_p_gazeRoiDlssHandle);
    }

    savedWidth.Restore(InParameters);
    savedHeight.Restore(InParameters);
    savedOutWidth.Restore(InParameters);
    savedOutHeight.Restore(InParameters);
    savedOutputSubrects.Restore(InParameters);
    savedFeatureFlags.Restore(InParameters);
    savedPerfQuality.Restore(InParameters);

    if (createResult != NVSDK_NGX_Result_Success)
    {
        LOG_WARN("Gaze ROI DLSS create failed ({:X}): roi feature {}x{} -> {}x{}",
                 static_cast<unsigned int>(createResult), inputRect.width, inputRect.height, outputRect.width,
                 outputRect.height);
        _p_gazeRoiDlssHandle = nullptr;
        _gazeRoiDlssHandle = {};
        return false;
    }

    _gazeRoiHandleInputRect = inputRect;
    _gazeRoiHandleOutputRect = outputRect;
    LOG_INFO("Gaze ROI DLSS feature created: {}x{} -> {}x{}", inputRect.width, inputRect.height, outputRect.width,
             outputRect.height);
    return true;
}

bool DLSSFeatureDx12::TryEvaluateGazeRoi(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters,
                                         NVSDK_NGX_Result& nvResult)
{
    nvResult = NVSDK_NGX_Result_Fail;

    if (InCommandList == nullptr || InParameters == nullptr || Device == nullptr || _p_dlssHandle == nullptr)
        return false;

    ID3D12Resource* color = nullptr;
    if (InParameters->Get(NVSDK_NGX_Parameter_Color, &color) != NVSDK_NGX_Result_Success)
        InParameters->Get(NVSDK_NGX_Parameter_Color, (void**)&color);

    ID3D12Resource* output = nullptr;
    if (InParameters->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success)
        InParameters->Get(NVSDK_NGX_Parameter_Output, (void**)&output);

    ID3D12Resource* motionVectors = nullptr;
    if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &motionVectors) != NVSDK_NGX_Result_Success)
        InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**)&motionVectors);

    ID3D12Resource* depth = nullptr;
    if (InParameters->Get(NVSDK_NGX_Parameter_Depth, &depth) != NVSDK_NGX_Result_Success)
        InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**)&depth);

    if (color == nullptr || output == nullptr || motionVectors == nullptr || depth == nullptr)
    {
        LOG_WARN("Gaze ROI disabled for this frame: missing color/depth/output/MV");
        return false;
    }

    UpdateVirtualGazePoint();

    GazeRoiRect outputRect {};
    GazeRoiRect inputRect {};
    if (!BuildGazeRoiRects(outputRect, inputRect))
    {
        LOG_WARN("Gaze ROI disabled for this frame: invalid ROI rect");
        return false;
    }

    if (GazeRoi == nullptr)
        GazeRoi = std::make_unique<GazeRoi_Dx12>("GazeRoi", Device);

    if (GazeRoiMvPatch == nullptr)
        GazeRoiMvPatch = std::make_unique<GazeRoiMvPatch_Dx12>("GazeRoiMvPatch", Device);

    if (GazeRoi == nullptr || !GazeRoi->IsInit() ||
        !GazeRoi->CreateDlssOutputResource(Device, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        LOG_WARN("Gaze ROI disabled for this frame: helper init/resource failed");
        return false;
    }

    if (GazeRoiMvPatch == nullptr || !GazeRoiMvPatch->IsInit() ||
        !GazeRoiMvPatch->CreatePatchedResource(Device, motionVectors, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        LOG_WARN("Gaze ROI disabled for this frame: MV patch init/resource failed");
        return false;
    }

    if (!EnsureGazeRoiDlssHandle(InCommandList, InParameters, outputRect, inputRect))
    {
        LOG_WARN("Gaze ROI disabled for this frame: ROI DLSS feature create failed");
        return false;
    }

    SavedNgxResourceParam savedOutput(InParameters, NVSDK_NGX_Parameter_Output);
    SavedNgxResourceParam savedMotionVectors(InParameters, NVSDK_NGX_Parameter_MotionVectors);
    SavedNgxParam<unsigned int> savedOutputSubrects(InParameters, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    SavedNgxParam<unsigned int> savedColorBaseX(InParameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0);
    SavedNgxParam<unsigned int> savedColorBaseY(InParameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0);
    SavedNgxParam<unsigned int> savedDepthBaseX(InParameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0);
    SavedNgxParam<unsigned int> savedDepthBaseY(InParameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0);
    SavedNgxParam<unsigned int> savedMvBaseX(InParameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0);
    SavedNgxParam<unsigned int> savedMvBaseY(InParameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0);
    SavedNgxParam<unsigned int> savedOutputBaseX(InParameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);
    SavedNgxParam<unsigned int> savedOutputBaseY(InParameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0);
    SavedNgxParam<unsigned int> savedTranslucencyBaseX(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, 0);
    SavedNgxParam<unsigned int> savedTranslucencyBaseY(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y, 0);
    SavedNgxParam<unsigned int> savedBiasBaseX(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, 0);
    SavedNgxParam<unsigned int> savedBiasBaseY(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, 0);
    SavedNgxParam<unsigned int> savedTransparencyLayerBaseX(
        InParameters, NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_X, 0);
    SavedNgxParam<unsigned int> savedTransparencyLayerBaseY(
        InParameters, NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_Y, 0);
    SavedNgxParam<unsigned int> savedTransparencyOpacityBaseX(
        InParameters, NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_X, 0);
    SavedNgxParam<unsigned int> savedTransparencyOpacityBaseY(
        InParameters, NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_Y, 0);
    SavedNgxParam<unsigned int> savedTransparencyMvecsBaseX(
        InParameters, NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_X, 0);
    SavedNgxParam<unsigned int> savedTransparencyMvecsBaseY(
        InParameters, NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_Y, 0);
    SavedNgxParam<unsigned int> savedDisocclusionBaseX(
        InParameters, NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_X, 0);
    SavedNgxParam<unsigned int> savedDisocclusionBaseY(
        InParameters, NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_Y, 0);
    SavedNgxParam<unsigned int> savedRenderWidth(InParameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
                                                 RenderWidth());
    SavedNgxParam<unsigned int> savedRenderHeight(
        InParameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, RenderHeight());
    unsigned int featureWidth = RenderWidth();
    unsigned int featureHeight = RenderHeight();
    unsigned int featureOutWidth = TargetWidth();
    unsigned int featureOutHeight = TargetHeight();
    InParameters->Get(NVSDK_NGX_Parameter_Width, &featureWidth);
    InParameters->Get(NVSDK_NGX_Parameter_Height, &featureHeight);
    InParameters->Get(NVSDK_NGX_Parameter_OutWidth, &featureOutWidth);
    InParameters->Get(NVSDK_NGX_Parameter_OutHeight, &featureOutHeight);

    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
    if (std::abs(mvScaleX) < 0.0001f)
        mvScaleX = 1.0f;
    if (std::abs(mvScaleY) < 0.0001f)
        mvScaleY = 1.0f;

    float rawMvOffsetX = 0.0f;
    float rawMvOffsetY = 0.0f;
    if (_gazeHasPreviousInputRect)
    {
        const int32_t deltaX = static_cast<int32_t>(inputRect.x) - static_cast<int32_t>(_gazePreviousInputRect.x);
        const int32_t deltaY = static_cast<int32_t>(inputRect.y) - static_cast<int32_t>(_gazePreviousInputRect.y);
        rawMvOffsetX = static_cast<float>(deltaX) / mvScaleX;
        rawMvOffsetY = static_cast<float>(deltaY) / mvScaleY;
    }

    auto mvDesc = motionVectors->GetDesc();
    GazeRoiMvConstants mvConstants {};
    mvConstants.width = static_cast<int32_t>(mvDesc.Width);
    mvConstants.height = static_cast<int32_t>(mvDesc.Height);
    mvConstants.rawOffsetX = rawMvOffsetX;
    mvConstants.rawOffsetY = rawMvOffsetY;

    GazeRoiMvPatch->SetPatchedState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!GazeRoiMvPatch->Dispatch(InCommandList, motionVectors, mvConstants))
    {
        LOG_WARN("Gaze ROI disabled for this frame: MV patch dispatch failed");
        _gazeHasPreviousInputRect = false;
        return false;
    }
    GazeRoiMvPatch->SetPatchedState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    GazeRoi->SetDlssOutputState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    InParameters->Set(NVSDK_NGX_Parameter_Output, GazeRoi->DlssOutput());
    InParameters->Set(NVSDK_NGX_Parameter_MotionVectors, GazeRoiMvPatch->PatchedMotionVectors());
    InParameters->Set(NVSDK_NGX_Parameter_Width, inputRect.width);
    InParameters->Set(NVSDK_NGX_Parameter_Height, inputRect.height);
    InParameters->Set(NVSDK_NGX_Parameter_OutWidth, outputRect.width);
    InParameters->Set(NVSDK_NGX_Parameter_OutHeight, outputRect.height);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, inputRect.x);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, inputRect.y);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, inputRect.x);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, inputRect.y);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, inputRect.x);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, inputRect.y);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, inputRect.x);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y, inputRect.y);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, inputRect.x);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, inputRect.y);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_X, inputRect.x);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_Y, inputRect.y);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_X, inputRect.x);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_Y, inputRect.y);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_X, inputRect.x);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_Y, inputRect.y);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_X, inputRect.x);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_Y, inputRect.y);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, outputRect.x);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, outputRect.y);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, inputRect.width);
    InParameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, inputRect.height);

    nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _p_gazeRoiDlssHandle, InParameters, NULL);

    savedOutput.Restore(InParameters);
    savedMotionVectors.Restore(InParameters);
    savedOutputSubrects.Restore(InParameters);
    savedColorBaseX.Restore(InParameters);
    savedColorBaseY.Restore(InParameters);
    savedDepthBaseX.Restore(InParameters);
    savedDepthBaseY.Restore(InParameters);
    savedMvBaseX.Restore(InParameters);
    savedMvBaseY.Restore(InParameters);
    savedOutputBaseX.Restore(InParameters);
    savedOutputBaseY.Restore(InParameters);
    savedTranslucencyBaseX.Restore(InParameters);
    savedTranslucencyBaseY.Restore(InParameters);
    savedBiasBaseX.Restore(InParameters);
    savedBiasBaseY.Restore(InParameters);
    savedTransparencyLayerBaseX.Restore(InParameters);
    savedTransparencyLayerBaseY.Restore(InParameters);
    savedTransparencyOpacityBaseX.Restore(InParameters);
    savedTransparencyOpacityBaseY.Restore(InParameters);
    savedTransparencyMvecsBaseX.Restore(InParameters);
    savedTransparencyMvecsBaseY.Restore(InParameters);
    savedDisocclusionBaseX.Restore(InParameters);
    savedDisocclusionBaseY.Restore(InParameters);
    savedRenderWidth.Restore(InParameters);
    savedRenderHeight.Restore(InParameters);
    InParameters->Set(NVSDK_NGX_Parameter_Width, featureWidth);
    InParameters->Set(NVSDK_NGX_Parameter_Height, featureHeight);
    InParameters->Set(NVSDK_NGX_Parameter_OutWidth, featureOutWidth);
    InParameters->Set(NVSDK_NGX_Parameter_OutHeight, featureOutHeight);

    if (nvResult != NVSDK_NGX_Result_Success)
    {
        LOG_WARN("Gaze ROI DLSS evaluate failed ({:X}): roiFeature {}x{} -> {}x{}, subrect input {}x{}+{},{} "
                 "originalFeature {}x{} -> {}x{}, "
                 "outputBase {},{} expectedOut {}x{}, mvScale {},{} rawMvOffset {},{} mvTex {}x{}",
                 static_cast<unsigned int>(nvResult), inputRect.width, inputRect.height, outputRect.width,
                 outputRect.height, inputRect.width, inputRect.height, inputRect.x, inputRect.y, featureWidth,
                 featureHeight, featureOutWidth, featureOutHeight, outputRect.x, outputRect.y, outputRect.width,
                 outputRect.height, mvScaleX, mvScaleY, rawMvOffsetX, rawMvOffsetY, mvDesc.Width, mvDesc.Height);
        _gazeHasPreviousInputRect = false;
        return true;
    }

    GazeRoi->SetDlssOutputState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    GazeRoiConstants constants {};
    constants.srcWidth = static_cast<int32_t>(RenderWidth());
    constants.srcHeight = static_cast<int32_t>(RenderHeight());
    constants.dstWidth = static_cast<int32_t>(TargetWidth());
    constants.dstHeight = static_cast<int32_t>(TargetHeight());
    constants.roiX = static_cast<int32_t>(outputRect.x);
    constants.roiY = static_cast<int32_t>(outputRect.y);
    constants.roiWidth = static_cast<int32_t>(outputRect.width);
    constants.roiHeight = static_cast<int32_t>(outputRect.height);
    constants.featherPx = Config::Instance()->GazeRoiFeatherPx.value_or_default();
    constants.debugBorderPx = Config::Instance()->GazeRoiDebugBorder.value_or_default() ? 2 : 0;

    if (!GazeRoi->Dispatch(InCommandList, color, output, constants))
    {
        LOG_WARN("Gaze ROI composite failed");
        nvResult = NVSDK_NGX_Result_Fail;
        _gazeHasPreviousInputRect = false;
        return true;
    }

    _gazePreviousInputRect = inputRect;
    _gazeHasPreviousInputRect = true;

    LOG_DEBUG("Gaze ROI: out {}x{}+{},{} input {}x{}+{},{} rawMvOffset {},{}",
              outputRect.width, outputRect.height, outputRect.x, outputRect.y, inputRect.width, inputRect.height,
              inputRect.x, inputRect.y, rawMvOffsetX, rawMvOffsetY);
    return true;
}

void DLSSFeatureDx12::Shutdown(ID3D12Device* InDevice)
{
    if (_dlssInited)
    {
        if (NVNGXProxy::D3D12_Shutdown() != nullptr)
            NVNGXProxy::D3D12_Shutdown()();
        else if (NVNGXProxy::D3D12_Shutdown1() != nullptr)
            NVNGXProxy::D3D12_Shutdown1()(InDevice);
    }

    DLSSFeature::Shutdown();
}

DLSSFeatureDx12::DLSSFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters), DLSSFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSFeatureDx12::~DLSSFeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    if (NVNGXProxy::D3D12_ReleaseFeature() != nullptr && _p_gazeRoiDlssHandle != nullptr)
        NVNGXProxy::D3D12_ReleaseFeature()(_p_gazeRoiDlssHandle);

    if (NVNGXProxy::D3D12_ReleaseFeature() != nullptr && _p_dlssHandle != nullptr)
        NVNGXProxy::D3D12_ReleaseFeature()(_p_dlssHandle);
}
