#pragma once
#include <windows.h>
#include <string>
namespace dl {
void ShowSourceMenu(HWND anchor);
void UpdateSourceControls();
bool HandleSourceCommand(UINT command);
bool CopySourceComparison();
bool SourceComparisonActive();
bool RouteSourceComparisonMessage(MSG& message);
void CloseSourceComparison();
std::wstring AnalysisSourceText();
}
