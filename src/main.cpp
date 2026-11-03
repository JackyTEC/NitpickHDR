// Nitpick — per-monitor HDR control
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Combines HDRTray (res2k, GPL-3.0-or-later: tray icon, status detection,
// Win11 24H2 HDR APIs, autostart) with HDRToggler (GiulioSamp, AGPL-3.0:
// per-monitor toggling), and adds per-monitor "HDR lock".
//
//  * One tray icon per display, showing HDR / SDR / locked for that display.
//  * Left-click a display's icon        -> toggle HDR on that display (1 click)
//  * Right-click any icon -> click item -> toggle / lock any display     (2 clicks)
//  * Locked displays are forced back to SDR whenever anything turns HDR on, and
//    Win+Alt+B is intercepted so it only toggles unlocked displays.

#include "hdr.h"
#include "icons.h"
#include "keyhook.h"
#include "resource.h"
#include "settings.h"
#include "version.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kAppName[] = L"Nitpick";
constexpr wchar_t kWindowClass[] = L"NitpickHDRWindow";
constexpr wchar_t kIdentifyClass[] = L"NitpickHDRIdentify";
constexpr wchar_t kMutexName[] = L"Local\\NitpickHDR-SingleInstance";

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_WIN_ALT_B = WM_APP + 2;

enum : UINT_PTR { TIMER_POLL = 1, TIMER_RECHECK = 2, TIMER_WAIT_TASKBAR = 3, TIMER_HOTKEY_GRACE = 4 };

constexpr UINT kPollWithLocksMs = 1000;
constexpr UINT kPollIdleMs = 4000;
constexpr UINT kFallbackIconUid = 100;
constexpr DWORD kEnforceRetryMs = 2500;

enum : UINT {
    IDM_DISPLAY_TOGGLE = 1000, // + index
    IDM_DISPLAY_LOCK = 2000,   // + index
    IDM_DISPLAY_HIDE = 2500,   // + index
    IDM_ALL_ON = 3000,
    IDM_ALL_OFF,
    IDM_IDENTIFY,
    IDM_OPT_HOTKEY,
    IDM_OPT_OWN_HOTKEY,
    IDM_OPT_SHOW_UNSUPPORTED,
    IDM_OPT_AUTOSTART,
    IDM_OPEN_SETTINGS,
    IDM_EXIT,
};

struct TrayEntry
{
    UINT uid = 0;
    icons::State state = icons::State::Off;
    int number = 0;
    int size = 0;
    bool dark = false;
    std::wstring tip;
    HICON icon = nullptr;
    bool added = false;
};

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
UINT g_msgTaskbarCreated = 0;

std::vector<hdr::Display> g_displays;
std::vector<TrayEntry> g_tray;
bool g_darkTaskbar = true;
int g_recheckCount = 0;
UINT g_pollInterval = 0;
DWORD g_ignoreClicksBefore = 0;
std::map<std::wstring, DWORD> g_lastEnforce;

// Win+Alt+B handling. Windows (Xbox Game Bar) may react to the shortcut even
// though the hook swallows it. Switching HDR is slow (~1 s per change), so we
// don't switch alongside Windows. Instead we pick the intended result from the
// state *before* the press, let Windows do the work, keep locked displays off,
// and only switch unlocked displays ourselves if, after a grace period, they
// still aren't where they should be (Windows didn't react, or did the wrong thing).
bool g_hotkeyActive = false;
bool g_hotkeyTarget = false;
DWORD g_hotkeyGraceUntil = 0;
DWORD g_hotkeyUntil = 0;
std::map<std::wstring, DWORD> g_lastHotkeyFix;
constexpr DWORD kHotkeyGraceMs = 1500;
constexpr DWORD kHotkeySettleMs = 6000;
constexpr DWORD kHotkeyRetryMs = 1200;
std::map<std::wstring, bool> g_enforceWarned;

// --------------------------------------------------------------------------
// Dark mode for menus (same undocumented uxtheme ordinals HDRTray uses)

enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();
SetPreferredAppModeFn g_setPreferredAppMode = nullptr;
FlushMenuThemesFn g_flushMenuThemes = nullptr;

void InitDarkMenus()
{
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux)
        return;
    g_setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
        reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
    g_flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
        reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
}

