// Main window caption: extend only the navigation background to the window top.
#pragma once
#include <windows.h>

namespace dl {
void CreateMainFrame(HWND window);
void UpdateMainFrame(HWND window, int navigationWidth);
int MainCaptionHeight();
int MainBodyInset();
bool HandleMainFrame(HWND window, UINT message, WPARAM wp, LPARAM lp, LRESULT& result);
}  // namespace dl
