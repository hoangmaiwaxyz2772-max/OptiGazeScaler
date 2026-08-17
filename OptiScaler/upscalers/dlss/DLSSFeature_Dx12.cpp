#include <pch.h>
#include "DLSSFeature_Dx12.h"
#include <dxgi1_4.h>
#include <Config.h>
#include <NVNGX_Parameter.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <resource_tracking/ResTrack_dx12.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <variant>

#pragma comment(lib, "Ws2_32.lib")

namespace
{
constexpr const char* gazeRoiPresetKeys[] = {
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
};

using NgxParameterValue = std::variant<unsigned long long, float, double, unsigned int, int, ID3D11Resource*,
                                       ID3D12Resource*, void*>;

std::string FormatNgxParameterValue(const NgxParameterValue& value)
{
    return std::visit(
        [](const auto& item) -> std::string
        {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_pointer_v<T>)
                return std::format("0x{:X}", reinterpret_cast<uintptr_t>(item));
            else if constexpr (std::is_floating_point_v<T>)
                return std::format("{:.9g}", item);
            else
                return std::format("{}", item);
        },
        value);
}

void TraceNgxParameter(const std::string& phase, const char* operation, const char* key, const char* type,
                       NVSDK_NGX_Result result, const std::string& value)
{
    static std::mutex traceMutex;
    static std::unordered_map<std::string, std::string> lastValues;

    const std::string traceKey = std::format("{}|{}|{}|{}", phase, operation, key, type);
    const std::string traceValue = std::format("result=0x{:X} value={}", static_cast<unsigned int>(result), value);
    {
        std::scoped_lock lock(traceMutex);
        if (const auto it = lastValues.find(traceKey); it != lastValues.end() && it->second == traceValue)
            return;
        lastValues[traceKey] = traceValue;
    }

    LOG_INFO("[GROI_PARAM] phase={} op={} key={} type={} {}", phase, operation, key, type, traceValue);
}

/**
 * Temporarily changes the native game-owned parameter table. NGX receives the
 * original object and therefore retains its expected ABI and implementation;
 * every ROI write is restored when this scope ends.
 */
class NgxParameterScope final
{
  public:
    NgxParameterScope(NVSDK_NGX_Parameter* parameters, std::string phase)
        : _parameters(parameters), _phase(std::move(phase))
    {
    }

    ~NgxParameterScope()
    {
        for (auto it = _restores.rbegin(); it != _restores.rend(); ++it)
            (*it)();
    }

    template <typename T> bool Override(const char* key, T value, const char* type)
    {
        if (_parameters == nullptr || key == nullptr)
            return false;

        T original {};
        if (_parameters->Get(key, &original) != NVSDK_NGX_Result_Success)
        {
            TraceNgxParameter(_phase, "OverrideSkipped", key, type, NVSDK_NGX_Result_Fail, "original-missing");
            return false;
        }

        _restores.emplace_back([parameters = _parameters, key, original, phase = _phase, type]
                               {
                                   parameters->Set(key, original);
                                   TraceNgxParameter(phase, "Restore", key, type, NVSDK_NGX_Result_Success,
                                                     FormatNgxParameterValue(original));
                               });
        _parameters->Set(key, value);
        TraceNgxParameter(_phase, "Override", key, type, NVSDK_NGX_Result_Success,
                          FormatNgxParameterValue(value));
        return true;
    }

  private:
    NVSDK_NGX_Parameter* _parameters = nullptr;
    std::string _phase;
    std::vector<std::function<void()>> _restores;
};

/**
 * A read-through NGX parameter table used only for local diagnostics and the
 * in-process optimal-settings callback. It is never passed into NVIDIA NGX.
 */
class NgxParameterOverlay final : public NVSDK_NGX_Parameter
{
  public:
    NgxParameterOverlay(NVSDK_NGX_Parameter* original, std::string phase)
        : _original(original), _phase(std::move(phase))
    {
    }

    void SetOverride(const char* key, unsigned long long value) { setOverride(key, value); }
    void SetOverride(const char* key, float value) { setOverride(key, value); }
    void SetOverride(const char* key, double value) { setOverride(key, value); }
    void SetOverride(const char* key, unsigned int value) { setOverride(key, value); }
    void SetOverride(const char* key, int value) { setOverride(key, value); }
    void SetOverride(const char* key, ID3D11Resource* value) { setOverride(key, value); }
    void SetOverride(const char* key, ID3D12Resource* value) { setOverride(key, value); }
    void SetOverride(const char* key, void* value) { setOverride(key, value); }

    const std::unordered_map<std::string, NgxParameterValue>& LocalValues() const { return _values; }

    void Set(const char* key, unsigned long long value) override { setLocal(key, value, "ulong"); }
    void Set(const char* key, float value) override { setLocal(key, value, "float"); }
    void Set(const char* key, double value) override { setLocal(key, value, "double"); }
    void Set(const char* key, unsigned int value) override { setLocal(key, value, "uint"); }
    void Set(const char* key, int value) override { setLocal(key, value, "int"); }
    void Set(const char* key, ID3D11Resource* value) override { setLocal(key, value, "d3d11"); }
    void Set(const char* key, ID3D12Resource* value) override { setLocal(key, value, "d3d12"); }
    void Set(const char* key, void* value) override { setLocal(key, value, "void"); }

    NVSDK_NGX_Result Get(const char* key, unsigned long long* value) const override
    {
        return getValue(key, value, "ulong");
    }
    NVSDK_NGX_Result Get(const char* key, float* value) const override { return getValue(key, value, "float"); }
    NVSDK_NGX_Result Get(const char* key, double* value) const override
    {
        return getValue(key, value, "double");
    }
    NVSDK_NGX_Result Get(const char* key, unsigned int* value) const override
    {
        return getValue(key, value, "uint");
    }
    NVSDK_NGX_Result Get(const char* key, int* value) const override { return getValue(key, value, "int"); }
    NVSDK_NGX_Result Get(const char* key, ID3D11Resource** value) const override
    {
        return getValue(key, value, "d3d11");
    }
    NVSDK_NGX_Result Get(const char* key, ID3D12Resource** value) const override
    {
        return getValue(key, value, "d3d12");
    }
    NVSDK_NGX_Result Get(const char* key, void** value) const override { return getValue(key, value, "void"); }

    void Reset() override
    {
        _values.clear();
        for (const auto& [key, value] : _forcedValues)
            _values[key] = value;
        TraceNgxParameter(_phase, "Reset", "*", "table", NVSDK_NGX_Result_Success, "local-only");
    }

  private:
    NVSDK_NGX_Parameter* _original = nullptr;
    std::string _phase;
    std::unordered_map<std::string, NgxParameterValue> _values;
    std::unordered_map<std::string, NgxParameterValue> _forcedValues;

    template <typename T> void setOverride(const char* key, T value)
    {
        if (key == nullptr)
            return;
        _forcedValues[key] = value;
        _values[key] = value;
        TraceNgxParameter(_phase, "Override", key, "value", NVSDK_NGX_Result_Success, FormatNgxParameterValue(value));
    }

    template <typename T> void setLocal(const char* key, T value, const char* type)
    {
        if (key == nullptr)
            return;
        _values[key] = value;
        TraceNgxParameter(_phase, "Set", key, type, NVSDK_NGX_Result_Success, FormatNgxParameterValue(value));
    }

    template <typename T> static bool readLocal(const NgxParameterValue& stored, T& value)
    {
        return std::visit(
            [&value](const auto& item)
            {
                using Source = std::decay_t<decltype(item)>;
                if constexpr (std::is_pointer_v<T>)
                {
                    if constexpr (std::is_same_v<Source, T>)
                    {
                        value = item;
                        return true;
                    }
                    else if constexpr (std::is_same_v<Source, void*>)
                    {
                        value = reinterpret_cast<T>(item);
                        return true;
                    }
                    else if constexpr (std::is_same_v<T, void*> && std::is_pointer_v<Source>)
                    {
                        value = static_cast<void*>(item);
                        return true;
                    }
                    else
                        return false;
                }
                else if constexpr (std::is_arithmetic_v<Source> && std::is_arithmetic_v<T>)
                {
                    value = static_cast<T>(item);
                    return true;
                }
                else
                    return false;
            },
            stored);
    }

    template <typename T> NVSDK_NGX_Result getValue(const char* key, T* value, const char* type) const
    {
        if (key == nullptr || value == nullptr)
            return NVSDK_NGX_Result_Fail;

        NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
        std::string formatted = "<missing>";
        if (const auto it = _values.find(key); it != _values.end() && readLocal(it->second, *value))
        {
            result = NVSDK_NGX_Result_Success;
            formatted = FormatNgxParameterValue(*value);
        }
        else if (_original != nullptr)
        {
            result = _original->Get(key, value);
            if (result == NVSDK_NGX_Result_Success)
                formatted = FormatNgxParameterValue(*value);
        }

        TraceNgxParameter(_phase, "Get", key, type, result, formatted);
        return result;
    }
};

void TraceNgxKnownParameters(NVSDK_NGX_Parameter* parameters, const std::string& phase)
{
    if (parameters == nullptr)
        return;

    NgxParameterOverlay observer(parameters, phase);

    const char* unsignedKeys[] = {
        NVSDK_NGX_Parameter_Width,
        NVSDK_NGX_Parameter_Height,
        NVSDK_NGX_Parameter_OutWidth,
        NVSDK_NGX_Parameter_OutHeight,
        NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
        NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
        NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
        NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X,
        NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y,
        NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        NVSDK_NGX_Parameter_TonemapperType,
        NVSDK_NGX_Parameter_CreationNodeMask,
        NVSDK_NGX_Parameter_VisibilityNodeMask,
    };
    for (const char* key : unsignedKeys)
    {
        unsigned int value = 0;
        observer.Get(key, &value);
    }

    const char* intKeys[] = {
        NVSDK_NGX_Parameter_PerfQualityValue,
        NVSDK_NGX_Parameter_Reset,
        NVSDK_NGX_Parameter_NumFrames,
        NVSDK_NGX_Parameter_RTXValue,
        NVSDK_NGX_Parameter_DLSSMode,
        NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,
        NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis,
        NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis,
        "FreeMemOnReleaseFeature",
        "DLSS.Denoise.Mode",
        "DLSS.Use.Folded.Network",
        "DLSS.Roughness.Mode",
        "DLSS.Use.HW.Depth",
        "Debug",
        "RayReconstruction.Hint.Render.Preset.DLAA",
        "RayReconstruction.Hint.Render.Preset.Quality",
        "RayReconstruction.Hint.Render.Preset.Balanced",
        "RayReconstruction.Hint.Render.Preset.Performance",
        "RayReconstruction.Hint.Render.Preset.UltraPerformance",
        "RayReconstruction.Hint.Render.Preset.UltraQuality",
        "DLSS.Hint.Render.Preset.DLAA",
        "DLSS.Hint.Render.Preset.Quality",
        "DLSS.Hint.Render.Preset.Balanced",
        "DLSS.Hint.Render.Preset.Performance",
        "DLSS.Hint.Render.Preset.UltraPerformance",
        "DLSS.Hint.Render.Preset.UltraQuality",
    };
    for (const char* key : intKeys)
    {
        int value = 0;
        observer.Get(key, &value);
    }

    const char* floatKeys[] = {
        NVSDK_NGX_Parameter_Scale,
        NVSDK_NGX_Parameter_SuperSampling_ScaleFactor,
        NVSDK_NGX_Parameter_MV_Scale_X,
        NVSDK_NGX_Parameter_MV_Scale_Y,
        NVSDK_NGX_Parameter_MV_Offset_X,
        NVSDK_NGX_Parameter_MV_Offset_Y,
        NVSDK_NGX_Parameter_Jitter_Offset_X,
        NVSDK_NGX_Parameter_Jitter_Offset_Y,
        NVSDK_NGX_Parameter_Sharpness,
        NVSDK_NGX_Parameter_Denoise,
        NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,
        NVSDK_NGX_Parameter_DLSS_Pre_Exposure,
        NVSDK_NGX_Parameter_DLSS_Exposure_Scale,
        NVSDK_NGX_EParameter_BlendFactor,
    };
    for (const char* key : floatKeys)
    {
        float value = 0.0f;
        observer.Get(key, &value);
    }

    const char* resourceKeys[] = {
        NVSDK_NGX_Parameter_Color,
        NVSDK_NGX_Parameter_Depth,
        NVSDK_NGX_Parameter_MotionVectors,
        NVSDK_NGX_Parameter_Output,
        NVSDK_NGX_Parameter_ExposureTexture,
        NVSDK_NGX_Parameter_TransparencyMask,
        NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
        NVSDK_NGX_Parameter_RayTracingHitDistance,
        NVSDK_NGX_Parameter_GBuffer_Normals,
        NVSDK_NGX_Parameter_MotionVectors3D,
        NVSDK_NGX_Parameter_DepthHighRes,
        NVSDK_NGX_Parameter_MotionVectorsReflection,
        NVSDK_NGX_Parameter_IsParticleMask,
        NVSDK_NGX_Parameter_AnimatedTextureMask,
        NVSDK_NGX_Parameter_Position_ViewSpace,
        NVSDK_NGX_Parameter_GBuffer_Albedo,
        NVSDK_NGX_Parameter_GBuffer_Roughness,
        NVSDK_NGX_Parameter_GBuffer_Metallic,
        NVSDK_NGX_Parameter_GBuffer_Specular,
        NVSDK_NGX_Parameter_GBuffer_Subsurface,
        NVSDK_NGX_Parameter_GBuffer_ShadingModelId,
        NVSDK_NGX_Parameter_GBuffer_MaterialId,
        NVSDK_NGX_Parameter_GBuffer_DisocclusionMask,
        NVSDK_NGX_Parameter_DLSS_TransparencyLayer,
        NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity,
        NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs,
        NVSDK_NGX_Parameter_DLSS_DisocclusionMask,
    };
    for (const char* key : resourceKeys)
    {
        ID3D12Resource* resource = nullptr;
        if (observer.Get(key, &resource) == NVSDK_NGX_Result_Success && resource != nullptr)
        {
            const auto desc = resource->GetDesc();
            LOG_INFO("[GROI_PARAM_RESOURCE] phase={} key={} resource=0x{:X} {}x{} format={} flags=0x{:X} "
                     "mips={} array={} samples={}",
                     phase, key, reinterpret_cast<uintptr_t>(resource), desc.Width, desc.Height,
                     static_cast<uint32_t>(desc.Format), static_cast<uint32_t>(desc.Flags), desc.MipLevels,
                     desc.DepthOrArraySize, desc.SampleDesc.Count);
        }
    }

    const char* pointerKeys[] = {
        NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback,
        NVSDK_NGX_Parameter_DLSSGetStatsCallback,
        NVSDK_NGX_Parameter_DLSS_INV_VIEW_PROJECTION_MATRIX,
        NVSDK_NGX_Parameter_DLSS_CLIP_TO_PREV_CLIP_MATRIX,
        NVSDK_NGX_EParameter_PreviousOutput,
    };
    for (const char* key : pointerKeys)
    {
        void* value = nullptr;
        observer.Get(key, &value);
    }
}

