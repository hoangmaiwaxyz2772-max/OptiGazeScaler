#include "pch.h"

#include "NVNGX_Parameter.h"

#include "SysUtils.h"
#include "Config.h"
#include <ankerl/unordered_dense.h>
#include <misc/IdentifyGpu.h>
#include <framegen/nvngx/Nvngx_FG.h>
#include <upscalers/dlssnr/DLSSNRMethodHooks.h>
#include <upscalers/dlssnr/DLSSNRNativeParameters.h>

namespace
{
struct NativeParameterRecord
{
    std::unordered_map<std::string, Parameter> pointers;
    bool complete = true;
};
std::mutex nativeParameterMutex;
std::unordered_map<const NVSDK_NGX_Parameter*, NativeParameterRecord> nativeParameters;
thread_local unsigned nativeParameterDepth = 0;
struct NativeParameterCall
{
    bool outer = nativeParameterDepth++ == 0;
    ~NativeParameterCall() { --nativeParameterDepth; }
};

const char* CanonicalNativePointerKey(const char* key)
{
    // Native NGX accepts compact and readable spellings for the same entry.
    // Keep one shadow slot so an overwrite cannot retain an obsolete pointer.
    static constexpr std::pair<std::string_view, const char*> aliases[] {
        {NVSDK_NGX_EParameter_Color, NVSDK_NGX_Parameter_Color},
        {NVSDK_NGX_EParameter_Output, NVSDK_NGX_Parameter_Output},
        {NVSDK_NGX_EParameter_Depth, NVSDK_NGX_Parameter_Depth},
        {NVSDK_NGX_EParameter_MotionVectors, NVSDK_NGX_Parameter_MotionVectors},
        {NVSDK_NGX_EParameter_Albedo, NVSDK_NGX_Parameter_Albedo},
        {NVSDK_NGX_EParameter_ResourceAllocCallback, NVSDK_NGX_Parameter_ResourceAllocCallback},
        {NVSDK_NGX_EParameter_ResourceReleaseCallback, NVSDK_NGX_Parameter_ResourceReleaseCallback},
        {NVSDK_NGX_EParameter_DLSSOptimalSettingsCallback, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback}
    };
    for (const auto& [compact, readable] : aliases) if (compact == key) return readable;
    return key;
}

template<unsigned Slot, class T> struct NativeParameterSet
{
    using Fn = void(STDMETHODCALLTYPE*)(NVSDK_NGX_Parameter*, const char*, T);
    using Hook = DLSSNRMethodHooks::MethodHook<36000 + Slot, Fn>;
    static void STDMETHODCALLTYPE Call(NVSDK_NGX_Parameter* parameters, const char* key, T value)
    {
        NativeParameterCall scope;
        Hook::Forward(parameters, key, value);
        if (!scope.outer || !key) return;
        key = CanonicalNativePointerKey(key);
        std::lock_guard lock(nativeParameterMutex);
        const auto found = nativeParameters.find(parameters);
        if (found == nativeParameters.end()) return;
        auto& record = found->second;
        if constexpr (std::is_pointer_v<T>)
        {
            if (!value) record.pointers.erase(key);
            else if (record.pointers.size() < 4096 || record.pointers.contains(key)) record.pointers[key] = value;
            else record.complete = false;
        }
        else record.pointers.erase(key); // Numeric overwrite removes a previous resource.
    }
    static bool Install(void** table) { return Hook::Install(table[Slot], Call) == NO_ERROR; }
};
using NativeResetHook = DLSSNRMethodHooks::MethodHook<36016, void(STDMETHODCALLTYPE*)(NVSDK_NGX_Parameter*)>;
void STDMETHODCALLTYPE NativeParameterReset(NVSDK_NGX_Parameter* parameters)
{
    NativeParameterCall scope;
    NativeResetHook::Forward(parameters);
    if (!scope.outer) return;
    std::lock_guard lock(nativeParameterMutex);
    if (auto it = nativeParameters.find(parameters); it != nativeParameters.end()) it->second = {};
}

bool ParameterResources(const auto& values, std::vector<ID3D12Resource*>& resources, std::string& unknown)
{
    bool complete = true;
    for (const auto& [name, value] : values)
    {
        if (value.pointerKind == Parameter::PointerKind::None || !value.values.vp) continue;
        // The NGX C API also accepts resource keys through SetVoidPointer.
        // Classify by its public key contract; never QueryInterface an arbitrary
        // callback/CPU-data pointer to guess whether it is a resource.
        static constexpr std::string_view resourceKeys[] {
            NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Output, NVSDK_NGX_Parameter_Depth,
            NVSDK_NGX_Parameter_MotionVectors, NVSDK_NGX_Parameter_ExposureTexture,
            NVSDK_NGX_Parameter_TransparencyMask, NVSDK_NGX_Parameter_Albedo,
            NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
            NVSDK_NGX_Parameter_MotionVectors3D, NVSDK_NGX_Parameter_DepthHighRes,
            NVSDK_NGX_Parameter_MotionVectorsReflection, NVSDK_NGX_Parameter_IsParticleMask,
            NVSDK_NGX_Parameter_AnimatedTextureMask, NVSDK_NGX_Parameter_RayTracingHitDistance,
            NVSDK_NGX_EParameter_Color, NVSDK_NGX_EParameter_Output, NVSDK_NGX_EParameter_Depth,
            NVSDK_NGX_EParameter_MotionVectors, NVSDK_NGX_EParameter_Albedo,
            NVSDK_NGX_Parameter_GBuffer_Normals, NVSDK_NGX_Parameter_GBuffer_Albedo,
            NVSDK_NGX_Parameter_GBuffer_Roughness, NVSDK_NGX_Parameter_GBuffer_DiffuseAlbedo,
            NVSDK_NGX_Parameter_GBuffer_SpecularAlbedo, NVSDK_NGX_Parameter_GBuffer_IndirectAlbedo,
            NVSDK_NGX_Parameter_GBuffer_SpecularMvec, NVSDK_NGX_Parameter_GBuffer_DisocclusionMask
        };
        if (value.pointerKind == Parameter::PointerKind::D3D12 ||
            (value.pointerKind == Parameter::PointerKind::Untyped &&
             std::find(std::begin(resourceKeys), std::end(resourceKeys), name) != std::end(resourceKeys)))
            resources.push_back(static_cast<ID3D12Resource*>(value.values.vp));
        else if (value.pointerKind == Parameter::PointerKind::Untyped &&
                 (name == NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback ||
                  name == NVSDK_NGX_EParameter_DLSSOptimalSettingsCallback ||
                  name == NVSDK_NGX_Parameter_DLSSGetStatsCallback || name == "DLSSDOptimalSettingsCallback" ||
                  name == NVSDK_NGX_Parameter_ResourceAllocCallback ||
                  name == NVSDK_NGX_EParameter_ResourceAllocCallback ||
                  name == NVSDK_NGX_Parameter_ResourceReleaseCallback ||
                  name == NVSDK_NGX_EParameter_ResourceReleaseCallback ||
                  name == NVSDK_NGX_Parameter_DLSS_INV_VIEW_PROJECTION_MATRIX ||
                  name == NVSDK_NGX_Parameter_DLSS_CLIP_TO_PREV_CLIP_MATRIX))
        { /* SDK callbacks and CPU matrices do not identify GPU resources.
             Alloc/release callbacks supply SDK-private allocations, covered by
             the native-call domain; these function pointers are not resources. */ }
        else
        {
            complete = false;
            // Keep all key classes in one sampled failure, with an explicit
            // bound instead of exposing only the next key after each repair.
            if (unknown.size() < 2048) { if (!unknown.empty()) unknown += ','; unknown += name; }
            else if (!unknown.ends_with(",truncated")) unknown += ",truncated";
        }
    }
    return complete;
}
}

