// diallog.cpp — modem_mng 日志排查工具 (Win32 原生 GUI, MinGW 编译, 无运行时依赖)
// 界面层。解析/分析在 logmodel.* (纯标准 C++, 已用真实日志对拍验证)。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   // Win7+
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "logmodel.h"
#include "version.h"

using namespace dl;

// ============================ 控件 ID ============================
#define IDC_TAB       1001
#define IDC_OPEN      1002
#define IDC_APPLY     1003
#define IDC_CLEAR     1004
#define IDC_EXPORT    1005
#define IDC_TAGBOX    1006
#define IDC_GREPBOX   1007
#define IDC_SINCEBOX  1008
#define IDC_UNTILBOX  1009
#define IDC_FILELBL   1010
#define IDC_SUMMARY   1011
#define IDC_TIMELINE  1012
#define IDC_OUTAGE    1013
#define IDC_METRIC    1014
#define IDC_TAGS      1015
#define IDC_RAW       1016
#define IDC_CHART     1017
#define IDC_STATUS    1018
#define IDC_FINDINGS  1019
#define IDC_UNPARSED  1020
#define IDC_PASTE     1021

// ============================ 配色 ============================
static const COLORREF CRED  = RGB(200, 40, 40);
static const COLORREF CGRN  = RGB(30, 140, 60);
static const COLORREF CYEL  = RGB(170, 120, 0);
static const COLORREF CBLU  = RGB(40, 90, 200);
static const COLORREF CMAG  = RGB(160, 40, 160);
static const COLORREF CTEAL = RGB(0, 130, 140);
static const COLORREF CTXT  = RGB(0, 0, 0);

// ============================ 全局状态 ============================
static HWND hMain, hTab, hStatus, hFileLbl;
static HWND hTagBox, hGrepBox, hSinceBox, hUntilBox;
static HWND hSummary, hTimeline, hOutage, hMetric, hTags, hRaw, hChart, hExport;
static HWND hFindings, hUnparsed;
static HFONT hFontUI, hFontMono;

static std::vector<LogLine>  g_all, g_view;
static std::vector<std::string> g_sessions;
static std::vector<Outage>   g_outages;
static std::vector<MetricRow> g_metrics;
static ParseAudit            g_audit;      // 未识别行审计(“没漏消息”的硬证据)
static PlatformInfo          g_plat;       // 自动识别的来源平台
static std::vector<Finding>  g_findings;   // 结论引擎输出
static std::vector<COLORREF> g_tlColors, g_ogColors;
static std::vector<std::pair<long long,int>> g_csq;   // 供图表
static int g_curPage = 0;

static const int TOP_H   = 78;
static const int CHART_H = 190;

// ============================ 字符串工具 ============================
static std::wstring U8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string WToU8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    std::wstring w((size_t)n + 1, L'\0');
    GetWindowTextW(h, &w[0], n + 1);
    w.resize((size_t)n);
    return w;
}
static std::wstring FmtW(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    return buf;
}

