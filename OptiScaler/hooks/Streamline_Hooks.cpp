#include <pch.h>

#include "Streamline_Hooks.h"

#include <Util.h>
#include <Config.h>

#include <nvapi/fakenvapi.h>
#include <misc/IdentifyGpu.h>
#include <hooks/Reflex_Hooks.h>
#include <menu/menu_overlay_base.h>
#include <framegen/nvngx/Nvngx_FG.h>
#include <proxies/KernelBase_Proxy.h>
#include <gaze_roi/GazeRoiInput.h>
#include <hooks/GazeRoiStreamlineContext.h>

#include <json.hpp>
#include <sl1_reflex.h>
#include <magic_enum.hpp>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>
#include "detours/detours.h"

sl::RenderAPI StreamlineHooks::renderApi = sl::RenderAPI::eCount;
std::mutex StreamlineHooks::setConstantsMutex {};
SystemCaps* StreamlineHooks::systemCaps = nullptr;
SystemCapsSl15* StreamlineHooks::systemCapsSl15 = nullptr;

// interposer
decltype(&slInit) StreamlineHooks::o_slInit = nullptr;
decltype(&slSetTag) StreamlineHooks::o_slSetTag = nullptr;
decltype(&slSetTagForFrame) StreamlineHooks::o_slSetTagForFrame = nullptr;
decltype(&slEvaluateFeature) StreamlineHooks::o_slEvaluateFeature = nullptr;
decltype(&slAllocateResources) StreamlineHooks::o_slAllocateResources = nullptr;
decltype(&slSetConstants) StreamlineHooks::o_slSetConstants = nullptr;
decltype(&slGetNativeInterface) StreamlineHooks::o_slGetNativeInterface = nullptr;
decltype(&slSetD3DDevice) StreamlineHooks::o_slSetD3DDevice = nullptr;
decltype(&slGetNewFrameToken) StreamlineHooks::o_slGetNewFrameToken = nullptr;

decltype(&sl1::slInit) StreamlineHooks::o_slInit_sl1 = nullptr;
decltype(&sl1::slSetTag) StreamlineHooks::o_slSetTag_sl1 = nullptr;
decltype(&sl1::slSetConstants) StreamlineHooks::o_slSetConstants_interposer_sl1 = nullptr;
decltype(&sl1::slEvaluateFeature) StreamlineHooks::o_slEvaluateFeature_sl1 = nullptr;
using PFN_slGetPluginJSONConfig_sl1 = const char* (*) ();

sl::PFun_LogMessageCallback* StreamlineHooks::o_logCallback = nullptr;
sl1::pfunLogMessageCallback* StreamlineHooks::o_logCallback_sl1 = nullptr;

// DLSS
StreamlineHooks::PFN_slGetPluginFunction StreamlineHooks::o_dlss_slGetPluginFunction = nullptr;
StreamlineHooks::PFN_slOnPluginLoad StreamlineHooks::o_dlss_slOnPluginLoad = nullptr;
decltype(&slDLSSGetOptimalSettings) StreamlineHooks::o_slDLSSGetOptimalSettings = nullptr;
decltype(&slDLSSSetOptions) StreamlineHooks::o_slDLSSSetOptions = nullptr;

// DLSSG
StreamlineHooks::PFN_slGetPluginFunction StreamlineHooks::o_dlssg_slGetPluginFunction = nullptr;
StreamlineHooks::PFN_slOnPluginLoad StreamlineHooks::o_dlssg_slOnPluginLoad = nullptr;
decltype(&slDLSSGSetOptions) StreamlineHooks::o_slDLSSGSetOptions = nullptr;
decltype(&slDLSSGGetState) StreamlineHooks::o_slDLSSGGetState = nullptr;
static PFN_slGetPluginJSONConfig_sl1 o_dlssg_slGetPluginJSONConfig_sl1;

// Local DLSSG
StreamlineHooks::PFN_slGetPluginFunction StreamlineHooks::o_local_dlssg_slGetPluginFunction = nullptr;
StreamlineHooks::PFN_slOnPluginLoad StreamlineHooks::o_local_dlssg_slOnPluginLoad = nullptr;

// Reflex
StreamlineHooks::PFN_slGetPluginFunction StreamlineHooks::o_reflex_slGetPluginFunction = nullptr;
StreamlineHooks::PFN_slSetConstants_sl1 StreamlineHooks::o_reflex_slSetConstants_sl1 = nullptr;
StreamlineHooks::PFN_slOnPluginLoad StreamlineHooks::o_reflex_slOnPluginLoad = nullptr;
decltype(&slReflexSetOptions) StreamlineHooks::o_slReflexSetOptions = nullptr;
decltype(&slReflexSleep) StreamlineHooks::o_slReflexSleep = nullptr;
sl::ReflexMode StreamlineHooks::reflexGamesLastMode = sl::ReflexMode::eOff;

// PCL
StreamlineHooks::PFN_slGetPluginFunction StreamlineHooks::o_pcl_slGetPluginFunction = nullptr;
StreamlineHooks::PFN_slOnPluginLoad StreamlineHooks::o_pcl_slOnPluginLoad = nullptr;
decltype(&slPCLSetMarker) StreamlineHooks::o_slPCLSetMarker = nullptr;

// Common
StreamlineHooks::PFN_slGetPluginFunction StreamlineHooks::o_common_slGetPluginFunction = nullptr;
StreamlineHooks::PFN_slOnPluginLoad StreamlineHooks::o_common_slOnPluginLoad = nullptr;
StreamlineHooks::PFN_slSetParameters_sl1 StreamlineHooks::o_common_slSetParameters_sl1 = nullptr;
StreamlineHooks::PFN_setVoid StreamlineHooks::o_setVoid = nullptr;

static const char* hkdlssg_slGetPluginJSONConfig_sl1();

static bool IsSL1AndDLSSGActive()
{
    return State::Instance().streamlineVersion.major == 1 && State::Instance().activeFgInput == FGInput::DLSSG &&
           (State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG);
}

static bool IsGazeContractCaptureEnabled()
{
    return Config::Instance()->GazeRoiEnabled.value_or_default();
}

static void LogUniqueGazeStreamlineContract(const char* kind, const std::string& message,
                                            const std::string& identity = {})
{
    static std::mutex logMutex;
    static std::unordered_map<std::string, std::string> lastSignatures;

    std::scoped_lock lock(logMutex);
    const auto& dedupeKey = identity.empty() ? message : identity;
    auto& last = lastSignatures[kind];
    if (last == dedupeKey)
        return;

    last = dedupeKey;
    LOG_INFO("[GROI_SL_CONTRACT] kind={} {}", kind, message);
}

static void AppendSlMatrix(std::ostringstream& stream, const char* name, const sl::float4x4& matrix)
{
    stream << ' ' << name << '=';
    for (uint32_t row = 0; row < 4; row++)
    {
        if (row != 0)
            stream << ';';
        const auto& value = matrix[row];
        stream << value.x << ',' << value.y << ',' << value.z << ',' << value.w;
    }
}

static void LogGazeStreamlineConstants(const sl::Constants& values, uint32_t frame, uint32_t viewport)
{
    if (!IsGazeContractCaptureEnabled())
        return;

    std::ostringstream contract;
    contract << "viewport=" << viewport << " jitter=" << values.jitterOffset.x << ','
             << values.jitterOffset.y << " mvecScale=" << values.mvecScale.x << ',' << values.mvecScale.y
             << " pinhole=" << values.cameraPinholeOffset.x << ',' << values.cameraPinholeOffset.y << " cameraPos="
             << values.cameraPos.x << ',' << values.cameraPos.y << ',' << values.cameraPos.z << " nearFar="
             << values.cameraNear << ',' << values.cameraFar << " fov=" << values.cameraFOV
             << " aspect=" << values.cameraAspectRatio << " mvInvalid=" << values.motionVectorsInvalidValue
             << " depthInverted=" << static_cast<int>(values.depthInverted)
             << " cameraMotionIncluded=" << static_cast<int>(values.cameraMotionIncluded)
             << " motionVectors3D=" << static_cast<int>(values.motionVectors3D)
             << " reset=" << static_cast<int>(values.reset)
             << " orthographic=" << static_cast<int>(values.orthographicProjection)
             << " mvDilated=" << static_cast<int>(values.motionVectorsDilated)
             << " mvJittered=" << static_cast<int>(values.motionVectorsJittered)
             << " minDepthSeparation=" << values.minRelativeLinearDepthObjectSeparation;
    AppendSlMatrix(contract, "viewToClip", values.cameraViewToClip);
    AppendSlMatrix(contract, "clipToView", values.clipToCameraView);
    AppendSlMatrix(contract, "clipToLens", values.clipToLensClip);
    AppendSlMatrix(contract, "clipToPrev", values.clipToPrevClip);
    AppendSlMatrix(contract, "prevToClip", values.prevClipToClip);
    LogUniqueGazeStreamlineContract("constants", std::format("frame={} {}", frame, contract.str()), contract.str());
}

static void AppendGazeStreamlineTag(std::ostringstream& contract, const sl::ResourceTag& tag, bool d3d12)
{
    const auto typeEnum = static_cast<BufferType>(tag.type);
    contract << " tag=" << magic_enum::enum_name(typeEnum) << '(' << static_cast<uint64_t>(tag.type) << ')'
             << ":life" << static_cast<uint32_t>(tag.lifecycle) << ":extent(" << tag.extent.left << ','
             << tag.extent.top << ',' << tag.extent.width << ',' << tag.extent.height << ')';

    if (tag.resource == nullptr || tag.resource->native == nullptr)
    {
        contract << ":null";
        return;
    }

    contract << ":native" << tag.resource->native << ":state0x" << std::hex << tag.resource->state << std::dec
             << ":slDesc(" << tag.resource->width << 'x' << tag.resource->height << ",fmt"
             << tag.resource->nativeFormat << ",mips" << tag.resource->mipLevels << ",layers"
             << tag.resource->arrayLayers << ')';

    if (d3d12 && tag.resource->type == sl::ResourceType::eTex2d)
    {
        auto* resource = static_cast<ID3D12Resource*>(tag.resource->native);
        const auto desc = resource->GetDesc();
        contract << ":d3d12(" << desc.Width << 'x' << desc.Height << ",fmt" << static_cast<uint32_t>(desc.Format)
                 << ",flags0x" << std::hex << static_cast<uint32_t>(desc.Flags) << std::dec << ",mips"
                 << desc.MipLevels << ",array" << desc.DepthOrArraySize << ",samples" << desc.SampleDesc.Count << ')';
    }
}

static void LogGazeStreamlineTags(const char* kind, uint32_t frame, uint32_t viewport, const sl::ResourceTag* tags,
                                  uint32_t count, bool d3d12)
{
    if (!IsGazeContractCaptureEnabled())
        return;

    std::ostringstream contract;
    contract << "viewport=" << viewport << " count=" << count;

    if (tags == nullptr)
        contract << " removed=true";
    else
        for (uint32_t i = 0; i < count; i++)
            AppendGazeStreamlineTag(contract, tags[i], d3d12);

    std::ostringstream message;
    message << "frame=";
    if (frame == UINT_MAX)
        message << "none";
    else
        message << frame;
    message << ' ' << contract.str();

    LogUniqueGazeStreamlineContract(kind, message.str(), contract.str());
}

