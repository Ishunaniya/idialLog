// load_controller.h — 打开、粘贴、筛选、卸载与导出用例协调
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace dl {

constexpr UINT WM_APP_LOAD_PROGRESS = WM_APP + 61;
constexpr UINT WM_APP_LOAD_COMPLETE = WM_APP + 62;

void RefreshAll();
void LoadFiles(const std::vector<std::wstring>& paths);
void DoPaste();
void DoOpen();
void DoExportCsv();
void DoExportReport();
bool HandleLoadControllerMessage(UINT message, WPARAM wparam, LPARAM lparam);
bool LoadInProgress();
void CancelLoad();
bool ConsumeLoadActionClick();
void ShutdownLoadController();

} // namespace dl
