# DLSS / Ray Reconstruction Gaze ROI MVP

This branch contains experimental D3D12 DLSS Super Resolution and Ray Reconstruction ROI paths for gaze-driven local reconstruction. Neither is a general OptiScaler feature yet.

## Current Scope

- API: D3D12 only.
- Upscaler: DLSS Super Resolution and DLSS Ray Reconstruction.
- Input: virtual gaze point controlled by mouse, keyboard, shared memory, or legacy UDP.
- ROI: one rectangular DLSS region composited over a low-cost full-frame upscale.
- Frame generation combinations, Vulkan and DX11 are out of scope for this MVP.

## Runtime Controls

Open the OptiScaler overlay and use **Gaze ROI DLSS** or **Gaze ROI Ray Reconstruction**. Enable, ROI size, feather, gaze input, and subpixel-jitter controls are shared; the peripheral algorithm and model-specific Color bypass switch automatically with the active feature.

- **Enable Gaze ROI**: turns the experimental path on or off.
- **ROI Width / ROI Height**: edit the target ROI size in output/upscaled pixels. The running DLSS feature is not
  changed until **Apply ROI Size** is pressed, so partial text input cannot trigger handle recreation.
- **Feather**: current inward blend width in output pixels. Default is `96`.
- **Peripheral Blur**: applies a single-frame 9-tap tent blur to the low-cost peripheral upscale.
- **Blur Radius**: blur radius in low-resolution source texels. Default is `1.0`.
- **Peripheral Jitter Cancel**: offsets low-resolution peripheral sampling by the DLSS jitter offset before blur.
- **Jitter Sign**: experimental sign for jitter cancellation. Test `+1` and `-1`; keep the sign that reduces static-scene shimmer.
- **Peripheral Temporal Stabilizer**: enables a low-resolution temporal history pass for the peripheral image.
- **Stabilization Strength**: ordinary DLSS peripheral accumulation strength. MV reprojection, native MV/jitter scaling, and depth rejection are always used while temporal stabilization is enabled. The older current-weight/reactive-scale controls are hidden; reactive scale is fixed and strength maps to a bounded current-frame weight.
- **RR Spatial Passes / Radius / Temporal History**: Ray Reconstruction periphery controls. The filter runs at half linear render resolution and uses only Color, Depth, and original game MV.
- **Debug Border**: draws a red rectangle around the composited ROI.
- **Motion Vectors (1:1)**: overlays the exact cropped and origin-corrected MV resource consumed by the active private DLSS/RR model.
- **Current/Raw Color Bypass**: skips only the private model for the ROI and point-scales the same zero-based Color input; the periphery and final composite stay unchanged.
- **Show Advanced Debug**: reveals magenta pre-clear, border, standalone RR filter views, parameter-isolation tests, reset tests, and GPU timing controls. These are hidden by default.
- **Private Output Magenta Pre-Clear**: diagnostic only. Clears the entire zero-based private DLSS ROI output immediately before Evaluate. Magenta remaining after Evaluate means NGX did not overwrite those local pixels; old scene content means NGX wrote that content from its own path or history.
- **GPU Timing (MV / DLSS / Peripheral / Composite)**: diagnostic only. Adds D3D12 timestamp queries and logs split `peripheralMs`, `compositeMs`, aggregate `postMs`, and the existing MV/DLSS/total values after the frame-slot fence completes. Leave it disabled for production measurements that do not need per-stage timing.
- **Current Color Point Bypass**: diagnostic only. Skips private DLSS Evaluate, nearest-neighbor scales the exact current Color ROI into the same private output, and retains the normal peripheral/composite path.
- **Zero-Based Color Copy**: copies the current Color ROI without resampling into an identically formatted zero-based texture and supplies private DLSS with Color base `(0,0)`.
- **Motion Vector Mode**: controls only the extra motion-vector offset caused by moving the ROI:
  - `Disabled`: copy the game's MV unchanged.
  - `InputDelta`: current scaled input-origin correction.
  - `InputDeltaReversed`: reverse the current correction.
  - `InputDeltaUnscaled`: use the input-origin delta without MV scale conversion.
  - `OutputDelta`: derive the correction from output-origin movement and map it to render space.
  - `OutputDeltaReversed`: reverse the output-origin correction.