void DLSSNRNativeParameters::Register(NVSDK_NGX_Parameter* parameters, bool fresh)
{
    if (!parameters || !Config::Instance()->DLSSNRPipelineAsync.value_or_default()) return;
    auto** table = *reinterpret_cast<void***>(parameters);
    // MSVC groups overloads in reverse declaration order in this NGX ABI.
    // Set(void*) is slot 0, Set(unsigned long long) slot 7; Reset is slot 16.
    // Install outside the metadata mutex: Detours enlists other game threads.
    bool ready = NativeParameterSet<0, void*>::Install(table);
    ready &= NativeParameterSet<1, ID3D12Resource*>::Install(table);
    ready &= NativeParameterSet<2, ID3D11Resource*>::Install(table);
    ready &= NativeParameterSet<3, int>::Install(table);
    ready &= NativeParameterSet<4, unsigned int>::Install(table);
    ready &= NativeParameterSet<5, double>::Install(table);
    ready &= NativeParameterSet<6, float>::Install(table);
    ready &= NativeParameterSet<7, unsigned long long>::Install(table);
    ready &= NativeResetHook::Install(table[16], NativeParameterReset) == NO_ERROR;
    {
        std::lock_guard lock(nativeParameterMutex);
        if (!ready) nativeParameters.erase(parameters);
        else if (fresh) nativeParameters[parameters] = {};
        else
        {
            const auto [it, inserted] = nativeParameters.try_emplace(parameters);
            // Legacy GetParameters can return an SDK-owned table used before
            // interception. Only Reset establishes an empty observed history.
            if (inserted) it->second.complete = false;
        }
    }
    LOG_INFO("[DLSSNR_ASYNC] native-parameters=0x{:X} tracking={} fresh={}", (uintptr_t)parameters, ready, fresh);
}
void DLSSNRNativeParameters::Forget(NVSDK_NGX_Parameter* parameters)
{
    std::lock_guard lock(nativeParameterMutex);
    nativeParameters.erase(parameters);
}

