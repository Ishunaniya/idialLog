// overview_page.h — 总览、详情卡片与结论页
#pragma once

#include <windows.h>

namespace dl {

LRESULT CALLBACK DashProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK FindingsProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK SummaryProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

void RenderSummary();
void RenderFindings();
void ReleaseOverviewPageData();
// 概览 / 结论的 Ctrl+C 与显式复制入口共用同一份页面文本。
bool CopyOverviewPage(int page);

} // namespace dl
