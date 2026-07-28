#include <pch.h>
#include "DLSSDFeature_Dx12.h"
#include <dxgi1_4.h>
#include <Config.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shaders/gaze_roi/GazeRoi_Dx12.h>
#include <resource_tracking/ResTrack_dx12.h>
#include <gaze_roi/GazeRoiInput.h>
#include <hooks/GazeRoiStreamlineContext.h>

#include <functional>
#include <limits>
#include <sstream>

namespace
{
constexpr bool kGazeRoiRrTemporarilyDisabled = true;

template <typename T> T GetNgxValue(NVSDK_NGX_Parameter* params, const char* key, T fallback)
{
    T value = fallback;
    if (params != nullptr)
        params->Get(key, &value);
    return value;
}

ID3D12Resource* GetNgxResource(NVSDK_NGX_Parameter* params, const char* key)
{
    ID3D12Resource* resource = nullptr;
    if (params != nullptr)
        params->Get(key, &resource);
    return resource;
}

bool TryGetNgxUint(NVSDK_NGX_Parameter* params, const char* key, uint32_t& value)
{
    return params != nullptr && key != nullptr &&
           params->Get(key, &value) == NVSDK_NGX_Result_Success;
}

bool ResourceRectFits(ID3D12Resource* resource, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (resource == nullptr || width == 0 || height == 0)
        return false;
    const auto desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 &&
           static_cast<uint64_t>(x) + width <= desc.Width && static_cast<uint64_t>(y) + height <= desc.Height;
}

bool AddBase(uint32_t base, uint32_t offset, uint32_t& result)
{
    const uint64_t sum = static_cast<uint64_t>(base) + offset;
    if (sum > std::numeric_limits<uint32_t>::max())
        return false;
    result = static_cast<uint32_t>(sum);
    return true;
}

bool RectsOverlap(const GazeRoiRect& a, const GazeRoiRect& b)
{
    return static_cast<uint64_t>(a.x) < static_cast<uint64_t>(b.x) + b.width &&
           static_cast<uint64_t>(b.x) < static_cast<uint64_t>(a.x) + a.width &&
           static_cast<uint64_t>(a.y) < static_cast<uint64_t>(b.y) + b.height &&
           static_cast<uint64_t>(b.y) < static_cast<uint64_t>(a.y) + a.height;
}

class ScopedNgxOverrides
{
  public:
    explicit ScopedNgxOverrides(NVSDK_NGX_Parameter* parameters) : _parameters(parameters) {}
    ~ScopedNgxOverrides()
    {
        for (auto it = _restores.rbegin(); it != _restores.rend(); ++it)
            (*it)();
    }

    template <typename T> bool Override(const char* key, T value)
    {
        if (_parameters == nullptr || key == nullptr)
            return false;
        T original {};
        if (_parameters->Get(key, &original) != NVSDK_NGX_Result_Success)
            return false;
        _restores.emplace_back([parameters = _parameters, key, original]() { parameters->Set(key, original); });
        _parameters->Set(key, value);
        return true;
    }

  private:
    NVSDK_NGX_Parameter* _parameters = nullptr;
    std::vector<std::function<void()>> _restores;
};

struct RrGuideSubrect
{
    const char* label;
    const char* resourceKey;
    const char* baseXKey;
    const char* baseYKey;
};

// Streamline's DLSSD plugin converts these ResourceTags into native NGX keys.
// Keep the resource pointer and format untouched; only localize the source base.
constexpr RrGuideSubrect kRrGuideSubrects[] = {
    { "biasCurrentColor", NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
      NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,
      NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y },
    { "diffuseAlbedo", "DLSS.Input.DiffuseAlbedo", "DLSS.Input.DiffuseAlbedo.Subrect.Base.X",
      "DLSS.Input.DiffuseAlbedo.Subrect.Base.Y" },
    { "specularAlbedo", "DLSS.Input.SpecularAlbedo", "DLSS.Input.SpecularAlbedo.Subrect.Base.X",
      "DLSS.Input.SpecularAlbedo.Subrect.Base.Y" },
    { "normals", NVSDK_NGX_Parameter_GBuffer_Normals, "DLSS.Input.Normals.Subrect.Base.X",
      "DLSS.Input.Normals.Subrect.Base.Y" },
    { "roughness", NVSDK_NGX_Parameter_GBuffer_Roughness, "DLSS.Input.Roughness.Subrect.Base.X",
      "DLSS.Input.Roughness.Subrect.Base.Y" },
    { "colorBeforeParticles", "DLSSD.ColorBeforeParticles", "DLSSD.ColorBeforeParticles.Subrect.Base.X",
      "DLSSD.ColorBeforeParticles.Subrect.Base.Y" },
    { "colorBeforeTransparency", "DLSSD.ColorBeforeTransparency",
      "DLSSD.ColorBeforeTransparency.Subrect.Base.X", "DLSSD.ColorBeforeTransparency.Subrect.Base.Y" },
    { "colorBeforeFog", "DLSSD.ColorBeforeFog", "DLSSD.ColorBeforeFog.Subrect.Base.X",
      "DLSSD.ColorBeforeFog.Subrect.Base.Y" },
    { "colorBeforeDepthOfField", "DLSSD.ColorBeforeDepthOfField",
      "DLSSD.ColorBeforeDepthOfField.Subrect.Base.X", "DLSSD.ColorBeforeDepthOfField.Subrect.Base.Y" },
    { "colorBeforeSss", "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering",
      "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering.Subrect.Base.X",
      "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering.Subrect.Base.Y" },
    { "colorBeforeRefraction", "DLSSD.ColorBeforeScreenSpaceRefraction",
      "DLSSD.ColorBeforeScreenSpaceRefraction.Subrect.Base.X",
      "DLSSD.ColorBeforeScreenSpaceRefraction.Subrect.Base.Y" },
    { "colorAfterParticles", "DLSSD.ColorAfterParticles", "DLSSD.ColorAfterParticles.Subrect.Base.X",
      "DLSSD.ColorAfterParticles.Subrect.Base.Y" },
    { "colorAfterTransparency", "DLSSD.ColorAfterTransparency",
      "DLSSD.ColorAfterTransparency.Subrect.Base.X", "DLSSD.ColorAfterTransparency.Subrect.Base.Y" },
    { "colorAfterFog", "DLSSD.ColorAfterFog", "DLSSD.ColorAfterFog.Subrect.Base.X",
      "DLSSD.ColorAfterFog.Subrect.Base.Y" },
    { "colorAfterDepthOfField", "DLSSD.ColorAfterDepthOfField",
      "DLSSD.ColorAfterDepthOfField.Subrect.Base.X", "DLSSD.ColorAfterDepthOfField.Subrect.Base.Y" },
    { "colorAfterSss", "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering",
      "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering.Subrect.Base.X",
      "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering.Subrect.Base.Y" },
    { "specularHitDistance", "DLSSD.SpecularHitDistance", "DLSSD.SpecularHitDistance.Subrect.Base.X",
      "DLSSD.SpecularHitDistance.Subrect.Base.Y" },
    { "specularRayDirection", "DLSSD.SpecularRayDirection",
      "DLSSD.SpecularRayDirection.Subrect.Base.X", "DLSSD.SpecularRayDirection.Subrect.Base.Y" },
    { "specularRayDirectionHitDistance", "DLSSD.SpecularRayDirectionHitDistance",
      "DLSSD.SpecularRayDirectionHitDistance.Subrect.Base.X",
      "DLSSD.SpecularRayDirectionHitDistance.Subrect.Base.Y" },
    { "diffuseHitDistance", "DLSSD.DiffuseHitDistance", "DLSSD.DiffuseHitDistance.Subrect.Base.X",
      "DLSSD.DiffuseHitDistance.Subrect.Base.Y" },
    { "diffuseRayDirection", "DLSSD.DiffuseRayDirection", "DLSSD.DiffuseRayDirection.Subrect.Base.X",
      "DLSSD.DiffuseRayDirection.Subrect.Base.Y" },
    { "diffuseRayDirectionHitDistance", "DLSSD.DiffuseRayDirectionHitDistance",
      "DLSSD.DiffuseRayDirectionHitDistance.Subrect.Base.X",
      "DLSSD.DiffuseRayDirectionHitDistance.Subrect.Base.Y" },
    { "reflectedAlbedo", "DLSSD.ReflectedAlbedo", "DLSSD.ReflectedAlbedo.Subrect.Base.X",
      "DLSSD.ReflectedAlbedo.Subrect.Base.Y" },
    { "subsurfaceGuide", "DLSSD.ScreenSpaceSubsurfaceScatteringGuide",
      "DLSSD.ScreenSpaceSubsurfaceScatteringGuide.Subrect.Base.X",
      "DLSSD.ScreenSpaceSubsurfaceScatteringGuide.Subrect.Base.Y" },
    { "highResDepth", NVSDK_NGX_Parameter_DepthHighRes, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
      NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y },
    { "disocclusionMask", NVSDK_NGX_Parameter_GBuffer_DisocclusionMask,
      NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_X,
      NVSDK_NGX_Parameter_DLSS_DisocclusionMask_Subrect_Base_Y },
    { "transparencyLayer", NVSDK_NGX_Parameter_DLSS_TransparencyLayer,
      NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_X,
      NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_Y },
    { "transparencyLayerOpacity", NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity,
      NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_X,
      NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity_Subrect_Base_Y },
    { "transparencyLayerMotionVectors", NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs,
      NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_X,
      NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs_Subrect_Base_Y },
};

bool OverrideRrGuideSubrects(ScopedNgxOverrides& scope, NVSDK_NGX_Parameter* parameters,
                             uint32_t baseX, uint32_t baseY)
{
    bool valid = true;
    for (const auto& guide : kRrGuideSubrects)
    {
        if (GetNgxResource(parameters, guide.resourceKey) == nullptr)
            continue;

        uint32_t originalX = 0;
        uint32_t originalY = 0;
        const bool hasX = TryGetNgxUint(parameters, guide.baseXKey, originalX);
        const bool hasY = TryGetNgxUint(parameters, guide.baseYKey, originalY);
        if (!hasX || !hasY)
        {
            LOG_WARN("[GROI_RR] guide={} resource is present but native subrect keys are missing x={} y={}",
                     guide.label, hasX, hasY);
            continue;
        }
        valid = scope.Override(guide.baseXKey, baseX) && scope.Override(guide.baseYKey, baseY) && valid;
    }
    return valid;
}

} // namespace

