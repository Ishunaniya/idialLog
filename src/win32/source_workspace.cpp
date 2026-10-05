#include "source_workspace.h"
#include "rssi_summary.h"
#include "workspace_window.h"
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include "app_context.h"
#include "load_controller.h"
#include "ui_pages.h"
#include "win_text.h"
#include "text_catalog.h"
#include "log_time.h"
#include "theme.h"
#include "modern_shell.h"

namespace dl {
namespace {
HWND g_comparison = nullptr;
constexpr int kFirst = 1160, kSecond = 1161, kCompareList = 1162;
const wchar_t* const* Fields() { static const wchar_t* fields[18]; const wchar_t* values[] = {UiText(TextId::ui_0244), UiText(TextId::ui_0245), UiText(TextId::ui_0246), UiText(TextId::ui_0247), UiText(TextId::ui_0248), UiText(TextId::ui_0249),
    UiText(TextId::ui_0250), UiText(TextId::ui_0251), UiText(TextId::ui_0252), UiText(TextId::ui_0253), UiText(TextId::ui_0254), UiText(TextId::ui_0255),
    UiText(TextId::ui_0256), UiText(TextId::ui_0257), UiText(TextId::ui_0258), UiText(TextId::ui_0259), UiText(TextId::rssi_cell_range), UiText(TextId::rssi_samples)}; std::copy(std::begin(values),std::end(values),fields);return fields; }

std::array<std::wstring, 18> Values(std::size_t index) {
    const auto& doc = App().document;
    std::array<std::wstring, 18> result;
    result.fill(L"—");
    if (index >= doc.comparisons.size()) return result;
    const auto& stats = doc.comparisons[index];
    result[0] = doc.sources[index].label;
    if (stats.lines) {
        result[1] = U8ToW(fmtTime(stats.firstTime, "FULL"));
        result[2] = U8ToW(fmtTime(stats.lastTime, "FULL"));
        result[3] = U8ToW(fmtDur(stats.observedSeconds));
    }
    result[4] = std::to_wstring(stats.lines); result[5] = std::to_wstring(stats.samples);
    result[6] = std::to_wstring(stats.outages); result[7] = std::to_wstring(stats.unrecovered);
    result[8] = U8ToW(fmtDur(stats.outageSeconds));
    if (stats.availability.runtimeValid()) result[9] = FmtW(L"%.3f%%%s", stats.availability.runtimePercent(),
        stats.availability.evidenceLimited() ? UiText(TextId::ui_0260) : L"");
    if (stats.availability.fullValid()) result[10] = FmtW(L"%.3f%%%s", stats.availability.fullPercent(),
        stats.availability.evidenceLimited() ? UiText(TextId::ui_0260) : L"");
    if (stats.availability.connectedStartupSegments) result[11] = U8ToW(fmtDur(stats.availability.longestStartupSeconds));
    if (stats.rsrpSamples) result[12] = FmtW(L"%d / %d / %d dBm", stats.rsrpAverage, stats.rsrpMin, stats.rsrpMax);
    result[13] = std::to_wstring(stats.rsrpSamples);
    if (stats.csqSamples) result[14] = FmtW(L"%d / %d / %d", stats.csqAverage, stats.csqMin, stats.csqMax);
    result[15] = std::to_wstring(stats.csqSamples);
    if (stats.rssi.samples) result[16] = U8ToW(rssiRangeText(stats.rssi)) + L" dBm";
    result[17] = std::to_wstring(stats.rssi.samples);
    return result;
}

void RenderComparison(HWND window) {
    HWND table = GetDlgItem(window, kCompareList);
    const auto a = static_cast<std::size_t>(SendDlgItemMessageW(window, kFirst, CB_GETCURSEL, 0, 0));
    const auto b = static_cast<std::size_t>(SendDlgItemMessageW(window, kSecond, CB_GETCURSEL, 0, 0));
    const auto left = Values(a), right = Values(b);
    SendMessageW(table, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(table);
    for (int row = 0; row < 18; ++row) {
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = row; item.pszText = const_cast<wchar_t*>(Fields()[row]);
        ListView_InsertItem(table, &item);
        ListView_SetItemText(table, row, 1, const_cast<wchar_t*>(left[row].c_str()));
        ListView_SetItemText(table, row, 2, const_cast<wchar_t*>(right[row].c_str()));
    }
    SendMessageW(table, WM_SETREDRAW, TRUE, 0); InvalidateRect(table, nullptr, TRUE);
}

void SelectSource(std::size_t index) {
    auto& doc = App().document;
    if (index >= doc.sources.size()) return;
    ClosePageDetail(); doc.selectedSource = index; doc.sourceMode = DocumentState::SourceMode::Independent;
    RefreshAll();
}

LRESULT CALLBACK ComparisonProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        auto control = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                0, 0, 10, 10, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontUI), TRUE); return child;
        };
        control(L"STATIC", UiText(TextId::ui_0261), SS_LEFT, 1163);
        HWND first = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kFirst);
        HWND second = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kSecond);
        control(L"BUTTON", UiText(TextId::ui_0262), BS_PUSHBUTTON | WS_TABSTOP, 1164);
        control(L"BUTTON", UiText(TextId::ui_0263), BS_PUSHBUTTON | WS_TABSTOP, 1165);
        control(L"BUTTON", UiText(TextId::ui_0421), BS_PUSHBUTTON | WS_TABSTOP, 1166);
        HWND table = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, kCompareList);
        ListView_SetExtendedListViewStyle(table, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LvAddCol(table, 0, UiText(TextId::ui_0264), S(238)); LvAddCol(table, 1, UiText(TextId::ui_0265), S(280)); LvAddCol(table, 2, UiText(TextId::ui_0266), S(280));
        for (const auto& source : App().document.sources) {
            SendMessageW(first, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(source.label.c_str()));
            SendMessageW(second, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(source.label.c_str()));
        }
        SendMessageW(first, CB_SETCURSEL, App().document.selectedSource, 0);
        SendMessageW(second, CB_SETCURSEL, App().document.sources.size() > 1 ?
            (App().document.selectedSource == 0 ? 1 : 0) : 0, 0);
        ApplyModernTheme(window); RenderComparison(window); return 0;
    }
    case WM_SIZE: {
        RECT r{}; GetClientRect(window, &r); const int width = r.right - S(32);
        MoveWindow(GetDlgItem(window,1163), S(16),S(12),width-S(100),S(88),TRUE);
        MoveWindow(GetDlgItem(window,1166),r.right-S(100),S(12),S(84),S(30),TRUE);
        const int column = std::max(S(160), (width-S(200))/2);
        MoveWindow(GetDlgItem(window,kFirst),S(16),S(108),column,S(320),TRUE);
        MoveWindow(GetDlgItem(window,1164),S(24)+column,S(108),S(84),S(30),TRUE);
        MoveWindow(GetDlgItem(window,kSecond),S(116)+column,S(108),column,S(320),TRUE);
        MoveWindow(GetDlgItem(window,1165),S(124)+2*column,S(108),S(84),S(30),TRUE);
        HWND table=GetDlgItem(window,kCompareList);
        MoveWindow(table,S(16),S(148),width,std::max(1,static_cast<int>(r.bottom)-S(164)),TRUE);
        ListView_SetColumnWidth(table,0,S(238));
        for(int i=1;i<=2;++i) ListView_SetColumnWidth(table,i,std::max(S(180),(width-S(258))/2));
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam)==1166) { CopySourceComparison();return 0; }
        if (HIWORD(wparam)==CBN_SELCHANGE) { RenderComparison(window); return 0; }
        if (LOWORD(wparam)==1164 || LOWORD(wparam)==1165) {
            auto index=SendDlgItemMessageW(window,LOWORD(wparam)==1164?kFirst:kSecond,CB_GETCURSEL,0,0);
            DestroyWindow(window); SelectSource(static_cast<std::size_t>(index)); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(ModernControlBrush(message,reinterpret_cast<HDC>(wparam),reinterpret_cast<HWND>(lparam)));
    case WM_ERASEBKGND: {
        RECT rect{};GetClientRect(window,&rect);FillSolid(reinterpret_cast<HDC>(wparam),rect,th::surface);return 1;
    }
    case WM_GETMINMAXINFO: {
        auto* info=reinterpret_cast<MINMAXINFO*>(lparam); info->ptMinTrackSize={S(680),S(480)};return 0;
    }
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: g_comparison=nullptr; return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

void ShowComparison() {
    if (g_comparison) { SetForegroundWindow(g_comparison); return; }
    const auto& doc=App().document;
    if (doc.comparisons.empty()) {
        LogView view=doc.filtered; App().document.rebuildComparisons(view);
    }
    WNDCLASSW cls{}; cls.lpfnWndProc=ComparisonProc; cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpszClassName=L"dialSourceComparison"; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.hbrBackground=GetSysColorBrush(COLOR_WINDOW); RegisterClassW(&cls);
    g_comparison=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,UiText(TextId::ui_0267),WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,CW_USEDEFAULT,S(960),S(690),App().hMain,nullptr,cls.hInstance,nullptr);
    if (g_comparison) SetFocus(GetDlgItem(g_comparison,kCompareList));
}
}

