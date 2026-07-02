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
- **Debug Border**: draws a red rectangle around the composited ROI.
- **Control**:
  - `Mouse`: use the foreground window cursor position as the gaze point.
  - `Keyboard`: use `F5/F6/F7/F8` to move and `F9` to center.

Changing **ROI Width** or **ROI Height** changes the ROI DLSS feature size. The implementation will recreate the ROI DLSS feature when the ROI dimensions change. The requested size is expressed in final output pixels; the actual DLSS subrect can be adjusted by a few pixels because the render-space input rect is aligned for DLSS and then mapped back to output-space.

## INI Settings

```ini
[GazeRoi]
Enabled=false
WidthPx=1280
HeightPx=720
FeatherPx=96
DebugBorder=false
Control=Keyboard
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
7. Composite the ROI over a full-frame bilinear peripheral upscale.

## Known Issues

- Feather currently blends inward from the ROI edge. This can make pixels near the ROI edge softer than the ROI core.
- Peripheral rendering is currently simple bilinear upscale and can shimmer.
- Mouse mode depends on foreground-window cursor coordinates and may be affected by games that lock or hide the cursor.
- The ROI motion-vector offset is deterministic but still experimental.

## Roadmap

1. Better boundary blending:
   - Split ROI into foveal core, guard band and transition band.
   - Keep the core 100% DLSS.
   - Feather only inside the guard/transition band.
   - Clip the transition band naturally at screen edges.

2. Peripheral blur and temporal stability:
   - Replace plain bilinear peripheral upscale with a low-cost stable blur/upscale.
   - Reduce shimmer without adding objectionable motion smearing.
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
