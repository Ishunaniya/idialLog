// chart_page.cpp — 信号图绘制、降采样缓存与指标页模型
#include "chart_page.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <set>
#include <string>

#include "app_context.h"
#include "chartmodel.h"
#include "log_time.h"
#include "memoryutil.h"
#include "modern_shell.h"
#include "signal_quality.h"
#include "theme.h"
#include "win_text.h"

namespace dl {

static ChartSeries g_csq;   // 供图表
static ChartSeries g_rsrp;  // LTE 详情图:RSRP dBm
static ChartSeries g_rsrq;  // LTE 详情图:RSRQ dB
static ChartSeries g_snr10; // 独立 SNR 图:SDK 原值(0.1dB)
static std::vector<ChartGap> g_csqGaps, g_rsrpGaps, g_rsrqGaps, g_snrGaps;
// 图表绘制缓存:按当前窗口像素宽度对完整序列做峰谷降采样。鼠标每移动 1px 都会
// 触发 WM_PAINT,缓存让这些重绘只消费数千点,不再反复扫描/绘制近十万点。
static ChartSeries g_chartCsqDraw, g_chartDetailDraw, g_chartSnrDraw;
static unsigned long long g_chartDataRevision = 1, g_chartCacheRevision = 0;
static int g_chartCacheWidth = -1, g_chartCacheDetail = -1;
static long long g_chartCacheT0 = 0, g_chartCacheT1 = 0;
static int  g_chartDetail    = 0;                     // 0=RSRP 1=RSRQ；SNR 固定独立显示
static int  g_chartHoverX   = -1;                     // 悬停 X(客户区),-1=未悬停
static int  g_chartHoverY   = -1;
static RECT g_chartModeRects[2]{};
static long long g_chartFocusTime = LLONG_MIN;
static long long g_chartVisibleT0 = 0, g_chartVisibleT1 = 0;
static int g_chartPlotLeft = 0, g_chartPlotRight = 0;
static RECT g_chartPlotRects[3]{};
static std::string g_latestCellId;
static std::size_t g_visibleCellCount = 0;
static bool g_chartHasInferredTime = false;

struct ChartGuide {
    int value;
    const wchar_t* label;
    COLORREF color;
};

static void ResetChartSampleCache() {
    if (++g_chartDataRevision == 0) g_chartDataRevision = 1; // 无符号回绕防御
    g_chartCacheRevision = 0;
    g_chartCacheWidth = g_chartCacheDetail = -1;
    g_chartCsqDraw.clear();
    g_chartDetailDraw.clear();
    g_chartSnrDraw.clear();
}


// ============================ ListView 工具 ============================

LRESULT CALLBACK ChartProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto detailSeries = [](int mode) -> const ChartSeries& {
        return mode == 1 ? g_rsrq : g_rsrp;
    };
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_SIZE) {
        // 尺寸变化时旧画面可能被系统复制保留，必须重绘整张图而非仅新增区域。
        g_chartHoverX = g_chartHoverY = -1;
        for (RECT& rect : g_chartModeRects) rect = RECT{};
        for (RECT& rect : g_chartPlotRects) rect = RECT{};
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_GETDLGCODE) return DLGC_WANTARROWS | DLGC_WANTCHARS;
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    if (msg == WM_KEYDOWN && (wp == VK_LEFT || wp == VK_RIGHT) && !App().document.metricView.empty()) {
        int row = ListView_GetNextItem(App().hMetric, -1, LVNI_SELECTED);
        if (row < 0) row = wp == VK_RIGHT ? 0 : static_cast<int>(App().document.metricView.size()) - 1;
        else row = std::max(0, std::min(static_cast<int>(App().document.metricView.size()) - 1,
                                       row + (wp == VK_RIGHT ? 1 : -1)));
        ListView_SetItemState(App().hMetric, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(App().hMetric, row, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(App().hMetric, row, FALSE);
        SetChartFocusTime(App().document.metricView[row]->t);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        SetFocus(hwnd);
        POINT point{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        bool changedMode = false;
        for (int mode = 0; mode < 2; ++mode) {
            if (PtInRect(&g_chartModeRects[mode], point) && !detailSeries(mode).empty()) {
                g_chartDetail = mode; changedMode = true; break;
            }
        }
        bool overPlot = false;
        for (const RECT& rect : g_chartPlotRects) if (PtInRect(&rect, point)) overPlot = true;
        if (!changedMode && overPlot && g_chartVisibleT1 >= g_chartVisibleT0) {
            const double fraction = double(point.x - g_chartPlotLeft) /
                                    std::max(1, g_chartPlotRight - g_chartPlotLeft);
            const long long target = g_chartVisibleT0 +
                static_cast<long long>(fraction * (g_chartVisibleT1 - g_chartVisibleT0));
            auto found = std::min_element(App().document.metricView.begin(), App().document.metricView.end(),
                [target](const MetricRow* a, const MetricRow* b) {
                    return std::llabs(a->t - target) < std::llabs(b->t - target);
                });
            if (found != App().document.metricView.end()) {
                const int row = static_cast<int>(found - App().document.metricView.begin());
                ListView_SetItemState(App().hMetric, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(App().hMetric, row, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(App().hMetric, row, FALSE);
                SetChartFocusTime((*found)->t);
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_SETCURSOR) {
        POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd, &point);
        bool overMode = false;
        for (int mode = 0; mode < 2; ++mode)
            if (!detailSeries(mode).empty() && PtInRect(&g_chartModeRects[mode], point)) overMode = true;
        SetCursor(LoadCursorW(nullptr, overMode ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    if (msg == WM_MOUSEMOVE) {
        int mx = (int)(short)LOWORD(lp);
        int my = (int)(short)HIWORD(lp);
        if (mx != g_chartHoverX || my != g_chartHoverY) {
            g_chartHoverX = mx; g_chartHoverY = my;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE; tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        g_chartHoverX = -1;
        g_chartHoverY = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg != WM_PAINT) return DefWindowProcW(hwnd, msg, wp, lp);

    PAINTSTRUCT ps;
    HDC hdcWin = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HDC hdc = CreateCompatibleDC(hdcWin);
    HBITMAP bmp = CreateCompatibleBitmap(hdcWin, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(hdc, bmp);
    HBRUSH bg = CreateSolidBrush(th::surface);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    SelectObject(hdc, App().hFontUI);
    SetBkMode(hdc, TRANSPARENT);

    const auto& detail = detailSeries(g_chartDetail);
    const wchar_t* detailName = g_chartDetail == 1 ? L"RSRQ" : L"RSRP";
    const int detailLo = g_chartDetail == 1 ? -25 : -140;
    const int detailHi = g_chartDetail == 1 ?   0 :  -40;

    for (RECT& rect : g_chartModeRects) rect = RECT{};
    for (RECT& rect : g_chartPlotRects) rect = RECT{};
    const int plotCount = (!g_csq.empty() ? 1 : 0) + (!detail.empty() ? 1 : 0) + (!g_snr10.empty() ? 1 : 0);
    const bool haveAny = plotCount > 0;
    const int minimumHeight = S(std::max(160, 77 + plotCount * 70));
    if (!haveAny || rc.right < S(260) || rc.bottom < minimumHeight) {
        const wchar_t* title = haveAny ? L"窗口空间不足" : L"暂无信号趋势";
        const wchar_t* detailText = haveAny ? L"收起顶部筛选栏、增加图表高度，或切换到图表全页。"
                                             : L"可切换到表格查看其他指标。CSQ 99 为未知，不绘制。";
        int cardW = std::min(S(460), std::max(S(260), static_cast<int>(rc.right) - S(64)));
        int cardH = S(112), x = (rc.right - cardW) / 2, y = (rc.bottom - cardH) / 2;
        RECT card{x, y, x + cardW, y + cardH}; FillRound(hdc, card, S(12), th::page, th::border);
        RECT icon{x + S(22), y + S(30), x + S(66), y + S(74)};
        FillRound(hdc, icon, S(22), th::accentSoft, th::accentSoft);
        HGDIOBJ oldFont = SelectObject(hdc, App().hFontSect); SetTextColor(hdc, th::accent);
        HPEN iconPen = CreatePen(PS_SOLID, std::max(1, S(2)), th::accent);
        HGDIOBJ previousPen = SelectObject(hdc, iconPen);
        const POINT trend[]{{x + S(30), y + S(59)}, {x + S(38), y + S(48)},
                            {x + S(46), y + S(55)}, {x + S(58), y + S(43)}};
        Polyline(hdc, trend, 4);
        SelectObject(hdc, previousPen); DeleteObject(iconPen);
        RECT titleRect{x + S(82), y + S(22), card.right - S(18), y + S(50)};
        SetTextColor(hdc, th::inkPri);
        DrawTextW(hdc, title, -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, App().hFontUI); SetTextColor(hdc, th::inkSec);
        RECT detailRect{x + S(82), y + S(54), card.right - S(18), y + S(92)};
        DrawTextW(hdc, detailText, -1, &detailRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
        SelectObject(hdc, oldFont);
        BitBlt(hdcWin, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, oldBmp); DeleteObject(bmp); DeleteDC(hdc); EndPaint(hwnd, &ps);
        return 0;
    }

    long long t0 = 0, t1 = 0;
    bool haveRange = false;
    auto takeRange = [&](const ChartSeries& s) {
        if (s.empty()) return;
        if (!haveRange) {
            t0 = s.front().first;
            t1 = s.back().first;
            haveRange = true;
        } else {
            if (s.front().first < t0) t0 = s.front().first;
            if (s.back().first  > t1) t1 = s.back().first;
        }
    };
    takeRange(g_csq);
    takeRange(detail);
    takeRange(g_snr10);
    const double total = std::max<double>(1.0, (double)(t1 - t0));
    // 分界说明使用独立右栏，不再压在曲线和末端采样点上。
    const int left = S(58), canvasRight = rc.right - S(12);
    const int guideWidth = S(108), right = canvasRight - guideWidth;
    g_chartVisibleT0 = t0; g_chartVisibleT1 = t1;
    g_chartPlotLeft = left; g_chartPlotRight = right;
    const int section = (static_cast<int>(rc.bottom) - S(77)) / plotCount;
    int slot = 0;
    auto plotRect = [&](bool present) {
        if (!present) return RECT{};
        const int y = S(26) + slot++ * section;
        return RECT{left, y + S(25), right, y + section - S(10)};
    };
    RECT top = plotRect(!g_csq.empty()), middle = plotRect(!detail.empty()), bottom = plotRect(!g_snr10.empty());
    g_chartPlotRects[0] = top; g_chartPlotRects[1] = middle; g_chartPlotRects[2] = bottom;
    const RECT lastPlot = !g_snr10.empty() ? bottom : !detail.empty() ? middle : top;
    auto X = [&](long long t) {
        return left + (int)((double)(t - t0) / total * (right - left));
    };

    const int plotWidth = std::max(1, right - left + 1);
    if (g_chartCacheRevision != g_chartDataRevision ||
        g_chartCacheWidth != plotWidth || g_chartCacheDetail != g_chartDetail ||
        g_chartCacheT0 != t0 || g_chartCacheT1 != t1) {
        downsampleChartSeries(g_csq, t0, t1, (size_t)plotWidth, g_chartCsqDraw);
        downsampleChartSeries(detail, t0, t1, (size_t)plotWidth, g_chartDetailDraw);
        downsampleChartSeries(g_snr10, t0, t1, (size_t)plotWidth, g_chartSnrDraw);
        g_chartCacheRevision = g_chartDataRevision;
        g_chartCacheWidth = plotWidth;
        g_chartCacheDetail = g_chartDetail;
        g_chartCacheT0 = t0;
        g_chartCacheT1 = t1;
    }

    auto paintOutages = [&](const RECT& pr) {
        HBRUSH band = CreateSolidBrush(th::outageBand);
        for (const auto& o : App().document.outages) {
            long long e = o.recovered ? o.end : t1;
            if (e < t0 || o.start > t1) continue;
            int xs = X(std::max(t0, o.start)), xe = X(std::min(t1, e));
            if (xe < xs + 2) xe = xs + 2;
            if (xe < pr.left || xs > pr.right) continue;
            RECT rb{ std::max(xs, (int)pr.left), pr.top,
                     std::min(xe, (int)pr.right), pr.bottom };
            FillRect(hdc, &rb, band);
        }
        DeleteObject(band);
    };

    auto drawPlot = [&](const RECT& pr, const ChartSeries& series,
                        const std::vector<ChartGap>& gaps,
                        int lo, int hi, COLORREF color,
                        std::initializer_list<ChartGuide> guides, bool scaled10) {
        if (series.empty()) return;
        auto Y = [&](int v) {
            int c = std::max(lo, std::min(hi, v));
            return pr.bottom - (int)((double)(c - lo) / (hi - lo) * (pr.bottom - pr.top));
        };
        paintOutages(pr);
        HPEN gridPen = CreatePen(PS_SOLID, 1, th::grid);
        HGDIOBJ oldPen = SelectObject(hdc, gridPen);
        SetTextColor(hdc, th::inkMuted);
        const int intervals = pr.bottom - pr.top < S(100) ? 2 : 4;
        for (int i = 0; i <= intervals; ++i) {
            int val = hi - (hi - lo) * i / intervals;
            int y = Y(val);
            MoveToEx(hdc, pr.left, y, nullptr); LineTo(hdc, pr.right, y);
            std::wstring lb = scaled10 ? FmtW(L"%.1f", val / 10.0) : FmtW(L"%d", val);
            SIZE sz{}; GetTextExtentPoint32W(hdc, lb.c_str(), (int)lb.size(), &sz);
            TextOutW(hdc, pr.left - sz.cx - S(5), y - sz.cy / 2, lb.c_str(), (int)lb.size());
        }
        SelectObject(hdc, oldPen); DeleteObject(gridPen);

        HPEN axisPen = CreatePen(PS_SOLID, 1, th::axis);
        oldPen = SelectObject(hdc, axisPen);
        HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, pr.left, pr.top, pr.right, pr.bottom);
        SelectObject(hdc, oldBr); SelectObject(hdc, oldPen); DeleteObject(axisPen);

        for (const ChartGuide& guide : guides) {
            HPEN thresholdPen = CreatePen(PS_SOLID, 1, guide.color);
            oldPen = SelectObject(hdc, thresholdPen);
            MoveToEx(hdc, pr.left, Y(guide.value), nullptr);
            LineTo(hdc, pr.right, Y(guide.value));
            SelectObject(hdc, oldPen); DeleteObject(thresholdPen);
        }

        if (!series.empty()) {
            HPEN dataPen = CreatePen(PS_SOLID, 2, color);
            oldPen = SelectObject(hdc, dataPen);
            HBRUSH dots = CreateSolidBrush(color);
            HGDIOBJ oldDotBrush = SelectObject(hdc, dots);
            bool first = true;
            long long previousTime = 0;
            for (const auto& p : series) {
                int x = X(p.first), y = Y(p.second);
                const auto gap = std::lower_bound(gaps.begin(), gaps.end(), previousTime,
                    [](const ChartGap& item, long long time) { return item.first < time; });
                const bool segmentStart = first || (gap != gaps.end() && gap->first < p.first);
                if (segmentStart) { MoveToEx(hdc, x, y, nullptr); first = false; }
                else LineTo(hdc, x, y);
                if (segmentStart || series.size() <= 60) Ellipse(hdc, x - S(2), y - S(2), x + S(3), y + S(3));
                previousTime = p.first;
            }
            if (series.size() == 1)
                LineTo(hdc, X(series[0].first) + 1, Y(series[0].second));
            SelectObject(hdc, oldDotBrush); DeleteObject(dots);
            SelectObject(hdc, oldPen); DeleteObject(dataPen);
        }

        // 标签按“优秀→良好→一般”纵向排列；颜色线段与图内分界线一一对应。
        // 不强行贴着实际 Y 坐标，避免 RSRP/RSRQ 相邻阈值只有十余像素时文字重叠。
        HGDIOBJ guideFont = SelectObject(hdc, App().hFontSmall);
        int labelIndex = 0;
        for (auto item = guides.end(); item != guides.begin(); ++labelIndex) {
            --item;
            const ChartGuide& guide = *item;
            const int labelY = pr.top + labelIndex * S(17);
            HPEN keyPen = CreatePen(PS_SOLID, std::max(2, S(2)), guide.color);
            HGDIOBJ previousPen = SelectObject(hdc, keyPen);
            MoveToEx(hdc, pr.right + S(6), labelY + S(6), nullptr);
            LineTo(hdc, pr.right + S(15), labelY + S(6));
            SelectObject(hdc, previousPen); DeleteObject(keyPen);
            RECT labelRect{pr.right + S(19), labelY, canvasRight, labelY + S(15)};
            SetTextColor(hdc, th::inkSec);
            if (labelRect.bottom <= pr.bottom + S(4)) DrawTextW(hdc, guide.label, -1, &labelRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(hdc, guideFont);
    };

        HGDIOBJ oldFont = SelectObject(hdc, App().hFontSmall);
    SetTextColor(hdc, th::inkSec);
    const std::wstring rangeText = U8ToW(fmtTime(t0, "FULL")) + L" → " + U8ToW(fmtTime(t1, "FULL")) +
        (g_chartHasInferredTime ? L" · 含推定时间" : L"");
    RECT rangeRect{left, S(3), canvasRight, S(24)};
    DrawTextW(hdc, rangeText.c_str(), -1, &rangeRect, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    auto title = [&](const RECT& plot, const std::wstring& text, size_t samples, bool detailPlot) {
        if (plot.right <= plot.left) return;
        SelectObject(hdc, App().hFontUI); SetTextColor(hdc, th::inkPri);
        RECT label{plot.left, plot.top - S(25), plot.right - (detailPlot ? S(140) : 0), plot.top - S(2)};
        DrawTextW(hdc, text.c_str(), -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, App().hFontSmall); SetTextColor(hdc, th::inkMuted);
        RECT count{plot.right + S(6), label.top, canvasRight, label.bottom};
        const std::wstring countText = FmtW(L"%d 点", static_cast<int>(samples));
        DrawTextW(hdc, countText.c_str(), -1, &count, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    title(top, L"CSQ · 0–31（99 未知）", g_csq.size(), false);
    title(middle, g_chartDetail == 1 ? L"RSRQ · -25～0 dB" : L"RSRP · -140～-40 dBm", detail.size(), true);
    title(bottom, L"SNR · -20～30 dB（模组上报值）", g_snr10.size(), false);
    if (!detail.empty()) {
        for (int mode = 0; mode < 2; ++mode) {
            RECT button{right - S(130) + mode * S(65), middle.top - S(25), right - S(70) + mode * S(65), middle.top - S(2)};
            g_chartModeRects[mode] = button;
            const bool selected = mode == g_chartDetail, enabled = !detailSeries(mode).empty();
            FillRound(hdc, button, S(9), selected ? th::accentSoft : th::surface, selected ? th::accent : th::border);
            SelectObject(hdc, App().hFontSmall); SetTextColor(hdc, enabled ? th::inkPri : th::inkMuted);
            DrawTextW(hdc, mode == 1 ? L"RSRQ" : L"RSRP", -1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(hdc, oldFont);
    drawPlot(top, g_chartCsqDraw, g_csqGaps, 0, 31, th::s1_blue,
             {{kCsqFair, L"≥10 一般", th::warning}, {kCsqGood, L"≥15 良好", th::accent},
              {kCsqExcellent, L"≥20 优秀", th::good}}, false);
    if (g_chartDetail == 0)
        drawPlot(middle, g_chartDetailDraw, g_rsrpGaps, detailLo, detailHi, th::s7_violet,
                 {{kRsrpFair, L"≥-100 一般", th::warning}, {kRsrpGood, L"≥-90 良好", th::accent},
                  {kRsrpExcellent, L"≥-80 优秀", th::good}}, false);
    else
        drawPlot(middle, g_chartDetailDraw, g_rsrqGaps, detailLo, detailHi, th::s7_violet,
                 {{kRsrqFair, L"≥-20 一般", th::warning}, {kRsrqGood, L"≥-15 良好", th::accent},
                  {kRsrqExcellent, L"≥-10 优秀", th::good}}, false);
    drawPlot(bottom, g_chartSnrDraw, g_snrGaps, -200, 300, th::s5_aqua,
             {{kSnrFair10, L">0 一般", th::warning}, {kSnrGood10, L"≥13 良好", th::accent},
              {kSnrExcellent10, L"≥20 优秀", th::good}}, true);

    // 时间轴随秒/分钟/跨日跨度自适应，始终标注首尾完整日期和秒。
    SelectObject(hdc, App().hFontSmall);
    SetTextColor(hdc, th::inkSec);
    const auto ticks = chartTimeTicks(t0, t1, plotWidth, S(132));
    HPEN timePen = CreatePen(PS_SOLID, 1, th::grid);
    HGDIOBJ oldPen = SelectObject(hdc, timePen);
    for (size_t index = 0; index < ticks.size(); ++index) {
        const long long time = ticks[index];
        const int x = X(time);
        for (const RECT& plot : {top, middle, bottom}) {
            if (plot.right <= plot.left) continue;
            MoveToEx(hdc, x, plot.top, nullptr); LineTo(hdc, x, plot.bottom);
        }
        std::wstring label = U8ToW(fmtTime(time, "FULL")); label[10] = L'\n';
        RECT labelRect{x - S(54), lastPlot.bottom + S(3), x + S(54), lastPlot.bottom + S(37)};
        UINT align = DT_CENTER;
        if (index == 0) { labelRect.left = left; labelRect.right = left + S(108); align = DT_LEFT; }
        else if (index + 1 == ticks.size()) { labelRect.left = right - S(108); labelRect.right = right; align = DT_RIGHT; }
        DrawTextW(hdc, label.c_str(), -1, &labelRect, align | DT_NOPREFIX);
    }
    SelectObject(hdc, oldPen); DeleteObject(timePen);
    RECT footer{left, rc.bottom - S(20), canvasRight, rc.bottom - S(2)};
    SetTextColor(hdc, th::inkMuted);
    const std::wstring footerText = (g_latestCellId.empty() ? L"小区 ID 未提供" :
        FmtW(L"最近小区 %s · %d 个", U8ToW(g_latestCellId).c_str(), static_cast<int>(g_visibleCellCount))) +
        std::wstring(L" · 浅红为断网 · >10 分钟采样空缺断线");
    DrawTextW(hdc, footerText.c_str(), -1, &footer,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    POINT hover{g_chartHoverX, g_chartHoverY};
    bool hoveringPlot = false;
    for (const RECT& plot : {top, middle, bottom}) if (PtInRect(&plot, hover)) hoveringPlot = true;
    auto drawCursorSegments = [&](int x, COLORREF color, int width) {
        HPEN cursorPen = CreatePen(PS_SOLID, width, color);
        HGDIOBJ previousPen = SelectObject(hdc, cursorPen);
        for (const RECT& plot : {top, middle, bottom}) {
            if (plot.right <= plot.left) continue;
            MoveToEx(hdc, x, plot.top, nullptr);
            LineTo(hdc, x, plot.bottom);
        }
        SelectObject(hdc, previousPen); DeleteObject(cursorPen);
    };
    if (!hoveringPlot && g_chartFocusTime != LLONG_MIN &&
        g_chartFocusTime >= t0 && g_chartFocusTime <= t1) {
        const int focusX = X(g_chartFocusTime);
        drawCursorSegments(focusX, th::accent, std::max(2, S(2)));
    }

    // 悬停时只保留一条活动准线，并分段绘制，避免穿过三张图之间的标题区域。
    if (hoveringPlot) {
        double frac = (double)(g_chartHoverX - left) / std::max(1, right - left);
        long long ht = t0 + (long long)(frac * (t1 - t0));
        long long cd = LLONG_MAX, dd = LLONG_MAX, sd = LLONG_MAX;
        const auto* cp = nearestChartPoint(g_csq, ht, &cd);
        const auto* dp = nearestChartPoint(detail, ht, &dd);
        const auto* sp = nearestChartPoint(g_snr10, ht, &sd);
        const ChartPoint* closest = cp;
        long long best = cd;
        if (dp && dd < best) { closest = dp; best = dd; }
        if (sp && sd < best) { closest = sp; }
        long long markT = closest ? closest->first : ht;
        int hx = X(markT);
        drawCursorSegments(hx, th::inkMuted, 1);
        std::wstring info = L"最近采样  " + U8ToW(fmtTime(markT, "FULL")) +
            (g_chartHasInferredTime ? L"（时间含推定，见表格 ~）" : L"");
        auto addValue = [&](const ChartSeries& series, const wchar_t* name, bool decimal, const wchar_t* unit) {
            long long distance = LLONG_MAX;
            const auto* point = nearestChartPoint(series, markT, &distance);
            if (!point) return;
            info += L"\n" + std::wstring(name) + L"  ";
            if (distance > 600) info += L"附近无采样";
            else info += (decimal ? FmtW(L"%.1f", point->second / 10.0) : FmtW(L"%d", point->second)) +
                L" " + unit + L"  ·  " + U8ToW(fmtTime(point->first, "FULL"));
        };
        addValue(g_csq, L"CSQ", false, L"");
        addValue(detail, detailName, false, g_chartDetail == 1 ? L"dB" : L"dBm");
        addValue(g_snr10, L"SNR", true, L"dB");
        const int width = std::min(S(410), static_cast<int>(rc.right) - S(16));
        RECT measure{0, 0, width - S(20), 10000};
        DrawTextW(hdc, info.c_str(), -1, &measure, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        const int height = measure.bottom + S(16);
        int bx = hx + S(12);
        if (bx + width > rc.right - S(8)) bx = hx - width - S(12);
        bx = std::clamp(bx, S(8), std::max(S(8), static_cast<int>(rc.right) - width - S(8)));
        const int by = std::clamp(g_chartHoverY + S(12), S(8), std::max(S(8), static_cast<int>(rc.bottom) - height - S(8)));
        RECT popup{bx, by, bx + width, by + height};
        FillRound(hdc, popup, S(8), th::surface, th::border);
        InflateRect(&popup, -S(10), -S(8)); SetTextColor(hdc, th::inkPri);
        DrawTextW(hdc, info.c_str(), -1, &popup, DT_WORDBREAK | DT_NOPREFIX);
    }

    if (GetFocus() == hwnd && !(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT focus = rc; InflateRect(&focus, -S(3), -S(3));
        HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), th::accent);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, focus.left, focus.top, focus.right, focus.bottom, S(5), S(5));
        SelectObject(hdc, oldBrush); SelectObject(hdc, oldPen); DeleteObject(pen);
    }

    BitBlt(hdcWin, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp); DeleteObject(bmp); DeleteDC(hdc);
    EndPaint(hwnd, &ps);
    return 0;
}

// ============================ 渲染 ============================

void RenderMetrics() {
    g_csq.clear();
    g_rsrp.clear();
    g_rsrq.clear();
    g_snr10.clear();
    g_latestCellId.clear();
    g_visibleCellCount = 0;
    g_chartHasInferredTime = false;
    std::set<std::string> visibleCells;
    const MetricRow* latestCell = nullptr;
    for (const MetricRow* metric : App().document.metricView) {
        const MetricRow& m = *metric;
        g_chartHasInferredTime = g_chartHasInferredTime || m.inferredTime;
        if (usesLteEngineeringReference(m.rat)) {
            if (m.csqVal >= 0 && m.csqVal <= 31) g_csq.push_back({ m.t, m.csqVal });
            if (m.rsrp < 0)    g_rsrp.push_back({ m.t, m.rsrp });
            if (m.rsrq < 0)    g_rsrq.push_back({ m.t, m.rsrq });
            if (m.snr10 != 100000) g_snr10.push_back({ m.t, m.snr10 });
        }
        if (!m.cellId.empty()) {
            visibleCells.insert(m.cellId.str());
            if (!latestCell || m.t > latestCell->t ||
                (m.t == latestCell->t && m.lineNo > latestCell->lineNo)) latestCell = &m;
        }
    }
    if (latestCell) g_latestCellId = latestCell->cellId.str();
    g_visibleCellCount = visibleCells.size();
    // 合并日志理论上已按时间定序；若单文件内部确有乱序,只排序图表副本，表格与
    // 结论仍保持原始证据顺序。排序一次后悬停即可稳定使用 O(log n) 二分查询。
    sortChartSeriesByTime(g_csq);
    sortChartSeriesByTime(g_rsrp);
    sortChartSeriesByTime(g_rsrq);
    sortChartSeriesByTime(g_snr10);
    g_csqGaps = chartSampleGaps(g_csq, 600);
    g_rsrpGaps = chartSampleGaps(g_rsrp, 600);
    g_rsrqGaps = chartSampleGaps(g_rsrq, 600);
    g_snrGaps = chartSampleGaps(g_snr10, 600);
    ResetChartSampleCache();
    ListView_SetItemCountEx(App().hMetric, (int)App().document.metricView.size(),
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(App().hMetric, nullptr, TRUE);
    const bool detailEmpty = g_chartDetail == 0 ? g_rsrp.empty() : g_rsrq.empty();
    if (detailEmpty) {
        if (!g_rsrp.empty()) g_chartDetail = 0;
        else if (!g_rsrq.empty()) g_chartDetail = 1;
    }
    InvalidateRect(App().hChart, nullptr, TRUE);
}


void ReleaseChartPageData() {
    releaseVector(g_csq);
    releaseVector(g_rsrp);
    releaseVector(g_rsrq);
    releaseVector(g_snr10);
    releaseVector(g_csqGaps); releaseVector(g_rsrpGaps); releaseVector(g_rsrqGaps); releaseVector(g_snrGaps);
    g_latestCellId.clear();
    g_visibleCellCount = 0;
    g_chartHasInferredTime = false;
    g_chartFocusTime = LLONG_MIN;

    ResetChartSampleCache();
    releaseVector(g_chartCsqDraw);
    releaseVector(g_chartDetailDraw);
    releaseVector(g_chartSnrDraw);

    for (RECT& rect : g_chartModeRects) rect = RECT{};
    for (RECT& rect : g_chartPlotRects) rect = RECT{};
    g_chartDetail = 0;
    g_chartHoverX = g_chartHoverY = -1;
}

void SetChartFocusTime(long long time) {
    g_chartFocusTime = time;
    if (App().hChart) InvalidateRect(App().hChart, nullptr, FALSE);
}

int PreferredChartHeight() {
    const bool haveDetail = !g_rsrp.empty() || !g_rsrq.empty();
    const int plots = (!g_csq.empty() ? 1 : 0) + (haveDetail ? 1 : 0) + (!g_snr10.empty() ? 1 : 0);
    return S(77 + std::max(1, plots) * 84);
}

} // namespace dl
