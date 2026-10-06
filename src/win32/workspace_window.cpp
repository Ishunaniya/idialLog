#include "workspace_window.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <set>
#include <limits>
#include <cstdio>
#include <cwchar>
#include <cmath>
#include <stdexcept>
#include "app_context.h"
#include "win_file_io.h"
#include "win_text.h"
#include "load_controller.h"
#include "source_workspace.h"
#include "ui_pages.h"
#include "text_view.h"
#include "text_catalog.h"
#include "modern_shell.h"
#include "theme.h"
#include "sha256.h"
#include "log_time.h"
#include "log_analysis.h"
#include "signal_chart.h"
#include "report_chart.h"
#include "incident_export.h"
#include "incident_review.h"
#include "version.h"
#include "app_settings.h"
#include "paint_buffer.h"
#include <sstream>
#include <iomanip>
#include <locale>

namespace dl {
namespace {
HWND g_workspace = nullptr, g_editor = nullptr;
int g_page = 1;
bool g_writable = true;
ReviewRecord g_edit;
std::vector<std::string> g_reviewKeys;

struct Scope {
    std::wstring label;
    std::string device;
    std::size_t source = SIZE_MAX;
};

std::vector<Scope> g_scopes;
std::string g_compareText;
SignalChartSeries g_signalA, g_signalB;
long long g_a0 = 0, g_a1 = 0, g_b0 = 0, g_b1 = 0;
constexpr int tab = 1400, list = 1401, device = 1402, firmware = 1403, configuration = 1404, save = 1405, hint = 1406,
              deviceLabel = 1407, firmwareLabel = 1408, configurationLabel = 1409, scopeA = 1410, scopeB = 1411,
              startA = 1412, endA = 1413, startB = 1414, endB = 1415, run = 1416, copy = 1417, exportHtml = 1418,
              relative = 1419, result = 1420, plot = 1421, metric = 1422, assignView = 1460, periodView = 1461,
              trendView = 1462, resetPlot = 1463, viewA = 1464, viewB = 1465, boardScope = 1470, boardStatus = 1471,
              boardSort = 1472, boardSearch = 1473, boardReview = 1474, boardNote = 1475, markPending = 1476,
              markConfirmed = 1477, markHandled = 1478, copyEvents = 1479, exportEvents = 1480, boardMinimum = 1486,
              boardMinimumLabel = 1487, chainDiagram = 1481, chainList = 1482, chainDetail = 1483, copyChain = 1484,
              exportChain = 1485, sessionName = 1490, sessionSave = 1491, sessionOpen = 1492, sessionInfo = 1493,
              trendScope = 1500, trendStart = 1501, trendEnd = 1502, trendStep = 1503, trendCustom = 1504,
              trendRun = 1505, trendCopy = 1506, trendExport = 1507;
void Layout();
void FillList();
void EditDevice();
void BuildBoard();
void RefreshChain();
void BuildTrend();
void ClearPlotCache();
void ResetPlotCache();
void RestoreWidgetState();
WorkspaceSession CaptureSession();
WorkspaceSession g_savedSession;
bool g_haveSavedSession = false, g_building = false;
int g_deviceView = 2, g_scroll = 0;
std::vector<WorkspaceEvent> g_events, g_boardBaseEvents;
std::string g_boardBaseScope;
bool g_boardBaseReady = false;
std::vector<ChainNode> g_chain;
std::vector<TrendPeriod> g_trends;
std::string g_trendText;
std::array<ChartSeries, 5> g_plotA, g_plotB;
std::array<std::vector<ChartGap>, 5> g_plotAGaps, g_plotBGaps;
std::array<int, 5> g_plotLow{{0, -140, -25, -200, -120}}, g_plotHigh{{31, -40, 0, 300, -20}};
std::array<ChartSeries, 2> g_drawCache;
int g_drawMode = -1, g_drawWidth = 0;
ChartTimeWindow g_drawWindow;
ChartTimeWindow g_plotBounds, g_plotWindow, g_dragWindow;
bool g_plotReady = false, g_hover = false, g_selecting = false, g_panning = false;
int g_hoverX = 0, g_dragX = 0;
RECT g_plotArea{};
HWND g_plotTooltip = nullptr;
std::wstring g_tooltipText;
int g_tipX = -1, g_tipY = -1;
long long g_tipTime = LLONG_MIN;
PaintAppearance g_tipAppearance{};
bool g_plotTracking = false;
unsigned long long g_plotRevision = 0;
PaintBuffer g_plotBase, g_plotFrame;

struct PlotPaintKey {
    unsigned long long revision;
    int width, height, mode, page;
    long long start, end;
    PaintAppearance appearance;

    bool operator==(const PlotPaintKey& other) const {
        return revision == other.revision && width == other.width && height == other.height && mode == other.mode &&
               page == other.page && start == other.start && end == other.end && appearance == other.appearance;
    }
};

PlotPaintKey g_plotPaintKey{};
bool g_plotBaseValid = false;
std::string ScopeKey(std::size_t index);

int TopTab() {
    return g_page == 1 ? 0 : g_page == 3 ? 2 : 1;
}

std::wstring StorePath() {
    wchar_t folder[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, folder)))
        return {};

    std::wstring root = std::wstring(folder) + L"\\dialLog";
    CreateDirectoryW(root.c_str(), nullptr);
    root += L"\\workspaces";
    CreateDirectoryW(root.c_str(), nullptr);

    std::vector<std::string> hashes;
    for (const auto& s : App().document.sources)
        hashes.push_back(s.workspaceKey());
    std::sort(hashes.begin(), hashes.end());

    Sha256 digest;
    for (const auto& h : hashes)
        digest.update(h);

    return root + L"\\" + U8ToW(digest.finish()) + L".json";
}

std::string LineIdentity(std::size_t line) {
    const auto& d = App().document;
    for (const auto& s : d.sources)
        if (s.first < s.last && line >= d.lines[s.first].lineNo && line <= d.lines[s.last - 1].lineNo)
            return s.workspaceKey() + ":" + std::to_string(line - s.rawLineOffset);
    return "unknown:" + std::to_string(line);
}

HWND Control(HWND w, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, w,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(App().hFontUI), TRUE);
    return c;
}

