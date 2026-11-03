// Nitpick — per-monitor HDR control
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The hook lives on its own thread: HDR mode switches block the UI thread for a
// second or more, and Windows silently removes low-level hooks whose thread
// doesn't respond in time.

#include "keyhook.h"

namespace keyhook {

namespace {
constexpr UINT WM_HOOK_MASK_KEY = WM_APP + 50;

HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
HHOOK g_hook = nullptr;
HWND g_target = nullptr;
UINT g_message = 0;
bool g_swallowingB = false;

bool IsDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (k->vkCode == 'B' && !(k->flags & LLKHF_INJECTED)) {
            const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            if (down) {
                // Modifiers were pressed in earlier events, so their async state is current.
                const bool win = IsDown(VK_LWIN) || IsDown(VK_RWIN);
                const bool alt = (k->flags & LLKHF_ALTDOWN) || IsDown(VK_MENU);
                if (win && alt && !IsDown(VK_CONTROL) && !IsDown(VK_SHIFT)) {
                    if (!g_swallowingB) { // ignore auto-repeat
                        g_swallowingB = true;
                        PostThreadMessageW(g_threadId, WM_HOOK_MASK_KEY, 0, 0);
                        PostMessageW(g_target, g_message, 0, 0);
                    }
                    return 1; // Windows never sees it
                }
            } else if (g_swallowingB) {
                g_swallowingB = false;
                return 1;
            }
        }
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

DWORD WINAPI HookThread(LPVOID ready)
{
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE); // create the message queue
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    SetEvent(static_cast<HANDLE>(ready));
    if (!g_hook)
        return 1;

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOOK_MASK_KEY) {
            // Tap an unassigned key so releasing Win doesn't open the Start menu.
            INPUT in[2] {};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = 0xE8;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
        }
    }
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    return 0;
}
} // namespace

bool Install(HWND target, UINT message)
{
    if (g_thread)
        return true;
    g_target = target;
    g_message = message;
    g_swallowingB = false;

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, HookThread, ready, 0, &g_threadId);
    if (!g_thread) {
        CloseHandle(ready);
        return false;
    }
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    if (!g_hook) {
        WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = nullptr;
        return false;
    }
    return true;
}

void Uninstall()
{
    if (!g_thread)
        return;
    PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_threadId = 0;
}

bool IsInstalled()
{
    return g_thread != nullptr;
}

} // namespace keyhook