void UpdateThemeState()
{
    DWORD light = 0, size = sizeof(light);
    g_darkTaskbar = true;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size)
        == ERROR_SUCCESS)
        g_darkTaskbar = light == 0;
    if (g_setPreferredAppMode && g_flushMenuThemes) {
        g_setPreferredAppMode(g_darkTaskbar ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
        g_flushMenuThemes();
    }
}

int TrayIconSize()
{
    UINT dpi = GetDpiForWindow(g_window);
    if (!dpi)
        dpi = GetDpiForSystem();
    return GetSystemMetricsForDpi(SM_CXSMICON, dpi);
}

// --------------------------------------------------------------------------
// Helpers

bool IsLocked(const hdr::Display& d)
{
    return d.status != hdr::Status::Unsupported && settings::IsLocked(d.Key());
}

bool AnyLockActive()
{
    return std::any_of(g_displays.begin(), g_displays.end(), [](const hdr::Display& d) { return IsLocked(d); });
}

icons::State StateFor(const hdr::Display& d)
{
    if (d.status == hdr::Status::Unsupported)
        return icons::State::Unsupported;
    if (IsLocked(d))
        return icons::State::Locked;
    return d.status == hdr::Status::On ? icons::State::On : icons::State::Off;
}

std::wstring Truncate(std::wstring s, size_t max)
{
    if (s.size() > max)
        s = s.substr(0, max - 1) + L"…";
    return s;
}

std::wstring DisplayLabel(const hdr::Display& d)
{
    std::wstring s = std::to_wstring(d.number) + L" · " + Truncate(d.name, 40);
    if (d.primary)
        s += L" (main)";
    return s;
}

std::wstring TooltipFor(const hdr::Display& d)
{
    std::wstring line2;
    switch (StateFor(d)) {
    case icons::State::On: line2 = L"HDR on — click for SDR"; break;
    case icons::State::Off: line2 = L"SDR — click for HDR"; break;
    case icons::State::Locked: line2 = L"SDR — HDR locked off"; break;
    case icons::State::Unsupported: line2 = L"SDR — HDR not supported"; break;
    }
    return Truncate(DisplayLabel(d), 120 - line2.size()) + L"\n" + line2;
}

hdr::Display* FindByKey(const std::wstring& key)
{
    for (auto& d : g_displays)
        if (d.Key() == key)
            return &d;
    return nullptr;
}

