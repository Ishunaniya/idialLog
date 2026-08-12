// ui.cpp — dialLog 的现代 Win32 应用外壳与程序入口
// 解析/分析仍位于 src/core；这里仅负责导航、命令、筛选和页面布局。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "app_context.h"
#include "load_controller.h"
#include "modern_shell.h"
#include "theme.h"
#include "ui_pages.h"
#include "version.h"

using namespace dl;

#define IDC_NAV        1001
#define IDC_OPEN       1002
#define IDC_APPLY      1003
#define IDC_CLEAR      1004
#define IDC_EXPORT     1005
#define IDC_TAGBOX     1006
#define IDC_GREPBOX    1007
#define IDC_SINCEBOX   1008
#define IDC_UNTILBOX   1009
#define IDC_FILELBL    1010
#define IDC_SUMMARY    1011
#define IDC_TIMELINE   1012
#define IDC_OUTAGE     1013
#define IDC_METRIC     1014
#define IDC_TAGS       1015
#define IDC_RAW        1016
#define IDC_CHART      1017
#define IDC_STATUS     1018
#define IDC_FINDINGS   1019
#define IDC_UNPARSED   1020
#define IDC_PASTE      1021
#define IDC_DASH       1022
#define IDC_FILTER     1023
#define IDC_PAGETITLE  1024
#define IDC_CLOSELOG   1030

namespace {

constexpr UINT_PTR kFilterTimer = 7;
bool g_filtersExpanded = false;
int g_headerHeight = 88;
RECT g_filterPanel{};

int NavWidth() { return S(204); }
int StatusHeight() { return S(32); }
int ChartHeight() { return S(300); }

bool HasText(HWND edit) { return edit && GetWindowTextLengthW(edit) > 0; }

void UpdateFilterButton() {
    int count = 0;
    for (HWND edit : {App().hTagBox, App().hGrepBox, App().hSinceBox, App().hUntilBox})
        if (HasText(edit)) ++count;
    std::wstring label = L"筛选";
    if (count) label += L" (" + std::to_wstring(count) + L")";
    SetWindowTextW(App().hFilterToggle, label.c_str());
    SetModernButtonActive(App().hFilterToggle, g_filtersExpanded || count > 0);
}

void SetFilterControlsVisible(bool visible) {
    const int command = visible ? SW_SHOW : SW_HIDE;
    for (HWND control : {App().hTagLabel, App().hGrepLabel, App().hSinceLabel, App().hUntilLabel,
                         App().hTagBox, App().hGrepBox, App().hSinceBox, App().hUntilBox,
                         App().hApplyFilter, App().hClearFilter})
        if (control) ShowWindow(control, command);
}

HWND CreateControl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT font,
                   bool visible = true) {
    DWORD windowStyle = WS_CHILD | style;
    if (lstrcmpiW(cls, L"BUTTON") == 0 || lstrcmpiW(cls, L"EDIT") == 0)
        windowStyle |= WS_TABSTOP;
    if (visible) windowStyle |= WS_VISIBLE;
    HWND control = CreateWindowExW(0, cls, text, windowStyle, 0, 0, 10, 10, App().hMain,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return control;
}

HWND CreateButton(const wchar_t* text, int id, ModernButtonKind kind, bool visible = true) {
    HWND button = CreateControl(L"BUTTON", text, BS_OWNERDRAW, id, App().hFontUI, visible);
    ConfigureModernButton(button, kind);
    return button;
}

HWND CreateList(int id, std::initializer_list<std::pair<const wchar_t*, int>> columns,
                bool ownerData = false) {
    DWORD style = WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS;
    if (ownerData) style |= LVS_OWNERDATA;
    HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"", style, 0, 0, 10, 10, App().hMain,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    if (App().hTableRows) ListView_SetImageList(list, App().hTableRows, LVSIL_SMALL);
    SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontMono), TRUE);
    int index = 0;
    for (const auto& column : columns) LvAddCol(list, index++, column.first, column.second);
    return list;
}

