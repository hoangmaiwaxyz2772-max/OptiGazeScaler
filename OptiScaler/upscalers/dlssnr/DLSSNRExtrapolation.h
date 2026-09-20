#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DlssNrExtrapolation
{
// The production learn/apply shaders share this compact, fixed-size bank.
// Screen-anchored 32/16/8/4/2/1 grids each have a one-cell halo on both
// sides. Ten texels encode a colour/spatial fit, support and observed bounds;
// the global record uses three more for its bounded nonlinear RGB basis.
// Local texel 8.w is a soft HDR radiance support limit (zero is unrestricted).
inline constexpr unsigned int FieldSide = 34;
inline constexpr unsigned int RecordTexels = 13;
inline constexpr unsigned int FieldWidth = FieldSide * RecordTexels;
// 34+18+10+6+4+3 local rows, followed by the global fit.
inline constexpr unsigned int FieldHeight = 76;
inline constexpr unsigned int StatisticsTexels = 28;
inline constexpr unsigned int StatisticsWidth = FieldSide * StatisticsTexels;
inline constexpr unsigned int StatisticsHeight = FieldSide;
// Persistent full-output cache: affine maps plus colour support and correction
// bounds for both displayed and target records, and two original-scene guides.
// The first ten planes retain the original layout; planes 10..13 and 14..17
// contain displayed/target support and bounds. FP16 payload at 4K: 4.50 MiB/bank.
// Displayed/target support texel 4.w retains the radiance limit after confidence
// has been folded into the map weight; no extra plane or resource is required.
inline constexpr unsigned int CachePitch = 16;
inline constexpr unsigned int CachePlanes = 18;

// Sampling shares the first ten geometry constants with application. The
// remaining constants carry the exact HDR restoration contract and sampling
// cadence; the effective white is resolved on the GPU once per observation.
struct SampleConstants
{
    uint32_t width, height, destinationX, destinationY;
    uint32_t originalX, originalY, roiX, roiY, roiWidth, roiHeight;
    float restorationWhite = 0, restorationPreExposure = 1;
    uint32_t restorationFlags = 0, sampleFrame = 0, sparseSampling = 0;
    float restorationExposureScale = 1;
};
static_assert(sizeof(SampleConstants) == 16 * sizeof(uint32_t));

inline float SourceHistoryWeight(float seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0f || seconds > 0.5f)
        return 0.0f;
    return std::min(0.9999f, std::exp(-seconds / 1.2f));
}

// Shared with the independent GPU fixture; seven HLSL constant-buffer rows.
struct TemporalConstants
{
    uint32_t roiX = 0, roiY = 0, previousRoiX = 0, previousRoiY = 0;
    uint32_t roiWidth = 0, roiHeight = 0, previousRoiWidth = 0, previousRoiHeight = 0;
    uint32_t motionBaseX = 0, motionBaseY = 0, motionWidth = 0, motionHeight = 0;
    uint32_t depthBaseX = 0, depthBaseY = 0, depthWidth = 0, depthHeight = 0;
    float motionScaleX = 0, motionScaleY = 0, jitterCorrectionX = 0, jitterCorrectionY = 0;
    uint32_t originalBaseX = 0, originalBaseY = 0, outputWidth = 0, outputHeight = 0;
    float historyWeight = 0;
    uint32_t historyValid = 0, depthInverted = 0, padding = 0;
};
static_assert(sizeof(TemporalConstants) == 28 * sizeof(uint32_t));

inline float TemporalWeight(float seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0f || seconds > 0.5f)
        return 0.0f;
    return std::min(0.97f, std::exp(-seconds / 0.12f));
}

struct Axis
{
    unsigned int base, size, roiOffset, allocationSize;
};

// Current boundary algorithm: bounded pixel extent, clipped independently.
// Allocation does not depend on gaze, including at the edge of the image.
inline Axis ExpandBandAxis(unsigned int roiBase, unsigned int roiSize,
                           unsigned int fullBase, unsigned int fullSize, unsigned int band)
{
    band = std::min(band, 2048u);
    const auto left = std::min(roiBase - fullBase, band);
    const auto right = std::min(fullSize - (roiBase - fullBase) - roiSize, band);
    return { roiBase - left, roiSize + left + right, left, std::min(fullSize, roiSize + 2u * band) };
}

// Inputs describe a valid ROI inside the logical output, including allocation
// bases. Clip each side independently, preserving the unclipped center.
inline Axis ExpandAxis(unsigned int roiBase, unsigned int roiSize,
                       unsigned int fullBase, unsigned int fullSize, float scale)
{
    scale = std::isfinite(scale) ? std::clamp(scale, 1.0f, 4.0f) : 1.0f;
    // Match the float multiplication used by the analytic HLSL outer mask.
    const auto desired = static_cast<unsigned int>(std::ceil(float(roiSize) * scale));
    const auto extra = desired - roiSize;
    const auto left = std::min(roiBase - fullBase, extra / 2);
    const auto right = std::min(fullSize - (roiBase - fullBase) - roiSize, extra - extra / 2);
    return { roiBase - left, roiSize + left + right, left, std::min(fullSize, desired) };
}
}
