#include "pch.h"
#include <menu/input/input_system_internal.h>
#include <include/imgui/imgui.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace
{
std::array<bool, 256> physicalKeys {};
std::array<unsigned, 256> polls {};
unsigned checks = 0;

void Require(bool condition, const char* message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
SHORT WINAPI FakeKeyState(int vk)
{
    if (vk < 0 || vk >= 256) return 0;
    ++polls[vk];
    return physicalKeys[vk] ? SHORT(0x8000) : 0;
}
BOOL WINAPI FakeCursor(POINT* point) { *point = {111, 222}; return TRUE; }
unsigned PollCount()
{
    unsigned count = 0;
    for (const auto value : polls) count += value;
    return count;
}
void PollFrame()
{
    OptiInput::ClearTransientState();
    polls.fill(0);
    OptiInput::PollInputFallbackLocked();
}
void TestPolling()
{
    const auto originalKeys = OptiInput::o_GetAsyncKeyState;
    const auto originalCursor = OptiInput::o_GetCursorPos;
    OptiInput::o_GetAsyncKeyState = FakeKeyState;
    OptiInput::o_GetCursorPos = FakeCursor;
    auto& state = OptiInput::_state;
    state.Keys = {};
    state.MouseButtons = {};
    state.Focused = true;
    state.MenuVisible = false;
    const int shortcuts[] {VK_INSERT, VK_F1, VK_F2, VK_F3};
    OptiInput::SetHiddenMenuPollingKeys(shortcuts, std::size(shortcuts));
    PollFrame();
    Require(PollCount() == 15, "Closed menu polls four shortcuts, six modifier keys and five mouse buttons");
    Require(polls['A'] == 0, "Unrelated idle keys do not invoke Win32 polling");
    Require(state.MouseScreenPos.x == 111 && state.MouseScreenPos.y == 222,
            "Closed-menu mouse position remains available to the magnifier");

    physicalKeys[VK_INSERT] = true;
    PollFrame();
    Require(OptiInput::IsKeyPressed(VK_INSERT), "A configured shortcut produces a press");
    PollFrame();
    Require(!OptiInput::IsKeyPressed(VK_INSERT) && OptiInput::IsKeyDown(VK_INSERT), "Held shortcuts do not repeat presses");
    physicalKeys[VK_INSERT] = false;
    PollFrame();
    Require(OptiInput::IsKeyReleased(VK_INSERT), "Shortcut release reaches the menu toggle consumer");
    PollFrame();
    Require(!OptiInput::IsKeyReleased(VK_INSERT), "Shortcut release is transient");

    for (const auto [left, right, aggregate] : {
             std::array<int, 3>{VK_LSHIFT, VK_RSHIFT, VK_SHIFT},
             std::array<int, 3>{VK_LCONTROL, VK_RCONTROL, VK_CONTROL},
             std::array<int, 3>{VK_LMENU, VK_RMENU, VK_MENU}})
    {
        physicalKeys[left] = true;
        PollFrame();
        Require(OptiInput::IsKeyDown(aggregate), "Generic modifiers track their left key");
        physicalKeys[left] = false;
        physicalKeys[right] = true;
        PollFrame();
        Require(OptiInput::IsKeyDown(aggregate), "Generic modifiers survive a left/right handoff");
        physicalKeys[right] = false;
        PollFrame();
        Require(!OptiInput::IsKeyDown(aggregate), "Generic modifiers release cleanly");
    }

    state.MenuVisible = true;
    physicalKeys['A'] = true;
    PollFrame();
    Require(PollCount() == 253, "Open menu retains full keyboard and mouse fallback");
    Require(OptiInput::IsKeyDown('A'), "Open menu captures an unconfigured key");
    state.MenuVisible = false;
    physicalKeys['A'] = false;
    PollFrame();
    Require(polls['A'] == 1 && OptiInput::IsKeyReleased('A'), "Closing the menu releases previously held keys");
    PollFrame();
    Require(polls['A'] == 0 && PollCount() == 15, "Released non-shortcuts leave the reduced polling set");

    physicalKeys[VK_F1] = true;
    PollFrame();
    const int rebound[] {VK_INSERT, VK_F4, VK_F2, VK_F3, VK_F4, -1, 256};
    OptiInput::SetHiddenMenuPollingKeys(rebound, std::size(rebound));
    physicalKeys[VK_F1] = false;
    physicalKeys[VK_F4] = true;
    PollFrame();
    Require(polls[VK_F1] == 1 && OptiInput::IsKeyReleased(VK_F1), "Rebinding does not strand the old held shortcut");
    Require(polls[VK_F4] == 1 && OptiInput::IsKeyPressed(VK_F4), "New shortcut works immediately and duplicates are deduplicated");
    Require(PollCount() == 16, "Invalid bindings are ignored and only the old held key adds a poll");

    state.Focused = false;
    PollFrame();
    Require(PollCount() == 0, "Unfocused windows do not poll keyboard or mouse");
    state.Focused = true;
    physicalKeys.fill(false);
    OptiInput::SetHiddenMenuPollingKeys(nullptr, 0);
    PollFrame();
    Require(PollCount() == 253, "A null key list restores the full fallback for other consumers");
    OptiInput::o_GetAsyncKeyState = originalKeys;
    OptiInput::o_GetCursorPos = originalCursor;
    state.Keys = {};
    state.MouseButtons = {};
}

void TestBlockingFocus()
{
    auto& state = OptiInput::_state;
    state.Focused = false;
    OptiInput::SetMenuVisible(true);
    state.Focused = true;
    Require(OptiInput::ShouldBlockKeyboardInputLocked() && OptiInput::ShouldBlockMouseInputLocked() &&
            OptiInput::ShouldBlockCursorInputLocked(), "Visible focused menu blocks game input");
    state.Focused = false;
    Require(!OptiInput::ShouldBlockKeyboardInputLocked() && !OptiInput::ShouldBlockMouseInputLocked() &&
            !OptiInput::ShouldBlockCursorInputLocked(), "Focus loss releases all game-input blocking policies");
    state.Focused = true;
    {
        OptiInput::ScopedHookBypass bypass;
        Require(!OptiInput::ShouldBlockKeyboardInputLocked(), "Internal input calls bypass blocking after focus returns");
    }
    Require(OptiInput::ShouldBlockKeyboardInputLocked(), "Focus regain restores blocking for the open menu");
    OptiInput::SetMenuVisible(false);
    Require(!OptiInput::ShouldBlockKeyboardInputLocked(), "Closing the menu releases game-input blocking");
    state.Focused = false;
}

template<class Function> void Measure(const char* name, Function work)
{
    constexpr int count = 512;
    for (int i = 0; i < 16; ++i) work();
    const auto before = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) work();
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()-before).count()/count;
    std::printf("%s us_per_call=%.3f\n", name, us);
}
}

