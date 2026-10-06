// Private workbench controls, included inside workspace_window.cpp's namespace.
std::string ScopeKey(std::size_t index) {
    if (index >= g_scopes.size())
        return {};
    const auto& s = g_scopes[index];
    return s.source == SIZE_MAX ? "device:" + s.device : "source:" + App().document.sources[s.source].workspaceKey();
}

std::size_t ScopeIndex(const std::string& key) {
    for (std::size_t i = 0; i < g_scopes.size(); ++i)
        if (ScopeKey(i) == key)
            return i;
    return g_scopes.empty() ? SIZE_MAX : 0;
}

void ChooseScope(int id, const std::string& key) {
    const auto index = ScopeIndex(key);
    SendDlgItemMessageW(g_workspace, id, CB_SETCURSEL, index == SIZE_MAX ? -1 : index, 0);
}

void InvalidatePlotCursor(HWND window, int x) {
    RECT strip{x - S(2), g_plotArea.top, x + S(3), g_plotArea.bottom + S(1)};
    InvalidateRect(window, &strip, FALSE);
}

void HidePlotTip() {
    g_hover = false;
    g_tipX = g_tipY = -1;
    g_tipTime = LLONG_MIN;
    if (g_plotTooltip) {
        TOOLINFOW info{};
        info.cbSize = sizeof(info);
        info.hwnd = GetDlgItem(g_workspace, plot);
        info.uId = 1;
        SendMessageW(g_plotTooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&info));
    }
}

void UpdatePlotButtons() {
    const bool changed =
        g_plotReady && (g_plotWindow.start != g_plotBounds.start || g_plotWindow.end != g_plotBounds.end);
    EnableWindow(GetDlgItem(g_workspace, resetPlot), changed);
    for (int id : {viewA, viewB})
        EnableWindow(GetDlgItem(g_workspace, id), g_plotReady && g_page == 2);
}

void ClearPlotCache() {
    ++g_plotRevision;
    g_plotBaseValid = false;
    g_plotBase.reset();
    g_plotFrame.reset();
    g_drawMode = -1;
    g_plotReady = false;
    g_selecting = g_panning = false;
    HidePlotTip();
    for (auto& s : g_plotA)
        s.clear();
    for (auto& s : g_plotB)
        s.clear();
    if (GetCapture() == GetDlgItem(g_workspace, plot))
        ReleaseCapture();
    SetWindowTextW(GetDlgItem(g_workspace, plot), L"");
    UpdatePlotButtons();
}

void ResetPlotCache() {
    ++g_plotRevision;
    g_plotBaseValid = false;
    HidePlotTip();
    g_selecting = g_panning = false;
    g_drawMode = -1;
    g_plotLow = {{0, -140, -25, -200, -120}};
    g_plotHigh = {{31, -40, 0, 300, -20}};
    const bool relativeMode =
        g_page == 2 && SendDlgItemMessageW(g_workspace, relative, BM_GETCHECK, 0, 0) == BST_CHECKED;
    for (int m = 0; m < 5; ++m) {
        g_plotA[m] = Series(g_signalA, m);
        g_plotB[m] = Series(g_signalB, m);
        if (relativeMode) {
            for (auto& p : g_plotA[m])
                p.first -= g_a0;
            for (auto& p : g_plotB[m])
                p.first -= g_b0;
        }

        const auto gap = g_page == 4 && !g_trends.empty() ? g_trends.front().end - g_trends.front().start + 1 : 600;
        g_plotAGaps[m] = chartSampleGaps(g_plotA[m], gap);
        g_plotBGaps[m] = chartSampleGaps(g_plotB[m], gap);
        for (const auto* points : {&g_plotA[m], &g_plotB[m]})
            for (const auto& p : *points) {
                g_plotLow[m] = std::min(g_plotLow[m], p.second);
                g_plotHigh[m] = std::max(g_plotHigh[m], p.second);
            }
    }

    g_plotBounds = {relativeMode ? 0 : std::min(g_a0, g_b0),
                    relativeMode ? std::max(g_a1 - g_a0, g_b1 - g_b0) : std::max(g_a1, g_b1)};
    g_plotWindow = g_plotBounds;
    g_plotReady = g_page == 2 ? !g_compareText.empty() : g_page == 4 && !g_trendText.empty();
    UpdatePlotButtons();
}

long long PlotTime(int x) {
    return chartTimeAtPixel(g_plotWindow.start, g_plotWindow.end, g_plotArea.left, g_plotArea.right, x);
}

std::wstring MetricValue(int value, int mode) {
    wchar_t text[64]{};
    if (mode == 3)
        std::swprintf(text, 64, L"%.1f dB", value / 10.0);
    else
        std::swprintf(text, 64, L"%d%ls", value, mode == 0 ? L"" : mode == 2 ? L" dB" : L" dBm");
    return text;
}

