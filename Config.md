# Configuration
This document will try to explain the `OptiScaler.ini` and in-game menu (shortcut key for opening menu is **INSERT**) settings as much as possible. 

![in-game menu](images/menu043.png)

### Upscalers
OptiScaler supports DirectX 11, DirectX 12 and Vulkan APIs with multiple upscaler backends. You can select which upscaler to use in the `[Upscalers]` section of the `OptiScaler.ini` file.

```ini
[Upscalers]
; Select upscaler for Dx11 games
; fsr22 (native dx11), xess (with dx12), fsr21_12 (dx11 with dx12) or fsr22_12 (dx11 with dx12)
; Default (auto) is fsr22
Dx11Upscaler=auto

; Select upscaler for Dx12 games
; xess, fsr21 or fsr22
; Default (auto) is xess
Dx12Upscaler=auto

; Select upscaler for Vulkan games
; fsr21 or fsr22
; Default (auto) is fsr21
VulkanUpscaler=auto
```

* `fsr21` means FSR 2.1.2
* `fsr22` means FSR 2.2.1
* `xess` means XeSS

*For DirectX11 `fsr21_12`, `fsr22_12` and `xess` use a DirectX12 background device to be able to use DirectX12 only upscalers. There is a %10-15 performance penalty for this method, but it allows much more upscaler options. Also, the native DirectX11 implementation of FSR 2.2.1 is a backport from the Unity renderer and has it's own problems, some of which are avoided by OptiScaler.*

For selecting upscalers from in-game menus `Upscalers` section could be used.

![upscalers](images/Upscalers.png)

### Pseudo SuperSampling
With OptiScaler 0.4 there are new options for pseudo-supersampling under `[Upscalers]`

```ini
[Upscalers]
; Enable pseudo-supersampling option for Dx12 and Dx11 with Dx12 backends
; true or false - Default (auto) is false
SuperSamplingEnabled=auto

; Pseudo-supersampling ratio 
; 0.0 - 5.0 - Default (auto) is 2.5
SuperSamplingMultiplier=auto
```

To explain it clearly, for example, normally when your game is running at 1080p and  `Quality` is selected as DLSS preset, it would render a 720p image and send it to the upscaler with other necessary input information and generate a 1080p image as output.

If pseudo-supersampling is enabled, it uses `SuperSamplingMultiplier` to calculate the target render size of the upscaler. For 720p with default multiplier (2.5) it would be 1800p. So now the upscaler will upscale the image to 1800p instead of 1080p, then OmniSaler will downsample the output image to 1080p.

![pseudo superSampling](images/pss.png)

Because of the higher resolution of the upscaled target, there will be a performance loss compared to just upscaling. But subjectively it could produce images close to DLAA quality with higher performance levels.

It can be changed from the in-game menu with real-time results.

![pss config](images/pss_config.png)

### Dx11withDx12 Sync Settings
For DirectX11 with `fsr21_12`, `fsr22_12` and `xess` upscaler options, OptiScaler uses a DirectX12 background device to be able to use these DirectX12 only upscalers. This is a very niche feature and can cause issues with unstable GPU drivers (especially on Intel). To mitigate and prevent crashes or graphical issues, this option could be used.

```ini
[Dx11withDx12]
; Syncing methods for Dx11 with Dx12
;
; Valid values are;
;	0 - No syncing                                  (fastest, most prone to errors)
;	1 - Fence                                 
;	2 - Fences + Flush 
;	3 - Fences + Event
;	4 - Fences + Flush + Event
;	5 - Query Only

; Default (auto) is 1
TextureSyncMethod=auto

; Default (auto) is 5
CopyBackSyncMethod=auto

; Start output copy back sync after or before Dx12 execution
; true or false - Default (auto) is true
SyncAfterDx12=auto

; Delay some operations during creation of D11wDx12 features to increase compatibility
; true or false - Default (auto) is false
UseDelayedInit=auto
```
The diagram below shows the flow of Dx11 with Dx12 upscaling process. Yellow circles are sync points (or possible sync points). `SyncAfterDx12` selects when the second sync will happen.  

![dx11 with dx12 flow](images/Dx11wDx12.png)