namespace
{
constexpr bool kGazeRoiRrTemporarilyDisabled = true;

thread_local GazeRoiStreamlineEvaluationContext currentRrEvaluationContext {};

struct CachedResourceTags
{
    std::vector<sl::ResourceTag> tags;
    std::vector<sl::Resource> resources;

    CachedResourceTags() = default;
    CachedResourceTags(const CachedResourceTags& other) { Assign(other.tags.data(), static_cast<uint32_t>(other.tags.size())); }
    CachedResourceTags& operator=(const CachedResourceTags& other)
    {
        if (this != &other)
            Assign(other.tags.data(), static_cast<uint32_t>(other.tags.size()));
        return *this;
    }
    CachedResourceTags(CachedResourceTags&& other) noexcept
    {
        Assign(other.tags.data(), static_cast<uint32_t>(other.tags.size()));
    }
    CachedResourceTags& operator=(CachedResourceTags&& other) noexcept
    {
        Assign(other.tags.data(), static_cast<uint32_t>(other.tags.size()));
        return *this;
    }

    void Assign(const sl::ResourceTag* source, uint32_t count)
    {
        if (source == nullptr || count == 0)
        {
            tags.clear();
            resources.clear();
            return;
        }

        // Build the replacement before releasing the old storage. Merge can
        // pass tags whose Resource pointers still refer to this cache.
        std::vector<sl::ResourceTag> newTags;
        std::vector<sl::Resource> newResources;
        newTags.reserve(count);
        newResources.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            newTags.push_back(source[i]);
            if (source[i].resource != nullptr)
            {
                newResources.push_back(*source[i].resource);
                newTags.back().resource = &newResources.back();
            }
        }
        tags = std::move(newTags);
        resources = std::move(newResources);
    }

    void Merge(const sl::ResourceTag* source, uint32_t count)
    {
        if (source == nullptr)
        {
            Assign(nullptr, 0);
            return;
        }
        std::vector<sl::ResourceTag> merged = tags;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto existing = std::find_if(merged.begin(), merged.end(), [&](const sl::ResourceTag& tag)
            {
                return tag.type == source[i].type;
            });
            if (existing == merged.end())
                merged.push_back(source[i]);
            else
                *existing = source[i];
        }
        Assign(merged.data(), static_cast<uint32_t>(merged.size()));
    }
};

std::mutex resourceTagCacheMutex;
std::unordered_map<uint32_t, CachedResourceTags> staticResourceTags;
std::unordered_map<uint64_t, CachedResourceTags> frameResourceTags;

uint64_t MakeFrameViewportKey(uint32_t frame, uint32_t viewport)
{
    return (static_cast<uint64_t>(frame) << 32U) | viewport;
}

uint32_t FindViewport(const sl::BaseStructure** inputs, uint32_t numInputs)
{
    for (uint32_t i = 0; inputs != nullptr && i < numInputs; ++i)
    {
        if (inputs[i] != nullptr && inputs[i]->structType == sl::ViewportHandle::s_structType)
            return static_cast<uint32_t>(*static_cast<const sl::ViewportHandle*>(inputs[i]));
    }
    return UINT_MAX;
}

sl::Extent ResolveTagExtent(const sl::ResourceTag& tag)
{
    if (tag.extent)
        return tag.extent;
    if (tag.resource == nullptr || tag.resource->native == nullptr)
        return {};

    uint32_t width = tag.resource->width;
    uint32_t height = tag.resource->height;
    if ((width == 0 || height == 0) && tag.resource->type == sl::ResourceType::eTex2d)
    {
        const auto desc = static_cast<ID3D12Resource*>(tag.resource->native)->GetDesc();
        width = static_cast<uint32_t>(std::min<uint64_t>(desc.Width, UINT32_MAX));
        height = desc.Height;
    }
    return { 0, 0, width, height };
}

GazeRoiStreamlineRect ToContextRect(const sl::Extent& extent)
{
    return { extent.left, extent.top, extent.width, extent.height };
}

sl::Extent MapNormalizedRoi(const sl::Extent& extent, double left, double top, double width, double height)
{
    if (!extent)
        return {};
    const uint32_t mappedWidth = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::llround(width * extent.width)), 1U, extent.width);
    const uint32_t mappedHeight = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::llround(height * extent.height)), 1U, extent.height);
    const uint32_t maxLeft = extent.width - mappedWidth;
    const uint32_t maxTop = extent.height - mappedHeight;
    const uint32_t mappedLeft = static_cast<uint32_t>(std::clamp<int64_t>(
        static_cast<int64_t>(std::llround(left * extent.width)), 0, maxLeft));
    const uint32_t mappedTop = static_cast<uint32_t>(std::clamp<int64_t>(
        static_cast<int64_t>(std::llround(top * extent.height)), 0, maxTop));
    return { extent.top + mappedTop, extent.left + mappedLeft, mappedWidth, mappedHeight };
}

bool BuildLocalizedRrTags(const CachedResourceTags& original, const GazeRoiInputSample& gaze,
                          CachedResourceTags& localized, GazeRoiStreamlineEvaluationContext& context)
{
    const auto output = std::find_if(original.tags.begin(), original.tags.end(), [](const sl::ResourceTag& tag)
    {
        return tag.type == sl::kBufferTypeScalingOutputColor && tag.resource != nullptr &&
               tag.resource->native != nullptr;
    });
    if (output == original.tags.end())
        return false;

    const sl::Extent outputExtent = ResolveTagExtent(*output);
    if (!outputExtent)
        return false;

    const uint32_t outputWidth = std::clamp<uint32_t>(
        Config::Instance()->GazeRoiWidthPx.value_or_default(), std::min(64U, outputExtent.width), outputExtent.width);
    const uint32_t outputHeight = std::clamp<uint32_t>(
        Config::Instance()->GazeRoiHeightPx.value_or_default(), std::min(64U, outputExtent.height), outputExtent.height);
    const uint32_t outputLeft = static_cast<uint32_t>(std::clamp<int64_t>(
        static_cast<int64_t>(std::lround(gaze.x * outputExtent.width)) - outputWidth / 2U,
        0, outputExtent.width - outputWidth));
    const uint32_t outputTop = static_cast<uint32_t>(std::clamp<int64_t>(
        static_cast<int64_t>(std::lround(gaze.y * outputExtent.height)) - outputHeight / 2U,
        0, outputExtent.height - outputHeight));
    const double normalizedLeft = static_cast<double>(outputLeft) / outputExtent.width;
    const double normalizedTop = static_cast<double>(outputTop) / outputExtent.height;
    const double normalizedWidth = static_cast<double>(outputWidth) / outputExtent.width;
    const double normalizedHeight = static_cast<double>(outputHeight) / outputExtent.height;

    localized = original;
    context = {};
    context.active = true;
    context.recentered = gaze.recentered;
    context.gazeX = gaze.x;
    context.gazeY = gaze.y;

    for (auto& tag : localized.tags)
    {
        const sl::Extent originalExtent = ResolveTagExtent(tag);
        if (!originalExtent)
            continue;
        const sl::Extent localExtent = MapNormalizedRoi(
            originalExtent, normalizedLeft, normalizedTop, normalizedWidth, normalizedHeight);
        tag.extent = localExtent;

        GazeRoiStreamlineRect* originalRect = nullptr;
        GazeRoiStreamlineRect* localRect = nullptr;
        if (tag.type == sl::kBufferTypeScalingInputColor)
        {
            originalRect = &context.colorOriginal;
            localRect = &context.colorLocal;
        }
        else if (tag.type == sl::kBufferTypeDepth ||
                 ((tag.type == sl::kBufferTypeLinearDepth || tag.type == sl::kBufferTypeHiResDepth) &&
                  context.depthOriginal.width == 0))
        {
            originalRect = &context.depthOriginal;
            localRect = &context.depthLocal;
        }
        else if (tag.type == sl::kBufferTypeMotionVectors)
        {
            originalRect = &context.motionVectorsOriginal;
            localRect = &context.motionVectorsLocal;
        }
        else if (tag.type == sl::kBufferTypeScalingOutputColor)
        {
            originalRect = &context.outputOriginal;
            localRect = &context.outputLocal;
        }
        if (originalRect != nullptr)
        {
            *originalRect = ToContextRect(originalExtent);
            *localRect = ToContextRect(localExtent);
        }
    }

    return context.colorLocal.width != 0 && context.depthLocal.width != 0 &&
           context.motionVectorsLocal.width != 0 && context.outputLocal.width != 0;
}
} // namespace

const GazeRoiStreamlineEvaluationContext* GazeRoiStreamlineContext::Current()
{
    return currentRrEvaluationContext.active ? &currentRrEvaluationContext : nullptr;
}

void GazeRoiStreamlineContext::Set(const GazeRoiStreamlineEvaluationContext& context)
{
    currentRrEvaluationContext = context;
}

void GazeRoiStreamlineContext::Clear()
{
    currentRrEvaluationContext = {};
}

static bool IsSL1AndFGActive()
{
    const auto& state = State::Instance();

    return state.streamlineVersion.major == 1 &&
           (state.activeFgInput == FGInput::DLSSG || state.activeFgOutput == FGOutput::FSRFG ||
            state.activeFgOutput == FGOutput::XeFG);
}

static void PatchSL1PluginJson(nlohmann::json& configJson)
{
    if (!IsSL1AndFGActive())
        return;

    LOG_DEBUG("Patching SL1 plugin JSON for external FG management");

    if (configJson.contains("/hooks"_json_pointer))
        configJson["hooks"].clear();

    if (configJson.contains("/exclusive_hooks"_json_pointer))
        configJson["exclusive_hooks"].clear();

    if (configJson.contains("/external/feature/tags"_json_pointer))
        configJson["external"]["feature"]["tags"].clear();

    if (configJson.contains("/vsync/supported"_json_pointer))
        configJson["vsync"]["supported"] = true;

    if (configJson.contains("/external/hws/required"_json_pointer))
        configJson["external"]["hws"]["required"] = false;
}

char* StreamlineHooks::trimStreamlineLog(const char* msg)
{
    char* result = (char*) malloc(strlen(msg) + 1);
    if (!result)
        return nullptr;

    strcpy(result, msg);

    size_t length = strlen(result);
    if (length > 0 && result[length - 1] == '\n')
    {
        result[length - 1] = '\0';
    }

    return result;
}

void StreamlineHooks::streamlineLogCallback(sl::LogType type, const char* msg)
{
    if (msg == nullptr)
        return;

    char* trimmed_msg = trimStreamlineLog(msg);
    if (trimmed_msg != nullptr)
    {
        switch (type)
        {
        case sl::LogType::eWarn:
            LOG_WARN("{}", trimmed_msg);
            break;
        case sl::LogType::eInfo:
            LOG_INFO("{}", trimmed_msg);
            break;
        case sl::LogType::eError:
            LOG_ERROR("{}", trimmed_msg);
            break;
        case sl::LogType::eCount:
            LOG_ERROR("{}", trimmed_msg);
            break;
        }

        free(trimmed_msg);
    }

    if (o_logCallback != nullptr)
        o_logCallback(type, msg);
}

