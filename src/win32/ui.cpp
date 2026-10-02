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
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "app_settings.h"
#include "app_context.h"
#include "load_controller.h"
#include "source_workspace.h"
#include "modern_shell.h"
#include "theme.h"
#include "ui_pages.h"
#include "version.h"
#include "win_text.h"
#include "text_catalog.h"

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
#define IDC_SEARCH_HISTORY 1025
#define IDC_CELLS      1026
#define IDC_METRIC_FILTER 1027
#define IDC_CLOSELOG   1030
#define IDC_OPEN_PICK  1040
#define IDC_RECENT_CLEAR 1041
#define IDC_BOOKMARKS  1042
#define IDC_BOOKMARK_CLEAR 1043
#define IDC_EXPORT_REPORT 1044
#define IDC_EXPORT_CSV 1045
#define IDC_EXPORT_HTML 1046
#define IDC_METRIC_VIEW_CHART 1047
#define IDC_METRIC_VIEW_SPLIT 1048
#define IDC_METRIC_VIEW_TABLE 1049
#define IDC_DETAIL_CLOSE 1111
#define IDC_DETAIL_TEXT  1112
#define IDC_RECENT_BASE  1050
#define IDC_BOOKMARK_BASE 1080
#define IDC_SEARCH_CLEAR 1110
#define IDC_SEARCH_BASE 1120
#define IDC_APPEARANCE 1135
#define IDC_FONT_UI 1136
#define IDC_FONT_LOG 1137
#define IDC_FONT_RESET 1138
#define IDC_PAGE_HINT 1139
#define IDC_TIME_RANGE_LABEL 1140
#define IDC_TIME_RANGE_RESET 1141
#define IDC_METRIC_COLUMNS 1142
#define IDC_SOURCES 1143
#define IDC_LANGUAGE_ZH 1170
#define IDC_LANGUAGE_EN 1171

namespace {

constexpr UINT_PTR kFilterTimer = 7;
constexpr UINT kRefreshFilterButton = WM_APP + 43;
bool g_filterButtonRefreshPending = false;
bool g_filtersExpanded = false;
int g_headerHeight = 88;
RECT g_filterPanel{};
struct FilterFrame { HWND edit = nullptr; RECT rect{}; };
std::array<FilterFrame, 4> g_filterFrames{};
enum class MetricViewMode { Chart, Split, Table };
MetricViewMode g_metricViewMode = MetricViewMode::Split;
int g_metricSplitY = 0;
bool g_metricSplitUserSized = false;
int g_detailHeight = 0;
bool g_dragMetricSplitter = false, g_dragDetailSplitter = false;
RECT g_lastContent{};

void UpdateFilterButton();
void SetFilterControlsVisible(bool visible);
void Layout();

std::wstring MenuSafe(std::wstring text) {
    for (size_t position = 0; (position = text.find(L'&', position)) != std::wstring::npos; position += 2)
        text.insert(position, 1, L'&');
    if (text.size() > 86) text = text.substr(0, 38) + L"…" + text.substr(text.size() - 45);
    return text;
}

void OpenRecent(size_t index) {
    const auto files = GetAppSettings().recentFiles;
    if (index >= files.size()) return;
    const std::wstring path = files[index];
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        RemoveRecentFile(path);
        ShowModernNotice(UiText(TextId::ui_0144), UiText(TextId::ui_0145),
                         ModernNoticeKind::Warning, 6000);
        return;
    }
    LoadFiles({path});
}

void ShowOpenMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) { DoOpen(); return; }
    AppendMenuW(menu, MF_STRING, IDC_OPEN_PICK, UiText(TextId::ui_0146));
    const auto& recent = GetAppSettings().recentFiles;
    if (!recent.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, UiText(TextId::ui_0147));
        for (size_t index = 0; index < recent.size(); ++index) {
            std::wstring label = std::to_wstring(index + 1) + L"  " + MenuSafe(recent[index]);
            AppendMenuW(menu, MF_STRING, IDC_RECENT_BASE + static_cast<UINT>(index), label.c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDC_RECENT_CLEAR, UiText(TextId::ui_0148));
    }
    RECT button{}; GetWindowRect(App().hOpen, &button);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                        button.left, button.bottom + S(4), 0, App().hMain, nullptr);
    DestroyMenu(menu);
    if (command == IDC_OPEN_PICK) DoOpen();
    else if (command == IDC_RECENT_CLEAR) {
        ClearRecentFiles();
        ShowModernNotice(UiText(TextId::ui_0149), UiText(TextId::ui_0150), ModernNoticeKind::Success);
    } else if (command >= IDC_RECENT_BASE && command < IDC_RECENT_BASE + 5) {
        OpenRecent(command - IDC_RECENT_BASE);
    }
}

void ShowSearchHistoryMenu() {
    const auto& history = GetAppSettings().searchHistory;
    if (history.empty()) {
        ShowModernNotice(UiText(TextId::ui_0151), UiText(TextId::ui_0152),
                         ModernNoticeKind::Info);
        return;
    }
    HMENU menu = CreatePopupMenu();
    for (size_t index = 0; index < history.size(); ++index)
        AppendMenuW(menu, MF_STRING, IDC_SEARCH_BASE + static_cast<UINT>(index), MenuSafe(history[index]).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDC_SEARCH_CLEAR, UiText(TextId::ui_0153));
    RECT button{}; GetWindowRect(App().hSearchHistory, &button);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                        button.left, button.bottom + S(4), 0, App().hMain, nullptr);
    DestroyMenu(menu);
    if (command == IDC_SEARCH_CLEAR) {
        ClearSearchHistory();
        ShowModernNotice(UiText(TextId::ui_0154), UiText(TextId::ui_0155), ModernNoticeKind::Success);
    } else if (command >= IDC_SEARCH_BASE && command < IDC_SEARCH_BASE + history.size()) {
        SetWindowTextW(App().hGrepBox, history[command - IDC_SEARCH_BASE].c_str());
        KillTimer(App().hMain, kFilterTimer);
        UpdateFilterButton();
        if (!App().document.lines.empty()) RefreshAll();
    }
}