HFONT CreateAppFont(int height, int weight, const wchar_t* face, DWORD pitch) {
    return CreateFontW(SF(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, pitch, face);
}

void CreateFonts() {
    App().hFontUI = CreateAppFont(-14, FW_NORMAL, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
    App().hFontMono = CreateAppFont(-13, FW_NORMAL, L"Consolas", FIXED_PITCH | FF_MODERN);
    App().hFontTitle = CreateAppFont(-28, FW_SEMIBOLD, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
    App().hFontSmall = CreateAppFont(-12, FW_NORMAL, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
    App().hFontHero = CreateAppFont(-48, FW_SEMIBOLD, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
    App().hFontTileVal = CreateAppFont(-22, FW_SEMIBOLD, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
    App().hFontTileLbl = CreateAppFont(-13, FW_NORMAL, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
    App().hFontSect = CreateAppFont(-16, FW_SEMIBOLD, L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
}

void CreateTableRowImageList() {
    if (App().hTableRows) {
        for (HWND list : {App().hTimeline, App().hOutage, App().hMetric, App().hTags, App().hUnparsed})
            if (list) ListView_SetImageList(list, nullptr, LVSIL_SMALL);
        ImageList_Destroy(App().hTableRows);
    }
    App().hTableRows = ImageList_Create(1, S(28), ILC_COLOR32 | ILC_MASK, 1, 1);
    if (!App().hTableRows) return;
    HBITMAP pixel = CreateBitmap(1, S(28), 1, 1, nullptr);
    ImageList_AddMasked(App().hTableRows, pixel, RGB(0, 0, 0));
    DeleteObject(pixel);
    for (HWND list : {App().hTimeline, App().hOutage, App().hMetric, App().hTags, App().hUnparsed})
        if (list) ListView_SetImageList(list, App().hTableRows, LVSIL_SMALL);
}

void ApplyFontsToControls() {
    for (HWND child = GetWindow(App().hMain, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontUI), TRUE);
    for (HWND control : {App().hTimeline, App().hOutage, App().hMetric, App().hTags,
                         App().hUnparsed, App().hRaw})
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontMono), TRUE);
    if (App().hPageTitle)
        SendMessageW(App().hPageTitle, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontTitle), TRUE);
    if (App().hFileLbl)
        SendMessageW(App().hFileLbl, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontSmall), TRUE);
    for (HWND label : {App().hTagLabel, App().hGrepLabel, App().hSinceLabel, App().hUntilLabel})
        if (label) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontSmall), TRUE);
}

void DeleteFonts(const std::array<HFONT, 8>& fonts) {
    for (HFONT font : fonts) if (font) DeleteObject(font);
}

RECT ContentRect(const RECT& client) {
    return RECT{NavWidth() + S(20), S(g_headerHeight + 14), client.right - S(20),
                client.bottom - StatusHeight() - S(14)};
}

void MoveIf(HWND window, int x, int y, int width, int height) {
    if (window) MoveWindow(window, x, y, std::max(1, width), std::max(1, height), TRUE);
}

void LayoutFilterPanel(int contentLeft, int contentWidth) {
    const int x = contentLeft + S(20);
    const int y = S(78);
    const int width = contentWidth - S(40);
    const bool compact = width < S(840);
    g_headerHeight = compact ? 218 : 164;
    g_filterPanel = RECT{x, y, x + width, S(g_headerHeight - 14)};
    if (!g_filtersExpanded) {
        g_headerHeight = 88;
        SetRectEmpty(&g_filterPanel);
        return;
    }

    const int pad = S(14), labelH = S(17), editH = S(34), gap = S(10);
    if (!compact) {
        int available = width - pad * 2;
        int fixed = S(135 + 120 + 120 + 82 + 74) + gap * 5;
        int searchW = std::max(S(150), available - fixed);
        int cursor = x + pad;
        struct Field { HWND label; HWND edit; int width; } fields[] = {
            {App().hTagLabel, App().hTagBox, S(135)}, {App().hGrepLabel, App().hGrepBox, searchW},
            {App().hSinceLabel, App().hSinceBox, S(120)}, {App().hUntilLabel, App().hUntilBox, S(120)}};
        for (const auto& field : fields) {
            MoveIf(field.label, cursor, y + S(9), field.width, labelH);
            MoveIf(field.edit, cursor, y + S(29), field.width, editH);
            cursor += field.width + gap;
        }
        MoveIf(App().hApplyFilter, cursor, y + S(28), S(82), S(36)); cursor += S(82) + gap;
        MoveIf(App().hClearFilter, cursor, y + S(28), S(74), S(36));
        return;
    }

    const int inner = width - pad * 2;
    const int tagW = std::max(S(120), inner * 35 / 100);
    int cursor = x + pad;
    MoveIf(App().hTagLabel, cursor, y + S(9), tagW, labelH);
    MoveIf(App().hTagBox, cursor, y + S(29), tagW, editH); cursor += tagW + gap;
    MoveIf(App().hGrepLabel, cursor, y + S(9), inner - tagW - gap, labelH);
    MoveIf(App().hGrepBox, cursor, y + S(29), inner - tagW - gap, editH);

    const int row2 = y + S(74);
    const int dateW = std::max(S(108), (inner - S(82 + 74) - gap * 3) / 2);
    cursor = x + pad;
    MoveIf(App().hSinceLabel, cursor, row2, dateW, labelH);
    MoveIf(App().hSinceBox, cursor, row2 + S(20), dateW, editH); cursor += dateW + gap;
    MoveIf(App().hUntilLabel, cursor, row2, dateW, labelH);
    MoveIf(App().hUntilBox, cursor, row2 + S(20), dateW, editH); cursor += dateW + gap;
    MoveIf(App().hApplyFilter, cursor, row2 + S(19), S(82), S(36)); cursor += S(82) + gap;
    MoveIf(App().hClearFilter, cursor, row2 + S(19), S(74), S(36));
}

void Layout() {
    if (!App().hMain) return;
    RECT client{}; GetClientRect(App().hMain, &client);
    const int navW = NavWidth(), statusH = StatusHeight();
    const int contentLeft = navW, contentWidth = client.right - navW;
    MoveIf(App().hNav, 0, 0, navW, client.bottom);
    MoveIf(App().hStatus, contentLeft, client.bottom - statusH, contentWidth, statusH);

    int right = client.right - S(20);
    auto command = [&](HWND button, int width) {
        right -= S(width); MoveIf(button, right, S(20), S(width), S(36)); right -= S(8);
    };
    command(App().hCloseLog, 82);
    command(App().hFilterToggle, 82);
    command(App().hPaste, 92);
    command(App().hOpen, 108);
    if (CurrentPage() == 4) command(App().hExport, 110);
    MoveIf(App().hPageTitle, contentLeft + S(24), S(12), std::max(S(140), right - contentLeft - S(32)), S(38));
    MoveIf(App().hFileLbl, contentLeft + S(25), S(51), std::max(S(140), right - contentLeft - S(33)), S(20));

    LayoutFilterPanel(contentLeft, contentWidth);
    RECT content = ContentRect(client);
    const int width = static_cast<int>(content.right - content.left);
    const int height = std::max(S(40), static_cast<int>(content.bottom - content.top));

    int dashHeight = std::min(S(336), height - S(90));
    dashHeight = std::max(S(130), dashHeight);
    MoveIf(App().hDash, content.left, content.top, width, dashHeight);
    MoveIf(App().hSummary, content.left, content.top + dashHeight, width, height - dashHeight);
    for (HWND page : {App().hFindings, App().hTimeline, App().hOutage, App().hTags,
                      App().hRaw, App().hUnparsed})
        MoveIf(page, content.left, content.top, width, height);

    int chartHeight = std::min(ChartHeight(), height / 2);
    chartHeight = std::max(S(120), chartHeight);
    MoveIf(App().hChart, content.left, content.top, width, chartHeight);
    MoveIf(App().hMetric, content.left, content.top + chartHeight, width, height - chartHeight);
    LayoutModernOverlays();
    InvalidateRect(App().hMain, nullptr, FALSE);
}

void PaintInputFrame(HDC dc, HWND edit) {
    if (!edit || !IsWindowVisible(edit)) return;
    RECT rect{}; GetWindowRect(edit, &rect);
    MapWindowPoints(nullptr, App().hMain, reinterpret_cast<POINT*>(&rect), 2);
    InflateRect(&rect, S(2), S(2));
    FillRound(dc, rect, S(7), th::surface, GetFocus() == edit ? th::accent : th::border);
}

void PaintShell(HWND hwnd) {
    PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
    RECT client{}; GetClientRect(hwnd, &client);
    FillSolid(dc, client, th::page);
    RECT header{NavWidth(), 0, client.right, S(g_headerHeight)};
    FillSolid(dc, header, th::surface);
    HPEN separator = CreatePen(PS_SOLID, 1, th::border);
    HGDIOBJ old = SelectObject(dc, separator);
    MoveToEx(dc, header.left, header.bottom - 1, nullptr); LineTo(dc, header.right, header.bottom - 1);
    SelectObject(dc, old); DeleteObject(separator);
    if (g_filtersExpanded && !IsRectEmpty(&g_filterPanel)) {
        FillRound(dc, g_filterPanel, S(10), th::surface, th::border);
        for (HWND edit : {App().hTagBox, App().hGrepBox, App().hSinceBox, App().hUntilBox})
            PaintInputFrame(dc, edit);
    }
    EndPaint(hwnd, &ps);
}

void ScheduleFilterRefresh(HWND source) {
    if (source != App().hTagBox && source != App().hGrepBox &&
        source != App().hSinceBox && source != App().hUntilBox) return;
    UpdateFilterButton();
    if (!App().document.lines.empty()) {
        KillTimer(App().hMain, kFilterTimer);
        SetTimer(App().hMain, kFilterTimer, 450, nullptr);
    }
}

void ClearFilters(bool refresh) {
    KillTimer(App().hMain, kFilterTimer);
    for (HWND edit : {App().hTagBox, App().hGrepBox, App().hSinceBox, App().hUntilBox})
        SetWindowTextW(edit, L"");
    UpdateFilterButton();
    if (refresh) RefreshAll();
}

LRESULT CALLBACK RawEditSubclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, RawEditSubclass, 1);
        return DefSubclassProc(window, message, wparam, lparam);
    }
    LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_PAINT && GetWindowTextLengthW(window) == 0) {
        HDC dc = GetDC(window);
        RECT client{}; GetClientRect(window, &client);
        const int width = std::min(S(440), std::max(S(280), static_cast<int>(client.right) - S(64)));
        const int height = S(126), x = (client.right - width) / 2;
        const int y = std::max(S(24), (static_cast<int>(client.bottom) - height) / 2);
        RECT card{x, y, x + width, y + height};
        FillRound(dc, card, S(12), th::page, th::border);
        RECT icon{x + S(22), y + S(31), x + S(66), y + S(75)};
        FillRound(dc, icon, S(22), th::accentSoft, th::accentSoft);
        HGDIOBJ old = SelectObject(dc, App().hFontSect);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, th::accent);
        DrawTextW(dc, L">_", -1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT title{x + S(82), y + S(26), card.right - S(18), y + S(53)};
        SetTextColor(dc, th::inkPri);
        DrawTextW(dc, App().document.lines.empty() ? L"还没有原始日志" : L"筛选结果为空",
                  -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, App().hFontUI); SetTextColor(dc, th::inkSec);
        RECT detail{x + S(82), y + S(57), card.right - S(18), y + S(100)};
        DrawTextW(dc, App().document.lines.empty() ? L"加载日志后，可在这里核对解析后的原文。"
                                                   : L"清空筛选条件即可恢复全部日志。",
                  -1, &detail, DT_LEFT | DT_TOP | DT_WORDBREAK);
        SelectObject(dc, old); ReleaseDC(window, dc);
    }
    return result;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        App().hMain = hwnd;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
        auto getDpi = user32 ? reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow"))) : nullptr;
        if (getDpi) {
            UINT dpi = getDpi(hwnd);
            if (dpi >= 72 && dpi <= 480) App().dpi = static_cast<int>(dpi);
        }
        CreateFonts();
        CreateTableRowImageList();

        App().hNav = CreateModernNavigation(hwnd, IDC_NAV);
        App().hPageTitle = CreateControl(L"STATIC", L"概览", SS_LEFT | SS_ENDELLIPSIS,
                                         IDC_PAGETITLE, App().hFontTitle);
        App().hFileLbl = CreateControl(L"STATIC",
            L"未加载日志 · 可拖入文件，或从剪贴板直接分析",
            SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, IDC_FILELBL, App().hFontSmall);
        App().hOpen = CreateButton(L"＋ 打开日志", IDC_OPEN, ModernButtonKind::Primary);
        App().hPaste = CreateButton(L"粘贴日志", IDC_PASTE, ModernButtonKind::Neutral);
        App().hFilterToggle = CreateButton(L"筛选", IDC_FILTER, ModernButtonKind::Neutral);
        App().hCloseLog = CreateButton(L"关闭日志", IDC_CLOSELOG, ModernButtonKind::Danger);
        App().hExport = CreateButton(L"导出 CSV", IDC_EXPORT, ModernButtonKind::Neutral, false);

        App().hTagLabel = CreateControl(L"STATIC", L"标签", SS_LEFT, 0, App().hFontSmall, false);
        App().hGrepLabel = CreateControl(L"STATIC", L"消息正则", SS_LEFT, 0, App().hFontSmall, false);
        App().hSinceLabel = CreateControl(L"STATIC", L"起始时间", SS_LEFT, 0, App().hFontSmall, false);
        App().hUntilLabel = CreateControl(L"STATIC", L"结束时间", SS_LEFT, 0, App().hFontSmall, false);
        App().hTagBox = CreateControl(L"EDIT", L"", ES_AUTOHSCROLL, IDC_TAGBOX, App().hFontUI, false);
        App().hGrepBox = CreateControl(L"EDIT", L"", ES_AUTOHSCROLL, IDC_GREPBOX, App().hFontUI, false);
        App().hSinceBox = CreateControl(L"EDIT", L"", ES_AUTOHSCROLL, IDC_SINCEBOX, App().hFontUI, false);
        App().hUntilBox = CreateControl(L"EDIT", L"", ES_AUTOHSCROLL, IDC_UNTILBOX, App().hFontUI, false);
        for (HWND edit : {App().hTagBox, App().hGrepBox, App().hSinceBox, App().hUntilBox})
            SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(9), S(9)));
        SendMessageW(App().hTagBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"例如 MODEM"));
        SendMessageW(App().hGrepBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"支持正则表达式"));
        SendMessageW(App().hSinceBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"HH:MM:SS"));
        SendMessageW(App().hUntilBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"HH:MM:SS"));
        App().hApplyFilter = CreateButton(L"应用", IDC_APPLY, ModernButtonKind::Primary, false);
        App().hClearFilter = CreateButton(L"清空", IDC_CLEAR, ModernButtonKind::Neutral, false);

        App().hDash = CreateWindowExW(0, L"dialDashCls", L"", WS_CHILD, 0, 0, 10, 10, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DASH)),
                                      GetModuleHandleW(nullptr), nullptr);
        App().hSummary = CreateWindowExW(0, L"dialSummaryCls", L"", WS_CHILD | WS_VSCROLL,
                                         0, 0, 10, 10, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SUMMARY)),
                                         GetModuleHandleW(nullptr), nullptr);
        App().hFindings = CreateWindowExW(0, L"dialFindingsCls", L"", WS_CHILD | WS_VSCROLL,
                                          0, 0, 10, 10, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FINDINGS)),
                                          GetModuleHandleW(nullptr), nullptr);
        App().hUnparsed = CreateList(IDC_UNPARSED, {{L"原始行号", 90}, {L"未识别的原文", 960}});
        App().hTimeline = CreateList(IDC_TIMELINE, {{L"时间", 140}, {L"标签", 90}, {L"消息", 820}}, true);
        App().hOutage = CreateList(IDC_OUTAGE, {{L"#", 44}, {L"开始", 160}, {L"恢复", 160}, {L"时长", 90}});
        App().hMetric = CreateList(IDC_METRIC, {{L"时间", 140}, {L"CH", 90}, {L"CSQ", 60}, {L"Tmax", 60},
            {L"ConsecFail", 90}, {L"RX_PKT", 110}, {L"ΔRX", 80}, {L"RSRP", 70}, {L"RSRQ", 70},
            {L"SNR(dB)", 80}, {L"RSSI", 70}, {L"SRV", 55}, {L"RAT", 80}, {L"DENY", 60}, {L"OPER", 150}}, true);
        App().hTags = CreateList(IDC_TAGS, {{L"标签", 150}, {L"次数", 80}, {L"占比", 600}});
        App().hRaw = CreateControl(L"EDIT", L"", WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY,
                                   IDC_RAW, App().hFontMono, false);
        SendMessageW(App().hRaw, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(12), S(12)));
        SetWindowSubclass(App().hRaw, RawEditSubclass, 1, 0);
        App().hChart = CreateWindowExW(0, L"dialChartCls", L"", WS_CHILD, 0, 0, 10, 10, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHART)),
                                       GetModuleHandleW(nullptr), nullptr);
        App().hStatus = CreateModernStatus(hwnd, IDC_STATUS);

        ApplyModernTheme(hwnd);
        ShowPage(0);
        UpdateFilterButton();
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintShell(hwnd); return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_APP_NAVIGATE: ShowPage(static_cast<int>(wparam)); return 0;
    case WM_APP_SHELL_LAYOUT: Layout(); return 0;
    case WM_DRAWITEM:
        if (DrawModernButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam))) return TRUE;
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return reinterpret_cast<LRESULT>(ModernControlBrush(message, reinterpret_cast<HDC>(wparam),
                                                            reinterpret_cast<HWND>(lparam)));
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        ApplyModernTheme(hwnd); return 0;
    case WM_DPICHANGED: {
        App().dpi = HIWORD(wparam);
        std::array<HFONT, 8> old{App().hFontUI, App().hFontMono, App().hFontTitle, App().hFontSmall,
                                 App().hFontHero, App().hFontTileVal, App().hFontTileLbl, App().hFontSect};
        CreateFonts(); ApplyFontsToControls(); CreateTableRowImageList(); DeleteFonts(old);
        RECT* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RefreshNavigation(); InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = S(980); info->ptMinTrackSize.y = S(640); return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wparam);
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT index = 0; index < count; ++index) {
            UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring path(static_cast<size_t>(length) + 1, L'\0');
            DragQueryFileW(drop, index, &path[0], length + 1); path.resize(length);
            paths.push_back(std::move(path));
        }
        DragFinish(drop); if (!paths.empty()) LoadFiles(paths); return 0;
    }
    case WM_TIMER:
        if (wparam == kFilterTimer) { KillTimer(hwnd, kFilterTimer); RefreshAll(); return 0; }
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wparam), code = HIWORD(wparam);
        if (code == EN_CHANGE) ScheduleFilterRefresh(reinterpret_cast<HWND>(lparam));
        switch (id) {
        case IDC_OPEN: DoOpen(); return 0;
        case IDC_PASTE: DoPaste(); return 0;
        case IDC_FILTER:
            g_filtersExpanded = !g_filtersExpanded;
            SetFilterControlsVisible(g_filtersExpanded); UpdateFilterButton(); Layout();
            if (g_filtersExpanded) SetFocus(App().hTagBox);
            return 0;
        case IDC_APPLY: KillTimer(hwnd, kFilterTimer); RefreshAll(); return 0;
        case IDC_EXPORT: DoExportCsv(); return 0;
        case IDC_CLEAR: ClearFilters(true); return 0;
        case IDC_CLOSELOG:
            ReleaseLoadedData(); ClearFilters(false); MarkAllPagesDirty(); RenderPage(CurrentPage());
            SetWindowTextW(App().hFileLbl, L"未加载日志 · 可拖入文件，或从剪贴板直接分析");
            SetWindowTextW(App().hStatus, L"尚未加载日志。"); RefreshNavigation(); return 0;
        }
        return 0;
    }
    case WM_NOTIFY: {
        LRESULT result = 0; return HandlePageNotify(lparam, result) ? result : 0;
    }
    case WM_DESTROY: {
        KillTimer(hwnd, kFilterTimer);
        std::array<HFONT, 8> fonts{App().hFontUI, App().hFontMono, App().hFontTitle, App().hFontSmall,
                                   App().hFontHero, App().hFontTileVal, App().hFontTileLbl, App().hFontSect};
        if (App().hTableRows) {
            for (HWND list : {App().hTimeline, App().hOutage, App().hMetric, App().hTags, App().hUnparsed})
                if (list) ListView_SetImageList(list, nullptr, LVSIL_SMALL);
            ImageList_Destroy(App().hTableRows); App().hTableRows = nullptr;
        }
        DeleteFonts(fonts); PostQuitMessage(0); return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int show) {
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    RegisterModernShellClasses(instance);

    auto registerClass = [instance](const wchar_t* name, WNDPROC procedure) {
        WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.lpfnWndProc = procedure;
        cls.hInstance = instance; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = name;
        return RegisterClassExW(&cls);
    };
    registerClass(L"dialDashCls", DashProc);
    registerClass(L"dialSummaryCls", SummaryProc);
    registerClass(L"dialFindingsCls", FindingsProc);
    registerClass(L"dialChartCls", ChartProc);

    WNDCLASSEXW mainClass{}; mainClass.cbSize = sizeof(mainClass); mainClass.lpfnWndProc = WndProc;
    mainClass.hInstance = instance; mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.lpszClassName = L"dialLogMainCls";
    mainClass.hIcon = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                         0, 0, LR_DEFAULTSIZE | LR_SHARED));
    mainClass.hIconSm = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!mainClass.hIcon) mainClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!mainClass.hIconSm) mainClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&mainClass);

    HWND window = CreateWindowExW(WS_EX_ACCEPTFILES, L"dialLogMainCls",
        DL_APP_NAME_W L" v" DL_VER_WSTR L" — 拨号日志分析",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    if (App().dpi != 96)
        SetWindowPos(window, nullptr, 0, 0, MulDiv(1280, App().dpi, 96), MulDiv(820, App().dpi, 96),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, show); UpdateWindow(window);

    if (commandLine && *commandLine) {
        int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            std::vector<std::wstring> paths; int page = -1; bool paste = false;
            for (int index = 1; index < argc; ++index) {
                std::wstring argument = argv[index];
                if (argument.compare(0, 6, L"--tab=") == 0) page = _wtoi(argument.c_str() + 6);
                else if (argument == L"--paste") paste = true;
                else paths.push_back(std::move(argument));
            }
            LocalFree(argv);
            if (!paths.empty()) LoadFiles(paths); else if (paste) DoPaste();
            if (page >= 0 && page < 8) ShowPage(page);
        }
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == 'V' &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            HWND focus = GetFocus();
            if (focus != App().hTagBox && focus != App().hGrepBox &&
                focus != App().hSinceBox && focus != App().hUntilBox) {
                DoPaste(); continue;
            }
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            HWND focus = GetFocus();
            if (focus == App().hTagBox || focus == App().hGrepBox ||
                focus == App().hSinceBox || focus == App().hUntilBox) {
                KillTimer(window, kFilterTimer); RefreshAll(); continue;
            }
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && g_filtersExpanded) {
            g_filtersExpanded = false; SetFilterControlsVisible(false); UpdateFilterButton(); Layout(); continue;
        }
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return 0;
}
