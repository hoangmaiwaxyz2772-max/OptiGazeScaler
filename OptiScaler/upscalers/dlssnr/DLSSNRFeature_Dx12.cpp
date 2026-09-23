#include <pch.h>
#include "DLSSNRFeature_Dx12.h"
#include "DLSSNRPreview.h"
#include "DLSSNRLatePass.h"
#include "DLSSNRExtrapolation.h"
#include <DLSSNRExtrapolationShaders.h>

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <proxies/NVNGX_Proxy.h>
#include <resource_tracking/ResTrack_dx12.h>
#include <shaders/Shader_Common.h>

#include <d3dx/d3dx12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

namespace
{
class DLSSNRFrameSyncGuard
{
  public:
    explicit DLSSNRFrameSyncGuard(ID3D12GraphicsCommandList* commandList, std::optional<uint32_t> slot = std::nullopt)
        : _commandList(commandList)
    {
        if (slot) { _slot = *slot; _valid = true; }
        else _valid = _acquired = GazeRoiFrameSync::TryAcquire(_commandList, _slot);
    }

    ~DLSSNRFrameSyncGuard()
    {
        if (_acquired)
            GazeRoiFrameSync::Cancel(_slot, _commandList);
    }

    bool Acquired() const { return _valid; }
    uint32_t Slot() const { return _slot; }
    void Commit() { _acquired = false; }

  private:
    ID3D12GraphicsCommandList* _commandList = nullptr;
    uint32_t _slot = 0;
    bool _acquired = false;
    bool _valid = false;
};

constexpr DXGI_FORMAT kDlssNrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
// The compatible rel_310.8 NR snippet rejects Witchfire's legacy NGX app ID
// (4919) at Init_Ext. RenoDX uses this ID successfully with the identical
// compatibility DLL on this same D3D12 title.
constexpr unsigned long long kDlssNrSnippetApplicationId = 0x0876232CULL;
constexpr NVSDK_NGX_Feature kDlssNrFeature = static_cast<NVSDK_NGX_Feature>(18);
constexpr int kDlssNrDefaultPreset = 1;
constexpr UINT kGuidanceDescriptorHeapCount = GAZE_ROI_FRAME_SLOTS;
constexpr UINT kGuidanceDescriptorsPerHeap = 123;
// Each recorded pass owns a descriptor table for the lifetime of the command list.
constexpr UINT kBoundaryPassCount = 2;
constexpr UINT kBoundaryLayerCount = 11;
constexpr DXGI_FORMAT kBoundaryFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr UINT kBoundarySeedDescriptorBase = 80; // two SRVs + one UAV
constexpr UINT kBoundaryDiffuseDescriptorBase = 83; // two tables, one SRV + one UAV each
constexpr UINT kBoundaryApplyDescriptorBase = 97; // three SRVs + one UAV
constexpr UINT kBoundaryFitDescriptorBase = 102; // two SRVs + one UAV
constexpr UINT kSourceTransferDescriptorBase = 110; // three SRVs + two UAVs
constexpr UINT kFusedDownsampleDescriptorBase = 115; // two SRVs + two UAVs
constexpr UINT kFusedGlobalCropDescriptorBase = 119; // two SRVs + two UAVs
static_assert(kBoundarySeedDescriptorBase + 3 == kBoundaryDiffuseDescriptorBase);
static_assert(kBoundaryDiffuseDescriptorBase + kBoundaryPassCount * 2 <= kBoundaryApplyDescriptorBase);
static_assert(kBoundaryApplyDescriptorBase + 4 <= kBoundaryFitDescriptorBase);
static_assert(kBoundaryFitDescriptorBase + 3 <= kSourceTransferDescriptorBase);
static_assert(kSourceTransferDescriptorBase + 5 == kFusedDownsampleDescriptorBase);
static_assert(kFusedDownsampleDescriptorBase + 4 == kFusedGlobalCropDescriptorBase);
static_assert(kFusedGlobalCropDescriptorBase + 4 == kGuidanceDescriptorsPerHeap);
constexpr UINT kOutputTemporalDescriptorBase = 66;
constexpr UINT kExtrapolationDescriptorBase = 76;
constexpr UINT kRestoreSourceDescriptorBase = 56;
// Keep the low-color dispatch away from the guide-resample descriptors. A
// command list reads shader-visible descriptors when it executes, so reusing
// slots 0..2 after recording Prepare() would make the earlier guide dispatch
// sample the later color bindings.
constexpr UINT kLowDownsampleDescriptorBase = 37;
constexpr UINT kLowResidualDescriptorBase = 4;
constexpr UINT kLowCompositeDescriptorBase = 52;
constexpr UINT kColorTransferDescriptorBase = 12;
constexpr UINT kInverseTransferDescriptorBase = 16;
constexpr UINT kFormatConversionDescriptorBase = 48;
constexpr UINT kRoiColorDescriptorBase = 40;
constexpr UINT kDebugDescriptorBase = 20;
// Debug staging and final writeback are recorded on the same command list.
// Keep writeback descriptors separate because shader-visible descriptors are
// resolved when the GPU executes, not when the dispatch is recorded.
constexpr UINT kDebugCompositeDescriptorBase = 44;
constexpr UINT kTemporalGuidanceDescriptorBase = 24;
constexpr UINT kTemporalResidualDescriptorBase = 28;
constexpr UINT kZeroMotionDescriptor = 35;
constexpr UINT kZeroDepthDescriptor = 36;
// Keep the fixed-grid A/B color preparation independent from every normal NR
// dispatch. Shader-visible descriptors are resolved only at GPU execution.
constexpr UINT kGlobalDownsampleDescriptorBase = 60;
constexpr UINT kGlobalCropDescriptorBase = 63;
constexpr UINT kGuidanceConstantCount = 16;
constexpr UINT kColorClampConstantCount = 28;

constexpr char kGuidanceResampleShader[] = R"(
cbuffer Params : register(b0)
{
    uint2 OutputSize;
    uint2 MotionBase;
    uint2 MotionSize;
    uint2 DepthBase;
    uint2 DepthSize;
    float2 MotionValueScale;
    float2 MotionOriginOffset;
    uint DepthInverted;
    uint Padding;
};

Texture2D<float4> MotionInput : register(t0);
Texture2D<float4> DepthInput : register(t1);
RWTexture2D<float2> MotionOutput : register(u0);
RWTexture2D<float> DepthOutput : register(u1);

float2 LoadMotion(int2 position)
{
    position = clamp(position, int2(0, 0), int2(MotionSize) - 1);
    return MotionInput.Load(int3(position + int2(MotionBase), 0)).xy;
}

float LoadDepth(int2 position)
{
    position = clamp(position, int2(0, 0), int2(DepthSize) - 1);
    return DepthInput.Load(int3(position + int2(DepthBase), 0)).x;
}

// Integrate texel coverage when shrinking; use a center-aligned tent when
// enlarging. The same footprint is used for depth ownership and motion weights.
float AxisWeight(int sample, float begin, float end)
{
    if (end - begin >= 1.0)
        return max(0.0, min(end, float(sample + 1)) - max(begin, float(sample)));
    return max(0.0, 1.0 - abs(float(sample) + 0.5 - (begin + end) * 0.5));
}

float SampleWeight(int2 sample, float2 begin, float2 end)
{
    return AxisWeight(sample.x, begin.x, end.x) * AxisWeight(sample.y, begin.y, end.y);
}

void Footprint(uint2 pixel, uint2 size, out float2 begin, out float2 end,
               out int2 first, out int2 last)
{
    begin = float2(pixel) * float2(size) / float2(OutputSize);
    end = float2(pixel + 1) * float2(size) / float2(OutputSize);
    float2 center = (begin + end) * 0.5;
    first = max(int2(floor(float2(end.x - begin.x >= 1.0 ? begin.x : center.x - 0.5,
                                 end.y - begin.y >= 1.0 ? begin.y : center.y - 0.5))), int2(0, 0));
    last = min(int2(ceil(float2(end.x - begin.x >= 1.0 ? end.x - 1.0 : center.x - 0.5,
                               end.y - begin.y >= 1.0 ? end.y - 1.0 : center.y - 0.5))), int2(size) - 1);
}

bool SameSurface(float a, float b)
{
    // Device-depth heuristic, not a metric distance: express both conventions
    // with near=1 so far-plane values do not admit a large unrelated depth range.
    float nearA = DepthInverted != 0 ? a : 1.0 - a;
    float nearB = DepthInverted != 0 ? b : 1.0 - b;
    return isfinite(a) && isfinite(b) &&
        abs(a - b) <= max(1e-6, 0.02 * max(abs(nearA), abs(nearB)));
}

void ResampleSurface(uint2 pixel, out float2 resultMotion, out float resultDepth)
{
    float2 depthBegin, depthEnd;
    int2 depthFirst, depthLast;
    Footprint(pixel, DepthSize, depthBegin, depthEnd, depthFirst, depthLast);
    float2 depthCenter = (depthBegin + depthEnd) * 0.5;
    int2 centerTexel = clamp(int2(floor(depthCenter)), depthFirst, depthLast);
    // The center texel has maximum separable coverage. Reject tiny fringe
    // overlaps before the nearest-depth comparison; otherwise an arbitrarily
    // small overlap can replace the entire output vector at fractional ratios.
    float minimumCoverage = 0.25 * SampleWeight(centerTexel, depthBegin, depthEnd);
    bool found = false;
    float selectedWeight = 0.0;
    float selectedDistance = 0.0;
    float2 anchorMotion = 0.0;
    resultDepth = DepthInverted != 0 ? 0.0 : 1.0;

    [loop] for (int y = depthFirst.y; y <= depthLast.y; ++y)
    [loop] for (int x = depthFirst.x; x <= depthLast.x; ++x)
    {
        int2 pos = int2(x, y);
        float weight = SampleWeight(pos, depthBegin, depthEnd);
        if (weight <= 0.0 || weight < minimumCoverage)
            continue;
        float depth = LoadDepth(pos);
        // Clamp the winning texel center to this output pixel's screen extent
        // before mapping between grids (depth can be coarser than motion).
        float2 uv = clamp((float2(pos) + 0.5) / float2(DepthSize),
                          (float2(pixel) + 1e-4) / float2(OutputSize),
                          (float2(pixel + 1) - 1e-4) / float2(OutputSize));
        float2 motion = LoadMotion(int2(floor(uv * float2(MotionSize)))) * MotionValueScale;
        if (!isfinite(depth) || !all(isfinite(motion)))
            continue;
        float2 offset = float2(pos) + 0.5 - depthCenter;
        float distance = dot(offset, offset);
        bool nearer = DepthInverted != 0 ? depth > resultDepth : depth < resultDepth;
        bool tie = depth == resultDepth && (weight > selectedWeight ||
                   (weight == selectedWeight && distance < selectedDistance));
        if (!found || nearer || tie)
        {
            found = true;
            resultDepth = depth;
            anchorMotion = motion;
            selectedWeight = weight;
            selectedDistance = distance;
        }
    }

    resultMotion = 0.0;
    if (!found)
    {
        // ROI translation is a coordinate transform, not scene motion. It
        // must survive even when malformed guide samples force the fallback.
        resultMotion = MotionOriginOffset;
        return;
    }

    float2 motionBegin, motionEnd;
    int2 motionFirst, motionLast;
    Footprint(pixel, MotionSize, motionBegin, motionEnd, motionFirst, motionLast);
    float2 weightedMotion = 0.0;
    float totalWeight = 0.0;
    [loop] for (int my = motionFirst.y; my <= motionLast.y; ++my)
    [loop] for (int mx = motionFirst.x; mx <= motionLast.x; ++mx)
    {
        int2 pos = int2(mx, my);
        float weight = SampleWeight(pos, motionBegin, motionEnd);
        if (weight <= 0.0)
            continue;
        float2 uv = (float2(pos) + 0.5) / float2(MotionSize);
        float depth = LoadDepth(int2(floor(uv * float2(DepthSize))));
        float2 motion = LoadMotion(pos) * MotionValueScale;
        if (!SameSurface(depth, resultDepth) || !all(isfinite(motion)))
            continue;
        // Average only a coherent motion cluster. The radius is measured in
        // NR pixels AFTER per-axis unit conversion, independent of input scale.
        float2 difference = motion - anchorMotion;
        weight *= saturate(1.0 - dot(difference, difference) / (0.5 * 0.5));
        weightedMotion += weight * motion;
        totalWeight += weight;
    }
    resultMotion = (totalWeight > 1e-6 ? weightedMotion / totalWeight : anchorMotion) +
                   MotionOriginOffset;
    // The production motion UAV is FP16; keep even malformed finite inputs
    // from overflowing during the store.
    resultMotion = clamp(resultMotion, -65504.0, 65504.0);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;
    if (any(pixel >= OutputSize))
        return;
    float2 motion;
    float depth;
    ResampleSurface(pixel, motion, depth);
    MotionOutput[pixel] = motion;
    DepthOutput[pixel] = depth;
}
)";

constexpr char kGuidanceDebugShader[] = R"(
cbuffer Params : register(b0)
{
    uint2 OutputSize;
    uint2 ColorBase;
    uint2 ColorSize;
    uint2 MotionBase;
    uint2 MotionSize;
    uint2 DepthBase;
    uint2 DepthSize;
    uint DepthInverted;
    uint Padding;
};

Texture2D<float4> ColorInput : register(t0);
Texture2D<float4> MotionInput : register(t1);
Texture2D<float4> DepthInput : register(t2);
RWTexture2D<float4> Output : register(u0);

float3 HsvToRgb(float3 value)
{
    float4 constants = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(value.xxx + constants.xyz) * 6.0 - constants.www);
    return value.z * lerp(constants.xxx, saturate(p - constants.xxx), value.y);
}

uint2 SamplePosition(uint2 localPixel, uint2 panelSize, uint2 sourceBase, uint2 sourceSize)
{
    return sourceBase + min(uint2((float2(localPixel) + 0.5) * float2(sourceSize) / float2(panelSize)),
                            sourceSize - 1);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint panelWidth = max(OutputSize.x / 3, 1u);
    uint panelHeight = max(OutputSize.y / 3, 1u);
    uint totalWidth = panelWidth * 3;
    if (dispatchThreadID.x >= totalWidth || dispatchThreadID.y >= panelHeight)
        return;

    uint panel = min(dispatchThreadID.x / panelWidth, 2u);
    uint2 localPixel = uint2(dispatchThreadID.x - panel * panelWidth, dispatchThreadID.y);
    uint2 panelSize = uint2(panelWidth, panelHeight);
    float3 color = 0.0;
    if (panel == 0)
    {
        uint2 source = SamplePosition(localPixel, panelSize, ColorBase, ColorSize);
        color = saturate(ColorInput.Load(int3(source, 0)).rgb);
    }
    else if (panel == 1)
    {
        uint2 source = SamplePosition(localPixel, panelSize, MotionBase, MotionSize);
        float2 motion = MotionInput.Load(int3(source, 0)).xy;
        float magnitude = length(motion);
        float hue = frac(atan2(-motion.y, motion.x) / 6.28318530718 + 1.0);
        color = HsvToRgb(float3(hue, saturate(magnitude * 0.08),
                                saturate(0.12 + log2(1.0 + magnitude) * 0.22)));
    }
    else
    {
        uint2 source = SamplePosition(localPixel, panelSize, DepthBase, DepthSize);
        float depth = saturate(DepthInput.Load(int3(source, 0)).x);
        if (DepthInverted == 0)
            depth = 1.0 - depth;
        color = depth.xxx;
    }

    uint2 destination = uint2(OutputSize.x - totalWidth + dispatchThreadID.x,
                              OutputSize.y - panelHeight + dispatchThreadID.y);
    Output[destination] = float4(color, 1.0);
}
)";

// A compact bank of source-to-final-model appearance maps. It is learned only
// after output stabilization, HDR restoration and any residual reconstruction.
// Unlike the legacy log-gain field, RGB covariance and an independent intercept
// can represent hue changes, exposure and new light on a black source.
// Historical affine/cache shader sources below are retained for the legacy offline
// fixtures only. Production bytecode comes exclusively from DLSSNRBoundary.hlsl.
constexpr char kExtrapolationSamplesShader[] = R"(
cbuffer Params : register(b0)
{
    uint2 OutputSize; uint2 DestinationBase;
    uint2 OriginalBase; uint2 RoiOffset;
    uint2 RoiSize; float RestorationWhite; float RestorationPreExposure;
    uint RestorationFlags; uint SampleFrame; uint SparseSampling; float RestorationExposureScale;
};
Texture2D<float4> ModelColor : register(t0);
Texture2D<float4> ExposureInput : register(t1);
Texture2D<float4> OriginalColor : register(t2);
RWTexture2D<float4> Appearance : register(u0);
groupshared float4 Statistics[26 * 64];
groupshared float3 DeltaMinimum[64];
groupshared float3 DeltaMaximum[64];

float EffectiveRestorationWhite()
{
    if ((RestorationFlags & 1u) == 0u) return 0.0;
    float white = clamp(RestorationWhite, 0.001, 65504.0);
    if ((RestorationFlags & 2u) != 0u)
    {
        float exposure = ExposureInput.Load(int3(0, 0, 0)).r;
        if (isfinite(exposure) && exposure > 1e-6 && exposure < 1e6 &&
            isfinite(RestorationPreExposure) && RestorationPreExposure > 1e-6 &&
            isfinite(RestorationExposureScale) && RestorationExposureScale > 1e-6)
            white = clamp(RestorationPreExposure * RestorationExposureScale / exposure, 0.01, 4096.0);
    }
    // Signed metadata distinguishes display-HDR edit transport from filmic.
    return (RestorationFlags & 4u) != 0u ? -white : white;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_GroupID, uint lane : SV_GroupIndex)
{
    if (id.x >= 34u || id.y >= 34u) return;
    uint2 address = uint2(id.x * 28u, id.y);
    uint2 basePitch = (RoiSize + 31u) / 32u;
    float2 pitch = float2(basePitch);
    float2 roiOrigin = float2(DestinationBase + RoiOffset);
    int2 gridOrigin = int2(floor(roiOrigin / pitch)) - 1;
    float2 center = (float2(gridOrigin + int2(id.xy)) + 0.5) * pitch;
    float mass = 0.0;
    float weightSquareSum = 0.0;
    float count = 0.0;
    float3 supportSum = 0.0, supportSquare = 0.0, supportOff = 0.0;
    float3 sourceSum = 0.0, deltaSum = 0.0, squareSum = 0.0, offSum = 0.0;
    float3 crossR = 0.0, crossG = 0.0, crossB = 0.0, deltaSquareSum = 0.0;
    float chromaticError = 0.0;
    float3 minimumDelta = 1e20, maximumDelta = -1e20;
    float2 positionSum = 0.0, positionSquare = 0.0;
    float positionOff = 0.0;
    float3 sourceX = 0.0, sourceY = 0.0, deltaX = 0.0, deltaY = 0.0;
    float3 curveSum = 0.0, curveSquare = 0.0, curveOff = 0.0;
    float3 sourceCurveR = 0.0, sourceCurveG = 0.0, sourceCurveB = 0.0;
    float3 deltaCurveR = 0.0, deltaCurveG = 0.0, deltaCurveB = 0.0;
    // Small ROIs and frames without appearance history remain dense. With strong
    // history, larger tiles sample one of each 2x2 footprint per frame. Both
    // spatial and temporal hashing avoid fixed checkerboard/texture aliases.
    bool sparse = SparseSampling != 0u && min(basePitch.x, basePitch.y) >= 16u;
    uint2 samplePitch = sparse ? (basePitch + 1u) / 2u : basePitch;
    [loop] for (uint sample = lane; sample < samplePitch.x * samplePitch.y; sample += 64u)
    {
        uint2 offset = uint2(sample % samplePitch.x, sample / samplePitch.x);
        float represented = 1.0;
        if (sparse)
        {
            offset *= 2u;
            uint2 extent = min(2u, basePitch - offset);
            int2 anchored = (gridOrigin + int2(id.xy)) * int2(basePitch) + int2(offset);
            uint hash = uint(anchored.x) * 1973u + uint(anchored.y) * 9277u + SampleFrame * 26699u + 911u;
            hash = (hash ^ (hash >> 16u)) * 2246822519u; hash ^= hash >> 13u;
            offset += uint2(hash & 1u, (hash >> 1u) & 1u) * (extent - 1u);
            represented = float(extent.x * extent.y);
        }
        int2 q = (gridOrigin + int2(id.xy)) * int2(basePitch) +
            int2(offset) - int2(DestinationBase + RoiOffset);
        if (any(q < 0) || any(q >= int2(RoiSize))) continue;
        float2 border = min(float2(q) + 0.5, float2(RoiSize) - float2(q) - 0.5);
        float2 observation = smoothstep(0.0, float2(basePitch), border);
        float weight = observation.x * observation.y * represented;
        if (weight <= 0.0) continue;
        float3 source = OriginalColor.Load(int3(OriginalBase + RoiOffset + q, 0)).rgb;
        float3 model = ModelColor.Load(int3(q, 0)).rgb;
        if (any((asuint(source) & 0x7f800000u) == 0x7f800000u) ||
            any((asuint(model) & 0x7f800000u) == 0x7f800000u)) continue;
        source = clamp(source, 0.0, 65504.0);
        model = clamp(model, 0.0, 65504.0);
        float sourcePeak = max(max(source.r, source.g), max(source.b, 0.01));
        float modelPeak = max(max(model.r, model.g), model.b);
        // An isolated, model-only HDR speck has no low-frequency evidence to
        // transport. Reject that sample, not all new light on a black source:
        // a bright region with bright neighbours remains fully represented.
        [branch] if (modelPeak > 64.0 * sourcePeak)
        {
            float neighbourPeak = 0.0;
            [unroll] for (uint n = 0u; n < 4u; ++n)
            {
                int2 step = n < 2u ? int2(n == 0u ? -1 : 1, 0) : int2(0, n == 2u ? -1 : 1);
                float3 neighbour = ModelColor.Load(int3(clamp(q + step, 0, int2(RoiSize) - 1), 0)).rgb;
                if (all((asuint(neighbour) & 0x7f800000u) != 0x7f800000u))
                    neighbourPeak = max(neighbourPeak, max(max(neighbour.r, neighbour.g), neighbour.b));
            }
            if (modelPeak > 8.0 * max(neighbourPeak, 0.01)) continue;
        }
        count += weight;
        // Colour coverage must not be diluted by HDR luminance variance. In
        // raw RGB, a dim yellow can lie near the mean of a bright green patch
        // even though that patch never observed yellow. Fit coverage in
        // chromaticity; correction magnitude is checked at the actual pixel.
        // Sum normalization puts every nonblack colour on one plane. Max
        // normalization bends that distribution around RGB cube corners and
        // can reject observed saturated colours in a broad colour population.
        float3 supportColor = source / max(dot(source, 1.0), 0.01);
        supportSum += supportColor * weight;
        supportSquare += supportColor * supportColor * weight;
        supportOff += supportColor.xxy * supportColor.yzz * weight;
        float deltaNorm = max(max(source.r, source.g), max(source.b, 0.01));
        float3 relativeDelta = (model - source) / deltaNorm;
        minimumDelta = min(minimumDelta, relativeDelta);
        maximumDelta = max(maximumDelta, relativeDelta);
        // Relative-error fitting prevents one HDR highlight from determining
        // the tone of every ordinary surface. Scale before forming moments.
        // Weight by the ORIGINAL, never by ordinary model brightness. Using
        // max(original,model) downweighted positive model fluctuations and
        // systematically darkened/colour-biased their supposedly zero-mean
        // average. Outliers were classified above without asymmetric weights.
        float scale = sourcePeak;
        float reciprocal = rcp(scale);
        float3 s = source * reciprocal;
        float3 d = (model - source) * reciprocal;
        float3 chromatic = d - s * (dot(d, s) / max(dot(s, s), 1e-12));
        chromaticError += dot(chromatic, chromatic) * weight;
        // A bounded nonlinear basis describes global tone-curve curvature
        // without the HDR overflow/unstable extrapolation of RGB polynomials.
        float3 curve = (source / (1.0 + source)) * reciprocal;
        curveSum += curve * reciprocal * weight;
        curveSquare += curve * curve * weight;
        curveOff += curve.xxy * curve.yzz * weight;
        sourceCurveR += s.r * curve * weight;
        sourceCurveG += s.g * curve * weight;
        sourceCurveB += s.b * curve * weight;
        deltaCurveR += d.r * curve * weight;
        deltaCurveG += d.g * curve * weight;
        deltaCurveB += d.b * curve * weight;
        float2 p = (float2(q) + roiOrigin + 0.5 - center) / pitch;
        float fitWeight = reciprocal * reciprocal * weight;
        weightSquareSum += fitWeight * fitWeight;
        positionSum += p * fitWeight;
        positionSquare += p * p * fitWeight;
        positionOff += p.x * p.y * fitWeight;
        sourceX += s * p.x * reciprocal * weight;
        sourceY += s * p.y * reciprocal * weight;
        deltaX += d * p.x * reciprocal * weight;
        deltaY += d * p.y * reciprocal * weight;
        mass += reciprocal * reciprocal * weight;
        sourceSum += s * reciprocal * weight;
        deltaSum += d * reciprocal * weight;
        squareSum += s * s * weight;
        offSum += s.xxy * s.yzz * weight;
        crossR += d.r * s * weight;
        crossG += d.g * s * weight;
        crossB += d.b * s * weight;
        deltaSquareSum += d * d * weight;
    }
    Statistics[lane] = float4(sourceSum, mass);
    Statistics[64 + lane] = float4(deltaSum, count);
    Statistics[128 + lane] = float4(squareSum, weightSquareSum);
    Statistics[192 + lane] = float4(offSum, 0.0);
    Statistics[256 + lane] = float4(crossR, 0.0);
    Statistics[320 + lane] = float4(crossG, 0.0);
    Statistics[384 + lane] = float4(crossB, 0.0);
    Statistics[448 + lane] = float4(deltaSquareSum, 0.0);
    Statistics[512 + lane] = float4(supportSum, chromaticError);
    Statistics[576 + lane] = float4(supportSquare, 0.0);
    Statistics[640 + lane] = float4(supportOff, 0.0);
    Statistics[704 + lane] = float4(positionSum, positionSquare);
    Statistics[768 + lane] = float4(positionOff, 0.0, 0.0, 0.0);
    Statistics[832 + lane] = float4(sourceX, 0.0);
    Statistics[896 + lane] = float4(sourceY, 0.0);
    Statistics[960 + lane] = float4(deltaX, 0.0);
    Statistics[1024 + lane] = float4(deltaY, 0.0);
    Statistics[1088 + lane] = float4(curveSum, 0.0);
    Statistics[1152 + lane] = float4(curveSquare, 0.0);
    Statistics[1216 + lane] = float4(curveOff, 0.0);
    Statistics[1280 + lane] = float4(sourceCurveR, 0.0);
    Statistics[1344 + lane] = float4(sourceCurveG, 0.0);
    Statistics[1408 + lane] = float4(sourceCurveB, 0.0);
    Statistics[1472 + lane] = float4(deltaCurveR, 0.0);
    Statistics[1536 + lane] = float4(deltaCurveG, 0.0);
    Statistics[1600 + lane] = float4(deltaCurveB, 0.0);
    DeltaMinimum[lane] = minimumDelta;
    DeltaMaximum[lane] = maximumDelta;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint offset = 32; offset != 0; offset >>= 1)
    {
        if (lane < offset)
        {
            [unroll] for (uint row = 0; row < 26; ++row)
                Statistics[row * 64 + lane] += Statistics[row * 64 + lane + offset];
            DeltaMinimum[lane] = min(DeltaMinimum[lane], DeltaMinimum[lane + offset]);
            DeltaMaximum[lane] = max(DeltaMaximum[lane], DeltaMaximum[lane + offset]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane != 0) return;

    [unroll] for (uint row = 0; row < 26u; ++row)
        Appearance[address + uint2(row, 0)] = Statistics[row * 64u];
    // Off-diagonal moments use RGB only. Keep the current restoration white
    // even in empty tiles, so no ROI sampling pattern can change the policy.
    Appearance[address + uint2(3, 0)] = float4(Statistics[192u].rgb, EffectiveRestorationWhite());
    bool valid = Statistics[0].w > 0.0;
    Appearance[address + uint2(26, 0)] = valid ? float4(DeltaMinimum[0], 0.0) : 0.0;
    Appearance[address + uint2(27, 0)] = valid ? float4(DeltaMaximum[0], 0.0) : 0.0;
}
)";

constexpr char kExtrapolationLearnShader[] = R"(
cbuffer Params : register(b0)
{
    uint2 OutputSize; uint2 DestinationBase;
    uint2 OriginalBase; uint2 RoiOffset;
    uint2 RoiSize; float AreaScale; uint EdgeBlendData;
    uint DisableLocal; uint2 Padding; uint GlobalColorMixing;
};
Texture2D<float4> ModelColor : register(t0);
Texture2D<float4> Samples : register(t1);
Texture2D<float4> OriginalColor : register(t2);
RWTexture2D<float4> Appearance : register(u0);
// One 32-thread group reduces each fit. Between-tile moments validate the
// LOW-frequency prediction, independently of unmodelled high-frequency detail.
groupshared float4 Statistics[34 * 32];
groupshared float3 DeltaMinimum[32];
groupshared float3 DeltaMaximum[32];

float3 CovMultiply(float3 diagonal, float3 off, float3 v)
{
    return diagonal * v + float3(off.x * v.y + off.y * v.z,
        off.x * v.x + off.z * v.z, off.y * v.x + off.z * v.y);
}
void InvertCovariance(float3 diagonal, float3 off, out float3 inverseD, out float3 inverseO)
{
    float3 cofactorD = diagonal.yzx * diagonal.zxy - off.zyx * off.zyx;
    float3 cofactorO = float3(off.y * off.z - off.x * diagonal.z,
        off.x * off.z - off.y * diagonal.y, off.x * off.y - off.z * diagonal.x);
    float determinant = dot(float3(diagonal.x, off.x, off.y),
                            float3(cofactorD.x, cofactorO.x, cofactorO.y));
    if (determinant > 1e-16 && all(isfinite(cofactorD)) && all(isfinite(cofactorO)))
    {
        inverseD = cofactorD / determinant;
        inverseO = cofactorO / determinant;
    }
    else
    {
        inverseD = rcp(max(diagonal, 1e-4));
        inverseO = 0.0;
    }
}

[numthreads(8, 4, 1)]
void CSMain(uint3 id : SV_GroupID, uint lane : SV_GroupIndex)
{
    if (id.x >= 34 || id.y >= 76) return;
    uint2 address = uint2(id.x * 13, id.y);
    // Initialize even unused global-row cells; no uninitialized bank texels.
    if (DisableLocal != 0u)
    {
        // The global-only dispatch has one group per row. All lanes clear
        // records, instead of launching 34 mostly idle groups per row.
        for (uint cellX = lane; cellX < 34u; cellX += 32u)
            [unroll] for (uint clearRecord = 0; clearRecord < 13u; ++clearRecord)
                Appearance[uint2(cellX * 13u + clearRecord, id.y)] = 0.0;
    }
    else if (lane == 0)
        [unroll] for (uint clearRecord = 0; clearRecord < 13; ++clearRecord)
            Appearance[address + uint2(clearRecord, 0)] = 0.0;
    bool global = id.y == 75;
    // Clear local records before returning so toggling back on cannot reuse
    // an older local history. Global learning/history stays uninterrupted.
    if (!global && DisableLocal != 0u) return;
    uint side = 32u, firstRow = 0u, selectedLevel = 0u;
    [unroll] for (uint level = 0; level < 5u; ++level)
    {
        if (!global && id.y >= firstRow + side + 2u)
        {
            firstRow += side + 2u;
            side >>= 1u;
            ++selectedLevel;
        }
    }
    if ((global && id.x != 0) || (!global && id.x >= side + 2u)) return;
    uint2 cell = uint2(id.x, id.y - firstRow);
    uint2 basePitch = (RoiSize + 31u) / 32u;
    float2 pitch = float2(basePitch << selectedLevel);
    float2 roiOrigin = float2(DestinationBase + RoiOffset);
    int2 gridOrigin = int2(floor(roiOrigin / pitch)) - 1;
    float2 center = (float2(gridOrigin + int2(cell)) + 0.5) * pitch;

    float mass = 0.0;
    float count = 0.0;
    float3 supportSum = 0.0, supportSquare = 0.0, supportOff = 0.0;
    float3 sourceSum = 0.0, deltaSum = 0.0, squareSum = 0.0, offSum = 0.0;
    float3 crossR = 0.0, crossG = 0.0, crossB = 0.0, deltaSquareSum = 0.0;
    float3 minimumDelta = 1e20, maximumDelta = -1e20;
    float2 positionSum = 0.0, positionSquare = 0.0;
    float positionOff = 0.0;
    float3 sourceX = 0.0, sourceY = 0.0, deltaX = 0.0, deltaY = 0.0;

    float4 moments[34];
    [unroll] for (uint initRow = 0u; initRow < 34u; ++initRow) moments[initRow] = 0.0;
    int2 fineOrigin = int2(floor(roiOrigin / float2(basePitch))) - 1;
    int2 first = global ? int2(0, 0) :
        clamp(int2(floor((center - pitch) / float2(basePitch))) - fineOrigin, 0, 33);
    int2 last = global ? int2(33, 33) :
        clamp(int2(ceil((center + pitch) / float2(basePitch))) - fineOrigin - 1, 0, 33);
    uint2 extent = uint2(max(last - first + 1, 0));
    float ratio = exp2(-float(selectedLevel));
    [loop] for (uint sample = lane; sample < extent.x * extent.y; sample += 32u)
    {
        int2 q = first + int2(sample % extent.x, sample / extent.x);
        float2 offset = ((float2(fineOrigin + q) + 0.5) * float2(basePitch) - center) / pitch;
        float2 kernel = saturate(1.0 - offset * offset);
        kernel *= kernel;
        float weight = global ? 1.0 : kernel.x * kernel.y;
        if (weight <= 0.0) continue;
        int2 address = int2(q.x * 28, q.y);
        float4 m[34];
        m[0] = Samples.Load(int3(address, 0));
        if (m[0].w <= 0.0 || !isfinite(m[0].w)) continue;
        [unroll] for (uint row = 1u; row < 17u; ++row)
            m[row] = Samples.Load(int3(address + int2(row, 0), 0));
        [unroll] for (uint extraRow = 17u; extraRow < 26u; ++extraRow)
            m[extraRow] = global ? Samples.Load(int3(address + int2(extraRow, 0), 0)) : 0.0;
        [unroll] for (uint betweenRow = 26u; betweenRow < 34u; ++betweenRow) m[betweenRow] = 0.0;
        if (global)
        {
            // Between-tile moments distinguish a real broad relighting from
            // zero-mean high-frequency model texture on any original.
            float invMass = rcp(m[0].w);
            // Effective weighted sample count estimates the noise floor of
            // each tile mean; different original brightness weights matter.
            m[2].w *= invMass;
            // Global fitting has no XY term; reuse those six rows.
            m[11] = float4(m[0].rgb * m[0].rgb * invMass, 0.0);
            m[12] = float4(m[0].xxy * m[0].yzz * invMass, 0.0);
            m[13] = float4(m[1].r * m[0].rgb * invMass, 0.0);
            m[14] = float4(m[1].g * m[0].rgb * invMass, 0.0);
            m[15] = float4(m[1].b * m[0].rgb * invMass, 0.0);
            m[16] = float4(m[1].rgb * m[1].rgb * invMass, 0.0);
            m[26] = float4(m[17].rgb * m[17].rgb * invMass, 0.0);
            m[27] = float4(m[17].xxy * m[17].yzz * invMass, 0.0);
            m[28] = float4(m[0].r * m[17].rgb * invMass, 0.0);
            m[29] = float4(m[0].g * m[17].rgb * invMass, 0.0);
            m[30] = float4(m[0].b * m[17].rgb * invMass, 0.0);
            m[31] = float4(m[1].r * m[17].rgb * invMass, 0.0);
            m[32] = float4(m[1].g * m[17].rgb * invMass, 0.0);
            m[33] = float4(m[1].b * m[17].rgb * invMass, 0.0);
        }
        else
        {
            // Translate/re-scale tile-centred spatial moments exactly.
            float4 p = m[11];
            m[11] = float4(p.xy * ratio + m[0].w * offset,
                p.zw * ratio * ratio + 2.0 * ratio * offset * p.xy + m[0].w * offset * offset);
            m[12].x = m[12].x * ratio * ratio + ratio * dot(offset.yx, p.xy) + m[0].w * offset.x * offset.y;
            m[13].rgb = m[13].rgb * ratio + m[0].rgb * offset.x;
            m[14].rgb = m[14].rgb * ratio + m[0].rgb * offset.y;
            m[15].rgb = m[15].rgb * ratio + m[1].rgb * offset.x;
            m[16].rgb = m[16].rgb * ratio + m[1].rgb * offset.y;
        }
        [unroll] for (uint sumRow = 0u; sumRow < 34u; ++sumRow) moments[sumRow] += m[sumRow] * weight;
        minimumDelta = min(minimumDelta, Samples.Load(int3(address + int2(26, 0), 0)).rgb);
        maximumDelta = max(maximumDelta, Samples.Load(int3(address + int2(27, 0), 0)).rgb);
    }
    [unroll] for (uint publishRow = 0u; publishRow < 34u; ++publishRow) Statistics[publishRow * 32u + lane] = moments[publishRow];
    DeltaMinimum[lane] = minimumDelta;
    DeltaMaximum[lane] = maximumDelta;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint offset = 16; offset != 0; offset >>= 1)
    {
        if (lane < offset)
        {
            [unroll] for (uint reduceRow = 0; reduceRow < 34; ++reduceRow)
                Statistics[reduceRow * 32 + lane] += Statistics[reduceRow * 32 + lane + offset];
            DeltaMinimum[lane] = min(DeltaMinimum[lane], DeltaMinimum[lane + offset]);
            DeltaMaximum[lane] = max(DeltaMaximum[lane], DeltaMaximum[lane + offset]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane != 0) return;
    sourceSum = Statistics[0].xyz; mass = Statistics[0].w;
    deltaSum = Statistics[32].xyz; count = Statistics[32].w;
    squareSum = Statistics[64].xyz; offSum = Statistics[96].xyz;
    crossR = Statistics[128].xyz; crossG = Statistics[160].xyz; crossB = Statistics[192].xyz;
    deltaSquareSum = Statistics[224].xyz;
    supportSum = Statistics[256].xyz; supportSquare = Statistics[288].xyz; supportOff = Statistics[320].xyz;
    if (mass <= 0.0 || !isfinite(mass)) return;
    float reciprocalMass = rcp(mass);
    float3 mean = sourceSum * reciprocalMass;
    float3 meanDelta = deltaSum * reciprocalMass;
    float normalization = max(max(mean.r, mean.g), max(mean.b, 0.01));
    float invNormalization = rcp(normalization);
    mean *= invNormalization;
    meanDelta *= invNormalization;
    float momentScale = reciprocalMass * invNormalization * invNormalization;
    float3 diagonal = max(squareSum * momentScale - mean * mean, 0.0);
    float3 off = offSum * momentScale - mean.xxy * mean.yzz;
    float3 cross0 = crossR * momentScale - meanDelta.r * mean;
    float3 cross1 = crossG * momentScale - meanDelta.g * mean;
    float3 cross2 = crossB * momentScale - meanDelta.b * mean;
    float3 deltaVariance = max(deltaSquareSum * momentScale - meanDelta * meanDelta, 0.0);
    float2 meanPosition = global ? 0.0 : Statistics[352].xy * reciprocalMass;
    float2 positionD = global ? 0.0 : max(Statistics[352].zw * reciprocalMass - meanPosition * meanPosition, 0.0);
    float positionO = global ? 0.0 : Statistics[384].x * reciprocalMass - meanPosition.x * meanPosition.y;
    float crossScale = reciprocalMass * invNormalization;
    sourceX = global ? 0.0 : Statistics[416].xyz * crossScale - mean * meanPosition.x;
    sourceY = global ? 0.0 : Statistics[448].xyz * crossScale - mean * meanPosition.y;
    deltaX = global ? 0.0 : Statistics[480].xyz * crossScale - meanDelta * meanPosition.x;
    deltaY = global ? 0.0 : Statistics[512].xyz * crossScale - meanDelta * meanPosition.y;

    float ridge = 1e-5 + 0.001 * dot(diagonal, 1.0);
    float3 inverseD, inverseO;
    // Each channel learns its own gain. Correlated RGB observations must not
    // produce cancelling cross-channel slopes that amplify unseen colours.
    // The relative prior preserves texture on nearly flat materials; the
    // independently fitted intercept can still learn additive model light.
    float3 prior = meanDelta * mean / (mean * mean + 0.0004);
    float3 crossSelf = float3(cross0.r, cross1.g, cross2.b);
    float3 gain = prior + (crossSelf - diagonal * prior) / (diagonal + ridge);
    float3 rgbChannelError = max(deltaVariance - 2.0 * gain * crossSelf + gain * gain * diagonal, 0.0);
    float rgbError = dot(rgbChannelError, 1.0);
    float3 a0 = float3(gain.r, 0.0, 0.0);
    float3 a1 = float3(0.0, gain.g, 0.0);
    float3 a2 = float3(0.0, 0.0, gain.b);
)"
R"(    float3 spatialX = 0.0, spatialY = 0.0;
    float3 q0 = 0.0, q1 = 0.0, q2 = 0.0, meanCurve = 0.0;
    float3 curveD = 0.0, curveO = 0.0;
    float3 sourceCurveR = 0.0, sourceCurveG = 0.0, sourceCurveB = 0.0;
    float3 deltaCurveR = 0.0, deltaCurveG = 0.0, deltaCurveB = 0.0;
    if (global)
    {
        meanCurve = Statistics[544].rgb * crossScale;
        curveD = max(Statistics[576].rgb * momentScale - meanCurve * meanCurve, 0.0);
        curveO = Statistics[608].rgb * momentScale - meanCurve.xxy * meanCurve.yzz;
        sourceCurveR = Statistics[640].rgb * momentScale - mean.r * meanCurve;
        sourceCurveG = Statistics[672].rgb * momentScale - mean.g * meanCurve;
        sourceCurveB = Statistics[704].rgb * momentScale - mean.b * meanCurve;
        deltaCurveR = Statistics[736].rgb * momentScale - meanDelta.r * meanCurve;
        deltaCurveG = Statistics[768].rgb * momentScale - meanDelta.g * meanCurve;
        deltaCurveB = Statistics[800].rgb * momentScale - meanDelta.b * meanCurve;
        // Joint own-channel RGB + c/(1+c) regression. A scalar Schur
        // complement retains nonlinear tone curves without RGB mixing.
        float3 sourceCurve = float3(sourceCurveR.r, sourceCurveG.g, sourceCurveB.b);
        float3 deltaCurve = float3(deltaCurveR.r, deltaCurveG.g, deltaCurveB.b);
        float3 projected = sourceCurve / (diagonal + ridge);
        float3 schur = max(curveD - sourceCurve * projected, 0.0) +
                       (1e-5 + 0.0001 * dot(curveD, 1.0));
        float3 curveGain = (deltaCurve - gain * sourceCurve) / schur;
        // A linear grade should stay linear. Require residual curvature in
        // that channel before accepting a second, nearly collinear feature.
        curveGain *= smoothstep(0.002, 0.02,
            rgbChannelError / max(deltaVariance + meanDelta * meanDelta, 1e-10));
        gain -= curveGain * projected;
        a0 = float3(gain.r, 0.0, 0.0);
        a1 = float3(0.0, gain.g, 0.0);
        a2 = float3(0.0, 0.0, gain.b);
        q0 = float3(curveGain.r, 0.0, 0.0);
        q1 = float3(0.0, curveGain.g, 0.0);
        q2 = float3(0.0, 0.0, curveGain.b);

        // Optional global colour grade: fit only the residual left by the
        // independent curves, with a strong ridge and a hard sensitivity cap.
        // Local maps always remain diagonal. Near-grey/one-colour observations
        // do not establish a cross-channel transform outside their ROI.
        if (GlobalColorMixing != 0u)
        {
            // Smallest eigenvalue in the orthonormal (R-G, R+G-2B) plane.
            float chromaX = 0.5 * (diagonal.r + diagonal.g - 2.0 * off.x);
            float chromaY = (diagonal.r + diagonal.g + 4.0 * diagonal.b +
                            2.0 * off.x - 4.0 * off.y - 4.0 * off.z) / 6.0;
            float chromaOff = (diagonal.r - diagonal.g - 2.0 * off.y + 2.0 * off.z) * 0.288675135;
            float chromaMinimum = max(0.0, 0.5 * (chromaX + chromaY -
                sqrt((chromaX - chromaY) * (chromaX - chromaY) + 4.0 * chromaOff * chromaOff)));
            float coverage = smoothstep(0.0025, 0.01, chromaMinimum) *
                smoothstep(0.005, 0.03, chromaMinimum / max(dot(diagonal, 1.0), 1e-8));
            if (coverage > 0.0)
            {
                float3 r0 = cross0 - CovMultiply(diagonal, off, a0) -
                    q0.r * float3(sourceCurveR.r, sourceCurveG.r, sourceCurveB.r);
                float3 r1 = cross1 - CovMultiply(diagonal, off, a1) -
                    q1.g * float3(sourceCurveR.g, sourceCurveG.g, sourceCurveB.g);
                float3 r2 = cross2 - CovMultiply(diagonal, off, a2) -
                    q2.b * float3(sourceCurveR.b, sourceCurveG.b, sourceCurveB.b);
                InvertCovariance(diagonal + (0.005 + 0.05 * dot(diagonal, 1.0)), off, inverseD, inverseO);
                float3 u0 = CovMultiply(inverseD, inverseO, r0);
                float3 u1 = CovMultiply(inverseD, inverseO, r1);
                float3 u2 = CovMultiply(inverseD, inverseO, r2);
                // Frobenius norm bounds the extra map's spectral norm: a
                // source RGB perturbation gains at most 0.35 times its size.
                float bound = min(1.0, 0.35 / max(sqrt(dot(u0, u0) + dot(u1, u1) + dot(u2, u2)), 1e-8));
                u0 *= bound; u1 *= bound; u2 *= bound;
                float improvement = 2.0 * (dot(u0, r0) + dot(u1, r1) + dot(u2, r2)) -
                    dot(u0, CovMultiply(diagonal, off, u0)) - dot(u1, CovMultiply(diagonal, off, u1)) -
                    dot(u2, CovMultiply(diagonal, off, u2));
                // Broad tile means must agree with the added grade. Pixel
                // error supplies its strength even when diverse fine texture
                // averages to almost the same colour in every tile.
                float3 bd = max(Statistics[352].rgb * momentScale - mean * mean, 0.0);
                float3 bo = Statistics[384].rgb * momentScale - mean.xxy * mean.yzz;
                float3 br = Statistics[416].rgb * momentScale - meanDelta.r * mean;
                float3 bg = Statistics[448].rgb * momentScale - meanDelta.g * mean;
                float3 bb = Statistics[480].rgb * momentScale - meanDelta.b * mean;
                float3 bsqr = Statistics[896].rgb * momentScale - mean.r * meanCurve;
                float3 bsqg = Statistics[928].rgb * momentScale - mean.g * meanCurve;
                float3 bsqb = Statistics[960].rgb * momentScale - mean.b * meanCurve;
                br -= CovMultiply(bd, bo, a0) + q0.r * float3(bsqr.r, bsqg.r, bsqb.r);
                bg -= CovMultiply(bd, bo, a1) + q1.g * float3(bsqr.g, bsqg.g, bsqb.g);
                bb -= CovMultiply(bd, bo, a2) + q2.b * float3(bsqr.b, bsqg.b, bsqb.b);
                float betweenImprovement = 2.0 * (dot(u0, br) + dot(u1, bg) + dot(u2, bb)) -
                    dot(u0, CovMultiply(bd, bo, u0)) - dot(u1, CovMultiply(bd, bo, u1)) -
                    dot(u2, CovMultiply(bd, bo, u2));
                float energy = dot(deltaVariance, 1.0) + dot(meanDelta, meanDelta);
                float strength = betweenImprovement > 0.0 ? coverage *
                    smoothstep(0.002, 0.02, improvement / max(energy, 1e-10)) : 0.0;
                a0 += u0 * strength; a1 += u1 * strength; a2 += u2 * strength;
            }
        }
    }
    else
    {
        // Joint own-channel RGB + XY fit. Three scalar-colour Schur
        // complements preserve local lighting gradients without letting
        // correlated RGB channels compensate for one another.
        float3 projectedX = sourceX / (diagonal + ridge);
        float3 projectedY = sourceY / (diagonal + ridge);
        float3 spatialDx = max(positionD.x - sourceX * projectedX, 0.0) + 0.001;
        float3 spatialDy = max(positionD.y - sourceY * projectedY, 0.0) + 0.001;
        float3 spatialOff = positionO - sourceX * projectedY;
        float3 spatialLimit = 0.99 * sqrt(spatialDx * spatialDy);
        spatialOff = clamp(spatialOff, -spatialLimit, spatialLimit);
        float3 residualX = deltaX - gain * sourceX;
        float3 residualY = deltaY - gain * sourceY;
        float3 determinant = max(spatialDx * spatialDy - spatialOff * spatialOff, 1e-8);
        spatialX = (residualX * spatialDy - residualY * spatialOff) / determinant;
        spatialY = (residualY * spatialDx - residualX * spatialOff) / determinant;
        gain -= spatialX * projectedX + spatialY * projectedY;
        a0 = float3(gain.r, 0.0, 0.0);
        a1 = float3(0.0, gain.g, 0.0);
        a2 = float3(0.0, 0.0, gain.b);
    }
    float3 intercept = meanDelta - float3(dot(a0, mean), dot(a1, mean), dot(a2, mean)) -
                       spatialX * meanPosition.x - spatialY * meanPosition.y -
                       float3(dot(q0, meanCurve), dot(q1, meanCurve), dot(q2, meanCurve));
    float error = dot(deltaVariance, 1.0) -
        2.0 * (dot(a0, cross0) + dot(a1, cross1) + dot(a2, cross2)) +
        dot(a0, CovMultiply(diagonal, off, a0)) + dot(a1, CovMultiply(diagonal, off, a1)) +
        dot(a2, CovMultiply(diagonal, off, a2)) - 2.0 * dot(spatialX, deltaX) - 2.0 * dot(spatialY, deltaY) +
        positionD.x * dot(spatialX, spatialX) + positionD.y * dot(spatialY, spatialY) +
        2.0 * positionO * dot(spatialX, spatialY) +
        2.0 * dot(spatialX, float3(dot(a0, sourceX), dot(a1, sourceX), dot(a2, sourceX))) +
        2.0 * dot(spatialY, float3(dot(a0, sourceY), dot(a1, sourceY), dot(a2, sourceY)));
    error += -2.0 * (dot(q0, deltaCurveR) + dot(q1, deltaCurveG) + dot(q2, deltaCurveB)) +
        dot(q0, CovMultiply(curveD, curveO, q0)) + dot(q1, CovMultiply(curveD, curveO, q1)) +
        dot(q2, CovMultiply(curveD, curveO, q2)) +
        2.0 * (dot(q0, a0.r * sourceCurveR + a0.g * sourceCurveG + a0.b * sourceCurveB) +
               dot(q1, a1.r * sourceCurveR + a1.g * sourceCurveG + a1.b * sourceCurveB) +
               dot(q2, a2.r * sourceCurveR + a2.g * sourceCurveG + a2.b * sourceCurveB));
    error = max(error, 0.0);
    float energy = dot(deltaVariance, 1.0) + dot(meanDelta, meanDelta);
    // A measured identity patch is positive evidence against a global change,
    // not missing data. It must be able to keep an unchanged object unchanged.
    // Confidence concerns the estimated mean/map, not whether one affine map
    // explains every high-frequency model pixel. Dividing by pixel variance
    // alone attenuated even a well-measured DC light under zero-mean detail.
    float signal = max(energy - error, 0.0);
    float confidence = energy < 1e-10 ? 1.0 :
        smoothstep(32.0, 128.0, signal * max(count, 1.0) / max(error, 1e-10));
    float expectedArea = global ? float(RoiSize.x) * float(RoiSize.y) : 4.0 * pitch.x * pitch.y;
    confidence *= smoothstep(0.002, 0.10, count / expectedArea);
    if (!all(isfinite(a0)) || !all(isfinite(a1)) ||
        !all(isfinite(a2)) || !all(isfinite(intercept)) ||
        !all(isfinite(spatialX)) || !all(isfinite(spatialY)) ||
        !all(isfinite(q0)) || !all(isfinite(q1)) || !all(isfinite(q2))) return;

    // Coverage counts pixels, not relative-error fitting weights. Otherwise
    // valid bright strands in dark texture look statistically "unseen".
    float3 supportMean = supportSum / count;
    float supportNorm = max(max(supportMean.r, supportMean.g), max(supportMean.b, 0.01));
    float3 supportD = max((supportSquare / count - supportMean * supportMean) / (supportNorm * supportNorm), 0.0);
    float3 supportO = (supportOff / count - supportMean.xxy * supportMean.yzz) / (supportNorm * supportNorm);
    InvertCovariance(supportD + 0.0016, supportO, inverseD, inverseO);
    Appearance[address] = float4(a0, intercept.r);
    Appearance[address + uint2(1, 0)] = float4(a1, intercept.g);
    Appearance[address + uint2(2, 0)] = float4(a2, intercept.b);
    Appearance[address + uint2(3, 0)] = float4(supportMean / supportNorm, normalization);
    Appearance[address + uint2(4, 0)] = float4(inverseD, confidence);
    Appearance[address + uint2(5, 0)] = float4(inverseO, supportNorm);
    float globalConsistency = 1.0 - smoothstep(0.002, 0.02, error / max(energy, 1e-10));
    if (global)
    {
        float3 bd = max(Statistics[352].rgb * momentScale - mean * mean, 0.0);
        float3 bo = Statistics[384].rgb * momentScale - mean.xxy * mean.yzz;
        float3 br = Statistics[416].rgb * momentScale - meanDelta.r * mean;
        float3 bg = Statistics[448].rgb * momentScale - meanDelta.g * mean;
        float3 bb = Statistics[480].rgb * momentScale - meanDelta.b * mean;
        float3 bv = max(Statistics[512].rgb * momentScale - meanDelta * meanDelta, 0.0);
        float3 bqd = max(Statistics[832].rgb * momentScale - meanCurve * meanCurve, 0.0);
        float3 bqo = Statistics[864].rgb * momentScale - meanCurve.xxy * meanCurve.yzz;
        float3 bsqr = Statistics[896].rgb * momentScale - mean.r * meanCurve;
        float3 bsqg = Statistics[928].rgb * momentScale - mean.g * meanCurve;
        float3 bsqb = Statistics[960].rgb * momentScale - mean.b * meanCurve;
        float3 bdqr = Statistics[992].rgb * momentScale - meanDelta.r * meanCurve;
        float3 bdqg = Statistics[1024].rgb * momentScale - meanDelta.g * meanCurve;
        float3 bdqb = Statistics[1056].rgb * momentScale - meanDelta.b * meanCurve;
        float betweenError = dot(bv, 1.0) - 2.0 * (dot(a0, br) + dot(a1, bg) + dot(a2, bb)) +
            dot(a0, CovMultiply(bd, bo, a0)) + dot(a1, CovMultiply(bd, bo, a1)) + dot(a2, CovMultiply(bd, bo, a2));
        betweenError += -2.0 * (dot(q0, bdqr) + dot(q1, bdqg) + dot(q2, bdqb)) +
            dot(q0, CovMultiply(bqd, bqo, q0)) + dot(q1, CovMultiply(bqd, bqo, q1)) +
            dot(q2, CovMultiply(bqd, bqo, q2)) +
            2.0 * (dot(q0, a0.r * bsqr + a0.g * bsqg + a0.b * bsqb) +
                   dot(q1, a1.r * bsqr + a1.g * bsqg + a1.b * bsqb) +
                   dot(q2, a2.r * bsqr + a2.g * bsqg + a2.b * bsqb));
        betweenError = max(betweenError, 0.0);
        float betweenEnergy = dot(bv, 1.0) + dot(meanDelta, meanDelta);
        // Test the predicted tile mean, including mean(curve(source)), not
        // curve(mean(source)). New fine detail must not switch a correct
        // broad tone off; genuine broad spatial disagreement still rejects it.
        float meanNoiseFloor = error * Statistics[64].w * reciprocalMass;
        float coherentError = max(betweenError - 2.0 * meanNoiseFloor, 0.0);
        globalConsistency = max(globalConsistency,
            1.0 - smoothstep(0.002, 0.02, coherentError / max(betweenEnergy, 1e-10)));
    }
    Appearance[address + uint2(6, 0)] = float4(DeltaMinimum[0], globalConsistency);
    Appearance[address + uint2(7, 0)] = float4(DeltaMaximum[0], 0.0);
    // A local affine approximation to restored HDR may fit dark observations
    // but extrapolate its slope badly into highlights of the same chromaticity.
    // Retain a soft radiance support limit only when that fit has model error;
    // an accurately explained gain/gradient needs no such restriction. This
    // uses the relative-error-weighted source distribution actually fitted.
    float radianceLimit = 0.0;
    if (!global && Samples.Load(int3(3, 0, 0)).w > 0.0)
    {
        float mismatch = smoothstep(0.002, 0.02, error / max(energy, 1e-10));
        if (mismatch > 0.0)
            radianceLimit = min(65504.0, normalization * (1.0 + 4.0 * sqrt(dot(diagonal, 1.0))) / mismatch);
    }
    // The projected-refinement coherence field is no longer needed locally.
    // Zero means unrestricted for analytical/test records and legacy banks.
    Appearance[address + uint2(8, 0)] = float4(spatialX, global ?
        saturate((rgbError - error) / max(rgbError, 1e-8)) : radianceLimit);
    Appearance[address + uint2(9, 0)] = float4(spatialY, rgbError * normalization * normalization);
    // Quantized neutral relighting is positive evidence that a regression
    // must not invent a hue change on correlated or unseen source colours.
    float neutral = 1.0 - smoothstep(1e-6, 1e-4,
        Statistics[256].w / max(dot(Statistics[224].rgb, 1.0), 1e-12));
    Appearance[address + uint2(10, 0)] = float4(q0, neutral);
    Appearance[address + uint2(11, 0)] = float4(q1, 0.0);
    Appearance[address + uint2(12, 0)] = float4(q2, global ? Samples.Load(int3(3, 0, 0)).w : 0.0);
}
)";

constexpr char kExtrapolationApplyShader[] = R"(
#ifndef DLSSNR_EXTERIOR_CACHE
#define DLSSNR_EXTERIOR_CACHE 1
#endif
#ifndef DLSSNR_EXTERIOR_LOCAL
#define DLSSNR_EXTERIOR_LOCAL 1
#endif
cbuffer Params : register(b0)
{
    uint2 OutputSize; uint2 DestinationBase;
    uint2 OriginalBase; uint2 RoiOffset;
    uint2 RoiSize; float AreaScale; uint EdgeBlendData;
    uint DisableLocal; uint2 Padding; uint GlobalColorMixing;
};
Texture2D<float4> ModelColor : register(t0);
Texture2D<float4> Appearance : register(t1);
Texture2D<float4> OriginalColor : register(t2);
RWTexture2D<float4> OutputColor : register(u0);
groupshared float4 LocalBank[3 * 16 * 11];
groupshared float4 GlobalBank[13];
groupshared int2 CacheOrigins[3];
groupshared uint CacheValid[3];
groupshared uint CacheFirstLevel;
groupshared uint NeedsLocal;

float3 BoundRestoredDelta(float3 source, float3 delta)
{
    float white = GlobalBank[12].w;
    if (white < 0.0)
    {
        // Same absolute display-light edit bound as RestoreSDR. Do not apply
        // the scene-filmic inverse bound to an already rendered HDR image.
        float peak = max(max(source.r, source.g), source.b) / -white;
        float tail = max(peak - 0.75, 0.0);
        float gain = (0.75 + 0.25 * tail / (0.25 + tail)) / max(peak, 1e-6);
        float confidence = peak <= 0.75 ? 1.0 : smoothstep(0.1, 0.5, gain);
        float bound = -white * confidence;
        float magnitude = max(max(abs(delta.r), abs(delta.g)), abs(delta.b));
        return delta * min(1.0, bound / max(magnitude, 1e-12));
    }
    if (white == 0.0) return delta;
    float x = min(max(max(source.r, source.g), source.b) / white, 8.0);
    float elasticity = (1.408 * x * x + 0.7028 * x + 0.0042) /
        ((2.51 * x + 0.03) * (x * (2.43 * x + 0.59) + 0.14));
    float reliability = smoothstep(0.05, 0.30, elasticity);
    // RestoreSDR's decoded endpoints are in [0, 7.25 * white], and its
    // confidence cannot exceed the original highlight's confidence. This
    // bounds any possible restored correction without applying the shoulder
    // weight twice. Fully protected highlights must stay original outside ROI.
    float maximumDelta = 7.25 * white * reliability;
    float peakDelta = max(max(abs(delta.r), abs(delta.g)), abs(delta.b));
    return delta * min(1.0, maximumDelta / max(peakDelta, 1e-12));
}

float3 Sanitize(float3 value)
{
    bool3 valid = (asuint(value) & 0x7f800000u) != 0x7f800000u;
    return float3(valid.r ? clamp(value.r, 0.0, 65504.0) : 0.0,
                  valid.g ? clamp(value.g, 0.0, 65504.0) : 0.0,
                  valid.b ? clamp(value.b, 0.0, 65504.0) : 0.0);
}
float RoundedDistance(float2 p, float2 lo, float2 hi, uint mask, float width)
{
    float4 d = float4(p - lo, hi - p);
    float distance = width;
    if (mask & 1u) distance = min(distance, d.x);
    if (mask & 2u) distance = min(distance, d.y);
    if (mask & 4u) distance = min(distance, d.z);
    if (mask & 8u) distance = min(distance, d.w);
    float radius = min(2.0 * width, 0.5 * min(hi.x - lo.x, hi.y - lo.y));
    if ((mask & 3u) == 3u) distance = min(distance, radius - length(max(radius - d.xy, 0.0)));
    if ((mask & 6u) == 6u) distance = min(distance, radius - length(max(radius - d.zy, 0.0)));
    if ((mask & 12u) == 12u) distance = min(distance, radius - length(max(radius - d.zw, 0.0)));
    if ((mask & 9u) == 9u) distance = min(distance, radius - length(max(radius - d.xw, 0.0)));
    return distance;
}
float RoundedWeight(float2 p, float2 lo, float2 hi, uint mask, float width)
{
    float t = saturate(RoundedDistance(p, lo, hi, mask, width) / max(width, 1e-4));
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}
float AppearanceDistance(float2 p, float2 lo, float2 hi, uint mask, float width, bool smoothLod)
{
    float outside = -RoundedDistance(p, lo, hi, mask, width);
    if (smoothLod)
    {
        // Smooth the fine-to-coarse transition across the entire feather,
        // including its inner half. A rectangular LOD=0 interior previously
        // retained local model appearance even where the model alpha was zero.
        float w = max(width, 1.0);
        float t = saturate(0.5 + 0.5 * outside / w);
        outside = outside >= w ? outside : w * t * t;
    }
    return max(outside, 0.0) / max(1.0, float(min(RoiSize.x, RoiSize.y)));
}
float2 BankPosition(float2 pixel, uint level)
{
    float2 pitch = float2(((RoiSize + 31u) / 32u) << level);
    float2 origin = floor(float2(DestinationBase + RoiOffset) / pitch) - 1.0;
    return (float2(DestinationBase) + pixel + 0.5) / pitch - origin - 0.5;
}
float4 ReadMap(int2 cell, uint row, uint level)
{
    int side = int(32u >> level) + 2;
    if (any(cell < 0) || any(cell >= side)) return 0.0;
    float4 coefficient = 0.0;
#if DLSSNR_EXTERIOR_CACHE
    uint slot = min(level - min(level, CacheFirstLevel), 2u);
    int2 local = cell - CacheOrigins[slot];
    bool cached = level >= CacheFirstLevel && level - CacheFirstLevel < 3u &&
                  CacheValid[slot] != 0u && all(local >= 0) && all(local < 4);
    local = clamp(local, 0, 3);
    [branch] if (cached)
        coefficient = LocalBank[((slot * 16u) + local.y * 4 + local.x) * 11 + row];
    else
#endif
    {
        uint firstRow = 64u - (64u >> level) + 2u * level;
        coefficient = Appearance.Load(int3(cell.x * 13 + row, cell.y + firstRow, 0));
    }
    return coefficient;
}
float4 Predict(float3 source, float4 a0, float4 a1, float4 a2,
               float4 mean, float4 inverseD, float4 inverseO,
               float3 minimumDelta, float3 maximumDelta,
               float3 spatialX, float3 spatialY, float2 position, bool global,
               float3 q0, float3 q1, float3 q2)
{
    if (mean.w <= 0.0 || inverseD.w <= 0.0) return 0.0;
    float peak = max(max(source.r, source.g), source.b);
    float supportNorm = max(inverseO.w, 0.01);
    // Coverage is a distribution of chromaticity, independent of scene
    // radiance. Bright and dim instances of the same colour share evidence;
    // unrelated dim colours cannot hide inside a patch's HDR variance.
    float3 v = source / (max(dot(source, 1.0), 0.01) * supportNorm) - mean.rgb;
    float mahalanobis = max(0.0, dot(inverseD.rgb, v * v) +
        2.0 * dot(inverseO.xyz, v.xxy * v.yzz));
    // A full-ROI distribution has real saturated tails even when its RGB
    // transform is exact. Give global coverage four-sigma headroom; local
    // material selection remains narrower and distance weighted.
    float support = 1.0 - smoothstep(global ? 16.0 : 9.0, global ? 36.0 : 25.0, mahalanobis);
    if (!global) support *= exp2(-0.25 * mahalanobis);
    float3 delta;
    if (global)
    {
        float3 s = source / mean.w;
        delta = float3(dot(a0.xyz, s) + a0.w, dot(a1.xyz, s) + a1.w, dot(a2.xyz, s) + a2.w);
        delta = (delta + spatialX * position.x + spatialY * position.y) * mean.w;
        float3 curve = source / (1.0 + source);
        delta += float3(dot(q0, curve), dot(q1, curve), dot(q2, curve));
    }
    else
    {
        // Slopes already operate in scene units. Cancel the per-tap colour
        // normalization analytically; only intercept/spatial terms need it.
        delta = float3(dot(a0.xyz, source), dot(a1.xyz, source), dot(a2.xyz, source)) +
                (float3(a0.w, a1.w, a2.w) + spatialX * position.x + spatialY * position.y) * mean.w;
    }
    if (!all(isfinite(delta))) return 0.0;
    // Local colour support remains per observation. Apply its RGB-vector
    // bound after interpolation, rather than up to nine times per query.
    if (!global) return float4(delta, support * inverseD.w);
    // Affine fitting is not permission to invent an unobserved channel change.
    // Bound the complete correction vector by observed relative deltas using
    // ONE scalar. Independent RGB clipping would itself rotate the colour.
    float3 relative = delta / max(peak, 0.01);
    float3 limit = float3(relative.r >= 0.0 ? max(maximumDelta.r, 0.0) : min(minimumDelta.r, 0.0),
                         relative.g >= 0.0 ? max(maximumDelta.g, 0.0) : min(minimumDelta.g, 0.0),
                         relative.b >= 0.0 ? max(maximumDelta.b, 0.0) : min(minimumDelta.b, 0.0));
    float3 ratio = abs(limit) / max(abs(relative), 1e-8);
    ratio = float3(abs(relative.r) < 1e-6 ? 1.0 : ratio.r,
                   abs(relative.g) < 1e-6 ? 1.0 : ratio.g,
                   abs(relative.b) < 1e-6 ? 1.0 : ratio.b);
    float boundScale = saturate(min(min(ratio.r, ratio.g), ratio.b));
    delta *= saturate(boundScale * 1.001 + 1e-5);
    return float4(clamp(source + delta, 0.0, 65504.0) - source, support * inverseD.w);
}

float4 LocalDelta(float3 source, float2 pixel, uint level, float3 globalDelta)
{
    float2 position = BankPosition(pixel, level);
    int side = int(32u >> level) + 2;
    // Destination queries keep nine-tap support, including the
    // out-of-bank tail; no boundary sample is projected into the exterior.
    int2 center = int2(floor(clamp(position, 0.0, float(side - 1)) + 0.5));
    float3 localSum = 0.0;
    float3 minimumSum = 0.0, maximumSum = 0.0;
    float weightSum = 0.0, spatialSum = 0.0;
    float coherenceSum = 0.0, coherenceWeight = 0.0;
    [loop] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 cell = center + int2(x, y);
        float2 distance = float2(cell) - position;
        float2 kernel = saturate(1.0 - distance * distance / 2.25);
        kernel *= kernel;
        float spatial = kernel.x * kernel.y;
        float4 spatialMap = ReadMap(cell, 8, level);
        float4 spatialMapY = ReadMap(cell, 9, level);
        float3 minimumDelta = ReadMap(cell, 6, level).rgb;
        float3 maximumDelta = ReadMap(cell, 7, level).rgb;
        float4 prediction = Predict(source, ReadMap(cell, 0, level),
            ReadMap(cell, 1, level), ReadMap(cell, 2, level),
            ReadMap(cell, 3, level), ReadMap(cell, 4, level),
            ReadMap(cell, 5, level), minimumDelta, maximumDelta,
            spatialMap.rgb,
            spatialMapY.rgb, position - float2(cell), false, 0.0, 0.0, 0.0);
        if (spatialMap.w > 0.0)
            prediction.w *= 1.0 - smoothstep(spatialMap.w, 2.0 * spatialMap.w, max(max(source.r, source.g), source.b));
        float neutral = ReadMap(cell, 10, level).w;
        prediction.rgb = lerp(prediction.rgb,
            source * (dot(prediction.rgb, source) / max(dot(source, source), 1e-12)), neutral);
        float weight = spatial * prediction.w;
        localSum += prediction.rgb * weight;
        minimumSum += minimumDelta * weight;
        maximumSum += maximumDelta * weight;
        weightSum += weight;
        coherenceSum += spatialMap.w * spatialMapY.w * weight;
        coherenceWeight += spatialMapY.w * weight;
        spatialSum += spatial;
    }
    float confidence = saturate(weightSum / max(0.08 * spatialSum, 1e-6));
    float3 delta = localSum / max(weightSum, 1e-6);
    // Average bounds with exactly the same colour/support weights as the
    // affine corrections. Keeping both sides unnormalized cancels a division.
    float peak = max(max(source.r, source.g), source.b);
    float3 relative = localSum / max(peak, 0.01);
    float3 limit = float3(relative.r >= 0.0 ? max(maximumSum.r, 0.0) : min(minimumSum.r, 0.0),
                         relative.g >= 0.0 ? max(maximumSum.g, 0.0) : min(minimumSum.g, 0.0),
                         relative.b >= 0.0 ? max(maximumSum.b, 0.0) : min(minimumSum.b, 0.0));
    float3 ratio = abs(limit) / max(abs(relative), 1e-8);
    // Fade a near-zero channel's veto continuously while retaining ONE RGB
    // scale. This preserves colour direction and the zero-crossing repair.
    float minorRange = max(1e-6 * weightSum, 0.2 * max(max(abs(relative.r), abs(relative.g)), abs(relative.b)));
    ratio = lerp(1.0, saturate(ratio), smoothstep(0.0, max(minorRange, 1e-8), abs(relative)));
    delta *= saturate(min(min(ratio.r, ratio.g), ratio.b));
    delta = clamp(source + delta, 0.0, 65504.0) - source;
    return float4(lerp(globalDelta, delta, confidence),
                  coherenceSum / max(coherenceWeight, 1e-8));
}
)"
R"(
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    uint2 pixel = id.xy;
    float2 roiMin = float2(RoiOffset);
    float2 roiMax = roiMin + float2(RoiSize) - 1.0;
    float width = float(EdgeBlendData & 0xffffu);
    uint mask = (EdgeBlendData >> 16u) & 15u;
    // Honour wide feather settings up to the distance to the ROI centre.
    // The former quarter-size cap made the upper half of the slider inert.
    float innerWidth = min(width, max(1.0, 0.5 * float(min(RoiSize.x, RoiSize.y) - 1u)));
    uint2 groupBase = group.xy * 8u;
    // Test the farthest group corner against the inset rounded rectangle.
    // Convexity guarantees every lane is in the exact model core. This also
    // works for wide feathers whose core is round instead of rectangular.
    float2 halfExtent = 0.5 * (roiMax - roiMin);
    float2 center = 0.5 * (roiMax + roiMin);
    float2 farthest = max(abs(float2(groupBase) - center), abs(float2(groupBase + 7u) - center));
    float radius = min(2.0 * innerWidth, min(halfExtent.x, halfExtent.y));
    float2 cornerDistance = max(farthest - (halfExtent - radius), 0.0);
    if (all(farthest <= halfExtent - innerWidth) &&
        dot(cornerDistance, cornerDistance) <= (radius - innerWidth) * (radius - innerWidth))
    {
        if (all(pixel < OutputSize))
            OutputColor[DestinationBase + pixel] = ModelColor.Load(int3(pixel - RoiOffset, 0));
        return;
    }
    if (index == 0u) NeedsLocal = 0u;
    if (index < 13u) GlobalBank[index] = Appearance.Load(int3(index, 75, 0));
    GroupMemoryBarrierWithGroupSync();

    bool inside = all(pixel >= RoiOffset) && all(pixel < RoiOffset + RoiSize);
    float innerW = inside ? RoundedWeight(pixel, roiMin, roiMax, mask, innerWidth) : 0.0;
    uint2 modelPixel = uint2(clamp(int2(pixel) - int2(RoiOffset), 0, int2(RoiSize) - 1));
    float4 model = ModelColor.Load(int3(modelPixel, 0));
    float2 extra = ceil(float2(RoiSize) * AreaScale) - float2(RoiSize);
    float2 outerMin = roiMin - floor(extra * 0.5);
    float2 outerMax = roiMax + extra - floor(extra * 0.5);
    float outerWidth = min(width, max(1.0, 0.5 * min(extra.x, extra.y)));
    float outerW = RoundedWeight(pixel, outerMin, outerMax, 15u, outerWidth);
    float4 original = OriginalColor.Load(int3(OriginalBase + pixel, 0));
    float3 source = Sanitize(original.rgb);
    float4 global = Predict(source, GlobalBank[0], GlobalBank[1], GlobalBank[2],
                            GlobalBank[3], GlobalBank[4], GlobalBank[5],
                            GlobalBank[6].rgb, GlobalBank[7].rgb,
                            GlobalBank[8].rgb, GlobalBank[9].rgb, 0.0, true,
                            GlobalBank[10].rgb, GlobalBank[11].rgb, GlobalBank[12].rgb);
    // An overall fit only overrides local evidence when it actually explains
    // the observations. A mixed-lighting ROI must not become a blanket grade.
    float globalDominance = global.w * GlobalBank[6].w;
    float3 globalDelta = global.rgb * globalDominance;
    globalDelta = lerp(globalDelta, source * (dot(globalDelta, source) / max(dot(source, source), 1e-12)),
                       saturate(GlobalBank[10].w));
    float3 delta = globalDelta;
#if DLSSNR_EXTERIOR_LOCAL
    float distance = 0.0, fraction = 0.0;
    uint level = 0u;
    bool localNeeded = DisableLocal == 0u && all(pixel < OutputSize) && innerW < 1.0 && outerW > 0.0 &&
                       globalDominance < 0.9999;
    [branch] if (localNeeded)
    {
        // The rounded local domain follows the same broad transition as the
        // visible model; scale mixing has no rectangular or diagonal switch.
        distance = AppearanceDistance(pixel, roiMin, roiMax, mask, innerWidth, false);
        float lodDistance = AppearanceDistance(pixel, roiMin, roiMax, mask, innerWidth, true);
        float lod = min(5.0, log2(1.0 + 64.0 * lodDistance));
        level = uint(floor(lod));
        fraction = frac(lod);
        fraction = fraction * fraction * (3.0 - 2.0 * fraction);
        localNeeded = distance < 1.0;
    }
    if (localNeeded) InterlockedOr(NeedsLocal, 1u);
    GroupMemoryBarrierWithGroupSync();
    [branch] if (NeedsLocal != 0u)
    {
#if DLSSNR_EXTERIOR_CACHE
        // Cache three adjacent destination LODs.
        // A group crossing more levels (tiny ROIs) falls back to exact loads.
        float2 groupCenter = float2(groupBase) + 3.5;
        // The rounded distance and its smoothed positive part are 1-Lipschitz.
        // This bound covers all lanes, even when the group straddles an arc.
        float nearest = max(0.0, AppearanceDistance(groupCenter, roiMin, roiMax, mask, innerWidth, true) -
                                  5.0 / max(1.0, float(min(RoiSize.x, RoiSize.y))));
        uint firstLevel = uint(min(5.0, floor(log2(1.0 + 64.0 * nearest))));
        if (index == 0u) CacheFirstLevel = firstLevel;
        [unroll] for (uint slot = 0u; slot < 3u; ++slot)
        {
            uint cacheLevel = min(firstLevel + slot, 5u);
            float2 firstPixel = float2(groupBase);
            float2 lastPixel = float2(groupBase + 7u);
            int side = int(32u >> cacheLevel) + 2;
            int2 first = int2(floor(clamp(BankPosition(firstPixel, cacheLevel), 0.0, float(side - 1)) + 0.5));
            int2 last = int2(floor(clamp(BankPosition(lastPixel, cacheLevel), 0.0, float(side - 1)) + 0.5));
            int2 origin = first - 1;
            bool valid = all(last - first <= 1);
            if (index == slot) { CacheOrigins[slot] = origin; CacheValid[slot] = valid ? 1u : 0u; }
            [branch] if (valid)
            {
                uint firstRow = 64u - (64u >> cacheLevel) + 2u * cacheLevel;
                [unroll] for (uint part = 0u; part < 3u; ++part)
                {
                    uint item = index + part * 64u;
                    if (item < 16u * 11u)
                    {
                        uint cell = item / 11u, row = item % 11u;
                        int2 q = origin + int2(cell % 4u, cell / 4u);
                        LocalBank[slot * 176u + item] = all(q >= 0) && all(q < side) ?
                            Appearance.Load(int3(q.x * 13 + row, q.y + firstRow, 0)) : 0.0;
                    }
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
#endif
    }
    if (any(pixel >= OutputSize)) return;
    if (innerW >= 1.0) { OutputColor[DestinationBase + pixel] = model; return; }
    // Beyond this fade the result is exactly global. Do not evaluate up to
    // 27 local predictions only to multiply their entire contribution by zero.
    [branch] if (localNeeded)
    {
        float4 local = LocalDelta(source, pixel, level, globalDelta);
        float4 coarser = local;
        [branch] if (fraction > 0.0)
        {
            coarser = LocalDelta(source, pixel, min(level + 1u, 5u), globalDelta);
            local = lerp(local, coarser, fraction);
        }
        delta = lerp(local.rgb, globalDelta, max(globalDominance, smoothstep(0.25, 1.0, distance)));
    }
#else
    if (any(pixel >= OutputSize)) return;
    if (innerW >= 1.0) { OutputColor[DestinationBase + pixel] = model; return; }
#endif
    // The global tone is a full-frame base, never switched on/off by gaze's
    // moving outer mask. Only the difference from that base is local.
    float3 exterior = clamp(source + BoundRestoredDelta(source,
        globalDelta + outerW * (delta - globalDelta)), 0.0, 65504.0);
    // Retain signed wide-gamut display colours outside the learned SDR gamut.
    if (GlobalBank[12].w < 0.0)
    {
        float low = min(min(original.r, original.g), original.b);
        float peak = max(max(original.r, original.g), max(original.b, 1e-6));
        float gamutConfidence = smoothstep(0.0, 0.02, low / peak);
        exterior = original.rgb + gamutConfidence * (exterior - source);
    }
    float3 modelRgb = GlobalBank[12].w < 0.0 ? model.rgb : Sanitize(model.rgb);
    float3 result = innerW > 0.0 ? lerp(exterior, modelRgb, innerW) : exterior;
    OutputColor[DestinationBase + pixel] = float4(result, lerp(original.a, model.a, innerW));
}
)";

// Stabilize the inferred appearance, never the original pixels. Local maps are
// transported by ORIGINAL scene motion in output coordinates (no gaze motion).
constexpr char kExtrapolationTemporalShader[] = R"(
cbuffer Params : register(b0)
{
    uint2 RoiOrigin; uint2 PreviousRoiOrigin;
    uint2 RoiSize; uint2 PreviousRoiSize;
    uint2 MotionBase; uint2 MotionSize;
    uint2 DepthBase; uint2 DepthSize;
    float2 MotionScale; float2 JitterCorrection;
    uint2 OriginalBase; uint2 OutputSize;
    float HistoryWeight; uint HistoryValid; uint DepthInverted; uint Padding;
};
Texture2D<float4> Current : register(t0);
Texture2D<float4> Previous : register(t1);
Texture2D<float4> Motion : register(t2);
Texture2D<float4> Depth : register(t3);
Texture2D<float4> Original : register(t4);
RWTexture2D<float4> Filtered : register(u0);
groupshared float SceneDifference[64];

float Peak(float3 v) { return max(max(v.r, v.g), v.b); }
float3 Signature(uint index)
{
    float2 cell = float2(index & 7u, index >> 3u);
    float3 sum = 0.0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        uint2 p = min(uint2((cell + 0.25 + 0.5 * float2(i & 1u, i >> 1u)) *
                           float2(OutputSize) / 8.0), OutputSize - 1u);
        float3 s = Original.Load(int3(OriginalBase + p, 0)).rgb;
        s = all(isfinite(s)) ? clamp(s, 0.0, 65504.0) : 0.0;
        sum += s / (0.25 + s);
    }
    return sum * 0.25;
}
void ToScene(inout float4 m[13], float2 offset, bool global)
{
    float normalization = m[3].w;
    m[8].rgb *= normalization;
    m[9].rgb *= normalization;
    float3 b = float3(m[0].w, m[1].w, m[2].w) * normalization +
               m[8].rgb * offset.x + m[9].rgb * offset.y;
    m[0].w = b.r; m[1].w = b.g; m[2].w = b.b;
    // The offline dense history keeps global coherence in energy units.
    // Local 8.w now carries radiance support; zero means unrestricted.
    if (global) m[8].w *= m[9].w;
    else if (m[8].w <= 0.0) m[8].w = 65504.0;
    if (global)
    {
        float dominance = saturate(m[4].w * m[6].w);
        m[0] *= dominance; m[1] *= dominance; m[2] *= dominance;
        m[6].rgb *= dominance; m[7].rgb *= dominance;
        m[10].rgb *= dominance; m[11].rgb *= dominance; m[12].rgb *= dominance;
        m[6].w = dominance;
    }
}
float3 MapDelta(float4 m[13], float3 source, bool global)
{
    float3 d = float3(dot(m[0].rgb, source) + m[0].w,
                      dot(m[1].rgb, source) + m[1].w,
                      dot(m[2].rgb, source) + m[2].w);
    if (global)
    {
        float3 q = source / (1.0 + source);
        d += float3(dot(m[10].rgb, q), dot(m[11].rgb, q), dot(m[12].rgb, q));
    }
    return d;
}
float ColourAgreement(float4 now[13], float4 old[13])
{
    float3 source = now[3].rgb * now[5].w;
    float3 v = source / max(old[5].w, 0.01) - old[3].rgb;
    float distance = max(0.0, dot(old[4].rgb, v * v) +
        2.0 * dot(old[5].rgb, v.xxy * v.yzz));
    return 1.0 - smoothstep(4.0, 16.0, distance);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    // A spatial 8x8 signature of the ORIGINAL full frame is independent of
    // gaze. Equal-average but differently arranged scenes also invalidate it.
    // Each group reduces the same tiny signature before any lane can return.
    float3 signature = Signature(lane);
    float3 oldSignature = Previous.Load(int3(13u + lane, 75, 0)).rgb;
    SceneDifference[lane] = dot(abs(signature - oldSignature), 1.0 / 3.0);
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (lane < stride) SceneDifference[lane] += SceneDifference[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (id.x >= 34u || id.y >= 76u) return;
    uint2 address = uint2(id.x * 13u, id.y);
    bool global = id.y == 75u && id.x == 0u;
    uint level = id.y < 34u ? 0u : id.y < 52u ? 1u : id.y < 62u ? 2u :
                 id.y < 68u ? 3u : id.y < 72u ? 4u : 5u;
    uint firstRow = 64u - (64u >> level) + 2u * level;
    uint side = (32u >> level) + 2u;
    bool local = id.y < 75u && id.x < side;
    if (!global && !local)
    {
        [unroll] for (uint r = 0; r < 13u; ++r)
        {
            uint sampleIndex = (id.x - 1u) * 13u + r;
            Filtered[address + uint2(r, 0)] = id.y == 75u && id.x >= 1u && sampleIndex < 64u ?
                float4(Signature(sampleIndex), 1.0) : 0.0;
        }
        return;
    }

    float4 now[13], history[13];
    bool finiteCurrent = true;
    [unroll] for (uint n = 0; n < 13u; ++n)
    {
        now[n] = Current.Load(int3(address + uint2(n, 0), 0));
        finiteCurrent = finiteCurrent && all(isfinite(now[n]));
        history[n] = 0.0;
    }
    // Invalid or missing current evidence must not resurrect a stale map.
    if (!finiteCurrent || now[3].w <= 0.0 || now[5].w <= 0.0 || (!global && now[4].w <= 0.0))
    {
        [unroll] for (uint clear = 0; clear < 13u; ++clear)
            Filtered[address + uint2(clear, 0)] = 0.0;
        return;
    }
    float2 pitch = float2(((RoiSize + 31u) / 32u) << level);
    float2 center = (floor(float2(RoiOrigin) / pitch) - 1.0 +
                     float2(id.x, id.y - firstRow) + 0.5) * pitch;
    float2 sceneMotion = 0.0, motionSquare = 0.0;
    float depthMean = 0.0, depthSquare = 0.0, validGuides = 0.0;
    if (!global)
    {
        [unroll] for (uint guide = 0; guide < 4u; ++guide)
        {
            float2 p = clamp(center + (float2(guide & 1u, guide >> 1u) - 0.5) * 0.5 * pitch,
                             float2(RoiOrigin) + 0.5, float2(RoiOrigin + RoiSize) - 0.5);
            float2 uv = saturate((p - float2(RoiOrigin)) / float2(RoiSize));
            int2 mp = int2(min(uint2(uv * MotionSize), MotionSize - 1u) + MotionBase);
            int2 dp = int2(min(uint2(uv * DepthSize), DepthSize - 1u) + DepthBase);
            float2 mv = Motion.Load(int3(mp, 0)).xy * MotionScale + JitterCorrection;
            float z = Depth.Load(int3(dp, 0)).r;
            bool valid = all(isfinite(mv)) && isfinite(z) && z >= 0.0 && z <= 1.0 &&
                         all(abs(mv) <= float2(OutputSize));
            if (valid)
            {
                z = DepthInverted != 0u ? z : 1.0 - z;
                sceneMotion += mv; motionSquare += mv * mv;
                depthMean += z; depthSquare += z * z; validGuides += 1.0;
            }
        }
        float reciprocal = rcp(max(validGuides, 1.0));
        sceneMotion *= reciprocal; motionSquare *= reciprocal;
        depthMean *= reciprocal; depthSquare *= reciprocal;
    }
    float depthDeviation = sqrt(max(0.0, depthSquare - depthMean * depthMean));
    float motionDispersion = length(sqrt(max(motionSquare - sceneMotion * sceneMotion, 0.0)) / pitch);
    float sceneTrust = 1.0 - smoothstep(0.08, 0.24, SceneDifference[0] / 64.0);
    float guideTrust = global ? 1.0 : (validGuides * 0.25) *
        (1.0 - smoothstep(0.10, 0.50, motionDispersion));
    float2 previousPosition = (center + sceneMotion) / pitch -
                             floor(float2(PreviousRoiOrigin) / pitch) + 0.5;
    float2 fraction = frac(previousPosition);
    int2 baseCell = int2(floor(previousPosition));
    float mass = 0.0;
    bool accept = HistoryValid != 0u && all(RoiSize == PreviousRoiSize) &&
                  HistoryWeight > 0.0 && sceneTrust > 0.0 && guideTrust > 0.0;
    if (accept)
    {
        [loop] for (uint tap = 0; tap < (global ? 1u : 4u); ++tap)
        {
            int2 cell = baseCell + int2(tap & 1u, tap >> 1u);
            float2 xy = float2(tap & 1u, tap >> 1u);
            float2 weights = lerp(1.0 - fraction, fraction, xy);
            float weight = global ? 1.0 : weights.x * weights.y;
            if (!global && (any(cell < 0) || any(cell >= int(side)))) continue;
            uint2 oldAddress = global ? uint2(0, 75) : uint2(cell.x * 13, cell.y + firstRow);
            float4 old[13];
            bool finiteMap = true;
            [unroll] for (uint read = 0; read < 13u; ++read)
            {
                old[read] = Previous.Load(int3(oldAddress + uint2(read, 0), 0));
                finiteMap = finiteMap && all(isfinite(old[read]));
            }
            if (!finiteMap || old[3].w <= 0.0 || old[5].w <= 0.0 || old[7].w <= 0.0) continue;
            if (!global)
            {
                float tolerance = 0.002 + 0.02 * max(depthMean, old[10].r) +
                                  2.0 * (depthDeviation + old[10].g);
                float depthTrust = (1.0 - smoothstep(1.0, 3.0, abs(depthMean - old[10].r) / tolerance)) *
                                   saturate(old[10].b);
                weight *= depthTrust * ColourAgreement(now, old);
            }
            ToScene(old, global ? 0.0 : previousPosition - float2(cell), global);
            [unroll] for (uint add = 0; add < 13u; ++add) history[add] += old[add] * weight;
            mass += weight;
        }
    }
    if (mass > 1e-6)
    {
        [unroll] for (uint divide = 0; divide < 13u; ++divide) history[divide] /= mass;
        float4 currentScene[13];
        [unroll] for (uint copy = 0; copy < 13u; ++copy) currentScene[copy] = now[copy];
        ToScene(currentScene, 0.0, global);
        // Coverage now stores chromaticity. Evaluate scene-unit corrections
        // at the fit's radiance scale when deciding whether history reacts.
        float3 sourceChroma = now[3].rgb * now[5].w;
        float3 sourceMean = sourceChroma * (now[3].w / max(Peak(sourceChroma), 0.01));
        float3 a = MapDelta(currentScene, sourceMean, global);
        float3 b = MapDelta(history, sourceMean, global);
        float change = Peak(abs(a - b)) / max(0.02 + Peak(sourceMean) + max(Peak(abs(a)), Peak(abs(b))), 0.02);
        float reactive = 1.0 - smoothstep(0.20, 0.75, change);
        float age = clamp(history[7].w, 1.0, 32.0);
        // Missing/rejected bilinear taps reduce history; normalizing them to
        // full strength would smear newly exposed regions across ROI edges.
        float h = min(saturate(HistoryWeight), age / (age + 1.0)) * saturate(mass) * sceneTrust * guideTrust * reactive;
        [unroll] for (uint mix = 0; mix < 13u; ++mix)
            currentScene[mix] = lerp(currentScene[mix], history[mix], h);
        float normalization = now[3].w;
        if (global)
        {
            float dominance = currentScene[6].w;
            float inv = dominance > 1e-6 ? rcp(dominance) : 0.0;
            currentScene[0] *= inv; currentScene[1] *= inv; currentScene[2] *= inv;
            currentScene[6].rgb *= inv; currentScene[7].rgb *= inv;
            currentScene[10].rgb *= inv; currentScene[11].rgb *= inv; currentScene[12].rgb *= inv;
            currentScene[6].w = dominance / max(currentScene[4].w, 1e-6);
        }
        [unroll] for (uint channel = 0; channel < 3u; ++channel)
        {
            now[channel] = currentScene[channel]; now[channel].w /= normalization;
        }
        now[4].w = currentScene[4].w;
        now[6] = currentScene[6]; now[7] = currentScene[7];
        now[8] = float4(currentScene[8].rgb / normalization, global ?
                        saturate(currentScene[8].w / max(currentScene[9].w, 1e-8)) : currentScene[8].w);
        now[9] = float4(currentScene[9].rgb / normalization, currentScene[9].w);
        if (global)
        {
            float restorationWhite = now[12].w;
            now[10] = currentScene[10]; now[11] = currentScene[11]; now[12] = currentScene[12];
            now[12].w = restorationWhite;
        }
        now[7].w = min(32.0, 1.0 + h * age);
    }
    else now[7].w = 1.0;
    if (!global) now[10] = float4(depthMean, depthDeviation, validGuides * 0.25, now[10].w);
    [unroll] for (uint write = 0; write < 13u; ++write)
        Filtered[address + uint2(write, 0)] = now[write];
}
)";

// Perceptual exterior cache: current targets are refreshed on rotating phases,
// while the displayed colour transforms accumulate every frame after scene reprojection.
constexpr char kExtrapolationCacheShader[] = R"(cbuffer Params : register(b0)
{
    uint2 OutputSize; uint2 DestinationBase;
    uint2 OriginalBase; uint2 RoiOffset;
    uint2 RoiSize; float AreaScale; uint EdgeBlendData;
    uint DisableLocal; uint3 Unused;
    uint2 MotionBase; uint2 MotionSize;
    uint2 DepthBase; uint2 DepthSize;
    float2 MotionScale; float2 JitterCorrection;
    float HistoryWeight; uint HistoryValid; uint DepthInverted; uint FrameIndex;
};
Texture2D<float4> Appearance : register(t0);
Texture2D<float4> Previous : register(t1);
Texture2D<float4> Original : register(t2);
Texture2D<float4> Motion : register(t3);
Texture2D<float4> Depth : register(t4);
RWTexture2D<float4> Output : register(u0);
SamplerState LinearClamp : register(s0);
static const uint Pitch = 16u;
static const uint Planes = 18u;

uint RecordPlane(uint row, bool target)
{
    return row < 4u ? row + (target ? 6u : 0u) : row + (target ? 10u : 6u);
}

float RoundedDistance(float2 p, float2 lo, float2 hi, uint mask, float width)
{
    float4 d = float4(p - lo, hi - p);
    float distance = width;
    if (mask & 1u) distance = min(distance, d.x);
    if (mask & 2u) distance = min(distance, d.y);
    if (mask & 4u) distance = min(distance, d.z);
    if (mask & 8u) distance = min(distance, d.w);
    float radius = min(2.0 * width, 0.5 * min(hi.x - lo.x, hi.y - lo.y));
    if ((mask & 3u) == 3u) distance = min(distance, radius - length(max(radius - d.xy, 0.0)));
    if ((mask & 6u) == 6u) distance = min(distance, radius - length(max(radius - d.zy, 0.0)));
    if ((mask & 12u) == 12u) distance = min(distance, radius - length(max(radius - d.zw, 0.0)));
    if ((mask & 9u) == 9u) distance = min(distance, radius - length(max(radius - d.xw, 0.0)));
    return distance;
}
float SmoothWeight(float d, float width)
{
    float t = saturate(d / max(width, 1e-4));
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}
float4 Old(float2 node, uint row, uint2 size)
{
    node = clamp(node, 0.0, float2(size - 1u));
    return Previous.SampleLevel(LinearClamp,
        (node + 0.5 + float2(0, row * size.y)) / float2(size.x, size.y * Planes), 0);
}
float3 Signature(float2 pixel)
{
    float3 sum = 0.0;
    uint textureWidth, textureHeight;
    Original.GetDimensions(textureWidth, textureHeight);
    [unroll] for (uint i = 0; i < 4u; ++i)
    {
        float2 p = clamp(pixel + (float2(i & 1u, i >> 1u) - 0.5) * float(Pitch),
                         0.0, float2(OutputSize - 1u));
        float3 s = Original.SampleLevel(LinearClamp,
            (float2(OriginalBase) + p + 0.5) / float2(textureWidth, textureHeight), 0).rgb;
        s = all(isfinite(s)) ? clamp(s, 0.0, 65504.0) : 0.0;
        // Retain dark-scene sensitivity and a logarithmic HDR tail. A purely
        // rational signature saturates and mistakes large bright-scene cuts
        // for unchanged input, retaining unrelated colour transforms.
        sum += s / (0.25 + s) + 0.125 * log2(1.0 + s);
    }
    return sum * 0.25;
}
float3 MatchedSignature(float2 previousNode, float2 motion, uint2 size)
{
    float2 base = floor(previousNode), fraction = frac(previousNode);
    float3 sum = 0.0;
    [unroll] for (uint i = 0; i < 4u; ++i)
    {
        float2 tap = float2(i & 1u, i >> 1u);
        float2 node = clamp(base + tap, 0.0, float2(size - 1u));
        float2 oldPixel = min(node * float(Pitch), float2(OutputSize - 1u));
        float2 weight = lerp(1.0 - fraction, fraction, tap);
        sum += Signature(oldPixel - motion) * weight.x * weight.y;
    }
    return sum;
}
void Query(float2 pixel, uint level, float3 source, out float4 m[8], out float neutral)
{
    float2 pitch = float2(((RoiSize + 31u) / 32u) << level);
    float2 position = (float2(DestinationBase) + pixel + 0.5) / pitch -
        floor(float2(DestinationBase + RoiOffset) / pitch) + 0.5;
    uint firstRow = 64u - (64u >> level) + 2u * level;
    int side = int(32u >> level) + 2;
    int2 center = int2(floor(clamp(position, 0.0, float(side - 1)) + 0.5));
    [unroll] for (uint init = 0; init < 8u; ++init) m[init] = 0.0;
    float mass = 0.0, spatialMass = 0.0, radianceLimitSum = 0.0;
    neutral = 0.0;
    [loop] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 cell = center + int2(x, y);
        if (any(cell < 0) || any(cell >= side)) continue;
        float2 offset = position - float2(cell);
        float2 kernel = saturate(1.0 - offset * offset / 2.25);
        kernel *= kernel;
        float spatial = kernel.x * kernel.y;
        spatialMass += spatial;
        int2 address = int2(cell.x * 13, cell.y + firstRow);
        float4 a[10];
        [unroll] for (uint read = 0; read < 10u; ++read)
            a[read] = Appearance.Load(int3(address + int2(read, 0), 0));
        if (a[3].w <= 0.0 || a[5].w <= 0.0 || a[4].w <= 0.0) continue;
        // Select observations for the destination colour before mixing their
        // affine maps. Testing only an averaged distribution can admit one
        // material while still applying another material's coefficients.
        float norm = max(a[5].w, 0.01);
        float3 v = (source / max(dot(source, 1.0), 0.01) - a[3].rgb * norm) / norm;
        float mahalanobis = max(0.0, dot(a[4].rgb, v * v) + 2.0 * dot(a[5].rgb, v.xxy * v.yzz));
        float support = (1.0 - smoothstep(9.0, 25.0, mahalanobis)) * exp2(-0.25 * mahalanobis);
        float radianceLimit = a[8].w > 0.0 ? a[8].w : 65504.0;
        support *= 1.0 - smoothstep(radianceLimit, 2.0 * radianceLimit, max(max(source.r, source.g), source.b));
        float weight = spatial * a[4].w * support;
        float3 intercept = (float3(a[0].w, a[1].w, a[2].w) +
                            a[8].rgb * offset.x + a[9].rgb * offset.y) * a[3].w;
        a[0].w = intercept.r; a[1].w = intercept.g; a[2].w = intercept.b;
        a[3].w = 1.0;
        [unroll] for (uint add = 0; add < 8u; ++add) m[add] += a[add] * weight;
        mass += weight;
        neutral += Appearance.Load(int3(address + int2(10, 0), 0)).w * weight;
        radianceLimitSum += radianceLimit * weight;
    }
    [unroll] for (uint divide = 0; divide < 8u; ++divide) m[divide] /= max(mass, 1e-8);
    m[4].w = saturate(mass / max(0.08 * spatialMass, 1e-8));
    m[3].w = mass > 1e-8 ? 1.0 : 0.0;
    m[7].w = radianceLimitSum / max(mass, 1e-8);
    neutral /= max(mass, 1e-8);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 size = (OutputSize + Pitch - 1u) / Pitch + 1u;
    if (any(id.xy >= size)) return;
    float2 pixel = min(float2(id.xy * Pitch), float2(OutputSize - 1u));
    float3 signature = 0.0;
    float2 motion = 0.0, motionSquare = 0.0;
    float depth = 0.0, depthSquare = 0.0, valid = 0.0;
    [unroll] for (uint i = 0; i < 4u; ++i)
    {
        float2 p = clamp(pixel + (float2(i & 1u, i >> 1u) - 0.5) * float(Pitch), 0.0, float2(OutputSize - 1u));
        uint2 cp = uint2(p);
        float3 source = Original.Load(int3(OriginalBase + cp, 0)).rgb;
        source = all(isfinite(source)) ? clamp(source, 0.0, 65504.0) : 0.0;
        signature += source / (0.25 + source) + 0.125 * log2(1.0 + source);
        float2 uv = (p + 0.5) / float2(OutputSize);
        float2 mv = Motion.Load(int3(MotionBase + min(uint2(uv * MotionSize), MotionSize - 1u), 0)).xy *
                    MotionScale + JitterCorrection;
        float z = Depth.Load(int3(DepthBase + min(uint2(uv * DepthSize), DepthSize - 1u), 0)).r;
        if (all(isfinite(mv)) && isfinite(z) && z >= 0.0 && z <= 1.0 && all(abs(mv) <= float2(OutputSize)))
        {
            z = DepthInverted != 0u ? z : 1.0 - z;
            motion += mv; motionSquare += mv * mv;
            depth += z; depthSquare += z * z; valid += 1.0;
        }
    }
    signature *= 0.25;
    motion /= max(valid, 1.0); motionSquare /= max(valid, 1.0);
    depth /= max(valid, 1.0); depthSquare /= max(valid, 1.0);
    float deviation = sqrt(max(depthSquare - depth * depth, 0.0));
    float2 previousPixel = pixel + motion;
    float2 previousNode = previousPixel / float(Pitch);
    float4 oldSignature = Old(previousNode, 4u, size);
    float4 oldDepth = Old(previousNode, 5u, size);
    // Compare exactly the old grid's reprojected sample footprints. Comparing
    // a current point signature to a bilinearly resampled old grid mistakes
    // sub-cell camera translation over texture for a change in the scene.
    float3 matched = HistoryValid != 0u ? MatchedSignature(previousNode, motion, size) : signature;
    float difference = max(max(abs(matched.r - oldSignature.r), abs(matched.g - oldSignature.g)),
                           abs(matched.b - oldSignature.b));
    float depthTolerance = 0.002 + 0.02 * max(depth, oldDepth.r) + 2.0 * (deviation + oldDepth.g);
    float trust = HistoryValid != 0u && all(previousPixel >= 0.0) && all(previousPixel <= float2(OutputSize - 1u)) &&
                  oldSignature.w > 0.0 ? 1.0 : 0.0;
    trust *= valid * 0.25 * (1.0 - smoothstep(0.015, 0.10, difference)) *
             (1.0 - smoothstep(1.0, 3.0, abs(depth - oldDepth.r) / depthTolerance)) * saturate(oldDepth.b) *
             (1.0 - smoothstep(0.10, 0.50, length(sqrt(max(motionSquare - motion * motion, 0.0))) / float(Pitch)));
    float4 m[8];
    bool active = DisableLocal == 0u && AreaScale > 1.0;
    bool evidence = Appearance.Load(int3(3, 75, 0)).w > 0.0;
    float4 oldTargetInfo = Old(previousNode, RecordPlane(7u, true), size);
    uint refreshHash = FrameIndex * 747796405u + id.x * 2891336453u + id.y * 277803737u;
    refreshHash = ((refreshHash >> ((refreshHash >> 28u) + 4u)) ^ refreshHash) * 277803737u;
    refreshHash ^= refreshHash >> 22u;
    bool reuse = trust > 0.999 && HistoryWeight > 0.0 &&
                 evidence && (active ? 1.0 : 0.0) == oldDepth.w &&
                 (refreshHash & 3u) != 0u && oldTargetInfo.w < 7.0;
    if (reuse)
    {
        [unroll] for (uint copy = 0; copy < 8u; ++copy)
            m[copy] = Old(previousNode, RecordPlane(copy, true), size);
    }
    else
    {
        float2 lo = float2(RoiOffset), hi = lo + float2(RoiSize) - 1.0;
        float width = min(float(EdgeBlendData & 0xffffu), max(1.0, 0.5 * float(min(RoiSize.x, RoiSize.y) - 1u)));
        float outside = -RoundedDistance(pixel, lo, hi, (EdgeBlendData >> 16u) & 15u, width);
        float t = saturate(0.5 + 0.5 * outside / max(width, 1.0));
        float lodDistance = (outside >= width ? outside : max(width, 1.0) * t * t) /
                            max(1.0, float(min(RoiSize.x, RoiSize.y)));
        float lod = min(5.0, log2(1.0 + 64.0 * lodDistance));
        uint level = uint(floor(lod));
        float fraction = smoothstep(0.0, 1.0, frac(lod));
        float globalDominance = saturate(Appearance.Load(int3(4, 75, 0)).w * Appearance.Load(int3(6, 75, 0)).w);
        float2 extra = ceil(float2(RoiSize) * AreaScale) - float2(RoiSize);
        float outerWidth = min(float(EdgeBlendData & 0xffffu), max(1.0, 0.5 * min(extra.x, extra.y)));
        float outer = SmoothWeight(RoundedDistance(pixel, lo - floor(extra * 0.5),
            hi + extra - floor(extra * 0.5), 15u, outerWidth), outerWidth);
        float fade = (1.0 - globalDominance) *
                     (1.0 - smoothstep(0.25, 1.0, max(outside, 0.0) / max(1.0, float(min(RoiSize.x, RoiSize.y)))));
        [unroll] for (uint init = 0; init < 8u; ++init) m[init] = 0.0;
        if (DisableLocal == 0u && AreaScale > 1.0 && fade > 0.0)
        {
            float3 querySource = Original.Load(int3(OriginalBase + uint2(pixel), 0)).rgb;
            querySource = all(isfinite(querySource)) ? clamp(querySource, 0.0, 65504.0) : 0.0;
            float neutral;
            Query(pixel, level, querySource, m, neutral);
            if (fraction > 0.0)
            {
                float coarseNeutral;
                float4 coarse[8]; Query(pixel, min(level + 1u, 5u), querySource, coarse, coarseNeutral);
                [unroll] for (uint mix = 0; mix < 8u; ++mix) m[mix] = lerp(m[mix], coarse[mix], fraction);
                neutral = lerp(neutral, coarseNeutral, fraction);
            }
            float3 source = querySource;
            float3 delta = float3(dot(m[0], float4(source, 1.0)), dot(m[1], float4(source, 1.0)),
                                  dot(m[2], float4(source, 1.0)));
            float3 relative = delta / max(max(max(source.r, source.g), source.b), 0.01);
            float3 limit = float3(relative.r >= 0.0 ? max(m[7].r, 0.0) : min(m[6].r, 0.0),
                                 relative.g >= 0.0 ? max(m[7].g, 0.0) : min(m[6].g, 0.0),
                                 relative.b >= 0.0 ? max(m[7].b, 0.0) : min(m[6].b, 0.0));
            float3 ratio = abs(limit) / max(abs(relative), 1e-8);
            float minor = max(1e-6, 0.2 * max(max(abs(relative.r), abs(relative.g)), abs(relative.b)));
            ratio = lerp(1.0, saturate(ratio), smoothstep(0.0, minor, abs(relative)));
            float scale = min(min(ratio.r, ratio.g), ratio.b);
            m[0] *= scale; m[1] *= scale; m[2] *= scale;
            m[3].w *= fade * m[4].w;
            // Confidence is now folded into the map weight. Its cache channel
            // retains radiance support for checking each actual output pixel.
            m[4].w = m[7].w;
            // Retain neutral evidence alongside the displayed/target
            // bounds for validation at the actual pixel.
            m[6].w = neutral;
        }
    }
    if (!active || !evidence)
        [unroll] for (uint clear = 0; clear < 8u; ++clear) m[clear] = 0.0;
    // Decorrelate refreshes from periodic model noise; a fixed four-frame
    // cadence aliases alternating corrections into a stationary grid bias.
    // The otherwise unused target channel caps stale targets at eight frames.
    // Keep all three support-mean channels. Reusing mean.g for target age
    // would corrupt the per-pixel support test after a history refresh.
    m[7].w = reuse ? oldTargetInfo.w + 1.0 : 0.0;
    // Targets refresh on decorrelated bounded schedules, but the displayed field
    // approaches that target EVERY frame. Holding the displayed field for
    // three frames and making a larger fourth-frame step would cause shimmer.
    [unroll] for (uint target = 0; target < 8u; ++target)
)"
R"(        Output[id.xy + uint2(0, RecordPlane(target, true) * size.y)] =
            all(isfinite(m[target])) ? clamp(m[target], -65504.0, 65504.0) : 0.0;
    float2 extent = max(abs(pixel - (float2(RoiOffset) + 0.5 * float2(RoiSize - 1u))) -
                        0.5 * float2(RoiSize - 1u), 0.0);
    float distance = length(extent) / max(1.0, float(min(RoiSize.x, RoiSize.y)));
    float near = 1.0 - smoothstep(0.05, 0.30, distance);
    // HistoryWeight is the far-field retention. Near the model use half its
    // time constant, so appearance can agree with the current core sooner.
    float h = lerp(HistoryWeight, HistoryWeight * HistoryWeight, near) * trust;
    if (h > 0.0 && active && evidence)
    {
        float4 previous[8];
        [unroll] for (uint read = 0; read < 8u; ++read)
            previous[read] = Old(previousNode, RecordPlane(read, false), size);
        float currentWeight = m[3].w * (1.0 - h), previousWeight = previous[3].w * h;
        float total = currentWeight + previousWeight;
        [unroll] for (uint mix = 0; mix < 8u; ++mix)
            m[mix] = (m[mix] * currentWeight + previous[mix] * previousWeight) / max(total, 1e-8);
        m[3].w = total;
    }
    [unroll] for (uint row = 0; row < 8u; ++row)
        Output[id.xy + uint2(0, RecordPlane(row, false) * size.y)] =
            all(isfinite(m[row])) ? clamp(m[row], -65504.0, 65504.0) : 0.0;
    Output[id.xy + uint2(0, 4u * size.y)] = float4(signature, 1.0);
    Output[id.xy + uint2(0, 5u * size.y)] = float4(depth, deviation, valid * 0.25, active ? 1.0 : 0.0);
}
)";

constexpr char kExtrapolationCachedApplyShader[] = R"(#ifndef DLSSNR_EXTERIOR_INPLACE
#define DLSSNR_EXTERIOR_INPLACE 0
#endif
cbuffer Params : register(b0)
{
    uint2 OutputSize; uint2 DestinationBase;
    uint2 OriginalBase; uint2 RoiOffset;
    uint2 RoiSize; float AreaScale; uint EdgeBlendData;
    uint DisableLocal; uint2 Padding; uint GlobalColorMixing;
};
Texture2D<float4> ModelColor : register(t0);
Texture2D<float4> Appearance : register(t1);
Texture2D<float4> OriginalColor : register(t2);
RWTexture2D<float4> OutputColor : register(u0);
Texture2D<float4> Cache : register(t3);
SamplerState LinearClamp : register(s0);
groupshared float4 GlobalBank[13];
groupshared float4 CacheBank[4 * 8];

float3 BoundRestoredDelta(float3 source, float3 delta)
{
    float white = GlobalBank[12].w;
    if (white < 0.0)
    {
        // Same absolute display-light edit bound as RestoreSDR. Do not apply
        // the scene-filmic inverse bound to an already rendered HDR image.
        float peak = max(max(source.r, source.g), source.b) / -white;
        float tail = max(peak - 0.75, 0.0);
        float gain = (0.75 + 0.25 * tail / (0.25 + tail)) / max(peak, 1e-6);
        float confidence = peak <= 0.75 ? 1.0 : smoothstep(0.1, 0.5, gain);
        float bound = -white * confidence;
        float magnitude = max(max(abs(delta.r), abs(delta.g)), abs(delta.b));
        return delta * min(1.0, bound / max(magnitude, 1e-12));
    }
    if (white == 0.0) return delta;
    float x = min(max(max(source.r, source.g), source.b) / white, 8.0);
    float elasticity = (1.408 * x * x + 0.7028 * x + 0.0042) /
        ((2.51 * x + 0.03) * (x * (2.43 * x + 0.59) + 0.14));
    float reliability = smoothstep(0.05, 0.30, elasticity);
    // RestoreSDR's decoded endpoints are in [0, 7.25 * white], and its
    // confidence cannot exceed the original highlight's confidence. This
    // bounds any possible restored correction without applying the shoulder
    // weight twice. Fully protected highlights must stay original outside ROI.
    float maximumDelta = 7.25 * white * reliability;
    float peakDelta = max(max(abs(delta.r), abs(delta.g)), abs(delta.b));
    return delta * min(1.0, maximumDelta / max(peakDelta, 1e-12));
}

float3 Sanitize(float3 value)
{
    bool3 valid = (asuint(value) & 0x7f800000u) != 0x7f800000u;
    return float3(valid.r ? clamp(value.r, 0.0, 65504.0) : 0.0,
                  valid.g ? clamp(value.g, 0.0, 65504.0) : 0.0,
                  valid.b ? clamp(value.b, 0.0, 65504.0) : 0.0);
}
float RoundedDistance(float2 p, float2 lo, float2 hi, uint mask, float width)
{
    float4 d = float4(p - lo, hi - p);
    float distance = width;
    if (mask & 1u) distance = min(distance, d.x);
    if (mask & 2u) distance = min(distance, d.y);
    if (mask & 4u) distance = min(distance, d.z);
    if (mask & 8u) distance = min(distance, d.w);
    float radius = min(2.0 * width, 0.5 * min(hi.x - lo.x, hi.y - lo.y));
    if ((mask & 3u) == 3u) distance = min(distance, radius - length(max(radius - d.xy, 0.0)));
    if ((mask & 6u) == 6u) distance = min(distance, radius - length(max(radius - d.zy, 0.0)));
    if ((mask & 12u) == 12u) distance = min(distance, radius - length(max(radius - d.zw, 0.0)));
    if ((mask & 9u) == 9u) distance = min(distance, radius - length(max(radius - d.xw, 0.0)));
    return distance;
}
float RoundedWeight(float2 p, float2 lo, float2 hi, uint mask, float width)
{
    float t = saturate(RoundedDistance(p, lo, hi, mask, width) / max(width, 1e-4));
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}
float4 Predict(float3 source, float4 a0, float4 a1, float4 a2,
               float4 mean, float4 inverseD, float4 inverseO,
               float3 minimumDelta, float3 maximumDelta,
               float3 spatialX, float3 spatialY, float2 position, bool global,
               float3 q0, float3 q1, float3 q2)
{
    if (mean.w <= 0.0 || inverseD.w <= 0.0) return 0.0;
    float peak = max(max(source.r, source.g), source.b);
    float supportNorm = max(inverseO.w, 0.01);
    // Coverage is a distribution of chromaticity, independent of scene
    // radiance. Bright and dim instances of the same colour share evidence;
    // unrelated dim colours cannot hide inside a patch's HDR variance.
    float3 v = source / (max(dot(source, 1.0), 0.01) * supportNorm) - mean.rgb;
    float mahalanobis = max(0.0, dot(inverseD.rgb, v * v) +
        2.0 * dot(inverseO.xyz, v.xxy * v.yzz));
    // Match the reference/global-only path's full-ROI coverage envelope.
    float support = 1.0 - smoothstep(global ? 16.0 : 9.0, global ? 36.0 : 25.0, mahalanobis);
    if (!global) support *= exp2(-0.25 * mahalanobis);
    float3 delta;
    if (global)
    {
        float3 s = source / mean.w;
        delta = float3(dot(a0.xyz, s) + a0.w, dot(a1.xyz, s) + a1.w, dot(a2.xyz, s) + a2.w);
        delta = (delta + spatialX * position.x + spatialY * position.y) * mean.w;
        float3 curve = source / (1.0 + source);
        delta += float3(dot(q0, curve), dot(q1, curve), dot(q2, curve));
    }
    else
    {
        // Slopes already operate in scene units. Cancel the per-tap colour
        // normalization analytically; only intercept/spatial terms need it.
        delta = float3(dot(a0.xyz, source), dot(a1.xyz, source), dot(a2.xyz, source)) +
                (float3(a0.w, a1.w, a2.w) + spatialX * position.x + spatialY * position.y) * mean.w;
    }
    if (!all(isfinite(delta))) return 0.0;
    // Local colour support remains per observation. Apply its RGB-vector
    // bound after interpolation, rather than up to nine times per query.
    if (!global) return float4(delta, support * inverseD.w);
    // Affine fitting is not permission to invent an unobserved channel change.
    // Bound the complete correction vector by observed relative deltas using
    // ONE scalar. Independent RGB clipping would itself rotate the colour.
    float3 relative = delta / max(peak, 0.01);
    float3 limit = float3(relative.r >= 0.0 ? max(maximumDelta.r, 0.0) : min(minimumDelta.r, 0.0),
                         relative.g >= 0.0 ? max(maximumDelta.g, 0.0) : min(minimumDelta.g, 0.0),
                         relative.b >= 0.0 ? max(maximumDelta.b, 0.0) : min(minimumDelta.b, 0.0));
    float3 ratio = abs(limit) / max(abs(relative), 1e-8);
    ratio = float3(abs(relative.r) < 1e-6 ? 1.0 : ratio.r,
                   abs(relative.g) < 1e-6 ? 1.0 : ratio.g,
                   abs(relative.b) < 1e-6 ? 1.0 : ratio.b);
    float boundScale = saturate(min(min(ratio.r, ratio.g), ratio.b));
    delta *= saturate(boundScale * 1.001 + 1e-5);
    return float4(clamp(source + delta, 0.0, 65504.0) - source, support * inverseD.w);
}



float3 CachedDelta(float3 source, float2 pixel, float3 globalDelta)
{
    float2 fraction = frac(pixel / 16.0);
    float peak = max(max(source.r, source.g), source.b);
    float3 chromaticity = source / max(dot(source, 1.0), 0.01);
    float3 sum = 0.0;
    float mass = 0.0;
    [unroll] for (uint tap = 0u; tap < 4u; ++tap)
    {
        uint base = tap * 8u;
        float4 mean = CacheBank[base + 3u];
        // Record weights are shared by the whole group, so this branch is
        // uniform and avoids evaluating empty/rejected cache neighbours.
        [branch] if (mean.w <= 0.0) continue;
        float4 inverseD = CacheBank[base + 4u], inverseO = CacheBank[base + 5u];
        float3 v = chromaticity - mean.rgb;
        float mahalanobis = max(0.0, dot(inverseD.rgb, v * v) +
            2.0 * dot(inverseO.rgb, v.xxy * v.yzz));
        float support = (1.0 - smoothstep(9.0, 25.0, mahalanobis)) * exp2(-0.25 * mahalanobis);
        if (inverseD.w > 0.0)
            support *= 1.0 - smoothstep(inverseD.w, 2.0 * inverseD.w, peak);
        float2 weight = lerp(1.0 - fraction, fraction, float2(tap & 1u, tap >> 1u));
        float confidence = weight.x * weight.y * saturate(support / 0.08) * saturate(mean.w);
        float4 a0 = CacheBank[base], a1 = CacheBank[base + 1u], a2 = CacheBank[base + 2u];
        float3 delta = float3(dot(a0, float4(source, 1.0)), dot(a1, float4(source, 1.0)), dot(a2, float4(source, 1.0)));
        float neutral = CacheBank[base + 6u].w;
        [branch] if (neutral > 0.0)
            delta = lerp(delta, source * (dot(delta, source) / max(dot(source, source), 1e-12)), saturate(neutral));
        float3 minimumDelta = CacheBank[base + 6u].rgb, maximumDelta = CacheBank[base + 7u].rgb;
        float3 relative = delta / max(peak, 0.01);
        float3 limit = float3(relative.r >= 0.0 ? max(maximumDelta.r, 0.0) : min(minimumDelta.r, 0.0),
                             relative.g >= 0.0 ? max(maximumDelta.g, 0.0) : min(minimumDelta.g, 0.0),
                             relative.b >= 0.0 ? max(maximumDelta.b, 0.0) : min(minimumDelta.b, 0.0));
        float3 ratio = abs(limit) / max(abs(relative), 1e-8);
        float minor = max(1e-6, 0.2 * max(max(abs(relative.r), abs(relative.g)), abs(relative.b)));
        ratio = lerp(1.0, saturate(ratio), smoothstep(0.0, minor, abs(relative)));
        delta *= saturate(min(min(ratio.r, ratio.g), ratio.b));
        delta = clamp(source + delta, 0.0, 65504.0) - source;
        sum += delta * confidence;
        mass += confidence;
    }
    // Validate each record before interpolation. An invalid/unsupported
    // neighbour contributes the global result, never its affine coefficients.
    return globalDelta * saturate(1.0 - mass) + sum;
}
[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    uint2 pixel = id.xy;
    float2 roiMin = float2(RoiOffset);
    float2 roiMax = roiMin + float2(RoiSize) - 1.0;
    float width = float(EdgeBlendData & 0xffffu);
    uint mask = (EdgeBlendData >> 16u) & 15u;
    // Honour wide feather settings up to the distance to the ROI centre.
    // The former quarter-size cap made the upper half of the slider inert.
    float innerWidth = min(width, max(1.0, 0.5 * float(min(RoiSize.x, RoiSize.y) - 1u)));
    uint2 groupBase = group.xy * 16u;
    // Test the farthest group corner against the inset rounded rectangle.
    // Convexity guarantees every lane is in the exact model core. This also
    // works for wide feathers whose core is round instead of rectangular.
    float2 halfExtent = 0.5 * (roiMax - roiMin);
    float2 center = 0.5 * (roiMax + roiMin);
    float2 farthest = max(abs(float2(groupBase) - center), abs(float2(groupBase + 15u) - center));
    float radius = min(2.0 * innerWidth, min(halfExtent.x, halfExtent.y));
    float2 cornerDistance = max(farthest - (halfExtent - radius), 0.0);
    if (all(farthest <= halfExtent - innerWidth) &&
        dot(cornerDistance, cornerDistance) <= (radius - innerWidth) * (radius - innerWidth))
    {
        if (all(pixel < OutputSize))
            OutputColor[DestinationBase + pixel] = ModelColor.Load(int3(pixel - RoiOffset, 0));
        return;
    }
    if (index < 13u) GlobalBank[index] = Appearance.Load(int3(index, 75, 0));
    // Each 16x16 group shares the same four cache nodes. Loading their records
    // once keeps per-pixel validation independent of texture bandwidth.
    if (index < 32u)
    {
        uint record = index & 7u, tap = index >> 3u;
        uint2 size = (OutputSize + 15u) / 16u + 1u;
        uint2 node = min(group.xy + uint2(tap & 1u, tap >> 1u), size - 1u);
        uint plane = record < 4u ? record : record + 6u;
        float4 value = Cache.Load(int3(node + uint2(0, plane * size.y), 0));
        if (record >= 3u && record <= 5u)
        {
            // Convert support from its stored normalized units once per
            // group. Every pixel can then test chromaticity directly, with
            // the same precision and mean, without four repeated decodes.
            float norm = max(Cache.Load(int3(node + uint2(0, 11u * size.y), 0)).w, 0.01);
            value.rgb *= record == 3u ? norm : rcp(norm * norm);
        }
        CacheBank[index] = value;
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(pixel >= OutputSize)) return;
    bool inside = all(pixel >= RoiOffset) && all(pixel < RoiOffset + RoiSize);
    float innerW = inside ? RoundedWeight(pixel, roiMin, roiMax, mask, innerWidth) : 0.0;
    uint2 modelPixel = uint2(clamp(int2(pixel) - int2(RoiOffset), 0, int2(RoiSize) - 1));
    float4 model = ModelColor.Load(int3(modelPixel, 0));
    // All neighbourhood queries have finished before this pass. Each lane
    // reads and writes only its own pixel, so typed UAV loads avoid a full
    // original-image copy without cross-pixel read/write dependencies.
#if DLSSNR_EXTERIOR_INPLACE
    float4 original = OutputColor[DestinationBase + pixel];
#else
    float4 original = OriginalColor.Load(int3(OriginalBase + pixel, 0));
#endif
    float3 source = Sanitize(original.rgb);
    float4 global = 0.0;
    [branch] if (GlobalBank[4].w * GlobalBank[6].w > 1e-6) global = Predict(source, GlobalBank[0], GlobalBank[1], GlobalBank[2],
                            GlobalBank[3], GlobalBank[4], GlobalBank[5],
                            GlobalBank[6].rgb, GlobalBank[7].rgb,
                            GlobalBank[8].rgb, GlobalBank[9].rgb, 0.0, true,
                            GlobalBank[10].rgb, GlobalBank[11].rgb, GlobalBank[12].rgb);
    // An overall fit only overrides local evidence when it actually explains
    // the observations. A mixed-lighting ROI must not become a blanket grade.
    float globalDominance = global.w * GlobalBank[6].w;
    float3 globalDelta = global.rgb * globalDominance;
    [branch] if (GlobalBank[10].w > 0.0)
        globalDelta = lerp(globalDelta, source * (dot(globalDelta, source) / max(dot(source, source), 1e-12)),
                           saturate(GlobalBank[10].w));
    if (any(pixel >= OutputSize)) return;
    if (innerW >= 1.0) { OutputColor[DestinationBase + pixel] = model; return; }
    float3 delta = globalDelta;
    [branch] if (DisableLocal == 0u && AreaScale > 1.0 && globalDominance < 0.9999)
    {
        float2 extra = ceil(float2(RoiSize) * AreaScale) - float2(RoiSize);
        float outerWidth = min(width, max(1.0, 0.5 * min(extra.x, extra.y)));
        float outerW = RoundedWeight(pixel, roiMin - floor(extra * 0.5),
                                     roiMax + extra - floor(extra * 0.5), 15u, outerWidth);
        if (outerW > 0.0) delta = lerp(globalDelta, CachedDelta(source, pixel, globalDelta), outerW);
    }
    float3 exterior = clamp(source + BoundRestoredDelta(source, delta), 0.0, 65504.0);
    // Retain signed wide-gamut display colours outside the learned SDR gamut.
    if (GlobalBank[12].w < 0.0)
    {
        float low = min(min(original.r, original.g), original.b);
        float peak = max(max(original.r, original.g), max(original.b, 1e-6));
        float gamutConfidence = smoothstep(0.0, 0.02, low / peak);
        exterior = original.rgb + gamutConfidence * (exterior - source);
    }
    float3 modelRgb = GlobalBank[12].w < 0.0 ? model.rgb : Sanitize(model.rgb);
    float3 result = innerW > 0.0 ? lerp(exterior, modelRgb, innerW) : exterior;
    OutputColor[DestinationBase + pixel] = float4(result, lerp(original.a, model.a, innerW));
}
)";

constexpr char kExtrapolationGlobalTemporalShader[] = R"(cbuffer Params : register(b0)
{
    uint2 OutputSize; uint2 DestinationBase;
    uint2 OriginalBase; uint2 RoiOffset;
    uint2 RoiSize; float AreaScale; uint EdgeBlendData;
    uint DisableLocal; uint3 Unused;
    uint2 MotionBase; uint2 MotionSize;
    uint2 DepthBase; uint2 DepthSize;
    float2 MotionScale; float2 JitterCorrection;
    float HistoryWeight; uint HistoryValid; uint DepthInverted; uint FrameIndex;
};
Texture2D<float4> Current : register(t0);
Texture2D<float4> Previous : register(t1);
Texture2D<float4> Motion : register(t2);
Texture2D<float4> Depth : register(t3);
Texture2D<float4> Original : register(t4);
Texture2D<float4> PreviousCache : register(t5);
RWTexture2D<float4> Filtered : register(u0);
SamplerState LinearClamp : register(s0);
groupshared float2 Scene[64];

float Peak(float3 x) { return max(max(x.r, x.g), x.b); }
float3 Signature(float2 pixel)
{
    float3 sum = 0.0;
    uint textureWidth, textureHeight;
    Original.GetDimensions(textureWidth, textureHeight);
    [unroll] for (uint i = 0; i < 4u; ++i)
    {
        float2 p = clamp(pixel + (float2(i & 1u, i >> 1u) - 0.5) * 16.0,
                         0.0, float2(OutputSize - 1u));
        float3 s = Original.SampleLevel(LinearClamp,
            (float2(OriginalBase) + p + 0.5) / float2(textureWidth, textureHeight), 0).rgb;
        s = all(isfinite(s)) ? clamp(s, 0.0, 65504.0) : 0.0;
        sum += s / (0.25 + s) + 0.125 * log2(1.0 + s);
    }
    return sum * 0.25;
}
float4 OldGuide(float2 node, uint plane)
{
    uint2 size = (OutputSize + 15u) / 16u + 1u;
    node = clamp(node, 0.0, float2(size - 1u));
    return PreviousCache.SampleLevel(LinearClamp,
        (node + 0.5 + float2(0, plane * size.y)) / float2(size.x, size.y * 18u), 0);
}
float2 SceneSample(uint lane)
{
    if (HistoryValid == 0u) return 0.0;
    float2 pixel = floor((float2(lane & 7u, lane >> 3u) + 0.5) * float2(OutputSize) / 128.0 + 0.5) * 16.0;
    pixel = min(pixel, float2(OutputSize - 1u));
    float2 motion = 0.0, motionSquare = 0.0;
    float depth = 0.0, depthSquare = 0.0, valid = 0.0;
    [unroll] for (uint i = 0; i < 4u; ++i)
    {
        float2 p = clamp(pixel + (float2(i & 1u, i >> 1u) - 0.5) * 16.0, 0.0, float2(OutputSize - 1u));
        float2 uv = (p + 0.5) / float2(OutputSize);
        float2 mv = Motion.Load(int3(MotionBase + min(uint2(uv * MotionSize), MotionSize - 1u), 0)).xy * MotionScale + JitterCorrection;
        float z = Depth.Load(int3(DepthBase + min(uint2(uv * DepthSize), DepthSize - 1u), 0)).r;
        if (all(isfinite(mv)) && isfinite(z) && z >= 0.0 && z <= 1.0 && all(abs(mv) <= float2(OutputSize)))
        {
            z = DepthInverted != 0u ? z : 1.0 - z;
            motion += mv; motionSquare += mv * mv; depth += z; depthSquare += z * z; valid += 1.0;
        }
    }
    motion /= max(valid, 1.0); motionSquare /= max(valid, 1.0);
    depth /= max(valid, 1.0); depthSquare /= max(valid, 1.0);
    float2 oldPixel = pixel + motion, node = oldPixel / 16.0;
    if (any(oldPixel < 0.0) || any(oldPixel > float2(OutputSize - 1u))) return 0.0;
    float4 old = OldGuide(node, 4u), oldDepth = OldGuide(node, 5u);
    float2 base = floor(node), fraction = frac(node);
    float3 matched = 0.0;
    uint2 size = (OutputSize + 15u) / 16u + 1u;
    [unroll] for (uint tap = 0; tap < 4u; ++tap)
    {
        float2 xy = float2(tap & 1u, tap >> 1u);
        float2 q = min(clamp(base + xy, 0.0, float2(size - 1u)) * 16.0, float2(OutputSize - 1u));
        float2 weight = lerp(1.0 - fraction, fraction, xy);
        matched += Signature(q - motion) * weight.x * weight.y;
    }
    float tolerance = 0.002 + 0.02 * max(depth, oldDepth.r) +
                      2.0 * (sqrt(max(depthSquare - depth * depth, 0.0)) + oldDepth.g);
    float coverage = valid * 0.25 * saturate(old.a) * saturate(oldDepth.b) *
        (1.0 - smoothstep(1.0, 3.0, abs(depth - oldDepth.r) / tolerance)) *
        (1.0 - smoothstep(0.10, 0.50, length(sqrt(max(motionSquare - motion * motion, 0.0))) / 16.0));
    return float2(Peak(abs(matched - old.rgb)) * coverage, coverage);
}
void Inverse(float3 d, float3 o, out float3 id, out float3 io)
{
    float3 cd = d.yzx * d.zxy - o.zyx * o.zyx;
    float3 co = float3(o.y * o.z - o.x * d.z, o.x * o.z - o.y * d.y, o.x * o.y - o.z * d.x);
    float determinant = dot(float3(d.x, o.x, o.y), float3(cd.x, co.x, co.y));
    if (determinant > 1e-16 && all(isfinite(cd)) && all(isfinite(co)))
    { id = cd / determinant; io = co / determinant; }
    else { id = rcp(max(d, 1e-4)); io = 0.0; }
}
void ToScene(inout float4 m[13])
{
    m[0].w *= m[3].w; m[1].w *= m[3].w; m[2].w *= m[3].w;
    float dominance = saturate(m[4].w * m[6].w);
    m[0] *= dominance; m[1] *= dominance; m[2] *= dominance;
    m[6].rgb *= dominance; m[7].rgb *= dominance;
    m[10].rgb *= dominance; m[11].rgb *= dominance; m[12].rgb *= dominance;
    m[6].w = dominance;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    // Only the group containing the global record needs the scene reduction.
    if (group.x == 0u && group.y == 9u)
    {
        Scene[lane] = SceneSample(lane);
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint step = 32u; step > 0u; step >>= 1u)
        {
            if (lane < step) Scene[lane] += Scene[lane + step];
            GroupMemoryBarrierWithGroupSync();
        }
    }
    if (id.x >= 34u || id.y >= 76u) return;
    uint2 address = uint2(id.x * 13u, id.y);
    float4 now[13];
    [unroll] for (uint currentRecord = 0; currentRecord < 13u; ++currentRecord)
        now[currentRecord] = Current.Load(int3(address + uint2(currentRecord, 0), 0));
    if (id.x == 0u && id.y == 75u && now[3].w > 0.0 && now[5].w > 0.0)
    {
        float4 old[13];
        bool finiteMap = true;
        [unroll] for (uint historyRecord = 0; historyRecord < 13u; ++historyRecord)
        {
            old[historyRecord] = Previous.Load(int3(address + uint2(historyRecord, 0), 0));
            finiteMap = finiteMap && all(isfinite(old[historyRecord])) && all(isfinite(now[historyRecord]));
        }
        float sceneTrust = (1.0 - smoothstep(0.02, 0.10, Scene[0].x / max(Scene[0].y, 1e-8))) *
                           smoothstep(4.0, 16.0, Scene[0].y);
        float h = HistoryValid != 0u && finiteMap && old[3].w > 0.0 && old[5].w > 0.0 ?
                  saturate(HistoryWeight) * sceneTrust : 0.0;
        if (h > 0.0)
        {
            // Support is part of the visible global appearance. Keep its
            // distribution stable as gaze selects different observations.
            float3 meanA = now[3].rgb * now[5].w, meanB = old[3].rgb * old[5].w;
            float3 dA, oA, dB, oB;
            Inverse(now[4].rgb, now[5].rgb, dA, oA);
            Inverse(old[4].rgb, old[5].rgb, dB, oB);
            float3 mean = lerp(meanA, meanB, h);
            float3 covarianceD = max(lerp(dA * now[5].w * now[5].w + meanA * meanA,
                                         dB * old[5].w * old[5].w + meanB * meanB, h) - mean * mean, 0.0);
            float3 covarianceO = lerp(oA * now[5].w * now[5].w + meanA.xxy * meanA.yzz,
                                     oB * old[5].w * old[5].w + meanB.xxy * meanB.yzz, h) - mean.xxy * mean.yzz;
            float norm = max(Peak(mean), 0.01);
            float3 inverseD, inverseO;
            Inverse(max(covarianceD / (norm * norm), 0.0016), covarianceO / (norm * norm), inverseD, inverseO);
            float normalization = now[3].w;
            float restorationWhite = now[12].w;
            ToScene(now); ToScene(old);
            [unroll] for (uint mix = 0; mix < 13u; ++mix) now[mix] = lerp(now[mix], old[mix], h);
            float dominance = now[6].w;
            float inv = dominance > 1e-6 ? rcp(dominance) : 0.0;
            now[0] *= inv; now[1] *= inv; now[2] *= inv;
            now[0].w /= normalization; now[1].w /= normalization; now[2].w /= normalization;
            now[6].rgb *= inv; now[7].rgb *= inv;
            now[10].rgb *= inv; now[11].rgb *= inv; now[12].rgb *= inv;
            now[6].w = dominance / max(now[4].w, 1e-6);
            now[3] = float4(mean / norm, normalization);
            now[4].rgb = inverseD;
            now[5] = float4(inverseO, norm);
            // This is the current frame's exposure contract, not appearance
            // history. Blending it would delay highlight protection on cuts.
            now[12].w = restorationWhite;
        }
        now[7].w = 1.0;
    }
    // Local fits remain current. Their final, spatially evaluated effect is
    // accumulated in the persistent cache after this pass.
    [unroll] for (uint outputRecord = 0; outputRecord < 13u; ++outputRecord)
        Filtered[address + uint2(outputRecord, 0)] = all(isfinite(now[outputRecord])) ? now[outputRecord] : 0.0;
}
)";

// Modes 7/8 below are retained solely as the old depth/log-gain experiment and
// its regression reference. Production exterior composition uses the three
// independent statistics/learning/application stages above; ordinary colour
// transfer is unchanged.
constexpr char kColorClampShader[] = R"(
#ifndef DLSSNR_COLOR_TRANSFER_LIBRARY
cbuffer Params : register(b0)
{
    uint2 OutputSize;
    uint2 SourceBase;
    uint2 SourceSize;
    uint2 DestinationBase;
    float PaperWhite;
    float TransferStrength;
    float ColorStrength;
    uint Mode;
    float ExposurePreExposure;
    float ExposureScale;
    uint UseExposure;
    uint EdgeBlendData;
    uint2 OriginalBase;
    uint2 RoiOffset;
    uint2 RoiSize;
    uint2 ColorClampPadding;
    float2 ExtrapolationDepthScale;
    float2 ExtrapolationDepthBias;
};

Texture2D<float4> InputColor : register(t0);
Texture2D<float4> ExposureInput : register(t1);
Texture2D<float4> OriginalColor : register(t2);
RWTexture2D<float4> OutputColor : register(u0);
#ifdef DLSSNR_PRESERVE_SOURCE
RWTexture2D<float4> PreservedSource : register(u1);
#endif
#endif
static const float MaxFp16Value = 65504.0;
// Keep the encoded HDR headroom below 1.0 after an FP16 store.  A value rounded
// to exactly 1.0 is deliberately decoded as the finite top of this transfer.
static const float MaxEncodedValue = 0.99951171875;
// Keep reference white at the model-domain midpoint while reserving a finite
// HDR headroom.  The input scale follows from
// log(1 + InputScale) / log(1 + HdrHeadroom * InputScale) = 0.5.
static const float HdrHeadroom = 32.0;
static const float LogInputScale = HdrHeadroom - 2.0;

float LogTransferRange()
{
    return log2(1.0 + HdrHeadroom * LogInputScale);
}

// Adapted with reference to the game-exposure-texture reading and white-point
// handling approach in Dagherbou's OptiScaler_DLSSNR (dlss-neural-rendering).
// Consulted snapshot: 393e0706b950a0ff1498e9dcf66989a80de72f31; see CREDITS.md
// for the scope of this reference, which is not an exact import revision.
// Thank you to Dagherbou and the OptiScaler contributors. See
// Licenses/OptiScaler_DLSSNR_ATTRIBUTION.txt for source links and GPL-3.0 terms.
// This adaptation uses the live NGX exposure ratio, validation and manual fallback.
float EffectivePaperWhite()
{
    float paperWhite = clamp(PaperWhite, 0.001, MaxFp16Value);
    if (UseExposure != 0)
    {
        const float exposure = ExposureInput.Load(int3(0, 0, 0)).r;
        if (isfinite(exposure) && exposure > 1e-6 && exposure < 1e6 &&
            isfinite(ExposurePreExposure) && ExposurePreExposure > 1e-6 &&
            isfinite(ExposureScale) && ExposureScale > 1e-6)
            paperWhite = clamp(ExposurePreExposure * ExposureScale / exposure, 0.01, 4096.0);
    }
    return paperWhite;
}

float3 SanitizeScene(float3 value)
{
    // Bit checks survive FXC's fast-math assumptions when this is inlined.
    bool3 valid = (asuint(value) & 0x7f800000u) != 0x7f800000u;
    // Display HDR can contain signed Rec.709 components after Rec.2020 decode.
    float floorValue = TransferStrength < 0.0 ? -MaxFp16Value : 0.0;
    return float3(valid.x ? clamp(value.x, floorValue, MaxFp16Value) : 0.0,
                  valid.y ? clamp(value.y, floorValue, MaxFp16Value) : 0.0,
                  valid.z ? clamp(value.z, floorValue, MaxFp16Value) : 0.0);
}

// Narkowicz's ACES-inspired scalar fit (not the full ACES color transform):
// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
// Applied per RGB channel, followed by the IEC sRGB display encoding.
float3 Filmic(float3 x)
{
    x = min(x, 8.0); // Already white here; avoid overflow for extreme exposure.
    return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14));
}

float3 LinearToSRGB(float3 x)
{
    return float3(x.x <= 0.0031308 ? 12.92 * x.x : 1.055 * pow(x.x, 1.0 / 2.4) - 0.055,
                  x.y <= 0.0031308 ? 12.92 * x.y : 1.055 * pow(x.y, 1.0 / 2.4) - 0.055,
                  x.z <= 0.0031308 ? 12.92 * x.z : 1.055 * pow(x.z, 1.0 / 2.4) - 0.055);
}

float3 SRGBToLinear(float3 x)
{
    return float3(x.x <= 0.04045 ? x.x / 12.92 : pow((x.x + 0.055) / 1.055, 2.4),
                  x.y <= 0.04045 ? x.y / 12.92 : pow((x.y + 0.055) / 1.055, 2.4),
                  x.z <= 0.04045 ? x.z / 12.92 : pow((x.z + 0.055) / 1.055, 2.4));
}

// Display HDR has already received the game's rendering/grade. Keep linear
// display values unchanged below 0.75 SDR white; only compress the shoulder.
// One gain preserves in-gamut RGB ratios, unlike another per-channel filmic.
float DisplayShoulder(float peak)
{
    float tail = max(peak - 0.75, 0.0);
    return peak <= 0.75 ? peak : 0.75 + 0.25 * tail / (0.25 + tail);
}

float DisplayEditConfidence(float peak)
{
    float gain = DisplayShoulder(peak) / max(peak, 1e-6);
    return peak <= 0.75 ? 1.0 : smoothstep(0.1, 0.5, gain);
}

float3 EncodeDisplaySDR(float3 value)
{
    float3 rgb = SanitizeScene(value) / EffectivePaperWhite();
    // SDR cannot represent negative components (wide-gamut HDR). Project
    // towards equal-luminance neutral only as far as needed to enter Rec.709.
    float low = min(min(rgb.r, rgb.g), rgb.b);
    if (low < 0.0)
    {
        float luminance = max(dot(rgb, float3(0.2126, 0.7152, 0.0722)), 0.0);
        rgb = lerp(luminance.xxx, rgb, luminance / max(luminance - low, 1e-6));
    }
    rgb = max(rgb, 0.0);
    float peak = max(max(rgb.r, rgb.g), rgb.b);
    return saturate(LinearToSRGB(rgb * (peak <= 0.75 ? 1.0 : DisplayShoulder(peak) / max(peak, 1e-6))));
}

float3 EncodeSDR(float3 value)
{
    // A zero transfer strength is reserved for already display-encoded SDR.
    // Keep it in the same reconstruction pipeline without applying filmic twice.
    if (TransferStrength == 0.0) return saturate(SanitizeScene(value));
    if (TransferStrength < 0.0) return EncodeDisplaySDR(value);
    float3 displayLinear = Filmic(SanitizeScene(value) / EffectivePaperWhite());
    float3 encoded = saturate(LinearToSRGB(displayLinear));
    // Preserve exact white despite pow approximation and FP16 store rounding.
    return float3(displayLinear.x >= 1.0 ? 1.0 : encoded.x,
                  displayLinear.y >= 1.0 ? 1.0 : encoded.y,
                  displayLinear.z >= 1.0 ? 1.0 : encoded.z);
}

float3 DecodeSDRRelative(float3 value)
{
    if (TransferStrength == 0.0) return saturate(value);
    float3 y = SRGBToLinear(saturate(value));
    float3 a = 2.51 - 2.43 * y;
    float3 b = 0.03 - 0.59 * y;
    float3 root = sqrt(b * b + 0.56 * a * y);
    // Choose the numerically stable quadratic form near black. At SDR white
    // the inverse is finite (~7.24 P); the original supplies clipped energy.
    return float3(b.x >= 0.0 ? 0.28 * y.x / (root.x + b.x) : (root.x - b.x) / (2.0 * a.x),
                  b.y >= 0.0 ? 0.28 * y.y / (root.y + b.y) : (root.y - b.y) / (2.0 * a.y),
                  b.z >= 0.0 ? 0.28 * y.z / (root.z + b.z) : (root.z - b.z) / (2.0 * a.z));
}

float HighlightConfidence(float peak)
{
    float x = min(peak, 8.0);
    // Relative contrast retained by the filmic curve, x * F'(x) / F(x).
    // This decreases along the shoulder; one RGB weight avoids hue seams.
    float elasticity = (1.408 * x * x + 0.7028 * x + 0.0042) /
                       ((2.51 * x + 0.03) * (x * (2.43 * x + 0.59) + 0.14));
    return smoothstep(0.05, 0.30, elasticity);
}

float3 RestoreSDR(float3 model, float3 original)
{
    original = SanitizeScene(original);
    if (!all(isfinite(model)))
        return original;
    if (TransferStrength == 0.0)
        return saturate(original + saturate(model) - f16tof32(f32tof16(saturate(original))));
    if (TransferStrength < 0.0)
    {
        // Transport the actual model edit in display-linear light. The original
        // supplies HDR energy and signed gamut; never invert the compressed
        // shoulder, whose steep derivative magnifies SDR noise/quantization.
        float white = EffectivePaperWhite();
        float3 reference = f16tof32(f32tof16(EncodeDisplaySDR(original)));
        float3 delta = (SRGBToLinear(saturate(model)) - SRGBToLinear(reference)) * white;
        float peak = max(max(original.r, original.g), original.b) / white;
        return clamp(original + DisplayEditConfidence(peak) * delta, -MaxFp16Value, MaxFp16Value);
    }
    float3 modelRelative = DecodeSDRRelative(model);
    float paperWhite = EffectivePaperWhite();
    float3 sourceRelative = original / paperWhite;
    float peak = max(max(sourceRelative.x, sourceRelative.y), sourceRelative.z);
    peak = max(peak, max(max(modelRelative.x, modelRelative.y), modelRelative.z));
    float confidence = HighlightConfidence(peak);
    // Match the actual FP16 model input before measuring its change. An
    // identity model then returns the original, even beyond SDR clipping.
    float3 encodedReference = f16tof32(f32tof16(EncodeSDR(original)));
    float3 delta = (modelRelative - DecodeSDRRelative(encodedReference)) * paperWhite;
    return clamp(original + confidence * delta, 0.0, MaxFp16Value);
}

)" R"(
float3 Encode(float3 value)
{
#if defined(DLSSNR_PRESERVE_SOURCE) || defined(DLSSNR_COLOR_TRANSFER_LIBRARY)
    // FXC's existing texture-load path lowers min(NaN, ceiling) to the
    // ceiling. Keep that legacy result explicit after an in-register FP16
    // conversion, whose NaN comparison otherwise optimizes differently.
    uint3 magnitude = asuint(value) & 0x7fffffffu;
    value = float3(magnitude.x > 0x7f800000u ? MaxFp16Value : value.x,
                   magnitude.y > 0x7f800000u ? MaxFp16Value : value.y,
                   magnitude.z > 0x7f800000u ? MaxFp16Value : value.z);
#endif
    // Sanitize before the logarithmic transfer. This also keeps invalid HDR
    // samples from reaching the model or contaminating the inverse pass.
    value.x = (value.x == value.x) ? min(value.x, MaxFp16Value) : 0.0;
    value.y = (value.y == value.y) ? min(value.y, MaxFp16Value) : 0.0;
    value.z = (value.z == value.z) ? min(value.z, MaxFp16Value) : 0.0;
    value = max(value, 0.0);
    const float transferStrength = clamp(TransferStrength, 0.001, MaxFp16Value);
    const float paperWhite = EffectivePaperWhite();
    float3 scaled = min(value * transferStrength, MaxFp16Value);
    // The old rational shoulder used an asymptotic inverse: its scene-domain
    // error grew as 1 / (1 - encoded)^2. NR output and the intermediate are
    // RGBA16F, so that amplification turned tiny encoded-domain variation into
    // visible HDR noise. This finite-headroom curve retains P -> 0.5 but makes
    // 32P -> 1.0, giving the inverse a bounded slope at the FP16 ceiling.
    float logRange = LogTransferRange();
    float3 mapped = log2(1.0 + scaled * (LogInputScale / paperWhite)) / logRange;
    return min(pow(saturate(mapped), max(ColorStrength, 0.001)), MaxEncodedValue);
}

float3 Decode(float3 value)
{
    // NR can propagate non-finite samples from HDR inputs.  Keep them out of
    // pow and the inverse shoulder; writing NaN/INF to the FP16 output can
    // poison the following post-processing chain.
    value.x = (value.x == value.x) ? min(value.x, MaxEncodedValue) : 0.0;
    value.y = (value.y == value.y) ? min(value.y, MaxEncodedValue) : 0.0;
    value.z = (value.z == value.z) ? min(value.z, MaxEncodedValue) : 0.0;
    value = max(value, 0.0);
    float3 mapped = pow(value, 1.0 / max(ColorStrength, 0.001));
    const float paperWhite = EffectivePaperWhite();
    const float transferStrength = clamp(TransferStrength, 0.001, MaxFp16Value);
    float logRange = LogTransferRange();
    float3 scaled = paperWhite * (exp2(mapped * logRange) - 1.0) / LogInputScale;
    return min(scaled / transferStrength, MaxFp16Value);
}

#ifndef DLSSNR_COLOR_TRANSFER_LIBRARY
float RoundedWeight(float2 p, float2 lo, float2 hi, uint mask, float width)
{
    float4 d = float4(p - lo, hi - p);
    float distance = width;
    if (mask & 1u) distance = min(distance, d.x);
    if (mask & 2u) distance = min(distance, d.y);
    if (mask & 4u) distance = min(distance, d.z);
    if (mask & 8u) distance = min(distance, d.w);
    float radius = min(2.0 * width, 0.5 * min(hi.x - lo.x, hi.y - lo.y));
    if ((mask & 3u) == 3u) distance = min(distance, radius - length(max(radius - d.xy, 0.0)));
    if ((mask & 6u) == 6u) distance = min(distance, radius - length(max(radius - d.zy, 0.0)));
    if ((mask & 12u) == 12u) distance = min(distance, radius - length(max(radius - d.zw, 0.0)));
    if ((mask & 9u) == 9u) distance = min(distance, radius - length(max(radius - d.xw, 0.0)));
    float t = saturate(distance / max(width, 1e-4));
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float ExtrapolationDepth(float2 p)
{
    uint2 q = uint2(clamp((p + 0.5) * ExtrapolationDepthScale + ExtrapolationDepthBias,
                         0.0, float2(SourceSize) - 1.0));
    return ExposureInput.Load(int3(SourceBase + q, 0)).x;
}

float3 ExtrapolationGain(uint2 q)
{
    float2 pos = (float2(q) + 0.5) * 32.0 / float2(RoiSize) - 0.5;
    int2 p = int2(floor(pos));
    float2 f = frac(pos);
    uint2 base = ColorClampPadding;
    float3 a = OriginalColor.Load(int3(base + uint2(clamp(p, 0, 31)), 0)).rgb;
    float3 b = OriginalColor.Load(int3(base + uint2(clamp(p + int2(1, 0), 0, 31)), 0)).rgb;
    float3 c = OriginalColor.Load(int3(base + uint2(clamp(p + int2(0, 1), 0, 31)), 0)).rgb;
    float3 d = OriginalColor.Load(int3(base + uint2(clamp(p + int2(1, 1), 0, 31)), 0)).rgb;
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float3 RawExtrapolationGain(uint2 q)
{
    float3 m = SanitizeScene(InputColor.Load(int3(q, 0)).rgb);
    float3 o = SanitizeScene(OriginalColor.Load(int3(OriginalBase + RoiOffset + q, 0)).rgb);
    float floorValue = max(1e-4, dot(o, float3(0.2126, 0.7152, 0.0722)) * 0.02);
    return clamp(log2((m + floorValue) / (o + floorValue)), -1.5, 1.5);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;
    if (any(pixel >= OutputSize))
        return;
#if defined(DLSSNR_PRESERVE_SOURCE)
    float4 original = InputColor.Load(int3(SourceBase + pixel, 0));
    if (ColorClampPadding.x != 0u)
        original.rgb = SanitizeScene(original.rgb);
    // Reproduce the old source-copy store/load before encoding. This matters
    // for native formats with more precision or range than the private FP16.
    original = f16tof32(f32tof16(original));
    PreservedSource[pixel] = original;
    float3 encoded = Mode == 4u ? EncodeSDR(original.rgb) : Encode(original.rgb);
    OutputColor[pixel] = float4(encoded, original.a);
#elif defined(DLSSNR_INVERSE_RESIDUAL)
    float4 original = OriginalColor.Load(int3(SourceBase + pixel, 0));
    float4 model = InputColor.Load(int3(pixel, 0));
    model.rgb = Mode == 5u ? RestoreSDR(model.rgb, original.rgb) : Decode(model.rgb);
    // The previous inverse wrote an FP16 decoded image before subtraction.
    // Keep that rounding boundary while eliminating its texture round trip.
    model = f16tof32(f32tof16(model));
    float3 delta = model.rgb - original.rgb;
    delta = all(delta == delta) ? clamp(delta, -65504.0, 65504.0) : 0.0;
    OutputColor[DestinationBase + pixel] = float4(delta, model.a - original.a);
#else
    if (Mode == 8)
    {
        float3 gain = 0.0;
        if (pixel.y == 32)
        {
            if (pixel.x != 0) return;
            [loop] for (uint gy = 0; gy < 32; ++gy)
            [loop] for (uint gx = 0; gx < 32; ++gx)
            {
                uint2 q = min(uint2((float2(gx, gy) + 0.5) * float2(RoiSize) / 32.0), RoiSize - 1u);
                gain += RawExtrapolationGain(q) / 1024.0;
            }
            OutputColor[DestinationBase + pixel] = float4(gain, 1.0);
            return;
        }
        // Area samples cover each coarse cell, suppressing fine AO/noise before
        // the field is extended. The expensive log runs only on this small grid.
        [loop] for (uint y = 0; y < 8; ++y)
        [loop] for (uint x = 0; x < 8; ++x)
        {
            uint2 q = min(uint2((float2(pixel) + (float2(x, y) + 0.5) / 8.0) *
                float2(RoiSize) / 32.0), RoiSize - 1u);
            gain += RawExtrapolationGain(q) / 64.0;
        }
        OutputColor[DestinationBase + pixel] = float4(gain, 1.0);
        return;
    }
    uint2 source = SourceBase + min(pixel, SourceSize - 1);
    // Mode 5 uses SourceBase for the original HDR subrect; NR output is zero-based.
    uint2 modelPixel = pixel;
    if (Mode == 7 && all(RoiSize != 0))
    {
        int2 roiRelative = int2(pixel) - int2(RoiOffset);
        modelPixel = uint2(clamp(roiRelative, int2(0, 0), int2(RoiSize) - 1));
    }
    float4 color = InputColor.Load(int3((Mode == 5 || Mode == 7) ? modelPixel : source, 0));
    if (Mode == 0)
        color.rgb = saturate(color.rgb);
    else if (Mode == 1)
        color.rgb = Encode(color.rgb);
    else if (Mode == 2)
        color.rgb = Decode(color.rgb);
    else if (Mode == 4)
        color.rgb = EncodeSDR(color.rgb);
    else if (Mode == 5)
        color.rgb = RestoreSDR(color.rgb, OriginalColor.Load(int3(source, 0)).rgb);
    else if (Mode == 6)
        color.rgb = SanitizeScene(color.rgb);
    uint edgeBlendPx = EdgeBlendData & 0xffffu;
)" R"(
    if (Mode == 7 && edgeBlendPx != 0 && ExposurePreExposure > 1.0)
    {
        // Mode 7 uses SourceBase/SourceSize for the logical depth rectangle.
        // Model input is the zero-origin ROI; the dispatch rectangle may be
        // larger and samples the ROI edge for its extrapolated model signal.
        uint edgeMask = (EdgeBlendData >> 16u) & 15u;
        float2 roiMin = float2(RoiOffset);
        float2 roiMax = roiMin + float2(RoiSize) - 1.0;
        bool insideRoi = all(pixel >= RoiOffset) && all(pixel < RoiOffset + RoiSize);
        float innerWidth = min(float(edgeBlendPx), max(1.0, 0.25 * float(min(RoiSize.x, RoiSize.y) - 1u)));
        float innerW = insideRoi ? RoundedWeight(pixel, roiMin, roiMax, edgeMask, innerWidth) : 0.0;
        // Compute the mask against the centered, UNCLIPPED rectangle. Clipping
        // the dispatch to the screen must neither move nor shrink this mask.
        float2 extra = ceil(float2(RoiSize) * ExposurePreExposure) - float2(RoiSize);
        float2 outerMin = roiMin - floor(extra * 0.5);
        float2 outerMax = roiMax + extra - floor(extra * 0.5);
        float outerWidth = min(float(edgeBlendPx), max(1.0, 0.5 * min(extra.x, extra.y)));
        float outerW = RoundedWeight(pixel, outerMin, outerMax, 15u, outerWidth);

        uint2 originalPixel = OriginalBase + pixel;
        float4 original = OriginalColor.Load(int3(originalPixel, 0));
        if (innerW >= 1.0)
        {
            OutputColor[DestinationBase + pixel] = color;
            return;
        }

        // Broad low-frequency gain, not a one-pixel blur of additive RGB.
        // Log gains keep unrelated colors from adding into one another, and
        // bounding the gain avoids dark-source division spikes. No history.
        if (outerW <= 0.0 && innerW <= 0.0)
        {
            OutputColor[DestinationBase + pixel] = original;
            return;
        }
        float2 stepPx = max(float2(RoiSize) / 12.0, 1.0);
        float2 anchor = clamp(float2(pixel) - roiMin, stepPx, max(stepPx, float2(RoiSize) - 1.0 - stepPx));
        float3 surfaceGainSum = 0.0;
        float weightSum = 0.0, surfaceWeightSum = 0.0;
        float centerDepth = 0.0;
        if (ExposureScale > 0.5) centerDepth = ExtrapolationDepth(pixel);
        float3 globalGain = OriginalColor.Load(int3(ColorClampPadding + uint2(0, 32), 0)).rgb;
        [branch] if (ExposureScale > 0.5)
        {
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            uint2 modelQ = uint2(clamp(anchor + float2(x, y) * stepPx, 0.0, float2(RoiSize) - 1.0));
            float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            float3 gain = ExtrapolationGain(modelQ);
            weightSum += w;
            if (ExposureScale > 0.5)
            {
                float d = ExtrapolationDepth(float2(RoiOffset + modelQ));
                // NGX DepthInverted: near=1 in reverse-Z, near=1-depth otherwise.
                float nearD = UseExposure != 0 ? d : 1.0 - d;
                float nearCenter = UseExposure != 0 ? centerDepth : 1.0 - centerDepth;
                float tolerance = max(1e-6, 0.02 * max(abs(nearD), abs(nearCenter)));
                float surface = isfinite(d) && isfinite(centerDepth) &&
                    d >= 0.0 && d <= 1.0 && centerDepth >= 0.0 && centerDepth <= 1.0
                    ? exp2(-4.0 * pow(abs(d - centerDepth) / tolerance, 2.0)) : 0.0;
                surfaceGainSum += gain * (w * surface);
                surfaceWeightSum += w * surface;
            }
        }
        }
        float3 gain = globalGain;
        if (ExposureScale > 0.5 && isfinite(centerDepth) && centerDepth >= 0.0 && centerDepth <= 1.0)
        {
            // Surface-specific deviation fades back to the global tone if no
            // matching surface was observed inside the ROI.
            float confidence = saturate(surfaceWeightSum / max(weightSum * 0.1, 1e-4));
            gain = lerp(globalGain, surfaceGainSum / max(surfaceWeightSum, 1e-4), confidence);
        }
        float3 originalRgb = SanitizeScene(original.rgb);
        float floorValue = max(1e-4, dot(originalRgb, float3(0.2126, 0.7152, 0.0722)) * 0.02);
        float3 extrapolated = clamp((originalRgb + floorValue) * exp2(gain) - floorValue, 0.0, MaxFp16Value);
        color.rgb = lerp(lerp(originalRgb, extrapolated, outerW), color.rgb, innerW);
        color.a = lerp(original.a, color.a, innerW);
    }
    else
    if (edgeBlendPx != 0)
    {
        // Use a rounded-rectangle signed distance. Adjacent active edges join
        // as a circular arc instead of the diagonal crease produced by min()
        // at a square corner. A radius twice the blend width leaves both the
        // outside and inside of the transition rounded when space permits.
        // Bits 16..19 are left, top, right and bottom respectively; logical
        // full-frame edges remain exempt.
        uint edgeMask = EdgeBlendData >> 16u;
        float left = float(pixel.x);
        float top = float(pixel.y);
        float right = float(OutputSize.x - 1u - pixel.x);
        float bottom = float(OutputSize.y - 1u - pixel.y);
        float edgeDistance = float(edgeBlendPx);
        if ((edgeMask & 1u) != 0) edgeDistance = min(edgeDistance, left);
        if ((edgeMask & 2u) != 0) edgeDistance = min(edgeDistance, top);
        if ((edgeMask & 4u) != 0) edgeDistance = min(edgeDistance, right);
        if ((edgeMask & 8u) != 0) edgeDistance = min(edgeDistance, bottom);

        float maxRadius = 0.5 * float(min(OutputSize.x - 1u, OutputSize.y - 1u));
        float radius = min(2.0 * float(edgeBlendPx), maxRadius);
        if ((edgeMask & 3u) == 3u)
            edgeDistance = min(edgeDistance, radius - length(max(radius - float2(left, top), 0.0)));
        if ((edgeMask & 6u) == 6u)
            edgeDistance = min(edgeDistance, radius - length(max(radius - float2(right, top), 0.0)));
        if ((edgeMask & 12u) == 12u)
            edgeDistance = min(edgeDistance, radius - length(max(radius - float2(right, bottom), 0.0)));
        if ((edgeMask & 9u) == 9u)
            edgeDistance = min(edgeDistance, radius - length(max(radius - float2(left, bottom), 0.0)));

        // Quintic smootherstep has zero first and second derivatives at both
        // ends, removing the two faint contour lines of the old linear ramp.
        float t = saturate(edgeDistance / float(edgeBlendPx));
        float processedWeight = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
        float4 original = OriginalColor.Load(int3(pixel, 0));
        color = lerp(original, color, processedWeight);
        color.a = 1.0;
    }
    OutputColor[DestinationBase + pixel] = color;
#endif
}
#endif
)";

constexpr char kLowDownsampleShader[] = R"(
cbuffer Params : register(b0) {
    uint2 SourceBase; uint2 SourceSize;
    uint2 OutputSize; uint Scale; uint Transfer;
    float PaperWhite; float TransferStrength; float ColorStrength; uint UseExposure;
    float ExposurePreExposure; float ExposureScale; uint2 Padding;
};
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> Baseline : register(u0);
#ifndef DLSSNR_DOWNSAMPLE_BASELINE_ONLY
RWTexture2D<float4> NRColor : register(u1);
#endif
#ifdef DLSSNR_COLOR_TRANSFER_LIBRARY
Texture2D<float4> ExposureInput : register(t1);
float3 Encode(float3 value);
float3 EncodeSDR(float3 value);
#endif

// Production specializes the tap count and shared-memory size for each scale.
// The generic variant remains available to standalone shader consumers.
#ifndef DLSSNR_DOWNSAMPLE_SCALE
#define DLSSNR_DOWNSAMPLE_SCALE 0
#endif
#if DLSSNR_DOWNSAMPLE_SCALE == 0
#define FilterScale Scale
#define CacheSide 35
#define TapLoop [loop]
#else
#define FilterScale DLSSNR_DOWNSAMPLE_SCALE
#define CacheSide (11 * FilterScale + 2)
#define TapLoop [unroll]
#endif
groupshared float4 SourceTile[CacheSide * CacheSide];
groupshared float4 Horizontal[CacheSide * 8];

float CatmullRomWeight(float x) {
    x = abs(x);
    if (x < 1.0)
        return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0)
        return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}
[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID,
                              uint3 lane : SV_GroupThreadID, uint threadIndex : SV_GroupIndex) {
    int side = int(11 * FilterScale + 2);
    int tapCount = int(4 * FilterScale);
    int firstTap = 1 - int(2 * FilterScale);
    // Eight outputs span at most seven Scale-sized source steps. Adding the
    // four-Scale-wide kernel needs 22x22 (2x) or 33x33 (3x) source texels.
    // One guard texel on each side also covers float center rounding across
    // integer boundaries on large/odd grids, without changing that mapping.
    int2 firstCenter = int2(floor((float2(group.xy * 8) + 0.5) *
                                  float2(SourceSize) / float2(OutputSize) - 0.5));
    int2 origin = firstCenter + firstTap - 1;
    [loop] for (uint index = threadIndex; index < uint(side * side); index += 64) {
        int2 p = clamp(origin + int2(index % uint(side), index / uint(side)),
                       int2(0, 0), int2(SourceSize) - 1);
        SourceTile[index] = InputColor.Load(int3(int2(SourceBase) + p, 0));
    }
    GroupMemoryBarrierWithGroupSync();

    // Inactive edge lanes must produce their horizontal rows and participate
    // in BOTH barriers. Only the final UAV write is restricted to OutputSize.
    uint2 outputPixel = min(id.xy, OutputSize - 1);
    float2 sourcePosition = (float2(outputPixel) + 0.5) *
                             float2(SourceSize) / float2(OutputSize) - 0.5;
    int2 center = int2(floor(sourcePosition));
    float2 fraction = frac(sourcePosition);
    int2 offset = center - firstCenter + 1;
    float horizontalWeights[12];
    float horizontalWeightSum = 0.0;
    [unroll] for (int tap = 0; tap < 12; ++tap) {
        float weight = tap < tapCount
                           ? CatmullRomWeight((float(firstTap + tap) - fraction.x) / float(FilterScale))
                           : 0.0;
        horizontalWeights[tap] = weight;
        horizontalWeightSum += weight;
    }
    [loop] for (int row = int(lane.y); row < side; row += 8) {
        float4 color = 0.0;
        TapLoop for (int xi = 0; xi < tapCount; ++xi)
            color += SourceTile[row * side + offset.x + xi] * horizontalWeights[xi];
        Horizontal[row * 8 + lane.x] = color / horizontalWeightSum;
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id.xy >= OutputSize)) return;

    float4 filtered = 0.0;
    float verticalWeightSum = 0.0;
    TapLoop for (int yi = 0; yi < tapCount; ++yi) {
        float verticalWeight = CatmullRomWeight((float(firstTap + yi) - fraction.y) / float(FilterScale));
        filtered += Horizontal[(offset.y + yi) * 8 + lane.x] * verticalWeight;
        verticalWeightSum += verticalWeight;
    }
    filtered /= verticalWeightSum;

    // Preserve the exact source-footprint HDR clamp, including rounded sizes.
    // These texels are already in SourceTile; no second texture scan is needed.
    // OutputSize=ceil(SourceSize/FilterScale) keeps this footprint in the tile.
    uint2 footprintOrigin = id.xy * FilterScale;
    float4 footprintMin = 3.402823466e+38;
    float4 footprintMax = -footprintMin;
    TapLoop for (uint fy = 0; fy < FilterScale; ++fy) TapLoop for (uint fx = 0; fx < FilterScale; ++fx) {
        int2 p = int2(min(footprintOrigin + uint2(fx, fy), SourceSize - 1)) - origin;
        float4 sample = SourceTile[p.y * side + p.x];
        footprintMin = min(footprintMin, sample);
        footprintMax = max(footprintMax, sample);
    }
    float4 base = clamp(filtered, footprintMin, footprintMax);
    Baseline[id.xy] = base;
#ifdef DLSSNR_COLOR_TRANSFER_LIBRARY
    // Match the FP16 baseline read by the former separate transfer dispatch.
    // Encoding remains after the scene-linear filter and HDR footprint clamp.
    base = f16tof32(f32tof16(base));
    float3 encoded = Transfer == 2u ? EncodeSDR(base.rgb) : Encode(base.rgb);
    NRColor[id.xy] = float4(encoded, base.a);
#elif !defined(DLSSNR_DOWNSAMPLE_BASELINE_ONLY)
    NRColor[id.xy] = base;
#endif
}
)";

constexpr char kLowResidualShader[] = R"(
cbuffer Params : register(b0) { uint2 Size; float PaperWhite; float TransferStrength; float ColorStrength; uint Transfer; uint ZeroResidual; uint3 Padding; };
Texture2D<float4> NRColor : register(t0);
Texture2D<float4> Baseline : register(t1);
RWTexture2D<float4> Residual : register(u0);
[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size)) return;
    if (ZeroResidual != 0) {
        Residual[id.xy] = 0.0;
        return;
    }
    float4 nr = NRColor.Load(int3(id.xy,0));
    float4 base = Baseline.Load(int3(id.xy,0));
    // The residual is the actual scene-domain correction produced by NR. Do
    // not clamp it relative to the baseline: that would discard exposure and
    // chroma changes before the high-resolution reconstruction can apply them.
    // Only reject non-finite values, which otherwise poison the whole output.
    float3 delta = nr.rgb - base.rgb;
    if (all(delta == delta))
        delta = clamp(delta, -65504.0, 65504.0);
    else
        delta = 0.0;
    Residual[id.xy] = float4(delta, nr.a - base.a);
}
)";

// Stabilize model changes relative to the current input, before inverse
// transfer/reconstruction. Source alpha holds the effective sample count;
// model alpha and the exact model guide/subrect contract remain untouched.
constexpr char kOutputTemporalShader[] = R"(
cbuffer Params : register(b0)
{
    uint2 Size; uint HistoryValid; uint DepthInverted;
    uint2 MotionBase; uint2 MotionSize;
    uint2 DepthBase; uint2 DepthSize;
    uint2 SourceBase; uint2 SourceSize;
    float2 MotionScale; float2 JitterCorrection;
    uint4 Padding;
};
Texture2D<float4> Current : register(t0);
Texture2D<float2> Motion : register(t1);
Texture2D<float> Depth : register(t2);
Texture2D<float4> Source : register(t3);
Texture2D<float4> History : register(t4);
Texture2D<float4> PreviousSource : register(t5);
Texture2D<float> PreviousDepth : register(t6);
RWTexture2D<float4> Result : register(u0);
RWTexture2D<float4> SourceHistory : register(u1);
RWTexture2D<float> DepthHistory : register(u2);
SamplerState LinearClamp : register(s0);

float Max3(float3 v) { return max(v.x, max(v.y, v.z)); }
float3 RoundToHalf(float3 v)
{
    // Explicit nearest-even rounding prevents a repeated FP16 UAV narrowing
    // from biasing the accumulated innovation. Preserve NaN/Inf for rejection.
    uint3 bits = asuint(v), magnitude = bits & 0x7fffffffu;
    uint3 rounded = (magnitude + 0xfffu + ((magnitude >> 13) & 1u)) & 0xffffe000u;
    float3 normal = asfloat(rounded | (bits & 0x80000000u));
    float3 subnormal = round(v * 16777216.0) / 16777216.0;
    float3 result = magnitude < 0x38800000u ? subnormal : normal;
    return magnitude >= 0x7f800000u ? v : result;
}
float3 ReadSource(float2 uv)
{
    uint w, h; Source.GetDimensions(w, h);
    float2 p = SourceBase + clamp(uv * SourceSize, 0.5, float2(SourceSize) - 0.5);
    // Use the same FP16 reference now and in SourceHistory. Otherwise the
    // source-history rounding error is re-added every frame by the predictor,
    // especially when full-output mode interpolates a smaller model input.
    return RoundToHalf(Source.SampleLevel(LinearClamp, p / float2(w, h), 0).rgb);
}
[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (any(p >= Size)) return;
    float2 uv = (float2(p) + 0.5) / Size;
    float4 current = Current.Load(int3(p, 0));
    float3 source = ReadSource(uv);
    // Integer pixel-center mapping avoids a float reciprocal rounding an
    // exact native-guide texel boundary down to its neighbour at 2x/3x grids.
    uint2 depthPixel = min(((2 * p + 1) * DepthSize) / (2 * Size), DepthSize - 1);
    float z = Depth.Load(int3(DepthBase + depthPixel, 0));
    bool validColor = all(isfinite(current)) && all(isfinite(source)) && all(abs(source) <= 65504.0);
    bool validDepth = isfinite(z) && z >= 0 && z <= 1;
    SourceHistory[p] = validColor ? float4(source, validDepth ? 1 : 0) : 0;
    DepthHistory[p] = z;
    Result[p] = current;
    if (HistoryValid == 0 || !validColor || !validDepth) return;

    uint2 motionPixel = min(((2 * p + 1) * MotionSize) / (2 * Size), MotionSize - 1);
    float2 mv = Motion.Load(int3(MotionBase + motionPixel, 0));
    float2 previous = float2(p) + mv * MotionScale + JitterCorrection;
    if (!all(isfinite(previous)) || any(previous < -0.5) || any(previous > float2(Size) - 0.5)) return;

    // Reject geometry per tap, but compare color AFTER reconstructing the
    // corresponding source footprint. Comparing each texel with its subpixel
    // destination mistakes ordinary texture variation for temporal change.
    int2 base = int2(floor(previous));
    float2 f = frac(previous);
    float3 history = 0, oldSource = 0, sourceMoment = 0;
    float3 modelLo = current.rgb, modelHi = current.rgb;
    float support = 0, sampleCount = 0;
    float depthTolerance = 1e-5 + 0.01 * max(DepthInverted != 0 ? z : 1 - z, 1e-3);
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x)
    {
        int2 q = base + int2(x, y);
        if (any(q < 0) || any(q >= int2(Size))) continue;
        float weight = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
        if (weight <= 0) continue;
        float hz = PreviousDepth.Load(int3(q, 0));
        float4 hs = PreviousSource.Load(int3(q, 0));
        float3 hc = History.Load(int3(q, 0)).rgb;
        if (!isfinite(hz) || hz < 0 || hz > 1 || abs(hz - z) > depthTolerance ||
            !all(isfinite(hs)) || hs.a < 1 || !all(isfinite(hc))) continue;
        float3 ds = hs.rgb - source;
        modelLo = min(modelLo, hc); modelHi = max(modelHi, hc);
        history += weight * hc;
        oldSource += weight * ds;
        sourceMoment += weight * ds * ds;
        sampleCount += weight * min(hs.a, 20.0);
        support += weight;
    }
    if (support < 1e-4) return;

    history /= support;
    oldSource /= support;
    // Fractional reconstruction has a measurable spatial uncertainty. It is
    // zero for an integer reprojection or a uniform input, so stationary
    // recolors/exposure changes cannot hide behind a large nearby color range.
    float3 footprintSigma = sqrt(max(0, sourceMoment / support - oldSource * oldSource));
    float3 difference = max(0, abs(oldSource) - footprintSigma);
    float change = Max3(difference / (0.05 + max(abs(source + oldSource), abs(source))));
    float confidence = 1 - smoothstep(0.04, 0.20, change);
    if (confidence <= 0) return;

    // Fit a regularized local affine source -> model predictor. Reprojecting
    // its innovation instead of the complete image preserves current source
    // detail, including the model's local contrast (not just a unit residual).
    // Centered moments avoid cancellation on bright, nearly uniform inputs.
    float3 meanS = 0, meanC = 0, momentS = 0, momentC = 0, crossMoment = 0;
    float3 dxS = 0, dxC = 0, dxSC = 0, dyS = 0, dyC = 0, dySC = 0;
    float taps = 0;
    [unroll] for (int j = -1; j <= 1; ++j)
    [unroll] for (int i = -1; i <= 1; ++i)
    {
        int2 q = clamp(int2(p) + int2(i, j), 0, int2(Size) - 1);
        float3 c = Current.Load(int3(q, 0)).rgb;
        float3 s = ReadSource((float2(q) + 0.5) / Size);
        if (!all(isfinite(c)) || !all(isfinite(s)) || any(abs(s) > 65504.0)) continue;
        modelLo = min(modelLo, c); modelHi = max(modelHi, c);
        float3 ds = s - source, dc = c - current.rgb;
        meanS += ds; meanC += dc;
        momentS += ds * ds; momentC += dc * dc; crossMoment += ds * dc;
        if (j == 0 && i != 0) { dxS += ds * ds; dxC += dc * dc; dxSC += ds * dc; }
        if (i == 0 && j != 0) { dyS += ds * ds; dyC += dc * dc; dySC += ds * dc; }
        taps += 1;
    }
    meanS /= taps; meanC /= taps;
    float3 varianceS = max(0, momentS / taps - meanS * meanS);
    float3 varianceC = max(0, momentC / taps - meanC * meanC);
    float3 covariance = crossMoment / taps - meanS * meanC;
    float3 regularizer = 0.005 + 0.01 * abs(source);
    regularizer *= regularizer;
    float3 gain = clamp((covariance + regularizer) / (varianceS + regularizer), 0, 4);
    float3 innovationMean = meanC - gain * meanS;
    float3 innovationSigma = sqrt(max(0, varianceC + gain * gain * varianceS - 2 * gain * covariance));

    // Bilinear transport is uncertain on model-only detail that the source
    // predictor cannot reconstruct. Its footprint variance is f*(1-f) per
    // axis; measure the unexplained directional gradients in the current
    // neighborhood. Inverse-variance weighting prevents repeated resampling
    // from erasing this detail. Integer/static transport and spatially smooth
    // model changes pay no penalty, regardless of camera/ROI translation.
    float3 gradientX = max(0, 0.5 * (dxC + gain * gain * dxS - 2 * gain * dxSC));
    float3 gradientY = max(0, 0.5 * (dyC + gain * gain * dyS - 2 * gain * dySC));
    float3 transportVariance = f.x * (1 - f.x) * gradientX + f.y * (1 - f.y) * gradientY;
    float signalScale = Max3(max(abs(current.rgb), abs(source)));
    float transportTolerance = 0.015 + 0.04 * signalScale;
    confidence /= 1 + Max3(transportVariance / (transportTolerance * transportTolerance));

    // Work relative to the current center. The interval always includes zero,
    // so a genuine persistent thin model feature is never clipped by its own
    // neighborhood. A source-validated innovation budget also lets spatially
    // uniform model flicker accumulate instead of tracking each noisy frame.
    float allowance = 0.05 + 0.15 * signalScale;
    float3 lo = min(0, innovationMean - 2 * innovationSigma) - allowance;
    float3 hi = max(0, innovationMean + 2 * innovationSigma) + allowance;
    float3 extent = 0.5 * (hi - lo), center = 0.5 * (hi + lo);
    float3 prediction = history - current.rgb - gain * oldSource;
    float3 delta = prediction - center;
    float clipping = max(1, Max3(abs(delta) / extent));
    float3 boundedInnovation = center + delta / clipping;
    // Source transport must not invent extrema beyond either observed model
    // image, especially past an encoded HDR shoulder. Constrain each fitted
    // channel independently: a model channel fixed at black must not disable
    // accumulation in the other two. Including the old model range permits
    // flat flicker to average even with a spatially constant current frame.
    boundedInnovation = clamp(boundedInnovation, modelLo - current.rgb, modelHi - current.rgb);

    // Evidence, not camera/ROI speed, controls memory. Consistent surfaces
    // converge to 95% history; incomplete support, input changes and clipped
    // model changes continuously shorten it. New/rejected pixels start at 1.
    float retained = min(sampleCount / support, 19.0) * saturate(support) * confidence / clipping;
    float count = 1 + retained;
    float3 resolved = current.rgb + (retained / count) * boundedInnovation;
    if (!all(isfinite(resolved))) return;
    Result[p] = float4(RoundToHalf(clamp(resolved, -65504.0, 65504.0)), current.a);
    SourceHistory[p] = float4(source, RoundToHalf(count.xxx).x);
}
)";

constexpr char kTemporalResidualShader[] = R"(
cbuffer Params : register(b0) {
    uint2 Size;
    uint Reset;
    uint HistoryValid;
    uint2 MotionBase;
    uint2 MotionSize;
    uint2 DepthBase;
    uint2 DepthSize;
    float2 MotionScale;
    float2 JitterCorrection;
};
Texture2D<float4> CurrentResidual : register(t0);
Texture2D<float2> Motion : register(t1);
Texture2D<float> CurrentDepth : register(t2);
Texture2D<float4> PreviousResidual : register(t3);
Texture2D<float> PreviousDepth : register(t4);
RWTexture2D<float4> NextResidual : register(u0);
RWTexture2D<float> NextDepth : register(u1);

float4 LoadPreviousResidual(float2 position)
{
    position = clamp(position, 0.0, float2(Size) - 1.0);
    int2 p = int2(floor(position));
    float2 f = frac(position);
    int2 q = min(p + 1, int2(Size) - 1);
    float4 top = lerp(PreviousResidual.Load(int3(p, 0)),
                      PreviousResidual.Load(int3(q.x, p.y, 0)), f.x);
    float4 bottom = lerp(PreviousResidual.Load(int3(p.x, q.y, 0)),
                         PreviousResidual.Load(int3(q, 0)), f.x);
    return lerp(top, bottom, f.y);
}

[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size))
        return;

    float3 current = CurrentResidual.Load(int3(id.xy, 0)).rgb;
    uint2 depthPixel = min(((2 * id.xy + 1) * DepthSize) / (2 * Size), DepthSize - 1);
    float currentDepth = CurrentDepth.Load(int3(DepthBase + depthPixel, 0));
    float3 neighborhoodMin = current;
    float3 neighborhoodMax = current;
    [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
        int2 p = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(Size) - 1);
        float3 sampleValue = CurrentResidual.Load(int3(p, 0)).rgb;
        neighborhoodMin = min(neighborhoodMin, sampleValue);
        neighborhoodMax = max(neighborhoodMax, sampleValue);
    }

    // Motion and depth may retain different native grids and non-zero bases.
    // Convert vector values to residual pixels only after the native texel read.
    uint2 motionPixel = min(((2 * id.xy + 1) * MotionSize) / (2 * Size), MotionSize - 1);
    float2 motion = Motion.Load(int3(MotionBase + motionPixel, 0)) *
                    MotionScale + JitterCorrection;
    float2 previousPosition = float2(id.xy) + motion;
    bool inBounds = all(isfinite(previousPosition)) &&
                    all(previousPosition >= 0.0) && all(previousPosition < float2(Size));
    // Unfiltered game vectors can contain invalid or extreme values. Reject
    // them before converting the position to an integer history coordinate.
    previousPosition = inBounds ? previousPosition : float2(id.xy);
    motion = inBounds ? motion : 0.0;
    int2 previousDepthPosition = clamp(int2(round(previousPosition)), int2(0, 0), int2(Size) - 1);
    float previousDepth = PreviousDepth.Load(int3(previousDepthPosition, 0));
    float depthThreshold = max(0.002, max(abs(currentDepth), abs(previousDepth)) * 0.02);
    bool finiteDepth = currentDepth == currentDepth && previousDepth == previousDepth;
    bool depthMatches = finiteDepth && abs(currentDepth - previousDepth) <= depthThreshold;
    bool acceptHistory = HistoryValid != 0 && Reset == 0 && inBounds && depthMatches;

    float4 previous = LoadPreviousResidual(previousPosition);
    float3 clippedHistory = clamp(previous.rgb, neighborhoodMin, neighborhoodMax);
    float3 localRange = max(neighborhoodMax - neighborhoodMin, max(abs(current) * 0.1, 0.005));
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    float disagreement = dot(abs(current - clippedHistory) / localRange, luma);
    float agreement = 1.0 - smoothstep(0.20, 1.25, disagreement);
    float motionConfidence = 1.0 - saturate(length(motion) * 0.125);
    float previousConfidence = saturate(previous.a);
    float confidence = acceptHistory
                           ? saturate(lerp(previousConfidence, agreement * motionConfidence, 0.25))
                           : 0.0;

    // Random residuals need a longer average; stable residuals only need a
    // modest history weight and therefore retain their model-produced detail.
    float historyWeight = lerp(0.88, 0.62, agreement) * lerp(0.55, 1.0, motionConfidence);
    float3 resolved = acceptHistory ? lerp(current, clippedHistory, historyWeight) : current;
    // History blending must not change the residual's luminance energy. Use a
    // single scalar for all channels so temporal stabilization cannot shift
    // the model's chroma while restoring magnitude lost to averaging.
    float currentEnergy = dot(abs(current), luma);
    float resolvedEnergy = dot(abs(resolved), luma);
    if (acceptHistory && currentEnergy > 1e-5 && resolvedEnergy > 1e-5)
        resolved *= currentEnergy / resolvedEnergy;
    resolved = clamp(resolved, -65504.0, 65504.0);
    if (!all(resolved == resolved)) {
        resolved = 0.0;
        confidence = 0.0;
    }
    NextResidual[id.xy] = float4(resolved, confidence);
    NextDepth[id.xy] = currentDepth;
}
)";

constexpr char kGlobalDownsampleCropShader[] = R"(
cbuffer Params : register(b0) {
    uint2 FullSize;
    uint2 LowSize;
    uint2 RoiOrigin;
    uint2 RoiFullSize;
    uint2 OutputSize;
    float PaperWhite; float TransferStrength; float ColorStrength;
    float ExposurePreExposure; float ExposureScale; uint Transfer;
};
Texture2D<float4> GlobalLowColor : register(t0);
RWTexture2D<float4> RoiBaseline : register(u0);
RWTexture2D<float4> RoiColor : register(u1);
#ifdef DLSSNR_COLOR_TRANSFER_LIBRARY
Texture2D<float4> ExposureInput : register(t1);
#define UseExposure (Transfer >> 2u)
float3 Encode(float3 value);
float3 EncodeSDR(float3 value);
#endif

float4 LoadLow(int2 position)
{
    position = clamp(position, int2(0, 0), int2(LowSize) - 1);
    return GlobalLowColor.Load(int3(position, 0));
}

[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputSize))
        return;

    // Produce the exact low-resolution ROI extent expected by the normal NR
    // contract, but source it from a decimation lattice fixed to the complete
    // frame. When ROI origin and extent align to the scale this is an integer
    // crop; otherwise bilinear sampling preserves the same screen-space extent
    // without re-running the downsample at a moving local phase.
    float2 fullPosition = float2(RoiOrigin) +
        (float2(id.xy) + 0.5) * float2(RoiFullSize) / float2(OutputSize);
    float2 lowPosition = fullPosition * float2(LowSize) / float2(FullSize) - 0.5;
    int2 lowBase = int2(floor(lowPosition));
    float2 fraction = frac(lowPosition);
    float4 top = lerp(LoadLow(lowBase), LoadLow(lowBase + int2(1, 0)), fraction.x);
    float4 bottom = lerp(LoadLow(lowBase + int2(0, 1)),
                         LoadLow(lowBase + int2(1, 1)), fraction.x);
    float4 color = lerp(top, bottom, fraction.y);
    RoiBaseline[id.xy] = color;
#ifdef DLSSNR_COLOR_TRANSFER_LIBRARY
    color = f16tof32(f32tof16(color));
    float3 encoded = (Transfer & 3u) == 2u ? EncodeSDR(color.rgb) : Encode(color.rgb);
    RoiColor[id.xy] = float4(encoded, color.a);
#else
    RoiColor[id.xy] = color;
#endif
}
)";

constexpr char kLowCompositeShader[] = R"(
#ifndef DLSSNR_DISPLAY_COMPOSITE
#define DLSSNR_DISPLAY_COMPOSITE 0
#endif
#ifndef DLSSNR_FAST_COMPOSITE
#define DLSSNR_FAST_COMPOSITE 0
#endif
cbuffer Params : register(b0) {
    uint2 HighSize; uint2 LowSize; uint2 BaseSource; uint2 DestinationBase;
    uint CompositeMode; uint TemporalReconstruction; uint HighResolutionGuided; uint FeatureGuided; uint SourceScale; uint DisplayHDR; float DisplayPaperWhite; uint CompositePadding;
};
Texture2D<float4> BaseColor : register(t0);
Texture2D<float4> LowBaseline : register(t1);
Texture2D<float4> Correction : register(t2);
RWTexture2D<float4> Output : register(u0);
// One shared tile of source/model pairs. A additionally measures footprint
// purity so mixed low cells are not treated as pure destination surfaces.
// B trades that protection and the outer support ring for lower cost.
#if DLSSNR_FAST_COMPOSITE
static const int FeatureRadius = 1;
#else
static const int FeatureRadius = 2;
#endif
static const int FeatureTaps = 2 * FeatureRadius + 1;
static const int FeatureCacheWidth = 8 + 2 * FeatureRadius;
groupshared float4 FeatureSourceCache[FeatureCacheWidth * FeatureCacheWidth];
groupshared float4 FeatureDeltaCache[FeatureCacheWidth * FeatureCacheWidth];

void LoadFeatureSample(int2 cell, out float4 source, out float3 delta)
{
#if DLSSNR_DISPLAY_COMPOSITE
    // Display-linear Rec.709 can legitimately be signed after HDR gamut
    // conversion. Fit the actual restored edit, without clipping either pair.
    float3 base = LowBaseline.Load(int3(cell, 0)).rgb;
    delta = Correction.Load(int3(cell, 0)).rgb;
#else
    float3 base = max(LowBaseline.Load(int3(cell, 0)).rgb, 0.0);
    delta = max(base + Correction.Load(int3(cell, 0)).rgb, 0.0) - base;
#endif
#if DLSSNR_FAST_COMPOSITE
    // B deliberately omits source-footprint purity. Destination RGB affinity
    // and the full colour covariance fit still select and transport appearance.
    source = float4(base, 1.0);
#else
    float3 lo = 65504.0;
    float3 hi = 0.0;
    uint scale = clamp(SourceScale, 1u, 3u);
    // Match the footprint used by the production downsample's anti-ringing
    // clamp, including odd dimensions and one-pixel axes.
    [loop] for (uint y = 0; y < scale; ++y) {
        [loop] for (uint x = 0; x < scale; ++x) {
            uint2 pixel = min(uint2(cell) * scale + uint2(x, y), HighSize - 1u);
#if DLSSNR_DISPLAY_COMPOSITE
            float3 c = BaseColor.Load(int3(BaseSource + pixel, 0)).rgb;
#else
            float3 c = max(BaseColor.Load(int3(BaseSource + pixel, 0)).rgb, 0.0);
#endif
            lo = min(lo, c);
            hi = max(hi, c);
        }
    }
    float3 range = hi - lo;
    float energy = max(max(hi.r, hi.g), max(hi.b, 0.01));
    float mixed = dot(range, range) / (energy * energy);
    source = float4(base, 1.0 / (1.0 + 8.0 * mixed));
#endif
}

float3 RelativeDifference(float3 a, float3 b)
{
    return abs(a - b) / max(max(abs(a), abs(b)), 0.25);
}

float GuideConfidence(float3 highColor, float3 bilinearBaseline)
{
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    float3 difference = RelativeDifference(highColor, bilinearBaseline);
    float metric = dot(difference, luma);
    return 1.0 - smoothstep(0.08, 0.55, metric);
}

float2 LowPosition(float2 p)
{
    return (p + 0.5) * float2(LowSize) / float2(HighSize) - 0.5;
}

float4 SampleBilinearBaseline(float2 p)
{
    float2 q = LowPosition(p);
    q = clamp(q, float2(0.0, 0.0), float2(LowSize) - 1.0);
    int2 i = int2(floor(q));
    float2 f = frac(q);
    int2 j = min(i + 1, int2(LowSize) - 1);
    float4 a = lerp(LowBaseline.Load(int3(i, 0)), LowBaseline.Load(int3(j.x, i.y, 0)), f.x);
    float4 b = lerp(LowBaseline.Load(int3(i.x, j.y, 0)), LowBaseline.Load(int3(j, 0)), f.x);
    return lerp(a, b, f.y);
}

float4 SampleBilinearResidual(float2 p)
{
    float2 q = LowPosition(p);
    q = clamp(q, float2(0.0, 0.0), float2(LowSize) - 1.0);
    int2 i = int2(floor(q));
    float2 f = frac(q);
    int2 j = min(i + 1, int2(LowSize) - 1);
    float4 a = lerp(Correction.Load(int3(i, 0)), Correction.Load(int3(j.x, i.y, 0)), f.x);
    float4 b = lerp(Correction.Load(int3(i.x, j.y, 0)), Correction.Load(int3(j, 0)), f.x);
    return lerp(a, b, f.y);
}

void SampleBilinearLowPair(float2 p, out float4 baseline, out float4 residual)
{
    float2 q = LowPosition(p);
    q = clamp(q, float2(0.0, 0.0), float2(LowSize) - 1.0);
    int2 i = int2(floor(q));
    float2 f = frac(q);
    int2 j = min(i + 1, int2(LowSize) - 1);

    float4 baseA = lerp(LowBaseline.Load(int3(i, 0)), LowBaseline.Load(int3(j.x, i.y, 0)), f.x);
    float4 baseB = lerp(LowBaseline.Load(int3(i.x, j.y, 0)), LowBaseline.Load(int3(j, 0)), f.x);
    baseline = lerp(baseA, baseB, f.y);

    float4 residualA = lerp(Correction.Load(int3(i, 0)), Correction.Load(int3(j.x, i.y, 0)), f.x);
    float4 residualB = lerp(Correction.Load(int3(i.x, j.y, 0)), Correction.Load(int3(j, 0)), f.x);
    residual = lerp(residualA, residualB, f.y);
}

float3 LoadSourceFootprintAverage(int2 cell)
{
    float3 sum = 0.0;
    float count = 0.0;
    [unroll] for (uint fy = 0; fy < 3; ++fy) {
        [unroll] for (uint fx = 0; fx < 3; ++fx) {
            if (fy < SourceScale && fx < SourceScale) {
                uint2 sourcePixel = min(uint2(cell) * max(SourceScale, 1u) + uint2(fx, fy),
                                        HighSize - 1u);
                sum += BaseColor.Load(int3(BaseSource + sourcePixel, 0)).rgb;
                count += 1.0;
            }
        }
    }
    return sum / max(count, 1.0);
}

float LoadSourceFootprintCoverage(int2 cell, float3 highChroma)
{
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    float coverage = 0.0;
    float count = 0.0;
    [unroll] for (uint fy = 0; fy < 3; ++fy) {
        [unroll] for (uint fx = 0; fx < 3; ++fx) {
            if (fy < SourceScale && fx < SourceScale) {
                uint2 sourcePixel = min(uint2(cell) * max(SourceScale, 1u) + uint2(fx, fy),
                                        HighSize - 1u);
                float3 sourceColor = max(BaseColor.Load(int3(BaseSource + sourcePixel, 0)).rgb, 0.0);
                float sourceLuminance = max(dot(sourceColor, luma), 1e-3);
                float3 sourceChroma = sourceColor / sourceLuminance;
                float chromaError = dot(abs(highChroma - sourceChroma), luma);
                coverage += 1.0 - smoothstep(0.06, 0.28, chromaError);
                count += 1.0;
            }
        }
    }
    return coverage / max(count, 1.0);
}

 )" R"(
// Solve a symmetric positive definite 3x3 covariance using LDL^T.
// The ridge regularizes unobserved colour directions, not the mean change.
float3 SolveAppearance(float3 diagonal, float3 off, float3 v, float ridge)
{
    float d0 = max(diagonal.x, ridge);
    float l10 = off.x / d0;
    float l20 = off.y / d0;
    float d1 = max(diagonal.y - l10 * off.x, ridge);
    float l21 = (off.z - l20 * off.x) / d1;
    float d2 = max(diagonal.z - l20 * off.y - l21 * l21 * d1, ridge);
    float3 y;
    y.x = v.x;
    y.y = v.y - l10 * y.x;
    y.z = v.z - l20 * y.x - l21 * y.y;
    float3 z = y / float3(d0, d1, d2);
    float3 result;
    result.z = z.z;
    result.y = z.y - l21 * result.z;
    result.x = z.x - l10 * result.y - l20 * result.z;
    return result;
}

#if DLSSNR_DISPLAY_COMPOSITE
// Match the input adapter's configurable paper white in 80-nit scRGB units.
// Match the display restoration's shoulder, never a scene-radiance gain.
float DisplayCompositeConfidence(float3 color)
{
    float peak = max(max(color.r, color.g), color.b) / max(DisplayPaperWhite, 0.001);
    float shoulder = peak <= 0.75 ? peak : 0.75 + 0.25 * (peak - 0.75) / (peak - 0.5);
    return peak <= 0.75 ? 1.0 : smoothstep(0.1, 0.5, shoulder / max(peak, 1e-6));
}
#endif

float3 SampleFeatureGuidedReconstruction(float2 p, float3 highColor, int2 cacheOrigin, uint2 cacheSize)
{
#if DLSSNR_DISPLAY_COMPOSITE
    float3 high = highColor;
#else
    float3 high = max(highColor, 0.0);
#endif
    float4 anchorSample, anchorResidual;
    SampleBilinearLowPair(p, anchorSample, anchorResidual);
#if DLSSNR_DISPLAY_COMPOSITE
    float3 anchor = anchorSample.rgb;
#else
    float3 anchor = max(anchorSample.rgb, 0.0);
#endif
    float3 deltaAnchor = anchorResidual.rgb;
#if DLSSNR_DISPLAY_COMPOSITE
    float3 magnitude = max(abs(high), abs(anchor));
#else
    float3 magnitude = max(high, anchor);
#endif
    float unit = max(max(magnitude.r, magnitude.g), max(magnitude.b, 0.01));
    float inverseUnit = rcp(unit);
    // Centre before accumulating moments. E[x*x]-E[x]*E[x] on uncentred
    // nearly flat HDR colours can lose positive definiteness in FP32.
    float3 target = (high - anchor) * inverseUnit;
    float2 q = clamp(LowPosition(p), 0.0, float2(LowSize) - 1.0);
    int2 center = int2(floor(q + 0.5));
    float weightSum = 0.0;
    float3 sourceSum = 0.0, deltaSum = 0.0;
    float3 squareSum = 0.0, productSum = 0.0;
    float3 crossR = 0.0, crossG = 0.0, crossB = 0.0;
    float detailWeightSum = 0.0;
    float3 detailSourceSum = 0.0, detailDeltaSum = 0.0;
#if DLSSNR_DISPLAY_COMPOSITE
    float3 observedMinimum = 65504.0, observedMaximum = -65504.0;
    float displayConfidence = DisplayHDR != 0u ? DisplayCompositeConfidence(high) : 1.0;
#endif
    float highMagnitude = length(high);
    float2 horizontalWeight[FeatureTaps];
    int horizontalCell[FeatureTaps];
    [unroll] for (int ix = 0; ix < FeatureTaps; ++ix) {
        int cx = center.x + ix - FeatureRadius;
        horizontalCell[ix] = clamp(cx, 0, int(LowSize.x) - 1);
        float dx = float(cx) - q.x;
        // Fade finite support to zero before a tap enters/leaves the window.
        // Measure distances before texture clamping so borders are continuous too.
        float window = 1.0 - smoothstep(float(FeatureRadius) - 0.5, float(FeatureRadius) + 0.5, abs(dx));
        horizontalWeight[ix] = exp(-dx * dx / float2(2.0 * 1.5 * 1.5, 2.0 * 0.65 * 0.65)) * window;
    }
    [loop] for (int oy = -FeatureRadius; oy <= FeatureRadius; ++oy) {
        int cy = clamp(center.y + oy, 0, int(LowSize.y) - 1);
        float dy = float(center.y + oy) - q.y;
        float window = 1.0 - smoothstep(float(FeatureRadius) - 0.5, float(FeatureRadius) + 0.5, abs(dy));
        float2 verticalWeight = exp(-dy * dy / float2(2.0 * 1.5 * 1.5, 2.0 * 0.65 * 0.65)) * window;
        [unroll] for (int ox = -FeatureRadius; ox <= FeatureRadius; ++ox) {
            int2 cell = int2(horizontalCell[ox + FeatureRadius], cy);
            int2 local = cell - cacheOrigin;
            float4 sample;
            float3 delta;
            float sourceMagnitude;
            if (all(local >= 0) && all(local < int2(cacheSize))) {
                sample = FeatureSourceCache[local.y * FeatureCacheWidth + local.x];
                float4 cachedDelta = FeatureDeltaCache[local.y * FeatureCacheWidth + local.x];
                delta = cachedDelta.rgb;
                sourceMagnitude = cachedDelta.a;
            } else {
                LoadFeatureSample(cell, sample, delta);
                sourceMagnitude = length(sample.rgb);
            }
#if DLSSNR_DISPLAY_COMPOSITE
            if (DisplayHDR != 0u)
            {
                // A tiny highlight may disappear into a low-resolution cell.
                // Its destination confidence must still protect the actual
                // high pixel. Never undo attenuation already present at low res.
                float sampleConfidence = DisplayCompositeConfidence(sample.rgb);
                delta *= min(1.0, displayConfidence / max(sampleConfidence, 1e-6));
                if (sampleConfidence <= 1e-6) delta = 0.0;
                observedMinimum = min(observedMinimum, delta);
                observedMaximum = max(observedMaximum, delta);
            }
#endif
            float3 source = (sample.rgb - anchor) * inverseUnit;
            delta = (delta - deltaAnchor) * inverseUnit;
            // Full RGB distance distinguishes neutral brightness levels too.
            // A soft nonzero kernel lets mixed cells constrain slopes when no
            // pure sample of a subpixel object exists. Never fade the result
            // towards identity according to the fraction of matching neighbours.
            float3 difference = sample.rgb - high;
            float range = 0.04 + 0.25 * (sourceMagnitude + highMagnitude);
            float rangeSquared = range * range;
            float affinity = rangeSquared / (rangeSquared + dot(difference, difference));
#if DLSSNR_FAST_COMPOSITE
            float guide = affinity * affinity;
#else
            float guide = affinity * affinity * sample.a;
#endif
            float2 spatial = horizontalWeight[ox + FeatureRadius] * verticalWeight;
            float weight = spatial.x * guide;
            float3 weightedSource = source * weight;
            weightSum += weight;
            sourceSum += weightedSource;
            deltaSum += delta * weight;
            squareSum += source * weightedSource;
            productSum += source.xxy * weightedSource.yzz;
            crossR += weightedSource * delta.r;
            crossG += weightedSource * delta.g;
            crossB += weightedSource * delta.b;
            float detailWeight = spatial.y * guide;
            detailWeightSum += detailWeight;
            detailSourceSum += source * detailWeight;
            detailDeltaSum += delta * detailWeight;
        }
    }
    float invWeight = rcp(max(weightSum, 1e-20));
    float3 meanSource = sourceSum * invWeight;
    float3 meanDelta = deltaSum * invWeight;
    float3 variance = max(squareSum * invWeight - meanSource * meanSource, 0.0);
    float3 covariance = productSum * invWeight - meanSource.xxy * meanSource.yzz;
    crossR = crossR * invWeight - meanSource * meanDelta.r;
    crossG = crossG * invWeight - meanSource * meanDelta.g;
    crossB = crossB * invWeight - meanSource * meanDelta.b;

    // In a flat/rank-deficient neighbourhood, many transforms explain the
    // same low image. Prefer bounded relative detail transport on observed
    // source channels. The fitted intercept still carries new light on black.
    float3 prior = clamp((meanDelta + deltaAnchor / unit) /
                         max(meanSource + anchor / unit, 1e-6 / unit), -1.0, 3.0);
#if DLSSNR_DISPLAY_COMPOSITE
    // Display restoration carries bounded additive edits. In an unobserved
    // colour direction, brightness-proportional relighting invents a gain that
    // was never produced by the model. Keep the fitted slopes/intercept where
    // observed, with an additive prior in ambiguous directions only.
    if (DisplayHDR != 0u) prior = 0.0;
#endif
    // Near-flat low images do not identify how independent illumination
    // should modulate unresolved source texture. Keep that direction near
    // the relative prior instead of fitting huge slopes to sampling noise.
    float varianceEnergy = dot(variance, 1.0);
    float3 meanColor = meanSource + anchor / unit;
    float relativeVariance = varianceEnergy / max(dot(meanColor, meanColor), 1e-8);
    float flatAmbiguity = 1.0 - smoothstep(1e-4, 0.01, relativeVariance);
    float ridge = max(varianceEnergy * 1e-3, 1e-7) + 1e-3 * flatAmbiguity;
    float3 diagonal = variance + ridge;
    // Solve only the departure from the prior. This avoids subtracting large
    // nearly equal terms after inversion for neutral/rank-one colour data.
    crossR -= prior.r * float3(variance.x, covariance.x, covariance.y);
    crossG -= prior.g * float3(covariance.x, variance.y, covariance.z);
    crossB -= prior.b * float3(covariance.y, covariance.z, variance.z);

    // Reconstruct unexplained model detail from its own normalized narrow
    // support, after subtracting the SAME affine prediction. Thus a source
    // edge explained by relighting cannot return as an additive blurred edge.
    float detailInvWeight = rcp(max(detailWeightSum, 1e-20));
    float3 detailSource = detailSourceSum * detailInvWeight;
    float3 detailDelta = detailDeltaSum * detailInvWeight;
    const float detailStrength = 0.5;
    float3 v = target - meanSource + detailStrength * (meanSource - detailSource);
    float3 solved = SolveAppearance(diagonal, covariance, v, ridge);
    float3 correction = deltaAnchor / unit + meanDelta + detailStrength * (detailDelta - meanDelta) + prior * v +
                        float3(dot(crossR, solved), dot(crossG, solved), dot(crossB, solved));
#if DLSSNR_DISPLAY_COMPOSITE
    float3 appliedCorrection = correction * unit;
    if (DisplayHDR != 0u)
        appliedCorrection = clamp(appliedCorrection, observedMinimum, observedMaximum);
    float3 result = high + appliedCorrection;
#else
    float3 result = high + correction * unit;
#endif
#if DLSSNR_DISPLAY_COMPOSITE
    return all(isfinite(result)) ? clamp(result, -65504.0, 65504.0) : high;
#else
    return all(isfinite(result)) ? clamp(result, 0.0, 65504.0) : clamp(high, 0.0, 65504.0);
#endif
}

 )" R"(
float4 SampleEnergyPreservingResidual(float2 p, float3 highColor, out float confidence)
{
    float2 q = LowPosition(p);
    q = clamp(q, float2(0.0, 0.0), float2(LowSize) - 1.0);
    int2 i = int2(floor(q));
    float2 f = frac(q);
    int2 j = min(i + 1, int2(LowSize) - 1);
    int2 points[4] = { i, int2(j.x, i.y), int2(i.x, j.y), j };
    float spatial[4] = { (1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y),
                         (1.0 - f.x) * f.y, f.x * f.y };
    float4 baseline = 0.0;
    float3 signedResidual = 0.0;
    float3 residualEnergy = 0.0;
    [unroll] for (uint k = 0; k < 4; ++k) {
        float weight = spatial[k];
        float4 lowBase = LowBaseline.Load(int3(points[k], 0));
        float3 residual = Correction.Load(int3(points[k], 0)).rgb;
        baseline += lowBase * weight;
        signedResidual += residual * weight;
        residualEnergy += abs(residual) * weight;
    }
    confidence = GuideConfidence(highColor, baseline.rgb);

    int2 nearest = clamp(int2(round(q)), int2(0, 0), int2(LowSize) - 1);
    float3 nearestResidual = Correction.Load(int3(nearest, 0)).rgb;
    float3 mixed = lerp(nearestResidual, signedResidual, confidence);
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    float targetEnergy = dot(residualEnergy, luma);
    if (targetEnergy <= 1e-5)
        targetEnergy = dot(abs(nearestResidual), luma);
    float currentEnergy = dot(abs(mixed), luma);
    if (currentEnergy <= 1e-5)
        mixed = dot(abs(nearestResidual), luma) > 1e-5 ? nearestResidual : signedResidual;
    currentEnergy = max(dot(abs(mixed), luma), 1e-5);
    float3 preserved = mixed * (targetEnergy / currentEnergy);
    float nearestAlpha = Correction.Load(int3(nearest, 0)).a;
    return float4(preserved, lerp(nearestAlpha, SampleBilinearResidual(p).a, confidence));
}

float4 SampleHighResolutionGuidedResidual(float2 p, float3 highColor)
{
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    const float epsilon = 1e-3;

    float2 q = clamp(LowPosition(p), float2(0.0, 0.0), float2(LowSize) - 1.0);
    float highLuminance = max(dot(highColor, luma), epsilon);
    float3 highChroma = highColor / highLuminance;

    float gainLog = 0.0;
    float3 chromaLog = 0.0;
    float gainSecondMoment = 0.0;
    float chromaSecondMoment = 0.0;
    float luminanceWeight = 0.0;
    float chromaWeight = 0.0;
    float chromaSupport = 0.0;
    float chromaSupportWeight = 0.0;
    float spatialTotal = 0.0;
    float coverageWeight = 0.0;
    const int radius = 4;
    const float sigma = 1.85;
    int2 center = int2(floor(q));
    [loop] for (int oy = -radius; oy <= radius; ++oy) {
        [loop] for (int ox = -radius; ox <= radius; ++ox) {
            int2 cell = clamp(center + int2(ox, oy), int2(0, 0), int2(LowSize) - 1);
            float2 distance = ((float2(cell) + 0.5) - (q + 0.5)) / sigma;
            float spatialWeight = exp(-0.5 * dot(distance, distance));

            float3 lowBase = LowBaseline.Load(int3(cell, 0)).rgb;
            float3 modelOutput = lowBase + Correction.Load(int3(cell, 0)).rgb;
            lowBase = max(lowBase, 0.0);
            modelOutput = max(modelOutput, 0.0);
            float baseLuminance = max(dot(lowBase, luma), epsilon);
            float modelLuminance = max(dot(modelOutput, luma), epsilon);
            float3 lowChroma = lowBase / baseLuminance;
            float3 modelChroma = modelOutput / modelLuminance;

            float3 sourceCoverage = LoadSourceFootprintAverage(cell);
            float sourceLuminance = max(dot(sourceCoverage, luma), epsilon);
            float3 sourceChroma = sourceCoverage / sourceLuminance;
            float coverageError = dot(abs(highChroma - sourceChroma), luma);
            float averageCoverage = 1.0 - smoothstep(0.06, 0.28, coverageError);
            float sampleCoverage = LoadSourceFootprintCoverage(cell, highChroma);
            float coverage = 0.5 * averageCoverage + 0.5 * sampleCoverage;

            float baseChromaError = dot(abs(highChroma - lowChroma), luma);
            float modelChromaError = dot(abs(highChroma - modelChroma), luma);
            float baseMatch = 1.0 - smoothstep(0.04, 0.32, baseChromaError);
            float modelMatch = 1.0 - smoothstep(0.04, 0.32, modelChromaError);
            float semanticSupport = saturate(baseMatch * (1.0 - modelMatch));

            float lumWeight = spatialWeight * lerp(0.72, 1.0, coverage);
            float colourWeight = spatialWeight * pow(saturate(coverage), 1.5) * semanticSupport;
            float3 candidateRgbLog = clamp(log2(max(modelOutput, epsilon) / max(lowBase, epsilon)),
                                           -3.0, 3.0);
            float candidateGain = dot(candidateRgbLog, luma);
            float3 candidateChromaLog = candidateRgbLog - candidateGain.xxx;
            gainLog += candidateGain * lumWeight;
            chromaLog += candidateChromaLog * colourWeight;
            gainSecondMoment += candidateGain * candidateGain * lumWeight;
            chromaSecondMoment += dot(candidateChromaLog, candidateChromaLog) * colourWeight;
            luminanceWeight += lumWeight;
            chromaWeight += colourWeight;
            float candidateChromaMagnitude = dot(abs(candidateChromaLog), luma);
            float supportWeight = spatialWeight * saturate(candidateChromaMagnitude * 8.0);
            chromaSupport += semanticSupport * supportWeight;
            chromaSupportWeight += supportWeight;
            spatialTotal += spatialWeight;
            coverageWeight += spatialWeight * coverage;
        }
    }
    gainLog /= max(luminanceWeight, epsilon);
    chromaLog = chromaWeight > epsilon ? chromaLog / chromaWeight : 0.0;
    gainLog = clamp(gainLog, -2.5, 2.5);
    chromaLog = clamp(chromaLog, -2.5, 2.5);
    float gainVariance = max(gainSecondMoment / max(luminanceWeight, epsilon) -
                             gainLog * gainLog, 0.0);
    float gainSmoothness = 1.0 - smoothstep(0.015, 0.12, sqrt(gainVariance));
    float chromaVariance = max(chromaSecondMoment / max(chromaWeight, epsilon) -
                               dot(chromaLog, chromaLog), 0.0);
    float chromaSmoothness = 1.0 - smoothstep(0.02, 0.16, sqrt(chromaVariance));
    float chromaCompatibility = chromaSupportWeight > epsilon
        ? saturate(chromaSupport / chromaSupportWeight)
        : 1.0;
    float coverageSupport = coverageWeight / max(spatialTotal, epsilon);
    float luminanceExplanation = smoothstep(0.08, 0.62, coverageSupport);
    gainLog *= lerp(0.55, 1.0, luminanceExplanation);
    float edgeSignal = 0.0;
    int2 highPixel = int2(p);
    [unroll] for (int oy = -1; oy <= 1; ++oy) [unroll] for (int ox = -1; ox <= 1; ++ox) {
        if (ox == 0 && oy == 0)
            continue;
        int2 neighbour = clamp(highPixel + int2(ox, oy), int2(0, 0), int2(HighSize) - 1);
        float3 neighbourColor = BaseColor.Load(int3(BaseSource + uint2(neighbour), 0)).rgb;
        edgeSignal += dot(abs(neighbourColor - highColor), luma) /
                      max(highLuminance, 0.25);
    }
    float localEdgeWeight = smoothstep(0.10, 0.55, edgeSignal / 8.0);
    float3 bilinearBaseline = SampleBilinearBaseline(p).rgb;
    float baselineLuminance = max(dot(bilinearBaseline, luma), 0.25);
    float lossLuminance = dot(abs(highColor - bilinearBaseline), luma) / baselineLuminance;
    float3 baselineChroma = bilinearBaseline / baselineLuminance;
    float lossChroma = dot(abs(highChroma - baselineChroma), luma);
    float fieldEdgeWeight = saturate(max(localEdgeWeight, smoothstep(0.10, 0.30, lossChroma)));
    float positiveGain = max(gainLog, 0.0);
    gainLog -= positiveGain * saturate(localEdgeWeight * 0.72);
    float luminanceFieldWeight = saturate(0.88 - 0.48 * localEdgeWeight) * gainSmoothness;
    float3 multiplicativeLuminance = max(highColor * exp2(gainLog), 0.0);
    float3 multiplicativeChroma = max(highColor * exp2(gainLog.xxx + chromaLog), 0.0);
    float edgeChromaNeed = smoothstep(0.08, 0.35, lossChroma);
    float chromaGate = lerp(1.0, chromaCompatibility, edgeChromaNeed);
    float chromaFieldWeight = saturate(0.70 + 0.30 * fieldEdgeWeight) * chromaSmoothness * chromaGate;

    float3 modelField = lerp(highColor, multiplicativeLuminance,
                             saturate(luminanceFieldWeight));
    modelField = lerp(modelField, multiplicativeChroma,
                      saturate(chromaFieldWeight));
    float3 residual = SampleBilinearResidual(p).rgb;
    float3 lowModelOutput = bilinearBaseline + residual;
    float3 lowMultiplicativeLuminance = max(bilinearBaseline * exp2(gainLog), 0.0);
    float3 lowMultiplicativeChroma = max(bilinearBaseline * exp2(gainLog.xxx + chromaLog), 0.0);
    float3 fieldAtLowBaseline = lerp(bilinearBaseline,
                                     lowMultiplicativeLuminance,
                                     saturate(luminanceFieldWeight));
    fieldAtLowBaseline = lerp(fieldAtLowBaseline,
                              lowMultiplicativeChroma,
                              saturate(chromaFieldWeight));
    float3 detailResidual = lowModelOutput - fieldAtLowBaseline;
    float detailLuminance = dot(detailResidual, luma);
    float3 neutralResidual = detailLuminance.xxx;
    float3 chromaResidual = detailResidual - neutralResidual;
    float detailWeight = saturate((1.0 - fieldEdgeWeight) * 0.24 +
                                  (1.0 - gainSmoothness) * 0.12);
    float neutralResidualGate = lerp(0.15, 1.0, luminanceExplanation);
    float chromaResidualGate = chromaGate * smoothstep(0.18, 0.68, coverageSupport);
    float3 guidedResidual = neutralResidual * neutralResidualGate +
                            chromaResidual * chromaResidualGate;
    float3 result = modelField + guidedResidual * detailWeight;
    float3 correction = result - highColor;
    correction = all(correction == correction) ? clamp(correction, -65504.0, 65504.0) : 0.0;
    return float4(correction, 0.0);
}
 )" R"(

float4 SampleSoftResidual(float2 p, float3 highColor, out float confidence)
{
    return SampleEnergyPreservingResidual(p, highColor, confidence);
}
float4 SampleSharpResidual(float2 p, float3 highColor, out float confidence)
{
    return SampleEnergyPreservingResidual(p, highColor, confidence);
}
[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID,
                                 uint3 groupId : SV_GroupID,
                                 uint3 groupThreadId : SV_GroupThreadID) {
    int2 cacheOrigin = 0;
    uint2 cacheSize = 0;
    if (FeatureGuided != 0 && CompositeMode != 1 && CompositeMode != 2) {
        uint2 firstHigh = groupId.xy * 8u;
        uint2 lastHigh = min(firstHigh + 7u, HighSize - 1u);
        float2 firstQ = clamp(LowPosition(float2(firstHigh)), float2(0.0, 0.0),
                              float2(LowSize) - 1.0);
        float2 lastQ = clamp(LowPosition(float2(lastHigh)), float2(0.0, 0.0),
                             float2(LowSize) - 1.0);
        cacheOrigin = max(min(int2(floor(firstQ + 0.5)), int2(floor(lastQ + 0.5))) - FeatureRadius, 0);
        int2 cacheEnd = min(max(int2(floor(firstQ + 0.5)), int2(floor(lastQ + 0.5))) + FeatureRadius, int2(LowSize) - 1);
        cacheSize = uint2(min(cacheEnd - cacheOrigin + 1, FeatureCacheWidth));
        uint threadIndex = groupThreadId.y * 8u + groupThreadId.x;
        // Load only unique, in-bounds cells. Even inactive edge threads must
        // populate the tile and reach the barrier before the bounds return.
        [loop] for (uint index = threadIndex; index < cacheSize.x * cacheSize.y; index += 64u) {
            uint2 local = uint2(index % cacheSize.x, index / cacheSize.x);
            float4 source;
            float3 delta;
            LoadFeatureSample(cacheOrigin + int2(local), source, delta);
            FeatureSourceCache[local.y * FeatureCacheWidth + local.x] = source;
            FeatureDeltaCache[local.y * FeatureCacheWidth + local.x] = float4(delta, length(source.rgb));
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (any(id.xy >= HighSize)) return;
    float4 base = BaseColor.Load(int3(BaseSource + id.xy,0));
    float4 result;
    if (CompositeMode == 2) {
        result = LowSize.x == HighSize.x && LowSize.y == HighSize.y
                   ? Correction.Load(int3(id.xy, 0))
                   : SampleBilinearResidual(float2(id.xy));
    } else if (CompositeMode == 1) {
        float3 reference = SampleBilinearBaseline(float2(id.xy)).rgb;
        float3 delta = Correction.Load(int3(id.xy, 0)).rgb - reference;
        delta = all(delta == delta) ? clamp(delta, -65504.0, 65504.0) : 0.0;
        result = float4(base.rgb + delta, base.a);
#if DLSSNR_DISPLAY_COMPOSITE
        // Display-HDR inverse already restored this full-resolution model
        // against BaseColor. Adding base - bilinear(lowBaseline) again would
        // duplicate the source detail even for a model with no edit.
        if (DisplayHDR != 0u) result.rgb = Correction.Load(int3(id.xy, 0)).rgb;
#endif
    } else {
        float4 correction = 0.0;
        if (FeatureGuided != 0) {
            correction.rgb = SampleFeatureGuidedReconstruction(float2(id.xy), base.rgb, cacheOrigin, cacheSize) - base.rgb;
        } else if (HighResolutionGuided != 0) {
            correction = SampleHighResolutionGuidedResidual(float2(id.xy), base.rgb);
        } else if (TemporalReconstruction != 0) {
            float softConfidence = 0.0;
            float sharpConfidence = 0.0;
            float4 soft = SampleSoftResidual(float2(id.xy), base.rgb, softConfidence);
            float4 sharp = SampleSharpResidual(float2(id.xy), base.rgb, sharpConfidence);
            float confidence = max(softConfidence, sharpConfidence);
            correction = lerp(soft, sharp, smoothstep(0.25, 0.75, confidence));
        } else {
            float confidence = 0.0;
            correction = SampleEnergyPreservingResidual(float2(id.xy), base.rgb, confidence);
        }
        result = float4(base.rgb + correction.rgb, base.a);
    }
    // Bound display edits using the same gamut projection as RestoreSDR.
    // A zero/negative source channel is not evidence that an edit is invalid.
    if (DisplayHDR != 0u && CompositeMode == 0u)
    {
#if !DLSSNR_DISPLAY_COMPOSITE
        float low = min(min(base.r, base.g), base.b);
        float peak = max(max(base.r, base.g), max(base.b, 1e-6));
        float gamutConfidence = smoothstep(0.0, 0.02, low / peak);
#endif
#if DLSSNR_DISPLAY_COMPOSITE
        // Restoration bounds belong to the destination pixel, not only the
        // downsampled source. Applies to the legacy residual variants too.
        float white = max(DisplayPaperWhite, 0.001);
        float3 reference = base.rgb / white;
        float low = min(min(reference.r, reference.g), reference.b);
        if (low < 0.0) {
            float luminance = max(dot(reference, float3(0.2126, 0.7152, 0.0722)), 0.0);
            reference = lerp(luminance.xxx, reference, luminance / max(luminance - low, 1e-6));
        }
        reference = max(reference, 0.0);
        float relativePeak = max(max(reference.r, reference.g), reference.b);
        float shoulder = relativePeak <= 0.75 ? relativePeak :
            0.75 + 0.25 * (relativePeak - 0.75) / (relativePeak - 0.5);
        reference *= relativePeak <= 0.75 ? 1.0 : shoulder / relativePeak;
        float bound = white * DisplayCompositeConfidence(base.rgb);
        float3 edit = result.rgb - base.rgb;
        edit = all(isfinite(edit)) ? clamp(edit, -reference * bound, (1.0 - reference) * bound) : 0.0;
        result.rgb = clamp(base.rgb + edit, -65504.0, 65504.0);
#else
        result.rgb = lerp(base.rgb, result.rgb, gamutConfidence);
#endif
    }
    Output[DestinationBase + id.xy] = float4(result.rgb, base.a);
}
)";

std::atomic<void*> g_callerHookOwner = nullptr;
std::atomic<HMODULE> g_callerModule = nullptr;
using GetModuleFileNameWFn = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);
std::atomic<GetModuleFileNameWFn> g_originalGetModuleFileNameW = nullptr;
std::atomic<bool> g_originalGuidesObserved = false;
std::atomic<uint64_t> g_originalMotionLogical = 0;
std::atomic<uint64_t> g_originalMotionAllocation = 0;
std::atomic<uint64_t> g_originalMotionBase = 0;
std::atomic<uint64_t> g_originalDepthLogical = 0;
std::atomic<uint64_t> g_originalDepthAllocation = 0;
std::atomic<uint64_t> g_originalDepthBase = 0;
std::mutex g_callerHookMutex;
size_t g_callerHookUsers = 0;
HMODULE g_callerHookTargetModule = nullptr;
void** g_callerHookIatSlot = nullptr;
GetModuleFileNameWFn g_callerHookOriginal = nullptr;

constexpr uint64_t PackDimensions(unsigned int x, unsigned int y)
{
    return static_cast<uint64_t>(x) | (static_cast<uint64_t>(y) << 32);
}

void UnpackDimensions(uint64_t packed, unsigned int& x, unsigned int& y)
{
    x = static_cast<unsigned int>(packed);
    y = static_cast<unsigned int>(packed >> 32);
}

DWORD WINAPI CallerCompatibleGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) noexcept
{
    if (module == g_callerModule.load(std::memory_order_acquire))
    {
        constexpr wchar_t authorized[] = L"nvngx.dll";
        constexpr DWORD length = ARRAYSIZE(authorized) - 1;
        if (filename == nullptr || size == 0)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (size <= length)
        {
            if (size > 1)
                std::memcpy(filename, authorized, (size - 1) * sizeof(wchar_t));
            filename[size - 1] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        std::memcpy(filename, authorized, sizeof(authorized));
        return length;
    }
    auto original = g_originalGetModuleFileNameW.load(std::memory_order_acquire);
    return original ? original(module, filename, size) : 0;
}

void** FindImportedFunctionSlot(HMODULE module, const char* functionName) noexcept
{
    if (!module || !functionName) return nullptr;
    auto* base = reinterpret_cast<std::byte*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size || dir.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
        dir.Size > nt->OptionalHeader.SizeOfImage || dir.VirtualAddress > nt->OptionalHeader.SizeOfImage - dir.Size) return nullptr;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    auto* end = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress + dir.Size);
    for (; descriptor < end && descriptor->Name; ++descriptor)
    {
        if (descriptor->Name >= nt->OptionalHeader.SizeOfImage) continue;
        const char* library = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(library, "KERNEL32.dll") != 0 && _stricmp(library, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
            _stricmp(library, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) continue;
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), functionName) == 0)
                return reinterpret_cast<void**>(&addresses->u1.Function);
        }
    }
    return nullptr;
}

template <typename T> T GetParameter(NVSDK_NGX_Parameter* parameters, const char* key, T fallback)
{
    T value = fallback;
    if (parameters != nullptr)
        parameters->Get(key, &value);
    return value;
}

// The signed NR snippet is called with the driver's native NGX parameter table.
// That table's resource and (on some driver builds) float setters do not match
// the overload slots advertised by the public header.  Use the slots verified by
// the working standalone forwarder: ULL/resource=0 and uint=3.  Float is
// discovered once by a write/read round trip on the live table.
using NgxSetUllFn = void(__thiscall*)(void*, const char*, unsigned long long);
using NgxSetFloatFn = void(__thiscall*)(void*, const char*, float);
using NgxSetUIntFn = void(__thiscall*)(void*, const char*, unsigned int);

void SetRawUll(NVSDK_NGX_Parameter* parameters, const char* key, unsigned long long value)
{
    if (parameters == nullptr || key == nullptr)
        return;
    auto** vtable = *reinterpret_cast<void***>(parameters);
    reinterpret_cast<NgxSetUllFn>(vtable[0])(parameters, key, value);
}

void SetRawUInt(NVSDK_NGX_Parameter* parameters, const char* key, unsigned int value)
{
    if (parameters == nullptr || key == nullptr)
        return;
    auto** vtable = *reinterpret_cast<void***>(parameters);
    reinterpret_cast<NgxSetUIntFn>(vtable[3])(parameters, key, value);
}

int DiscoverRawFloatSlot(NVSDK_NGX_Parameter* parameters)
{
    if (parameters == nullptr)
        return -1;
    auto** vtable = *reinterpret_cast<void***>(parameters);
    constexpr int candidates[] = {1, 2, 5, 6, 7, 4, 3, 0};
    constexpr float expected = 0.375f;
    for (const int slot : candidates)
    {
        reinterpret_cast<NgxSetFloatFn>(vtable[slot])(parameters,
                                                       "DLSSNR.OptiScalerFloatProbe", expected);
        float readBack = 0.0f;
        if (parameters->Get("DLSSNR.OptiScalerFloatProbe", &readBack) == NVSDK_NGX_Result_Success &&
            readBack == expected)
            return slot;
    }
    return -1;
}

void SetRawFloat(NVSDK_NGX_Parameter* parameters, const char* key, float value, int slot)
{
    if (parameters == nullptr || key == nullptr)
        return;
    if (slot >= 0)
    {
        auto** vtable = *reinterpret_cast<void***>(parameters);
        reinterpret_cast<NgxSetFloatFn>(vtable[slot])(parameters, key, value);
    }
    else
        parameters->Set(key, value);
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* parameters, const char* key)
{
    ID3D12Resource* value = nullptr;
    if (parameters != nullptr && parameters->Get(key, &value) != NVSDK_NGX_Result_Success)
        parameters->Get(key, reinterpret_cast<void**>(&value));
    return value;
}

void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (commandList == nullptr || resource == nullptr || before == after)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commandList->ResourceBarrier(1, &barrier);
}

DXGI_FORMAT ShaderReadableFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return format;
    }
}

bool IsTypelessDepthFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16_TYPELESS || format == DXGI_FORMAT_R32_TYPELESS ||
           format == DXGI_FORMAT_R24G8_TYPELESS || format == DXGI_FORMAT_R32G8X24_TYPELESS;
}

bool IsShaderReadableTexture(const D3D12_RESOURCE_DESC& desc)
{
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 &&
           desc.MipLevels >= 1 && (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
}

bool IsExposureTexture(const D3D12_RESOURCE_DESC& desc)
{
    // The NGX exposure contract is a single 2D texture. CreateSrv emits a
    // non-array TEXTURE2D view, so reject array resources before attempting to
    // bind one as the live exposure source.
    return IsShaderReadableTexture(desc) && desc.DepthOrArraySize == 1 && desc.Width > 0 && desc.Height > 0;
}

bool PrepareExposureRead(ID3D12GraphicsCommandList* commandList, ID3D12Resource* exposure,
                        bool useExposure, D3D12_RESOURCE_STATES* restoreState)
{
    if (restoreState != nullptr)
        *restoreState = D3D12_RESOURCE_STATE_COMMON;
    if (!useExposure || commandList == nullptr || exposure == nullptr || restoreState == nullptr)
        return false;

    const auto configuredState = Config::Instance()->ExposureResourceBarrier;
    if (!configuredState.has_value())
        return false;

    *restoreState = static_cast<D3D12_RESOURCE_STATES>(configuredState.value());
    if (*restoreState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        return false;

    Transition(commandList, exposure, *restoreState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}

void RestoreExposureState(ID3D12GraphicsCommandList* commandList, ID3D12Resource* exposure,
                          bool transitioned, D3D12_RESOURCE_STATES restoreState)
{
    if (transitioned)
        Transition(commandList, exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, restoreState);
}

bool CreateComputePipeline(ID3D12Device* device, UINT srvCount, UINT uavCount, const char* shaderSource,
                            Microsoft::WRL::ComPtr<ID3D12RootSignature>& rootSignature,
                            Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipelineState,
                            bool linearSampler = false, UINT constantCount = kGuidanceConstantCount,
                            D3D12_SHADER_BYTECODE bytecode = {})
{
    CD3DX12_DESCRIPTOR_RANGE1 ranges[2] {};
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srvCount, 0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, uavCount, 0);
    CD3DX12_ROOT_PARAMETER1 rootParameters[2] {};
    rootParameters[0].InitAsDescriptorTable(_countof(ranges), ranges);
    rootParameters[1].InitAsConstants(constantCount, 0);
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.Init_1_1(_countof(rootParameters), rootParameters,
                      linearSampler ? 1u : 0u, linearSampler ? &sampler : nullptr);

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT result = D3D12SerializeVersionedRootSignature(&rootDesc, &signature, &errors);
    if (FAILED(result))
    {
        LOG_ERROR("[DLSSNR_GUIDANCE] root signature serialization failed: {:X} {}",
                  static_cast<unsigned int>(result),
                  errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
    }
    result = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                         IID_PPV_ARGS(&rootSignature));
    if (FAILED(result))
    {
        LOG_ERROR("[DLSSNR_GUIDANCE] root signature creation failed: {:X}", static_cast<unsigned int>(result));
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> shader;
    if (bytecode.pShaderBytecode == nullptr || bytecode.BytecodeLength == 0)
    {
        if (shaderSource == nullptr) return false;
        shader.Attach(CompileShader(shaderSource, "CSMain", "cs_5_0"));
        if (shader == nullptr) return false;
        bytecode = CD3DX12_SHADER_BYTECODE(shader.Get());
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc {};
    pipelineDesc.pRootSignature = rootSignature.Get();
    pipelineDesc.CS = bytecode;
    result = device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipelineState));
    if (FAILED(result))
    {
        LOG_ERROR("[DLSSNR_GUIDANCE] compute pipeline creation failed: {:X}", static_cast<unsigned int>(result));
        return false;
    }
    return true;
}

bool IsSupportedColor(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, std::string& reason)
{
    if (device == nullptr)
    {
        reason = "D3D12 device is null";
        return false;
    }
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
        desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
    {
        reason = "requires a single-sample, single-mip, non-array 2D texture";
        return false;
    }
    if ((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0)
    {
        reason = "resource denies shader-resource views";
        return false;
    }
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
    {
        reason = "resource does not allow unordered-access views";
        return false;
    }
    if (desc.Format == DXGI_FORMAT_UNKNOWN)
    {
        reason = "resource format is unknown";
        return false;
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT support { desc.Format, D3D12_FORMAT_SUPPORT1_NONE,
                                                D3D12_FORMAT_SUPPORT2_NONE };
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
    {
        reason = std::format("D3D12 format support query failed for format 0x{:X}",
                             static_cast<unsigned int>(desc.Format));
        return false;
    }
    if ((support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) == 0 ||
        (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) == 0)
    {
        reason = std::format("format 0x{:X} lacks typed 2D shader-load support",
                             static_cast<unsigned int>(desc.Format));
        return false;
    }
    if ((support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
    {
        reason = std::format("format 0x{:X} lacks typed UAV-store support",
                             static_cast<unsigned int>(desc.Format));
        return false;
    }
    return true;
}

void SetSubrect(NVSDK_NGX_Parameter* parameters, const char* resource, unsigned int width, unsigned int height,
                unsigned int baseX, unsigned int baseY)
{
    const std::string prefix = std::string("DLSSNR.") + resource + "Subrect";
    SetRawUInt(parameters, (prefix + "Width").c_str(), width);
    SetRawUInt(parameters, (prefix + "Height").c_str(), height);
    SetRawUInt(parameters, (prefix + "BaseX").c_str(), baseX);
    SetRawUInt(parameters, (prefix + "BaseY").c_str(), baseY);
}

} // namespace

struct DLSSNRFeatureDx12::GuidanceResources
{
    bool identityColorTransfer = false;
    bool displayHDRColorTransfer = false;
    float displayPaperWhite = 203.0f / 80.0f;
    float ColorTransferStrength(float strength = 1.0f) const
    {
        // Existing constant slot: zero = encoded SDR, negative = display HDR,
        // positive = the established scene-linear transfer.
        return identityColorTransfer ? 0.0f : displayHDRColorTransfer ? -1.0f : strength;
    }
    struct ResampleConstants
    {
        uint32_t outputWidth;
        uint32_t outputHeight;
        uint32_t motionBaseX;
        uint32_t motionBaseY;
        uint32_t motionWidth;
        uint32_t motionHeight;
        uint32_t depthBaseX;
        uint32_t depthBaseY;
        uint32_t depthWidth;
        uint32_t depthHeight;
        float motionScaleX;
        float motionScaleY;
        float motionOriginOffsetX;
        float motionOriginOffsetY;
        uint32_t depthInverted;
        uint32_t padding;
    };

    struct DebugConstants
    {
        uint32_t outputWidth;
        uint32_t outputHeight;
        uint32_t colorBaseX;
        uint32_t colorBaseY;
        uint32_t colorWidth;
        uint32_t colorHeight;
        uint32_t motionBaseX;
        uint32_t motionBaseY;
        uint32_t motionWidth;
        uint32_t motionHeight;
        uint32_t depthBaseX;
        uint32_t depthBaseY;
        uint32_t depthWidth;
        uint32_t depthHeight;
        uint32_t depthInverted;
        uint32_t padding;
    };

    struct ColorClampConstants
    {
        uint32_t outputWidth;
        uint32_t outputHeight;
        uint32_t sourceBaseX;
        uint32_t sourceBaseY;
        uint32_t sourceWidth;
        uint32_t sourceHeight;
        uint32_t destinationBaseX;
        uint32_t destinationBaseY;
        float paperWhite;
        float transferStrength;
        float colorStrength;
        uint32_t mode;
        float exposurePreExposure = 1.0f;
        float exposureScale = 1.0f;
        uint32_t useExposure = 0;
        uint32_t edgeBlendData = 0;
        uint32_t originalBaseX = 0;
        uint32_t originalBaseY = 0;
        uint32_t roiOffsetX = 0;
        uint32_t roiOffsetY = 0;
        uint32_t roiWidth = 0;
        uint32_t roiHeight = 0;
        uint32_t colorClampPadding[2] {};
        float extrapolationDepthScale[2] {};
        float extrapolationDepthBias[2] {};
    };

    struct LowDownsampleConstants
    {
        uint32_t sourceBaseX, sourceBaseY, sourceWidth, sourceHeight;
        uint32_t outputWidth, outputHeight, scale, transfer;
        float paperWhite, transferStrength, colorStrength;
        uint32_t useExposure = 0;
        float preExposure = 1.0f, exposureScale = 1.0f;
        uint32_t padding[2] {};
    };

    struct ExtrapolationConstants
    {
        uint32_t width, height, destinationBaseX, destinationBaseY;
        uint32_t originalBaseX, originalBaseY, roiOffsetX, roiOffsetY;
        uint32_t roiWidth, roiHeight;
        float areaScale;
        uint32_t edgeBlendData;
        uint32_t disableLocal = 0;
        uint32_t padding[2] {};
        uint32_t globalColorMixing = 0;
    };
    static_assert(sizeof(ExtrapolationConstants) == kGuidanceConstantCount * sizeof(uint32_t));
    static_assert(offsetof(ExtrapolationConstants, areaScale) == 10 * sizeof(uint32_t));
    static_assert(offsetof(ExtrapolationConstants, disableLocal) == 12 * sizeof(uint32_t));
    static_assert(offsetof(ExtrapolationConstants, globalColorMixing) == 15 * sizeof(uint32_t));

    struct BoundaryConstants
    {
        uint32_t width, height, destinationX, destinationY;
        uint32_t originalX, originalY, roiX, roiY;
        uint32_t roiWidth, roiHeight, edgeBlendData, outerWidth;
        uint32_t gridWidth, gridHeight, phaseX, phaseY;
        uint32_t step = 0, tileWidth = 0, tileHeight = 0;
        float historyWeight = 0.0f;
    };
    static_assert(sizeof(BoundaryConstants) == 20 * sizeof(uint32_t));

    struct LowResidualConstants
    {
        uint32_t width, height;
        float paperWhite, transferStrength, colorStrength;
        uint32_t transfer, zeroResidual;
        uint32_t padding[9] {};
    };

    struct GlobalCropConstants
    {
        uint32_t fullWidth, fullHeight;
        uint32_t lowWidth, lowHeight;
        uint32_t roiX, roiY;
        uint32_t roiFullWidth, roiFullHeight;
        uint32_t outputWidth, outputHeight;
        float paperWhite = 2.044f, transferStrength = 1.0f, colorStrength = 1.0f;
        float preExposure = 1.0f, exposureScale = 1.0f;
        uint32_t transfer = 0;
    };

    struct LowCompositeConstants
    {
        uint32_t highWidth, highHeight, lowWidth, lowHeight;
        uint32_t baseSourceX, baseSourceY;
        uint32_t destinationBaseX, destinationBaseY;
        uint32_t compositeMode;
        uint32_t temporalReconstruction;
        uint32_t highResolutionGuided;
        uint32_t featureGuided;
        uint32_t sourceScale;
        uint32_t displayHDR = 0;
        float displayPaperWhite = 203.0f / 80.0f;
        uint32_t padding = 0;
    };

    struct TemporalResidualConstants
    {
        uint32_t width, height;
        uint32_t reset, historyValid;
        uint32_t motionBaseX, motionBaseY, motionWidth, motionHeight;
        uint32_t depthBaseX, depthBaseY, depthWidth, depthHeight;
        float motionScaleX, motionScaleY;
        float jitterCorrectionX, jitterCorrectionY;
    };

    struct OutputTemporalConstants
    {
        uint32_t width, height, historyValid, depthInverted;
        uint32_t motionBaseX, motionBaseY, motionWidth, motionHeight;
        uint32_t depthBaseX, depthBaseY, depthWidth, depthHeight;
        uint32_t sourceBaseX, sourceBaseY, sourceWidth, sourceHeight;
        float motionScaleX, motionScaleY, jitterCorrectionX, jitterCorrectionY;
        uint32_t padding[4] {};
    };
    static_assert(sizeof(OutputTemporalConstants) == 24 * sizeof(uint32_t));
    Microsoft::WRL::ComPtr<ID3D12RootSignature> outputTemporalRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> outputTemporalPipeline;
    // Separate from residual reconstruction history, also valid at scale 1
    // and in full-output/debug modes. Source alpha stores an effective
    // sample count (0 invalid, 1..20 valid); output alpha remains the model's.
    // Depth remains FP32. No additional temporal surfaces are needed.
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> outputHistory, outputSourceHistory, outputDepthHistory;
    std::array<bool, 2> outputHistoryReadable {};
    unsigned int outputHistoryWidth = 0, outputHistoryHeight = 0, outputHistoryIndex = 0;
    bool outputHistoryValid = false, outputTemporalFailureLogged = false;
    float outputPreviousJitterX = 0, outputPreviousJitterY = 0;

    static_assert(sizeof(ResampleConstants) == kGuidanceConstantCount * sizeof(uint32_t));
    static_assert(sizeof(DebugConstants) == kGuidanceConstantCount * sizeof(uint32_t));
    static_assert(sizeof(ColorClampConstants) == kColorClampConstantCount * sizeof(uint32_t));
    static_assert(sizeof(LowDownsampleConstants) == kGuidanceConstantCount * sizeof(uint32_t));
    static_assert(offsetof(LowDownsampleConstants, scale) == 6 * sizeof(uint32_t));
    static_assert(offsetof(LowDownsampleConstants, paperWhite) == 8 * sizeof(uint32_t));
    static_assert(sizeof(GlobalCropConstants) == kGuidanceConstantCount * sizeof(uint32_t));
    static_assert(sizeof(LowResidualConstants) == kGuidanceConstantCount * sizeof(uint32_t));
    static_assert(sizeof(LowCompositeConstants) == kGuidanceConstantCount * sizeof(uint32_t));
    static_assert(offsetof(LowCompositeConstants, displayPaperWhite) == 14 * sizeof(uint32_t));
    static_assert(sizeof(TemporalResidualConstants) == kGuidanceConstantCount * sizeof(uint32_t));

    Microsoft::WRL::ComPtr<ID3D12RootSignature> resampleRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> resamplePipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> debugRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> debugPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> colorClampRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> colorClampPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> sourceTransferRoot, inverseResidualRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> sourceTransferPipeline, inverseResidualPipeline;
    std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, 2> lowDownsampleRoots;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> lowDownsamplePipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, 2> globalDownsampleRoots;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> globalDownsamplePipelines;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> globalCropRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> globalCropPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> lowResidualRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> lowResidualPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> lowCompositeRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> lowCompositePipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> fastCompositeRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> fastCompositePipeline;
    bool fastCompositeAttempted = false;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> displayCompositeRoot, displayFastCompositeRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displayCompositePipeline, displayFastCompositePipeline;
    bool displayFastCompositeAttempted = false;
    int lastCompositeVariant = -1;
    int lastCompositeDomain = -1;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> temporalResidualRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> temporalResidualPipeline;
    std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, kGuidanceDescriptorHeapCount> heaps;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth;
    Microsoft::WRL::ComPtr<ID3D12Resource> lowMotion;
    Microsoft::WRL::ComPtr<ID3D12Resource> lowDepth;
    Microsoft::WRL::ComPtr<ID3D12Resource> typedDepthClone;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> retiredTypedDepthClones;
    Microsoft::WRL::ComPtr<ID3D12Resource> zeroMotion;
    Microsoft::WRL::ComPtr<ID3D12Resource> zeroDepth;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> retiredZeroGuides;
    Microsoft::WRL::ComPtr<ID3D12Resource> debugOverlay;
    Microsoft::WRL::ComPtr<ID3D12Resource> clampedColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> roiColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> lowBaseline;
    Microsoft::WRL::ComPtr<ID3D12Resource> lowColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> lowResidual;
    Microsoft::WRL::ComPtr<ID3D12Resource> globalLowBaseline;
    Microsoft::WRL::ComPtr<ID3D12Resource> restoreSource;
    Microsoft::WRL::ComPtr<ID3D12Resource> extrapolationRestored;
    bool extrapolationRestoredReadable = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> extrapolationOriginal;
    bool extrapolationOriginalReadable = false;
    // Current maps/seed, two spatial scratch atlases, and previous maps.
    // Swap maps after composition; never read and write the history together.
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4> boundaryFields;
    std::array<bool, 4> boundaryReadable {};
    bool boundaryHistoryValid = false;
    uint64_t boundaryAttempt = 0, boundaryLastAttempt = 0;
    BoundaryConstants boundaryPrevious {};
    DXGI_FORMAT boundaryHistoryFormat = DXGI_FORMAT_UNKNOWN;
    uint64_t boundaryDestinationWidth = 0;
    UINT boundaryDestinationHeight = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> boundarySeedRoot, boundaryDiffuseRoot, boundaryApplyRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> boundarySeedPipeline, boundaryDiffusePipeline, boundaryApplyPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> boundaryFitRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> boundaryFitPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> extrapolationInPlaceRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> extrapolationInPlacePipeline;
    DXGI_FORMAT extrapolationInPlaceFormat = DXGI_FORMAT_UNKNOWN;
    bool extrapolationInPlaceSupported = false, extrapolationInPlaceAttempted = false;
    bool extrapolationPipelinesAttempted = false, extrapolationFailureLogged = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> highComposite;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> temporalResidual;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> temporalDepth;
    UINT descriptorSize = 0;
    UINT activeHeap = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int lowWidth = 0;
    unsigned int lowHeight = 0;
    bool guidanceReadable = false;
    bool lowGuidanceReadable = false;
    bool typedDepthCloneReadable = false;
    D3D12_RESOURCE_DESC typedDepthSourceDesc {};
    ID3D12Resource* preparedMotion = nullptr;
    ID3D12Resource* preparedDepth = nullptr;
    bool colorClampReadable = false;
    bool roiColorReadable = false;
    bool restoreSourceReadable = false;
    bool highCompositeReadable = false;
    bool lowBaselineReadable = false;
    bool lowColorReadable = false;
    bool lowResidualReadable = false;
    bool globalLowReadable = false;
    unsigned int globalFullWidth = 0;
    unsigned int globalFullHeight = 0;
    unsigned int globalLowWidth = 0;
    unsigned int globalLowHeight = 0;
    std::array<bool, 2> temporalResidualReadable {};
    std::array<bool, 2> temporalDepthReadable {};
    unsigned int temporalHistoryIndex = 0;
    bool temporalHistoryValid = false;
    bool initialized = false;
    bool prepareFailureLogged = false;
    bool debugFailureLogged = false;
    bool colorClampFailureLogged = false;
    bool contractLogged = false;
    bool temporalFailureLogged = false;

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index) const
    {
        auto handle = heaps[activeHeap]->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptorSize;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT index) const
    {
        auto handle = heaps[activeHeap]->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * descriptorSize;
        return handle;
    }

    bool EnsureLowDownsamplePipeline(ID3D12Device* device, unsigned int scale, bool baselineOnly = false)
    {
        const unsigned int index = scale - 2;
        if (index >= lowDownsamplePipelines.size())
            return false;
        auto& pipeline = baselineOnly ? globalDownsamplePipelines[index] : lowDownsamplePipelines[index];
        auto& root = baselineOnly ? globalDownsampleRoots[index] : lowDownsampleRoots[index];
        if (pipeline != nullptr)
            return true;
        std::string source = std::format("#define DLSSNR_DOWNSAMPLE_SCALE {}\n#define {} 1\n{}", scale,
            baselineOnly ? "DLSSNR_DOWNSAMPLE_BASELINE_ONLY" : "DLSSNR_COLOR_TRANSFER_LIBRARY",
            kLowDownsampleShader);
        if (!baselineOnly)
            source += kColorClampShader;
        return CreateComputePipeline(device, baselineOnly ? 1u : 2u, baselineOnly ? 1u : 2u,
                                      source.c_str(), root, pipeline);
    }

    bool EnsureInitialized(ID3D12Device* device)
    {
        if (initialized)
            return true;
        if (device == nullptr ||
            !CreateComputePipeline(device, 3, 1, kColorClampShader, colorClampRoot, colorClampPipeline,
                                   false, kColorClampConstantCount))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kGuidanceDescriptorsPerHeap;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        for (auto& heap : heaps)
        {
            if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap))))
            {
                LOG_ERROR("[DLSSNR_GUIDANCE] descriptor heap creation failed");
                return false;
            }
        }
        descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        initialized = true;
        return true;
    }

    bool CreateTexture(ID3D12Device* device, DXGI_FORMAT format, unsigned int textureWidth,
                       unsigned int textureHeight, const wchar_t* name,
                       Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, textureWidth, textureHeight, 1, 1, 1, 0,
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        Microsoft::WRL::ComPtr<ID3D12Resource> created;
        const HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                                IID_PPV_ARGS(&created));
        if (FAILED(result))
        {
            LOG_ERROR("[DLSSNR_GUIDANCE] {}x{} texture creation failed: {:X}", textureWidth, textureHeight,
                      static_cast<unsigned int>(result));
            return false;
        }
        created->SetName(name);
        resource = std::move(created);
        return true;
    }

    void DeferResourceRelease(Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
    {
        if (resource == nullptr)
            return;
        auto retired = std::make_shared<Microsoft::WRL::ComPtr<ID3D12Resource>>(std::move(resource));
        GazeRoiFrameSync::DeferCallback([retired]() mutable { retired->Reset(); });
    }

    bool EnsureTextures(ID3D12Device* device, unsigned int textureWidth, unsigned int textureHeight,
                        bool needGuidance, bool needDebug, bool needClamp, bool needTransfer)
    {
        if (width != textureWidth || height != textureHeight)
        {
            DeferResourceRelease(motion);
            DeferResourceRelease(depth);
            DeferResourceRelease(lowMotion);
            DeferResourceRelease(lowDepth);
            DeferResourceRelease(debugOverlay);
            DeferResourceRelease(clampedColor);
            DeferResourceRelease(roiColor);
            DeferResourceRelease(restoreSource);
            DeferResourceRelease(highComposite);
            for (auto& resource : temporalResidual)
                DeferResourceRelease(resource);
            for (auto& resource : temporalDepth)
                DeferResourceRelease(resource);
            width = textureWidth;
            height = textureHeight;
            guidanceReadable = false;
            lowGuidanceReadable = false;
            colorClampReadable = false;
            roiColorReadable = false;
            restoreSourceReadable = false;
            highCompositeReadable = false;
            lowBaselineReadable = false;
            lowColorReadable = false;
            lowResidualReadable = false;
            temporalResidualReadable = {};
            temporalDepthReadable = {};
            temporalHistoryValid = false;
        }
        if (needGuidance &&
            ((motion == nullptr && !CreateTexture(device, DXGI_FORMAT_R16G16_FLOAT, width, height,
                                                   L"OptiScaler_DLSSNR_FullRes_Motion", motion)) ||
             (depth == nullptr && !CreateTexture(device, DXGI_FORMAT_R32_FLOAT, width, height,
                                                  L"OptiScaler_DLSSNR_FullRes_Depth", depth))))
            return false;
        if (needDebug && debugOverlay == nullptr &&
            !CreateTexture(device, kDlssNrFormat, width, height, L"OptiScaler_DLSSNR_Debug_Overlay", debugOverlay))
            return false;
        if ((needClamp || needTransfer) && clampedColor == nullptr &&
            !CreateTexture(device, kDlssNrFormat, width, height, L"OptiScaler_DLSSNR_Clamped_Color", clampedColor))
            return false;
        return true;
    }

    bool EnsureLowGuidanceTextures(ID3D12Device* device, unsigned int textureWidth, unsigned int textureHeight,
                                   unsigned int scale)
    {
        const unsigned int targetWidth = std::max((textureWidth + scale - 1) / scale, 1u);
        const unsigned int targetHeight = std::max((textureHeight + scale - 1) / scale, 1u);
        if (lowMotion != nullptr && lowDepth != nullptr)
            return true;
        lowMotion.Reset();
        lowDepth.Reset();
        return CreateTexture(device, DXGI_FORMAT_R16G16_FLOAT, targetWidth, targetHeight,
                             L"OptiScaler_DLSSNR_LowRes_Motion", lowMotion) &&
               CreateTexture(device, DXGI_FORMAT_R32_FLOAT, targetWidth, targetHeight,
                             L"OptiScaler_DLSSNR_LowRes_Depth", lowDepth);
    }

    bool EnsureLowTextures(ID3D12Device* device, unsigned int textureWidth, unsigned int textureHeight,
                           unsigned int scale, bool needTemporal)
    {
        const unsigned int targetWidth = std::max((textureWidth + scale - 1) / scale, 1u);
        const unsigned int targetHeight = std::max((textureHeight + scale - 1) / scale, 1u);
        const bool temporalReady = !needTemporal ||
            (temporalResidual[0] != nullptr && temporalResidual[1] != nullptr &&
             temporalDepth[0] != nullptr && temporalDepth[1] != nullptr);
        const bool coreReady = lowWidth == targetWidth && lowHeight == targetHeight && lowBaseline != nullptr &&
            lowColor != nullptr && lowResidual != nullptr && highComposite != nullptr &&
            lowMotion != nullptr && lowDepth != nullptr;
        if (coreReady)
        {
            if (temporalReady)
                return true;
            // Enabling the experiment must not release the already submitted
            // low-resolution pipeline resources. Add only its private history.
            for (auto& resource : temporalResidual)
                DeferResourceRelease(resource);
            for (auto& resource : temporalDepth)
                DeferResourceRelease(resource);
            temporalResidualReadable = {};
            temporalDepthReadable = {};
            temporalHistoryValid = false;
            return CreateTexture(device, kDlssNrFormat, lowWidth, lowHeight,
                                 L"OptiScaler_DLSSNR_Temporal_Residual_0", temporalResidual[0]) &&
                   CreateTexture(device, kDlssNrFormat, lowWidth, lowHeight,
                                 L"OptiScaler_DLSSNR_Temporal_Residual_1", temporalResidual[1]) &&
                   CreateTexture(device, DXGI_FORMAT_R32_FLOAT, lowWidth, lowHeight,
                                 L"OptiScaler_DLSSNR_Temporal_Depth_0", temporalDepth[0]) &&
                   CreateTexture(device, DXGI_FORMAT_R32_FLOAT, lowWidth, lowHeight,
                                 L"OptiScaler_DLSSNR_Temporal_Depth_1", temporalDepth[1]);
        }
        DeferResourceRelease(lowBaseline);
        DeferResourceRelease(lowColor);
        DeferResourceRelease(lowResidual);
        DeferResourceRelease(lowMotion);
        DeferResourceRelease(lowDepth);
        DeferResourceRelease(highComposite);
        for (auto& resource : temporalResidual)
            DeferResourceRelease(resource);
        for (auto& resource : temporalDepth)
            DeferResourceRelease(resource);
        highCompositeReadable = false;
        lowBaselineReadable = false;
        lowColorReadable = false;
        lowResidualReadable = false;
        lowGuidanceReadable = false;
        temporalResidualReadable = {};
        temporalDepthReadable = {};
        temporalHistoryValid = false;
        lowWidth = targetWidth;
        lowHeight = targetHeight;
        return CreateTexture(device, kDlssNrFormat, textureWidth, textureHeight,
                             L"OptiScaler_DLSSNR_High_Composite", highComposite) &&
               CreateTexture(device, kDlssNrFormat, lowWidth, lowHeight,
                             L"OptiScaler_DLSSNR_Low_Baseline", lowBaseline) &&
               CreateTexture(device, kDlssNrFormat, lowWidth, lowHeight,
                             L"OptiScaler_DLSSNR_Low_Color", lowColor) &&
               CreateTexture(device, kDlssNrFormat, lowWidth, lowHeight,
                             L"OptiScaler_DLSSNR_Low_Residual", lowResidual) &&
               CreateTexture(device, DXGI_FORMAT_R16G16_FLOAT, lowWidth, lowHeight,
                             L"OptiScaler_DLSSNR_LowRes_Motion", lowMotion) &&
               CreateTexture(device, DXGI_FORMAT_R32_FLOAT, lowWidth, lowHeight,
                             L"OptiScaler_DLSSNR_LowRes_Depth", lowDepth) &&
               (!needTemporal ||
                (CreateTexture(device, kDlssNrFormat, lowWidth, lowHeight,
                               L"OptiScaler_DLSSNR_Temporal_Residual_0", temporalResidual[0]) &&
                 CreateTexture(device, kDlssNrFormat, lowWidth, lowHeight,
                               L"OptiScaler_DLSSNR_Temporal_Residual_1", temporalResidual[1]) &&
                 CreateTexture(device, DXGI_FORMAT_R32_FLOAT, lowWidth, lowHeight,
                               L"OptiScaler_DLSSNR_Temporal_Depth_0", temporalDepth[0]) &&
                 CreateTexture(device, DXGI_FORMAT_R32_FLOAT, lowWidth, lowHeight,
                               L"OptiScaler_DLSSNR_Temporal_Depth_1", temporalDepth[1])));
    }

    bool EnsureGlobalDownsampleTextures(ID3D12Device* device,
                                        unsigned int fullWidth, unsigned int fullHeight,
                                        unsigned int scale)
    {
        if (device == nullptr || fullWidth == 0 || fullHeight == 0 || scale < 2 || scale > 3)
            return false;
        const unsigned int targetLowWidth = std::max((fullWidth + scale - 1) / scale, 1u);
        const unsigned int targetLowHeight = std::max((fullHeight + scale - 1) / scale, 1u);
        const bool matches = globalLowBaseline != nullptr &&
            globalFullWidth == fullWidth && globalFullHeight == fullHeight &&
            globalLowWidth == targetLowWidth && globalLowHeight == targetLowHeight;
        if (matches)
            return true;

        DeferResourceRelease(globalLowBaseline);
        globalLowReadable = false;
        globalFullWidth = fullWidth;
        globalFullHeight = fullHeight;
        globalLowWidth = targetLowWidth;
        globalLowHeight = targetLowHeight;
        return CreateTexture(device, kDlssNrFormat, globalLowWidth, globalLowHeight,
                             L"OptiScaler_DLSSNR_Debug_Global_Low_Baseline", globalLowBaseline);
    }

    bool PrepareGlobalDownsampleRoiColor(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                                         ID3D12Resource* source,
                                         unsigned int sourceBaseX, unsigned int sourceBaseY,
                                         unsigned int fullWidth, unsigned int fullHeight,
                                         unsigned int roiX, unsigned int roiY,
                                         unsigned int roiWidth, unsigned int roiHeight,
                                         unsigned int scale, float paperWhite, ID3D12Resource* exposure,
                                         float preExposure, float exposureScale, bool useExposure,
                                         bool sdrToneMapping)
    {
        if (device == nullptr || commandList == nullptr || source == nullptr ||
            scale < 2 || scale > 3 || fullWidth == 0 || fullHeight == 0 ||
            globalLowBaseline == nullptr || lowBaseline == nullptr || lowColor == nullptr ||
            globalLowWidth != (fullWidth + scale - 1) / scale ||
            globalLowHeight != (fullHeight + scale - 1) / scale ||
            static_cast<uint64_t>(roiX) + roiWidth > fullWidth ||
            static_cast<uint64_t>(roiY) + roiHeight > fullHeight)
            return false;
        const auto sourceDesc = source->GetDesc();
        if (!IsShaderReadableTexture(sourceDesc) ||
            static_cast<uint64_t>(sourceBaseX) + fullWidth > sourceDesc.Width ||
            static_cast<uint64_t>(sourceBaseY) + fullHeight > sourceDesc.Height)
            return false;

        if (!EnsureLowDownsamplePipeline(device, scale, true))
            return false;
        if (globalCropPipeline == nullptr)
        {
            const std::string shader = std::string("#define DLSSNR_COLOR_TRANSFER_LIBRARY 1\n") +
                kGlobalDownsampleCropShader + kColorClampShader;
            if (!CreateComputePipeline(device, 2, 2, shader.c_str(), globalCropRoot, globalCropPipeline))
                return false;
        }
        if (globalLowReadable)
        {
            Transition(commandList, globalLowBaseline.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            globalLowReadable = false;
        }
        if (!CreateSrv(device, source, kGlobalDownsampleDescriptorBase))
            return false;
        CreateUav(device, globalLowBaseline.Get(), kDlssNrFormat, kGlobalDownsampleDescriptorBase + 1);
        LowDownsampleConstants downsampleConstants {
            sourceBaseX, sourceBaseY, fullWidth, fullHeight,
            globalLowWidth, globalLowHeight, scale, 0u, 2.044f, 1.0f, 1.0f
        };
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(globalDownsampleRoots[scale - 2].Get());
        commandList->SetPipelineState(globalDownsamplePipelines[scale - 2].Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kGlobalDownsampleDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kGuidanceConstantCount, &downsampleConstants, 0);
        commandList->Dispatch((globalLowWidth + 7) / 8, (globalLowHeight + 7) / 8, 1);
        Transition(commandList, globalLowBaseline.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        globalLowReadable = true;

        if (lowBaselineReadable)
            Transition(commandList, lowBaseline.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (lowColorReadable)
            Transition(commandList, lowColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lowBaselineReadable = false;
        lowColorReadable = false;
        if (!CreateSrv(device, globalLowBaseline.Get(), kFusedGlobalCropDescriptorBase) ||
            !CreateExposureSrv(device, source, exposure, kFusedGlobalCropDescriptorBase + 1, useExposure))
            return false;
        CreateUav(device, lowBaseline.Get(), kDlssNrFormat, kFusedGlobalCropDescriptorBase + 2);
        CreateUav(device, lowColor.Get(), kDlssNrFormat, kFusedGlobalCropDescriptorBase + 3);
        GlobalCropConstants cropConstants {
            fullWidth, fullHeight, globalLowWidth, globalLowHeight,
            roiX, roiY, roiWidth, roiHeight, lowWidth, lowHeight,
            paperWhite, ColorTransferStrength(), 1.0f, preExposure, exposureScale,
            (sdrToneMapping ? 2u : 1u) | (useExposure ? 4u : 0u)
        };
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(globalCropRoot.Get());
        commandList->SetPipelineState(globalCropPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kFusedGlobalCropDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kGuidanceConstantCount, &cropConstants, 0);
        D3D12_RESOURCE_STATES exposureRestoreState = D3D12_RESOURCE_STATE_COMMON;
        const bool exposureTransitioned = PrepareExposureRead(commandList, exposure, useExposure,
                                                               &exposureRestoreState);
        commandList->Dispatch((lowWidth + 7) / 8, (lowHeight + 7) / 8, 1);
        RestoreExposureState(commandList, exposure, exposureTransitioned, exposureRestoreState);
        Transition(commandList, lowBaseline.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, lowColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lowBaselineReadable = true;
        lowColorReadable = true;
        return true;
    }

    void BeginFrame(uint32_t frameSlot, uint64_t evaluationAttempt)
    {
        // Reuse only the descriptor heap whose GPU lifetime was acquired by
        // the enclosing evaluation, including after skipped/failed frames.
        activeHeap = frameSlot;
        if (boundaryLastAttempt + 1 != evaluationAttempt) boundaryHistoryValid = false;
        boundaryAttempt = evaluationAttempt;
    }

    bool CreateSrv(ID3D12Device* device, ID3D12Resource* resource, UINT descriptorIndex) const
    {
        if (resource == nullptr)
            return false;
        const auto resourceDesc = resource->GetDesc();
        if (!IsShaderReadableTexture(resourceDesc))
            return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = ShaderReadableFormat(resourceDesc.Format);
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &srv, CpuHandle(descriptorIndex));
        return true;
    }

    // Keep the optional exposure slot valid even when the game does not provide
    // one. The shader gates the duplicate source SRV with UseExposure.
    bool CreateExposureSrv(ID3D12Device* device, ID3D12Resource* source,
                           ID3D12Resource* exposure, UINT descriptorIndex, bool useExposure) const
    {
        return CreateSrv(device, useExposure && exposure != nullptr ? exposure : source, descriptorIndex);
    }

    void CreateUav(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, UINT descriptorIndex) const
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource, nullptr, &uav, CpuHandle(descriptorIndex));
    }

    bool PrepareZeroGuide(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                          DXGI_FORMAT format, unsigned int guideWidth, unsigned int guideHeight,
                          const wchar_t* name, UINT descriptorIndex,
                          Microsoft::WRL::ComPtr<ID3D12Resource>& resource, ID3D12Resource** result)
    {
        if (device == nullptr || commandList == nullptr || result == nullptr ||
            guideWidth == 0 || guideHeight == 0)
            return false;

        const bool matches = resource != nullptr && resource->GetDesc().Width == guideWidth &&
                             resource->GetDesc().Height == guideHeight && resource->GetDesc().Format == format;
        if (!matches)
        {
            if (resource != nullptr)
                retiredZeroGuides.emplace_back(std::move(resource));
            if (!CreateTexture(device, format, guideWidth, guideHeight, name, resource))
                return false;

            CreateUav(device, resource.Get(), format, descriptorIndex);
            ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
            commandList->SetDescriptorHeaps(1, &descriptorHeap);
            constexpr float zero[4] {};
            commandList->ClearUnorderedAccessViewFloat(GpuHandle(descriptorIndex), CpuHandle(descriptorIndex),
                                                       resource.Get(), zero, 0, nullptr);
            Transition(commandList, resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            LOG_INFO("[DLSSNR_GUIDANCE] created zero guide {}x{} format=0x{:X}", guideWidth, guideHeight,
                     static_cast<unsigned int>(format));
        }
        *result = resource.Get();
        return true;
    }

    bool PrepareTypelessDepthClone(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                                   ID3D12Resource* source, ID3D12Resource** result)
    {
        if (result == nullptr)
            return false;
        *result = source;
        if (source == nullptr)
            return true;

        const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
        if (!IsTypelessDepthFormat(sourceDesc.Format))
            return true;

        const DXGI_FORMAT typedFormat = ShaderReadableFormat(sourceDesc.Format);
        const auto cloneMatches = [&]()
        {
            return typedDepthClone != nullptr && typedDepthSourceDesc.Dimension == sourceDesc.Dimension &&
                   typedDepthSourceDesc.Alignment == sourceDesc.Alignment &&
                   typedDepthSourceDesc.Width == sourceDesc.Width &&
                   typedDepthSourceDesc.Height == sourceDesc.Height &&
                   typedDepthSourceDesc.DepthOrArraySize == sourceDesc.DepthOrArraySize &&
                   typedDepthSourceDesc.MipLevels == sourceDesc.MipLevels &&
                   typedDepthSourceDesc.Format == sourceDesc.Format &&
                   typedDepthSourceDesc.SampleDesc.Count == sourceDesc.SampleDesc.Count &&
                   typedDepthSourceDesc.SampleDesc.Quality == sourceDesc.SampleDesc.Quality &&
                   typedDepthSourceDesc.Layout == sourceDesc.Layout;
        };

        if (!cloneMatches())
        {
            if (typedDepthClone != nullptr)
                retiredTypedDepthClones.emplace_back(std::move(typedDepthClone));

            D3D12_RESOURCE_DESC cloneDesc = sourceDesc;
            cloneDesc.Format = typedFormat;
            cloneDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const HRESULT createResult = device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &cloneDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&typedDepthClone));
            if (FAILED(createResult) || typedDepthClone == nullptr)
            {
                LOG_ERROR("[DLSSNR_GUIDANCE] typed depth clone creation failed: {:X}",
                          static_cast<unsigned int>(createResult));
                return false;
            }
            typedDepthClone->SetName(L"OptiScaler_DLSSNR_Typed_Depth_Clone");
            typedDepthSourceDesc = sourceDesc;
            typedDepthCloneReadable = false;
            LOG_INFO("[DLSSNR_GUIDANCE] cloning typeless depth format 0x{:X} as 0x{:X}",
                     static_cast<unsigned int>(sourceDesc.Format), static_cast<unsigned int>(typedFormat));
        }

        if (typedDepthCloneReadable)
        {
            Transition(commandList, typedDepthClone.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            typedDepthCloneReadable = false;
        }
        Transition(commandList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->CopyResource(typedDepthClone.Get(), source);
        Transition(commandList, source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, typedDepthClone.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        typedDepthCloneReadable = true;
        *result = typedDepthClone.Get();
        return true;
    }

    bool Prepare(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                 ID3D12Resource* sourceMotion, ID3D12Resource* sourceDepth,
                 unsigned int motionBaseX, unsigned int motionBaseY,
                 unsigned int motionWidth, unsigned int motionHeight,
                 unsigned int depthBaseX, unsigned int depthBaseY,
                 unsigned int depthWidth, unsigned int depthHeight,
                  float motionScaleX, float motionScaleY,
                  unsigned int targetWidth = 0, unsigned int targetHeight = 0,
                  bool depthInverted = false, UINT descriptorBase = 0,
                  float motionOriginOffsetX = 0.0f, float motionOriginOffsetY = 0.0f)
    {
        if (sourceMotion == nullptr || sourceDepth == nullptr || motionWidth == 0 || motionHeight == 0 ||
            depthWidth == 0 || depthHeight == 0)
            return false;
        if (resamplePipeline == nullptr &&
            !CreateComputePipeline(device, 2, 2, kGuidanceResampleShader, resampleRoot, resamplePipeline))
            return false;
        const auto motionDesc = sourceMotion->GetDesc();
        const auto depthDesc = sourceDepth->GetDesc();
        if (!IsShaderReadableTexture(motionDesc) || !IsShaderReadableTexture(depthDesc) ||
            static_cast<uint64_t>(motionBaseX) + motionWidth > motionDesc.Width ||
            static_cast<uint64_t>(motionBaseY) + motionHeight > motionDesc.Height ||
            static_cast<uint64_t>(depthBaseX) + depthWidth > depthDesc.Width ||
            static_cast<uint64_t>(depthBaseY) + depthHeight > depthDesc.Height)
            return false;

        targetWidth = targetWidth != 0 ? targetWidth : width;
        targetHeight = targetHeight != 0 ? targetHeight : height;
        const bool lowTarget = targetWidth != width || targetHeight != height;
        ID3D12Resource* motionOutput = lowTarget ? lowMotion.Get() : motion.Get();
        ID3D12Resource* depthOutput = lowTarget ? lowDepth.Get() : depth.Get();
        if (motionOutput == nullptr || depthOutput == nullptr)
            return false;

        if (guidanceReadable)
        {
            Transition(commandList, motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(commandList, depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            guidanceReadable = false;
        }
        if (lowGuidanceReadable)
        {
            Transition(commandList, lowMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(commandList, lowDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            lowGuidanceReadable = false;
        }
        if (!CreateSrv(device, sourceMotion, descriptorBase) ||
            !CreateSrv(device, sourceDepth, descriptorBase + 1))
            return false;
        CreateUav(device, motionOutput, DXGI_FORMAT_R16G16_FLOAT, descriptorBase + 2);
        CreateUav(device, depthOutput, DXGI_FORMAT_R32_FLOAT, descriptorBase + 3);

        ResampleConstants constants { targetWidth, targetHeight, motionBaseX, motionBaseY, motionWidth, motionHeight,
                                      depthBaseX, depthBaseY, depthWidth, depthHeight,
                                      motionScaleX * static_cast<float>(targetWidth) / static_cast<float>(motionWidth),
                                      motionScaleY * static_cast<float>(targetHeight) / static_cast<float>(motionHeight),
                                      motionOriginOffsetX, motionOriginOffsetY,
                                      depthInverted ? 1u : 0u };
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(resampleRoot.Get());
        commandList->SetPipelineState(resamplePipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(descriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kGuidanceConstantCount, &constants, 0);
        commandList->Dispatch((targetWidth + 7) / 8, (targetHeight + 7) / 8, 1);
        Transition(commandList, motionOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, depthOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        guidanceReadable = !lowTarget;
        lowGuidanceReadable = lowTarget;
        preparedMotion = motionOutput;
        preparedDepth = depthOutput;
        return true;
    }

    bool PrepareColorClamp(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                           ID3D12Resource* source, unsigned int sourceBaseX, unsigned int sourceBaseY,
                           unsigned int sourceWidth, unsigned int sourceHeight, bool clamp = true)
    {
        if (device == nullptr || commandList == nullptr || source == nullptr || clampedColor == nullptr ||
            sourceWidth == 0 || sourceHeight == 0)
            return false;
        const auto sourceDesc = source->GetDesc();
        if (!IsShaderReadableTexture(sourceDesc) ||
            static_cast<uint64_t>(sourceBaseX) + sourceWidth > sourceDesc.Width ||
            static_cast<uint64_t>(sourceBaseY) + sourceHeight > sourceDesc.Height)
            return false;

        if (colorClampReadable)
        {
            Transition(commandList, clampedColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            colorClampReadable = false;
        }
        if (!CreateSrv(device, source, kColorTransferDescriptorBase) ||
            !CreateSrv(device, source, kColorTransferDescriptorBase + 1) ||
            !CreateSrv(device, source, kColorTransferDescriptorBase + 2))
            return false;
        CreateUav(device, clampedColor.Get(), kDlssNrFormat, kColorTransferDescriptorBase + 3);
        ColorClampConstants constants { width, height, sourceBaseX, sourceBaseY, sourceWidth, sourceHeight,
                                        0u, 0u, 1.0f, 1.0f, 1.0f, clamp ? 0u : 3u };
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(colorClampRoot.Get());
        commandList->SetPipelineState(colorClampPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kColorTransferDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kColorClampConstantCount, &constants, 0);
        commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        Transition(commandList, clampedColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        colorClampReadable = true;
        return true;
    }

    bool PrepareColorTransfer(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                              ID3D12Resource* source, unsigned int sourceBaseX, unsigned int sourceBaseY,
                              unsigned int sourceWidth, unsigned int sourceHeight,
                              float paperWhite, float transferStrength, float colorStrength,
                              ID3D12Resource* exposure = nullptr, float preExposure = 1.0f,
                              float exposureScale = 1.0f, bool useExposure = false, bool sdrToneMapping = false,
                              ID3D12Resource* preserveSource = nullptr, bool sanitizeSource = false)
    {
        if (device == nullptr || commandList == nullptr || source == nullptr || clampedColor == nullptr ||
            sourceWidth == 0 || sourceHeight == 0)
            return false;
        const auto sourceDesc = source->GetDesc();
        if (!IsShaderReadableTexture(sourceDesc) ||
            static_cast<uint64_t>(sourceBaseX) + sourceWidth > sourceDesc.Width ||
            static_cast<uint64_t>(sourceBaseY) + sourceHeight > sourceDesc.Height)
            return false;
        if (preserveSource != nullptr && sourceTransferPipeline == nullptr)
        {
            const std::string shader = std::string("#define DLSSNR_PRESERVE_SOURCE 1\n") + kColorClampShader;
            if (!CreateComputePipeline(device, 3, 2, shader.c_str(), sourceTransferRoot,
                                       sourceTransferPipeline, false, kColorClampConstantCount))
                return false;
        }
        if (preserveSource != nullptr &&
            (!ValidReadRect(preserveSource, 0, 0, sourceWidth, sourceHeight) || preserveSource == source))
            return false;
        const UINT descriptors = preserveSource != nullptr ? kSourceTransferDescriptorBase : kColorTransferDescriptorBase;
        if (colorClampReadable)
        {
            Transition(commandList, clampedColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            colorClampReadable = false;
        }
        if (!CreateSrv(device, source, descriptors) ||
            !CreateExposureSrv(device, source, exposure, descriptors + 1, useExposure) ||
            !CreateSrv(device, source, descriptors + 2))
            return false;
        CreateUav(device, clampedColor.Get(), kDlssNrFormat, descriptors + 3);
        if (preserveSource != nullptr)
            CreateUav(device, preserveSource, kDlssNrFormat, descriptors + 4);
        ColorClampConstants constants { sourceWidth, sourceHeight, sourceBaseX, sourceBaseY,
                                        sourceWidth, sourceHeight, 0u, 0u,
                                        paperWhite, ColorTransferStrength(transferStrength), colorStrength, sdrToneMapping ? 4u : 1u,
                                        preExposure, exposureScale, useExposure ? 1u : 0u, 0u };
        constants.colorClampPadding[0] = sanitizeSource ? 1u : 0u;
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(preserveSource != nullptr ? sourceTransferRoot.Get() : colorClampRoot.Get());
        commandList->SetPipelineState(preserveSource != nullptr ? sourceTransferPipeline.Get() : colorClampPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(descriptors));
        commandList->SetComputeRoot32BitConstants(1, kColorClampConstantCount, &constants, 0);
        D3D12_RESOURCE_STATES exposureRestoreState = D3D12_RESOURCE_STATE_COMMON;
        const bool exposureTransitioned = PrepareExposureRead(commandList, exposure, useExposure,
                                                               &exposureRestoreState);
        commandList->Dispatch((sourceWidth + 7) / 8, (sourceHeight + 7) / 8, 1);
        RestoreExposureState(commandList, exposure, exposureTransitioned, exposureRestoreState);
        Transition(commandList, clampedColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        colorClampReadable = true;
        if (preserveSource != nullptr)
        {
            Transition(commandList, preserveSource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (preserveSource == roiColor.Get()) roiColorReadable = true;
            if (preserveSource == restoreSource.Get()) restoreSourceReadable = true;
        }
        return true;
    }

    bool ApplyColorTransfer(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                            ID3D12Resource* source, ID3D12Resource* destination,
                            unsigned int textureWidth, unsigned int textureHeight,
                            float paperWhite, float transferStrength, float colorStrength,
                            ID3D12Resource* exposure = nullptr, float preExposure = 1.0f,
                            float exposureScale = 1.0f, bool useExposure = false, bool sdrToneMapping = false)
    {
        if (device == nullptr || commandList == nullptr || source == nullptr || destination == nullptr ||
            textureWidth == 0 || textureHeight == 0)
            return false;
        const auto sourceDesc = source->GetDesc();
        const auto destinationDesc = destination->GetDesc();
        if (!IsShaderReadableTexture(sourceDesc) || !IsShaderReadableTexture(destinationDesc) ||
            textureWidth > sourceDesc.Width || textureHeight > sourceDesc.Height ||
            textureWidth > destinationDesc.Width || textureHeight > destinationDesc.Height)
            return false;
        if (!CreateSrv(device, source, kColorTransferDescriptorBase) ||
            !CreateExposureSrv(device, source, exposure, kColorTransferDescriptorBase + 1, useExposure) ||
            !CreateSrv(device, source, kColorTransferDescriptorBase + 2))
            return false;
        CreateUav(device, destination, kDlssNrFormat, kColorTransferDescriptorBase + 3);
        ColorClampConstants constants { textureWidth, textureHeight, 0u, 0u, textureWidth, textureHeight,
                                        0u, 0u, paperWhite, ColorTransferStrength(transferStrength), colorStrength, sdrToneMapping ? 4u : 1u,
                                        preExposure, exposureScale, useExposure ? 1u : 0u, 0u };
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(colorClampRoot.Get());
        commandList->SetPipelineState(colorClampPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kColorTransferDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kColorClampConstantCount, &constants, 0);
        D3D12_RESOURCE_STATES exposureRestoreState = D3D12_RESOURCE_STATE_COMMON;
        const bool exposureTransitioned = PrepareExposureRead(commandList, exposure, useExposure,
                                                               &exposureRestoreState);
        commandList->Dispatch((textureWidth + 7) / 8, (textureHeight + 7) / 8, 1);
        RestoreExposureState(commandList, exposure, exposureTransitioned, exposureRestoreState);
        return true;
    }

    bool ApplyColorTransferInverse(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                                   ID3D12Resource* source, ID3D12Resource* destination, unsigned int destinationBaseX,
                                   unsigned int destinationBaseY, unsigned int width, unsigned int height,
                                   float paperWhite, float transferStrength, float colorStrength,
                                   DXGI_FORMAT destinationFormat = kDlssNrFormat,
                                   ID3D12Resource* exposure = nullptr, float preExposure = 1.0f,
                                   float exposureScale = 1.0f, bool useExposure = false, bool sdrToneMapping = false,
                                   ID3D12Resource* original = nullptr, unsigned int originalBaseX = 0,
                                   unsigned int originalBaseY = 0, unsigned int edgeBlendPx = 0,
                                   unsigned int edgeBlendMask = 0, float extrapolationScale = 0.0f,
                                   unsigned int extrapolationOutputBaseX = 0,
                                   unsigned int extrapolationOutputBaseY = 0,
                                   unsigned int extrapolationOutputWidth = 0,
                                   unsigned int extrapolationOutputHeight = 0,
                                   unsigned int extrapolationRoiOffsetX = 0,
                                   unsigned int extrapolationRoiOffsetY = 0,
                                   ID3D12Resource* extrapolationOriginalSource = nullptr, bool emitResidual = false)
    {
        if (device == nullptr || commandList == nullptr || source == nullptr || destination == nullptr ||
            width == 0 || height == 0)
            return false;
        if (emitResidual)
        {
            if (edgeBlendPx != 0 || extrapolationScale != 0.0f || destinationFormat != kDlssNrFormat)
                return false;
            if (inverseResidualPipeline == nullptr)
            {
                const std::string shader = std::string("#define DLSSNR_INVERSE_RESIDUAL 1\n") + kColorClampShader;
                if (!CreateComputePipeline(device, 3, 1, shader.c_str(), inverseResidualRoot,
                                           inverseResidualPipeline, false, kColorClampConstantCount))
                    return false;
            }
        }
        // Restore the stabilized model with the existing exposure/SDR contract
        // before learning appearance. These dispatches have distinct descriptor
        // tables; the bank and the visible model must use the same final RGB.
        if (extrapolationScale > 0.0f && edgeBlendPx != 0)
        {
            if (original == nullptr || originalBaseX != 0 || originalBaseY != 0 ||
                device == nullptr || commandList == nullptr || width == 0 || height == 0)
                return false;
            if (extrapolationRestored != nullptr &&
                (extrapolationRestored->GetDesc().Width != width ||
                 extrapolationRestored->GetDesc().Height != height))
            {
                DeferResourceRelease(extrapolationRestored);
                extrapolationRestoredReadable = false;
            }
            if (extrapolationRestored == nullptr &&
                !CreateTexture(device, kDlssNrFormat, width, height,
                               L"OptiScaler_DLSSNR_Extrapolation_Restored", extrapolationRestored))
                return false;
            if (extrapolationRestoredReadable)
            {
                Transition(commandList, extrapolationRestored.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                extrapolationRestoredReadable = false;
            }
            if (!ApplyColorTransferInverse(device, commandList, source, extrapolationRestored.Get(),
                    0, 0, width, height, paperWhite, transferStrength, colorStrength, kDlssNrFormat,
                    exposure, preExposure, exposureScale, useExposure, sdrToneMapping,
                    original, originalBaseX, originalBaseY))
                return false;
            Transition(commandList, extrapolationRestored.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            extrapolationRestoredReadable = true;
            const bool expanded = extrapolationOutputWidth != 0 && extrapolationOutputHeight != 0;
            return ApplyColorCopy(device, commandList, extrapolationRestored.Get(), destination,
                expanded ? extrapolationOutputBaseX : destinationBaseX,
                expanded ? extrapolationOutputBaseY : destinationBaseY,
                expanded ? extrapolationOutputWidth : width,
                expanded ? extrapolationOutputHeight : height, destinationFormat, 0, 0,
                kExtrapolationDescriptorBase, false,
                expanded && extrapolationOriginalSource != nullptr ? extrapolationOriginalSource : original,
                edgeBlendPx, edgeBlendMask,
                extrapolationScale,
                expanded ? extrapolationRoiOffsetX : 0u,
                expanded ? extrapolationRoiOffsetY : 0u,
                width, height, 0u, 0u);
        }
        const auto destinationDesc = destination->GetDesc();
        if (!IsShaderReadableTexture(destinationDesc) ||
            static_cast<uint64_t>(destinationBaseX) + width > destinationDesc.Width ||
            static_cast<uint64_t>(destinationBaseY) + height > destinationDesc.Height)
            return false;
        if (sdrToneMapping || emitResidual || edgeBlendPx != 0)
        {
            if (original == nullptr || original == destination)
                return false;
            const auto originalDesc = original->GetDesc();
            if (!IsShaderReadableTexture(originalDesc) ||
                static_cast<uint64_t>(originalBaseX) + width > originalDesc.Width ||
                static_cast<uint64_t>(originalBaseY) + height > originalDesc.Height)
                return false;
            // Edge blending uses the fixed-origin copy created by
            // PrepareRoiColor. SDR restoration may also use a subrectangle in
            // the full-frame path, where edge blending is disabled.
            if (edgeBlendPx != 0 && (originalBaseX != 0 || originalBaseY != 0))
                return false;
        }
        if (!CreateSrv(device, source, kInverseTransferDescriptorBase) ||
            !CreateExposureSrv(device, source, exposure, kInverseTransferDescriptorBase + 1, useExposure) ||
            !CreateSrv(device, (sdrToneMapping || emitResidual || edgeBlendPx != 0) ? original : source,
                       kInverseTransferDescriptorBase + 2))
            return false;
        CreateUav(device, destination, destinationFormat, kInverseTransferDescriptorBase + 3);
        ColorClampConstants constants { width, height, (sdrToneMapping || emitResidual) ? originalBaseX : 0u,
                                        (sdrToneMapping || emitResidual) ? originalBaseY : 0u, width, height,
                                        destinationBaseX, destinationBaseY, paperWhite,
                                        ColorTransferStrength(transferStrength), colorStrength, sdrToneMapping ? 5u : 2u,
                                        preExposure, exposureScale, useExposure ? 1u : 0u,
                                        edgeBlendPx | ((edgeBlendMask & 0xfu) << 16u) };
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(emitResidual ? inverseResidualRoot.Get() : colorClampRoot.Get());
        commandList->SetPipelineState(emitResidual ? inverseResidualPipeline.Get() : colorClampPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kInverseTransferDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kColorClampConstantCount, &constants, 0);
        D3D12_RESOURCE_STATES exposureRestoreState = D3D12_RESOURCE_STATE_COMMON;
        const bool exposureTransitioned = PrepareExposureRead(commandList, exposure, useExposure,
                                                               &exposureRestoreState);
        commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        RestoreExposureState(commandList, exposure, exposureTransitioned, exposureRestoreState);
        return true;
    }

    bool EnsureExtrapolationPipelines(ID3D12Device* device)
    {
        if (boundarySeedPipeline && boundaryDiffusePipeline && boundaryFitPipeline && boundaryApplyPipeline) return true;
        if (extrapolationPipelinesAttempted) return false;
        extrapolationPipelinesAttempted = true;
        using namespace DlssNrExtrapolation::Shaders;
        if (!CreateComputePipeline(device, 2, 1, nullptr, boundarySeedRoot, boundarySeedPipeline,
                                   false, 20u, { BoundarySeed, sizeof(BoundarySeed) }) ||
            !CreateComputePipeline(device, 1, 1, nullptr, boundaryDiffuseRoot, boundaryDiffusePipeline,
                                   false, 20u, { BoundaryDiffuse, sizeof(BoundaryDiffuse) }) ||
            !CreateComputePipeline(device, 2, 1, nullptr, boundaryFitRoot, boundaryFitPipeline,
                                   false, 20u, { BoundaryFit, sizeof(BoundaryFit) }) ||
            !CreateComputePipeline(device, 3, 1, nullptr, boundaryApplyRoot, boundaryApplyPipeline,
                                   false, 20u, { BoundaryApply, sizeof(BoundaryApply) }))
        {
            LOG_WARN("[DLSSNR_EXTERIOR] boundary shader setup failed; keeping ROI-only blend");
            return false;
        }
        LOG_INFO("[DLSSNR_EXTERIOR] algorithm=edge-only-v11 gridPitch=32 momentLayers=11 "
                 "spatialPasses=2 maxEdgeWidth=512 edgeHistory=0.55 boundaryHistoryRamp=96 "
                 "sampling=fixed-2x2 guides=hue-distribution-colour-brightness sampleFade=32 fit=perceptual-gain-offset "
                 "extension=local-edge-only decayHalfDistance=192 endFadeMax=128 source=final-restored-reconstructed-ROI");
        return true;
    }

    bool CanUseExtrapolationInPlace(ID3D12Device* device, DXGI_FORMAT format)
    {
        if (device == nullptr || format == DXGI_FORMAT_UNKNOWN) return false;
        if (format != extrapolationInPlaceFormat)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support { format, D3D12_FORMAT_SUPPORT1_NONE,
                                                        D3D12_FORMAT_SUPPORT2_NONE };
            extrapolationInPlaceSupported = SUCCEEDED(device->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
                (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
            extrapolationInPlaceFormat = format;
        }
        if (!extrapolationInPlaceSupported) return false;
        if (extrapolationInPlacePipeline == nullptr && !extrapolationInPlaceAttempted)
        {
            extrapolationInPlaceAttempted = true;
            using namespace DlssNrExtrapolation::Shaders;
            if (!CreateComputePipeline(device, 3, 1, nullptr,
                    extrapolationInPlaceRoot, extrapolationInPlacePipeline, false,
                    20u, { BoundaryApplyInPlace, sizeof(BoundaryApplyInPlace) }))
                LOG_WARN("[DLSSNR_EXTERIOR] typed in-place pipeline unavailable; using boundary-region staging");
        }
        return extrapolationInPlacePipeline != nullptr;
    }

    bool ApplyExtrapolation(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                            ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* destination,
                            DXGI_FORMAT destinationFormat, ExtrapolationConstants constants)
    {
        bool historyValid = boundaryHistoryValid;
        boundaryHistoryValid = false; // Any failure invalidates the next use.
        const bool inPlace = original == destination;
        if (!device || !commandList || !model || !original || !destination || model == destination ||
            constants.width == 0 || constants.height == 0 || constants.roiWidth == 0 ||
            constants.roiHeight == 0 || !EnsureExtrapolationPipelines(device))
            return false;
        if (inPlace && (constants.originalBaseX != constants.destinationBaseX ||
                        constants.originalBaseY != constants.destinationBaseY ||
                        !CanUseExtrapolationInPlace(device, destinationFormat)))
            return false;
        if (!ValidReadRect(model, 0, 0, constants.roiWidth, constants.roiHeight) ||
            !ValidReadRect(original, constants.originalBaseX, constants.originalBaseY, constants.width, constants.height) ||
            !ValidReadRect(destination, constants.destinationBaseX, constants.destinationBaseY, constants.width, constants.height) ||
            uint64_t(constants.roiOffsetX) + constants.roiWidth > constants.width ||
            uint64_t(constants.roiOffsetY) + constants.roiHeight > constants.height)
            return false;

        constexpr UINT pitch = 32u;
        BoundaryConstants c { constants.width, constants.height, constants.destinationBaseX, constants.destinationBaseY,
            constants.originalBaseX, constants.originalBaseY, constants.roiOffsetX, constants.roiOffsetY,
            constants.roiWidth, constants.roiHeight, constants.edgeBlendData,
            static_cast<UINT>(std::clamp(Config::Instance()->DLSSNRGazeRoiExtrapolationDistancePx.value_or_default(), 0, 512)),
            0, 0, constants.destinationBaseX, constants.destinationBaseY };
        // The low-resolution domain is the entire destination texture, fixed
        // across gaze movement. A moving cropped diffusion domain would discard
        // different paths whenever its grid origin crossed a lattice cell.
        const auto destinationDesc = destination->GetDesc();
        c.gridWidth = (static_cast<UINT>(destinationDesc.Width) + pitch - 1u) / pitch + 1u;
        c.gridHeight = (destinationDesc.Height + pitch - 1u) / pitch + 1u;
        c.tileWidth = c.gridWidth;
        c.tileHeight = c.gridHeight;
        const UINT allocationWidth = c.tileWidth * 4u;
        if (allocationWidth > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            c.tileHeight * 3u > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) return false;
        for (UINT i = 0; i < boundaryFields.size(); ++i)
        {
            const UINT allocationHeight = c.tileHeight * 3u;
            auto& field = boundaryFields[i];
            if (field && (field->GetDesc().Width != allocationWidth || field->GetDesc().Height != allocationHeight))
            {
                DeferResourceRelease(field);
                boundaryReadable[i] = false;
                historyValid = false;
            }
            if (!field && !CreateTexture(device, kBoundaryFormat, allocationWidth, allocationHeight,
                                         L"OptiScaler_DLSSNR_GuardedPerceptualProjection", field))
                return false;
        }
        const auto& prev = boundaryPrevious;
        // The expanded copy rectangle moves with gaze. The lattice/history is
        // anchored to the destination texture, not to that changing subrect.
        historyValid = historyValid && boundaryReadable[3] && boundaryHistoryFormat == destinationFormat &&
            boundaryDestinationWidth == destinationDesc.Width && boundaryDestinationHeight == destinationDesc.Height &&
            prev.roiWidth == c.roiWidth && prev.roiHeight == c.roiHeight &&
            prev.outerWidth == c.outerWidth && prev.edgeBlendData == c.edgeBlendData;
        const float dx = float(c.destinationX) + float(c.roiX) - float(prev.destinationX) - float(prev.roiX);
        const float dy = float(c.destinationY) + float(c.roiY) - float(prev.destinationY) - float(prev.roiY);
        const float displacement2 = dx * dx + dy * dy;
        c.historyWeight = historyValid && displacement2 < 128.0f * 128.0f
            ? 0.55f * std::exp2(-displacement2 / (32.0f * 32.0f)) : 0.0f;
        // Complete setup before recording any dispatch; failure can safely use
        // the ordinary ROI blend. Never overwrite another recorded pass's table.
        if (!CreateSrv(device, model, kBoundarySeedDescriptorBase) ||
            !CreateSrv(device, original, kBoundarySeedDescriptorBase + 1)) return false;
        CreateUav(device, boundaryFields[0].Get(), kBoundaryFormat, kBoundarySeedDescriptorBase + 2);
        auto passOutput = [](UINT pass) -> UINT { return 1u + (pass & 1u); };
        for (UINT pass = 0; pass < kBoundaryPassCount; ++pass)
        {
            const UINT input = pass == 0u ? 0u : passOutput(pass - 1u);
            const UINT output = passOutput(pass);
            const UINT table = kBoundaryDiffuseDescriptorBase + pass * 2u;
            if (!CreateSrv(device, boundaryFields[input].Get(), table)) return false;
            CreateUav(device, boundaryFields[output].Get(), kBoundaryFormat, table + 1);
        }
        const UINT finalField = passOutput(kBoundaryPassCount - 1u);
        if (!CreateSrv(device, boundaryFields[finalField].Get(), kBoundaryFitDescriptorBase) ||
            !CreateSrv(device, boundaryFields[historyValid ? 3 : finalField].Get(), kBoundaryFitDescriptorBase + 1)) return false;
        CreateUav(device, boundaryFields[0].Get(), kBoundaryFormat, kBoundaryFitDescriptorBase + 2);
        if (!CreateSrv(device, model, kBoundaryApplyDescriptorBase) ||
            !CreateSrv(device, inPlace ? model : original, kBoundaryApplyDescriptorBase + 1) ||
            !CreateSrv(device, boundaryFields[0].Get(), kBoundaryApplyDescriptorBase + 2)) return false;
        CreateUav(device, destination, destinationFormat, kBoundaryApplyDescriptorBase + 3);

        if (inPlace)
            Transition(commandList, destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        auto writable = [&](UINT index) {
            if (boundaryReadable[index])
                Transition(commandList, boundaryFields[index].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            boundaryReadable[index] = false;
        };
        auto readable = [&](UINT index) {
            Transition(commandList, boundaryFields[index].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            boundaryReadable[index] = true;
        };
        writable(0);
        commandList->SetComputeRootSignature(boundarySeedRoot.Get());
        commandList->SetPipelineState(boundarySeedPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kBoundarySeedDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, 20u, &c, 0);
        commandList->Dispatch(c.gridWidth, c.gridHeight, 1);
        readable(0);
        commandList->SetComputeRootSignature(boundaryDiffuseRoot.Get());
        commandList->SetPipelineState(boundaryDiffusePipeline.Get());
        for (UINT pass = 0; pass < kBoundaryPassCount; ++pass)
        {
            const UINT output = passOutput(pass);
            writable(output);
            // Same 160px interior support, now horizontal then vertical.
            c.step = pass;
            commandList->SetComputeRootDescriptorTable(0, GpuHandle(kBoundaryDiffuseDescriptorBase + pass * 2u));
            commandList->SetComputeRoot32BitConstants(1, 20u, &c, 0);
            commandList->Dispatch((c.gridWidth + 7u) / 8u, (c.gridHeight + 7u) / 8u, kBoundaryLayerCount);
            readable(output);
        }
        // Neighbourhood reads of the game output are finished before the
        // in-place pass. Its only game-output read is its own pixel's UAV load.
        // Reuse the consumed seed atlas for the single eleven-record local map.
        writable(0);
        commandList->SetComputeRootSignature(boundaryFitRoot.Get());
        commandList->SetPipelineState(boundaryFitPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kBoundaryFitDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, 20u, &c, 0);
        commandList->Dispatch((c.gridWidth + 7u) / 8u, (c.gridHeight + 7u) / 8u, 1);
        readable(0);
        if (inPlace)
            Transition(commandList, destination, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->SetComputeRootSignature(inPlace ? extrapolationInPlaceRoot.Get() : boundaryApplyRoot.Get());
        commandList->SetPipelineState(inPlace ? extrapolationInPlacePipeline.Get() : boundaryApplyPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kBoundaryApplyDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, 20u, &c, 0);
        commandList->Dispatch((c.width + 7u) / 8u, (c.height + 7u) / 8u, 1);
        std::swap(boundaryFields[0], boundaryFields[3]);
        std::swap(boundaryReadable[0], boundaryReadable[3]);
        boundaryPrevious = c;
        boundaryHistoryFormat = destinationFormat;
        boundaryDestinationWidth = destinationDesc.Width;
        boundaryDestinationHeight = destinationDesc.Height;
        boundaryLastAttempt = boundaryAttempt;
        boundaryHistoryValid = true;
        return true;
    }

    bool ApplyColorCopy(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                        ID3D12Resource* source, ID3D12Resource* destination,
                        unsigned int destinationBaseX, unsigned int destinationBaseY,
                        unsigned int width, unsigned int height, DXGI_FORMAT destinationFormat,
                        unsigned int sourceBaseX = 0, unsigned int sourceBaseY = 0,
                        UINT descriptorBase = kFormatConversionDescriptorBase, bool sanitizeScene = false,
                        ID3D12Resource* blendOriginal = nullptr, unsigned int edgeBlendPx = 0,
                        unsigned int edgeBlendMask = 0, float extrapolationScale = 0.0f,
                        unsigned int extrapolationRoiOffsetX = 0,
                        unsigned int extrapolationRoiOffsetY = 0,
                        unsigned int extrapolationRoiWidth = 0,
                        unsigned int extrapolationRoiHeight = 0,
                        unsigned int extrapolationOriginalBaseX = 0,
                        unsigned int extrapolationOriginalBaseY = 0)
    {
        if (device == nullptr || commandList == nullptr || source == nullptr || destination == nullptr ||
            width == 0 || height == 0)
            return false;
        const auto destinationDesc = destination->GetDesc();
        if (!IsShaderReadableTexture(destinationDesc) ||
            static_cast<uint64_t>(destinationBaseX) + width > destinationDesc.Width ||
            static_cast<uint64_t>(destinationBaseY) + height > destinationDesc.Height)
            return false;

        const bool extrapolate = std::isfinite(extrapolationScale) && extrapolationScale > 0.0f &&
            edgeBlendPx != 0 && blendOriginal != nullptr;
        if (edgeBlendPx != 0)
        {
            if (blendOriginal == nullptr || (blendOriginal == destination && !extrapolate))
                return false;
            const auto originalDesc = blendOriginal->GetDesc();
            if (!IsShaderReadableTexture(originalDesc) || width > originalDesc.Width || height > originalDesc.Height)
                return false;
        }
        const unsigned int roiWidth = extrapolationRoiWidth != 0 ? extrapolationRoiWidth : width;
        const unsigned int roiHeight = extrapolationRoiHeight != 0 ? extrapolationRoiHeight : height;
        if (extrapolate)
        {
            ExtrapolationConstants constants { width, height, destinationBaseX, destinationBaseY,
                blendOriginal == destination ? destinationBaseX : extrapolationOriginalBaseX,
                blendOriginal == destination ? destinationBaseY : extrapolationOriginalBaseY,
                extrapolationRoiOffsetX, extrapolationRoiOffsetY, roiWidth, roiHeight,
                extrapolationScale, edgeBlendPx | ((edgeBlendMask & 0xfu) << 16u) };
            if (ApplyExtrapolation(device, commandList, source, blendOriginal, destination,
                                   destinationFormat, constants))
            {
                extrapolationFailureLogged = false;
                return true;
            }
            if (!extrapolationFailureLogged)
                LOG_WARN("[DLSSNR_EXTERIOR] boundary residual field unavailable; keeping normal ROI-only edge blend");
            extrapolationFailureLogged = true;
            return roiColor != nullptr &&
                ApplyColorCopy(device, commandList, source, destination,
                    destinationBaseX + extrapolationRoiOffsetX, destinationBaseY + extrapolationRoiOffsetY,
                    roiWidth, roiHeight, destinationFormat, sourceBaseX, sourceBaseY,
                    descriptorBase, sanitizeScene, roiColor.Get(), edgeBlendPx, edgeBlendMask);
        }
        if (!CreateSrv(device, source, descriptorBase) ||
            !CreateSrv(device, source, descriptorBase + 1) ||
            !CreateSrv(device, edgeBlendPx != 0 ? blendOriginal : source, descriptorBase + 2))
            return false;
        CreateUav(device, destination, destinationFormat, descriptorBase + 3);
        ColorClampConstants constants { width, height, sourceBaseX, sourceBaseY, width, height,
                                        destinationBaseX, destinationBaseY, 1.0f, ColorTransferStrength(), 1.0f,
                                        sanitizeScene ? 6u : 3u, 1.0f, 1.0f, 0u,
                                        edgeBlendPx | ((edgeBlendMask & 0xfu) << 16u) };
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(colorClampRoot.Get());
        commandList->SetPipelineState(colorClampPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(descriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kColorClampConstantCount, &constants, 0);
        commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        return true;
    }

    bool PrepareRestoreSource(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                              ID3D12Resource* source, unsigned int baseX, unsigned int baseY,
                              unsigned int textureWidth, unsigned int textureHeight, bool prepareOnly = false)
    {
        if (restoreSource != nullptr &&
            (restoreSource->GetDesc().Width != textureWidth || restoreSource->GetDesc().Height != textureHeight))
        {
            DeferResourceRelease(restoreSource);
            restoreSourceReadable = false;
        }
        if (restoreSource == nullptr &&
            !CreateTexture(device, kDlssNrFormat, textureWidth, textureHeight,
                           L"OptiScaler_DLSSNR_Restore_Source", restoreSource))
            return false;
        if (restoreSourceReadable)
        {
            Transition(commandList, restoreSource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            restoreSourceReadable = false;
        }
        if (prepareOnly)
            return true;
        if (!ApplyColorCopy(device, commandList, source, restoreSource.Get(), 0, 0,
                            textureWidth, textureHeight, kDlssNrFormat, baseX, baseY,
                            kRestoreSourceDescriptorBase, true))
            return false;
        Transition(commandList, restoreSource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        restoreSourceReadable = true;
        return true;
    }

    bool PrepareRoiColor(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                         ID3D12Resource* source, unsigned int sourceBaseX, unsigned int sourceBaseY,
                         unsigned int sourceWidth, unsigned int sourceHeight, bool prepareOnly = false)
    {
        if (device == nullptr || commandList == nullptr || source == nullptr || sourceWidth == 0 || sourceHeight == 0)
            return false;
        const auto sourceDesc = source->GetDesc();
        if (!IsShaderReadableTexture(sourceDesc) ||
            static_cast<uint64_t>(sourceBaseX) + sourceWidth > sourceDesc.Width ||
            static_cast<uint64_t>(sourceBaseY) + sourceHeight > sourceDesc.Height)
            return false;
        if (roiColor != nullptr &&
            (roiColor->GetDesc().Width != sourceWidth || roiColor->GetDesc().Height != sourceHeight))
        {
            // Previously recorded frames may still read the old allocation.
            DeferResourceRelease(roiColor);
            roiColorReadable = false;
        }
        if (roiColor == nullptr &&
            !CreateTexture(device, kDlssNrFormat, sourceWidth, sourceHeight,
                           L"OptiScaler_DLSSNR_ROI_Color", roiColor))
            return false;
        if (roiColorReadable)
        {
            Transition(commandList, roiColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            roiColorReadable = false;
        }
        if (prepareOnly)
            return true;
        if (!ApplyColorCopy(device, commandList, source, roiColor.Get(), 0, 0,
                            sourceWidth, sourceHeight, kDlssNrFormat, sourceBaseX, sourceBaseY,
                            kRoiColorDescriptorBase))
            return false;
        Transition(commandList, roiColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        roiColorReadable = true;
        return true;
    }

    bool PrepareExtrapolationOriginal(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                                      ID3D12Resource* source, unsigned int sourceBaseX, unsigned int sourceBaseY,
                                      unsigned int width, unsigned int height,
                                      unsigned int allocationWidth, unsigned int allocationHeight)
    {
        if (device == nullptr || commandList == nullptr || source == nullptr || width == 0 || height == 0)
            return false;
        const auto sourceDesc = source->GetDesc();
        if (!IsShaderReadableTexture(sourceDesc) ||
            static_cast<uint64_t>(sourceBaseX) + width > sourceDesc.Width ||
            static_cast<uint64_t>(sourceBaseY) + height > sourceDesc.Height)
            return false;
        if (allocationWidth < width || allocationHeight < height || sourceDesc.DepthOrArraySize != 1)
            return false;
        if (extrapolationOriginal != nullptr &&
            (extrapolationOriginal->GetDesc().Width != allocationWidth ||
             extrapolationOriginal->GetDesc().Height != allocationHeight ||
             extrapolationOriginal->GetDesc().Format != sourceDesc.Format))
        {
            DeferResourceRelease(extrapolationOriginal);
            extrapolationOriginalReadable = false;
        }
        if (extrapolationOriginal == nullptr &&
            !CreateTexture(device, sourceDesc.Format, allocationWidth, allocationHeight,
                           L"OptiScaler_DLSSNR_Extrapolation_Original", extrapolationOriginal))
            return false;
        Transition(commandList, extrapolationOriginal.Get(), extrapolationOriginalReadable
            ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(commandList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const CD3DX12_TEXTURE_COPY_LOCATION from(source, 0), to(extrapolationOriginal.Get(), 0);
        const D3D12_BOX box { sourceBaseX, sourceBaseY, 0u, sourceBaseX + width, sourceBaseY + height, 1u };
        commandList->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        Transition(commandList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, extrapolationOriginal.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        extrapolationOriginalReadable = true;
        return true;
    }

    bool PrepareLowResolutionColor(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                                   ID3D12Resource* source, unsigned int sourceBaseX, unsigned int sourceBaseY,
                                   unsigned int sourceWidth, unsigned int sourceHeight, unsigned int scale,
                                   float paperWhite, ID3D12Resource* exposure, float preExposure,
                                   float exposureScale, bool useExposure, bool sdrToneMapping)
    {
        if (device == nullptr || commandList == nullptr || source == nullptr ||
            lowColor == nullptr || lowBaseline == nullptr || scale < 2 || scale > 3 ||
            sourceWidth == 0 || sourceHeight == 0 ||
            lowWidth != (sourceWidth + scale - 1) / scale ||
            lowHeight != (sourceHeight + scale - 1) / scale)
            return false;
        if (!ValidReadRect(source, sourceBaseX, sourceBaseY, sourceWidth, sourceHeight) ||
            !EnsureLowDownsamplePipeline(device, scale))
            return false;
        if (lowColorReadable)
            Transition(commandList, lowColor.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (lowBaselineReadable)
            Transition(commandList, lowBaseline.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lowColorReadable = false;
        lowBaselineReadable = false;
        if (!CreateSrv(device, source, kFusedDownsampleDescriptorBase) ||
            !CreateExposureSrv(device, source, exposure, kFusedDownsampleDescriptorBase + 1, useExposure))
            return false;
        CreateUav(device, lowBaseline.Get(), kDlssNrFormat, kFusedDownsampleDescriptorBase + 2);
        CreateUav(device, lowColor.Get(), kDlssNrFormat, kFusedDownsampleDescriptorBase + 3);
        LowDownsampleConstants constants { sourceBaseX, sourceBaseY, sourceWidth, sourceHeight,
                                           lowWidth, lowHeight, scale, sdrToneMapping ? 2u : 1u,
                                           paperWhite, ColorTransferStrength(), 1.0f, useExposure ? 1u : 0u, preExposure, exposureScale };
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(lowDownsampleRoots[scale - 2].Get());
        commandList->SetPipelineState(lowDownsamplePipelines[scale - 2].Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kFusedDownsampleDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kGuidanceConstantCount, &constants, 0);
        D3D12_RESOURCE_STATES exposureRestoreState = D3D12_RESOURCE_STATE_COMMON;
        const bool exposureTransitioned = PrepareExposureRead(commandList, exposure, useExposure,
                                                               &exposureRestoreState);
        commandList->Dispatch((lowWidth + 7) / 8, (lowHeight + 7) / 8, 1);
        RestoreExposureState(commandList, exposure, exposureTransitioned, exposureRestoreState);
        Transition(commandList, lowColor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, lowBaseline.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lowColorReadable = true;
        lowBaselineReadable = true;
        return true;
    }

    bool PrepareLowResidual(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                            ID3D12Resource* nrOutput, bool nrOutputWritable, bool zeroResidual)
    {
        if (nrOutput == nullptr || lowBaseline == nullptr || lowResidual == nullptr)
            return false;
        if (lowResidualPipeline == nullptr &&
            !CreateComputePipeline(device, 2, 1, kLowResidualShader, lowResidualRoot, lowResidualPipeline))
            return false;
        if (nrOutputWritable)
            Transition(commandList, nrOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (lowResidualReadable)
            Transition(commandList, lowResidual.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!CreateSrv(device, nrOutput, kLowResidualDescriptorBase) ||
            !CreateSrv(device, lowBaseline.Get(), kLowResidualDescriptorBase + 1))
            return false;
        CreateUav(device, lowResidual.Get(), kDlssNrFormat, kLowResidualDescriptorBase + 2);
        LowResidualConstants constants { lowWidth, lowHeight, 1.0f, 1.0f, 1.0f,
                                         0u, zeroResidual ? 1u : 0u };
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(lowResidualRoot.Get());
        commandList->SetPipelineState(lowResidualPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kLowResidualDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kGuidanceConstantCount, &constants, 0);
        commandList->Dispatch((lowWidth + 7) / 8, (lowHeight + 7) / 8, 1);
        Transition(commandList, lowResidual.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lowResidualReadable = true;
        return true;
    }

    void ResetTemporalHistory()
    {
        temporalHistoryValid = false;
        boundaryHistoryValid = false;
    }

    void ReleaseOutputHistory()
    {
        for (auto& resource : outputHistory) DeferResourceRelease(resource);
        for (auto& resource : outputSourceHistory) DeferResourceRelease(resource);
        for (auto& resource : outputDepthHistory) DeferResourceRelease(resource);
        outputHistoryReadable = {};
        outputHistoryValid = false;
        outputHistoryWidth = outputHistoryHeight = 0;
    }

    static bool ValidReadRect(ID3D12Resource* resource, UINT x, UINT y, UINT w, UINT h)
    {
        if (resource == nullptr || w == 0 || h == 0) return false;
        const auto desc = resource->GetDesc();
        return IsShaderReadableTexture(desc) && desc.DepthOrArraySize == 1 &&
            uint64_t(x) + w <= desc.Width && uint64_t(y) + h <= desc.Height;
    }

    bool StabilizeOutput(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                         ID3D12Resource* output, ID3D12Resource* modelMotion,
                         ID3D12Resource* modelDepth, ID3D12Resource* modelSource,
                         OutputTemporalConstants constants, bool reset,
                         float jitterX, float jitterY, ID3D12Resource** stabilizedOutput)
    {
        if (stabilizedOutput == nullptr || !ValidReadRect(modelMotion, constants.motionBaseX, constants.motionBaseY,
                           constants.motionWidth, constants.motionHeight) ||
            !ValidReadRect(modelDepth, constants.depthBaseX, constants.depthBaseY,
                           constants.depthWidth, constants.depthHeight) ||
            !ValidReadRect(modelSource, constants.sourceBaseX, constants.sourceBaseY,
                           constants.sourceWidth, constants.sourceHeight)) return false;
        if (outputTemporalPipeline == nullptr &&
            !CreateComputePipeline(device, 7, 3, kOutputTemporalShader,
                                   outputTemporalRoot, outputTemporalPipeline, true, 24)) return false;
        if (outputHistoryWidth != constants.width || outputHistoryHeight != constants.height ||
            outputHistory[0] == nullptr || outputHistory[1] == nullptr ||
            outputSourceHistory[0] == nullptr || outputSourceHistory[1] == nullptr ||
            outputDepthHistory[0] == nullptr || outputDepthHistory[1] == nullptr)
        {
            ReleaseOutputHistory();
            for (UINT i = 0; i < 2; ++i)
                if (!CreateTexture(device, kDlssNrFormat, constants.width, constants.height,
                                   L"OptiScaler_DLSSNR_Output_History", outputHistory[i]) ||
                    !CreateTexture(device, kDlssNrFormat, constants.width, constants.height,
                                   L"OptiScaler_DLSSNR_Output_Source_History", outputSourceHistory[i]) ||
                    !CreateTexture(device, DXGI_FORMAT_R32_FLOAT, constants.width, constants.height,
                                   L"OptiScaler_DLSSNR_Output_Depth_History", outputDepthHistory[i])) return false;
            outputHistoryWidth = constants.width;
            outputHistoryHeight = constants.height;
        }
        constants.historyValid = outputHistoryValid && !reset ? 1u : 0u;
        const UINT previous = outputHistoryIndex;
        const UINT next = outputHistoryValid ? 1u - previous : 0u;
        // Use current inputs as harmless SRV bindings on the first frame,
        // avoiding an SRV/UAV alias of the history being initialized.
        ID3D12Resource* reads[] { output, modelMotion, modelDepth, modelSource,
            constants.historyValid ? outputHistory[previous].Get() : output,
            constants.historyValid ? outputSourceHistory[previous].Get() : modelSource,
            constants.historyValid ? outputDepthHistory[previous].Get() : modelDepth };
        for (UINT i = 0; i < 7; ++i)
            if (!CreateSrv(device, reads[i], kOutputTemporalDescriptorBase + i)) return false;
        ID3D12Resource* writes[] { outputHistory[next].Get(), outputSourceHistory[next].Get(),
                                  outputDepthHistory[next].Get() };
        for (UINT i = 0; i < 3; ++i)
        {
            if (outputHistoryReadable[next])
                Transition(commandList, writes[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            CreateUav(device, writes[i], i == 2 ? DXGI_FORMAT_R32_FLOAT : kDlssNrFormat,
                      kOutputTemporalDescriptorBase + 7 + i);
        }
        Transition(commandList, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap* heap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &heap);
        commandList->SetComputeRootSignature(outputTemporalRoot.Get());
        commandList->SetPipelineState(outputTemporalPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kOutputTemporalDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, 24, &constants, 0);
        commandList->Dispatch((constants.width + 7) / 8, (constants.height + 7) / 8, 1);
        // Consumers can read the newly written history directly. Feature 18
        // keeps its own output allocation/UAV state for the next evaluation.
        Transition(commandList, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (UINT i = 0; i < 3; ++i)
            Transition(commandList, writes[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        *stabilizedOutput = writes[0];
        outputHistoryReadable[next] = true;
        outputHistoryIndex = next;
        outputHistoryValid = true;
        outputPreviousJitterX = jitterX;
        outputPreviousJitterY = jitterY;
        return true;
    }

    bool ResolveTemporalResidual(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                                 ID3D12Resource* sourceMotion, ID3D12Resource* sourceDepth,
                                 TemporalResidualConstants constants,
                                 ID3D12Resource** resolvedResidual)
    {
        if (device == nullptr || commandList == nullptr || resolvedResidual == nullptr ||
            lowResidual == nullptr || constants.width != lowWidth || constants.height != lowHeight ||
            !ValidReadRect(sourceMotion, constants.motionBaseX, constants.motionBaseY,
                           constants.motionWidth, constants.motionHeight) ||
            !ValidReadRect(sourceDepth, constants.depthBaseX, constants.depthBaseY,
                           constants.depthWidth, constants.depthHeight) ||
            temporalResidual[0] == nullptr || temporalResidual[1] == nullptr ||
            temporalDepth[0] == nullptr || temporalDepth[1] == nullptr)
            return false;

        if (temporalResidualPipeline == nullptr &&
            !CreateComputePipeline(device, 5, 2, kTemporalResidualShader,
                                   temporalResidualRoot, temporalResidualPipeline))
            return false;
        const unsigned int previousIndex = temporalHistoryIndex;
        const unsigned int nextIndex = temporalHistoryValid ? 1u - temporalHistoryIndex : 0u;
        if (temporalResidualReadable[nextIndex])
        {
            Transition(commandList, temporalResidual[nextIndex].Get(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            temporalResidualReadable[nextIndex] = false;
        }
        if (temporalDepthReadable[nextIndex])
        {
            Transition(commandList, temporalDepth[nextIndex].Get(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            temporalDepthReadable[nextIndex] = false;
        }

        ID3D12Resource* previousResidual = temporalHistoryValid
                                               ? temporalResidual[previousIndex].Get()
                                               : lowResidual.Get();
        ID3D12Resource* previousDepth = temporalHistoryValid
                                            ? temporalDepth[previousIndex].Get()
                                            : sourceDepth;
        if (!CreateSrv(device, lowResidual.Get(), kTemporalResidualDescriptorBase) ||
            !CreateSrv(device, sourceMotion, kTemporalResidualDescriptorBase + 1) ||
            !CreateSrv(device, sourceDepth, kTemporalResidualDescriptorBase + 2) ||
            !CreateSrv(device, previousResidual, kTemporalResidualDescriptorBase + 3) ||
            !CreateSrv(device, previousDepth, kTemporalResidualDescriptorBase + 4))
            return false;
        CreateUav(device, temporalResidual[nextIndex].Get(), kDlssNrFormat,
                  kTemporalResidualDescriptorBase + 5);
        CreateUav(device, temporalDepth[nextIndex].Get(), DXGI_FORMAT_R32_FLOAT,
                  kTemporalResidualDescriptorBase + 6);
        constants.historyValid = temporalHistoryValid ? 1u : 0u;
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(temporalResidualRoot.Get());
        commandList->SetPipelineState(temporalResidualPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kTemporalResidualDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kGuidanceConstantCount, &constants, 0);
        commandList->Dispatch((lowWidth + 7) / 8, (lowHeight + 7) / 8, 1);
        Transition(commandList, temporalResidual[nextIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, temporalDepth[nextIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        temporalResidualReadable[nextIndex] = true;
        temporalDepthReadable[nextIndex] = true;
        temporalHistoryIndex = nextIndex;
        temporalHistoryValid = true;
        *resolvedResidual = temporalResidual[nextIndex].Get();
        return true;
    }

    bool EnsureFastComposite(ID3D12Device* device)
    {
        auto& pipeline = displayHDRColorTransfer ? displayFastCompositePipeline : fastCompositePipeline;
        auto& root = displayHDRColorTransfer ? displayFastCompositeRoot : fastCompositeRoot;
        auto& attempted = displayHDRColorTransfer ? displayFastCompositeAttempted : fastCompositeAttempted;
        if (pipeline != nullptr)
            return true;
        if (attempted)
            return false;
        attempted = true;
        const std::string source = std::string("#define DLSSNR_FAST_COMPOSITE 1\n") +
            (displayHDRColorTransfer ? "#define DLSSNR_DISPLAY_COMPOSITE 1\n" : "") + kLowCompositeShader;
        if (!CreateComputePipeline(device, 3, 1, source.c_str(), root, pipeline))
        {
            pipeline.Reset();
            root.Reset();
            LOG_WARN("[DLSSNR_LOWRES] fast reconstruction shader unavailable; retaining quality A (no per-frame retry)");
            return false;
        }
        return true;
    }

    bool ApplyLowResidual(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                          ID3D12Resource* baseColor, ID3D12Resource* correction, unsigned int compositeMode,
                          bool temporalReconstruction, bool highResolutionGuided,
                          bool featureGuided, bool fastReconstruction,
                          unsigned int baseSourceX, unsigned int baseSourceY,
                          unsigned int destinationBaseX, unsigned int destinationBaseY,
                          unsigned int highWidth, unsigned int highHeight, unsigned int sourceScale,
                          ID3D12Resource** compositeOutput)
    {
        if (compositeOutput == nullptr || baseColor == nullptr || correction == nullptr ||
            highComposite == nullptr || (compositeMode == 0 && lowResidual == nullptr))
            return false;
        if (highCompositeReadable)
        {
            Transition(commandList, highComposite.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            highCompositeReadable = false;
        }
        if (!CreateSrv(device, baseColor, kLowCompositeDescriptorBase) ||
            !CreateSrv(device, lowBaseline.Get(), kLowCompositeDescriptorBase + 1) ||
            !CreateSrv(device, correction, kLowCompositeDescriptorBase + 2))
            return false;
        CreateUav(device, highComposite.Get(), kDlssNrFormat, kLowCompositeDescriptorBase + 3);
        LowCompositeConstants constants { highWidth, highHeight, lowWidth, lowHeight,
                                          baseSourceX, baseSourceY,
                                          destinationBaseX, destinationBaseY,
                                          compositeMode, temporalReconstruction ? 1u : 0u,
                                          highResolutionGuided ? 1u : 0u,
                                          featureGuided ? 1u : 0u, std::max(sourceScale, 1u) };
        constants.displayHDR = displayHDRColorTransfer ? 1u : 0u;
        constants.displayPaperWhite = displayPaperWhite;
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        const bool useFastPipeline = fastReconstruction && featureGuided && compositeMode == 0 &&
                                     sourceScale >= 2 && sourceScale <= 3 &&
                                     EnsureFastComposite(device);
        // Both spatial PSOs consume the same resources/root constants. Do not
        // reset NGX or either temporal history for an A/B composition change.
        const int variant = compositeMode == 2 ? 3 : compositeMode == 1 ? 2 :
                            featureGuided ? (useFastPipeline ? 1 : 0) : highResolutionGuided ? 4 : 5;
        const int compositeDomain = displayHDRColorTransfer ? 2 : identityColorTransfer ? 1 : 0;
        if (variant != lastCompositeVariant || compositeDomain != lastCompositeDomain)
        {
            const char* methods[] { "quality-5x5", "fast-3x3", "full-output",
                                    "model-preview", "legacy-guided", "legacy-residual" };
            const char* domains[] { "scene-linear", "display-sdr", "display-linear-hdr-bounded-edit" };
            LOG_INFO("[DLSSNR_LOWRES] reconstruction={} fastRequested={} output={}x{} scale={} domain={}",
                     methods[variant], fastReconstruction, highWidth, highHeight, sourceScale, domains[compositeDomain]);
            lastCompositeVariant = variant;
            lastCompositeDomain = compositeDomain;
        }
        auto& root = displayHDRColorTransfer ? (useFastPipeline ? displayFastCompositeRoot : displayCompositeRoot) :
                                              (useFastPipeline ? fastCompositeRoot : lowCompositeRoot);
        auto& pipeline = displayHDRColorTransfer ? (useFastPipeline ? displayFastCompositePipeline : displayCompositePipeline) :
                                                  (useFastPipeline ? fastCompositePipeline : lowCompositePipeline);
        if (!useFastPipeline && pipeline == nullptr)
        {
            const std::string source = std::string(displayHDRColorTransfer ? "#define DLSSNR_DISPLAY_COMPOSITE 1\n" : "") +
                                       kLowCompositeShader;
            if (!CreateComputePipeline(device, 3, 1, source.c_str(), root, pipeline)) return false;
        }
        commandList->SetComputeRootSignature(root.Get());
        commandList->SetPipelineState(pipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kLowCompositeDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kGuidanceConstantCount, &constants, 0);
        commandList->Dispatch((highWidth + 7) / 8, (highHeight + 7) / 8, 1);
        *compositeOutput = highComposite.Get();
        return true;
    }

    bool StageDebug(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                    ID3D12Resource* color, ID3D12Resource* debugMotion, ID3D12Resource* debugDepth,
                    unsigned int colorBaseX, unsigned int colorBaseY,
                    unsigned int colorWidth, unsigned int colorHeight,
                    unsigned int motionBaseX, unsigned int motionBaseY,
                    unsigned int motionWidth, unsigned int motionHeight,
                    unsigned int depthBaseX, unsigned int depthBaseY,
                    unsigned int depthWidth, unsigned int depthHeight, bool depthInverted)
    {
        if (device == nullptr || commandList == nullptr ||
            debugOverlay == nullptr || color == nullptr || debugMotion == nullptr || debugDepth == nullptr ||
            colorWidth == 0 || colorHeight == 0 || motionWidth == 0 || motionHeight == 0 ||
            depthWidth == 0 || depthHeight == 0)
            return false;
        if (debugPipeline == nullptr &&
            !CreateComputePipeline(device, 3, 1, kGuidanceDebugShader, debugRoot, debugPipeline))
            return false;
        const auto colorDesc = color->GetDesc();
        const auto motionDesc = debugMotion->GetDesc();
        const auto depthDesc = debugDepth->GetDesc();
        if (static_cast<uint64_t>(colorBaseX) + colorWidth > colorDesc.Width ||
            static_cast<uint64_t>(colorBaseY) + colorHeight > colorDesc.Height ||
            static_cast<uint64_t>(motionBaseX) + motionWidth > motionDesc.Width ||
            static_cast<uint64_t>(motionBaseY) + motionHeight > motionDesc.Height ||
            static_cast<uint64_t>(depthBaseX) + depthWidth > depthDesc.Width ||
            static_cast<uint64_t>(depthBaseY) + depthHeight > depthDesc.Height ||
            !CreateSrv(device, color, kDebugDescriptorBase) ||
            !CreateSrv(device, debugMotion, kDebugDescriptorBase + 1) ||
            !CreateSrv(device, debugDepth, kDebugDescriptorBase + 2))
            return false;
        CreateUav(device, debugOverlay.Get(), kDlssNrFormat, kDebugDescriptorBase + 3);
        DebugConstants constants { width, height, colorBaseX, colorBaseY, colorWidth, colorHeight,
                                   motionBaseX, motionBaseY, motionWidth, motionHeight,
                                   depthBaseX, depthBaseY, depthWidth, depthHeight,
                                   depthInverted ? 1u : 0u, 0u };
        ID3D12DescriptorHeap* descriptorHeap = heaps[activeHeap].Get();
        commandList->SetDescriptorHeaps(1, &descriptorHeap);
        commandList->SetComputeRootSignature(debugRoot.Get());
        commandList->SetPipelineState(debugPipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, GpuHandle(kDebugDescriptorBase));
        commandList->SetComputeRoot32BitConstants(1, kGuidanceConstantCount, &constants, 0);
        const UINT totalWidth = (width / 3) * 3;
        const UINT panelHeight = std::max(height / 3, 1u);
        commandList->Dispatch((totalWidth + 7) / 8, (panelHeight + 7) / 8, 1);
        return true;
    }

    void CompositeDebug(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                        ID3D12Resource* nrOutput, unsigned int destinationBaseX = 0,
                        unsigned int destinationBaseY = 0)
    {
        if (device == nullptr || commandList == nullptr || debugOverlay == nullptr || nrOutput == nullptr)
            return;
        const auto sourceDesc = debugOverlay->GetDesc();
        const auto destinationDesc = nrOutput->GetDesc();
        const UINT totalWidth = (width / 3) * 3;
        const UINT panelHeight = std::max(height / 3, 1u);
        const UINT left = width - totalWidth;
        const UINT top = height - panelHeight;
        if (totalWidth == 0 || panelHeight == 0 ||
            static_cast<uint64_t>(left) + totalWidth > sourceDesc.Width ||
            static_cast<uint64_t>(top) + panelHeight > sourceDesc.Height ||
            static_cast<uint64_t>(destinationBaseX) + left + totalWidth > destinationDesc.Width ||
            static_cast<uint64_t>(destinationBaseY) + top + panelHeight > destinationDesc.Height)
        {
            return;
        }
        // Use the existing raw format-conversion compute pass instead of a
        // COPY_SOURCE/COPY_DEST operation. The latter is rejected by some
        // games' output resources even though the same resource is valid UAV.
        Transition(commandList, debugOverlay.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const bool copied = ApplyColorCopy(device, commandList, debugOverlay.Get(), nrOutput,
                                           destinationBaseX + left, destinationBaseY + top,
                                           totalWidth, panelHeight, destinationDesc.Format,
                                           left, top, kDebugCompositeDescriptorBase);
        Transition(commandList, debugOverlay.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!copied)
            LOG_WARN("[DLSSNR_GUIDANCE] debug overlay compute composite failed");
    }

    void ReturnGuidanceToWritable(ID3D12GraphicsCommandList* commandList)
    {
        if (lowGuidanceReadable)
        {
            Transition(commandList, lowMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(commandList, lowDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            lowGuidanceReadable = false;
        }
        if (!guidanceReadable)
            return;
        Transition(commandList, motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(commandList, depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        guidanceReadable = false;
    }


};

DLSSNRFeatureDx12::DLSSNRFeatureDx12() = default;

void DLSSNRFeatureDx12::SetParameterUInt(NVSDK_NGX_Parameter* parameters, const char* key,
                                         unsigned int value)
{
    SetRawUInt(parameters, key, value);
    if (parameters == nullptr)
        return;
    unsigned int readBack = 0;
    if (parameters->Get(key, &readBack) != NVSDK_NGX_Result_Success ||
        readBack != value)
        parameters->Set(key, value);
}

void DLSSNRFeatureDx12::SetParameterResource(NVSDK_NGX_Parameter* parameters, const char* key,
                                             ID3D12Resource* value)
{
    // Do not follow this with the public typed overload.  On the driver's
    // capability table that overload addresses a different vtable slot and
    // is exactly the operation that made neutral guides appear to have no
    // effect in the first place.
    SetRawUll(parameters, key, reinterpret_cast<unsigned long long>(value));
}

void DLSSNRFeatureDx12::SetParameterFloat(NVSDK_NGX_Parameter* parameters, const char* key, float value)
{
    if (_floatSetterSlot < 0)
    {
        _floatSetterSlot = DiscoverRawFloatSlot(parameters);
        if (_floatSetterSlot >= 0)
            LOG_INFO("[DLSSNR] native NGX float setter slot={}", _floatSetterSlot);
        else
            LOG_WARN("[DLSSNR] native NGX float setter slot could not be discovered");
    }
    SetRawFloat(parameters, key, value, _floatSetterSlot);
    if (parameters == nullptr)
        return;
    float readBack = 0.0f;
    if (parameters->Get(key, &readBack) != NVSDK_NGX_Result_Success ||
        readBack != value)
        parameters->Set(key, value);
}

DLSSNRFeatureDx12::OriginalGuideDimensions DLSSNRFeatureDx12::GetLastOriginalGuideDimensions()
{
    OriginalGuideDimensions dimensions {};
    dimensions.observed = g_originalGuidesObserved.load(std::memory_order_acquire);
    UnpackDimensions(g_originalMotionLogical.load(std::memory_order_relaxed),
                     dimensions.motionWidth, dimensions.motionHeight);
    UnpackDimensions(g_originalMotionAllocation.load(std::memory_order_relaxed),
                     dimensions.motionAllocationWidth, dimensions.motionAllocationHeight);
    UnpackDimensions(g_originalMotionBase.load(std::memory_order_relaxed),
                     dimensions.motionBaseX, dimensions.motionBaseY);
    UnpackDimensions(g_originalDepthLogical.load(std::memory_order_relaxed),
                     dimensions.depthWidth, dimensions.depthHeight);
    UnpackDimensions(g_originalDepthAllocation.load(std::memory_order_relaxed),
                     dimensions.depthAllocationWidth, dimensions.depthAllocationHeight);
    UnpackDimensions(g_originalDepthBase.load(std::memory_order_relaxed),
                     dimensions.depthBaseX, dimensions.depthBaseY);
    return dimensions;
}

DLSSNRFeatureDx12::~DLSSNRFeatureDx12()
{
    Shutdown(nullptr);
}

void DLSSNRFeatureDx12::LogBypass(const std::string& reason)
{
    if (_guidance != nullptr)
        _guidance->outputHistoryValid = false;
    if (_lastBypassReason == reason)
        return;

    _lastBypassReason = reason;
    LOG_WARN("[DLSSNR] bypassed: {}", reason);
}

void DLSSNRFeatureDx12::LogStatistics(double evaluateCpuMs, double recordCpuMs, uint64_t copiedBytes)
{
    ++_successfulEvaluations;
    _evaluateCpuMs.push_back(evaluateCpuMs);
    _recordCpuMs.push_back(recordCpuMs);
    constexpr size_t kWindow = 120;
    if (_evaluateCpuMs.size() > kWindow)
        _evaluateCpuMs.pop_front();
    if (_recordCpuMs.size() > kWindow)
        _recordCpuMs.pop_front();
    if ((_successfulEvaluations % kWindow) != 0)
        return;

    auto summarize = [](const std::deque<double>& values) {
        std::vector<double> sorted(values.begin(), values.end());
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (double value : sorted)
            sum += value;
        const size_t p95Index = sorted.empty() ? 0 : (sorted.size() - 1) * 95 / 100;
        return std::array<double, 3> { sorted.empty() ? 0.0 : sum / sorted.size(),
                                       sorted.empty() ? 0.0 : sorted[p95Index],
                                       sorted.empty() ? 0.0 : sorted.back() };
    };

    const auto eval = summarize(_evaluateCpuMs);
    const auto record = summarize(_recordCpuMs);
    const double copiedMiB = static_cast<double>(copiedBytes) / (1024.0 * 1024.0);
    LOG_INFO("[DLSSNR_STATS] samples={} attempts={} success={} window={} "
             "evaluateCpuMs(avg/p95/max)={:.4f}/{:.4f}/{:.4f} "
             "recordCpuMs(avg/p95/max)={:.4f}/{:.4f}/{:.4f} "
             "copyMiBPerFrame={:.3f} note=CPU_recording_only_GPU_time_requires_PIX_or_timestamps",
             _successfulEvaluations, _attemptedEvaluations, _successfulEvaluations, _evaluateCpuMs.size(),
             eval[0], eval[1], eval[2], record[0], record[1], record[2], copiedMiB);
}

void DLSSNRFeatureDx12::ReleaseFeature()
{
    if (_guidance != nullptr)
        _guidance->outputHistoryValid = false;
    if (_handle != nullptr && _releaseFeature != nullptr)
    {
        NVSDK_NGX_Handle* retired = _handle;
        _handle = nullptr;
        const auto release = _releaseFeature;
        // DeferCallback runs immediately when no tracked command list still
        // references the handle, and otherwise waits for the queue fence. This
        // avoids a stale per-instance flag deciding whether release is safe.
        GazeRoiFrameSync::DeferCallback([release, retired]() { release(retired); });
        LOG_DEBUG("[DLSSNR_LIFETIME] scheduled feature release handle={:X}",
                  reinterpret_cast<uintptr_t>(retired));
        return;
    }
    _handle = nullptr;
}

bool DLSSNRFeatureDx12::InstallCallerCompatibility()
{
    _getModuleFileNameIatSlot = FindImportedFunctionSlot(_module, "GetModuleFileNameW");
    if (!_getModuleFileNameIatSlot)
    {
        LogBypass("DLSSNR DLL has no GetModuleFileNameW import");
        return false;
    }
    std::scoped_lock lock(g_callerHookMutex);
    if (g_callerHookUsers != 0)
    {
        if (g_callerHookTargetModule != _module || g_callerHookIatSlot != _getModuleFileNameIatSlot)
        {
            LogBypass("DLSSNR caller compatibility is already owned by another DLL");
            return false;
        }
        ++g_callerHookUsers;
        _originalGetModuleFileNameW = g_callerHookOriginal;
        _callerHookInstalled = true;
        LOG_DEBUG("[DLSSNR] caller compatibility reused for shared module session (users={})",
                  g_callerHookUsers);
        return true;
    }
    void* hookAddress = reinterpret_cast<void*>(&CallerCompatibleGetModuleFileNameW);
    HMODULE callerModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(hookAddress), &callerModule))
    {
        LogBypass(std::format("resolve caller compatibility module failed (Win32 {})", GetLastError()));
        return false;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(_getModuleFileNameIatSlot, sizeof(void*), PAGE_READWRITE, &oldProtection))
    {
        LogBypass(std::format("make DLSSNR IAT writable failed (Win32 {})", GetLastError()));
        return false;
    }
    g_callerModule.store(callerModule, std::memory_order_release);
    void* original = InterlockedExchangePointer(reinterpret_cast<void* volatile*>(_getModuleFileNameIatSlot), hookAddress);
    std::memcpy(&_originalGetModuleFileNameW, &original, sizeof(original));
    if (!_originalGetModuleFileNameW)
    {
        DWORD ignored = 0;
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(_getModuleFileNameIatSlot), original);
        VirtualProtect(_getModuleFileNameIatSlot, sizeof(void*), oldProtection, &ignored);
        g_callerModule.store(nullptr, std::memory_order_release);
        LogBypass("DLSSNR IAT GetModuleFileNameW was null");
        return false;
    }
    g_callerHookTargetModule = _module;
    g_callerHookIatSlot = _getModuleFileNameIatSlot;
    g_callerHookOriginal = _originalGetModuleFileNameW;
    g_callerHookUsers = 1;
    g_callerHookOwner.store(this, std::memory_order_release);
    g_originalGetModuleFileNameW.store(_originalGetModuleFileNameW, std::memory_order_release);
    _callerHookInstalled = true;
    DWORD ignored = 0;
    VirtualProtect(_getModuleFileNameIatSlot, sizeof(void*), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), _getModuleFileNameIatSlot, sizeof(void*));
    LOG_DEBUG("[DLSSNR] caller compatibility installed for module nvngx.dll");
    return true;
}

bool DLSSNRFeatureDx12::RestoreCallerCompatibility()
{
    if (!_callerHookInstalled)
        return true;
    std::scoped_lock lock(g_callerHookMutex);
    if (g_callerHookUsers == 0 || g_callerHookIatSlot != _getModuleFileNameIatSlot)
        return false;
    if (g_callerHookUsers > 1)
    {
        --g_callerHookUsers;
        _callerHookInstalled = false;
        _getModuleFileNameIatSlot = nullptr;
        _originalGetModuleFileNameW = nullptr;
        LOG_DEBUG("[DLSSNR] caller compatibility retained for shared module session (users={})",
                  g_callerHookUsers);
        return true;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(g_callerHookIatSlot, sizeof(void*), PAGE_READWRITE, &oldProtection))
    {
        ++g_callerHookUsers;
        return false;
    }
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(g_callerHookIatSlot),
                               reinterpret_cast<void*>(g_callerHookOriginal));
    DWORD ignored = 0;
    VirtualProtect(g_callerHookIatSlot, sizeof(void*), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_callerHookIatSlot, sizeof(void*));
    g_callerHookIatSlot = nullptr;
    g_callerHookUsers = 0;
    g_callerHookTargetModule = nullptr;
    g_callerHookOriginal = nullptr;
    g_originalGetModuleFileNameW.store(nullptr, std::memory_order_release);
    g_callerModule.store(nullptr, std::memory_order_release);
    g_callerHookOwner.store(nullptr, std::memory_order_release);
    _callerHookInstalled = false;
    _getModuleFileNameIatSlot = nullptr;
    _originalGetModuleFileNameW = nullptr;
    return true;
}

void DLSSNRFeatureDx12::Shutdown(ID3D12Device* device)
{
    DLSSNRLatePass::Cancel(this);
    // No further queue submissions can reference this object during teardown.
    // Release the current handle through the frame-generation lifetime queue;
    // it may still be referenced by an already submitted command list.
    ReleaseFeature();
    if (_parameters != nullptr)
    {
        if (auto destroyParameters = NVNGXProxy::D3D12_DestroyParameters(); destroyParameters != nullptr)
            destroyParameters(_parameters);
    }
    _parameters = nullptr;
    _output.Reset();
    _guidance.reset();
    _gazeRoiMvPatch.reset();
    // Shutdown1 and FreeLibrary must retire after the same generation as the
    // handle release. Otherwise a queued callback could call into an unloaded
    // signed snippet DLL during a display/scale transition.
    const auto shutdown1 = _shutdown1;
    const HMODULE module = _module;
    if (_initialized && shutdown1 != nullptr)
    {
        GazeRoiFrameSync::DeferCallback([shutdown1, device, module]() {
            shutdown1(device);
            if (module != nullptr)
                FreeLibrary(module);
        });
    }
    else if (module != nullptr)
    {
        GazeRoiFrameSync::DeferCallback([module]() { FreeLibrary(module); });
    }
    _initialized = false;
    _featureId = NVSDK_NGX_Feature_Reserved0;
    _width = 0;
    _height = 0;
    _pendingReset = true;
    _lastFullResolutionGuidance = false;
    _lastTemporalResidualReconstruction = false;
    _lastHighResolutionGuidedResidual = false;
    _lastLowResolutionOriginalMVec = false;
    _lastLowResolutionMVecScale = false;
    _lastCloneTypelessDepth = false;
    _lastZeroMotionInput = false;
    _lastZeroDepthInput = false;
    _lastDisableGazeRoiMotionInjection = false;
    _previousFoveatedRegion = {};
    _hasPreviousFoveatedRegion = false;
    _floatSetterSlot = -1;
    _hasPreviousTemporalJitter = false;
    _previousTemporalJitterX = 0.0f;
    _previousTemporalJitterY = 0.0f;
    _lastGameOutputResource = nullptr;
    _lastGameOutputDesc = {};
    _hasGameOutputContract = false;

    if (!RestoreCallerCompatibility())
        LOG_WARN("[DLSSNR] failed to restore caller compatibility IAT");

    _module = nullptr;
    _initExt = nullptr;
    _shutdown1 = nullptr;
    _createFeature = nullptr;
    _evaluateFeature = nullptr;
    _releaseFeature = nullptr;
}

bool DLSSNRFeatureDx12::EnsureInitialized(ID3D12Device* device)
{
    if (_initialized)
        return true;
    if (_initializationAttempted)
        return false;
    _initializationAttempted = true;
    if (device == nullptr)
    {
        LogBypass("D3D12 device is unavailable");
        return false;
    }

    const auto configuredPath = Config::Instance()->DLSSNRLibraryPath.value_or(std::wstring {});
    std::filesystem::path path(configuredPath);
    if (configuredPath.empty() || configuredPath == L"auto")
    {
        const auto directory = Util::DllPath().parent_path();
        // Probe known NR names only; ordinary NGX/SR DLLs share these exports.
        for (const auto* name : { L"nvngx_dlssnr.dll", L"nvngx.dll_dlssnr.dll" })
        {
            const auto candidate = directory / name;
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error))
            {
                path = candidate;
                break;
            }
        }
        if (path.empty() || path == L"auto")
        {
            LogBypass(std::format("no DLSS-NR DLL found in {} (expected nvngx_dlssnr.dll or nvngx.dll_dlssnr.dll)",
                                  wstring_to_string(directory.wstring())));
            return false;
        }
    }
    _module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (_module == nullptr)
    {
        LogBypass(std::format("could not load {} (Win32 error {})", wstring_to_string(path.wstring()), GetLastError()));
        return false;
    }

    auto resolve = [this](const char* name) { return GetProcAddress(_module, name); };
    _initExt = reinterpret_cast<InitExtFn>(resolve("NVSDK_NGX_D3D12_Init_Ext"));
    _shutdown1 = reinterpret_cast<Shutdown1Fn>(resolve("NVSDK_NGX_D3D12_Shutdown1"));
    _createFeature = reinterpret_cast<CreateFeatureFn>(resolve("NVSDK_NGX_D3D12_CreateFeature"));
    _evaluateFeature = reinterpret_cast<EvaluateFeatureFn>(resolve("NVSDK_NGX_D3D12_EvaluateFeature"));
    _releaseFeature = reinterpret_cast<ReleaseFeatureFn>(resolve("NVSDK_NGX_D3D12_ReleaseFeature"));
    if (_initExt == nullptr || _shutdown1 == nullptr || _createFeature == nullptr || _evaluateFeature == nullptr ||
        _releaseFeature == nullptr)
    {
        std::string missing;
        auto appendMissing = [&missing](const char* name, FARPROC address) {
            if (address != nullptr)
                return;
            if (!missing.empty())
                missing += ", ";
            missing += name;
        };
        appendMissing("NVSDK_NGX_D3D12_Init_Ext", reinterpret_cast<FARPROC>(_initExt));
        appendMissing("NVSDK_NGX_D3D12_Shutdown1", reinterpret_cast<FARPROC>(_shutdown1));
        appendMissing("NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<FARPROC>(_createFeature));
        appendMissing("NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<FARPROC>(_evaluateFeature));
        appendMissing("NVSDK_NGX_D3D12_ReleaseFeature", reinterpret_cast<FARPROC>(_releaseFeature));
        LogBypass(std::format("configured DLL is missing required exports: {}", missing));
        Shutdown(device);
        return false;
    }

    // The compatible NR snippet validates its caller module name. RenoDX's
    // working path patches only this DLL's import table to report nvngx.dll.
    if (!InstallCallerCompatibility())
    {
        Shutdown(device);
        return false;
    }

    _modulePath = path.wstring();
    _moduleDirectory = path.parent_path().wstring();
    auto& state = State::Instance();
    std::wstring applicationDataPath = state.NVNGX_ApplicationDataPath;
    if (applicationDataPath.empty())
        applicationDataPath = Util::ExePath().remove_filename().wstring();
    const NVSDK_NGX_Version version = state.NVNGX_Version == 0 ? NVSDK_NGX_Version_API : state.NVNGX_Version;
    // Signed Snippet contract: fixed application ID, application directory,
    // and nullptr FeatureCommonInfo. Feature ID 18 is not discoverable via
    // the normal Core requirements API in this compatibility runtime.
    NVSDK_NGX_Result initResult = _initExt(kDlssNrSnippetApplicationId, applicationDataPath.c_str(), device,
                                           version, nullptr);
    if (initResult != NVSDK_NGX_Result_Success)
    {
        LogBypass(std::format("NGX Init_Ext failed (0x{:X}, appId=0x{:X})", static_cast<unsigned int>(initResult),
                              kDlssNrSnippetApplicationId));
        Shutdown(device);
        return false;
    }

    auto allocateParameters = NVNGXProxy::D3D12_AllocateParameters();
    if (allocateParameters == nullptr || allocateParameters(&_parameters) != NVSDK_NGX_Result_Success ||
        _parameters == nullptr)
    {
        LogBypass("DLSSNR AllocateParameters failed");
        Shutdown(device);
        return false;
    }

    _featureId = kDlssNrFeature;

    _initialized = true;
    _pendingReset = true;
    _lastBypassReason.clear();
    LOG_INFO("[DLSSNR] initialized signed snippet module={} featureId={} (Init_Ext appId=0x{:X})",
             wstring_to_string(_modulePath), static_cast<int>(_featureId), kDlssNrSnippetApplicationId);
    return true;
}

bool DLSSNRFeatureDx12::EnsureOutput(ID3D12Device* device, unsigned int width, unsigned int height)
{
    if (_output != nullptr && _width == width && _height == height)
        return true;

    ReleaseFeature();
    if (_output != nullptr)
    {
        auto retiredOutput = std::move(_output);
        GazeRoiFrameSync::DeferCallback([retiredOutput]() mutable { retiredOutput.Reset(); });
    }
    _width = width;
    _height = height;
    _pendingReset = true;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kDlssNrFormat;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&_output));
    if (FAILED(result))
    {
        LogBypass(std::format("could not create {}x{} RGBA16F NR output (0x{:X})", width, height,
                              static_cast<unsigned int>(result)));
        _width = 0;
        _height = 0;
        return false;
    }
    _output->SetName(L"OptiScaler_DLSSNR_Output");
    return true;
}

bool DLSSNRFeatureDx12::EnsureFeature(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters,
                                      unsigned int inputWidth, unsigned int inputHeight,
                                      unsigned int outputWidth, unsigned int outputHeight)
{
    const std::optional<int> style = Config::Instance()->DLSSNRStyle.value_for_config();
    const std::optional<int> preset = Config::Instance()->DLSSNRPreset.value_for_config();
    if (_handle != nullptr && style == _createStyle && preset == _createPreset &&
        _featureInputWidth == inputWidth && _featureInputHeight == inputHeight &&
        _featureOutputWidth == outputWidth && _featureOutputHeight == outputHeight)
        return true;

    ReleaseFeature();
    _createStyle = style;
    _createPreset = preset;
    _featureInputWidth = inputWidth;
    _featureInputHeight = inputHeight;
    _featureOutputWidth = outputWidth;
    _featureOutputHeight = outputHeight;
    _pendingReset = true;
    const bool upscaling = inputWidth != outputWidth || inputHeight != outputHeight;
    const float scalingRatio = std::min(static_cast<float>(inputWidth) / static_cast<float>(outputWidth),
                                        static_cast<float>(inputHeight) / static_cast<float>(outputHeight));
    SetParameterUInt(parameters, "DLSSNR.Width", outputWidth);
    SetParameterUInt(parameters, "DLSSNR.Height", outputHeight);
    SetParameterUInt(parameters, "DLSSNR.InputWidth", inputWidth);
    SetParameterUInt(parameters, "DLSSNR.InputHeight", inputHeight);
    SetParameterUInt(parameters, "DLSSNR.OutputWidth", outputWidth);
    SetParameterUInt(parameters, "DLSSNR.OutputHeight", outputHeight);
    SetParameterUInt(parameters, "DLSSNR.Output.Width", outputWidth);
    SetParameterUInt(parameters, "DLSSNR.Output.Height", outputHeight);
    SetParameterUInt(parameters, "DLSSNR.Upscaling", upscaling ? 1u : 0u);
    SetParameterFloat(parameters, "DLSSNR.Scale", scalingRatio);
    SetParameterFloat(parameters, "DLSSNR.ScalingRatio", scalingRatio);
    SetParameterUInt(parameters, "DLSSNR.Hint.Render.Preset", preset.value_or(kDlssNrDefaultPreset));
    SetParameterUInt(parameters, NVSDK_NGX_Parameter_PerfQualityValue,
                     static_cast<unsigned int>(NVSDK_NGX_PerfQuality_Value_Balanced));
    SetParameterUInt(parameters, NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    SetParameterUInt(parameters, NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    if (style)
        SetParameterUInt(parameters, "DLSSNR.Style", static_cast<unsigned int>(*style));
    const NVSDK_NGX_Result result = _createFeature(commandList, _featureId, parameters, &_handle);
    if (result != NVSDK_NGX_Result_Success || _handle == nullptr)
    {
        LogBypass(std::format("CreateFeature failed (0x{:X})", static_cast<unsigned int>(result)));
        _handle = nullptr;
        return false;
    }
    LOG_INFO("[DLSSNR] feature created: input={}x{} output={}x{} upscaling={} ratio={:.6f} style={} preset={} featureId={}",
             inputWidth, inputHeight, outputWidth, outputHeight, upscaling ? 1 : 0, scalingRatio,
             style.value_or(-1), preset.value_or(-1), static_cast<int>(_featureId));
    return true;
}

bool DLSSNRFeatureDx12::Evaluate(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                                 NVSDK_NGX_Parameter* parameters, unsigned int outputWidth,
                                 unsigned int outputHeight, bool depthInverted,
                                 const FoveatedRegion* foveatedRegion, ColorDomain domain, std::optional<uint32_t> lifetimeSlot,
                                 ID3D12Resource** lateOutput)
{
    if (lateOutput) *lateOutput = nullptr;
    if (domain == ColorDomain::Scene && Config::Instance()->DLSSNRLateHudless.value_or_default())
        return DLSSNRLatePass::Stage(this, device, commandList, parameters, outputWidth, outputHeight,
                                     depthInverted, foveatedRegion);
    if (domain == ColorDomain::Scene)
        DLSSNRLatePass::Cancel(this);
    if (_lastColorDomain != domain)
    {
        _lastColorDomain = domain;
        InvalidateHistory();
        if (_guidance) _guidance->ResetTemporalHistory();
    }
    ++_attemptedEvaluations;
    const auto recordStart = std::chrono::steady_clock::now();
    if (!Config::Instance()->DLSSNREnabled.value_or_default())
        return false;
    if (commandList == nullptr || parameters == nullptr || outputWidth == 0 || outputHeight == 0)
    {
        LogBypass("missing command list, parameter table, or logical output size");
        return false;
    }

    ID3D12Resource* gameOutput = GetResource(parameters, NVSDK_NGX_Parameter_Output);
    if (gameOutput == nullptr)
    {
        LogBypass("the completed DLSS output resource is null");
        return false;
    }
    const auto gameOutputDesc = gameOutput->GetDesc();
    std::string colorContractReason;
    if (!IsSupportedColor(device, gameOutputDesc, colorContractReason))
    {
        LogBypass(std::format(
            "the completed DLSS output is incompatible: {} dimension={} allocation={}x{} array={} mips={} format=0x{:X} samples={} quality={} flags=0x{:X}",
            colorContractReason,
            static_cast<unsigned int>(gameOutputDesc.Dimension), gameOutputDesc.Width, gameOutputDesc.Height,
            gameOutputDesc.DepthOrArraySize, gameOutputDesc.MipLevels,
            static_cast<unsigned int>(gameOutputDesc.Format), gameOutputDesc.SampleDesc.Count,
            gameOutputDesc.SampleDesc.Quality, static_cast<unsigned int>(gameOutputDesc.Flags)));
        return false;
    }
    const unsigned int fullOutputBaseX = GetParameter<unsigned int>(
        parameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);
    const unsigned int fullOutputBaseY = GetParameter<unsigned int>(
        parameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0);
    const unsigned int fullOutputWidth = outputWidth;
    const unsigned int fullOutputHeight = outputHeight;
    unsigned int outputBaseX = fullOutputBaseX;
    unsigned int outputBaseY = fullOutputBaseY;
    bool foveated = false;
    if (foveatedRegion != nullptr)
    {
        if (foveatedRegion->outputWidth == 0 || foveatedRegion->outputHeight == 0 ||
            static_cast<uint64_t>(foveatedRegion->outputX) + foveatedRegion->outputWidth > fullOutputWidth ||
            static_cast<uint64_t>(foveatedRegion->outputY) + foveatedRegion->outputHeight > fullOutputHeight)
        {
            LogBypass("invalid DLSSNR gaze output region");
            return false;
        }
        outputBaseX += foveatedRegion->outputX;
        outputBaseY += foveatedRegion->outputY;
        outputWidth = foveatedRegion->outputWidth;
        outputHeight = foveatedRegion->outputHeight;
        foveated = true;
    }
    const auto& outputDesc = gameOutputDesc;
    if (static_cast<uint64_t>(outputBaseX) + outputWidth > outputDesc.Width ||
        static_cast<uint64_t>(outputBaseY) + outputHeight > outputDesc.Height)
    {
        LogBypass("the logical DLSS output subrect is outside the output allocation");
        return false;
    }

    // Register this command list before any resize/reconfiguration can retire
    // resources referenced by the preceding NR dispatch. The existing queue
    // hook associates the slot with a fence without blocking the render thread.
    // DLSS-NR can be the first D3D12 feature used by a game, so install the
    // queue hook here as the normal DLSS/RR ROI paths do before acquiring a
    // lifetime slot. Without this, slots remain in Recording forever and the
    // eighth NR frame permanently bypasses.
    ResTrack_Dx12::EnsureQueueHook(device);
    DLSSNRFrameSyncGuard frameSyncGuard(commandList, lifetimeSlot);
    if (!frameSyncGuard.Acquired())
    {
        LogBypass("DLSSNR frame lifetime slot unavailable; skipping evaluation");
        return false;
    }
    LOG_DEBUG("[DLSSNR_LIFETIME] command list tracked for evaluation");

    const auto sameResourceDesc = [](const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
    {
        return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width &&
               a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize &&
               a.MipLevels == b.MipLevels && a.Format == b.Format &&
               a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality &&
               a.Layout == b.Layout && a.Flags == b.Flags;
    };
    // Games may rotate a ring of same-sized Output resources every frame. That
    // is a normal resource identity change, not a DLSS-NR feature contract
    // change. Rebuild only when the logical allocation/format contract changes;
    // retain the latest pointer for diagnostics and future transitions.
    const bool gameOutputContractChanged = !_hasGameOutputContract ||
        !sameResourceDesc(_lastGameOutputDesc, gameOutputDesc);
    if (gameOutputContractChanged)
    {
        if (_hasGameOutputContract)
        {
            LOG_INFO("[DLSSNR_LIFETIME] game output changed resource={:X}->{:X} allocation={}x{}->{}x{} "
                     "format=0x{:X}->0x{:X}; rebuilding NR state",
                     reinterpret_cast<uintptr_t>(_lastGameOutputResource), reinterpret_cast<uintptr_t>(gameOutput),
                     _lastGameOutputDesc.Width, _lastGameOutputDesc.Height,
                     gameOutputDesc.Width, gameOutputDesc.Height,
                     static_cast<unsigned int>(_lastGameOutputDesc.Format),
                     static_cast<unsigned int>(gameOutputDesc.Format));
            ReleaseFeature();
            if (_guidance != nullptr)
                _guidance->ResetTemporalHistory();
            _hasPreviousTemporalJitter = false;
            _hasPreviousFoveatedRegion = false;
            _pendingReset = true;
        }
        _lastGameOutputResource = gameOutput;
        _lastGameOutputDesc = gameOutputDesc;
        _hasGameOutputContract = true;
    }

    const auto& config = *Config::Instance();
    float hudlessHDRPaperWhiteNits = config.DLSSNRHudlessHDRPaperWhiteNits.value_or_default();
    hudlessHDRPaperWhiteNits = std::isfinite(hudlessHDRPaperWhiteNits)
        ? std::clamp(hudlessHDRPaperWhiteNits, 80.0f, 1000.0f) : 203.0f;
    if (domain == ColorDomain::DisplayLinearHDR &&
        hudlessHDRPaperWhiteNits != _lastHudlessHDRPaperWhiteNits)
    {
        // Conversion, restoration and reconstruction share one frame snapshot.
        // Histories trained under the old white must not bleed into the new input.
        _lastHudlessHDRPaperWhiteNits = hudlessHDRPaperWhiteNits;
        InvalidateHistory();
        if (_guidance) _guidance->ResetTemporalHistory();
        LOG_INFO("[DLSSNR_COLOR] HUDless HDR input paper white={:.1f} nits; reset history",
                 hudlessHDRPaperWhiteNits);
    }
    // Exposure-texture reading / white-point adaptation: referenced the approach
    // in OptiScaler_DLSSNR. Credit to Dagherbou and the OptiScaler_DLSSNR
    // contributors, https://github.com/Dagherbou/OptiScaler_DLSSNR.
    // See Licenses/OptiScaler_DLSSNR_ATTRIBUTION.txt for the pinned reference
    // and adaptation scope of this live NGX exposure-texture path.
    // NGX exposes the game's exposure independently from the color buffer. Keep
    // the pointer for the transfer shader so it can sample the current frame
    // directly, avoiding a CPU readback delay. Invalid/missing resources simply
    // fall back to the existing manual paper-white path.
    ID3D12Resource* exposureResource = GetResource(parameters, NVSDK_NGX_Parameter_ExposureTexture);
    float preExposure = GetParameter<float>(parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    if (!std::isfinite(preExposure) || preExposure <= 1.0e-6f)
        preExposure = 1.0f;
    const float exposureScale = std::clamp(config.DLSSNRExposureScale.value_or_default(), 0.01f, 16.0f);
    const bool exposureResourceReadable = exposureResource != nullptr &&
        IsExposureTexture(exposureResource->GetDesc());
    const bool useGameExposure = domain == ColorDomain::Scene && config.DLSSNRWhitePointSource.value_or_default() == 1 &&
        exposureResourceReadable;
    struct ExposureContractLog
    {
        bool valid = false;
        bool useGame = false;
        bool readable = false;
        float preExposure = 0.0f;
        float scale = 0.0f;
    };
    static ExposureContractLog loggedExposureContract;
    const bool exposureContractChanged = !loggedExposureContract.valid ||
        loggedExposureContract.useGame != useGameExposure ||
        loggedExposureContract.readable != exposureResourceReadable ||
        std::abs(loggedExposureContract.preExposure - preExposure) >
            std::max(0.01f * std::abs(preExposure), 1.0e-4f) ||
        std::abs(loggedExposureContract.scale - exposureScale) > 1.0e-4f;
    if (exposureContractChanged)
    {
        loggedExposureContract.valid = true;
        loggedExposureContract.useGame = useGameExposure;
        loggedExposureContract.readable = exposureResourceReadable;
        loggedExposureContract.preExposure = preExposure;
        loggedExposureContract.scale = exposureScale;
        LOG_INFO("[DLSSNR_EXPOSURE] source={} resource={} preExposure={:.6f} scale={:.4f}",
                 useGameExposure ? "game" : "manual",
                 exposureResourceReadable ? "supplied" : "not supplied/unsupported",
                 preExposure, exposureScale);
    }
    // The visible DLSS5 render-scale control is shared by full-frame and gaze
    // NR. Keep the legacy LowResolutionScale key readable for old INIs, but do
    // not let it create a second runtime scale contract.
    const int configuredScale = std::clamp(config.DLSSNRGazeRoiScale.value_or_default(), 1, 3);
    const bool lowResolution = configuredScale == 2 || configuredScale == 3;
    const bool lowResolutionOriginalMVec = lowResolution &&
        config.DLSSNRLowResolutionOriginalMVec.value_or_default();
    const bool originalMVecChanged = lowResolutionOriginalMVec != _lastLowResolutionOriginalMVec;
    const bool lowResolutionFullOutput = lowResolution &&
                                         config.DLSSNRLowResolutionFullOutput.value_or_default();
    const bool disableGazeRoiMotionInjection = foveated &&
        config.DLSSNRDisableGazeRoiMotionInjection.value_or_default();
    const bool zeroMotionInput = !lowResolutionOriginalMVec && !disableGazeRoiMotionInjection &&
        config.DLSSNRZeroMotionInput.value_or_default();
    const bool temporalResidualReconstruction = lowResolution && !lowResolutionFullOutput &&
        config.DLSSNRTemporalResidualReconstruction.value_or_default();
    const bool highResolutionGuidedResidual = lowResolution && !lowResolutionFullOutput &&
        config.DLSSNRHighResolutionGuidedResidual.value_or_default();
    const bool featureGuidedReconstruction = lowResolution && !lowResolutionFullOutput &&
        !config.DLSSNRLegacyResidualReconstruction.value_or_default();
    const bool fastReconstruction = featureGuidedReconstruction &&
        config.DLSSNRFastReconstruction.value_or_default();
    const bool debugGlobalDownsampleRequested =
        config.DLSSNRDebugGlobalDownsampleOutput.value_or_default();
    const bool debugGlobalDownsampleOutput =
        debugGlobalDownsampleRequested && lowResolution && foveated;
    const unsigned int nrWidth = lowResolution ? std::max((outputWidth + static_cast<unsigned int>(configuredScale) - 1) /
                                                               static_cast<unsigned int>(configuredScale), 1u)
                                               : outputWidth;
    const unsigned int nrHeight = lowResolution ? std::max((outputHeight + static_cast<unsigned int>(configuredScale) - 1) /
                                                                static_cast<unsigned int>(configuredScale), 1u)
                                                : outputHeight;
    const unsigned int nrOutputWidth = lowResolutionFullOutput ? outputWidth : nrWidth;
    const unsigned int nrOutputHeight = lowResolutionFullOutput ? outputHeight : nrHeight;
    const unsigned int roiEdgeBlendPx = foveated
        ? static_cast<unsigned int>(std::clamp(config.DLSSNRGazeRoiEdgeBlendPx.value_or_default(), 0, 512))
        : 0u;
    bool roiExtrapolation = foveated && config.DLSSNRGazeRoiExtrapolation.value_or_default() &&
        roiEdgeBlendPx != 0;
    // Positive legacy call-chain flag for bounded local edge correction.
    constexpr float roiExtrapolationScale = 1.0f;
    unsigned int roiEdgeBlendMask = 0;
    if (foveated && roiEdgeBlendPx != 0)
    {
        if (foveatedRegion->outputX > 0)
            roiEdgeBlendMask |= 1u;
        if (foveatedRegion->outputY > 0)
            roiEdgeBlendMask |= 2u;
        if (static_cast<uint64_t>(foveatedRegion->outputX) + outputWidth < fullOutputWidth)
            roiEdgeBlendMask |= 4u;
        if (static_cast<uint64_t>(foveatedRegion->outputY) + outputHeight < fullOutputHeight)
            roiEdgeBlendMask |= 8u;
    }
    unsigned int extrapolationBaseX = outputBaseX;
    unsigned int extrapolationBaseY = outputBaseY;
    unsigned int extrapolationWidth = outputWidth;
    unsigned int extrapolationHeight = outputHeight;
    unsigned int extrapolationRoiOffsetX = 0;
    unsigned int extrapolationRoiOffsetY = 0;
    const unsigned int extrapolationBand = static_cast<unsigned int>(
        std::clamp(config.DLSSNRGazeRoiExtrapolationDistancePx.value_or_default(), 0, 512));
    const DlssNrExtrapolation::Axis extrapolationX = roiExtrapolation
        ? DlssNrExtrapolation::ExpandBandAxis(outputBaseX, outputWidth, fullOutputBaseX, fullOutputWidth, extrapolationBand)
        : DlssNrExtrapolation::Axis { outputBaseX, outputWidth, 0u, outputWidth };
    const DlssNrExtrapolation::Axis extrapolationY = roiExtrapolation
        ? DlssNrExtrapolation::ExpandBandAxis(outputBaseY, outputHeight, fullOutputBaseY, fullOutputHeight, extrapolationBand)
        : DlssNrExtrapolation::Axis { outputBaseY, outputHeight, 0u, outputHeight };
    extrapolationBaseX = extrapolationX.base;
    extrapolationBaseY = extrapolationY.base;
    extrapolationWidth = extrapolationX.size;
    extrapolationHeight = extrapolationY.size;
    extrapolationRoiOffsetX = extrapolationX.roiOffset;
    extrapolationRoiOffsetY = extrapolationY.roiOffset;
    const int roiEdgeBlendSignature = static_cast<int>(roiEdgeBlendPx | (roiEdgeBlendMask << 16u));
    if (foveated && roiEdgeBlendSignature != _lastGazeRoiEdgeBlendSignature)
    {
        _lastGazeRoiEdgeBlendSignature = roiEdgeBlendSignature;
        LOG_INFO("[DLSSNR_GROI] EdgeBlendPx={} edgeMask=0x{:X}", roiEdgeBlendPx,
                 roiEdgeBlendMask);
    }
    else if (!foveated)
    {
        _lastGazeRoiEdgeBlendSignature = -1;
    }
    if (debugGlobalDownsampleRequested != _lastDebugGlobalDownsampleOutput)
    {
        _lastDebugGlobalDownsampleOutput = debugGlobalDownsampleRequested;
        _pendingReset = true;
        _hasPreviousTemporalJitter = false;
        _hasPreviousFoveatedRegion = false;
        if (_guidance != nullptr)
            _guidance->ResetTemporalHistory();
        LOG_INFO("[DLSSNR_DEBUG] fixed-grid full-frame downsample crop={} (NR history reset)",
                 debugGlobalDownsampleRequested ? 1 : 0);
    }
    if (originalMVecChanged)
    {
        _lastLowResolutionOriginalMVec = lowResolutionOriginalMVec;
        _pendingReset = true;
        _hasPreviousTemporalJitter = false;
        _hasPreviousFoveatedRegion = false;
        if (_guidance != nullptr)
        {
            _guidance->ResetTemporalHistory();
            _guidance->outputHistoryValid = false;
            _guidance->contractLogged = false;
        }
        LOG_INFO("[DLSSNR_GUIDANCE] original-resolution MVec={} (NR and temporal histories reset)",
                 lowResolutionOriginalMVec ? 1 : 0);
    }

    // A gaze window can move every frame while retaining an overlapping,
    // same-sized history domain.  Some callers propagate their own reset bit
    // for that ROI update; forwarding it to NR would defeat the overlap-aware
    // history preservation above.  Keep this separate from _pendingReset so
    // real game resets and contract changes are never suppressed.
    bool suppressUpstreamResetForRoiMotion = false;
    bool regionChangedForReset = false;
    if (foveated)
    {
        const bool regionChanged = !_hasPreviousFoveatedRegion ||
            std::memcmp(&_previousFoveatedRegion, foveatedRegion, sizeof(FoveatedRegion)) != 0;
        regionChangedForReset = regionChanged;
        const auto rectsOverlap = [](unsigned int ax, unsigned int ay, unsigned int aw, unsigned int ah,
                                     unsigned int bx, unsigned int by, unsigned int bw, unsigned int bh)
        {
            return static_cast<uint64_t>(ax) < static_cast<uint64_t>(bx) + bw &&
                   static_cast<uint64_t>(bx) < static_cast<uint64_t>(ax) + aw &&
                   static_cast<uint64_t>(ay) < static_cast<uint64_t>(by) + bh &&
                   static_cast<uint64_t>(by) < static_cast<uint64_t>(ay) + ah;
        };
        const bool outputOverlap = _hasPreviousFoveatedRegion &&
            rectsOverlap(_previousFoveatedRegion.outputX, _previousFoveatedRegion.outputY,
                         _previousFoveatedRegion.outputWidth, _previousFoveatedRegion.outputHeight,
                         foveatedRegion->outputX, foveatedRegion->outputY,
                         foveatedRegion->outputWidth, foveatedRegion->outputHeight);
        const bool inputOverlap = _hasPreviousFoveatedRegion &&
            rectsOverlap(_previousFoveatedRegion.inputX, _previousFoveatedRegion.inputY,
                         _previousFoveatedRegion.inputWidth, _previousFoveatedRegion.inputHeight,
                         foveatedRegion->inputX, foveatedRegion->inputY,
                         foveatedRegion->inputWidth, foveatedRegion->inputHeight);
        const bool sizeChanged = _hasPreviousFoveatedRegion &&
            (_previousFoveatedRegion.outputWidth != foveatedRegion->outputWidth ||
             _previousFoveatedRegion.outputHeight != foveatedRegion->outputHeight ||
             _previousFoveatedRegion.inputWidth != foveatedRegion->inputWidth ||
             _previousFoveatedRegion.inputHeight != foveatedRegion->inputHeight);
        // Preserve temporal history while a gaze window moves through an
        // overlapping area.  The existing reverse-MV injection accounts for
        // that translation; resetting every movement would discard the very
        // history that the patch is meant to preserve.
        const bool resetForRegion = regionChanged &&
            (!_hasPreviousFoveatedRegion || sizeChanged || !outputOverlap || !inputOverlap);
        suppressUpstreamResetForRoiMotion = regionChanged && !resetForRegion;
        if (resetForRegion)
            _pendingReset = true;
        LOG_DEBUG("[DLSSNR_GROI] active output={}x{}+{},{} input={}x{}+{},{} scale={} regionChanged={} overlap={} resetOnRegionChange={}",
                  outputWidth, outputHeight, outputBaseX, outputBaseY,
                  foveatedRegion->inputWidth, foveatedRegion->inputHeight,
                  foveatedRegion->inputX, foveatedRegion->inputY, configuredScale,
                  regionChanged ? 1 : 0, (outputOverlap && inputOverlap) ? 1 : 0, resetForRegion ? 1 : 0);
    }
    const bool needsFormatConversion = gameOutputDesc.Format != kDlssNrFormat;
    if (!EnsureInitialized(device) || !EnsureOutput(device, nrOutputWidth, nrOutputHeight))
        return false;

    NVSDK_NGX_Parameter* nrParameters = _parameters;
    if (nrParameters == nullptr)
    {
        LogBypass("NR parameter table is unavailable after initialization");
        return false;
    }

    SetParameterResource(nrParameters, "DLSSNR.Color", gameOutput);
    SetParameterResource(nrParameters, "DLSSNR.Output", _output.Get());
    SetParameterUInt(nrParameters, "DLSSNR.Width", nrOutputWidth);
    SetParameterUInt(nrParameters, "DLSSNR.Height", nrOutputHeight);
    SetParameterUInt(nrParameters, "DLSSNR.InputWidth", nrWidth);
    SetParameterUInt(nrParameters, "DLSSNR.InputHeight", nrHeight);
    SetParameterUInt(nrParameters, "DLSSNR.OutputWidth", nrOutputWidth);
    SetParameterUInt(nrParameters, "DLSSNR.OutputHeight", nrOutputHeight);
    SetParameterUInt(nrParameters, "DLSSNR.Output.Width", nrOutputWidth);
    SetParameterUInt(nrParameters, "DLSSNR.Output.Height", nrOutputHeight);
    const float nrScalingRatio = std::min(static_cast<float>(nrWidth) / static_cast<float>(nrOutputWidth),
                                          static_cast<float>(nrHeight) / static_cast<float>(nrOutputHeight));
    SetParameterUInt(nrParameters, "DLSSNR.Upscaling", lowResolutionFullOutput ? 1u : 0u);
    SetParameterFloat(nrParameters, "DLSSNR.Scale", nrScalingRatio);
    SetParameterFloat(nrParameters, "DLSSNR.ScalingRatio", nrScalingRatio);
    SetSubrect(nrParameters, "Color", nrWidth, nrHeight,
               lowResolution ? 0u : outputBaseX, lowResolution ? 0u : outputBaseY);
    SetSubrect(nrParameters, "Output", nrOutputWidth, nrOutputHeight, 0, 0);

    const unsigned int fullRenderWidth = GetParameter<unsigned int>(
        parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
        GetParameter<unsigned int>(parameters, NVSDK_NGX_Parameter_Width, fullOutputWidth));
    const unsigned int fullRenderHeight = GetParameter<unsigned int>(
        parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
        GetParameter<unsigned int>(parameters, NVSDK_NGX_Parameter_Height, fullOutputHeight));
    if (foveated &&
        (foveatedRegion->inputWidth == 0 || foveatedRegion->inputHeight == 0 ||
         static_cast<uint64_t>(foveatedRegion->inputX) + foveatedRegion->inputWidth > fullRenderWidth ||
         static_cast<uint64_t>(foveatedRegion->inputY) + foveatedRegion->inputHeight > fullRenderHeight))
    {
        LogBypass("invalid DLSSNR gaze input region");
        return false;
    }
    const unsigned int featureFlags = GetParameter<unsigned int>(parameters,
        NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 0);
    const bool mvLowRes = (featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    const unsigned int fullMvWidth = mvLowRes ? fullRenderWidth :
        GetParameter<unsigned int>(parameters, "DLSSNR.OriginalOutputWidth", fullOutputWidth);
    const unsigned int fullMvHeight = mvLowRes ? fullRenderHeight :
        GetParameter<unsigned int>(parameters, "DLSSNR.OriginalOutputHeight", fullOutputHeight);
    unsigned int renderWidth = foveated ? foveatedRegion->inputWidth : fullRenderWidth;
    unsigned int renderHeight = foveated ? foveatedRegion->inputHeight : fullRenderHeight;
    const unsigned int mvOutputX = foveated ? foveatedRegion->outputX : 0u;
    const unsigned int mvOutputY = foveated ? foveatedRegion->outputY : 0u;
    const unsigned int mvWidth = mvLowRes ? renderWidth : static_cast<unsigned int>(
        (static_cast<uint64_t>(mvOutputX + outputWidth) * fullMvWidth) / fullOutputWidth -
        (static_cast<uint64_t>(mvOutputX) * fullMvWidth) / fullOutputWidth);
    const unsigned int mvHeight = mvLowRes ? renderHeight : static_cast<unsigned int>(
        (static_cast<uint64_t>(mvOutputY + outputHeight) * fullMvHeight) / fullOutputHeight -
        (static_cast<uint64_t>(mvOutputY) * fullMvHeight) / fullOutputHeight);
    unsigned int sourceMvBaseX = GetParameter<unsigned int>(
        parameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0);
    unsigned int sourceMvBaseY = GetParameter<unsigned int>(
        parameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0);
    const unsigned int fullSourceMvBaseX = sourceMvBaseX;
    const unsigned int fullSourceMvBaseY = sourceMvBaseY;
    unsigned int sourceDepthBaseX = GetParameter<unsigned int>(
        parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0);
    unsigned int sourceDepthBaseY = GetParameter<unsigned int>(
        parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0);
    const unsigned int fullDepthBaseX = sourceDepthBaseX;
    const unsigned int fullDepthBaseY = sourceDepthBaseY;
    const float sourceMvScaleX = GetParameter<float>(parameters, NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    const float sourceMvScaleY = GetParameter<float>(parameters, NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    ID3D12Resource* const sourceMotion = GetResource(parameters, NVSDK_NGX_Parameter_MotionVectors);
    ID3D12Resource* const sourceDepth = GetResource(parameters, NVSDK_NGX_Parameter_Depth);
    ID3D12Resource* nrMotion = sourceMotion;
    ID3D12Resource* nrDepth = sourceDepth;
    const auto motionAllocation = nrMotion != nullptr ? nrMotion->GetDesc() : D3D12_RESOURCE_DESC {};
    const auto depthAllocation = nrDepth != nullptr ? nrDepth->GetDesc() : D3D12_RESOURCE_DESC {};
    g_originalMotionLogical.store(PackDimensions(nrMotion != nullptr ? mvWidth : 0,
                                                 nrMotion != nullptr ? mvHeight : 0),
                                  std::memory_order_relaxed);
    g_originalMotionAllocation.store(PackDimensions(static_cast<unsigned int>(motionAllocation.Width),
                                                    motionAllocation.Height),
                                     std::memory_order_relaxed);
    g_originalMotionBase.store(PackDimensions(sourceMvBaseX, sourceMvBaseY), std::memory_order_relaxed);
    g_originalDepthLogical.store(PackDimensions(nrDepth != nullptr ? renderWidth : 0,
                                                nrDepth != nullptr ? renderHeight : 0),
                                 std::memory_order_relaxed);
    g_originalDepthAllocation.store(PackDimensions(static_cast<unsigned int>(depthAllocation.Width),
                                                   depthAllocation.Height),
                                    std::memory_order_relaxed);
    g_originalDepthBase.store(PackDimensions(sourceDepthBaseX, sourceDepthBaseY), std::memory_order_relaxed);
    g_originalGuidesObserved.store(true, std::memory_order_release);
    unsigned int nrMvBaseX = sourceMvBaseX;
    unsigned int nrMvBaseY = sourceMvBaseY;
    unsigned int nrMvWidth = mvWidth;
    unsigned int nrMvHeight = mvHeight;
    unsigned int nrDepthBaseX = sourceDepthBaseX;
    unsigned int nrDepthBaseY = sourceDepthBaseY;
    unsigned int nrDepthWidth = renderWidth;
    unsigned int nrDepthHeight = renderHeight;
    float nrMvScaleX = sourceMvScaleX;
    float nrMvScaleY = sourceMvScaleY;

    if (foveated)
    {
        // Depth is on the render grid. MV is either render-grid or output-grid
        // depending on the same DLSS feature flag as the game call.
        sourceDepthBaseX += foveatedRegion->inputX;
        sourceDepthBaseY += foveatedRegion->inputY;
        if (mvLowRes)
        {
            sourceMvBaseX += foveatedRegion->inputX;
            sourceMvBaseY += foveatedRegion->inputY;
        }
        else
        {
            sourceMvBaseX += static_cast<unsigned int>(
                (static_cast<uint64_t>(foveatedRegion->outputX) * fullMvWidth) / fullOutputWidth);
            sourceMvBaseY += static_cast<unsigned int>(
                (static_cast<uint64_t>(foveatedRegion->outputY) * fullMvHeight) / fullOutputHeight);
        }
        nrMvBaseX = sourceMvBaseX;
        nrMvBaseY = sourceMvBaseY;
        nrDepthBaseX = sourceDepthBaseX;
        nrDepthBaseY = sourceDepthBaseY;
    }

    if (lowResolutionOriginalMVec &&
        !GuidanceResources::ValidReadRect(nrMotion, nrMvBaseX, nrMvBaseY, nrMvWidth, nrMvHeight))
    {
        LogBypass("original-resolution MVec resource/subrect is unavailable or invalid");
        _pendingReset = true;
        return false;
    }

    // Keep the moving-window transform out of surface-aware resampling. The
    // native-MVec experiment always crops the ROI at its original texel size,
    // including first/stationary frames and disabled origin injection.
    ID3D12Resource* gazePatchedMotion = nullptr;
    unsigned int gazePatchedMotionBaseX = nrMvBaseX;
    unsigned int gazePatchedMotionBaseY = nrMvBaseY;
    float resampledOriginOffsetX = 0.0f;
    float resampledOriginOffsetY = 0.0f;
    float nativeMotionOffsetX = 0.0f;
    float nativeMotionOffsetY = 0.0f;
    bool patchNativeMotion = foveated && lowResolutionOriginalMVec;
    if (foveated && !disableGazeRoiMotionInjection && nrMotion != nullptr && _hasPreviousFoveatedRegion &&
        !zeroMotionInput)
    {
        const unsigned int previousMvX = mvLowRes
            ? _previousFoveatedRegion.inputX
            : static_cast<unsigned int>((static_cast<uint64_t>(_previousFoveatedRegion.outputX) * fullMvWidth) /
                                         std::max(fullOutputWidth, 1u));
        const unsigned int previousMvY = mvLowRes
            ? _previousFoveatedRegion.inputY
            : static_cast<unsigned int>((static_cast<uint64_t>(_previousFoveatedRegion.outputY) * fullMvHeight) /
                                         std::max(fullOutputHeight, 1u));
        const unsigned int currentMvX = mvLowRes
            ? foveatedRegion->inputX
            : static_cast<unsigned int>((static_cast<uint64_t>(foveatedRegion->outputX) * fullMvWidth) /
                                         std::max(fullOutputWidth, 1u));
        const unsigned int currentMvY = mvLowRes
            ? foveatedRegion->inputY
            : static_cast<unsigned int>((static_cast<uint64_t>(foveatedRegion->outputY) * fullMvHeight) /
                                         std::max(fullOutputHeight, 1u));
        const int32_t deltaX = static_cast<int32_t>(currentMvX) - static_cast<int32_t>(previousMvX);
        const int32_t deltaY = static_cast<int32_t>(currentMvY) - static_cast<int32_t>(previousMvY);
        const int32_t outputDeltaX = static_cast<int32_t>(foveatedRegion->outputX) -
                                     static_cast<int32_t>(_previousFoveatedRegion.outputX);
        const int32_t outputDeltaY = static_cast<int32_t>(foveatedRegion->outputY) -
                                     static_cast<int32_t>(_previousFoveatedRegion.outputY);
        if (lowResolution && !lowResolutionOriginalMVec)
        {
            // Convert the Color-window displacement directly to the final NR
            // grid. This exact constant is added after surface-aware filtering,
            // including the invalid-guide fallback path.
            resampledOriginOffsetX = static_cast<float>(outputDeltaX) * static_cast<float>(nrWidth) /
                                     static_cast<float>(std::max(outputWidth, 1u));
            resampledOriginOffsetY = static_cast<float>(outputDeltaY) * static_cast<float>(nrHeight) /
                                     static_cast<float>(std::max(outputHeight, 1u));
            LOG_DEBUG("[DLSSNR_GROI] post-resample MV origin offset outputDelta=({}, {}) NR=({:.6f}, {:.6f})",
                      outputDeltaX, outputDeltaY, resampledOriginOffsetX, resampledOriginOffsetY);
        }
        else if ((deltaX != 0 || deltaY != 0) &&
            std::isfinite(nrMvScaleX) && std::isfinite(nrMvScaleY) &&
            std::abs(nrMvScaleX) >= 0.0001f && std::abs(nrMvScaleY) >= 0.0001f)
        {
            // currentPixel + MV = previousPixel in the native guide grid.
            nativeMotionOffsetX = static_cast<float>(deltaX) / nrMvScaleX;
            nativeMotionOffsetY = static_cast<float>(deltaY) / nrMvScaleY;
            patchNativeMotion = true;
        }
        else if ((!lowResolution || lowResolutionOriginalMVec) && (deltaX != 0 || deltaY != 0))
            LOG_WARN("[DLSSNR_GROI] MV delta injection skipped because MV scale is invalid ({}, {})",
                     nrMvScaleX, nrMvScaleY);
    }
    if (patchNativeMotion)
    {
        // Reuse the enclosing evaluation's fence-tracked descriptor slot.
        const uint32_t frameSlot = frameSyncGuard.Slot();
        if (_gazeRoiMvPatch == nullptr)
            _gazeRoiMvPatch = std::make_unique<GazeRoiMvPatch_Dx12>("DLSSNRGazeRoiMvPatch", device);
        GazeRoiMvConstants constants {};
        constants.width = static_cast<int32_t>(mvWidth);
        constants.height = static_cast<int32_t>(mvHeight);
        constants.sourceBaseX = static_cast<int32_t>(nrMvBaseX);
        constants.sourceBaseY = static_cast<int32_t>(nrMvBaseY);
        constants.rawOffsetX = nativeMotionOffsetX;
        constants.rawOffsetY = nativeMotionOffsetY;
        if (GuidanceResources::ValidReadRect(nrMotion, nrMvBaseX, nrMvBaseY, mvWidth, mvHeight) &&
            _gazeRoiMvPatch->CreatePatchedResource(device, nrMotion, mvWidth, mvHeight,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            // Even a later NGX failure leaves this crop/barrier recorded on
            // the game's command list. Retain its slot until queue completion.
            frameSyncGuard.Commit();
            _gazeRoiMvPatch->SetPatchedState(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (_gazeRoiMvPatch->Dispatch(commandList, nrMotion, constants, frameSlot))
            {
                _gazeRoiMvPatch->SetPatchedState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                gazePatchedMotion = _gazeRoiMvPatch->PatchedMotionVectors();
                gazePatchedMotionBaseX = gazePatchedMotionBaseY = 0;
                LOG_DEBUG("[DLSSNR_GROI] native MVec crop={}x{}+{},{} rawOffset=({:.6f}, {:.6f}) "
                          "originalResolution={} frameSlot={}",
                          mvWidth, mvHeight, nrMvBaseX, nrMvBaseY,
                          nativeMotionOffsetX, nativeMotionOffsetY, lowResolutionOriginalMVec ? 1 : 0, frameSlot);
            }
        }
        if (gazePatchedMotion == nullptr)
        {
            if (lowResolutionOriginalMVec)
            {
                LogBypass("original-resolution gaze MVec crop failed");
                _pendingReset = true;
                return false;
            }
            LOG_WARN("[DLSSNR_GROI] MV delta injection unavailable; using original subrect");
        }
    }

    // Publish the effective source bases after applying the gaze-region offset.
    // The menu/debug overlay reports these values, so storing the pre-offset
    // parameters would make a correctly offset model input appear anchored at
    // the allocation's top-left corner.
    g_originalMotionBase.store(PackDimensions(sourceMvBaseX, sourceMvBaseY),
                               std::memory_order_relaxed);
    g_originalDepthBase.store(PackDimensions(sourceDepthBaseX, sourceDepthBaseY),
                              std::memory_order_relaxed);

    const bool fullResolutionGuidance = config.DLSSNRFullResolutionGuidance.value_or_default();
    const bool debugInputView = config.DLSSNRDebugInputView.value_or_default();
    const bool debugInputViewComposite = config.DLSSNRDebugInputViewComposite.value_or_default();
    // Late composition can consume our final private image directly. The
    // caller uses it immediately on this command list and leaves it in UAV.
    // Keep ROI/blend/debug paths on their existing destination contract.
    const bool returnLateOutput = lateOutput && domain != ColorDomain::Scene && !foveated &&
        !debugInputView && fullOutputBaseX == 0 && fullOutputBaseY == 0;
    if (_successfulEvaluations == 0)
        LOG_INFO("[DLSSNR_GUIDANCE] debugInputView={} effectiveLowResolution={} scale={}",
                 debugInputView ? 1 : 0,
                 lowResolution ? 1 : 0, configuredScale);
    const bool debugModelOutput = lowResolution && !lowResolutionFullOutput &&
                                  config.DLSSNRDebugModelOutput.value_or_default();
    const bool lowResolutionMVecScale = lowResolution && !lowResolutionOriginalMVec &&
        config.DLSSNRLowResolutionMVecScale.value_or_default();
    const bool cloneTypelessDepth = lowResolution &&
        config.DLSSNRCloneTypelessDepth.value_or_default();
    const bool zeroDepthInput = config.DLSSNRZeroDepthInput.value_or_default();
    constexpr bool hdrColorTransfer = true;
    const bool sdrToneMapping = domain != ColorDomain::Scene || !config.DLSSNRLegacyHDRTransfer.value_or_default();
    if (sdrToneMapping != _lastSDRToneMapping)
    {
        _lastSDRToneMapping = sdrToneMapping;
        _pendingReset = true;
        _hasPreviousTemporalJitter = false;
        if (_guidance != nullptr)
            _guidance->ResetTemporalHistory();
        LOG_INFO("[DLSSNR_COLOR] SDR filmic tone mapping={} (NR and residual history reset)",
                 sdrToneMapping ? 1 : 0);
    }
    constexpr bool clampColor = false;
    // Low-resolution NR resamples the game's guides into the compact NR grid.
    // Full-resolution resampling is a diagnostic for the full-resolution path.
    const bool prepareFullResolutionGuidance = fullResolutionGuidance && !lowResolution;
    if (fullResolutionGuidance != _lastFullResolutionGuidance)
    {
        _lastFullResolutionGuidance = fullResolutionGuidance;
        _pendingReset = true;
        if (_guidance != nullptr)
            _guidance->contractLogged = false;
        LOG_INFO("[DLSSNR_GUIDANCE] FullResolutionGuidance={} (history reset)", fullResolutionGuidance);
    }
    bool guidancePrepared = false;
    bool guidanceHelperReady = false;
    ID3D12Resource* guidanceMotion = nrMotion;
    unsigned int guidanceMotionBaseX = sourceMvBaseX;
    unsigned int guidanceMotionBaseY = sourceMvBaseY;
    unsigned int guidanceMotionWidth = mvWidth;
    unsigned int guidanceMotionHeight = mvHeight;
    if (gazePatchedMotion != nullptr && !lowResolutionOriginalMVec)
    {
        guidanceMotion = gazePatchedMotion;
        guidanceMotionBaseX = 0;
        guidanceMotionBaseY = 0;
    }
    // Depth retains the normal surface-aware compact preparation even when
    // MVec passes through at its native resolution. Use unpatched scene motion
    // for that depth selection; the ROI-origin term is not a surface property.
    if (foveated || lowResolution || prepareFullResolutionGuidance || debugInputView || hdrColorTransfer || needsFormatConversion)
    {
        if (_guidance == nullptr)
            _guidance = std::make_unique<GuidanceResources>();
        _guidance->identityColorTransfer = domain == ColorDomain::DisplaySDR;
        _guidance->displayHDRColorTransfer = domain == ColorDomain::DisplayLinearHDR;
        _guidance->displayPaperWhite = hudlessHDRPaperWhiteNits / 80.0f;
        guidanceHelperReady = _guidance->EnsureInitialized(device) &&
            _guidance->EnsureTextures(device, debugInputView ? fullOutputWidth : outputWidth,
                                      debugInputView ? fullOutputHeight : outputHeight,
                                      prepareFullResolutionGuidance, debugInputView,
                                      !lowResolution && needsFormatConversion,
                                      (!lowResolution || lowResolutionFullOutput) && hdrColorTransfer);
        if (guidanceHelperReady && lowResolution)
            guidanceHelperReady = _guidance->EnsureLowTextures(device, outputWidth, outputHeight,
                                                                static_cast<unsigned int>(configuredScale),
                                                                temporalResidualReconstruction);
        if (guidanceHelperReady && debugGlobalDownsampleOutput)
            guidanceHelperReady = _guidance->EnsureGlobalDownsampleTextures(
                device, fullOutputWidth, fullOutputHeight, static_cast<unsigned int>(configuredScale));
        if (guidanceHelperReady)
        {
            _guidance->BeginFrame(frameSyncGuard.Slot(), _attemptedEvaluations);
            // All following helper commands need fence tracking even when
            // input preparation or Feature 18 subsequently rejects the frame.
            frameSyncGuard.Commit();
        }
    }
    // Native MVec and compact Depth are also consumed after NGX. Keep private
    // guides readable until all consumers finish, and restore them on errors.
    struct GuidanceStateGuard
    {
        GuidanceResources* resources;
        ID3D12GraphicsCommandList* commandList;
        ~GuidanceStateGuard()
        {
            if (resources != nullptr)
                resources->ReturnGuidanceToWritable(commandList);
        }
    } guidanceStateGuard { _guidance.get(), commandList };
    if (temporalResidualReconstruction != _lastTemporalResidualReconstruction)
    {
        _lastTemporalResidualReconstruction = temporalResidualReconstruction;
        _hasPreviousTemporalJitter = false;
        if (_guidance != nullptr)
            _guidance->ResetTemporalHistory();
        LOG_INFO("[DLSSNR_LOWRES] temporal residual reconstruction={} (history reset)",
                 temporalResidualReconstruction ? 1 : 0);
    }
    if (highResolutionGuidedResidual != _lastHighResolutionGuidedResidual)
    {
        _lastHighResolutionGuidedResidual = highResolutionGuidedResidual;
        LOG_INFO("[DLSSNR_LOWRES] high-resolution guided residual reconstruction={}",
                 highResolutionGuidedResidual ? 1 : 0);
    }
    if (lowResolutionMVecScale != _lastLowResolutionMVecScale)
    {
        _lastLowResolutionMVecScale = lowResolutionMVecScale;
        _pendingReset = true;
        LOG_INFO("[DLSSNR_GUIDANCE] low-resolution MVec work-domain scale={} (NR reset)",
                 lowResolutionMVecScale ? 1 : 0);
    }
    if (cloneTypelessDepth != _lastCloneTypelessDepth)
    {
        _lastCloneTypelessDepth = cloneTypelessDepth;
        _pendingReset = true;
        LOG_INFO("[DLSSNR_GUIDANCE] typeless depth clone={} (NR reset)", cloneTypelessDepth ? 1 : 0);
    }
    if (zeroMotionInput != _lastZeroMotionInput)
    {
        _lastZeroMotionInput = zeroMotionInput;
        _pendingReset = true;
        LOG_INFO("[DLSSNR_GUIDANCE] zero motion input={} (NR reset)", zeroMotionInput ? 1 : 0);
    }
    if (zeroDepthInput != _lastZeroDepthInput)
    {
        _lastZeroDepthInput = zeroDepthInput;
        _pendingReset = true;
        LOG_INFO("[DLSSNR_GUIDANCE] zero depth input={} (NR reset)", zeroDepthInput ? 1 : 0);
    }
    if (disableGazeRoiMotionInjection != _lastDisableGazeRoiMotionInjection)
    {
        _lastDisableGazeRoiMotionInjection = disableGazeRoiMotionInjection;
        _pendingReset = true;
        _hasPreviousTemporalJitter = false;
        if (_guidance != nullptr)
            _guidance->ResetTemporalHistory();
        LOG_INFO("[DLSSNR_GROI] ROI motion-vector injection disabled={} (NR history reset)",
                 disableGazeRoiMotionInjection ? 1 : 0);
    }
    if ((prepareFullResolutionGuidance || lowResolution) && guidanceHelperReady)
    {
        const unsigned int guidanceTargetWidth = lowResolution ? nrWidth : 0;
        const unsigned int guidanceTargetHeight = lowResolution ? nrHeight : 0;
        guidancePrepared = _guidance->Prepare(device, commandList, guidanceMotion, nrDepth,
                                              guidanceMotionBaseX, guidanceMotionBaseY,
                                              guidanceMotionWidth, guidanceMotionHeight,
                                              sourceDepthBaseX, sourceDepthBaseY, renderWidth, renderHeight,
                                              sourceMvScaleX, sourceMvScaleY,
                                              guidanceTargetWidth, guidanceTargetHeight,
                                              depthInverted, 0,
                                              resampledOriginOffsetX, resampledOriginOffsetY);
        if (guidancePrepared)
        {
            if (!lowResolutionOriginalMVec)
            {
                nrMotion = _guidance->preparedMotion;
                nrMvBaseX = nrMvBaseY = 0;
                nrMvWidth = lowResolution ? nrWidth : outputWidth;
                nrMvHeight = lowResolution ? nrHeight : outputHeight;
                nrMvScaleX = nrMvScaleY = 1.0f;
            }
            nrDepth = _guidance->preparedDepth;
            nrDepthBaseX = nrDepthBaseY = 0;
            nrDepthWidth = lowResolution ? nrWidth : outputWidth;
            nrDepthHeight = lowResolution ? nrHeight : outputHeight;
            if (_guidance->prepareFailureLogged)
                LOG_INFO("[DLSSNR_GUIDANCE] full-resolution guidance recovered");
            _guidance->prepareFailureLogged = false;
            if (!_guidance->contractLogged)
            {
                const auto motionDesc = GetResource(parameters, NVSDK_NGX_Parameter_MotionVectors)->GetDesc();
                const auto depthDesc = GetResource(parameters, NVSDK_NGX_Parameter_Depth)->GetDesc();
                LOG_INFO("[DLSSNR_GUIDANCE] resampler=coverage-surface-v3 source motionAlloc={}x{} format=0x{:X} valid={}x{}+{},{} "
                         "depthAlloc={}x{} format=0x{:X} valid={}x{}+{},{} motionContract={}x{} depthContract={}x{} "
                         "sourceMvScale={:.6f},{:.6f} outputMvValueScale={:.6f},{:.6f} originalMVec={}",
                         motionDesc.Width, motionDesc.Height, static_cast<unsigned int>(motionDesc.Format),
                         mvWidth, mvHeight, sourceMvBaseX, sourceMvBaseY,
                         depthDesc.Width, depthDesc.Height, static_cast<unsigned int>(depthDesc.Format),
                         renderWidth, renderHeight, sourceDepthBaseX, sourceDepthBaseY,
                         nrMvWidth, nrMvHeight, nrDepthWidth, nrDepthHeight, sourceMvScaleX, sourceMvScaleY,
                         sourceMvScaleX * static_cast<float>(nrMvWidth) / static_cast<float>(mvWidth),
                         sourceMvScaleY * static_cast<float>(nrMvHeight) / static_cast<float>(mvHeight),
                         lowResolutionOriginalMVec ? 1 : 0);
                _guidance->contractLogged = true;
            }
        }
        else if (!_guidance->prepareFailureLogged)
        {
            LOG_WARN("[DLSSNR_GUIDANCE] full-resolution preparation failed; using original DLSS guidance");
            _guidance->prepareFailureLogged = true;
        }
    }

    // A moving gaze ROI must not be presented to Feature 18 as a changing
    // subrect on the game-owned resource.  Stage the active color rectangle
    // into a fixed-origin private texture so the model always sees the same
    // coordinate domain while the ROI moves.
    ID3D12Resource* colorSource = gameOutput;
    Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    unsigned int colorSourceBaseX = outputBaseX;
    unsigned int colorSourceBaseY = outputBaseY;
    if (foveated)
    {
        if (!guidanceHelperReady ||
            !_guidance->PrepareRoiColor(device, commandList, gameOutput, outputBaseX, outputBaseY,
                                        outputWidth, outputHeight, !lowResolution && hdrColorTransfer))
        {
            LogBypass(std::format("ROI color staging failed: ROI={}x{}+{},{} source={}x{} format=0x{:X} helperReady={}",
                                  outputWidth, outputHeight, outputBaseX, outputBaseY,
                                  gameOutputDesc.Width, gameOutputDesc.Height,
                                  static_cast<unsigned int>(gameOutputDesc.Format), guidanceHelperReady));
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return false;
        }
        colorSource = _guidance->roiColor.Get();
        colorSourceBaseX = colorSourceBaseY = 0;
        LOG_DEBUG("[DLSSNR_GROI] staged Color resource={:X} size={}x{} base=(0,0)",
                  reinterpret_cast<uintptr_t>(colorSource), outputWidth, outputHeight);
    }
    ID3D12Resource* extrapolationOriginalSource = foveated ? _guidance->roiColor.Get() : gameOutput;
    if (roiExtrapolation)
    {
        if (_guidance->CanUseExtrapolationInPlace(device, gameOutputDesc.Format))
        {
            // Every restore/reconstruction branch keeps gameOutput untouched
            // until ApplyExtrapolation. Its final pass can update each pixel
            // independently after all original-image neighbourhood reads finish.
            extrapolationOriginalSource = gameOutput;
        }
        else if (!_guidance->PrepareExtrapolationOriginal(device, commandList, gameOutput,
                                                     extrapolationBaseX, extrapolationBaseY,
                                                     extrapolationWidth, extrapolationHeight,
                                                     extrapolationX.allocationSize, extrapolationY.allocationSize))
        {
            LOG_WARN("[DLSSNR_GROI] boundary-region original staging failed; using ROI-only blend");
            roiExtrapolation = false;
        }
        else
        {
            extrapolationOriginalSource = _guidance->extrapolationOriginal.Get();
        }
    }
    if (roiExtrapolation && extrapolationOriginalSource == nullptr)
        roiExtrapolation = false;
    ID3D12Resource* nrColor = colorSource;
    unsigned int nrColorBaseX = colorSourceBaseX;
    unsigned int nrColorBaseY = colorSourceBaseY;
    bool colorClampPrepared = false;
    bool colorTransferPrepared = false;
    bool lowColorTransferPrepared = false;
    ID3D12Resource* lowCompositeBase = nullptr;
    unsigned int lowCompositeBaseX = 0;
    unsigned int lowCompositeBaseY = 0;
    bool colorFormatPrepared = false;

    // Full-resolution inverse writes gameOutput. Preserve its HDR reference
    // in a separate SRV to avoid sampling the destination UAV. Reduced output
    // already has lowBaseline, and gaze ROI already has a private color copy.
    if (sdrToneMapping && !lowResolution && colorSource == gameOutput && !returnLateOutput)
    {
        if (!guidanceHelperReady ||
            !_guidance->PrepareRestoreSource(device, commandList, colorSource, colorSourceBaseX,
                                             colorSourceBaseY, outputWidth, outputHeight, true))
        {
            LogBypass("SDR tone mapping source preservation failed");
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            _pendingReset = true;
            return false;
        }
        colorSource = _guidance->restoreSource.Get();
        colorSourceBaseX = colorSourceBaseY = 0;
        nrColor = colorSource;
        nrColorBaseX = nrColorBaseY = 0;
    }

    if (lowResolution && guidanceHelperReady)
    {
        const float paperWhite = domain == ColorDomain::DisplaySDR ? 1.0f :
            domain == ColorDomain::DisplayLinearHDR ? hudlessHDRPaperWhiteNits / 80.0f :
            config.DLSSNRHDRPaperWhite.value_or(2.044f);
        // A typed SRV load already exposes the game's native output as linear
        // float values. Read that validated subrect directly so both the
        // filtered baseline and final composite retain the known-good game
        // image. The nonlinear HDR shoulder still runs only after filtering.
        bool lowColorPrepared = debugGlobalDownsampleOutput
            ? _guidance->PrepareGlobalDownsampleRoiColor(
                  device, commandList, gameOutput, fullOutputBaseX, fullOutputBaseY,
                  fullOutputWidth, fullOutputHeight,
                  foveatedRegion->outputX, foveatedRegion->outputY,
                  outputWidth, outputHeight, static_cast<unsigned int>(configuredScale),
                  std::isfinite(paperWhite) ? paperWhite : 2.044f, exposureResource, preExposure,
                  exposureScale, useGameExposure, sdrToneMapping)
            : _guidance->PrepareLowResolutionColor(
                  device, commandList, colorSource, colorSourceBaseX, colorSourceBaseY,
                  outputWidth, outputHeight, static_cast<unsigned int>(configuredScale),
                  std::isfinite(paperWhite) ? paperWhite : 2.044f, exposureResource, preExposure,
                  exposureScale, useGameExposure, sdrToneMapping);
        // Both preparations now preserve the rounded linear baseline and
        // encode the model input in the same dispatch.
        lowColorTransferPrepared = lowColorPrepared;
        if (lowColorPrepared)
        {
            nrColor = _guidance->lowColor.Get();
            nrColorBaseX = nrColorBaseY = 0;
            lowCompositeBase = colorSource;
            lowCompositeBaseX = colorSourceBaseX;
            lowCompositeBaseY = colorSourceBaseY;
            if (_successfulEvaluations == 0)
                LOG_INFO("[DLSSNR_LOWRES] enabled scale={} NR={}x{} filter={} domain=scene transfer={} paperWhite={:.4f}",
                         configuredScale, nrWidth, nrHeight,
                         debugGlobalDownsampleOutput ? "full-frame-tiled-separable-catrom-then-roi" :
                                                       "roi-then-tiled-separable-catrom",
                         hdrColorTransfer ? 1 : 0, paperWhite);
        }
        else
        {
            LogBypass("low-resolution color preparation failed");
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return false;
        }
    }
    else if (hdrColorTransfer && guidanceHelperReady)
    {
        const float paperWhite = domain == ColorDomain::DisplaySDR ? 1.0f :
            domain == ColorDomain::DisplayLinearHDR ? hudlessHDRPaperWhiteNits / 80.0f :
            config.DLSSNRHDRPaperWhite.value_or(2.044f);
        constexpr float transferStrength = 1.0f;
        constexpr float colorStrength = 1.0f;
        colorTransferPrepared = _guidance->PrepareColorTransfer(
            device, commandList, gameOutput, outputBaseX, outputBaseY, outputWidth, outputHeight,
            std::isfinite(paperWhite) ? paperWhite : 2.044f,
            std::isfinite(transferStrength) ? transferStrength : 1.0f,
            std::isfinite(colorStrength) ? colorStrength : 1.0f,
            exposureResource, preExposure, exposureScale, useGameExposure, sdrToneMapping,
            colorSource != gameOutput ? colorSource : nullptr, !foveated && sdrToneMapping);
        if (colorTransferPrepared)
        {
            nrColor = _guidance->clampedColor.Get();
            nrColorBaseX = nrColorBaseY = 0;
            // Legacy inverse failure still uses typed-format conversion for
            // non-RGBA16F output. SDR experimental failures preserve the source.
            colorFormatPrepared = needsFormatConversion;
            if (_successfulEvaluations == 0)
                LOG_INFO("[DLSSNR_COLOR] HDR transfer active paperWhite={:.4f} strength={:.4f} colorStrength={:.4f}",
                         paperWhite, transferStrength, colorStrength);
        }
        else if (!_guidance->colorClampFailureLogged)
        {
            LOG_WARN("[DLSSNR_COLOR] HDR transfer preparation failed; using original DLSS color");
            _guidance->colorClampFailureLogged = true;
        }
    }
    else if (clampColor && guidanceHelperReady)
    {
        colorClampPrepared = _guidance->PrepareColorClamp(device, commandList, colorSource,
                                                          colorSourceBaseX, colorSourceBaseY,
                                                          outputWidth, outputHeight);
        if (colorClampPrepared)
        {
            nrColor = _guidance->clampedColor.Get();
            nrColorBaseX = nrColorBaseY = 0;
            colorFormatPrepared = needsFormatConversion;
        }
        else if (!_guidance->colorClampFailureLogged)
        {
            LOG_WARN("[DLSSNR_COLOR] color clamp preparation failed; using original DLSS color");
            _guidance->colorClampFailureLogged = true;
        }
        if (needsFormatConversion && !colorClampPrepared)
        {
            LogBypass("could not convert the game's output format to RGBA16F");
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return false;
        }
    }
    else if (needsFormatConversion && guidanceHelperReady)
    {
        colorFormatPrepared = _guidance->PrepareColorClamp(device, commandList, colorSource,
                                                           colorSourceBaseX, colorSourceBaseY,
                                                           outputWidth, outputHeight, false);
        if (colorFormatPrepared)
        {
            nrColor = _guidance->clampedColor.Get();
            nrColorBaseX = nrColorBaseY = 0;
            if (_successfulEvaluations == 0)
                LOG_INFO("[DLSSNR_COLOR] converting game output format 0x{:X} to RGBA16F for NR",
                         static_cast<unsigned int>(gameOutputDesc.Format));
        }
        else
        {
            LogBypass("could not convert the game's output format to RGBA16F");
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return false;
        }
    }
    if ((sdrToneMapping || (foveated && !_guidance->roiColorReadable)) &&
        !colorTransferPrepared && !lowColorTransferPrepared)
    {
        LogBypass("color transfer or source preservation failed");
        Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        _pendingReset = true;
        return false;
    }
    if (colorClampPrepared && _guidance->colorClampFailureLogged)
    {
        LOG_INFO("[DLSSNR_COLOR] color clamp recovered");
        _guidance->colorClampFailureLogged = false;
    }
    if (colorTransferPrepared && _guidance->colorClampFailureLogged)
    {
        LOG_INFO("[DLSSNR_COLOR] HDR transfer recovered");
        _guidance->colorClampFailureLogged = false;
    }

    float modelMvScaleX = nrMvScaleX;
    float modelMvScaleY = nrMvScaleY;
    // GuidanceResources::Prepare already converts motion values into the NR
    // target pixel domain. Only retain the legacy scale diagnostic when the
    // original guide contract is actually being passed through.
    if (lowResolutionMVecScale && !(lowResolution && guidancePrepared))
    {
        modelMvScaleX *= static_cast<float>(nrWidth) / static_cast<float>(std::max(outputWidth, 1u));
        modelMvScaleY *= static_cast<float>(nrHeight) / static_cast<float>(std::max(outputHeight, 1u));
    }

    ID3D12Resource* modelMotion = nrMotion;
    unsigned int modelMvBaseX = nrMvBaseX;
    unsigned int modelMvBaseY = nrMvBaseY;
    unsigned int modelMvWidth = nrMvWidth;
    unsigned int modelMvHeight = nrMvHeight;
    if (gazePatchedMotion != nullptr && (!guidancePrepared || lowResolutionOriginalMVec))
    {
        modelMotion = gazePatchedMotion;
        modelMvBaseX = gazePatchedMotionBaseX;
        modelMvBaseY = gazePatchedMotionBaseY;
    }
    if (zeroMotionInput)
    {
        if (!_guidance->PrepareZeroGuide(device, commandList, DXGI_FORMAT_R16G16_FLOAT,
                                         nrMvWidth, nrMvHeight, L"OptiScaler_DLSSNR_Zero_Motion",
                                         kZeroMotionDescriptor, _guidance->zeroMotion, &modelMotion))
        {
            LogBypass("zero motion input preparation failed");
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return false;
        }
        modelMvBaseX = modelMvBaseY = 0;
    }

    ID3D12Resource* modelDepth = nrDepth;
    unsigned int modelDepthBaseX = nrDepthBaseX;
    unsigned int modelDepthBaseY = nrDepthBaseY;
    unsigned int modelDepthWidth = nrDepthWidth;
    unsigned int modelDepthHeight = nrDepthHeight;
    if (cloneTypelessDepth && !zeroDepthInput &&
        (!_guidance->PrepareTypelessDepthClone(device, commandList, nrDepth, &modelDepth) || modelDepth == nullptr))
    {
        LogBypass("typeless depth clone preparation failed");
        Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return false;
    }
    if (zeroDepthInput)
    {
        if (!_guidance->PrepareZeroGuide(device, commandList, DXGI_FORMAT_R32_FLOAT,
                                         nrDepthWidth, nrDepthHeight, L"OptiScaler_DLSSNR_Zero_Depth",
                                         kZeroDepthDescriptor, _guidance->zeroDepth, &modelDepth))
        {
            LogBypass("zero depth input preparation failed");
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return false;
        }
        modelDepthBaseX = modelDepthBaseY = 0;
    }

    if (nrColor != gameOutput)
    {
        SetParameterResource(nrParameters, "DLSSNR.Color", nrColor);
        SetSubrect(nrParameters, "Color", nrWidth, nrHeight, nrColorBaseX, nrColorBaseY);
    }

    if (modelMotion != nullptr)
    {
        SetParameterResource(nrParameters, "DLSSNR.MVec", modelMotion);
        SetSubrect(nrParameters, "MVec", modelMvWidth, modelMvHeight, modelMvBaseX, modelMvBaseY);
    }
    if (modelDepth != nullptr)
    {
        SetParameterResource(nrParameters, "DLSSNR.Depth", modelDepth);
        SetSubrect(nrParameters, "Depth", modelDepthWidth, modelDepthHeight,
                   modelDepthBaseX, modelDepthBaseY);
    }
    SetParameterFloat(nrParameters, "DLSSNR.MVecScaleX", modelMvScaleX);
    SetParameterFloat(nrParameters, "DLSSNR.MVecScaleY", modelMvScaleY);
    SetParameterUInt(nrParameters, "DLSSNR.DepthInverted", depthInverted ? 1u : 0u);
    SetParameterUInt(nrParameters, "DLSSNR.Enabled", 1u);
    const bool upstreamReset = GetParameter<int>(parameters, NVSDK_NGX_Parameter_Reset, 0) != 0;
    const bool effectiveReset = _pendingReset || (upstreamReset && !suppressUpstreamResetForRoiMotion);
    if (effectiveReset && _guidance) _guidance->boundaryHistoryValid = false;
    SetParameterUInt(nrParameters, "DLSSNR.Reset", effectiveReset ? 1u : 0u);
    if (regionChangedForReset || upstreamReset || effectiveReset)
        LOG_DEBUG("[DLSSNR_RESET] pending={} upstream={} suppressRoiMotion={} effective={}",
                  _pendingReset ? 1 : 0, upstreamReset ? 1 : 0,
                  suppressUpstreamResetForRoiMotion ? 1 : 0, effectiveReset ? 1 : 0);
    const unsigned int upstreamFeatureFlags = GetParameter<unsigned int>(
        parameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 0);
    const bool upstreamHDR = (upstreamFeatureFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
    SetParameterUInt(nrParameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, upstreamFeatureFlags);
    SetParameterUInt(nrParameters, "DLSSNR.IsHDR", upstreamHDR ? 1u : 0u);
    if (_successfulEvaluations == 0)
        LOG_INFO("[DLSSNR_COLOR] upstream DLSS HDR flag={}", upstreamHDR ? 1 : 0);

    if (config.DLSSNRIntensity && std::isfinite(*config.DLSSNRIntensity))
        SetParameterFloat(nrParameters, "DLSSNR.Intensity", *config.DLSSNRIntensity);
    if (config.DLSSNRLocalToneStrength && std::isfinite(*config.DLSSNRLocalToneStrength))
        SetParameterFloat(nrParameters, "DLSSNR.LocalToneStrength", *config.DLSSNRLocalToneStrength);
    if (config.DLSSNRLocalStructureStrength && std::isfinite(*config.DLSSNRLocalStructureStrength))
        SetParameterFloat(nrParameters, "DLSSNR.LocalStructureStrength", *config.DLSSNRLocalStructureStrength);
    if (config.DLSSNRSkinStructureStrength && std::isfinite(*config.DLSSNRSkinStructureStrength))
        SetParameterFloat(nrParameters, "DLSSNR.SkinStructureStrength", *config.DLSSNRSkinStructureStrength);
    // Do not leave these switches at the leaked DLL's private defaults.  The
    // signed-snippet reference explicitly supplies both values on every
    // evaluation; an omitted UseAutoMask can enable an internal mask path that
    // produces blocky scene-UI regions.  `auto` therefore means the safe
    // reference defaults here: auto-mask off and UI correction off.
    const bool useAutoMask = config.DLSSNRUseAutoMask.value_or(false);
    // The signed-snippet reference always supplies zero for UICorrection.
    constexpr bool uiCorrection = false;
    SetParameterUInt(nrParameters, "DLSSNR.UseAutoMask", useAutoMask ? 1u : 0u);
    SetParameterUInt(nrParameters, "DLSSNR.UICorrection", uiCorrection ? 1u : 0u);
    if (_successfulEvaluations == 0)
        LOG_INFO("[DLSSNR] effective mask settings: UseAutoMask={} UICorrection={}", useAutoMask ? 1 : 0,
                 uiCorrection ? 1 : 0);

    bool debugStaged = false;
    if (debugInputView && guidanceHelperReady)
    {
            debugStaged = _guidance->StageDebug(device, commandList, nrColor, modelMotion, modelDepth,
                                             nrColorBaseX, nrColorBaseY, nrWidth, nrHeight,
                                             modelMvBaseX, modelMvBaseY, modelMvWidth, modelMvHeight,
                                             modelDepthBaseX, modelDepthBaseY, modelDepthWidth, modelDepthHeight,
                                             depthInverted);
        if (debugStaged)
        {
            if (_guidance->debugFailureLogged)
                LOG_INFO("[DLSSNR_GUIDANCE] input debug view recovered");
            _guidance->debugFailureLogged = false;
        }
        else if (!_guidance->debugFailureLogged)
        {
            LOG_WARN("[DLSSNR_GUIDANCE] input debug view could not be staged");
            _guidance->debugFailureLogged = true;
        }
    }
    if (debugStaged && !debugInputViewComposite && !_debugInputViewCompositeDisabledLogged)
    {
        LOG_INFO("[DLSSNR_GUIDANCE] input debug view compositing disabled by DebugInputViewComposite=0");
        _debugInputViewCompositeDisabledLogged = true;
    }
    else if (debugInputViewComposite)
    {
        _debugInputViewCompositeDisabledLogged = false;
    }
    if (!EnsureFeature(commandList, nrParameters, nrWidth, nrHeight, nrOutputWidth, nrOutputHeight))
    {
        Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return false;
    }
    if (lowResolution && (_successfulEvaluations == 0 || originalMVecChanged))
    {
        const auto lowColorDesc = nrColor->GetDesc();
        const auto modelMotionDesc = modelMotion != nullptr ? modelMotion->GetDesc() : D3D12_RESOURCE_DESC {};
        const auto modelDepthDesc = modelDepth != nullptr ? modelDepth->GetDesc() : D3D12_RESOURCE_DESC {};
        LOG_INFO("[DLSSNR_LOWRES] Evaluate contract color={}x{} subrect={}x{} NR output={}x{} game output={}x{} "
                 "fullOutput={} motion={}x{} format=0x{:X} subrect={}x{}+{},{} scale={:.6f},{:.6f} "
                 "depth={}x{} format=0x{:X} subrect={}x{}+{},{} mvWorkScale={} typedDepthClone={} "
                 "zeroMotion={} zeroDepth={} originalMVec={}",
                  lowColorDesc.Width, lowColorDesc.Height, nrWidth, nrHeight, nrOutputWidth, nrOutputHeight,
                  outputWidth, outputHeight, lowResolutionFullOutput ? 1 : 0,
                  modelMotionDesc.Width, modelMotionDesc.Height, static_cast<unsigned int>(modelMotionDesc.Format),
                  modelMvWidth, modelMvHeight, modelMvBaseX, modelMvBaseY, modelMvScaleX, modelMvScaleY,
                  modelDepthDesc.Width, modelDepthDesc.Height, static_cast<unsigned int>(modelDepthDesc.Format),
                  modelDepthWidth, modelDepthHeight, modelDepthBaseX, modelDepthBaseY,
                  lowResolutionMVecScale ? 1 : 0, cloneTypelessDepth ? 1 : 0,
                  zeroMotionInput ? 1 : 0, zeroDepthInput ? 1 : 0, lowResolutionOriginalMVec ? 1 : 0);
    }
    if (config.DLSSNRPresentPreview.value_or_default() == 1)
    {
        // Capture the exact active Color subrect after all preparation. Commit
        // the generation even if NGX fails, so this copy remains fence-tracked.
        frameSyncGuard.Commit();
        DLSSNRPreview::Capture(device, commandList, nrColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              nrColorBaseX, nrColorBaseY, nrWidth, nrHeight, 1);
    }
    const auto evaluateStart = std::chrono::steady_clock::now();
    const NVSDK_NGX_Result result = _evaluateFeature(commandList, _handle, nrParameters, nullptr);
    const double evaluateCpuMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - evaluateStart).count();
    if (result != NVSDK_NGX_Result_Success)
    {
        LogBypass(std::format("EvaluateFeature failed (0x{:X})", static_cast<unsigned int>(result)));
        Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        _pendingReset = true;
        return false;
    }

    // Preserve tracking for NGX even if no helper dispatch was recorded.
    frameSyncGuard.Commit();

    if (config.DLSSNRPresentPreview.value_or_default() == 2)
        DLSSNRPreview::Capture(device, commandList, _output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              0, 0, nrOutputWidth, nrOutputHeight, 2);

    // Track the selected result independently from NGX's stable output
    // allocation. Temporal history stays readable, including every error exit.
    struct ModelOutputState
    {
        ID3D12Resource* resource;
        ID3D12GraphicsCommandList* commands;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES restore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        void Set(D3D12_RESOURCE_STATES next)
        {
            if (state != next) Transition(commands, resource, state, next);
            state = next;
        }
        void Restore() { Set(restore); }
        ~ModelOutputState() { Restore(); }
    } modelOutput { _output.Get(), commandList };

    // Consume guides before ReturnGuidanceToWritable changes their states.
    // These are exactly the resources/subrects/scales supplied to NGX above,
    // including cropped depth and injected ROI translation. Do not re-prepare
    // them from the game's original full-frame guide resources.
    if (config.DLSSNROutputTemporalStabilization.value_or_default() && guidanceHelperReady)
    {
        const float jitterX = GetParameter<float>(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
        const float jitterY = GetParameter<float>(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
        const bool jittered = (featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
        const float correctionX = jittered && _guidance->outputHistoryValid
            ? (jitterX - _guidance->outputPreviousJitterX) * nrOutputWidth / std::max(renderWidth, 1u) : 0.0f;
        const float correctionY = jittered && _guidance->outputHistoryValid
            ? (jitterY - _guidance->outputPreviousJitterY) * nrOutputHeight / std::max(renderHeight, 1u) : 0.0f;
        GuidanceResources::OutputTemporalConstants constants {
            nrOutputWidth, nrOutputHeight, 0u, depthInverted ? 1u : 0u,
            modelMvBaseX, modelMvBaseY, modelMvWidth, modelMvHeight,
            modelDepthBaseX, modelDepthBaseY, modelDepthWidth, modelDepthHeight,
            nrColorBaseX, nrColorBaseY, nrWidth, nrHeight,
            modelMvScaleX * nrOutputWidth / std::max(modelMvWidth, 1u),
            modelMvScaleY * nrOutputHeight / std::max(modelMvHeight, 1u), correctionX, correctionY };
        const bool wasValid = _guidance->outputHistoryValid;
        if (!_guidance->StabilizeOutput(device, commandList, _output.Get(), modelMotion, modelDepth,
                                        nrColor, constants, effectiveReset, jitterX, jitterY, &modelOutput.resource))
        {
            _guidance->outputHistoryValid = false;
            if (!_guidance->outputTemporalFailureLogged)
                LOG_WARN("[DLSSNR_TEMPORAL] output stabilization unavailable; using current model output");
            _guidance->outputTemporalFailureLogged = true;
        }
        else
        {
            modelOutput.state = modelOutput.restore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (!wasValid || _guidance->outputTemporalFailureLogged)
                LOG_INFO("[DLSSNR_TEMPORAL] output stabilization active {}x{}; source-anchored adaptive history (max 95%)",
                         nrOutputWidth, nrOutputHeight);
            _guidance->outputTemporalFailureLogged = false;
        }
    }
    else if (_guidance != nullptr)
    {
        _guidance->ReleaseOutputHistory();
    }

    if (lowResolution)
    {
        const float paperWhite = domain == ColorDomain::DisplaySDR ? 1.0f :
            domain == ColorDomain::DisplayLinearHDR ? hudlessHDRPaperWhiteNits / 80.0f :
            config.DLSSNRHDRPaperWhite.value_or(2.044f);
        ID3D12Resource* residualColor = modelOutput.resource;
        bool residualColorWritable = modelOutput.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const bool fusedResidual = lowColorTransferPrepared && !lowResolutionFullOutput && !debugModelOutput;
        if (lowColorTransferPrepared)
        {
            // Decode NR back to the same linear domain as lowBaseline before
            // subtraction. Full-output mode needs a full-resolution decoded
            // surface; clampedColor is otherwise idle in the low-res HDR path.
            ID3D12Resource* decodedOutput = lowResolutionFullOutput
                                                 ? _guidance->clampedColor.Get()
                                                 : fusedResidual ? _guidance->lowResidual.Get() : _guidance->lowColor.Get();
            bool& decodedReadable = lowResolutionFullOutput
                                        ? _guidance->colorClampReadable
                                        : fusedResidual ? _guidance->lowResidualReadable : _guidance->lowColorReadable;
            modelOutput.Set(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (decodedReadable)
            {
                Transition(commandList, decodedOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                decodedReadable = false;
            }
            const bool decoded = _guidance->ApplyColorTransferInverse(
                device, commandList, modelOutput.resource, decodedOutput, 0, 0, nrOutputWidth, nrOutputHeight,
                std::isfinite(paperWhite) ? paperWhite : 2.044f, 1.0f, 1.0f, kDlssNrFormat,
                exposureResource, preExposure, exposureScale, useGameExposure, sdrToneMapping,
                lowResolutionFullOutput ? colorSource : _guidance->lowBaseline.Get(),
                lowResolutionFullOutput ? colorSourceBaseX : 0u,
                lowResolutionFullOutput ? colorSourceBaseY : 0u,
                0u, 0u, 0.0f, 0u, 0u, 0u, 0u, 0u, 0u, nullptr, fusedResidual);
            modelOutput.Restore();
            if (!decoded)
            {
                LogBypass("low-resolution HDR inverse transfer failed");
                _pendingReset = true;
                Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                return false;
            }
            Transition(commandList, decodedOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            decodedReadable = true;
            residualColor = decodedOutput;
            residualColorWritable = false;
        }
        if (!lowResolutionFullOutput && !debugModelOutput && !fusedResidual)
        {
            if (residualColorWritable)
                modelOutput.Set(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const bool residualPrepared = _guidance != nullptr &&
                _guidance->PrepareLowResidual(device, commandList, residualColor, false, false);
            if (!residualPrepared)
            {
                LogBypass("low-resolution residual preparation failed");
                Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                return false;
            }
            // The existing low-resolution residual pass consumes this source
            // immediately and leaves the final residual in its own texture.
            if (residualColorWritable)
                modelOutput.Restore();
        }
        else if (residualColorWritable)
        {
            // Full-output fusion and direct model-output preview consume
            // the NR image directly, so keep it
            // shader-readable for composition.
            modelOutput.Set(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        const bool compositeUsesModelOutput = lowResolutionFullOutput || debugModelOutput;
        ID3D12Resource* compositeCorrection = compositeUsesModelOutput
                                                   ? residualColor
                                                   : _guidance->lowResidual.Get();
        bool temporalComposite = false;
        if (temporalResidualReconstruction && !debugModelOutput && !lowResolutionFullOutput)
        {
            const bool temporalReset = effectiveReset || !_hasPreviousTemporalJitter;
            const float currentJitterX = GetParameter<float>(
                parameters, NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
            const float currentJitterY = GetParameter<float>(
                parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
            const bool motionVectorsJittered =
                (featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
            const float jitterCorrectionX = motionVectorsJittered && _hasPreviousTemporalJitter
                ? (currentJitterX - _previousTemporalJitterX) *
                    static_cast<float>(nrWidth) / static_cast<float>(std::max(renderWidth, 1u))
                : 0.0f;
            const float jitterCorrectionY = motionVectorsJittered && _hasPreviousTemporalJitter
                ? (currentJitterY - _previousTemporalJitterY) *
                    static_cast<float>(nrHeight) / static_cast<float>(std::max(renderHeight, 1u))
                : 0.0f;
            ID3D12Resource* temporalMotion = modelMotion;
            ID3D12Resource* temporalDepth = modelDepth;
            GuidanceResources::TemporalResidualConstants temporalConstants {
                nrWidth, nrHeight, temporalReset ? 1u : 0u, 0u,
                modelMvBaseX, modelMvBaseY, modelMvWidth, modelMvHeight,
                modelDepthBaseX, modelDepthBaseY, modelDepthWidth, modelDepthHeight,
                modelMvScaleX * nrWidth / std::max(modelMvWidth, 1u),
                modelMvScaleY * nrHeight / std::max(modelMvHeight, 1u),
                jitterCorrectionX, jitterCorrectionY };
            bool temporalGuidesReady = lowResolutionOriginalMVec;
            if (!lowResolutionOriginalMVec)
            {
                // Compact guidance remains readable through both temporal
                // consumers. Zero/scale/clone diagnostics only change NGX's
                // bindings, so reuse the original prepared scene guides.
                temporalGuidesReady = guidancePrepared ||
                    _guidance->Prepare(device, commandList, sourceMotion, sourceDepth,
                    sourceMvBaseX, sourceMvBaseY, mvWidth, mvHeight,
                    sourceDepthBaseX, sourceDepthBaseY, renderWidth, renderHeight,
                    sourceMvScaleX, sourceMvScaleY, nrWidth, nrHeight,
                    depthInverted, kTemporalGuidanceDescriptorBase,
                    resampledOriginOffsetX, resampledOriginOffsetY);
                if (temporalGuidesReady)
                {
                    temporalMotion = _guidance->preparedMotion;
                    temporalDepth = _guidance->preparedDepth;
                    temporalConstants.motionBaseX = temporalConstants.motionBaseY = 0;
                    temporalConstants.depthBaseX = temporalConstants.depthBaseY = 0;
                    temporalConstants.motionWidth = temporalConstants.depthWidth = nrWidth;
                    temporalConstants.motionHeight = temporalConstants.depthHeight = nrHeight;
                    temporalConstants.motionScaleX = temporalConstants.motionScaleY = 1.0f;
                }
            }
            ID3D12Resource* stabilizedResidual = nullptr;
            const bool temporalResolved = temporalGuidesReady &&
                _guidance->ResolveTemporalResidual(device, commandList, temporalMotion, temporalDepth,
                                                    temporalConstants, &stabilizedResidual);
            if (temporalResolved && stabilizedResidual != nullptr)
            {
                compositeCorrection = stabilizedResidual;
                temporalComposite = true;
                _previousTemporalJitterX = currentJitterX;
                _previousTemporalJitterY = currentJitterY;
                _hasPreviousTemporalJitter = true;
                if (_guidance->temporalFailureLogged || originalMVecChanged || _successfulEvaluations == 0)
                    LOG_INFO("[DLSSNR_LOWRES] temporal residual guides={} motion={}x{}+{},{} "
                             "scale={:.6f},{:.6f} depth={}x{}+{},{}",
                             lowResolutionOriginalMVec ? "model-original-mvec" : "resampled-original-guides",
                             temporalConstants.motionWidth, temporalConstants.motionHeight,
                             temporalConstants.motionBaseX, temporalConstants.motionBaseY,
                             temporalConstants.motionScaleX, temporalConstants.motionScaleY,
                             temporalConstants.depthWidth, temporalConstants.depthHeight,
                             temporalConstants.depthBaseX, temporalConstants.depthBaseY);
                _guidance->temporalFailureLogged = false;
            }
            else
            {
                _guidance->ResetTemporalHistory();
                _hasPreviousTemporalJitter = false;
                if (!_guidance->temporalFailureLogged)
                {
                    LOG_WARN("[DLSSNR_LOWRES] temporal residual reconstruction failed; using current residual");
                    _guidance->temporalFailureLogged = true;
                }
            }
        }
        const unsigned int compositeMode = debugModelOutput ? 2u : (lowResolutionFullOutput ? 1u : 0u);
        if (_successfulEvaluations == 0)
        {
            const auto baseDesc = lowCompositeBase != nullptr ? lowCompositeBase->GetDesc() : D3D12_RESOURCE_DESC{};
            const auto correctionDesc = compositeCorrection != nullptr ? compositeCorrection->GetDesc() : D3D12_RESOURCE_DESC{};
            LOG_INFO("[DLSSNR_LOWRES] composite contract mode={} guided={} featureGuided={} temporal={} base={}x{}+{},{} modelOrCorrection={}x{} low={}x{} output={}x{}",
                     compositeMode, highResolutionGuidedResidual ? 1 : 0,
                     featureGuidedReconstruction ? 1 : 0, temporalComposite ? 1 : 0,
                     baseDesc.Width, baseDesc.Height, lowCompositeBaseX, lowCompositeBaseY,
                     correctionDesc.Width, correctionDesc.Height, _guidance->lowWidth, _guidance->lowHeight,
                     outputWidth, outputHeight);
        }
        ID3D12Resource* compositeOutput = nullptr;
        if (lowCompositeBase == nullptr ||
            !_guidance->ApplyLowResidual(device, commandList, lowCompositeBase, compositeCorrection,
                                          compositeMode, temporalComposite, highResolutionGuidedResidual,
                                          featureGuidedReconstruction, fastReconstruction,
                                          lowCompositeBaseX, lowCompositeBaseY, 0, 0,
                                          outputWidth, outputHeight, static_cast<unsigned int>(configuredScale),
                                          &compositeOutput) ||
            compositeOutput == nullptr)
        {
            LogBypass("low-resolution composition failed");
            if (compositeUsesModelOutput && residualColorWritable)
                modelOutput.Restore();
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return false;
        }
        if (compositeUsesModelOutput && residualColorWritable)
            modelOutput.Restore();
        if (returnLateOutput)
        {
            // Avoid copying highComposite into the late adapter only for the
            // adapter to read it once more during packed/typed writeback.
            *lateOutput = compositeOutput;
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
        Transition(commandList, compositeOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool writeback = _guidance->ApplyColorCopy(device, commandList, compositeOutput, gameOutput,
                                                         roiExtrapolation ? extrapolationBaseX : outputBaseX,
                                                         roiExtrapolation ? extrapolationBaseY : outputBaseY,
                                                         roiExtrapolation ? extrapolationWidth : outputWidth,
                                                         roiExtrapolation ? extrapolationHeight : outputHeight,
                                                         gameOutputDesc.Format, 0, 0,
                                                         kExtrapolationDescriptorBase, false,
                                                         foveated ? (roiExtrapolation ? extrapolationOriginalSource : _guidance->roiColor.Get()) : nullptr,
                                                         roiEdgeBlendPx, roiEdgeBlendMask,
                                                         roiExtrapolation ? roiExtrapolationScale : 0.0f,
                                                         roiExtrapolation ? extrapolationRoiOffsetX : 0u,
                                                         roiExtrapolation ? extrapolationRoiOffsetY : 0u,
                                                         roiExtrapolation ? outputWidth : 0u,
                                                         roiExtrapolation ? outputHeight : 0u,
                                                         0u, 0u);
        if (!writeback)
        {
            LogBypass("low-resolution composite format conversion failed");
            Transition(commandList, compositeOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            return false;
        }
        Transition(commandList, compositeOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (debugStaged && debugInputViewComposite)
            _guidance->CompositeDebug(device, commandList, gameOutput, fullOutputBaseX, fullOutputBaseY);
        // Leave the game's output in UAV state, matching the existing direct
        // copy and HDR inverse paths for the next DLSS/NR invocation.
        _pendingReset = false;
        if (foveated)
        {
            _previousFoveatedRegion = *foveatedRegion;
            _hasPreviousFoveatedRegion = true;
        }
        _lastBypassReason.clear();
        const double recordCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - recordStart).count();
        if (debugModelOutput)
            LOG_DEBUG("[DLSSNR_LOWRES] displaying bilinear model output directly");
        else
            LOG_DEBUG("[DLSSNR_LOWRES] residual evaluated and composited into original output");
        LogStatistics(evaluateCpuMs, recordCpuMs,
                      static_cast<uint64_t>(outputWidth) * outputHeight * sizeof(uint16_t) * 4);
        return true;
    }

    if (colorTransferPrepared)
    {
        const float paperWhite = domain == ColorDomain::DisplaySDR ? 1.0f :
            domain == ColorDomain::DisplayLinearHDR ? hudlessHDRPaperWhiteNits / 80.0f :
            config.DLSSNRHDRPaperWhite.value_or(2.044f);
        constexpr float transferStrength = 1.0f;
        constexpr float colorStrength = 1.0f;
        modelOutput.Set(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* restoreDestination = gameOutput;
        if (returnLateOutput)
        {
            // NGX has consumed clampedColor. Reuse it for the restored result,
            // keeping the adapter's canonical source as the HDR reference.
            // This also removes the duplicate full-resolution source snapshot.
            restoreDestination = _guidance->clampedColor.Get();
            Transition(commandList, restoreDestination, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            _guidance->colorClampReadable = false;
        }
        else
        {
        Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        const bool inverseApplied = _guidance->ApplyColorTransferInverse(
            device, commandList, modelOutput.resource, restoreDestination, outputBaseX, outputBaseY, outputWidth, outputHeight,
            std::isfinite(paperWhite) ? paperWhite : 2.044f,
            std::isfinite(transferStrength) ? transferStrength : 1.0f,
            std::isfinite(colorStrength) ? colorStrength : 1.0f, gameOutputDesc.Format,
            exposureResource, preExposure, exposureScale, useGameExposure, sdrToneMapping,
            colorSource, colorSourceBaseX, colorSourceBaseY,
            roiEdgeBlendPx, roiEdgeBlendMask,
            roiExtrapolation ? roiExtrapolationScale : 0.0f,
            roiExtrapolation ? extrapolationBaseX : 0u,
            roiExtrapolation ? extrapolationBaseY : 0u,
            roiExtrapolation ? extrapolationWidth : 0u,
            roiExtrapolation ? extrapolationHeight : 0u,
            roiExtrapolation ? extrapolationRoiOffsetX : 0u,
            roiExtrapolation ? extrapolationRoiOffsetY : 0u,
            roiExtrapolation ? extrapolationOriginalSource : nullptr);
        modelOutput.Restore();
        if (returnLateOutput)
        {
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (inverseApplied) *lateOutput = restoreDestination;
        }
        if (inverseApplied)
        {
            if (debugStaged && debugInputViewComposite)
                _guidance->CompositeDebug(device, commandList, gameOutput, fullOutputBaseX, fullOutputBaseY);
            _pendingReset = false;
            if (foveated)
            {
                _previousFoveatedRegion = *foveatedRegion;
                _hasPreviousFoveatedRegion = true;
            }
            _lastBypassReason.clear();
            const double recordCpuMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - recordStart).count();
            LogStatistics(evaluateCpuMs, recordCpuMs,
                          static_cast<uint64_t>(outputWidth) * outputHeight * sizeof(uint16_t) * 4);
            return true;
        }
        if (sdrToneMapping)
        {
            LogBypass("SDR tone mapping restoration failed; preserving original DLSS output");
            _pendingReset = true;
            return false;
        }
        Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        LOG_WARN("[DLSSNR_COLOR] HDR inverse transfer failed; falling back to direct NR output copy");
    }

    if (colorFormatPrepared)
    {
        modelOutput.Set(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool converted = _guidance->ApplyColorCopy(
            device, commandList, modelOutput.resource, gameOutput,
            roiExtrapolation ? extrapolationBaseX : outputBaseX,
            roiExtrapolation ? extrapolationBaseY : outputBaseY,
            roiExtrapolation ? extrapolationWidth : outputWidth,
            roiExtrapolation ? extrapolationHeight : outputHeight, gameOutputDesc.Format,
            0, 0, kExtrapolationDescriptorBase, false,
            foveated ? (roiExtrapolation ? extrapolationOriginalSource : _guidance->roiColor.Get()) : nullptr,
            roiEdgeBlendPx, roiEdgeBlendMask,
            roiExtrapolation ? roiExtrapolationScale : 0.0f,
            roiExtrapolation ? extrapolationRoiOffsetX : 0u,
            roiExtrapolation ? extrapolationRoiOffsetY : 0u,
            roiExtrapolation ? outputWidth : 0u,
            roiExtrapolation ? outputHeight : 0u,
            0u, 0u);
        modelOutput.Restore();
        if (!converted)
        {
            LogBypass("could not convert RGBA16F NR output to the game's output format");
            Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return false;
        }
        if (debugStaged && debugInputViewComposite)
            _guidance->CompositeDebug(device, commandList, gameOutput, fullOutputBaseX, fullOutputBaseY);
        _pendingReset = false;
        if (foveated)
        {
            _previousFoveatedRegion = *foveatedRegion;
            _hasPreviousFoveatedRegion = true;
        }
        _lastBypassReason.clear();
        const double recordCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - recordStart).count();
        LogStatistics(evaluateCpuMs, recordCpuMs,
                      static_cast<uint64_t>(outputWidth) * outputHeight * sizeof(uint16_t) * 4);
        return true;
    }

    if (foveated && roiEdgeBlendPx != 0)
    {
        modelOutput.Set(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, gameOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool writeback = _guidance->ApplyColorCopy(
            device, commandList, modelOutput.resource, gameOutput,
            roiExtrapolation ? extrapolationBaseX : outputBaseX,
            roiExtrapolation ? extrapolationBaseY : outputBaseY,
            roiExtrapolation ? extrapolationWidth : outputWidth,
            roiExtrapolation ? extrapolationHeight : outputHeight, gameOutputDesc.Format,
            0, 0, kExtrapolationDescriptorBase, false,
            roiExtrapolation ? extrapolationOriginalSource : _guidance->roiColor.Get(),
            roiEdgeBlendPx, roiEdgeBlendMask,
            roiExtrapolation ? roiExtrapolationScale : 0.0f,
            roiExtrapolation ? extrapolationRoiOffsetX : 0u,
            roiExtrapolation ? extrapolationRoiOffsetY : 0u,
            roiExtrapolation ? outputWidth : 0u,
            roiExtrapolation ? outputHeight : 0u,
            0u, 0u);
        modelOutput.Restore();
        if (!writeback)
        {
            LogBypass("gaze ROI edge-blend writeback failed");
            return false;
        }
    }
    else
    {
        D3D12_RESOURCE_BARRIER postEvaluateBarriers[2] {};
        postEvaluateBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postEvaluateBarriers[0].Transition.pResource = modelOutput.resource;
        postEvaluateBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postEvaluateBarriers[0].Transition.StateBefore = modelOutput.state;
        postEvaluateBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        postEvaluateBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postEvaluateBarriers[1].Transition.pResource = gameOutput;
        postEvaluateBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postEvaluateBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        postEvaluateBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        commandList->ResourceBarrier(2, postEvaluateBarriers);
        modelOutput.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        D3D12_TEXTURE_COPY_LOCATION source { modelOutput.resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_TEXTURE_COPY_LOCATION destination { gameOutput, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_BOX sourceBox { 0, 0, 0, outputWidth, outputHeight, 1 };
        commandList->CopyTextureRegion(&destination, outputBaseX, outputBaseY, 0, &source, &sourceBox);
        D3D12_RESOURCE_BARRIER postCopyBarriers[2] {};
        postCopyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postCopyBarriers[0].Transition.pResource = gameOutput;
        postCopyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postCopyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        postCopyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        postCopyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postCopyBarriers[1].Transition.pResource = modelOutput.resource;
        postCopyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postCopyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        postCopyBarriers[1].Transition.StateAfter = modelOutput.restore;
        commandList->ResourceBarrier(2, postCopyBarriers);
        modelOutput.state = modelOutput.restore;
    }
    // Debug imagery belongs only in the visible output. The selected model
    // result may be persistent temporal history used by the next frame.
    if (debugStaged && debugInputViewComposite)
        _guidance->CompositeDebug(device, commandList, gameOutput, fullOutputBaseX, fullOutputBaseY);
    _pendingReset = false;
    if (foveated)
    {
        _previousFoveatedRegion = *foveatedRegion;
        _hasPreviousFoveatedRegion = true;
    }
    _lastBypassReason.clear();
    const double recordCpuMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - recordStart).count();
    LogStatistics(evaluateCpuMs, recordCpuMs,
                  static_cast<uint64_t>(outputWidth) * outputHeight * sizeof(uint16_t) * 4);
    return true;
}