sl::Result StreamlineHooks::hkslInit(const sl::Preferences& pref, uint64_t sdkVersion)
{
    LOG_FUNC();

    sl::Preferences localPref = pref;

    if (localPref.logMessageCallback != &streamlineLogCallback)
        o_logCallback = localPref.logMessageCallback;
    localPref.logLevel = sl::LogLevel::eCount;
    localPref.logMessageCallback = &streamlineLogCallback;

    // renderAPI is optional so need to be careful, should only matter for Vulkan
    renderApi = localPref.renderAPI;

    State::Instance().slFGInputs.reportEngineType(localPref.engine);

    // Treat engine type set in Streamline as ground truth
    if (localPref.engine == sl::EngineType::eUnreal)
        State::Instance().gameQuirks |= GameQuirk::ForceUnrealEngine;

    std::filesystem::path localSlPath(Config::Instance()->MainDllPath.value());
    localSlPath = localSlPath / L"streamline"; // Hardcoded streamline folder

    auto localSlPathStr = localSlPath.wstring();

    std::vector<const wchar_t*> storage;

    // Replace the SL files to allow for MFG
    // TODO: ensure the path contains all the required plugins
    if (State::Instance().activeFgInput == FGInput::NvngxFG && std::filesystem::exists(localSlPath / L"sl.common.dll"))
    {
        storage.assign(localPref.pathsToPlugins, localPref.pathsToPlugins + localPref.numPathsToPlugins);

        storage.insert(storage.begin(), localSlPathStr.c_str());

        localPref.pathsToPlugins = storage.data();
        localPref.numPathsToPlugins = (uint32_t) storage.size();
    }

    // bool hookSetTag =
    //     (State::Instance().activeFgInput == FGInput::NvngxFG || State::Instance().activeFgInput == FGInput::DLSSG);

    // if (hookSetTag)
    //     localPref->flags &= ~(sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins);

    // To prevent mixed up OTA situations
    // if (State::Instance().activeFgOutput == FGOutput::DLSSG)
    //{
    //    localPref.flags &= ~sl::PreferenceFlags::eAllowOTA;
    //    localPref.flags &= ~sl::PreferenceFlags::eLoadDownloadedPlugins;
    //}

    return o_slInit(localPref, sdkVersion);
}

sl::Result StreamlineHooks::hkslSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                                       uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    LogGazeStreamlineTags("tags", UINT_MAX, static_cast<uint32_t>(viewport), tags, numTags,
                          renderApi == sl::RenderAPI::eD3D12);

    if (renderApi == sl::RenderAPI::eD3D11 || renderApi == sl::RenderAPI::eVulkan)
    {
        LOG_ERROR("hkslSetTag only supports DX12");
        return o_slSetTag(viewport, tags, numTags, cmdBuffer);
    }

    if (renderApi == sl::RenderAPI::eCount)
        LOG_WARN("Incomplete Streamline hooks");

    {
        std::lock_guard lock(resourceTagCacheMutex);
        if (tags == nullptr)
            staticResourceTags.erase(static_cast<uint32_t>(viewport));
    }

    if (tags == nullptr)
    {
        LOG_WARN("Game trying to remove a tag");
        return o_slSetTag(viewport, tags, numTags, cmdBuffer);
    }

    const bool fgTagBehaviorEnabled = State::Instance().activeFgInput == FGInput::NvngxFG ||
                                      State::Instance().activeFgInput == FGInput::DLSSG;

    for (uint32_t i = 0; i < numTags; i++)
    {
        const auto typeEnum = (BufferType) tags[i].type;

        if (tags[i].resource == nullptr || tags[i].resource->native == nullptr)
        {
            LOG_TRACE("Resource of type: {} is null, continuing", magic_enum::enum_name(typeEnum));
            continue;
        }

        // Cyberpunk hudless state fix for RDNA 2
        if (fgTagBehaviorEnabled && State::Instance().gameQuirks & GameQuirk::CyberpunkHudlessState &&
            tags[i].resource->state ==
                (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
            tags[i].type == sl::kBufferTypeHUDLessColor)
        {
            tags[i].resource->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            LOG_TRACE("Changing hudless resource state");
        }

        if (State::Instance().activeFgInput == FGInput::DLSSG &&
            (tags[i].type == sl::kBufferTypeHUDLessColor || tags[i].type == sl::kBufferTypeDepth ||
             tags[i].type == sl::kBufferTypeHiResDepth || tags[i].type == sl::kBufferTypeLinearDepth ||
             tags[i].type == sl::kBufferTypeMotionVectors || tags[i].type == sl::kBufferTypeUIColorAndAlpha ||
             tags[i].type == sl::kBufferTypeBidirectionalDistortionField))
        {
            State::Instance().slFGInputs.reportResource(tags[i], (ID3D12GraphicsCommandList*) cmdBuffer, 0);
        }
        else if (State::Instance().activeFgInput == FGInput::NvngxFG)
        {
            LOG_TRACE("Tagging resource of type: {}", magic_enum::enum_name(typeEnum));

            // Workaround a bug in the FSR 3 MFG mod where it composits the UI incorrectly
            if (tags[i].type == sl::kBufferTypeUIColorAndAlpha && tags[i].resource->native && Nvngx_FG::isMFG())
            {
                tags[i].resource->native = nullptr;
            }
        }
    }

    {
        std::lock_guard lock(resourceTagCacheMutex);
        staticResourceTags[static_cast<uint32_t>(viewport)].Merge(tags, numTags);
    }

    auto result = o_slSetTag(viewport, tags, numTags, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                                               const sl::ResourceTag* resources, uint32_t numResources,
                                               sl::CommandBuffer* cmdBuffer)
{
    LogGazeStreamlineTags("frameTags", static_cast<uint32_t>(frame), static_cast<uint32_t>(viewport), resources,
                          numResources, renderApi == sl::RenderAPI::eD3D12);

    if (renderApi == sl::RenderAPI::eD3D11 || renderApi == sl::RenderAPI::eVulkan)
    {
        LOG_ERROR("hkslSetTagForFrame only supports DX12");
        return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    }

    if (renderApi == sl::RenderAPI::eCount)
        LOG_WARN("Incomplete Streamline hooks");

    const uint32_t frameIndex = static_cast<uint32_t>(frame);
    const uint32_t viewportIndex = static_cast<uint32_t>(viewport);
    {
        std::lock_guard lock(resourceTagCacheMutex);
        const uint64_t key = MakeFrameViewportKey(frameIndex, viewportIndex);
        if (resources == nullptr)
            frameResourceTags.erase(key);
        for (auto iterator = frameResourceTags.begin(); iterator != frameResourceTags.end();)
        {
            const uint32_t cachedFrame = static_cast<uint32_t>(iterator->first >> 32U);
            if (cachedFrame + 8U < frameIndex)
                iterator = frameResourceTags.erase(iterator);
            else
                ++iterator;
        }
    }

    if (resources == nullptr)
    {
        LOG_WARN("Game trying to remove a tag");
        return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    }

    LOG_DEBUG("frameIndex: {}", static_cast<uint32_t>(frame));

    for (uint32_t i = 0; i < numResources; i++)
    {
        const auto typeEnum = (BufferType) resources[i].type;

        if (resources[i].resource == nullptr || resources[i].resource->native == nullptr)
        {
            LOG_TRACE("Resource of type: {} is null, continuing", magic_enum::enum_name(typeEnum));
            continue;
        }

        if (State::Instance().activeFgInput == FGInput::DLSSG &&
            (resources[i].type == sl::kBufferTypeHUDLessColor || resources[i].type == sl::kBufferTypeDepth ||
             resources[i].type == sl::kBufferTypeHiResDepth || resources[i].type == sl::kBufferTypeLinearDepth ||
             resources[i].type == sl::kBufferTypeMotionVectors || resources[i].type == sl::kBufferTypeUIColorAndAlpha ||
             resources[i].type == sl::kBufferTypeBidirectionalDistortionField))
        {
            State::Instance().slFGInputs.reportResource(resources[i], (ID3D12GraphicsCommandList*) cmdBuffer,
                                                        (uint32_t) frame);
        }
        else if (State::Instance().activeFgInput == FGInput::NvngxFG)
        {
            LOG_TRACE("Tagging resource of type: {}", magic_enum::enum_name(typeEnum));

            // Workaround a bug in the FSR 3 MFG mod where it composits the UI incorrectly
            if (resources[i].type == sl::kBufferTypeUIColorAndAlpha && resources[i].resource->native &&
                Nvngx_FG::isMFG())
            {
                resources[i].resource->native = nullptr;
            }
        }
    }

    {
        std::lock_guard lock(resourceTagCacheMutex);
        frameResourceTags[MakeFrameViewportKey(frameIndex, viewportIndex)].Merge(resources, numResources);
    }

    auto result = o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame,
                                                 const sl::BaseStructure** inputs, uint32_t numInputs,
                                                 sl::CommandBuffer* cmdBuffer)
{
    const uint32_t frameIndex = static_cast<uint32_t>(frame);
    const uint32_t viewport = FindViewport(inputs, numInputs);
    LOG_DEBUG("frameIndex: {}", frameIndex);

    if (IsGazeContractCaptureEnabled())
    {
        std::ostringstream contract;
        contract << "feature=" << static_cast<uint32_t>(feature) << " viewport=" << viewport << " inputs="
                 << numInputs;
        for (uint32_t i = 0; inputs != nullptr && i < numInputs; i++)
        {
            if (inputs[i] == nullptr)
            {
                contract << " input" << i << "=null";
                continue;
            }

            if (inputs[i]->structType == sl::ViewportHandle::s_structType)
            {
                contract << " viewportInput=true";
            }
            else if (inputs[i]->structType == sl::ResourceTag::s_structType)
            {
                AppendGazeStreamlineTag(contract, *static_cast<const sl::ResourceTag*>(inputs[i]),
                                        renderApi == sl::RenderAPI::eD3D12);
            }
            else if (inputs[i]->structType == sl::Constants::s_structType)
            {
                contract << " constants=true";
                LogGazeStreamlineConstants(*static_cast<const sl::Constants*>(inputs[i]),
                                           frameIndex, viewport);
            }
            else if (inputs[i]->structType == sl::DLSSOptions::s_structType)
            {
                contract << " dlssOptions=true";
            }
            else
            {
                contract << " input" << i << "Version=" << inputs[i]->structVersion;
            }
        }
        LogUniqueGazeStreamlineContract(
            "evaluate", std::format("frame={} {}", frameIndex, contract.str()), contract.str());
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG && numInputs > 0 && inputs != nullptr)
    {
        for (uint32_t i = 0; i < numInputs; i++)
        {
            if (inputs[i] == nullptr)
                continue;

            if (inputs[i]->structType == sl::ResourceTag::s_structType)
            {
                auto tag = (const sl::ResourceTag*) inputs[i];

                if (tag->type == sl::kBufferTypeHUDLessColor || tag->type == sl::kBufferTypeDepth ||
                    tag->type == sl::kBufferTypeHiResDepth || tag->type == sl::kBufferTypeLinearDepth ||
                    tag->type == sl::kBufferTypeMotionVectors || tag->type == sl::kBufferTypeUIColorAndAlpha ||
                    tag->type == sl::kBufferTypeBidirectionalDistortionField)
                {
                    State::Instance().slFGInputs.reportResource(*tag, (ID3D12GraphicsCommandList*) cmdBuffer,
                                                                frameIndex);
                }
            }
        }
    }

    const bool localizeRr = !kGazeRoiRrTemporarilyDisabled && feature == sl::kFeatureDLSS_RR && renderApi == sl::RenderAPI::eD3D12 &&
                            Config::Instance()->GazeRoiEnabled.value_or_default() && viewport != UINT_MAX;
    if (!localizeRr)
        return o_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);

    CachedResourceTags originalTags;
    bool frameBasedTags = false;
    uint32_t staticTagCount = 0;
    uint32_t frameTagCount = 0;
    uint32_t inlineTagCount = 0;
    {
        std::lock_guard lock(resourceTagCacheMutex);
        const auto staticTags = staticResourceTags.find(viewport);
        if (staticTags != staticResourceTags.end())
        {
            originalTags = staticTags->second;
            staticTagCount = static_cast<uint32_t>(staticTags->second.tags.size());
        }

        const auto frameTags = frameResourceTags.find(MakeFrameViewportKey(frameIndex, viewport));
        if (frameTags != frameResourceTags.end())
        {
            originalTags.Merge(frameTags->second.tags.data(),
                               static_cast<uint32_t>(frameTags->second.tags.size()));
            frameBasedTags = true;
            frameTagCount = static_cast<uint32_t>(frameTags->second.tags.size());
        }
    }
    for (uint32_t i = 0; inputs != nullptr && i < numInputs; ++i)
    {
        if (inputs[i] != nullptr && inputs[i]->structType == sl::ResourceTag::s_structType)
        {
            originalTags.Merge(static_cast<const sl::ResourceTag*>(inputs[i]), 1);
            ++inlineTagCount;
        }
    }

    CachedResourceTags localizedTags;
    GazeRoiStreamlineEvaluationContext evaluationContext {};
    const GazeRoiInputSample gaze = GazeRoiInput::Sample();
    if (!BuildLocalizedRrTags(originalTags, gaze, localizedTags, evaluationContext))
    {
        LOG_ERROR("[GROI_SL_RR] frame={} viewport={} missing Color, Depth, MV, Output tag contract; RR ROI skipped",
                  frameIndex, viewport);
        return o_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    }

    std::vector<const sl::BaseStructure*> localizedInputs;
    std::vector<sl::ResourceTag> localizedInlineTags;
    if (inputs != nullptr && numInputs != 0)
    {
        localizedInputs.assign(inputs, inputs + numInputs);
        localizedInlineTags.reserve(numInputs);
        for (uint32_t i = 0; i < numInputs; ++i)
        {
            if (inputs[i] == nullptr || inputs[i]->structType != sl::ResourceTag::s_structType)
                continue;
            localizedInlineTags.push_back(*static_cast<const sl::ResourceTag*>(inputs[i]));
            const auto localized = std::find_if(
                localizedTags.tags.begin(), localizedTags.tags.end(), [&](const sl::ResourceTag& tag)
                {
                    return tag.type == localizedInlineTags.back().type;
                });
            if (localized != localizedTags.tags.end())
                localizedInlineTags.back().extent = localized->extent;
            localizedInputs[i] = &localizedInlineTags.back();
        }
    }

    const sl::ViewportHandle viewportHandle(viewport);
    const auto registerTags = [&](const CachedResourceTags& tags)
    {
        if (frameBasedTags)
            return o_slSetTagForFrame(frame, viewportHandle, tags.tags.data(),
                                      static_cast<uint32_t>(tags.tags.size()), cmdBuffer);
        return o_slSetTag(viewportHandle, tags.tags.data(), static_cast<uint32_t>(tags.tags.size()), cmdBuffer);
    };

    const sl::Result localizedTagResult = registerTags(localizedTags);
    if (localizedTagResult != sl::Result::eOk)
    {
        registerTags(originalTags);
        LOG_ERROR("[GROI_SL_RR] frame={} viewport={} localized tag registration failed result={}",
                  frameIndex, viewport, static_cast<uint32_t>(localizedTagResult));
        return o_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    }

    LogGazeStreamlineTags("rrLocalizedTags", frameIndex, viewport, localizedTags.tags.data(),
                          static_cast<uint32_t>(localizedTags.tags.size()), true);
    LOG_DEBUG("[GROI_SL_RR] frame={} viewport={} localized {} tags sources static={} frame={} inline={}",
              frameIndex, viewport, localizedTags.tags.size(), staticTagCount, frameTagCount, inlineTagCount);
    GazeRoiStreamlineContext::Set(evaluationContext);
    const sl::Result result = o_slEvaluateFeature(
        feature, frame, localizedInputs.empty() ? inputs : localizedInputs.data(), numInputs, cmdBuffer);
    GazeRoiStreamlineContext::Clear();
    const sl::Result restoreResult = registerTags(originalTags);
    if (restoreResult != sl::Result::eOk)
        LOG_ERROR("[GROI_SL_RR] frame={} viewport={} original tag restore failed result={}",
                  frameIndex, viewport, static_cast<uint32_t>(restoreResult));
    return result;
}

