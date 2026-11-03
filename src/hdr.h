// Nitpick — per-monitor HDR control
// SPDX-License-Identifier: AGPL-3.0-or-later
// HDR query/set logic derived from HDRTray (Frank Richter, GPL-3.0-or-later),
// itself based on Kodi (GPL-2.0-or-later).
#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace hdr {

enum class Status { Unsupported = 0, Off = 1, On = 2 };

struct Display
{
    LUID adapterId {};
    UINT32 targetId = 0;
    std::wstring name;       // Friendly name from EDID (or fallback)
    std::wstring devicePath; // Stable identifier for this monitor+connector
    Status status = Status::Unsupported;
    RECT desktopRect {};     // Position on the desktop (physical pixels)
    bool primary = false;
    int number = 0;          // 1-based, ordered left-to-right

    // Key used for persisted settings (locks)
    std::wstring Key() const;
};

/// All active displays, sorted left-to-right and numbered from 1.
std::vector<Display> GetDisplays();

/// Re-query the status of one display.
Status GetStatus(const Display& display);

/// Turn HDR on/off for a single display. Returns the re-queried status,
/// or nullopt if the request failed.
std::optional<Status> SetHDR(const Display& display, bool enable);

/// Ask Windows' color system to refresh calibration after a change
/// (same thing HDRTray does after toggling).
void RefreshCalibration();

} // namespace hdr