void ShowBookmarksMenu() {
    const auto& bookmarks = EvidenceBookmarks();
    if (bookmarks.empty()) return;
    HMENU menu = CreatePopupMenu();
    for (size_t index = 0; index < bookmarks.size(); ++index) {
        std::wstring label = FmtW(UiText(TextId::ui_0156), static_cast<int>(bookmarks[index].lineNo)) + bookmarks[index].text;
        AppendMenuW(menu, MF_STRING, IDC_BOOKMARK_BASE + static_cast<UINT>(index), MenuSafe(label).c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDC_BOOKMARK_CLEAR, UiText(TextId::ui_0157));
    RECT button{}; GetWindowRect(App().hBookmarks, &button);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                        button.left, button.bottom + S(4), 0, App().hMain, nullptr);
    DestroyMenu(menu);
    if (command == IDC_BOOKMARK_CLEAR) ClearEvidenceBookmarks();
    else if (command >= IDC_BOOKMARK_BASE && command < IDC_BOOKMARK_BASE + bookmarks.size())
        JumpToRawLine(bookmarks[command - IDC_BOOKMARK_BASE].lineNo);
}

void ShowExportMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDC_EXPORT_REPORT, UiText(TextId::ui_0158));
    AppendMenuW(menu, MF_STRING, IDC_EXPORT_HTML, UiText(TextId::ui_0159));
    AppendMenuW(menu, MF_STRING, IDC_EXPORT_CSV, UiText(TextId::ui_0160));
    RECT button{}; GetWindowRect(App().hExport, &button);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN,
                                        button.right, button.bottom + S(4), 0, App().hMain, nullptr);
    DestroyMenu(menu);
    if (command) SendMessageW(App().hMain,WM_COMMAND,command,0);
}

void FocusGlobalSearch() {
    if (!g_filtersExpanded) {
        g_filtersExpanded = true;
        SetFilterControlsVisible(true);
        UpdateFilterButton();
        Layout();
    }
    SetFocus(App().hGrepBox);
    SendMessageW(App().hGrepBox, EM_SETSEL, 0, -1);
}

void PersistUiSettings(HWND window) {
    AppSettings& settings = MutableAppSettings();
    settings.tagFilter = GetText(App().hTagBox);
    settings.grepFilter = GetText(App().hGrepBox);
    settings.sinceFilter = GetText(App().hSinceBox);
    settings.untilFilter = GetText(App().hUntilBox);
    settings.lastPage = CurrentPage();
    settings.filtersExpanded = g_filtersExpanded;
    settings.placement.length = sizeof(WINDOWPLACEMENT);
    settings.hasPlacement = GetWindowPlacement(window, &settings.placement) != FALSE;
    SaveAppSettings();
}

int NavWidth() { return S(232); }
int StatusHeight() { return S(32); }
bool HasText(HWND edit) { return edit && GetWindowTextLengthW(edit) > 0; }

void UpdateMetricViewButtons() {
    SetModernButtonActive(App().hMetricViewChart, g_metricViewMode == MetricViewMode::Chart);
    SetModernButtonActive(App().hMetricViewSplit, g_metricViewMode == MetricViewMode::Split);
    SetModernButtonActive(App().hMetricViewTable, g_metricViewMode == MetricViewMode::Table);
}

void SetMetricView(MetricViewMode mode) {
    g_metricViewMode = mode;
    UpdateMetricViewButtons();
    Layout();
    RedrawWindow(App().hMain, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

LRESULT CALLBACK SplitterSubclass(HWND splitter, UINT message, WPARAM wparam, LPARAM lparam,
                                   UINT_PTR, DWORD_PTR role) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(splitter, &paint);
        RECT rect{}; GetClientRect(splitter, &rect); FillSolid(dc, rect, th::page);
        const int center = (rect.left + rect.right) / 2;
        RECT grip{center - S(24), rect.bottom / 2, center + S(24), rect.bottom / 2 + std::max(1, S(1))};
        FillSolid(dc, grip, th::axis); EndPaint(splitter, &paint); return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_SIZENS)); return TRUE;
    case WM_LBUTTONDOWN:
        SetCapture(splitter);
        if (role == 1) g_dragMetricSplitter = true;
        else g_dragDetailSplitter = true;
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == splitter && (g_dragMetricSplitter || g_dragDetailSplitter)) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            MapWindowPoints(splitter, App().hMain, &point, 1);
            if (g_dragMetricSplitter) {
                g_metricSplitUserSized = true;
                g_metricSplitY = point.y - g_lastContent.top - S(38);
            } else
                g_detailHeight = g_lastContent.bottom - point.y - S(7);
            Layout();
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == splitter) ReleaseCapture();
        g_dragMetricSplitter = g_dragDetailSplitter = false;
        return 0;
    case WM_CAPTURECHANGED:
        g_dragMetricSplitter = g_dragDetailSplitter = false;
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(splitter, SplitterSubclass, 1); break;
    }
    return DefSubclassProc(splitter, message, wparam, lparam);
}

void UpdateFilterButton() {
    int count = App().document.timeRange.active ? 1 : 0;
    for (HWND edit : {App().hTagBox, App().hGrepBox, App().hSinceBox, App().hUntilBox})
        if (HasText(edit)) ++count;
    std::wstring label = UiText(TextId::ui_0000);
    if (count) label += L" (" + std::to_wstring(count) + L")";
    SetWindowTextW(App().hFilterToggle, label.c_str());
    SetModernButtonActive(App().hFilterToggle, g_filtersExpanded || count > 0);
}

void SetFilterControlsVisible(bool visible) {
    const int command = visible ? SW_SHOW : SW_HIDE;
    for (HWND control : {App().hTagLabel, App().hGrepLabel, App().hSinceLabel, App().hUntilLabel,
                         App().hTagBox, App().hGrepBox, App().hSinceBox, App().hUntilBox,
                         App().hApplyFilter, App().hClearFilter, App().hSearchHistory,
                         App().hMetricFilter})
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
    DWORD style = WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS;
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
    const auto& settings = GetAppSettings();
    const wchar_t* face = settings.uiFont.c_str();
    App().hFontUI = CreateAppFont(-14, FW_NORMAL, face, DEFAULT_PITCH | FF_DONTCARE);
    App().hFontMono = CreateAppFont(-13, FW_NORMAL, settings.logFont.c_str(), FIXED_PITCH | FF_MODERN);
    App().hFontTitle = CreateAppFont(-28, FW_SEMIBOLD, face, DEFAULT_PITCH | FF_DONTCARE);
    App().hFontSmall = CreateAppFont(-12, FW_NORMAL, face, DEFAULT_PITCH | FF_DONTCARE);
    App().hFontHero = CreateAppFont(-48, FW_SEMIBOLD, face, DEFAULT_PITCH | FF_DONTCARE);
    App().hFontTileVal = CreateAppFont(-22, FW_SEMIBOLD, face, DEFAULT_PITCH | FF_DONTCARE);
    App().hFontTileLbl = CreateAppFont(-13, FW_NORMAL, face, DEFAULT_PITCH | FF_DONTCARE);
    App().hFontSect = CreateAppFont(-16, FW_SEMIBOLD, face, DEFAULT_PITCH | FF_DONTCARE);
}

