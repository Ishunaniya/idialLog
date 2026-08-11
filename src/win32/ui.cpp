// ui.cpp — modem_mng 日志排查工具的 Win32 主窗口与程序入口
// 解析/分析位于 src/core，页面与加载用例由同目录的独立模块负责。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   // Win7+
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "app_context.h"
#include "version.h"
#include "ui_pages.h"
#include "load_controller.h"

using namespace dl;

// ============================ 控件 ID ============================
#define IDC_TAB       1001
#define IDC_OPEN      1002
#define IDC_APPLY     1003
#define IDC_CLEAR     1004
#define IDC_EXPORT    1005
#define IDC_CLOSELOG  1030   // 关闭日志(卸载当前数据)
#define IDC_TAGBOX    1006
#define IDC_GREPBOX   1007
#define IDC_SINCEBOX  1008
#define IDC_UNTILBOX  1009
#define IDC_FILELBL   1010
#define IDC_SUMMARY   1011
#define IDC_TIMELINE  1012
#define IDC_OUTAGE    1013
#define IDC_METRIC    1014
#define IDC_TAGS      1015
#define IDC_RAW       1016
#define IDC_CHART     1017
#define IDC_STATUS    1018
#define IDC_FINDINGS  1019
#define IDC_UNPARSED  1020
#define IDC_PASTE     1021
#define IDC_DASH      1022

// ============================ DPI 缩放中枢 ============================
// PerMonitorV2:所有尺寸以 96 DPI 逻辑像素书写,经 S() 换算成当前显示器物理像素。
// App().dpi 在 WM_CREATE 初始化、WM_DPICHANGED 更新。一处控制,避免 45 处散改漏改。
// 字体高度是负值(-12 表示字符高 12px),换算要保留符号

// 这些原来是编译期常量,DPI 化后必须运行期算(依赖 App().dpi),故改成取值函数。
static inline int TOP_H()   { return S(78); }
static inline int CHART_H() { return S(300); }

