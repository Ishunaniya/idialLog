// load_controller.h — 打开、粘贴、筛选、卸载与导出用例协调
#pragma once

#include <string>
#include <vector>

namespace dl {

void RefreshAll();
void LoadFiles(const std::vector<std::wstring>& paths);
void DoPaste();
void DoOpen();
void DoExportCsv();

} // namespace dl
