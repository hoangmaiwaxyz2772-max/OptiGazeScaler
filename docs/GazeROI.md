# Gaze-driven rendering

OptiGazeScaler has separate controls for DLSS Super Resolution ROI, DLSS5 / NR
ROI and FSR frame-generation ROI. They share gaze input, but each controls a
different part of rendering. Enabling one does not automatically enable the
others.

## Gaze input

Open **Gaze ROI Control** and select mouse, keyboard, shared memory or legacy
UDP input. Shared memory is the preferred external eye-tracker bridge; see
[Gaze ROI External Input](GazeRoiExternalInput.md) for the protocol and setup.
Mouse control can be affected by games that lock or recenter the pointer.

## DLSS Super Resolution ROI

The **Gaze ROI DLSS** panel enables local D3D12 DLSS Super Resolution over a
less expensive peripheral reconstruction. Set width and height in output
pixels, then use **Apply ROI Size**. Edge feathering and peripheral
reconstruction settings control the transition and surrounding image.

This is an experimental rendering path. Resolution, motion vectors, HDR and
game integration affect quality and performance. The Ray Reconstruction ROI
panel currently reports that its experimental local path is unavailable;
native full-frame Ray Reconstruction remains available.

## DLSS5 / NR ROI

Open **DLSS5 / NR** for the independent neural-rendering region, resolution
scale and optional exterior edge correction. This runs after DLSS SR or RR,
and can use a different ROI size from DLSS SR. Full-frame NR is available by
leaving its gaze mode off. See the [DLSS5 guide](DLSS5.md).

## Frame generation

FSR frame-generation ROI has its own settings and peripheral reconstruction.
It is not the suspended cross-frame NR scheduler. Frame-generation combinations
remain game-dependent; successful use of one path does not establish every
combination as supported.
