// chart_page.cpp — 信号图绘制、降采样缓存与指标页模型
#include "chart_page.h"

#include <commctrl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <windowsx.h>
#include <cmath>
#include "log_filter.h"
#include <initializer_list>
#include <limits>
#include <set>
#include <string>

#include "app_context.h"
#include "chartmodel.h"
#include "log_time.h"
#include "load_controller.h"
#include "memoryutil.h"
#include "modern_shell.h"
#include "signal_quality.h"
#include "signal_chart.h"
#include "theme.h"
#include "win_text.h"
#include "text_catalog.h"

namespace dl {

static ChartSeries g_csq;   // 供图表
static ChartSeries g_rsrp;  // LTE 详情图:RSRP dBm
static ChartSeries g_rsrq;  // LTE 详情图:RSRQ dB
static ChartSeries g_rssi;  // Actual reported RSSI; never derived from CSQ.
static std::pair<int,int> g_rssiBounds{-120,-20};
static ChartSeries g_snr10; // 独立 SNR 图:SDK 原值(0.1dB)
static std::vector<ChartGap> g_csqGaps, g_rsrpGaps, g_rsrqGaps, g_rssiGaps, g_snrGaps;
// 图表绘制缓存:按当前窗口像素宽度对完整序列做峰谷降采样。鼠标每移动 1px 都会
// 触发 WM_PAINT,缓存让这些重绘只消费数千点,不再反复扫描/绘制近十万点。
static ChartSeries g_chartCsqDraw, g_chartDetailDraw, g_chartSnrDraw;
static unsigned long long g_chartDataRevision = 1, g_chartCacheRevision = 0;
static int g_chartCacheWidth = -1, g_chartCacheDetail = -1;
static long long g_chartCacheT0 = 0, g_chartCacheT1 = 0;
static int  g_chartDetail    = 0;                     // 0=RSRP 1=RSRQ 2=RSSI
static int  g_chartHoverX   = -1;                     // 悬停 X(客户区),-1=未悬停
static int  g_chartHoverY   = -1;
static RECT g_chartModeRects[3]{};
static long long g_chartFocusTime = LLONG_MIN;
static long long g_chartVisibleT0 = 0, g_chartVisibleT1 = 0;
static int g_chartPlotLeft = 0, g_chartPlotRight = 0;
static RECT g_chartPlotRects[3]{};
static std::string g_latestCellId;
static std::size_t g_visibleCellCount = 0;
static bool g_chartHasInferredTime = false;
static bool g_selectingTime = false;
static int g_selectionStartX = 0, g_selectionEndX = 0;
static long long g_selectionT0 = 0, g_selectionT1 = 0;

struct CurveRasterKey {
    unsigned long long revision = 0;
    int width = 0, height = 0, dpi = 0, detail = 0, lo = 0, hi = 0;
    long long start = 0, end = 0;
    std::array<COLORREF,8> palette{};
    bool operator==(const CurveRasterKey& other) const {
        return revision==other.revision && width==other.width && height==other.height &&
            dpi==other.dpi && detail==other.detail && lo==other.lo && hi==other.hi &&
            start==other.start && end==other.end && palette==other.palette;
    }
};
struct CurveRaster {
    HBITMAP bitmap = nullptr;
    CurveRasterKey key{};
    unsigned long long used = 0;
    CurveRaster() = default;
    CurveRaster(const CurveRaster&) = delete;
    CurveRaster& operator=(const CurveRaster&) = delete;
    void clear() { if(bitmap) DeleteObject(bitmap); bitmap=nullptr; used=0; }
    ~CurveRaster() { clear(); }
};
// Two geometries per plot retain chart/split views. Only the plot rectangle is
// cached; labels, focus, time selection and hover remain live.
static std::array<std::array<CurveRaster,2>,3> g_curveRasters;
static unsigned long long g_curveRasterUse = 0;

static void ClearCurveRasters() {
    for(auto& plot : g_curveRasters) for(auto& raster : plot) raster.clear();
    g_curveRasterUse=0;
}

struct ChartGuide {
    int value;
    const wchar_t* label;
    COLORREF color;
};

struct ChartDrawingRuntime {
    ULONG_PTR token = 0;
    ChartDrawingRuntime() { Gdiplus::GdiplusStartupInput input; if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok) token=0; }
    ~ChartDrawingRuntime() { if(token) Gdiplus::GdiplusShutdown(token); }
};