void Move(HWND w, int id, int x, int y, int width, int height) {
    MoveWindow(GetDlgItem(w, id), S(x), S(y - (w == g_workspace ? g_scroll : 0)), S(width), S(height), TRUE);
}

std::string Escape(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {

        case '&':
            out += "&amp;";
            break;

        case '<':
            out += "&lt;";
            break;

        case '>':
            out += "&gt;";
            break;

        case '"':
            out += "&quot;";
            break;

        default:
            out += c;
        }
    }

    return out;
}

bool Date(HWND w, int id, long long& t) {
    const auto text = WToU8(GetText(GetDlgItem(w, id)));
    int y, mo, d, h, m, s;
    char trailing;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d%c", &y, &mo, &d, &h, &m, &s, &trailing) != 6 || y < 1970 ||
        y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59)
        return false;
    t = mkEpoch(y, mo, d, h, m, s);
    return fmtTime(t, "FULL") == text;
}

LogView ScopeRows(std::size_t index) {
    LogView rows;
    auto& d = App().document;
    if (index >= g_scopes.size())
        return rows;
    rows.reserve(d.lines.size());
    for (const auto& row : d.lines)
        rows.push_back(&row);

    const auto& scope = g_scopes[index];
    if (scope.source != SIZE_MAX)
        d.restrictToSource(rows, scope.source);
    else
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const LogLine* row) {
                                      auto index = static_cast<std::size_t>(row - d.lines.data());
                                      auto source = std::upper_bound(
                                          d.sources.begin(), d.sources.end(), index,
                                          [](std::size_t n, const SourceSummary& s) { return n < s.last; });
                                      if (source == d.sources.end() || index < source->first)
                                          return true;
                                      auto found = d.workspace.devices.find(source->workspaceKey());
                                      return found == d.workspace.devices.end() || found->second.device != scope.device;
                                  }),
                   rows.end());
    return rows;
}

std::string ScopeDescription(std::size_t index) {
    if (index >= g_scopes.size())
        return {};

    const auto& scope = g_scopes[index];
    std::string text = WToU8(scope.label);
    const auto& d = App().document;
    for (std::size_t i = 0; i < d.sources.size(); ++i) {
        auto found = d.workspace.devices.find(d.sources[i].workspaceKey());
        if (found == d.workspace.devices.end())
            continue;
        if (scope.source != i && !(scope.source == SIZE_MAX && found->second.device == scope.device))
            continue;
        const auto& m = found->second;
        text += "\n" + WToU8(d.sources[i].label) + " | " + UiText8(TextId::workspace_device) + ": " + m.device + " | " +
                UiText8(TextId::workspace_firmware) + ": " + m.firmware + " | " +
                UiText8(TextId::workspace_configuration) + ": " + m.configuration;
    }

    return text;
}

void FillScopes() {
    g_scopes.clear();
    auto& d = App().document;
    std::set<std::string> groups;
    for (std::size_t i = 0; i < d.sources.size(); ++i) {
        auto m = d.workspace.devices.find(d.sources[i].workspaceKey());
        std::string key = m == d.workspace.devices.end() ? "" : m->second.device;
        g_scopes.push_back({d.sources[i].label, key, i});
        if (!key.empty())
            groups.insert(key);
    }

    for (const auto& key : groups)
        g_scopes.push_back({UiText(TextId::workspace_group) + U8ToW(key), key, SIZE_MAX});
    for (int id : {scopeA, scopeB, trendScope, boardScope}) {
        auto c = GetDlgItem(g_workspace, id);
        SendMessageW(c, CB_RESETCONTENT, 0, 0);
        if (id == boardScope)
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(UiText(TextId::wb_all)));
        for (const auto& s : g_scopes)
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.label.c_str()));
        SendMessageW(c, CB_SETCURSEL, 0, 0);
    }
}

void DefaultPeriod(int combo, int start, int end) {
    auto index = SendDlgItemMessageW(g_workspace, combo, CB_GETCURSEL, 0, 0);
    auto rows = ScopeRows(static_cast<std::size_t>(index));
    long long a = 0, b = 0;
    if (!rows.empty()) {
        a = b = rows.front()->t;
        for (const auto* r : rows) {
            a = std::min(a, r->t);
            b = std::max(b, r->t);
        }
    }

    SetWindowTextW(GetDlgItem(g_workspace, start), U8ToW(fmtTime(a, "FULL")).c_str());
    SetWindowTextW(GetDlgItem(g_workspace, end), U8ToW(fmtTime(b, "FULL")).c_str());
}

const ChartSeries& Series(const SignalChartSeries& s, int mode) {
    return mode == 1 ? s.rsrp : mode == 2 ? s.rsrq : mode == 3 ? s.snr10 : mode == 4 ? s.rssi : s.csq;
}

void Calculate() {
    long long a0, a1, b0, b1;
    if (!Date(g_workspace, startA, a0) || !Date(g_workspace, endA, a1) || !Date(g_workspace, startB, b0) ||
        !Date(g_workspace, endB, b1) || a1 < a0 || b1 < b0) {
        ShowModernNotice(UiText(TextId::workspace_invalid_time), L"", ModernNoticeKind::Warning);
        return;
    }

    g_a0 = a0;
    g_a1 = a1;
    g_b0 = b0;
    g_b1 = b1;
    auto ai = static_cast<std::size_t>(SendDlgItemMessageW(g_workspace, scopeA, CB_GETCURSEL, 0, 0)),
         bi = static_cast<std::size_t>(SendDlgItemMessageW(g_workspace, scopeB, CB_GETCURSEL, 0, 0));
    if (ai >= g_scopes.size() || bi >= g_scopes.size())
        return;

    auto ar = ScopeRows(ai), br = ScopeRows(bi);
    auto a = comparePeriod(ar, g_a0, g_a1), b = comparePeriod(br, g_b0, g_b1);
    const bool same = !g_scopes[ai].device.empty() && g_scopes[ai].device == g_scopes[bi].device;
    g_compareText =
        periodComparisonText(a, b, ScopeDescription(ai) + " " + fmtTime(g_a0, "FULL") + " → " + fmtTime(g_a1, "FULL"),
                             ScopeDescription(bi) + " " + fmtTime(g_b0, "FULL") + " → " + fmtTime(g_b1, "FULL"), same);

    auto signal = [&](const LogView& rows, long long start, long long end) {
        const auto values = buildMetrics(rows);
        MetricView view;
        for (const auto& metric : values)
            if (metric.t >= start && metric.t <= end)
                view.push_back(&metric);
        return reportedSignalSeries(view);
    };

    g_signalA = signal(ar, g_a0, g_a1);
    g_signalB = signal(br, g_b0, g_b1);
    SetWindowTextW(GetDlgItem(g_workspace, result), U8ToW(g_compareText).c_str());
    ResetPlotCache();
    InvalidateRect(GetDlgItem(g_workspace, plot), nullptr, FALSE);
}

