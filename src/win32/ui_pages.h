// ui_pages.h — Win32 页签、自绘视图与虚拟表生命周期
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "chart_page.h"
#include "overview_page.h"

namespace dl {
void RefreshBookmarkButton();

struct EvidenceBookmark {
    size_t lineNo = 0;
    std::wstring text;
};

void LvAddCol(HWND list, int index, const wchar_t* text, int width);

void MarkAllPagesDirty();
void ResetVirtualTables();
void ReleaseLoadedData();
void RenderPage(int page);
void ShowPage(int page);
void ReviewSelectedOutage();
// Clear edit conditions and cancel their deferred refresh as one UI operation.
void ClearMainFilters(bool refresh = true);
void SetSessionFilters(const std::string& tag, const std::string& message, const std::string& since,
                       const std::string& until);
int CurrentPage();
void JumpToRawLine(size_t lineNo);
bool ToggleEvidenceBookmark(size_t lineNo, const std::wstring& text);
bool ToggleCurrentRawBookmark();
void ClearEvidenceBookmarks();
const std::vector<EvidenceBookmark>& EvidenceBookmarks();
void ShowMetricQuickFilterMenu(HWND anchor);
void ShowMetricColumnMenu(HWND anchor);
void ApplyMetricColumnSettings();
bool ApplyMetricColumnCommand(UINT command);
void ClearMetricQuickFilters(bool refresh = true);

struct MetricFilterState {
    std::string cell, rat, channel;
    bool hasDeny = false;
    int deny = -1;
};

MetricFilterState CaptureMetricFilters();
void RestoreMetricFilters(const MetricFilterState& state);
void RebuildMetricQuickFilterView();
bool CopySelectedPageRows();
void ConfigurePageList(HWND list);
void FitPrimaryTableColumns(int contentWidth);
bool PageDetailVisible();
HWND PageDetailOwner();
void ClosePageDetail();

// 处理虚拟列表取数与页面表格自绘；返回 true 表示通知已消费。
bool HandlePageNotify(LPARAM lparam, LRESULT& result);

}  // namespace dl