// ============================ 布局 ============================
static void Layout() {
    RECT rc;
    GetClientRect(App().hMain, &rc);
    SendMessageW(App().hStatus, WM_SIZE, 0, 0);
    RECT rs;
    GetWindowRect(App().hStatus, &rs);
    int statusH = rs.bottom - rs.top;

    int tabTop = TOP_H();
    int tabH = rc.bottom - tabTop - statusH;
    if (tabH < 40) tabH = 40;
    MoveWindow(App().hTab, 0, tabTop, rc.right, tabH, TRUE);

    RECT d{ 0, tabTop, rc.right, tabTop + tabH };
    TabCtrl_AdjustRect(App().hTab, FALSE, &d);

    // 总览页:上=仪表盘(固定高),下=详情文字
    const int DASH_H = S(336);
    int dh = std::min<int>(DASH_H, (int)(d.bottom - d.top) - S(60));
    if (dh < S(80)) dh = S(80);
    MoveWindow(App().hDash,    d.left, d.top, d.right - d.left, dh, TRUE);
    MoveWindow(App().hSummary, d.left, d.top + dh, d.right - d.left, (d.bottom - d.top) - dh, TRUE);
    MoveWindow(App().hFindings, d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(App().hTimeline, d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(App().hOutage,   d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(App().hTags,     d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(App().hRaw,      d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(App().hUnparsed, d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);

    // 指标页:图表(上) + 表格(中) + 导出按钮(下)
    int ch = std::min<int>(CHART_H(), (int)(d.bottom - d.top) / 2);   // RECT 成员是 LONG,显式定型
    MoveWindow(App().hChart,  d.left, d.top, d.right - d.left, ch, TRUE);
    int btnH = S(28);
    int gridTop = d.top + ch;
    int gridH = (d.bottom - d.top) - ch - btnH;
    if (gridH < S(30)) gridH = S(30);
    MoveWindow(App().hMetric, d.left, gridTop, d.right - d.left, gridH, TRUE);
    MoveWindow(App().hExport, d.left, gridTop + gridH, S(160), btnH, TRUE);

    // 顶部文件名标签跟随宽度
    MoveWindow(App().hFileLbl, S(196), S(10), std::max(S(200), (int)rc.right - S(208)), S(20), TRUE);  // 让开“粘贴日志”按钮
}

// ============================ 主窗口 ============================
static HWND Mk(const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id, HFONT f) {
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, App().hMain, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

static HWND MkLv(int id, std::initializer_list<std::pair<const wchar_t*, int>> cols,
                 bool ownerData = false) {
    DWORD style = WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS;
    if (ownerData) style |= LVS_OWNERDATA;
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              style,
                              0, 0, 10, 10, App().hMain, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);  // 不要网格线:老派且抢视觉
    SendMessageW(lv, WM_SETFONT, (WPARAM)App().hFontMono, TRUE);
    int i = 0;
    for (auto& c : cols) LvAddCol(lv, i++, c.first, c.second);
    return lv;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        App().hMain = hwnd;
        // DPI 初始化(必须在建控件/字体前):PerMonitorV2 下 GetDpiForWindow 给出本窗口所在
        // 显示器的 DPI。动态取函数指针,老系统(无此 API)回退 96。
        {
            HMODULE u32 = GetModuleHandleW(L"user32.dll");
            typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
            auto pGetDpiForWindow = u32 ? reinterpret_cast<GetDpiForWindow_t>(
                reinterpret_cast<void*>(GetProcAddress(u32, "GetDpiForWindow"))) : nullptr;
            if (pGetDpiForWindow) {
                UINT d = pGetDpiForWindow(hwnd);
                if (d >= 72 && d <= 480) App().dpi = (int)d;   // 合理范围保护
            }
        }
        // 字体
        App().hFontUI = CreateFontW(SF(-12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        App().hFontMono = CreateFontW(SF(-12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");
        // 仪表盘字体。hero ≥48px;大数字用比例数字(非等宽),等宽只留给要对齐的列
        auto mkf = [](int h, int w) {
            return CreateFontW(SF(h), 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               L"Microsoft YaHei UI");
        };
        App().hFontHero    = mkf(-48, FW_SEMIBOLD);
        App().hFontTileVal = mkf(-22, FW_SEMIBOLD);
        App().hFontTileLbl = mkf(-12, FW_NORMAL);
        App().hFontSect    = mkf(-14, FW_SEMIBOLD);

        // 顶部工具栏(逻辑像素,S() 换算到物理像素)
        Mk(L"BUTTON", L"打开日志…", BS_PUSHBUTTON, S(8), S(6), S(96), S(26), IDC_OPEN, App().hFontUI);
        Mk(L"BUTTON", L"粘贴日志", BS_PUSHBUTTON, S(108), S(6), S(80), S(26), IDC_PASTE, App().hFontUI);
        App().hFileLbl = Mk(L"STATIC", L"未加载 —— 拖入 dial_*.log(可多选),或复制日志文本后按 Ctrl+V / 点“粘贴日志”",
                      SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, S(196), S(10), S(820), S(20), IDC_FILELBL, App().hFontUI);

        int y = 44;
        Mk(L"STATIC", L"标签:", SS_LEFT, S(8), S(y + 4), S(36), S(18), 0, App().hFontUI);
        App().hTagBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, S(46), S(y), S(140), S(22), IDC_TAGBOX, App().hFontUI);
        Mk(L"STATIC", L"筛选(正则):", SS_LEFT, S(196), S(y + 4), S(74), S(18), 0, App().hFontUI);
        App().hGrepBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, S(272), S(y), S(210), S(22), IDC_GREPBOX, App().hFontUI);
        Mk(L"STATIC", L"起:", SS_LEFT, S(492), S(y + 4), S(22), S(18), 0, App().hFontUI);
        App().hSinceBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, S(516), S(y), S(84), S(22), IDC_SINCEBOX, App().hFontUI);
        Mk(L"STATIC", L"止:", SS_LEFT, S(608), S(y + 4), S(22), S(18), 0, App().hFontUI);
        App().hUntilBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, S(632), S(y), S(84), S(22), IDC_UNTILBOX, App().hFontUI);
        Mk(L"BUTTON", L"应用筛选", BS_PUSHBUTTON, S(728), S(y - 2), S(84), S(26), IDC_APPLY, App().hFontUI);
        Mk(L"BUTTON", L"清空筛选", BS_PUSHBUTTON, S(818), S(y - 2), S(84), S(26), IDC_CLEAR, App().hFontUI);
        Mk(L"BUTTON", L"关闭日志", BS_PUSHBUTTON, S(908), S(y - 2), S(84), S(26), IDC_CLOSELOG, App().hFontUI);

        // 页签
        App().hTab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, TOP_H(), 100, 100, hwnd, (HMENU)(INT_PTR)IDC_TAB,
                               GetModuleHandleW(nullptr), nullptr);
        SendMessageW(App().hTab, WM_SETFONT, (WPARAM)App().hFontUI, TRUE);
        const wchar_t* tabs[] = { L"总览", L"结论 ★", L"时间线", L"断网",
                                  L"指标 / 信号图", L"标签", L"原始行", L"未识别行" };
        for (int i = 0; i < (int)(sizeof(tabs)/sizeof(tabs[0])); ++i) {
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = (LPWSTR)tabs[i];
            TabCtrl_InsertItem(App().hTab, i, &ti);
        }

        // 各页控件
        App().hDash = CreateWindowExW(0, L"dialDashCls", L"", WS_CHILD,
                                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_DASH,
                                GetModuleHandleW(nullptr), nullptr);
        App().hSummary = CreateWindowExW(0, L"dialSummaryCls", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                                   0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_SUMMARY, GetModuleHandleW(nullptr), nullptr);
        App().hFindings = CreateWindowExW(0, L"dialFindingsCls", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                                    0, 0, 100, 100, hwnd, (HMENU)(INT_PTR)IDC_FINDINGS, GetModuleHandleW(nullptr), nullptr);
        App().hUnparsed = MkLv(IDC_UNPARSED, { {L"原始行号", 90}, {L"未识别的原文", 960} });
        App().hTimeline = MkLv(IDC_TIMELINE, { {L"时间", 140}, {L"标签", 90}, {L"消息", 820} }, true);
        App().hOutage   = MkLv(IDC_OUTAGE,   { {L"#", 44}, {L"开始", 160}, {L"恢复", 160}, {L"时长", 90} });
        App().hMetric   = MkLv(IDC_METRIC,   { {L"时间", 140}, {L"CH", 90}, {L"CSQ", 60}, {L"Tmax", 60},
                                         {L"ConsecFail", 90}, {L"RX_PKT", 110}, {L"ΔRX", 80},
                                         {L"RSRP", 70}, {L"RSRQ", 70}, {L"SNR(dB)", 80},
                                         {L"RSSI", 70}, {L"SRV", 55}, {L"RAT", 80},
                                         {L"DENY", 60}, {L"OPER", 150} }, true);
        App().hTags     = MkLv(IDC_TAGS,     { {L"标签", 150}, {L"次数", 80}, {L"占比", 600} });
        App().hRaw = Mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY,
                  0, 0, 10, 10, IDC_RAW, App().hFontMono);
        App().hChart = CreateWindowExW(0, L"dialChartCls", L"", WS_CHILD | WS_BORDER,
                                 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_CHART,
                                 GetModuleHandleW(nullptr), nullptr);
        App().hExport = Mk(L"BUTTON", L"导出指标 CSV…", BS_PUSHBUTTON, 0, 0, 160, 28, IDC_EXPORT, App().hFontUI);

        // 状态栏
        App().hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"  就绪。拖入 dial_*.log 开始。",
                                  WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                                  hwnd, (HMENU)(INT_PTR)IDC_STATUS, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(App().hStatus, WM_SETFONT, (WPARAM)App().hFontUI, TRUE);

        ShowPage(0);
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }

    case WM_SIZE:
        Layout();
        return 0;

    // 窗口被拖到不同 DPI 的显示器(或系统缩放变更):更新 App().dpi,重建字体(字体尺寸是
    // 创建时固定的,不随 DPI 自动变),按系统建议的新矩形调整窗口,再重排。
    case WM_DPICHANGED: {
        App().dpi = HIWORD(wp);   // 新 DPI(x,y 相同)
        // 重建所有字体
        auto mkf2 = [](int h, int w, const wchar_t* face, DWORD pitch) {
            return CreateFontW(SF(h), 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, pitch, face);
        };
        HFONT oUI = App().hFontUI, oMono = App().hFontMono, oHero = App().hFontHero,
              oTV = App().hFontTileVal, oTL = App().hFontTileLbl, oSect = App().hFontSect;
        App().hFontUI      = mkf2(-12, FW_NORMAL,  L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        App().hFontMono    = mkf2(-12, FW_NORMAL,  L"Consolas",           FIXED_PITCH | FF_MODERN);
        App().hFontHero    = mkf2(-48, FW_SEMIBOLD,L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        App().hFontTileVal = mkf2(-22, FW_SEMIBOLD,L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        App().hFontTileLbl = mkf2(-12, FW_NORMAL,  L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        App().hFontSect    = mkf2(-14, FW_SEMIBOLD,L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        // 把新字体铺给顶部控件(子控件字体逐个重设)
        for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            SendMessageW(c, WM_SETFONT, (WPARAM)App().hFontUI, TRUE);
        SendMessageW(App().hTab, WM_SETFONT, (WPARAM)App().hFontUI, TRUE);
        // MkLv / 原始行编辑框初始使用等宽字体；上面的统一设置会覆盖它们，必须恢复。
        for (HWND c : { App().hTimeline, App().hOutage, App().hMetric, App().hTags, App().hUnparsed, App().hRaw })
            if (c) SendMessageW(c, WM_SETFONT, (WPARAM)App().hFontMono, TRUE);
        // 删旧字体
        for (HFONT f : { oUI, oMono, oHero, oTV, oTL, oSect }) if (f) DeleteObject(f);
        // 按系统建议矩形调整窗口(会触发 WM_SIZE → Layout)
        RECT* nr = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, nr->left, nr->top,
                     nr->right - nr->left, nr->bottom - nr->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = S(920);
        mmi->ptMinTrackSize.y = S(520);
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(hd, i, nullptr, 0);
            std::wstring p((size_t)len + 1, L'\0');
            DragQueryFileW(hd, i, &p[0], len + 1);
            p.resize(len);
            paths.push_back(p);
        }
        DragFinish(hd);
        if (!paths.empty()) LoadFiles(paths);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_OPEN:   DoOpen(); return 0;
        case IDC_PASTE:  DoPaste(); return 0;
        case IDC_APPLY:  RefreshAll(); return 0;
        case IDC_EXPORT: DoExportCsv(); return 0;
        case IDC_CLEAR:
            SetWindowTextW(App().hTagBox, L"");
            SetWindowTextW(App().hGrepBox, L"");
            SetWindowTextW(App().hSinceBox, L"");
            SetWindowTextW(App().hUntilBox, L"");
            RefreshAll();
            return 0;
        case IDC_CLOSELOG: {
            // 关闭日志:卸载数据回到"尚未加载"。RefreshAll 对空数据会提前 return
            // (不刷各页),故此处手动逐页渲染,否则列表/卡片残留上一份日志。
            ReleaseLoadedData();
            SetWindowTextW(App().hTagBox, L"");
            SetWindowTextW(App().hGrepBox, L"");
            SetWindowTextW(App().hSinceBox, L"");
            SetWindowTextW(App().hUntilBox, L"");
            MarkAllPagesDirty();
            RenderPage(CurrentPage());
            SetWindowTextW(App().hFileLbl, L"");
            SetWindowTextW(App().hStatus, L"尚未加载日志。");
            return 0;
        }
        }
        return 0;

    case WM_NOTIFY: {
        LPNMHDR hdr = reinterpret_cast<LPNMHDR>(lp);
        if (hdr->idFrom == IDC_TAB && hdr->code == TCN_SELCHANGE) {
            ShowPage(TabCtrl_GetCurSel(App().hTab));
            return 0;
        }
        LRESULT result = 0;
        return HandlePageNotify(lp, result) ? result : 0;
    }

    case WM_DESTROY:
        if (App().hFontUI)   DeleteObject(App().hFontUI);
        if (App().hFontMono) DeleteObject(App().hFontMono);
        if (App().hFontHero) DeleteObject(App().hFontHero);
        if (App().hFontTileVal) DeleteObject(App().hFontTileVal);
        if (App().hFontTileLbl) DeleteObject(App().hFontTileLbl);
        if (App().hFontSect) DeleteObject(App().hFontSect);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wcDash{};
    wcDash.cbSize = sizeof(wcDash);
    wcDash.lpfnWndProc = DashProc;
    wcDash.hInstance = hInst;
    wcDash.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcDash.lpszClassName = L"dialDashCls";
    RegisterClassExW(&wcDash);

    WNDCLASSEXW wcSum{};
    wcSum.cbSize = sizeof(wcSum);
    wcSum.lpfnWndProc = SummaryProc;
    wcSum.hInstance = hInst;
    wcSum.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcSum.lpszClassName = L"dialSummaryCls";
    RegisterClassExW(&wcSum);

    WNDCLASSEXW wcFind{};
    wcFind.cbSize = sizeof(wcFind);
    wcFind.lpfnWndProc = FindingsProc;
    wcFind.hInstance = hInst;
    wcFind.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcFind.lpszClassName = L"dialFindingsCls";
    RegisterClassExW(&wcFind);

    WNDCLASSEXW wcChart{};
    wcChart.cbSize = sizeof(wcChart);
    wcChart.lpfnWndProc = ChartProc;
    wcChart.hInstance = hInst;
    wcChart.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcChart.lpszClassName = L"dialChartCls";
    RegisterClassExW(&wcChart);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"dialLogMainCls";
    // 自定义应用图标(resource.rc 中 IDI_APPICON=101):任务栏/Alt-Tab/标题栏都用它。
    // LoadImage 按需求尺寸取 ico 内最匹配的一档(大图标取 32,小图标取 16)。
    HICON hIconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                       0, 0, LR_DEFAULTSIZE | LR_SHARED);
    HICON hIconSm  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                       LR_SHARED);
    wc.hIcon   = hIconBig ? hIconBig : LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = hIconSm  ? hIconSm  : LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // 标题带版本:用户发来的截图能直接看出是哪个 build
    HWND w = CreateWindowExW(WS_EX_ACCEPTFILES, L"dialLogMainCls",
                             DL_APP_NAME_W L" v" DL_VER_WSTR L" — 拨号日志分析",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760,
                             nullptr, nullptr, hInst, nullptr);
    if (!w) return 1;
    // 初始尺寸 1180×760 是 96 DPI 逻辑值。WM_CREATE 已把 App().dpi 设为本窗口 DPI,
    // 若非 96 则按比例放大外框(否则高分屏上窗口偏小、装不下放大后的内容)。
    if (App().dpi != 96) {
        RECT wr; GetWindowRect(w, &wr);
        SetWindowPos(w, nullptr, 0, 0,
                     MulDiv(1180, App().dpi, 96), MulDiv(760, App().dpi, 96),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(w, nCmdShow);
    UpdateWindow(w);

    // 命令行:dialLog.exe [--tab=N] a.log [b.log ...]
    // 支持“拖到 exe 图标上打开”“右键→打开方式”“命令行直开”;--tab=0..7 指定初始页签(0总览 1结论 2时间线 3断网 4指标 5标签 6原始行 7未识别行)
    if (lpCmdLine && *lpCmdLine) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            std::vector<std::wstring> paths;
            int tab = -1;
            bool paste = false;
            for (int i = 1; i < argc; ++i) {
                std::wstring a = argv[i];
                if (a.compare(0, 6, L"--tab=") == 0) tab = _wtoi(a.c_str() + 6);
                else if (a == L"--paste") paste = true;   // 直接分析剪贴板内容
                else paths.push_back(a);
            }
            LocalFree(argv);
            if (!paths.empty()) LoadFiles(paths);
            else if (paste) DoPaste();
            if (tab >= 0 && tab < 8) {
                TabCtrl_SetCurSel(App().hTab, tab);
                ShowPage(tab);
            }
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Ctrl+V 载入剪贴板日志。但焦点在筛选输入框里时不拦——那里 Ctrl+V 该是正常粘贴文字。
        if (msg.message == WM_KEYDOWN && msg.wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            HWND f = GetFocus();
            if (f != App().hTagBox && f != App().hGrepBox && f != App().hSinceBox && f != App().hUntilBox) {
                DoPaste();
                continue;
            }
        }
        if (IsDialogMessageW(w, &msg)) continue;   // Tab 键在输入框间跳转
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