bool ResourceRectFits(ID3D12Resource* resource, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (resource == nullptr || width == 0 || height == 0)
        return false;

    const auto desc = resource->GetDesc();
    return static_cast<uint64_t>(x) + width <= desc.Width && static_cast<uint64_t>(y) + height <= desc.Height;
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

static uint32_t ScaleRoiDimension(uint32_t outputSize, uint32_t renderExtent, uint32_t targetExtent)
{
    if (targetExtent == 0)
        return 0;

    const uint64_t scaled = static_cast<uint64_t>(outputSize) * renderExtent;
    return static_cast<uint32_t>((scaled + targetExtent / 2) / targetExtent);
}

static uint32_t MapCenteredRectOrigin(uint32_t sourceOrigin, uint32_t sourceSize, uint32_t sourceExtent,
                                      uint32_t targetSize, uint32_t targetExtent)
{
    if (sourceExtent == 0 || targetExtent <= targetSize)
        return 0;

    const double sourceCenter = static_cast<double>(sourceOrigin) + static_cast<double>(sourceSize) * 0.5;
    const double mappedOrigin = sourceCenter * static_cast<double>(targetExtent) /
                                    static_cast<double>(sourceExtent) -
                                static_cast<double>(targetSize) * 0.5;
    return static_cast<uint32_t>(std::clamp<int64_t>(
        static_cast<int64_t>(std::llround(mappedOrigin)), 0, static_cast<int64_t>(targetExtent - targetSize)));
}

enum class GazeRoiMotionVectorMode
{
    Disabled,
    InputDelta,
    InputDeltaReversed,
    InputDeltaUnscaled,
    OutputDelta,
    OutputDeltaReversed,
};

GazeRoiMotionVectorMode GetGazeRoiMotionVectorMode()
{
    const auto mode = Config::Instance()->GazeRoiMotionVectorMode.value_or_default();
    if (mode == "Disabled")
        return GazeRoiMotionVectorMode::Disabled;
    if (mode == "InputDeltaReversed")
        return GazeRoiMotionVectorMode::InputDeltaReversed;
    if (mode == "InputDeltaUnscaled")
        return GazeRoiMotionVectorMode::InputDeltaUnscaled;
    if (mode == "OutputDelta")
        return GazeRoiMotionVectorMode::OutputDelta;
    if (mode == "OutputDeltaReversed")
        return GazeRoiMotionVectorMode::OutputDeltaReversed;
    return GazeRoiMotionVectorMode::InputDelta;
}

const char* GazeRoiMotionVectorModeName(GazeRoiMotionVectorMode mode)
{
    switch (mode)
    {
    case GazeRoiMotionVectorMode::Disabled:
        return "Disabled";
    case GazeRoiMotionVectorMode::InputDeltaReversed:
        return "InputDeltaReversed";
    case GazeRoiMotionVectorMode::InputDeltaUnscaled:
        return "InputDeltaUnscaled";
    case GazeRoiMotionVectorMode::OutputDelta:
        return "OutputDelta";
    case GazeRoiMotionVectorMode::OutputDeltaReversed:
        return "OutputDeltaReversed";
    case GazeRoiMotionVectorMode::InputDelta:
    default:
        return "InputDelta";
    }
}

struct GazeUdpSample
{
    float x = 0.5f;
    float y = 0.5f;
    uint64_t receivedTick = 0;
    bool valid = false;
};

#pragma pack(push, 1)
struct GazeSharedMemorySlot
{
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t seq = 0;
    uint32_t flags = 0;
    double timestampMs = 0.0;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float confidence = 0.0f;
    float reserved0 = 0.0f;
    double sourceTimestampMs = 0.0;
    uint64_t reserved1 = 0;
};
#pragma pack(pop)

static_assert(sizeof(GazeSharedMemorySlot) == 64);

constexpr uint32_t GazeSharedMemoryMagic = 0x315A4745; // "EGZ1", little endian.
constexpr uint32_t GazeSharedMemoryVersion = 1;
constexpr const wchar_t* GazeSharedMemoryName = L"Local\\EyeTracingGazeV1";

ID3D12Resource* GetNgxResource(NVSDK_NGX_Parameter* params, const char* key)
{
    if (params == nullptr)
        return nullptr;

    ID3D12Resource* resource = nullptr;
    if (params->Get(key, &resource) != NVSDK_NGX_Result_Success)
        params->Get(key, reinterpret_cast<void**>(&resource));

    return resource;
}

template <typename T> T GetNgxValue(NVSDK_NGX_Parameter* params, const char* key, T fallback)
{
    T value = fallback;
    if (params != nullptr)
        params->Get(key, &value);
    return value;
}

void AppendGazeResourceContract(std::ostringstream& stream, const char* name, ID3D12Resource* resource)
{
    stream << ' ' << name << '=';
    if (resource == nullptr)
    {
        stream << "null";
        return;
    }

    const auto desc = resource->GetDesc();
    stream << static_cast<const void*>(resource) << ':' << desc.Width << 'x' << desc.Height << ":fmt"
           << static_cast<uint32_t>(desc.Format) << ":flags0x" << std::hex << static_cast<uint32_t>(desc.Flags)
           << std::dec << ":mips" << desc.MipLevels << ":array" << desc.DepthOrArraySize << ":samples"
           << desc.SampleDesc.Count;
}

bool ExtractJsonNumber(const std::string& text, const char* key, double& value)
{
    const std::string quotedKey = std::format("\"{}\"", key);
    const size_t keyPos = text.find(quotedKey);
    if (keyPos == std::string::npos)
        return false;

    const size_t colonPos = text.find(':', keyPos + quotedKey.size());
    if (colonPos == std::string::npos)
        return false;

    const char* begin = text.c_str() + colonPos + 1;
    char* end = nullptr;
    value = std::strtod(begin, &end);
    return end != begin && std::isfinite(value);
}

bool ExtractJsonBool(const std::string& text, const char* key, bool& value)
{
    const std::string quotedKey = std::format("\"{}\"", key);
    const size_t keyPos = text.find(quotedKey);
    if (keyPos == std::string::npos)
        return false;

    const size_t colonPos = text.find(':', keyPos + quotedKey.size());
    if (colonPos == std::string::npos)
        return false;

    const size_t valuePos = text.find_first_not_of(" \t\r\n", colonPos + 1);
    if (valuePos == std::string::npos)
        return false;

    if (text.compare(valuePos, 4, "true") == 0)
    {
        value = true;
        return true;
    }

    if (text.compare(valuePos, 5, "false") == 0)
    {
        value = false;
        return true;
    }

    return false;
}

bool ParseGazeUdpPacket(const char* data, int length, GazeUdpSample& sample)
{
    if (data == nullptr || length <= 0)
        return false;

    const std::string text(data, data + length);

    double x = 0.0;
    double y = 0.0;
    if (!ExtractJsonNumber(text, "x", x) || !ExtractJsonNumber(text, "y", y))
        return false;

    bool valid = true;
    ExtractJsonBool(text, "valid", valid);

    double width = 0.0;
    double height = 0.0;
    const bool hasDimensions = ExtractJsonNumber(text, "width", width) && ExtractJsonNumber(text, "height", height) &&
                               width > 0.0 && height > 0.0;

    if (hasDimensions)
    {
        x /= width;
        y /= height;
    }
    else if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0)
    {
        return false;
    }

    sample.x = std::clamp(static_cast<float>(x), 0.0f, 1.0f);
    sample.y = std::clamp(static_cast<float>(y), 0.0f, 1.0f);
    sample.valid = valid;
    sample.receivedTick = GetTickCount64();
    return true;
}

class GazeUdpReceiver
{
  public:
    static GazeUdpReceiver& Instance()
    {
        static GazeUdpReceiver receiver;
        return receiver;
    }

    bool EnsureStarted(int port)
    {
        port = std::clamp(port, 1024, 65535);

        if (_running.load() && _port == port)
            return true;

        Stop();

        const uint64_t now = GetTickCount64();
        if (_lastFailedPort == port && now - _lastFailureTick < 2000)
            return false;

        WSADATA wsaData {};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            MarkFailure(port);
            LOG_WARN("Gaze ROI UDP failed to initialize Winsock");
            return false;
        }

        SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socketHandle == INVALID_SOCKET)
        {
            WSACleanup();
            MarkFailure(port);
            LOG_WARN("Gaze ROI UDP failed to create socket");
            return false;
        }

        u_long nonBlocking = 1;
        ioctlsocket(socketHandle, FIONBIO, &nonBlocking);

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
        {
            closesocket(socketHandle);
            WSACleanup();
            MarkFailure(port);
            LOG_WARN("Gaze ROI UDP failed to bind 127.0.0.1:{}", port);
            return false;
        }

        _socket = socketHandle;
        _port = port;
        _stop.store(false);
        _running.store(true);
        _thread = std::thread([this]() { ReceiveLoop(); });
        LOG_INFO("Gaze ROI UDP listening on 127.0.0.1:{}", port);
        return true;
    }

    void Stop()
    {
        if (!_running.exchange(false))
            return;

        _stop.store(true);
        if (_socket != INVALID_SOCKET)
        {
            closesocket(_socket);
            _socket = INVALID_SOCKET;
        }

        if (_thread.joinable())
            _thread.join();

        WSACleanup();
    }

    bool TryGetFreshSample(int staleMs, GazeUdpSample& sample)
    {
        std::lock_guard lock(_sampleMutex);
        if (!_sample.valid)
            return false;

        const uint64_t now = GetTickCount64();
        if (now - _sample.receivedTick > static_cast<uint64_t>(std::max(1, staleMs)))
            return false;

        sample = _sample;
        return true;
    }

    ~GazeUdpReceiver() { Stop(); }

  private:
    void ReceiveLoop()
    {
        char buffer[1024] {};
        while (!_stop.load())
        {
            sockaddr_in from {};
            int fromLength = sizeof(from);
            const int received =
                recvfrom(_socket, buffer, static_cast<int>(sizeof(buffer)), 0, reinterpret_cast<sockaddr*>(&from),
                         &fromLength);

            if (received > 0)
            {
                GazeUdpSample sample {};
                if (ParseGazeUdpPacket(buffer, received, sample))
                {
                    std::lock_guard lock(_sampleMutex);
                    _sample = sample;
                }
                continue;
            }

            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAENOTSOCK && !_stop.load())
                Sleep(5);
            else
                Sleep(1);
        }
    }

    void MarkFailure(int port)
    {
        _lastFailedPort = port;
        _lastFailureTick = GetTickCount64();
    }

    std::atomic<bool> _running { false };
    std::atomic<bool> _stop { false };
    SOCKET _socket = INVALID_SOCKET;
    std::thread _thread {};
    std::mutex _sampleMutex {};
    GazeUdpSample _sample {};
    int _port = 0;
    int _lastFailedPort = 0;
    uint64_t _lastFailureTick = 0;
};