struct PlotRuntime {
    ULONG_PTR token = 0;

    PlotRuntime() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
            token = 0;
    }

    ~PlotRuntime() {
        if (token)
            Gdiplus::GdiplusShutdown(token);
    }
};

#include "workspace_extras.inc"

void PaintPlotBase(HDC dc, RECT bounds) {
    SelectObject(dc, App().hFontSmall);
    FillSolid(dc, bounds, th::surface);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, th::inkPri);
    if (!g_plotReady) {
        DrawTextW(dc, UiText(TextId::workspace_compare_empty), -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        const int mode = static_cast<int>(SendDlgItemMessageW(g_workspace, metric, CB_GETCURSEL, 0, 0));
        const bool align = g_page == 2 && SendDlgItemMessageW(g_workspace, relative, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const auto& a = g_plotA[std::clamp(mode, 0, 4)];
        const auto& b = g_plotB[std::clamp(mode, 0, 4)];
        const long long start = g_plotWindow.start, end = g_plotWindow.end;
        long long low = g_plotLow[std::clamp(mode, 0, 4)], high = g_plotHigh[std::clamp(mode, 0, 4)];
        if (low == high)
            ++high;
        const int left = S(56), right = std::max(left + 1, static_cast<int>(bounds.right) - S(18));
        const int top = S(25), bottom = std::max(top + 1, static_cast<int>(bounds.bottom) - S(48));
        g_plotArea = {left, top, right, bottom};
        for (int i = 0; i <= 4; ++i) {
            const int y = top + (bottom - top) * i / 4;
            const auto value = high - (high - low) * i / 4;
            wchar_t label[48]{};
            if (mode == 3)
                std::swprintf(label, 48, L"%.1f", value / 10.0);
            else
                std::swprintf(label, 48, L"%lld", value);
            TextOutW(dc, S(4), y - S(7), label, lstrlenW(label));
            HPEN pen = CreatePen(PS_SOLID, 1, th::border);
            auto oldPen = SelectObject(dc, pen);
            MoveToEx(dc, left, y, nullptr);
            LineTo(dc, right, y);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }

        static PlotRuntime runtime;
        if (g_drawMode != mode || g_drawWidth != right - left || g_drawWindow.start != start ||
            g_drawWindow.end != end) {
            downsampleChartSeries(a, start, end, std::max(1, right - left), g_drawCache[0]);
            downsampleChartSeries(b, start, end, std::max(1, right - left), g_drawCache[1]);
            g_drawMode = mode;
            g_drawWidth = right - left;
            g_drawWindow = {start, end};
        }

        int index = 0;
        for (const auto* input : {&a, &b}) {
            (void)input;
            const auto& points = g_drawCache[index];
            const auto& gaps = index ? g_plotBGaps[std::clamp(mode, 0, 4)] : g_plotAGaps[std::clamp(mode, 0, 4)];
            const COLORREF color = index ? th::s2_green : th::s1_blue;
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            const Gdiplus::Color ink(255, GetRValue(color), GetGValue(color), GetBValue(color));
            Gdiplus::Pen pen(ink, std::max(1.5f, App().dpi / 96.0f * 1.5f));
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            if (index)
                pen.SetDashStyle(Gdiplus::DashStyleDash);
            Gdiplus::SolidBrush dots(ink);
            long long previous = 0;
            Gdiplus::PointF last;
            bool first = true;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const auto& point = points[i];
                const Gdiplus::PointF at(
                    left + static_cast<float>((static_cast<long double>(point.first) - start) /
                                              std::max(1.0L, static_cast<long double>(end) - start) * (right - left)),
                    bottom - static_cast<float>((static_cast<long double>(point.second) - low) / (high - low) *
                                                (bottom - top)));
                const auto gap = std::lower_bound(gaps.begin(), gaps.end(), previous,
                                                  [](const ChartGap& g, long long t) { return g.first < t; });
                const bool begin = first || (gap != gaps.end() && gap->first < point.first);
                if (!begin)
                    graphics.DrawLine(&pen, last, at);
                if (points.size() <= 60 || begin || i + 1 == points.size()) {
                    const float radius = S(points.size() == 1 ? 3 : 2);
                    graphics.FillEllipse(&dots, at.X - radius, at.Y - radius, radius * 2, radius * 2);
                }

                last = at;
                previous = point.first;
                first = false;
            }

            ++index;
        }

        const auto firstLabel = g_page == 4 ? UiText(TextId::wb_trend_p50) : L"A";
        TextOutW(dc, left, S(4), firstLabel, lstrlenW(firstLabel));
        if (g_page == 2)
            TextOutW(dc, left + S(58), S(4), L"B", 1);
        {
            Gdiplus::Graphics legend(dc);
            legend.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const auto color = [](COLORREF value) {
                return Gdiplus::Color(255, GetRValue(value), GetGValue(value), GetBValue(value));
            };

            Gdiplus::Pen aPen(color(th::s1_blue), 2.0f), bPen(color(th::s2_green), 2.0f);
            bPen.SetDashStyle(Gdiplus::DashStyleDash);
            if (g_page == 2)
                legend.DrawLine(&aPen, left + S(17), S(12), left + S(44), S(12));
            if (g_page == 2)
                legend.DrawLine(&bPen, left + S(75), S(12), left + S(102), S(12));
        }

        const std::wstring title = mode == 3   ? L"SNR dB"
                                   : mode == 0 ? L"CSQ"
                                   : mode == 2 ? L"RSRQ dB"
                                   : mode == 1 ? L"RSRP dBm"
                                               : L"RSSI dBm";
        TextOutW(dc, left + S(118), S(4), title.c_str(), title.size());
        const std::wstring aText = align ? std::to_wstring(start) + L" s" : U8ToW(fmtTime(start, "FULL"));
        const std::wstring bText = align ? std::to_wstring(end) + L" s" : U8ToW(fmtTime(end, "FULL"));
        TextOutW(dc, left, bottom + S(10), aText.c_str(), aText.size());
        RECT label{left, bottom + S(10), right, bounds.bottom};
        DrawTextW(dc, bText.c_str(), -1, &label, DT_RIGHT | DT_SINGLELINE);
    }
}