void CreateTableRowImageList() {
    if (App().hTableRows) {
        for (HWND list : {App().hTimeline, App().hOutage, App().hMetric, App().hTags,
                          App().hUnparsed, App().hRaw, App().hCells})
            if (list) ListView_SetImageList(list, nullptr, LVSIL_SMALL);
        ImageList_Destroy(App().hTableRows);
    }
    App().hTableRows = ImageList_Create(1, S(28), ILC_COLOR32 | ILC_MASK, 1, 1);
    if (!App().hTableRows) return;
    // 只使用图像列表的高度撑开行距，不添加未初始化的占位位图。
    // 非虚拟列表可能默认绘制第 0 张图片，原 1px 位图因此形成黑色竖线。
    for (HWND list : {App().hTimeline, App().hOutage, App().hMetric, App().hTags,
                      App().hUnparsed, App().hRaw, App().hCells})
        if (list) ListView_SetImageList(list, App().hTableRows, LVSIL_SMALL);
}

void ApplyFontsToControls() {
    for (HWND child = GetWindow(App().hMain, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontUI), TRUE);
    for (HWND control : {App().hTimeline, App().hOutage, App().hMetric, App().hTags,
                         App().hUnparsed, App().hRaw, App().hCells, App().hDetailText})
        if (control) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontMono), TRUE);
            if (control != App().hDetailText) {
                HWND header = ListView_GetHeader(control);
                if (header) SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontUI), TRUE);
            }
        }
    if (App().hPageTitle)
        SendMessageW(App().hPageTitle, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontTitle), TRUE);
    if (App().hFileLbl)
        SendMessageW(App().hFileLbl, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontSmall), TRUE);
    for (HWND label : {App().hTagLabel, App().hGrepLabel, App().hSinceLabel, App().hUntilLabel})
        if (label) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontSmall), TRUE);
    for (HWND label : {App().hMetricToolbar, App().hDetailLabel, App().hPageHint, App().hTimeRangeLabel})
        if (label) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontSmall), TRUE);
}

void DeleteFonts(const std::array<HFONT, 8>& fonts) {
    for (HFONT font : fonts) if (font) DeleteObject(font);
}

void ApplyAppearanceCommand(UINT command);

BOOL CALLBACK RelocalizeControl(HWND child, LPARAM) {
        wchar_t cls[64]{}; GetClassNameW(child,cls,64);
        if (lstrcmpiW(cls,L"BUTTON")==0 || lstrcmpiW(cls,L"STATIC")==0)
            SetWindowTextW(child,RelocalizeUiText(GetText(child)).c_str());
        if (lstrcmpiW(cls,WC_LISTVIEWW)==0) {
            const int count=Header_GetItemCount(ListView_GetHeader(child));
            for(int column=0;column<count;++column) {
                wchar_t text[256]{};LVCOLUMNW item{};item.mask=LVCF_TEXT;item.pszText=text;item.cchTextMax=256;
                ListView_GetColumn(child,column,&item);
                const std::wstring translated=RelocalizeUiText(text);
                item.pszText=const_cast<wchar_t*>(translated.c_str());ListView_SetColumn(child,column,&item);
            }
        }
        return TRUE;
}

void ApplyLanguage(bool english) {
    if (LoadInProgress()) return;
    CloseSourceComparison(); ClosePageDetail();
    MutableAppSettings().english = english; SetEnglish(english); SaveAppSettings();
    EnumChildWindows(App().hMain, RelocalizeControl, 0);
    SendMessageW(App().hTagBox,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(UiText(TextId::ui_0177)));
    SendMessageW(App().hGrepBox,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(UiText(TextId::ui_0178)));
    SetWindowTextW(App().hMain,(std::wstring(DL_APP_NAME_W L" v" DL_VER_WSTR)+UiText(TextId::ui_0220)).c_str());
    if (!App().document.lines.empty()) {
        const auto& doc=App().document;
        SetWindowTextW(App().hFileLbl,(AnalysisSourceText()+FmtW(UiText(TextId::ui_0018),
            static_cast<int>(doc.lines.size()),static_cast<int>(doc.sessions.size()),
            U8ToW(GeneratedText(doc.platform.name)).c_str())).c_str());
    }
    MarkAllPagesDirty(); RebuildMetricQuickFilterView(); RefreshBookmarkButton(); RefreshPresentation();
    ShowPage(CurrentPage()); UpdateFilterButton(); ApplyMetricColumnSettings(); Layout();
    RedrawWindow(App().hMain,nullptr,nullptr,RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}


void ShowAppearanceMenu() {
    HMENU menu = CreatePopupMenu();
    const auto& settings = GetAppSettings();
    const UINT availability=LoadInProgress()?MF_DISABLED:0;
    AppendMenuW(menu, MF_STRING | availability | (!settings.english?MF_CHECKED:0), IDC_LANGUAGE_ZH, L"中文（简体）");
    AppendMenuW(menu, MF_STRING | availability | (settings.english?MF_CHECKED:0), IDC_LANGUAGE_EN, L"English");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDC_FONT_UI, (UiText(TextId::ui_0161) + MenuSafe(settings.uiFont) + L"…").c_str());
    AppendMenuW(menu, MF_STRING, IDC_FONT_LOG, (UiText(TextId::ui_0162) + MenuSafe(settings.logFont) + L"…").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDC_FONT_RESET, UiText(TextId::ui_0163));
    RECT anchor{}; GetWindowRect(App().hAppearance, &anchor);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                                        anchor.left, anchor.top - S(4), 0, App().hMain, nullptr);
    DestroyMenu(menu);
    if (command==IDC_LANGUAGE_ZH || command==IDC_LANGUAGE_EN) ApplyLanguage(command==IDC_LANGUAGE_EN);
    else if (command) ApplyAppearanceCommand(command);
}

