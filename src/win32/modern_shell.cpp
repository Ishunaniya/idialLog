// modern_shell.cpp — 不引入 Windows App SDK 的 Fluent 风格 Win32 外壳
#include "modern_shell.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <string>

#include "app_context.h"
#include "theme.h"
#include "version.h"

namespace dl {
namespace {

constexpr wchar_t kNavigationClass[] = L"dialModernNavigation";
constexpr wchar_t kStatusClass[] = L"dialModernStatus";
constexpr wchar_t kNoticeClass[] = L"dialModernNotice";
constexpr wchar_t kButtonHoverProp[] = L"dialModernButtonHover";
constexpr wchar_t kButtonKindProp[] = L"dialModernButtonKind";
constexpr wchar_t kButtonActiveProp[] = L"dialModernButtonActive";

HWND g_navigation = nullptr;
int g_selectedPage = 0;
int g_hoverPage = -1;
HBRUSH g_surfaceBrush = nullptr;
COLORREF g_surfaceBrushColor = CLR_INVALID;
HWND g_notice = nullptr;
std::wstring g_noticeTitle;
std::wstring g_noticeDetail;
ModernNoticeKind g_noticeKind = ModernNoticeKind::Info;
bool g_busy = false;
bool g_noticeVisible = false;
std::wstring g_statusBeforeBusy;
std::wstring g_busyStatus;

struct NavItem { int page; const wchar_t* text; int y; };

std::array<NavItem, 8> NavItems() {
    return {{{0, L"概览", 112}, {1, L"诊断结论", 152}, {3, L"断网记录", 192},
             {2, L"事件时间线", 272}, {4, L"信号指标", 312}, {5, L"标签统计", 352},
             {6, L"原始日志", 392}, {7, L"未识别行", 432}}};
}

void DrawTextAt(HDC dc, const wchar_t* text, RECT rect, HFONT font, COLORREF color,
                UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, &rect, format);
    SelectObject(dc, old);
}

void DrawNavIcon(HDC dc, int page, int x, int y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    const int l = x + S(3), t = y + S(3), r = x + S(17), b = y + S(17);
    switch (page) {
    case 0:
        MoveToEx(dc, l, y + S(10), nullptr); LineTo(dc, x + S(10), t); LineTo(dc, r, y + S(10));
        Rectangle(dc, x + S(5), y + S(9), x + S(16), b); break;
    case 1:
        MoveToEx(dc, x + S(10), t, nullptr); LineTo(dc, r, b); LineTo(dc, l, b); LineTo(dc, x + S(10), t);
        MoveToEx(dc, x + S(10), y + S(7), nullptr); LineTo(dc, x + S(10), y + S(12));
        Ellipse(dc, x + S(9), y + S(14), x + S(11), y + S(16)); break;
    case 2:
        Ellipse(dc, l, t, r, b); MoveToEx(dc, x + S(10), y + S(6), nullptr);
        LineTo(dc, x + S(10), y + S(11)); LineTo(dc, x + S(14), y + S(13)); break;
    case 3:
        MoveToEx(dc, l, y + S(6), nullptr); LineTo(dc, r, y + S(6));
        MoveToEx(dc, l, y + S(10), nullptr); LineTo(dc, x + S(13), y + S(10));
        MoveToEx(dc, l, y + S(14), nullptr); LineTo(dc, r, y + S(14)); break;
    case 4:
        MoveToEx(dc, l, b, nullptr); LineTo(dc, l, t);
        MoveToEx(dc, l, b, nullptr); LineTo(dc, r, b);
        MoveToEx(dc, x + S(5), y + S(13), nullptr); LineTo(dc, x + S(9), y + S(9));
        LineTo(dc, x + S(12), y + S(12)); LineTo(dc, x + S(17), y + S(5)); break;
    case 5:
        MoveToEx(dc, l, y + S(6), nullptr); LineTo(dc, x + S(12), t); LineTo(dc, r, y + S(8));
        LineTo(dc, x + S(10), b); LineTo(dc, l, y + S(12)); LineTo(dc, l, y + S(6));
        Ellipse(dc, x + S(7), y + S(7), x + S(10), y + S(10)); break;
    case 6:
        Rectangle(dc, x + S(5), t, x + S(16), b);
        MoveToEx(dc, x + S(8), y + S(8), nullptr); LineTo(dc, x + S(14), y + S(8));
        MoveToEx(dc, x + S(8), y + S(12), nullptr); LineTo(dc, x + S(14), y + S(12)); break;
    default:
        Ellipse(dc, l, t, r, b); MoveToEx(dc, x + S(8), y + S(8), nullptr);
        LineTo(dc, x + S(10), y + S(6)); LineTo(dc, x + S(13), y + S(8));
        LineTo(dc, x + S(10), y + S(11)); LineTo(dc, x + S(10), y + S(13));
        Ellipse(dc, x + S(9), y + S(15), x + S(11), y + S(17)); break;
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

std::wstring BadgeForPage(int page) {
    size_t count = 0;
    if (page == 1) count = App().document.findings.size();
    else if (page == 3) count = App().document.outages.size();
    else if (page == 7) count = App().document.audit.unparsed;
    if (!count) return {};
    return count > 999 ? L"999+" : std::to_wstring(count);
}

int HitNavigationPage(int y) {
    for (const auto& item : NavItems())
        if (y >= S(item.y) && y < S(item.y + 36)) return item.page;
    return -1;
}

LRESULT CALLBACK NavigationProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        const int page = HitNavigationPage(GET_Y_LPARAM(lparam));
        if (page != g_hoverPage) { g_hoverPage = page; InvalidateRect(hwnd, nullptr, FALSE); }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hoverPage = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONUP: {
        const int page = HitNavigationPage(GET_Y_LPARAM(lparam));
        if (page >= 0) SendMessageW(GetParent(hwnd), WM_APP_NAVIGATE, page, 0);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{}; GetClientRect(hwnd, &client);
        FillSolid(dc, client, th::nav);

        RECT logo{S(20), S(18), S(52), S(50)};
        FillRound(dc, logo, S(9), th::accent, th::accent);
        DrawTextAt(dc, L"dL", logo, App().hFontSect, th::onAccent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT brand{S(62), S(15), client.right - S(12), S(39)};
        DrawTextAt(dc, L"dialLog", brand, App().hFontSect, th::inkPri);
        RECT sub{S(62), S(36), client.right - S(12), S(58)};
        DrawTextAt(dc, L"日志诊断工作台", sub, App().hFontSmall, th::inkMuted);

        RECT group1{S(20), S(83), client.right - S(16), S(105)};
        DrawTextAt(dc, L"工作台", group1, App().hFontSmall, th::inkMuted);
        RECT group2{S(20), S(243), client.right - S(16), S(265)};
        DrawTextAt(dc, L"数据", group2, App().hFontSmall, th::inkMuted);

        for (const auto& item : NavItems()) {
            RECT row{S(10), S(item.y), client.right - S(10), S(item.y + 36)};
            const bool selected = item.page == g_selectedPage;
            const bool hovered = item.page == g_hoverPage;
            if (selected || hovered)
                FillRound(dc, row, S(7), selected ? th::accentSoft : th::hover,
                          selected ? th::accentSoft : th::hover);
            if (selected) {
                RECT mark{row.left, row.top + S(8), row.left + S(3), row.bottom - S(8)};
                FillRound(dc, mark, S(2), th::accent, th::accent);
            }
            DrawNavIcon(dc, item.page, S(20), S(item.y + 8), selected ? th::accent : th::inkSec);
            RECT label{S(50), row.top, client.right - S(44), row.bottom};
            DrawTextAt(dc, item.text, label, selected ? App().hFontSect : App().hFontUI,
                       selected ? th::inkPri : th::inkSec);
            std::wstring badge = BadgeForPage(item.page);
            if (!badge.empty()) {
                RECT br{client.right - S(46), row.top + S(8), client.right - S(18), row.bottom - S(8)};
                if (badge.size() > 2) br.left -= S(8);
                FillRound(dc, br, S(9), selected ? th::accent : th::surface,
                          selected ? th::accent : th::border);
                DrawTextAt(dc, badge.c_str(), br, App().hFontSmall,
                           selected ? th::onAccent : th::inkSec, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }

        RECT version{S(20), client.bottom - S(42), client.right - S(12), client.bottom - S(16)};
        DrawTextAt(dc, L"v" DL_VER_WSTR L"  ·  本地离线分析", version, App().hFontSmall, th::inkMuted);
        HPEN sep = CreatePen(PS_SOLID, 1, th::border);
        HGDIOBJ old = SelectObject(dc, sep);
        MoveToEx(dc, client.right - 1, 0, nullptr); LineTo(dc, client.right - 1, client.bottom);
        SelectObject(dc, old); DeleteObject(sep);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK StatusProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SETTEXT) {
        const LRESULT result = DefWindowProcW(hwnd, message, wparam, lparam);
        InvalidateRect(hwnd, nullptr, FALSE);
        return result;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        RECT client{}; GetClientRect(hwnd, &client);
        FillSolid(dc, client, th::surface);
        HPEN pen = CreatePen(PS_SOLID, 1, th::border);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, 0, 0, nullptr); LineTo(dc, client.right, 0);
        SelectObject(dc, old); DeleteObject(pen);
        RECT dot{S(14), S(12), S(20), S(18)};
        COLORREF dotColor = g_busy ? th::accent : th::good;
        FillRound(dc, dot, S(3), dotColor, dotColor);
        wchar_t text[2048]{}; GetWindowTextW(hwnd, text, 2048);
        RECT tr{S(28), 0, client.right - S(12), client.bottom};
        DrawTextAt(dc, text, tr, App().hFontSmall, th::inkSec);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

COLORREF NoticeColor() {
    switch (g_noticeKind) {
    case ModernNoticeKind::Success: return th::good;
    case ModernNoticeKind::Warning: return th::warning;
    case ModernNoticeKind::Error: return th::critical;
    default: return th::accent;
    }
}

LRESULT CALLBACK NoticeProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        KillTimer(hwnd, 1); g_noticeVisible = false; ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_LBUTTONUP:
        KillTimer(hwnd, 1); g_noticeVisible = false; ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        RECT client{}; GetClientRect(hwnd, &client);
        FillSolid(dc, client, th::surface);
        FillRound(dc, client, S(10), th::surface, th::border);
        RECT band{0, 0, S(5), client.bottom};
        FillRound(dc, band, S(3), NoticeColor(), NoticeColor());
        RECT title{S(20), S(10), client.right - S(30), S(34)};
        DrawTextAt(dc, g_noticeTitle.c_str(), title, App().hFontSect, th::inkPri);
        RECT detail{S(20), S(34), client.right - S(18), client.bottom - S(9)};
        DrawTextAt(dc, g_noticeDetail.c_str(), detail, App().hFontSmall, th::inkSec,
                   DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        RECT close{client.right - S(26), S(8), client.right - S(8), S(26)};
        DrawTextAt(dc, L"×", close, App().hFontUI, th::inkMuted,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps); return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK ButtonSubclass(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_MOUSEMOVE:
        if (!GetPropW(hwnd, kButtonHoverProp)) {
            SetPropW(hwnd, kButtonHoverProp, reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking); InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(hwnd, kButtonHoverProp); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        InvalidateRect(hwnd, nullptr, FALSE); break;
    case WM_NCDESTROY:
        RemovePropW(hwnd, kButtonHoverProp); RemovePropW(hwnd, kButtonKindProp);
        RemovePropW(hwnd, kButtonActiveProp);
        RemoveWindowSubclass(hwnd, ButtonSubclass, 1); break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void RefreshSurfaceBrush() {
    if (g_surfaceBrush && g_surfaceBrushColor == th::surface) return;
    if (g_surfaceBrush) DeleteObject(g_surfaceBrush);
    g_surfaceBrush = CreateSolidBrush(th::surface);
    g_surfaceBrushColor = th::surface;
}

void ApplyThemeToControl(HWND control) {
    if (!control) return;
    wchar_t cls[64]{}; GetClassNameW(control, cls, 64);
    if (lstrcmpiW(cls, WC_LISTVIEWW) == 0) {
        SetWindowTheme(control, th::dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        ListView_SetBkColor(control, th::surface);
        ListView_SetTextBkColor(control, th::surface);
        ListView_SetTextColor(control, th::inkPri);
        if (HWND header = ListView_GetHeader(control)) {
            SetWindowTheme(header, th::dark ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
            SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontUI), TRUE);
        }
    } else if (lstrcmpiW(cls, L"Edit") == 0) {
        SetWindowTheme(control, th::dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
    }
    InvalidateRect(control, nullptr, TRUE);
}

} // namespace

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush);
}

void FillRound(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
    DeleteObject(brush); DeleteObject(pen);
}

bool RegisterModernShellClasses(HINSTANCE instance) {
    WNDCLASSEXW nav{}; nav.cbSize = sizeof(nav); nav.lpfnWndProc = NavigationProc;
    nav.hInstance = instance; nav.hCursor = LoadCursorW(nullptr, IDC_ARROW); nav.lpszClassName = kNavigationClass;
    WNDCLASSEXW status{}; status.cbSize = sizeof(status); status.lpfnWndProc = StatusProc;
    status.hInstance = instance; status.hCursor = LoadCursorW(nullptr, IDC_ARROW); status.lpszClassName = kStatusClass;
    WNDCLASSEXW notice{}; notice.cbSize = sizeof(notice); notice.lpfnWndProc = NoticeProc;
    notice.hInstance = instance; notice.hCursor = LoadCursorW(nullptr, IDC_ARROW); notice.lpszClassName = kNoticeClass;
    return RegisterClassExW(&nav) && RegisterClassExW(&status) && RegisterClassExW(&notice);
}

HWND CreateModernNavigation(HWND parent, int id) {
    g_navigation = CreateWindowExW(0, kNavigationClass, L"", WS_CHILD | WS_VISIBLE,
                                   0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    return g_navigation;
}

HWND CreateModernStatus(HWND parent, int id) {
    return CreateWindowExW(0, kStatusClass, L"就绪。拖入日志文件，或使用“打开日志”。",
                           WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

void ConfigureModernButton(HWND button, ModernButtonKind kind) {
    SetPropW(button, kButtonKindProp,
             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(static_cast<int>(kind) + 1)));
    SetWindowSubclass(button, ButtonSubclass, 1, 0);
}

void SetModernButtonActive(HWND button, bool active) {
    if (!button) return;
    if (active) SetPropW(button, kButtonActiveProp, reinterpret_cast<HANDLE>(1));
    else RemovePropW(button, kButtonActiveProp);
    InvalidateRect(button, nullptr, FALSE);
}

bool DrawModernButton(const DRAWITEMSTRUCT& item) {
    if (item.CtlType != ODT_BUTTON) return false;
    const auto stored = reinterpret_cast<INT_PTR>(GetPropW(item.hwndItem, kButtonKindProp));
    const ModernButtonKind kind = stored ? static_cast<ModernButtonKind>(stored - 1) : ModernButtonKind::Neutral;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool hovered = GetPropW(item.hwndItem, kButtonHoverProp) != nullptr;
    const bool active = GetPropW(item.hwndItem, kButtonActiveProp) != nullptr;
    COLORREF fill = th::surface, border = th::border, text = th::inkPri;
    if (kind == ModernButtonKind::Primary) {
        fill = (pressed || hovered) ? th::accentHover : th::accent;
        border = fill; text = th::onAccent;
    } else if (kind == ModernButtonKind::Danger && hovered) {
        fill = th::dark ? HEX2RGB(0x49282c) : HEX2RGB(0xffe8e8); border = th::critical; text = th::critical;
    } else if (pressed || hovered || active) {
        fill = active ? th::accentSoft : (pressed ? th::accentSoft : th::hover);
        border = (active || pressed) ? th::accent : th::border;
        if (active) text = th::accent;
    }
    if (disabled) text = th::inkMuted;
    RECT r = item.rcItem; InflateRect(&r, -1, -1);
    FillRound(item.hDC, r, S(7), fill, border);
    wchar_t label[128]{}; GetWindowTextW(item.hwndItem, label, 128);
    DrawTextAt(item.hDC, label, r, App().hFontUI, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = r; InflateRect(&focus, -S(3), -S(3));
        DrawFocusRect(item.hDC, &focus);
    }
    return true;
}

void SetNavigationPage(int page) {
    g_selectedPage = page;
    if (g_navigation) InvalidateRect(g_navigation, nullptr, FALSE);
}

void RefreshNavigation() {
    if (g_navigation) InvalidateRect(g_navigation, nullptr, FALSE);
}

void LayoutModernOverlays() {
    if (!g_notice || !App().hMain || !g_noticeVisible) return;
    RECT client{}; GetClientRect(App().hMain, &client);
    const int width = std::min(S(380), std::max(S(280), static_cast<int>(client.right) - S(250)));
    const int height = S(78);
    POINT position{client.right - width - S(22), client.bottom - S(32) - height - S(18)};
    ClientToScreen(App().hMain, &position);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, S(20), S(20));
    if (!SetWindowRgn(g_notice, region, FALSE)) DeleteObject(region);
    SetWindowPos(g_notice, HWND_TOP, position.x, position.y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void ShowModernNotice(const wchar_t* title, const wchar_t* detail,
                      ModernNoticeKind kind, UINT durationMs) {
    if (!App().hMain) return;
    if (!g_notice) {
        g_notice = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kNoticeClass, L"", WS_POPUP,
            0, 0, 10, 10, App().hMain, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    g_noticeTitle = title ? title : L"";
    g_noticeDetail = detail ? detail : L"";
    g_noticeKind = kind;
    g_noticeVisible = true;
    KillTimer(g_notice, 1);
    LayoutModernOverlays();
    SetWindowPos(g_notice, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_notice, nullptr, TRUE);
    if (durationMs) SetTimer(g_notice, 1, durationMs, nullptr);
}

void SetShellBusy(bool busy, const wchar_t* text) {
    if (busy && !g_busy && App().hStatus) {
        wchar_t previous[2048]{};
        GetWindowTextW(App().hStatus, previous, 2048);
        g_statusBeforeBusy = previous;
    }
    g_busy = busy;
    for (HWND button : {App().hOpen, App().hPaste, App().hCloseLog, App().hExport,
                         App().hApplyFilter, App().hClearFilter})
        if (button) EnableWindow(button, !busy);
    if (busy && text && App().hStatus) {
        g_busyStatus = text;
        SetWindowTextW(App().hStatus, text);
    } else if (!busy && App().hStatus && !g_busyStatus.empty()) {
        wchar_t current[2048]{};
        GetWindowTextW(App().hStatus, current, 2048);
        if (g_busyStatus == current) SetWindowTextW(App().hStatus, g_statusBeforeBusy.c_str());
        g_busyStatus.clear(); g_statusBeforeBusy.clear();
    }
    if (App().hStatus) {
        RedrawWindow(App().hStatus, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    SetCursor(LoadCursorW(nullptr, busy ? IDC_WAIT : IDC_ARROW));
}

bool ShellBusy() { return g_busy; }

bool SystemPrefersDarkTheme() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) return false;
    DWORD useLight = 1, size = sizeof(useLight);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &useLight, &size) != ERROR_SUCCESS)
        return false;
    return useLight == 0;
}

void ApplyModernTheme(HWND root) {
    th::ApplyPalette(SystemPrefersDarkTheme());
    RefreshSurfaceBrush();
    BOOL darkTitle = th::dark ? TRUE : FALSE;
    DwmSetWindowAttribute(root, 20, &darkTitle, sizeof(darkTitle));
    int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(root, 33, &corner, sizeof(corner));

    for (HWND control = GetWindow(root, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT))
        ApplyThemeToControl(control);
    if (g_notice) InvalidateRect(g_notice, nullptr, TRUE);
    RefreshNavigation();
    InvalidateRect(root, nullptr, TRUE);
}

HBRUSH ModernControlBrush(UINT message, HDC dc, HWND) {
    RefreshSurfaceBrush();
    SetTextColor(dc, th::inkPri);
    if (message == WM_CTLCOLORSTATIC) {
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, th::surface);
        return g_surfaceBrush;
    }
    SetBkColor(dc, th::surface);
    return g_surfaceBrush;
}

} // namespace dl