LRESULT CALLBACK PlotProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (PlotInput(window, message, wp, lp))
        return 0;
    if (message == WM_GETDLGCODE)
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    if (message == WM_ERASEBKGND)
        return 1;
    if (message != WM_PAINT)
        return DefWindowProcW(window, message, wp, lp);

    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    RECT bounds{};
    GetClientRect(window, &bounds);
    const PlotPaintKey key{g_plotRevision,
                           int(bounds.right),
                           int(bounds.bottom),
                           int(SendDlgItemMessageW(g_workspace, metric, CB_GETCURSEL, 0, 0)),
                           g_page,
                           g_plotWindow.start,
                           g_plotWindow.end,
                           CurrentPaintAppearance()};
    const bool buffered = g_plotFrame.ensure(target, bounds.right, bounds.bottom) &&
                          g_plotBase.ensure(target, bounds.right, bounds.bottom);
    HDC dc = buffered ? g_plotFrame.dc() : target;
    const int saved = SaveDC(dc);

    if (buffered) {
        if (!g_plotBaseValid || !(g_plotPaintKey == key)) {
            const int baseSaved = SaveDC(g_plotBase.dc());
            PaintPlotBase(g_plotBase.dc(), bounds);
            RestoreDC(g_plotBase.dc(), baseSaved);
            g_plotPaintKey = key;
            g_plotBaseValid = true;
            if (g_hover && g_tipY >= 0)
                ShowPlotTip(window, g_hoverX, g_tipY);
        }

        g_plotBase.copyTo(dc, paint.rcPaint);
    } else
        PaintPlotBase(dc, bounds);

    // Restore the dirty region from the static image before drawing overlays.
    IntersectClipRect(dc, paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right, paint.rcPaint.bottom);
    if (g_plotReady && (g_hover || g_selecting)) {
        HPEN cross = CreatePen(PS_DOT, 1, th::inkSec);
        auto old = SelectObject(dc, cross);
        MoveToEx(dc, g_hoverX, g_plotArea.top, nullptr);
        LineTo(dc, g_hoverX, g_plotArea.bottom);
        if (g_selecting) {
            auto brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, std::min(g_dragX, g_hoverX), g_plotArea.top, std::max(g_dragX, g_hoverX), g_plotArea.bottom);
            SelectObject(dc, brush);
        }

        SelectObject(dc, old);
        DeleteObject(cross);
    }

    RestoreDC(dc, saved);
    if (buffered)
        g_plotFrame.copyTo(target, paint.rcPaint);
    EndPaint(window, &paint);
    return 0;
}