void ApplyAppearanceCommand(UINT command) {
    CloseSourceComparison();
    auto& preferences = MutableAppSettings();
    if (command == IDC_FONT_RESET) {
        preferences.uiFont = L"Microsoft YaHei UI"; preferences.logFont = L"Consolas";
    } else {
        LOGFONTW font{};
        GetObjectW(command == IDC_FONT_UI ? App().hFontUI : App().hFontMono, sizeof(font), &font);
        CHOOSEFONTW choice{}; choice.lStructSize = sizeof(choice); choice.hwndOwner = App().hMain;
        choice.lpLogFont = &font;
        choice.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSIZESEL | CF_NOSTYLESEL | CF_NOVERTFONTS;
        if (command == IDC_FONT_LOG) choice.Flags |= CF_FIXEDPITCHONLY;
        if (!ChooseFontW(&choice)) return;
        (command == IDC_FONT_UI ? preferences.uiFont : preferences.logFont) = font.lfFaceName;
    }
    const std::array<HFONT, 8> old{App().hFontUI, App().hFontMono, App().hFontTitle, App().hFontSmall,
                                  App().hFontHero, App().hFontTileVal, App().hFontTileLbl, App().hFontSect};
    CreateFonts(); ApplyFontsToControls(); DeleteFonts(old);
    SaveAppSettings(); Layout();
    RedrawWindow(App().hMain, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

RECT ContentRect(const RECT& client) {
    return RECT{NavWidth() + S(20), S(g_headerHeight + 14), client.right - S(20),
                client.bottom - StatusHeight() - S(14)};
}

void MoveIf(HWND window, int x, int y, int width, int height) {
    if (window) MoveWindow(window, x, y, std::max(1, width), std::max(1, height), TRUE);
}

void MoveFilterEdit(HWND edit, int x, int y, int width, int height) {
    for (auto& frame : g_filterFrames) {
        if (!frame.edit || frame.edit == edit) {
            frame.edit = edit; frame.rect = RECT{x, y, x + width, y + height};
            break;
        }
    }
    HDC dc = GetDC(edit);
    HGDIOBJ old = SelectObject(dc, App().hFontUI);
    TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old); ReleaseDC(edit, dc);
    const int textHeight = std::clamp(static_cast<int>(metrics.tmHeight) + S(2), 1, height);
    // 保持原生单行 EDIT 的输入、光标、选区和 IME 行为，编辑区放在外框中央。
    MoveIf(edit, x, y + (height - textHeight) / 2, width, textHeight);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(9), S(9)));
}

