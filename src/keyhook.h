// Nitpick — per-monitor HDR control
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <windows.h>

// Intercepts Win+Alt+B (Windows' global "toggle HDR" shortcut) before Windows
// sees it, and posts `message` to `target` instead, so the app can perform a
// lock-aware toggle.
namespace keyhook {

bool Install(HWND target, UINT message);
void Uninstall();
bool IsInstalled();

} // namespace keyhook
