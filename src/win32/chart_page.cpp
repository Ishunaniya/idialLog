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
static std::string g_latestCellId;
static std::size_t g_visibleCellCount = 0;

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
        if (!changedMode && point.x >= g_chartPlotLeft && point.x <= g_chartPlotRight &&
            g_chartVisibleT1 >= g_chartVisibleT0) {
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

    bool haveAny = !g_csq.empty() || !detail.empty() || !g_snr10.empty();
    if (!haveAny || rc.right < S(260) || rc.bottom < S(260)) {
        const wchar_t* title = haveAny ? L"窗口空间不足" : L"暂无信号趋势";
        const wchar_t* detailText = haveAny ? L"放大窗口后即可恢复三图表视图。"
                                             : L"加载包含 LTE（或未标注 RAT）信号采样的日志后自动显示。";
        int cardW = std::min(S(460), std::max(S(260), static_cast<int>(rc.right) - S(64)));
        int cardH = S(112), x = (rc.right - cardW) / 2, y = (rc.bottom - cardH) / 2;
        RECT card{x, y, x + cardW, y + cardH}; FillRound(hdc, card, S(12), th::page, th::border);
        RECT icon{x + S(22), y + S(30), x + S(66), y + S(74)};
        FillRound(hdc, icon, S(22), th::accentSoft, th::accentSoft);
        HGDIOBJ oldFont = SelectObject(hdc, App().hFontSect); SetTextColor(hdc, th::accent);
        DrawTextW(hdc, L"⌁", -1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
    const int left = S(48), canvasRight = rc.right - S(12);
    const int guideWidth = S(92), right = canvasRight - guideWidth;
    g_chartVisibleT0 = t0; g_chartVisibleT1 = t1;
    g_chartPlotLeft = left; g_chartPlotRight = right;
    const int section = std::max(S(42), (static_cast<int>(rc.bottom) - S(123)) / 3);
    RECT top{ left, S(31), right, S(31) + section };
    RECT middle{ left, top.bottom + S(34), right, top.bottom + S(34) + section };
    RECT bottom{ left, middle.bottom + S(34), right, rc.bottom - S(24) };
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
            int xs = X(o.start), xe = X(e);
            if (xe < xs + 2) xe = xs + 2;
            if (xe < pr.left || xs > pr.right) continue;
            RECT rb{ std::max(xs, (int)pr.left), pr.top,
                     std::min(xe, (int)pr.right), pr.bottom };
            FillRect(hdc, &rb, band);
        }
        DeleteObject(band);
    };

    auto drawPlot = [&](const RECT& pr, const ChartSeries& series,
                        int lo, int hi, COLORREF color,
                        std::initializer_list<ChartGuide> guides, bool scaled10) {
        auto Y = [&](int v) {
            int c = std::max(lo, std::min(hi, v));
            return pr.bottom - (int)((double)(c - lo) / (hi - lo) * (pr.bottom - pr.top));
        };
        paintOutages(pr);
        HPEN gridPen = CreatePen(PS_SOLID, 1, th::grid);
        HGDIOBJ oldPen = SelectObject(hdc, gridPen);
        SetTextColor(hdc, th::inkMuted);
        for (int i = 0; i < 4; ++i) {
            int val = hi - (hi - lo) * i / 3;
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
            bool first = true;
            for (const auto& p : series) {
                int x = X(p.first), y = Y(p.second);
                if (first) { MoveToEx(hdc, x, y, nullptr); first = false; }
                else LineTo(hdc, x, y);
            }
            if (series.size() == 1)
                LineTo(hdc, X(series[0].first) + 1, Y(series[0].second));
            SelectObject(hdc, oldPen); DeleteObject(dataPen);
        }

        // 标签按“优秀→良好→一般”纵向排列；颜色线段与图内分界线一一对应。
        // 不强行贴着实际 Y 坐标，避免 RSRP/RSRQ 相邻阈值只有十余像素时文字重叠。
        HGDIOBJ guideFont = SelectObject(hdc, App().hFontSmall);
        int labelIndex = 0;
        for (auto item = guides.end(); item != guides.begin(); ++labelIndex) {
            --item;
            const ChartGuide& guide = *item;
            const int labelY = pr.top + S(4) + labelIndex * S(17);
            HPEN keyPen = CreatePen(PS_SOLID, std::max(2, S(2)), guide.color);
            HGDIOBJ previousPen = SelectObject(hdc, keyPen);
            MoveToEx(hdc, pr.right + S(6), labelY + S(6), nullptr);
            LineTo(hdc, pr.right + S(15), labelY + S(6));
            SelectObject(hdc, previousPen); DeleteObject(keyPen);
            RECT labelRect{pr.right + S(19), labelY, canvasRight, labelY + S(15)};
            SetTextColor(hdc, th::inkSec);
            DrawTextW(hdc, guide.label, -1, &labelRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(hdc, guideFont);
    };

    HGDIOBJ oldFont = SelectObject(hdc, App().hFontSect);
    SetTextColor(hdc, th::inkPri);
    const wchar_t* topTitle = L"CSQ 信号强度";
    TextOutW(hdc, top.left, S(4), topTitle, (int)wcslen(topTitle));
    SIZE topTitleSize{};
    GetTextExtentPoint32W(hdc, topTitle, (int)wcslen(topTitle), &topTitleSize);
    SelectObject(hdc, App().hFontSmall); SetTextColor(hdc, th::inkMuted);
    const std::wstring cellLabel = g_latestCellId.empty()
        ? L"小区 ID  日志未提供"
        : FmtW(L"最近小区 ID  %s  ·  %d 个", U8ToW(g_latestCellId).c_str(),
               static_cast<int>(g_visibleCellCount));
    SIZE cellLabelSize{};
    GetTextExtentPoint32W(hdc, cellLabel.c_str(), static_cast<int>(cellLabel.size()), &cellLabelSize);
    RECT cellChip{canvasRight - cellLabelSize.cx - S(20), S(3), canvasRight, S(28)};
    FillRound(hdc, cellChip, S(12), th::accentSoft, th::border);
    SetTextColor(hdc, g_latestCellId.empty() ? th::inkMuted : th::accent);
    DrawTextW(hdc, cellLabel.c_str(), -1, &cellChip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(hdc, th::inkMuted);
    const wchar_t* legend = L"LTE 工程参考 · 有效 0–31 · 99 未知";
    SIZE legendSize{};
    GetTextExtentPoint32W(hdc, legend, static_cast<int>(wcslen(legend)), &legendSize);
    const int legendX = top.left + topTitleSize.cx + S(12);
    if (legendX + legendSize.cx + S(12) < cellChip.left)
        TextOutW(hdc, legendX, S(8), legend, static_cast<int>(wcslen(legend)));
    SelectObject(hdc, App().hFontSect); SetTextColor(hdc, th::inkPri);
    const wchar_t* middleTitle = L"LTE 覆盖质量 · 工程参考";
    TextOutW(hdc, middle.left, top.bottom + S(8), middleTitle,
             static_cast<int>(wcslen(middleTitle)));
    const wchar_t* snrTitle = L"SNR 信噪比";
    TextOutW(hdc, bottom.left, middle.bottom + S(8), snrTitle, (int)wcslen(snrTitle));
    SIZE snrTitleSize{};
    GetTextExtentPoint32W(hdc, snrTitle, (int)wcslen(snrTitle), &snrTitleSize);
    SelectObject(hdc, App().hFontSmall); SetTextColor(hdc, th::inkMuted);
    const wchar_t* snrLegend = L"模组值 dB · LTE 工程参考";
    TextOutW(hdc, bottom.left + snrTitleSize.cx + S(12), middle.bottom + S(12),
             snrLegend, (int)wcslen(snrLegend));

    const wchar_t* modeNames[] = {L"RSRP", L"RSRQ"};
    int modeRight = canvasRight;
    for (int mode = 1; mode >= 0; --mode) {
        int width = S(58);
        g_chartModeRects[mode] = RECT{modeRight - width, top.bottom + S(4), modeRight, top.bottom + S(30)};
        const bool selected = mode == g_chartDetail;
        const bool enabled = !detailSeries(mode).empty();
        FillRound(hdc, g_chartModeRects[mode], S(13), selected ? th::accentSoft : th::surface,
                  selected ? th::accent : th::border);
        SelectObject(hdc, App().hFontSmall);
        SetTextColor(hdc, enabled ? (selected ? th::accent : th::inkSec) : th::inkMuted);
        DrawTextW(hdc, modeNames[mode], -1, &g_chartModeRects[mode],
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        modeRight -= width + S(7);
    }
    SelectObject(hdc, oldFont);

    drawPlot(top, g_chartCsqDraw, 0, 31, th::s1_blue,
             {{kCsqFair, L"≥10 一般", th::warning}, {kCsqGood, L"≥15 良好", th::accent},
              {kCsqExcellent, L"≥20 优秀", th::good}}, false);
    if (g_chartDetail == 0)
        drawPlot(middle, g_chartDetailDraw, detailLo, detailHi, th::s7_violet,
                 {{kRsrpFair, L"≥-100 一般", th::warning}, {kRsrpGood, L"≥-90 良好", th::accent},
                  {kRsrpExcellent, L"≥-80 优秀", th::good}}, false);
    else
        drawPlot(middle, g_chartDetailDraw, detailLo, detailHi, th::s7_violet,
                 {{kRsrqFair, L"≥-20 一般", th::warning}, {kRsrqGood, L"≥-15 良好", th::accent},
                  {kRsrqExcellent, L"≥-10 优秀", th::good}}, false);
    drawPlot(bottom, g_chartSnrDraw, -200, 300, th::s5_aqua,
             {{kSnrFair10, L">0 一般", th::warning}, {kSnrGood10, L"≥13 良好", th::accent},
              {kSnrExcellent10, L"≥20 优秀", th::good}}, true);

    // 共享时间轴：只在下图标注，竖线同时贯穿两张图，便于对齐而不引入第二量纲。
    SetTextColor(hdc, th::inkMuted);
    long long spanSec = t1 - t0;
    int stepH = 1;
    const int cand[] = { 1, 2, 3, 6, 12, 24 };
    for (int v : cand) { stepH = v; if (spanSec / (v * 3600LL) <= 10) break; }
    long long stepSec = stepH * 3600LL;
    long long firstT = ((t0 + stepSec - 1) / stepSec) * stepSec;
    HPEN timePen = CreatePen(PS_SOLID, 1, th::grid);
    HGDIOBJ oldPen = SelectObject(hdc, timePen);
    for (long long t = firstT; t <= t1; t += stepSec) {
        int x = X(t);
        MoveToEx(hdc, x, top.top, nullptr); LineTo(hdc, x, top.bottom);
        MoveToEx(hdc, x, middle.top, nullptr); LineTo(hdc, x, middle.bottom);
        MoveToEx(hdc, x, bottom.top, nullptr); LineTo(hdc, x, bottom.bottom);
        std::wstring lb = U8ToW(fmtTime(t, "HM"));
        SIZE sz{}; GetTextExtentPoint32W(hdc, lb.c_str(), (int)lb.size(), &sz);
        TextOutW(hdc, x - sz.cx / 2, bottom.bottom + S(3), lb.c_str(), (int)lb.size());
    }
    SelectObject(hdc, oldPen); DeleteObject(timePen);

    const bool hoveringPlot = g_chartHoverX >= left && g_chartHoverX <= right;
    auto drawCursorSegments = [&](int x, COLORREF color, int width) {
        HPEN cursorPen = CreatePen(PS_SOLID, width, color);
        HGDIOBJ previousPen = SelectObject(hdc, cursorPen);
        for (const RECT& plot : {top, middle, bottom}) {
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
        long long markT = cp ? cp->first : (dp ? dp->first : (sp ? sp->first : ht));
        int hx = X(markT);
        drawCursorSegments(hx, th::inkMuted, 1);

        std::wstring info = U8ToW(fmtTime(markT, "HM"));
        if (cp && cd <= 600) info += FmtW(L"   CSQ %d(%s)", cp->second,
            U8ToW(signalQualityName(csqQuality(cp->second))).c_str());
        if (dp && dd <= 600) info += FmtW(L"   %s %d %s(%s)", detailName, dp->second,
            g_chartDetail == 0 ? L"dBm" : L"dB",
            U8ToW(signalQualityName(g_chartDetail == 0 ? rsrpQuality(dp->second)
                                                        : rsrqQuality(dp->second))).c_str());
        if (sp && sd <= 600) info += FmtW(L"   SNR %.1f dB(%s)", sp->second / 10.0,
            U8ToW(signalQualityName(snrQuality10(sp->second))).c_str());
        SIZE sz{}; GetTextExtentPoint32W(hdc, info.c_str(), (int)info.size(), &sz);
        int bx = hx + S(8);
        if (bx + sz.cx + S(10) > right) bx = hx - sz.cx - S(14);
        RECT ib{ bx - S(4), top.top + S(3), bx + sz.cx + S(6), top.top + sz.cy + S(8) };
        FillRound(hdc, ib, S(6), th::surface, th::border);
        SetTextColor(hdc, th::inkPri);
        TextOutW(hdc, bx, top.top + S(5), info.c_str(), (int)info.size());
    }

    if (GetFocus() == hwnd) {
        RECT focus = rc; InflateRect(&focus, -S(3), -S(3)); DrawFocusRect(hdc, &focus);
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
    std::set<std::string> visibleCells;
    const MetricRow* latestCell = nullptr;
    for (const MetricRow* metric : App().document.metricView) {
        const MetricRow& m = *metric;
        if (usesLteEngineeringReference(m.rat)) {
            if (m.csqVal >= 0) g_csq.push_back({ m.t, m.csqVal });
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
    g_latestCellId.clear();
    g_visibleCellCount = 0;
    g_chartFocusTime = LLONG_MIN;

    ResetChartSampleCache();
    releaseVector(g_chartCsqDraw);
    releaseVector(g_chartDetailDraw);
    releaseVector(g_chartSnrDraw);

    g_chartDetail = 0;
    g_chartHoverX = g_chartHoverY = -1;
}

void SetChartFocusTime(long long time) {
    g_chartFocusTime = time;
    if (App().hChart) InvalidateRect(App().hChart, nullptr, FALSE);
}

} // namespace dl
