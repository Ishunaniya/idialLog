#include "incident_review.h"
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <climits>
#include <exception>
#include <stdexcept>
#include "app_context.h"
#include "incidentmodel.h"
#include "incident_export.h"
#include "source_workspace.h"
#include "workspace_window.h"
#include "load_controller.h"
#include "ui_pages.h"
#include "tablemodel.h"
#include "text_catalog.h"
#include "win_text.h"
#include "win_file_io.h"
#include "text_view.h"
#include "modern_shell.h"
#include "theme.h"
#include "version.h"
#include "log_time.h"

namespace dl {
namespace {
HWND g_window = nullptr;
IncidentCatalog g_catalog;
IncidentReview g_review;
IncidentExportContext g_context;
std::size_t g_index = 0, g_restoredIndex = SIZE_MAX;
int g_tab = 0;
constexpr int previous = 1300, next = 1301, before = 1302, after = 1303, summary = 1304, tabs = 1305, table = 1306,
              detail = 1307, signal = 1308, copy = 1309, exportZip = 1310, jump = 1311, bookmark = 1312,
              beforeLabel = 1313, afterLabel = 1314;
constexpr int minutes[] = {0, 1, 5, 10, 30};

std::wstring FindingBody(const Finding& f) {
    std::wstring text = U8ToW(GeneratedText(f.title)) + UiText(TextId::ui_0361) + U8ToW(GeneratedText(f.detail)) +
                        UiText(TextId::ui_0362) + U8ToW(GeneratedText(f.advice)) + UiText(TextId::ui_0363);
    for (const auto& e : f.ev)
        text += L"#" + std::to_wstring(e.lineNo) + L" " + U8ToW(e.ts) + L" " + U8ToW(e.text) + L"\r\n";
    return text;
}

std::wstring SummaryText() {
    const auto text = U8ToW(incidentSummaryText(g_review, g_context));
    std::wstring result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r'))
            result += L'\r';
        result += text[i];
    }