`No syncing` : Self explanotory  
`Fence` : Sync using shared `Fence`s (Signal & Wait). These should happen on GPU which is pretty fast.  
`Fence + Event` : Sync using shared `Fence`s (Signal & Event). `Event`s are waited on CPU which is slower.  
`Flush` : After Signal shared `Fence`, `Flush`es Dx11 DeviceContext.  
`Query Only` : Uses Dx11 `Query` to sync, in general faster that `Event`s but slower than `Fence`s.  

When using `Event`s for syncing output `SyncAfterDx12=false` is usually more performant.


**These settings are game and hardware dependent. Default values are set for balanced performance and stable image, for high performance the user might need to tweak them per game.**

These can be changed from the in-game menu with real-time results (except `UseDelayedInit`).

![dx11 sync setings](images/dx11wdx12menu.png)

### XeSS Settings

```ini
[XeSS]
; Building pipeline for XeSS before init
; true or false - Default (auto) is true
BuildPipelines=auto 

;Select XeSS network model
; 0 = KPSS
; 1 = Splat
; 2 = Model 3
; 3 = Model 4
; 4 = Model 5
; 5 = Model 6
;
; Default (auto) is 0
NetworkModel=auto

[CAS]
; Enables CAS sharpening for XeSS
; true or false - Default (auto) is false
Enabled=auto

; Color space conversion for input and output
; Possible values are at the end of the file - Default (auto) is 0
ColorSpaceConversion=auto
```

The `BuildPipelines` parameter allows XeSS pipelines to be built during context creation to prevent stuttering later.

`NetworkModel` is for selecting the network model to be used with XeSS upscaling. **(Currently has no visible effect on the upscaled image)**

#### CAS
Normally XeSS tends to produce softer final image compared to other upscalers and has no sharpening option to mitigate it. So OptiScaler allows you to use AMD's CAS sharpening filter on the final image to balance upscaled images soft look. CAS is not perfect though, on some games it causes some artifacts/issues like dissapering bloom effects, shifting color tone of the image or causing black screen with no image at all.

![cas](images/cas.png)

1. Bloom removed
2. Color tone is changed
   
`ColorSpaceConversion` to fix color space conversion issues but **almost always** the default setting would work fine.

It can be changed from the in-game menu with real-time results.

![xess](images/xess.png)

`Dump` option is for debugging purposes, which would dump input and output parameters and textures for XeSS to game folder.

### FSR Settings

```ini
[FSR]
; 0.0 to 180.0 - Default (auto) is 60.0
VerticalFov=auto

; If vertical fov is not defined will be used to calculate vertical fov
; 0.0 to 180.0 - Default (auto) is off
HorizontalFov=auto
```

To improve the image quality you can try to match the vertical or horizontal FOV of your game with these settings. The default is 60° vertical FOV and most of the time it works fine.

It can be changed from the in-game menu with real-time results.

![fsr](images/fsr.png)

### Sharpness
DLSS used to have a sharpening option, but later it was removed. So some games have sharpness slider and some do not. With this option you can disable or enable the sharpness of the final image. FSR has built in sharpness but for XeSS CAS option must be enabled.

```ini
[Sharpness]
; Override DLSS sharpness paramater with fixed shapness value
; true or false - Default (auto) is false
OverrideSharpness=auto

; Strength of sharpening, 
; value range between 0.0 and 1.0 - Default (auto) is 0.3
Sharpness=auto
```

It can be changed from the in-game menu with real-time results.

![sharpness](images/sharpness.png)

### Upscaling Ratios
OptiScaler provides several options for overriding and locking upscaling ratios.

#### Upscale Ratio Override
`UpscaleRatioOverride` allows you to select a single upscale ratio for all quality presets.

```ini
[UpscaleRatio]
; Set this to true to enable the internal resolution override 
; true or false - Default (auto) is false
UpscaleRatioOverrideEnabled=auto

; Set this to true to enable limiting DRS max resolution to overriden ratio
; true or false - Default (auto) is false
DrsMaxOverrideEnabled=auto

; Set the forced upscale ratio value
; Default (auto) is 1.3
UpscaleRatioOverrideValue=auto
```

This can be changed and saved from the in-game menu, but usually the change will take effect after a restart or resolution change.

![us ratio](images/us_ratio.png)

#### Quality Ratio Override
`QualityRatioOverride` allows you to override the upscale ratio for each quality preset.

