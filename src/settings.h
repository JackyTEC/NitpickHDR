// Nitpick — per-monitor HDR control
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <string>

namespace settings {

// Stored under HKCU\Software\NitpickHDR
bool GetBool(const wchar_t* name, bool defaultValue);
void SetBool(const wchar_t* name, bool value);

// Per-display "HDR locked off" flags, keyed by monitor device path
bool IsLocked(const std::wstring& displayKey);
void SetLocked(const std::wstring& displayKey, bool locked);

// Per-display "hide tray icon" flags, keyed the same way
bool IsIconHidden(const std::wstring& displayKey);
void SetIconHidden(const std::wstring& displayKey, bool hidden);

// HKCU\...\Run autostart entry
bool IsAutostartEnabled();
void SetAutostartEnabled(bool enabled);

inline constexpr wchar_t kHotkeyRespectsLocks[] = L"WinAltBRespectsLocks";
inline constexpr wchar_t kShowUnsupported[] = L"ShowNonHdrDisplays";

} // namespace settings