/// @brief Calculates the resolution scaling ratio override based on the provided quality level and current
/// configuration.
/// @param input The performance quality value (e.g. Quality, Balanced, Performance).
/// @return An optional float containing the ratio if an override applies.
std::optional<float> GetQualityOverrideRatio(const NVSDK_NGX_PerfQuality_Value input)
{
    std::optional<float> output;

    auto sliderLimit = Config::Instance()->ExtendedLimits.value_or_default() ? 0.1f : 1.0f;

    if (Config::Instance()->UpscaleRatioOverrideEnabled.value_or_default() &&
        Config::Instance()->UpscaleRatioOverrideValue.value_or_default() >= sliderLimit)
    {
        output = Config::Instance()->UpscaleRatioOverrideValue.value_or_default();

        return output;
    }

    if (!Config::Instance()->QualityRatioOverrideEnabled.value_or_default())
        return output; // override not enabled

    switch (input)
    {
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
        if (Config::Instance()->QualityRatio_UltraPerformance.value_or_default() >= sliderLimit)
            output = Config::Instance()->QualityRatio_UltraPerformance.value_or_default();

        break;

    case NVSDK_NGX_PerfQuality_Value_MaxPerf:
        if (Config::Instance()->QualityRatio_Performance.value_or_default() >= sliderLimit)
            output = Config::Instance()->QualityRatio_Performance.value_or_default();

        break;

    case NVSDK_NGX_PerfQuality_Value_Balanced:
        if (Config::Instance()->QualityRatio_Balanced.value_or_default() >= sliderLimit)
            output = Config::Instance()->QualityRatio_Balanced.value_or_default();

        break;

    case NVSDK_NGX_PerfQuality_Value_MaxQuality:
        if (Config::Instance()->QualityRatio_Quality.value_or_default() >= sliderLimit)
            output = Config::Instance()->QualityRatio_Quality.value_or_default();

        break;

    case NVSDK_NGX_PerfQuality_Value_UltraQuality:
        if (Config::Instance()->QualityRatio_UltraQuality.value_or_default() >= sliderLimit)
            output = Config::Instance()->QualityRatio_UltraQuality.value_or_default();

        break;

    case NVSDK_NGX_PerfQuality_Value_DLAA:
        if (Config::Instance()->QualityRatio_DLAA.value_or_default() >= sliderLimit)
            output = Config::Instance()->QualityRatio_DLAA.value_or_default();

        break;

    default:
        LOG_WARN("Unknown quality: {0}", (int) input);
        break;
    }

    return output;
}

NVNGX_Parameters::NVNGX_Parameters(std::string_view name, bool isPersistent) : Name(name)
{
    implementation.store(*reinterpret_cast<void**>(this), std::memory_order_release);
    // Old flag used to indicate custom table. Obsolete?
    Set("OptiScaler", 1);
    // New tracking flag
    Set(NGX_AllocTypes::AllocKey.data(),
        isPersistent ? NGX_AllocTypes::InternPersistent : NGX_AllocTypes::InternDynamic);
}

bool NVNGX_Parameters::D3D12Resources(const NVSDK_NGX_Parameter* parameters,
                                     std::vector<ID3D12Resource*>& resources)
{
    if (!parameters) return false;
    bool complete = false;
    std::string unknown;
    if (*reinterpret_cast<void* const*>(parameters) == implementation.load(std::memory_order_acquire))
    {
        const auto* own = static_cast<const NVNGX_Parameters*>(parameters);
        std::lock_guard lock(own->m_mutex);
        complete = ParameterResources(own->m_values, resources, unknown);
    }
    else
    {
        std::lock_guard lock(nativeParameterMutex);
        const auto it = nativeParameters.find(parameters);
        if (it != nativeParameters.end())
        {
            complete = ParameterResources(it->second.pointers, resources, unknown) && it->second.complete;
            if (!it->second.complete) { if (!unknown.empty()) unknown += ','; unknown += "native-history-incomplete"; }
        }
        else unknown = "unregistered-native-table";
    }
    if (!complete)
    {
        static std::atomic<UINT64> failures {0};
        const auto count = ++failures;
        if (count <= 3 || count % 300 == 0)
            LOG_INFO("[DLSSNR_ASYNC] native-parameter-incomplete={} table=0x{:X} key={} resources={}",
                     count, (uintptr_t)parameters, unknown, resources.size());
    }
    else
    {
        static std::atomic<UINT64> successes {0};
        const auto count = ++successes;
        if (count <= 3 || count % 300 == 0)
            LOG_INFO("[DLSSNR_ASYNC] native-parameter-snapshot={} table=0x{:X} resources={} complete=true",
                     count, (uintptr_t)parameters, resources.size());
    }
    return complete;
}

void NVNGX_Parameters::Set(const char* key, unsigned long long value)
{
    LOG_PARAM("ulong('{0}', {1})", key, value);
    setT(key, value);
}
void NVNGX_Parameters::Set(const char* key, float value)
{
    LOG_PARAM("float('{0}', {1})", key, value);
    setT(key, value);
}
void NVNGX_Parameters::Set(const char* key, double value)
{
    LOG_PARAM("double('{0}', {1})", key, value);
    setT(key, value);
}
void NVNGX_Parameters::Set(const char* key, unsigned int value)
{
    LOG_PARAM("uint('{0}', {1})", key, value);
    setT(key, value);
}
void NVNGX_Parameters::Set(const char* key, int value)
{
    LOG_PARAM("int('{0}', {1})", key, value);
    setT(key, value);
}
void NVNGX_Parameters::Set(const char* key, void* value)
{
    LOG_PARAM("void('{0}', '{1}null')", key, value == nullptr ? "" : "not ");
    setT(key, value);
}
void NVNGX_Parameters::Set(const char* key, ID3D11Resource* value)
{
    LOG_PARAM("d3d11('{0}', '{1}null')", key, value == nullptr ? "" : "not ");
    setT(key, value);
}
void NVNGX_Parameters::Set(const char* key, ID3D12Resource* value)
{
    LOG_PARAM("d3d12('{0}', '{1}null')", key, value == nullptr ? "" : "not ");
    setT(key, value);
}

