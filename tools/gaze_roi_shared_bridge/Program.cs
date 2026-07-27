using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using System.IO.MemoryMappedFiles;
using System.Net;
using System.Net.WebSockets;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

const int SlotSize = 64;

var wsHost = Environment.GetEnvironmentVariable("GAZE_WS_HOST") ?? "127.0.0.1";
var wsPort = ReadIntEnv("GAZE_WS_PORT", 38478);
var mapName = Environment.GetEnvironmentVariable("GAZE_SHM_NAME") ?? "Local\\EyeTracingGazeV1";

using var map = MemoryMappedFile.CreateOrOpen(mapName, SlotSize, MemoryMappedFileAccess.ReadWrite);
using var view = map.CreateViewAccessor(0, SlotSize, MemoryMappedFileAccess.ReadWrite);
unsafe
{
    view.SafeMemoryMappedViewHandle.AcquirePointer(ref SharedSlot.Pointer);
}
try
{
    SharedSlot.Initialize();
    using var listener = new HttpListener();
    listener.Prefixes.Add($"http://{wsHost}:{wsPort}/");
    listener.Start();

    Console.WriteLine($"[gaze-roi-shared-bridge] listening ws://{wsHost}:{wsPort}");
    Console.WriteLine($"[gaze-roi-shared-bridge] writing shared memory {mapName}");

    using var cts = new CancellationTokenSource();
    Console.CancelKeyPress += (_, eventArgs) =>
    {
        eventArgs.Cancel = true;
        cts.Cancel();
        listener.Stop();
    };

    var stats = new BridgeStats();
    _ = Task.Run(() => PrintStats(stats, cts.Token), cts.Token);

    while (!cts.IsCancellationRequested)
    {
        HttpListenerContext context;
        try
        {
            context = await listener.GetContextAsync();
        }
        catch when (cts.IsCancellationRequested)
        {
            break;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[gaze-roi-shared-bridge] accept failed: {ex.Message}");
            continue;
        }

        if (!context.Request.IsWebSocketRequest)
        {
            context.Response.StatusCode = 400;
            context.Response.Close();
            continue;
        }

        _ = Task.Run(() => HandleClient(context, stats, cts.Token), cts.Token);
    }
}
finally
{
    unsafe
    {
        if (SharedSlot.Pointer != null)
            view.SafeMemoryMappedViewHandle.ReleasePointer();
    }
}

static int ReadIntEnv(string name, int fallback)
{
    var text = Environment.GetEnvironmentVariable(name);
    return int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value) ? value : fallback;
}

static async Task HandleClient(HttpListenerContext context, BridgeStats stats, CancellationToken token)
{
    WebSocket? socket = null;
    try
    {
        var wsContext = await context.AcceptWebSocketAsync(subProtocol: null);
        socket = wsContext.WebSocket;
        Console.WriteLine($"[gaze-roi-shared-bridge] WebSocket client connected: {context.Request.RemoteEndPoint}");

        var buffer = new byte[4096];
        while (!token.IsCancellationRequested && socket.State == WebSocketState.Open)
        {
            using var message = new MemoryStream();
            WebSocketReceiveResult result;
            do
            {
                result = await socket.ReceiveAsync(buffer, token);
                if (result.MessageType == WebSocketMessageType.Close)
                    return;
                message.Write(buffer, 0, result.Count);
            } while (!result.EndOfMessage);

            stats.In++;
            if (TryNormalizeGaze(message.ToArray(), out var gaze))
            {
                SharedSlot.Write(gaze);
                stats.Out++;
                stats.LastX = gaze.X;
                stats.LastY = gaze.Y;
                stats.LastValid = gaze.Valid;
                stats.LastWriteTick = Environment.TickCount64;
            }
        }
    }
    catch (OperationCanceledException)
    {
    }
    catch (Exception ex)
    {
        Console.WriteLine($"[gaze-roi-shared-bridge] client error: {ex.Message}");
    }
    finally
    {
        if (socket != null)
            socket.Dispose();
        Console.WriteLine("[gaze-roi-shared-bridge] WebSocket client disconnected");
    }
}

