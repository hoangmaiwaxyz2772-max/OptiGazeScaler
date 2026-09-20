// EDGE_ONLY_V11
// Colour-supported boundary correction with bounded temporal stabilization.
cbuffer Params : register(b0)
{
    uint2 OutputSize; uint2 DestinationBase;
    uint2 OriginalBase; uint2 RoiOffset;
    uint2 RoiSize; uint EdgeBlendData; uint OuterWidth;
    uint2 GridSize; uint2 GridPhase;
    uint Step; uint2 TileSize; float HistoryWeight;
};
static const uint Pitch = 32u;
float3 EncodeAppearance(float3 v)
{
    // Finite derivative at black; inverse is exact, including signed inputs.
    return sign(v) * (sqrt(min(abs(v), 65504.0) + 0.000001) - 0.001);
}
float3 DecodeAppearance(float3 v)
{
    return sign(v) * (abs(v) * abs(v) + 0.002 * abs(v));
}
float Smooth(float x)
{
    x = saturate(x);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}
float InnerWidth()
{
    return min(float(EdgeBlendData & 65535u), max(1.0, 0.5 * float(min(RoiSize.x, RoiSize.y) - 1u)));
}
float InnerDistance(float2 p)
{
    float2 lo = float2(RoiOffset), hi = lo + float2(RoiSize) - 1.0;
    float4 d = float4(p - lo, hi - p);
    // Keep the same rounded shape when touching a screen edge. Toggling
    // edge-mask bits there would abruptly change a whole corner of the field.
    uint mask = 15u;
    float w = InnerWidth(), distance = w;
    if (mask & 1u) distance = min(distance, d.x);
    if (mask & 2u) distance = min(distance, d.y);
    if (mask & 4u) distance = min(distance, d.z);
    if (mask & 8u) distance = min(distance, d.w);
    float radius = min(2.0 * w, 0.5 * min(hi.x - lo.x, hi.y - lo.y));
    if ((mask & 3u) == 3u) distance = min(distance, radius - length(max(radius - d.xy, 0.0)));
    if ((mask & 6u) == 6u) distance = min(distance, radius - length(max(radius - d.zy, 0.0)));
    if ((mask & 12u) == 12u) distance = min(distance, radius - length(max(radius - d.zw, 0.0)));
    if ((mask & 9u) == 9u) distance = min(distance, radius - length(max(radius - d.xw, 0.0)));
    return distance;
}
bool Inside(int2 p)
{
    return all(p >= int2(RoiOffset)) && all(p < int2(RoiOffset + RoiSize));
}
uint2 Address(uint2 p, uint layer)
{
    return p + uint2(layer & 3u, layer >> 2u) * TileSize;
}
float3 Chromaticity(float3 rgb)
{
    float3 positive = max(rgb, 0.0) + 0.00001;
    return positive / dot(positive, 1.0);
}
float HueCoordinate(float3 chroma)
{
    float2 opponent = float2(chroma.r - 0.5 * (chroma.g + chroma.b),
                             0.8660254 * (chroma.g - chroma.b));
    // Neutral samples do not contribute to hue, but avoid atan2(0,0).
    if (dot(opponent, opponent) < 1e-12) return 0.0;
    return frac(atan2(opponent.y, opponent.x) * 0.159154943 + 1.0) * 12.0;
}
float ColouredFraction(float3 chroma)
{
    float3 d = chroma - 1.0 / 3.0;
    return Smooth(length(d) / 0.08);
}
float4 HueBins(float hue, uint block)
{
    float4 d = abs(hue - (float4(0, 1, 2, 3) + float(block * 4u)));
    float4 t = saturate(1.0 - min(d, 12.0 - d));
    // Periodic smooth splat; its two nonzero weights sum to one, including
    // across red's wrap. Small hue changes never flip a histogram bucket.
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}
float3 Shoulder(float3 value, float3 limit)
{
    // Identity below the limit, C1 shoulder above it; no weak channel can
    // suppress a valid change in the other channels.
    limit = max(limit, 0.000001);
    float3 excess = max(abs(value) - limit, 0.0);
    return sign(value) * (min(abs(value), limit) + limit * excess / (limit + excess));
}
#if DLSSNR_BOUNDARY_STAGE == 0
Texture2D<float4> Model : register(t0);
Texture2D<float4> Original : register(t1);
RWTexture2D<float4> Seeds : register(u0);
groupshared float4 Moments[704];
[numthreads(8, 8, 1)]
void CSMain(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    if (any(group.xy >= GridSize)) return;
    float4 sums[11];
    [unroll] for (uint record = 0u; record < 11u; ++record) sums[record] = 0.0;
    // Fixed screen-phase 2x2 sampling: 256 observations per 32px tile.
    // No rotating sample pattern or gaze-relative phase.
    int2 origin = int2(group.xy * Pitch) - int2(GridPhase) - 16;
    if (any(origin + 31 < int2(RoiOffset)) || any(origin >= int2(RoiOffset + RoiSize)))
    {
        // Uniform empty-tile path: clear required records without allocating
        // work to the eleven shared reductions across the unused screen.
        if (index < 11u) Seeds[Address(group.xy, index)] = 0.0;
        return;
    }
    {
        [unroll] for (int y = 0; y < 2; ++y)
        [unroll] for (int x = 0; x < 2; ++x)
        {
            int2 q = origin + int2(lane.xy * 4u) + int2(x, y) * 2 + 1;
            if (!Inside(q)) continue;
            float3 source = Original.Load(int3(int2(OriginalBase) + q, 0)).rgb;
            float3 model = Model.Load(int3(q - int2(RoiOffset), 0)).rgb;
            if (!all(isfinite(source)) || !all(isfinite(model))) continue;
            source = EncodeAppearance(source);
            model = EncodeAppearance(model);
            float4 d = float4(q - int2(RoiOffset), int2(RoiOffset + RoiSize) - 1 - q);
            // Dense observations allow a shorter entrance ramp without sparse
            // donor switches. It still has zero endpoint slope and curvature.
            float coverage = Smooth(min(min(d.x, d.y), min(d.z, d.w)) /
                                    max(1.0, min(32.0, 0.25 * float(min(RoiSize.x, RoiSize.y)))));
            coverage /= 256.0;
            float peak = max(max(abs(source.r), abs(source.g)), abs(source.b));
            float3 delta = Shoulder(model - source, 8.0 * max(peak, 0.25));
            // Weight depends only on original radiance. FP32 raw moments retain
            // affine gain AND additive changes rather than averaging ratios.
            float weight = coverage / (max(peak, 1.0) * max(peak, 1.0));
            float3 chroma = Chromaticity(source);
            sums[0] += float4(source, 1.0) * weight;
            sums[1] += float4(source * source, 0.0) * weight;
            sums[2] += float4(delta, 0.0) * weight;
            sums[3] += float4(source * delta, 0.0) * weight;
            // Colour coverage is brightness-independent, as in the old fits.
            sums[4] += float4(chroma, 1.0) * coverage;
            sums[5] += float4(chroma * chroma, 0.0) * coverage;
            sums[6] += float4(chroma.xxy * chroma.yzz, 0.0) * coverage;
            sums[7] += float4(delta * delta, 0.0) * weight;
            float coloured = ColouredFraction(chroma), hue = HueCoordinate(chroma);
            sums[7].a += (1.0 - coloured) * coverage;
            [unroll] for (uint h = 0u; h < 3u; ++h)
                sums[8u + h] += HueBins(hue, h) * (coloured * coverage);
        }
    }
    [unroll] for (uint r = 0u; r < 11u; ++r) Moments[r * 64u + index] = sums[r];
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (index < stride)
            [unroll] for (uint r = 0u; r < 11u; ++r)
                Moments[r * 64u + index] += Moments[r * 64u + index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index < 11u) Seeds[Address(group.xy, index)] = Moments[index * 64u];
}
#elif DLSSNR_BOUNDARY_STAGE == 1
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= GridSize)) return;
    float4 sum = 0.0;
    // Separable form of the previous four 3x3 kernels (steps 1,1,1,2).
    // Same interior support/weights; 22 atlas taps instead of 36, two passes.
    static const float kernel[11] = { 1, 6, 17, 32, 46, 52, 46, 32, 17, 6, 1 };
    [unroll] for (int tap = -5; tap <= 5; ++tap)
    {
        int2 q = int2(id.xy) + (Step == 0u ? int2(tap, 0) : int2(0, tap));
        if (any(q < 0) || any(q >= int2(GridSize))) continue;
        sum += Source.Load(int3(Address(uint2(q), id.z), 0)) * (kernel[tap + 5] / 256.0);
    }
    // Propagate sufficient statistics, with no intermediate fits, donor
    // selection or guide rejection that could cut holes through texture.
    Target[Address(id.xy, id.z)] = sum;
}
#elif DLSSNR_BOUNDARY_STAGE == 2
Texture2D<float4> Broad : register(t0);
Texture2D<float4> PreviousMaps : register(t1);
RWTexture2D<float4> Maps : register(u0);
float4 Moment(uint2 p, uint record, uint nearField)
{
    // A single local fit from the spatially filtered moments.
    return Broad.Load(int3(Address(p, record), 0));
}
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= GridSize)) return;
    uint first = id.z * 11u;
    float4 s0 = Moment(id.xy, 0u, id.z), c0 = Moment(id.xy, 4u, id.z);
    if (s0.a <= 1e-20 || c0.a <= 1e-8)
    {
        [unroll] for (uint r = 0u; r < 11u; ++r) Maps[Address(id.xy, first + r)] = 0.0;
        return;
    }
    float3 mean = s0.rgb / s0.a;
    float3 second = Moment(id.xy, 1u, id.z).rgb / s0.a;
    float3 delta = Moment(id.xy, 2u, id.z).rgb / s0.a;
    float3 covariance = Moment(id.xy, 3u, id.z).rgb / s0.a - mean * delta;
    float3 variance = max(second - mean * mean, 0.0);
    // Ridge regularization is part of the current spatial estimate, not
    // temporal convergence. Flat samples retain their observed mean exactly;
    // well-observed ramps learn independent per-channel slopes.
    float energy = dot(mean, mean);
    float3 priorGain = delta * mean / (mean * mean + 0.0025 * energy + 0.000001);
    float3 ridge = 0.0025 * max(second, 0.0004);
    float3 gain = Shoulder((covariance + ridge * priorGain) / (variance + ridge), 4.0);
    float3 intercept = delta - gain * mean;
    float3 rmsDelta = sqrt(max(Moment(id.xy, 7u, id.z).rgb / s0.a, 0.0));

    float3 chroma = c0.rgb / c0.a;
    float3 diagonal = max(Moment(id.xy, 5u, id.z).rgb / c0.a - chroma * chroma, 0.0) + 0.0009;
    float3 off = Moment(id.xy, 6u, id.z).rgb / c0.a - chroma.xxy * chroma.yzz;
    float3 adjD = diagonal.yxx * diagonal.zzy - off.zyx * off.zyx;
    float3 adjO = float3(off.y * off.z - off.x * diagonal.z,
                        off.x * off.z - off.y * diagonal.y,
                        off.x * off.y - off.z * diagonal.x);
    float determinant = max(diagonal.x * adjD.x + off.x * adjO.x + off.y * adjO.y, 1e-12);
    float strength = Smooth(c0.a / 0.005);
    float3 stableGain = gain, stableIntercept = intercept;
    if (HistoryWeight > 0.0)
    {
        float4 oldGain = PreviousMaps.Load(int3(Address(id.xy, first), 0));
        float4 oldMean = PreviousMaps.Load(int3(Address(id.xy, first + 2u), 0));
        float oldBrightness = PreviousMaps.Load(int3(Address(id.xy, first + 1u), 0)).a;
        float3 colourChange = chroma - oldMean.rgb;
        float brightnessChange = abs(dot(mean, 1.0 / 3.0) - oldBrightness) /
                                 max(0.03, 0.25 * max(oldMean.a, sqrt(max(energy, 0.0) / 3.0)));
        float distributionChange = 0.0;
        [unroll] for (uint h = 0u; h < 3u; ++h)
        {
            float4 currentHue = Moment(id.xy, 8u + h, id.z) / c0.a;
            float4 oldHue = PreviousMaps.Load(int3(Address(id.xy, first + 6u + h), 0));
            distributionChange += dot(abs(currentHue - oldHue), 1.0);
        }
        float acceptance = (1.0 - Smooth(dot(colourChange, colourChange) / 0.01)) *
                           (1.0 - Smooth(brightnessChange)) *
                           (1.0 - Smooth(distributionChange / 0.6));
        float weight = min(HistoryWeight, 0.55) * acceptance * min(strength, oldGain.a);
        float3 oldStableGain = PreviousMaps.Load(int3(Address(id.xy, first + 9u), 0)).rgb;
        float3 oldStableIntercept = PreviousMaps.Load(int3(Address(id.xy, first + 10u), 0)).rgb;
        if (all(isfinite(oldStableGain)) && all(isfinite(oldStableIntercept)))
        {
            stableGain = lerp(gain, oldStableGain, weight);
            stableIntercept = lerp(intercept, oldStableIntercept, weight);
        }
    }
    Maps[Address(id.xy, first + 0u)] = float4(gain, strength);
    Maps[Address(id.xy, first + 1u)] = float4(intercept, dot(mean, 1.0 / 3.0));
    Maps[Address(id.xy, first + 2u)] = float4(chroma, sqrt(max(max(second.r, second.g), max(second.b, 0.000001))));
    Maps[Address(id.xy, first + 3u)] = float4(adjD / determinant, sqrt(dot(variance, 1.0 / 3.0)));
    Maps[Address(id.xy, first + 4u)] = float4(adjO / determinant, Moment(id.xy, 7u, id.z).a / c0.a);
    Maps[Address(id.xy, first + 5u)] = float4(rmsDelta, 0.0);
    Maps[Address(id.xy, first + 9u)] = float4(stableGain, 0.0);
    Maps[Address(id.xy, first + 10u)] = float4(stableIntercept, 0.0);
    [unroll] for (uint h = 0u; h < 3u; ++h)
        Maps[Address(id.xy, first + 6u + h)] = Moment(id.xy, 8u + h, id.z) / c0.a;
}
#else
Texture2D<float4> Model : register(t0);
Texture2D<float4> Original : register(t1);
Texture2D<float4> Maps : register(t2);
RWTexture2D<float4> Output : register(u0);
// Projection onto the ROI is continuous and monotone on each axis. A group's
// projected footprint is no larger than its 8x8 footprint, including corners.
// Nine fixed-screen nodes therefore cover all lanes without donor switches.
groupshared float4 Cached[99];
float4 Gather(float2 node, uint2 cacheOrigin, float3 source, float temporalInfluence)
{
    uint2 base = uint2(floor(node)) - cacheOrigin;
    float2 f = float2(Smooth(frac(node.x)), Smooth(frac(node.y)));
    float3 chroma = Chromaticity(source);
    float hue = HueCoordinate(chroma), coloured = ColouredFraction(chroma);
    float4 hue0 = HueBins(hue, 0u), hue1 = HueBins(hue, 1u), hue2 = HueBins(hue, 2u);
    float peak = max(max(abs(source.r), abs(source.g)), abs(source.b));
    float4 sum = 0.0;
    [unroll] for (uint tap = 0u; tap < 4u; ++tap)
    {
        uint2 o = uint2(tap & 1u, tap >> 1u), cell = base + o;
        uint address = (cell.y * 3u + cell.x) * 11u;
        float4 gain = Cached[address], mean = Cached[address + 2u];
        float3 v = chroma - mean.rgb;
        float mahalanobis = max(0.0, dot(Cached[address + 3u].rgb, v * v) +
                                    2.0 * dot(Cached[address + 4u].rgb, v.xxy * v.yzz));
        float2 w = lerp(1.0 - f, f, float2(o));
        // Covariance alone can declare a broad mixture of objects compatible
        // with an unrelated colour. Bound absolute chromaticity distance too.
        // This is a soft interval, not a binary texture-edge rejection.
        float colourSupport = Smooth((0.04 - dot(v, v)) / 0.0375);
        // Mixed objects must not manufacture support for a hue between them.
        // Histogram mass retains separated modes; neutrals have their own mass.
        float hueSupport = dot(hue0, Cached[address + 6u]) +
                           dot(hue1, Cached[address + 7u]) + dot(hue2, Cached[address + 8u]);
        float distributionSupport = Smooth(4.0 * lerp(Cached[address + 4u].a, hueSupport, coloured));
        // An observed mode may legitimately be far from the mixture's mean.
        // Let actual distribution evidence rescue it, never covariance alone.
        colourSupport = max(colourSupport, distributionSupport);
        float brightnessScale = Cached[address + 3u].a + 0.08 * mean.a + 0.015;
        float brightnessError = abs(dot(source, 1.0 / 3.0) - Cached[address + 1u].a) / brightnessScale;
        float brightnessTail = max(brightnessError - 2.0, 0.0);
        float weight = w.x * w.y * gain.a * colourSupport * distributionSupport *
                       exp2(-0.5 * (mahalanobis + brightnessTail * brightnessTail));
        float3 prediction = gain.rgb * source + Cached[address + 1u].rgb;
        float3 stablePrediction = Cached[address + 9u].rgb * source + Cached[address + 10u].rgb;
        // Current source support always wins. History only adjusts a bounded
        // correction, never source detail, alpha, hue coverage or outer fade.
        float3 limit = 0.025 + 0.5 * abs(prediction);
        prediction += temporalInfluence * clamp(stablePrediction - prediction, -limit, limit);
        float3 bound = 4.0 * Cached[address + 5u].rgb * max(1.0, peak / max(mean.a, 0.001)) + 0.00001;
        sum += float4(Shoulder(prediction, bound), 1.0) * weight;
    }
    // Retain absolute support after normalization. A weakly matching donor
    // must not regain full strength just because every donor is a poor match.
    float confidence = Smooth(sum.a / 0.5);
    return float4(sum.rgb * (confidence / max(sum.a, 0.000001)), confidence);
}
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    uint2 lo = group.xy * 8u, hi = min(lo + 7u, OutputSize - 1u);
    // Uniform early return, before the shared-memory barrier. Exact core
    // groups need neither appearance coefficients nor the original image.
    float closest = min(min(InnerDistance(lo), InnerDistance(hi)),
                        min(InnerDistance(float2(lo.x, hi.y)), InnerDistance(float2(hi.x, lo.y))));
    if (Inside(int2(lo)) && Inside(int2(hi)) && closest >= InnerWidth())
    {
        if (all(id.xy < OutputSize))
            Output[DestinationBase + id.xy] = Model.Load(int3(int2(id.xy) - int2(RoiOffset), 0));
        return;
    }
    // Groups outside local support preserve the source without map traffic.
    float2 gap = max(float2(RoiOffset) - float2(hi),
                     float2(lo) - float2(RoiOffset + RoiSize - 1u));
    if (max(gap.x, gap.y) > 0.0 && length(max(gap, 0.0)) >= float(OuterWidth))
    {
#if !DLSSNR_BOUNDARY_INPLACE
        if (all(id.xy < OutputSize))
            Output[DestinationBase + id.xy] = Original.Load(int3(OriginalBase + id.xy, 0));
#endif
        return;
    }
    uint2 cacheOrigin = (clamp(lo, RoiOffset, RoiOffset + RoiSize - 1u) + GridPhase) / Pitch;
    [unroll] for (uint item = index; item < 99u; item += 64u)
    {
        uint record = item % 11u, cell = item / 11u;
        uint2 q = min(cacheOrigin + uint2(cell % 3u, cell / 3u), GridSize - 1u);
        Cached[item] = Maps.Load(int3(Address(q, record), 0));
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id.xy >= OutputSize)) return;
    int2 p = int2(id.xy);
    bool inside = Inside(p);
    float distance = InnerDistance(p);
    float a = inside ? Smooth(distance / InnerWidth()) : 0.0;
    if (a >= 1.0)
    {
        Output[DestinationBase + id.xy] = Model.Load(int3(p - int2(RoiOffset), 0));
        return;
    }