void FillList() {
    if (g_page == 1) {
        BuildBoard();
        return;
    }

    if (g_page != 0)
        return;
    g_building = true;
    HWND table = GetDlgItem(g_workspace, list);
    ListView_DeleteAllItems(table);
    while (Header_GetItemCount(ListView_GetHeader(table)))
        ListView_DeleteColumn(table, 0);
    LvAddCol(table, 0, UiText(TextId::workspace_source), S(280));
    LvAddCol(table, 1, UiText(TextId::workspace_device), S(180));
    LvAddCol(table, 2, UiText(TextId::workspace_firmware), S(140));
    LvAddCol(table, 3, UiText(TextId::workspace_configuration), S(380));
    for (std::size_t i = 0; i < App().document.sources.size(); ++i) {
        const auto& source = App().document.sources[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = int(i);
        item.pszText = const_cast<wchar_t*>(source.label.c_str());
        ListView_InsertItem(table, &item);
        auto found = App().document.workspace.devices.find(source.workspaceKey());
        if (found != App().document.workspace.devices.end()) {
            int col = 1;
            for (const auto* text : {&found->second.device, &found->second.firmware, &found->second.configuration}) {
                auto value = U8ToW(*text);
                ListView_SetItemText(table, item.iItem, col++, const_cast<wchar_t*>(value.c_str()));
            }
        }
    }

    g_building = false;
    if (ListView_GetItemCount(table))
        ListView_SetItemState(table, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void Layout() {
    if (!g_workspace)
        return;

    RECT rect{};
    GetClientRect(g_workspace, &rect);
    const int width = MulDiv(rect.right, 96, App().dpi), height = MulDiv(rect.bottom, 96, App().dpi);
    const int content = std::max(height, g_page == 1   ? 850
                                         : g_page == 2 ? 860
                                         : g_page == 4 ? 820
                                         : g_page == 0 ? 620
                                                       : 460);
    g_scroll = std::clamp(g_scroll, 0, std::max(0, content - height));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, content - 1, UINT(height), g_scroll, 0};
    SetScrollInfo(g_workspace, SB_VERT, &info, TRUE);

    const std::initializer_list<int> controls = {list,
                                                 device,
                                                 firmware,
                                                 configuration,
                                                 save,
                                                 deviceLabel,
                                                 firmwareLabel,
                                                 configurationLabel,
                                                 scopeA,
                                                 scopeB,
                                                 startA,
                                                 endA,
                                                 startB,
                                                 endB,
                                                 run,
                                                 copy,
                                                 exportHtml,
                                                 relative,
                                                 result,
                                                 plot,
                                                 metric,
                                                 1423,
                                                 1424,
                                                 1425,
                                                 1426,
                                                 assignView,
                                                 periodView,
                                                 trendView,
                                                 resetPlot,
                                                 viewA,
                                                 viewB,
                                                 boardScope,
                                                 boardStatus,
                                                 boardSort,
                                                 boardSearch,
                                                 boardMinimum,
                                                 boardMinimumLabel,
                                                 boardReview,
                                                 boardNote,
                                                 markPending,
                                                 markConfirmed,
                                                 markHandled,
                                                 copyEvents,
                                                 exportEvents,
                                                 chainDiagram,
                                                 chainList,
                                                 chainDetail,
                                                 copyChain,
                                                 exportChain,
                                                 sessionName,
                                                 sessionSave,
                                                 sessionOpen,
                                                 sessionInfo,
                                                 trendScope,
                                                 trendStart,
                                                 trendEnd,
                                                 trendStep,
                                                 trendCustom,
                                                 trendRun,
                                                 trendCopy,
                                                 trendExport,
                                                 1508,
                                                 1509,
                                                 1510,
                                                 1511,
                                                 1494};

    std::set<int> visible;
    auto show = [&](std::initializer_list<int> ids) { visible.insert(ids.begin(), ids.end()); };
    Move(g_workspace, tab, 12, 8, width - 24, 30);
    Move(g_workspace, hint, 12, 48, width - 24, 60);
    SetWindowTextW(GetDlgItem(g_workspace, hint), UiText(g_page == 0   ? TextId::workspace_device_hint
                                                         : g_page == 1 ? TextId::wb_board_basis
                                                         : g_page == 3 ? TextId::wb_session_basis
                                                         : g_page == 4 ? TextId::wb_trend_basis
                                                                       : TextId::wb_plot_help));

    if (TopTab() == 1) {
        show({assignView, periodView, trendView});
        SetModernButtonActive(GetDlgItem(g_workspace, assignView), g_page == 0);
        SetModernButtonActive(GetDlgItem(g_workspace, periodView), g_page == 2);
        SetModernButtonActive(GetDlgItem(g_workspace, trendView), g_page == 4);
        int col = (width - 32) / 3;
        Move(g_workspace, assignView, 12, 114, col, 30);
        Move(g_workspace, periodView, 16 + col, 114, col, 30);
        Move(g_workspace, trendView, 20 + 2 * col, 114, col, 30);
    }

    auto style = GetWindowLongPtrW(GetDlgItem(g_workspace, list), GWL_STYLE);
    SetWindowLongPtrW(GetDlgItem(g_workspace, list), GWL_STYLE,
                      g_page == 1 ? style & ~LVS_SINGLESEL : style | LVS_SINGLESEL);

    const int col = (width - 32) / 2;
    if (g_page == 0) {
        show({list, device, firmware, configuration, save, deviceLabel, firmwareLabel, configurationLabel});
        Move(g_workspace, list, 12, 156, width - 24, content - 410);
        int y = content - 244;
        Move(g_workspace, deviceLabel, 12, y, 150, 28);
        Move(g_workspace, device, 168, y, width - 180, 28);
        Move(g_workspace, firmwareLabel, 12, y + 38, 150, 28);
        Move(g_workspace, firmware, 168, y + 38, width - 180, 28);
        Move(g_workspace, configurationLabel, 12, y + 76, 150, 28);
        Move(g_workspace, configuration, 168, y + 76, width - 180, 98);
        Move(g_workspace, save, width - 192, content - 44, 180, 32);
    }

    if (g_page == 1) {
        show({list, boardScope, boardStatus, boardSort, boardSearch, boardMinimum, boardMinimumLabel, boardReview,
              boardNote, markPending, markConfirmed, markHandled, copyEvents, exportEvents, chainDiagram, chainList,
              chainDetail, copyChain, exportChain});
        int third = (width - 32) / 3;
        Move(g_workspace, boardScope, 12, 114, third, 280);
        Move(g_workspace, boardStatus, 16 + third, 114, third, 180);
        Move(g_workspace, boardSort, 20 + 2 * third, 114, third, 120);
        Move(g_workspace, boardSearch, 12, 152, width - 280, 28);
        Move(g_workspace, boardMinimumLabel, width - 260, 152, 150, 28);
        Move(g_workspace, boardMinimum, width - 102, 152, 90, 28);
        Move(g_workspace, list, 12, 190, width - 24, content - 648);
        int y = content - 448;
        Move(g_workspace, boardReview, 12, y, third, 32);
        Move(g_workspace, boardNote, 16 + third, y, third, 32);
        Move(g_workspace, copyEvents, 20 + 2 * third, y, third, 32);
        y += 38;
        int quarter = (width - 36) / 4;
        Move(g_workspace, markPending, 12, y, quarter, 32);
        Move(g_workspace, markConfirmed, 16 + quarter, y, quarter, 32);
        Move(g_workspace, markHandled, 20 + 2 * quarter, y, quarter, 32);
        Move(g_workspace, exportEvents, 24 + 3 * quarter, y, quarter, 32);
        Move(g_workspace, chainDiagram, 12, content - 362, width - 24, 92);
        Move(g_workspace, chainList, 12, content - 260, width - 24, 120);
        Move(g_workspace, chainDetail, 12, content - 130, width - 24, 76);
        Move(g_workspace, copyChain, 12, content - 44, col, 32);
        Move(g_workspace, exportChain, 20 + col, content - 44, col, 32);
    }

    if (g_page == 2) {
        show({scopeA, scopeB, startA, endA, startB, endB, run,  copy,      exportHtml, relative,
              result, plot,   metric, 1423, 1424,   1425, 1426, resetPlot, viewA,      viewB});
        Move(g_workspace, scopeA, 12, 156, col, 300);
        Move(g_workspace, scopeB, 20 + col, 156, col, 300);
        Move(g_workspace, 1423, 12, 192, col, 20);
        Move(g_workspace, 1425, 20 + col, 192, col, 20);
        Move(g_workspace, startA, 12, 216, col, 28);
        Move(g_workspace, startB, 20 + col, 216, col, 28);
        Move(g_workspace, 1424, 12, 250, col, 20);
        Move(g_workspace, 1426, 20 + col, 250, col, 20);
        Move(g_workspace, endA, 12, 274, col, 28);
        Move(g_workspace, endB, 20 + col, 274, col, 28);
        int third = (width - 32) / 3;
        Move(g_workspace, run, 12, 312, third, 32);
        Move(g_workspace, copy, 16 + third, 312, third, 32);
        Move(g_workspace, exportHtml, 20 + 2 * third, 312, third, 32);
        Move(g_workspace, relative, 12, 352, width - 220, 30);
        Move(g_workspace, metric, width - 204, 352, 192, 220);
        Move(g_workspace, resetPlot, 12, 390, third, 32);
        Move(g_workspace, viewA, 16 + third, 390, third, 32);
        Move(g_workspace, viewB, 20 + 2 * third, 390, third, 32);
        int chartHeight = std::max(260, (content - 456) * 2 / 3);
        Move(g_workspace, plot, 12, 434, width - 24, chartHeight);
        Move(g_workspace, result, 12, 444 + chartHeight, width - 24, content - 456 - chartHeight);
    }

    if (g_page == 3) {
        show({1494, sessionName, sessionSave, sessionOpen, sessionInfo});
        Move(g_workspace, 1494, 12, 114, 160, 28);
        Move(g_workspace, sessionName, 180, 114, width - 192, 28);
        Move(g_workspace, sessionSave, 12, 154, col, 34);
        Move(g_workspace, sessionOpen, 20 + col, 154, col, 34);
        Move(g_workspace, sessionInfo, 12, 204, width - 24, content - 216);
        SessionInfo();
    }

    if (g_page == 4) {
        show({trendScope, trendStart, trendEnd, trendStep, trendCustom, trendRun, trendCopy, trendExport, result, plot,
              metric, resetPlot, 1508, 1509, 1510, 1511});
        Move(g_workspace, trendScope, 12, 156, width - 24, 260);
        Move(g_workspace, 1508, 12, 192, col, 20);
        Move(g_workspace, 1509, 20 + col, 192, col, 20);
        Move(g_workspace, trendStart, 12, 216, col, 28);
        Move(g_workspace, trendEnd, 20 + col, 216, col, 28);
        Move(g_workspace, 1510, 12, 250, col, 20);
        Move(g_workspace, 1511, 20 + col, 250, col, 20);
        Move(g_workspace, trendStep, 12, 274, col, 180);
        Move(g_workspace, trendCustom, 20 + col, 274, col, 28);
        EnableWindow(GetDlgItem(g_workspace, trendCustom),
                     SendDlgItemMessageW(g_workspace, trendStep, CB_GETCURSEL, 0, 0) == 3);
        int third = (width - 32) / 3;
        Move(g_workspace, trendRun, 12, 312, third, 32);
        Move(g_workspace, trendCopy, 16 + third, 312, third, 32);
        Move(g_workspace, trendExport, 20 + 2 * third, 312, third, 32);
        Move(g_workspace, resetPlot, 12, 352, col, 32);
        Move(g_workspace, metric, 20 + col, 352, col, 220);
        int chartHeight = std::max(260, (content - 418) * 2 / 3);
        Move(g_workspace, plot, 12, 396, width - 24, chartHeight);
        Move(g_workspace, result, 12, 406 + chartHeight, width - 24, content - 418 - chartHeight);
    }

    for (int id : controls) {
        auto child = GetDlgItem(g_workspace, id);
        const bool want = visible.count(id) != 0;
        const bool shown = (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) != 0;
        if (want != shown)
            ShowWindow(child, want ? SW_SHOW : SW_HIDE);
    }

    UpdatePlotButtons();
}

void EditDevice() {
    int row = ListView_GetNextItem(GetDlgItem(g_workspace, list), -1, LVNI_SELECTED);
    if (row < 0 || static_cast<std::size_t>(row) >= App().document.sources.size())
        return;
    const auto& s = App().document.sources[row];
    auto found = App().document.workspace.devices.find(s.workspaceKey());
    DeviceMetadata m = found == App().document.workspace.devices.end() ? DeviceMetadata{} : found->second;
    SetWindowTextW(GetDlgItem(g_workspace, device), U8ToW(m.device).c_str());
    SetWindowTextW(GetDlgItem(g_workspace, firmware), U8ToW(m.firmware).c_str());
    SetWindowTextW(GetDlgItem(g_workspace, configuration), U8ToW(m.configuration).c_str());
}

LRESULT CALLBACK EditorProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        Control(w, L"STATIC", UiText(TextId::workspace_review_hint), SS_LEFT, 1450);
        auto c = Control(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 1451);
        for (auto id : {TextId::review_pending, TextId::review_confirmed, TextId::review_handled})
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(UiText(id)));
        SendMessageW(c, CB_SETCURSEL, static_cast<int>(g_edit.status), 0);
        Control(w, L"EDIT", U8ToW(g_edit.note).c_str(), ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 1452);
        SendDlgItemMessageW(w, 1452, EM_SETLIMITTEXT, 8000, 0);
        Control(w, L"BUTTON", UiText(TextId::workspace_save), BS_PUSHBUTTON | WS_TABSTOP, 1453);
        ApplyModernTheme(w);
        return 0;
    }

    case WM_SIZE: {
        RECT r{};
        GetClientRect(w, &r);
        int width = MulDiv(r.right, 96, App().dpi), height = MulDiv(r.bottom, 96, App().dpi);
        Move(w, 1450, 16, 12, width - 32, 70);
        Move(w, 1451, 16, 88, 240, 300);
        Move(w, 1452, 16, 132, width - 32, height - 194);
        Move(w, 1453, width - 136, height - 46, 120, 30);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == 1453) {
            g_edit.note = WToU8(GetText(GetDlgItem(w, 1452)));
            g_edit.status = static_cast<ReviewStatus>(SendDlgItemMessageW(w, 1451, CB_GETCURSEL, 0, 0));
            auto& records = App().document.workspace.reviews;
            auto old = records.find(g_edit.key);
            bool had = old != records.end();
            ReviewRecord previous = had ? old->second : ReviewRecord{};
            records[g_edit.key] = g_edit;
            if (SaveWorkspaceRecords()) {
                DestroyWindow(w);
                RefreshIncidentAnnotation();
                if (g_workspace && g_page == 1)
                    BuildBoard();
                InvalidateRect(App().hFindings, nullptr, FALSE);
            } else {
                if (had)
                    records[g_edit.key] = previous;
                else
                    records.erase(g_edit.key);
            }

            return 0;
        }

        break;

    case WM_CLOSE:
        DestroyWindow(w);
        return 0;

    case WM_DESTROY:
        g_editor = nullptr;
        return 0;

    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(600), S(420)};
        return 0;

    case WM_CTLCOLORSTATIC:

    case WM_CTLCOLOREDIT:
        return reinterpret_cast<LRESULT>(
            ModernControlBrush(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp)));

    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(w, &r);
        FillSolid(reinterpret_cast<HDC>(wp), r, th::surface);
        return 1;
    }
    }

    return DefWindowProcW(w, msg, wp, lp);
}

