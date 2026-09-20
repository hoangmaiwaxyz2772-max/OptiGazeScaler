# Credits and source attribution

OptiGazeScaler is based on [OptiScaler](https://github.com/optiscaler/OptiScaler).
Thank you to its authors and contributors for the upscaler, frame-generation,
HUDfix, configuration and graphics integration on which this fork depends.
The project retains the GNU GPL version 3 license in [LICENSE](LICENSE).
Existing third-party notices and licenses remain applicable to their components.

## DLSS NR exposure and white-point adaptation

Thank you to **Dagherbou** and the contributors to
[OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR), particularly
the `dlss-neural-rendering` branch, for their DLSS NR integration and
exposure/white-point work used as a reference for the adaptation in this fork.

The local reference checkout is pinned to
[`393e0706b950a0ff1498e9dcf66989a80de72f31`](https://github.com/Dagherbou/OptiScaler_DLSSNR/tree/393e0706b950a0ff1498e9dcf66989a80de72f31).
Its NR processing is in `OptiScaler/dlssnr/DlssNrFeature_Dx12.cpp`, with
conversion/composition in `OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl`.
The earlier white-point metering work is recorded in
[`64b02a6f`](https://github.com/Dagherbou/OptiScaler_DLSSNR/commit/64b02a6f).
These references identify the work consulted, not a claim of verbatim copying
or an exact line-by-line import revision.

The corresponding implementation here is in
[`DLSSNRFeature_Dx12.cpp`](OptiScaler/upscalers/dlssnr/DLSSNRFeature_Dx12.cpp),
at the NGX exposure-resource acquisition and `EffectivePaperWhite()` shader
helper. This fork samples the game's exposure texture on the GPU and derives
reference white from `PreExposure * ExposureScale / ExposureTexture`, with
finite-value checks, bounds and a manual-white fallback. The reference branch's
image-brightness meter is not part of this path. The HUDfix display-image route
does not use game exposure. ROI extrapolation is a separate algorithm.

OptiScaler_DLSSNR carries the GNU GPL version 3 license. The adapted project
remains under that license; attribution does not replace its terms. The
[packaged notice](Licenses/OptiScaler_DLSSNR_ATTRIBUTION.txt) and the full
project license accompany the Release build. When distributing a binary,
provide its matching source under those terms, including the production HLSL
and the script used to compile it.

## Other algorithm references

- The scene-linear SDR mapping uses Krzysztof Narkowicz's published
  [ACES filmic fit](https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/).
  This scalar curve is not the complete ACES output transform.
- The former optional ratio compositor was adapted from
  [RenoDX by clshortfuse](https://github.com/clshortfuse/renodx).
  That path has been removed; its attribution and full MIT license remain in
  [RenoDX_ATTRIBUTION.txt](Licenses/RenoDX_ATTRIBUTION.txt).
- NVIDIA's external NR runtime/model is not included in this source project or
  licensed by the project's GPL. Obtain a compatible module separately under
  its own terms.

The upstream acknowledgements in [README.md](README.md#thanks) are retained.