sl::Result StreamlineHooks::hkslAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature,
                                                  const sl::ViewportHandle& viewport)
{
    LOG_FUNC();
    auto result = o_slAllocateResources(cmdBuffer, feature, viewport);
    return result;
}

sl::Result StreamlineHooks::hkslGetNativeInterface(void* proxyInterface, void** baseInterface)
{
    LOG_FUNC();
    auto result = o_slGetNativeInterface(proxyInterface, baseInterface);
    return result;
}

sl::Result StreamlineHooks::hkslSetD3DDevice(void* d3dDevice)
{
    LOG_FUNC();
    auto result = o_slSetD3DDevice(d3dDevice);
    return result;
}

void StreamlineHooks::streamlineLogCallback_sl1(sl1::LogType type, const char* msg)
{
    if (msg == nullptr)
        return;

    char* trimmed_msg = trimStreamlineLog(msg);

    if (trimmed_msg != nullptr)
    {
        switch (type)
        {
        case sl1::LogType::eLogTypeWarn:
            LOG_WARN("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeInfo:
            LOG_INFO("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeError:
            LOG_ERROR("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeCount:
            LOG_ERROR("{}", trimmed_msg);
            break;
        }

        free(trimmed_msg);
    }

    if (o_logCallback_sl1)
        o_logCallback_sl1(type, msg);
}

bool StreamlineHooks::hkslInit_sl1(const sl1::Preferences& pref, int applicationId)
{
    LOG_FUNC();

    sl1::Preferences localPref = pref;

    if (localPref.logMessageCallback != &streamlineLogCallback_sl1)
        o_logCallback_sl1 = localPref.logMessageCallback;
    localPref.logLevel = sl1::LogLevel::eLogLevelCount;
    localPref.logMessageCallback = &streamlineLogCallback_sl1;
    return o_slInit_sl1(localPref, applicationId);
}

bool StreamlineHooks::hkslSetTag_sl1(const sl1::Resource* resource, sl1::BufferType tag, uint32_t id,
                                     const sl1::Extent* extent)
{
    if (IsSL1AndFGActive())
        State::Instance().s_sl1FGInputs.setTag(resource, tag, id, extent);

    return o_slSetTag_sl1(resource, tag, id, extent);
}

bool StreamlineHooks::hkslSetConstants_sl1(const sl1::Constants& values, uint32_t frameIndex, uint32_t id)
{
    std::scoped_lock lock(setConstantsMutex);

    LOG_TRACE("SL1 slSetConstants frameIndex: {}, id: {}", frameIndex, id);

    if (IsSL1AndFGActive())
        State::Instance().s_sl1FGInputs.setConstants(values, frameIndex, id);

    return o_slSetConstants_interposer_sl1(values, frameIndex, id);
}

bool StreamlineHooks::hkslEvaluateFeature_sl1(sl1::CommandBuffer* cmdBuffer, sl1::Feature feature, uint32_t frameIndex,
                                              uint32_t id)
{
    LOG_TRACE("SL1 slEvaluateFeature feature: {}, frameIndex: {}, id: {}", magic_enum::enum_name(feature), frameIndex,
              id);

    if (IsSL1AndFGActive())
        State::Instance().s_sl1FGInputs.evaluateFeature(cmdBuffer, feature, frameIndex, id);

    return o_slEvaluateFeature_sl1(cmdBuffer, feature, frameIndex, id);
}

void StreamlineHooks::hookSystemCaps(sl::param::IParameters* params)
{
    if (State::Instance().streamlineVersion.major > 1)
    {
        if (!systemCaps)
            sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &systemCaps);
    }
    else if (State::Instance().streamlineVersion.major == 1)
    {
        // This should be Streamline 1.5 as previous versions don't even have slOnPluginLoad
        if (!systemCapsSl15)
        {
            LOG_TRACE(
                "Attempting to get system caps for Streamline v1, this could fail depending on the exact version");
            sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &systemCapsSl15);
        }
    }
}

uint32_t StreamlineHooks::getSystemCapsArch(SystemCaps* altSystemCaps)
{
    uint32_t highestArch = 0;

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (!fakenvapi::isUsingAsMainNvapi() && primaryGpu.vendorId == VendorId::Nvidia)
    {
        if (State::Instance().streamlineVersion.major > 1)
        {
            auto caps = altSystemCaps != nullptr ? altSystemCaps : systemCaps;
            if (caps)
            {
                for (auto& adapter : caps->adapters)
                {
                    if (adapter.architecture > highestArch)
                        highestArch = adapter.architecture;
                }
            }
        }
        else if (State::Instance().streamlineVersion.major == 1)
        {
            if (systemCapsSl15)
            {
                for (uint32_t i = 0; i < systemCapsSl15->gpuCount; i++)
                {
                    if (systemCapsSl15->architecture[i] > highestArch)
                        highestArch = systemCapsSl15->architecture[i];
                }
            }
        }
    }

    // By default spoof Pascal, gets Reflex but not DLSSD
    // Could be problematic if not using fakenvapi but nvapi might not be initialized yet
    if (highestArch == 0)
        highestArch = NV_GPU_ARCHITECTURE_GP100;

    return highestArch;
}

void StreamlineHooks::setArch(uint32_t arch, SystemCaps* altSystemCaps)
{
    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    // altSystemCaps has to be sl2+
    if (State::Instance().streamlineVersion.major > 1 || altSystemCaps)
    {
        // Assumes that altCaps are always for SL2+
        auto caps = altSystemCaps != nullptr ? altSystemCaps : systemCaps;
        if (caps)
        {
            for (uint32_t i = 0; i < caps->gpuCount; i++)
            {
                caps->adapters[i].architecture = arch;
                caps->adapters[i].vendor = VendorId::Nvidia;
            }

            if (fakenvapi::isUsingAsMainNvapi() || primaryGpu.vendorId != VendorId::Nvidia)
                caps->driverVersionMajor = 999;

            caps->hwsSupported = true;
        }
    }
    else if (State::Instance().streamlineVersion.major == 1)
    {
        if (systemCapsSl15)
        {
            for (uint32_t i = 0; i < systemCapsSl15->gpuCount; i++)
                systemCapsSl15->architecture[i] = arch;

            if (fakenvapi::isUsingAsMainNvapi() || primaryGpu.vendorId != VendorId::Nvidia)
                systemCapsSl15->driverVersionMajor = 999;

            systemCapsSl15->hwSchedulingEnabled = true;
        }
    }
}

// Spoof arch based on feature and current arch
void StreamlineHooks::spoofArch(uint32_t currentArch, sl::Feature feature, SystemCaps* altSystemCaps)
{
    constexpr uint32_t maxArch = 0xFFFFFFFF;

    // Don't change arch for DLSS/DLSSD with turing and above
    if (feature == sl::kFeatureDLSS)
    {
        if (currentArch < NV_GPU_ARCHITECTURE_TU100)
            return setArch(maxArch, altSystemCaps);
    }

    // Don't spoof DLSSD at all
    else if (feature == sl::kFeatureDLSS_RR)
    {
        return;
    }

    // Don't change arch for DLSSG with ada and above
    else if (feature == sl::kFeatureDLSS_G)
    {
        if (State::Instance().activeFgOutput == FGOutput::NvngxFG ||
            State::Instance().activeFgOutput == FGOutput::DLSSGWithNvngx)
        {
            Nvngx_FG::InitDLSSGMod_Dx12();
            Nvngx_FG::InitDLSSGMod_Vulkan();
            if (!Nvngx_FG::isDx12Available() && !Nvngx_FG::isVulkanAvailable())
                return setArch(0);
        }

        if (currentArch < NV_GPU_ARCHITECTURE_AD100)
            return setArch(maxArch, altSystemCaps);
    }

    else if (feature == sl::kFeatureReflex || feature == sl::kFeaturePCL)
    {
        if (fakenvapi::isUsingAsMainNvapi())
            return setArch(maxArch, altSystemCaps);
    }
}

bool StreamlineHooks::hkdlss_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                            const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureDLSS);
    }

    auto result = o_dlss_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (Config::Instance()->VulkanExtensionSpoofing.value_or_default())
        {
            if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
                configJson["external"]["vk"]["instance"]["extensions"].clear();

            if (configJson.contains("/external/vk/device/extensions"_json_pointer))
                configJson["external"]["vk"]["device"]["extensions"].clear();

            if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
                configJson["external"]["vk"]["device"]["1.2_features"].clear();

            if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
                configJson["external"]["vk"]["device"]["1.3_features"].clear();
        }
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslDLSSGetOptimalSettings(const sl::DLSSOptions& options,
                                                       sl::DLSSOptimalSettings& settings)
{
    static bool modesBroken = false;

    auto localOptions = options;

    if (localOptions.mode == sl::DLSSMode::eOff)
        modesBroken = true;

    if (modesBroken)
    {
        if (localOptions.mode == sl::DLSSMode::eMaxPerformance)
            localOptions.mode = sl::DLSSMode::eUltraPerformance;
        else if (localOptions.mode == sl::DLSSMode::eBalanced)
            localOptions.mode = sl::DLSSMode::eMaxPerformance;
        else if (localOptions.mode == sl::DLSSMode::eMaxQuality)
            localOptions.mode = sl::DLSSMode::eBalanced;
        else if (localOptions.mode == sl::DLSSMode::eUltraQuality)
            localOptions.mode = sl::DLSSMode::eMaxQuality;
        else if (localOptions.mode == sl::DLSSMode::eUltraPerformance)
            localOptions.mode = sl::DLSSMode::eDLAA;
    }

    return o_slDLSSGetOptimalSettings(localOptions, settings);
}