    return result;
}

std::size_t RowCount() {
    return g_tab == 0 ? g_review.events.size() : g_tab == 1 ? g_review.metrics.size() : g_review.findings.size();
}

std::size_t SelectedLine() {
    const int row = ListView_GetNextItem(GetDlgItem(g_window, table), -1, LVNI_SELECTED);
    if (row < 0 || static_cast<std::size_t>(row) >= RowCount())
        return 0;
    if (g_tab == 0)
        return g_review.events[row]->lineNo;
    if (g_tab == 1)
        return g_review.metrics[row].lineNo;
    return 0;
}

std::wstring Cell(int row, int col) {
    if (row < 0 || col < 0 || static_cast<std::size_t>(row) >= RowCount())
        return {};
    std::size_t line = 0;
    if (g_tab == 2) {
        const auto& f = g_review.findings[row];
        return col == 0 ? U8ToW(GeneratedText(f.title)) : U8ToW(GeneratedText(f.detail));
    }

    if (g_tab == 0) {
        const auto* event = g_review.events[row];
        line = event->lineNo;
        if (col == 0)
            return U8ToW(event->ts);
        if (col == 1) {
            if (line == g_review.outage.endLine && g_review.outage.recovered)
                return UiText(TextId::incident_end);
            if (line == g_review.outage.startLine)
                return UiText(TextId::incident_start);
            return U8ToW(GeneratedText(timelineCellText(*event, 1)));
        }

        if (col == 5)
            return U8ToW(event->msg);
    } else {
        const auto& m = g_review.metrics[row];
        line = m.lineNo;
        constexpr std::size_t metricColumns[] = {0, 5, 10, 11, 12, 13, 2, 9, 15};
        if (col < 9)
            return U8ToW(metricCellText(m, metricColumns[col]));
        if (col == 9) {
            const auto phase = incidentSamplePhase(g_review, m);
            return UiText(phase == IncidentPhase::Before  ? TextId::incident_pre
                          : phase == IncidentPhase::After ? TextId::incident_post
                                                          : TextId::incident_during);
        }

        col -= 8;  // source/original/global are columns 10/11/12.
    }

    const auto origin = evidenceOrigin(g_context.sources, line);
    if (col == 2)
        return L"#" + std::to_wstring(origin.index) + L" " + U8ToW(origin.label);
    if (col == 3)
        return origin.line ? std::to_wstring(origin.line) : L"—";
    if (col == 4)
        return std::to_wstring(line);
    return {};
}

void UpdateDetail() {
    const int row = ListView_GetNextItem(GetDlgItem(g_window, table), -1, LVNI_SELECTED);
    std::wstring text;
    if (row >= 0 && static_cast<std::size_t>(row) < RowCount()) {
        if (g_tab == 2)
            text =
                std::wstring(UiText(TextId::incident_rules_note)) + L"\r\n\r\n" + FindingBody(g_review.findings[row]);
        else {
            const int columns = g_tab == 0 ? 6 : 13;
            for (int c = 0; c < columns; ++c) {
                if (c)
                    text += c == columns - 1 ? L"\r\n" : L"  |  ";
                text += Cell(row, c);
            }
        }
    } else if (g_tab == 2)
        text = UiText(TextId::incident_no_rules);
    SetWindowTextW(GetDlgItem(g_window, detail), text.c_str());
    EnableWindow(GetDlgItem(g_window, jump), SelectedLine() != 0);
    EnableWindow(GetDlgItem(g_window, bookmark), SelectedLine() != 0);
}

void RebuildTable() {
    HWND list = GetDlgItem(g_window, table);
    ListView_SetItemCountEx(list, 0, 0);
    while (Header_GetItemCount(ListView_GetHeader(list)))
        ListView_DeleteColumn(list, 0);
    auto column = [&](const wchar_t* name, int width) {
        LvAddCol(list, Header_GetItemCount(ListView_GetHeader(list)), name, S(width));
    };

    if (g_tab == 0) {
        column(UiText(TextId::incident_time), 185);
        column(UiText(TextId::incident_kind), 120);
        column(UiText(TextId::incident_source), 170);
        column(UiText(TextId::incident_local_line), 100);
        column(UiText(TextId::incident_global_line), 100);
        column(UiText(TextId::incident_message), 620);
    } else if (g_tab == 1) {
        column(UiText(TextId::incident_time), 185);
        column(L"CSQ", 60);
        column(L"RSRP (dBm)", 105);
        column(L"RSRQ (dB)", 100);
        column(L"SNR (dB)", 95);
        column(L"RSSI (dBm)", 105);
        column(L"Cell ID", 100);
        column(L"ΔRX", 100);
        column(L"RAT", 80);
        column(UiText(TextId::incident_phase), 125);
        column(UiText(TextId::incident_source), 170);
        column(UiText(TextId::incident_local_line), 100);
        column(UiText(TextId::incident_global_line), 100);
    } else {
        column(UiText(TextId::incident_rules), 350);
        column(UiText(TextId::ui_0192), 700);
    }

    ListView_SetItemCountEx(list, static_cast<int>(std::min<std::size_t>(RowCount(), INT_MAX)), 0);
    if (RowCount()) {
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, 0, FALSE);
    }

    UpdateDetail();
}

void Review() {
    HWND list = GetDlgItem(g_window, table);
    ListView_SetItemCountEx(list, 0, 0);
    const int b = static_cast<int>(SendDlgItemMessageW(g_window, before, CB_GETCURSEL, 0, 0));
    const int a = static_cast<int>(SendDlgItemMessageW(g_window, after, CB_GETCURSEL, 0, 0));
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    g_review = reviewIncident(g_catalog, g_index, minutes[std::clamp(b, 0, 4)] * 60, minutes[std::clamp(a, 0, 4)] * 60);
    SetCursor(old);
    g_review.resolvedOutsideFilter = g_index == g_restoredIndex;
    g_context.reviews.clear();
    auto record = App().document.workspace.reviews.find(IncidentReviewKey(g_review.outage));
    if (record != App().document.workspace.reviews.end())
        g_context.reviews.push_back(record->second);
    SetWindowTextW(GetDlgItem(g_window, summary), SummaryText().c_str());
    EnableWindow(GetDlgItem(g_window, previous), g_index > 0);
    EnableWindow(GetDlgItem(g_window, next), g_index + 1 < g_catalog.outages.size());
    for (int id : {signal, copy, exportZip})
        EnableWindow(GetDlgItem(g_window, id), g_review.valid);
    RebuildTable();
}

void MoveIncident(int delta) {
    if (delta < 0 && g_index == 0)
        return;
    if (delta > 0 && g_index + 1 >= g_catalog.outages.size())
        return;
    g_index = delta < 0 ? g_index - 1 : g_index + 1;
    Review();
}

