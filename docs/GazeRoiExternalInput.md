# Gaze ROI External Input

This document describes the first external gaze input path for the experimental DLSS Gaze ROI MVP.

Last checked: 2026-07-25. The shared-memory bridge build and WebSocket-to-slot smoke test pass; the stale-input history-preservation repair is compiled and deployed pending real-game validation.

## Data Path

```text
eye-tracking webpage
  -> WebSocket ws://127.0.0.1:38478
Shared-memory bridge
  -> Windows shared memory Local\EyeTracingGazeV1
OptiScaler Gaze ROI
```

The browser still sends WebSocket messages, but the bridge writes only the latest gaze sample to a single shared-memory slot. OptiScaler reads that slot every frame with a seqlock-style `seq` field, avoiding UDP/socket buffering and stale queued samples.

## OptiScaler Settings

Open the OptiScaler overlay:

- Enable **Gaze ROI DLSS**.
- Set **Control** to `ExternalSharedMemory`.
- Leave **Stale** at `50 ms` for the first tests.

Equivalent INI:

```ini
[GazeRoi]
Enabled=true
Control=ExternalSharedMemory
StaleMs=50
WidthPx=960
HeightPx=960
PeripheralBlur=true
PeripheralBlurRadius=1.0
PeripheralJitterCancel=true
PeripheralJitterSign=1
DebugBorder=true
```

`WidthPx` and `HeightPx` are output/upscaled pixels. For eye tracking, start with a square ROI.

## Start The Shared-Memory Bridge

From the repository root:

```powershell
cd tools\gaze_roi_shared_bridge
dotnet run
```

Optional environment variables:

```powershell
$env:GAZE_WS_PORT=38478
$env:GAZE_SHM_NAME="Local\EyeTracingGazeV1"
dotnet run
```

Or run `tools\gaze_roi_shared_bridge\start-shared-bridge.bat`.

## Legacy UDP Bridge

The old Node UDP bridge remains available for comparison:

```powershell
cd tools\gaze_roi_bridge
npm install
npm start
```

Use `Control=ExternalUdp` and `UdpPort=38479` when testing the legacy path.

## WebSocket Message Format

Send one JSON message per gaze update:

```json
{
  "type": "gaze",
  "t": 123456789.123,
  "x": 960.4,
  "y": 540.2,
  "width": 1920,
  "height": 1080,
  "valid": true,
  "confidence": 0.92,
  "predicted": true
}
```

Required fields:

- `x`, `y`: gaze point.
- `valid`: optional, defaults to `true`.

Recommended fields:

- `width`, `height`: coordinate-space size for `x` and `y`. Usually the browser viewport, capture canvas, or game output size.
- `t`: sample timestamp from the eye tracker or prediction stage.
- `confidence`: tracking confidence.
- `predicted`: whether latency prediction has already been applied.

If `width` and `height` are present, OptiScaler normalizes `x / width` and `y / height`. If they are omitted, `x` and `y` must already be normalized in `[0, 1]`.

## Browser-Side Example

```js
const gazeSocket = new WebSocket("ws://127.0.0.1:38478");

function sendGaze(sample) {
  if (gazeSocket.readyState !== WebSocket.OPEN)
    return;

  gazeSocket.send(JSON.stringify({
    type: "gaze",
    t: sample.t ?? performance.now(),
    x: sample.x,
    y: sample.y,
    width: window.innerWidth,
    height: window.innerHeight,
    valid: sample.valid !== false,
    confidence: sample.confidence ?? 1,
    predicted: sample.predicted === true
  }));
}
```

## Runtime Behavior

- OptiScaler uses only the latest fresh packet.
- If no fresh valid packet arrives within `StaleMs`, the ROI freezes at the last accepted position and private DLSS continues accumulating history for that fixed ROI. A temporarily slow tracker or stopped bridge must not turn every rendered frame into a DLSS reset frame.
- `valid=false` packets are accepted by the bridge but ignored by OptiScaler as active gaze.
- Dropped or overwritten gaze updates are fine; the stream is treated as realtime state, not reliable history.
- Logs report `[GROI_INPUT] ... source is fresh` and transitions to unavailable/invalid/stale. The latter is a freeze notification, not a rendering failure.
- Keep **Current Color Point Bypass** disabled for normal use. If enabled, private DLSS is deliberately skipped and the ROI is point-scaled current Color; the overlay now displays a red warning while this diagnostic is active.
