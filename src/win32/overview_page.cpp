// overview_page.cpp — 总览仪表盘、摘要卡片与证据结论页
#include "overview_page.h"
#include "rssi_summary.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app_context.h"
#include "log_analysis.h"
#include "log_time.h"
#include "load_controller.h"
#include "source_workspace.h"
#include "text_view.h"
#include "memoryutil.h"
#include "modern_shell.h"
#include "signal_quality.h"
#include "theme.h"
#include "ui_pages.h"
#include "win_text.h"
#include "text_catalog.h"
#include "findingmodel.h"

namespace dl {

struct SumCard {
    std::wstring title;              // 卡标题(如"断网""报错/告警")
    std::vector<std::wstring> lines; // 卡内每行文本
    int accent = 0;                  // 0=中性 1=告警(黄) 2=严重(红)
    bool mono = false;               // 内容是否等宽(报错/证据类用等宽对齐)
};
static std::vector<SumCard> g_sumCards;
struct CopyHit { RECT rect{}; std::wstring text; };
static std::vector<CopyHit> g_summaryCopyHits, g_findingCopyHits;
static RECT g_dashCopyRect{}, g_findCopyRect{}, g_dashTileRects[4]{};
static RECT g_findExpandAllRect{};
struct FindingExpandHit { RECT rect{}; std::size_t index = 0; };
static std::vector<FindingExpandHit> g_findingExpandHits;
static std::vector<FindingExpandHit> g_selectTextHits;
static std::size_t g_focusedFinding=0;
static bool g_findingKeyboardFocus=false;
static bool g_findingPointerFocus=false;
static std::set<std::size_t> g_expandedFindings;
static bool g_expandAllFindings = false;
struct DashboardStats {
    long long start = 0, end = 0, total = 0, longest = 0, csqSum = 0;
    int buckets[4]{}, csqCount = 0, csqMin = 9999, csqMax = -1;
    AvailabilityStats availability;
    std::wstring startText, endText;
};
static DashboardStats g_dashboard;
static void BuildDashboardStats() {
    g_dashboard = DashboardStats{};
    if (App().document.filtered.empty()) return;
    g_dashboard.start = App().document.filtered.front()->t; g_dashboard.end = App().document.filtered.back()->t;
    auto timestamp = [](const LogLine* line) {
        return U8ToW(line->inferredTime && !line->ts.empty() ? line->ts : fmtTime(line->t, "FULL"));
    };
    g_dashboard.startText = timestamp(App().document.filtered.front());
    g_dashboard.endText = timestamp(App().document.filtered.back());
    g_dashboard.availability = availabilityStats(App().document.filtered, App().document.outages);
    for (const auto& outage : App().document.outages) {
        if (!outage.recovered) continue;
        g_dashboard.total += outage.dur; g_dashboard.longest = std::max(g_dashboard.longest, static_cast<long long>(outage.dur));
        ++g_dashboard.buckets[outage.dur <= 30 ? 0 : outage.dur <= 60 ? 1 : outage.dur <= 300 ? 2 : 3];
    }
    for (const auto& metric : App().document.metrics) {
        if (!usesLteEngineeringReference(metric.rat) || metric.csqVal < 0 || metric.csqVal > 31) continue;
        g_dashboard.csqSum += metric.csqVal; ++g_dashboard.csqCount;
        g_dashboard.csqMin = std::min(g_dashboard.csqMin, metric.csqVal); g_dashboard.csqMax = std::max(g_dashboard.csqMax, metric.csqVal);
    }
}
static bool CopyText(const std::wstring& text);
static int ScrollThumb(HWND window) {
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_TRACKPOS;
    GetScrollInfo(window, SB_VERT, &info);
    return info.nTrackPos;
}
static void ClampPageScroll(HWND window, int& position) {
    SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_POS;
    GetScrollInfo(window, SB_VERT, &info);
    if (position != info.nPos) {
        position = info.nPos;
        InvalidateRect(window, nullptr, FALSE);
    }
}
static void CopyNotice(const std::wstring& text) {
    const bool copied = CopyText(text);
    ShowModernNotice(copied ? UiText(TextId::ui_0478) : UiText(TextId::ui_0357),
                     copied ? UiText(TextId::ui_0358) : UiText(TextId::ui_0359),
                     copied ? ModernNoticeKind::Success : ModernNoticeKind::Error, 3000);
}
static std::wstring CardText(const SumCard& card) {
    std::wstring text = card.title + L"\r\n";
    if (App().document.sources.size()>1) text+=AnalysisSourceText()+L"\r\n";
    if (HasAnalysisTimeFilter()) text += AnalysisTimeRangeText() + L"\r\n";
    for (const auto& line : card.lines) text += line + L"\r\n";
    return text;
}
static std::wstring FindingText(size_t index) {
    const auto& finding = App().document.findings[index];
    std::wstring text = FmtW(L"%d. [%s] ", static_cast<int>(index + 1),
        finding.severity == 2 ? UiText(TextId::ui_0115) : finding.severity == 1 ? UiText(TextId::ui_0116) : UiText(TextId::ui_0117)) + AnalysisScopedText(finding.title);
    if (App().document.sources.size()>1 && !HasAnalysisTimeFilter()) text+=L"\r\n"+AnalysisSourceText();
    if (HasAnalysisTimeFilter()) text += L"\r\n" + AnalysisSourceText() + L"\r\n" + AnalysisTimeRangeText() + UiText(TextId::ui_0360);
    text += UiText(TextId::ui_0361) + AnalysisScopedText(finding.detail) + UiText(TextId::ui_0362) + U8ToW(GeneratedText(finding.advice)) + UiText(TextId::ui_0363);
    for (const auto& item : finding.ev)
        text += FmtW(UiText(TextId::ui_0156), static_cast<int>(item.lineNo)) + U8ToW(item.ts) + L"  " + U8ToW(item.text) + L"\r\n";
    return text;
}
static void SelectFindingText(std::size_t index) {
    if(index>=App().document.findings.size())return;
    std::vector<TextEvidence> evidence;
    for(const auto& e:App().document.findings[index].ev)
        evidence.push_back({e.lineNo,U8ToW(e.ts)+L"  "+U8ToW(e.text)});
    ShowSelectableText(UiText(TextId::text_select),FindingText(index),evidence);
}
static std::wstring FindingsMetaText() {
    const auto& doc = App().document;
    std::wstring text = AnalysisSourceText() + L"\r\n" + UiText(TextId::ui_0364) + U8ToW(GeneratedText(doc.platform.name)) + L"\r\n";
    if (HasAnalysisTimeFilter()) text += AnalysisTimeRangeText() +
        UiText(TextId::ui_0365);
    text += FmtW(UiText(TextId::ui_0366),
        static_cast<int>(doc.audit.parsed), static_cast<int>(doc.audit.unparsed), doc.audit.unparsedRatio() * 100.0,
        static_cast<int>(doc.audit.nulBytes), static_cast<int>(doc.audit.nulLines));
    text += L"\r\n" + U8ToW(rssiSummaryText(doc.rssi)) + L"\r\n";
    if (doc.platform.evidenceLine) text += FmtW(UiText(TextId::ui_0367), static_cast<int>(doc.platform.evidenceLine)) + U8ToW(doc.platform.evidence) + L"\r\n";
    if (doc.audit.clockJump) text += FmtW(UiText(TextId::ui_0368),
        static_cast<int>(doc.audit.jumpAtLine), U8ToW(fmtTime(doc.audit.jumpFromT, "FULL")).c_str(), U8ToW(fmtTime(doc.audit.jumpToT, "FULL")).c_str());
    return text;
}
bool CopyOverviewPage(int page) {
    if (page != 0 && page != 1) return false;
    if (App().document.lines.empty()) return false;
    RenderPage(page);
    std::wstring text = page == 0 ? UiText(TextId::ui_0477) : UiText(TextId::ui_0369);
    text += UiText(TextId::ui_0370) + GetText(App().hFileLbl) + L"\r\n";
    text += AnalysisTimeRangeText() + L"\r\n";
    if (page == 0) {
        for (const auto& card : g_sumCards) text += L"\r\n" + CardText(card);
    } else {
        text += FindingsMetaText();
        if (App().document.findings.empty()) text += UiText(TextId::ui_0371);
        for (size_t index = 0; index < App().document.findings.size(); ++index) text += L"\r\n" + FindingText(index);
    }
    CopyNotice(text);
    return true;
}
static bool HandleCopyMenu(HWND hwnd, UINT msg, LPARAM lp, const std::vector<CopyHit>& hits, int page) {
    if (msg != WM_CONTEXTMENU) return false;
    POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (point.x == -1 && point.y == -1) { point = POINT{S(24), S(24)}; ClientToScreen(hwnd, &point); }
    POINT client = point; ScreenToClient(hwnd, &client);
    std::wstring cardText;
    for (const auto& hit : hits) if (PtInRect(&hit.rect, client)) { cardText = hit.text; break; }
    HMENU menu = CreatePopupMenu();
    if (!cardText.empty()) AppendMenuW(menu, MF_STRING, 1, page == 0 ? UiText(TextId::ui_0372) : UiText(TextId::ui_0373));
    if(page==1 && !cardText.empty())AppendMenuW(menu,MF_STRING,3,UiText(TextId::text_select));
    AppendMenuW(menu, MF_STRING, 2, page == 0 ? UiText(TextId::ui_0374) : UiText(TextId::ui_0375));
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD, point.x, point.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (command == 1) CopyNotice(cardText);
    if (command == 2) CopyOverviewPage(page);
    if(command==3)for(std::size_t i=0;i<hits.size();++i)if(hits[i].text==cardText){SelectFindingText(i);break;}
    return true;
}


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
    DrawTextW(hdc, detail, -1, &detailRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
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
    HGDIOBJ old = SelectObject(hdc, App().hFontTileLbl);
    RECT title{r.left + S(24), r.top + S(8), r.right - S(12), r.top + S(37)};
    SetTextColor(hdc, th::inkSec);
    DrawTextW(hdc, label.c_str(), -1, &title, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, TextW_(hdc, val, App().hFontTileVal) > r.right - r.left - S(28)
                          ? App().hFontUI : App().hFontTileVal);
    RECT value{r.left + S(14), r.top + S(40), r.right - S(12), r.top + S(68)};
    SetTextColor(hdc, valColor);
    DrawTextW(hdc, val.c_str(), -1, &value, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (!note.empty()) {
        SelectObject(hdc, App().hFontSmall); SetTextColor(hdc, th::inkMuted);
        RECT text{r.left + S(14), r.top + S(72), r.right - S(12), r.bottom - S(8)};
        DrawTextW(hdc, note.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(hdc, old);
}

LRESULT CALLBACK DashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_LBUTTONUP) {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&g_dashCopyRect, point)) { CopyOverviewPage(0); return 0; }
        const int destinations[] = {3, 3, 7, 4};
        for (int index = 0; index < 4; ++index) {
            if (PtInRect(&g_dashTileRects[index], point)) { ShowPage(destinations[index]); return 0; }
        }
    }
    if (msg == WM_SETCURSOR) {
        POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd, &point);
        bool interactive = PtInRect(&g_dashCopyRect, point);
        for (const RECT& rect : g_dashTileRects) if (PtInRect(&rect, point)) interactive = true;
        SetCursor(LoadCursorW(nullptr, interactive ? IDC_HAND : IDC_ARROW)); return TRUE;
    }
    if (HandleCopyMenu(hwnd, msg, lp, {}, 0)) return 0;
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
            DrawPageEmpty(hdc, rc, UiText(TextId::ui_0376),
                          UiText(TextId::ui_0377));
        else
            DrawPageEmpty(hdc, rc, UiText(TextId::ui_0378),
                          UiText(TextId::ui_0379));
        BitBlt(hw, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, obm); DeleteObject(bmp); DeleteDC(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    // 统计在文档刷新时计算一次，窗口重绘不再遍历完整日志。
    const auto& availability = g_dashboard.availability;
    const bool evidenceLimited = availability.evidenceLimited();
    const bool availValid = availability.runtimeValid() && !evidenceLimited;
    const double avail = availability.runtimePercent();
    const long long total = g_dashboard.total, longest = g_dashboard.longest, csqSum = g_dashboard.csqSum;
    const int csqN = g_dashboard.csqCount, csqMin = g_dashboard.csqMin, csqMax = g_dashboard.csqMax;
    const int* b = g_dashboard.buckets;

    // ---- hero:可用率(每视图仅此一个大数字) ----
    // 状态色须配文字标签,不能只靠颜色表意 —— 故旁边永远写着"可用率"
    const bool noFirstConnection = availability.neverConnectedStartupSegments > 0 &&
                                   !availability.runtimeValid();
    const wchar_t* heroTag = evidenceLimited ? UiText(TextId::ui_0380) : noFirstConnection ? UiText(TextId::ui_0381) : !availValid ? UiText(TextId::ui_0382) :
                              (avail >= 99.9 ? UiText(TextId::ui_0383) : (avail >= 99.0 ? UiText(TextId::ui_0384) : UiText(TextId::ui_0385)));
    const int pad = S(20);
    DrawText_(hdc, pad, S(14), UiText(TextId::ui_0386), App().hFontTileLbl, th::inkSec);
    std::wstring hv = availValid ? FmtW(L"%.3f%%", avail) : L"—";
    DrawText_(hdc, pad, S(30), hv, App().hFontHero, th::inkPri);
    int hx = pad + TextW_(hdc, hv, App().hFontHero) + S(14);
    const std::wstring state = heroTag;
    const COLORREF heroColor = evidenceLimited ? th::warning : noFirstConnection ? th::critical : !availValid ? th::inkMuted :
                               (avail >= 99.9 ? th::good : (avail >= 99.0 ? th::warning : th::critical));
    DrawPill(hdc, hx, S(48), state, th::accentSoft, th::inkSec, heroColor);
    DrawText_(hdc, pad, S(87),
              g_dashboard.startText + L" → " + g_dashboard.endText,
              App().hFontTileLbl, th::inkSec);
    g_dashCopyRect = RECT{rc.right - S(122), S(14), rc.right - S(20), S(42)};
    DrawPill(hdc, g_dashCopyRect.left, g_dashCopyRect.top, UiText(TextId::ui_0387), th::accentSoft, th::accent);

    // ---- 指标卡 ----
    int gap = S(10), ty = S(114), th_ = S(106);
    int tw = (rc.right - pad * 2 - gap * 3) / 4;
    if (tw > 60) {
        RECT r1{ pad, ty, pad + tw, ty + th_ };
        DrawTile(hdc, r1, UiText(TextId::ui_0250), FmtW(L"%d", (int)App().document.outages.size()), th::inkPri,
                 FmtW(UiText(TextId::ui_0388), U8ToW(fmtDur(total)).c_str()));
        RECT r2{ r1.right + gap, ty, r1.right + gap + tw, ty + th_ };
        DrawTile(hdc, r2, UiText(TextId::ui_0389), U8ToW(fmtDur(longest)), th::inkPri, L"");
        RECT r3{ r2.right + gap, ty, r2.right + gap + tw, ty + th_ };
        DrawTile(hdc, r3, UiText(TextId::ui_0390),
                 FmtW(L"%d / %d NUL", (int)App().document.audit.unparsed,
                      (int)App().document.audit.nulBytes),
                 App().document.audit.unparsed ? th::inkPri : th::inkPri,
                 (App().document.audit.unparsed || App().document.audit.nulBytes)
                     ? FmtW(UiText(TextId::ui_0391),
                            App().document.audit.unparsedRatio()*100.0,
                            (int)App().document.audit.nulLines)
                     : UiText(TextId::ui_0392));
        RECT r4{ r3.right + gap, ty, r3.right + gap + tw, ty + th_ };
        g_dashTileRects[0] = r1; g_dashTileRects[1] = r2; g_dashTileRects[2] = r3; g_dashTileRects[3] = r4;
        DrawTile(hdc, r4, UiText(TextId::ui_0393),
                 csqN ? FmtW(L"%d / %.1f / %d", csqMin, (double)csqSum/csqN, csqMax) : L"—",
                 th::inkPri, csqN ? FmtW(UiText(TextId::ui_0394), csqN) : L"");
    }

    // ---- 断网时长分布(横条)----
    int by = ty + th_ + S(18);
    if (rc.bottom >= S(352)) {
    DrawText_(hdc, pad, by, UiText(TextId::ui_0395), App().hFontSect, th::inkPri);
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
    DrawTextW(hdc, s.c_str(), (int)s.size(), &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    int h = r.bottom - r.top;
    r.right = x + maxW;
    DrawTextW(hdc, s.c_str(), (int)s.size(), &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, of);
    return h;
}

static void EnsureFocusedFinding(HWND hwnd) {
    UpdateWindow(hwnd);
    if(g_focusedFinding>=g_findingCopyHits.size())return;
    RECT client{};GetClientRect(hwnd,&client);const auto rect=g_findingCopyHits[g_focusedFinding].rect;
    int delta=rect.top<S(12)?rect.top-S(12):rect.bottom>client.bottom?rect.bottom-client.bottom+S(12):0;
    if(rect.bottom-rect.top>client.bottom-S(24))delta=rect.top-S(12);
    if(delta) {
        g_findScroll=std::clamp(g_findScroll+delta,0,std::max(0,g_findContentH-static_cast<int>(client.bottom)));
        SetScrollPos(hwnd,SB_VERT,g_findScroll,TRUE);
    }
    InvalidateRect(hwnd,nullptr,FALSE);
}
bool RouteFindingKeyboardMessage(MSG& message) {
    if(message.message!=WM_KEYDOWN || GetFocus()!=App().hFindings || App().document.findings.empty())return false;
    const auto last=App().document.findings.size()-1;
    g_focusedFinding=std::min(g_focusedFinding,last);
    if(message.wParam=='C' && (GetKeyState(VK_CONTROL)&0x8000)){CopyNotice(FindingText(g_focusedFinding));return true;}
    if(message.wParam==VK_F2){SelectFindingText(g_focusedFinding);return true;}
    if(message.wParam==VK_RETURN || message.wParam==VK_SPACE) {
        if(g_expandedFindings.count(g_focusedFinding))g_expandedFindings.erase(g_focusedFinding);
        else g_expandedFindings.insert(g_focusedFinding);
    } else if(message.wParam==VK_UP){if(g_focusedFinding)--g_focusedFinding;}
    else if(message.wParam==VK_DOWN)g_focusedFinding=std::min(last,g_focusedFinding+1);
    else if(message.wParam==VK_HOME)g_focusedFinding=0;
    else if(message.wParam==VK_END)g_focusedFinding=last;
    else return false;
    g_findingKeyboardFocus=true;InvalidateRect(App().hFindings,nullptr,FALSE);EnsureFocusedFinding(App().hFindings);return true;
}
LRESULT CALLBACK FindingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if(msg==WM_GETDLGCODE)return DLGC_WANTARROWS|DLGC_WANTCHARS;
    if(msg==WM_KEYDOWN){MSG key{};key.hwnd=hwnd;key.message=msg;key.wParam=wp;key.lParam=lp;if(RouteFindingKeyboardMessage(key))return 0;}
    if(msg==WM_SETFOCUS){
        g_findingKeyboardFocus=!g_findingPointerFocus;
        if(g_findingKeyboardFocus)EnsureFocusedFinding(hwnd);
        else InvalidateRect(hwnd,nullptr,FALSE);
        return 0;
    }
    if(msg==WM_KILLFOCUS){InvalidateRect(hwnd,nullptr,FALSE);return 0;}
    if(msg==WM_LBUTTONDOWN){
        // Pointer focus must not scroll before hit-testing the clicked card.
        // Keyboard focus still reveals its selected conclusion.
        g_findingPointerFocus=true;SetFocus(hwnd);g_findingPointerFocus=false;
        g_findingKeyboardFocus=false;
        POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        for(std::size_t i=0;i<g_findingCopyHits.size();++i)if(PtInRect(&g_findingCopyHits[i].rect,point)){g_focusedFinding=i;break;}
        InvalidateRect(hwnd,nullptr,FALSE);return 0;}

    if (msg == WM_SETCURSOR) {
        POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd, &point);
        if (EvidenceAt(point)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
    }
    if (msg == WM_LBUTTONUP) {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for(const auto& hit:g_selectTextHits)if(PtInRect(&hit.rect,point)){SelectFindingText(hit.index);return 0;}
        if (PtInRect(&g_findExpandAllRect,point)) {
            g_expandAllFindings=!g_expandAllFindings; g_expandedFindings.clear();
            g_findScroll=0; InvalidateRect(hwnd,nullptr,FALSE); return 0;
        }
        for(const auto& hit : g_findingExpandHits) if(PtInRect(&hit.rect,point)) {
            if(g_expandedFindings.count(hit.index)) g_expandedFindings.erase(hit.index);
            else g_expandedFindings.insert(hit.index);
            InvalidateRect(hwnd,nullptr,FALSE); return 0;
        }
        if (const EvidenceHit* hit = EvidenceAt(point)) { JumpToRawLine(hit->lineNo); return 0; }
    }
    if (msg == WM_CONTEXTMENU) {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &point);
        if (const EvidenceHit* found = EvidenceAt(point)) {
            const EvidenceHit evidenceHit = *found;
            const EvidenceHit* hit = &evidenceHit;
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, UiText(TextId::ui_0396));
            AppendMenuW(menu, MF_STRING, 2, UiText(TextId::ui_0397));
            AppendMenuW(menu, MF_STRING, 3, UiText(TextId::ui_0398));
            POINT screen = point; ClientToScreen(hwnd, &screen);
            const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                                screen.x, screen.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (command == 1) JumpToRawLine(hit->lineNo);
            else if (command == 2) {
                if (CopyText(hit->text))
                    ShowModernNotice(UiText(TextId::ui_0399), UiText(TextId::ui_0400),
                                     ModernNoticeKind::Success, 3500);
                else
                    ShowModernNotice(UiText(TextId::ui_0357), UiText(TextId::ui_0401),
                                     ModernNoticeKind::Error);
            } else if (command == 3) {
                const bool added = ToggleEvidenceBookmark(hit->lineNo, hit->text);
                ShowModernNotice(added ? UiText(TextId::ui_0299) : UiText(TextId::ui_0300),
                                 FmtW(UiText(TextId::ui_0301), static_cast<int>(hit->lineNo)).c_str(),
                                 added ? ModernNoticeKind::Success : ModernNoticeKind::Info);
            }
            return 0;
        }
        if (HandleCopyMenu(hwnd, msg, lp, g_findingCopyHits, 1)) return 0;
    }
    if (msg == WM_LBUTTONUP) {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&g_findCopyRect, point)) { CopyOverviewPage(1); return 0; }
        for (const auto& hit : g_findingCopyHits) {
            RECT button{hit.rect.right - S(126), hit.rect.top + S(12), hit.rect.right - S(18), hit.rect.top + S(42)};
            if (PtInRect(&button, point)) { CopyNotice(hit.text); return 0; }
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
            case SB_THUMBPOSITION: g_findScroll = ScrollThumb(hwnd); break;
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
    g_findingCopyHits.clear();
    g_findCopyRect = RECT{};
    g_findingExpandHits.clear(); g_selectTextHits.clear(); g_findExpandAllRect=RECT{};

    if (App().document.lines.empty()) {
        DrawPageEmpty(hdc, rc, UiText(TextId::ui_0222),
                      UiText(TextId::ui_0402));
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
        DrawText_(hdc, textX0, y + S(14), UiText(TextId::ui_0403), App().hFontSect, th::inkPri);
        DrawText_(hdc, textX0, y + S(40),
                  FmtW(UiText(TextId::ui_0404), (int)App().document.findings.size()),
                  App().hFontSmall, th::inkMuted);
        g_findCopyRect = RECT{M + cardW - S(144), y + S(12), M + cardW - S(18), y + S(40)};
        DrawPill(hdc, g_findCopyRect.left, g_findCopyRect.top, UiText(TextId::ui_0405), th::accentSoft, th::accent);
        g_findExpandAllRect=RECT{M+cardW-S(268),y+S(12),M+cardW-S(152),y+S(40)};
        DrawPill(hdc,g_findExpandAllRect.left,g_findExpandAllRect.top,
            UiText(g_expandAllFindings?TextId::ui_0542:TextId::ui_0541),th::accentSoft,th::accent);
        DrawText_(hdc, textX0 + S(140), y + S(40), FmtW(UiText(TextId::ui_0406), severe, warning, info), App().hFontSmall, th::inkSec);
        y += h + GAP;
    }

    // ── 头部元信息卡 ──
    {
        std::wstring platform = UiText(TextId::ui_0407) + U8ToW(GeneratedText(App().document.platform.name));
        std::wstring coverage = FmtW(UiText(TextId::ui_0408),
            (int)App().document.audit.parsed, (int)App().document.audit.unparsed,
            App().document.audit.unparsedRatio() * 100.0, (int)App().document.audit.nulBytes,
            (int)App().document.audit.nulLines,
            App().document.audit.unparsed == 0 && App().document.audit.nulBytes == 0
                ? UiText(TextId::ui_0409) : UiText(TextId::ui_0410));
        coverage += L"\r\n" + U8ToW(rssiSummaryText(App().document.rssi));
        std::wstring evidence;
        if (g_expandAllFindings && App().document.platform.evidenceLine)
            evidence = FmtW(UiText(TextId::ui_0411), (int)App().document.platform.evidenceLine) +
                       U8ToW(App().document.platform.evidence);
        auto measureMeta = [&](const std::wstring& text) {
            HGDIOBJ old = SelectObject(hdc, App().hFontUI);
            RECT measured{0, 0, textW, S(1000)};
            DrawTextW(hdc, text.c_str(), -1, &measured,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            SelectObject(hdc, old);
            return std::max(S(20), static_cast<int>(measured.bottom));
        };
        bool jump = App().document.audit.clockJump;
        const std::wstring jumpTitle = jump ? FmtW(UiText(TextId::ui_0412), static_cast<int>(App().document.audit.jumpAtLine),
            U8ToW(fmtTime(App().document.audit.jumpFromT, "FULL")).c_str(), U8ToW(fmtTime(App().document.audit.jumpToT, "FULL")).c_str()) : L"";
        const std::wstring jumpDetail = UiText(TextId::ui_0413);
        const std::wstring scope = HasAnalysisTimeFilter() ?
            UiText(TextId::ui_0414) : L"";
        int h = CARD_PAD * 2 + measureMeta(platform) + measureMeta(coverage) +
                (evidence.empty() ? 0 : measureMeta(evidence)) + (jump ? measureMeta(jumpTitle) + measureMeta(jumpDetail) : 0) +
                (scope.empty() ? 0 : measureMeta(scope));
        drawCard(y, h, th::inkMuted);
        int ty = y + CARD_PAD;
        ty += DrawWrapped(hdc, textX0, ty, textW, platform, App().hFontUI, th::inkPri);
        if (!scope.empty()) ty += DrawWrapped(hdc, textX0, ty, textW, scope, App().hFontUI, th::inkSec);
        ty += DrawWrapped(hdc, textX0, ty, textW, coverage, App().hFontUI,
                          App().document.audit.unparsed == 0 && App().document.audit.nulBytes == 0
                              ? th::inkSec : th::rowWarn);
        if (!evidence.empty()) {
            const int evidenceH = DrawWrapped(hdc, textX0, ty, textW, evidence,
                                              App().hFontUI, th::accent);
            g_evidenceHits.push_back(EvidenceHit{RECT{textX0, ty, textX0 + textW, ty + evidenceH},
                                                 App().document.platform.evidenceLine, evidence});
            ty += evidenceH;
        }
        if (jump) {
            ty += DrawWrapped(hdc, textX0, ty, textW, jumpTitle, App().hFontUI, th::inkPri);
            DrawWrapped(hdc, textX0, ty, textW, jumpDetail, App().hFontUI, th::inkSec);
        }
        y += h + GAP;
    }

    if (App().document.findings.empty()) {
        const std::wstring description = UiText(TextId::ui_0415);
        HGDIOBJ old = SelectObject(hdc, App().hFontUI);
        RECT measure{0, 0, textW, 10000};
        DrawTextW(hdc, description.c_str(), -1, &measure, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(hdc, old);
        int h = CARD_PAD * 2 + S(26) + measure.bottom;
        drawCard(y, h, th::inkMuted);
        DrawText_(hdc, textX0, y + CARD_PAD, UiText(TextId::ui_0416), App().hFontSect, th::inkPri);
        DrawWrapped(hdc, textX0, y + CARD_PAD + S(26), textW, description, App().hFontUI, th::inkSec);
        y += h + GAP;
    }

    y+=DrawWrapped(hdc,textX0,y,textW,UiText(TextId::finding_keys),App().hFontSmall,th::inkMuted)+GAP;

    // ── 每条结论一张卡(先测高度,再画白底,最后画字)──
    int n = 0;
    for (const auto& f : App().document.findings) {
        COLORREF band = (f.severity == 2) ? th::rowFault : (f.severity == 1 ? th::rowWarn : th::rowState);
        const wchar_t* lv = (f.severity == 2) ? UiText(TextId::ui_0115) : (f.severity == 1 ? UiText(TextId::ui_0116) : UiText(TextId::ui_0117));
        ++n;
        const bool expanded=g_expandAllFindings!=static_cast<bool>(g_expandedFindings.count(n-1));
        std::wstring title = FmtW(L"%d. ", n) + U8ToW(findingDisplayTitle(WToU8(AnalysisScopedText(f.title))));
        const std::string detailText=WToU8(AnalysisScopedText(f.detail));
        const std::string adviceText=GeneratedText(f.advice);
        std::wstring detail=U8ToW(expanded?detailText:findingPreview(detailText,IsEnglish()?320:110));
        std::wstring advice=U8ToW(expanded?adviceText:findingPreview(adviceText,IsEnglish()?240:85));
        std::vector<std::wstring> evidence;
        evidence.reserve(f.ev.size());
        if(expanded) for (const auto& e : f.ev)
            evidence.push_back(FmtW(UiText(TextId::ui_0417), (int)e.lineNo) + U8ToW(e.ts) + L"  " + U8ToW(e.text));

        // —— 测量 pass:算这张卡多高(不画,只用 DT_CALCRECT)——
        auto measureWrap = [&](const std::wstring& s, HFONT font, int width = 0) {
            HGDIOBJ of = SelectObject(hdc, font);
            RECT r{ 0, 0, width ? width : textW, 10000 };
            DrawTextW(hdc, s.c_str(), (int)s.size(), &r, DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX|DT_CALCRECT);
            SelectObject(hdc, of);
            return (int)(r.bottom - r.top);
        };
        const std::wstring badge = lv;
        const int badgeW = TextW_(hdc, badge, App().hFontSmall) + S(30);
        const int titleWidth = std::max(S(80), textW - badgeW - S(120));
        HGDIOBJ titleFont = SelectObject(hdc, App().hFontSect);
        RECT measuredTitle{0, 0, titleWidth, 10000};
        DrawTextW(hdc, title.c_str(), -1, &measuredTitle, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(hdc, titleFont);
        int titleH = std::max(S(28), static_cast<int>(measuredTitle.bottom)), lblH = S(20);
        const int SEC = S(12);
        int h = CARD_PAD;                       // 顶内边距
        h += titleH + S(8);                     // 标题
        h += lblH + measureWrap(detail, App().hFontUI) + SEC;   // 依据
        h += lblH + measureWrap(advice, App().hFontUI) + SEC;   // 建议
        if(expanded) h += lblH;
        for (const auto& line : evidence) h += measureWrap(line, App().hFontMono, textW - S(8)) + S(5);
        h += S(32);
        h += CARD_PAD;                          // 底内边距

        // —— 画 pass:白底卡 + 色带,再叠字 ——
        drawCard(y, h, band);
        if(g_findingKeyboardFocus && GetFocus()==hwnd && g_focusedFinding==static_cast<std::size_t>(n-1)) {
            HPEN pen=CreatePen(PS_SOLID,S(2),th::accent);HGDIOBJ oldPen=SelectObject(hdc,pen),oldBrush=SelectObject(hdc,GetStockObject(NULL_BRUSH));
            Rectangle(hdc,M+S(2),y+S(2),M+cardW-S(2),y+h-S(2));
            SelectObject(hdc,oldBrush);SelectObject(hdc,oldPen);DeleteObject(pen);
        }
        int ty = y + CARD_PAD;
        DrawPill(hdc, textX0, ty, badge,
                 f.severity == 2 ? th::outageBand : (f.severity == 1 ? th::cellWeak : th::accentSoft),
                 band);
        DrawWrapped(hdc, textX0 + badgeW, ty + S(2), titleWidth, title, App().hFontSect, th::inkPri);
        DrawPill(hdc, M + cardW - S(120), ty, UiText(TextId::ui_0418), th::accentSoft, th::accent);
        g_findingCopyHits.push_back(CopyHit{RECT{M, y, M + cardW, y + h}, FindingText(n - 1)});
        ty += titleH + S(8);
        DrawText_(hdc, textX0, ty, UiText(expanded?TextId::ui_0192:TextId::ui_0545), App().hFontUI, th::inkMuted); ty += lblH;
        ty += DrawWrapped(hdc, textX0, ty, textW, detail, App().hFontUI, th::inkPri) + SEC;
        DrawText_(hdc, textX0, ty, UiText(expanded?TextId::ui_0419:TextId::ui_0546), App().hFontUI, th::inkMuted); ty += lblH;
        ty += DrawWrapped(hdc, textX0, ty, textW, advice, App().hFontUI, th::inkSec) + SEC;
        if(expanded) {DrawText_(hdc, textX0, ty, UiText(TextId::ui_0420), App().hFontUI, th::inkMuted); ty += lblH;}
        for (size_t evidenceIndex = 0; evidenceIndex < evidence.size(); ++evidenceIndex) {
            const int evidenceH = DrawWrapped(hdc, textX0 + S(8), ty, textW - S(8),
                                              evidence[evidenceIndex], App().hFontMono, th::accent);
            g_evidenceHits.push_back(EvidenceHit{
                RECT{textX0 + S(8), ty, textX0 + textW, ty + evidenceH},
                f.ev[evidenceIndex].lineNo, evidence[evidenceIndex]});
            ty += evidenceH + S(5);
        }
        const auto toggleText=expanded?std::wstring(UiText(TextId::ui_0544)):
            FmtW(UiText(TextId::ui_0543),static_cast<int>(f.ev.size()));
        RECT toggle{textX0,ty,textX0+TextW_(hdc,toggleText,App().hFontSmall)+S(24),ty+S(27)};
        DrawPill(hdc,toggle.left,toggle.top,toggleText,th::accentSoft,th::accent);
        g_findingExpandHits.push_back(FindingExpandHit{toggle,static_cast<std::size_t>(n-1)});
        const int selectWidth=TextW_(hdc,UiText(TextId::text_select),App().hFontSmall)+S(24);
        RECT select{M+cardW-CARD_PAD-selectWidth,ty,M+cardW-CARD_PAD,ty+S(27)};
        DrawPill(hdc,select.left,select.top,UiText(TextId::text_select),th::accentSoft,th::accent);
        g_selectTextHits.push_back({select,static_cast<std::size_t>(n-1)});
        y += h + GAP;
    }

    g_findContentH = y + g_findScroll;   // 记录总高(去掉本帧偏移)
    // 更新滚动范围
    SCROLLINFO si{}; si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0; si.nMax = std::max(0, g_findContentH); si.nPage = rc.bottom; si.nPos = g_findScroll;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    ClampPageScroll(hwnd, g_findScroll);

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
    if (HandleCopyMenu(hwnd, msg, lp, g_summaryCopyHits, 0)) return 0;
    if (msg == WM_LBUTTONUP) {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (const auto& hit : g_summaryCopyHits) {
            RECT button{hit.rect.right - S(102), hit.rect.top + S(14), hit.rect.right - S(18), hit.rect.top + S(44)};
            if (PtInRect(&button, point)) { CopyNotice(hit.text); return 0; }
        }
    }
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
            case SB_THUMBTRACK: case SB_THUMBPOSITION: g_sumScroll = ScrollThumb(hwnd); break;
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

    g_summaryCopyHits.clear();
    for (const auto& c : g_sumCards) {
        COLORREF band = (c.accent == 2) ? th::rowFault : (c.accent == 1 ? th::rowWarn : th::inkMuted);
        HFONT lineFont = c.mono ? App().hFontMono : App().hFontUI;
        HGDIOBJ oldTitle = SelectObject(hdc, App().hFontSect);
        RECT titleMeasure{0, 0, std::max(S(80), textW - S(92)), 10000};
        DrawTextW(hdc, c.title.c_str(), -1, &titleMeasure, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(hdc, oldTitle);
        int titleH = std::max(S(24), static_cast<int>(titleMeasure.bottom));
        auto measure = [&](const std::wstring& line) {
            HGDIOBJ old = SelectObject(hdc, lineFont);
            RECT measured{0, 0, textW, S(1000)};
            DrawTextW(hdc, line.c_str(), -1, &measured,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
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
        DrawWrapped(hdc, textX0, ty, std::max(S(80), textW - S(92)), c.title, App().hFontSect, th::inkPri);
        DrawPill(hdc, cr.right - S(94), ty - S(2), UiText(TextId::ui_0421), th::accentSoft, th::accent);
        g_summaryCopyHits.push_back(CopyHit{cr, CardText(c)});
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
    ClampPageScroll(hwnd, g_sumScroll);

    BitBlt(hw, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, obm); DeleteObject(bmp); DeleteDC(hdc);
    EndPaint(hwnd, &ps);
    return 0;
}


void RenderSummary() {
    BuildDashboardStats();
    g_summaryCopyHits.clear();
    g_sumCards.clear();
    g_sumScroll = 0;
    if (App().hSummary) { SetScrollPos(App().hSummary, SB_VERT, 0, TRUE); }
    if (App().document.filtered.empty()) {
        if (!App().document.lines.empty())      // 未加载日志时不显示卡片(上方已有提示)
            g_sumCards.push_back({ UiText(TextId::ui_0422), { UiText(TextId::ui_0423) }, 0, false });
        if (App().hSummary) InvalidateRect(App().hSummary, nullptr, FALSE);
        return;
    }

    auto add = [&](std::wstring title, std::vector<std::wstring> lines, int accent = 0, bool mono = false) {
        g_sumCards.push_back({ std::move(title), std::move(lines), accent, mono });
    };

    {
        const auto observation = observationStats(App().document.filtered);
        const auto availability = availabilityStats(App().document.filtered, App().document.outages);
        std::vector<std::wstring> facts;
        facts.push_back(g_dashboard.startText + L" → " + g_dashboard.endText);
        facts.push_back(UiText(TextId::ui_0364) + U8ToW(GeneratedText(App().document.platform.name)));
        facts.push_back(availability.runtimeValid() && !availability.evidenceLimited()
            ? FmtW(UiText(TextId::ui_0424), availability.runtimePercent())
            : UiText(TextId::ui_0425));
        if (availability.evidenceLimited()) facts.push_back(UiText(TextId::ui_0426));
        else if (availability.fullValid()) {
            facts.push_back(FmtW(UiText(TextId::ui_0427),
                availability.fullPercent(), U8ToW(fmtDur(availability.longestStartupSeconds)).c_str(),
                availability.terminalOutages ? UiText(TextId::ui_0428) : L""));
            if (availability.neverConnectedStartupSegments) facts.push_back(FmtW(UiText(TextId::ui_0429), static_cast<int>(availability.neverConnectedStartupSegments)));
        } else facts.push_back(UiText(TextId::ui_0430));
        facts.push_back(FmtW(UiText(TextId::ui_0431),
            U8ToW(fmtDur(observation.observedSpan)).c_str(), U8ToW(fmtDur(observation.calendarSpan)).c_str(), observation.coveragePercent));
        if (observation.clockDiscontinuities) facts.push_back(FmtW(UiText(TextId::ui_0432), static_cast<int>(observation.clockDiscontinuities)));
        facts.push_back(FmtW(UiText(TextId::ui_0433),
            static_cast<int>(App().document.audit.unparsed), App().document.audit.unparsedRatio() * 100.0,
            static_cast<int>(App().document.audit.nulBytes), static_cast<int>(App().document.audit.nulLines)));
        add(UiText(TextId::ui_0434), facts);
    }

    // ── 概览 ──
    {
        std::vector<std::wstring> ls;
        ls.push_back(FmtW(UiText(TextId::ui_0435),
                          (int)App().document.filtered.size(), (int)App().document.audit.logOpened,
                          (int)App().document.sessions.size()));
        int acc = 0;
        if (App().document.sessions.size() > 1) {
            ls.push_back(FmtW(UiText(TextId::ui_0436),
                              (int)App().document.sessions.size()));
            for (size_t i = 0; i < App().document.sessions.size() && i < 6; ++i)
                ls.push_back(L"   " + U8ToW(App().document.sessions[i]));
        }
        if (App().document.audit.logOpened > App().document.sessions.size()) {
            acc = 1;
            ls.push_back(UiText(TextId::ui_0437));
        }
        add(UiText(TextId::ui_0164), ls, acc);
    }

    if (App().document.sources.size() > 1) {
        std::vector<std::wstring> lines;
        lines.push_back(FmtW(UiText(TextId::ui_0438),
                             static_cast<int>(App().document.sources.size())));
        for (const auto& source : App().document.sources) {
            std::wstring label = source.label;
            const size_t slash = label.find_last_of(L"\\/");
            if (slash != std::wstring::npos) label = label.substr(slash + 1);
            lines.push_back(FmtW(UiText(TextId::ui_0439),
                                 label.c_str(), static_cast<int>(source.parsedLines),
                                 static_cast<int>(source.outages), static_cast<int>(source.metricRows),
                                 source.hasAverageRsrp
                                     ? FmtW(L"%d dBm", source.averageRsrp).c_str() : L"—"));
        }
        add(UiText(TextId::ui_0440), lines, 0, true);
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
    {
        std::vector<std::wstring> ls; int acc = 0;
        ls.push_back(FmtW(UiText(TextId::ui_0441),
            static_cast<int>(App().document.outages.size()), U8ToW(fmtDur(total)).c_str(), U8ToW(fmtDur(longest)).c_str()));
        ls.push_back(FmtW(UiText(TextId::ui_0442), b0, b1, b2, b3));
        if (longest > 0)
            ls.push_back(FmtW(UiText(TextId::ui_0443), U8ToW(fmtDur(longest)).c_str(), U8ToW(fmtTime(longestAt, "FULL")).c_str()));
        if (!App().document.outages.empty() && !App().document.outages.back().recovered) {
            acc = 2;
            ls.push_back(FmtW(UiText(TextId::ui_0479), U8ToW(fmtTime(App().document.outages.back().start, "FULL")).c_str()));
        }
        if (!ls.empty()) add(UiText(TextId::ui_0485), ls, acc);
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
        if (usesLteEngineeringReference(m.rat)) {
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
    if (csqN) {
        const int average = static_cast<int>(csqSum / csqN);
        std::vector<std::wstring> lines{
            UiText(TextId::ui_0444),
            FmtW(UiText(TextId::ui_0445), csqMin, static_cast<double>(csqSum) / csqN, csqMax, csqN,
                 U8ToW(GeneratedText(signalQualityName(csqQuality(average)))).c_str()),
            UiText(TextId::ui_0446)
        };
        if (weak)
            lines.push_back(FmtW(UiText(TextId::ui_0447), weak, csqN,
                                 U8ToW(fmtTime(weakFirst, "FULL")).c_str()));
        add(UiText(TextId::ui_0483), lines, weak ? 1 : 0);
    }
    // LTE 详情。SNR 是 SDK 原值(0.1dB);阈值仅作工程观察,不冒充协议定论。
    if (rsrpN || rsrqN || snrN) {
        std::vector<std::wstring> ls; int acc = 0;
        if (rsrpN) {
            int avg = (int)(rsrpSum / rsrpN);
            ls.push_back(FmtW(UiText(TextId::ui_0448),
                              rsrpMin, avg, rsrpMax,
                              U8ToW(GeneratedText(signalQualityName(rsrpQuality(avg)))).c_str()));
            ls.push_back(UiText(TextId::ui_0449));
            if (avg < -100 || rsrpMin < -110) acc = std::max(acc, 1);
        }
        if (rsrqN) {
            int avg = (int)(rsrqSum / rsrqN);
            ls.push_back(FmtW(UiText(TextId::ui_0450),
                              rsrqMin, avg, rsrqMax,
                              U8ToW(GeneratedText(signalQualityName(rsrqQuality(avg)))).c_str()));
            ls.push_back(UiText(TextId::ui_0451));
            if (avg < -20) acc = std::max(acc, 1);
        }
        if (snrN) {
            double avg = (double)snrSum10 / (10.0 * snrN);
            const int avg10 = static_cast<int>(snrSum10 / snrN);
            ls.push_back(FmtW(UiText(TextId::ui_0452),
                              snrMin / 10.0, avg, snrMax / 10.0,
                              U8ToW(GeneratedText(signalQualityName(snrQuality10(avg10)))).c_str(),
                              snrNonPositive, snrN));
            ls.push_back(UiText(TextId::ui_0453));
            if (snrN >= 5 && snrNonPositive * 2 >= snrN) acc = std::max(acc, 1);
        }
        add(UiText(TextId::ui_0454), ls, acc);
    }
    add(L"RSSI", {U8ToW(rssiSummaryText(App().document.rssi)), UiText(TextId::rssi_note)});
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
        if (denyN) ls.push_back(FmtW(UiText(TextId::ui_0455), denyNonzero, denyN));
        if (!opers.empty()) ls.push_back(L"OPER " + dist(opers));
        add(UiText(TextId::ui_0456), ls, denyNonzero ? 1 : 0);
    }
    if (tN) {
        std::wstring s = FmtW(L"max=%d°C   avg=%.0f°C", tMax, (double)tSum / tN);
        if (hot) s += FmtW(UiText(TextId::ui_0457), hot);
        add(UiText(TextId::ui_0458), { s }, hot ? 1 : 0);
    }
    if (!chans.empty()) {
        int tot = 0; for (auto& kv : chans) tot += kv.second;
        std::wstring s;
        for (auto& kv : chans) s += FmtW(L"%s:%.0f%%    ", U8ToW(kv.first).c_str(), 100.0 * kv.second / tot);
        add(UiText(TextId::ui_0459), { s });
    }
    if (!cells.empty()) {
        std::vector<std::pair<std::string, int>> ranked(cells.begin(), cells.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
            return left.second != right.second ? left.second > right.second : left.first < right.first;
        });
        int totalSamples = 0;
        for (const auto& cell : ranked) totalSamples += cell.second;
        std::vector<std::wstring> ls;
        ls.push_back(FmtW(UiText(TextId::ui_0460), (int)ranked.size(), totalSamples));
        for (size_t index = 0; index < ranked.size() && index < 6; ++index)
            ls.push_back(FmtW(UiText(TextId::ui_0461), U8ToW(ranked[index].first).c_str(),
                              ranked[index].second, 100.0 * ranked[index].second / totalSamples));
        if (ranked.size() > 6) ls.push_back(FmtW(UiText(TextId::ui_0462), (int)ranked.size() - 6));
        add(UiText(TextId::ui_0463), ls, ranked.size() > 12 ? 1 : 0, true);
    }

    // RX 停滞
    auto stalls = detectRxStall(rxs);
    if (!stalls.empty()) {
        std::vector<std::wstring> ls;
        for (size_t i = 0; i < stalls.size() && i < 6; ++i)
            ls.push_back(FmtW(UiText(TextId::ui_0464), U8ToW(fmtDur(stalls[i].dur)).c_str(),
                         U8ToW(fmtTime(stalls[i].start, "FULL")).c_str(), U8ToW(fmtTime(stalls[i].end, "FULL")).c_str()));
        if (stalls.size() > 6) ls.push_back(FmtW(UiText(TextId::ui_0482), (int)stalls.size() - 6));
        add(UiText(TextId::ui_0465), ls, 1, true);
    }

    // 报错
    std::vector<const LogLine*> errs;
    for (const LogLine* l : App().document.filtered) if (isErrLine(*l)) errs.push_back(l);
    if (!errs.empty()) {
        std::vector<std::wstring> ls;
        for (size_t i = 0; i < errs.size() && i < 12; ++i)
            ls.push_back(FmtW(L"%s  [%s] %s", U8ToW(errs[i]->ts).c_str(),
                         U8ToW(errs[i]->tagText()).c_str(), U8ToW(errs[i]->msg.substr(0, 90)).c_str()));
        if (errs.size() > 12) ls.push_back(FmtW(UiText(TextId::ui_0466), (int)errs.size() - 12));
        add(FmtW(UiText(TextId::ui_0467), (int)errs.size()), ls, 2, true);
    }

    // 关键事件计数
    int sw = 0, states = 0, cfun = 0, slot = 0, oper = 0, cellChanges = 0;
    const DataCallStats dataCalls = collectDataCallStats(App().document.filtered);
    for (const LogLine* item : App().document.filtered) {
        const LogLine& l = *item;
        if (l.msg.find("switching to SIM") != std::string::npos ||
            l.msg.find("switching to Roamlink") != std::string::npos) sw++;
        if (l.tagText() == "STATE") states++;
        if (l.tagText() == "CFUN" || l.msg.find("CFUN=0") != std::string::npos ||
            l.msg.find("CFUN toggle") != std::string::npos) cfun++;
        if (l.tagText() == "SLOT") slot++;
        if (l.tagText() == "OPER") oper++;
        if (l.tagText().compare(0, 4, "CELL") == 0) cellChanges++;
    }
    std::vector<std::wstring> eventLines = {
          FmtW(UiText(TextId::ui_0487),
               sw, (int)dataCalls.disconnected, states, cfun, slot, oper, cellChanges),
          FmtW(UiText(TextId::ui_0468),
               (int)dataCalls.appStop, (int)dataCalls.unsolicited,
               (int)dataCalls.legacy, (int)dataCalls.stopRequested),
          L"",
          UiText(TextId::ui_0469)
    };
    if (!dataCalls.reasons.empty()) {
        std::wstring reasons = UiText(TextId::ui_0470);
        bool firstReason = true;
        for (const auto& entry : dataCalls.reasons) {
            if (!firstReason) reasons += L"   ";
            firstReason = false;
            reasons += U8ToW(entry.first) + L":" + std::to_wstring(entry.second);
        }
        eventLines.insert(eventLines.begin() + 2, std::move(reasons));
    }
    add(UiText(TextId::ui_0471), eventLines);

    if (App().hSummary) InvalidateRect(App().hSummary, nullptr, FALSE);
}


void RenderFindings() {
    g_findingCopyHits.clear(); g_evidenceHits.clear();
    g_findingExpandHits.clear(); g_selectTextHits.clear(); g_findExpandAllRect=RECT{};
    g_expandedFindings.clear();
    g_findScroll = 0;
    g_focusedFinding=0;g_findingKeyboardFocus=false;
    if (App().hFindings) {
        SetScrollPos(App().hFindings, SB_VERT, 0, TRUE);
        InvalidateRect(App().hFindings, nullptr, FALSE);
    }
}

// “未识别行”页:把解析不了的行摆出来,这是“完完整整不漏消息”的唯一硬证据

void ReleaseOverviewPageData() {
    releaseVector(g_sumCards);
    releaseVector(g_summaryCopyHits);
    releaseVector(g_findingCopyHits);
    releaseVector(g_findingExpandHits);releaseVector(g_selectTextHits); g_expandedFindings.clear();g_expandAllFindings=false;
    g_findExpandAllRect=RECT{};
    releaseVector(g_evidenceHits);
    g_dashCopyRect = g_findCopyRect = RECT{};
    for (RECT& rect : g_dashTileRects) rect = RECT{};
    g_dashboard = DashboardStats{};
    g_findScroll = g_findContentH = 0;
    g_sumScroll = g_sumContentH = 0;
}

} // namespace dl
