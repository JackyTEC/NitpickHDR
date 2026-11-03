// Nitpick — per-monitor HDR control
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "settings.h"

#include <windows.h>

#include <cwchar>
#include <iterator>
#include <string>

namespace settings {

namespace {
constexpr wchar_t kRootKey[] = L"Software\\NitpickHDR";
constexpr wchar_t kLocksKey[] = L"Software\\NitpickHDR\\LockedDisplays";
constexpr wchar_t kHiddenKey[] = L"Software\\NitpickHDR\\HiddenIcons";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"NitpickHDR";

bool ReadDword(const wchar_t* key, const wchar_t* name, DWORD& out)
{
    DWORD size = sizeof(out);
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
}

void WriteDword(const wchar_t* key, const wchar_t* name, DWORD value)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_DWORD, &value, sizeof(value));
}

std::wstring AutostartCommand()
{
    wchar_t path[MAX_PATH * 2] {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return L"\"" + std::wstring(path) + L"\"";
}
} // namespace

bool GetBool(const wchar_t* name, bool defaultValue)
{
    DWORD v = 0;
    return ReadDword(kRootKey, name, v) ? v != 0 : defaultValue;
}

void SetBool(const wchar_t* name, bool value)
{
    WriteDword(kRootKey, name, value ? 1 : 0);
}

namespace {
bool GetFlag(const wchar_t* key, const std::wstring& name)
{
    DWORD v = 0;
    return ReadDword(key, name.c_str(), v) && v != 0;
}

void SetFlag(const wchar_t* key, const std::wstring& name, bool value)
{
    if (value)
        WriteDword(key, name.c_str(), 1);
    else
        RegDeleteKeyValueW(HKEY_CURRENT_USER, key, name.c_str());
}
} // namespace

bool IsLocked(const std::wstring& displayKey) { return GetFlag(kLocksKey, displayKey); }
void SetLocked(const std::wstring& displayKey, bool locked) { SetFlag(kLocksKey, displayKey, locked); }

bool IsIconHidden(const std::wstring& displayKey) { return GetFlag(kHiddenKey, displayKey); }
void SetIconHidden(const std::wstring& displayKey, bool hidden) { SetFlag(kHiddenKey, displayKey, hidden); }

bool IsAutostartEnabled()
{
    wchar_t buf[MAX_PATH * 2 + 4] {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    return _wcsicmp(buf, AutostartCommand().c_str()) == 0;
}

void SetAutostartEnabled(bool enabled)
{
    if (enabled) {
        auto cmd = AutostartCommand();
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(),
                        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }
}

} // namespace settings