static bool TryNormalizeGaze(byte[] utf8, out GazeSample gaze)
{
    gaze = default;
    try
    {
        using var doc = JsonDocument.Parse(utf8);
        var root = doc.RootElement;
        if (root.TryGetProperty("type", out var type) && type.ValueKind == JsonValueKind.String &&
            type.GetString() != "gaze")
            return false;

        if (!TryGetFinite(root, "x", out var x) || !TryGetFinite(root, "y", out var y))
            return false;

        TryGetFinite(root, "width", out var width);
        TryGetFinite(root, "height", out var height);
        TryGetFinite(root, "confidence", out var confidence);
        TryGetFinite(root, "t", out var sourceTimestamp);

        var valid = !root.TryGetProperty("valid", out var validElement) ||
                    validElement.ValueKind != JsonValueKind.False;

        gaze = new GazeSample(
            X: (float)x,
            Y: (float)y,
            Width: width > 0 ? (float)width : 0.0f,
            Height: height > 0 ? (float)height : 0.0f,
            Confidence: double.IsFinite(confidence) ? (float)confidence : 1.0f,
            Valid: valid,
            TimestampMs: Environment.TickCount64,
            SourceTimestampMs: double.IsFinite(sourceTimestamp) ? sourceTimestamp : 0.0);
        return true;
    }
    catch
    {
        return false;
    }
}

static bool TryGetFinite(JsonElement root, string name, out double value)
{
    value = 0.0;
    return root.TryGetProperty(name, out var element) &&
           element.ValueKind == JsonValueKind.Number &&
           element.TryGetDouble(out value) &&
           double.IsFinite(value);
}

static async Task PrintStats(BridgeStats stats, CancellationToken token)
{
    while (!token.IsCancellationRequested)
    {
        await Task.Delay(TimeSpan.FromSeconds(5), token).ContinueWith(_ => { }, CancellationToken.None);
        var age = stats.LastWriteTick > 0 ? Environment.TickCount64 - stats.LastWriteTick : -1;
        var last = stats.Out > 0
            ? $" last=({stats.LastX:F1}, {stats.LastY:F1}) valid={stats.LastValid} age={age}ms"
            : "";
        Console.WriteLine($"[gaze-roi-shared-bridge] in={stats.In} out={stats.Out}{last}");
    }
}

internal readonly record struct GazeSample(
    float X,
    float Y,
    float Width,
    float Height,
    float Confidence,
    bool Valid,
    double TimestampMs,
    double SourceTimestampMs);

internal sealed class BridgeStats
{
    public long In;
    public long Out;
    public float LastX;
    public float LastY;
    public bool LastValid;
    public long LastWriteTick;
}

internal static unsafe class SharedSlot
{
    private const uint Magic = 0x315A4745; // "EGZ1", little endian.
    private const uint Version = 1;
    private static readonly object WriteLock = new();

    public static byte* Pointer;

    public static void Initialize()
    {
        WriteUInt32(0, Magic);
        WriteUInt32(4, Version);
        WriteUInt32(8, 0);
        WriteUInt32(12, 0);
    }

    public static void Write(GazeSample gaze)
    {
        lock (WriteLock)
        {
            ref var seqRef = ref Unsafe.AsRef<uint>(Pointer + 8);
            var nextSeq = Volatile.Read(ref seqRef) + 1;
            if ((nextSeq & 1U) == 0)
                nextSeq++;

            Volatile.Write(ref seqRef, nextSeq);
            WriteUInt32(0, Magic);
            WriteUInt32(4, Version);
            WriteUInt32(12, gaze.Valid ? 1U : 0U);
            WriteDouble(16, gaze.TimestampMs);
            WriteFloat(24, gaze.X);
            WriteFloat(28, gaze.Y);
            WriteFloat(32, gaze.Width);
            WriteFloat(36, gaze.Height);
            WriteFloat(40, gaze.Confidence);
            WriteFloat(44, 0.0f);
            WriteDouble(48, gaze.SourceTimestampMs);
            WriteUInt64(56, 0);
            Volatile.Write(ref seqRef, nextSeq + 1);
        }
    }

    private static void WriteUInt32(int offset, uint value) =>
        BinaryPrimitives.WriteUInt32LittleEndian(new Span<byte>(Pointer + offset, sizeof(uint)), value);

    private static void WriteUInt64(int offset, ulong value) =>
        BinaryPrimitives.WriteUInt64LittleEndian(new Span<byte>(Pointer + offset, sizeof(ulong)), value);

    private static void WriteFloat(int offset, float value) =>
        BinaryPrimitives.WriteSingleLittleEndian(new Span<byte>(Pointer + offset, sizeof(float)), value);

    private static void WriteDouble(int offset, double value) =>
        BinaryPrimitives.WriteDoubleLittleEndian(new Span<byte>(Pointer + offset, sizeof(double)), value);
}