void ShowPlotTip(HWND w, int x, int y) {
    const auto time = PlotTime(x);
    const auto appearance = CurrentPaintAppearance();
    const bool contentChanged = time != g_tipTime || !(g_tipAppearance == appearance);
    if (g_tipX == x && g_tipY == y && !contentChanged)
        return;
    if (contentChanged) {
        const bool aligned =
            g_page == 2 && SendDlgItemMessageW(g_workspace, relative, BM_GETCHECK, 0, 0) == BST_CHECKED;
        std::wstring text = aligned ? std::to_wstring(time) + L" s" : U8ToW(fmtTime(time, "FULL"));
        if (g_page == 4) {
            text += L"\n" + std::wstring(UiText(TextId::wb_trend_p50));
            for (const auto& p : g_trends)
                if (time >= p.start && time <= p.end) {
                    text += L"\n" + U8ToW(fmtTime(p.start, "FULL")) + L" → " + U8ToW(fmtTime(p.end, "FULL"));
                    int m = 0;
                    for (const auto* d : {&p.stats.csq, &p.stats.rsrp, &p.stats.rsrq, &p.stats.snr, &p.stats.rssi}) {
                        text += L"\n" +
                                std::wstring(m == 0   ? L"CSQ"
                                             : m == 1 ? L"RSRP"
                                             : m == 2 ? L"RSRQ"
                                             : m == 3 ? L"SNR"
                                                      : L"RSSI") +
                                L": " + (d->samples ? MetricValue(d->p50, m) : UiText(TextId::wb_near_missing)) +
                                L" (n=" + std::to_wstring(d->samples) + L")";
                        ++m;
                    }

                    break;
                }
        } else
            for (int m = 0; m < 5; ++m) {
                const auto name = m == 0 ? L"CSQ" : m == 1 ? L"RSRP" : m == 2 ? L"RSRQ" : m == 3 ? L"SNR" : L"RSSI";
                long long da = 0, db = 0;
                auto a = nearestChartPoint(Series(g_signalA, m), aligned ? g_a0 + time : time, &da);
                auto b = nearestChartPoint(Series(g_signalB, m), aligned ? g_b0 + time : time, &db);
                if (!a || da > 600)
                    a = nullptr;
                if (!b || db > 600)
                    b = nullptr;
                text += L"\nA " + std::wstring(name) + L": " +
                        (a ? MetricValue(a->second, m) + L" · " + U8ToW(fmtTime(a->first, "FULL"))
                           : UiText(TextId::wb_near_missing));
                text += L"\nB " + std::wstring(name) + L": " +
                        (b ? MetricValue(b->second, m) + L" · " + U8ToW(fmtTime(b->first, "FULL"))
                           : UiText(TextId::wb_near_missing));
                if (a && b) {
                    wchar_t difference[64]{};
                    std::swprintf(difference, 64, L"  Δ(B−A): %.1f",
                                  (static_cast<double>(b->second) - a->second) / (m == 3 ? 10 : 1));
                    text += difference;
                }
            }
        g_tooltipText = std::move(text);
        SetWindowTextW(w, g_tooltipText.c_str());
        g_tipTime = time;
        g_tipAppearance = appearance;
    }

    g_tipX = x;
    g_tipY = y;
    if (!g_plotTooltip) {
        g_plotTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, L"", WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0,
                                        0, 0, 0, w, nullptr, GetModuleHandleW(nullptr), nullptr);
        TOOLINFOW info{};
        info.cbSize = sizeof(info);
        info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        info.hwnd = w;
        info.uId = 1;
        info.lpszText = const_cast<wchar_t*>(L"");
        SendMessageW(g_plotTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        SendMessageW(g_plotTooltip, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontSmall), 0);
        SendMessageW(g_plotTooltip, TTM_SETMAXTIPWIDTH, 0, S(650));
    }

    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.hwnd = w;
    info.uId = 1;
    info.lpszText = const_cast<wchar_t*>(g_tooltipText.c_str());
    if (contentChanged) {
        SendMessageW(g_plotTooltip, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontSmall), 0);
        SendMessageW(g_plotTooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
    }

    POINT at{x + S(16), y + S(16)};
    ClientToScreen(w, &at);
    RECT screen{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
    at.x = std::min(at.x, screen.right - S(660));
    at.x = std::max(screen.left, at.x);
    at.y = std::min(at.y, screen.bottom - S(300));
    SendMessageW(g_plotTooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(at.x, at.y));
    SendMessageW(g_plotTooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
}

bool PlotInput(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETFOCUS) {
        SendMessageW(GetParent(w), WM_APP + 77, 0, reinterpret_cast<LPARAM>(w));
        return false;
    }

    if (msg == WM_SIZE || msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE) {
        if (msg == WM_SIZE) {
            g_plotBaseValid = false;
            HidePlotTip();
            InvalidateRect(w, nullptr, FALSE);
        }
        if (g_selecting || g_panning) {
            g_plotWindow = g_dragWindow;
            g_selecting = g_panning = false;
            if (GetCapture() == w)
                ReleaseCapture();
            InvalidateRect(w, nullptr, FALSE);
        }

        return false;
    }

    if (msg == WM_DESTROY) {
        g_plotBase.reset();
        g_plotFrame.reset();
        g_plotBaseValid = false;
        g_plotTracking = false;
        if (g_plotTooltip)
            DestroyWindow(g_plotTooltip);
        g_plotTooltip = nullptr;
        return false;
    }

    if (msg == WM_MOUSELEAVE) {
        if (g_hover)
            InvalidatePlotCursor(w, g_hoverX);
        HidePlotTip();
        g_plotTracking = false;
        return true;
    }

    if (!g_plotReady)
        return false;

    if (msg == WM_MOUSEWHEEL) {
        POINT at{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(w, &at);
        const double ticks = GET_WHEEL_DELTA_WPARAM(wp) / double(WHEEL_DELTA);
        g_plotWindow = navigateChartWindow(g_plotWindow, g_plotBounds, PlotTime(at.x),
                                           (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) ? 1 : std::pow(.75, ticks),
                                           (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) ? -.15 * ticks : 0);
        HidePlotTip();
        UpdatePlotButtons();
        InvalidateRect(w, nullptr, FALSE);
        return true;
    }

    if (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN) {
        POINT at{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!PtInRect(&g_plotArea, at))
            return true;
        if (g_hover)
            InvalidatePlotCursor(w, g_hoverX);
        g_dragX = g_hoverX = at.x;
        g_dragWindow = g_plotWindow;
        g_selecting = msg == WM_LBUTTONDOWN;
        g_panning = msg == WM_MBUTTONDOWN;
        HidePlotTip();
        SetFocus(w);
        SetCapture(w);
        return true;
    }

    if (msg == WM_MOUSEMOVE) {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g_panning) {
            const auto next =
                navigateChartWindow(g_dragWindow, g_plotBounds, g_dragWindow.start, 1,
                                    -double(x - g_dragX) / std::max(1L, g_plotArea.right - g_plotArea.left));
            if (next.start != g_plotWindow.start || next.end != g_plotWindow.end) {
                g_plotWindow = next;
                UpdatePlotButtons();
                InvalidateRect(w, nullptr, FALSE);
            }
        } else if (g_selecting) {
            const int next = std::clamp(x, int(g_plotArea.left), int(g_plotArea.right));
            if (next != g_hoverX) {
                RECT selection{std::min({g_dragX, g_hoverX, next}) - S(2), g_plotArea.top,
                               std::max({g_dragX, g_hoverX, next}) + S(3), g_plotArea.bottom + S(1)};
                g_hoverX = next;
                InvalidateRect(w, &selection, FALSE);
            }
        } else {
            POINT at{x, y};
            const bool hovered = PtInRect(&g_plotArea, at) != FALSE;
            if (g_hover && (!hovered || x != g_hoverX))
                InvalidatePlotCursor(w, g_hoverX);
            if (hovered && (!g_hover || x != g_hoverX))
                InvalidatePlotCursor(w, x);

            g_hover = hovered;
            g_hoverX = x;
            if (hovered)
                ShowPlotTip(w, x, y);
            else if (g_tipTime != LLONG_MIN)
                HidePlotTip();

            if (!g_plotTracking) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, w, 0};
                g_plotTracking = TrackMouseEvent(&track) != FALSE;
            }
        }

        return true;
    }

    if (msg == WM_LBUTTONUP || msg == WM_MBUTTONUP) {
        if (g_selecting && std::abs(GET_X_LPARAM(lp) - g_dragX) >= 3) {
            auto a = PlotTime(g_dragX), b = PlotTime(GET_X_LPARAM(lp));
            if (a > b)
                std::swap(a, b);
            if (a < b)
                g_plotWindow = {a, b};
        }

        g_selecting = g_panning = false;
        if (GetCapture() == w)
            ReleaseCapture();
        UpdatePlotButtons();
        InvalidateRect(w, nullptr, FALSE);
        return true;
    }

    if (msg == WM_KEYDOWN && (wp == VK_ESCAPE || (wp == '0' && (GetKeyState(VK_CONTROL) & 0x8000)))) {
        if (wp == VK_ESCAPE && (g_selecting || g_panning))
            g_plotWindow = g_dragWindow;
        else if (wp == '0')
            g_plotWindow = g_plotBounds;
        g_selecting = g_panning = false;
        if (GetCapture() == w)
            ReleaseCapture();
        HidePlotTip();
        UpdatePlotButtons();
        InvalidateRect(w, nullptr, FALSE);
        return true;
    }

    return false;
}

