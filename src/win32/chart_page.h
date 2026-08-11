// chart_page.h — 信号图与指标页模型
#pragma once

#include <windows.h>

namespace dl {

LRESULT CALLBACK ChartProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

void RenderMetrics();
void ReleaseChartPageData();

} // namespace dl