std::wstring AnalysisSourceText() {
    const auto& doc=App().document;
    if (doc.sources.empty()) return UiText(TextId::ui_0268);
    if(doc.sourceMode==DocumentState::SourceMode::Device)return UiText(TextId::workspace_group)+U8ToW(doc.selectedDevice);
    if (doc.sourceMode==DocumentState::SourceMode::Continuation) return UiText(TextId::ui_0269);
    return UiText(TextId::ui_0270)+doc.sources[std::min(doc.selectedSource,doc.sources.size()-1)].label;
}

void UpdateSourceControls() {
    if (!App().hSources) return;
    EnableWindow(App().hSources, !App().document.sources.empty() && !LoadInProgress());
    SetWindowTextW(App().hSources,App().document.sources.size()>1 ? UiText(TextId::ui_0271) : UiText(TextId::ui_0166));
    const auto& doc=App().document;
    if (doc.sources.size()>1 && App().hFileLbl)
        SetWindowTextW(App().hFileLbl,(AnalysisSourceText()+FmtW(UiText(TextId::ui_0018),
            static_cast<int>(doc.lines.size()),static_cast<int>(doc.sessions.size()),
            U8ToW(GeneratedText(doc.platform.name)).c_str())).c_str());
}

void CloseSourceComparison() { if (g_comparison) DestroyWindow(g_comparison); }