std::wstring StageText(ChainStage stage) {
    return UiText(stage == ChainStage::Fault     ? TextId::wb_chain_fault
                  : stage == ChainStage::Action  ? TextId::wb_chain_action
                  : stage == ChainStage::Network ? TextId::wb_chain_network
                                                 : TextId::wb_chain_business);
}

std::wstring EvidenceLine(std::size_t line) {
    const auto& d = App().document;
    for (const auto& source : d.sources)
        if (source.first < source.last && line >= d.lines[source.first].lineNo &&
            line <= d.lines[source.last - 1].lineNo)
            return source.label + L":" + std::to_wstring(line - source.rawLineOffset) + L" [#" + std::to_wstring(line) +
                   L"]";
    return L"#" + std::to_wstring(line);
}

std::wstring ChainText() {
    std::wstring text = UiText(TextId::wb_chain_basis);
    text += L"\r\n";
    if (!g_events.empty()) {
        int selected = ListView_GetNextItem(GetDlgItem(g_workspace, list), -1, LVNI_SELECTED);
        if (selected >= 0 && std::size_t(selected) < g_events.size())
            text += U8ToW(g_events[selected].source) + L" · " + U8ToW(reviewRecordText(g_events[selected].review));
    }

    for (int stage = 0; stage < 4; ++stage) {
        const auto kind = static_cast<ChainStage>(stage);
        text += L"\r\n" + StageText(kind) + L"\r\n";
        bool found = false;
        for (const auto& node : g_chain)
            if (node.stage == kind) {
                found = true;
                text += U8ToW(fmtTime(node.time, "FULL")) + L" " + EvidenceLine(node.line) + L" " + U8ToW(node.text) +
                        L"\r\n";
            }
        if (!found)
            text += std::wstring(UiText(TextId::wb_no_evidence)) + L"\r\n";
    }

    return text;
}