```ini
[QualityOverrides]
; Set this to true to enable custom quality mode overrides
; true or false - Default (auto) is false
QualityRatioOverrideEnabled=auto

; Set custom upscaling ratio for each quality mode
;
; Default (auto) values:
; Ultra Quality         : 1.3
; Quality               : 1.5
; Balanced              : 1.7
; Performance           : 2.0
; Ultra Performance     : 3.0
QualityRatioUltraQuality=auto
QualityRatioQuality=auto
QualityRatioBalanced=auto
QualityRatioPerformance=auto
QualityRatioUltraPerformance=auto
```

**
If both overrides are enabled, the `UpscaleRatioOverride` has priority over the `QualityRatioOverride`**

When `DrsMaxOverrideEnabled` is enabled, it limits the maximum internal rendering resolution to the default rendering resolution instead of the display resolution for DRS supported games. When enabled, it effectively disables DRS. Works with both `QualityRatioOverride` and `UpscaleRatioOverride`.

These can be changed and saved from the in-game menu, but usually the change will take effect after a restart or resolution change.

![quality ratio](images/q_ratio.png)

### Init Flags
These settings allow you to override the DLSS init flags to fix some issues.

```ini
[Depth]
; Force add INVERTED_DEPTH to init flags
; true or false - Default (auto) is DLSS value
DepthInverted=auto

[Color]
; Force add ENABLE_AUTOEXPOSURE to init flags
; Some Unreal Engine games needs this, fixes colors specially in dark areas
; true or false - Default (auto) is  DLSS value
AutoExposure=auto

; Force add HDR_INPUT_COLOR to init flags
; true or false - Default (auto) is  DLSS value
HDR=auto

[MotionVectors]
; Force add JITTERED_MV flag to init flags
; true or false - Default (auto) is  DLSS value
JitterCancellation=auto

; Force add HIGH_RES_MV flag to init flags
; true or false - Default (auto) is  DLSS value
DisplayResolution=auto

[Hotfix]
; Force remove RESPONSIVE_PIXEL_MASK from init flags
; true or false - Default (auto) is true
DisableReactiveMask=auto
```

Enabling `AutoExposure` helps correct problems with dark or washed-out colors.

![exposure](/images/exposure.png)

Enabling `HDR` has been reported to help with purple hue in some games.

Enabling `DisableReactiveMask` can help FSR backends in some games, but it usually causes more problems than it solves. That's why it is disabled by default.


Some games may set the motion vector size flag incorrectly, causing excessive motion blur when the camera moves. Enabling or disabling `DisplayResolution` might help in these situations.

![wrong mv flag](/images/mv_wrong.png)

These can be changed from the in-game menu with real-time results.

![init flags](images/init_flags.png)

### DLSS5 / DLSS NR (D3D12)

`[DLSSNR]` controls the external neural-rendering module on the D3D12 DLSS SR
and Ray Reconstruction paths. It supports full-frame processing and an
independent gaze ROI. Open **DLSS5 / NR** in the main overlay for common
settings. See the [DLSS5 guide](docs/DLSS5.md) for setup, algorithm behaviour
and limitations, and [CREDITS.md](CREDITS.md) for source attribution.

```ini
[DLSSNR]
Enabled=false
LibraryPath=auto
LateHudless=false
WhitePointSource=1
ExposureScale=1.0
HDRPaperWhite=auto
HudlessHDRPaperWhiteNits=203.0
LowResolutionScale=auto
FastReconstruction=true
GazeRoiEnabled=false
GazeRoiWidthPx=1280
GazeRoiHeightPx=720
GazeRoiScale=1
GazeRoiEdgeBlendPx=96
GazeRoiExtrapolation=false
GazeRoiExtrapolationDistancePx=512
OutputTemporalStabilization=false
PresentPreview=0
PreviewWhiteNits=203.0
```