bool StreamlineHooks::hkdlssg_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                             const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    bool shouldSpoofArch =
        Config::Instance()->StreamlineSpoofing.value_or_default() &&
        (Config::Instance()->FGInput == FGInput::NvngxFG || Config::Instance()->FGInput == FGInput::DLSSG);

    uint32_t currentArch = 0;
    if (shouldSpoofArch)
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureDLSS_G);
    }

    auto result = o_dlssg_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (shouldSpoofArch)
        setArch(currentArch);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    // Kill the DLSSG streamline swapchain hooks
    if (State::Instance().activeFgInput == FGInput::DLSSG ||
        (State::Instance().activeFgOutput == FGOutput::DLSSG ||
         State::Instance().activeFgOutput == FGOutput::DLSSGWithNvngx))
    {
        if (configJson.contains("/hooks"_json_pointer))
            configJson["hooks"].clear();

        if (configJson.contains("/exclusive_hooks"_json_pointer))
            configJson["exclusive_hooks"].clear();

        if (configJson.contains("/external/feature/tags"_json_pointer))
            configJson["external"]["feature"]["tags"].clear(); // We handle the DLSSG resources

        if (configJson.contains("/external/vk/device/queues/compute/count"_json_pointer))
            configJson["external"]["vk"]["device"]["queues"]["compute"]["count"] = 0;

        if (configJson.contains("/external/vk/device/queues/graphics/count"_json_pointer))
            configJson["external"]["vk"]["device"]["queues"]["graphics"]["count"] = 0;

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgInput == FGInput::NvngxFG)
    {
        if (configJson.contains("/vsync/supported"_json_pointer))
            configJson["vsync"]["supported"] = true; // disable eVSyncOffRequired

        if (configJson.contains("/external/hws/required"_json_pointer))
            configJson["external"]["hws"]["required"] = false; // disable eHardwareSchedulingRequired

        // if (configJson.contains("/external/vk/opticalflow/supported"_json_pointer))
        //     configJson["external"]["vk"]["opticalflow"]["supported"] = true;
    }

    if (Config::Instance()->VulkanExtensionSpoofing.value_or_default())
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

static const char* hkdlssg_slGetPluginJSONConfig_sl1()
{
    static std::string patchedConfig;

    const char* originalConfig = o_dlssg_slGetPluginJSONConfig_sl1();

    if (originalConfig == nullptr)
        return originalConfig;

    try
    {
        auto configJson = nlohmann::json::parse(originalConfig);

        LOG_DEBUG("SL1 DLSSG JSON before patch: {}", configJson.dump());

        PatchSL1PluginJson(configJson);
        // RemoveSL1DLSSGHookEntriesRecursive(configJson);

        patchedConfig = configJson.dump();

        LOG_DEBUG("SL1 DLSSG JSON after patch: {}", patchedConfig);

        return patchedConfig.c_str();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to patch SL1 DLSSG JSON config: {}", e.what());
        return originalConfig;
    }
}

bool StreamlineHooks::hklocal_dlssg_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                                   const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    bool shouldSpoofArch = Config::Instance()->StreamlineSpoofing.value_or_default();

    uint32_t currentArch = 0;
    SystemCaps* localSystemCaps = nullptr;
    if (shouldSpoofArch)
    {
        sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &localSystemCaps);

        if (localSystemCaps)
        {
            currentArch = getSystemCapsArch(localSystemCaps);
            spoofArch(currentArch, sl::kFeatureDLSS_G, localSystemCaps);
        }
    }

    auto result = o_local_dlssg_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (shouldSpoofArch && localSystemCaps)
        setArch(currentArch, localSystemCaps);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    if (configJson.contains("/external/hws/required"_json_pointer))
        configJson["external"]["hws"]["required"] = false; // disable eHardwareSchedulingRequired

    if (Config::Instance()->VulkanExtensionSpoofing.value_or_default())
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslSetConstants(const sl::Constants& values, const sl::FrameToken& frame,
                                              const sl::ViewportHandle& viewport)
{
    std::scoped_lock lock(setConstantsMutex);
    LOG_TRACE("called with frameIndex: {}, viewport: {}", (unsigned int) frame, (unsigned int) viewport);

    LogGazeStreamlineConstants(values, static_cast<uint32_t>(frame), static_cast<uint32_t>(viewport));

    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgInput == FGInput::NvngxFG)
        State::Instance().slFGInputs.setConstants(values, (uint32_t) frame);

    return o_slSetConstants(values, frame, viewport);
}

sl::Result StreamlineHooks::hkslDLSSSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options)
{
    if (IsGazeContractCaptureEnabled())
    {
        std::ostringstream contract;
        contract << "viewport=" << static_cast<uint32_t>(viewport) << " mode=" << static_cast<uint32_t>(options.mode)
                 << " output=" << options.outputWidth << 'x' << options.outputHeight
                 << " sharpness=" << options.sharpness << " preExposure=" << options.preExposure
                 << " exposureScale=" << options.exposureScale
                 << " colorHDR=" << static_cast<int>(options.colorBuffersHDR)
                 << " invertX=" << static_cast<int>(options.indicatorInvertAxisX)
                 << " invertY=" << static_cast<int>(options.indicatorInvertAxisY)
                 << " presets=" << static_cast<uint32_t>(options.dlaaPreset) << ','
                 << static_cast<uint32_t>(options.qualityPreset) << ','
                 << static_cast<uint32_t>(options.balancedPreset) << ','
                 << static_cast<uint32_t>(options.performancePreset) << ','
                 << static_cast<uint32_t>(options.ultraPerformancePreset) << ','
                 << static_cast<uint32_t>(options.ultraQualityPreset)
                 << " autoExposure=" << static_cast<int>(options.useAutoExposure)
                 << " alphaUpscaling=" << static_cast<int>(options.alphaUpscalingEnabled);
        LogUniqueGazeStreamlineContract("dlssOptions", contract.str());
    }

    return o_slDLSSSetOptions(viewport, options);
}

bool StreamlineHooks::hkcommon_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                              const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    auto result = o_common_slOnPluginLoad(params, loaderJSON, pluginJSON);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    auto& slVersion = State::Instance().streamlineVersion;

    // Grab a version of the potentially updated sl.common
    // Opti assumes that all plugins will have this version
    configJson.at("version").at("major").get_to(slVersion.major);
    configJson.at("version").at("minor").get_to(slVersion.minor);
    configJson.at("version").at("build").get_to(slVersion.patch);

    // Completely disables Streamline hooks
    // if (true)
    //    configJson["hooks"].clear();
    //    configJson["exclusive_hooks"].clear();
    //}

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    lastDlssgViewport = viewport;
    lastDlssgOptions = options;

    // Avoid reading past the game's struct's size
    sl::DLSSGOptions newOptions {};
    auto newStructVer = newOptions.structVersion;

    if (options.structVersion == 1)
        memcpy(&newOptions, &options, 104);
    else if (options.structVersion == 2 || options.structVersion == 3)
        memcpy(&newOptions, &options, 112);
    else if (options.structVersion == 4 || options.structVersion == 5)
        memcpy(&newOptions, &options, 120);
    else
        newOptions = options;

    newOptions.structVersion = newStructVer;

    auto& state = State::Instance();

    if (state.activeFgInput != FGInput::DLSSG &&
        (state.activeFgOutput == FGOutput::DLSSG || state.activeFgOutput == FGOutput::DLSSGWithNvngx))
    {
        newOptions.mode = sl::DLSSGMode::eOff;
        return o_slDLSSGSetOptions(viewport, newOptions);
    }

    // Make DLSSG auto always mean On
    if (newOptions.mode == sl::DLSSGMode::eAuto)
        newOptions.mode = sl::DLSSGMode::eOn;

    const auto dlssgPotentiallyActive = newOptions.mode == sl::DLSSGMode::eOn ||
                                        newOptions.mode == sl::DLSSGMode::eAuto ||
                                        newOptions.mode == sl::DLSSGMode::eDynamic;

    bool enableDynamicMode = Config::Instance()->FGDLSSGOverrideForceDMFG.value_or_default() &&
                             state.dlssgGameDMFGSupported && dlssgPotentiallyActive;

    if (enableDynamicMode)
    {
        newOptions.mode = sl::DLSSGMode::eDynamic;
    }

    if (newOptions.mode == sl::DLSSGMode::eDynamic && Config::Instance()->FGDLSSGFramerateTargetDMFG.has_value())
    {
        newOptions.dynamicTargetFrameRate = Config::Instance()->FGDLSSGFramerateTargetDMFG.value();
    }

    if (state.swapchainApi == API::Vulkan)
    {
        // Only matters for Vulkan, DX doesn't use this delay
        if (dlssgPotentiallyActive && !MenuOverlayBase::IsVisible())
            state.delayMenuRenderBy = 10;

        if (MenuOverlayBase::IsVisible())
        {
            newOptions.mode = sl::DLSSGMode::eOff;
            newOptions.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
            ReflexHooks::setDlssgFrameCount(0);
        }
    }

    LOG_TRACE("DLSSG Modified Mode: {}", magic_enum::enum_name(newOptions.mode));

    if (dlssgPotentiallyActive && state.streamlineVersion >= feature_version { 2, 7, 1 })
    {
        // Populate dlssgMfgMax once
        if (!state.dlssgMfgMax.has_value())
        {
            sl::DLSSGState localState {};
            sl::DLSSGOptions localOptions {};
            if (o_slDLSSGGetState(viewport, localState, &localOptions) == sl::Result::eOk &&
                localState.numFramesToGenerateMax > 0 && localState.numFramesToGenerateMax < 6)
            {
                state.dlssgMfgMax = localState.numFramesToGenerateMax;
                LOG_TRACE("Saving original numFramesToGenerateMax: {}", state.dlssgMfgMax.value());

                if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
                    Config::Instance()->FGDLSSGOverrideInterpolationCount.value() > state.dlssgMfgMax.value())
                {
                    Config::Instance()->FGDLSSGOverrideInterpolationCount = state.dlssgMfgMax.value();
                }
            }
        }

        // Won't take effect with Dynamic
        if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value())
        {
            auto overrideCount = Config::Instance()->FGDLSSGOverrideInterpolationCount.value();
            if (overrideCount != 0)
                newOptions.numFramesToGenerate = overrideCount;
            else if (!enableDynamicMode)
                newOptions.mode = sl::DLSSGMode::eOff;
        }
    }

    state.dlssgLastSetMode = newOptions.mode;

    return o_slDLSSGSetOptions(viewport, newOptions);
}