static void UpdateChartAccessibleLabel() {
    const wchar_t* name=g_chartDetail==2?L"RSSI":g_chartDetail==1?L"RSRQ":L"RSRP";
    const auto count=g_chartDetail==2?g_rssi.size():g_chartDetail==1?g_rsrq.size():g_rsrp.size();
    if(App().hChart) SetWindowTextW(App().hChart,FmtW(UiText(TextId::ui_0547),name,static_cast<int>(count)).append(L"\r\n").append(UiText(TextId::chart_navigation)).c_str());
}

static void ResetChartSampleCache() {
    ClearCurveRasters();
    if (++g_chartDataRevision == 0) g_chartDataRevision = 1; // 无符号回绕防御
    g_chartCacheRevision = 0;
    g_chartCacheWidth = g_chartCacheDetail = -1;
    g_chartCsqDraw.clear();
    g_chartDetailDraw.clear();
    g_chartSnrDraw.clear();
}


// ============================ ListView 工具 ============================

static bool g_panning=false;
static int g_panStart=0;
static ChartTimeWindow g_panWindow;
static void NavigateRange(ChartTimeWindow current,long long anchor,double zoom,double pan) {
    auto& doc=App().document;
    auto rows=applyFilterView(doc.lines,WToU8(GetText(App().hTagBox)),WToU8(GetText(App().hGrepBox)),WToU8(GetText(App().hSinceBox)),WToU8(GetText(App().hUntilBox)));
    doc.restrictToSelection(rows);
    bool have=false;ChartTimeWindow bounds;
    const bool wall=current.start>=946598400LL;
    for(const auto* row:rows){if((row->t>=946598400LL)!=wall)continue;if(!have){bounds={row->t,row->t};have=true;}else{bounds.start=std::min(bounds.start,row->t);bounds.end=std::max(bounds.end,row->t);}}
    if(!have||bounds.start==bounds.end)return;
    auto next=navigateChartWindow(current,bounds,anchor,zoom,pan);
    if(next.start!=current.start||next.end!=current.end)SelectAnalysisTimeRange(next.start,next.end);
}