LRESULT CALLBACK ChainProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND)
        return 1;

    if (msg == WM_LBUTTONUP) {
        RECT r{};
        GetClientRect(w, &r);
        int index = std::clamp(GET_X_LPARAM(lp) * 4 / std::max(1L, r.right), 0L, 3L);
        for (const auto& node : g_chain)
            if (int(node.stage) == index) {
                JumpToRawLine(node.line);
                break;
            }
        return 0;
    }

    if (msg != WM_PAINT)
        return DefWindowProcW(w, msg, wp, lp);
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(w, &paint);
    RECT r{};
    GetClientRect(w, &r);
    FillSolid(dc, r, th::surface);
    auto font = SelectObject(dc, App().hFontSmall);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < 4; ++i) {
        RECT box{r.right * i / 4 + S(3), S(3), r.right * (i + 1) / 4 - S(12), r.bottom - S(3)};
        int count = 0;
        const ChainNode* first = nullptr;
        for (const auto& node : g_chain)
            if (int(node.stage) == i) {
                ++count;
                if (!first)
                    first = &node;
            }
        FillRound(dc, box, S(5),
                  !first   ? th::zebra
                  : i == 0 ? th::outageBand
                  : i == 1 ? th::accentSoft
                           : th::qualityExcellent,
                  th::border);
        std::wstring text = StageText(static_cast<ChainStage>(i)) + L"\n" +
                            (first ? U8ToW(fmtTime(first->time, "FULL")) + L"\n#" + std::to_wstring(first->line) +
                                         (count > 1 ? L" (" + std::to_wstring(count) + L")" : L"")
                                   : UiText(TextId::wb_no_evidence));
        SetTextColor(dc, th::inkPri);
        InflateRect(&box, -S(4), -S(3));
        DrawTextW(dc, text.c_str(), -1, &box, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        if (i < 3) {
            RECT arrow{r.right * (i + 1) / 4 - S(12), S(16), r.right * (i + 1) / 4 + S(4), r.bottom};
            DrawTextW(dc, L"→", 1, &arrow, DT_CENTER);
        }
    }

    SelectObject(dc, font);
    EndPaint(w, &paint);
    return 0;
}

void RefreshChain() {
    g_chain.clear();
    HWND chain = GetDlgItem(g_workspace, chainList);
    ListView_DeleteAllItems(chain);
    const int row = ListView_GetNextItem(GetDlgItem(g_workspace, list), -1, LVNI_SELECTED);
    if (row >= 0 && std::size_t(row) < g_events.size()) {
        const auto& e = g_events[row];
        auto catalog = buildIncidentCatalog(workspaceScopeRows(App().document, e.scope));
        const auto index = findIncident(catalog, e.outage);
        if (index < catalog.outages.size())
            g_chain = incidentEvidenceChain(reviewIncident(catalog, index, 300, 300));
    }

    for (std::size_t i = 0; i < g_chain.size(); ++i) {
        const auto& node = g_chain[i];
        auto stage = StageText(node.stage), time = U8ToW(fmtTime(node.time, "FULL")), line = EvidenceLine(node.line),
             message = U8ToW(node.text);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = int(i);
        item.pszText = const_cast<wchar_t*>(stage.c_str());
        ListView_InsertItem(chain, &item);
        ListView_SetItemText(chain, int(i), 1, const_cast<wchar_t*>(time.c_str()));
        ListView_SetItemText(chain, int(i), 2, const_cast<wchar_t*>(line.c_str()));
        ListView_SetItemText(chain, int(i), 3, const_cast<wchar_t*>(message.c_str()));
    }

    SetWindowTextW(GetDlgItem(g_workspace, chainDetail), ChainText().c_str());
    InvalidateRect(GetDlgItem(g_workspace, chainDiagram), nullptr, FALSE);
}

