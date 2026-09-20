# DLSS5 / DLSS Neural Rendering

OptiGazeScaler supports full-frame DLSS5 / DLSS NR and an independent
gaze-region mode on its D3D12 DLSS Super Resolution and Ray Reconstruction
paths. This is an experimental integration with an external NR module.
NR, NR gaze mode and exterior edge correction are disabled by default.

## Getting started

1. Install OptiGazeScaler using the normal OptiScaler installation procedure.
2. Place a compatible external NR module beside the injected OptiScaler DLL as
   `nvngx_dlssnr.dll` or `nvngx.dll_dlssnr.dll`. The first name takes priority.
   The module is not bundled. An explicit **External DLL Path** override is
   available under **DLSS5 / NR > Debug**; restart after changing it.
3. Enable a supported D3D12 DLSS SR or RR path and open the overlay with Insert.
4. Open **DLSS5 / NR**, enable **Enable DLSS NR**, and select an injection mode.
5. Leave gaze mode off for full-frame NR, or enable its independent gaze region
   and select a source in **Gaze ROI Control**.

If the module or required resources are unavailable, NR is skipped. Consult
`OptiScaler.log` for the reason. A successful build or NGX return code does not
establish visual correctness in every game.

## Injection and exposure

| Injection mode | Input and use |
| --- | --- |
| After post-processing (HUDfix) | Uses the selected scene after game post-processing and before UI. Capture controls are shown for this route. No usable capture means NR is skipped that frame. |
| Linear scene — automatic exposure | Uses the completed DLSS image with the game's exposure texture and pre-exposure. **Exposure trim** defaults to 1. Missing or invalid exposure falls back to the configured manual white point. |
| Linear scene — manual white point | Uses the completed DLSS image with a fixed reference white. Increasing white point darkens the SDR model input. The default fallback is 2.044 in scene units. |

The linear routes prepare SDR model input and restore model edits over the
original image, preserving original HDR highlight energy when the SDR proxy
cannot represent it. The HUDfix route uses display-image conversion instead:
SDR avoids the scene filmic curve, while HDR uses a fixed 203-nit SDR reference
with bounded highlight handling. HUDfix ignores game exposure.

HUDfix capture uses OptiScaler's existing resource selection. **Capture index**,
the **Resources** window and advanced capture exclusions are shared with
OptiFG; frame generation itself can stay off. If NR was disabled at startup,
enable it, save settings and restart to collect existing descriptor metadata.

Exposure/white-point source acknowledgements and the differences from the
reference implementation are in [CREDITS.md](../CREDITS.md).

## Independent gaze region

DLSS5 ROI width and height are in output pixels, independently configurable
from the DLSS SR ROI. Apply the edited dimensions together using the ROI
resolution apply button. Supported dimensions are 64–8192 per axis, clipped
to the available output. Default dimensions are 1280 × 720.

`GazeRoiScale=1` uses the ROI's full resolution; 2 and 3 reduce each model
dimension to approximately one half or one third. Reduced output is
reconstructed over the original image. The default fast reconstruction uses a
3×3 fit; the Debug fallback uses the more expensive 5×5 fit.

NR gaze mode can follow full-frame DLSS or the existing DLSS SR ROI. It shares
the gaze coordinates from **Gaze ROI Control**, not the SR ROI's dimensions or
enable switch. See [external gaze input](GazeRoiExternalInput.md).

## Edge correction

**DLSS NR ROI edge correction** extends a locally estimated model colour change
outside the ROI. It is optional and requires a nonzero ROI edge blend width.
**DLSS NR local edge reach (px)** controls the maximum reach from 0 to 512 px
(default 512); zero removes the exterior correction. The inward model blend
is separate and defaults to 96 px.

The algorithm uses a screen-anchored 32 px grid, sparse paired observations,
two separable spatial smoothing passes and a local gain/offset fit in a
perceptual colour representation. Colour distribution, hue and brightness
support attenuate transfer onto dissimilar pixels. These are colour guards,
not object recognition; similarly coloured objects can still differ in how
the model treats them.

Exterior decay has a fixed 192 px half-weight distance, independent of the
configured reach, followed by a smooth fade at the outer limit. Increasing
reach therefore does not stretch a correction stripe at unchanged strength.
There is no independent surrounding-tone or global colour fit.

Coefficient history has a maximum weight of 0.55. Historical influence is zero
on and inside the ROI rectangle and ramps up over 96 exterior pixels. Colour
changes and gaze displacement reduce history acceptance; gaze jumps of at
least 128 px discard it. History corrections remain bounded. This can reduce
outer changes while keeping the immediate boundary responsive, at the cost of
slower adaptation farther outside the ROI.

The edge path uses five dispatches: seed, horizontal smoothing, vertical
smoothing, fit and apply. Compatible typed-UAV formats use in-place output;
other formats use bounded staging. These implementation choices do not promise
a fixed GPU cost, seamless boundaries or a frame-rate improvement in every game.

## Direct output previews

The **Direct output** menu follows the processing order:

1. **Captured game frame** — the selected HUDfix scene before NR; unavailable
   for the linear injection modes.
2. **Model input (SDR)** — the prepared image passed to the model.
3. **Model output (SDR)** — the raw model result before restoration and
   reconstruction.

Choose **Off** for normal output. Previews replace the final screen image before
Present and bypass game post-processing/UI; the OptiScaler overlay remains
available. Model views preserve aspect ratio and use bilinear resizing. On HDR
displays, **Preview SDR white** changes preview brightness only.

## Scope and compatibility

The external module, GPU, driver and game resources determine availability.
DX11/Vulkan NR and universal frame-generation compatibility are not claimed.
ROI edge correction estimates appearance changes; it cannot reproduce unseen
model output or infer object semantics outside the ROI.

Cross-frame asynchronous NR scheduling is suspended. `PipelineSplit`,
`PipelineAsync` and `PipelineDebug` are forced off even in old configurations.
This is independent of FSR FG's own asynchronous setting.

The current edge implementation is `edge-only-v11`. Release preparation keeps
that algorithm and its accepted settings; it does not introduce another visual
algorithm revision. See [Config.md](../Config.md#dlss5--dlss-nr-d3d12) for keys.
