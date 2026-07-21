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
#include "theme.h"

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
#define IDC_DASH      1022

// ============================ 全局状态 ============================
static HWND hMain, hTab, hStatus, hFileLbl;
static HWND hTagBox, hGrepBox, hSinceBox, hUntilBox;
static HWND hSummary, hTimeline, hOutage, hMetric, hTags, hRaw, hChart, hExport, hDash;
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

// ============================ DPI 缩放中枢 ============================
// PerMonitorV2:所有尺寸以 96 DPI 逻辑像素书写,经 S() 换算成当前显示器物理像素。
// g_dpi 在 WM_CREATE 初始化、WM_DPICHANGED 更新。一处控制,避免 45 处散改漏改。
static int g_dpi = 96;
static inline int S(int logical) { return MulDiv(logical, g_dpi, 96); }
// 字体高度是负值(-12 表示字符高 12px),换算要保留符号
static inline int SF(int logicalNeg) { return -MulDiv(-logicalNeg, g_dpi, 96); }

// 这些原来是编译期常量,DPI 化后必须运行期算(依赖 g_dpi),故改成取值函数。
static inline int TOP_H()   { return S(78); }
static inline int CHART_H() { return S(190); }

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
static bool ReadFileBytes(const std::wstring& path, std::string& buf) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    buf.assign((size_t)sz.QuadPart, '\0');
    DWORD got = 0, total = 0;
    while (total < (DWORD)sz.QuadPart) {
        if (!ReadFile(h, &buf[total], (DWORD)sz.QuadPart - total, &got, nullptr) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    buf.resize(total);
    return true;
}

// 字节缓冲 → 文本行。先剥 UTF-8 BOM(见 stripBom 说明:BOM 会破坏首行时间戳判定)。
static void SplitLines(std::string buf, std::vector<std::string>& out) {
    dl::stripBom(buf);
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
}

// 读一个路径 → 一到多份行缓冲(chunks)+ 各自展示名(labels)。
// 压缩包(.zip/.tar.gz/.gz)在内存中解压;一个包里的每个文件成为独立 chunk,
// 这样它们照常走后续的定序 / 时基混合防护(与手工解压后多选拖入等价)。
// 返回 false = 连字节都读不到(路径错/占用);解压失败会退化为"把原始字节当普通日志"。
static bool ReadPathExpand(const std::wstring& path,
                           std::vector<std::vector<std::string>>& chunks,
                           std::vector<std::wstring>& labels) {
    std::string buf;
    if (!ReadFileBytes(path, buf)) return false;

    // 取纯文件名用于包内条目命名
    std::wstring base = path;
    size_t slash = base.find_last_of(L"\\/");
    if (slash != std::wstring::npos) base = base.substr(slash + 1);

    if (dl::archiveKindOf(buf) != dl::ARC_NONE) {
        std::vector<dl::ArchiveEntry> entries;
        std::string err;
        if (dl::extractArchive(buf, entries, err)) {
            for (auto& e : entries) {
                std::vector<std::string> lines;
                SplitLines(std::move(e.data), lines);
                if (lines.empty()) continue;
                chunks.push_back(std::move(lines));
                // 展示名:包名!内部名(内部名可能为空,如单文件 .gz)
                std::wstring inner = U8ToW(e.name);
                labels.push_back(inner.empty() ? base : (base + L"!" + inner));
            }
            if (!chunks.empty()) return true;
            // 解压成功但没有可用条目 → 退化为普通读取
        }
        // 解压失败:静默退化,把原始字节当普通日志(可能只是扩展名碰巧像压缩包)
    }

    std::vector<std::string> lines;
    SplitLines(std::move(buf), lines);
    chunks.push_back(std::move(lines));
    labels.push_back(base);
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

// ============================ 仪表盘(总览页顶部,自绘) ============================
// 设计约束(照做):hero 数字每视图只允许一个(=可用率);文字一律 ink 系,绝不用数据色;
// 条形 ≤24px 厚、数据端 4px 圆角而基线端方角;网格/轴发丝实线、退让。

static HFONT hFontHero, hFontTileVal, hFontTileLbl, hFontSect;

// 圆角数据端 + 方角基线端的水平条
static void DrawBar(HDC hdc, int x, int y, int w, int h, COLORREF c) {
    if (w <= 0) return;
    HBRUSH br = CreateSolidBrush(c);
    HPEN   pn = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(hdc, br), op = SelectObject(hdc, pn);
    if (w > 6) {
        RoundRect(hdc, x, y, x + w, y + h, 8, 8);   // 4px 半径
        RECT sq{ x, y, x + 5, y + h };
        FillRect(hdc, &sq, br);                      // 基线端压回方角
    } else {
        RECT r{ x, y, x + w, y + h };
        FillRect(hdc, &r, br);
    }
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(br); DeleteObject(pn);
}

static void DrawText_(HDC hdc, int x, int y, const std::wstring& s, HFONT f, COLORREF c) {
    HGDIOBJ of = SelectObject(hdc, f);
    SetTextColor(hdc, c);
    TextOutW(hdc, x, y, s.c_str(), (int)s.size());
    SelectObject(hdc, of);
}
static int TextW_(HDC hdc, const std::wstring& s, HFONT f) {
    HGDIOBJ of = SelectObject(hdc, f);
    SIZE sz{}; GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &sz);
    SelectObject(hdc, of);
    return sz.cx;
}

// 指标卡:发丝描边 + 标签(次要 ink)+ 数值(主 ink 半粗)
static void DrawTile(HDC hdc, RECT r, const std::wstring& label, const std::wstring& val,
                     COLORREF valColor, const std::wstring& note) {
    HBRUSH bg = CreateSolidBrush(th::surface);
    FillRect(hdc, &r, bg);
    DeleteObject(bg);
    HPEN pn = CreatePen(PS_SOLID, 1, th::border);
    HGDIOBJ op = SelectObject(hdc, pn);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(pn);

    // 行位从 top 顺排,不用 bottom 反推 —— 反推会让 22px 的数值和注释叠在一起
    DrawText_(hdc, r.left + 12, r.top + 8,  label, hFontTileLbl, th::inkSec);
    DrawText_(hdc, r.left + 12, r.top + 26, val,   hFontTileVal, valColor);
    if (!note.empty())
        DrawText_(hdc, r.left + 12, r.top + 56, note, hFontTileLbl, th::inkMuted);
}

static LRESULT CALLBACK DashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg != WM_PAINT) return DefWindowProcW(hwnd, msg, wp, lp);

    PAINTSTRUCT ps;
    HDC hw = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    HDC hdc = CreateCompatibleDC(hw);
    HBITMAP bmp = CreateCompatibleBitmap(hw, rc.right, rc.bottom);
    HGDIOBJ obm = SelectObject(hdc, bmp);
    HBRUSH pg = CreateSolidBrush(th::page);
    FillRect(hdc, &rc, pg);
    DeleteObject(pg);
    SetBkMode(hdc, TRANSPARENT);

    if (g_view.empty()) {
        DrawText_(hdc, 20, 20, L"未加载日志 —— 拖入 dial_*.log,或复制日志文本后按 Ctrl+V",
                  hFontTileLbl, th::inkMuted);
        BitBlt(hw, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, obm); DeleteObject(bmp); DeleteDC(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    // ---- 统计 ----
    const long long t0 = g_view.front().t, t1 = g_view.back().t;
    const double span = (double)(t1 - t0);
    long long total = 0, longest = 0;
    int b[4] = {0,0,0,0};
    for (const auto& o : g_outages) {
        if (!o.recovered) continue;
        total += o.dur;
        if (o.dur > longest) longest = o.dur;
        if (o.dur <= 30) b[0]++; else if (o.dur <= 60) b[1]++; else if (o.dur <= 300) b[2]++; else b[3]++;
    }
    double avail = span > 0 ? 100.0 * (1.0 - total / span) : 0.0;
    long long csqSum = 0; int csqN = 0, csqMin = 9999, csqMax = -1;
    for (const auto& m : g_metrics)
        if (m.csqVal >= 0) { csqSum += m.csqVal; csqN++;
                             csqMin = std::min(csqMin, m.csqVal); csqMax = std::max(csqMax, m.csqVal); }

    // ---- hero:可用率(每视图仅此一个大数字) ----
    // 状态色须配文字标签,不能只靠颜色表意 —— 故旁边永远写着"可用率"
    COLORREF heroC = avail >= 99.9 ? th::good : (avail >= 99.0 ? th::warning : th::critical);
    const wchar_t* heroTag = avail >= 99.9 ? L"良好" : (avail >= 99.0 ? L"偏低" : L"差");
    DrawText_(hdc, 20, 14, L"可用率", hFontTileLbl, th::inkSec);
    std::wstring hv = FmtW(L"%.3f%%", avail);
    DrawText_(hdc, 20, 30, hv, hFontHero, th::inkPri);       // 大数字用主 ink,不用状态色
    int hx = 20 + TextW_(hdc, hv, hFontHero) + 12;
    DrawBar(hdc, hx, 56, 10, 10, heroC);                      // 色块承载状态,文字在旁
    DrawText_(hdc, hx + 16, 52, heroTag, hFontTileLbl, th::inkSec);
    DrawText_(hdc, 20, 86,
              FmtW(L"%s → %s   ·   %s   ·   %s",
                   U8ToW(fmtTime(t0, "FULL")).c_str(), U8ToW(fmtTime(t1, "HM")).c_str(),
                   U8ToW(fmtDur(t1 - t0)).c_str(), U8ToW(g_plat.name).c_str()),
              hFontTileLbl, th::inkMuted);

    // ---- 指标卡 ----
    int pad = 20, gap = 10, ty = 112, th_ = 78;
    int tw = (rc.right - pad * 2 - gap * 3) / 4;
    if (tw > 60) {
        RECT r1{ pad, ty, pad + tw, ty + th_ };
        DrawTile(hdc, r1, L"断网次数", FmtW(L"%d", (int)g_outages.size()), th::inkPri,
                 FmtW(L"累计 %s", U8ToW(fmtDur(total)).c_str()));
        RECT r2{ r1.right + gap, ty, r1.right + gap + tw, ty + th_ };
        DrawTile(hdc, r2, L"最长单次断网", U8ToW(fmtDur(longest)), th::inkPri, L"");
        RECT r3{ r2.right + gap, ty, r2.right + gap + tw, ty + th_ };
        DrawTile(hdc, r3, L"未识别行", FmtW(L"%d", (int)g_audit.unparsed),
                 g_audit.unparsed ? th::inkPri : th::inkPri,
                 g_audit.unparsed ? FmtW(L"占比 %.2f%% —— 见“未识别行”页", g_audit.unparsedRatio()*100.0)
                                  : L"无遗漏(已全部识别)");
        RECT r4{ r3.right + gap, ty, r3.right + gap + tw, ty + th_ };
        DrawTile(hdc, r4, L"信号 CSQ(最小/均/最大)",
                 csqN ? FmtW(L"%d / %.1f / %d", csqMin, (double)csqSum/csqN, csqMax) : L"—",
                 th::inkPri, csqN ? FmtW(L"%d 个样本", csqN) : L"");
    }

    // ---- 断网时长分布(横条)----
    int by = ty + th_ + 20;
    DrawText_(hdc, pad, by, L"断网时长分布", hFontSect, th::inkPri);
    by += 22;
    const wchar_t* bl[4] = { L"≤30s", L"31-60s", L"1-5m", L">5m" };
    int mx = std::max(1, std::max(std::max(b[0], b[1]), std::max(b[2], b[3])));
    int labW = 56, barX = pad + labW, barMaxW = rc.right - barX - pad - 40;
    for (int i = 0; i < 4; ++i) {
        int y = by + i * 22;
        DrawText_(hdc, pad, y + 1, bl[i], hFontTileLbl, th::inkSec);
        int w = barMaxW * b[i] / mx;
        DrawBar(hdc, barX, y, w, 14, th::s1_blue);            // 单序列 → 不需要图例
        DrawText_(hdc, barX + std::max(w, 2) + 8, y + 1, FmtW(L"%d", b[i]), hFontTileLbl, th::inkSec);
    }

    BitBlt(hw, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, obm); DeleteObject(bmp); DeleteDC(hdc);
    EndPaint(hwnd, &ps);
    return 0;
}

// ============================ 结论页(自绘卡片) ============================
// 把结论从"文本墙"改成卡片:浅底、白卡圆角、左侧严重度色带、留白分层。
// 内容仍来自 g_findings / g_plat / g_audit(逻辑层不动),此处只负责画。
// 支持垂直滚动(内容常超一屏)。绘制手法沿用仪表盘(RoundRect/DrawText_/双缓冲)。
static int g_findScroll = 0;      // 当前滚动偏移(逻辑像素,已过 S())
static int g_findContentH = 0;    // 内容总高(用于滚动范围)

// 自动换行输出一段文字,返回占用高度。用于卡片内的依据/建议(可能很长)。
static int DrawWrapped(HDC hdc, int x, int y, int maxW, const std::wstring& s,
                       HFONT f, COLORREF c) {
    HGDIOBJ of = SelectObject(hdc, f);
    SetTextColor(hdc, c);
    RECT r{ x, y, x + maxW, y + 10000 };
    DrawTextW(hdc, s.c_str(), (int)s.size(), &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
    int h = r.bottom - r.top;
    r.right = x + maxW;
    DrawTextW(hdc, s.c_str(), (int)s.size(), &r, DT_LEFT | DT_TOP | DT_WORDBREAK);
    SelectObject(hdc, of);
    return h;
}

static LRESULT CALLBACK FindingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;

    if (msg == WM_VSCROLL) {
        RECT rc; GetClientRect(hwnd, &rc);
        int page = rc.bottom;
        int maxScroll = std::max(0, g_findContentH - page);
        int old = g_findScroll;
        int line = S(40);
        switch (LOWORD(wp)) {
            case SB_LINEUP:   g_findScroll -= line; break;
            case SB_LINEDOWN: g_findScroll += line; break;
            case SB_PAGEUP:   g_findScroll -= page; break;
            case SB_PAGEDOWN: g_findScroll += page; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: g_findScroll = HIWORD(wp); break;
        }
        g_findScroll = std::max(0, std::min(g_findScroll, maxScroll));
        if (g_findScroll != old) {
            SetScrollPos(hwnd, SB_VERT, g_findScroll, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        SendMessageW(hwnd, WM_VSCROLL, MAKEWPARAM(delta > 0 ? SB_LINEUP : SB_LINEDOWN, 0), 0);
        SendMessageW(hwnd, WM_VSCROLL, MAKEWPARAM(delta > 0 ? SB_LINEUP : SB_LINEDOWN, 0), 0);
        return 0;
    }
    if (msg == WM_SIZE) {
        // 窗口尺寸变了(如 Layout 把结论页从初始 100×100 拉到全宽)必须整窗重绘,
        // 否则卡片宽度停留在旧尺寸 —— 表现为"卡片只占左半、右侧残留空框"。
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    if (msg != WM_PAINT) return DefWindowProcW(hwnd, msg, wp, lp);

    PAINTSTRUCT ps; HDC hw = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    // 双缓冲
    HDC hdc = CreateCompatibleDC(hw);
    HBITMAP bmp = CreateCompatibleBitmap(hw, rc.right, rc.bottom);
    HGDIOBJ obm = SelectObject(hdc, bmp);
    // 页面浅底
    HBRUSH pageBg = CreateSolidBrush(th::page);
    FillRect(hdc, &rc, pageBg);
    DeleteObject(pageBg);

    const int M = S(16);              // 页边距
    const int CARD_PAD = S(14);       // 卡内边距
    const int GAP = S(12);            // 卡间距
    const int BAND = S(4);            // 左侧严重度色带宽
    int cardW = rc.right - 2 * M;
    int textX0 = M + CARD_PAD + BAND;
    int textW = cardW - 2 * CARD_PAD - BAND;
    int y = M - g_findScroll;         // 应用滚动偏移

    auto drawCard = [&](int topY, int height, COLORREF band) {
        RECT cr{ M, topY, M + cardW, topY + height };
        HBRUSH bg = CreateSolidBrush(th::surface);
        FillRect(hdc, &cr, bg); DeleteObject(bg);
        HPEN pn = CreatePen(PS_SOLID, 1, th::border);
        HGDIOBJ op = SelectObject(hdc, pn), ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, cr.left, cr.top, cr.right, cr.bottom, S(8), S(8));
        SelectObject(hdc, ob); SelectObject(hdc, op); DeleteObject(pn);
        if (band) {   // 左侧色带
            RECT b{ M + S(1), topY + S(2), M + S(1) + BAND, topY + height - S(2) };
            HBRUSH bb = CreateSolidBrush(band); FillRect(hdc, &b, bb); DeleteObject(bb);
        }
    };

    // ── 头部元信息卡 ──
    {
        // 先量高度:平台 + 覆盖 +(可能)跳变
        int lines = 2;                        // 平台 / 覆盖
        if (g_plat.evidenceLine) lines++;
        bool jump = g_audit.clockJump;
        int h = CARD_PAD * 2 + lines * S(20) + (jump ? S(40) : 0);
        drawCard(y, h, th::inkMuted);
        int ty = y + CARD_PAD;
        DrawText_(hdc, textX0, ty, L"来源平台:  " + U8ToW(g_plat.name), hFontUI, th::inkPri); ty += S(20);
        DrawText_(hdc, textX0, ty,
                  FmtW(L"解析覆盖:  已解析 %d 行,未识别 %d 行(%.2f%%)%s",
                       (int)g_audit.parsed, (int)g_audit.unparsed, g_audit.unparsedRatio()*100.0,
                       g_audit.unparsed==0 ? L"  → 无遗漏" : L"  → 见“未识别行”页"),
                  hFontUI, g_audit.unparsed==0 ? th::inkSec : th::rowWarn); ty += S(20);
        if (g_plat.evidenceLine) {
            DrawText_(hdc, textX0, ty, FmtW(L"识别依据:  第 %d 行  ", (int)g_plat.evidenceLine) + U8ToW(g_plat.evidence),
                      hFontUI, th::inkMuted); ty += S(20);
        }
        if (jump) {
            DrawText_(hdc, textX0, ty, FmtW(L"⚠ 时钟跳变: 第 %d 行 %s → %s", (int)g_audit.jumpAtLine,
                      U8ToW(fmtTime(g_audit.jumpFromT,"FULL")).c_str(), U8ToW(fmtTime(g_audit.jumpToT,"FULL")).c_str()),
                      hFontUI, th::rowFault); ty += S(20);
            DrawText_(hdc, textX0, ty, L"   跨跳变点的断网时长/可用率不可信,请分段看", hFontUI, th::inkMuted);
        }
        y += h + GAP;
    }

    if (g_findings.empty()) {
        int h = CARD_PAD * 2 + S(44);
        drawCard(y, h, th::inkMuted);
        DrawText_(hdc, textX0, y + CARD_PAD, L"未得出任何有证据支撑的结论。", hFontSect, th::inkPri);
        DrawText_(hdc, textX0, y + CARD_PAD + S(22), L"(不等于“没问题”:也可能证据不足。本工具不臆测。)", hFontUI, th::inkMuted);
        y += h + GAP;
    }

    // ── 每条结论一张卡(先测高度,再画白底,最后画字)──
    int n = 0;
    for (const auto& f : g_findings) {
        COLORREF band = (f.severity == 2) ? th::rowFault : (f.severity == 1 ? th::rowWarn : th::rowState);
        const wchar_t* lv = (f.severity == 2) ? L"严重" : (f.severity == 1 ? L"告警" : L"信息");
        std::wstring title = FmtW(L"%d. 【%s】", ++n, lv) + U8ToW(f.title);
        std::wstring detail = U8ToW(f.detail), advice = U8ToW(f.advice);

        // —— 测量 pass:算这张卡多高(不画,只用 DT_CALCRECT)——
        auto measureWrap = [&](const std::wstring& s, HFONT font) {
            HGDIOBJ of = SelectObject(hdc, font);
            RECT r{ 0, 0, textW, 10000 };
            DrawTextW(hdc, s.c_str(), (int)s.size(), &r, DT_LEFT|DT_TOP|DT_WORDBREAK|DT_CALCRECT);
            SelectObject(hdc, of);
            return (int)(r.bottom - r.top);
        };
        int titleH = S(22), lblH = S(18), evH = S(18);
        int h = CARD_PAD;                       // 顶内边距
        h += titleH + S(4);                     // 标题
        h += lblH + measureWrap(detail, hFontUI) + S(6);   // 依据
        h += lblH + measureWrap(advice, hFontUI) + S(6);   // 建议
        h += lblH + (int)f.ev.size() * evH;     // 证据
        h += CARD_PAD;                          // 底内边距

        // —— 画 pass:白底卡 + 色带,再叠字 ——
        drawCard(y, h, band);
        int ty = y + CARD_PAD;
        DrawText_(hdc, textX0, ty, title, hFontSect, band); ty += titleH + S(4);
        DrawText_(hdc, textX0, ty, L"依据", hFontUI, th::inkMuted); ty += lblH;
        ty += DrawWrapped(hdc, textX0, ty, textW, detail, hFontUI, th::inkPri) + S(6);
        DrawText_(hdc, textX0, ty, L"建议", hFontUI, th::inkMuted); ty += lblH;
        ty += DrawWrapped(hdc, textX0, ty, textW, advice, hFontUI, th::inkSec) + S(6);
        DrawText_(hdc, textX0, ty, L"证据", hFontUI, th::inkMuted); ty += lblH;
        for (const auto& e : f.ev) {
            DrawText_(hdc, textX0 + S(8), ty,
                      FmtW(L"· 第 %d 行  ", (int)e.lineNo) + U8ToW(e.ts) + L"  " + U8ToW(e.text),
                      hFontMono, th::inkSec);
            ty += evH;
        }
        y += h + GAP;
    }

    g_findContentH = y + g_findScroll;   // 记录总高(去掉本帧偏移)
    // 更新滚动范围
    SCROLLINFO si{}; si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0; si.nMax = std::max(0, g_findContentH); si.nPage = rc.bottom; si.nPos = g_findScroll;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

    BitBlt(hw, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, obm); DeleteObject(bmp); DeleteDC(hdc);
    EndPaint(hwnd, &ps);
    return 0;
}
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
    HBRUSH bg = CreateSolidBrush(th::surface);
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

    SetTextColor(hdc, th::inkMuted);
    const wchar_t* title = L"CSQ 信号强度(0-31,越高越好;竖红带=断网;黄虚线=弱信号阈值 10)";
    TextOutW(hdc, 42, 4, title, (int)wcslen(title));

    if (g_csq.size() < 2) {
        SetTextColor(hdc, th::inkMuted);
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
    HBRUSH band = CreateSolidBrush(th::outageBand);
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
    HPEN gridPen = CreatePen(PS_SOLID, 1, th::grid);
    HGDIOBJ oldPen = SelectObject(hdc, gridPen);
    SetTextColor(hdc, th::inkMuted);
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
    HPEN axisPen = CreatePen(PS_SOLID, 1, th::axis);
    oldPen = SelectObject(hdc, axisPen);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, r.left, r.top, r.right, r.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(axisPen);

    // 弱信号阈值线(黄虚线)
    HPEN weakPen = CreatePen(PS_DOT, 1, th::warning);
    oldPen = SelectObject(hdc, weakPen);
    MoveToEx(hdc, r.left, Y(10), nullptr);
    LineTo(hdc, r.right, Y(10));
    SelectObject(hdc, oldPen);
    DeleteObject(weakPen);

    // CSQ 折线
    HPEN linePen = CreatePen(PS_SOLID, 2, th::s1_blue);
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
    SetTextColor(hdc, th::inkMuted);
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
    // 用 theme.h 的角色色(CVD 色盲安全、经校验),不再用裸 RGB —— theme.h 硬规矩:代码只写角色。
    //
    // 着色的语义识别比断网引擎更宽:除 isFaultStart/isRecovered(喂断网统计,措辞严格,
    // 不能动)外,这里额外认 open_dial/SDK 那套措辞 —— 真机(open_dial 1.27.3)的故障/恢复
    // 主线是 "Network Recovered in SDK phase"、"Snapshot ..._fault_/_recovery_"、[RECOVERY*],
    // 只按 tag 上色会把它们漏成黑色。**只影响颜色,不影响任何统计**。
    const std::string& m = l.msg;
    auto has = [&](const char* k){ return m.find(k) != std::string::npos; };

    // 恢复(绿):断网引擎认的 + SDK 相 + recovery 快照 + RECOVERY 动作 tag
    if (isRecovered(m, nullptr) || has("Network Recovered") || has("_recovery_") ||
        l.tag.compare(0, 8, "RECOVERY") == 0)
        return th::rowRecovered;
    // 故障(红):断网引擎认的 + fault 快照 + 硬告警
    if (isFaultStart(m) || has("_fault_") || has("Net Fail Duration"))
        return th::rowFault;

    std::string t = l.tag;
    if (t == "ERROR" || t == "FATAL" || t == "CFUN") return th::rowErr;
    if (t == "WARN" || t == "WARNING" || t == "ALARM" || t == "SLOT" || t == "OPER") return th::rowWarn;
    if (t == "ROAMLINK") return th::rowRoamlink;
    if (t == "STATE")    return th::rowState;
    if (t == "SDK")      return th::rowSdk;
    return th::inkPri;
}

static void RenderSummary() {
    std::wstring o;
    if (g_view.empty()) { SetWindowTextW(hSummary, L"筛选后没有可解析的日志行。"); return; }

    // 跨度/平台/可用率已在仪表盘,这里从"明细"起头(t0/t1 与 span 随之不再需要)
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
    // 断网次数/累计/可用率/分布 已在上方仪表盘,这里不重复,只补仪表盘没有的
    if (!g_outages.empty()) {
        o += L"\r\n── 断网 ──\r\n";
        if (longest > 0)
            o += FmtW(L"  最长单次 %s 发生在 %s\r\n", U8ToW(fmtDur(longest)).c_str(),
                      U8ToW(fmtTime(longestAt, "MD")).c_str());
        if (!g_outages.back().recovered)
            o += FmtW(L"  ⚠ 日志结束时仍处于断网(未见恢复),始于 %s\r\n",
                      U8ToW(fmtTime(g_outages.back().start, "MD")).c_str());
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
    // CSQ 三值已在仪表盘卡片上,这里只留卡片放不下的弱信号统计
    if (csqN && weak) {
        o += L"\r\n── 信号 CSQ ──\r\n";
        o += FmtW(L"  弱信号(<10) : %d 次 / 共 %d 样本  首次 %s\r\n",
                  weak, csqN, U8ToW(fmtTime(weakFirst, "MD")).c_str());
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
            g_ogColors.push_back(o.dur > 60 ? th::critical : (o.dur > 30 ? th::rowWarn : th::inkPri));
        } else {
            LvSet(hOutage, row, 2, L"未恢复");
            LvSet(hOutage, row, 3, L"?");
            g_ogColors.push_back(th::critical);
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

// “结论”页现由 FindingsProc 自绘卡片,数据直接读 g_findings/g_plat/g_audit。
// 此函数只需在数据更新后重置滚动并触发重绘。
static void RenderFindings() {
    g_findScroll = 0;
    if (hFindings) {
        SetScrollPos(hFindings, SB_VERT, 0, TRUE);
        InvalidateRect(hFindings, nullptr, FALSE);
    }
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
        { &hDash, 0 }, { &hSummary, 0 }, { &hFindings, 1 }, { &hTimeline, 2 }, { &hOutage, 3 },
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
    InvalidateRect(hDash, nullptr, TRUE);
}

// 载入的公共尾段:拿到原始行之后的处理,文件与剪贴板共用
static void LoadRawLines(const std::vector<std::string>& raw, const std::wstring& srcLabel,
                         const std::vector<size_t>& fileBoundaries = {}) {
    parseLines(raw, g_all, g_sessions, &g_audit, fileBoundaries);
    g_plat = detectPlatform(g_all);   // 平台识别用全量行(不受筛选影响)
    std::wstring lbl = srcLabel;
    lbl += FmtW(L"   (%d 行, %d 会话, %s)", (int)g_all.size(), (int)g_sessions.size(),
                U8ToW(g_plat.name).c_str());
    SetWindowTextW(hFileLbl, lbl.c_str());
    RefreshAll();
}

static void LoadFiles(const std::vector<std::wstring>& paths) {
    // 逐份读入 → 跨时基混合防护 → 按首时间戳定序 → 拼接。
    // 拖入顺序(资源管理器多选)与文件对话框返回顺序都不保证按时间,而 parseLines 不排序,
    // 顺序拼接会让时间线/断网/可用率全错(v1.3.0 及之前的行为)。定序与时基判定逻辑均在
    // logmodel(orderByTime / detectMix),此处只做 I/O、排除、拼接、提示。
    std::vector<std::vector<std::string>> chunks;
    std::vector<std::wstring> ok;
    for (const auto& p : paths) {
        // ReadPathExpand:普通文件 → 1 个 chunk;压缩包 → 包内每个文件各 1 个 chunk。
        // 展开后的 chunk 与手工解压后多选拖入完全等价,照常走定序 / 时基混合防护。
        std::vector<std::vector<std::string>> sub;
        std::vector<std::wstring> subLabels;
        if (ReadPathExpand(p, sub, subLabels)) {
            for (size_t i = 0; i < sub.size(); ++i) {
                chunks.push_back(std::move(sub[i]));
                ok.push_back(subLabels[i]);
            }
        } else {
            MessageBoxW(hMain, (L"读取失败:\n" + p).c_str(), L"错误", MB_ICONERROR);
        }
    }
    if (ok.empty()) return;

    // 跨时基混合防护(方案A):同时含墙钟与"时钟未同步"日志时,未同步批与墙钟批不在同一
    // 时间坐标系,混合拼接会把跨度撑成几十年、可用率从"差"翻成"良好"。此处排除未同步批,
    // 只用墙钟批出结论,并明确列出被排除的文件让用户知情。未同步批未销毁,可单独再拖入分析。
    MixReport mix = detectMix(chunks);
    std::wstring excludedNote;
    if (mix.mixed) {
        std::wstring names;
        for (size_t i : mix.unsyncedIdx) {
            // 只取文件名,不带路径,提示更短
            const std::wstring& full = ok[i];
            size_t slash = full.find_last_of(L"\\/");
            names += L"\n  · " + (slash == std::wstring::npos ? full : full.substr(slash + 1));
        }
        std::wstring msg = FmtW(L"检测到 %d 份日志时钟未同步(时间戳落在 1970 年),\n"
                               L"与其余 %d 份墙钟日志不在同一时间坐标系。\n\n"
                               L"已从本次合并中排除以下未同步日志,仅用墙钟日志出结论:%s\n\n"
                               L"如需查看未同步日志,请单独拖入分析。",
                               (int)mix.unsyncedIdx.size(), (int)mix.wallIdx.size(), names.c_str());
        MessageBoxW(hMain, msg.c_str(), L"时钟未同步日志已排除", MB_ICONWARNING | MB_OK);

        // 用墙钟批重建 chunks / ok,后续定序拼接只在墙钟批内进行
        std::vector<std::vector<std::string>> keptChunks;
        std::vector<std::wstring> keptOk;
        for (size_t i : mix.wallIdx) { keptChunks.push_back(std::move(chunks[i])); keptOk.push_back(ok[i]); }
        chunks.swap(keptChunks);
        ok.swap(keptOk);
        excludedNote = FmtW(L",已排除 %d 份未同步日志", (int)mix.unsyncedIdx.size());
    }
    if (ok.empty()) return;   // 理论上不会:mixed 时 wallIdx 必非空,防御性保留

    std::vector<size_t> ord = orderByTime(chunks);

    bool reordered = false;
    int noTs = 0;
    for (size_t i = 0; i < ord.size(); ++i) {
        if (ord[i] != i) reordered = true;
        if (!firstTimestamp(chunks[ord[i]], nullptr)) noTs++;
    }

    std::vector<std::string> raw;
    std::vector<size_t> fileBoundaries;   // 各文件在 raw 中的起始下标 → 阻止跨文件续行
    for (size_t i : ord) {
        fileBoundaries.push_back(raw.size());   // 本文件从这里开始
        raw.insert(raw.end(), chunks[i].begin(), chunks[i].end());
    }

    std::wstring lbl;
    if (ok.size() == 1) {
        lbl = ok[0] + excludedNote;   // 排除后只剩一份时,仍要带上排除提示
    } else {
        // 多文件必须让人看见到底按什么顺序拼的 —— 否则重排是隐形的,出了错也无从察觉
        lbl = FmtW(L"%d 个文件合并", (int)ok.size());
        lbl += reordered ? L"(已按时间重排)" : L"(拖入顺序已是时间顺序)";
        if (noTs > 0) lbl += FmtW(L",其中 %d 份扫不到时间戳→拼在最后", noTs);
        lbl += excludedNote;
    }
    LoadRawLines(raw, lbl, fileBoundaries);
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
    ofn.lpstrFilter = L"日志与压缩包 (*.log;*.txt;*.zip;*.gz)\0*.log;*.txt;*.zip;*.gz;*.tar.gz\0所有文件 (*.*)\0*.*\0\0";
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

    int tabTop = TOP_H();
    int tabH = rc.bottom - tabTop - statusH;
    if (tabH < 40) tabH = 40;
    MoveWindow(hTab, 0, tabTop, rc.right, tabH, TRUE);

    RECT d{ 0, tabTop, rc.right, tabTop + tabH };
    TabCtrl_AdjustRect(hTab, FALSE, &d);

    // 总览页:上=仪表盘(固定高),下=详情文字
    const int DASH_H = S(336);
    int dh = std::min<int>(DASH_H, (int)(d.bottom - d.top) - S(60));
    if (dh < S(80)) dh = S(80);
    MoveWindow(hDash,    d.left, d.top, d.right - d.left, dh, TRUE);
    MoveWindow(hSummary, d.left, d.top + dh, d.right - d.left, (d.bottom - d.top) - dh, TRUE);
    MoveWindow(hFindings, d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(hTimeline, d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(hOutage,   d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(hTags,     d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(hRaw,      d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);
    MoveWindow(hUnparsed, d.left, d.top, d.right - d.left, d.bottom - d.top, TRUE);

    // 指标页:图表(上) + 表格(中) + 导出按钮(下)
    int ch = std::min<int>(CHART_H(), (int)(d.bottom - d.top) / 2);   // RECT 成员是 LONG,显式定型
    MoveWindow(hChart,  d.left, d.top, d.right - d.left, ch, TRUE);
    int btnH = S(28);
    int gridTop = d.top + ch;
    int gridH = (d.bottom - d.top) - ch - btnH;
    if (gridH < S(30)) gridH = S(30);
    MoveWindow(hMetric, d.left, gridTop, d.right - d.left, gridH, TRUE);
    MoveWindow(hExport, d.left, gridTop + gridH, S(160), btnH, TRUE);

    // 顶部文件名标签跟随宽度
    MoveWindow(hFileLbl, S(196), S(10), std::max(S(200), (int)rc.right - S(208)), S(20), TRUE);  // 让开“粘贴日志”按钮
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
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);  // 不要网格线:老派且抢视觉
    SendMessageW(lv, WM_SETFONT, (WPARAM)hFontMono, TRUE);
    int i = 0;
    for (auto& c : cols) LvAddCol(lv, i++, c.first, c.second);
    return lv;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        hMain = hwnd;
        // DPI 初始化(必须在建控件/字体前):PerMonitorV2 下 GetDpiForWindow 给出本窗口所在
        // 显示器的 DPI。动态取函数指针,老系统(无此 API)回退 96。
        {
            HMODULE u32 = GetModuleHandleW(L"user32.dll");
            typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
            auto pGetDpiForWindow = u32 ? reinterpret_cast<GetDpiForWindow_t>(
                reinterpret_cast<void*>(GetProcAddress(u32, "GetDpiForWindow"))) : nullptr;
            if (pGetDpiForWindow) {
                UINT d = pGetDpiForWindow(hwnd);
                if (d >= 72 && d <= 480) g_dpi = (int)d;   // 合理范围保护
            }
        }
        // 字体
        hFontUI = CreateFontW(SF(-12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        hFontMono = CreateFontW(SF(-12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");
        // 仪表盘字体。hero ≥48px;大数字用比例数字(非等宽),等宽只留给要对齐的列
        auto mkf = [](int h, int w) {
            return CreateFontW(SF(h), 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               L"Microsoft YaHei UI");
        };
        hFontHero    = mkf(-48, FW_SEMIBOLD);
        hFontTileVal = mkf(-22, FW_SEMIBOLD);
        hFontTileLbl = mkf(-12, FW_NORMAL);
        hFontSect    = mkf(-14, FW_SEMIBOLD);

        // 顶部工具栏(逻辑像素,S() 换算到物理像素)
        Mk(L"BUTTON", L"打开日志…", BS_PUSHBUTTON, S(8), S(6), S(96), S(26), IDC_OPEN, hFontUI);
        Mk(L"BUTTON", L"粘贴日志", BS_PUSHBUTTON, S(108), S(6), S(80), S(26), IDC_PASTE, hFontUI);
        hFileLbl = Mk(L"STATIC", L"未加载 —— 拖入 dial_*.log(可多选),或复制日志文本后按 Ctrl+V / 点“粘贴日志”",
                      SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, S(196), S(10), S(820), S(20), IDC_FILELBL, hFontUI);

        int y = 44;
        Mk(L"STATIC", L"标签:", SS_LEFT, S(8), S(y + 4), S(36), S(18), 0, hFontUI);
        hTagBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, S(46), S(y), S(140), S(22), IDC_TAGBOX, hFontUI);
        Mk(L"STATIC", L"筛选(正则):", SS_LEFT, S(196), S(y + 4), S(74), S(18), 0, hFontUI);
        hGrepBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, S(272), S(y), S(210), S(22), IDC_GREPBOX, hFontUI);
        Mk(L"STATIC", L"起:", SS_LEFT, S(492), S(y + 4), S(22), S(18), 0, hFontUI);
        hSinceBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, S(516), S(y), S(84), S(22), IDC_SINCEBOX, hFontUI);
        Mk(L"STATIC", L"止:", SS_LEFT, S(608), S(y + 4), S(22), S(18), 0, hFontUI);
        hUntilBox = Mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, S(632), S(y), S(84), S(22), IDC_UNTILBOX, hFontUI);
        Mk(L"BUTTON", L"应用筛选", BS_PUSHBUTTON, S(728), S(y - 2), S(84), S(26), IDC_APPLY, hFontUI);
        Mk(L"BUTTON", L"清空", BS_PUSHBUTTON, S(818), S(y - 2), S(60), S(26), IDC_CLEAR, hFontUI);

        // 页签
        hTab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, TOP_H(), 100, 100, hwnd, (HMENU)(INT_PTR)IDC_TAB,
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
        hDash = CreateWindowExW(0, L"dialDashCls", L"", WS_CHILD,
                                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_DASH,
                                GetModuleHandleW(nullptr), nullptr);
        hSummary = Mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY,
                      0, 0, 10, 10, IDC_SUMMARY, hFontMono);
        hFindings = CreateWindowExW(0, L"dialFindingsCls", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                                    0, 0, 100, 100, hwnd, (HMENU)(INT_PTR)IDC_FINDINGS, GetModuleHandleW(nullptr), nullptr);
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

    // 窗口被拖到不同 DPI 的显示器(或系统缩放变更):更新 g_dpi,重建字体(字体尺寸是
    // 创建时固定的,不随 DPI 自动变),按系统建议的新矩形调整窗口,再重排。
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);   // 新 DPI(x,y 相同)
        // 重建所有字体
        auto mkf2 = [](int h, int w, const wchar_t* face, DWORD pitch) {
            return CreateFontW(SF(h), 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, pitch, face);
        };
        HFONT oUI = hFontUI, oMono = hFontMono, oHero = hFontHero,
              oTV = hFontTileVal, oTL = hFontTileLbl, oSect = hFontSect;
        hFontUI      = mkf2(-12, FW_NORMAL,  L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        hFontMono    = mkf2(-12, FW_NORMAL,  L"Consolas",           FIXED_PITCH | FF_MODERN);
        hFontHero    = mkf2(-48, FW_SEMIBOLD,L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        hFontTileVal = mkf2(-22, FW_SEMIBOLD,L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        hFontTileLbl = mkf2(-12, FW_NORMAL,  L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        hFontSect    = mkf2(-14, FW_SEMIBOLD,L"Microsoft YaHei UI", DEFAULT_PITCH | FF_DONTCARE);
        // 把新字体铺给顶部控件(子控件字体逐个重设)
        for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            SendMessageW(c, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        SendMessageW(hTab, WM_SETFONT, (WPARAM)hFontUI, TRUE);
        // 删旧字体
        for (HFONT f : { oUI, oMono, oHero, oTV, oTL, oSect }) if (f) DeleteObject(f);
        // 按系统建议矩形调整窗口(会触发 WM_SIZE → Layout)
        RECT* nr = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, nr->left, nr->top,
                     nr->right - nr->left, nr->bottom - nr->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = S(920);
        mmi->ptMinTrackSize.y = S(520);
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
        if (hFontHero) DeleteObject(hFontHero);
        if (hFontTileVal) DeleteObject(hFontTileVal);
        if (hFontTileLbl) DeleteObject(hFontTileLbl);
        if (hFontSect) DeleteObject(hFontSect);
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

    WNDCLASSEXW wcDash{};
    wcDash.cbSize = sizeof(wcDash);
    wcDash.lpfnWndProc = DashProc;
    wcDash.hInstance = hInst;
    wcDash.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcDash.lpszClassName = L"dialDashCls";
    RegisterClassExW(&wcDash);

    WNDCLASSEXW wcFind{};
    wcFind.cbSize = sizeof(wcFind);
    wcFind.lpfnWndProc = FindingsProc;
    wcFind.hInstance = hInst;
    wcFind.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcFind.lpszClassName = L"dialFindingsCls";
    RegisterClassExW(&wcFind);

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
    // 自定义应用图标(resource.rc 中 IDI_APPICON=101):任务栏/Alt-Tab/标题栏都用它。
    // LoadImage 按需求尺寸取 ico 内最匹配的一档(大图标取 32,小图标取 16)。
    HICON hIconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                       0, 0, LR_DEFAULTSIZE | LR_SHARED);
    HICON hIconSm  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                       LR_SHARED);
    wc.hIcon   = hIconBig ? hIconBig : LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = hIconSm  ? hIconSm  : LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // 标题带版本:用户发来的截图能直接看出是哪个 build
    HWND w = CreateWindowExW(WS_EX_ACCEPTFILES, L"dialLogMainCls",
                             DL_APP_NAME_W L" v" DL_VER_WSTR L" — 拨号日志分析",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760,
                             nullptr, nullptr, hInst, nullptr);
    if (!w) return 1;
    // 初始尺寸 1180×760 是 96 DPI 逻辑值。WM_CREATE 已把 g_dpi 设为本窗口 DPI,
    // 若非 96 则按比例放大外框(否则高分屏上窗口偏小、装不下放大后的内容)。
    if (g_dpi != 96) {
        RECT wr; GetWindowRect(w, &wr);
        SetWindowPos(w, nullptr, 0, 0,
                     MulDiv(1180, g_dpi, 96), MulDiv(760, g_dpi, 96),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
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