- **Control**:
  - `Mouse`: use the foreground window cursor position as the gaze point.
  - `ExternalSharedMemory`: receive gaze from `Local\EyeTracingGazeV1` shared memory.
  - `ExternalUdp`: receive gaze from the Node bridge documented in [GazeRoiExternalInput.md](GazeRoiExternalInput.md).
  - `Keyboard`: use `F5/F6/F7/F8` to move and `F9` to center.

Changing **ROI Width** or **ROI Height** changes the ROI DLSS feature size. The implementation will recreate the ROI DLSS feature when the ROI dimensions change. The requested size is expressed in final output pixels; the actual DLSS subrect can be adjusted by a few pixels because the render-space input rect is aligned for DLSS and then mapped back to output-space.

## INI Settings

```ini
[GazeRoi]
Enabled=false
WidthPx=1280
HeightPx=720
FeatherPx=96
PeripheralReconstructionMode=1
PeripheralTemporalDetail=false
PeripheralTemporalDetailScale=1.0
PeripheralTemporalDetailStrength=2.0
PeripheralBlur=true
PeripheralBlurRadius=1.0
PeripheralJitterCancel=true
PeripheralJitterSign=1
MotionVectorMode=InputDelta
PeripheralTemporalHistoryWeight=0.75
PeripheralTemporalReactiveScale=2.0
DebugBorder=false
MotionVectorDebugView=false
OutputClearDebug=false
CurrentColorPointBypass=false
ColorCopy=false
DepthCopy=true
OmitBiasCurrentColorHint=false
MinimalPrivateParameters=false
ResetOnMove=false
GpuTiming=false
ShowAdvancedDebug=false
Control=Keyboard
UdpPort=38479
StaleMs=50
```

`WidthPx` and `HeightPx` are output/upscaled pixel counts. For eye-tracking experiments a square region such as `960 x 960` or `1280 x 1280` is usually the most useful starting point.

`PeripheralReconstructionMode=0` is a performance baseline: it runs the same jitter-aware 3x3 current reconstruction as mode 1, but reads no Depth/MV and allocates no temporal history. `PeripheralReconstructionMode=1` is the default lightweight Cauldron/MJP-derived TAA with variance clipping, closest-depth MV/Depth, conservative previous-Depth coverage, PreExposure-normalized comparison, confidence-selected bilinear/Catmull-Rom history, and thin-feature preservation. `PeripheralTemporalDetail=true` adds an `RG16_FLOAT` signed-luminance-residual/depth history and adds it to the render-resolution TAA base in the existing final composite. At Scale `1.0`, detail history is updated inside the main TAA dispatch and reuses its closest-depth/MV result. Higher scales use a separate pass. `PeripheralReconstructionMode=2` is the experimental fixed-1.25x lightweight TAAU: one independent pass uses a jitter-aware 3x3 approximate-Lanczos current reconstruction, same-sample YCoCg rectification statistics, alpha-packed accumulation/lock state, and the existing MV/depth disocclusion contract on Color/R32 Depth histories at `min(output, render * 1.25)`. It also applies the shared `Detail Strength` to a five-point residual derived from the same nine samples before temporal accumulation, adding no read, texture, or dispatch. It replaces rather than enlarges mode 1's base-plus-detail paths and allocates no residual history. The former enlarged-mode-1, EASU, joint dual-band, and output-resolution UE-TAAU runtime options are retired after testing or analysis found poor quality/cost; their implementation notes remain in `docs/issues/peripheral-lightweight-taa.md`.

`CurrentColorPointBypass=true` skips NGX Evaluate and point-scales current Color into the existing private output; it does not pass jitter because no DLSS call occurs. `ColorCopy=true` uses an input-ROI-sized, identically formatted zero-based Color copy and preserves the original jitter supplied to DLSS. `DepthCopy=true` uses the ROI-sized zero-based `R32_FLOAT` depth copy. Set either copy option to `false` to pass the corresponding original full texture with its moving ROI subrect base. `OmitBiasCurrentColorHint=true` sets the optional bias-current-color mask to null only during private ROI Evaluate; the native game parameter is restored immediately afterwards. `MinimalPrivateParameters=true` is the more conservative isolation test: private Create/Evaluate receive a separate NVIDIA-allocated parameter map containing only the documented DLSS SR minimum, with optional, preset, Streamline/vendor, matrix, and unknown keys omitted. `ResetOnMove=true` forces `Reset=1` only on frames where the input or output ROI origin changes; this intentionally disables temporal reuse during movement and resumes history when the ROI stops. Changing any diagnostic requests one reset; changing parameter mode also recreates the private feature.