void BuildBoard() {
    if (!g_workspace)
        return;

    g_building = true;
    HWND table = GetDlgItem(g_workspace, list);
    std::string oldKey;
    int selected = ListView_GetNextItem(table, -1, LVNI_SELECTED);
    if (selected >= 0 && std::size_t(selected) < g_events.size())
        oldKey = g_events[selected].key;

    ListView_DeleteAllItems(table);
    while (Header_GetItemCount(ListView_GetHeader(table)))
        ListView_DeleteColumn(table, 0);
    for (auto entry : {std::pair<TextId, int>{TextId::workspace_status, 110},
                       {TextId::workspace_device, 140},
                       {TextId::workspace_source, 200},
                       {TextId::wb_event_start, 180},
                       {TextId::wb_duration, 100},
                       {TextId::wb_recovery, 130},
                       {TextId::workspace_note, 320}})
        LvAddCol(table, Header_GetItemCount(ListView_GetHeader(table)), UiText(entry.first), S(entry.second));

    const int scope = int(SendDlgItemMessageW(g_workspace, boardScope, CB_GETCURSEL, 0, 0)) - 1,
              status = int(SendDlgItemMessageW(g_workspace, boardStatus, CB_GETCURSEL, 0, 0)) - 1;
    const auto minimum = std::max(0LL, _wtoi64(GetText(GetDlgItem(g_workspace, boardMinimum)).c_str()));
    const auto query = WToU8(GetText(GetDlgItem(g_workspace, boardSearch)));
    const auto requestedScope = scope < 0 ? "" : ScopeKey(scope);
    if (!g_boardBaseReady || g_boardBaseScope != requestedScope) {
        g_boardBaseEvents = workspaceEvents(App().document, requestedScope);
        g_boardBaseScope = requestedScope;
        g_boardBaseReady = true;
    }

    g_events = g_boardBaseEvents;
    for (auto& event : g_events) {
        auto record = App().document.workspace.reviews.find(event.key);
        event.review = record == App().document.workspace.reviews.end()
                           ? ReviewRecord{event.key, fmtTime(event.outage.start, "FULL"), "", ReviewStatus::Pending}
                           : record->second;
    }

    g_events.erase(std::remove_if(g_events.begin(), g_events.end(),
                                  [&](const WorkspaceEvent& e) {
                                      const auto text = e.device + "\n" + e.source + "\n" +
                                                        fmtTime(e.outage.start, "FULL") + "\n" + e.review.note;
                                      return e.outage.dur < minimum ||
                                             (status >= 0 && int(e.review.status) != status) ||
                                             (!query.empty() && text.find(query) == std::string::npos);
                                  }),
                   g_events.end());
    if (SendDlgItemMessageW(g_workspace, boardSort, CB_GETCURSEL, 0, 0) == 1)
        std::stable_sort(g_events.begin(), g_events.end(),
                         [](const auto& a, const auto& b) { return a.outage.dur > b.outage.dur; });

    int restore = 0;
    for (std::size_t i = 0; i < g_events.size(); ++i) {
        const auto& e = g_events[i];
        if (e.key == oldKey)
            restore = int(i);
        const std::array<std::wstring, 7> cells{
            {U8ToW(reviewStatusText(e.review.status)),
             e.device.empty() ? UiText(TextId::wb_device_unknown) : U8ToW(e.device), U8ToW(e.source),
             U8ToW(fmtTime(e.outage.start, "FULL")), std::to_wstring(e.outage.dur),
             e.outage.recovered ? U8ToW(fmtTime(e.outage.end, "FULL")) : UiText(TextId::wb_open_outage),
             U8ToW(e.review.note)}};
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = int(i);
        item.pszText = const_cast<wchar_t*>(cells[0].c_str());
        ListView_InsertItem(table, &item);
        for (int c = 1; c < 7; ++c)
            ListView_SetItemText(table, int(i), c, const_cast<wchar_t*>(cells[c].c_str()));
    }

    g_building = false;
    if (!g_events.empty())
        ListView_SetItemState(table, restore, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    RefreshChain();
}

void SelectEventScope(const WorkspaceEvent& event) {
    auto& d = App().document;
    if (event.scope.compare(0, 7, "device:") == 0) {
        d.sourceMode = DocumentState::SourceMode::Device;
        d.selectedDevice = event.scope.substr(7);
    } else {
        d.sourceMode = DocumentState::SourceMode::Independent;
        for (std::size_t i = 0; i < d.sources.size(); ++i)
            if ("source:" + d.sources[i].workspaceKey() == event.scope)
                d.selectedSource = i;
        d.selectedDevice.clear();
    }
}

void OpenBoardEvent(bool note = false) {
    const int row = ListView_GetNextItem(GetDlgItem(g_workspace, list), -1, LVNI_SELECTED);
    if (row < 0 || std::size_t(row) >= g_events.size())
        return;
    const auto event = g_events[row];
    if (note) {
        EditManualReview(event.key,
                         std::string(UiText8(TextId::incident_title)) + " " + fmtTime(event.outage.start, "FULL"));
        return;
    }

    SelectEventScope(event);
    RefreshAll();
    ShowIncidentReview(event.outage);
}

void MarkBoardEvents(ReviewStatus status) {
    auto& records = App().document.workspace.reviews;
    const auto previous = records;
    HWND table = GetDlgItem(g_workspace, list);
    bool changed = false;
    for (int row = ListView_GetNextItem(table, -1, LVNI_SELECTED); row >= 0;
         row = ListView_GetNextItem(table, row, LVNI_SELECTED))
        if (std::size_t(row) < g_events.size()) {
            auto value = g_events[row].review;
            value.status = status;
            value.title =
                std::string(UiText8(TextId::incident_title)) + " " + fmtTime(g_events[row].outage.start, "FULL");
            records[value.key] = std::move(value);
            changed = true;
        }
    if (!changed)
        return;
    if (!SaveWorkspaceRecords()) {
        records = previous;
        return;
    }

    BuildBoard();
    RefreshIncidentAnnotation();
    InvalidateRect(App().hFindings, nullptr, FALSE);
}

std::string CsvField(const std::string& text) {
    std::string value = "\"";
    auto first = text.find_first_not_of(" \r\n\t");
    if (first != std::string::npos && std::string("=+-@").find(text[first]) != std::string::npos)
        value += '\'';
    for (char c : text) {
        if (c == '"')
            value += '"';
        value += c;
    }

    return value + '"';
}

std::string BoardCsv() {
    std::string text = "\xef\xbb\xbfstatus,device,source,start,end,duration_s,recovered,note\r\n";
    for (const auto& e : g_events)
        text += CsvField(reviewStatusText(e.review.status)) + "," + CsvField(e.device) + "," + CsvField(e.source) +
                "," + CsvField(fmtTime(e.outage.start, "FULL")) + "," +
                (e.outage.recovered ? CsvField(fmtTime(e.outage.end, "FULL")) : "\"\"") + "," +
                std::to_string(e.outage.dur) + "," + (e.outage.recovered ? "true" : "false") + "," +
                CsvField(e.review.note) + "\r\n";
    return text;
}

void SaveText(const wchar_t* initial, const wchar_t* filter, const std::string& text) {
    if (text.empty())
        return;
    wchar_t path[MAX_PATH]{};
    lstrcpynW(path, initial, MAX_PATH);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = App().hMain;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = filter;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    const auto host = g_workspace;
    EnableWindow(host, FALSE);
    bool accepted = GetSaveFileNameW(&dialog) != FALSE;
    if (IsWindow(host))
        EnableWindow(host, TRUE);
    if (accepted) {
        std::wstring error;
        if (!WriteFileBytesAtomic(path, text, error))
            ShowModernNotice(UiText(TextId::workspace_error), error.c_str(), ModernNoticeKind::Error);
    }
}

std::string TextHtml(const std::wstring& title, const std::wstring& text) {
    return "<!doctype html><html lang=\"" + std::string(IsEnglish() ? "en-US" : "zh-CN") +
           "\"><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\"><title>" +
           Escape(WToU8(title)) +
           "</title><style>body{font:14px/1.6 "
           "system-ui;margin:24px;color:#223043}pre{white-space:pre-wrap;overflow-wrap:anywhere} "
           ".signal-chart{background:#fff;padding:12px}.chart-scroll{overflow:auto}svg{width:100%;min-width:640px}."
           "chart-grid{stroke:#dce3ed}.chart-label{font-size:13px;fill:#526072}.chart-axis{fill:none;stroke:#9aa8b9}."
           "sample.dense{opacity:0}.sample:hover{opacity:1}</style><h1>" +
           Escape(WToU8(title)) + " · dialLog v" + DL_VER_STR + "</h1><pre>" + Escape(WToU8(text)) + "</pre></html>";
}

IncidentExportContext CompleteContext() {
    const auto& d = App().document;
    IncidentExportContext c;
    c.version = DL_VER_STR;
    c.build = __DATE__ " " __TIME__;
    c.workspace = d.workspace;
    c.selectedDevice = d.sourceMode == DocumentState::SourceMode::Device ? d.selectedDevice : "";
    c.continuation = d.sourceMode == DocumentState::SourceMode::Continuation;
    if (d.selectedSource < d.sources.size())
        c.selectedSourceHash = d.sources[d.selectedSource].workspaceKey();
    if (d.sources.size() > 1000)
        throw std::runtime_error("Too many sources");
    std::size_t totalBytes = 0;
    for (std::size_t i = 0; i < d.sources.size(); ++i) {
        const auto& s = d.sources[i];
        c.sources.push_back({WToU8(s.label), i + 1, s.first < s.last ? d.lines[s.first].lineNo : s.rawLineOffset + 1,
                             s.first < s.last ? d.lines[s.last - 1].lineNo : s.rawLineOffset, s.rawLineOffset});
        std::string bytes;
        std::wstring error;
        if (!s.pastedBytes.empty())
            bytes = s.pastedBytes;
        else if (!ReadOriginalBytes({s.originalPath, s.originalEntry, s.originalHash}, bytes, error))
            throw std::runtime_error(WToU8(error));
        if (bytes.size() > 256u * 1024 * 1024 || bytes.size() > 512u * 1024 * 1024 - totalBytes)
            throw std::runtime_error("Session originals exceed package limit");
        totalBytes += bytes.size();
        c.originals.push_back({s.originalEntry, WToU8(s.label), std::move(bytes), s.originalHash, s.workspaceKey()});
    }

    for (const auto& note : EvidenceBookmarks())
        c.bookmarks.push_back({note.lineNo, WToU8(note.text)});
    return c;
}

WorkspaceSession CaptureSession() {
    auto s = g_haveSavedSession ? g_savedSession : WorkspaceSession{};
    auto& d = App().document;
    s.page = CurrentPage();
    s.workPage = g_page;
    s.tag = WToU8(GetText(App().hTagBox));
    s.message = WToU8(GetText(App().hGrepBox));
    s.since = WToU8(GetText(App().hSinceBox));
    s.until = WToU8(GetText(App().hUntilBox));
    s.rangeActive = d.timeRange.active;
    s.start = d.timeRange.start;
    s.end = d.timeRange.end;
    s.columns = GetAppSettings().metricColumns;
    auto quick = CaptureMetricFilters();
    s.cell = quick.cell;
    s.rat = quick.rat;
    s.channel = quick.channel;
    s.hasDeny = quick.hasDeny;
    s.deny = quick.deny;
    if (g_workspace) {
        s.name = WToU8(GetText(GetDlgItem(g_workspace, sessionName)));
        const auto boardIndex = SendDlgItemMessageW(g_workspace, boardScope, CB_GETCURSEL, 0, 0);
        s.boardScope = boardIndex <= 0 ? "" : ScopeKey(boardIndex - 1);
        s.boardStatus = int(SendDlgItemMessageW(g_workspace, boardStatus, CB_GETCURSEL, 0, 0)) - 1;
        s.boardSort = int(SendDlgItemMessageW(g_workspace, boardSort, CB_GETCURSEL, 0, 0));
        s.boardQuery = WToU8(GetText(GetDlgItem(g_workspace, boardSearch)));
        s.boardMinimum = _wtoi64(GetText(GetDlgItem(g_workspace, boardMinimum)).c_str());
        const auto a = SendDlgItemMessageW(g_workspace, scopeA, CB_GETCURSEL, 0, 0),
                   b = SendDlgItemMessageW(g_workspace, scopeB, CB_GETCURSEL, 0, 0);
        s.comparison.a = ScopeKey(a);
        s.comparison.b = ScopeKey(b);
        long long a0 = 0, a1 = 0, b0 = 0, b1 = 0;
        if (Date(g_workspace, startA, a0) && Date(g_workspace, endA, a1) && Date(g_workspace, startB, b0) &&
            Date(g_workspace, endB, b1)) {
            s.comparison.aStart = a0;
            s.comparison.aEnd = a1;
            s.comparison.bStart = b0;
            s.comparison.bEnd = b1;
        } else if (!d.sources.empty())
            throw std::invalid_argument(WToU8(UiText(TextId::workspace_invalid_time)));
        s.comparison.relative = SendDlgItemMessageW(g_workspace, relative, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (g_page == 2) {
            s.comparison.metric = int(SendDlgItemMessageW(g_workspace, metric, CB_GETCURSEL, 0, 0));
            if (s.comparison.metric < 0)
                s.comparison.metric = 0;
            s.comparison.viewActive = g_plotReady;
            if (g_plotReady) {
                s.comparison.viewStart = g_plotWindow.start;
                s.comparison.viewEnd = g_plotWindow.end;
            }
        }

        s.trendScope = ScopeKey(SendDlgItemMessageW(g_workspace, trendScope, CB_GETCURSEL, 0, 0));
        long long lo = 0, hi = 0;
        if (Date(g_workspace, trendStart, lo) && Date(g_workspace, trendEnd, hi)) {
            s.trendStart = lo;
            s.trendEnd = hi;
        } else if (!d.sources.empty())
            throw std::invalid_argument(WToU8(UiText(TextId::workspace_invalid_time)));
        const int length = int(SendDlgItemMessageW(g_workspace, trendStep, CB_GETCURSEL, 0, 0));
        s.trendStep = length == 0   ? 3600
                      : length == 1 ? 86400
                      : length == 2 ? 604800
                                    : _wtoi64(GetText(GetDlgItem(g_workspace, trendCustom)).c_str());
    }

    return s;
}

void SessionInfo() {
    std::wstring text = UiText(TextId::wb_session_basis);
    text += L"\r\n\r\n" + std::wstring(UiText(TextId::wb_session_files)) + L": " +
            std::to_wstring(App().document.sources.size()) + L"\r\n";
    for (const auto& source : App().document.sources)
        text += source.label + L"\r\n";
    text += L"\r\n" + AnalysisSourceText() + L"\r\n" + AnalysisTimeRangeText() + L"\r\n" +
            std::wstring(UiText(TextId::workspace_reviews)) + L": " +
            std::to_wstring(App().document.workspace.reviews.size()) + L"\r\n" + UiText(TextId::ui_0170) + L": " +
            std::to_wstring(EvidenceBookmarks().size());
    if (!App().document.workspace.reviews.empty())
        text += L"\r\n\r\n" + U8ToW(WorkspaceReviewText());
    if (App().document.sources.empty())
        text += L"\r\n" + std::wstring(UiText(TextId::wb_session_empty));
    SetWindowTextW(GetDlgItem(g_workspace, sessionInfo), text.c_str());
    EnableWindow(GetDlgItem(g_workspace, sessionSave), !App().document.sources.empty());
}

void SaveSession() {
    if (App().document.sources.empty())
        return;
    try {
        const auto snapshot = CaptureSession();
        SaveText(L"diallog_session.zip", L"ZIP (*.zip)\0*.zip\0\0",
                 buildWorkspaceSessionZip(CompleteContext(), snapshot));
    } catch (const std::exception& e) {
        ShowModernNotice(UiText(TextId::wb_session_error), U8ToW(e.what()).c_str(), ModernNoticeKind::Error);
    }
}

void OpenSession() {
    if (LoadInProgress())
        return;
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = App().hMain;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"ZIP (*.zip)\0*.zip\0\0";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog))
        LoadFiles({path}, true);
}