sl::Result StreamlineHooks::hkslDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                              const sl::DLSSGOptions* options)
{
    sl::Result result {};

    const auto originalStructVersion = state.structVersion;
    if (originalStructVersion < 4)
    {
        sl::DLSSGState newState {};

        // We might be feeding a newer struct to an older SL but that seems to work just fine for this Get function
        result = o_slDLSSGGetState(viewport, dynamic_cast<sl::DLSSGState&>(newState), options);

        // Copy back data to game's struct
        memcpy(&state, &newState, 56); // struct ver 1 size
        state.structVersion = originalStructVersion;

        if (originalStructVersion >= 2)
        {
            state.numFramesToGenerateMax = newState.numFramesToGenerateMax;
            state.bReserved4 = newState.bReserved4;
            state.bIsVsyncSupportAvailable = newState.bIsVsyncSupportAvailable;
        }

        if (originalStructVersion >= 3)
        {
            state.inputsProcessingCompletionFence = newState.inputsProcessingCompletionFence;
            state.lastPresentInputsProcessingCompletionFenceValue =
                newState.lastPresentInputsProcessingCompletionFenceValue;
        }

        State::Instance().dlssgGameDMFGSupported = newState.bIsDynamicMFGSupported == sl::eTrue;
    }
    else
    {
        result = o_slDLSSGGetState(viewport, state, options);
        State::Instance().dlssgGameDMFGSupported = state.bIsDynamicMFGSupported == sl::eTrue;
    }

    if (!State::Instance().dlssgGameDMFGSupported)
    {
        Config::Instance()->FGDLSSGOverrideForceDMFG.set_volatile_value(false);
    }

    auto& optiState = State::Instance();

    if (optiState.streamlineVersion >= feature_version { 2, 7, 1 })
    {
        if (!optiState.dlssgMfgMax.has_value())
        {
            sl::DLSSGState localState {};
            sl::DLSSGOptions localOptions {};
            if (o_slDLSSGGetState(viewport, localState, &localOptions) == sl::Result::eOk &&
                localState.numFramesToGenerateMax > 0 && localState.numFramesToGenerateMax < 6)
            {
                optiState.dlssgMfgMax = localState.numFramesToGenerateMax;
                LOG_TRACE("Saving original numFramesToGenerateMax: {}", optiState.dlssgMfgMax.value());

                if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
                    Config::Instance()->FGDLSSGOverrideInterpolationCount.value() > optiState.dlssgMfgMax.value())
                {
                    Config::Instance()->FGDLSSGOverrideInterpolationCount = optiState.dlssgMfgMax.value();
                }
            }
        }
    }

    if (optiState.activeFgInput == FGInput::DLSSG)
    {
        auto fg = optiState.currentFG;

        if (fg != nullptr)
        {
            if (options != nullptr && options->flags & sl::DLSSGFlags::eRequestVRAMEstimate)
                state.estimatedVRAMUsageInBytes = static_cast<uint64_t>(256 * 1024) * 1024;

            if (fg->IsActive() && !fg->IsPaused())
            {
                state.numFramesActuallyPresented = fg->GetInterpolatedFrameCount() + 1;
            }
            else
            {
                state.numFramesActuallyPresented = 1;
            }
        }
        else
        {
            state.numFramesActuallyPresented = 1;
        }

        state.numFramesToGenerateMax = 1;

        LOG_DEBUG("Status: {}, numFramesActuallyPresented: {}", magic_enum::enum_name(state.status),
                  state.numFramesActuallyPresented);
    }

    return result;
}

bool StreamlineHooks::hkreflex_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                              const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureReflex);
    }

    auto result = o_reflex_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    if (IdentifyGpu::getPrimaryGpu().vendorId != VendorId::Nvidia &&
        Config::Instance()->VulkanExtensionSpoofing.value_or_default())
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslReflexSetOptions(const sl::ReflexOptions& options)
{
    reflexGamesLastMode = options.mode;

    sl::ReflexOptions newOptions = options;

    if (Config::Instance()->FN_ForceReflex == ForceReflex::ForceEnable)
        newOptions.mode = sl::ReflexMode::eLowLatencyWithBoost;

    // Will cause a pink screen when used with DLSSG
    // if (Config::Instance()->FN_ForceReflex == 1)
    //     newOptions.mode = sl::ReflexMode::eOff;

    return o_slReflexSetOptions(newOptions);
}

sl::Result StreamlineHooks::hkslReflexSleep(const sl::FrameToken& frame)
{
    // if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
    //     Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default())
    //{
    //     return StreamlineProxy::ReflexSleep()(frame);
    // }

    return o_slReflexSleep(frame);
}

void* StreamlineHooks::hkdlss_slGetPluginFunction(const char* functionName)
{
    LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_dlss_slOnPluginLoad = (PFN_slOnPluginLoad) o_dlss_slGetPluginFunction(functionName);
        return &hkdlss_slOnPluginLoad;
    }

    if (strcmp(functionName, "slDLSSGetOptimalSettings") == 0 &&
        State::Instance().gameQuirks & GameQuirk::PregmataFixDLSSModes)
    {
        o_slDLSSGetOptimalSettings = (decltype(&slDLSSGetOptimalSettings)) o_dlss_slGetPluginFunction(functionName);
        return &hkslDLSSGetOptimalSettings;
    }

    if (strcmp(functionName, "slDLSSSetOptions") == 0 && IsGazeContractCaptureEnabled())
    {
        o_slDLSSSetOptions = (decltype(&slDLSSSetOptions)) o_dlss_slGetPluginFunction(functionName);
        return o_slDLSSSetOptions != nullptr ? &hkslDLSSSetOptions : nullptr;
    }

    return o_dlss_slGetPluginFunction(functionName);
}

void* StreamlineHooks::hkdlssg_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_dlssg_slOnPluginLoad = (PFN_slOnPluginLoad) o_dlssg_slGetPluginFunction(functionName);
        return &hkdlssg_slOnPluginLoad;
    }

    if (strcmp(functionName, "slDLSSGSetOptions") == 0)
    {
        o_slDLSSGSetOptions = (decltype(&slDLSSGSetOptions)) o_dlssg_slGetPluginFunction(functionName);

        // Give steam overlay the original as it seems to be hooking it
        auto steamOverlay = KernelBaseProxy::GetModuleHandleA_()("gameoverlayrenderer64.dll");
        if (steamOverlay != nullptr)
        {
            if (HMODULE callerModule = Util::GetCallerModule(_ReturnAddress()); callerModule == steamOverlay)
                return o_slDLSSGSetOptions;
        }

        return &hkslDLSSGSetOptions;
    }

    if (strcmp(functionName, "slDLSSGGetState") == 0)
    {
        o_slDLSSGGetState = (decltype(&slDLSSGGetState)) o_dlssg_slGetPluginFunction(functionName);

        // Give steam overlay the original as it seems to be hooking it
        auto steamOverlay = KernelBaseProxy::GetModuleHandleA_()("gameoverlayrenderer64.dll");
        if (steamOverlay != nullptr)
        {
            if (HMODULE callerModule = Util::GetCallerModule(_ReturnAddress()); callerModule == steamOverlay)
                return o_slDLSSGGetState;
        }

        return &hkslDLSSGGetState;
    }

    if (strcmp(functionName, "slGetPluginJSONConfig") == 0 && IsSL1AndDLSSGActive())
    {
        o_dlssg_slGetPluginJSONConfig_sl1 =
            reinterpret_cast<PFN_slGetPluginJSONConfig_sl1>(o_dlssg_slGetPluginFunction(functionName));

        if (o_dlssg_slGetPluginJSONConfig_sl1 != nullptr)
        {
            LOG_WARN("Hooking SL1 DLSSG slGetPluginJSONConfig");
            return &hkdlssg_slGetPluginJSONConfig_sl1;
        }
    }

    // Ensure that we have those DLSSG calls
    if (!o_slDLSSGSetOptions)
        o_slDLSSGSetOptions = (decltype(&slDLSSGSetOptions)) o_dlssg_slGetPluginFunction("slDLSSGSetOptions");

    if (!o_slDLSSGGetState)
        o_slDLSSGGetState = (decltype(&slDLSSGGetState)) o_dlssg_slGetPluginFunction("slDLSSGGetState");

    return o_dlssg_slGetPluginFunction(functionName);
}

void* StreamlineHooks::hklocal_dlssg_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0 && Config::Instance()->FGOutput == FGOutput::DLSSGWithNvngx)
    {
        o_local_dlssg_slOnPluginLoad = (PFN_slOnPluginLoad) o_local_dlssg_slGetPluginFunction(functionName);
        return &hklocal_dlssg_slOnPluginLoad;
    }

    return o_local_dlssg_slGetPluginFunction(functionName);
}

bool StreamlineHooks::hkreflex_slSetConstants_sl1(const void* data, uint32_t frameIndex, uint32_t id)
{
    // Streamline v1's version of slReflexSetOptions + slPCLSetMarker
    static sl1::ReflexConstants constants {};
    constants = *(const sl1::ReflexConstants*) data;

    reflexGamesLastMode = (sl::ReflexMode) constants.mode;

    LOG_DEBUG("mode: {}, frameIndex: {}, id: {}", (uint32_t) constants.mode, frameIndex, id);

    if (Config::Instance()->FN_ForceReflex == ForceReflex::ForceEnable)
        constants.mode = sl1::ReflexMode::eReflexModeLowLatencyWithBoost;

    // Will cause a pink screen when used with DLSSG
    // else if (Config::Instance()->FN_ForceReflex == 1)
    //     constants.mode = sl1::ReflexMode::eReflexModeOff;

    return o_reflex_slSetConstants_sl1(&constants, frameIndex, id);
}

