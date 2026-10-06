#include "main_frame.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>

#include "app_context.h"
#include "modern_shell.h"
#include "text_catalog.h"
#include "theme.h"
#include "win_text.h"

namespace dl {
namespace {
constexpr wchar_t kCaptionClass[] = L"dialMainCaption";
constexpr int kMinimize = 39001, kMaximize = 39002, kClose = 39003;
HWND g_caption = nullptr;
std::array<HWND, 3> g_buttons{};
bool g_custom = false;
int g_navigationWidth = 0;

int FrameInset() {
    using MetricsFn = int(WINAPI*)(int, UINT);
    const auto user = GetModuleHandleW(L"user32.dll");
    const auto metrics =
        user ? reinterpret_cast<MetricsFn>(reinterpret_cast<void*>(GetProcAddress(user, "GetSystemMetricsForDpi")))
             : nullptr;
    if (metrics)
        return metrics(SM_CXSIZEFRAME, App().dpi) + metrics(SM_CXPADDEDBORDER, App().dpi);
    return std::max(1, MulDiv(GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER), App().dpi, 96));
}

LRESULT CALLBACK CaptionButtonProc(HWND button, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    // Let the main window expose HTMAXBUTTON so Windows can recognize its
    // maximize target (including the Windows 11 snap-layout entry point).
    if (message == WM_NCHITTEST && GetDlgCtrlID(button) == kMaximize)
        return HTTRANSPARENT;
    if (message == WM_LBUTTONDOWN) {
        HWND focus = GetFocus();
        if (focus && focus != button && IsChild(App().hMain, focus))
            SetPropW(button, L"dialCaptionPreviousFocus", focus);
    }
    if (message == WM_MOUSEMOVE && !GetPropW(button, L"dialCaptionHover")) {
        SetPropW(button, L"dialCaptionHover", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, button, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(button, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE) {
        RemovePropW(button, L"dialCaptionHover");
        InvalidateRect(button, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemovePropW(button, L"dialCaptionHover");
        RemovePropW(button, L"dialCaptionPreviousFocus");
        RemoveWindowSubclass(button, CaptionButtonProc, 1);
    }

    return DefSubclassProc(button, message, wp, lp);
}

void DrawCaptionButton(const DRAWITEMSTRUCT& item) {
    const bool hover = GetPropW(item.hwndItem, L"dialCaptionHover") || (item.itemState & ODS_SELECTED);
    const bool close = item.CtlID == kClose;
    FillSolid(item.hDC, item.rcItem, hover ? (close ? th::critical : th::hover) : th::surface);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), hover && close ? th::onAccent : th::inkPri);
    auto oldPen = SelectObject(item.hDC, pen);
    auto oldBrush = SelectObject(item.hDC, GetStockObject(HOLLOW_BRUSH));
    const int cx = (item.rcItem.left + item.rcItem.right) / 2;
    const int cy = (item.rcItem.top + item.rcItem.bottom) / 2;
    const int half = S(5);
    if (item.CtlID == kMinimize) {
        MoveToEx(item.hDC, cx - half, cy, nullptr);
        LineTo(item.hDC, cx + half + 1, cy);
    } else if (close) {
        MoveToEx(item.hDC, cx - half, cy - half, nullptr);
        LineTo(item.hDC, cx + half + 1, cy + half + 1);
        MoveToEx(item.hDC, cx + half, cy - half, nullptr);
        LineTo(item.hDC, cx - half - 1, cy + half + 1);
    } else if (IsZoomed(App().hMain)) {
        Rectangle(item.hDC, cx - half + S(3), cy - half, cx + half + S(1), cy + half - S(2));
        RECT front{cx - half, cy - half + S(3), cx + half - S(2), cy + half + S(1)};
        FillSolid(item.hDC, front, hover ? th::hover : th::surface);
        Rectangle(item.hDC, front.left, front.top, front.right, front.bottom);
    } else {
        Rectangle(item.hDC, cx - half, cy - half, cx + half + 1, cy + half + 1);
    }

    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(pen);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -S(4), -S(4));
        DrawFocusRect(item.hDC, &focus);
    }
}