NVSDK_NGX_Result NVNGX_Parameters::Get(const char* key, unsigned long long* value) const
{
    auto result = getT(key, value);
    if (result == NVSDK_NGX_Result_Success)
    {
        LOG_PARAM("ulong('{0}', {1})", key, *value);
        return NVSDK_NGX_Result_Success;
    }

#ifdef ENABLE_ENCAPSULATED_PARAMS
    if (OriginalParam != nullptr)
    {
        LOG_PARAM("calling original ulong('{0}')", key);
        result = OriginalParam->Get(key, value);
        LOG_PARAM("calling original ulong('{0}') result: {1:X}", key, (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            LOG_PARAM("from original ulong('{0}', {1})", key, *value);
            return result;
        }
    }
#endif // ENABLE_ENCAPSULATED_PARAMS

    return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result NVNGX_Parameters::Get(const char* key, float* value) const
{
    auto result = getT(key, value);
    if (result == NVSDK_NGX_Result_Success)
    {
        LOG_PARAM("float('{0}', {1})", key, *value);
        return NVSDK_NGX_Result_Success;
    }

#ifdef ENABLE_ENCAPSULATED_PARAMS
    if (OriginalParam != nullptr)
    {
        LOG_PARAM("calling original float('{0}')", key);
        result = OriginalParam->Get(key, value);
        LOG_PARAM("calling original float('{0}') result: {1:X}", key, (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            LOG_PARAM("from original float('{0}', {1})", key, *value);
            return result;
        }
    }
#endif // ENABLE_ENCAPSULATED_PARAMS

    return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result NVNGX_Parameters::Get(const char* key, double* value) const
{
    auto result = getT(key, value);
    if (result == NVSDK_NGX_Result_Success)
    {
        LOG_PARAM("double('{0}', {1})", key, *value);
        return NVSDK_NGX_Result_Success;
    }

#ifdef ENABLE_ENCAPSULATED_PARAMS
    if (OriginalParam != nullptr)
    {
        LOG_PARAM("calling original double('{0}')", key);
        result = OriginalParam->Get(key, value);
        LOG_PARAM("calling original double('{0}') result: {1:X}", key, (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            LOG_PARAM("from original double('{0}', {1})", key, *value);
            return result;
        }
    }
#endif // ENABLE_ENCAPSULATED_PARAMS

    return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result NVNGX_Parameters::Get(const char* key, unsigned int* value) const
{
    auto result = getT(key, value);
    if (result == NVSDK_NGX_Result_Success)
    {
        LOG_PARAM("uint('{0}', {1})", key, *value);
        return NVSDK_NGX_Result_Success;
    }

#ifdef ENABLE_ENCAPSULATED_PARAMS
    if (OriginalParam != nullptr)
    {
        LOG_PARAM("calling original uint('{0}')", key);
        result = OriginalParam->Get(key, value);
        LOG_PARAM("calling original uint('{0}') result: {1:X}", key, (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            LOG_PARAM("from original uint('{0}', {1})", key, *value);
            return result;
        }
    }
#endif // ENABLE_ENCAPSULATED_PARAMS

    return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result NVNGX_Parameters::Get(const char* key, int* value) const
{
    auto result = getT(key, value);
    if (result == NVSDK_NGX_Result_Success)
    {
        LOG_PARAM("int('{0}', {1})", key, *value);
        return NVSDK_NGX_Result_Success;
    }

#ifdef ENABLE_ENCAPSULATED_PARAMS
    if (OriginalParam != nullptr)
    {
        LOG_PARAM("calling original int('{0}')", key);
        result = OriginalParam->Get(key, value);
        LOG_PARAM("calling original int('{0}') result: {1:X}", key, (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            LOG_PARAM("from original int('{0}', {1})", key, *value);
            return result;
        }
    }
#endif // ENABLE_ENCAPSULATED_PARAMS

    return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result NVNGX_Parameters::Get(const char* key, void** value) const
{
    auto result = getT(key, value);
    if (result == NVSDK_NGX_Result_Success)
    {
        LOG_PARAM("void('{0}')", key);
        return NVSDK_NGX_Result_Success;
    }

#ifdef ENABLE_ENCAPSULATED_PARAMS
    if (OriginalParam != nullptr)
    {
        LOG_PARAM("calling original void('{0}')", key);
        result = OriginalParam->Get(key, value);
        LOG_PARAM("calling original void('{0}') result: {1:X}", key, (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            LOG_PARAM("from original void('{0}')", key);
            return result;
        }
    }
#endif // ENABLE_ENCAPSULATED_PARAMS

    return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result NVNGX_Parameters::Get(const char* key, ID3D11Resource** value) const
{
    auto result = getT(key, value);
    if (result == NVSDK_NGX_Result_Success)
    {
        LOG_PARAM("d3d11('{0}')", key);
        return NVSDK_NGX_Result_Success;
    }

#ifdef ENABLE_ENCAPSULATED_PARAMS
    if (OriginalParam != nullptr)
    {
        LOG_PARAM("calling original d3d11('{0}')", key);
        result = OriginalParam->Get(key, value);
        LOG_PARAM("calling original d3d11('{0}') result: {1:X}", key, (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            LOG_PARAM("from original d3d11('{0}')", key);
            return result;
        }
    }
#endif // ENABLE_ENCAPSULATED_PARAMS

    return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result NVNGX_Parameters::Get(const char* key, ID3D12Resource** value) const
{
    auto result = getT(key, value);
    if (result == NVSDK_NGX_Result_Success)
    {
        LOG_PARAM("d3d12('{0}')", key);
        return NVSDK_NGX_Result_Success;
    }

#ifdef ENABLE_ENCAPSULATED_PARAMS
    if (OriginalParam != nullptr)
    {
        LOG_PARAM("calling original d3d12('{0}')", key);
        result = OriginalParam->Get(key, value);
        LOG_PARAM("calling original d3d12('{0}') result: {1:X}", key, (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            LOG_PARAM("from original d3d12('{0}')", key);
            return result;
        }
    }
#endif // ENABLE_ENCAPSULATED_PARAMS

    return NVSDK_NGX_Result_Fail;
}

void NVNGX_Parameters::Reset()
{
    if (!m_values.empty())
    {
        // Preserve usage type if set
        uint32_t allocType = NGX_AllocTypes::Unknown;
        NVSDK_NGX_Result result = Get(NGX_AllocTypes::AllocKey.data(), &allocType);
        m_values.clear();

        if (result != NVSDK_NGX_Result_Fail)
            Set(NGX_AllocTypes::AllocKey.data(), allocType);
    }

    LOG_DEBUG("Start");

    InitNGXParameters(this);

    LOG_DEBUG("End");
}

std::vector<std::string> NVNGX_Parameters::enumerate() const
{
    std::vector<std::string> keys;
    for (auto& value : m_values)
    {
        keys.push_back(value.first);
    }
    return keys;
}

template <typename T> void NVNGX_Parameters::setT(const char* key, T& value)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_values[key] = value;
}

template <typename T> NVSDK_NGX_Result NVNGX_Parameters::getT(const char* key, T* value) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto k = m_values.find(key);

    if (k == m_values.end())
    {
        LOG_TRACE("('{0}', FAIL)", key);
        return NVSDK_NGX_Result_Fail;
    };

    const Parameter& p = (*k).second;
    *value = p;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_DLSS_GetOptimalSettingsCallback(NVSDK_NGX_Parameter* InParams)
{
    unsigned int Width;
    unsigned int Height;
    unsigned int OutWidth;
    unsigned int OutHeight;
    float scalingRatio = 0.0f;
    int PerfQualityValue;

    if (InParams->Get(NVSDK_NGX_Parameter_Width, &Width) != NVSDK_NGX_Result_Success ||
        InParams->Get(NVSDK_NGX_Parameter_Height, &Height) != NVSDK_NGX_Result_Success ||
        InParams->Get(NVSDK_NGX_Parameter_PerfQualityValue, &PerfQualityValue) != NVSDK_NGX_Result_Success)
        return NVSDK_NGX_Result_Fail;

    auto enumPQValue = (NVSDK_NGX_PerfQuality_Value) PerfQualityValue;

    LOG_DEBUG("Display Resolution: {0}x{1}", Width, Height);

    const std::optional<float> QualityRatio = GetQualityOverrideRatio(enumPQValue);

    if (QualityRatio.has_value())
    {
        OutHeight = (unsigned int) ((float) Height / QualityRatio.value());
        OutWidth = (unsigned int) ((float) Width / QualityRatio.value());
        scalingRatio = 1.0f / QualityRatio.value();
    }
    else
    {
        LOG_DEBUG("Quality: {0}", PerfQualityValue);

        switch (enumPQValue)
        {
        case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
            OutHeight = (unsigned int) ((float) Height / 3.0);
            OutWidth = (unsigned int) ((float) Width / 3.0);
            scalingRatio = 0.33333333f;
            break;

        case NVSDK_NGX_PerfQuality_Value_MaxPerf:
            OutHeight = (unsigned int) ((float) Height / 2.0);
            OutWidth = (unsigned int) ((float) Width / 2.0);
            scalingRatio = 0.5f;
            break;

        case NVSDK_NGX_PerfQuality_Value_Balanced:
            OutHeight = (unsigned int) ((float) Height / 1.7);
            OutWidth = (unsigned int) ((float) Width / 1.7);
            scalingRatio = 1.0f / 1.7f;
            break;

        case NVSDK_NGX_PerfQuality_Value_MaxQuality:
            OutHeight = (unsigned int) ((float) Height / 1.5);
            OutWidth = (unsigned int) ((float) Width / 1.5);
            scalingRatio = 1.0f / 1.5f;
            break;

        case NVSDK_NGX_PerfQuality_Value_UltraQuality:
            OutHeight = (unsigned int) ((float) Height / 1.3);
            OutWidth = (unsigned int) ((float) Width / 1.3);
            scalingRatio = 1.0f / 1.3f;
            break;

        case NVSDK_NGX_PerfQuality_Value_DLAA:
            OutHeight = Height;
            OutWidth = Width;
            scalingRatio = 1.0f;
            break;

        default:
            OutHeight = (unsigned int) ((float) Height / 1.7);
            OutWidth = (unsigned int) ((float) Width / 1.7);
            scalingRatio = 1.0f / 1.7f;
            break;
        }
    }

    if (Config::Instance()->RoundInternalResolution.has_value())
    {
        OutHeight -= OutHeight % Config::Instance()->RoundInternalResolution.value();
        OutWidth -= OutWidth % Config::Instance()->RoundInternalResolution.value();
        scalingRatio = (float) OutWidth / (float) Width;
    }

    InParams->Set(NVSDK_NGX_Parameter_Scale, scalingRatio);
    InParams->Set(NVSDK_NGX_Parameter_SuperSampling_ScaleFactor, scalingRatio);
    InParams->Set(NVSDK_NGX_Parameter_OutWidth, OutWidth);
    InParams->Set(NVSDK_NGX_Parameter_OutHeight, OutHeight);

    // DRS minimum resolution
    if (Config::Instance()->DrsMinOverrideEnabled.value_or_default() || enumPQValue == NVSDK_NGX_PerfQuality_Value_DLAA)
    {
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, OutWidth);
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, OutHeight);
    }
    else
    {
        if (Config::Instance()->ExtendedLimits.value_or_default() && OutWidth > Width)
        {
            InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, OutWidth);
            InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, OutHeight);
        }
        else
        {
            // DLSS normally only supports DRS in range of 0.5 and 1.0
            auto drsMinWidth = (unsigned int) ((float) Width * 0.5f);
            auto drsMinHeight = (unsigned int) ((float) Height * 0.5f);

            if (OutWidth < drsMinWidth || OutHeight < drsMinHeight)
            {
                InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, OutWidth);
                InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, OutHeight);
            }
            else
            {
                InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, drsMinWidth);
                InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, drsMinHeight);
            }
        }
    }

    // DRS maximum resolution

    if (Config::Instance()->DrsMaxOverrideEnabled.value_or_default())
    {
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, OutWidth);
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, OutHeight);
    }
    else
    {
        if (Config::Instance()->ExtendedLimits.value_or_default() && OutWidth > Width)
        {
            InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, OutWidth);
            InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, OutHeight);
        }
        else
        {
            InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, Width);
            InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, Height);
        }
    }

    InParams->Set(NVSDK_NGX_Parameter_SizeInBytes, Width * Height * 31);
    InParams->Set(NVSDK_NGX_Parameter_DLSSMode, NVSDK_NGX_DLSS_Mode_DLSS_DLISP);

    InParams->Set(NVSDK_NGX_EParameter_Scale, scalingRatio);
    InParams->Set(NVSDK_NGX_EParameter_OutWidth, OutWidth);
    InParams->Set(NVSDK_NGX_EParameter_OutHeight, OutHeight);
    InParams->Set(NVSDK_NGX_EParameter_SizeInBytes, Width * Height * 31);
    InParams->Set(NVSDK_NGX_EParameter_DLSSMode, NVSDK_NGX_DLSS_Mode_DLSS_DLISP);

    LOG_DEBUG("NVSDK_NGX_DLSS_GetOptimalSettingsCallback: Display Resolution: {0}x{1} Render Resolution: {2}x{3}",
              Width, Height, OutWidth, OutHeight);
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_DLSSD_GetOptimalSettingsCallback(NVSDK_NGX_Parameter* InParams)
{
    unsigned int Width;
    unsigned int Height;
    unsigned int OutWidth;
    unsigned int OutHeight;
    float scalingRatio = 0.0f;
    int PerfQualityValue;

    // If any of these params are uninitialized, return fail
    if (InParams->Get(NVSDK_NGX_Parameter_Width, &Width) != NVSDK_NGX_Result_Success ||
        InParams->Get(NVSDK_NGX_Parameter_Height, &Height) != NVSDK_NGX_Result_Success ||
        InParams->Get(NVSDK_NGX_Parameter_PerfQualityValue, &PerfQualityValue) != NVSDK_NGX_Result_Success)
        return NVSDK_NGX_Result_Fail;

    auto enumPQValue = (NVSDK_NGX_PerfQuality_Value) PerfQualityValue;

    LOG_DEBUG("Display Resolution: {0}x{1}", Width, Height);

    const std::optional<float> QualityRatio = GetQualityOverrideRatio(enumPQValue);

    if (QualityRatio.has_value())
    {
        OutHeight = (unsigned int) ((float) Height / QualityRatio.value());
        OutWidth = (unsigned int) ((float) Width / QualityRatio.value());
        scalingRatio = 1.0f / QualityRatio.value();
    }
    else
    {
        LOG_DEBUG("Quality: {0}", PerfQualityValue);

        switch (enumPQValue)
        {
        case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
            OutHeight = (unsigned int) ((float) Height / 3.0);
            OutWidth = (unsigned int) ((float) Width / 3.0);
            scalingRatio = 0.33333333f;
            break;

        case NVSDK_NGX_PerfQuality_Value_MaxPerf:
            OutHeight = (unsigned int) ((float) Height / 2.0);
            OutWidth = (unsigned int) ((float) Width / 2.0);
            scalingRatio = 0.5f;
            break;

        case NVSDK_NGX_PerfQuality_Value_Balanced:
            OutHeight = (unsigned int) ((float) Height / 1.7);
            OutWidth = (unsigned int) ((float) Width / 1.7);
            scalingRatio = 1.0f / 1.7f;
            break;

        case NVSDK_NGX_PerfQuality_Value_MaxQuality:
            OutHeight = (unsigned int) ((float) Height / 1.5);
            OutWidth = (unsigned int) ((float) Width / 1.5);
            scalingRatio = 1.0f / 1.5f;
            break;

        case NVSDK_NGX_PerfQuality_Value_UltraQuality:
            OutHeight = (unsigned int) ((float) Height / 1.3);
            OutWidth = (unsigned int) ((float) Width / 1.3);
            scalingRatio = 1.0f / 1.3f;
            break;

        case NVSDK_NGX_PerfQuality_Value_DLAA:
            OutHeight = Height;
            OutWidth = Width;
            scalingRatio = 1.0f;
            break;

        default:
            OutHeight = (unsigned int) ((float) Height / 1.7);
            OutWidth = (unsigned int) ((float) Width / 1.7);
            scalingRatio = 1.0f / 1.7f;
            break;
        }
    }

    if (Config::Instance()->RoundInternalResolution.has_value())
    {
        OutHeight -= OutHeight % Config::Instance()->RoundInternalResolution.value();
        OutWidth -= OutWidth % Config::Instance()->RoundInternalResolution.value();
    }

    InParams->Set(NVSDK_NGX_Parameter_Scale, scalingRatio);
    InParams->Set(NVSDK_NGX_Parameter_SuperSampling_ScaleFactor, scalingRatio);
    InParams->Set(NVSDK_NGX_Parameter_OutWidth, OutWidth);
    InParams->Set(NVSDK_NGX_Parameter_OutHeight, OutHeight);

    // DRS minimum resolution
    if (Config::Instance()->DrsMinOverrideEnabled.value_or_default())
    {
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, OutWidth);
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, OutHeight);
    }
    else if (enumPQValue == NVSDK_NGX_PerfQuality_Value_DLAA)
    {
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, Width);
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, Height);
    }
    else
    {
        // DLSS normally only supports DRS in range of 0.5 and 1.0
        auto drsMinWidth = (unsigned int) ((float) Width * 0.5f);
        auto drsMinHeight = (unsigned int) ((float) Height * 0.5f);

        if (OutWidth < drsMinWidth || OutHeight < drsMinHeight)
        {
            InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, OutWidth);
            InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, OutHeight);
        }
        else
        {
            InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, drsMinWidth);
            InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, drsMinHeight);
        }
    }

    // DRS maximum resolution
    if (Config::Instance()->DrsMaxOverrideEnabled.value_or_default())
    {
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, OutWidth);
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, OutHeight);
    }
    else
    {
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, Width);
        InParams->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, Height);
    }

    InParams->Set(NVSDK_NGX_Parameter_SizeInBytes, Width * Height * 31);
    InParams->Set(NVSDK_NGX_Parameter_DLSSMode, NVSDK_NGX_DLSS_Mode_DLSS_DLISP);

    InParams->Set(NVSDK_NGX_EParameter_Scale, scalingRatio);
    InParams->Set(NVSDK_NGX_EParameter_OutWidth, OutWidth);
    InParams->Set(NVSDK_NGX_EParameter_OutHeight, OutHeight);
    InParams->Set(NVSDK_NGX_EParameter_SizeInBytes, Width * Height * 31);
    InParams->Set(NVSDK_NGX_EParameter_DLSSMode, NVSDK_NGX_DLSS_Mode_DLSS_DLISP);

    LOG_DEBUG("Display Resolution: {0}x{1} Render Resolution: {2}x{3}", Width, Height, OutWidth, OutHeight);
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_DLSS_GetStatsCallback(NVSDK_NGX_Parameter* InParams)
{
    LOG_DEBUG("NVSDK_NGX_DLSS_GetStatsCallback");

    if (!InParams)
        return NVSDK_NGX_Result_Success;

    unsigned int Width = 1920;
    unsigned int Height = 1080;

    InParams->Get(NVSDK_NGX_Parameter_Width, &Width);
    InParams->Get(NVSDK_NGX_Parameter_Height, &Height);
    InParams->Set(NVSDK_NGX_Parameter_SizeInBytes, Width * Height * 31);

    return NVSDK_NGX_Result_Success;
}

