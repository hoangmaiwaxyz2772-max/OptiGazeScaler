# DLSS Gaze ROI MVP

This branch contains an experimental D3D12 DLSS Super Resolution ROI path for gaze-driven local upscaling. It is not a general OptiScaler feature yet.

## Current Scope

- API: D3D12 only.
- Upscaler: DLSS Super Resolution only.
- Input: virtual gaze point controlled by mouse or keyboard.
- ROI: one rectangular DLSS region composited over a low-cost full-frame upscale.
- Frame generation, ray reconstruction, Vulkan and DX11 are out of scope for this MVP.

## Runtime Controls

Open the OptiScaler overlay and use **Gaze ROI DLSS**.

- **Enable Gaze ROI**: turns the experimental path on or off.
- **ROI Width / ROI Height**: target ROI size in output/upscaled pixels. Defaults are `1280 x 720`.
- **Feather**: current inward blend width in output pixels. Default is `96`.
- **Peripheral Blur**: applies a single-frame 9-tap tent blur to the low-cost peripheral upscale.
- **Blur Radius**: blur radius in low-resolution source texels. Default is `1.0`.
- **Peripheral Jitter Cancel**: offsets low-resolution peripheral sampling by the DLSS jitter offset before blur.
- **Jitter Sign**: experimental sign for jitter cancellation. Test `+1` and `-1`; keep the sign that reduces static-scene shimmer.
- **Peripheral Temporal Stabilizer**: enables a low-resolution temporal history pass for the peripheral image.
- **Temporal Current Weight**: current-frame blend weight. Lower values reduce shimmer more; higher values reduce trails.
- **Temporal Reactive Scale**: increases current-frame weight when the current frame differs from clipped history.
- **Debug Border**: draws a red rectangle around the composited ROI.
- **Control**:
  - `Mouse`: use the foreground window cursor position as the gaze point.
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
PeripheralBlur=true
PeripheralBlurRadius=1.0
PeripheralJitterCancel=true
PeripheralJitterSign=1
PeripheralTemporal=true
PeripheralTemporalCurrentWeight=0.2
PeripheralTemporalReactiveScale=4.0
DebugBorder=false
Control=Keyboard
UdpPort=38479
StaleMs=50
```

`WidthPx` and `HeightPx` are output/upscaled pixel counts. For eye-tracking experiments a square region such as `960 x 960` or `1280 x 1280` is usually the most useful starting point.

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

## Current Pipeline

1. Read color, depth, motion vectors and output from the intercepted DLSS call.
2. Build an output-space ROI from the virtual gaze point and `WidthPx` / `HeightPx`.
3. Map the ROI to render-space input coordinates.
4. Create or reuse a separate ROI DLSS feature sized for `input ROI -> output ROI`.
5. Patch motion vectors with a constant ROI-coordinate delta.
6. Evaluate DLSS once for the ROI.
7. Build the peripheral image at render resolution:
   - sample the low-resolution color with optional DLSS jitter cancellation;
   - apply optional 9-tap tent blur;
   - apply optional temporal stabilization using a ping-pong low-resolution history texture;
   - clip history to the current frame's local 3x3 color neighborhood before blending.
8. Composite the ROI over the peripheral image. The final composite samples the peripheral texture bilinearly into output space.

## Known Issues

- Feather blends inward from ROI edges that are inside the screen. When an ROI edge is clamped to the screen edge, that side's transition band is disabled.
- Peripheral temporal stabilization has no motion-vector reprojection. It is intentionally lightweight and relies on current-frame neighborhood clipping plus reactive blending to limit trails.
- Mouse mode depends on foreground-window cursor coordinates and may be affected by games that lock or hide the cursor.
- The ROI motion-vector offset is deterministic but still experimental.

## Roadmap

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