LRESULT CALLBACK CaptionProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;  // The parent supplies caption dragging and edge resizing.
    case WM_ERASEBKGND:
        return 1;
    case WM_COMMAND:
        PostMessageW(GetParent(window), WM_COMMAND, wp, lp);
        return 0;
    case WM_DRAWITEM:
        DrawCaptionButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillSolid(dc, client, th::surface);
        RECT left{0, 0, std::min(g_navigationWidth, static_cast<int>(client.right)), client.bottom};
        FillSolid(dc, left, th::nav);
        HICON icon = reinterpret_cast<HICON>(GetClassLongPtrW(GetParent(window), GCLP_HICONSM));
        const int size = S(16);
        if (icon)
            DrawIconEx(dc, S(10), (client.bottom - size) / 2, icon, size, size, 0, nullptr, DI_NORMAL);
        RECT title{S(34), 0, client.right - S(46 * 3 + 8), client.bottom};
        auto old = SelectObject(dc, App().hFontSmall);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetActiveWindow() == App().hMain ? th::inkPri : th::inkSec);
        const auto text = GetText(GetParent(window));
        DrawTextW(dc, text.c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, old);
        EndPaint(window, &paint);
        return 0;
    }
    }

    return DefWindowProcW(window, message, wp, lp);
}
}  // namespace

int MainCaptionHeight() {
    return g_custom ? S(32) : 0;
}

int MainBodyInset() {
    return g_custom && !IsZoomed(App().hMain) ? FrameInset() : 0;
}

void CreateMainFrame(HWND window) {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = CaptionProc;
    cls.hInstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kCaptionClass;
    RegisterClassExW(&cls);
    g_caption = CreateWindowExW(0, kCaptionClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, window,
                                nullptr, cls.hInstance, nullptr);
    for (size_t index = 0; index < g_buttons.size(); ++index) {
        g_buttons[index] =
            CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 1, 1, g_caption,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMinimize + index)), cls.hInstance, nullptr);
        SetWindowSubclass(g_buttons[index], CaptionButtonProc, 1, 0);
    }

    UpdateMainFrame(window, S(232));
}

