// Embedded workbench creation and command routing. Control IDs remain stable.
void RestorePlotView() {
    if (!g_plotReady || !g_haveSavedSession || !g_savedSession.comparison.viewActive)
        return;
    const auto& view = g_savedSession.comparison;
    if (view.aStart != g_a0 || view.aEnd != g_a1 || view.bStart != g_b0 || view.bEnd != g_b1)
        return;
    g_plotWindow = {std::max(g_plotBounds.start, view.viewStart), std::min(g_plotBounds.end, view.viewEnd)};
    if (g_plotWindow.start > g_plotWindow.end)
        g_plotWindow = g_plotBounds;
    UpdatePlotButtons();
    InvalidateRect(GetDlgItem(g_workspace, plot), nullptr, FALSE);
}

void SwitchWorkPage(int page) {
    if (g_workspace && !g_building) {
        try {
            g_savedSession = CaptureSession();
            g_haveSavedSession = true;
        } catch (const std::exception&) {
        }
    }

    g_page = page;
    if (page == 0 || page == 2 || page == 4)
        g_deviceView = page;
    g_scroll = 0;
    TabCtrl_SetCurSel(GetDlgItem(g_workspace, tab), TopTab());
    ClearPlotCache();
    g_compareText.clear();
    g_trendText.clear();
    g_signalA = {};
    g_signalB = {};
    SetWindowTextW(GetDlgItem(g_workspace, result), L"");
    FillList();
    Layout();
    if (g_page == 2 && !App().document.sources.empty()) {
        Calculate();
        RestorePlotView();
    }

    if (g_page == 4 && !App().document.sources.empty())
        BuildTrend();
}

void RevealControl(HWND child) {
    if (!g_workspace || !child || !IsWindowVisible(child))
        return;
    RECT area{}, r{};
    GetClientRect(g_workspace, &area);
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, g_workspace, reinterpret_cast<POINT*>(&r), 2);
    if (r.top < 0)
        g_scroll += MulDiv(r.top, 96, App().dpi) - 8;
    else if (r.bottom > area.bottom)
        g_scroll += MulDiv(r.bottom - area.bottom, 96, App().dpi) + 8;
    else
        return;
    Layout();
}