void Follow(bool mark) {
    const auto line = SelectedLine();
    if (!line)
        return;
    if (mark) {
        ToggleEvidenceBookmark(line, GetText(GetDlgItem(g_window, detail)));
        return;
    }

    CloseIncidentReview();
    JumpToRawLine(line);
    SetForegroundWindow(App().hMain);
}

void CopyReview() {
    std::wstring text = SummaryText() + L"\r\n" + UiText(TextId::incident_rules_note) + L"\r\n";
    for (const auto& f : g_review.findings)
        text += L"\r\n" + FindingBody(f);
    text += L"\r\n" + std::wstring(UiText(TextId::incident_events)) + L"\r\n";
    for (const auto* row : g_review.events)
        text += L"#" + std::to_wstring(row->lineNo) + L" " + U8ToW(row->ts) + L" " + U8ToW(row->msg) + L"\r\n";
    const bool ok = CopyWindowText(g_window, text);
    ShowModernNotice(ok ? UiText(TextId::ui_0478) : UiText(TextId::ui_0357),
                     ok ? UiText(TextId::ui_0358) : UiText(TextId::ui_0359),
                     ok ? ModernNoticeKind::Success : ModernNoticeKind::Error);
}

void Export() {
    wchar_t path[MAX_PATH] = L"diallog_incident.zip";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = App().hMain;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"ZIP (*.zip)\0*.zip\0\0";
    dialog.lpstrDefExt = L"zip";
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrTitle = UiText(TextId::incident_export);

    // Use the main modal owner for the system dialog, but also prevent its
    // sibling review from changing the selected incident while saving.
    const HWND reviewWindow = g_window;
    const bool wasEnabled = IsWindowEnabled(reviewWindow) != FALSE;
    EnableWindow(reviewWindow, FALSE);
    const bool accepted = GetSaveFileNameW(&dialog) != FALSE;
    if (IsWindow(reviewWindow))
        EnableWindow(reviewWindow, wasEnabled);
    if (!accepted || reviewWindow != g_window || !g_review.valid)
        return;
    SetForegroundWindow(reviewWindow);

    struct ClearOriginals {
        ~ClearOriginals() {
            g_context.originals.clear();
        }
    } clearOriginals;

    try {
        g_context.bookmarks.clear();
        for (const auto& note : EvidenceBookmarks())
            g_context.bookmarks.push_back({note.lineNo, WToU8(note.text)});
        g_context.originals.clear();
        g_context.workspace = App().document.workspace;
        g_context.reviews.clear();
        auto record = App().document.workspace.reviews.find(IncidentReviewKey(g_review.outage));
        if (record != App().document.workspace.reviews.end())
            g_context.reviews.push_back(record->second);
        if (SendDlgItemMessageW(g_window, 1316, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            for (const auto& source : App().document.sources) {
                std::string data;
                std::wstring why;
                if (!source.pastedBytes.empty())
                    data = source.pastedBytes;
                else if (!ReadOriginalBytes({source.originalPath, source.originalEntry, source.originalHash}, data,
                                            why))
                    throw std::runtime_error(WToU8(why));
                g_context.originals.push_back({source.originalEntry, WToU8(source.label), std::move(data),
                                               source.originalHash, source.workspaceKey()});
            }
        }

        const auto bytes = buildIncidentZip(g_review, g_context);
        std::wstring error;
        if (!WriteFileBytesAtomic(path, bytes, error)) {
            ShowModernNotice(UiText(TextId::incident_export_fail), error.c_str(), ModernNoticeKind::Error);
            return;
        }

        ShowModernNotice(UiText(TextId::incident_export_done), path, ModernNoticeKind::Success);
    } catch (const std::length_error&) {
        ShowModernNotice(UiText(TextId::incident_export_fail), UiText(TextId::incident_export_limit),
                         ModernNoticeKind::Error);
    } catch (const std::exception& e) {
        ShowModernNotice(UiText(TextId::incident_export_fail), U8ToW(e.what()).c_str(), ModernNoticeKind::Error);
    }
}

LRESULT CALLBACK ReviewProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_window = window;
        auto control = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            HWND child =
                CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontUI), TRUE);
            return child;
        };

        auto button = [&](TextId text, int id) { control(L"BUTTON", UiText(text), BS_PUSHBUTTON | WS_TABSTOP, id); };
        button(TextId::incident_previous, previous);
        button(TextId::incident_next, next);
        control(L"STATIC", UiText(TextId::incident_before), SS_CENTERIMAGE, beforeLabel);
        control(L"STATIC", UiText(TextId::incident_after), SS_CENTERIMAGE, afterLabel);
        for (int id : {before, after}) {
            HWND cb = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
            for (int value : minutes) {
                auto text = FmtW(UiText(TextId::incident_minutes), value);
                SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            }

            SendMessageW(cb, CB_SETCURSEL, 2, 0);
        }

        for (int id : {summary, detail}) {
            HWND edit =
                control(L"EDIT", L"", ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, id);
            SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);
        }

        button(TextId::incident_signal, signal);
        button(TextId::incident_copy, copy);
        button(TextId::incident_export, exportZip);
        button(TextId::incident_jump, jump);
        button(TextId::incident_bookmark, bookmark);
        button(TextId::workspace_edit_review, 1315);
        button(TextId::wb_chain, 1317);
        control(L"BUTTON", UiText(TextId::package_original), BS_AUTOCHECKBOX | WS_TABSTOP, 1316);
        SendDlgItemMessageW(window, 1316, BM_SETCHECK, BST_CHECKED, 0);
        HWND tab = control(WC_TABCONTROLW, L"", WS_TABSTOP, tabs);
        for (TextId id : {TextId::incident_events, TextId::incident_metrics, TextId::incident_rules}) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(UiText(id));
            TabCtrl_InsertItem(tab, TabCtrl_GetItemCount(tab), &item);
        }

        HWND list = control(WC_LISTVIEWW, L"",
                            LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, table);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ApplyModernTheme(window);
        Review();
        return 0;
    }

    case WM_SIZE: {
        RECT r{};
        GetClientRect(window, &r);
        int width = std::max(1, static_cast<int>(r.right) - S(32));
        auto move = [&](int id, int x, int y, int w, int h) {
            MoveWindow(GetDlgItem(window, id), S(x), S(y), S(w), S(h), TRUE);
        };

        move(1317, 568, MulDiv(r.bottom, 96, App().dpi) - 50, 160, 32);
        move(1315, 358, MulDiv(r.bottom, 96, App().dpi) - 50, 200, 32);
        move(1316, 16, MulDiv(r.bottom, 96, App().dpi) - 92, MulDiv(width, 96, App().dpi), 28);
        move(previous, 16, 12, 105, 32);
        move(next, 128, 12, 105, 32);
        move(beforeLabel, 250, 12, 100, 32);
        move(before, 352, 12, 92, 240);
        move(afterLabel, 460, 12, 100, 32);
        move(after, 562, 12, 92, 240);
        const int summaryH = std::clamp(static_cast<int>(r.bottom) / 4, S(130), S(190));
        MoveWindow(GetDlgItem(window, summary), S(16), S(56), width, summaryH, TRUE);
        const int actions = S(68) + summaryH, tabTop = actions + S(44), tableTop = tabTop + S(34);
        const int detailH = std::clamp(static_cast<int>(r.bottom) / 4, S(100), S(180));
        const int detailTop = static_cast<int>(r.bottom) - detailH - S(100);
        MoveWindow(GetDlgItem(window, signal), S(16), actions, S(170), S(32), TRUE);
        MoveWindow(GetDlgItem(window, copy), S(194), actions, S(145), S(32), TRUE);
        MoveWindow(GetDlgItem(window, exportZip), S(347), actions, S(205), S(32), TRUE);
        MoveWindow(GetDlgItem(window, tabs), S(16), tabTop, width, S(30), TRUE);
        MoveWindow(GetDlgItem(window, table), S(16), tableTop, width, std::max(1, detailTop - S(8) - tableTop), TRUE);
        MoveWindow(GetDlgItem(window, detail), S(16), detailTop, width, detailH, TRUE);
        MoveWindow(GetDlgItem(window, jump), S(16), r.bottom - S(50), S(160), S(32), TRUE);
        MoveWindow(GetDlgItem(window, bookmark), S(184), r.bottom - S(50), S(160), S(32), TRUE);
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (HIWORD(wp) == CBN_SELCHANGE && (id == before || id == after)) {
            Review();
            return 0;
        }

        if (id == previous || id == next) {
            MoveIncident(id == previous ? -1 : 1);
            return 0;
        }

        if (id == 1317 && g_review.valid) {
            const auto outage = g_review.outage;
            CloseIncidentReview();
            ShowWorkbenchIncident(outage);
            return 0;
        }

        if (id == 1315) {
            EditManualReview(IncidentReviewKey(g_review.outage), std::string(UiText8(TextId::incident_title)) + " " +
                                                                     fmtTime(g_review.outage.start, "FULL"));
            return 0;
        }

        if (id == copy) {
            CopyReview();
            return 0;
        }

        if (id == exportZip) {
            Export();
            return 0;
        }

        if (id == jump || id == bookmark) {
            Follow(id == bookmark);
            return 0;
        }

        if (id == signal && g_review.valid) {
            const auto first = g_review.start, last = g_review.end;
            CloseIncidentReview();
            SelectAnalysisTimeRange(first, last);
            ShowPage(4);
            SetForegroundWindow(App().hMain);
            return 0;
        }

        break;
    }

    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->idFrom == tabs && hdr->code == TCN_SELCHANGE) {
            g_tab = TabCtrl_GetCurSel(GetDlgItem(window, tabs));
            RebuildTable();
            return 0;
        }

        if (hdr->idFrom != table)
            break;
        if (hdr->code == NM_CUSTOMDRAW) {
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && draw->nmcd.dwItemSpec < RowCount() &&
                !(ListView_GetItemState(GetDlgItem(window, table), draw->nmcd.dwItemSpec, LVIS_SELECTED) &
                  LVIS_SELECTED)) {
                const std::size_t row = draw->nmcd.dwItemSpec;
                draw->clrText = th::inkPri;
                draw->clrTextBk = row % 2 ? th::zebra : th::surface;
                if (g_tab == 0) {
                    const auto* event = g_review.events[row];
                    const auto kind = logEventKind(*event);
                    if (event->lineNo == g_review.outage.endLine && g_review.outage.recovered)
                        draw->clrTextBk = th::qualityExcellent;
                    else if (event->lineNo == g_review.outage.startLine || kind == LogEventKind::Fault ||
                             kind == LogEventKind::Error)
                        draw->clrTextBk = th::outageBand;
                    else if (kind == LogEventKind::RecoveryAction)
                        draw->clrTextBk = th::accentSoft;
                    else if (kind == LogEventKind::Warning)
                        draw->clrTextBk = th::cellWeak;
                }

                return CDRF_NEWFONT;
            }

            return CDRF_DODEFAULT;
        }

        if (hdr->code == LVN_GETDISPINFOW) {
            auto* info = reinterpret_cast<NMLVDISPINFOW*>(lp);
            if ((info->item.mask & LVIF_TEXT) && info->item.pszText && info->item.cchTextMax > 0) {
                auto text = Cell(info->item.iItem, info->item.iSubItem);
                lstrcpynW(info->item.pszText, text.c_str(), info->item.cchTextMax);
            }

            return 0;
        }

        if (hdr->code == LVN_ITEMCHANGED) {
            UpdateDetail();
            return 0;
        }

        if (hdr->code == NM_DBLCLK) {
            if (g_tab != 2)
                Follow(false);
            else
                SetFocus(GetDlgItem(window, detail));
            return 0;
        }

        break;
    }

    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(window, &r);
        FillSolid(reinterpret_cast<HDC>(wp), r, th::surface);
        return 1;
    }

    case WM_CTLCOLORSTATIC:

    case WM_CTLCOLOREDIT:

    case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(
            ModernControlBrush(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp)));

    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(780), S(700)};
        return 0;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        g_window = nullptr;
        g_review = IncidentReview{};
        g_catalog = IncidentCatalog{};
        g_context = IncidentExportContext{};
        return 0;
    }

    return DefWindowProcW(window, msg, wp, lp);
}
}  // namespace