std::string CompareHtml() {
    std::string html =
        "<!doctype html><html lang=\"" + std::string(IsEnglish() ? "en-US" : "zh-CN") +
        "\"><meta charset=\"utf-8\"><title>" + Escape(UiText8(TextId::workspace_compare)) +
        "</title><style>body{font-family:Segoe "
        "UI,sans-serif;margin:24px;background:#f5f7fa;color:#223043}pre{white-space:pre-wrap;overflow-wrap:anywhere;"
        "background:white;padding:16px}.signal-chart{background:white;padding:12px}.chart-scroll{overflow:auto}svg{min-"
        "width:880px;width:100%}.chart-grid{stroke:#dce3ed}.chart-label{font-size:13px;fill:#526072}.chart-axis{fill:"
        "none;stroke:#9aa8b9}.sample.dense{opacity:0}.sample:hover{opacity:1}</style><h1>" +
        Escape(UiText8(TextId::workspace_compare)) + " · v" + DL_VER_STR + "</h1><pre>" + Escape(g_compareText) +
        "</pre>";

    // Both curves share an axis. Relative alignment retains originals in hover titles.
    const bool align = SendDlgItemMessageW(g_workspace, relative, BM_GETCHECK, 0, 0) == BST_CHECKED;
    for (int mode = 0; mode < 5; ++mode) {
        ChartSeries a = Series(g_signalA, mode), b = Series(g_signalB, mode);
        ReportChartOptions o;
        o.english = IsEnglish();
        o.title = mode == 0 ? "CSQ" : mode == 1 ? "RSRP" : mode == 2 ? "RSRQ" : mode == 3 ? "SNR" : "RSSI";
        o.unit = mode == 0 ? "" : mode == 1 || mode == 4 ? "dBm" : "dB";
        o.low = mode == 0 ? 0 : mode == 1 ? -140 : mode == 2 ? -25 : mode == 3 ? -200 : -120;
        o.high = mode == 0 ? 31 : mode == 1 ? -40 : mode == 2 ? 0 : mode == 3 ? 300 : -20;
        o.scaled10 = mode == 3;
        o.color = "#2a78d6";
        if (align) {
            for (auto& p : a)
                p.first -= g_a0;
            for (auto& p : b)
                p.first -= g_b0;
        }

        o.start = align ? 0 : std::min(g_a0, g_b0);
        o.end = align ? std::max(g_a1 - g_a0, g_b1 - g_b0) : std::max(g_a1, g_b1);
        for (const auto* s : {&a, &b})
            for (const auto& p : *s) {
                o.low = std::min(o.low, p.second);
                o.high = std::max(o.high, p.second);
            }
        o.comparison = b;
        o.comparisonMode = true;
        o.relative = align;
        o.originalStart = g_a0;
        o.comparisonOriginalStart = g_b0;
        html += renderReportChart(a, {}, o).html;
    }

    return html + reportChartScript(IsEnglish()) + "</html>";
}

