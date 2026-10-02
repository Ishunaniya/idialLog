// app_settings.cpp — 使用 HKCU 保存界面状态；不在便携目录旁写入配置文件
#include "app_settings.h"
#include "text_catalog.h"

#include <algorithm>
#include <cwchar>
#include <vector>

namespace dl {

namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\dialLog";
constexpr size_t kMaxRecentFiles = 5;
constexpr size_t kMaxSearchHistory = 8;
AppSettings g_settings;

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = fallback, bytes = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes) != ERROR_SUCCESS ||
        type != REG_DWORD || bytes != sizeof(value)) return fallback;
    return value;
}

void WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

std::wstring ReadFont(HKEY key, const wchar_t* name, const std::wstring& fallback) {
    wchar_t face[LF_FACESIZE]{};
    DWORD type = 0, bytes = sizeof(face);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(face), &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t) || bytes > sizeof(face) ||
        bytes % sizeof(wchar_t) || face[bytes / sizeof(wchar_t) - 1] != L'\0' || !face[0]) return fallback;
    return face;
}

void WriteFont(HKEY key, const wchar_t* name, const std::wstring& face) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(face.c_str()),
                   static_cast<DWORD>((face.size() + 1) * sizeof(wchar_t)));
}

std::vector<std::wstring> ReadMultiString(HKEY key, const wchar_t* name, size_t limit) {
    std::vector<std::wstring> result;
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_MULTI_SZ || bytes < 2 * sizeof(wchar_t)) return result;
    std::vector<wchar_t> values(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, nullptr,
                         reinterpret_cast<BYTE*>(values.data()), &bytes) != ERROR_SUCCESS) return result;
    for (const wchar_t* item = values.data(); *item && result.size() < limit;
         item += wcslen(item) + 1) result.emplace_back(item);
    return result;
}

void WriteMultiString(HKEY key, const wchar_t* name, const std::vector<std::wstring>& values) {
    size_t characters = 2;
    for (const auto& value : values) characters += value.size() + 1;
    std::vector<wchar_t> packed(characters, L'\0');
    wchar_t* output = packed.data();
    for (const auto& value : values) {
        std::copy(value.begin(), value.end(), output);
        output += value.size() + 1;
    }
    RegSetValueExW(key, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(packed.data()),
                   static_cast<DWORD>(packed.size() * sizeof(wchar_t)));
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

} // namespace

void LoadAppSettings() {
    g_settings = AppSettings{};
    SetEnglish(false);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return;
    g_settings.uiFont = ReadFont(key, L"UiFont", g_settings.uiFont);
    g_settings.logFont = ReadFont(key, L"LogFont", g_settings.logFont);

    g_settings.english = ReadDword(key, L"English", 0) == 1;
    SetEnglish(g_settings.english);
    g_settings.metricColumns = ReadDword(key, L"MetricColumns", (1u << 22) - 1) & ((1u << 22) - 1);
    g_settings.metricColumns |= 1u; // Timestamp is mandatory.

    // 筛选是一次分析的临时范围，跨会话恢复会把新日志静默筛成空页，故不再读取旧值。
    g_settings.lastPage = static_cast<int>(ReadDword(key, L"LastPage", 0));
    if (g_settings.lastPage < 0 || g_settings.lastPage >= 9) g_settings.lastPage = 0;
    g_settings.filtersExpanded = ReadDword(key, L"FiltersExpanded", 0) != 0;

    DWORD type = 0, bytes = sizeof(g_settings.placement);
    g_settings.placement.length = sizeof(WINDOWPLACEMENT);
    if (RegQueryValueExW(key, L"WindowPlacement", nullptr, &type,
                         reinterpret_cast<BYTE*>(&g_settings.placement), &bytes) == ERROR_SUCCESS &&
        type == REG_BINARY && bytes == sizeof(g_settings.placement) &&
        g_settings.placement.length == sizeof(WINDOWPLACEMENT)) {
        g_settings.hasPlacement = true;
    }

    g_settings.recentFiles = ReadMultiString(key, L"RecentFiles", kMaxRecentFiles);
    g_settings.searchHistory = ReadMultiString(key, L"SearchHistory", kMaxSearchHistory);
    RegCloseKey(key);
}

void SaveAppSettings() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    // 清理旧版本留下的值，避免用户升级后首开仍被隐藏筛选影响。
    WriteFont(key, L"UiFont", g_settings.uiFont);
    WriteFont(key, L"LogFont", g_settings.logFont);
    WriteDword(key, L"English", g_settings.english ? 1 : 0);
    WriteDword(key, L"MetricColumns", g_settings.metricColumns);
    RegDeleteValueW(key, L"TagFilter");
    RegDeleteValueW(key, L"GrepFilter");
    RegDeleteValueW(key, L"SinceFilter");
    RegDeleteValueW(key, L"UntilFilter");
    WriteDword(key, L"LastPage", static_cast<DWORD>(g_settings.lastPage));
    WriteDword(key, L"FiltersExpanded", g_settings.filtersExpanded ? 1 : 0);
    if (g_settings.hasPlacement) {
        RegSetValueExW(key, L"WindowPlacement", 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&g_settings.placement), sizeof(g_settings.placement));
    }

    WriteMultiString(key, L"RecentFiles", g_settings.recentFiles);
    WriteMultiString(key, L"SearchHistory", g_settings.searchHistory);
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

void RememberSearchQuery(const std::wstring& query) {
    if (query.empty()) return;
    auto found = std::find(g_settings.searchHistory.begin(), g_settings.searchHistory.end(), query);
    if (found != g_settings.searchHistory.end()) g_settings.searchHistory.erase(found);
    g_settings.searchHistory.insert(g_settings.searchHistory.begin(), query);
    if (g_settings.searchHistory.size() > kMaxSearchHistory)
        g_settings.searchHistory.resize(kMaxSearchHistory);
    SaveAppSettings();
}

void ClearSearchHistory() {
    g_settings.searchHistory.clear();
    SaveAppSettings();
}

} // namespace dl
