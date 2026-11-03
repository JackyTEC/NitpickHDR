// Nitpick — per-monitor HDR control
// SPDX-License-Identifier: AGPL-3.0-or-later
// HDR query/set logic derived from HDRTray (Frank Richter, GPL-3.0-or-later),
// itself based on Kodi (GPL-2.0-or-later).

#include "hdr.h"

#include <algorithm>
#include <cstdint>

namespace hdr {

namespace {

// The structures below are defined locally so the project builds with any
// Windows SDK / MinGW version. Layouts match wingdi.h (SDK 10.0.26100).
constexpr auto kGetAdvancedColorInfo = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(9);
constexpr auto kSetAdvancedColorState = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(10);
constexpr auto kGetAdvancedColorInfo2 = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15); // Win11 24H2+
constexpr auto kSetHdrState = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(16);           // Win11 24H2+

struct AdvancedColorInfo
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value; // bit0 supported, bit1 enabled, bit2 wideColorEnforced, bit3 forceDisabled
    UINT32 colorEncoding;
    UINT32 bitsPerColorChannel;
};

struct SetAdvancedColorState
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value; // bit0 enableAdvancedColor
};

struct AdvancedColorInfo2
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value; // bit4 highDynamicRangeSupported, bit5 highDynamicRangeUserEnabled, ...
    UINT32 colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode; // 0 SDR, 1 WCG, 2 HDR
};

struct SetHdrState
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 value; // bit0 enableHdr
};

constexpr UINT32 kAdvColorSupported = 1u << 0;
constexpr UINT32 kAdvColorEnabled = 1u << 1;
constexpr UINT32 kHdrSupported2 = 1u << 4;
constexpr UINT32 kColorModeHdr = 2;

static_assert(sizeof(AdvancedColorInfo) == 32);
static_assert(sizeof(AdvancedColorInfo2) == 36);
static_assert(sizeof(SetHdrState) == 24);

bool IsBuildAtLeast(DWORD build)
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    if (!rtlGetVersion)
        return false;
    OSVERSIONINFOW vi { sizeof(vi) };
    if (rtlGetVersion(&vi) != 0)
        return false;
    return vi.dwMajorVersion > 10 || (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= build);
}

const bool g_win11_24h2 = IsBuildAtLeast(26100);

template<typename T> T MakePacket(DISPLAYCONFIG_DEVICE_INFO_TYPE type, const LUID& adapter, UINT32 id)
{
    T packet {};
    packet.header.type = type;
    packet.header.size = sizeof(T);
    packet.header.adapterId = adapter;
    packet.header.id = id;
    return packet;
}

Status QueryStatus(const LUID& adapter, UINT32 id)
{
    // Prefer GET_ADVANCED_COLOR_INFO_2: reports the real HDR mode even with
    // Auto Color Management enabled.
    if (g_win11_24h2) {
        auto info2 = MakePacket<AdvancedColorInfo2>(kGetAdvancedColorInfo2, adapter, id);
        if (DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) {
            if (!(info2.value & kHdrSupported2))
                return Status::Unsupported;
            return info2.activeColorMode == kColorModeHdr ? Status::On : Status::Off;
        }
    }

    auto info = MakePacket<AdvancedColorInfo>(kGetAdvancedColorInfo, adapter, id);
    if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS)
        return Status::Unsupported;
    if (!(info.value & kAdvColorSupported))
        return Status::Unsupported;
    return (info.value & kAdvColorEnabled) ? Status::On : Status::Off;
}

} // namespace

std::wstring Display::Key() const
{
    if (!devicePath.empty())
        return devicePath;
    return name + L"#" + std::to_wstring(targetId);
}