| Setting | Meaning |
| --- | --- |
| `Enabled` | Enable external NR. Default false; unavailable modules/resources cause a logged skip. |
| `LibraryPath` | `auto` searches beside OptiScaler for `nvngx_dlssnr.dll`, then `nvngx.dll_dlssnr.dll`. The module is not bundled. An explicit path overrides discovery; restart after changing it. |
| `LateHudless` | True selects **After post-processing (HUDfix)**. False uses linear DLSS output and `WhitePointSource`. Default false. |
| `WhitePointSource` | 1 selects automatic game exposure; 0 selects manual reference white. Ignored for HUDfix display-image input. |
| `ExposureScale` | Automatic exposure trim, default 1.0; applied as `PreExposure * ExposureScale / ExposureTexture`. |
| `HDRPaperWhite` | Manual reference white, also the fallback for invalid/missing game exposure. `auto` uses 2.044 scene units. Increasing it darkens model input. |
| `HudlessHDRPaperWhiteNits` | Game paper white for HUDfix HDR-to-SDR model input and matching HDR restoration/reconstruction. Default 203 nits, range 80–1000. Match the game's paper white; higher values darken model input. Ignored for SDR captures and linear scene modes. Independent of `PreviewWhiteNits`. |
| `LowResolutionScale` | Full-frame model resolution: `auto`/0/1 uses full resolution; 2 and 3 reduce each dimension to approximately one half or one third. |
| `FastReconstruction` | Default true: fast 3×3 reconstruction of reduced-resolution model changes. False selects the 5×5 Debug fallback. |
| `GazeRoiEnabled` | Restrict NR to its own gaze rectangle. Independent of `[GazeRoi] Enabled`. |
| `GazeRoiWidthPx`, `GazeRoiHeightPx` | NR output-space dimensions, 64–8192 per axis and clipped to output. Defaults 1280×720. Old INI files missing these keys inherit the DLSS ROI size once for compatibility; explicit `auto` uses the NR defaults and subsequent saved settings are independent. |
| `GazeRoiScale` | Model resolution within the NR ROI: 1, 2 or 3. Independent of full-frame `LowResolutionScale`. |
| `GazeRoiEdgeBlendPx` | Inward model/original blend width in output pixels, default 96; bounded by ROI dimensions. |
| `GazeRoiExtrapolation` | Enable colour-supported local ROI edge correction. Default false; requires nonzero edge blend. |
| `GazeRoiExtrapolationDistancePx` | Maximum exterior reach, 0–512 px, default 512. Zero removes exterior correction. Decay has a fixed 192 px half-weight distance and a smooth outer fade. |
| `OutputTemporalStabilization` | Optional stabilization of model output, default false. Separate from edge correction's own bounded coefficient history. |
| `PresentPreview` | 0 = normal output, 3 = captured HUDfix scene, 1 = SDR model input, 2 = SDR model output. The menu orders these by processing stage. Mode 3 requires HUDfix. |
| `PreviewWhiteNits` | SDR model-preview brightness on HDR displays, default 203 nits. Does not alter model exposure or normal output. |

The shared gaze source is selected under **Gaze ROI Control** or `[GazeRoi]`.
DLSS SR and NR dimensions are edited and applied separately. See the
[gaze guide](docs/GazeROI.md) and [external input protocol](docs/GazeRoiExternalInput.md).

For HUDfix injection, the capture index, resource selector and advanced
exclusions are shared with OptiFG. Frame generation can remain off. No usable
capture means NR is skipped for that frame. Only this injection mode exposes
the capture controls. If NR was disabled at startup, save the enabled setting
and restart to initialize descriptor tracking for existing resources.

The linear routes encode SDR model input and restore the model's change over
the original scene. HUDfix uses display-image conversion and does not apply
game exposure. Direct output previews run before Present, bypass game
post-processing/UI and retain the OptiScaler overlay.

Edge correction uses colour/hue/brightness support, spatially smoothed local
fits and bounded history with a 0.55 maximum coefficient weight. History is
suppressed at the ROI boundary and during large gaze movement. Its strength
ramps with exterior distance, so the immediate boundary remains responsive.
The removed `GazeRoiTone` and `GazeRoiToneDistancePx` keys are ignored and are
deleted when saving. There is no independent surrounding-tone stage.

Advanced/debug keys remain for diagnosis: `FullResolutionGuidance`,
`LowResolutionFullOutput`, `TemporalResidualReconstruction`,
`HighResolutionGuidedResidual`, `LegacyResidualReconstruction`,
`LowResolutionOriginalMVec`, `LowResolutionMVecScale`, `CloneTypelessDepth`,
`ZeroMotionInput`, `ZeroDepthInput`, `DisableGazeRoiMotionInjection`,
`DebugGlobalDownsampleOutput` and `LegacyHDRTransfer` default false.
`DebugInputView` defaults false and `DebugInputViewComposite` defaults true.
The legacy `DebugModelOutput` preview migrates to `PresentPreview=2` when no
preview is selected. These switches are not required for ordinary setup.
External-module style/intensity controls are passed through when explicitly
configured; availability depends on that module. `UICorrection` stays false.