void InitNGXParameters(NVSDK_NGX_Parameter* InParams)
{
    InParams->Set(NVSDK_NGX_Parameter_SuperSampling_Available, 1);

    if (State::Instance().NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
        State::Instance().gameEngine == GameEngineType::Unreal ||
        State::Instance().gameQuirks & GameQuirk::ForceUnrealEngine)
    {
        InParams->Set(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, 10);
        InParams->Set(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, 10);
    }
    else
    {
        InParams->Set(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, 0);
        InParams->Set(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, 0);
    }

    InParams->Set(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, 0);
    InParams->Set(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, 1);
    InParams->Set(NVSDK_NGX_Parameter_OptLevel, 0);
    InParams->Set(NVSDK_NGX_Parameter_IsDevSnippetBranch, 0);
    InParams->Set(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, NVSDK_NGX_DLSS_GetOptimalSettingsCallback);
    InParams->Set("DLSSDOptimalSettingsCallback", NVSDK_NGX_DLSSD_GetOptimalSettingsCallback);
    InParams->Set(NVSDK_NGX_Parameter_DLSSGetStatsCallback, NVSDK_NGX_DLSS_GetStatsCallback);
    InParams->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    InParams->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    InParams->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    InParams->Set(NVSDK_NGX_Parameter_MV_Offset_X, 0.0f);
    InParams->Set(NVSDK_NGX_Parameter_MV_Offset_Y, 0.0f);
    InParams->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    InParams->Set(NVSDK_NGX_Parameter_PerfQualityValue, 0);
    InParams->Set(NVSDK_NGX_Parameter_SizeInBytes, 1920 * 1080 * 31);

    InParams->Set(NVSDK_NGX_EParameter_SuperSampling_Available, 1);
    InParams->Set(NVSDK_NGX_EParameter_OptLevel, 0);
    InParams->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 0);
    InParams->Set(NVSDK_NGX_EParameter_IsDevSnippetBranch, 0);
    InParams->Set(NVSDK_NGX_EParameter_DLSSOptimalSettingsCallback, NVSDK_NGX_DLSS_GetOptimalSettingsCallback);
    InParams->Set(NVSDK_NGX_EParameter_Sharpness, 0.0f);
    InParams->Set(NVSDK_NGX_EParameter_MV_Scale_X, 1.0f);
    InParams->Set(NVSDK_NGX_EParameter_MV_Scale_Y, 1.0f);
    InParams->Set(NVSDK_NGX_EParameter_MV_Offset_X, 0.0f);
    InParams->Set(NVSDK_NGX_EParameter_MV_Offset_Y, 0.0f);

    InParams->Set("RayReconstruction.Hint.Render.Preset.DLAA",
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    InParams->Set("RayReconstruction.Hint.Render.Preset.UltraQuality",
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    InParams->Set("RayReconstruction.Hint.Render.Preset.Quality",
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    InParams->Set("RayReconstruction.Hint.Render.Preset.Balanced",
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    InParams->Set("RayReconstruction.Hint.Render.Preset.Performance",
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    InParams->Set("RayReconstruction.Hint.Render.Preset.UltraPerformance",
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);

    InParams->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    InParams->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    InParams->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    InParams->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    InParams->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    InParams->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                  (unsigned int) NVSDK_NGX_DLSS_Hint_Render_Preset_Default);

    InParams->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1);
    InParams->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
    InParams->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 1);
    InParams->Set(NVSDK_NGX_Parameter_RTXValue, 0);

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (!primaryGpu.dlssCapable)
    {
        InParams->Set("SuperSamplingDenoising.NeedsUpdatedDriver", 0);

        if (State::Instance().NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
            State::Instance().gameEngine == GameEngineType::Unreal ||
            State::Instance().gameQuirks & GameQuirk::ForceUnrealEngine)
        {
            InParams->Set("SuperSamplingDenoising.MinDriverVersionMajor", 10);
            InParams->Set("SuperSamplingDenoising.MinDriverVersionMinor", 10);
        }
        else
        {
            InParams->Set("SuperSamplingDenoising.MinDriverVersionMajor", 0);
            InParams->Set("SuperSamplingDenoising.MinDriverVersionMinor", 0);
        }

        InParams->Set("SuperSamplingDenoising.Available", 0);
        InParams->Set("SuperSamplingDenoising.FeatureInitResult", 0);
    }

    // not ideal as it doesn't take different APIs into account
    if (State::Instance().activeFgInput == FGInput::NvngxFG || State::Instance().activeFgInput == FGInput::DLSSG ||
        State::Instance().activeFgOutput == FGOutput::DLSSGWithNvngx)
    {
        InParams->Set("FrameGeneration.Available", 1);
        InParams->Set("FrameGeneration.NeedsUpdatedDriver", 0);
        InParams->Set("FrameGeneration.FeatureInitResult", 1);
        InParams->Set("FrameInterpolation.Available", 1);
        InParams->Set(NVSDK_NGX_Parameter_FrameInterpolation_NeedsUpdatedDriver, 0);
        InParams->Set(NVSDK_NGX_Parameter_FrameInterpolation_FeatureInitResult, 1);

        // Streamline handle the max interpolated frame count
        InParams->Set("DLSSG.MultiFrameCountMax", Nvngx_FG::isMFG() ? 5 : 1);

        if (State::Instance().NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
            State::Instance().gameEngine == GameEngineType::Unreal ||
            State::Instance().gameQuirks & GameQuirk::ForceUnrealEngine)
        {
            InParams->Set(NVSDK_NGX_Parameter_FrameInterpolation_MinDriverVersionMajor, 10);
            InParams->Set("FrameGeneration.MinDriverVersionMajor", 10);
        }
        else
        {
            InParams->Set(NVSDK_NGX_Parameter_FrameInterpolation_MinDriverVersionMajor, 0);
            InParams->Set("FrameGeneration.MinDriverVersionMajor", 0);
        }
    }
}

NVNGX_Parameters* GetNGXParameters(std::string_view name, bool isPersistent)
{
    auto params = new NVNGX_Parameters(name, isPersistent);
    InitNGXParameters(params);
    return params;
}

void SetNGXParamAllocType(NVSDK_NGX_Parameter& params, uint32_t allocType)
{
    params.Set(NGX_AllocTypes::AllocKey.data(), allocType);
}
