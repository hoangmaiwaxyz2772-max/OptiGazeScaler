# Gaze ROI External Input

This document describes the first external gaze input path for the experimental DLSS Gaze ROI MVP.

## Data Path

```text
eye-tracking webpage
  -> WebSocket ws://127.0.0.1:38478
Node bridge
  -> UDP 127.0.0.1:38479
OptiScaler Gaze ROI
```

The browser never sends UDP directly. The Node bridge receives WebSocket messages and forwards compact JSON packets over localhost UDP. OptiScaler only listens on `127.0.0.1`, so the receiver is not exposed to the LAN.

## OptiScaler Settings

Open the OptiScaler overlay:

- Enable **Gaze ROI DLSS**.
- Set **Control** to `ExternalUdp`.
- Leave **UDP Port** at `38479`, unless the bridge is configured differently.
- Leave **Stale** at `50 ms` for the first tests.

Equivalent INI:

```ini
[GazeRoi]
Enabled=true
Control=ExternalUdp
UdpPort=38479
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

## Start The Node Bridge

From the repository root:

```powershell
cd tools\gaze_roi_bridge
npm install
npm start
```

Optional environment variables:

```powershell
$env:GAZE_WS_PORT=38478
$env:GAZE_UDP_PORT=38479
npm start
```

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
- If no fresh valid packet arrives within `StaleMs`, the ROI stays at the last position but motion-vector ROI history is reset.
- `valid=false` packets are accepted by the bridge but ignored by OptiScaler as active gaze.
- Lost UDP packets are fine; the stream is treated as realtime state, not reliable history.
