// overview_page.cpp — 总览仪表盘、摘要卡片与证据结论页
#include "overview_page.h"

#include <commctrl.h>

#include <algorithm>
#include <climits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "app_context.h"
#include "log_analysis.h"
#include "log_time.h"
#include "memoryutil.h"
#include "theme.h"
#include "win_text.h"

namespace dl {

struct SumCard {
    std::wstring title;              // 卡标题(如"断网""报错/告警")
    std::vector<std::wstring> lines; // 卡内每行文本
    int accent = 0;                  // 0=中性 1=告警(黄) 2=严重(红)
    bool mono = false;               // 内容是否等宽(报错/证据类用等宽对齐)
};
static std::vector<SumCard> g_sumCards;

// ============================ 仪表盘(总览页顶部,自绘) ============================
// 设计约束(照做):hero 数字每视图只允许一个(=可用率);文字一律 ink 系,绝不用数据色;
// 条形 ≤24px 厚、数据端 4px 圆角而基线端方角;网格/轴发丝实线、退让。

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
    DrawText_(hdc, r.left + 12, r.top + 8,  label, App().hFontTileLbl, th::inkSec);
    DrawText_(hdc, r.left + 12, r.top + 26, val,   App().hFontTileVal, valColor);
    if (!note.empty())
        DrawText_(hdc, r.left + 12, r.top + 56, note, App().hFontTileLbl, th::inkMuted);
}