void BuildTrend() {
    try {
        long long lo = 0, hi = 0;
        if (!Date(g_workspace, trendStart, lo) || !Date(g_workspace, trendEnd, hi))
            throw std::invalid_argument("Invalid dates");
        const int selected = int(SendDlgItemMessageW(g_workspace, trendScope, CB_GETCURSEL, 0, 0));
        if (selected < 0)
            throw std::invalid_argument("Missing source");
        const int length = int(SendDlgItemMessageW(g_workspace, trendStep, CB_GETCURSEL, 0, 0));
        long long step = length == 0   ? 3600
                         : length == 1 ? 86400
                         : length == 2 ? 604800
                                       : _wtoi64(GetText(GetDlgItem(g_workspace, trendCustom)).c_str());
        g_trends = workspaceTrend(ScopeRows(selected), lo, hi, step);
        g_trendText = workspaceTrendText(g_trends, ScopeDescription(selected));
        g_compareText.clear();
        g_signalA = {};
        g_signalB = {};
        for (const auto& period : g_trends) {
            int m = 0;
            for (const auto* d :
                 {&period.stats.csq, &period.stats.rsrp, &period.stats.rsrq, &period.stats.snr, &period.stats.rssi}) {
                if (d->samples) {
                    auto& series = m == 0   ? g_signalA.csq
                                   : m == 1 ? g_signalA.rsrp
                                   : m == 2 ? g_signalA.rsrq
                                   : m == 3 ? g_signalA.snr10
                                            : g_signalA.rssi;
                    series.emplace_back(period.start, d->p50);
                }

                ++m;
            }
        }

        g_a0 = g_b0 = lo;
        g_a1 = g_b1 = hi;
        SetWindowTextW(GetDlgItem(g_workspace, result), U8ToW(g_trendText).c_str());
        ResetPlotCache();
        InvalidateRect(GetDlgItem(g_workspace, plot), nullptr, FALSE);
    } catch (const std::exception& e) {
        g_trends.clear();
        g_trendText.clear();
        ClearPlotCache();
        SetWindowTextW(GetDlgItem(g_workspace, result), L"");
        InvalidateRect(GetDlgItem(g_workspace, plot), nullptr, FALSE);
        ShowModernNotice(UiText(TextId::wb_trend_invalid), U8ToW(e.what()).c_str(), ModernNoticeKind::Warning);
    }
}