// ============================ 文件读取 ============================
static bool ReadFileLines(const std::wstring& path, std::vector<std::string>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    std::string buf((size_t)sz.QuadPart, '\0');
    DWORD got = 0, total = 0;
    while (total < (DWORD)sz.QuadPart) {
        if (!ReadFile(h, &buf[total], (DWORD)sz.QuadPart - total, &got, nullptr) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    buf.resize(total);

    size_t a = 0;
    while (a <= buf.size()) {
        size_t b = buf.find('\n', a);
        if (b == std::string::npos) {
            if (a < buf.size()) out.push_back(buf.substr(a));
            break;
        }
        out.push_back(buf.substr(a, b - a));
        a = b + 1;
    }
    return true;
}

// ============================ ListView 工具 ============================
static void LvAddCol(HWND lv, int i, const wchar_t* text, int w) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.iSubItem = i;
    c.pszText = (LPWSTR)text;
    c.cx = w;
    ListView_InsertColumn(lv, i, &c);
}
static int LvAddRow(HWND lv, int row, const std::wstring& first) {
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = 0;
    it.pszText = (LPWSTR)first.c_str();
    return ListView_InsertItem(lv, &it);
}
static void LvSet(HWND lv, int row, int col, const std::wstring& s) {
    ListView_SetItemText(lv, row, col, (LPWSTR)s.c_str());
}

// ============================ 图表(自绘) ============================
static LRESULT CALLBACK ChartProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;   // 交给 WM_PAINT,避免闪烁
    if (msg != WM_PAINT) return DefWindowProcW(hwnd, msg, wp, lp);

    PAINTSTRUCT ps;
    HDC hdcWin = BeginPaint(hwnd, &ps);
    RECT rcC;
    GetClientRect(hwnd, &rcC);

    // 双缓冲
    HDC hdc = CreateCompatibleDC(hdcWin);
    HBITMAP bmp = CreateCompatibleBitmap(hdcWin, rcC.right, rcC.bottom);
    HGDIOBJ oldBmp = SelectObject(hdc, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &rcC, bg);
    DeleteObject(bg);

    SelectObject(hdc, hFontUI);
    SetBkMode(hdc, TRANSPARENT);

    RECT r{ 40, 22, rcC.right - 14, rcC.bottom - 22 };
    if (r.right - r.left < 30 || r.bottom - r.top < 30) {
        BitBlt(hdcWin, 0, 0, rcC.right, rcC.bottom, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, oldBmp); DeleteObject(bmp); DeleteDC(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    SetTextColor(hdc, RGB(110, 110, 110));
    const wchar_t* title = L"CSQ 信号强度(0-31,越高越好;竖红带=断网;黄虚线=弱信号阈值 10)";
    TextOutW(hdc, 42, 4, title, (int)wcslen(title));

    if (g_csq.size() < 2) {
        SetTextColor(hdc, RGB(170, 170, 170));
        const wchar_t* t = L"加载日志后显示信号趋势";
        TextOutW(hdc, r.left + 8, (r.top + r.bottom) / 2, t, (int)wcslen(t));
        BitBlt(hdcWin, 0, 0, rcC.right, rcC.bottom, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, oldBmp); DeleteObject(bmp); DeleteDC(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    const long long t0 = g_csq.front().first, t1 = g_csq.back().first;
    const double tot = std::max<double>(1.0, (double)(t1 - t0));
    const int CSQMAX = 31;
    auto X = [&](long long t) { return r.left + (int)((double)(t - t0) / tot * (r.right - r.left)); };
    auto Y = [&](int v) {
        int c = v < 0 ? 0 : (v > CSQMAX ? CSQMAX : v);
        return r.bottom - (int)((double)c / CSQMAX * (r.bottom - r.top));
    };

    // 断网红带
    HBRUSH band = CreateSolidBrush(RGB(250, 224, 224));
    for (const auto& o : g_outages) {
        long long e = o.recovered ? o.end : t1;
        int xs = X(o.start), xe = X(e);
        if (xe < xs + 2) xe = xs + 2;
        if (xe < r.left || xs > r.right) continue;
        RECT rb{ std::max(xs, (int)r.left), r.top, std::min(xe, (int)r.right), r.bottom };
        FillRect(hdc, &rb, band);
    }
    DeleteObject(band);

    // 网格 + Y 刻度
    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(235, 235, 235));
    HGDIOBJ oldPen = SelectObject(hdc, gridPen);
    SetTextColor(hdc, RGB(130, 130, 130));
    const int ticks[] = { 0, 10, 20, 31 };
    for (int i = 0; i < 4; ++i) {
        int y = Y(ticks[i]);
        MoveToEx(hdc, r.left, y, nullptr);
        LineTo(hdc, r.right, y);
        wchar_t lb[8];
        wsprintfW(lb, L"%d", ticks[i]);
        TextOutW(hdc, 12, y - 8, lb, (int)wcslen(lb));
    }
    SelectObject(hdc, oldPen);
    DeleteObject(gridPen);

    // 边框
    HPEN axisPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
    oldPen = SelectObject(hdc, axisPen);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, r.left, r.top, r.right, r.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(axisPen);

    // 弱信号阈值线(黄虚线)
    HPEN weakPen = CreatePen(PS_DOT, 1, RGB(220, 160, 0));
    oldPen = SelectObject(hdc, weakPen);
    MoveToEx(hdc, r.left, Y(10), nullptr);
    LineTo(hdc, r.right, Y(10));
    SelectObject(hdc, oldPen);
    DeleteObject(weakPen);

    // CSQ 折线
    HPEN linePen = CreatePen(PS_SOLID, 1, RGB(40, 90, 200));
    oldPen = SelectObject(hdc, linePen);
    bool first = true;
    for (const auto& c : g_csq) {
        int x = X(c.first), y = Y(c.second);
        if (first) { MoveToEx(hdc, x, y, nullptr); first = false; }
        else LineTo(hdc, x, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(linePen);

    // X 轴两端时间
    SetTextColor(hdc, RGB(130, 130, 130));
    std::wstring s0 = U8ToW(fmtTime(t0, "HM")), s1 = U8ToW(fmtTime(t1, "HM"));
    TextOutW(hdc, r.left, r.bottom + 3, s0.c_str(), (int)s0.size());
    SIZE sz{};
    GetTextExtentPoint32W(hdc, s1.c_str(), (int)s1.size(), &sz);
    TextOutW(hdc, r.right - sz.cx, r.bottom + 3, s1.c_str(), (int)s1.size());

    BitBlt(hdcWin, 0, 0, rcC.right, rcC.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
    EndPaint(hwnd, &ps);
    return 0;
}

// ============================ 渲染 ============================
static COLORREF RowColor(const LogLine& l) {
    if (isRecovered(l.msg, nullptr)) return CGRN;
    if (isFaultStart(l.msg))         return CRED;
    std::string t = l.tag;
    if (t == "ERROR" || t == "FATAL" || t == "CFUN") return CRED;
    if (t == "WARN" || t == "WARNING" || t == "ALARM" || t == "SLOT" || t == "OPER") return CYEL;
    if (t == "ROAMLINK") return CTEAL;
    if (t == "STATE")    return CBLU;
    if (t == "SDK")      return CMAG;
    return CTXT;
}

static void RenderSummary() {
    std::wstring o;
    if (g_view.empty()) { SetWindowTextW(hSummary, L"筛选后没有可解析的日志行。"); return; }

    const long long t0 = g_view.front().t, t1 = g_view.back().t;
    const double span = (double)(t1 - t0);

    o += L"══ modem_mng 日志总览 ══\r\n";
    o += FmtW(L"  文件跨度 : %s  →  %s   (%s)\r\n",
              U8ToW(fmtTime(t0, "FULL")).c_str(), U8ToW(fmtTime(t1, "FULL")).c_str(),
              U8ToW(fmtDur(t1 - t0)).c_str());
    o += FmtW(L"  日志行数 : %d    进程会话(重启): %d\r\n", (int)g_view.size(), (int)g_sessions.size());
    if (g_sessions.size() > 1) {
        o += FmtW(L"  ⚠ 检测到 %d 次进程重启 (L3 exit / watchdog 拉起?):\r\n", (int)g_sessions.size());
        for (size_t i = 0; i < g_sessions.size() && i < 6; ++i)
            o += L"      " + U8ToW(g_sessions[i]) + L"\r\n";
    }

    // 断网
    long long total = 0, longest = 0, longestAt = 0;
    int b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    for (const auto& x : g_outages) {
        if (!x.recovered) continue;
        total += x.dur;
        if (x.dur > longest) { longest = x.dur; longestAt = x.end; }
        if (x.dur <= 30) b0++; else if (x.dur <= 60) b1++; else if (x.dur <= 300) b2++; else b3++;
    }
    o += L"\r\n── 断网 ──\r\n";
    if (!g_outages.empty()) {
        o += FmtW(L"  断网次数 : %d   累计时长: %s   可用率≈ %.3f%%\r\n",
                  (int)g_outages.size(), U8ToW(fmtDur(total)).c_str(),
                  span > 0 ? 100.0 * (1.0 - total / span) : 0.0);
        if (longest > 0)
            o += FmtW(L"  最长单次 : %s  @ %s\r\n", U8ToW(fmtDur(longest)).c_str(),
                      U8ToW(fmtTime(longestAt, "MD")).c_str());
        o += FmtW(L"  时长分布 : ≤30s:%d  31-60s:%d  1-5m:%d  >5m:%d\r\n", b0, b1, b2, b3);
        if (!g_outages.back().recovered)
            o += FmtW(L"  ⚠ 日志结束时仍处于断网(未见恢复),始于 %s\r\n",
                      U8ToW(fmtTime(g_outages.back().start, "MD")).c_str());
    } else {
        o += L"  无断网记录(未出现 fault timer)\r\n";
    }

    // 信号/温度/通道
    long long csqSum = 0; int csqN = 0, csqMin = 9999, csqMax = -1, weak = 0; long long weakFirst = 0;
    long long tSum = 0; int tN = 0, tMax = -9999, hot = 0;
    std::map<std::string, int> chans;
    std::vector<std::pair<long long,long long>> rxs;
    for (const auto& m : g_metrics) {
        if (m.csqVal >= 0) {
            csqSum += m.csqVal; csqN++;
            csqMin = std::min(csqMin, m.csqVal);
            csqMax = std::max(csqMax, m.csqVal);
            if (m.csqVal < 10) { if (weak == 0) weakFirst = m.t; weak++; }
        }
        if (m.tmax != "-") { int v = atoi(m.tmax.c_str()); tSum += v; tN++; tMax = std::max(tMax, v); if (v >= 85) hot++; }
        if (m.ch != "-") chans[m.ch]++;
        if (m.rx != "-") rxs.push_back({ m.t, atoll(m.rx.c_str()) });
    }
    if (csqN) {
        o += L"\r\n── 信号 CSQ ──\r\n";
        o += FmtW(L"  min/avg/max : %d / %.1f / %d   样本:%d\r\n", csqMin, (double)csqSum / csqN, csqMax, csqN);
        if (weak) o += FmtW(L"  弱信号(<10) : %d 次  首次 %s\r\n", weak, U8ToW(fmtTime(weakFirst, "MD")).c_str());
    }
    if (tN) {
        o += L"\r\n── 温度 ──\r\n";
        o += FmtW(L"  max=%d°C  avg=%.0f°C", tMax, (double)tSum / tN);
        if (hot) o += FmtW(L"   ⚠ ≥85°C %d次", hot);
        o += L"\r\n";
    }
    if (!chans.empty()) {
        int tot = 0;
        for (auto& kv : chans) tot += kv.second;
        o += L"\r\n── 通道占比 ──\r\n  ";
        for (auto& kv : chans) o += FmtW(L"%s:%.0f%%   ", U8ToW(kv.first).c_str(), 100.0 * kv.second / tot);
        o += L"\r\n";
    }

    // RX 停滞
    auto stalls = detectRxStall(rxs);
    if (!stalls.empty()) {
        o += L"\r\n── 数据假死征兆(RX_PKT 停滞) ──\r\n";
        for (size_t i = 0; i < stalls.size() && i < 6; ++i)
            o += FmtW(L"  RX_PKT 卡住 %s  %s → %s\r\n", U8ToW(fmtDur(stalls[i].dur)).c_str(),
                      U8ToW(fmtTime(stalls[i].start, "MD")).c_str(), U8ToW(fmtTime(stalls[i].end, "HM")).c_str());
        if (stalls.size() > 6) o += FmtW(L"  ... 另有 %d 段\r\n", (int)stalls.size() - 6);
    }

    // 报错
    std::vector<const LogLine*> errs;
    for (const auto& l : g_view) if (isErrLine(l)) errs.push_back(&l);
    if (!errs.empty()) {
        o += FmtW(L"\r\n── 报错/告警 (%d) ──\r\n", (int)errs.size());
        for (size_t i = 0; i < errs.size() && i < 12; ++i)
            o += FmtW(L"  %s  [%s] %s\r\n", U8ToW(errs[i]->ts).c_str(),
                      U8ToW(errs[i]->tag).c_str(), U8ToW(errs[i]->msg.substr(0, 90)).c_str());
        if (errs.size() > 12) o += FmtW(L"  ... 另有 %d 条\r\n", (int)errs.size() - 12);
    }

    // 关键事件计数
    int sw = 0, disc = 0, states = 0, cfun = 0, slot = 0, oper = 0, cells = 0;
    for (const auto& l : g_view) {
        if (l.msg.find("switching to SIM") != std::string::npos ||
            l.msg.find("switching to Roamlink") != std::string::npos) sw++;
        if (l.msg.find("DataCall disconnected") != std::string::npos) disc++;
        if (l.tag == "STATE") states++;
        if (l.tag == "CFUN" || l.msg.find("CFUN=0") != std::string::npos ||
            l.msg.find("CFUN toggle") != std::string::npos) cfun++;
        if (l.tag == "SLOT") slot++;
        if (l.tag == "OPER") oper++;
        if (l.tag.compare(0, 4, "CELL") == 0) cells++;
    }
    o += L"\r\n── 关键事件计数 ──\r\n";
    o += FmtW(L"  通道切换:%d  SDK断开:%d  状态迁移:%d  CFUN:%d  切卡:%d  选网:%d  小区变更:%d\r\n",
              sw, disc, states, cfun, slot, oper, cells);
    o += L"\r\n提示: “时间线”看事件流, “断网”看逐次, “指标/信号图”看 CSQ 与 ΔRX(=0 即数据不通)。\r\n";

    SetWindowTextW(hSummary, o.c_str());
}

static void RenderTimeline() {
    ListView_DeleteAllItems(hTimeline);
    g_tlColors.clear();
    int row = 0;
    for (const auto& l : g_view) {
        bool keep = isEventLine(l);
        if (l.tag.compare(0, 9, "HEARTBEAT") == 0 && (isFaultStart(l.msg) || isRecovered(l.msg, nullptr)))
            keep = true;
        if (!keep) continue;
        LvAddRow(hTimeline, row, U8ToW(fmtTime(l.t, "MD")));
        LvSet(hTimeline, row, 1, U8ToW(l.tag));
        LvSet(hTimeline, row, 2, U8ToW(l.msg.substr(0, 200)));
        g_tlColors.push_back(RowColor(l));
        row++;
    }
}

static void RenderOutages() {
    ListView_DeleteAllItems(hOutage);
    g_ogColors.clear();
    int row = 0;
    for (const auto& o : g_outages) {
        LvAddRow(hOutage, row, FmtW(L"%d", row + 1));
        LvSet(hOutage, row, 1, U8ToW(fmtTime(o.start, "MD")));
        if (o.recovered) {
            LvSet(hOutage, row, 2, U8ToW(fmtTime(o.end, "MD")));
            LvSet(hOutage, row, 3, U8ToW(fmtDur(o.dur)));
            g_ogColors.push_back(o.dur > 60 ? CRED : (o.dur > 30 ? CYEL : CTXT));
        } else {
            LvSet(hOutage, row, 2, L"未恢复");
            LvSet(hOutage, row, 3, L"?");
            g_ogColors.push_back(CRED);
        }
        row++;
    }
}

static void RenderMetrics() {
    ListView_DeleteAllItems(hMetric);
    g_csq.clear();
    int row = 0;
    for (const auto& m : g_metrics) {
        LvAddRow(hMetric, row, U8ToW(m.ts));
        LvSet(hMetric, row, 1, U8ToW(m.ch));
        LvSet(hMetric, row, 2, U8ToW(m.csq));
        LvSet(hMetric, row, 3, U8ToW(m.tmax));
        LvSet(hMetric, row, 4, U8ToW(m.cf));
        LvSet(hMetric, row, 5, U8ToW(m.rx));
        LvSet(hMetric, row, 6, U8ToW(m.drx));
        if (m.csqVal >= 0) g_csq.push_back({ m.t, m.csqVal });
        row++;
    }
    InvalidateRect(hChart, nullptr, TRUE);
}

static void RenderTags() {
    ListView_DeleteAllItems(hTags);
    std::map<std::string, int> tc;
    for (const auto& l : g_view) tc[l.tag.empty() ? "(无标签)" : l.tag]++;
    int mx = 1;
    for (auto& kv : tc) mx = std::max(mx, kv.second);
    std::vector<std::pair<std::string,int>> v(tc.begin(), tc.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    int row = 0;
    for (auto& kv : v) {
        LvAddRow(hTags, row, U8ToW(kv.first));
        LvSet(hTags, row, 1, FmtW(L"%d", kv.second));
        LvSet(hTags, row, 2, std::wstring((size_t)std::max(0, kv.second * 50 / mx), L'█'));
        row++;
    }
}

static void RenderRaw() {
    const size_t CAP = 5000;
    std::string s;
    s.reserve(256 * 1024);
    size_t n = 0;
    for (const auto& l : g_view) {
        s += l.ts;
        // seas_log(artery)的严重度在 level 字段、且多数行没有内嵌 [TAG];
        // 无标签时不要打出空的 "[]"
        if (l.fmt == FMT_SEAS && !l.level.empty()) { s += " ["; s += l.level; s += "]"; }
        if (!l.tag.empty())                        { s += " ["; s += l.tag;   s += "]"; }
        s += " "; s += l.msg; s += "\r\n";
        if (++n >= CAP) { s += "\r\n… 已截断,仅显示前 5000 行(用筛选缩小范围)\r\n"; break; }
    }
    SetWindowTextW(hRaw, U8ToW(s).c_str());
}

// “结论”页:自动根因 + 处置建议 + 每条结论的日志证据(行号/时间戳)
static void RenderFindings() {
    std::wstring o;
    o += L"══ 自动结论(每条都附日志证据,无证据不输出)══\r\n\r\n";
    o += L"  来源平台 : " + U8ToW(g_plat.name) + L"\r\n";
    if (g_plat.evidenceLine)
        o += FmtW(L"  识别依据 : 第 %d 行  ", (int)g_plat.evidenceLine) + U8ToW(g_plat.evidence) + L"\r\n";
    o += FmtW(L"  解析覆盖 : 已解析 %d 行,未识别 %d 行(%.2f%%)",
              (int)g_audit.parsed, (int)g_audit.unparsed, g_audit.unparsedRatio() * 100.0);
    o += (g_audit.unparsed == 0) ? L"  → 无遗漏\r\n" : L"  → 详见“未识别行”页\r\n";
    o += L"\r\n";

    if (g_findings.empty()) {
        o += L"  未得出任何有证据支撑的结论。\r\n"
             L"  (这不等于“没问题”:可能是日志时段内确无异常,也可能是证据不足——\r\n"
             L"   本工具只在有日志证据时下结论,不臆测。)\r\n";
        SetWindowTextW(hFindings, o.c_str());
        return;
    }

    int n = 0;
    for (const auto& f : g_findings) {
        const wchar_t* lv = (f.severity == 2) ? L"【严重】" : (f.severity == 1 ? L"【告警】" : L"【信息】");
        o += FmtW(L"── %d. ", ++n) + std::wstring(lv) + U8ToW(f.title) + L"\r\n";
        o += L"   依据: " + U8ToW(f.detail) + L"\r\n";
        o += L"   建议: " + U8ToW(f.advice) + L"\r\n";
        o += L"   证据:\r\n";
        for (const auto& e : f.ev)
            o += FmtW(L"     · 第 %d 行  ", (int)e.lineNo) + U8ToW(e.ts) + L"  " + U8ToW(e.text) + L"\r\n";
        o += L"\r\n";
    }
    SetWindowTextW(hFindings, o.c_str());
}

// “未识别行”页:把解析不了的行摆出来,这是“完完整整不漏消息”的唯一硬证据
static void RenderUnparsed() {
    ListView_DeleteAllItems(hUnparsed);
    int row = 0;
    for (const auto& u : g_audit.samples) {
        LvAddRow(hUnparsed, row, FmtW(L"%d", (int)u.lineNo));
        LvSet(hUnparsed, row, 1, U8ToW(u.text.substr(0, 300)));
        row++;
    }
}

static void ShowPage(int page) {
    // 页序:0总览 1结论 2时间线 3断网 4指标 5标签 6原始行 7未识别行
    struct { HWND* h; int page; } items[] = {
        { &hSummary, 0 }, { &hFindings, 1 }, { &hTimeline, 2 }, { &hOutage, 3 },
        { &hChart, 4 }, { &hMetric, 4 }, { &hExport, 4 },
        { &hTags, 5 }, { &hRaw, 6 }, { &hUnparsed, 7 },
    };
    for (auto& it : items) {
        bool on = (it.page == page);
        ShowWindow(*it.h, on ? SW_SHOW : SW_HIDE);
        if (on) SetWindowPos(*it.h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    g_curPage = page;
}

static void RefreshAll() {
    if (g_all.empty()) { SetWindowTextW(hStatus, L"尚未加载日志。"); return; }
    bool bad = false;
    g_view = applyFilters(g_all, WToU8(GetText(hTagBox)), WToU8(GetText(hGrepBox)),
                          WToU8(GetText(hSinceBox)), WToU8(GetText(hUntilBox)), &bad);
    g_outages = collectOutages(g_view);
    g_metrics = buildMetrics(g_view);

    // 结论基于**筛选后**的视图,与各页展示保持一致
    g_findings = analyze(g_view, g_outages, g_metrics, g_plat, g_audit);

    RenderSummary();
    RenderFindings();
    RenderTimeline();
    RenderOutages();
    RenderMetrics();
    RenderTags();
    RenderRaw();
    RenderUnparsed();

    std::wstring st = FmtW(L"  筛选后 %d / 共 %d 行   ·   断网 %d 次   ·   会话(重启) %d",
                           (int)g_view.size(), (int)g_all.size(), (int)g_outages.size(), (int)g_sessions.size());
    st += L"   ·   平台: " + U8ToW(g_plat.name);
    // 未识别行占比常显在状态栏:任何一页都能看到覆盖率,而不是藏在某个页里
    if (g_audit.unparsed == 0)
        st += L"   ·   未识别 0 行(无遗漏)";
    else
        st += FmtW(L"   ·   ⚠ 未识别 %d 行(%.2f%%,见“未识别行”页)",
                   (int)g_audit.unparsed, g_audit.unparsedRatio() * 100.0);
    if (bad) st += L"   ·   ⚠ 正则非法,已忽略该条件";
    SetWindowTextW(hStatus, st.c_str());
}

// 载入的公共尾段:拿到原始行之后的处理,文件与剪贴板共用
static void LoadRawLines(const std::vector<std::string>& raw, const std::wstring& srcLabel) {
    parseLines(raw, g_all, g_sessions, &g_audit);
    g_plat = detectPlatform(g_all);   // 平台识别用全量行(不受筛选影响)
    std::wstring lbl = srcLabel;
    lbl += FmtW(L"   (%d 行, %d 会话, %s)", (int)g_all.size(), (int)g_sessions.size(),
                U8ToW(g_plat.name).c_str());
    SetWindowTextW(hFileLbl, lbl.c_str());
    RefreshAll();
}

static void LoadFiles(const std::vector<std::wstring>& paths) {
    std::vector<std::string> raw;
    std::vector<std::wstring> ok;
    for (const auto& p : paths) {
        std::vector<std::string> one;
        if (ReadFileLines(p, one)) { raw.insert(raw.end(), one.begin(), one.end()); ok.push_back(p); }
        else MessageBoxW(hMain, (L"读取失败:\n" + p).c_str(), L"错误", MB_ICONERROR);
    }
    if (ok.empty()) return;
    LoadRawLines(raw, (ok.size() == 1) ? ok[0] : FmtW(L"%d 个文件合并", (int)ok.size()));
}

// 从剪贴板粘贴日志文本分析(SSH 里 cat 日志后直接选中复制的场景,手上没有文件)
static void DoPaste() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        MessageBoxW(hMain, L"剪贴板里没有文本。\n\n"
                           L"请先复制日志内容(如在 SSH 终端里选中 dial 日志文本),再点“粘贴日志”。",
                    L"提示", MB_ICONINFORMATION);
        return;
    }
    if (!OpenClipboard(hMain)) {
        MessageBoxW(hMain, L"打不开剪贴板(可能被其它程序占用),请稍后重试。", L"错误", MB_ICONERROR);
        return;
    }
    std::wstring w;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) { w = p; GlobalUnlock(h); }
    }
    CloseClipboard();
    if (w.empty()) {
        MessageBoxW(hMain, L"剪贴板文本为空。", L"提示", MB_ICONINFORMATION);
        return;
    }

    // 按行切分(兼容 \r\n / \n / \r 三种换行)
    std::string u8 = WToU8(w);
    std::vector<std::string> raw;
    std::string cur;
    for (char c : u8) {
        if (c == '\n') { raw.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
        else { raw.push_back(cur); cur.clear(); }   // 单独的 \r 也当换行(老式 Mac/终端粘贴)
    }
    if (!cur.empty()) raw.push_back(cur);

    LoadRawLines(raw, FmtW(L"[剪贴板粘贴 %d 行]", (int)raw.size()));

    // 粘贴的往往是片段,若一行都没认出来,直接把原因摆出来(而不是让用户对着空界面猜)
    if (g_all.empty()) {
        MessageBoxW(hMain,
            L"粘贴的内容里没有解析出任何日志行。\n\n"
            L"本工具认两种格式:\n"
            L"  [YYYY-MM-DD HH:MM:SS] [TAG] message      (modem_mng / open_dial)\n"
            L"  YYYY-MM-DD HH:MM:SS.mmm [LEVEL] func (file:line) - message   (artery)\n\n"
            L"请确认复制时带上了行首的时间戳。\n"
            L"具体哪些行没被认出,可看“未识别行”页。",
            L"没有可分析的日志行", MB_ICONWARNING);
    }
}

static void DoOpen() {
    std::vector<wchar_t> buf(32768, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hMain;
    ofn.lpstrFilter = L"日志文件 (*.log;*.txt)\0*.log;*.txt\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrTitle = L"选择 dial 日志(可多选,将合并分析)";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;

    std::vector<std::wstring> paths;
    std::wstring dir = buf.data();
    size_t p = dir.size() + 1;
    if (buf[p] == 0) {           // 单选:buf 即完整路径
        paths.push_back(dir);
    } else {                     // 多选:目录\0文件1\0文件2\0\0
        while (buf[p]) {
            std::wstring f = &buf[p];
            paths.push_back(dir + L"\\" + f);
            p += f.size() + 1;
        }
    }
    LoadFiles(paths);
}

static void DoExportCsv() {
    if (g_metrics.empty()) { MessageBoxW(hMain, L"没有可导出的指标数据。", L"提示", MB_ICONINFORMATION); return; }
    wchar_t file[MAX_PATH] = L"diallog_metrics.csv";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hMain;
    ofn.lpstrFilter = L"CSV (*.csv)\0*.csv\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;

    std::string out = "\xEF\xBB\xBF";              // UTF-8 BOM,Excel 中文不乱码
    out += "time,ch,csq,tmax,consec_fail,rx_pkt,drx\r\n";
    for (const auto& m : g_metrics) {
        out += m.ts; out += ',';
        out += m.ch; out += ',';
        out += m.csq; out += ',';
        out += m.tmax; out += ',';
        out += m.cf; out += ',';
        out += m.rx; out += ',';
        out += m.drx; out += "\r\n";
    }
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { MessageBoxW(hMain, L"写入失败。", L"错误", MB_ICONERROR); return; }
    DWORD wr = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr);
    CloseHandle(h);
    SetWindowTextW(hStatus, (std::wstring(L"  已导出 ") + file).c_str());
}

// ============================ 布局 ============================
static void Layout() {
    RECT rc;
    GetClientRect(hMain, &rc);
    SendMessageW(hStatus, WM_SIZE, 0, 0);
    RECT rs;
    GetWindowRect(hStatus, &rs);
    int statusH = rs.bottom - rs.top;

    int tabTop = TOP_H;
    int tabH = rc.bottom - tabTop - statusH;
    if (tabH < 40) tabH = 40;
    MoveWindow(hTab, 0, tabTop, rc.right, tabH, TRUE);

    RECT d{ 0, tabTop, rc.right, tabTop + tabH };
    TabCtrl_AdjustRect(hTab, FALSE, &d);

    MoveWindow(hSummary,  d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(hFindings, d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(hTimeline, d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(hOutage,   d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(hTags,     d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(hRaw,      d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(hUnparsed, d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);

    // 指标页:图表(上) + 表格(中) + 导出按钮(下)
    int ch = std::min<int>(CHART_H, (int)(d.bottom - d.top) / 2);   // RECT 成员是 LONG,显式定型
    MoveWindow(hChart,  d.left, d.top, d.right - d.left, ch, TRUE);
    int btnH = 28;
    int gridTop = d.top + ch;
    int gridH = (d.bottom - d.top) - ch - btnH;
    if (gridH < 30) gridH = 30;
    MoveWindow(hMetric, d.left, gridTop, d.right - d.left, gridH, TRUE);
    MoveWindow(hExport, d.left, gridTop + gridH, 160, btnH, TRUE);

    // 顶部文件名标签跟随宽度
    MoveWindow(hFileLbl, 196, 10, std::max(200, (int)rc.right - 208), 20, TRUE);  // 让开“粘贴日志”按钮
}

// ============================ 主窗口 ============================
static HWND Mk(const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id, HFONT f) {
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, hMain, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

static HWND MkLv(int id, std::initializer_list<std::pair<const wchar_t*, int>> cols) {
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS,
                              0, 0, 10, 10, hMain, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    SendMessageW(lv, WM_SETFONT, (WPARAM)hFontMono, TRUE);
    int i = 0;
    for (auto& c : cols) LvAddCol(lv, i++, c.first, c.second);
    return lv;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        hMain = hwnd;
        // 字体
        hFontUI = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        hFontMono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");

        // 顶部工具栏
        Mk(L"BUTTON", L"打开日志…", BS_PUSHBUTTON, 8, 6, 96, 26, IDC_OPEN, hFontUI);
        Mk(L"BUTTON", L"粘贴日志", BS_PUSHBUTTON, 108, 6, 80, 26, IDC_PASTE, hFontUI);
        hFileLbl = Mk(L"STATIC", L"未加载 —— 拖入 dial_*.log(可多选),或复制日志文本后按 Ctrl+V / 点“粘贴日志”",
                      SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 196, 10, 820, 20, IDC_FILELBL, hFontUI);

        int y = 44;
        Mk(L"STATIC", L"标签:", SS_LEFT, 8, y + 4, 36, 18, 0, hFontUI);
        hTagBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 46, y, 140, 22, IDC_TAGBOX, hFontUI);
        Mk(L"STATIC", L"筛选(正则):", SS_LEFT, 196, y + 4, 74, 18, 0, hFontUI);
        hGrepBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 272, y, 210, 22, IDC_GREPBOX, hFontUI);
        Mk(L"STATIC", L"起:", SS_LEFT, 492, y + 4, 22, 18, 0, hFontUI);
        hSinceBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 516, y, 84, 22, IDC_SINCEBOX, hFontUI);
        Mk(L"STATIC", L"止:", SS_LEFT, 608, y + 4, 22, 18, 0, hFontUI);
        hUntilBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 632, y, 84, 22, IDC_UNTILBOX, hFontUI);
        Mk(L"BUTTON", L"应用筛选", BS_PUSHBUTTON, 728, y - 2, 84, 26, IDC_APPLY, hFontUI);
        Mk(L"BUTTON", L"清空", BS_PUSHBUTTON, 818, y - 2, 60, 26, IDC_CLEAR, hFontUI);

        // 页签
        hTab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, TOP_H, 100, 100, hwnd, (HMENU)(INT_PTR)IDC_TAB,
                               GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hTab, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        const wchar_t* tabs[] = { L"总览", L"结论 ★", L"时间线", L"断网",
                                  L"指标 / 信号图", L"标签", L"原始行", L"未识别行" };
        for (int i = 0; i < (int)(sizeof(tabs)/sizeof(tabs[0])); ++i) {
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = (LPWSTR)tabs[i];
            TabCtrl_InsertItem(hTab, i, &ti);
        }

        // 各页控件
        hSummary = Mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY,
                      0, 0, 10, 10, IDC_SUMMARY, hFontMono);
        hFindings = Mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY,
                       0, 0, 10, 10, IDC_FINDINGS, hFontMono);
        hUnparsed = MkLv(IDC_UNPARSED, { {L"原始行号", 90}, {L"未识别的原文", 960} });
        hTimeline = MkLv(IDC_TIMELINE, { {L"时间", 140}, {L"标签", 90}, {L"消息", 820} });
        hOutage   = MkLv(IDC_OUTAGE,   { {L"#", 44}, {L"开始", 160}, {L"恢复", 160}, {L"时长", 90} });
        hMetric   = MkLv(IDC_METRIC,   { {L"时间", 140}, {L"CH", 90}, {L"CSQ", 60}, {L"Tmax", 60},
                                         {L"ConsecFail", 90}, {L"RX_PKT", 110}, {L"ΔRX", 80} });
        hTags     = MkLv(IDC_TAGS,     { {L"标签", 150}, {L"次数", 80}, {L"占比", 600} });
        hRaw = Mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY,
                  0, 0, 10, 10, IDC_RAW, hFontMono);
        hChart = CreateWindowExW(0, L"dialChartCls", L"", WS_CHILD | WS_BORDER,
                                 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_CHART,
                                 GetModuleHandleW(nullptr), nullptr);
        hExport = Mk(L"BUTTON", L"导出指标 CSV…", BS_PUSHBUTTON, 0, 0, 160, 28, IDC_EXPORT, hFontUI);

        // 状态栏
        hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"  就绪。拖入 dial_*.log 开始。",
                                  WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                                  hwnd, (HMENU)(INT_PTR)IDC_STATUS, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hStatus, WM_SETFONT, (WPARAM)hFontUI, TRUE);

        ShowPage(0);
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }

    case WM_SIZE:
        Layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 920;
        mmi->ptMinTrackSize.y = 520;
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(hd, i, nullptr, 0);
            std::wstring p((size_t)len + 1, L'\0');
            DragQueryFileW(hd, i, &p[0], len + 1);
            p.resize(len);
            paths.push_back(p);
        }
        DragFinish(hd);
        if (!paths.empty()) LoadFiles(paths);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_OPEN:   DoOpen(); return 0;
        case IDC_PASTE:  DoPaste(); return 0;
        case IDC_APPLY:  RefreshAll(); return 0;
        case IDC_EXPORT: DoExportCsv(); return 0;
        case IDC_CLEAR:
            SetWindowTextW(hTagBox, L"");
            SetWindowTextW(hGrepBox, L"");
            SetWindowTextW(hSinceBox, L"");
            SetWindowTextW(hUntilBox, L"");
            RefreshAll();
            return 0;
        }
        return 0;

    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)lp;
        if (hdr->idFrom == IDC_TAB && hdr->code == TCN_SELCHANGE) {
            ShowPage(TabCtrl_GetCurSel(hTab));
            return 0;
        }
        if (hdr->code == NM_CUSTOMDRAW &&
            (hdr->hwndFrom == hTimeline || hdr->hwndFrom == hOutage || hdr->hwndFrom == hMetric)) {
            LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lp;
            // 时间线 / 断网:整行文字着色
            if (hdr->hwndFrom == hTimeline || hdr->hwndFrom == hOutage) {
                const std::vector<COLORREF>& cols = (hdr->hwndFrom == hTimeline) ? g_tlColors : g_ogColors;
                switch (cd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:     return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT: {
                    size_t i = (size_t)cd->nmcd.dwItemSpec;
                    if (i < cols.size()) cd->clrText = cols[i];
                    return CDRF_DODEFAULT;
                }
                }
                return CDRF_DODEFAULT;
            }
            // 指标:按单元格底色高亮(ΔRX=0 / 弱信号)
            switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:     return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
            case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                size_t i = (size_t)cd->nmcd.dwItemSpec;
                int sub = cd->iSubItem;
                cd->clrTextBk = GetSysColor(COLOR_WINDOW);
                if (i < g_metrics.size()) {
                    const MetricRow& m = g_metrics[i];
                    if (sub == 6 && m.drxZero)                      cd->clrTextBk = RGB(255, 214, 245);
                    else if (sub == 2 && m.csqVal >= 0 && m.csqVal < 10) cd->clrTextBk = RGB(255, 238, 200);
                }
                return CDRF_DODEFAULT;
            }
            }
            return CDRF_DODEFAULT;
        }
        return 0;
    }

    case WM_DESTROY:
        if (hFontUI)   DeleteObject(hFontUI);
        if (hFontMono) DeleteObject(hFontMono);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wcChart{};
    wcChart.cbSize = sizeof(wcChart);
    wcChart.lpfnWndProc = ChartProc;
    wcChart.hInstance = hInst;
    wcChart.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcChart.lpszClassName = L"dialChartCls";
    RegisterClassExW(&wcChart);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"dialLogMainCls";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // 标题带版本:用户发来的截图能直接看出是哪个 build
    HWND w = CreateWindowExW(WS_EX_ACCEPTFILES, L"dialLogMainCls",
                             DL_APP_NAME_W L" v" DL_VER_WSTR L" — 拨号日志分析",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760,
                             nullptr, nullptr, hInst, nullptr);
    if (!w) return 1;
    ShowWindow(w, nCmdShow);
    UpdateWindow(w);

    // 命令行:dialLog.exe [--tab=N] a.log [b.log ...]
    // 支持“拖到 exe 图标上打开”“右键→打开方式”“命令行直开”;--tab=0..7 指定初始页签(0总览 1结论 2时间线 3断网 4指标 5标签 6原始行 7未识别行)
    if (lpCmdLine && *lpCmdLine) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            std::vector<std::wstring> paths;
            int tab = -1;
            bool paste = false;
            for (int i = 1; i < argc; ++i) {
                std::wstring a = argv[i];
                if (a.compare(0, 6, L"--tab=") == 0) tab = _wtoi(a.c_str() + 6);
                else if (a == L"--paste") paste = true;   // 直接分析剪贴板内容
                else paths.push_back(a);
            }
            LocalFree(argv);
            if (!paths.empty()) LoadFiles(paths);
            else if (paste) DoPaste();
            if (tab >= 0 && tab < 8) {
                TabCtrl_SetCurSel(hTab, tab);
                ShowPage(tab);
            }
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Ctrl+V 载入剪贴板日志。但焦点在筛选输入框里时不拦——那里 Ctrl+V 该是正常粘贴文字。
        if (msg.message == WM_KEYDOWN && msg.wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            HWND f = GetFocus();
            if (f != hTagBox && f != hGrepBox && f != hSinceBox && f != hUntilBox) {
                DoPaste();
                continue;
            }
        }
        if (IsDialogMessageW(w, &msg)) continue;   // Tab 键在输入框间跳转
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