void CloseIncidentReview() {
    if (g_window)
        DestroyWindow(g_window);
}

void RefreshIncidentAnnotation() {
    if (!g_window || !g_review.valid)
        return;
    g_context.reviews.clear();
    auto record = App().document.workspace.reviews.find(IncidentReviewKey(g_review.outage));
    if (record != App().document.workspace.reviews.end())
        g_context.reviews.push_back(record->second);
    SetWindowTextW(GetDlgItem(g_window, summary), SummaryText().c_str());
}

void ShowIncidentReview(const Outage& selected) {
    if (LoadInProgress())
        return;
    CloseIncidentReview();
    const auto& doc = App().document;
    LogView scope;
    scope.reserve(doc.lines.size());
    for (const auto& line : doc.lines)
        scope.push_back(&line);
    doc.restrictToSelection(scope);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    g_catalog = buildIncidentCatalog(std::move(scope));
    SetCursor(old);
    g_index = findIncident(g_catalog, selected);
    g_tab = 0;
    if (g_index >= g_catalog.outages.size()) {
        g_catalog = IncidentCatalog{};
        ShowModernNotice(UiText(TextId::incident_title), UiText(TextId::incident_none), ModernNoticeKind::Info);
        return;
    }

    g_context.scope = WToU8(AnalysisSourceText());
    g_context.version = DL_VER_STR;
    g_context.build = __DATE__ " " __TIME__;
    g_context.continuation = doc.sourceMode == DocumentState::SourceMode::Continuation;
    g_context.selectedDevice = doc.sourceMode == DocumentState::SourceMode::Device ? doc.selectedDevice : "";
    if (doc.selectedSource < doc.sources.size())
        g_context.selectedSourceHash = doc.sources[doc.selectedSource].workspaceKey();
    for (std::size_t i = 0; i < doc.sources.size(); ++i) {
        const auto& s = doc.sources[i];
        if (s.first >= s.last || s.last > doc.lines.size())
            continue;
        std::size_t first = SIZE_MAX, last = 0;
        for (std::size_t row = s.first; row < s.last; ++row) {
            first = std::min(first, doc.lines[row].lineNo);
            last = std::max(last, doc.lines[row].lineNo);
        }

        g_context.sources.push_back({WToU8(s.label), i + 1, first, last, s.rawLineOffset});
    }

    const auto actual = g_catalog.outages[g_index];
    const bool restored = actual.startLine != selected.startLine || actual.endLine != selected.endLine ||
                          actual.recovered != selected.recovered;
    g_restoredIndex = restored ? g_index : SIZE_MAX;
    WNDCLASSW cls{};
    cls.lpfnWndProc = ReviewProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"dialIncidentReview";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    g_window =
        CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, UiText(TextId::incident_title), WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, S(1120), S(880), App().hMain, nullptr, cls.hInstance, nullptr);
    if (!g_window) {
        g_catalog = IncidentCatalog{};
        g_context = IncidentExportContext{};
        return;
    }

    g_review.resolvedOutsideFilter = restored;
    g_context.reviews.clear();
    auto record = App().document.workspace.reviews.find(IncidentReviewKey(g_review.outage));
    if (record != App().document.workspace.reviews.end())
        g_context.reviews.push_back(record->second);
    SetWindowTextW(GetDlgItem(g_window, summary), SummaryText().c_str());
    ShowWindow(g_window, SW_SHOW);
    SetFocus(GetDlgItem(g_window, table));
}