void ShowBalloon(const std::wstring& title, const std::wstring& text, DWORD flags = NIIF_INFO, UINT uid = 0)
{
    if (!uid || std::none_of(g_tray.begin(), g_tray.end(), [uid](const TrayEntry& e) { return e.uid == uid; }))
        uid = g_tray.empty() ? kFallbackIconUid : g_tray.front().uid;
    NOTIFYICONDATAW nid { sizeof(nid) };
    nid.hWnd = g_window;
    nid.uID = uid;
    nid.uFlags = NIF_INFO | NIF_REALTIME;
    nid.dwInfoFlags = flags;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Toggling HDR re-centres the mouse cursor, so restore it (as HDRTray does).
std::optional<hdr::Status> SetHdrKeepCursor(const hdr::Display& d, bool enable)
{
    POINT pos {};
    const bool hadPos = GetCursorPos(&pos) != FALSE;
    auto result = hdr::SetHDR(d, enable);
    if (hadPos)
        SetCursorPos(pos.x, pos.y);
    return result;
}

// --------------------------------------------------------------------------
// Tray icons

void DestroyEntry(TrayEntry& e)
{
    if (e.added) {
        NOTIFYICONDATAW nid { sizeof(nid) };
        nid.hWnd = g_window;
        nid.uID = e.uid;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        e.added = false;
    }
    if (e.icon) {
        DestroyIcon(e.icon);
        e.icon = nullptr;
    }
}

bool PushEntry(TrayEntry& e, bool add)
{
    NOTIFYICONDATAW nid { sizeof(nid) };
    nid.hWnd = g_window;
    nid.uID = e.uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = e.icon;
    wcsncpy_s(nid.szTip, e.tip.c_str(), _TRUNCATE);
    if (add) {
        if (!Shell_NotifyIconW(NIM_ADD, &nid))
            return false;
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        e.added = true;
        return true;
    }
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}

void SyncTray()
{
    struct Want
    {
        UINT uid;
        icons::State state;
        int number;
        std::wstring tip;
    };
    std::vector<Want> want;
    const bool showUnsupported = settings::GetBool(settings::kShowUnsupported, true);
    for (const auto& d : g_displays) {
        if (!showUnsupported && d.status == hdr::Status::Unsupported)
            continue;
        if (settings::IsIconHidden(d.Key()))
            continue;
        want.push_back({ static_cast<UINT>(d.number), StateFor(d), d.number, TooltipFor(d) });
    }
    if (want.empty()) {
        // Every display icon is hidden (or there are none): keep one summary icon
        // in the tray so the menu stays reachable. It shows HDR if any display has it on.
        icons::State state = icons::State::Unsupported;
        std::wstring summary;
        for (const auto& d : g_displays) {
            if (d.status == hdr::Status::On)
                state = icons::State::On;
            else if (d.status == hdr::Status::Off && state == icons::State::Unsupported)
                state = icons::State::Off;
            if (!summary.empty())
                summary += L"   ";
            summary += std::to_wstring(d.number) + L": "
                       + (StateFor(d) == icons::State::On ? L"HDR" : StateFor(d) == icons::State::Locked ? L"SDR (locked)" : L"SDR");
        }
        std::wstring tip = g_displays.empty() ? L"Nitpick\nNo displays found"
                                              : L"Nitpick \u2014 right-click for menu\n" + summary;
        want.push_back({ kFallbackIconUid, state, 0, Truncate(tip, 120) });
    }

    // Remove icons that are no longer wanted
    for (auto it = g_tray.begin(); it != g_tray.end();) {
        bool keep = std::any_of(want.begin(), want.end(), [&](const Want& w) { return w.uid == it->uid; });
        if (!keep) {
            DestroyEntry(*it);
            it = g_tray.erase(it);
        } else {
            ++it;
        }
    }

    const int size = TrayIconSize();
    for (const auto& w : want) {
        auto it = std::find_if(g_tray.begin(), g_tray.end(), [&](const TrayEntry& e) { return e.uid == w.uid; });
        if (it == g_tray.end()) {
            g_tray.push_back(TrayEntry { w.uid });
            it = std::prev(g_tray.end());
        }
        TrayEntry& e = *it;
        const bool changed = !e.icon || e.state != w.state || e.number != w.number || e.size != size
                             || e.dark != g_darkTaskbar || e.tip != w.tip;
        if (!changed && e.added)
            continue;
        if (changed) {
            if (e.icon)
                DestroyIcon(e.icon);
            e.state = w.state;
            e.number = w.number;
            e.size = size;
            e.dark = g_darkTaskbar;
            e.tip = w.tip;
            e.icon = icons::Create(size, w.state, w.number, g_darkTaskbar);
        }
        if (!PushEntry(e, !e.added) && e.added) {
            // Modify failed: the icon may have vanished, try to re-add
            e.added = false;
            PushEntry(e, true);
        }
    }

    // Keep tray order stable: ascending display number
    std::sort(g_tray.begin(), g_tray.end(), [](const TrayEntry& a, const TrayEntry& b) { return a.uid < b.uid; });
}

void RecreateAllTrayIcons()
{
    for (auto& e : g_tray)
        DestroyEntry(e);
    g_tray.clear();
    SyncTray();
}

// --------------------------------------------------------------------------
// Lock enforcement & hook management

void UpdateHookAndTimer()
{
    const bool locks = AnyLockActive();
    const bool wantHook = locks && settings::GetBool(settings::kHotkeyRespectsLocks, true);
    if (wantHook && !keyhook::IsInstalled())
        keyhook::Install(g_window, WM_WIN_ALT_B);
    else if (!wantHook && keyhook::IsInstalled())
        keyhook::Uninstall();

    const UINT interval = locks ? kPollWithLocksMs : kPollIdleMs;
    if (interval != g_pollInterval) {
        g_pollInterval = interval;
        SetTimer(g_window, TIMER_POLL, interval, nullptr);
    }
}

// Turn HDR off on any locked display that has it on. Returns true if it acted.
bool EnforceLocks()
{
    bool acted = false;
    const DWORD now = GetTickCount();
    for (const auto& d : g_displays) {
        if (d.status != hdr::Status::On || !IsLocked(d))
            continue;
        const auto key = d.Key();
        auto last = g_lastEnforce.find(key);
        if (last != g_lastEnforce.end() && now - last->second < kEnforceRetryMs)
            continue; // give the previous attempt time to take effect
        g_lastEnforce[key] = now;

        auto res = SetHdrKeepCursor(d, false);
        acted = true;
        if (!res && !g_enforceWarned[key]) {
            g_enforceWarned[key] = true;
            ShowBalloon(L"Couldn't block HDR", L"Display " + std::to_wstring(d.number)
                                                   + L" is locked, but turning HDR back off failed.",
                        NIIF_WARNING, d.number);
        }
    }

    if (g_hotkeyActive && static_cast<LONG>(g_hotkeyUntil - now) <= 0)
        g_hotkeyActive = false;
    // Unlocked displays: only step in once Windows has had its chance.
    if (g_hotkeyActive && static_cast<LONG>(now - g_hotkeyGraceUntil) >= 0) {
        for (const auto& d : g_displays) {
            if (d.status == hdr::Status::Unsupported || IsLocked(d))
                continue;
            if ((d.status == hdr::Status::On) == g_hotkeyTarget)
                continue;
            const auto key = d.Key();
            auto last = g_lastHotkeyFix.find(key);
            if (last != g_lastHotkeyFix.end() && now - last->second < kHotkeyRetryMs)
                continue;
            g_lastHotkeyFix[key] = now;
            SetHdrKeepCursor(d, g_hotkeyTarget);
            acted = true;
        }
    }

    if (acted)
        hdr::RefreshCalibration();
    return acted;
}

void Refresh()
{
    g_displays = hdr::GetDisplays();
    if (EnforceLocks())
        g_displays = hdr::GetDisplays();
    SyncTray();
    UpdateHookAndTimer();
}

void ScheduleRecheck()
{
    // HDR status isn't always up to date right after a change; re-check a few times.
    g_recheckCount = 10;
    SetTimer(g_window, TIMER_RECHECK, 500, nullptr);
}

// --------------------------------------------------------------------------
// Actions

void ToggleDisplay(const std::wstring& key)
{
    g_hotkeyActive = false; // a manual choice overrides a pending Win+Alt+B
    KillTimer(g_window, TIMER_HOTKEY_GRACE);
    Refresh();
    hdr::Display* d = FindByKey(key);
    if (!d)
        return;
    if (d->status == hdr::Status::Unsupported) {
        ShowBalloon(DisplayLabel(*d), L"This display doesn't support HDR.", NIIF_INFO, d->number);
        return;
    }
    if (IsLocked(*d)) {
        ShowBalloon(DisplayLabel(*d), L"HDR is locked off for this display. Right-click the tray icon and "
                                      L"untick “Lock HDR off” to allow it.",
                    NIIF_INFO, d->number);
        return;
    }
    const bool enable = d->status != hdr::Status::On;
    auto res = SetHdrKeepCursor(*d, enable);
    if (!res)
        ShowBalloon(DisplayLabel(*d), L"Changing the HDR mode failed.", NIIF_ERROR, d->number);
    hdr::RefreshCalibration();
    g_ignoreClicksBefore = GetTickCount() + 300;
    Refresh();
    ScheduleRecheck();
}

void SetAllUnlocked(bool enable)
{
    g_hotkeyActive = false;
    KillTimer(g_window, TIMER_HOTKEY_GRACE);
    Refresh();
    bool any = false;
    for (const auto& d : g_displays) {
        if (d.status == hdr::Status::Unsupported || IsLocked(d))
            continue;
        if ((d.status == hdr::Status::On) == enable)
            continue;
        SetHdrKeepCursor(d, enable);
        any = true;
    }
    if (any)
        hdr::RefreshCalibration();
    Refresh();
    ScheduleRecheck();
}

// Replacement for Windows' Win+Alt+B: same "all displays" behaviour, minus locked ones.
void HandleWinAltB()
{
    // Decide from the state *before* the key press. g_displays is kept current by
    // WM_DISPLAYCHANGE and polling; re-querying now could already see Windows'
    // own reaction to the shortcut and flip the decision.
    if (g_displays.empty())
        g_displays = hdr::GetDisplays();

    bool anyOn = false, anyUnlocked = false;
    for (const auto& d : g_displays) {
        if (d.status == hdr::Status::Unsupported || IsLocked(d))
            continue;
        anyUnlocked = true;
        anyOn |= d.status == hdr::Status::On;
    }
    if (!anyUnlocked) {
        ShowBalloon(L"Win+Alt+B blocked", L"HDR is locked off on every HDR-capable display.");
        Refresh(); // undo anything Windows did
        return;
    }

    const DWORD now = GetTickCount();
    g_hotkeyTarget = !anyOn;
    g_hotkeyActive = true;
    g_hotkeyGraceUntil = now + kHotkeyGraceMs;
    g_hotkeyUntil = now + kHotkeySettleMs;
    g_lastHotkeyFix.clear();

    // Nothing is switched here: Windows is (usually) already doing it. Locked
    // displays are kept off by the normal enforcement as soon as anything changes,
    // and the grace timer switches unlocked displays only if Windows didn't.
    SetTimer(g_window, TIMER_HOTKEY_GRACE, kHotkeyGraceMs, nullptr);
    ScheduleRecheck();
}

// Nitpick's own shortcut (Alt+B). Windows doesn't listen to it, so unlike
// Win+Alt+B it never touches locked displays: no flicker, and no waiting for
// Windows. Same "all displays" behaviour as Win+Alt+B, minus locked ones.
constexpr int kOwnHotkeyId = 1;
bool g_ownHotkeyRegistered = false;
bool g_ownHotkeyWarned = false;

void UpdateOwnHotkey()
{
    const bool want = settings::GetBool(settings::kOwnHotkey, true);
    if (want && !g_ownHotkeyRegistered) {
        g_ownHotkeyRegistered = RegisterHotKey(g_window, kOwnHotkeyId, MOD_ALT | MOD_NOREPEAT, 'B') != FALSE;
        if (!g_ownHotkeyRegistered && !g_ownHotkeyWarned) {
            g_ownHotkeyWarned = true;
            ShowBalloon(L"Alt+B unavailable", L"Another app is already using Alt+B.", NIIF_WARNING);
        }
    } else if (!want && g_ownHotkeyRegistered) {
        UnregisterHotKey(g_window, kOwnHotkeyId);
        g_ownHotkeyRegistered = false;
    }
}

void HandleOwnHotkey()
{
    Refresh();
    bool anyOn = false, anyUnlocked = false;
    for (const auto& d : g_displays) {
        if (d.status == hdr::Status::Unsupported || IsLocked(d))
            continue;
        anyUnlocked = true;
        anyOn |= d.status == hdr::Status::On;
    }
    if (!anyUnlocked) {
        ShowBalloon(L"Nothing to switch", L"HDR is locked off on every HDR-capable display.");
        return;
    }
    SetAllUnlocked(!anyOn);
}

void SetLock(const std::wstring& key, bool locked)
{
    settings::SetLocked(key, locked);
    g_enforceWarned.erase(key);
    g_lastEnforce.erase(key);
    Refresh(); // turns HDR off right away if needed
    ScheduleRecheck();
}

// --------------------------------------------------------------------------
// "Identify displays" overlay

struct IdentifyInfo
{
    int number;
    std::wstring name;
    std::wstring status;
    COLORREF accent;
};

LRESULT CALLBACK IdentifyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* info = reinterpret_cast<IdentifyInfo*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 24));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        RECT bar = rc;
        bar.bottom = rc.top + std::max<LONG>(4, (rc.bottom - rc.top) / 24);
        HBRUSH accent = CreateSolidBrush(info ? info->accent : RGB(128, 128, 128));
        FillRect(dc, &bar, accent);
        DeleteObject(accent);
        if (info) {
            const int h = rc.bottom - rc.top;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            HFONT big = CreateFontW(-h * 55 / 100, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                    CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT small = CreateFontW(-h * 8 / 100, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                      CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HGDIOBJ old = SelectObject(dc, big);
            RECT r1 { rc.left, rc.top + h * 4 / 100, rc.right, rc.top + h * 68 / 100 };
            auto num = std::to_wstring(info->number);
            DrawTextW(dc, num.c_str(), -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, small);
            RECT r2 { rc.left + h / 20, rc.top + h * 68 / 100, rc.right - h / 20, rc.top + h * 82 / 100 };
            DrawTextW(dc, info->name.c_str(), -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SetTextColor(dc, info->accent);
            RECT r3 { rc.left, rc.top + h * 82 / 100, rc.right, rc.bottom - h * 4 / 100 };
            DrawTextW(dc, info->status.c_str(), -1, &r3, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
            DeleteObject(big);
            DeleteObject(small);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        delete info;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void IdentifyDisplays()
{
    for (const auto& d : g_displays) {
        const RECT& r = d.desktopRect;
        const int mh = r.bottom - r.top;
        if (mh <= 0)
            continue;
        const int h = mh * 30 / 100;
        const int w = h * 3 / 2;
        const int margin = mh / 25;

        auto* info = new IdentifyInfo { d.number, d.name, L"", RGB(128, 128, 128) };
        switch (StateFor(d)) {
        case icons::State::On: info->status = L"HDR"; info->accent = RGB(255, 185, 0); break;
        case icons::State::Off: info->status = L"SDR"; info->accent = RGB(200, 200, 200); break;
        case icons::State::Locked: info->status = L"SDR · HDR locked off"; info->accent = RGB(255, 80, 90); break;
        case icons::State::Unsupported: info->status = L"SDR · no HDR support"; break;
        }
        HWND wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE
                                       | WS_EX_TRANSPARENT,
                                   kIdentifyClass, L"", WS_POPUP, r.left + margin, r.top + margin, w, h, nullptr,
                                   nullptr, g_instance, info);
        if (!wnd) {
            delete info;
            continue;
        }
        SetLayeredWindowAttributes(wnd, 0, 235, LWA_ALPHA);
        ShowWindow(wnd, SW_SHOWNOACTIVATE);
        SetTimer(wnd, 1, 3000, nullptr);
    }
}

// --------------------------------------------------------------------------
// Context menu

void AppendItem(HMENU menu, UINT id, const std::wstring& text, bool checked = false, bool enabled = true,
                bool isDefault = false)
{
    MENUITEMINFOW mii { sizeof(mii) };
    mii.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
    mii.fType = MFT_STRING;
    mii.wID = id;
    mii.dwTypeData = const_cast<wchar_t*>(text.c_str());
    mii.fState = (checked ? MFS_CHECKED : 0) | (enabled ? 0 : MFS_DISABLED) | (isDefault ? MFS_DEFAULT : 0);
    InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mii);
}

void ShowContextMenu(POINT pt, UINT clickedUid)
{
    Refresh();
    std::vector<std::wstring> keys;
    for (const auto& d : g_displays)
        keys.push_back(d.Key());

    HMENU menu = CreatePopupMenu();
    AppendItem(menu, 0, L"Nitpick  \u00B7  per-monitor HDR  \u00B7  v" NITPICK_VERSION_WSTR, false, false);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    for (size_t i = 0; i < g_displays.size(); ++i) {
        const auto& d = g_displays[i];
        const bool supported = d.status != hdr::Status::Unsupported;
        const bool locked = IsLocked(d);
        std::wstring right;
        if (!supported)
            right = L"SDR only";
        else if (locked)
            right = L"SDR (locked)";
        else
            right = d.status == hdr::Status::On ? L"HDR" : L"SDR";
        AppendItem(menu, IDM_DISPLAY_TOGGLE + static_cast<UINT>(i), DisplayLabel(d) + L"\t" + right,
                   d.status == hdr::Status::On, supported && !locked, static_cast<UINT>(d.number) == clickedUid);
        if (supported)
            AppendItem(menu, IDM_DISPLAY_LOCK + static_cast<UINT>(i), L"      Lock HDR off (block Win+Alt+B etc.)",
                       locked);
        AppendItem(menu, IDM_DISPLAY_HIDE + static_cast<UINT>(i), L"      Hide tray icon",
                   settings::IsIconHidden(d.Key()));
    }
    if (g_displays.empty())
        AppendItem(menu, 0, L"No displays found", false, false);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendItem(menu, IDM_ALL_ON, L"HDR on for all unlocked displays");
    AppendItem(menu, IDM_ALL_OFF, L"HDR off for all displays");
    AppendItem(menu, IDM_IDENTIFY, L"Identify displays");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendItem(menu, IDM_OPT_OWN_HOTKEY, L"Alt+B toggles HDR (skips locked, no flicker)",
               settings::GetBool(settings::kOwnHotkey, true));
    AppendItem(menu, IDM_OPT_HOTKEY, L"Win+Alt+B skips locked displays",
               settings::GetBool(settings::kHotkeyRespectsLocks, true));
    AppendItem(menu, IDM_OPT_SHOW_UNSUPPORTED, L"Show icons for non-HDR displays",
               settings::GetBool(settings::kShowUnsupported, true));
    AppendItem(menu, IDM_OPT_AUTOSTART, L"Start with Windows", settings::IsAutostartEnabled());
    AppendItem(menu, IDM_OPEN_SETTINGS, L"Windows display settings…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendItem(menu, IDM_EXIT, L"Exit");

    SetForegroundWindow(g_window); // so clicking elsewhere closes the menu
    const bool rightAlign = GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0;
    UINT cmd = static_cast<UINT>(TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | (rightAlign ? TPM_RIGHTALIGN : TPM_LEFTALIGN), pt.x,
        pt.y, g_window, nullptr));
    PostMessageW(g_window, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd >= IDM_DISPLAY_TOGGLE && cmd < IDM_DISPLAY_TOGGLE + keys.size()) {
        ToggleDisplay(keys[cmd - IDM_DISPLAY_TOGGLE]);
    } else if (cmd >= IDM_DISPLAY_HIDE && cmd < IDM_DISPLAY_HIDE + keys.size()) {
        const auto& key = keys[cmd - IDM_DISPLAY_HIDE];
        settings::SetIconHidden(key, !settings::IsIconHidden(key));
        SyncTray();
    } else if (cmd >= IDM_DISPLAY_LOCK && cmd < IDM_DISPLAY_LOCK + keys.size()) {
        const auto& key = keys[cmd - IDM_DISPLAY_LOCK];
        SetLock(key, !settings::IsLocked(key));
    } else {
        switch (cmd) {
        case IDM_ALL_ON: SetAllUnlocked(true); break;
        case IDM_ALL_OFF: SetAllUnlocked(false); break;
        case IDM_IDENTIFY: IdentifyDisplays(); break;
        case IDM_OPT_OWN_HOTKEY:
            settings::SetBool(settings::kOwnHotkey, !settings::GetBool(settings::kOwnHotkey, true));
            g_ownHotkeyWarned = false;
            UpdateOwnHotkey();
            break;
        case IDM_OPT_HOTKEY:
            settings::SetBool(settings::kHotkeyRespectsLocks, !settings::GetBool(settings::kHotkeyRespectsLocks, true));
            UpdateHookAndTimer();
            break;
        case IDM_OPT_SHOW_UNSUPPORTED:
            settings::SetBool(settings::kShowUnsupported, !settings::GetBool(settings::kShowUnsupported, true));
            SyncTray();
            break;
        case IDM_OPT_AUTOSTART: settings::SetAutostartEnabled(!settings::IsAutostartEnabled()); break;
        case IDM_OPEN_SETTINGS: ShellExecuteW(nullptr, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL); break;
        case IDM_EXIT: DestroyWindow(g_window); break;
        }
    }
}

// --------------------------------------------------------------------------
// Main window

void OnTrayMessage(WPARAM wParam, LPARAM lParam)
{
    const UINT event = LOWORD(lParam);
    const UINT uid = HIWORD(lParam);
    switch (event) {
    case NIN_SELECT:
    case NIN_KEYSELECT: {
        if (static_cast<LONG>(GetMessageTime() - g_ignoreClicksBefore) < 0)
            return; // swallow double-click / clicks queued while toggling
        if (uid == kFallbackIconUid) {
            POINT pt;
            GetCursorPos(&pt);
            ShowContextMenu(pt, uid);
            return;
        }
        for (const auto& d : g_displays)
            if (static_cast<UINT>(d.number) == uid) {
                ToggleDisplay(d.Key());
                return;
            }
        break;
    }
    case WM_CONTEXTMENU:
        ShowContextMenu(POINT { static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam)) }, uid);
        break;
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g_window = hwnd;
        g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        // Allow TaskbarCreated through UIPI if we ever run elevated
        ChangeWindowMessageFilterEx(hwnd, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
        UpdateThemeState();
        Refresh();
        if (g_tray.empty() || !g_tray.front().added)
            SetTimer(hwnd, TIMER_WAIT_TASKBAR, 30000, nullptr);
        UpdateOwnHotkey();
        return 0;

    case WM_TRAY:
        OnTrayMessage(wParam, lParam);
        return 0;

    case WM_HOTKEY:
        if (wParam == kOwnHotkeyId)
            HandleOwnHotkey();
        return 0;

    case WM_WIN_ALT_B:
        HandleWinAltB();
        return 0;

    case WM_DISPLAYCHANGE:
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Refresh();
        ScheduleRecheck();
        return 0;

    case WM_SETTINGCHANGE:
        UpdateThemeState();
        SyncTray();
        return 0;

    case WM_TIMER:
        switch (wParam) {
        case TIMER_POLL:
            Refresh();
            break;
        case TIMER_RECHECK:
            if (g_recheckCount-- > 0)
                Refresh();
            else
                KillTimer(hwnd, TIMER_RECHECK);
            break;
        case TIMER_HOTKEY_GRACE:
            KillTimer(hwnd, TIMER_HOTKEY_GRACE);
            Refresh(); // switches unlocked displays now if Windows didn't
            break;
        case TIMER_WAIT_TASKBAR:
            KillTimer(hwnd, TIMER_WAIT_TASKBAR);
            if (std::none_of(g_tray.begin(), g_tray.end(), [](const TrayEntry& e) { return e.added; }))
                DestroyWindow(hwnd); // no taskbar ever appeared
            break;
        }
        return 0;

    case WM_DESTROY:
        if (g_ownHotkeyRegistered)
            UnregisterHotKey(hwnd, kOwnHotkeyId);
        keyhook::Uninstall();
        for (auto& e : g_tray)
            DestroyEntry(e);
        g_tray.clear();
        PostQuitMessage(0);
        return 0;
    }

    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        // Explorer restarted or DPI changed: re-create all icons at the new size
        UpdateThemeState();
        RecreateAllTrayIcons();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Developer aid: `NitpickHDR.exe --dump-icons <dir>` writes every icon variant as a 32-bit BMP.
int DumpIcons(const std::wstring& dir)
{
    const icons::State states[] = { icons::State::On, icons::State::Off, icons::State::Locked, icons::State::Unsupported };
    const wchar_t* names[] = { L"on", L"off", L"locked", L"unsupported" };
    for (int size : { 16, 20, 24, 32 })
        for (int dark = 0; dark < 2; ++dark)
            for (int s = 0; s < 4; ++s) {
                HICON icon = icons::Create(size, states[s], 1 + s % 2, dark != 0);
                ICONINFO ii {};
                GetIconInfo(icon, &ii);
                BITMAPINFO bi {};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = size;
                bi.bmiHeader.biHeight = -size;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                std::vector<DWORD> px(static_cast<size_t>(size) * size);
                HDC dc = GetDC(nullptr);
                GetDIBits(dc, ii.hbmColor, 0, size, px.data(), &bi, DIB_RGB_COLORS);
                ReleaseDC(nullptr, dc);
                DeleteObject(ii.hbmColor);
                DeleteObject(ii.hbmMask);
                DestroyIcon(icon);

                BITMAPFILEHEADER fh {};
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
                fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size() * 4);
                auto path = dir + L"\\" + names[s] + (dark ? L"_dark_" : L"_light_") + std::to_wstring(size) + L".bmp";
                HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (f == INVALID_HANDLE_VALUE)
                    return 1;
                DWORD written;
                WriteFile(f, &fh, sizeof(fh), &written, nullptr);
                WriteFile(f, &bi.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr);
                WriteFile(f, px.data(), static_cast<DWORD>(px.size() * 4), &written, nullptr);
                CloseHandle(f);
            }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR cmdLine, int)
{
    g_instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (cmdLine && wcsncmp(cmdLine, L"--dump-icons ", 13) == 0)
        return DumpIcons(cmdLine + 13);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Nitpick is already running — look for it in the notification area.", kAppName,
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    InitDarkMenus();

    WNDCLASSEXW wc { sizeof(wc) };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW ic { sizeof(ic) };
    ic.lpfnWndProc = IdentifyWndProc;
    ic.hInstance = instance;
    ic.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    ic.lpszClassName = kIdentifyClass;
    RegisterClassExW(&ic);

    // Hidden top-level window at (0,0) so it lives on the primary monitor (where the tray is)
    HWND hwnd = CreateWindowExW(0, kWindowClass, kAppName, WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
                                instance, nullptr);
    if (!hwnd)
        return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex)
        CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