LRESULT CALLBACK FilterEditSubclass(HWND edit, UINT message, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR, DWORD_PTR) {
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        for (const auto& frame : g_filterFrames) if (frame.edit == edit) {
            RECT rect = frame.rect; InflateRect(&rect, S(2), S(2));
            InvalidateRect(App().hMain, &rect, FALSE);
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(edit, FilterEditSubclass, 1);
    return DefSubclassProc(edit, message, wparam, lparam);
}

void LayoutFilterPanel(int contentLeft, int contentWidth) {
    const int x = contentLeft + S(20);
    const int y = S(78);
    const int width = contentWidth - S(40);
    // 宽布局需要容纳 4 个字段、3 个操作按钮和“指标筛选”。保留至少 150px
    // 给消息搜索框；不足时切到三行布局，避免 100%/高 DPI 下末端按钮越界。
    const bool compact = width < S(970);
    g_headerHeight = compact ? 274 : 164;
    g_filterPanel = RECT{x, y, x + width, S(g_headerHeight - 14)};
    if (!g_filtersExpanded) {
        g_headerHeight = 88;
        SetRectEmpty(&g_filterPanel);
        return;
    }

    const int pad = S(14), labelH = S(17), editH = S(34), gap = S(10);
    if (!compact) {
        int available = width - pad * 2;
        int fixed = S(135 + 120 + 120 + 82 + 74 + 76 + 112) + gap * 7;
        int searchW = std::max(S(150), available - fixed);
        int cursor = x + pad;
        struct Field { HWND label; HWND edit; int width; } fields[] = {
            {App().hTagLabel, App().hTagBox, S(135)}, {App().hGrepLabel, App().hGrepBox, searchW},
            {App().hSinceLabel, App().hSinceBox, S(120)}, {App().hUntilLabel, App().hUntilBox, S(120)}};
        for (const auto& field : fields) {
            MoveIf(field.label, cursor, y + S(9), field.width, labelH);
            MoveFilterEdit(field.edit, cursor, y + S(29), field.width, editH);
            cursor += field.width + gap;
        }
        MoveIf(App().hApplyFilter, cursor, y + S(28), S(82), S(36)); cursor += S(82) + gap;
        MoveIf(App().hClearFilter, cursor, y + S(28), S(74), S(36)); cursor += S(74) + gap;
        MoveIf(App().hSearchHistory, cursor, y + S(28), S(76), S(36)); cursor += S(76) + gap;
        MoveIf(App().hMetricFilter, cursor, y + S(28), S(112), S(36));
        return;
    }

    const int inner = width - pad * 2;
    const int tagW = std::max(S(120), inner * 35 / 100);
    int cursor = x + pad;
    MoveIf(App().hTagLabel, cursor, y + S(9), tagW, labelH);
    MoveFilterEdit(App().hTagBox, cursor, y + S(29), tagW, editH); cursor += tagW + gap;
    MoveIf(App().hGrepLabel, cursor, y + S(9), inner - tagW - gap, labelH);
    MoveFilterEdit(App().hGrepBox, cursor, y + S(29), inner - tagW - gap, editH);

    const int row2 = y + S(74);
    const int dateW = std::max(S(96), (inner - gap) / 2);
    cursor = x + pad;
    MoveIf(App().hSinceLabel, cursor, row2, dateW, labelH);
    MoveFilterEdit(App().hSinceBox, cursor, row2 + S(20), dateW, editH); cursor += dateW + gap;
    MoveIf(App().hUntilLabel, cursor, row2, dateW, labelH);
    MoveFilterEdit(App().hUntilBox, cursor, row2 + S(20), dateW, editH);
    const int row3 = y + S(137);
    cursor = x + pad;
    MoveIf(App().hApplyFilter, cursor, row3, S(82), S(36)); cursor += S(82) + gap;
    MoveIf(App().hClearFilter, cursor, row3, S(74), S(36)); cursor += S(74) + gap;
    MoveIf(App().hSearchHistory, cursor, row3, S(86), S(36)); cursor += S(86) + gap;
    MoveIf(App().hMetricFilter, cursor, row3, std::max(S(112), inner - (cursor - x - pad)), S(36));
}

void Layout() {
    if (!App().hMain) return;
    RECT client{}; GetClientRect(App().hMain, &client);
    const int navW = NavWidth(), statusH = StatusHeight();
    const int contentLeft = navW, contentWidth = client.right - navW;
    MoveIf(App().hNav, 0, 0, navW, client.bottom);
    MoveIf(App().hStatus, contentLeft, client.bottom - statusH, contentWidth, statusH);

    const bool compactCommands = client.right < S(1120);
    const bool narrowCommands = client.right < S(960);
    ShowWindow(App().hPaste, compactCommands ? SW_HIDE : SW_SHOW);
    ShowWindow(App().hBookmarks, narrowCommands ? SW_HIDE : SW_SHOW);
    int right = client.right - S(20);
    auto command = [&](HWND button, int width) {
        right -= S(width); MoveIf(button, right, S(20), S(width), S(36)); right -= S(8);
    };
    command(App().hCloseLog, compactCommands ? 76 : 82);
    if (!narrowCommands) command(App().hBookmarks, compactCommands ? 78 : 90);
    command(App().hFilterToggle, compactCommands ? 72 : 82);
    if (!compactCommands) command(App().hPaste, 92);
    command(App().hOpen, compactCommands ? 106 : 122);
    command(App().hExport, compactCommands ? 82 : 92);
    MoveIf(App().hPageTitle, contentLeft + S(24), S(12), std::max(S(32), right - contentLeft - S(32)), S(38));
    MoveIf(App().hAppearance, S(20), client.bottom - S(84), navW - S(40), S(36));
    SetWindowPos(App().hAppearance, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    MoveIf(App().hFileLbl, contentLeft + S(25), S(51), contentWidth - S(50), S(20));

    LayoutFilterPanel(contentLeft, contentWidth);
    const int rangeY = S(g_headerHeight);
    MoveIf(App().hSources, contentLeft + S(20), rangeY + S(4), S(112), S(34));
    MoveIf(App().hTimeRangeLabel, contentLeft + S(142), rangeY,
           contentWidth - S(308), S(42));
    MoveIf(App().hTimeRangeReset, client.right - S(150), rangeY + S(4), S(130), S(34));
    g_headerHeight += 52;
    RECT content = ContentRect(client);
    g_lastContent = content;
    const int width = static_cast<int>(content.right - content.left);
    const int height = std::max(S(40), static_cast<int>(content.bottom - content.top));

    int pageHeight = height;
    const int pageHintHeight = CurrentPage() == 2 || CurrentPage() == 3 || CurrentPage() == 6 ? S(30) : 0;
    MoveIf(App().hPageHint, content.left, content.top, width, S(26));
    const bool detailPage = CurrentPage() == 2 || CurrentPage() == 4 || CurrentPage() == 6;
    if (PageDetailVisible() && detailPage) {
        const int minimumDetail = std::min(S(120), std::max(S(60), height / 3));
        const int desiredTop = CurrentPage() == 4 ? S(245) : S(150);
        const int minimumTop = std::min(desiredTop,
            std::max(S(80), height - minimumDetail - S(7)));
        const int maximumDetail = std::max(1, height - minimumTop - S(7));
        const int effectiveMinimum = std::min(minimumDetail, maximumDetail);
        if (g_detailHeight <= 0) g_detailHeight = std::min(S(220), height * 35 / 100);
        g_detailHeight = std::max(effectiveMinimum,
                                  std::min(g_detailHeight, maximumDetail));
        pageHeight = height - g_detailHeight - S(7);
        MoveIf(App().hDetailSplitter, content.left, content.top + pageHeight, width, S(7));
        MoveIf(App().hDetailLabel, content.left + S(14), content.top + pageHeight + S(13),
               width - S(66), S(24));
        MoveIf(App().hDetailClose, content.right - S(42), content.top + pageHeight + S(9),
               S(34), S(30));
        MoveIf(App().hDetailText, content.left, content.top + pageHeight + S(43),
               width, g_detailHeight - S(36));
    }

    int dashHeight = std::min(S(360), pageHeight - S(90));
    dashHeight = std::max(S(130), dashHeight);
    MoveIf(App().hDash, content.left, content.top, width, dashHeight);
    MoveIf(App().hSummary, content.left, content.top + dashHeight, width, pageHeight - dashHeight);
    for (HWND page : {App().hFindings, App().hTimeline, App().hOutage, App().hTags,
                      App().hRaw, App().hUnparsed, App().hCells}) {
        const int offset = page == App().hTimeline || page == App().hOutage || page == App().hRaw ? pageHintHeight : 0;
        MoveIf(page, content.left, content.top + offset, width, pageHeight - offset);
    }

    const int toolbarHeight = S(38), splitterHeight = S(7);
    int buttonRight = content.right - S(8);
    auto viewButton = [&](HWND button, int logicalWidth) {
        buttonRight -= S(logicalWidth);
        MoveIf(button, buttonRight, content.top + S(3), S(logicalWidth), S(32));
        buttonRight -= S(6);
    };
    viewButton(App().hMetricViewTable, 74);
    viewButton(App().hMetricViewSplit, 62);
    viewButton(App().hMetricViewChart, 62);
    viewButton(App().hMetricColumns, 78);
    MoveIf(App().hMetricToolbar, content.left, content.top,
           std::max(1, buttonRight - static_cast<int>(content.left)), toolbarHeight);
    const int metricTop = content.top + toolbarHeight;
    const int metricAreaHeight = std::max(S(80), pageHeight - toolbarHeight);
    if (g_metricViewMode == MetricViewMode::Chart) {
        ShowWindow(App().hChart, CurrentPage() == 4 ? SW_SHOW : SW_HIDE);
        ShowWindow(App().hMetric, SW_HIDE);
        ShowWindow(App().hMetricSplitter, SW_HIDE);
        MoveIf(App().hChart, content.left, metricTop, width, metricAreaHeight);
    } else if (g_metricViewMode == MetricViewMode::Table) {
        ShowWindow(App().hChart, SW_HIDE);
        ShowWindow(App().hMetric, CurrentPage() == 4 ? SW_SHOW : SW_HIDE);
        ShowWindow(App().hMetricSplitter, SW_HIDE);
        MoveIf(App().hMetric, content.left, metricTop, width, metricAreaHeight);
    } else {
        ShowWindow(App().hChart, CurrentPage() == 4 ? SW_SHOW : SW_HIDE);
        ShowWindow(App().hMetric, CurrentPage() == 4 ? SW_SHOW : SW_HIDE);
        ShowWindow(App().hMetricSplitter, CurrentPage() == 4 ? SW_SHOW : SW_HIDE);
        const int usableHeight = std::max(2, metricAreaHeight - splitterHeight);
        const int minimumTable = std::min(std::max(1, usableHeight / 3),
            usableHeight >= S(390) ? S(100) : S(60));
        const int minimumChart = std::min(PreferredChartHeight(), std::max(1, usableHeight - minimumTable));
        if (!g_metricSplitUserSized || g_metricSplitY <= 0) g_metricSplitY = usableHeight * 72 / 100;
        g_metricSplitY = std::max(minimumChart,
            std::min(g_metricSplitY, usableHeight - minimumTable));
        MoveIf(App().hChart, content.left, metricTop, width, g_metricSplitY);
        MoveIf(App().hMetricSplitter, content.left, metricTop + g_metricSplitY, width, splitterHeight);
        MoveIf(App().hMetric, content.left, metricTop + g_metricSplitY + splitterHeight,
               width, metricAreaHeight - g_metricSplitY - splitterHeight);
    }
    FitPrimaryTableColumns(width);
    LayoutModernOverlays();
    InvalidateRect(App().hMain, nullptr, FALSE);
}

void PaintInputFrame(HDC dc, HWND edit) {
    if (!edit || !IsWindowVisible(edit)) return;
    RECT rect{}; GetWindowRect(edit, &rect);
    MapWindowPoints(nullptr, App().hMain, reinterpret_cast<POINT*>(&rect), 2);
    for (const auto& frame : g_filterFrames) if (frame.edit == edit) { rect = frame.rect; break; }
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
    if (source == App().hSinceBox || source == App().hUntilBox)
        App().document.timeRange = DocumentState::TimeRange{};
    // EN_CHANGE can arrive inside EDIT's WM_SETTEXT. Read the four edit controls
    // after that notification returns, and coalesce a batch of cleared fields.
    if (!g_filterButtonRefreshPending)
        g_filterButtonRefreshPending = PostMessageW(App().hMain,kRefreshFilterButton,0,0)!=FALSE;
    if (!App().document.lines.empty()) {
        KillTimer(App().hMain, kFilterTimer);
        SetTimer(App().hMain, kFilterTimer, 450, nullptr);
    }
}

void ClearFilters(bool refresh) {
    KillTimer(App().hMain, kFilterTimer);
    App().document.timeRange = DocumentState::TimeRange{};
    for (HWND edit : {App().hTagBox, App().hGrepBox, App().hSinceBox, App().hUntilBox})
        SetWindowTextW(edit, L"");
    KillTimer(App().hMain, kFilterTimer);
    ClearMetricQuickFilters(false);
    UpdateFilterButton();
    UpdateAnalysisTimeRangeControls();
    if (refresh) RefreshAll();
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
        App().hPageTitle = CreateControl(L"STATIC", UiText(TextId::ui_0164), SS_LEFT | SS_ENDELLIPSIS,
                                         IDC_PAGETITLE, App().hFontTitle);
        App().hFileLbl = CreateControl(L"STATIC",
            UiText(TextId::ui_0165),
            SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, IDC_FILELBL, App().hFontSmall);
        App().hPageHint = CreateControl(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                                       IDC_PAGE_HINT, App().hFontSmall, false);
        App().hSources = CreateButton(UiText(TextId::ui_0166), IDC_SOURCES, ModernButtonKind::Neutral);
        App().hTimeRangeLabel = CreateControl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX,
                                             IDC_TIME_RANGE_LABEL, App().hFontSmall);
        App().hTimeRangeReset = CreateButton(UiText(TextId::ui_0167), IDC_TIME_RANGE_RESET, ModernButtonKind::Neutral);
        App().hOpen = CreateButton(UiText(TextId::ui_0168), IDC_OPEN, ModernButtonKind::Primary);
        App().hPaste = CreateButton(UiText(TextId::ui_0169), IDC_PASTE, ModernButtonKind::Neutral);
        App().hFilterToggle = CreateButton(UiText(TextId::ui_0000), IDC_FILTER, ModernButtonKind::Neutral);
        App().hCloseLog = CreateButton(UiText(TextId::ui_0054), IDC_CLOSELOG, ModernButtonKind::Danger);
        App().hBookmarks = CreateButton(UiText(TextId::ui_0170), IDC_BOOKMARKS, ModernButtonKind::Neutral);
        App().hAppearance = CreateButton(UiText(TextId::ui_0171), IDC_APPEARANCE, ModernButtonKind::Neutral);
        EnableWindow(App().hBookmarks, FALSE);
        App().hExport = CreateButton(UiText(TextId::ui_0172), IDC_EXPORT, ModernButtonKind::Neutral);

        App().hTagLabel = CreateControl(L"STATIC", UiText(TextId::ui_0173), SS_LEFT, 0, App().hFontSmall, false);
        App().hGrepLabel = CreateControl(L"STATIC", UiText(TextId::ui_0174), SS_LEFT, 0, App().hFontSmall, false);
        App().hSinceLabel = CreateControl(L"STATIC", UiText(TextId::ui_0175), SS_LEFT, 0, App().hFontSmall, false);
        App().hUntilLabel = CreateControl(L"STATIC", UiText(TextId::ui_0176), SS_LEFT, 0, App().hFontSmall, false);
        App().hTagBox = CreateControl(L"EDIT", L"", ES_AUTOHSCROLL, IDC_TAGBOX, App().hFontUI, false);
        App().hGrepBox = CreateControl(L"EDIT", L"", ES_AUTOHSCROLL, IDC_GREPBOX, App().hFontUI, false);
        App().hSinceBox = CreateControl(L"EDIT", L"", ES_AUTOHSCROLL, IDC_SINCEBOX, App().hFontUI, false);
        App().hUntilBox = CreateControl(L"EDIT", L"", ES_AUTOHSCROLL, IDC_UNTILBOX, App().hFontUI, false);
        for (HWND edit : {App().hTagBox, App().hGrepBox, App().hSinceBox, App().hUntilBox}) {
            SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(9), S(9)));
            SetWindowSubclass(edit, FilterEditSubclass, 1, 0);
        }
        SendMessageW(App().hTagBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(UiText(TextId::ui_0177)));
        SendMessageW(App().hGrepBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(UiText(TextId::ui_0178)));
        SendMessageW(App().hSinceBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"HH:MM:SS"));
        SendMessageW(App().hUntilBox, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"HH:MM:SS"));
        App().hApplyFilter = CreateButton(UiText(TextId::ui_0179), IDC_APPLY, ModernButtonKind::Primary, false);
        App().hClearFilter = CreateButton(UiText(TextId::ui_0180), IDC_CLEAR, ModernButtonKind::Neutral, false);
        App().hSearchHistory = CreateButton(UiText(TextId::ui_0181), IDC_SEARCH_HISTORY, ModernButtonKind::Neutral, false);
        App().hMetricFilter = CreateButton(UiText(TextId::ui_0182), IDC_METRIC_FILTER, ModernButtonKind::Neutral, false);

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
        App().hUnparsed = CreateList(IDC_UNPARSED, {{UiText(TextId::ui_0183), 90}, {UiText(TextId::ui_0184), 960}});
        App().hTimeline = CreateList(IDC_TIMELINE, {{UiText(TextId::ui_0185), 186}, {UiText(TextId::ui_0186), 100}, {UiText(TextId::ui_0173), 126}, {UiText(TextId::ui_0187), 820}}, true);
        App().hOutage = CreateList(IDC_OUTAGE, {{L"#", 44}, {UiText(TextId::ui_0188), 144}, {UiText(TextId::ui_0189), 186}, {UiText(TextId::ui_0190), 186}, {UiText(TextId::ui_0191), 90}, {UiText(TextId::ui_0192), 160}});
        App().hMetric = CreateList(IDC_METRIC, {{UiText(TextId::ui_0185), 186}, {L"CH", 82},
            {UiText(TextId::ui_0193), 105}, {L"PCI", 58}, {L"TAC", 70}, {L"CSQ", 58}, {L"Tmax", 58},
            {L"ConsecFail", 86}, {L"RX_PKT", 105}, {L"ΔRX", 76}, {L"RSRP", 68}, {L"RSRQ", 68},
            {L"SNR(dB)", 78}, {L"RSSI", 68}, {L"SRV", 52}, {L"RAT", 76}, {L"DENY", 58}, {L"OPER", 145},
            {UiText(TextId::ui_0194), 155}, {UiText(TextId::ui_0195), 88}, {UiText(TextId::ui_0196), 76}, {UiText(TextId::ui_0197), 112}}, true);
        App().hTags = CreateList(IDC_TAGS, {{UiText(TextId::ui_0173), 150}, {UiText(TextId::ui_0198), 80}, {UiText(TextId::ui_0199), 600}});
        App().hRaw = CreateList(IDC_RAW, {{UiText(TextId::ui_0183), 90}, {UiText(TextId::ui_0185), 186}, {UiText(TextId::ui_0200), 82},
            {UiText(TextId::ui_0173), 118}, {UiText(TextId::ui_0201), 900}}, true);
        App().hCells = CreateList(IDC_CELLS, {{UiText(TextId::ui_0193), 112}, {L"PCI", 58}, {L"TAC", 72},
            {UiText(TextId::ui_0202), 66}, {UiText(TextId::ui_0203), 76}, {UiText(TextId::ui_0204), 92}, {UiText(TextId::ui_0205), 86},
            {UiText(TextId::ui_0206), 86}, {UiText(TextId::ui_0207), 86}, {UiText(TextId::ui_0208), 82}, {UiText(TextId::ui_0209), 78},
            {UiText(TextId::ui_0210), 58}, {UiText(TextId::ui_0211), 58}, {UiText(TextId::ui_0212), 82}, {UiText(TextId::ui_0213), 100}}, true);
        App().hChart = CreateWindowExW(0, L"dialChartCls", L"", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHART)),
                                       GetModuleHandleW(nullptr), nullptr);
        for (HWND list : {App().hTimeline, App().hOutage, App().hMetric, App().hTags,
                          App().hUnparsed, App().hRaw, App().hCells})
            ConfigurePageList(list);

        App().hMetricToolbar = CreateControl(L"STATIC", UiText(TextId::ui_0214),
            SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, 0, App().hFontSmall, false);
        App().hMetricColumns = CreateButton(UiText(TextId::ui_0215), IDC_METRIC_COLUMNS, ModernButtonKind::Neutral, false);
        ApplyMetricColumnSettings();
        App().hMetricViewChart = CreateButton(UiText(TextId::ui_0216), IDC_METRIC_VIEW_CHART,
                                               ModernButtonKind::Neutral, false);
        App().hMetricViewSplit = CreateButton(UiText(TextId::ui_0217), IDC_METRIC_VIEW_SPLIT,
                                               ModernButtonKind::Neutral, false);
        App().hMetricViewTable = CreateButton(UiText(TextId::ui_0218), IDC_METRIC_VIEW_TABLE,
                                               ModernButtonKind::Neutral, false);
        App().hMetricSplitter = CreateControl(L"STATIC", L"", SS_NOTIFY | SS_ETCHEDHORZ,
                                               0, App().hFontUI, false);
        App().hDetailSplitter = CreateControl(L"STATIC", L"", SS_NOTIFY | SS_ETCHEDHORZ,
                                               0, App().hFontUI, false);
        SetWindowSubclass(App().hMetricSplitter, SplitterSubclass, 1, 1);
        SetWindowSubclass(App().hDetailSplitter, SplitterSubclass, 1, 2);
        App().hDetailLabel = CreateControl(L"STATIC", UiText(TextId::ui_0219), SS_LEFT | SS_CENTERIMAGE,
                                            0, App().hFontSmall, false);
        App().hDetailClose = CreateButton(L"×", IDC_DETAIL_CLOSE, ModernButtonKind::Neutral, false);
        App().hDetailText = CreateControl(L"EDIT", L"",
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY | WS_VSCROLL | WS_HSCROLL,
            IDC_DETAIL_TEXT, App().hFontMono, false);
        SendMessageW(App().hDetailText, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(S(10), S(10)));
        UpdateMetricViewButtons();
        App().hStatus = CreateModernStatus(hwnd, IDC_STATUS);

        const AppSettings& settings = GetAppSettings();
        SetWindowTextW(App().hTagBox, settings.tagFilter.c_str());
        SetWindowTextW(App().hGrepBox, settings.grepFilter.c_str());
        SetWindowTextW(App().hSinceBox, settings.sinceFilter.c_str());
        SetWindowTextW(App().hUntilBox, settings.untilFilter.c_str());
        g_filtersExpanded = settings.filtersExpanded;
        SetFilterControlsVisible(g_filtersExpanded);

        ApplyModernTheme(hwnd);
        ShowPage(settings.lastPage);
        UpdateFilterButton();
        DragAcceptFiles(hwnd, TRUE);
        UpdateAnalysisTimeRangeControls();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintShell(hwnd); return 0;
    case WM_LBUTTONDOWN: {
        if (g_filtersExpanded) {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            for (const auto& frame : g_filterFrames) if (frame.edit && PtInRect(&frame.rect, point)) {
                SetFocus(frame.edit); return 0;
            }
        }
        break;
    }
    case WM_SIZE: Layout(); return 0;
    case WM_APP_NAVIGATE: ShowPage(static_cast<int>(wparam)); return 0;
    case kRefreshFilterButton:
        g_filterButtonRefreshPending = false; UpdateFilterButton(); return 0;
    case WM_APP_SHELL_LAYOUT: UpdateFilterButton(); Layout(); return 0;
    case WM_APP_LOAD_PROGRESS:
    case WM_APP_LOAD_COMPLETE:
        HandleLoadControllerMessage(message, wparam, lparam); return 0;
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
        // The owned comparison uses the shared UI font; release its controls
        // before replacing that font, just as for an explicit font change.
        CloseSourceComparison();
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
        info->ptMinTrackSize.x = S(860); info->ptMinTrackSize.y = S(640); return 0;
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
        if (ApplyMetricColumnCommand(id) || HandleSourceCommand(id)) return 0;
        if (code == EN_CHANGE) ScheduleFilterRefresh(reinterpret_cast<HWND>(lparam));
        switch (id) {
        case IDC_LANGUAGE_ZH: case IDC_LANGUAGE_EN: ApplyLanguage(id==IDC_LANGUAGE_EN); return 0;
        case IDC_APPEARANCE: ShowAppearanceMenu(); return 0;
        case IDC_FONT_UI: case IDC_FONT_LOG: case IDC_FONT_RESET: ApplyAppearanceCommand(id); return 0;
        case IDC_OPEN: ShowOpenMenu(); return 0;
        case IDC_OPEN_PICK: DoOpen(); return 0;
        case IDC_PASTE: DoPaste(); return 0;
        case IDC_BOOKMARKS: ShowBookmarksMenu(); return 0;
        case IDC_SEARCH_HISTORY: ShowSearchHistoryMenu(); return 0;
        case IDC_SOURCES: ShowSourceMenu(App().hSources); return 0;
        case IDC_METRIC_COLUMNS: ShowMetricColumnMenu(App().hMetricColumns); return 0;
        case IDC_METRIC_FILTER: ShowMetricQuickFilterMenu(App().hMetricFilter); return 0;
        case IDC_METRIC_VIEW_CHART: SetMetricView(MetricViewMode::Chart); return 0;
        case IDC_METRIC_VIEW_SPLIT: SetMetricView(MetricViewMode::Split); return 0;
        case IDC_METRIC_VIEW_TABLE: SetMetricView(MetricViewMode::Table); return 0;
        case IDC_DETAIL_CLOSE: ClosePageDetail(); return 0;
        case IDC_TIME_RANGE_RESET:
            RestoreAnalysisTimeRange(); KillTimer(hwnd, kFilterTimer); return 0;
        case IDC_FILTER:
            g_filtersExpanded = !g_filtersExpanded;
            SetFilterControlsVisible(g_filtersExpanded); UpdateFilterButton(); Layout();
            if (g_filtersExpanded) SetFocus(App().hTagBox);
            return 0;
        case IDC_APPLY:
            KillTimer(hwnd, kFilterTimer); RememberSearchQuery(GetText(App().hGrepBox)); RefreshAll(); return 0;
        case IDC_EXPORT: ShowExportMenu(); return 0;
        case IDC_EXPORT_REPORT: DoExportReport(); return 0;
        case IDC_EXPORT_HTML: DoExportHtml(); return 0;
        case IDC_EXPORT_CSV: DoExportCsv(); return 0;
        case IDC_CLEAR: ClearFilters(true); return 0;
        case IDC_CLOSELOG:
            if (ConsumeLoadActionClick()) return 0;
            ReleaseLoadedData(); ClearFilters(false); MarkAllPagesDirty(); RenderPage(CurrentPage());
            SetWindowTextW(App().hFileLbl, UiText(TextId::ui_0165));
            SetWindowTextW(App().hStatus, UiText(TextId::ui_0011)); RefreshNavigation(); return 0;
        }
        if (id >= IDC_RECENT_BASE && id < IDC_RECENT_BASE + 5) {
            OpenRecent(static_cast<size_t>(id - IDC_RECENT_BASE)); return 0;
        }
        return 0;
    }
    case WM_NOTIFY: {
        LRESULT result = 0; return HandlePageNotify(lparam, result) ? result : 0;
    }
    case WM_CLOSE:
        PersistUiSettings(hwnd);
        ShutdownLoadController();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        KillTimer(hwnd, kFilterTimer);
        std::array<HFONT, 8> fonts{App().hFontUI, App().hFontMono, App().hFontTitle, App().hFontSmall,
                                   App().hFontHero, App().hFontTileVal, App().hFontTileLbl, App().hFontSect};
        if (App().hTableRows) {
            for (HWND list : {App().hTimeline, App().hOutage, App().hMetric, App().hTags,
                              App().hUnparsed, App().hRaw, App().hCells})
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
    LoadAppSettings();
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
        (std::wstring(DL_APP_NAME_W L" v" DL_VER_WSTR) + UiText(TextId::ui_0220)).c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    if (App().dpi != 96)
        SetWindowPos(window, nullptr, 0, 0, MulDiv(1280, App().dpi, 96), MulDiv(820, App().dpi, 96),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    int showCommand = show;
    AppSettings& settings = MutableAppSettings();
    if (settings.hasPlacement &&
        MonitorFromRect(&settings.placement.rcNormalPosition, MONITOR_DEFAULTTONULL)) {
        if (settings.placement.showCmd != SW_SHOWMAXIMIZED &&
            settings.placement.showCmd != SW_MAXIMIZE)
            settings.placement.showCmd = SW_SHOWNORMAL;
        SetWindowPlacement(window, &settings.placement);
        showCommand = settings.placement.showCmd;
    }
    ShowWindow(window, showCommand); UpdateWindow(window);

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
            if (page >= 0 && page < 9) ShowPage(page);
        }
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (RouteSourceComparisonMessage(message)) continue;
        if (message.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (message.wParam == 'C') {
                HWND focus = GetFocus();
                const bool editing = focus == App().hTagBox || focus == App().hGrepBox ||
                    focus == App().hSinceBox || focus == App().hUntilBox || focus == App().hDetailText;
                if (SourceComparisonActive()) { CopySourceComparison(); continue; }
                if (!editing && (CopyOverviewPage(CurrentPage()) || CopySelectedPageRows())) continue;
            }
            if (message.wParam == 'O') { DoOpen(); continue; }
            if (message.wParam == 'F') { FocusGlobalSearch(); continue; }
            if (message.wParam == '0') {
                RestoreAnalysisTimeRange(); KillTimer(window, kFilterTimer); continue;
            }
            if (message.wParam == 'B') { ToggleCurrentRawBookmark(); continue; }
            if (message.wParam >= '1' && message.wParam <= '9') {
                ShowPage(static_cast<int>(message.wParam - '1')); continue;
            }
        }
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
                KillTimer(window, kFilterTimer);
                RememberSearchQuery(GetText(App().hGrepBox));
                RefreshAll(); continue;
            }
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            if (CancelChartSelection()) continue;
            if (PageDetailVisible()) { ClosePageDetail(); continue; }
            if (g_filtersExpanded) {
                g_filtersExpanded = false; SetFilterControlsVisible(false); UpdateFilterButton(); Layout(); continue;
            }
        }
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return 0;
}