LRESULT CALLBACK DashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_SIZE) { InvalidateRect(hwnd, nullptr, TRUE); return 0; }
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

    if (App().document.filtered.empty()) {
        DrawText_(hdc, 20, 20, L"未加载日志 —— 拖入 dial_*.log,或复制日志文本后按 Ctrl+V",
                  App().hFontTileLbl, th::inkMuted);
        BitBlt(hw, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, obm); DeleteObject(bmp); DeleteDC(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    // ---- 统计 ----
    const long long t0 = App().document.filtered.front()->t, t1 = App().document.filtered.back()->t;
    const double span = (double)(t1 - t0);
    long long total = 0, longest = 0;
    int b[4] = {0,0,0,0};
    for (const auto& o : App().document.outages) {
        if (!o.recovered) continue;
        total += o.dur;
        if (o.dur > longest) longest = o.dur;
        if (o.dur <= 30) b[0]++; else if (o.dur <= 60) b[1]++; else if (o.dur <= 300) b[2]++; else b[3]++;
    }
    double avail = span > 0 ? 100.0 * (1.0 - total / span) : 0.0;
    long long csqSum = 0; int csqN = 0, csqMin = 9999, csqMax = -1;
    for (const auto& m : App().document.metrics)
        if (m.csqVal >= 0) { csqSum += m.csqVal; csqN++;
                             csqMin = std::min(csqMin, m.csqVal); csqMax = std::max(csqMax, m.csqVal); }

    // ---- hero:可用率(每视图仅此一个大数字) ----
    // 状态色须配文字标签,不能只靠颜色表意 —— 故旁边永远写着"可用率"
    COLORREF heroC = avail >= 99.9 ? th::good : (avail >= 99.0 ? th::warning : th::critical);
    const wchar_t* heroTag = avail >= 99.9 ? L"良好" : (avail >= 99.0 ? L"偏低" : L"差");
    DrawText_(hdc, 20, 14, L"可用率", App().hFontTileLbl, th::inkSec);
    std::wstring hv = FmtW(L"%.3f%%", avail);
    DrawText_(hdc, 20, 30, hv, App().hFontHero, th::inkPri);       // 大数字用主 ink,不用状态色
    int hx = 20 + TextW_(hdc, hv, App().hFontHero) + 12;
    DrawBar(hdc, hx, 56, 10, 10, heroC);                      // 色块承载状态,文字在旁
    DrawText_(hdc, hx + 16, 52, heroTag, App().hFontTileLbl, th::inkSec);
    DrawText_(hdc, 20, 86,
              FmtW(L"%s → %s   ·   %s   ·   %s",
                   U8ToW(fmtTime(t0, "FULL")).c_str(), U8ToW(fmtTime(t1, "HM")).c_str(),
                   U8ToW(fmtDur(t1 - t0)).c_str(), U8ToW(App().document.platform.name).c_str()),
              App().hFontTileLbl, th::inkMuted);

    // ---- 指标卡 ----
    int pad = 20, gap = 10, ty = 112, th_ = 78;
    int tw = (rc.right - pad * 2 - gap * 3) / 4;
    if (tw > 60) {
        RECT r1{ pad, ty, pad + tw, ty + th_ };
        DrawTile(hdc, r1, L"断网次数", FmtW(L"%d", (int)App().document.outages.size()), th::inkPri,
                 FmtW(L"累计 %s", U8ToW(fmtDur(total)).c_str()));
        RECT r2{ r1.right + gap, ty, r1.right + gap + tw, ty + th_ };
        DrawTile(hdc, r2, L"最长单次断网", U8ToW(fmtDur(longest)), th::inkPri, L"");
        RECT r3{ r2.right + gap, ty, r2.right + gap + tw, ty + th_ };
        DrawTile(hdc, r3, L"未识别行", FmtW(L"%d", (int)App().document.audit.unparsed),
                 App().document.audit.unparsed ? th::inkPri : th::inkPri,
                 App().document.audit.unparsed ? FmtW(L"占比 %.2f%% —— 见“未识别行”页", App().document.audit.unparsedRatio()*100.0)
                                  : L"无遗漏(已全部识别)");
        RECT r4{ r3.right + gap, ty, r3.right + gap + tw, ty + th_ };
        DrawTile(hdc, r4, L"信号 CSQ(最小/均/最大)",
                 csqN ? FmtW(L"%d / %.1f / %d", csqMin, (double)csqSum/csqN, csqMax) : L"—",
                 th::inkPri, csqN ? FmtW(L"%d 个样本", csqN) : L"");
    }

    // ---- 断网时长分布(横条)----
    int by = ty + th_ + 20;
    DrawText_(hdc, pad, by, L"断网时长分布", App().hFontSect, th::inkPri);
    by += 22;
    const wchar_t* bl[4] = { L"≤30s", L"31-60s", L"1-5m", L">5m" };
    int mx = std::max(1, std::max(std::max(b[0], b[1]), std::max(b[2], b[3])));
    int labW = 56, barX = pad + labW, barMaxW = rc.right - barX - pad - 40;
    for (int i = 0; i < 4; ++i) {
        int y = by + i * 22;
        DrawText_(hdc, pad, y + 1, bl[i], App().hFontTileLbl, th::inkSec);
        int w = barMaxW * b[i] / mx;
        DrawBar(hdc, barX, y, w, 14, th::s1_blue);            // 单序列 → 不需要图例
        DrawText_(hdc, barX + std::max(w, 2) + 8, y + 1, FmtW(L"%d", b[i]), App().hFontTileLbl, th::inkSec);
    }

    BitBlt(hw, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, obm); DeleteObject(bmp); DeleteDC(hdc);
    EndPaint(hwnd, &ps);
    return 0;
}

// ============================ 结论页(自绘卡片) ============================
// 把结论从"文本墙"改成卡片:浅底、白卡圆角、左侧严重度色带、留白分层。
// 内容仍来自 App().document.findings / App().document.platform / App().document.audit(逻辑层不动),此处只负责画。
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

LRESULT CALLBACK FindingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
    const int CARD_PAD = S(18);       // 卡内边距
    const int GAP = S(14);            // 卡间距
    const int BAND = S(5);            // 左侧严重度色带宽
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
        if (App().document.platform.evidenceLine) lines++;
        bool jump = App().document.audit.clockJump;
        int h = CARD_PAD * 2 + lines * S(20) + (jump ? S(40) : 0);
        drawCard(y, h, th::inkMuted);
        int ty = y + CARD_PAD;
        DrawText_(hdc, textX0, ty, L"来源平台:  " + U8ToW(App().document.platform.name), App().hFontUI, th::inkPri); ty += S(20);
        DrawText_(hdc, textX0, ty,
                  FmtW(L"解析覆盖:  已解析 %d 行,未识别 %d 行(%.2f%%)%s",
                       (int)App().document.audit.parsed, (int)App().document.audit.unparsed, App().document.audit.unparsedRatio()*100.0,
                       App().document.audit.unparsed==0 ? L"  → 无遗漏" : L"  → 见“未识别行”页"),
                  App().hFontUI, App().document.audit.unparsed==0 ? th::inkSec : th::rowWarn); ty += S(20);
        if (App().document.platform.evidenceLine) {
            DrawText_(hdc, textX0, ty, FmtW(L"识别依据:  第 %d 行  ", (int)App().document.platform.evidenceLine) + U8ToW(App().document.platform.evidence),
                      App().hFontUI, th::inkMuted); ty += S(20);
        }
        if (jump) {
            DrawText_(hdc, textX0, ty, FmtW(L"⚠ 时钟跳变: 第 %d 行 %s → %s", (int)App().document.audit.jumpAtLine,
                      U8ToW(fmtTime(App().document.audit.jumpFromT,"FULL")).c_str(), U8ToW(fmtTime(App().document.audit.jumpToT,"FULL")).c_str()),
                      App().hFontUI, th::rowFault); ty += S(20);
            DrawText_(hdc, textX0, ty, L"   跨跳变点的断网时长/可用率不可信,请分段看", App().hFontUI, th::inkMuted);
        }
        y += h + GAP;
    }

    if (App().document.findings.empty()) {
        int h = CARD_PAD * 2 + S(44);
        drawCard(y, h, th::inkMuted);
        DrawText_(hdc, textX0, y + CARD_PAD, L"未得出任何有证据支撑的结论。", App().hFontSect, th::inkPri);
        DrawText_(hdc, textX0, y + CARD_PAD + S(22), L"(不等于“没问题”:也可能证据不足。本工具不臆测。)", App().hFontUI, th::inkMuted);
        y += h + GAP;
    }

    // ── 每条结论一张卡(先测高度,再画白底,最后画字)──
    int n = 0;
    for (const auto& f : App().document.findings) {
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
        int titleH = S(24), lblH = S(20), evH = S(19);
        const int SEC = S(12);
        int h = CARD_PAD;                       // 顶内边距
        h += titleH + S(8);                     // 标题
        h += lblH + measureWrap(detail, App().hFontUI) + SEC;   // 依据
        h += lblH + measureWrap(advice, App().hFontUI) + SEC;   // 建议
        h += lblH + (int)f.ev.size() * evH;     // 证据
        h += CARD_PAD;                          // 底内边距

        // —— 画 pass:白底卡 + 色带,再叠字 ——
        drawCard(y, h, band);
        int ty = y + CARD_PAD;
        DrawText_(hdc, textX0, ty, title, App().hFontSect, band); ty += titleH + S(8);
        DrawText_(hdc, textX0, ty, L"依据", App().hFontUI, th::inkMuted); ty += lblH;
        ty += DrawWrapped(hdc, textX0, ty, textW, detail, App().hFontUI, th::inkPri) + SEC;
        DrawText_(hdc, textX0, ty, L"建议", App().hFontUI, th::inkMuted); ty += lblH;
        ty += DrawWrapped(hdc, textX0, ty, textW, advice, App().hFontUI, th::inkSec) + SEC;
        DrawText_(hdc, textX0, ty, L"证据", App().hFontUI, th::inkMuted); ty += lblH;
        for (const auto& e : f.ev) {
            DrawText_(hdc, textX0 + S(8), ty,
                      FmtW(L"· 第 %d 行  ", (int)e.lineNo) + U8ToW(e.ts) + L"  " + U8ToW(e.text),
                      App().hFontMono, th::inkSec);
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

// ============================ 总览页下半(自绘卡片) ============================
// 把明细区块从"文本墙"改成卡片,与结论页同一套手法。数据来自 g_sumCards
// (RenderSummary 填充,逻辑不变,只是输出改成结构化区块)。
static int g_sumScroll = 0;
static int g_sumContentH = 0;

LRESULT CALLBACK SummaryProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_SIZE) { InvalidateRect(hwnd, nullptr, TRUE); return 0; }
    if (msg == WM_VSCROLL) {
        RECT rc; GetClientRect(hwnd, &rc);
        int page = rc.bottom, maxScroll = std::max(0, g_sumContentH - page), old = g_sumScroll, line = S(40);
        switch (LOWORD(wp)) {
            case SB_LINEUP: g_sumScroll -= line; break;
            case SB_LINEDOWN: g_sumScroll += line; break;
            case SB_PAGEUP: g_sumScroll -= page; break;
            case SB_PAGEDOWN: g_sumScroll += page; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: g_sumScroll = HIWORD(wp); break;
        }
        g_sumScroll = std::max(0, std::min(g_sumScroll, maxScroll));
        if (g_sumScroll != old) { SetScrollPos(hwnd, SB_VERT, g_sumScroll, TRUE); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        SendMessageW(hwnd, WM_VSCROLL, MAKEWPARAM(d > 0 ? SB_LINEUP : SB_LINEDOWN, 0), 0);
        SendMessageW(hwnd, WM_VSCROLL, MAKEWPARAM(d > 0 ? SB_LINEUP : SB_LINEDOWN, 0), 0);
        return 0;
    }
    if (msg != WM_PAINT) return DefWindowProcW(hwnd, msg, wp, lp);

    PAINTSTRUCT ps; HDC hw = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    HDC hdc = CreateCompatibleDC(hw);
    HBITMAP bmp = CreateCompatibleBitmap(hw, rc.right, rc.bottom);
    HGDIOBJ obm = SelectObject(hdc, bmp);
    HBRUSH pageBg = CreateSolidBrush(th::page);
    FillRect(hdc, &rc, pageBg); DeleteObject(pageBg);

    const int M = S(16), CARD_PAD = S(18), GAP = S(14), BAND = S(5);
    int cardW = rc.right - 2 * M;
    int textX0 = M + CARD_PAD + BAND;
    int y = M - g_sumScroll;

    for (const auto& c : g_sumCards) {
        COLORREF band = (c.accent == 2) ? th::rowFault : (c.accent == 1 ? th::rowWarn : th::inkMuted);
        HFONT lineFont = c.mono ? App().hFontMono : App().hFontUI;
        int titleH = S(24), lineH = c.mono ? S(18) : S(20);
        int h = CARD_PAD + titleH + S(6) + (int)c.lines.size() * lineH + CARD_PAD;

        // 画卡
        RECT cr{ M, y, M + cardW, y + h };
        HBRUSH bg = CreateSolidBrush(th::surface); FillRect(hdc, &cr, bg); DeleteObject(bg);
        HPEN pn = CreatePen(PS_SOLID, 1, th::border);
        HGDIOBJ op = SelectObject(hdc, pn), ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, cr.left, cr.top, cr.right, cr.bottom, S(8), S(8));
        SelectObject(hdc, ob); SelectObject(hdc, op); DeleteObject(pn);
        RECT b{ M + S(1), y + S(2), M + S(1) + BAND, y + h - S(2) };
        HBRUSH bb = CreateSolidBrush(band); FillRect(hdc, &b, bb); DeleteObject(bb);

        int ty = y + CARD_PAD;
        DrawText_(hdc, textX0, ty, c.title, App().hFontSect,
                  c.accent == 2 ? th::rowFault : (c.accent == 1 ? th::rowWarn : th::inkPri));
        ty += titleH + S(6);
        for (const auto& ln : c.lines) {
            DrawText_(hdc, textX0, ty, ln, lineFont, th::inkPri);
            ty += lineH;
        }
        y += h + GAP;
    }

    g_sumContentH = y + g_sumScroll;
    SCROLLINFO si{}; si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0; si.nMax = std::max(0, g_sumContentH); si.nPage = rc.bottom; si.nPos = g_sumScroll;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

    BitBlt(hw, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, obm); DeleteObject(bmp); DeleteDC(hdc);
    EndPaint(hwnd, &ps);
    return 0;
}


