// overview_page.cpp — 总览仪表盘、摘要卡片与证据结论页
#include "overview_page.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "app_context.h"
#include "log_analysis.h"
#include "log_time.h"
#include "memoryutil.h"
#include "modern_shell.h"
#include "theme.h"
#include "ui_pages.h"
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

static void DrawPageEmpty(HDC hdc, RECT rc, const wchar_t* title, const wchar_t* detail) {
    const int width = std::min(S(500), std::max(S(300), static_cast<int>(rc.right) - S(72)));
    const int height = S(150), x = (rc.right - width) / 2;
    const int y = std::max(S(20), (static_cast<int>(rc.bottom) - height) / 2);
    RECT card{x, y, x + width, y + height};
    FillRound(hdc, card, S(14), th::surface, th::border);
    RECT icon{x + S(24), y + S(42), x + S(76), y + S(94)};
    FillRound(hdc, icon, S(26), th::accentSoft, th::accentSoft);
    HGDIOBJ old = SelectObject(hdc, App().hFontSect);
    SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, th::accent);
    DrawTextW(hdc, L"dL", -1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT titleRect{x + S(94), y + S(35), card.right - S(24), y + S(65)};
    SetTextColor(hdc, th::inkPri);
    DrawTextW(hdc, title, -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, App().hFontUI); SetTextColor(hdc, th::inkSec);
    RECT detailRect{x + S(94), y + S(70), card.right - S(24), y + S(124)};
    DrawTextW(hdc, detail, -1, &detailRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    SelectObject(hdc, old);
}

static void DrawPill(HDC hdc, int x, int y, const std::wstring& text,
                     COLORREF fill, COLORREF ink, COLORREF dot = CLR_INVALID) {
    const int width = TextW_(hdc, text, App().hFontSmall) + S(20);
    RECT pill{x, y, x + width, y + S(24)};
    FillRound(hdc, pill, S(12), fill, fill);
    HGDIOBJ old = SelectObject(hdc, App().hFontSmall);
    SetTextColor(hdc, ink); SetBkMode(hdc, TRANSPARENT);
    if (dot != CLR_INVALID) {
        RECT marker{x + S(10), y + S(9), x + S(16), y + S(15)};
        FillRound(hdc, marker, S(3), dot, dot);
        RECT textRect = pill; textRect.left += S(13);
        DrawTextW(hdc, text.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        DrawTextW(hdc, text.c_str(), -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(hdc, old);
}

// 指标卡:发丝描边 + 标签(次要 ink)+ 数值(主 ink 半粗)
static void DrawTile(HDC hdc, RECT r, const std::wstring& label, const std::wstring& val,
                     COLORREF valColor, const std::wstring& note) {
    FillRound(hdc, r, S(10), th::surface, th::border);
    RECT marker{r.left + S(13), r.top + S(12), r.left + S(17), r.top + S(28)};
    FillRound(hdc, marker, S(2), th::accent, th::accent);

    // 行位从 top 顺排,不用 bottom 反推 —— 反推会让 22px 的数值和注释叠在一起
    DrawText_(hdc, r.left + S(24), r.top + S(8), label, App().hFontTileLbl, th::inkSec);
    DrawText_(hdc, r.left + S(14), r.top + S(29), val, App().hFontTileVal, valColor);
    if (!note.empty())
        DrawText_(hdc, r.left + S(14), r.top + S(59), note, App().hFontTileLbl, th::inkMuted);
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
        if (App().document.lines.empty())
            DrawPageEmpty(hdc, rc, L"开始分析第一份日志",
                          L"将 dial_*.log、文本或压缩包拖到窗口中，也可以使用“打开日志”或“粘贴日志”。");
        else
            DrawPageEmpty(hdc, rc, L"筛选后没有结果",
                          L"当前日志已加载，但没有行满足筛选条件。展开筛选面板并清空条件即可恢复。");
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
    const wchar_t* heroTag = avail >= 99.9 ? L"良好" : (avail >= 99.0 ? L"偏低" : L"差");
    const int pad = S(20);
    DrawText_(hdc, pad, S(14), L"可用率", App().hFontTileLbl, th::inkSec);
    std::wstring hv = FmtW(L"%.3f%%", avail);
    DrawText_(hdc, pad, S(30), hv, App().hFontHero, th::inkPri);
    int hx = pad + TextW_(hdc, hv, App().hFontHero) + S(14);
    const std::wstring state = heroTag;
    const COLORREF heroColor = avail >= 99.9 ? th::good : (avail >= 99.0 ? th::warning : th::critical);
    DrawPill(hdc, hx, S(48), state, th::accentSoft, th::inkSec, heroColor);
    DrawText_(hdc, pad, S(87),
              FmtW(L"%s → %s   ·   %s   ·   %s",
                   U8ToW(fmtTime(t0, "FULL")).c_str(), U8ToW(fmtTime(t1, "HM")).c_str(),
                   U8ToW(fmtDur(t1 - t0)).c_str(), U8ToW(App().document.platform.name).c_str()),
              App().hFontTileLbl, th::inkMuted);

    // ---- 指标卡 ----
    int gap = S(10), ty = S(114), th_ = S(82);
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
    int by = ty + th_ + S(18);
    DrawText_(hdc, pad, by, L"断网时长分布", App().hFontSect, th::inkPri);
    by += S(22);
    const wchar_t* bl[4] = { L"≤30s", L"31-60s", L"1-5m", L">5m" };
    int mx = std::max(1, std::max(std::max(b[0], b[1]), std::max(b[2], b[3])));
    int labW = S(56), barX = pad + labW, barMaxW = rc.right - barX - pad - S(40);
    for (int i = 0; i < 4; ++i) {
        int y = by + i * S(22);
        DrawText_(hdc, pad, y + S(1), bl[i], App().hFontTileLbl, th::inkSec);
        int w = barMaxW * b[i] / mx;
        DrawBar(hdc, barX, y, barMaxW, S(14), th::grid);
        DrawBar(hdc, barX, y, w, S(14), th::s1_blue);
        DrawText_(hdc, barX + std::max(w, S(2)) + S(8), y + S(1), FmtW(L"%d", b[i]), App().hFontTileLbl, th::inkSec);
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

struct EvidenceHit {
    RECT rect{};
    size_t lineNo = 0;
    std::wstring text;
};
static std::vector<EvidenceHit> g_evidenceHits;

static const EvidenceHit* EvidenceAt(POINT point) {
    for (const auto& hit : g_evidenceHits)
        if (PtInRect(&hit.rect, point)) return &hit;
    return nullptr;
}

static bool CopyText(const std::wstring& text) {
    if (!OpenClipboard(App().hMain)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) { CloseClipboard(); return false; }
    void* output = GlobalLock(memory);
    if (!output) { GlobalFree(memory); CloseClipboard(); return false; }
    memcpy(output, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory); CloseClipboard(); return false;
    }
    CloseClipboard();
    return true;
}

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

    if (msg == WM_SETCURSOR) {
        POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd, &point);
        if (EvidenceAt(point)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
    }
    if (msg == WM_LBUTTONUP) {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (const EvidenceHit* hit = EvidenceAt(point)) { JumpToRawLine(hit->lineNo); return 0; }
    }
    if (msg == WM_RBUTTONUP) {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (const EvidenceHit* hit = EvidenceAt(point)) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"定位原始行");
            AppendMenuW(menu, MF_STRING, 2, L"复制证据");
            AppendMenuW(menu, MF_STRING, 3, L"添加 / 移除书签\tCtrl+B");
            POINT screen = point; ClientToScreen(hwnd, &screen);
            const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                                screen.x, screen.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (command == 1) JumpToRawLine(hit->lineNo);
            else if (command == 2) {
                if (CopyText(hit->text))
                    ShowModernNotice(L"证据已复制", L"原始行号、时间和日志正文已复制到剪贴板。",
                                     ModernNoticeKind::Success, 3500);
                else
                    ShowModernNotice(L"复制失败", L"剪贴板暂时不可用，请稍后重试。",
                                     ModernNoticeKind::Error);
            } else if (command == 3) {
                const bool added = ToggleEvidenceBookmark(hit->lineNo, hit->text);
                ShowModernNotice(added ? L"证据书签已添加" : L"证据书签已移除",
                                 FmtW(L"原始第 %d 行", static_cast<int>(hit->lineNo)).c_str(),
                                 added ? ModernNoticeKind::Success : ModernNoticeKind::Info);
            }
            return 0;
        }
    }

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
    g_evidenceHits.clear();

    if (App().document.lines.empty()) {
        DrawPageEmpty(hdc, rc, L"诊断结论将在这里形成",
                      L"加载日志后，结论会按严重程度展示依据、建议和可追溯的原始证据。");
        g_findContentH = rc.bottom;
        SCROLLINFO emptyScroll{}; emptyScroll.cbSize = sizeof(emptyScroll);
        emptyScroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS; emptyScroll.nMin = 0;
        emptyScroll.nMax = rc.bottom; emptyScroll.nPage = rc.bottom; emptyScroll.nPos = 0;
        SetScrollInfo(hwnd, SB_VERT, &emptyScroll, TRUE);
        BitBlt(hw, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, obm); DeleteObject(bmp); DeleteDC(hdc);
        EndPaint(hwnd, &ps); return 0;
    }

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
        FillRound(hdc, cr, S(11), th::surface, th::border);
        if (band) {   // 左侧色带
            RECT b{ M + S(1), topY + S(2), M + S(1) + BAND, topY + height - S(2) };
            HBRUSH bb = CreateSolidBrush(band); FillRect(hdc, &b, bb); DeleteObject(bb);
        }
    };

    // ── 一眼可读的诊断摘要 ──
    {
        int severe = 0, warning = 0, info = 0;
        for (const auto& finding : App().document.findings) {
            if (finding.severity == 2) ++severe;
            else if (finding.severity == 1) ++warning;
            else ++info;
        }
        const int h = S(74);
        drawCard(y, h, 0);
        DrawText_(hdc, textX0, y + S(14), L"诊断摘要", App().hFontSect, th::inkPri);
        DrawText_(hdc, textX0, y + S(40),
                  FmtW(L"共 %d 条有证据支撑的结论", (int)App().document.findings.size()),
                  App().hFontSmall, th::inkMuted);
        int px = M + cardW - S(18);
        auto pillRight = [&](const std::wstring& text, COLORREF fill, COLORREF ink) {
            int width = TextW_(hdc, text, App().hFontSmall) + S(20);
            px -= width; DrawPill(hdc, px, y + S(25), text, fill, ink); px -= S(8);
        };
        if (info) pillRight(FmtW(L"信息 %d", info), th::accentSoft, th::accent);
        if (warning) pillRight(FmtW(L"告警 %d", warning), th::cellWeak, th::rowWarn);
        if (severe) pillRight(FmtW(L"严重 %d", severe), th::outageBand, th::rowFault);
        y += h + GAP;
    }

    // ── 头部元信息卡 ──
    {
        std::wstring platform = L"来源平台:  " + U8ToW(App().document.platform.name);
        std::wstring coverage = FmtW(L"解析覆盖:  已解析 %d 行,未识别 %d 行(%.2f%%)%s",
            (int)App().document.audit.parsed, (int)App().document.audit.unparsed,
            App().document.audit.unparsedRatio() * 100.0,
            App().document.audit.unparsed == 0 ? L"  → 无遗漏" : L"  → 见“未识别行”页");
        std::wstring evidence;
        if (App().document.platform.evidenceLine)
            evidence = FmtW(L"识别依据:  第 %d 行  ", (int)App().document.platform.evidenceLine) +
                       U8ToW(App().document.platform.evidence);
        auto measureMeta = [&](const std::wstring& text) {
            HGDIOBJ old = SelectObject(hdc, App().hFontUI);
            RECT measured{0, 0, textW, S(1000)};
            DrawTextW(hdc, text.c_str(), -1, &measured,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
            SelectObject(hdc, old);
            return std::max(S(20), static_cast<int>(measured.bottom));
        };
        bool jump = App().document.audit.clockJump;
        int h = CARD_PAD * 2 + measureMeta(platform) + measureMeta(coverage) +
                (evidence.empty() ? 0 : measureMeta(evidence)) + (jump ? S(40) : 0);
        drawCard(y, h, th::inkMuted);
        int ty = y + CARD_PAD;
        ty += DrawWrapped(hdc, textX0, ty, textW, platform, App().hFontUI, th::inkPri);
        ty += DrawWrapped(hdc, textX0, ty, textW, coverage, App().hFontUI,
                          App().document.audit.unparsed == 0 ? th::inkSec : th::rowWarn);
        if (!evidence.empty()) {
            const int evidenceH = DrawWrapped(hdc, textX0, ty, textW, evidence,
                                              App().hFontUI, th::accent);
            g_evidenceHits.push_back(EvidenceHit{RECT{textX0, ty, textX0 + textW, ty + evidenceH},
                                                 App().document.platform.evidenceLine, evidence});
            ty += evidenceH;
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
        ++n;
        std::wstring title = FmtW(L"%d. ", n) + U8ToW(f.title);
        std::wstring detail = U8ToW(f.detail), advice = U8ToW(f.advice);
        std::vector<std::wstring> evidence;
        evidence.reserve(f.ev.size());
        for (const auto& e : f.ev)
            evidence.push_back(FmtW(L"· 第 %d 行  ", (int)e.lineNo) + U8ToW(e.ts) + L"  " + U8ToW(e.text));

        // —— 测量 pass:算这张卡多高(不画,只用 DT_CALCRECT)——
        auto measureWrap = [&](const std::wstring& s, HFONT font) {
            HGDIOBJ of = SelectObject(hdc, font);
            RECT r{ 0, 0, textW, 10000 };
            DrawTextW(hdc, s.c_str(), (int)s.size(), &r, DT_LEFT|DT_TOP|DT_WORDBREAK|DT_CALCRECT);
            SelectObject(hdc, of);
            return (int)(r.bottom - r.top);
        };
        int titleH = S(28), lblH = S(20);
        const int SEC = S(12);
        int h = CARD_PAD;                       // 顶内边距
        h += titleH + S(8);                     // 标题
        h += lblH + measureWrap(detail, App().hFontUI) + SEC;   // 依据
        h += lblH + measureWrap(advice, App().hFontUI) + SEC;   // 建议
        h += lblH;
        for (const auto& line : evidence) h += measureWrap(line, App().hFontMono) + S(5);
        h += CARD_PAD;                          // 底内边距

        // —— 画 pass:白底卡 + 色带,再叠字 ——
        drawCard(y, h, band);
        int ty = y + CARD_PAD;
        const std::wstring badge = lv;
        DrawPill(hdc, textX0, ty, badge,
                 f.severity == 2 ? th::outageBand : (f.severity == 1 ? th::cellWeak : th::accentSoft),
                 band);
        const int badgeW = TextW_(hdc, badge, App().hFontSmall) + S(30);
        DrawText_(hdc, textX0 + badgeW, ty + S(2), title, App().hFontSect, th::inkPri);
        ty += titleH + S(8);
        DrawText_(hdc, textX0, ty, L"依据", App().hFontUI, th::inkMuted); ty += lblH;
        ty += DrawWrapped(hdc, textX0, ty, textW, detail, App().hFontUI, th::inkPri) + SEC;
        DrawText_(hdc, textX0, ty, L"建议", App().hFontUI, th::inkMuted); ty += lblH;
        ty += DrawWrapped(hdc, textX0, ty, textW, advice, App().hFontUI, th::inkSec) + SEC;
        DrawText_(hdc, textX0, ty, L"证据 · 单击定位，右键复制", App().hFontUI, th::inkMuted); ty += lblH;
        for (size_t evidenceIndex = 0; evidenceIndex < evidence.size(); ++evidenceIndex) {
            const int evidenceH = DrawWrapped(hdc, textX0 + S(8), ty, textW - S(8),
                                              evidence[evidenceIndex], App().hFontMono, th::accent);
            g_evidenceHits.push_back(EvidenceHit{
                RECT{textX0 + S(8), ty, textX0 + textW, ty + evidenceH},
                f.ev[evidenceIndex].lineNo, evidence[evidenceIndex]});
            ty += evidenceH + S(5);
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
    int textW = cardW - CARD_PAD * 2 - BAND;
    int y = M - g_sumScroll;

    for (const auto& c : g_sumCards) {
        COLORREF band = (c.accent == 2) ? th::rowFault : (c.accent == 1 ? th::rowWarn : th::inkMuted);
        HFONT lineFont = c.mono ? App().hFontMono : App().hFontUI;
        int titleH = S(24);
        auto measure = [&](const std::wstring& line) {
            HGDIOBJ old = SelectObject(hdc, lineFont);
            RECT measured{0, 0, textW, S(1000)};
            DrawTextW(hdc, line.c_str(), -1, &measured,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
            SelectObject(hdc, old);
            return std::max(S(18), static_cast<int>(measured.bottom));
        };
        int h = CARD_PAD + titleH + S(6) + CARD_PAD;
        for (const auto& line : c.lines) h += measure(line) + S(4);

        // 画卡
        RECT cr{ M, y, M + cardW, y + h };
        FillRound(hdc, cr, S(11), th::surface, th::border);
        RECT b{ M + S(1), y + S(2), M + S(1) + BAND, y + h - S(2) };
        HBRUSH bb = CreateSolidBrush(band); FillRect(hdc, &b, bb); DeleteObject(bb);

        int ty = y + CARD_PAD;
        DrawText_(hdc, textX0, ty, c.title, App().hFontSect,
                  c.accent == 2 ? th::rowFault : (c.accent == 1 ? th::rowWarn : th::inkPri));
        ty += titleH + S(6);
        for (const auto& ln : c.lines) {
            ty += DrawWrapped(hdc, textX0, ty, textW, ln, lineFont, th::inkPri) + S(4);
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

    if (App().document.sources.size() > 1) {
        std::vector<std::wstring> lines;
        lines.push_back(FmtW(L"%d 份日志已按时间轴合并；下列指标按来源独立计算，可直接横向比较。",
                             static_cast<int>(App().document.sources.size())));
        for (const auto& source : App().document.sources) {
            std::wstring label = source.label;
            const size_t slash = label.find_last_of(L"\\/");
            if (slash != std::wstring::npos) label = label.substr(slash + 1);
            lines.push_back(FmtW(L"%-28s  行:%d  断网:%d  指标:%d  平均 RSRP:%s",
                                 label.c_str(), static_cast<int>(source.parsedLines),
                                 static_cast<int>(source.outages), static_cast<int>(source.metricRows),
                                 source.hasAverageRsrp
                                     ? FmtW(L"%d dBm", source.averageRsrp).c_str() : L"—"));
        }
        add(L"多日志对比", lines, 0, true);
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
    std::map<std::string, int> cells;
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
        if (!m.cellId.empty()) cells[m.cellId.str()]++;
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
    if (!cells.empty()) {
        std::vector<std::pair<std::string, int>> ranked(cells.begin(), cells.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
            return left.second != right.second ? left.second > right.second : left.first < right.first;
        });
        int totalSamples = 0;
        for (const auto& cell : ranked) totalSamples += cell.second;
        std::vector<std::wstring> ls;
        ls.push_back(FmtW(L"识别到 %d 个小区，覆盖 %d 条指标样本", (int)ranked.size(), totalSamples));
        for (size_t index = 0; index < ranked.size() && index < 6; ++index)
            ls.push_back(FmtW(L"%s    %d 次（%.1f%%）", U8ToW(ranked[index].first).c_str(),
                              ranked[index].second, 100.0 * ranked[index].second / totalSamples));
        if (ranked.size() > 6) ls.push_back(FmtW(L"… 另有 %d 个小区", (int)ranked.size() - 6));
        add(L"小区驻留分布", ls, ranked.size() > 12 ? 1 : 0, true);
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
    int sw = 0, disc = 0, states = 0, cfun = 0, slot = 0, oper = 0, cellChanges = 0;
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
        if (l.tagText().compare(0, 4, "CELL") == 0) cellChanges++;
    }
    add(L"关键事件计数",
        { FmtW(L"通道切换:%d   SDK断开:%d   状态迁移:%d   CFUN:%d   切卡:%d   选网:%d   小区变更:%d",
               sw, disc, states, cfun, slot, oper, cellChanges),
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
