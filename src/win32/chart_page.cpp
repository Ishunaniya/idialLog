// chart_page.cpp — 信号图绘制、降采样缓存与指标页模型
#include "chart_page.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>

#include "app_context.h"
#include "chartmodel.h"
#include "log_time.h"
#include "memoryutil.h"
#include "theme.h"
#include "win_text.h"

namespace dl {

static ChartSeries g_csq;   // 供图表
static ChartSeries g_rsrp;  // LTE 详情图:RSRP dBm
static ChartSeries g_rsrq;  // LTE 详情图:RSRQ dB
static ChartSeries g_snr10; // LTE 详情图:SNR SDK原值(0.1dB)
// 图表绘制缓存:按当前窗口像素宽度对完整序列做峰谷降采样。鼠标每移动 1px 都会
// 触发 WM_PAINT,缓存让这些重绘只消费数千点,不再反复扫描/绘制近十万点。
static ChartSeries g_chartCsqDraw, g_chartDetailDraw;
static unsigned long long g_chartDataRevision = 1, g_chartCacheRevision = 0;
static int g_chartCacheWidth = -1, g_chartCacheDetail = -1;
static long long g_chartCacheT0 = 0, g_chartCacheT1 = 0;
static int  g_chartDetail    = 0;                     // 0=RSRP 1=RSRQ 2=SNR,点击循环
static int  g_chartHoverX   = -1;                     // 悬停 X(客户区),-1=未悬停

static void ResetChartSampleCache() {
    if (++g_chartDataRevision == 0) g_chartDataRevision = 1; // 无符号回绕防御
    g_chartCacheRevision = 0;
    g_chartCacheWidth = g_chartCacheDetail = -1;
    g_chartCsqDraw.clear();
    g_chartDetailDraw.clear();
}


// ============================ ListView 工具 ============================

LRESULT CALLBACK ChartProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto detailSeries = [](int mode) -> const ChartSeries& {
        return mode == 1 ? g_rsrq : (mode == 2 ? g_snr10 : g_rsrp);
    };
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_LBUTTONDOWN) {
        for (int step = 1; step <= 3; ++step) {
            int next = (g_chartDetail + step) % 3;
            if (!detailSeries(next).empty()) { g_chartDetail = next; break; }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr,
            (!g_rsrp.empty() || !g_rsrq.empty() || !g_snr10.empty()) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    if (msg == WM_MOUSEMOVE) {
        int mx = (int)(short)LOWORD(lp);
        if (mx != g_chartHoverX) {
            g_chartHoverX = mx;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE; tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        g_chartHoverX = -1;
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
    const wchar_t* detailName = g_chartDetail == 1 ? L"RSRQ" : (g_chartDetail == 2 ? L"SNR" : L"RSRP");
    const int detailLo = g_chartDetail == 1 ? -25 : (g_chartDetail == 2 ? -200 : -120);
    const int detailHi = g_chartDetail == 1 ?   0 : (g_chartDetail == 2 ?  300 :  -60);

    bool haveAny = !g_csq.empty() || !detail.empty();
    if (!haveAny || rc.right < S(140) || rc.bottom < S(140)) {
        SetTextColor(hdc, th::inkMuted);
        const wchar_t* t = haveAny ? L"窗口过小，无法显示信号图" : L"加载含心跳的日志后显示信号趋势";
        TextOutW(hdc, S(42), std::max(S(8), (int)(rc.bottom / 2)), t, (int)wcslen(t));
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
    const double total = std::max<double>(1.0, (double)(t1 - t0));
    const int left = S(48), right = rc.right - S(12);
    const int mid = rc.bottom / 2;
    RECT top{ left, S(24), right, mid - S(13) };
    RECT bot{ left, mid + S(22), right, rc.bottom - S(24) };
    auto X = [&](long long t) {
        return left + (int)((double)(t - t0) / total * (right - left));
    };

    const int plotWidth = std::max(1, right - left + 1);
    if (g_chartCacheRevision != g_chartDataRevision ||
        g_chartCacheWidth != plotWidth || g_chartCacheDetail != g_chartDetail ||
        g_chartCacheT0 != t0 || g_chartCacheT1 != t1) {
        downsampleChartSeries(g_csq, t0, t1, (size_t)plotWidth, g_chartCsqDraw);
        downsampleChartSeries(detail, t0, t1, (size_t)plotWidth, g_chartDetailDraw);
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
                        int lo, int hi, COLORREF color, int threshold, bool showThreshold,
                        bool scaled10) {
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

        if (showThreshold) {
            HPEN thresholdPen = CreatePen(PS_SOLID, 1, th::warning);
            oldPen = SelectObject(hdc, thresholdPen);
            MoveToEx(hdc, pr.left, Y(threshold), nullptr);
            LineTo(hdc, pr.right, Y(threshold));
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
    };

    SetTextColor(hdc, th::inkMuted);
    const wchar_t* topTitle = L"CSQ (0–31)   黄线=弱信号提示阈值10   红带=断网";
    TextOutW(hdc, top.left, S(4), topTitle, (int)wcslen(topTitle));
    std::wstring bottomTitle = g_chartDetail == 2
        ? L"LTE SNR (dB，SDK原值×0.1)   黄线=0dB推断提示线   [点击切换 RSRP/RSRQ/SNR]"
        : FmtW(L"LTE %s (%s)   [点击切换 RSRP/RSRQ/SNR]",
               detailName, g_chartDetail == 0 ? L"dBm" : L"dB");
    TextOutW(hdc, bot.left, mid + S(4), bottomTitle.c_str(), (int)bottomTitle.size());

    drawPlot(top, g_chartCsqDraw, 0, 31, th::s1_blue, 10, true, false);
    drawPlot(bot, g_chartDetailDraw, detailLo, detailHi, th::s7_violet, 0,
             g_chartDetail == 2, g_chartDetail == 2);

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
        MoveToEx(hdc, x, bot.top, nullptr); LineTo(hdc, x, bot.bottom);
        std::wstring lb = U8ToW(fmtTime(t, "HM"));
        SIZE sz{}; GetTextExtentPoint32W(hdc, lb.c_str(), (int)lb.size(), &sz);
        TextOutW(hdc, x - sz.cx / 2, bot.bottom + S(3), lb.c_str(), (int)lb.size());
    }
    SelectObject(hdc, oldPen); DeleteObject(timePen);

    // 悬停：共享一条时间准线，同时报告上图 CSQ 与当前 LTE 详情值。
    if (g_chartHoverX >= left && g_chartHoverX <= right) {
        double frac = (double)(g_chartHoverX - left) / std::max(1, right - left);
        long long ht = t0 + (long long)(frac * (t1 - t0));
        long long cd = LLONG_MAX, dd = LLONG_MAX;
        const auto* cp = nearestChartPoint(g_csq, ht, &cd);
        const auto* dp = nearestChartPoint(detail, ht, &dd);
        long long markT = cp ? cp->first : (dp ? dp->first : ht);
        int hx = X(markT);
        HPEN crossPen = CreatePen(PS_SOLID, 1, th::inkMuted);
        oldPen = SelectObject(hdc, crossPen);
        MoveToEx(hdc, hx, top.top, nullptr); LineTo(hdc, hx, bot.bottom);
        SelectObject(hdc, oldPen); DeleteObject(crossPen);

        std::wstring info = U8ToW(fmtTime(markT, "HM"));
        if (cp && cd <= 600) info += FmtW(L"   CSQ %d", cp->second);
        if (dp && dd <= 600) {
            if (g_chartDetail == 2) info += FmtW(L"   SNR %.1f dB", dp->second / 10.0);
            else info += FmtW(L"   %s %d %s", detailName, dp->second,
                              g_chartDetail == 0 ? L"dBm" : L"dB");
        }
        SIZE sz{}; GetTextExtentPoint32W(hdc, info.c_str(), (int)info.size(), &sz);
        int bx = hx + S(8);
        if (bx + sz.cx + S(10) > right) bx = hx - sz.cx - S(14);
        RECT ib{ bx - S(4), top.top + S(3), bx + sz.cx + S(6), top.top + sz.cy + S(8) };
        HBRUSH ibg = CreateSolidBrush(th::surface); FillRect(hdc, &ib, ibg); DeleteObject(ibg);
        HPEN ibd = CreatePen(PS_SOLID, 1, th::border);
        HGDIOBJ op = SelectObject(hdc, ibd), ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, ib.left, ib.top, ib.right, ib.bottom);
        SelectObject(hdc, ob); SelectObject(hdc, op); DeleteObject(ibd);
        SetTextColor(hdc, th::inkPri);
        TextOutW(hdc, bx, top.top + S(5), info.c_str(), (int)info.size());
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
    for (const auto& m : App().document.metrics) {
        if (m.csqVal >= 0) g_csq.push_back({ m.t, m.csqVal });
        if (m.rsrp < 0)    g_rsrp.push_back({ m.t, m.rsrp });
        if (m.rsrq < 0)    g_rsrq.push_back({ m.t, m.rsrq });
        if (m.snr10 != 100000) g_snr10.push_back({ m.t, m.snr10 });
    }
    // 合并日志理论上已按时间定序；若单文件内部确有乱序,只排序图表副本，表格与
    // 结论仍保持原始证据顺序。排序一次后悬停即可稳定使用 O(log n) 二分查询。
    sortChartSeriesByTime(g_csq);
    sortChartSeriesByTime(g_rsrp);
    sortChartSeriesByTime(g_rsrq);
    sortChartSeriesByTime(g_snr10);
    ResetChartSampleCache();
    ListView_SetItemCountEx(App().hMetric, (int)App().document.metrics.size(),
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(App().hMetric, nullptr, TRUE);
    const bool detailEmpty = g_chartDetail == 0 ? g_rsrp.empty() :
                             (g_chartDetail == 1 ? g_rsrq.empty() : g_snr10.empty());
    if (detailEmpty) {
        if (!g_rsrp.empty()) g_chartDetail = 0;
        else if (!g_rsrq.empty()) g_chartDetail = 1;
        else if (!g_snr10.empty()) g_chartDetail = 2;
    }
    InvalidateRect(App().hChart, nullptr, TRUE);
}


void ReleaseChartPageData() {
    releaseVector(g_csq);
    releaseVector(g_rsrp);
    releaseVector(g_rsrq);
    releaseVector(g_snr10);

    ResetChartSampleCache();
    releaseVector(g_chartCsqDraw);
    releaseVector(g_chartDetailDraw);

    g_chartDetail = 0;
    g_chartHoverX = -1;
}

} // namespace dl