For `ExternalUdp` and `ExternalSharedMemory`, `StaleMs` controls whether a new sample may replace the current gaze point. Stale, invalid, or unavailable input freezes the last accepted ROI; it does not reset private DLSS history. This allows tracker cadence to be lower or less regular than render cadence without degrading the ROI into repeated current-frame reconstruction.

## Implementation Map

- Config:
  - `OptiScaler/Config.h`
  - `OptiScaler/Config.cpp`
- Overlay UI:
  - `OptiScaler/menu/menu_common.h`
  - `OptiScaler/menu/menu_common.cpp`
- DLSS D3D12 ROI dispatch:
  - `OptiScaler/upscalers/dlss/DLSSFeature_Dx12.h`
  - `OptiScaler/upscalers/dlss/DLSSFeature_Dx12.cpp`
- ROI composite and motion-vector patch shaders:
  - `OptiScaler/shaders/gaze_roi/GazeRoi_Dx12.h`
  - `OptiScaler/shaders/gaze_roi/GazeRoi_Dx12.cpp`
- External gaze bridge:
  - `tools/gaze_roi_shared_bridge/Program.cs`
  - `tools/gaze_roi_bridge/bridge.mjs`
- DLSS input-contract compatibility plan:
  - `docs/GazeRoiDlssContractPlan.md`

## Current Pipeline

1. Read color, depth, motion vectors and output from the intercepted DLSS call.
2. Build an output-space ROI from the virtual gaze point and `WidthPx` / `HeightPx`.
3. Map the ROI to render-space input coordinates.
4. Query DLSS optimal settings for the requested output ROI and create or reuse a private ROI DLSS feature.
5. When `ColorCopy=true`, copy Color without resampling into an input-ROI-sized, identically formatted zero-based texture; otherwise pass the original Color resource with its moving ROI subrect base. When `DepthCopy=true`, crop depth into an input-ROI-sized zero-based `R32_FLOAT` texture; otherwise pass the original depth resource with its moving ROI subrect base. Crop and patch motion vectors into a separate private zero-based ROI texture. Low-resolution MVs use render-space origins; high-resolution MVs use output-space origins. The optional bias-current-color mask normally uses its moving subrect base, or is temporarily null when `OmitBiasCurrentColorHint=true`.
6. Create or reuse a private output resource whose dimensions exactly match the output ROI. Private Create and Evaluate both receive this resource with output subrect base `(0,0)`; the resource does not move within a full-display canvas.
7. When `OutputClearDebug` is enabled, clear the entire private output to magenta and insert a UAV barrier. Magenta remaining after Evaluate proves that NGX did not overwrite those local pixels; old scene content was written by NGX itself.
8. Normally invoke the private ROI feature with the original NGX parameter map under scoped temporary overrides; unknown keys are forwarded and ROI values are restored after the synchronous call. With `MinimalPrivateParameters=true`, invoke it with the separate NVIDIA-native whitelist map instead. With `CurrentColorPointBypass=true`, skip Evaluate and point-scale the effective current Color ROI into the same local output resource. All three modes feed the unchanged final composite.
9. If peripheral reconstruction is enabled, run its separate resolve pass. Modes 0/1 use render-resolution surfaces; mode 2 uses a fixed 1.25x intermediate grid clamped to output size. Mode 0 writes one current-frame surface. Temporal modes use two Color and two Depth ping-pong histories, read the original full-frame MV/depth, sample history at `currentPixel + MV`, and reject reset/out-of-bounds/depth-discontinuous samples. Mode 2 has its own FSR2-core reconstruction/accumulation branch and stores accumulated sample weight in Color alpha; it does not dispatch mode 1 at a larger size. The final full-output composite samples the zero-based resolved surface. Resource resize and mode changes retire old surfaces only after their fence generation completes.
10. Run the production composite and write only the game's original active output subrect. In the no-effect path it directly samples the original low-resolution Color subrect and the zero-based private output in one full-output dispatch. A destination pixel in the global ROI reads `privateOutput[p - outputRect.xy]`; this is the only stage that maps the zero-based private output back to its global position. Border and `MotionVectorDebugView` run afterwards in a separate debug-only overlay pass; the MV view binds the exact final patched resource consumed by private DLSS and does not crop or inject a second copy.