void ShowSourceMenu(HWND anchor) {
    const auto& doc=App().document; if (doc.sources.empty() || LoadInProgress()) return;
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,33002,UiText(TextId::workspace_manage));
    AppendMenuW(menu,MF_STRING,33003,UiText(TextId::package_import));
    std::set<std::string> devices;for(const auto& source:doc.sources){auto d=doc.workspace.devices.find(source.workspaceKey());if(d!=doc.workspace.devices.end()&&!d->second.device.empty())devices.insert(d->second.device);}
    unsigned group=30000;for(const auto& key:devices){auto title=UiText(TextId::workspace_group)+U8ToW(key);AppendMenuW(menu,MF_STRING|(doc.sourceMode==DocumentState::SourceMode::Device&&doc.selectedDevice==key?MF_CHECKED:0),group++,title.c_str());}
    AppendMenuW(menu,MF_STRING,33000,UiText(TextId::ui_0272));
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING | (doc.sourceMode==DocumentState::SourceMode::Continuation?MF_CHECKED:0),33001,UiText(TextId::ui_0273));
    AppendMenuW(menu,MF_STRING | MF_DISABLED,0,UiText(TextId::ui_0274));
    for (std::size_t i=0;i<doc.sources.size();++i) {
        std::wstring label=std::to_wstring(i+1)+L"  "+doc.sources[i].label;
        for (std::size_t p=0;(p=label.find(L'&',p))!=std::wstring::npos;p+=2) label.insert(p,1,L'&');
        AppendMenuW(menu,MF_STRING | (doc.sourceMode==DocumentState::SourceMode::Independent && doc.selectedSource==i?MF_CHECKED:0),34000+i,label.c_str());
    }
    RECT rect{};GetWindowRect(anchor,&rect);
    const auto command=TrackPopupMenu(menu,TPM_RETURNCMD | TPM_LEFTALIGN,rect.left,rect.bottom,0,App().hMain,nullptr);
    DestroyMenu(menu);
    HandleSourceCommand(command);
}

bool HandleSourceCommand(UINT command) {
    const auto& doc=App().document;
    if (doc.sources.empty() || LoadInProgress()) return false;
    if(command==33002)ShowWorkspace();
    else if(command==33003)ImportEvidencePackage();
    else if(command>=30000&&command<31000){std::set<std::string> groups;for(const auto& source:doc.sources){auto d=doc.workspace.devices.find(source.workspaceKey());if(d!=doc.workspace.devices.end()&&!d->second.device.empty())groups.insert(d->second.device);}auto index=command-30000;if(index>=groups.size())return false;auto it=groups.begin();std::advance(it,index);App().document.selectedDevice=*it;App().document.sourceMode=DocumentState::SourceMode::Device;RefreshAll();}
    else if(command==33000) ShowComparison();
    else if(command==33001) {
        ClosePageDetail(); App().document.sourceMode=DocumentState::SourceMode::Continuation; RefreshAll();
    } else if(command>=34000 && static_cast<std::size_t>(command-34000)<doc.sources.size()) SelectSource(command-34000);
    else return false;
    return true;
}

bool SourceComparisonActive() {
    HWND focus=GetFocus(); return g_comparison && (focus==g_comparison || IsChild(g_comparison,focus));
}

bool RouteSourceComparisonMessage(MSG& message) {
    if (!SourceComparisonActive() || message.message!=WM_KEYDOWN) return false;
    if (message.wParam==VK_ESCAPE) { CloseSourceComparison();return true; }
    if (message.wParam==VK_TAB) return IsDialogMessageW(g_comparison,&message)!=FALSE;
    return false;
}

bool CopySourceComparison() {
    if (!SourceComparisonActive()) return false;
    HWND table=GetDlgItem(g_comparison,kCompareList);
    std::wstring text=AnalysisSourceText()+L"\r\n"+AnalysisTimeRangeText()+L"\r\n";
    for (int row=0;row<18;++row) {
        for (int column=0;column<3;++column) {
            wchar_t value[2048]{};ListView_GetItemText(table,row,column,value,2048);
            if (column) text+=L'\t';
            text+=value;
        }
        text+=L"\r\n";
    }
    if (!OpenClipboard(g_comparison)) return false;
    const auto bytes=(text.size()+1)*sizeof(wchar_t);HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    void* target=memory?GlobalLock(memory):nullptr;
    if (!target) {if(memory)GlobalFree(memory);CloseClipboard();return false;}
    std::memcpy(target,text.c_str(),bytes);GlobalUnlock(memory);EmptyClipboard();
    const bool copied=SetClipboardData(CF_UNICODETEXT,memory)!=nullptr;
    if(!copied) GlobalFree(memory);
    CloseClipboard();return copied;
}
}