std::string TrendHtml() {
    std::string html = TextHtml(UiText(TextId::wb_trends), U8ToW(g_trendText));
    html.erase(html.rfind("</html>"));
    for (int m = 0; m < 5; ++m) {
        ReportChartOptions o;
        o.english = IsEnglish();
        o.title = std::string(m == 0   ? "CSQ"
                              : m == 1 ? "RSRP"
                              : m == 2 ? "RSRQ"
                              : m == 3 ? "SNR"
                                       : "RSSI") +
                  " " + UiText8(TextId::wb_trend_p50);
        o.unit = m == 0 ? "" : m == 2 || m == 3 ? "dB" : "dBm";
        o.start = g_a0;
        o.end = g_a1;
        o.low = m == 0 ? 0 : m == 1 ? -140 : m == 2 ? -25 : m == 3 ? -200 : -120;
        o.high = m == 0 ? 31 : m == 1 ? -40 : m == 2 ? 0 : m == 3 ? 300 : -20;
        o.scaled10 = m == 3;
        o.color = "#2a78d6";
        o.gapSeconds = g_trends.empty() ? 600 : g_trends.front().end - g_trends.front().start + 1;
        for (const auto& p : g_trends) {
            const auto& d = m == 0   ? p.stats.csq
                            : m == 1 ? p.stats.rsrp
                            : m == 2 ? p.stats.rsrq
                            : m == 3 ? p.stats.snr
                                     : p.stats.rssi;
            o.periods.push_back({p.start, p.end, d.samples, d.p50});
            if (d.samples) {
                o.low = std::min(o.low, d.p50);
                o.high = std::max(o.high, d.p50);
            }
        }

        html += renderReportChart(Series(g_signalA, m), {}, o).html;
    }

    return html + reportChartScript(IsEnglish()) + "</html>";
}

