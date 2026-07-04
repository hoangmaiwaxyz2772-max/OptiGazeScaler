import dgram from "node:dgram";
import { WebSocketServer } from "ws";

const wsHost = process.env.GAZE_WS_HOST ?? "127.0.0.1";
const wsPort = Number.parseInt(process.env.GAZE_WS_PORT ?? "38478", 10);
const udpHost = process.env.GAZE_UDP_HOST ?? "127.0.0.1";
const udpPort = Number.parseInt(process.env.GAZE_UDP_PORT ?? "38479", 10);

const udp = dgram.createSocket("udp4");
const wss = new WebSocketServer({ host: wsHost, port: wsPort });

let packetsIn = 0;
let packetsOut = 0;
let lastGaze = null;

function asFiniteNumber(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function normalizeGazeMessage(raw) {
  let message;
  try {
    message = typeof raw === "string" ? JSON.parse(raw) : JSON.parse(raw.toString("utf8"));
  } catch {
    return null;
  }

  if (message.type && message.type !== "gaze")
    return null;

  const x = asFiniteNumber(message.x);
  const y = asFiniteNumber(message.y);
  if (x === null || y === null)
    return null;

  const width = asFiniteNumber(message.width);
  const height = asFiniteNumber(message.height);
  const normalized = {
    type: "gaze",
    t: asFiniteNumber(message.t) ?? performance.now(),
    x,
    y,
    valid: message.valid !== false
  };

  if (width !== null && height !== null && width > 0 && height > 0) {
    normalized.width = width;
    normalized.height = height;
  }

  const confidence = asFiniteNumber(message.confidence);
  if (confidence !== null)
    normalized.confidence = confidence;

  if (typeof message.predicted === "boolean")
    normalized.predicted = message.predicted;

  return normalized;
}

function forwardGaze(gaze) {
  const payload = Buffer.from(JSON.stringify(gaze));
  udp.send(payload, udpPort, udpHost, (error) => {
    if (error)
      console.warn(`[gaze-roi-bridge] UDP send failed: ${error.message}`);
  });
  packetsOut += 1;
  lastGaze = gaze;
}

wss.on("connection", (socket, request) => {
  const remote = `${request.socket.remoteAddress}:${request.socket.remotePort}`;
  console.log(`[gaze-roi-bridge] WebSocket client connected: ${remote}`);

  socket.on("message", (raw) => {
    packetsIn += 1;
    const gaze = normalizeGazeMessage(raw);
    if (gaze)
      forwardGaze(gaze);
  });

  socket.on("close", () => {
    console.log(`[gaze-roi-bridge] WebSocket client disconnected: ${remote}`);
  });
});

setInterval(() => {
  const gazeText = lastGaze
    ? ` last=(${lastGaze.x.toFixed(1)}, ${lastGaze.y.toFixed(1)}) valid=${lastGaze.valid}`
    : "";
  console.log(`[gaze-roi-bridge] ws:${wsHost}:${wsPort} -> udp:${udpHost}:${udpPort} in=${packetsIn} out=${packetsOut}${gazeText}`);
}, 5000).unref();

process.on("SIGINT", () => {
  console.log("\n[gaze-roi-bridge] shutting down");
  wss.close();
  udp.close();
  process.exit(0);
});

console.log(`[gaze-roi-bridge] listening ws://${wsHost}:${wsPort}`);
console.log(`[gaze-roi-bridge] forwarding to udp://${udpHost}:${udpPort}`);
