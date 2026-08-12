// modern_shell.h — 纯 Win32 的现代导航、按钮、状态栏与主题适配
#pragma once

#include <windows.h>

namespace dl {

constexpr UINT WM_APP_NAVIGATE = WM_APP + 41;
constexpr UINT WM_APP_SHELL_LAYOUT = WM_APP + 42;

enum class ModernButtonKind { Neutral, Primary, Danger };
enum class ModernNoticeKind { Info, Success, Warning, Error };

bool RegisterModernShellClasses(HINSTANCE instance);
HWND CreateModernNavigation(HWND parent, int id);
HWND CreateModernStatus(HWND parent, int id);

void ConfigureModernButton(HWND button, ModernButtonKind kind = ModernButtonKind::Neutral);
void SetModernButtonActive(HWND button, bool active);
bool DrawModernButton(const DRAWITEMSTRUCT& item);

void SetNavigationPage(int page);
void RefreshNavigation();

void ShowModernNotice(const wchar_t* title, const wchar_t* detail,
                      ModernNoticeKind kind = ModernNoticeKind::Info, UINT durationMs = 5000);
void LayoutModernOverlays();
void SetShellBusy(bool busy, const wchar_t* text = nullptr);
void SetShellProgress(int percent, const wchar_t* text = nullptr);
bool ShellBusy();

bool SystemPrefersDarkTheme();
void ApplyModernTheme(HWND root);
HBRUSH ModernControlBrush(UINT message, HDC dc, HWND control);

void FillSolid(HDC dc, const RECT& rect, COLORREF color);
void FillRound(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border);

} // namespace dl