void RenderSummary() {
    g_sumCards.clear();
    g_sumScroll = 0;
    if (App().hSummary) { SetScrollPos(App().hSummary, SB_VERT, 0, TRUE); }
    if (App().document.filtered.empty()) {
        if (!App().document.lines.empty())      // 未加载日志时不显示卡片(上方已有提示)
            g_sumCards.push_back({ L"明细", { L"筛选后没有可解析的日志行(试试“清空筛选”)。" }, 0, false });
        if (App().hSummary) InvalidateRect(App().hSummary, nullptr, FALSE);
        return;
    }

    auto add = [&](std::wstring title, std::vector<std::wstring> lines, int accent = 0, bool mono = false) {
        g_sumCards.push_back({ std::move(title), std::move(lines), accent, mono });
    };

    // ── 概览 ──
    {
        std::vector<std::wstring> ls;
        ls.push_back(FmtW(L"日志行数 %d    进程会话(重启) %d", (int)App().document.filtered.size(), (int)App().document.sessions.size()));
        int acc = 0;
        if (App().document.sessions.size() > 1) {
            acc = 1;
            ls.push_back(FmtW(L"⚠ 检测到 %d 次进程重启 (L3 exit / watchdog 拉起?)", (int)App().document.sessions.size()));
            for (size_t i = 0; i < App().document.sessions.size() && i < 6; ++i)
                ls.push_back(L"   " + U8ToW(App().document.sessions[i]));
        }
        add(L"概览", ls, acc);
    }

    // 断网
    long long total = 0, longest = 0, longestAt = 0;
    int b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    for (const auto& x : App().document.outages) {
        if (!x.recovered) continue;
        total += x.dur;
        if (x.dur > longest) { longest = x.dur; longestAt = x.end; }
        if (x.dur <= 30) b0++; else if (x.dur <= 60) b1++; else if (x.dur <= 300) b2++; else b3++;
    }
    if (!App().document.outages.empty()) {
        std::vector<std::wstring> ls; int acc = 0;
        if (longest > 0)
            ls.push_back(FmtW(L"最长单次 %s 发生在 %s", U8ToW(fmtDur(longest)).c_str(), U8ToW(fmtTime(longestAt, "MD")).c_str()));
        if (!App().document.outages.back().recovered) {
            acc = 2;
            ls.push_back(FmtW(L"⚠ 日志结束时仍处于断网(未见恢复),始于 %s", U8ToW(fmtTime(App().document.outages.back().start, "MD")).c_str()));
        }
        if (!ls.empty()) add(L"断网", ls, acc);
    }

    // 信号/温度/通道
    long long csqSum = 0; int csqN = 0, csqMin = 9999, csqMax = -1, weak = 0; long long weakFirst = 0;
    long long tSum = 0; int tN = 0, tMax = -9999, hot = 0;
    int rsrpN = 0, rsrpMin = 9999, rsrpMax = -9999; long long rsrpSum = 0;
    int rsrqN = 0, rsrqMin = 9999, rsrqMax = -9999; long long rsrqSum = 0;
    int snrN = 0, snrMin = 100000, snrMax = -100000, snrNonPositive = 0;
    long long snrSum10 = 0;
    std::map<std::string, int> chans;
    std::map<std::string, int> srvs, rats, opers;
    int denyNonzero = 0, denyN = 0;
    std::vector<std::pair<long long,long long>> rxs;
    for (const auto& m : App().document.metrics) {
        if (m.csqVal >= 0) {
            csqSum += m.csqVal; csqN++;
            csqMin = std::min(csqMin, m.csqVal);
            csqMax = std::max(csqMax, m.csqVal);
            if (m.csqVal < 10) { if (weak == 0) weakFirst = m.t; weak++; }
        }
        if (m.rsrp < 0) { rsrpSum += m.rsrp; rsrpN++; rsrpMin = std::min(rsrpMin, m.rsrp); rsrpMax = std::max(rsrpMax, m.rsrp); }
        if (m.rsrq < 0) { rsrqSum += m.rsrq; rsrqN++; rsrqMin = std::min(rsrqMin, m.rsrq); rsrqMax = std::max(rsrqMax, m.rsrq); }
        if (m.snr10 != 100000) {
            snrSum10 += m.snr10; snrN++;
            snrMin = std::min(snrMin, m.snr10); snrMax = std::max(snrMax, m.snr10);
            if (m.snr10 <= 0) snrNonPositive++;
        }
        if (m.tempMax != INT_MIN) { int v = m.tempMax; tSum += v; tN++; tMax = std::max(tMax, v); if (v >= 85) hot++; }
        if (!m.ch.empty()) chans[m.ch]++;
        if (m.srvVal >= 0) srvs[std::to_string(m.srvVal)]++;
        if (!m.rat.empty()) rats[m.rat]++;
        if (m.denyVal >= 0) { denyN++; if (m.denyVal > 0) denyNonzero++; }
        if (!m.oper.empty()) opers[m.oper]++;
        if (m.rx != LLONG_MIN) rxs.push_back({ m.t, m.rx });
    }
    if (csqN && weak)
        add(L"信号 CSQ", { FmtW(L"弱信号(<10)  %d 次 / 共 %d 样本   首次 %s", weak, csqN, U8ToW(fmtTime(weakFirst, "MD")).c_str()) }, 1);
    // LTE 详情。SNR 是 SDK 原值(0.1dB);阈值仅作工程观察,不冒充协议定论。
    if (rsrpN || rsrqN || snrN) {
        // 质量分档:RSRP ≥-90 良 / -90~-100 中 / <-100 差;RSRQ ≥-15 良 / -15~-20 中 / <-20 差
        auto rateP = [](int v) { return v >= -90 ? L"良" : (v >= -100 ? L"中" : L"差"); };
        auto rateQ = [](int v) { return v >= -15 ? L"良" : (v >= -20 ? L"中" : L"差"); };
        std::vector<std::wstring> ls; int acc = 0;
        if (rsrpN) {
            int avg = (int)(rsrpSum / rsrpN);
            ls.push_back(FmtW(L"RSRP  最低 %d / 均 %d / 最高 %d dBm   [均值:%s]",
                              rsrpMin, avg, rsrpMax, rateP(avg)));
            ls.push_back(L"      (≥-90 良 / -90~-100 中 / <-100 差,越大越好)");
            if (avg < -100 || rsrpMin < -110) acc = std::max(acc, 1);
        }
        if (rsrqN) {
            int avg = (int)(rsrqSum / rsrqN);
            ls.push_back(FmtW(L"RSRQ  最低 %d / 均 %d / 最高 %d dB   [均值:%s]",
                              rsrqMin, avg, rsrqMax, rateQ(avg)));
            ls.push_back(L"      (≥-15 良 / -15~-20 中 / <-20 差)");
            if (avg < -20) acc = std::max(acc, 1);
        }
        if (snrN) {
            double avg = (double)snrSum10 / (10.0 * snrN);
            ls.push_back(FmtW(L"SNR   最低 %.1f / 均 %.1f / 最高 %.1f dB   非正值 %d/%d",
                              snrMin / 10.0, avg, snrMax / 10.0, snrNonPositive, snrN));
            ls.push_back(L"      【源码直证】日志原值单位 0.1dB；【推断】≤0 dB 仅作低质量观察阈值。");
            if (snrN >= 5 && snrNonPositive * 2 >= snrN) acc = std::max(acc, 1);
        }
        add(L"LTE 信号质量 RSRP / RSRQ / SNR", ls, acc);
    }
    if (!srvs.empty() || !rats.empty() || denyN || !opers.empty()) {
        auto dist = [](const std::map<std::string, int>& xs) {
            std::wstring s;
            for (const auto& kv : xs) {
                if (!s.empty()) s += L"    ";
                s += U8ToW(kv.first) + L":" + std::to_wstring(kv.second);
            }
            return s;
        };
        std::vector<std::wstring> ls;
        if (!srvs.empty()) ls.push_back(L"SRV  " + dist(srvs) + L"    (0=NONE / 1=LIMITED / 2=FULL)");
        if (!rats.empty()) ls.push_back(L"RAT  " + dist(rats));
        if (denyN) ls.push_back(FmtW(L"DENY 非零 %d/%d（保留 SDK 原始码；不同产品 SDK 编码表不同）", denyNonzero, denyN));
        if (!opers.empty()) ls.push_back(L"OPER " + dist(opers));
        add(L"注网 / 运营商", ls, denyNonzero ? 1 : 0);
    }
    if (tN) {
        std::wstring s = FmtW(L"max=%d°C   avg=%.0f°C", tMax, (double)tSum / tN);
        if (hot) s += FmtW(L"    ⚠ ≥85°C %d 次", hot);
        add(L"温度", { s }, hot ? 1 : 0);
    }
    if (!chans.empty()) {
        int tot = 0; for (auto& kv : chans) tot += kv.second;
        std::wstring s;
        for (auto& kv : chans) s += FmtW(L"%s:%.0f%%    ", U8ToW(kv.first).c_str(), 100.0 * kv.second / tot);
        add(L"通道占比", { s });
    }

    // RX 停滞
    auto stalls = detectRxStall(rxs);
    if (!stalls.empty()) {
        std::vector<std::wstring> ls;
        for (size_t i = 0; i < stalls.size() && i < 6; ++i)
            ls.push_back(FmtW(L"RX_PKT 卡住 %s  %s → %s", U8ToW(fmtDur(stalls[i].dur)).c_str(),
                         U8ToW(fmtTime(stalls[i].start, "MD")).c_str(), U8ToW(fmtTime(stalls[i].end, "HM")).c_str()));
        if (stalls.size() > 6) ls.push_back(FmtW(L"... 另有 %d 段", (int)stalls.size() - 6));
        add(L"数据假死征兆(RX_PKT 停滞)", ls, 1, true);
    }

    // 报错
    std::vector<const LogLine*> errs;
    for (const LogLine* l : App().document.filtered) if (isErrLine(*l)) errs.push_back(l);
    if (!errs.empty()) {
        std::vector<std::wstring> ls;
        for (size_t i = 0; i < errs.size() && i < 12; ++i)
            ls.push_back(FmtW(L"%s  [%s] %s", U8ToW(errs[i]->ts).c_str(),
                         U8ToW(errs[i]->tagText()).c_str(), U8ToW(errs[i]->msg.substr(0, 90)).c_str()));
        if (errs.size() > 12) ls.push_back(FmtW(L"... 另有 %d 条", (int)errs.size() - 12));
        add(FmtW(L"报错/告警 (%d)", (int)errs.size()), ls, 2, true);
    }

    // 关键事件计数
    int sw = 0, disc = 0, states = 0, cfun = 0, slot = 0, oper = 0, cells = 0;
    for (const LogLine* item : App().document.filtered) {
        const LogLine& l = *item;
        if (l.msg.find("switching to SIM") != std::string::npos ||
            l.msg.find("switching to Roamlink") != std::string::npos) sw++;
        if (l.msg.find("DataCall disconnected") != std::string::npos) disc++;
        if (l.tagText() == "STATE") states++;
        if (l.tagText() == "CFUN" || l.msg.find("CFUN=0") != std::string::npos ||
            l.msg.find("CFUN toggle") != std::string::npos) cfun++;
        if (l.tagText() == "SLOT") slot++;
        if (l.tagText() == "OPER") oper++;
        if (l.tagText().compare(0, 4, "CELL") == 0) cells++;
    }
    add(L"关键事件计数",
        { FmtW(L"通道切换:%d   SDK断开:%d   状态迁移:%d   CFUN:%d   切卡:%d   选网:%d   小区变更:%d",
               sw, disc, states, cfun, slot, oper, cells),
          L"",
          L"提示: “时间线”看事件流，“断网”看逐次，“指标/信号图”看 CSQ、LTE 详情与 ΔRX(=0 即数据不通)。" });

    if (App().hSummary) InvalidateRect(App().hSummary, nullptr, FALSE);
}


void RenderFindings() {
    g_findScroll = 0;
    if (App().hFindings) {
        SetScrollPos(App().hFindings, SB_VERT, 0, TRUE);
        InvalidateRect(App().hFindings, nullptr, FALSE);
    }
}

// “未识别行”页:把解析不了的行摆出来,这是“完完整整不漏消息”的唯一硬证据

void ReleaseOverviewPageData() {
    releaseVector(g_sumCards);
    g_findScroll = g_findContentH = 0;
    g_sumScroll = g_sumContentH = 0;
}

} // namespace dl
