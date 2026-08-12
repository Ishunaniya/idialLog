// app_settings.h — 当前用户的轻量界面设置与最近文件记录
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace dl {

struct AppSettings {
    std::vector<std::wstring> recentFiles;
    std::vector<std::wstring> searchHistory;
    std::wstring tagFilter;
    std::wstring grepFilter;
    std::wstring sinceFilter;
    std::wstring untilFilter;
    WINDOWPLACEMENT placement{};
    int lastPage = 0;
    bool filtersExpanded = false;
    bool hasPlacement = false;
};

void LoadAppSettings();
void SaveAppSettings();
AppSettings& MutableAppSettings();
const AppSettings& GetAppSettings();

void RememberRecentFiles(const std::vector<std::wstring>& paths);
void RemoveRecentFile(const std::wstring& path);
void ClearRecentFiles();
void RememberSearchQuery(const std::wstring& query);
void ClearSearchHistory();

} // namespace dl