bool DLSSDFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;

    return InitDLSSD(InCommandList, InParameters);
}

bool DLSSDFeatureDx12::InitDLSSD(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_ERROR("nvngx.dll not loaded!");
        return false;
    }

    if (!_dlssdInited)
    {
        _dlssdInited = NVNGXProxy::InitDx12(Device);

        if (!_dlssdInited)
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

    LOG_INFO("Creating DLSSD feature");

    if (NVNGXProxy::D3D12_CreateFeature() != nullptr)
    {
        ProcessInitParams(InParameters);

        _p_dlssdHandle = &_dlssdHandle;

        NVSDK_NGX_Result nvResult;
        {
            ScopedSkipHeapCapture skipHeapCapture {};

            nvResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_RayReconstruction,
                                                         InParameters, &_p_dlssdHandle);
        }

        if (nvResult != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("_CreateFeature result: {0:X}", (unsigned int) nvResult);
            return false;
        }
        else
        {
            LOG_INFO("_CreateFeature result: NVSDK_NGX_Result_Success, HandleId: {0}", _p_dlssdHandle->Id);
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

void DLSSDFeatureDx12::UpdateGazePoint()
{
    const auto* streamlineContext = GazeRoiStreamlineContext::Current();
    const GazeRoiInputSample sample = streamlineContext != nullptr && streamlineContext->active
                                          ? GazeRoiInputSample { streamlineContext->gazeX,
                                                                streamlineContext->gazeY,
                                                                streamlineContext->recentered }
                                          : GazeRoiInput::Sample();
    _gazePointX = sample.x;
    _gazePointY = sample.y;
    if (sample.recentered)
        _gazeHasPreviousRect = false;
}

bool DLSSDFeatureDx12::BuildGazeRoiRects(GazeRoiRect& outputRect, GazeRoiRect& inputRect)
{
    const uint32_t renderWidth = RenderWidth();
    const uint32_t renderHeight = RenderHeight();
    const uint32_t targetWidth = TargetWidth();
    const uint32_t targetHeight = TargetHeight();
    if (renderWidth == 0 || renderHeight == 0 || targetWidth == 0 || targetHeight == 0)
        return false;

    outputRect.width = std::clamp<uint32_t>(Config::Instance()->GazeRoiWidthPx.value_or_default(), 64, targetWidth);
    outputRect.height = std::clamp<uint32_t>(Config::Instance()->GazeRoiHeightPx.value_or_default(), 64, targetHeight);
    const auto alignedInputSize = [](uint32_t outputSize, uint32_t renderExtent, uint32_t targetExtent)
    {
        const uint32_t scaled = std::max(64U, static_cast<uint32_t>(
            std::lround(static_cast<double>(outputSize) * renderExtent / targetExtent)));
        return std::min(renderExtent, (scaled + 7U) & ~7U);
    };
    inputRect.width = alignedInputSize(outputRect.width, renderWidth, targetWidth);
    inputRect.height = alignedInputSize(outputRect.height, renderHeight, targetHeight);

    const auto centeredOrigin = [](float gaze, uint32_t extent, uint32_t size)
    {
        const int64_t origin = static_cast<int64_t>(std::lround(gaze * extent)) - static_cast<int64_t>(size / 2);
        return static_cast<uint32_t>(std::clamp<int64_t>(origin, 0, static_cast<int64_t>(extent - size)));
    };
    inputRect.x = centeredOrigin(_gazePointX, renderWidth, inputRect.width);
    inputRect.y = centeredOrigin(_gazePointY, renderHeight, inputRect.height);

    const auto mapOrigin = [](uint32_t sourceOrigin, uint32_t sourceSize, uint32_t sourceExtent,
                              uint32_t targetSize, uint32_t targetExtent)
    {
        const double center = static_cast<double>(sourceOrigin) + static_cast<double>(sourceSize) * 0.5;
        const int64_t origin = static_cast<int64_t>(std::llround(
            center * static_cast<double>(targetExtent) / sourceExtent - static_cast<double>(targetSize) * 0.5));
        return static_cast<uint32_t>(std::clamp<int64_t>(origin, 0, static_cast<int64_t>(targetExtent - targetSize)));
    };
    outputRect.x = mapOrigin(inputRect.x, inputRect.width, renderWidth, outputRect.width, targetWidth);
    outputRect.y = mapOrigin(inputRect.y, inputRect.height, renderHeight, outputRect.height, targetHeight);
    return true;
}

bool DLSSDFeatureDx12::EnsureGazeRoiRrHandle(ID3D12GraphicsCommandList* commandList,
                                              NVSDK_NGX_Parameter* parameters,
                                              const GazeRoiRect& outputRect,
                                              const GazeRoiRect& inputRect)
{
    if (commandList == nullptr || parameters == nullptr || _gazeRoi == nullptr ||
        _gazeRoi->DlssOutput() == nullptr || NVNGXProxy::D3D12_CreateFeature() == nullptr)
        return false;

    const uint32_t flags = GetNgxValue<uint32_t>(parameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                                                  static_cast<uint32_t>(GetFeatureFlags()));
    const int quality = GetNgxValue<int>(parameters, NVSDK_NGX_Parameter_PerfQualityValue,
                                         static_cast<int>(PerfQualityValue()));
    const std::string signature = std::format("{}x{}->{}x{}:flags={}:quality={}:output={:X}",
                                               inputRect.width, inputRect.height, outputRect.width,
                                               outputRect.height, flags, quality,
                                               reinterpret_cast<uintptr_t>(_gazeRoi->DlssOutput()));
    if (_p_gazeRoiRrHandle != nullptr && signature == _gazeRoiHandleSignature)
    {
        _gazeRoiHandleWasCreated = false;
        return true;
    }

    if (_p_gazeRoiRrHandle != nullptr)
    {
        NVSDK_NGX_Handle* retired = _p_gazeRoiRrHandle;
        const bool copiedLocalHandle = retired == &_gazeRoiRrHandle;
        if (copiedLocalHandle)
            retired = new NVSDK_NGX_Handle(_gazeRoiRrHandle);
        GazeRoiFrameSync::DeferCallback([retired, copiedLocalHandle]()
        {
            if (const auto release = NVNGXProxy::D3D12_ReleaseFeature(); release != nullptr)
                release(retired);
            if (copiedLocalHandle)
                delete retired;
        });
        _p_gazeRoiRrHandle = nullptr;
        _gazeRoiRrHandle = {};
    }

    ScopedNgxOverrides scope(parameters);
    const bool createContract =
        scope.Override(NVSDK_NGX_Parameter_Width, inputRect.width) &&
        scope.Override(NVSDK_NGX_Parameter_Height, inputRect.height) &&
        scope.Override(NVSDK_NGX_Parameter_OutWidth, outputRect.width) &&
        scope.Override(NVSDK_NGX_Parameter_OutHeight, outputRect.height) &&
        scope.Override(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1) &&
        scope.Override(NVSDK_NGX_Parameter_Output, _gazeRoi->DlssOutput()) &&
        scope.Override(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0U) &&
        scope.Override(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0U);
    if (!createContract)
    {
        LOG_ERROR("[GROI_RR] native create parameter table is missing a required ROI key");
        return false;
    }

    _p_gazeRoiRrHandle = &_gazeRoiRrHandle;
    ScopedSkipHeapCapture skipHeapCapture {};
    const NVSDK_NGX_Result result = NVNGXProxy::D3D12_CreateFeature()(
        commandList, NVSDK_NGX_Feature_RayReconstruction, parameters, &_p_gazeRoiRrHandle);
    if (result != NVSDK_NGX_Result_Success)
    {
        LOG_ERROR("[GROI_RR] private Ray Reconstruction create failed result={:X} {}x{} -> {}x{}",
                  static_cast<uint32_t>(result), inputRect.width, inputRect.height,
                  outputRect.width, outputRect.height);
        _p_gazeRoiRrHandle = nullptr;
        _gazeRoiRrHandle = {};
        return false;
    }
    _gazeRoiHandleInputRect = inputRect;
    _gazeRoiHandleOutputRect = outputRect;
    _gazeRoiHandleSignature = signature;
    _gazeRoiHandleWasCreated = true;
    LOG_INFO("[GROI_RR] created private zero-based RR handle id={} {}x{} -> {}x{}",
             _p_gazeRoiRrHandle->Id, inputRect.width, inputRect.height, outputRect.width, outputRect.height);
    return true;
}

void DLSSDFeatureDx12::LogGazeRoiNativeResources(NVSDK_NGX_Parameter* parameters)
{
    if (parameters == nullptr)
        return;

    const std::pair<const char*, const char*> resourceKeys[] = {
        { "color", NVSDK_NGX_Parameter_Color },
        { "depth", NVSDK_NGX_Parameter_Depth },
        { "motionVectors", NVSDK_NGX_Parameter_MotionVectors },
        { "specularMotionVectors", NVSDK_NGX_Parameter_GBuffer_SpecularMvec },
        { "reflectionMotionVectors", NVSDK_NGX_Parameter_MotionVectorsReflection },
        { "motionVectors3D", NVSDK_NGX_Parameter_MotionVectors3D },
        { "highResDepth", NVSDK_NGX_Parameter_DepthHighRes },
        { "disocclusionMask", NVSDK_NGX_Parameter_GBuffer_DisocclusionMask },
        { "biasCurrentColor", NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask },
        { "particleMask", NVSDK_NGX_Parameter_IsParticleMask },
        { "animatedTextureMask", NVSDK_NGX_Parameter_AnimatedTextureMask },
        { "transparencyMask", NVSDK_NGX_Parameter_TransparencyMask },
        { "transparencyLayer", NVSDK_NGX_Parameter_DLSS_TransparencyLayer },
        { "transparencyLayerOpacity", NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity },
        { "transparencyLayerMotionVectors", NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs },
    };

    std::ostringstream signature;
    for (const auto& [label, key] : resourceKeys)
    {
        ID3D12Resource* resource = GetNgxResource(parameters, key);
        if (resource == nullptr)
            continue;
        const auto desc = resource->GetDesc();
        signature << ' ' << label << "=0x" << std::hex << reinterpret_cast<uintptr_t>(resource) << std::dec
                  << '(' << desc.Width << 'x' << desc.Height << ",fmt" << static_cast<uint32_t>(desc.Format)
                  << ')';
    }

    signature << " guides=";
    for (const auto& guide : kRrGuideSubrects)
    {
        ID3D12Resource* resource = GetNgxResource(parameters, guide.resourceKey);
        if (resource == nullptr)
            continue;
        const auto desc = resource->GetDesc();
        uint32_t baseX = 0;
        uint32_t baseY = 0;
        const bool hasX = TryGetNgxUint(parameters, guide.baseXKey, baseX);
        const bool hasY = TryGetNgxUint(parameters, guide.baseYKey, baseY);
        signature << ' ' << guide.label << "=0x" << std::hex << reinterpret_cast<uintptr_t>(resource) << std::dec
                  << '(' << desc.Width << 'x' << desc.Height << ",base=";
        if (hasX && hasY)
            signature << baseX << ',' << baseY;
        else
            signature << "missing";
        signature << ')';
    }

    const std::string current = signature.str();
    if (current == _gazeRoiNativeResourceSignature)
        return;
    _gazeRoiNativeResourceSignature = current;
    LOG_INFO("[GROI_RR_NATIVE]{}", current);
}

bool DLSSDFeatureDx12::TryEvaluateGazeRoi(ID3D12GraphicsCommandList* commandList,
                                           NVSDK_NGX_Parameter* parameters,
                                           NVSDK_NGX_Result& result)
{
    result = NVSDK_NGX_Result_Fail;
    ID3D12Resource* color = GetNgxResource(parameters, NVSDK_NGX_Parameter_Color);
    ID3D12Resource* depth = GetNgxResource(parameters, NVSDK_NGX_Parameter_Depth);
    ID3D12Resource* motionVectors = GetNgxResource(parameters, NVSDK_NGX_Parameter_MotionVectors);
    ID3D12Resource* output = GetNgxResource(parameters, NVSDK_NGX_Parameter_Output);
    if (commandList == nullptr || color == nullptr || depth == nullptr || motionVectors == nullptr || output == nullptr)
    {
        LOG_ERROR("[GROI_RR] missing required Color, Depth, MotionVectors, or Output");
        return false;
    }
    LogGazeRoiNativeResources(parameters);

    UpdateGazePoint();
    const auto* streamlineContext = GazeRoiStreamlineContext::Current();
    GazeRoiRect outputRect {};
    GazeRoiRect inputRect {};
    if (streamlineContext != nullptr && streamlineContext->active)
    {
        const auto makeRelativeRect = [](const GazeRoiStreamlineRect& original,
                                         const GazeRoiStreamlineRect& local, GazeRoiRect& rect)
        {
            if (original.width == 0 || original.height == 0 || local.width == 0 || local.height == 0 ||
                local.x < original.x || local.y < original.y ||
                static_cast<uint64_t>(local.x) + local.width > static_cast<uint64_t>(original.x) + original.width ||
                static_cast<uint64_t>(local.y) + local.height > static_cast<uint64_t>(original.y) + original.height)
                return false;
            rect = { local.x - original.x, local.y - original.y, local.width, local.height };
            return true;
        };
        if (!makeRelativeRect(streamlineContext->outputOriginal, streamlineContext->outputLocal, outputRect) ||
            !makeRelativeRect(streamlineContext->colorOriginal, streamlineContext->colorLocal, inputRect))
        {
            LOG_ERROR("[GROI_RR] invalid Streamline Color or Output extent contract");
            return false;
        }
    }
    else if (!BuildGazeRoiRects(outputRect, inputRect))
    {
        return false;
    }

    const uint32_t flags = GetNgxValue<uint32_t>(parameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                                                  static_cast<uint32_t>(GetFeatureFlags()));
    const bool lowResolutionMv = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    GazeRoiRect mvRect = lowResolutionMv ? inputRect : outputRect;
    if (streamlineContext != nullptr && streamlineContext->active)
    {
        const auto& original = streamlineContext->motionVectorsOriginal;
        const auto& local = streamlineContext->motionVectorsLocal;
        if (local.x < original.x || local.y < original.y || local.width == 0 || local.height == 0)
        {
            LOG_ERROR("[GROI_RR] invalid Streamline MotionVectors extent contract");
            return false;
        }
        mvRect = { local.x - original.x, local.y - original.y, local.width, local.height };
    }
    const uint32_t parameterColorBaseX = GetNgxValue<uint32_t>(
        parameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0);
    const uint32_t parameterColorBaseY = GetNgxValue<uint32_t>(
        parameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0);
    const uint32_t parameterDepthBaseX = GetNgxValue<uint32_t>(
        parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0);
    const uint32_t parameterDepthBaseY = GetNgxValue<uint32_t>(
        parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0);
    const uint32_t parameterMvBaseX = GetNgxValue<uint32_t>(
        parameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0);
    const uint32_t parameterMvBaseY = GetNgxValue<uint32_t>(
        parameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0);
    const uint32_t parameterOutputBaseX = GetNgxValue<uint32_t>(
        parameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);
    const uint32_t parameterOutputBaseY = GetNgxValue<uint32_t>(
        parameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0);
    uint32_t fullColorBaseX = parameterColorBaseX;
    uint32_t fullColorBaseY = parameterColorBaseY;
    uint32_t fullDepthBaseX = parameterDepthBaseX;
    uint32_t fullDepthBaseY = parameterDepthBaseY;
    uint32_t fullMvBaseX = parameterMvBaseX;
    uint32_t fullMvBaseY = parameterMvBaseY;
    uint32_t outputBaseX = parameterOutputBaseX;
    uint32_t outputBaseY = parameterOutputBaseY;
    uint32_t roiColorX = 0, roiColorY = 0, roiDepthX = 0, roiDepthY = 0, roiMvX = 0, roiMvY = 0;
    if (streamlineContext != nullptr && streamlineContext->active)
    {
        fullColorBaseX = streamlineContext->colorOriginal.x;
        fullColorBaseY = streamlineContext->colorOriginal.y;
        fullDepthBaseX = streamlineContext->depthOriginal.x;
        fullDepthBaseY = streamlineContext->depthOriginal.y;
        fullMvBaseX = streamlineContext->motionVectorsOriginal.x;
        fullMvBaseY = streamlineContext->motionVectorsOriginal.y;
        outputBaseX = streamlineContext->outputOriginal.x;
        outputBaseY = streamlineContext->outputOriginal.y;
        roiColorX = streamlineContext->colorLocal.x;
        roiColorY = streamlineContext->colorLocal.y;
        roiDepthX = streamlineContext->depthLocal.x;
        roiDepthY = streamlineContext->depthLocal.y;
        roiMvX = streamlineContext->motionVectorsLocal.x;
        roiMvY = streamlineContext->motionVectorsLocal.y;
    }
    else if (!AddBase(parameterColorBaseX, inputRect.x, roiColorX) ||
             !AddBase(parameterColorBaseY, inputRect.y, roiColorY) ||
             !AddBase(parameterDepthBaseX, inputRect.x, roiDepthX) ||
             !AddBase(parameterDepthBaseY, inputRect.y, roiDepthY) ||
             !AddBase(parameterMvBaseX, mvRect.x, roiMvX) ||
             !AddBase(parameterMvBaseY, mvRect.y, roiMvY))
    {
        LOG_ERROR("[GROI_RR] ROI subrect base overflow");
        return false;
    }
    const uint32_t depthRoiWidth = streamlineContext != nullptr && streamlineContext->active
                                       ? streamlineContext->depthLocal.width
                                       : inputRect.width;
    const uint32_t depthRoiHeight = streamlineContext != nullptr && streamlineContext->active
                                        ? streamlineContext->depthLocal.height
                                        : inputRect.height;
    const uint32_t fullRenderWidth = streamlineContext != nullptr && streamlineContext->active
                                         ? streamlineContext->colorOriginal.width
                                         : RenderWidth();
    const uint32_t fullRenderHeight = streamlineContext != nullptr && streamlineContext->active
                                          ? streamlineContext->colorOriginal.height
                                          : RenderHeight();
    const uint32_t fullOutputWidth = streamlineContext != nullptr && streamlineContext->active
                                         ? streamlineContext->outputOriginal.width
                                         : TargetWidth();
    const uint32_t fullOutputHeight = streamlineContext != nullptr && streamlineContext->active
                                          ? streamlineContext->outputOriginal.height
                                          : TargetHeight();
    const uint32_t fullMvWidth = streamlineContext != nullptr && streamlineContext->active
                                     ? streamlineContext->motionVectorsOriginal.width
                                     : (lowResolutionMv ? RenderWidth() : TargetWidth());
    const uint32_t fullMvHeight = streamlineContext != nullptr && streamlineContext->active
                                      ? streamlineContext->motionVectorsOriginal.height
                                      : (lowResolutionMv ? RenderHeight() : TargetHeight());
    if (
        !ResourceRectFits(color, roiColorX, roiColorY, inputRect.width, inputRect.height) ||
        !ResourceRectFits(depth, roiDepthX, roiDepthY, depthRoiWidth, depthRoiHeight) ||
        !ResourceRectFits(motionVectors, roiMvX, roiMvY, mvRect.width, mvRect.height) ||
        !ResourceRectFits(output, outputBaseX, outputBaseY, fullOutputWidth, fullOutputHeight))
    {
        LOG_ERROR("[GROI_RR] ROI subrect contract is out of bounds");
        return false;
    }

    const float mvScaleX = GetNgxValue<float>(parameters, NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    const float mvScaleY = GetNgxValue<float>(parameters, NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    if (!std::isfinite(mvScaleX) || !std::isfinite(mvScaleY) || std::abs(mvScaleX) < 0.00001f ||
        std::abs(mvScaleY) < 0.00001f)
    {
        LOG_ERROR("[GROI_RR] invalid motion-vector scale {},{}", mvScaleX, mvScaleY);
        return false;
    }

    if (_gazeRoi == nullptr)
        _gazeRoi = std::make_unique<GazeRoi_Dx12>("GazeRoiRR", Device);
    if (_gazeRoiMvPatch == nullptr)
        _gazeRoiMvPatch = std::make_unique<GazeRoiMvPatch_Dx12>("GazeRoiRRMv", Device);
    if (_peripheralDenoiser == nullptr)
        _peripheralDenoiser = std::make_unique<RayReconstructionPeripheralDenoiserDx12>(
            "GazeRoiRR Peripheral", Device);

    const uint32_t denoiserWidth = (fullRenderWidth + 1U) / 2U;
    const uint32_t denoiserHeight = (fullRenderHeight + 1U) / 2U;
    const bool rawColorBypass = Config::Instance()->DLSSDRawColorBypass.value_or_default();
    if (_gazeRoi == nullptr || !_gazeRoi->IsInit() ||
        !_gazeRoi->CreateDlssOutputResource(Device, output, outputRect.width, outputRect.height,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        _gazeRoiMvPatch == nullptr || !_gazeRoiMvPatch->IsInit() ||
        !_gazeRoiMvPatch->CreatePatchedResource(Device, motionVectors, mvRect.width, mvRect.height,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        _peripheralDenoiser == nullptr || !_peripheralDenoiser->IsInit() ||
        !_peripheralDenoiser->EnsureResources(
            denoiserWidth, denoiserHeight, false,
            static_cast<uint32_t>(std::clamp(
                Config::Instance()->DLSSDPeripheralDenoiserSpatialPasses.value_or_default(), 0, 3))) ||
        (!rawColorBypass && !EnsureGazeRoiRrHandle(commandList, parameters, outputRect, inputRect)))
    {
        LOG_ERROR("[GROI_RR] private resources or handle are unavailable");
        return false;
    }

    ResTrack_Dx12::EnsureQueueHook(Device);
    uint32_t frameSlot = 0;
    if (!GazeRoiFrameSync::Acquire(commandList, frameSlot))
        return false;

    const bool gameReset = GetNgxValue<int>(parameters, NVSDK_NGX_Parameter_Reset, 0) != 0;
    RayReconstructionDenoiserConstants denoiser {};
    denoiser.sourceWidth = fullRenderWidth;
    denoiser.sourceHeight = fullRenderHeight;
    denoiser.denoiserWidth = denoiserWidth;
    denoiser.denoiserHeight = denoiserHeight;
    denoiser.colorBaseX = fullColorBaseX;
    denoiser.colorBaseY = fullColorBaseY;
    denoiser.depthBaseX = fullDepthBaseX;
    denoiser.depthBaseY = fullDepthBaseY;
    denoiser.motionVectorBaseX = fullMvBaseX;
    denoiser.motionVectorBaseY = fullMvBaseY;
    denoiser.motionVectorWidth = fullMvWidth;
    denoiser.motionVectorHeight = fullMvHeight;
    denoiser.motionVectorScaleX = mvScaleX;
    denoiser.motionVectorScaleY = mvScaleY;
    denoiser.jitterOffsetX = GetNgxValue<float>(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    denoiser.jitterOffsetY = GetNgxValue<float>(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    denoiser.previousJitterOffsetX = _gazePreviousJitterOffsetX;
    denoiser.previousJitterOffsetY = _gazePreviousJitterOffsetY;
    denoiser.motionVectorsLowResolution = lowResolutionMv ? 1U : 0U;
    denoiser.motionVectorsJittered = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0 ? 1U : 0U;
    denoiser.resetHistory = gameReset ? 1U : 0U;
    denoiser.maxHistory = static_cast<uint32_t>(std::clamp(
        Config::Instance()->DLSSDPeripheralDenoiserMaxHistory.value_or_default(), 2, 32));
    denoiser.spatialPasses = static_cast<uint32_t>(std::clamp(
        Config::Instance()->DLSSDPeripheralDenoiserSpatialPasses.value_or_default(), 0, 3));
    denoiser.spatialRadius = std::clamp(
        Config::Instance()->DLSSDPeripheralDenoiserSpatialRadius.value_or_default(), 0.5f, 3.0f);
    if (!_peripheralDenoiser->Dispatch(commandList, color, depth, motionVectors, nullptr, denoiser, frameSlot) ||
        _peripheralDenoiser->FilteredOutput() == nullptr)
    {
        LOG_ERROR("[GROI_RR] peripheral denoiser dispatch failed");
        return false;
    }

    const GazeRoiRect& previousMvRect = _gazePreviousMvRect;
    const bool resetPrivate = gameReset || _gazeRoiHandleWasCreated || _gazeRoiRawColorBypassWasEnabled ||
                              !_gazeHasPreviousRect ||
                              !RectsOverlap(mvRect, previousMvRect);
    float rawOffsetX = 0.0f;
    float rawOffsetY = 0.0f;
    if (!resetPrivate)
    {
        rawOffsetX = static_cast<float>(static_cast<int32_t>(mvRect.x) -
                                        static_cast<int32_t>(previousMvRect.x)) / mvScaleX;
        rawOffsetY = static_cast<float>(static_cast<int32_t>(mvRect.y) -
                                        static_cast<int32_t>(previousMvRect.y)) / mvScaleY;
    }

    GazeRoiMvConstants mvConstants {};
    mvConstants.width = static_cast<int32_t>(mvRect.width);
    mvConstants.height = static_cast<int32_t>(mvRect.height);
    mvConstants.sourceBaseX = static_cast<int32_t>(roiMvX);
    mvConstants.sourceBaseY = static_cast<int32_t>(roiMvY);
    mvConstants.rawOffsetX = rawOffsetX;
    mvConstants.rawOffsetY = rawOffsetY;
    _gazeRoiMvPatch->SetPatchedState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!_gazeRoiMvPatch->Dispatch(commandList, motionVectors, mvConstants, frameSlot))
        return false;
    _gazeRoiMvPatch->SetPatchedState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    _gazeRoi->SetDlssOutputState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (rawColorBypass)
    {
        GazeRoiColorConstants colorConstants {};
        colorConstants.sourceWidth = static_cast<int32_t>(inputRect.width);
        colorConstants.sourceHeight = static_cast<int32_t>(inputRect.height);
        colorConstants.sourceBaseX = static_cast<int32_t>(roiColorX);
        colorConstants.sourceBaseY = static_cast<int32_t>(roiColorY);
        colorConstants.outputWidth = static_cast<int32_t>(outputRect.width);
        colorConstants.outputHeight = static_cast<int32_t>(outputRect.height);
        if (!_gazeRoi->DispatchCurrentColorPointBypass(commandList, color, colorConstants, frameSlot))
            return false;
        result = NVSDK_NGX_Result_Success;
    }
    else
    {
        if (Config::Instance()->GazeRoiOutputClearDebug.value_or_default())
        {
            const GazeRoiRect clear { 0, 0, outputRect.width, outputRect.height };
            if (!_gazeRoi->ClearDlssOutputRectMagenta(commandList, clear, frameSlot))
                return false;
        }
        ScopedNgxOverrides scope(parameters);
        const bool guideContract = OverrideRrGuideSubrects(scope, parameters, roiColorX, roiColorY);
        const bool evaluateContract =
            scope.Override(NVSDK_NGX_Parameter_Width, inputRect.width) &&
            scope.Override(NVSDK_NGX_Parameter_Height, inputRect.height) &&
            scope.Override(NVSDK_NGX_Parameter_OutWidth, outputRect.width) &&
            scope.Override(NVSDK_NGX_Parameter_OutHeight, outputRect.height) &&
            scope.Override(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1) &&
            scope.Override(NVSDK_NGX_Parameter_Color, color) &&
            scope.Override(NVSDK_NGX_Parameter_Depth, depth) &&
            scope.Override(NVSDK_NGX_Parameter_MotionVectors, _gazeRoiMvPatch->PatchedMotionVectors()) &&
            scope.Override(NVSDK_NGX_Parameter_Output, _gazeRoi->DlssOutput()) &&
            scope.Override(NVSDK_NGX_Parameter_Reset, resetPrivate ? 1 : 0) &&
            scope.Override(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, roiColorX) &&
            scope.Override(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, roiColorY) &&
            scope.Override(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, roiDepthX) &&
            scope.Override(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, roiDepthY) &&
            scope.Override(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0U) &&
            scope.Override(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0U) &&
            scope.Override(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0U) &&
            scope.Override(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0U) &&
            scope.Override(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, inputRect.width) &&
            scope.Override(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, inputRect.height) &&
            guideContract;
        if (!evaluateContract)
        {
            LOG_ERROR("[GROI_RR] native Evaluate parameter table is missing a required ROI key");
            _gazeHasPreviousRect = false;
            return false;
        }
        result = NVNGXProxy::D3D12_EvaluateFeature()(commandList, _p_gazeRoiRrHandle, parameters, nullptr);
        if (result != NVSDK_NGX_Result_Success)
        {
            LOG_ERROR("[GROI_RR] private Evaluate failed result={:X}", static_cast<uint32_t>(result));
            _gazeHasPreviousRect = false;
            return true;
        }
    }

    _gazeRoi->SetDlssOutputState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    GazeRoiConstants composite {};
    composite.srcWidth = static_cast<int32_t>(denoiserWidth);
    composite.srcHeight = static_cast<int32_t>(denoiserHeight);
    composite.srcTextureWidth = static_cast<int32_t>(denoiserWidth);
    composite.srcTextureHeight = static_cast<int32_t>(denoiserHeight);
    composite.dstWidth = static_cast<int32_t>(fullOutputWidth);
    composite.dstHeight = static_cast<int32_t>(fullOutputHeight);
    composite.dstBaseX = static_cast<int32_t>(outputBaseX);
    composite.dstBaseY = static_cast<int32_t>(outputBaseY);
    composite.roiX = static_cast<int32_t>(outputRect.x);
    composite.roiY = static_cast<int32_t>(outputRect.y);
    composite.roiWidth = static_cast<int32_t>(outputRect.width);
    composite.roiHeight = static_cast<int32_t>(outputRect.height);
    composite.featherPx = Config::Instance()->GazeRoiFeatherPx.value_or_default();
    composite.debugBorderPx = Config::Instance()->GazeRoiDebugBorder.value_or_default() ? 2 : 0;
    composite.motionVectorDebugView = Config::Instance()->GazeRoiMotionVectorDebugView.value_or_default() ? 1 : 0;
    composite.motionVectorWidth = static_cast<int32_t>(mvRect.width);
    composite.motionVectorHeight = static_cast<int32_t>(mvRect.height);
    composite.motionVectorScaleX = mvScaleX;
    composite.motionVectorScaleY = mvScaleY;
    GazeRoiDepthDebugConstants depthDebugConstants {};
    ID3D12Resource* depthDebugResource = nullptr;
    if (Config::Instance()->DLSSDDepthDebugView.value_or_default())
    {
        const auto depthDesc = depth->GetDesc();
        depthDebugResource = depth;
        depthDebugConstants.depthWidth = static_cast<int32_t>(depthRoiWidth);
        depthDebugConstants.depthHeight = static_cast<int32_t>(depthRoiHeight);
        depthDebugConstants.depthTextureWidth = static_cast<int32_t>(depthDesc.Width);
        depthDebugConstants.depthTextureHeight = static_cast<int32_t>(depthDesc.Height);
        depthDebugConstants.depthBaseX = static_cast<int32_t>(roiDepthX);
        depthDebugConstants.depthBaseY = static_cast<int32_t>(roiDepthY);
        depthDebugConstants.dstWidth = static_cast<int32_t>(fullOutputWidth);
        depthDebugConstants.dstHeight = static_cast<int32_t>(fullOutputHeight);
        depthDebugConstants.dstBaseX = static_cast<int32_t>(outputBaseX);
        depthDebugConstants.dstBaseY = static_cast<int32_t>(outputBaseY);
    }
    if (!_gazeRoi->DispatchComposite(commandList, _peripheralDenoiser->FilteredOutput(),
                                      _gazeRoiMvPatch->PatchedMotionVectors(), output, composite, frameSlot,
                                      depthDebugResource, depthDebugConstants))
    {
        LOG_ERROR("[GROI_RR] final feather composite failed");
        result = NVSDK_NGX_Result_Fail;
        return true;
    }

    _gazePreviousInputRect = inputRect;
    _gazePreviousOutputRect = outputRect;
    _gazePreviousMvRect = mvRect;
    _gazeHasPreviousRect = true;
    _gazeRoiRawColorBypassWasEnabled = rawColorBypass;
    _gazePreviousJitterOffsetX = denoiser.jitterOffsetX;
    _gazePreviousJitterOffsetY = denoiser.jitterOffsetY;
    LOG_DEBUG("[GROI_RR] roi={}x{}+{},{} input={}x{}+{},{} denoiser={}x{} mvOffset={},{} reset={} bypass={} depthDebug={}",
              outputRect.width, outputRect.height, outputRect.x, outputRect.y, inputRect.width, inputRect.height,
              inputRect.x, inputRect.y, denoiserWidth, denoiserHeight, rawOffsetX, rawOffsetY, resetPrivate,
              rawColorBypass, depthDebugResource != nullptr);
    return true;
}

bool DLSSDFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (!_moduleLoaded)
    {
        LOG_ERROR("nvngx.dll or _nvngx.dll is not loaded!");
        return false;
    }

    ProcessEvaluateParams(InParameters);

    if (Config::Instance()->GazeRoiEnabled.value_or_default() && !kGazeRoiRrTemporarilyDisabled)
    {
        NVSDK_NGX_Result gazeResult = NVSDK_NGX_Result_Fail;
        const bool attempted = TryEvaluateGazeRoi(InCommandList, InParameters, gazeResult);
        if (attempted && gazeResult == NVSDK_NGX_Result_Success)
        {
            _frameCount++;
            return true;
        }
        LOG_ERROR("[GROI_RR] replacement failed attempted={} result={:X}; full-frame RR was not evaluated",
                  attempted, static_cast<uint32_t>(gazeResult));
        _gazeHasPreviousRect = false;
        return false;
    }
    _gazeHasPreviousRect = false;
    _gazeRoiRawColorBypassWasEnabled = false;

    const bool rawColorBypassEnabled = !Config::Instance()->GazeRoiEnabled.value_or_default() &&
                                       Config::Instance()->DLSSDRawColorBypass.value_or_default();
    const bool peripheralDenoiserEnabled = !Config::Instance()->GazeRoiEnabled.value_or_default() &&
                                           Config::Instance()->DLSSDPeripheralDenoiser.value_or_default();
    if (rawColorBypassEnabled)
    {
        // Raw Color is a mutually exclusive baseline. Never resume an older temporal history after it.
        if (_peripheralDenoiserWasEnabled)
        {
            _peripheralDenoiserWasEnabled = false;
            _previousJitterOffsetX = 0.0f;
            _previousJitterOffsetY = 0.0f;
            if (_peripheralDenoiser != nullptr)
                _peripheralDenoiser->ResetHistory();
        }

        ID3D12Resource* sourceColor = nullptr;
        ID3D12Resource* output = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_Color, &sourceColor) != NVSDK_NGX_Result_Success ||
            InParameters->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success ||
            sourceColor == nullptr || output == nullptr || sourceColor == output)
        {
            LOG_ERROR("[DLSSD_DEBUG] RawColorBypass requires distinct Color and Output resources");
            return false;
        }

        if (_rawColorBypass == nullptr)
            _rawColorBypass = std::make_unique<RayReconstructionBypassDx12>("DLSSD Raw Color Bypass", Device);
        if (_rawColorBypass == nullptr || !_rawColorBypass->IsInit())
        {
            LOG_ERROR("[DLSSD_DEBUG] RawColorBypass shader is unavailable");
            return false;
        }

        RayReconstructionBypassConstants constants {};
        constants.sourceWidth = RenderWidth();
        constants.sourceHeight = RenderHeight();
        constants.outputWidth = TargetWidth();
        constants.outputHeight = TargetHeight();
        InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &constants.sourceBaseX);
        InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &constants.sourceBaseY);
        InParameters->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, &constants.outputBaseX);
        InParameters->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, &constants.outputBaseY);

        uint32_t frameSlot = 0;
        if (!GazeRoiFrameSync::Acquire(InCommandList, frameSlot))
        {
            LOG_ERROR("[DLSSD_DEBUG] RawColorBypass could not acquire a fence-safe descriptor slot");
            return false;
        }

        if (!_rawColorBypass->Dispatch(InCommandList, sourceColor, output, constants, frameSlot))
        {
            LOG_ERROR("[DLSSD_DEBUG] RawColorBypass dispatch failed");
            return false;
        }

        if (!_rawColorBypassWasEnabled)
        {
            const auto sourceDesc = sourceColor->GetDesc();
            const auto outputDesc = output->GetDesc();
            LOG_WARN("[DLSSD_DEBUG] RawColorBypass enabled; Ray Reconstruction Evaluate skipped");
            LOG_INFO("[DLSSD_DEBUG] Color resource={}x{} format={} base={},{} active={}x{}",
                     sourceDesc.Width, sourceDesc.Height, static_cast<uint32_t>(sourceDesc.Format),
                     constants.sourceBaseX, constants.sourceBaseY, constants.sourceWidth, constants.sourceHeight);
            LOG_INFO("[DLSSD_DEBUG] Output resource={}x{} format={} base={},{} active={}x{} pointSample=true",
                     outputDesc.Width, outputDesc.Height, static_cast<uint32_t>(outputDesc.Format),
                     constants.outputBaseX, constants.outputBaseY, constants.outputWidth, constants.outputHeight);
        }

        _rawColorBypassWasEnabled = true;
        _frameCount++;
        return true;
    }

    if (_rawColorBypassWasEnabled)
    {
        LOG_INFO("[DLSSD_DEBUG] RawColorBypass disabled; normal Ray Reconstruction Evaluate restored");
        _rawColorBypassWasEnabled = false;
    }

    if (peripheralDenoiserEnabled)
    {
        ID3D12Resource* color = GetNgxResource(InParameters, NVSDK_NGX_Parameter_Color);
        ID3D12Resource* depth = GetNgxResource(InParameters, NVSDK_NGX_Parameter_Depth);
        ID3D12Resource* motionVectors = GetNgxResource(InParameters, NVSDK_NGX_Parameter_MotionVectors);
        ID3D12Resource* output = GetNgxResource(InParameters, NVSDK_NGX_Parameter_Output);
        if (color == nullptr || depth == nullptr || motionVectors == nullptr || output == nullptr)
        {
            LOG_ERROR("[DLSSD_DENOISER] requires Color, Depth, MotionVectors, and Output");
            return false;
        }

        const uint32_t sourceWidth = RenderWidth();
        const uint32_t sourceHeight = RenderHeight();
        const uint32_t denoiserWidth = (sourceWidth + 1U) / 2U;
        const uint32_t denoiserHeight = (sourceHeight + 1U) / 2U;
        const uint32_t outputWidth = TargetWidth();
        const uint32_t outputHeight = TargetHeight();
        const unsigned int featureFlags = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<unsigned int>(GetFeatureFlags()));
        const bool lowResolutionMv = (featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
        const uint32_t motionVectorWidth = lowResolutionMv ? sourceWidth : outputWidth;
        const uint32_t motionVectorHeight = lowResolutionMv ? sourceHeight : outputHeight;
        const uint32_t debugView = static_cast<uint32_t>(std::clamp(
            Config::Instance()->DLSSDPeripheralDenoiserDebugView.value_or_default(), 0, 4));

        RayReconstructionDenoiserConstants constants {};
        constants.sourceWidth = sourceWidth;
        constants.sourceHeight = sourceHeight;
        constants.denoiserWidth = denoiserWidth;
        constants.denoiserHeight = denoiserHeight;
        constants.colorBaseX = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0);
        constants.colorBaseY = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0);
        constants.depthBaseX = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0);
        constants.depthBaseY = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0);
        constants.motionVectorBaseX = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0);
        constants.motionVectorBaseY = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0);
        constants.motionVectorWidth = motionVectorWidth;
        constants.motionVectorHeight = motionVectorHeight;
        constants.outputWidth = outputWidth;
        constants.outputHeight = outputHeight;
        constants.outputBaseX = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);
        constants.outputBaseY = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0);
        constants.motionVectorScaleX = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
        constants.motionVectorScaleY = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
        constants.jitterOffsetX = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
        constants.jitterOffsetY = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
        constants.previousJitterOffsetX = _previousJitterOffsetX;
        constants.previousJitterOffsetY = _previousJitterOffsetY;
        constants.motionVectorsLowResolution = lowResolutionMv ? 1U : 0U;
        constants.motionVectorsJittered =
            (featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0 ? 1U : 0U;
        constants.resetHistory = GetNgxValue<int>(InParameters, NVSDK_NGX_Parameter_Reset, 0) != 0 ? 1U : 0U;
        constants.debugView = debugView;
        constants.maxHistory = static_cast<uint32_t>(std::clamp(
            Config::Instance()->DLSSDPeripheralDenoiserMaxHistory.value_or_default(), 2, 32));
        constants.spatialPasses = static_cast<uint32_t>(std::clamp(
            Config::Instance()->DLSSDPeripheralDenoiserSpatialPasses.value_or_default(), 0, 3));
        constants.spatialRadius = std::clamp(
            Config::Instance()->DLSSDPeripheralDenoiserSpatialRadius.value_or_default(), 0.5f, 3.0f);

        if (!std::isfinite(constants.motionVectorScaleX) || !std::isfinite(constants.motionVectorScaleY) ||
            std::abs(constants.motionVectorScaleX) < 0.00001f || std::abs(constants.motionVectorScaleY) < 0.00001f)
        {
            LOG_ERROR("[DLSSD_DENOISER] invalid MV scale {}, {}", constants.motionVectorScaleX,
                      constants.motionVectorScaleY);
            return false;
        }

        if (_peripheralDenoiser == nullptr)
            _peripheralDenoiser = std::make_unique<RayReconstructionPeripheralDenoiserDx12>(
                "DLSSD Peripheral Denoiser", Device);
        if (_peripheralDenoiser == nullptr || !_peripheralDenoiser->IsInit() ||
            !_peripheralDenoiser->EnsureResources(
                denoiserWidth, denoiserHeight, debugView != 0,
                debugView != 0 ? 0U : static_cast<uint32_t>(std::clamp(
                    Config::Instance()->DLSSDPeripheralDenoiserSpatialPasses.value_or_default(), 0, 3))))
        {
            LOG_ERROR("[DLSSD_DENOISER] shader or history resources are unavailable");
            return false;
        }

        ResTrack_Dx12::EnsureQueueHook(Device);
        uint32_t frameSlot = 0;
        if (!GazeRoiFrameSync::Acquire(InCommandList, frameSlot) ||
            !_peripheralDenoiser->Dispatch(InCommandList, color, depth, motionVectors, output, constants, frameSlot))
        {
            LOG_ERROR("[DLSSD_DENOISER] dispatch failed");
            return false;
        }

        _previousJitterOffsetX = constants.jitterOffsetX;
        _previousJitterOffsetY = constants.jitterOffsetY;
        if (!_peripheralDenoiserWasEnabled)
        {
            LOG_WARN("[DLSSD_DENOISER] enabled; Ray Reconstruction Evaluate skipped, Color+Depth+MV temporal path active");
            LOG_INFO("[DLSSD_DENOISER] source={}x{} denoiser={}x{} output={}x{} lowResMV={} jitteredMV={} mvScale={},{} "
                     "debugView={} history={} spatialPasses={} spatialRadius={}",
                      sourceWidth, sourceHeight, denoiserWidth, denoiserHeight, outputWidth, outputHeight, lowResolutionMv,
                      constants.motionVectorsJittered != 0, constants.motionVectorScaleX, constants.motionVectorScaleY,
                      debugView, constants.maxHistory, constants.spatialPasses, constants.spatialRadius);
        }
        _peripheralDenoiserWasEnabled = true;
        _frameCount++;
        return true;
    }

    if (_peripheralDenoiserWasEnabled)
    {
        LOG_INFO("[DLSSD_DENOISER] disabled; normal Ray Reconstruction Evaluate restored");
        _peripheralDenoiserWasEnabled = false;
        _previousJitterOffsetX = 0.0f;
        _previousJitterOffsetY = 0.0f;
        if (_peripheralDenoiser != nullptr)
            _peripheralDenoiser->ResetHistory();
    }

    NVSDK_NGX_Result nvResult;
    if (NVNGXProxy::D3D12_EvaluateFeature() != nullptr)
    {

        nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _p_dlssdHandle, InParameters, NULL);

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

DLSSDFeatureDx12::DLSSDFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters),
      DLSSDFeature(InHandleId, InParameters)
{
    if (NVNGXProxy::NVNGXModule() == nullptr)
    {
        LOG_INFO("nvngx.dll not loaded, now loading");
        NVNGXProxy::InitNVNGX();
    }

    LOG_INFO("binding complete!");
}

DLSSDFeatureDx12::~DLSSDFeatureDx12()
{
    GazeRoiInput::Stop();
    if (State::Instance().isShuttingDown)
        return;

    GazeRoiFrameSync::FlushDeferred();
    if (NVNGXProxy::D3D12_ReleaseFeature() != nullptr && _p_gazeRoiRrHandle != nullptr)
        NVNGXProxy::D3D12_ReleaseFeature()(_p_gazeRoiRrHandle);
    if (NVNGXProxy::D3D12_ReleaseFeature() != nullptr && _p_dlssdHandle != nullptr)
        NVNGXProxy::D3D12_ReleaseFeature()(_p_dlssdHandle);
}