class GazeSharedMemoryReceiver
{
  public:
    static GazeSharedMemoryReceiver& Instance()
    {
        static GazeSharedMemoryReceiver receiver;
        return receiver;
    }

    bool EnsureOpened()
    {
        if (_view != nullptr)
            return true;

        const uint64_t now = GetTickCount64();
        if (now - _lastOpenFailureTick < 2000)
            return false;

        _mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, GazeSharedMemoryName);
        if (_mapping == nullptr)
        {
            _lastOpenFailureTick = now;
            return false;
        }

        _view = static_cast<const GazeSharedMemorySlot*>(
            MapViewOfFile(_mapping, FILE_MAP_READ, 0, 0, sizeof(GazeSharedMemorySlot)));
        if (_view == nullptr)
        {
            CloseHandle(_mapping);
            _mapping = nullptr;
            _lastOpenFailureTick = now;
            return false;
        }

        LOG_INFO("Gaze ROI shared memory opened: Local\\EyeTracingGazeV1");
        return true;
    }

    void Close()
    {
        if (_view != nullptr)
        {
            UnmapViewOfFile(_view);
            _view = nullptr;
        }

        if (_mapping != nullptr)
        {
            CloseHandle(_mapping);
            _mapping = nullptr;
        }
    }

    bool TryGetFreshSample(int staleMs, GazeUdpSample& sample)
    {
        if (!EnsureOpened() || _view == nullptr)
            return false;

        GazeSharedMemorySlot snapshot {};
        bool consistent = false;
        for (int attempt = 0; attempt < 3; attempt++)
        {
            const uint32_t seqBefore = _view->seq;
            if ((seqBefore & 1U) != 0)
                continue;

            std::atomic_thread_fence(std::memory_order_acquire);
            std::memcpy(&snapshot, _view, sizeof(snapshot));
            std::atomic_thread_fence(std::memory_order_acquire);

            const uint32_t seqAfter = _view->seq;
            if (seqBefore == seqAfter && (seqAfter & 1U) == 0)
            {
                consistent = true;
                break;
            }
        }

        if (!consistent || snapshot.magic != GazeSharedMemoryMagic || snapshot.version != GazeSharedMemoryVersion ||
            (snapshot.flags & 1U) == 0)
            return false;

        const uint64_t now = GetTickCount64();
        if (snapshot.timestampMs <= 0.0 ||
            now - static_cast<uint64_t>(snapshot.timestampMs) > static_cast<uint64_t>(std::max(1, staleMs)))
            return false;

        float x = snapshot.x;
        float y = snapshot.y;
        if (snapshot.width > 0.0f && snapshot.height > 0.0f)
        {
            x /= snapshot.width;
            y /= snapshot.height;
        }
        else if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f)
        {
            return false;
        }

        sample.x = std::clamp(x, 0.0f, 1.0f);
        sample.y = std::clamp(y, 0.0f, 1.0f);
        sample.valid = true;
        sample.receivedTick = static_cast<uint64_t>(snapshot.timestampMs);
        return true;
    }

    ~GazeSharedMemoryReceiver() { Close(); }

  private:
    HANDLE _mapping = nullptr;
    const GazeSharedMemorySlot* _view = nullptr;
    uint64_t _lastOpenFailureTick = 0;
};
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
        // Capture the effective create-time hints after OptiScaler has applied
        // its normal preset policy. The private ROI feature must use the same
        // model contract as the game-owned feature.
        CaptureGazeRoiCreatePresets(InParameters);

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
        const bool gazeRoiEnabled = Config::Instance()->GazeRoiEnabled.value_or_default();

        if (gazeRoiEnabled)
        {
            unsigned int renderWidth = 0;
            unsigned int renderHeight = 0;
            GetRenderResolution(InParameters, &renderWidth, &renderHeight);
            LogGazeRoiContract(InParameters, "Replacement");
            TraceNgxKnownParameters(InParameters, "original-input");

            const bool attempted = TryEvaluateGazeRoi(InCommandList, InParameters, nvResult);
            if (nvResult == NVSDK_NGX_Result_Success)
            {
                LogGazeRoiDecision("GROI_ACCEPT_REPLACEMENT");
                _frameCount++;
                return true;
            }

            LogGazeRoiDecision(attempted ? "GROI_FAIL_REPLACEMENT_EVALUATE" : "GROI_FAIL_REPLACEMENT_SETUP");
            LOG_ERROR("Gaze ROI replacement failed ({:X}); original full-frame DLSS was not evaluated",
                      static_cast<unsigned int>(nvResult));
            _gazeHasPreviousInputRect = false;
            return false;
        }

        _gazeHasPreviousInputRect = false;
        ProcessEvaluateParams(InParameters);
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

void DLSSFeatureDx12::LogGazeRoiDecision(const std::string& decision)
{
    if (_lastGazeContractDecision == decision)
        return;

    _lastGazeContractDecision = decision;
    LOG_WARN("[GROI_CONTRACT] {}", decision);
}

void DLSSFeatureDx12::LogGazeRoiContract(NVSDK_NGX_Parameter* InParameters, const std::string& mode)
{
    if (InParameters == nullptr)
        return;

    const auto width = GetNgxValue<unsigned int>(InParameters, NVSDK_NGX_Parameter_Width, 0);
    const auto height = GetNgxValue<unsigned int>(InParameters, NVSDK_NGX_Parameter_Height, 0);
    const auto outWidth = GetNgxValue<unsigned int>(InParameters, NVSDK_NGX_Parameter_OutWidth, 0);
    const auto outHeight = GetNgxValue<unsigned int>(InParameters, NVSDK_NGX_Parameter_OutHeight, 0);
    const auto renderWidth = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, width);
    const auto renderHeight = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, height);
    const auto flags = GetNgxValue<unsigned int>(InParameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 0);
    const auto reset = GetNgxValue<int>(InParameters, NVSDK_NGX_Parameter_Reset, 0);
    const auto mvScaleX = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    const auto mvScaleY = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    const auto jitterX = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    const auto jitterY = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    const auto preExposure = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    const auto exposureScale = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);

    const auto colorBaseX = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0);
    const auto colorBaseY = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0);
    const auto depthBaseX = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0);
    const auto depthBaseY = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0);
    const auto mvBaseX = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0);
    const auto mvBaseY = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0);
    const auto outputBaseX = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);
    const auto outputBaseY = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0);

    std::ostringstream contract;
    contract << "mode=" << mode << " create=" << width << 'x' << height << "->" << outWidth << 'x' << outHeight
             << " render=" << renderWidth << 'x' << renderHeight << " flags=0x" << std::hex << flags << std::dec
             << " lowResMV=" << ((flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0)
             << " jitteredMV=" << ((flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0)
             << " reset=" << reset << " mvScale=" << mvScaleX << ',' << mvScaleY << " jitter=" << jitterX << ','
             << jitterY << " preExposure=" << preExposure << " exposureScale=" << exposureScale
             << " bases=color(" << colorBaseX << ',' << colorBaseY << ") depth(" << depthBaseX << ',' << depthBaseY
             << ") mv(" << mvBaseX << ',' << mvBaseY << ") output(" << outputBaseX << ',' << outputBaseY << ')';

    AppendGazeResourceContract(contract, "color", GetNgxResource(InParameters, NVSDK_NGX_Parameter_Color));
    AppendGazeResourceContract(contract, "depth", GetNgxResource(InParameters, NVSDK_NGX_Parameter_Depth));
    AppendGazeResourceContract(contract, "mv", GetNgxResource(InParameters, NVSDK_NGX_Parameter_MotionVectors));
    AppendGazeResourceContract(contract, "output", GetNgxResource(InParameters, NVSDK_NGX_Parameter_Output));
    AppendGazeResourceContract(contract, "exposure", GetNgxResource(InParameters, NVSDK_NGX_Parameter_ExposureTexture));
    AppendGazeResourceContract(
        contract, "biasCurrentColor", GetNgxResource(InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask));
    AppendGazeResourceContract(
        contract, "transparency", GetNgxResource(InParameters, NVSDK_NGX_Parameter_TransparencyMask));
    AppendGazeResourceContract(contract, "motionVectors3D",
                               GetNgxResource(InParameters, NVSDK_NGX_Parameter_MotionVectors3D));
    AppendGazeResourceContract(contract, "depthHighRes",
                               GetNgxResource(InParameters, NVSDK_NGX_Parameter_DepthHighRes));
    AppendGazeResourceContract(contract, "motionVectorsReflection",
                               GetNgxResource(InParameters, NVSDK_NGX_Parameter_MotionVectorsReflection));

    const std::string signature = contract.str();
    if (_lastGazeContractSignature == signature)
        return;

    _lastGazeContractSignature = signature;
    _lastGazeContractDecision.clear();
    LOG_INFO("[GROI_CONTRACT] frame={} {}", _frameCount, signature);
}

void DLSSFeatureDx12::UpdateVirtualGazePoint()
{
    const std::string control = Config::Instance()->GazeRoiControl.value_or_default();
    if (control != _gazePreviousControl)
    {
        LOG_INFO("[GROI_INPUT] control changed {} -> {}; preserving the current ROI until the selected source "
                 "provides a fresh sample",
                 _gazePreviousControl.empty() ? "initial" : _gazePreviousControl, control);
        _gazePreviousControl = control;
        _gazeExternalSourceStateInitialized = false;
    }

    const auto updateExternalSourceState = [this, &control](bool fresh)
    {
        if (!_gazeExternalSourceStateInitialized || fresh != _gazeExternalSourceFresh)
        {
            if (fresh)
            {
                LOG_INFO("[GROI_INPUT] {} source is fresh; accepting gaze samples", control);
            }
            else
            {
                LOG_WARN("[GROI_INPUT] {} source is unavailable, invalid, or stale; freezing the last gaze point "
                         "without resetting private DLSS history",
                         control);
            }
        }

        _gazeExternalSourceStateInitialized = true;
        _gazeExternalSourceFresh = fresh;
    };

    if (control == "ExternalUdp")
    {
        GazeSharedMemoryReceiver::Instance().Close();
        auto& receiver = GazeUdpReceiver::Instance();
        if (!receiver.EnsureStarted(Config::Instance()->GazeRoiUdpPort.value_or_default()))
        {
            updateExternalSourceState(false);
            return;
        }

        GazeUdpSample sample {};
        if (receiver.TryGetFreshSample(Config::Instance()->GazeRoiStaleMs.value_or_default(), sample))
        {
            _gazePointX = sample.x;
            _gazePointY = sample.y;
            updateExternalSourceState(true);
        }
        else
        {
            updateExternalSourceState(false);
        }

        return;
    }

    GazeUdpReceiver::Instance().Stop();

    if (control == "ExternalSharedMemory")
    {
        auto& receiver = GazeSharedMemoryReceiver::Instance();
        GazeUdpSample sample {};
        if (receiver.TryGetFreshSample(Config::Instance()->GazeRoiStaleMs.value_or_default(), sample))
        {
            _gazePointX = sample.x;
            _gazePointY = sample.y;
            updateExternalSourceState(true);
        }
        else
        {
            updateExternalSourceState(false);
        }

        return;
    }

    GazeSharedMemoryReceiver::Instance().Close();
    _gazeExternalSourceStateInitialized = false;

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

    const auto clampConfiguredDimension = [](int configured, uint32_t target) -> uint32_t
    {
        if (target == 0)
            return 0;

        const uint32_t minimum = std::min<uint32_t>(16, target);
        const int upper = static_cast<int>(std::min<uint32_t>(target, static_cast<uint32_t>(std::numeric_limits<int>::max())));
        return static_cast<uint32_t>(std::clamp(configured, static_cast<int>(minimum), upper));
    };
    const uint32_t desiredOutputWidth =
        clampConfiguredDimension(Config::Instance()->GazeRoiWidthPx.value_or_default(), targetWidth);
    const uint32_t desiredOutputHeight =
        clampConfiguredDimension(Config::Instance()->GazeRoiHeightPx.value_or_default(), targetHeight);

    outputRect.width = desiredOutputWidth;
    outputRect.height = desiredOutputHeight;
    const int32_t centeredOutputX =
        static_cast<int32_t>(_gazePointX * targetWidth) - static_cast<int32_t>(outputRect.width / 2);
    const int32_t centeredOutputY =
        static_cast<int32_t>(_gazePointY * targetHeight) - static_cast<int32_t>(outputRect.height / 2);
    outputRect.x = static_cast<uint32_t>(
        std::clamp(centeredOutputX, 0, static_cast<int32_t>(targetWidth - outputRect.width)));
    outputRect.y = static_cast<uint32_t>(
        std::clamp(centeredOutputY, 0, static_cast<int32_t>(targetHeight - outputRect.height)));

    inputRect.width = std::max<uint32_t>(16, ScaleRoiDimension(desiredOutputWidth, renderWidth, targetWidth));
    inputRect.height = std::max<uint32_t>(16, ScaleRoiDimension(desiredOutputHeight, renderHeight, targetHeight));
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

    if (outputRect.x >= targetWidth || outputRect.y >= targetHeight)
        return false;

    outputRect.width = std::min(outputRect.width, targetWidth - outputRect.x);
    outputRect.height = std::min(outputRect.height, targetHeight - outputRect.y);
    return inputRect.width > 0 && inputRect.height > 0 && outputRect.width > 0 && outputRect.height > 0;
}

void DLSSFeatureDx12::CaptureGazeRoiCreatePresets(NVSDK_NGX_Parameter* InParameters)
{
    if (InParameters == nullptr)
        return;

    for (size_t index = 0; index < std::size(gazeRoiPresetKeys); ++index)
        _gazeRoiCreatePresetValues[index] = GetNgxValue<unsigned int>(InParameters, gazeRoiPresetKeys[index], 0);

    _gazeRoiCreatePresetsCaptured = true;
    LOG_INFO("[GROI_CONTRACT] captured main-feature create presets: {},{},{},{},{},{}",
             _gazeRoiCreatePresetValues[0], _gazeRoiCreatePresetValues[1], _gazeRoiCreatePresetValues[2],
             _gazeRoiCreatePresetValues[3], _gazeRoiCreatePresetValues[4], _gazeRoiCreatePresetValues[5]);
}

uint32_t DLSSFeatureDx12::GazeRoiCreatePresetValue(NVSDK_NGX_Parameter* InParameters, size_t index) const
{
    if (index >= std::size(gazeRoiPresetKeys))
        return 0;
    if (_gazeRoiCreatePresetsCaptured)
        return _gazeRoiCreatePresetValues[index];
    return GetNgxValue<unsigned int>(InParameters, gazeRoiPresetKeys[index], 0);
}

bool DLSSFeatureDx12::EnsureGazeRoiMinimalParameters()
{
    if (_gazeMinimalParameters != nullptr)
        return true;

    const auto allocateParameters = NVNGXProxy::D3D12_AllocateParameters();
    if (allocateParameters == nullptr)
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_MINIMAL_PARAMS_ALLOCATE api-missing");
        return false;
    }

    const NVSDK_NGX_Result result = allocateParameters(&_gazeMinimalParameters);
    if (result != NVSDK_NGX_Result_Success || _gazeMinimalParameters == nullptr)
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_MINIMAL_PARAMS_ALLOCATE result={:X} params={:X}",
                  static_cast<unsigned int>(result), reinterpret_cast<uintptr_t>(_gazeMinimalParameters));
        _gazeMinimalParameters = nullptr;
        return false;
    }

    LOG_INFO("[GROI_CONTRACT] allocated NVIDIA-native minimal private parameter table params={:X}",
             reinterpret_cast<uintptr_t>(_gazeMinimalParameters));
    return true;
}