The cross-frame NR experiment is suspended: `PipelineSplit`, `PipelineAsync`
and `PipelineDebug` are forced off on load, including in old INI files. This
does not change FSR FG's separate `AllowAsync` option.

### Resource Barriers (Dx12 Only)
Some games (especially Unreal Engine) send input resources to DLSS in wrong states, which leads to graphical problems (especially on AMD hardware). Normally OptiScaler tries to detect the engine type and mitigate these problems, but sometimes games do not report this information correctly. To fix problems, these ini parameters would help.

![early christmas](images/christmas.png)

**Setting a wrong resource state here can cause a crash!**

```ini
[Hotfix]
; Color texture resource state to fix for rainbow colors on AMD cards (for mostly UE games) 
; For UE engine games on AMD, set it to D3D12_RESOURCE_STATE_RENDER_TARGET (4)
; Default (auto) is state correction disabled
ColorResourceBarrier=auto

; MotionVector texture resource state, from this to D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE (for mostly debugging) 
; Default (auto) is state correction disabled
MotionVectorResourceBarrier=auto 

; Depth texture resource state, from this D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE (for mostly debugging) 
; Default (auto) is state correction disabled
DepthResourceBarrier=auto

; Color mask texture resource state, from this D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE (for mostly debugging) 
; Default (auto) is state correction disabled
ColorMaskResourceBarrier=auto

; Exposure texture resource state, from this D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE (for mostly debugging) 
; Default (auto) is state correction disabled
ExposureResourceBarrier=auto

; Output texture resource state, from this D3D12_RESOURCE_STATE_UNORDERED_ACCESS (for mostly debugging) 
; Default (auto) is state correction disabled
OutputResourceBarrier=auto
```

These can be changed from the in-game menu with real-time results.

![resource barriers](images/rb.png)

### Mipmap LOD Bias Override (Dx12 Only)
To achieve better texture clarity, `MipmapLodBias` can be overridden with this setting. -15 is the sharpest and +15 is the fuzziest.

```ini
[Hotfix]
; Override mipmap lod bias for textures
; -15.0 - 15.0 - Default (auto) is disabled
MipmapBiasOverride=auto
```

**Adjusting MipmapLODBias has an impact on performace!**

It can be changed from the in-game menu, needs resolution change to be effective.

![mipmap lod bias](images/mipmap.png)

### Restore Root Certificates (Dx12 Only)
This hotfix is based on the original CyberFSR2's restoring ComputeRootSignature logic, I also added the option to restore ComputeRootSignature. I haven't noticed any games that need these options.

```ini
[Hotfix]
; Restore last used compute signature after upscaling
; true or false - Default (auto) is false
RestoreComputeSignature=auto

; Restore last used graphics signature after upscaling
; true or false - Default (auto) is false
RestoreGraphicSignature=auto
```

These can be changed from the in-game menu with real-time results.

![root certificate](images/cs.png)

### Logging
```ini
[Log]
; Logging
; true or false- Default (auto) is true
LoggingEnabled=auto

; Log file, if undefined log_xess_xxxx.log file in current folder
;LogFile=./CyberXess.log

; Verbosity level of file logs
; 0 = Trace / 1 = Debug / 2 = Info / 3 = Warning / 4 = Error
; Default (auto) is 2 = Info
LogLevel=auto

; Log to console (Log level is always 2 (Info) for performance reasons) 
; true or false - Default (auto) is false
LogToConsole=auto

; Log to file 
; true or false - Default (auto) is false
LogToFile=auto

; Log to NVNGX API
; true or false - Default (auto) is false
LogToNGX=auto

; Open console window for logs
; true or false - Default (auto) is false
OpenConsole=auto
```

These can be changed from the in-game menu with real-time results.

![logging](images/logging.png)

### Menu
```ini
[Menu]
; In-game ImGui menu scale
; 1.0 to 2.0 - Default (auto) is 1.0
Scale=auto
```

These can be changed from the in-game menu with real-time results.

![menu scale](images/ui_scale.png)