void* StreamlineHooks::hkreflex_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slSetConstants") == 0 && State::Instance().streamlineVersion.major == 1)
    {
        o_reflex_slSetConstants_sl1 = (PFN_slSetConstants_sl1) o_reflex_slGetPluginFunction(functionName);
        return &hkreflex_slSetConstants_sl1;
    }

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_reflex_slOnPluginLoad = (PFN_slOnPluginLoad) o_reflex_slGetPluginFunction(functionName);
        return &hkreflex_slOnPluginLoad;
    }

    if (strcmp(functionName, "slReflexSetOptions") == 0)
    {
        o_slReflexSetOptions = (decltype(&slReflexSetOptions)) o_reflex_slGetPluginFunction(functionName);
        return &hkslReflexSetOptions;
    }

    if (strcmp(functionName, "slReflexSleep") == 0)
    {
        o_slReflexSleep = (decltype(&slReflexSleep)) o_reflex_slGetPluginFunction(functionName);
        return &hkslReflexSleep;
    }

    // TODO: Hopefully a game doesn't call both, maybe separate
    if (strcmp(functionName, "slReflexSetMarker") == 0 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||
         State::Instance().activeFgInput == FGInput::DLSSG))
    {
        o_slPCLSetMarker = (decltype(&slPCLSetMarker)) o_reflex_slGetPluginFunction(functionName);
        return &hkslPCLSetMarker;
    }

    return o_reflex_slGetPluginFunction(functionName);
}

sl::Result StreamlineHooks::hkslPCLSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame)
{
    // if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
    //     Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default())
    //{
    //     return StreamlineProxy::PCLSetMarker()(marker, frame);
    // }

    // HACK for broken games
    if (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers)
    {
        static uint64_t last_simulation_end_id = 0;
        if (marker == sl::PCLMarker::eSimulationEnd)
        {
            last_simulation_end_id = frame;
        }

        if (marker == sl::PCLMarker::eSimulationStart && last_simulation_end_id >= frame && o_slGetNewFrameToken)
        {
            const uint64_t correction_offset = last_simulation_end_id - frame + 1;
            uint32_t newFrameId = static_cast<uint32_t>(frame + correction_offset);

            sl::FrameToken* newFramePointer {};
            auto result = o_slGetNewFrameToken(newFramePointer, &newFrameId);

            LOG_WARN("Simulation start marker sent after end marker, offset: {}", correction_offset);

            result = o_slPCLSetMarker(marker, *newFramePointer);
            return result;
        }
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG)
    {
        if (State::Instance().streamlineVersion.major == 1)
        {
            if (marker == sl::PCLMarker::eRenderSubmitStart)
            {
                State::Instance().s_sl1FGInputs.evaluateState();
            }
            else if (marker == sl::PCLMarker::ePresentStart)
            {
                State::Instance().s_sl1FGInputs.markPresent(frame);
            }
        }
        else
        {
            if (marker == sl::PCLMarker::eRenderSubmitStart)
            {
                State::Instance().slFGInputs.evaluateState();
            }
            else if (marker == sl::PCLMarker::ePresentStart)
            {
                State::Instance().slFGInputs.markPresent(frame);
            }
        }
    }

    return o_slPCLSetMarker(marker, frame);
}

bool StreamlineHooks::hkpcl_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                           const char** pluginJSON)
{
    LOG_FUNC();

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeaturePCL);
    }

    auto result = o_pcl_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    return result;
}

void* StreamlineHooks::hkpcl_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slPCLSetMarker") == 0 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||
         State::Instance().activeFgInput == FGInput::DLSSG))
    {
        o_slPCLSetMarker = (decltype(&slPCLSetMarker)) o_pcl_slGetPluginFunction(functionName);
        return &hkslPCLSetMarker;
    }

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_pcl_slOnPluginLoad = (PFN_slOnPluginLoad) o_pcl_slGetPluginFunction(functionName);
        return &hkpcl_slOnPluginLoad;
    }

    return o_pcl_slGetPluginFunction(functionName);
}

bool StreamlineHooks::hk_setVoid(void* self, const char* key, void** value)
{
    // LOG_DEBUG("{}", key);

    if (strcmp(key, sl::param::common::kSystemCaps) == 0)
    {
        LOG_TRACE("Attempting to change system caps for Streamline v1, this could fail depending on the exact version");

        // SystemCapsSl15 is not entirely correct for Streamline 1.3
        // But we here only use the beginning that matches + extra
        auto caps = (SystemCapsSl15*) value;

        if (caps)
        {
            caps->gpuCount = 1;
            caps->architecture[0] = UINT_MAX;
            caps->driverVersionMajor = 999;

            // HAGS
            *((char*) value + 56) = (char) 0x01;
        }
    }

    return o_setVoid(self, key, value);
}

void StreamlineHooks::hkcommon_slSetParameters_sl1(void* params)
{
    LOG_FUNC();

    if (o_setVoid == nullptr && params)
    {
        void** vtable = *(void***) params;

        // It's flipped, 0 -> set void*, 7 -> get void*
        o_setVoid = (PFN_setVoid) vtable[0];

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_setVoid != nullptr)
            DetourAttach(&(PVOID&) o_setVoid, hk_setVoid);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook setVoid: {:X}", detourResult);
            o_setVoid = nullptr;
        }
    }

    o_common_slSetParameters_sl1(params);
}

void* StreamlineHooks::hkcommon_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_common_slOnPluginLoad = (PFN_slOnPluginLoad) o_common_slGetPluginFunction(functionName);
        return &hkcommon_slOnPluginLoad;
    }

    // Used around Streamline v1.3, as 1.5 doesn't seem to have it anymore
    if (strcmp(functionName, "slSetParameters") == 0)
    {
        o_common_slSetParameters_sl1 = (PFN_slSetParameters_sl1) o_common_slGetPluginFunction(functionName);
        return &hkcommon_slSetParameters_sl1;
    }

    return o_common_slGetPluginFunction(functionName);
}

void StreamlineHooks::updateForceReflex()
{
    // Not needed for Streamline v1 as slSetConstants is sent every frame
    if (o_slReflexSetOptions)
    {
        sl::ReflexOptions options;

        auto forceReflex = Config::Instance()->FN_ForceReflex.value_or_default();

        if (forceReflex == ForceReflex::ForceEnable)
            options.mode = sl::ReflexMode::eLowLatencyWithBoost;
        else if (forceReflex == ForceReflex::ForceDisable)
            options.mode = sl::ReflexMode::eOff;
        else if (forceReflex == ForceReflex::InGame)
            options.mode = reflexGamesLastMode;

        auto result = o_slReflexSetOptions(options);
        if (result != sl::Result::eOk)
        {
            LOG_WARN("Failed to update Reflex mode with error code: {} ({:X})", magic_enum::enum_name(result),
                     (UINT) result);
        }
    }
}

void StreamlineHooks::updateDlssgOptions()
{
    if (o_slDLSSGSetOptions)
    {
        LOG_FUNC();
        hkslDLSSGSetOptions(lastDlssgViewport, lastDlssgOptions);
    }
}

// SL INTERPOSER

void StreamlineHooks::unhookInterposer()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_slSetTag)
        DetourDetach(&(PVOID&) o_slSetTag, hkslSetTag);

    if (o_slSetTagForFrame)
        DetourDetach(&(PVOID&) o_slSetTagForFrame, hkslSetTagForFrame);

    if (o_slSetConstants)
        DetourDetach(&(PVOID&) o_slSetConstants, hkslSetConstants);

    if (o_slEvaluateFeature)
        DetourDetach(&(PVOID&) o_slEvaluateFeature, hkslEvaluateFeature);

    if (o_slInit)
        DetourDetach(&(PVOID&) o_slInit, hkslInit);

    if (o_slInit_sl1)
        DetourDetach(&(PVOID&) o_slInit_sl1, hkslInit_sl1);

    if (o_slSetTag_sl1)
        DetourDetach(&(PVOID&) o_slSetTag_sl1, hkslSetTag_sl1);

    if (o_slSetConstants_interposer_sl1)
        DetourDetach(&(PVOID&) o_slSetConstants_interposer_sl1, hkslSetConstants_sl1);

    if (o_slEvaluateFeature_sl1)
        DetourDetach(&(PVOID&) o_slEvaluateFeature_sl1, hkslEvaluateFeature_sl1);

    // if (o_logCallback)
    //     DetourDetach(&(PVOID&) o_logCallback, streamlineLogCallback);
    // else if (o_logCallback_sl1)
    //     DetourDetach(&(PVOID&) o_logCallback_sl1, streamlineLogCallback);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("DetourTransactionCommit error: {:X}", detourResult);
    }
    else
    {
        o_slInit = nullptr;
        o_slInit_sl1 = nullptr;
        o_slSetTag = nullptr;
        o_slSetTagForFrame = nullptr;
        o_slEvaluateFeature = nullptr;
        o_slSetConstants = nullptr;
        o_slSetTag_sl1 = nullptr;
        o_slSetConstants_interposer_sl1 = nullptr;
        o_slEvaluateFeature_sl1 = nullptr;
        o_logCallback = nullptr;
        o_logCallback_sl1 = nullptr;
    }
}

