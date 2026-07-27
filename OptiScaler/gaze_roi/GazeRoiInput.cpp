#include "pch.h"
#include "GazeRoiInput.h"

#include <Config.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

namespace
{
#pragma pack(push, 1)
struct GazeSharedMemorySlot
{
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t flags;
    double timestampMs;
    float x;
    float y;
    float width;
    float height;
    float confidence;
    float reserved0;
    double sourceTimestampMs;
    uint64_t reserved1;
};
#pragma pack(pop)

static_assert(sizeof(GazeSharedMemorySlot) == 64);
constexpr uint32_t GazeSharedMagic = 0x315A4745;
constexpr uint32_t GazeSharedVersion = 1;

std::mutex inputMutex;
GazeRoiInputSample currentSample {};
HANDLE sharedMapping = nullptr;
const void* sharedView = nullptr;
uint64_t sharedLastOpenFailureTick = 0;
SOCKET udpSocket = INVALID_SOCKET;
bool winsockStarted = false;
int udpPort = 0;
uint64_t udpLastTick = 0;

bool ExtractJsonNumber(const std::string& text, const char* key, double& value)
{
    const std::string quoted = std::format("\"{}\"", key);
    const size_t keyPos = text.find(quoted);
    const size_t colon = keyPos == std::string::npos ? std::string::npos : text.find(':', keyPos + quoted.size());
    if (colon == std::string::npos)
        return false;
    char* end = nullptr;
    value = std::strtod(text.c_str() + colon + 1, &end);
    return end != text.c_str() + colon + 1 && std::isfinite(value);
}

void CloseSharedMemory()
{
    if (sharedView != nullptr)
        UnmapViewOfFile(sharedView);
    if (sharedMapping != nullptr)
        CloseHandle(sharedMapping);
    sharedView = nullptr;
    sharedMapping = nullptr;
}

void StopUdp()
{
    if (udpSocket != INVALID_SOCKET)
        closesocket(udpSocket);
    udpSocket = INVALID_SOCKET;
    if (winsockStarted)
        WSACleanup();
    winsockStarted = false;
    udpPort = 0;
    udpLastTick = 0;
}

bool EnsureUdp(int port)
{
    port = std::clamp(port, 1024, 65535);
    if (udpSocket != INVALID_SOCKET && udpPort == port)
        return true;

    StopUdp();
    WSADATA data {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return false;
    winsockStarted = true;
    udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET)
    {
        StopUdp();
        return false;
    }

    u_long nonBlocking = 1;
    ioctlsocket(udpSocket, FIONBIO, &nonBlocking);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(udpSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        StopUdp();
        return false;
    }
    udpPort = port;
    return true;
}

void PollSharedMemory(int staleMs)
{
    StopUdp();
    if (sharedView == nullptr && GetTickCount64() - sharedLastOpenFailureTick >= 2000)
    {
        sharedMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\EyeTracingGazeV1");
        if (sharedMapping != nullptr)
            sharedView = MapViewOfFile(sharedMapping, FILE_MAP_READ, 0, 0, sizeof(GazeSharedMemorySlot));
        if (sharedView == nullptr)
        {
            CloseSharedMemory();
            sharedLastOpenFailureTick = GetTickCount64();
        }
    }

    if (sharedView == nullptr)
        return;

    const auto* view = static_cast<const GazeSharedMemorySlot*>(sharedView);
    GazeSharedMemorySlot sample {};
    bool consistent = false;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const uint32_t before = view->seq;
        if ((before & 1U) != 0)
            continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        std::memcpy(&sample, view, sizeof(sample));
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t after = view->seq;
        if (before == after && (after & 1U) == 0)
        {
            consistent = true;
            break;
        }
    }

    if (!consistent || sample.magic != GazeSharedMagic || sample.version != GazeSharedVersion ||
        (sample.flags & 1U) == 0 || sample.timestampMs <= 0.0 ||
        GetTickCount64() - static_cast<uint64_t>(sample.timestampMs) >
            static_cast<uint64_t>(std::max(1, staleMs)))
        return;