#if DLSSNR_BOUNDARY_INPLACE
    float4 original = Output[DestinationBase + id.xy];
#else
    float4 original = Original.Load(int3(OriginalBase + id.xy, 0));
#endif
    // Decay is a fixed screen-space curve, independent of the requested
    // reach. Extending reach must not stretch projected boundary colour bands.
    // Only the terminal fade moves with OuterWidth; its width is capped too.
    float exteriorDistance = max(-distance, 0.0);
    float decayDistance = exteriorDistance / 192.0;
    float endFadeWidth = max(1.0, min(128.0, 0.25 * float(OuterWidth)));
    float outer = OuterWidth > 0u
        ? exp2(-decayDistance * decayDistance) *
          Smooth((float(OuterWidth) - exteriorDistance) / endFadeWidth)
        : 0.0;
    float2 node = (clamp(float2(id.xy), float2(RoiOffset), float2(RoiOffset + RoiSize - 1u)) + float2(GridPhase)) / float(Pitch);
    float3 encoded = EncodeAppearance(original.rgb);
    // Temporal influence grows smoothly with distance. Use distance to
    // the actual rectangle, so even its rounded corners have a current-only edge.
    float2 outsideDelta = max(max(float2(RoiOffset) - float2(p),
                                  float2(p) - float2(RoiOffset + RoiSize - 1u)), 0.0);
    float temporalInfluence = Smooth(length(outsideDelta) / 96.0);
    float3 residual = 0.0;
    if (outer > 0.0) residual = Gather(node, cacheOrigin, encoded, temporalInfluence).rgb;
    // Project only invalid exterior channels. A tiny negative red prediction
    // must not erase valid green/blue correction. Blend the valid exterior
    // with the signed model afterwards, keeping the exact core contract.
    float3 result = max(DecodeAppearance(encoded + residual * outer), min(original.rgb, 0.0));
    float alpha = original.a;
    if (inside)
    {
        float4 model = Model.Load(int3(p - int2(RoiOffset), 0));
        // The SAME field on both sides of the ROI. All model detail fades
        // toward that field inside, instead of exposing a short detail seam.
        result = lerp(result, model.rgb, a);
        alpha = lerp(original.a, model.a, a);
    }
    result = all(isfinite(result)) ? clamp(result, -65504.0, 65504.0) : original.rgb;
    Output[DestinationBase + id.xy] = float4(result, alpha);
}
#endif