int main(int argc, char** argv)
{
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = L"OptiInputPollingTestWindow";
    Require(RegisterClassW(&wc) != 0, "Register hidden test window class");
    const auto hwnd = CreateWindowExW(0, wc.lpszClassName, L"Input tests", WS_POPUP, 0, 0, 32, 32,
                                      nullptr, nullptr, instance, nullptr);
    Require(hwnd != nullptr, "Create hidden test window");
    const auto xinput = LoadLibraryW(L"xinput1_4.dll");
    const auto dinput = LoadLibraryW(L"dinput8.dll");
    ImGui::CreateContext();
    Require(OptiInput::Initialize(hwnd, false), "Initialize production input hooks");
    Require(OptiInput::o_CreateFileA == &::CreateFileA && OptiInput::o_CreateFileW == &::CreateFileW &&
            OptiInput::o_ReadFile == &::ReadFile && OptiInput::o_DeviceIoControl == &::DeviceIoControl &&
            OptiInput::o_CloseHandle == &::CloseHandle,
            "HID file/IO hooks remain disabled in the default build");
    Require(OptiInput::_state.HooksInstalled, "Disabled HID hooks do not cause incomplete installation retries");
    TestPolling();
    TestBlockingFocus();
    std::printf("Passed %u checks against production input sources.\n", checks);
    if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0)
    {
        std::puts("Isolated Win32 timings with a hidden window; these are not game FPS measurements.");
        Measure("BeginFrame-not-focused", [&] { OptiInput::BeginFrame(hwnd); });
        Measure("EndFrame-menu-closed", [] { OptiInput::EndFrame(false); });
        OptiInput::_state.Focused = true;
        OptiInput::SetHiddenMenuPollingKeys(nullptr, 0);
        Measure("Input-fallback-full", [] { OptiInput::PollInputFallbackLocked(); });
        const int shortcuts[] {VK_INSERT, VK_F1, VK_F2, VK_F3};
        OptiInput::SetHiddenMenuPollingKeys(shortcuts, std::size(shortcuts));
        Measure("Input-fallback-shortcuts", [] { OptiInput::PollInputFallbackLocked(); });
    }
    OptiInput::_state.Focused = false;
    OptiInput::Shutdown();
    Require(!OptiInput::_state.HiddenMenuPollingConfigured, "Shutdown resets polling configuration");
    Require(!OptiInput::_state.Initialized && !OptiInput::_state.HooksInstalled, "Shutdown removes installed input hooks");
    Require(OptiInput::Initialize(hwnd, false), "Input hooks can be reinstalled after a complete shutdown");
    OptiInput::Shutdown();
    Require(!OptiInput::_state.Initialized && !OptiInput::_state.HooksInstalled, "Repeated lifecycle leaves no installed hooks");
    ImGui::DestroyContext();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    if (xinput) FreeLibrary(xinput);
    if (dinput) FreeLibrary(dinput);
    std::printf("Passed all %u checks; hooks removed and hidden window destroyed.\n", checks);
}