bool DLSSFeatureDx12::ResolveGazeRoiOptimalInput(NVSDK_NGX_Parameter* InParameters,
                                                 GazeRoiRect& outputRect, GazeRoiRect& inputRect)
{
    if (InParameters == nullptr || outputRect.width == 0 || outputRect.height == 0)
        return false;

    const int perfQuality = GetNgxValue<int>(InParameters, NVSDK_NGX_Parameter_PerfQualityValue,
                                             static_cast<int>(PerfQualityValue()));
    void* callbackPointer = nullptr;
    NVSDK_NGX_Result callbackLookupResult =
        InParameters->Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callbackPointer);
    if (callbackLookupResult != NVSDK_NGX_Result_Success || callbackPointer == nullptr)
    {
        callbackPointer = nullptr;
        callbackLookupResult =
            InParameters->Get(NVSDK_NGX_EParameter_DLSSOptimalSettingsCallback, &callbackPointer);
    }
    const bool hasOptimalSettingsCallback =
        callbackLookupResult == NVSDK_NGX_Result_Success && callbackPointer != nullptr;

    std::ostringstream signature;
    signature << outputRect.width << 'x' << outputRect.height << ":quality=" << perfQuality << ":render="
              << RenderWidth() << 'x' << RenderHeight() << ":target=" << TargetWidth() << 'x' << TargetHeight()
              << ":callback=" << hasOptimalSettingsCallback;
    for (size_t index = 0; index < std::size(gazeRoiPresetKeys); ++index)
        signature << ':' << GazeRoiCreatePresetValue(InParameters, index);

    const std::string querySignature = signature.str();
    if (_gazeRoiOptimalSignature != querySignature)
    {
        if (!hasOptimalSettingsCallback)
        {
            _gazeRoiOptimalWidth = inputRect.width;
            _gazeRoiOptimalHeight = inputRect.height;
            _gazeRoiOptimalMinWidth = inputRect.width;
            _gazeRoiOptimalMinHeight = inputRect.height;
            _gazeRoiOptimalMaxWidth = inputRect.width;
            _gazeRoiOptimalMaxHeight = inputRect.height;
            _gazeRoiOptimalSignature = querySignature;

            LOG_INFO(
                "[GROI_CONTRACT] optimal callback unavailable; using native-scale ROI input {}x{} -> {}x{} "
                "from render={}x{} target={}x{}",
                inputRect.width, inputRect.height, outputRect.width, outputRect.height, RenderWidth(),
                RenderHeight(), TargetWidth(), TargetHeight());
        }
        else
        {
            using OptimalSettingsCallback = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
            const auto callback = reinterpret_cast<OptimalSettingsCallback>(callbackPointer);

            NgxParameterOverlay optimalParameters(InParameters, "optimal");
            optimalParameters.SetOverride(NVSDK_NGX_Parameter_Width, outputRect.width);
            optimalParameters.SetOverride(NVSDK_NGX_Parameter_Height, outputRect.height);
            optimalParameters.SetOverride(NVSDK_NGX_Parameter_PerfQualityValue, perfQuality);
            for (size_t index = 0; index < std::size(gazeRoiPresetKeys); ++index)
                optimalParameters.SetOverride(gazeRoiPresetKeys[index],
                                              GazeRoiCreatePresetValue(InParameters, index));

            const NVSDK_NGX_Result queryResult = callback(&optimalParameters);
            uint32_t optimalWidth = 0;
            uint32_t optimalHeight = 0;
            if (queryResult != NVSDK_NGX_Result_Success ||
                optimalParameters.Get(NVSDK_NGX_Parameter_OutWidth, &optimalWidth) !=
                    NVSDK_NGX_Result_Success ||
                optimalParameters.Get(NVSDK_NGX_Parameter_OutHeight, &optimalHeight) !=
                    NVSDK_NGX_Result_Success ||
                optimalWidth == 0 || optimalHeight == 0)
            {
                LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_OPTIMAL_QUERY result={:X}",
                          static_cast<unsigned int>(queryResult));
                return false;
            }

            _gazeRoiOptimalWidth = optimalWidth;
            _gazeRoiOptimalHeight = optimalHeight;
            _gazeRoiOptimalMinWidth = GetNgxValue<unsigned int>(
                &optimalParameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, optimalWidth);
            _gazeRoiOptimalMinHeight = GetNgxValue<unsigned int>(
                &optimalParameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, optimalHeight);
            _gazeRoiOptimalMaxWidth = GetNgxValue<unsigned int>(
                &optimalParameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, optimalWidth);
            _gazeRoiOptimalMaxHeight = GetNgxValue<unsigned int>(
                &optimalParameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, optimalHeight);
            _gazeRoiOptimalSignature = querySignature;

            LOG_INFO("[GROI_CONTRACT] optimal ROI {}x{} -> {}x{}, dynamic {}x{}..{}x{} quality={}",
                     _gazeRoiOptimalWidth, _gazeRoiOptimalHeight, outputRect.width, outputRect.height,
                     _gazeRoiOptimalMinWidth, _gazeRoiOptimalMinHeight, _gazeRoiOptimalMaxWidth,
                     _gazeRoiOptimalMaxHeight, perfQuality);
        }
    }

    if (_gazeRoiOptimalWidth < _gazeRoiOptimalMinWidth || _gazeRoiOptimalHeight < _gazeRoiOptimalMinHeight ||
        _gazeRoiOptimalWidth > _gazeRoiOptimalMaxWidth || _gazeRoiOptimalHeight > _gazeRoiOptimalMaxHeight ||
        _gazeRoiOptimalWidth > RenderWidth() || _gazeRoiOptimalHeight > RenderHeight())
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_OPTIMAL_RANGE optimal={}x{} dynamic={}x{}..{}x{} render={}x{}",
                  _gazeRoiOptimalWidth, _gazeRoiOptimalHeight, _gazeRoiOptimalMinWidth,
                  _gazeRoiOptimalMinHeight, _gazeRoiOptimalMaxWidth, _gazeRoiOptimalMaxHeight, RenderWidth(),
                  RenderHeight());
        return false;
    }

    inputRect.width = _gazeRoiOptimalWidth;
    inputRect.height = _gazeRoiOptimalHeight;
    constexpr uint32_t minimumRoiInputDimension = 64;
    if (inputRect.width < minimumRoiInputDimension || inputRect.height < minimumRoiInputDimension)
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_ROI_INPUT_TOO_SMALL output={}x{} input={}x{} minimum={}",
                  outputRect.width, outputRect.height, inputRect.width, inputRect.height,
                  minimumRoiInputDimension);
        return false;
    }
    const float outputCenterX = static_cast<float>(outputRect.x) + static_cast<float>(outputRect.width) * 0.5f;
    const float outputCenterY = static_cast<float>(outputRect.y) + static_cast<float>(outputRect.height) * 0.5f;
    const int32_t centeredInputX = static_cast<int32_t>(
        std::round(outputCenterX * static_cast<float>(RenderWidth()) / static_cast<float>(TargetWidth()))) -
                                   static_cast<int32_t>(inputRect.width / 2);
    const int32_t centeredInputY = static_cast<int32_t>(
        std::round(outputCenterY * static_cast<float>(RenderHeight()) / static_cast<float>(TargetHeight()))) -
                                   static_cast<int32_t>(inputRect.height / 2);
    inputRect.x = static_cast<uint32_t>(
        std::clamp(centeredInputX, 0, static_cast<int32_t>(RenderWidth() - inputRect.width)));
    inputRect.y = static_cast<uint32_t>(
        std::clamp(centeredInputY, 0, static_cast<int32_t>(RenderHeight() - inputRect.height)));

    // Input pixels are the temporal coordinate grid. Derive the output origin
    // from the snapped input rect so an unchanged input crop cannot move in
    // output space without a corresponding motion-vector correction.
    outputRect.x = MapCenteredRectOrigin(inputRect.x, inputRect.width, RenderWidth(), outputRect.width,
                                         TargetWidth());
    outputRect.y = MapCenteredRectOrigin(inputRect.y, inputRect.height, RenderHeight(), outputRect.height,
                                         TargetHeight());
    return true;
}

void DLSSFeatureDx12::RetireGazeRoiDlssHandle()
{
    if (_p_gazeRoiDlssHandle == nullptr)
        return;

    NVSDK_NGX_Handle* retiredHandle = _p_gazeRoiDlssHandle;
    const bool copiedLocalHandle = retiredHandle == &_gazeRoiDlssHandle;
    if (copiedLocalHandle)
        retiredHandle = new NVSDK_NGX_Handle(_gazeRoiDlssHandle);

    const unsigned int retiredHandleId = retiredHandle->Id;
    const uintptr_t retiredHandleAddress = reinterpret_cast<uintptr_t>(retiredHandle);
    GazeRoiFrameSync::DeferCallback(
        [retiredHandle, copiedLocalHandle]()
        {
            if (const auto releaseFeature = NVNGXProxy::D3D12_ReleaseFeature(); releaseFeature != nullptr)
                releaseFeature(retiredHandle);
            if (copiedLocalHandle)
                delete retiredHandle;
        });
    LOG_INFO("[GROI_CONTRACT] retired private ROI feature handle ptr={:X} id={} pending fence release",
             retiredHandleAddress, retiredHandleId);
    _p_gazeRoiDlssHandle = nullptr;
    _gazeRoiDlssHandle = {};
    _gazeRoiHandleCreateSignature.clear();
}