void UpdateMainFrame(HWND window, int navigationWidth) {
    if (!g_caption)
        return;
    const bool custom = !SystemHighContrastEnabled() &&
                        std::all_of(g_buttons.begin(), g_buttons.end(), [](HWND button) { return button != nullptr; });
    if (custom != g_custom) {
        g_custom = custom;
        const DWMNCRENDERINGPOLICY policy = custom ? DWMNCRP_DISABLED : DWMNCRP_USEWINDOWSTYLE;
        DwmSetWindowAttribute(window, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        ShowWindow(g_caption, custom ? SW_SHOW : SW_HIDE);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    if (!custom)
        return;
    g_navigationWidth = navigationWidth + MainBodyInset();
    RECT client{};
    GetClientRect(window, &client);
    const int height = MainCaptionHeight(), buttonWidth = S(46);
    MoveWindow(g_caption, 0, 0, client.right, height, TRUE);
    for (size_t index = 0; index < g_buttons.size(); ++index)
        MoveWindow(g_buttons[index], client.right - buttonWidth * (3 - static_cast<int>(index)), 0, buttonWidth, height,
                   TRUE);
    SetWindowTextW(g_buttons[0], UiText(TextId::window_minimize));
    SetWindowTextW(g_buttons[1], UiText(IsZoomed(window) ? TextId::window_restore : TextId::window_maximize));
    SetWindowTextW(g_buttons[2], UiText(TextId::window_close));
    RedrawWindow(g_caption, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

bool HandleMainFrame(HWND window, UINT message, WPARAM wp, LPARAM lp, LRESULT& result) {
    if (message == WM_COMMAND && LOWORD(wp) >= kMinimize && LOWORD(wp) <= kClose && HIWORD(wp) == BN_CLICKED &&
        reinterpret_cast<HWND>(lp) == g_buttons[LOWORD(wp) - kMinimize]) {
        const int id = LOWORD(wp);
        HWND button = reinterpret_cast<HWND>(lp);
        HWND focus = reinterpret_cast<HWND>(RemovePropW(button, L"dialCaptionPreviousFocus"));
        if (focus && GetFocus() == button && IsWindow(focus) && IsChild(window, focus))
            SetFocus(focus);
        const UINT command = id == kMinimize    ? SC_MINIMIZE
                             : id == kClose     ? SC_CLOSE
                             : IsZoomed(window) ? SC_RESTORE
                                                : SC_MAXIMIZE;
        PostMessageW(window, WM_SYSCOMMAND, command, 0);
        result = 0;
        return true;
    }

    if (!g_custom)
        return false;
    if (message == WM_NCCALCSIZE) {
        RECT& rect = wp ? reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0] : *reinterpret_cast<RECT*>(lp);
        // Maximized overlapped windows include invisible resize borders outside
        // the work area. Restored windows use a fully client-drawn caption;
        // MainBodyInset preserves the original body's width below that caption.
        if (IsZoomed(window)) {
            const int inset = FrameInset();
            InflateRect(&rect, -inset, -inset);
        }

        result = 0;
        return true;
    }
    if (message == WM_NCHITTEST) {
        RECT bounds{};
        GetWindowRect(window, &bounds);
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!IsZoomed(window)) {
            const int border = FrameInset();
            const bool left = point.x < bounds.left + border, right = point.x >= bounds.right - border;
            const bool top = point.y < bounds.top + S(5), bottom = point.y >= bounds.bottom - border;
            if (top || bottom || left || right) {
                result = top      ? (left    ? HTTOPLEFT
                                     : right ? HTTOPRIGHT
                                             : HTTOP)
                         : bottom ? (left    ? HTBOTTOMLEFT
                                     : right ? HTBOTTOMRIGHT
                                             : HTBOTTOM)
                         : left   ? HTLEFT
                                  : HTRIGHT;
                return true;
            }
        }

        ScreenToClient(window, &point);
        if (point.y >= 0 && point.y < MainCaptionHeight()) {
            RECT maximize{};
            GetWindowRect(g_buttons[1], &maximize);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&maximize), 2);
            result = PtInRect(&maximize, point) ? HTMAXBUTTON : point.x < S(32) ? HTSYSMENU : HTCAPTION;
            return true;
        }
        result = HTCLIENT;
        return true;
    }
    if (message == WM_NCPAINT) {
        result = 0;
        return true;
    }
    if (message == WM_NCACTIVATE) {
        result = TRUE;
        InvalidateRect(g_caption, nullptr, FALSE);
        return true;
    }
    if (message == WM_NCMOUSEMOVE || message == WM_NCMOUSELEAVE) {
        const bool hover = message == WM_NCMOUSEMOVE && wp == HTMAXBUTTON;
        const bool previous = GetPropW(g_buttons[1], L"dialCaptionHover") != nullptr;
        if (hover != previous) {
            if (hover) {
                SetPropW(g_buttons[1], L"dialCaptionHover", reinterpret_cast<HANDLE>(1));
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE | TME_NONCLIENT, window, 0};
                TrackMouseEvent(&tracking);
            } else {
                RemovePropW(g_buttons[1], L"dialCaptionHover");
            }

            InvalidateRect(g_buttons[1], nullptr, FALSE);
        }
        // The default procedure must still see nonclient motion for system UI.
    }
    if ((message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONDBLCLK || message == WM_NCLBUTTONUP) &&
        wp == HTMAXBUTTON) {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(g_buttons[1], &point);
        SendMessageW(g_buttons[1], message == WM_NCLBUTTONUP ? WM_LBUTTONUP : WM_LBUTTONDOWN,
                     message == WM_NCLBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(point.x, point.y));
        result = 0;
        return true;
    }
    if (message == WM_SETTEXT || message == WM_ACTIVATE)
        InvalidateRect(g_caption, nullptr, FALSE);
    return false;
}
}  // namespace dl
