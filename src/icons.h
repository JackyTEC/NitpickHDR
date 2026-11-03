// Nitpick — per-monitor HDR control
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <windows.h>

namespace icons {

enum class State {
    On,          // HDR active          -> gold filled badge, "HDR"
    Off,         // SDR, can be toggled -> outlined badge, "SDR"
    Locked,      // HDR locked off      -> red outlined badge, "SDR"
    Unsupported, // display can't do HDR -> faded outlined badge, "SDR"
};

/// Render a tray icon of `size` px. `number` (1..9+) is the display number shown
/// under the label; 0 = no number. `darkTaskbar` picks the foreground colour.
HICON Create(int size, State state, int number, bool darkTaskbar);

} // namespace icons