LRESULT CALLBACK ChartProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto detailSeries = [](int mode) -> const ChartSeries& {
        return mode == 2 ? g_rssi : mode == 1 ? g_rsrq : g_rsrp;
    };
    if(msg==WM_MOUSEWHEEL){POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&p);bool over=false;for(const auto& rect:g_chartPlotRects)if(PtInRect(&rect,p))over=true;if(!over)return DefWindowProcW(hwnd,msg,wp,lp);
        const auto anchor=chartTimeAtPixel(g_chartVisibleT0,g_chartVisibleT1,g_chartPlotLeft,g_chartPlotRight,p.x);const double steps=GET_WHEEL_DELTA_WPARAM(wp)/120.0;
        const bool shift=(GET_KEYSTATE_WPARAM(wp)&MK_SHIFT)!=0;NavigateRange({g_chartVisibleT0,g_chartVisibleT1},anchor,shift?1:std::pow(0.8,steps),shift?-steps*0.15:0);return 0;}
    if(msg==WM_MBUTTONDOWN){POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};for(const auto& rect:g_chartPlotRects)if(PtInRect(&rect,p)){CancelChartSelection();g_panning=true;g_panStart=p.x;g_panWindow={g_chartVisibleT0,g_chartVisibleT1};SetCapture(hwnd);SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return 0;}}
    if(msg==WM_MBUTTONUP&&g_panning){g_panning=false;ReleaseCapture();const double delta=double(g_panStart-GET_X_LPARAM(lp))/std::max(1,g_chartPlotRight-g_chartPlotLeft);NavigateRange(g_panWindow,g_panWindow.start,1,delta);return 0;}
    if(msg==WM_CAPTURECHANGED||msg==WM_CANCELMODE)g_panning=false;
    if(msg==WM_KEYDOWN&&wp==VK_ESCAPE&&g_panning){g_panning=false;ReleaseCapture();return 0;}
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_SIZE) {
        if(g_panning){g_panning=false;if(GetCapture()==hwnd)ReleaseCapture();}
        CancelChartSelection();
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
        for (int mode = 0; mode < 3; ++mode) {
            if (PtInRect(&g_chartModeRects[mode], point) && !detailSeries(mode).empty()) {
                g_chartDetail = mode; changedMode = true; UpdateChartAccessibleLabel(); break;
            }
        }
        bool overPlot = false;
        for (const RECT& rect : g_chartPlotRects) if (PtInRect(&rect, point)) overPlot = true;
        if (!changedMode && overPlot && g_chartVisibleT1 >= g_chartVisibleT0) {
            g_selectionStartX = g_selectionEndX = point.x;
            g_selectionT0 = g_chartVisibleT0; g_selectionT1 = g_chartVisibleT1;
            g_selectingTime = true;
            SetCapture(hwnd);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_LBUTTONUP && g_selectingTime) {
        const int endX = std::clamp(static_cast<int>(static_cast<short>(LOWORD(lp))), g_chartPlotLeft, g_chartPlotRight);
        const bool range = std::abs(endX - g_selectionStartX) >= S(6) && g_selectionT1 > g_selectionT0;
        const long long start = chartTimeAtPixel(g_selectionT0, g_selectionT1, g_chartPlotLeft, g_chartPlotRight, g_selectionStartX);
        const long long end = chartTimeAtPixel(g_selectionT0, g_selectionT1, g_chartPlotLeft, g_chartPlotRight, endX);
        CancelChartSelection();
        if (range && start != end) {
            SelectAnalysisTimeRange(start, end);
            UpdateWindow(hwnd);
        } else {
            const long long target = end;
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
    if (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE) {
        CancelChartSelection(); return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && CancelChartSelection()) return 0;
    if (msg == WM_SETCURSOR) {
        POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd, &point);
        bool overMode = false;
        for (int mode = 0; mode < 3; ++mode)
            if (!detailSeries(mode).empty() && PtInRect(&g_chartModeRects[mode], point)) overMode = true;
        bool overPlot = false;
        for (const RECT& rect : g_chartPlotRects) if (PtInRect(&rect, point)) overPlot = true;
        SetCursor(LoadCursorW(nullptr, overMode ? IDC_HAND : overPlot ? IDC_CROSS : IDC_ARROW));
        return TRUE;
    }
    if (msg == WM_MOUSEMOVE) {
        if(g_panning){SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return 0;}
        int mx = (int)(short)LOWORD(lp);
        int my = (int)(short)HIWORD(lp);
        if (g_selectingTime) {
            g_selectionEndX = std::clamp(mx, g_chartPlotLeft, g_chartPlotRight);
            g_chartHoverX = g_chartHoverY = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
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
    const int detailLo = g_chartDetail == 2 ? g_rssiBounds.first : g_chartDetail == 1 ? -25 : -140;
    const int detailHi = g_chartDetail == 2 ? g_rssiBounds.second : g_chartDetail == 1 ?   0 :  -40;

    for (RECT& rect : g_chartModeRects) rect = RECT{};
    for (RECT& rect : g_chartPlotRects) rect = RECT{};
    const int plotCount = (!g_csq.empty() ? 1 : 0) + (!detail.empty() ? 1 : 0) + (!g_snr10.empty() ? 1 : 0);
    const bool haveAny = plotCount > 0;
    const int minimumHeight = S(std::max(160, 77 + plotCount * 70));
    if (!haveAny || rc.right < S(260) || rc.bottom < minimumHeight) {
        const wchar_t* title = haveAny ? UiText(TextId::ui_0333) : UiText(TextId::ui_0334);
        const wchar_t* detailText = haveAny ? UiText(TextId::ui_0335)
                                             : UiText(TextId::ui_0336);
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
    if (App().document.timeRange.active) {
        t0 = App().document.timeRange.start; t1 = App().document.timeRange.end;
    }
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
        return RECT{left, y + S(22), right, y + section - S(5)};
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
            return pr.bottom - (int)(((double)c - lo) / ((double)hi - lo) * (pr.bottom - pr.top));
        };
        if (g_selectingTime && std::abs(g_selectionEndX - g_selectionStartX) >= S(6)) {
            RECT selection{std::min(g_selectionStartX, g_selectionEndX), pr.top,
                           std::max(g_selectionStartX, g_selectionEndX), pr.bottom};
            HBRUSH brush = CreateSolidBrush(th::accentSoft);
            FillRect(hdc, &selection, brush); DeleteObject(brush);
        }
        paintOutages(pr);
        HPEN gridPen = CreatePen(PS_SOLID, 1, th::grid);
        HGDIOBJ oldPen = SelectObject(hdc, gridPen);
        SetTextColor(hdc, th::inkMuted);
        const int intervals = pr.bottom - pr.top < S(100) ? 2 : 4;
        for (int i = 0; i <= intervals; ++i) {
            int val = static_cast<int>(hi - (static_cast<long long>(hi) - lo) * i / intervals);
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

        const CurveRasterKey rasterKey{g_chartDataRevision,static_cast<int>(pr.right-pr.left+1),
            static_cast<int>(pr.bottom-pr.top+1),App().dpi,g_chartDetail,lo,hi,t0,t1,
            {th::surface,th::outageBand,th::grid,th::axis,th::warning,th::accent,th::good,color}};
        auto& rasters=g_curveRasters[&series==&g_chartCsqDraw?0:&series==&g_chartSnrDraw?2:1];
        CurveRaster* cached=nullptr;
        if(!g_selectingTime) for(auto& raster : rasters)
            if(raster.bitmap && raster.key==rasterKey) {cached=&raster;break;}
        bool reused=false;
        if(cached) {
            HDC source=CreateCompatibleDC(hdc);
            if(source) {
                HGDIOBJ previous=SelectObject(source,cached->bitmap);
                reused=BitBlt(hdc,pr.left,pr.top,rasterKey.width,rasterKey.height,source,0,0,SRCCOPY)!=FALSE;
                SelectObject(source,previous);DeleteDC(source);
                cached->used=++g_curveRasterUse;
            }
        }
        static ChartDrawingRuntime drawing;
        if (!reused && !series.empty() && drawing.token) {
            Gdiplus::Graphics graphics(hdc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            graphics.SetClip(Gdiplus::Rect(pr.left,pr.top,pr.right-pr.left+1,pr.bottom-pr.top+1));
            const Gdiplus::Color ink(255,GetRValue(color),GetGValue(color),GetBValue(color));
            Gdiplus::Pen pen(ink,std::max(1.5f,App().dpi/96.0f*1.5f));
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            Gdiplus::SolidBrush dots(ink);
            std::vector<Gdiplus::PointF> segment;
            auto flush = [&] {
                if(segment.size()>1) graphics.DrawLines(&pen,segment.data(),static_cast<INT>(segment.size()));
                segment.clear();
            };
            long long previousTime=0;
            for(const auto& point : series) {
                const auto gap=std::lower_bound(gaps.begin(),gaps.end(),previousTime,
                    [](const ChartGap& item,long long time) {return item.first<time;});
                const bool start=segment.empty() || (gap!=gaps.end() && gap->first<point.first);
                if(start) flush();
                const float x=static_cast<float>(left+(static_cast<double>(point.first-t0)/total)*(right-left));
                const float y=static_cast<float>(pr.bottom-((static_cast<double>(std::clamp(point.second,lo,hi))-lo)/
                    (static_cast<double>(hi)-lo))*(pr.bottom-pr.top));
                segment.emplace_back(x,y);
                if(start || series.size()<=60) {
                    // A sole sample often lies on the time-axis border; make
                    // its real position visible without shifting its time.
                    const float radius=std::max(1.5f,App().dpi/96.0f*(series.size()==1?4.0f:1.8f));
                    graphics.FillEllipse(&dots,x-radius,y-radius,2*radius,2*radius);
                }
                previousTime=point.first;
            }
            flush();
        } else if (!reused && !series.empty()) {
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

        if(!reused && !g_selectingTime) {
            HDC target=CreateCompatibleDC(hdc);
            HBITMAP image=target?CreateCompatibleBitmap(hdc,rasterKey.width,rasterKey.height):nullptr;
            if(image) {
                HGDIOBJ previous=SelectObject(target,image);
                const bool saved=BitBlt(target,0,0,rasterKey.width,rasterKey.height,hdc,pr.left,pr.top,SRCCOPY)!=FALSE;
                SelectObject(target,previous);
                if(saved) {
                    CurveRaster* slot=&rasters[0];
                    if(slot->bitmap && (!rasters[1].bitmap || rasters[1].used<slot->used)) slot=&rasters[1];
                    slot->clear();slot->bitmap=image;slot->key=rasterKey;slot->used=++g_curveRasterUse;
                } else DeleteObject(image);
            }
            if(target) DeleteDC(target);
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
    const long long selectionStart = chartTimeAtPixel(t0, t1, left, right, std::min(g_selectionStartX, g_selectionEndX));
    const long long selectionEnd = chartTimeAtPixel(t0, t1, left, right, std::max(g_selectionStartX, g_selectionEndX));
    const std::wstring rangeText = (g_selectingTime ? UiText(TextId::ui_0337) : L"") +
        U8ToW(fmtTime(g_selectingTime ? selectionStart : t0, "FULL")) + L" → " +
        U8ToW(fmtTime(g_selectingTime ? selectionEnd : t1, "FULL")) +
        (g_chartHasInferredTime ? UiText(TextId::ui_0481) : L"");
    RECT rangeRect{left, S(3), canvasRight, S(24)};
    DrawTextW(hdc, rangeText.c_str(), -1, &rangeRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    auto title = [&](const RECT& plot, const std::wstring& text, size_t samples, bool detailPlot) {
        if (plot.right <= plot.left) return;
        SelectObject(hdc, App().hFontUI); SetTextColor(hdc, th::inkPri);
        RECT label{plot.left, plot.top - S(22), plot.right - (detailPlot ? S(205) : 0), plot.top - S(2)};
        DrawTextW(hdc, text.c_str(), -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, App().hFontSmall); SetTextColor(hdc, th::inkMuted);
        RECT count{plot.right + S(6), label.top, canvasRight, label.bottom};
        const std::wstring countText = FmtW(UiText(TextId::ui_0338), static_cast<int>(samples));
        DrawTextW(hdc, countText.c_str(), -1, &count, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    title(top, UiText(TextId::ui_0339), g_csq.size(), false);
    title(middle, g_chartDetail == 2 ? UiText(TextId::ui_0540) : g_chartDetail == 1 ? L"RSRQ · -25～0 dB" : L"RSRP · -140～-40 dBm", detail.size(), true);
    title(bottom, UiText(TextId::ui_0340), g_snr10.size(), false);
    if (!detail.empty()) {
        for (int mode = 0; mode < 3; ++mode) {
            RECT button{right - S(195) + mode * S(65), middle.top - S(22), right - S(135) + mode * S(65), middle.top - S(2)};
            g_chartModeRects[mode] = button;
            const bool selected = mode == g_chartDetail, enabled = !detailSeries(mode).empty();
            FillRound(hdc, button, S(9), selected ? th::accentSoft : th::surface, selected ? th::accent : th::border);
            SelectObject(hdc, App().hFontSmall); SetTextColor(hdc, enabled ? th::inkPri : th::inkMuted);
            DrawTextW(hdc, mode == 2 ? L"RSSI" : mode == 1 ? L"RSRQ" : L"RSRP", -1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(hdc, oldFont);
    drawPlot(top, g_chartCsqDraw, g_csqGaps, 0, 31, th::s1_blue,
             {{kCsqFair, UiText(TextId::ui_0341), th::warning}, {kCsqGood, UiText(TextId::ui_0342), th::accent},
              {kCsqExcellent, UiText(TextId::ui_0343), th::good}}, false);
    if (g_chartDetail == 0)
        drawPlot(middle, g_chartDetailDraw, g_rsrpGaps, detailLo, detailHi, th::s7_violet,
                 {{kRsrpFair, UiText(TextId::ui_0344), th::warning}, {kRsrpGood, UiText(TextId::ui_0345), th::accent},
                  {kRsrpExcellent, UiText(TextId::ui_0346), th::good}}, false);
    else if (g_chartDetail == 1)
        drawPlot(middle, g_chartDetailDraw, g_rsrqGaps, detailLo, detailHi, th::s7_violet,
                 {{kRsrqFair, UiText(TextId::ui_0347), th::warning}, {kRsrqGood, UiText(TextId::ui_0348), th::accent},
                  {kRsrqExcellent, UiText(TextId::ui_0349), th::good}}, false);
    else
        drawPlot(middle,g_chartDetailDraw,g_rssiGaps,detailLo,detailHi,th::s6_orange,{},false);
    drawPlot(bottom, g_chartSnrDraw, g_snrGaps, -200, 300, th::s5_aqua,
             {{kSnrFair10, UiText(TextId::ui_0350), th::warning}, {kSnrGood10, UiText(TextId::ui_0351), th::accent},
              {kSnrExcellent10, UiText(TextId::ui_0343), th::good}}, true);

    if (g_selectingTime && std::abs(g_selectionEndX - g_selectionStartX) >= S(6)) {
        HPEN selectionPen = CreatePen(PS_SOLID, std::max(1, S(2)), th::accent);
        HGDIOBJ previousPen = SelectObject(hdc, selectionPen);
        HGDIOBJ previousBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        for (const RECT& plot : g_chartPlotRects) if (plot.right > plot.left)
            Rectangle(hdc, std::min(g_selectionStartX, g_selectionEndX), plot.top,
                      std::max(g_selectionStartX, g_selectionEndX), plot.bottom);
        SelectObject(hdc, previousBrush); SelectObject(hdc, previousPen); DeleteObject(selectionPen);
    }

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
    const std::wstring footerText = (g_latestCellId.empty() ? UiText(TextId::ui_0484) :
        FmtW(UiText(TextId::ui_0352), U8ToW(g_latestCellId).c_str(), static_cast<int>(g_visibleCellCount))) +
        std::wstring(HasAnalysisTimeFilter() ? UiText(TextId::ui_0353) :
                                               UiText(TextId::ui_0354));
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
        std::wstring info = UiText(TextId::ui_0355) + U8ToW(fmtTime(markT, "FULL")) +
            (g_chartHasInferredTime ? UiText(TextId::ui_0488) : L"");
        auto addValue = [&](const ChartSeries& series, const wchar_t* name, bool decimal, const wchar_t* unit) {
            long long distance = LLONG_MAX;
            const auto* point = nearestChartPoint(series, markT, &distance);
            info += L"\n" + std::wstring(name) + L"  ";
            if (!point) info += UiText(TextId::ui_0289);
            else if (distance > 600) info += UiText(TextId::ui_0356);
            else info += (decimal ? FmtW(L"%.1f", point->second / 10.0) : FmtW(L"%d", point->second)) +
                L" " + unit + L"  ·  " + U8ToW(fmtTime(point->first, "FULL"));
        };
        addValue(g_csq, L"CSQ", false, L"");
        addValue(g_rsrp, L"RSRP", false, L"dBm");
        addValue(g_rsrq, L"RSRQ", false, L"dB");
        addValue(g_rssi, L"RSSI", false, L"dBm");
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
    auto series=reportedSignalSeries(App().document.metricView);
    g_csq=std::move(series.csq);g_rsrp=std::move(series.rsrp);
    g_rsrq=std::move(series.rsrq);g_snr10=std::move(series.snr10);g_rssi=std::move(series.rssi);
    g_rssiBounds = rssiDisplayBounds(g_rssi);
    g_csqGaps = chartSampleGaps(g_csq, 600);
    g_rsrpGaps = chartSampleGaps(g_rsrp, 600);
    g_rsrqGaps = chartSampleGaps(g_rsrq, 600);
    g_snrGaps = chartSampleGaps(g_snr10, 600);
    g_rssiGaps = chartSampleGaps(g_rssi, 600);
    ResetChartSampleCache();
    ListView_SetItemCountEx(App().hMetric, (int)App().document.metricView.size(),
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(App().hMetric, nullptr, TRUE);
    const bool detailEmpty = g_chartDetail == 2 ? g_rssi.empty() : g_chartDetail == 0 ? g_rsrp.empty() : g_rsrq.empty();
    if (detailEmpty) {
        if (!g_rsrp.empty()) g_chartDetail = 0;
        else if (!g_rsrq.empty()) g_chartDetail = 1;
        else if (!g_rssi.empty()) g_chartDetail = 2;
    }
    UpdateChartAccessibleLabel();
    InvalidateRect(App().hChart, nullptr, TRUE);
}


void ReleaseChartPageData() {
    g_panning=false;
    if(GetCapture()==App().hChart)ReleaseCapture();
    CancelChartSelection();
    releaseVector(g_csq);
    releaseVector(g_rsrp);
    releaseVector(g_rsrq);
    releaseVector(g_rssi);
    g_rssiBounds={-120,-20};
    releaseVector(g_snr10);
    releaseVector(g_csqGaps); releaseVector(g_rsrpGaps); releaseVector(g_rsrqGaps); releaseVector(g_snrGaps);
    releaseVector(g_rssiGaps);
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

bool CancelChartSelection() {
    if (!g_selectingTime) return false;
    g_selectingTime = false;
    if (GetCapture() == App().hChart) ReleaseCapture();
    if (App().hChart) InvalidateRect(App().hChart, nullptr, FALSE);
    return true;
}

int PreferredChartHeight() {
    const bool haveDetail = !g_rsrp.empty() || !g_rsrq.empty() || !g_rssi.empty();
    const int plots = (!g_csq.empty() ? 1 : 0) + (haveDetail ? 1 : 0) + (!g_snr10.empty() ? 1 : 0);
    return S(77 + std::max(1, plots) * 104);
}

} // namespace dl