std::vector<Display> GetDisplays()
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;

    for (int attempt = 0; attempt < 4; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return {};
        paths.resize(pathCount);
        modes.resize(modeCount);
        LONG res = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (res == ERROR_INSUFFICIENT_BUFFER)
            continue; // Topology changed between the two calls
        if (res != ERROR_SUCCESS)
            return {};
        paths.resize(pathCount);
        modes.resize(modeCount);
        break;
    }

    std::vector<Display> result;
    for (const auto& path : paths) {
        if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE))
            continue;

        Display d;
        d.adapterId = path.targetInfo.adapterId;
        d.targetId = path.targetInfo.id;

        // Skip duplicates (e.g. clone mode)
        bool dup = std::any_of(result.begin(), result.end(), [&](const Display& o) {
            return o.targetId == d.targetId && o.adapterId.LowPart == d.adapterId.LowPart
                && o.adapterId.HighPart == d.adapterId.HighPart;
        });
        if (dup)
            continue;

        auto deviceName = MakePacket<DISPLAYCONFIG_TARGET_DEVICE_NAME>(DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
                                                                       d.adapterId, d.targetId);
        if (DisplayConfigGetDeviceInfo(&deviceName.header) == ERROR_SUCCESS) {
            d.name = deviceName.monitorFriendlyDeviceName;
            d.devicePath = deviceName.monitorDevicePath;
            if (d.name.empty() && deviceName.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL)
                d.name = L"Internal Display";
        }

        UINT32 srcIdx = path.sourceInfo.modeInfoIdx;
        if (srcIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && srcIdx < modes.size()
            && modes[srcIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const auto& sm = modes[srcIdx].sourceMode;
            d.desktopRect = RECT { sm.position.x, sm.position.y, sm.position.x + static_cast<LONG>(sm.width),
                                   sm.position.y + static_cast<LONG>(sm.height) };
            d.primary = sm.position.x == 0 && sm.position.y == 0;
        } else {
            // Fallback: look up the desktop position via the GDI device name
            auto src = MakePacket<DISPLAYCONFIG_SOURCE_DEVICE_NAME>(DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                                    path.sourceInfo.adapterId, path.sourceInfo.id);
            DEVMODEW dm { };
            dm.dmSize = sizeof(dm);
            if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS
                && EnumDisplaySettingsW(src.viewGdiDeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                d.desktopRect = RECT { dm.dmPosition.x, dm.dmPosition.y,
                                       dm.dmPosition.x + static_cast<LONG>(dm.dmPelsWidth),
                                       dm.dmPosition.y + static_cast<LONG>(dm.dmPelsHeight) };
                d.primary = dm.dmPosition.x == 0 && dm.dmPosition.y == 0;
            }
        }

        d.status = QueryStatus(d.adapterId, d.targetId);
        result.push_back(std::move(d));
    }

    std::stable_sort(result.begin(), result.end(), [](const Display& a, const Display& b) {
        if (a.desktopRect.left != b.desktopRect.left)
            return a.desktopRect.left < b.desktopRect.left;
        return a.desktopRect.top < b.desktopRect.top;
    });
    for (size_t i = 0; i < result.size(); ++i) {
        result[i].number = static_cast<int>(i + 1);
        if (result[i].name.empty())
            result[i].name = L"Display " + std::to_wstring(i + 1);
    }
    return result;
}

Status GetStatus(const Display& display)
{
    return QueryStatus(display.adapterId, display.targetId);
}

std::optional<Status> SetHDR(const Display& display, bool enable)
{
    if (QueryStatus(display.adapterId, display.targetId) == Status::Unsupported)
        return std::nullopt;

    // On Win11 24H2+, SET_HDR_STATE behaves better with ACM enabled.
    if (g_win11_24h2) {
        auto set = MakePacket<SetHdrState>(kSetHdrState, display.adapterId, display.targetId);
        set.value = enable ? 1u : 0u;
        if (DisplayConfigSetDeviceInfo(&set.header) == ERROR_SUCCESS)
            return QueryStatus(display.adapterId, display.targetId);
    }

    auto set = MakePacket<SetAdvancedColorState>(kSetAdvancedColorState, display.adapterId, display.targetId);
    set.value = enable ? 1u : 0u;
    if (DisplayConfigSetDeviceInfo(&set.header) != ERROR_SUCCESS)
        return std::nullopt;
    return QueryStatus(display.adapterId, display.targetId);
}

void RefreshCalibration()
{
    // mscms!InternalRefreshCalibration: undocumented, exported by name.
    using Fn = BOOL(WINAPI*)(LPCWSTR, uintptr_t, const GUID*, const GUID*);
    static HMODULE mscms = LoadLibraryExW(L"mscms.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto fn = mscms ? reinterpret_cast<Fn>(reinterpret_cast<void*>(
                                 GetProcAddress(mscms, "InternalRefreshCalibration")))
                           : nullptr;
    if (fn)
        fn(nullptr, 0, nullptr, nullptr);
}

} // namespace hdr