bool DLSSFeatureDx12::EnsureGazeRoiDlssHandle(ID3D12GraphicsCommandList* InCommandList,
                                              NVSDK_NGX_Parameter* InParameters, const GazeRoiRect& outputRect,
                                              const GazeRoiRect& inputRect)
{
    if (InCommandList == nullptr || InParameters == nullptr || NVNGXProxy::D3D12_CreateFeature() == nullptr ||
        GazeRoi == nullptr || GazeRoi->DlssOutput() == nullptr)
        return false;

    const unsigned int featureFlags = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<unsigned int>(GetFeatureFlags()));
    const int perfQuality = GetNgxValue<int>(InParameters, NVSDK_NGX_Parameter_PerfQualityValue,
                                             static_cast<int>(PerfQualityValue()));
    const bool minimalPrivateParameters =
        Config::Instance()->GazeRoiMinimalPrivateParameters.value_or_default();

    std::ostringstream signature;
    signature << inputRect.width << 'x' << inputRect.height << "->" << outputRect.width << 'x' << outputRect.height
               << ":flags=" << featureFlags << ":quality=" << perfQuality
               << ":output=" << static_cast<const void*>(GazeRoi->DlssOutput())
               << ":parameters=" << (minimalPrivateParameters ? "minimal-native" : "inherited-native");
    if (!minimalPrivateParameters)
    {
        for (size_t index = 0; index < std::size(gazeRoiPresetKeys); ++index)
            signature << ':' << GazeRoiCreatePresetValue(InParameters, index);
    }

    const std::string createSignature = signature.str();
    const bool handleMatches = _p_gazeRoiDlssHandle != nullptr &&
                               _gazeRoiHandleInputRect.width == inputRect.width &&
                               _gazeRoiHandleInputRect.height == inputRect.height &&
                               _gazeRoiHandleOutputRect.width == outputRect.width &&
                               _gazeRoiHandleOutputRect.height == outputRect.height &&
                               _gazeRoiHandleCreateSignature == createSignature;
    if (handleMatches)
    {
        _gazeRoiHandleWasCreated = false;
        return true;
    }

    RetireGazeRoiDlssHandle();

    NVSDK_NGX_Parameter* createParameters = InParameters;
    std::unique_ptr<NgxParameterScope> createScope;
    if (minimalPrivateParameters)
    {
        if (!EnsureGazeRoiMinimalParameters())
            return false;

        createParameters = _gazeMinimalParameters;
        createParameters->Reset();
        createParameters->Set(NVSDK_NGX_Parameter_CreationNodeMask,
                              GetNgxValue<unsigned int>(InParameters, NVSDK_NGX_Parameter_CreationNodeMask, 1));
        createParameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask,
                              GetNgxValue<unsigned int>(InParameters, NVSDK_NGX_Parameter_VisibilityNodeMask, 1));
        createParameters->Set(NVSDK_NGX_Parameter_Width, inputRect.width);
        createParameters->Set(NVSDK_NGX_Parameter_Height, inputRect.height);
        createParameters->Set(NVSDK_NGX_Parameter_OutWidth, outputRect.width);
        createParameters->Set(NVSDK_NGX_Parameter_OutHeight, outputRect.height);
        createParameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, perfQuality);
        createParameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                              static_cast<int>(featureFlags));
        createParameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);
        LOG_INFO("[GROI_CONTRACT] parameterMode=minimal-native phase=create "
                 "minimalKeys=nodeMasks,width,height,outWidth,outHeight,quality,flags,outputSubrects");
    }
    else
    {
        createScope = std::make_unique<NgxParameterScope>(InParameters, "create");
        createScope->Override(NVSDK_NGX_Parameter_Width, inputRect.width, "uint");
        createScope->Override(NVSDK_NGX_Parameter_Height, inputRect.height, "uint");
        createScope->Override(NVSDK_NGX_Parameter_OutWidth, outputRect.width, "uint");
        createScope->Override(NVSDK_NGX_Parameter_OutHeight, outputRect.height, "uint");
        createScope->Override(NVSDK_NGX_Parameter_PerfQualityValue, perfQuality, "int");
        createScope->Override(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, featureFlags, "uint");
        createScope->Override(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1, "int");
        createScope->Override(NVSDK_NGX_Parameter_Output, GazeRoi->DlssOutput(), "d3d12");
        createScope->Override(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0U, "uint");
        createScope->Override(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0U, "uint");
        for (size_t index = 0; index < std::size(gazeRoiPresetKeys); ++index)
            createScope->Override(gazeRoiPresetKeys[index], GazeRoiCreatePresetValue(InParameters, index), "uint");
    }
    TraceNgxKnownParameters(createParameters, "create-effective");

    _p_gazeRoiDlssHandle = &_gazeRoiDlssHandle;

    LOG_INFO("[GROI_CONTRACT] begin private feature create params={:X} cmd={:X} roi={}x{}->{}x{} flags=0x{:X} "
             "quality={}",
              reinterpret_cast<uintptr_t>(createParameters), reinterpret_cast<uintptr_t>(InCommandList),
             inputRect.width, inputRect.height, outputRect.width, outputRect.height, featureFlags, perfQuality);

    NVSDK_NGX_Result createResult;
    {
        ScopedSkipHeapCapture skipHeapCapture {};
        createResult = NVNGXProxy::D3D12_CreateFeature()(InCommandList, NVSDK_NGX_Feature_SuperSampling,
                                                         createParameters, &_p_gazeRoiDlssHandle);
    }

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
    _gazeRoiHandleCreateSignature = createSignature;
    _gazeRoiHandleWasCreated = true;
    LOG_INFO("Gaze ROI DLSS feature created: ptr={:X} id={} {}x{} -> {}x{}",
             reinterpret_cast<uintptr_t>(_p_gazeRoiDlssHandle), _p_gazeRoiDlssHandle->Id,
             inputRect.width, inputRect.height, outputRect.width, outputRect.height);
    return true;
}