void ApplyPlotScope(bool useB) {
    if (!g_plotReady || g_page != 2)
        return;
    const auto index = SendDlgItemMessageW(g_workspace, useB ? scopeB : scopeA, CB_GETCURSEL, 0, 0);
    if (index < 0 || std::size_t(index) >= g_scopes.size())
        return;
    WorkspaceEvent event;
    event.scope = ScopeKey(index);
    const bool align = SendDlgItemMessageW(g_workspace, relative, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const auto lo = useB ? g_b0 : g_a0, hi = useB ? g_b1 : g_a1;
    auto start = std::max(lo, g_plotWindow.start + (align ? lo : 0)),
         end = std::min(hi, g_plotWindow.end + (align ? lo : 0));
    if (start > end) {
        ShowModernNotice(UiText(TextId::wb_selection_empty), L"", ModernNoticeKind::Info);
        return;
    }

    SelectEventScope(event);
    if (start == end) {
        App().document.selectTimeRange(start, end);
        RefreshAll();
    } else
        SelectAnalysisTimeRange(start, end);
    ShowPage(4);
}

LRESULT CALLBACK WorkspaceProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_workspace = w;
        App().hReviewWorkbench = w;
        g_building = true;
        HWND t = Control(w, WC_TABCONTROLW, L"", WS_TABSTOP, tab);
        for (auto id : {TextId::wb_events, TextId::wb_compare, TextId::wb_sessions}) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(UiText(id));
            TabCtrl_InsertItem(t, TabCtrl_GetItemCount(t), &item);
        }

        TabCtrl_SetCurSel(t, TopTab());
        Control(w, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, hint);
        auto l = Control(w, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, list);
        ListView_SetExtendedListViewStyle(l, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        for (auto entry : {std::pair<int, TextId>{deviceLabel, TextId::workspace_device},
                           {firmwareLabel, TextId::workspace_firmware},
                           {configurationLabel, TextId::workspace_configuration},
                           {1423, TextId::workspace_start},
                           {1424, TextId::workspace_end},
                           {1425, TextId::workspace_start},
                           {1426, TextId::workspace_end},
                           {1494, TextId::wb_session_name},
                           {1508, TextId::workspace_start},
                           {1509, TextId::workspace_end},
                           {1510, TextId::wb_trend_step},
                           {1511, TextId::wb_step_custom},
                           {boardMinimumLabel, TextId::wb_min_duration}})
            Control(w, L"STATIC", UiText(entry.second), SS_CENTERIMAGE | SS_NOPREFIX, entry.first);
        for (int id : {device, firmware, configuration, boardSearch, boardMinimum, sessionName, trendCustom}) {
            Control(w, L"EDIT",
                    id == trendCustom    ? L"86400"
                    : id == boardMinimum ? L"0"
                                         : L"",
                    WS_TABSTOP | (id == configuration ? ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL : ES_AUTOHSCROLL) |
                        (id == trendCustom || id == boardMinimum ? ES_NUMBER : 0),
                    id);
            SendDlgItemMessageW(w, id, EM_SETLIMITTEXT,
                                id == configuration                       ? 1000
                                : id == boardSearch                       ? 1024
                                : id == sessionName                       ? 240
                                : id == trendCustom || id == boardMinimum ? 8
                                                                          : 120,
                                0);
        }

        SendDlgItemMessageW(w, boardSearch, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(UiText(TextId::wb_search)));
        for (int id : {scopeA, scopeB, boardScope, boardStatus, boardSort, trendScope, trendStep})
            Control(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, id);
        SendDlgItemMessageW(w, boardStatus, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(UiText(TextId::wb_all_status)));
        for (auto id : {TextId::review_pending, TextId::review_confirmed, TextId::review_handled})
            SendDlgItemMessageW(w, boardStatus, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(UiText(id)));
        SendDlgItemMessageW(w, boardStatus, CB_SETCURSEL, 0, 0);
        for (auto id : {TextId::wb_sort_time, TextId::wb_sort_duration})
            SendDlgItemMessageW(w, boardSort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(UiText(id)));
        SendDlgItemMessageW(w, boardSort, CB_SETCURSEL, 0, 0);
        for (auto id : {TextId::wb_step_hour, TextId::wb_step_day, TextId::wb_step_week, TextId::wb_step_custom})
            SendDlgItemMessageW(w, trendStep, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(UiText(id)));
        SendDlgItemMessageW(w, trendStep, CB_SETCURSEL, 1, 0);
        for (int id : {startA, endA, startB, endB, trendStart, trendEnd}) {
            auto c = Control(w, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, id);
            SendMessageW(c, EM_SETLIMITTEXT, 19, 0);
        }

        for (auto entry : {std::pair<int, TextId>{save, TextId::workspace_save},
                           {run, TextId::workspace_run},
                           {copy, TextId::workspace_copy},
                           {exportHtml, TextId::workspace_export},
                           {assignView, TextId::wb_assign},
                           {periodView, TextId::wb_period},
                           {trendView, TextId::wb_trends},
                           {resetPlot, TextId::wb_reset_plot},
                           {viewA, TextId::wb_apply_a},
                           {viewB, TextId::wb_apply_b},
                           {boardReview, TextId::wb_review},
                           {boardNote, TextId::wb_note},
                           {markPending, TextId::wb_pending},
                           {markConfirmed, TextId::wb_confirmed},
                           {markHandled, TextId::wb_handled},
                           {copyEvents, TextId::wb_copy_events},
                           {exportEvents, TextId::wb_export_events},
                           {copyChain, TextId::wb_copy_chain},
                           {exportChain, TextId::wb_export_chain},
                           {sessionSave, TextId::wb_session_save},
                           {sessionOpen, TextId::wb_session_open},
                           {trendRun, TextId::wb_trend_run},
                           {trendCopy, TextId::wb_trend_copy},
                           {trendExport, TextId::wb_trend_export}})
            Control(w, L"BUTTON", UiText(entry.second), BS_PUSHBUTTON | BS_NOTIFY | WS_TABSTOP, entry.first);
        Control(w, L"BUTTON", UiText(TextId::workspace_relative), BS_AUTOCHECKBOX | WS_TABSTOP, relative);
        SendDlgItemMessageW(w, relative, BM_SETCHECK, BST_CHECKED, 0);
        for (int id : {result, chainDetail, sessionInfo}) {
            auto edit =
                Control(w, L"EDIT", L"", ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, id);
            SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);
            if (id == result)
                SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontMono), TRUE);
        }

        auto cb = Control(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, metric);
        for (const wchar_t* label : {L"CSQ", L"RSRP", L"RSRQ", L"SNR", L"RSSI"})
            SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(cb, CB_SETCURSEL, 0, 0);
        auto chain =
            Control(w, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, chainList);
        ListView_SetExtendedListViewStyle(chain, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        int column = 0;
        for (auto entry : {std::pair<TextId, int>{TextId::wb_chain_role, 140},
                           {TextId::wb_chain_time, 180},
                           {TextId::wb_chain_line, 250},
                           {TextId::ui_0178, 650}})
            LvAddCol(chain, column++, UiText(entry.first), S(entry.second));
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        cls.lpfnWndProc = PlotProc;
        cls.lpszClassName = L"dialWorkspacePlot";
        RegisterClassW(&cls);
        Control(w, cls.lpszClassName, L"", WS_TABSTOP, plot);
        cls.hCursor = LoadCursorW(nullptr, IDC_HAND);
        cls.lpfnWndProc = ChainProc;
        cls.lpszClassName = L"dialEvidenceChain";
        RegisterClassW(&cls);
        Control(w, cls.lpszClassName, L"", 0, chainDiagram);
        FillScopes();
        DefaultPeriod(scopeA, startA, endA);
        DefaultPeriod(scopeB, startB, endB);
        DefaultPeriod(trendScope, trendStart, trendEnd);
        RestoreWidgetState();
        g_building = false;
        FillList();
        ApplyModernTheme(w);
        Layout();
        if (g_page == 2 && !App().document.sources.empty()) {
            Calculate();
            RestorePlotView();
        }

        if (g_page == 4 && !App().document.sources.empty())
            BuildTrend();
        return 0;
    }

    case WM_APP + 77:
        RevealControl(reinterpret_cast<HWND>(lp));
        return 0;

    case WM_SIZE:
        if (!g_building)
            Layout();
        return 0;

    case WM_VSCROLL: {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_ALL;
        GetScrollInfo(w, SB_VERT, &info);
        int position = g_scroll;
        switch (LOWORD(wp)) {

        case SB_LINEUP:
            position -= 28;
            break;

        case SB_LINEDOWN:
            position += 28;
            break;

        case SB_PAGEUP:
            position -= int(info.nPage) - 28;
            break;

        case SB_PAGEDOWN:
            position += int(info.nPage) - 28;
            break;

        case SB_THUMBTRACK:
            position = info.nTrackPos;
            break;

        case SB_TOP:
            position = 0;
            break;

        case SB_BOTTOM:
            position = info.nMax;
            break;
        }

        g_scroll = position;
        Layout();
        return 0;
    }

    case WM_MOUSEWHEEL:
        g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 60;
        Layout();
        return 0;

    case WM_NOTIFY: {
        if (g_building)
            return 0;
        auto h = reinterpret_cast<NMHDR*>(lp);
        if (h->code == NM_SETFOCUS)
            RevealControl(h->hwndFrom);
        if (h->idFrom == tab && h->code == TCN_SELCHANGE) {
            const int choice = TabCtrl_GetCurSel(GetDlgItem(w, tab));
            SwitchWorkPage(choice == 0 ? 1 : choice == 2 ? 3 : g_deviceView);
            return 0;
        }

        if (h->idFrom == list && h->code == LVN_ITEMCHANGED) {
            if (g_page == 0)
                EditDevice();
            if (g_page == 1)
                RefreshChain();
            return 0;
        }

        if (h->idFrom == list && h->code == NM_DBLCLK && g_page == 1) {
            OpenBoardEvent();
            return 0;
        }

        if (h->idFrom == chainList && h->code == NM_DBLCLK) {
            const int row = ListView_GetNextItem(GetDlgItem(w, chainList), -1, LVNI_SELECTED);
            if (row >= 0 && std::size_t(row) < g_chain.size())
                JumpToRawLine(g_chain[row].line);
            return 0;
        }

        if (h->idFrom == list && h->code == NM_CUSTOMDRAW && g_page == 1) {
            auto draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && draw->nmcd.dwItemSpec < g_events.size()) {
                const auto status = g_events[draw->nmcd.dwItemSpec].review.status;
                draw->clrTextBk = status == ReviewStatus::Handled     ? th::qualityExcellent
                                  : status == ReviewStatus::Confirmed ? th::accentSoft
                                                                      : th::outageBand;
                draw->clrText = th::inkPri;
                return CDRF_NEWFONT;
            }
        }

        break;
    }

    case WM_COMMAND: {
        if (g_building)
            return 0;
        const int id = LOWORD(wp), notification = HIWORD(wp);
        if (notification == EN_SETFOCUS || notification == CBN_SETFOCUS || notification == BN_SETFOCUS) {
            RevealControl(reinterpret_cast<HWND>(lp));
            return 0;
        }

        if (id == assignView || id == periodView || id == trendView) {
            SwitchWorkPage(id == assignView ? 0 : id == periodView ? 2 : 4);
            return 0;
        }

        if (id == boardScope || id == boardStatus || id == boardSort || id == boardSearch || id == boardMinimum) {
            if (notification == CBN_SELCHANGE || notification == EN_CHANGE)
                BuildBoard();
            return 0;
        }

        if (id == boardReview) {
            OpenBoardEvent();
            return 0;
        }

        if (id == boardNote) {
            OpenBoardEvent(true);
            return 0;
        }

        if (id == markPending || id == markConfirmed || id == markHandled) {
            MarkBoardEvents(id == markPending     ? ReviewStatus::Pending
                            : id == markConfirmed ? ReviewStatus::Confirmed
                                                  : ReviewStatus::Handled);
            return 0;
        }

        if (id == copyEvents) {
            CopyWindowText(w, U8ToW(BoardCsv()));
            return 0;
        }

        if (id == exportEvents) {
            SaveText(L"diallog_events.csv", L"CSV (*.csv)\0*.csv\0\0", BoardCsv());
            return 0;
        }

        if (id == copyChain) {
            CopyWindowText(w, ChainText());
            return 0;
        }

        if (id == exportChain) {
            SaveText(L"diallog_evidence_chain.html", L"HTML (*.html)\0*.html\0\0",
                     TextHtml(UiText(TextId::wb_chain), ChainText()));
            return 0;
        }

        if (id == sessionSave) {
            SaveSession();
            return 0;
        }

        if (id == sessionOpen) {
            OpenSession();
            return 0;
        }

        if (id == trendRun) {
            BuildTrend();
            return 0;
        }

        if (id == trendCopy) {
            if (!g_trendText.empty())
                CopyWindowText(w, U8ToW(g_trendText));
            return 0;
        }

        if (id == trendExport) {
            if (g_trendText.empty())
                BuildTrend();
            if (!g_trendText.empty())
                SaveText(L"diallog_trends.html", L"HTML (*.html)\0*.html\0\0", TrendHtml());
            return 0;
        }

        if ((id >= startA && id <= endB && notification == EN_CHANGE) ||
            ((id == trendStart || id == trendEnd || id == trendCustom) && notification == EN_CHANGE) ||
            (id == trendStep && notification == CBN_SELCHANGE)) {
            g_compareText.clear();
            g_trendText.clear();
            g_signalA = {};
            g_signalB = {};
            g_trends.clear();
            ClearPlotCache();
            SetWindowTextW(GetDlgItem(w, result), L"");
            InvalidateRect(GetDlgItem(w, plot), nullptr, FALSE);
            if (id == trendStep)
                Layout();
            return 0;
        }

        if (id == save) {
            const int row = ListView_GetNextItem(GetDlgItem(w, list), -1, LVNI_SELECTED);
            if (row < 0 || std::size_t(row) >= App().document.sources.size())
                return 0;
            const auto& source = App().document.sources[row];
            auto previous = App().document.workspace;
            std::string name = WToU8(GetText(GetDlgItem(w, device)));
            auto begin = name.find_first_not_of(" \t\r\n"), end = name.find_last_not_of(" \t\r\n");
            name = begin == std::string::npos ? "" : name.substr(begin, end - begin + 1);
            App().document.workspace.devices[source.workspaceKey()] = {name, WToU8(GetText(GetDlgItem(w, firmware))),
                                                                       WToU8(GetText(GetDlgItem(w, configuration)))};
            if (!SaveWorkspaceRecords()) {
                App().document.workspace = std::move(previous);
                return 0;
            }

            auto& d = App().document;
            if (d.sourceMode == DocumentState::SourceMode::Device) {
                bool exists = false;
                for (const auto& pair : d.workspace.devices)
                    if (pair.second.device == d.selectedDevice)
                        exists = true;
                if (!exists) {
                    d.sourceMode = DocumentState::SourceMode::Independent;
                    d.selectedDevice.clear();
                }
            }

            g_building = true;
            FillScopes();
            DefaultPeriod(scopeA, startA, endA);
            DefaultPeriod(scopeB, startB, endB);
            DefaultPeriod(trendScope, trendStart, trendEnd);
            g_building = false;
            ClearPlotCache();
            FillList();
            UpdateSourceControls();
            RefreshAll();
            ShowModernNotice(UiText(TextId::workspace_saved), L"", ModernNoticeKind::Success);
            return 0;
        }

        if (id == run) {
            g_trendText.clear();
            Calculate();
            return 0;
        }

        if (id == copy) {
            if (!g_compareText.empty())
                CopyWindowText(w, U8ToW(g_compareText));
            return 0;
        }

        if (id == metric && notification == CBN_SELCHANGE) {
            HidePlotTip();
            InvalidateRect(GetDlgItem(w, plot), nullptr, FALSE);
            return 0;
        }

        if (id == relative) {
            ResetPlotCache();
            InvalidateRect(GetDlgItem(w, plot), nullptr, FALSE);
            return 0;
        }

        if (id == scopeA || id == scopeB || id == trendScope) {
            if (notification == CBN_SELCHANGE)
                DefaultPeriod(id,
                              id == scopeA   ? startA
                              : id == scopeB ? startB
                                             : trendStart,
                              id == scopeA   ? endA
                              : id == scopeB ? endB
                                             : trendEnd);
            return 0;
        }

        if (id == resetPlot) {
            g_plotWindow = g_plotBounds;
            HidePlotTip();
            UpdatePlotButtons();
            InvalidateRect(GetDlgItem(w, plot), nullptr, FALSE);
            return 0;
        }

        if (id == viewA || id == viewB) {
            ApplyPlotScope(id == viewB);
            return 0;
        }

        if (id == exportHtml) {
            if (g_compareText.empty())
                Calculate();
            if (!g_compareText.empty())
                SaveText(L"diallog_comparison.html", L"HTML (*.html)\0*.html\0\0", CompareHtml());
            return 0;
        }

        break;
    }

    case WM_CLOSE:
        ShowPage(0);
        return 0;

    case WM_DESTROY:
        g_workspace = nullptr;
        App().hReviewWorkbench = nullptr;
        g_events.clear();
        g_boardBaseEvents.clear();
        g_boardBaseReady = false;
        g_chain.clear();
        g_trends.clear();
        g_compareText.clear();
        g_trendText.clear();
        g_signalA = {};
        g_signalB = {};
        g_plotReady = false;
        for (auto& points : g_plotA)
            ChartSeries{}.swap(points);
        for (auto& points : g_plotB)
            ChartSeries{}.swap(points);
        for (auto& points : g_drawCache)
            ChartSeries{}.swap(points);
        for (auto& gaps : g_plotAGaps)
            std::vector<ChartGap>{}.swap(gaps);
        for (auto& gaps : g_plotBGaps)
            std::vector<ChartGap>{}.swap(gaps);
        return 0;

    case WM_CTLCOLORSTATIC:

    case WM_CTLCOLOREDIT:

    case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(
            ModernControlBrush(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp)));

    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(w, &rect);
        FillSolid(reinterpret_cast<HDC>(wp), rect, th::surface);
        return 1;
    }
    }

    return DefWindowProcW(w, msg, wp, lp);
}