For Ray Reconstruction, localization begins in the `slEvaluateFeature(kFeatureDLSS_RR)` hook. The original static or frame-based tags are cached per viewport; for the current evaluation every tag with a valid texture extent receives the same normalized output ROI mapped into its own active coordinate domain. The localized tag set and any inline tag inputs are used only for that RR call, then the original tags are restored. Native texture pointers, resource descriptions, lifecycle values, chained extensions, constants, options, and other inputs remain unchanged. The native replacement keeps Color and Depth as original resources, copies/patches only MV into a zero-based ROI resource, and writes a fixed zero-based private RR output/history. The periphery maintains a fixed half-linear-resolution `Color + Depth + MV` temporal/moments/depth history and 0-3 a-trous passes; its filtered texture is fed directly to the final composite. The code is compiled but not yet game-validated.

When Gaze ROI is enabled, this is the only SR image path. Setup, validation, private DLSS evaluation, or composite failures are logged and returned to the caller; the implementation does not silently evaluate the original full-frame DLSS feature.

## Known Issues

- Feather blends inward from ROI edges that are inside the screen. When an ROI edge is clamped to the screen edge, that side's transition band is disabled.
- Cyberpunk 2077 testing confirms two separate repairs: the ROI-local private output eliminates the former moving-edge old/black strip, and fence-managed lifetime for frame-slot descriptors/constants eliminates the velocity- and depth-visible dragged-image defect under uncapped irregular pacing. The latter was a CPU-written D3D12 object lifetime race, not evidence that depth changes the ROI-origin MV identity. See `docs/issues/motion-vector-roi.md`.
- The depth-only localization, `BiasCurrentColorHint` omission, and `MinimalPrivateParameters` experiments are compiled, deployed, and real-game tested; none produces an observable change. Logs prove that minimal mode rebuilds the handle with a NVIDIA-native whitelist map, omits the intended optional/unknown keys, and evaluates successfully. The inherited parameter map is therefore ruled down; core resources, Create flags, the loaded DLSS implementation, and possible module-global Streamline state remain distinct variables.
- Peripheral jitter sign `+1` stabilized Death Stranding and the formerly successful Cyberpunk cases, but worsened shake in cases that still exposed the old frame-lifetime race. The option never changes private DLSS jitter. Mode 0 applies it only to current peripheral Color; mode 1 uses it for one aligned Color/Depth/MV sample center and stable-history MV phase conversion. Treat the sign as a fixed-camera residual-motion diagnostic rather than assuming the historical result still applies.
- Ordinary DLSS peripheral temporal stabilization now defaults to MV/depth reprojection. The new strength mapping and default-on path are compiled but require renewed game testing for MV direction, jitter convention, depth compatibility, and trail rejection.
- Mouse mode depends on foreground-window cursor coordinates and may be affected by games that lock or hide the cursor. The formerly reported persistent blur after Cyberpunk forcibly recentered the cursor no longer reproduces as of 2026-07-25.
- The ROI motion-vector offset is deterministic but still experimental.

## Roadmap

The input-contract and complex-game compatibility work takes priority over the visual-quality items below. See [GazeRoiDlssContractPlan.md](GazeRoiDlssContractPlan.md).

1. Better boundary blending:
   - Split ROI into foveal core, guard band and transition band.
   - Keep the core 100% DLSS.
   - Feather only inside the guard/transition band.
   - Clip the transition band naturally at screen edges.

2. Peripheral blur and temporal stability:
   - Tune the low-resolution temporal stabilizer against static shimmer and camera-motion trails.
   - Consider motion-vector-assisted peripheral history only if the lightweight clipped EMA is insufficient.
   - Keep the peripheral path cheaper than full-frame DLSS.

3. Eye-tracking integration:
   - Add an external gaze input API.
   - Accept normalized gaze coordinates and timestamps.
   - Add latency compensation for smooth pursuit.
   - Add saccade handling and temporary ROI expansion.

4. Size and blend controls:
   - Add explicit core/guard/transition pixel controls.
   - Rebuild ROI DLSS features only when dimensions actually change.
   - Optionally expose a square-lock toggle for eye-tracking workflows.