#include "workspace_controls.inc"
}  // namespace

void LoadWorkspaceRecords() {
    App().document.workspace = {};
    g_writable = true;
    auto path = StorePath();
    if (path.empty())
        return;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    try {
        std::string bytes;
        std::wstring error;
        if (!ReadBoundedFile(path, bytes, error, 16 * 1024 * 1024))
            throw std::runtime_error("Workspace read failed");
        App().document.workspace = decodeWorkspace(bytes);
    } catch (const std::exception& e) {
        g_writable = false;
        ShowModernNotice(UiText(TextId::workspace_error), U8ToW(e.what()).c_str(), ModernNoticeKind::Error);
    }
}

bool SaveWorkspaceRecords() {
    try {
        auto path = StorePath();
        std::wstring error;
        if (!g_writable || path.empty() ||
            !WriteFileBytesAtomic(path, encodeWorkspace(App().document.workspace), error)) {
            ShowModernNotice(UiText(TextId::workspace_error), error.c_str(), ModernNoticeKind::Error);
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        ShowModernNotice(UiText(TextId::workspace_error), U8ToW(e.what()).c_str(), ModernNoticeKind::Error);
        return false;
    }
}

void CloseWorkspaceWindows(bool reset) {
    if (g_workspace && !reset) {
        try {
            g_savedSession = CaptureSession();
            g_haveSavedSession = true;
        } catch (const std::exception&) {
        }
    }

    if (g_editor)
        DestroyWindow(g_editor);
    if (g_workspace)
        DestroyWindow(g_workspace);
    if (reset) {
        g_haveSavedSession = false;
        g_savedSession = {};
        g_page = 1;
        g_deviceView = 2;
        g_scroll = 0;
    }
}

void EnsureReviewWorkbench() {
    if (g_workspace)
        return;
    WNDCLASSW cls{};
    cls.lpfnWndProc = WorkspaceProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"dialWorkspace";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    g_workspace = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, UiText(TextId::wb_title),
                                  WS_CHILD | WS_CLIPCHILDREN | WS_VSCROLL, 0, 0, 800, 600, App().hMain, nullptr,
                                  cls.hInstance, nullptr);
    App().hReviewWorkbench = g_workspace;
    if (g_workspace && CurrentPage() == 9) {
        SendMessageW(App().hMain, WM_APP_SHELL_LAYOUT, 0, 0);
        ShowWindow(g_workspace, SW_SHOW);
    }

    EnableWindow(g_workspace, !LoadInProgress());
}