// Call it just after sl.interposer's load or if sl.interposer is already loaded
void StreamlineHooks::hookInterposer(HMODULE slInterposer)
{
    LOG_FUNC();

    if (!slInterposer)
    {
        LOG_WARN("Streamline module in NULL");
        return;
    }

    // Interposer needs this or it might end in an infinite loop calling itself
    static HMODULE last_slInterposer = nullptr;

    if (last_slInterposer == slInterposer)
        return;

    last_slInterposer = slInterposer;

    // Looks like when reading DLL version load methods are called
    // To prevent loops disabling checks for sl.interposer.dll
    auto owner = State::GetOwner();
    State::DisableChecks(owner, "sl.interposer");

    if (o_slSetTag || o_slInit || o_slInit_sl1 || o_slSetTag_sl1 || o_slSetConstants_interposer_sl1 ||
        o_slEvaluateFeature_sl1)
        unhookInterposer();

    {
        char dllPath[MAX_PATH];
        GetModuleFileNameA(slInterposer, dllPath, MAX_PATH);

        LOG_TRACE("slInterposer path: {}", dllPath);

        Util::version_t sl_version;
        Util::GetFileVersion(string_to_wstring(dllPath), &sl_version);

        State::Instance().streamlineVersion.major = sl_version.major;
        State::Instance().streamlineVersion.minor = sl_version.minor;
        State::Instance().streamlineVersion.patch = sl_version.patch;

        LOG_INFO("Streamline version: {}.{}.{}", sl_version.major, sl_version.minor, sl_version.patch);

        if (sl_version.major >= 2)
        {
            o_slSetTag =
                reinterpret_cast<decltype(&slSetTag)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTag"));
            o_slSetTagForFrame = reinterpret_cast<decltype(&slSetTagForFrame)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTagForFrame"));
            o_slInit = reinterpret_cast<decltype(&slInit)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slInit"));
            o_slEvaluateFeature = reinterpret_cast<decltype(&slEvaluateFeature)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slEvaluateFeature"));
            o_slAllocateResources = reinterpret_cast<decltype(&slAllocateResources)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slAllocateResources"));
            o_slSetConstants = reinterpret_cast<decltype(&slSetConstants)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetConstants"));
            o_slGetNativeInterface = reinterpret_cast<decltype(&slGetNativeInterface)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetNativeInterface"));
            o_slSetD3DDevice = reinterpret_cast<decltype(&slSetD3DDevice)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetD3DDevice"));
            o_slGetNewFrameToken = reinterpret_cast<decltype(&slGetNewFrameToken)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetNewFrameToken")); // Not hooked

            if (o_slInit != nullptr)
            {
                LOG_TRACE("Hooking v2");
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                DetourAttach(&(PVOID&) o_slInit, hkslInit);

                bool hookSetTag = (State::Instance().activeFgInput == FGInput::NvngxFG ||
                                   State::Instance().activeFgInput == FGInput::DLSSG ||
                                   IsGazeContractCaptureEnabled());

                if (o_slSetTag != nullptr && hookSetTag)
                    DetourAttach(&(PVOID&) o_slSetTag, hkslSetTag);

                if (o_slSetTagForFrame != nullptr && hookSetTag)
                    DetourAttach(&(PVOID&) o_slSetTagForFrame, hkslSetTagForFrame);

                if (o_slSetConstants != nullptr && hookSetTag)
                    DetourAttach(&(PVOID&) o_slSetConstants, hkslSetConstants);

                if (o_slEvaluateFeature != nullptr)
                    DetourAttach(&(PVOID&) o_slEvaluateFeature, hkslEvaluateFeature);

                // if (o_slAllocateResources != nullptr)
                //     DetourAttach(&(PVOID&) o_slAllocateResources, hkslAllocateResources);

                // if (o_slGetNativeInterface != nullptr)
                //     DetourAttach(&(PVOID&) o_slGetNativeInterface, hkslGetNativeInterface);

                // if (o_slSetD3DDevice != nullptr)
                //     DetourAttach(&(PVOID&) o_slSetD3DDevice, hkslSetD3DDevice);

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook sl.interposer v2: {:X}", detourResult);
                    o_slSetTag = nullptr;
                    o_slSetTagForFrame = nullptr;
                    o_slInit = nullptr;
                    o_slEvaluateFeature = nullptr;
                    o_slAllocateResources = nullptr;
                    o_slSetConstants = nullptr;
                    o_slGetNativeInterface = nullptr;
                    o_slSetD3DDevice = nullptr;
                }
            }
        }
        else if (sl_version.major == 1)
        {
            o_slInit_sl1 =
                reinterpret_cast<decltype(&sl1::slInit)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slInit"));
            o_slSetTag_sl1 = reinterpret_cast<decltype(&sl1::slSetTag)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTag"));
            o_slSetConstants_interposer_sl1 = reinterpret_cast<decltype(&sl1::slSetConstants)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetConstants"));
            o_slEvaluateFeature_sl1 = reinterpret_cast<decltype(&sl1::slEvaluateFeature)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slEvaluateFeature"));

            LOG_INFO("SL1 exports - slInit: {}, slSetTag: {}, slSetConstants: {}, slEvaluateFeature: {}",
                     o_slInit_sl1 != nullptr, o_slSetTag_sl1 != nullptr, o_slSetConstants_interposer_sl1 != nullptr,
                     o_slEvaluateFeature_sl1 != nullptr);

            if (o_slInit_sl1 || o_slSetTag_sl1 || o_slSetConstants_interposer_sl1 || o_slEvaluateFeature_sl1)
            {
                LOG_TRACE("Hooking v1");
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                if (o_slInit_sl1)
                    DetourAttach(&(PVOID&) o_slInit_sl1, hkslInit_sl1);

                if (IsSL1AndFGActive())
                {
                    if (o_slSetTag_sl1)
                        DetourAttach(&(PVOID&) o_slSetTag_sl1, hkslSetTag_sl1);

                    if (o_slSetConstants_interposer_sl1)
                        DetourAttach(&(PVOID&) o_slSetConstants_interposer_sl1, hkslSetConstants_sl1);

                    if (o_slEvaluateFeature_sl1)
                        DetourAttach(&(PVOID&) o_slEvaluateFeature_sl1, hkslEvaluateFeature_sl1);
                }

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook sl.interposer v1: {:X}", detourResult);
                    o_slInit_sl1 = nullptr;
                    o_slSetTag_sl1 = nullptr;
                    o_slSetConstants_interposer_sl1 = nullptr;
                    o_slEvaluateFeature_sl1 = nullptr;
                }
            }
        }
    }

    State::EnableChecks(owner);
}

// SL DLSS

void StreamlineHooks::unhookDlss()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_dlss_slGetPluginFunction)
        DetourDetach(&(PVOID&) o_dlss_slGetPluginFunction, hkdlss_slGetPluginFunction);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook DLSS: {:X}", detourResult);
    }
    else
    {
        o_dlss_slGetPluginFunction = nullptr;
    }
}

void StreamlineHooks::hookDlss(HMODULE slDlss)
{
    LOG_FUNC();

    if (!slDlss)
    {
        LOG_WARN("Dlss module in NULL");
        return;
    }

    if (o_dlss_slGetPluginFunction)
        unhookDlss();

    o_dlss_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slDlss, "slGetPluginFunction"));

    if (o_dlss_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in sl.dlss");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_dlss_slGetPluginFunction, hkdlss_slGetPluginFunction);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook DLSS: {:X}", detourResult);
            o_dlss_slGetPluginFunction = nullptr;
        }
    }
}

// SL DLSSG

void StreamlineHooks::unhookDlssg()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_dlssg_slGetPluginFunction)
        DetourDetach(&(PVOID&) o_dlssg_slGetPluginFunction, hkdlssg_slGetPluginFunction);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook DLSSG: {:X}", detourResult);
        o_dlssg_slGetPluginFunction = nullptr;
    }
}

void StreamlineHooks::hookDlssg(HMODULE slDlssg)
{
    LOG_FUNC();

    if (!slDlssg)
    {
        LOG_WARN("Dlssg module in NULL");
        return;
    }

    if (o_dlssg_slGetPluginFunction)
        unhookDlssg();

    o_dlssg_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slDlssg, "slGetPluginFunction"));

    if (o_dlssg_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in sl.dlssg");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_dlssg_slGetPluginFunction, hkdlssg_slGetPluginFunction);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook DLSSG: {:X}", detourResult);
            o_dlssg_slGetPluginFunction = nullptr;
        }
    }
}

// Local SL DLSSG

void StreamlineHooks::unhookLocalDlssg()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_local_dlssg_slGetPluginFunction)
    {
        DetourDetach(&(PVOID&) o_local_dlssg_slGetPluginFunction, hklocal_dlssg_slGetPluginFunction);
        o_local_dlssg_slGetPluginFunction = nullptr;
    }

    DetourTransactionCommit();
}

void StreamlineHooks::hookLocalDlssg(HMODULE slDlssg)
{
    LOG_FUNC();

    if (!slDlssg)
    {
        LOG_WARN("Dlssg module in NULL");
        return;
    }

    if (o_local_dlssg_slGetPluginFunction)
        unhookLocalDlssg();

    o_local_dlssg_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slDlssg, "slGetPluginFunction"));

    if (o_local_dlssg_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in local sl.dlssg");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_local_dlssg_slGetPluginFunction, hklocal_dlssg_slGetPluginFunction);

        DetourTransactionCommit();
    }
}

// SL REFLEX

void StreamlineHooks::unhookReflex()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_reflex_slGetPluginFunction)
        DetourDetach(&(PVOID&) o_reflex_slGetPluginFunction, hkreflex_slGetPluginFunction);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook Reflex: {:X}", detourResult);
    }
    else
    {
        o_reflex_slGetPluginFunction = nullptr;
    }
}

void StreamlineHooks::hookReflex(HMODULE slReflex)
{
    LOG_FUNC();

    if (!slReflex)
    {
        LOG_WARN("Reflex module in NULL");
        return;
    }

    if (o_reflex_slGetPluginFunction)
        unhookReflex();

    o_reflex_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slReflex, "slGetPluginFunction"));

    if (o_reflex_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in sl.reflex");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_reflex_slGetPluginFunction, hkreflex_slGetPluginFunction);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook Reflex: {:X}", detourResult);
            o_reflex_slGetPluginFunction = nullptr;
        }
    }
}

// SL PCL

void StreamlineHooks::unhookPcl()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_pcl_slGetPluginFunction)
        DetourDetach(&(PVOID&) o_pcl_slGetPluginFunction, hkpcl_slGetPluginFunction);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook PCL: {:X}", detourResult);
    }
    else
    {
        o_pcl_slGetPluginFunction = nullptr;
    }
}

void StreamlineHooks::hookPcl(HMODULE slPcl)
{
    LOG_FUNC();

    if (!slPcl)
    {
        LOG_WARN("Pcl module in NULL");
        return;
    }

    if (o_pcl_slGetPluginFunction)
        unhookPcl();

    o_pcl_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slPcl, "slGetPluginFunction"));

    if (o_pcl_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in sl.pcl");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_pcl_slGetPluginFunction, hkpcl_slGetPluginFunction);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook PCL: {:X}", detourResult);
            o_pcl_slGetPluginFunction = nullptr;
        }
    }
}

// SL COMMON

void StreamlineHooks::unhookCommon()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_common_slGetPluginFunction)
        DetourDetach(&(PVOID&) o_common_slGetPluginFunction, hkcommon_slGetPluginFunction);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook Common: {:X}", detourResult);
    }
    else
    {
        systemCaps = nullptr;
        systemCapsSl15 = nullptr;
        o_common_slGetPluginFunction = nullptr;
    }
}

void StreamlineHooks::hookCommon(HMODULE slCommon)
{
    LOG_FUNC();

    if (!slCommon)
    {
        LOG_WARN("Common module in NULL");
        return;
    }

    if (o_common_slGetPluginFunction)
        unhookCommon();

    o_common_slGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(KernelBaseProxy::GetProcAddress_()(slCommon, "slGetPluginFunction"));

    if (o_common_slGetPluginFunction != nullptr)
    {
        LOG_TRACE("Hooking slGetPluginFunction in sl.common");
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_common_slGetPluginFunction, hkcommon_slGetPluginFunction);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook Common: {:X}", detourResult);
            o_common_slGetPluginFunction = nullptr;
        }
    }
}

bool StreamlineHooks::isInterposerHooked() { return o_slInit != nullptr || o_slInit_sl1 != nullptr; }

bool StreamlineHooks::isDlssHooked() { return o_dlss_slGetPluginFunction != nullptr; }

bool StreamlineHooks::isDlssgHooked() { return o_dlssg_slGetPluginFunction != nullptr; }

bool StreamlineHooks::isLocalDlssgHooked() { return o_local_dlssg_slGetPluginFunction != nullptr; }

bool StreamlineHooks::isCommonHooked() { return o_common_slGetPluginFunction != nullptr; }

bool StreamlineHooks::isPclHooked() { return o_pcl_slGetPluginFunction != nullptr; }

bool StreamlineHooks::isReflexHooked() { return o_reflex_slGetPluginFunction != nullptr; }
