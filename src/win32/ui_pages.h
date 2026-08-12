// ui_pages.h — Win32 页签、自绘视图与虚拟表生命周期
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "chart_page.h"
#include "overview_page.h"

namespace dl {

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
int CurrentPage();
void JumpToRawLine(size_t lineNo);
bool ToggleEvidenceBookmark(size_t lineNo, const std::wstring& text);
bool ToggleCurrentRawBookmark();
void ClearEvidenceBookmarks();
const std::vector<EvidenceBookmark>& EvidenceBookmarks();

// 处理虚拟列表取数与页面表格自绘；返回 true 表示通知已消费。
bool HandlePageNotify(LPARAM lparam, LRESULT& result);

} // namespace dl
