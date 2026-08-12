// app_settings.cpp — 使用 HKCU 保存界面状态；不在便携目录旁写入配置文件
#include "app_settings.h"

#include <algorithm>
#include <cwchar>
#include <vector>

namespace dl {

namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\dialLog";
constexpr size_t kMaxRecentFiles = 5;
AppSettings g_settings;

std::wstring ReadString(HKEY key, const wchar_t* name) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t)) return L"";
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, nullptr,
                         reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) return L"";
    return value.data();
}

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = fallback, bytes = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes) != ERROR_SUCCESS ||
        type != REG_DWORD || bytes != sizeof(value)) return fallback;
    return value;
}

void WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

} // namespace

void LoadAppSettings() {
    g_settings = AppSettings{};
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return;

    g_settings.tagFilter = ReadString(key, L"TagFilter");
    g_settings.grepFilter = ReadString(key, L"GrepFilter");
    g_settings.sinceFilter = ReadString(key, L"SinceFilter");
    g_settings.untilFilter = ReadString(key, L"UntilFilter");
    g_settings.lastPage = static_cast<int>(ReadDword(key, L"LastPage", 0));
    if (g_settings.lastPage < 0 || g_settings.lastPage >= 8) g_settings.lastPage = 0;
    g_settings.filtersExpanded = ReadDword(key, L"FiltersExpanded", 0) != 0;

    DWORD type = 0, bytes = sizeof(g_settings.placement);
    g_settings.placement.length = sizeof(WINDOWPLACEMENT);
    if (RegQueryValueExW(key, L"WindowPlacement", nullptr, &type,
                         reinterpret_cast<BYTE*>(&g_settings.placement), &bytes) == ERROR_SUCCESS &&
        type == REG_BINARY && bytes == sizeof(g_settings.placement) &&
        g_settings.placement.length == sizeof(WINDOWPLACEMENT)) {
        g_settings.hasPlacement = true;
    }

    type = 0; bytes = 0;
    if (RegQueryValueExW(key, L"RecentFiles", nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        type == REG_MULTI_SZ && bytes >= 2 * sizeof(wchar_t)) {
        std::vector<wchar_t> values(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, L"RecentFiles", nullptr, nullptr,
                             reinterpret_cast<BYTE*>(values.data()), &bytes) == ERROR_SUCCESS) {
            for (const wchar_t* item = values.data(); *item && g_settings.recentFiles.size() < kMaxRecentFiles;
                 item += wcslen(item) + 1) {
                g_settings.recentFiles.emplace_back(item);
            }
        }
    }
    RegCloseKey(key);
}

void SaveAppSettings() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    WriteString(key, L"TagFilter", g_settings.tagFilter);
    WriteString(key, L"GrepFilter", g_settings.grepFilter);
    WriteString(key, L"SinceFilter", g_settings.sinceFilter);
    WriteString(key, L"UntilFilter", g_settings.untilFilter);
    WriteDword(key, L"LastPage", static_cast<DWORD>(g_settings.lastPage));
    WriteDword(key, L"FiltersExpanded", g_settings.filtersExpanded ? 1 : 0);
    if (g_settings.hasPlacement) {
        RegSetValueExW(key, L"WindowPlacement", 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&g_settings.placement), sizeof(g_settings.placement));
    }

    // REG_MULTI_SZ 即使为空也以两个 NUL 结束；每个条目自身的 NUL 之外再留一个终止 NUL。
    size_t characters = 2;
    for (const auto& path : g_settings.recentFiles) characters += path.size() + 1;
    std::vector<wchar_t> recent(characters, L'\0');
    wchar_t* output = recent.data();
    for (const auto& path : g_settings.recentFiles) {
        std::copy(path.begin(), path.end(), output);
        output += path.size() + 1;
    }
    RegSetValueExW(key, L"RecentFiles", 0, REG_MULTI_SZ,
                   reinterpret_cast<const BYTE*>(recent.data()),
                   static_cast<DWORD>(recent.size() * sizeof(wchar_t)));
    RegCloseKey(key);
}

AppSettings& MutableAppSettings() { return g_settings; }
const AppSettings& GetAppSettings() { return g_settings; }

void RememberRecentFiles(const std::vector<std::wstring>& paths) {
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
        if (it->empty()) continue;
        auto found = std::find_if(g_settings.recentFiles.begin(), g_settings.recentFiles.end(),
                                  [&](const std::wstring& value) { return SamePath(value, *it); });
        if (found != g_settings.recentFiles.end()) g_settings.recentFiles.erase(found);
        g_settings.recentFiles.insert(g_settings.recentFiles.begin(), *it);
    }
    if (g_settings.recentFiles.size() > kMaxRecentFiles)
        g_settings.recentFiles.resize(kMaxRecentFiles);
    SaveAppSettings();
}

void RemoveRecentFile(const std::wstring& path) {
    g_settings.recentFiles.erase(
        std::remove_if(g_settings.recentFiles.begin(), g_settings.recentFiles.end(),
                       [&](const std::wstring& value) { return SamePath(value, path); }),
        g_settings.recentFiles.end());
    SaveAppSettings();
}

void ClearRecentFiles() {
    g_settings.recentFiles.clear();
    SaveAppSettings();
}

} // namespace dl