bool DLSSFeatureDx12::TryEvaluateGazeRoi(ID3D12GraphicsCommandList* InCommandList,
                                         NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Result& nvResult)
{
    nvResult = NVSDK_NGX_Result_Fail;
    if (InCommandList == nullptr || InParameters == nullptr || Device == nullptr)
        return false;

    ID3D12Resource* color = GetNgxResource(InParameters, NVSDK_NGX_Parameter_Color);
    ID3D12Resource* depth = GetNgxResource(InParameters, NVSDK_NGX_Parameter_Depth);
    ID3D12Resource* motionVectors = GetNgxResource(InParameters, NVSDK_NGX_Parameter_MotionVectors);
    ID3D12Resource* output = GetNgxResource(InParameters, NVSDK_NGX_Parameter_Output);
    if (color == nullptr || depth == nullptr || motionVectors == nullptr || output == nullptr)
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_MISSING_REQUIRED_RESOURCE color={} depth={} mv={} output={}",
                  color != nullptr, depth != nullptr, motionVectors != nullptr, output != nullptr);
        return false;
    }

    const bool minimalPrivateParameters =
        Config::Instance()->GazeRoiMinimalPrivateParameters.value_or_default();

    const char* passthroughResources[] = {
        NVSDK_NGX_Parameter_RayTracingHitDistance,
        NVSDK_NGX_Parameter_GBuffer_Normals,
    };
    if (!minimalPrivateParameters)
    {
        for (const char* key : passthroughResources)
        {
            if (ID3D12Resource* resource = GetNgxResource(InParameters, key))
            {
                const auto desc = resource->GetDesc();
                LOG_INFO("[GROI_CONTRACT] optional passthrough key={} resource={:X} {}x{} format={} flags=0x{:X}",
                         key, reinterpret_cast<uintptr_t>(resource), desc.Width, desc.Height,
                         static_cast<uint32_t>(desc.Format), static_cast<uint32_t>(desc.Flags));
            }
        }
    }

    // The default path forwards vendor/engine resources through the native
    // game-owned table. MinimalPrivateParameters deliberately does not read or
    // forward them so this test isolates the documented DLSS SR core contract.

    UpdateVirtualGazePoint();

    GazeRoiRect outputRect {};
    GazeRoiRect inputRect {};
    if (!BuildGazeRoiRects(outputRect, inputRect))
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_INVALID_ROI_RECT");
        return false;
    }
    if (!ResolveGazeRoiOptimalInput(InParameters, outputRect, inputRect))
        return false;

    const unsigned int featureFlags = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<unsigned int>(GetFeatureFlags()));
    const bool lowResMotionVectors = (featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    const int perfQuality = GetNgxValue<int>(InParameters, NVSDK_NGX_Parameter_PerfQualityValue,
                                             static_cast<int>(PerfQualityValue()));

    const uint32_t colorBaseX = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0);
    const uint32_t colorBaseY = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0);
    const uint32_t depthBaseX = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0);
    const uint32_t depthBaseY = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0);
    const uint32_t originalMvBaseX = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0);
    const uint32_t originalMvBaseY = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0);
    const uint32_t outputBaseX = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);
    const uint32_t outputBaseY = GetNgxValue<unsigned int>(
        InParameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0);

    uint32_t roiColorBaseX = 0;
    uint32_t roiColorBaseY = 0;
    uint32_t roiDepthBaseX = 0;
    uint32_t roiDepthBaseY = 0;
    // Read the ROI from the game's original MV coordinate space, then write it
    // into a compact base-zero resource for the private ROI feature.
    const GazeRoiRect& mvRect = lowResMotionVectors ? inputRect : outputRect;
    uint32_t roiMvBaseX = 0;
    uint32_t roiMvBaseY = 0;
    uint32_t finalRoiOutputBaseX = 0;
    uint32_t finalRoiOutputBaseY = 0;
    if (!AddBase(colorBaseX, inputRect.x, roiColorBaseX) ||
        !AddBase(colorBaseY, inputRect.y, roiColorBaseY) ||
        !AddBase(depthBaseX, inputRect.x, roiDepthBaseX) ||
        !AddBase(depthBaseY, inputRect.y, roiDepthBaseY) ||
        !AddBase(originalMvBaseX, mvRect.x, roiMvBaseX) ||
        !AddBase(originalMvBaseY, mvRect.y, roiMvBaseY) ||
        !AddBase(outputBaseX, outputRect.x, finalRoiOutputBaseX) ||
        !AddBase(outputBaseY, outputRect.y, finalRoiOutputBaseY))
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_SUBRECT_OVERFLOW");
        return false;
    }

    if (!ResourceRectFits(color, roiColorBaseX, roiColorBaseY, inputRect.width, inputRect.height) ||
        !ResourceRectFits(depth, roiDepthBaseX, roiDepthBaseY, inputRect.width, inputRect.height) ||
        !ResourceRectFits(motionVectors, roiMvBaseX, roiMvBaseY, mvRect.width, mvRect.height) ||
        !ResourceRectFits(output, outputBaseX, outputBaseY, TargetWidth(), TargetHeight()) ||
        !ResourceRectFits(output, finalRoiOutputBaseX, finalRoiOutputBaseY, outputRect.width, outputRect.height))
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_SUBRECT_OUT_OF_BOUNDS lowResMV={} input={}x{}+{},{} "
                  "output={}x{}+{},{} bases=color({},{}) depth({},{}) mv({},{}) output({},{})",
                  lowResMotionVectors, inputRect.width, inputRect.height, inputRect.x, inputRect.y, outputRect.width,
                  outputRect.height, outputRect.x, outputRect.y, roiColorBaseX, roiColorBaseY, roiDepthBaseX,
                  roiDepthBaseY, roiMvBaseX, roiMvBaseY, outputBaseX, outputBaseY);
        return false;
    }

    ID3D12Resource* transparency = minimalPrivateParameters
                                      ? nullptr
                                      : GetNgxResource(InParameters, NVSDK_NGX_Parameter_TransparencyMask);
    ID3D12Resource* biasCurrentColor =
        minimalPrivateParameters
            ? nullptr
            : GetNgxResource(InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask);
    uint32_t transparencyBaseX = 0;
    uint32_t transparencyBaseY = 0;
    uint32_t biasBaseX = 0;
    uint32_t biasBaseY = 0;
    if (transparency != nullptr)
    {
        const uint32_t baseX = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, 0);
        const uint32_t baseY = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y, 0);
        if (!AddBase(baseX, inputRect.x, transparencyBaseX) || !AddBase(baseY, inputRect.y, transparencyBaseY) ||
            !ResourceRectFits(transparency, transparencyBaseX, transparencyBaseY, inputRect.width, inputRect.height))
        {
            LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_TRANSPARENCY_SUBRECT");
            return false;
        }
    }
    const bool omitBiasCurrentColorHint = Config::Instance()->GazeRoiOmitBiasCurrentColorHint.value_or_default();
    if (biasCurrentColor != nullptr && !omitBiasCurrentColorHint)
    {
        const uint32_t baseX = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, 0);
        const uint32_t baseY = GetNgxValue<unsigned int>(
            InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, 0);
        if (!AddBase(baseX, inputRect.x, biasBaseX) || !AddBase(baseY, inputRect.y, biasBaseY) ||
            !ResourceRectFits(biasCurrentColor, biasBaseX, biasBaseY, inputRect.width, inputRect.height))
        {
            LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_BIAS_CURRENT_COLOR_SUBRECT");
            return false;
        }
    }

    float mvScaleX = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    float mvScaleY = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    const float jitterOffsetX = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    const float jitterOffsetY = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    const float sharpness = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_Sharpness, 0.0f);
    const float preExposure = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    const float exposureScale = GetNgxValue<float>(InParameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    if (!std::isfinite(mvScaleX) || !std::isfinite(mvScaleY) || std::abs(mvScaleX) < 0.0001f ||
        std::abs(mvScaleY) < 0.0001f)
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_MV_SCALE_INVALID {},{}", mvScaleX, mvScaleY);
        return false;
    }

    const GazeRoiMotionVectorMode motionVectorMode = GetGazeRoiMotionVectorMode();
    const bool outputClearDebug = Config::Instance()->GazeRoiOutputClearDebug.value_or_default();
    const bool currentColorPointBypass =
        Config::Instance()->GazeRoiCurrentColorPointBypass.value_or_default();
    const bool colorCopy = Config::Instance()->GazeRoiColorCopy.value_or_default();
    const bool depthCopy = Config::Instance()->GazeRoiDepthCopy.value_or_default();
    const bool resetOnMove = Config::Instance()->GazeRoiResetOnMove.value_or_default();
    const auto peripheralMode = static_cast<GazeRoiPeripheralReconstructionMode>(
        std::clamp(Config::Instance()->GazeRoiPeripheralMode.value_or_default(), 0,
                   static_cast<int>(GazeRoiPeripheralReconstructionMode::IntermediateColorTaau)));
    const bool peripheralIntermediateTaau =
        peripheralMode == GazeRoiPeripheralReconstructionMode::IntermediateColorTaau;
    // Keep the retired joint shader and resource path available in source, but
    // do not expose or dispatch it through any public reconstruction mode.
    const bool peripheralJoint = false;
    const bool peripheralCurrentOnly = peripheralMode == GazeRoiPeripheralReconstructionMode::DejitteredCurrent;
    const bool peripheralUsesHistory = !peripheralCurrentOnly;
    const bool peripheralTemporalDetail =
        peripheralMode == GazeRoiPeripheralReconstructionMode::LightweightTaa &&
        Config::Instance()->GazeRoiPeripheralTemporalDetail.value_or_default();
    const float peripheralTemporalDetailScale = std::clamp(
        Config::Instance()->GazeRoiPeripheralTemporalDetailScale.value_or_default(), 1.0f, 2.0f);
    const float peripheralTemporalDetailStrength = std::clamp(
        Config::Instance()->GazeRoiPeripheralTemporalDetailStrength.value_or_default(), 0.0f, 4.0f);
    // EASU did not improve the high-frequency peripheral content that dominates
    // gaze-transition quality. Keep its persisted key readable for rollback,
    // but do not silently reactivate the hidden legacy path.
    const bool peripheralEasu = false;
    const bool peripheralBlur = !peripheralCurrentOnly &&
                                Config::Instance()->GazeRoiPeripheralBlur.value_or_default();
    // Keep the low bits as the shader reconstruction mode. Bit 2 carries the
    // feature's depth convention and bit 4 selects the independent intermediate
    // TAAU resolve without expanding the shared 64-dword root-constant block.
    const int peripheralTemporalMode = peripheralUsesHistory ? 2 : 8;
    const int peripheralTemporal = peripheralTemporalMode |
                                   (peripheralUsesHistory && DepthInverted() ? 4 : 0) |
                                   (peripheralIntermediateTaau ? 16 : 0);
    const bool peripheralTemporalMotionReprojection = peripheralUsesHistory;
    uint32_t peripheralResolveWidth = RenderWidth();
    uint32_t peripheralResolveHeight = RenderHeight();
    if (peripheralIntermediateTaau)
    {
        constexpr float intermediateScale = 1.25f;
        peripheralResolveWidth = std::min(
            TargetWidth(), std::max(RenderWidth(), static_cast<uint32_t>(
                                                   std::lround(RenderWidth() * intermediateScale))));
        peripheralResolveHeight = std::min(
            TargetHeight(), std::max(RenderHeight(), static_cast<uint32_t>(
                                                    std::lround(RenderHeight() * intermediateScale))));
    }
    const uint32_t peripheralDetailWidth = peripheralTemporalDetail
                                               ? std::min(TargetWidth(), std::max(RenderWidth(), static_cast<uint32_t>(
                                                     std::lround(RenderWidth() * peripheralTemporalDetailScale))))
                                               : 0;
    const uint32_t peripheralDetailHeight = peripheralTemporalDetail
                                                ? std::min(TargetHeight(), std::max(RenderHeight(), static_cast<uint32_t>(
                                                      std::lround(RenderHeight() * peripheralTemporalDetailScale))))
                                                : 0;
    const bool diagnosticOptionsChanged =
        _gazeDiagnosticOptionsInitialized &&
        (currentColorPointBypass != _gazePreviousCurrentColorPointBypass ||
         colorCopy != _gazePreviousColorCopy ||
         depthCopy != _gazePreviousDepthCopy ||
         omitBiasCurrentColorHint != _gazePreviousOmitBiasCurrentColorHint ||
         minimalPrivateParameters != _gazePreviousMinimalPrivateParameters ||
         resetOnMove != _gazePreviousResetOnMove);

    if (_gazeDiagnosticOptionsInitialized &&
        currentColorPointBypass != _gazePreviousCurrentColorPointBypass)
    {
        if (currentColorPointBypass)
        {
            LOG_WARN("[GROI_DIAGNOSTIC] CurrentColorPointBypass enabled: private DLSS Evaluate is skipped and the "
                     "ROI contains point-scaled current Color");
        }
        else
        {
            LOG_INFO("[GROI_DIAGNOSTIC] CurrentColorPointBypass disabled: private DLSS Evaluate restored");
        }
    }

    if (GazeRoi == nullptr)
        GazeRoi = std::make_unique<GazeRoi_Dx12>("GazeRoi", Device);
    if (GazeRoiMvPatch == nullptr)
        GazeRoiMvPatch = std::make_unique<GazeRoiMvPatch_Dx12>("GazeRoiMvPatch", Device);
    if (colorCopy && GazeRoiColorCrop == nullptr)
        GazeRoiColorCrop = std::make_unique<GazeRoiColorCrop_Dx12>("GazeRoiColorCrop", Device);
    if (depthCopy && GazeRoiDepthCrop == nullptr)
        GazeRoiDepthCrop = std::make_unique<GazeRoiDepthCrop_Dx12>("GazeRoiDepthCrop", Device);

    const auto outputDesc = output->GetDesc();
    bool privateOutputContractChanged = false;
    if (_p_gazeRoiDlssHandle != nullptr && GazeRoi != nullptr && GazeRoi->DlssOutput() != nullptr)
    {
        const auto privateOutputDesc = GazeRoi->DlssOutput()->GetDesc();
        const D3D12_RESOURCE_FLAGS expectedFlags =
            (outputDesc.Flags & ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) |
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        privateOutputContractChanged =
            privateOutputDesc.Width != outputRect.width || privateOutputDesc.Height != outputRect.height ||
            privateOutputDesc.Format != outputDesc.Format || privateOutputDesc.Flags != expectedFlags;
    }
    const bool privateHandleContractChanged =
        _p_gazeRoiDlssHandle != nullptr &&
        (_gazeRoiHandleInputRect.width != inputRect.width ||
         _gazeRoiHandleInputRect.height != inputRect.height ||
         _gazeRoiHandleOutputRect.width != outputRect.width ||
         _gazeRoiHandleOutputRect.height != outputRect.height || privateOutputContractChanged);
    if (privateHandleContractChanged)
        RetireGazeRoiDlssHandle();

    if (GazeRoi == nullptr || !GazeRoi->IsInit() ||
        !GazeRoi->CreateDlssOutputResource(Device, output, outputRect.width, outputRect.height,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        !GazeRoi->CreatePeripheralResource(Device, color, peripheralResolveWidth, peripheralResolveHeight,
                                            peripheralBlur || peripheralCurrentOnly, peripheralUsesHistory,
                                            peripheralTemporalMotionReprojection, peripheralJoint,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        !GazeRoi->CreatePeripheralDetailResources(Device, color, peripheralDetailWidth, peripheralDetailHeight,
                                                   peripheralTemporalDetail, peripheralJoint,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        GazeRoiMvPatch == nullptr || !GazeRoiMvPatch->IsInit() ||
         !GazeRoiMvPatch->CreatePatchedResource(Device, motionVectors, mvRect.width, mvRect.height,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        (colorCopy &&
         (GazeRoiColorCrop == nullptr || !GazeRoiColorCrop->IsInit() ||
          !GazeRoiColorCrop->CreateCroppedResource(Device, color, inputRect.width, inputRect.height,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS))) ||
        (depthCopy &&
         (GazeRoiDepthCrop == nullptr || !GazeRoiDepthCrop->IsInit() ||
          !GazeRoiDepthCrop->CreateCroppedResource(Device, depth, inputRect.width, inputRect.height,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS))))
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_PRIVATE_RESOURCE_CREATE");
        return false;
    }

    if (!currentColorPointBypass && !EnsureGazeRoiDlssHandle(InCommandList, InParameters, outputRect, inputRect))
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_PRIVATE_FEATURE_CREATE");
        return false;
    }

    ResTrack_Dx12::EnsureQueueHook(Device);
    uint32_t gazeRoiFrameSlot = 0;
    if (!GazeRoiFrameSync::Acquire(InCommandList, gazeRoiFrameSlot))
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_FRAME_SLOT_ACQUIRE");
        _gazeHasPreviousInputRect = false;
        return false;
    }
    const bool gpuTimingActive =
        GazeRoiFrameSync::BeginGpuTiming(Device, InCommandList, gazeRoiFrameSlot);
    if (gpuTimingActive)
        GazeRoiFrameSync::WriteGpuTimestamp(InCommandList, gazeRoiFrameSlot, 0);

    const GazeRoiRect& previousMvRect = lowResMotionVectors ? _gazePreviousInputRect : _gazePreviousOutputRect;
    const bool roiOriginMoved =
        _gazeHasPreviousInputRect &&
        (inputRect.x != _gazePreviousInputRect.x || inputRect.y != _gazePreviousInputRect.y ||
         outputRect.x != _gazePreviousOutputRect.x || outputRect.y != _gazePreviousOutputRect.y);
    const bool resetHistory = GetNgxValue<int>(InParameters, NVSDK_NGX_Parameter_Reset, 0) != 0 ||
                               _gazeRoiHandleWasCreated || !_gazeHasPreviousInputRect ||
                               !RectsOverlap(mvRect, previousMvRect) || diagnosticOptionsChanged ||
                               (resetOnMove && roiOriginMoved);
    const int32_t inputDeltaX = static_cast<int32_t>(mvRect.x) - static_cast<int32_t>(previousMvRect.x);
    const int32_t inputDeltaY = static_cast<int32_t>(mvRect.y) - static_cast<int32_t>(previousMvRect.y);
    const int32_t outputDeltaX = static_cast<int32_t>(outputRect.x) -
                                 static_cast<int32_t>(_gazePreviousOutputRect.x);
    const int32_t outputDeltaY = static_cast<int32_t>(outputRect.y) -
                                 static_cast<int32_t>(_gazePreviousOutputRect.y);
    float rawMvOffsetX = 0.0f;
    float rawMvOffsetY = 0.0f;
    if (!resetHistory && motionVectorMode != GazeRoiMotionVectorMode::Disabled)
    {
        const float outputToInputX = static_cast<float>(RenderWidth()) / static_cast<float>(TargetWidth());
        const float outputToInputY = static_cast<float>(RenderHeight()) / static_cast<float>(TargetHeight());
        const float outputDeltaInputX = static_cast<float>(outputDeltaX) * outputToInputX;
        const float outputDeltaInputY = static_cast<float>(outputDeltaY) * outputToInputY;
        switch (motionVectorMode)
        {
        case GazeRoiMotionVectorMode::InputDelta:
            rawMvOffsetX = static_cast<float>(inputDeltaX) / mvScaleX;
            rawMvOffsetY = static_cast<float>(inputDeltaY) / mvScaleY;
            break;
        case GazeRoiMotionVectorMode::InputDeltaReversed:
            rawMvOffsetX = -static_cast<float>(inputDeltaX) / mvScaleX;
            rawMvOffsetY = -static_cast<float>(inputDeltaY) / mvScaleY;
            break;
        case GazeRoiMotionVectorMode::InputDeltaUnscaled:
            rawMvOffsetX = static_cast<float>(inputDeltaX);
            rawMvOffsetY = static_cast<float>(inputDeltaY);
            break;
        case GazeRoiMotionVectorMode::OutputDelta:
            rawMvOffsetX = outputDeltaInputX / mvScaleX;
            rawMvOffsetY = outputDeltaInputY / mvScaleY;
            break;
        case GazeRoiMotionVectorMode::OutputDeltaReversed:
            rawMvOffsetX = -outputDeltaInputX / mvScaleX;
            rawMvOffsetY = -outputDeltaInputY / mvScaleY;
            break;
        case GazeRoiMotionVectorMode::Disabled:
        default:
            break;
        }
    }

    const auto motionVectorDesc = motionVectors->GetDesc();
    const auto privateOutputDesc = GazeRoi->DlssOutput()->GetDesc();
    LOG_INFO("[GROI_CONTRACT] mv mode={} resourceMode=cropped-copy lowRes={} format={} sourceResource={}x{} "
             "patchSourceBase=({}, {}) croppedResource={}x{} mvSubrectBase=(0, 0) "
             "outputMode=local-resource privateOutput={}x{} outputSubrectBase=(0, 0) "
             "finalOutput={}x{} finalRoiBase=({}, {}) "
             "roi=({}, {}, {}x{}) outputDelta=({}, {}) "
             "previous=({}, {}, {}x{}) delta=({}, {}) scale=({}, {}) rawOffset=({}, {}) injectionMode={} reset={} "
             "resetOnMove={} roiOriginMoved={} debugView={} debugRect={}x{} outputClearDebug={}",
             GazeRoiMotionVectorModeName(motionVectorMode), lowResMotionVectors,
             static_cast<uint32_t>(motionVectorDesc.Format), motionVectorDesc.Width, motionVectorDesc.Height,
             roiMvBaseX, roiMvBaseY, mvRect.width, mvRect.height, privateOutputDesc.Width, privateOutputDesc.Height,
             outputDesc.Width, outputDesc.Height, finalRoiOutputBaseX, finalRoiOutputBaseY, mvRect.x, mvRect.y,
             mvRect.width, mvRect.height, outputDeltaX, outputDeltaY,
             previousMvRect.x, previousMvRect.y, previousMvRect.width, previousMvRect.height,
             static_cast<int32_t>(mvRect.x) - static_cast<int32_t>(previousMvRect.x),
             static_cast<int32_t>(mvRect.y) - static_cast<int32_t>(previousMvRect.y), mvScaleX, mvScaleY,
             rawMvOffsetX, rawMvOffsetY, GazeRoiMotionVectorModeName(motionVectorMode), resetHistory, resetOnMove,
             roiOriginMoved,
             Config::Instance()->GazeRoiMotionVectorDebugView.value_or_default(),
             std::min(mvRect.width, TargetWidth()), std::min(mvRect.height, TargetHeight()), outputClearDebug);

    ID3D12Resource* effectiveMotionVectors = GazeRoiMvPatch->PatchedMotionVectors();
    {
        GazeRoiMvConstants mvConstants {};
        mvConstants.width = static_cast<int32_t>(mvRect.width);
        mvConstants.height = static_cast<int32_t>(mvRect.height);
        mvConstants.sourceBaseX = static_cast<int32_t>(roiMvBaseX);
        mvConstants.sourceBaseY = static_cast<int32_t>(roiMvBaseY);
        mvConstants.rawOffsetX = rawMvOffsetX;
        mvConstants.rawOffsetY = rawMvOffsetY;

        GazeRoiMvPatch->SetPatchedState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!GazeRoiMvPatch->Dispatch(InCommandList, motionVectors, mvConstants, gazeRoiFrameSlot))
        {
            LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_MV_PATCH_DISPATCH");
            _gazeHasPreviousInputRect = false;
            return false;
        }
        GazeRoiMvPatch->SetPatchedState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        effectiveMotionVectors = GazeRoiMvPatch->PatchedMotionVectors();
    }
    if (gpuTimingActive)
        GazeRoiFrameSync::WriteGpuTimestamp(InCommandList, gazeRoiFrameSlot, 1);
    ID3D12Resource* effectiveColor = color;
    uint32_t effectiveColorBaseX = roiColorBaseX;
    uint32_t effectiveColorBaseY = roiColorBaseY;
    const auto colorDesc = color->GetDesc();
    if (colorCopy)
    {
        GazeRoiColorConstants colorConstants {};
        colorConstants.sourceWidth = static_cast<int32_t>(inputRect.width);
        colorConstants.sourceHeight = static_cast<int32_t>(inputRect.height);
        colorConstants.sourceBaseX = static_cast<int32_t>(roiColorBaseX);
        colorConstants.sourceBaseY = static_cast<int32_t>(roiColorBaseY);
        colorConstants.outputWidth = static_cast<int32_t>(inputRect.width);
        colorConstants.outputHeight = static_cast<int32_t>(inputRect.height);

        GazeRoiColorCrop->SetCroppedState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!GazeRoiColorCrop->Dispatch(InCommandList, color, colorConstants, gazeRoiFrameSlot))
        {
            LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_COLOR_CROP_DISPATCH");
            _gazeHasPreviousInputRect = false;
            return false;
        }
        GazeRoiColorCrop->SetCroppedState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        effectiveColor = GazeRoiColorCrop->CroppedColor();
        effectiveColorBaseX = 0;
        effectiveColorBaseY = 0;
        const auto effectiveColorDesc = effectiveColor->GetDesc();
        LOG_INFO("[GROI_CONTRACT] color mode=cropped-copy sourceResource={}x{} format={} sourceBase=({}, {}) "
                 "effectiveResource={}x{} format={} colorSubrectBase=(0, 0)",
                 colorDesc.Width, colorDesc.Height, static_cast<uint32_t>(colorDesc.Format), roiColorBaseX,
                 roiColorBaseY, effectiveColorDesc.Width, effectiveColorDesc.Height,
                 static_cast<uint32_t>(effectiveColorDesc.Format));
    }
    else
    {
        LOG_INFO("[GROI_CONTRACT] color mode=source-subrect sourceResource={}x{} format={} "
                 "colorSubrectBase=({}, {}) subrect={}x{}",
                 colorDesc.Width, colorDesc.Height, static_cast<uint32_t>(colorDesc.Format), effectiveColorBaseX,
                 effectiveColorBaseY, inputRect.width, inputRect.height);
    }
    ID3D12Resource* effectiveDepth = depth;
    // This replacement runs below Streamline at the NGX layer, where the
    // equivalent of a ResourceTag extent is the input subrect base/dimensions.
    // Never mutate the game's Streamline ResourceTag for this private call.
    uint32_t effectiveDepthBaseX = roiDepthBaseX;
    uint32_t effectiveDepthBaseY = roiDepthBaseY;
    const auto depthDesc = depth->GetDesc();
    if (depthCopy)
    {
        GazeRoiDepthConstants depthConstants {};
        depthConstants.width = static_cast<int32_t>(inputRect.width);
        depthConstants.height = static_cast<int32_t>(inputRect.height);
        depthConstants.sourceBaseX = static_cast<int32_t>(roiDepthBaseX);
        depthConstants.sourceBaseY = static_cast<int32_t>(roiDepthBaseY);

        GazeRoiDepthCrop->SetCroppedState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!GazeRoiDepthCrop->Dispatch(InCommandList, depth, depthConstants, gazeRoiFrameSlot))
        {
            LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_DEPTH_CROP_DISPATCH");
            _gazeHasPreviousInputRect = false;
            return false;
        }
        GazeRoiDepthCrop->SetCroppedState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        effectiveDepth = GazeRoiDepthCrop->CroppedDepth();
        effectiveDepthBaseX = 0;
        effectiveDepthBaseY = 0;
        const auto effectiveDepthDesc = effectiveDepth->GetDesc();
        LOG_INFO("[GROI_CONTRACT] depth mode=cropped-copy sourceResource={}x{} format={} sourceBase=({}, {}) "
                 "effectiveResource={}x{} format={} depthSubrectBase=(0, 0)",
                 depthDesc.Width, depthDesc.Height, static_cast<uint32_t>(depthDesc.Format), roiDepthBaseX,
                 roiDepthBaseY, effectiveDepthDesc.Width, effectiveDepthDesc.Height,
                 static_cast<uint32_t>(effectiveDepthDesc.Format));
    }
    else
    {
        LOG_INFO("[GROI_CONTRACT] depth mode=source-subrect sourceResource={}x{} format={} "
                 "depthSubrectBase=({}, {}) subrect={}x{}",
                 depthDesc.Width, depthDesc.Height, static_cast<uint32_t>(depthDesc.Format), effectiveDepthBaseX,
                 effectiveDepthBaseY, inputRect.width, inputRect.height);
    }
    LOG_INFO("[GROI_CONTRACT] parameterMode={} biasCurrentColor mode={} sourcePresent={} effectivePresent={} "
             "privateOutputMode={} colorCopy={} optionsChanged={}",
             minimalPrivateParameters ? "minimal-native" : "inherited-native",
             minimalPrivateParameters ? "not-read" : (omitBiasCurrentColorHint ? "omitted" : "passthrough-subrect"),
              biasCurrentColor != nullptr, biasCurrentColor != nullptr && !omitBiasCurrentColorHint,
             currentColorPointBypass ? "current-color-point-bypass" : "private-dlss",
             colorCopy,
             diagnosticOptionsChanged);
    GazeRoi->SetDlssOutputState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (gpuTimingActive)
        GazeRoiFrameSync::WriteGpuTimestamp(InCommandList, gazeRoiFrameSlot, 2);
    if (currentColorPointBypass)
    {
        GazeRoiColorConstants bypassConstants {};
        bypassConstants.sourceWidth = static_cast<int32_t>(inputRect.width);
        bypassConstants.sourceHeight = static_cast<int32_t>(inputRect.height);
        bypassConstants.sourceBaseX = static_cast<int32_t>(effectiveColorBaseX);
        bypassConstants.sourceBaseY = static_cast<int32_t>(effectiveColorBaseY);
        bypassConstants.outputWidth = static_cast<int32_t>(outputRect.width);
        bypassConstants.outputHeight = static_cast<int32_t>(outputRect.height);
        if (!GazeRoi->DispatchCurrentColorPointBypass(InCommandList, effectiveColor, bypassConstants,
                                                       gazeRoiFrameSlot))
        {
            LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_CURRENT_COLOR_POINT_BYPASS source={}x{}+{},{} output={}x{}",
                      inputRect.width, inputRect.height, effectiveColorBaseX, effectiveColorBaseY,
                      outputRect.width, outputRect.height);
            _gazeHasPreviousInputRect = false;
            return false;
        }

        nvResult = NVSDK_NGX_Result_Success;
        LOG_INFO("[GROI_CONTRACT] privateOutputMode=current-color-point-bypass evaluateSkipped=true "
                 "source={}x{}+{},{} output={}x{} colorCopy={} jitterForwarded=false",
                 inputRect.width, inputRect.height, effectiveColorBaseX, effectiveColorBaseY,
                 outputRect.width, outputRect.height, colorCopy);
    }
    else
    {
    if (outputClearDebug)
    {
        const GazeRoiRect clearRect { 0, 0, outputRect.width, outputRect.height };
        if (!GazeRoi->ClearDlssOutputRectMagenta(InCommandList, clearRect, gazeRoiFrameSlot))
        {
            LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_OUTPUT_DEBUG_CLEAR rect=({}, {}, {}x{})", clearRect.x,
                      clearRect.y, clearRect.width, clearRect.height);
            _gazeHasPreviousInputRect = false;
            return false;
        }
    }

    NVSDK_NGX_Parameter* evaluateParameters = InParameters;
    std::unique_ptr<NgxParameterScope> evaluateScope;
    if (minimalPrivateParameters)
    {
        if (!EnsureGazeRoiMinimalParameters())
        {
            LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_MINIMAL_PARAMS_EVALUATE");
            _gazeHasPreviousInputRect = false;
            return false;
        }

        evaluateParameters = _gazeMinimalParameters;
        evaluateParameters->Reset();
        evaluateParameters->Set(NVSDK_NGX_Parameter_Color, effectiveColor);
        evaluateParameters->Set(NVSDK_NGX_Parameter_Output, GazeRoi->DlssOutput());
        evaluateParameters->Set(NVSDK_NGX_Parameter_Depth, effectiveDepth);
        evaluateParameters->Set(NVSDK_NGX_Parameter_MotionVectors, effectiveMotionVectors);
        evaluateParameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitterOffsetX);
        evaluateParameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitterOffsetY);
        evaluateParameters->Set(NVSDK_NGX_Parameter_Sharpness, sharpness);
        evaluateParameters->Set(NVSDK_NGX_Parameter_Reset, resetHistory ? 1 : 0);
        evaluateParameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, mvScaleX);
        evaluateParameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, mvScaleY);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, effectiveColorBaseX);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, effectiveColorBaseY);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, effectiveDepthBaseX);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, effectiveDepthBaseY);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0U);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0U);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0U);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0U);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, inputRect.width);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, inputRect.height);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure,
                                preExposure == 0.0f ? 1.0f : preExposure);
        evaluateParameters->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale,
                                exposureScale == 0.0f ? 1.0f : exposureScale);
    }
    else
    {
        evaluateScope = std::make_unique<NgxParameterScope>(InParameters, "evaluate");
        evaluateScope->Override(NVSDK_NGX_Parameter_Width, inputRect.width, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_Height, inputRect.height, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_OutWidth, outputRect.width, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_OutHeight, outputRect.height, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_PerfQualityValue, perfQuality, "int");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, featureFlags, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1, "int");
        evaluateScope->Override(NVSDK_NGX_Parameter_Color, effectiveColor, "d3d12");
        evaluateScope->Override(NVSDK_NGX_Parameter_Depth, effectiveDepth, "d3d12");
        evaluateScope->Override(NVSDK_NGX_Parameter_MotionVectors, effectiveMotionVectors, "d3d12");
        evaluateScope->Override(NVSDK_NGX_Parameter_Output, GazeRoi->DlssOutput(), "d3d12");
        evaluateScope->Override(NVSDK_NGX_Parameter_Reset, resetHistory ? 1 : 0, "int");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, effectiveColorBaseX, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, effectiveColorBaseY, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, effectiveDepthBaseX, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, effectiveDepthBaseY, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0U, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0U, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0U, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0U, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, inputRect.width, "uint");
        evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, inputRect.height, "uint");
        if (transparency != nullptr)
        {
            evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X,
                                    transparencyBaseX, "uint");
            evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y,
                                    transparencyBaseY, "uint");
        }
        if (biasCurrentColor != nullptr && omitBiasCurrentColorHint)
        {
            evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
                                    static_cast<ID3D12Resource*>(nullptr), "d3d12");
        }
        else if (biasCurrentColor != nullptr)
        {
            evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,
                                    biasBaseX, "uint");
            evaluateScope->Override(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y,
                                    biasBaseY, "uint");
        }
    }
    TraceNgxKnownParameters(evaluateParameters, "evaluate-effective");

    nvResult = NVNGXProxy::D3D12_EvaluateFeature()(InCommandList, _p_gazeRoiDlssHandle,
                                                   evaluateParameters, NULL);
    TraceNgxKnownParameters(evaluateParameters, "evaluate-output");
    if (nvResult != NVSDK_NGX_Result_Success)
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_PRIVATE_EVALUATE result={:X} lowResMV={} roi={}x{}->{}x{} "
                  "mvResource={}x{} mvSubrectBase={},{} privateOutput={}x{} outputSubrectBase=0,0 "
                  "mvOffset={},{} reset={}",
                  static_cast<unsigned int>(nvResult), lowResMotionVectors, inputRect.width, inputRect.height,
                  outputRect.width, outputRect.height, motionVectorDesc.Width, motionVectorDesc.Height, roiMvBaseX,
                  roiMvBaseY, privateOutputDesc.Width, privateOutputDesc.Height, rawMvOffsetX, rawMvOffsetY,
                  resetHistory);
        _gazeHasPreviousInputRect = false;
        return true;
    }
    }

    if (gpuTimingActive)
        GazeRoiFrameSync::WriteGpuTimestamp(InCommandList, gazeRoiFrameSlot, 3);

    GazeRoi->SetDlssOutputState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    GazeRoiConstants constants {};
    constants.srcWidth = static_cast<int32_t>(RenderWidth());
    constants.srcHeight = static_cast<int32_t>(RenderHeight());
    constants.srcTextureWidth = static_cast<int32_t>(colorDesc.Width);
    constants.srcTextureHeight = static_cast<int32_t>(colorDesc.Height);
    constants.srcBaseX = static_cast<int32_t>(colorBaseX);
    constants.srcBaseY = static_cast<int32_t>(colorBaseY);
    constants.dstWidth = static_cast<int32_t>(TargetWidth());
    constants.dstHeight = static_cast<int32_t>(TargetHeight());
    constants.dstBaseX = static_cast<int32_t>(outputBaseX);
    constants.dstBaseY = static_cast<int32_t>(outputBaseY);
    constants.roiX = static_cast<int32_t>(outputRect.x);
    constants.roiY = static_cast<int32_t>(outputRect.y);
    constants.roiWidth = static_cast<int32_t>(outputRect.width);
    constants.roiHeight = static_cast<int32_t>(outputRect.height);
    constants.featherPx = Config::Instance()->GazeRoiFeatherPx.value_or_default();
    constants.debugBorderPx = Config::Instance()->GazeRoiDebugBorder.value_or_default() ? 2 : 0;
    constants.peripheralBlur = peripheralBlur ? 1 : 0;
    constants.peripheralBlurRadius = std::clamp(Config::Instance()->GazeRoiPeripheralBlurRadius.value_or_default(),
                                                 0.0f, 3.0f);
    constants.peripheralJitterCancel = Config::Instance()->GazeRoiPeripheralJitterCancel.value_or_default() ? 1 : 0;
    constants.peripheralJitterSign = Config::Instance()->GazeRoiPeripheralJitterSign.value_or_default() < 0 ? -1 : 1;
    constants.jitterOffsetX = jitterOffsetX;
    constants.jitterOffsetY = jitterOffsetY;
    constants.peripheralTemporal = peripheralTemporal;
    constants.peripheralTemporalMotionReprojection = peripheralTemporalMotionReprojection ? 1 : 0;
    constants.peripheralTemporalHistoryReset = resetHistory ? 1 : 0;
    constants.peripheralDepthBaseX = static_cast<int32_t>(depthBaseX);
    constants.peripheralDepthBaseY = static_cast<int32_t>(depthBaseY);
    constants.peripheralMotionVectorBaseX = static_cast<int32_t>(originalMvBaseX);
    constants.peripheralMotionVectorBaseY = static_cast<int32_t>(originalMvBaseY);
    constants.peripheralMotionVectorWidth = static_cast<int32_t>(lowResMotionVectors ? RenderWidth() : TargetWidth());
    constants.peripheralMotionVectorHeight = static_cast<int32_t>(lowResMotionVectors ? RenderHeight() : TargetHeight());
    constants.peripheralMotionVectorScaleX = mvScaleX;
    constants.peripheralMotionVectorScaleY = mvScaleY;
    constants.peripheralMotionVectorsLowResolution = lowResMotionVectors ? 1 : 0;
    constants.peripheralMotionVectorsJittered =
        (featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0 ? 1 : 0;
    constants.previousJitterOffsetX = _gazePreviousJitterOffsetX;
    constants.previousJitterOffsetY = _gazePreviousJitterOffsetY;
    const float temporalHistoryWeight =
        std::clamp(Config::Instance()->GazeRoiPeripheralTemporalHistoryWeight.value_or_default(), 0.0f, 1.0f);
    constants.peripheralTemporalHistoryWeight = temporalHistoryWeight;
    // History is accepted only after MV/depth validation. Reactive rejection then
    // attenuates history on genuine current-frame changes without making the UI
    // control ineffective on noisy pixels.
    constants.peripheralTemporalReactiveScale = std::clamp(
        Config::Instance()->GazeRoiPeripheralTemporalReactiveScale.value_or_default(), 0.0f, 16.0f);
    constants.motionVectorDebugView = Config::Instance()->GazeRoiMotionVectorDebugView.value_or_default() ? 1 : 0;
    constants.motionVectorWidth = static_cast<int32_t>(mvRect.width);
    constants.motionVectorHeight = static_cast<int32_t>(mvRect.height);
    constants.motionVectorScaleX = mvScaleX;
    constants.motionVectorScaleY = mvScaleY;
    constants.peripheralResolveWidth = static_cast<int32_t>(peripheralResolveWidth);
    constants.peripheralResolveHeight = static_cast<int32_t>(peripheralResolveHeight);
    const float safePreExposure = std::isfinite(preExposure) && preExposure > 1.0e-6f ? preExposure : 1.0f;
    const float previousPreExposure = _gazeHasPreviousInputRect ? _gazePreviousPreExposure : safePreExposure;
    // EASU is retired. Preserve the fixed root-constant layout by using its
    // unused tail for inline TAAU detail strength and exposure scalars.
    constants.easuConst0[1] = std::bit_cast<uint32_t>(peripheralTemporalDetailStrength);
    constants.easuConst0[3] = std::bit_cast<uint32_t>(safePreExposure);
    constants.easuConst1[0] = std::bit_cast<uint32_t>(previousPreExposure);

    if (gpuTimingActive)
        GazeRoiFrameSync::WriteGpuTimestamp(InCommandList, gazeRoiFrameSlot, 4);
    if (!GazeRoi->Dispatch(InCommandList, color, depth, motionVectors, effectiveMotionVectors, output, constants,
                           gazeRoiFrameSlot, peripheralTemporalDetail, peripheralDetailWidth,
                           peripheralDetailHeight, peripheralTemporalDetailStrength, peripheralEasu))
    {
        LOG_ERROR("[GROI_CONTRACT] GROI_FAIL_COMPOSITE_DISPATCH");
        nvResult = NVSDK_NGX_Result_Fail;
        _gazeHasPreviousInputRect = false;
        return true;
    }
    if (gpuTimingActive)
    {
        GazeRoiFrameSync::WriteGpuTimestamp(InCommandList, gazeRoiFrameSlot, 6);
        GazeRoiFrameSync::ResolveGpuTiming(InCommandList, gazeRoiFrameSlot);
    }

    _gazePreviousInputRect = inputRect;
    _gazePreviousOutputRect = outputRect;
    _gazeHasPreviousInputRect = true;
    _gazeDiagnosticOptionsInitialized = true;
    _gazePreviousCurrentColorPointBypass = currentColorPointBypass;
    _gazePreviousColorCopy = colorCopy;
    _gazePreviousDepthCopy = depthCopy;
    _gazePreviousOmitBiasCurrentColorHint = omitBiasCurrentColorHint;
    _gazePreviousMinimalPrivateParameters = minimalPrivateParameters;
    _gazePreviousResetOnMove = resetOnMove;
    _gazePreviousJitterOffsetX = jitterOffsetX;
    _gazePreviousJitterOffsetY = jitterOffsetY;
    _gazePreviousPreExposure = safePreExposure;
    const char* detailPath = "off";
    if (peripheralIntermediateTaau)
        detailPath = "inline-taau";
    else if (peripheralTemporalDetail)
        detailPath = peripheralJoint
                         ? "joint"
                         : (peripheralDetailWidth == peripheralResolveWidth &&
                                    peripheralDetailHeight == peripheralResolveHeight
                                ? "fused"
                                : "separate");
    LOG_DEBUG("Gaze ROI replacement: lowResMV={} out {}x{}+{},{} input {}x{}+{},{} rawMvOffset {},{} reset={} "
              "privateOutputMode={} colorCopy={} peripheralMode={} peripheralEasu={} temporalDetail={} "
              "detailPath={} detailScale={:.2f} detailStrength={:.2f} peripheralResolve={}x{} detailResolve={}x{}",
              lowResMotionVectors, outputRect.width, outputRect.height, outputRect.x, outputRect.y, inputRect.width,
              inputRect.height, inputRect.x, inputRect.y, rawMvOffsetX, rawMvOffsetY, resetHistory,
              currentColorPointBypass ? "current-color-point-bypass" : "private-dlss", colorCopy,
              static_cast<int>(peripheralMode), peripheralEasu, peripheralTemporalDetail,
              detailPath, peripheralTemporalDetailScale, peripheralTemporalDetailStrength, peripheralResolveWidth,
              peripheralResolveHeight, peripheralDetailWidth, peripheralDetailHeight);
    return true;
}