void RestoreWidgetState() {
    if (!g_haveSavedSession)
        return;
    const auto& s = g_savedSession;
    SendDlgItemMessageW(g_workspace, boardScope, CB_SETCURSEL, s.boardScope.empty() ? 0 : ScopeIndex(s.boardScope) + 1,
                        0);
    SendDlgItemMessageW(g_workspace, boardStatus, CB_SETCURSEL, s.boardStatus + 1, 0);
    SendDlgItemMessageW(g_workspace, boardSort, CB_SETCURSEL, s.boardSort, 0);
    SetWindowTextW(GetDlgItem(g_workspace, boardSearch), U8ToW(s.boardQuery).c_str());
    SetWindowTextW(GetDlgItem(g_workspace, boardMinimum), std::to_wstring(s.boardMinimum).c_str());
    ChooseScope(scopeA, s.comparison.a);
    ChooseScope(scopeB, s.comparison.b);
    ChooseScope(trendScope, s.trendScope);
    for (auto p : {std::pair<int, long long>{startA, s.comparison.aStart},
                   {endA, s.comparison.aEnd},
                   {startB, s.comparison.bStart},
                   {endB, s.comparison.bEnd},
                   {trendStart, s.trendStart},
                   {trendEnd, s.trendEnd}})
        SetWindowTextW(GetDlgItem(g_workspace, p.first), U8ToW(fmtTime(p.second, "FULL")).c_str());
    SendDlgItemMessageW(g_workspace, relative, BM_SETCHECK, s.comparison.relative ? BST_CHECKED : BST_UNCHECKED, 0);
    SendDlgItemMessageW(g_workspace, metric, CB_SETCURSEL, s.comparison.metric, 0);
    SendDlgItemMessageW(g_workspace, trendStep, CB_SETCURSEL,
                        s.trendStep == 3600     ? 0
                        : s.trendStep == 86400  ? 1
                        : s.trendStep == 604800 ? 2
                                                : 3,
                        0);
    SetWindowTextW(GetDlgItem(g_workspace, trendCustom), std::to_wstring(s.trendStep).c_str());
    SetWindowTextW(GetDlgItem(g_workspace, sessionName), U8ToW(s.name).c_str());
}