void ShowWorkspace(int page) {
    if (LoadInProgress())
        return;
    page = std::clamp(page, 0, 4);
    if (g_workspace)
        SwitchWorkPage(page);
    else {
        g_page = page;
        if (page == 0 || page == 2 || page == 4)
            g_deviceView = page;
    }

    ShowPage(9);
    EnsureReviewWorkbench();
    if (g_workspace)
        Layout();
    SetFocus(GetDlgItem(g_workspace, tab));
}

void RestoreWorkspaceSession(const WorkspaceSession& session) {
    CloseWorkspaceWindows(true);
    g_savedSession = session;
    g_haveSavedSession = true;
    g_page = session.workPage;
    if (g_page == 0 || g_page == 2 || g_page == 4)
        g_deviceView = g_page;
    SetSessionFilters(session.tag, session.message, session.since, session.until);
    App().document.timeRange = {session.rangeActive, session.start, session.end};
    MutableAppSettings().metricColumns = session.columns;
    RestoreMetricFilters({session.cell, session.rat, session.channel, session.hasDeny, session.deny});
    ApplyMetricColumnSettings();
    RefreshAll();
    ShowPage(session.page);
}

void ShowWorkbenchIncident(const Outage& outage) {
    const auto key = workspaceIncidentKey(App().document, outage);
    ShowWorkspace(1);
    g_building = true;
    SendDlgItemMessageW(g_workspace, boardScope, CB_SETCURSEL, 0, 0);
    SendDlgItemMessageW(g_workspace, boardStatus, CB_SETCURSEL, 0, 0);
    SetWindowTextW(GetDlgItem(g_workspace, boardSearch), L"");
    g_building = false;
    BuildBoard();
    for (std::size_t i = 0; i < g_events.size(); ++i)
        if (g_events[i].key == key) {
            HWND table = GetDlgItem(g_workspace, list);
            ListView_SetItemState(table, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(table, int(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(table, int(i), FALSE);
            break;
        }
    RefreshChain();
}

void EditManualReview(const std::string& key, const std::string& title) {
    if (g_editor)
        DestroyWindow(g_editor);
    auto found = App().document.workspace.reviews.find(key);
    g_edit = found == App().document.workspace.reviews.end() ? ReviewRecord{key, title, "", ReviewStatus::Pending}
                                                             : found->second;
    WNDCLASSW cls{};
    cls.lpfnWndProc = EditorProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"dialManualReview";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    g_editor =
        CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, U8ToW(title).c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        CW_USEDEFAULT, CW_USEDEFAULT, S(760), S(540), App().hMain, nullptr, cls.hInstance, nullptr);
    if (g_editor)
        SetFocus(GetDlgItem(g_editor, 1452));
}

std::string IncidentReviewKey(const Outage& outage) {
    return workspaceIncidentKey(App().document, outage);
}

std::string FindingReviewKey(const Finding& f) {
    Sha256 hash;
    hash.update("finding:" + f.title + ":" + f.detail);
    for (const auto& e : f.ev) {
        hash.update(LineIdentity(e.lineNo));
        hash.update(e.text);
    }

    return hash.finish();
}

std::string WorkspaceReviewText() {
    std::string out;
    for (const auto& pair : App().document.workspace.reviews)
        out += reviewRecordText(pair.second) + "\n";
    return out;
}

bool RouteWorkspaceMessage(MSG& message) {
    for (HWND w : {g_editor, g_workspace})
        if (w && (message.hwnd == w || IsChild(w, message.hwnd))) {
            if (w == g_workspace && g_page == 1 && message.message == WM_KEYDOWN && message.wParam == VK_F2 &&
                message.hwnd == GetDlgItem(w, list)) {
                OpenBoardEvent(true);
                return true;
            }

            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
                if (w == g_workspace && message.hwnd == GetDlgItem(w, plot)) {
                    DispatchMessageW(&message);
                    return true;
                }

                SendMessageW(w, WM_CLOSE, 0, 0);
                return true;
            }

            if (w == g_workspace && message.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) &&
                ((message.wParam >= '1' && message.wParam <= '9') || message.wParam == 'O' || message.wParam == 'F' ||
                 (message.wParam == '0' && message.hwnd != GetDlgItem(w, plot))))
                return false;
            if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN && w == g_workspace) {
                if (message.hwnd == GetDlgItem(w, list) && g_page == 1) {
                    OpenBoardEvent();
                    return true;
                }

                if (message.hwnd == GetDlgItem(w, chainList)) {
                    int row = ListView_GetNextItem(message.hwnd, -1, LVNI_SELECTED);
                    if (row >= 0 && std::size_t(row) < g_chain.size())
                        JumpToRawLine(g_chain[row].line);
                    return true;
                }
            }

            if (message.message == WM_KEYDOWN && message.wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                wchar_t cls[32]{};
                GetClassNameW(message.hwnd, cls, 32);
                if (lstrcmpW(cls, L"Edit") == 0) {
                    SendMessageW(message.hwnd, EM_SETSEL, 0, -1);
                    return true;
                }

                if (w == g_workspace && message.hwnd == GetDlgItem(w, list) && g_page == 1) {
                    ListView_SetItemState(message.hwnd, -1, LVIS_SELECTED, LVIS_SELECTED);
                    return true;
                }
            }

            if (!IsDialogMessageW(w, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            return true;
        }
    return false;
}

void ImportEvidencePackage() {
    if (LoadInProgress())
        return;
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = App().hMain;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"ZIP\0*.zip\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    dialog.lpstrTitle = UiText(TextId::package_import);
    if (!GetOpenFileNameW(&dialog))
        return;
    try {
        std::string bytes;
        std::wstring error;
        if (!ReadBoundedFile(path, bytes, error, kMaxInputBytes))
            throw std::runtime_error(WToU8(error));
        auto package = readEvidencePackage(bytes);
        if (package.originals.empty()) {
            ShowSelectableText(UiText(TextId::package_report),
                               std::wstring(UiText(TextId::package_no_original)) + L"\r\n\r\n" + U8ToW(package.report));
            return;
        }

        std::string().swap(bytes);
        package = {};
        LoadFiles({path});
    } catch (const std::exception& e) {
        ShowModernNotice(UiText(TextId::package_invalid), U8ToW(e.what()).c_str(), ModernNoticeKind::Error);
    }
}
}  // namespace dl