void DLSSFeatureDx12::Shutdown(ID3D12Device* InDevice)
{
    GazeRoiFrameSync::FlushDeferred();

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

    GazeRoiFrameSync::FlushDeferred();

    if (NVNGXProxy::D3D12_ReleaseFeature() != nullptr && _p_gazeRoiDlssHandle != nullptr)
        NVNGXProxy::D3D12_ReleaseFeature()(_p_gazeRoiDlssHandle);

    if (_gazeMinimalParameters != nullptr)
    {
        if (const auto destroyParameters = NVNGXProxy::D3D12_DestroyParameters(); destroyParameters != nullptr)
        {
            const NVSDK_NGX_Result result = destroyParameters(_gazeMinimalParameters);
            if (result != NVSDK_NGX_Result_Success)
                LOG_WARN("[GROI_CONTRACT] minimal private parameter destroy failed result={:X}",
                         static_cast<unsigned int>(result));
        }
        else
        {
            LOG_WARN("[GROI_CONTRACT] minimal private parameter destroy API unavailable; leaking table");
        }
        _gazeMinimalParameters = nullptr;
    }

    if (NVNGXProxy::D3D12_ReleaseFeature() != nullptr && _p_dlssHandle != nullptr)
        NVNGXProxy::D3D12_ReleaseFeature()(_p_dlssHandle);
}