bool RouteIncidentReviewMessage(MSG& message) {
    if (!g_window || (message.hwnd != g_window && !IsChild(g_window, message.hwnd)))
        return false;
    if (message.message == WM_KEYDOWN) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const HWND focus = GetFocus();
        if (message.wParam == VK_ESCAPE) {
            CloseIncidentReview();
            return true;
        }

        if (ctrl && (message.wParam == VK_LEFT || message.wParam == VK_RIGHT)) {
            MoveIncident(message.wParam == VK_LEFT ? -1 : 1);
            return true;
        }

        if (ctrl && message.wParam == 'A' &&
            (focus == GetDlgItem(g_window, summary) || focus == GetDlgItem(g_window, detail))) {
            SendMessageW(focus, EM_SETSEL, 0, -1);
            return true;
        }

        if (ctrl && message.wParam == 'C' && focus != GetDlgItem(g_window, summary) &&
            focus != GetDlgItem(g_window, detail)) {
            CopyReview();
            return true;
        }

        if (message.wParam == VK_RETURN && focus == GetDlgItem(g_window, table)) {
            if (g_tab == 2)
                SetFocus(GetDlgItem(g_window, detail));
            else
                Follow(false);
            return true;
        }
    }

    if (!IsDialogMessageW(g_window, &message)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return true;
}
}  // namespace dl