    float x = sample.x;
    float y = sample.y;
    if (sample.width > 0.0f && sample.height > 0.0f)
    {
        x /= sample.width;
        y /= sample.height;
    }
    if (std::isfinite(x) && std::isfinite(y))
    {
        currentSample.x = std::clamp(x, 0.0f, 1.0f);
        currentSample.y = std::clamp(y, 0.0f, 1.0f);
    }
}

void PollUdp(int port, int staleMs)
{
    CloseSharedMemory();
    if (!EnsureUdp(port))
        return;

    char buffer[1024];
    for (;;)
    {
        const int received = recv(udpSocket, buffer, sizeof(buffer), 0);
        if (received <= 0)
            break;
        const std::string text(buffer, buffer + received);
        double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
        if (!ExtractJsonNumber(text, "x", x) || !ExtractJsonNumber(text, "y", y))
            continue;
        if (ExtractJsonNumber(text, "width", width) && ExtractJsonNumber(text, "height", height) && width > 0.0 &&
            height > 0.0)
        {
            x /= width;
            y /= height;
        }
        if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0)
            continue;
        currentSample.x = static_cast<float>(x);
        currentSample.y = static_cast<float>(y);
        udpLastTick = GetTickCount64();
    }

    if (udpLastTick == 0 || GetTickCount64() - udpLastTick > static_cast<uint64_t>(std::max(1, staleMs)))
        return;
}
} // namespace

GazeRoiInputSample GazeRoiInput::Sample()
{
    std::lock_guard lock(inputMutex);
    currentSample.recentered = false;
    const std::string control = Config::Instance()->GazeRoiControl.value_or_default();
    const int staleMs = Config::Instance()->GazeRoiStaleMs.value_or_default();

    if (control == "ExternalSharedMemory")
    {
        PollSharedMemory(staleMs);
        return currentSample;
    }
    if (control == "ExternalUdp")
    {
        PollUdp(Config::Instance()->GazeRoiUdpPort.value_or_default(), staleMs);
        return currentSample;
    }

    CloseSharedMemory();
    StopUdp();
    if (control == "Mouse")
    {
        POINT cursor {};
        RECT client {};
        HWND window = GetForegroundWindow();
        if (window != nullptr && GetCursorPos(&cursor) && ScreenToClient(window, &cursor) &&
            GetClientRect(window, &client) && client.right > client.left && client.bottom > client.top)
        {
            currentSample.x = std::clamp(
                static_cast<float>(cursor.x) / static_cast<float>(client.right - client.left), 0.0f, 1.0f);
            currentSample.y = std::clamp(
                static_cast<float>(cursor.y) / static_cast<float>(client.bottom - client.top), 0.0f, 1.0f);
        }
        return currentSample;
    }

    if (control == "Keyboard")
    {
        const float step = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0.01f : 0.05f;
        if (GetAsyncKeyState(VK_F5) & 0x0001) currentSample.x -= step;
        if (GetAsyncKeyState(VK_F6) & 0x0001) currentSample.x += step;
        if (GetAsyncKeyState(VK_F7) & 0x0001) currentSample.y -= step;
        if (GetAsyncKeyState(VK_F8) & 0x0001) currentSample.y += step;
        if (GetAsyncKeyState(VK_F9) & 0x0001)
        {
            currentSample.x = 0.5f;
            currentSample.y = 0.5f;
            currentSample.recentered = true;
        }
        currentSample.x = std::clamp(currentSample.x, 0.0f, 1.0f);
        currentSample.y = std::clamp(currentSample.y, 0.0f, 1.0f);
    }
    return currentSample;
}

void GazeRoiInput::Stop()
{
    std::lock_guard lock(inputMutex);
    CloseSharedMemory();
    StopUdp();
}
