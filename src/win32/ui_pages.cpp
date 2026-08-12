// ui_pages.cpp — 页签渲染、自绘视图与虚拟列表适配
#include "ui_pages.h"

#include <commctrl.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "app_context.h"
#include "chartmodel.h"
#include "log_analysis.h"
#include "log_time.h"
#include "memoryutil.h"
#include "modern_shell.h"
#include "tablemodel.h"
#include "theme.h"
#include "win_text.h"

namespace dl {

static std::vector<COLORREF> g_ogColors;
static int g_curPage = 0;
static bool g_pageDirty[8] = { true, true, true, true, true, true, true, true };
static size_t g_rawTargetLine = 0;
static LONG g_rawSelectionStart = 0;
static LONG g_rawSelectionEnd = 0;

void LvAddCol(HWND lv, int i, const wchar_t* text, int w) {
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
        l.tagText().compare(0, 8, "RECOVERY") == 0)
        return th::rowRecovered;
    // 故障(红):断网引擎认的 + fault 快照 + 硬告警
    if (isFaultStart(m) || has("_fault_") || has("Net Fail Duration"))
        return th::rowFault;

    std::string t = l.tagText();
    if (t == "ERROR" || t == "FATAL" || t == "CFUN") return th::rowErr;
    if (t == "WARN" || t == "WARNING" || t == "ALARM" || t == "SLOT" || t == "OPER") return th::rowWarn;
    if (t == "ROAMLINK") return th::rowRoamlink;
    if (t == "STATE")    return th::rowState;
    if (t == "SDK")      return th::rowSdk;
    return th::inkPri;
}

static void RenderTimeline() {
    buildTimelineView(App().document.filtered, App().document.timelineRows);
    ListView_SetItemCountEx(App().hTimeline, (int)App().document.timelineRows.size(),
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(App().hTimeline, nullptr, TRUE);
}

static void RenderOutages() {
    ListView_DeleteAllItems(App().hOutage);
    g_ogColors.clear();
    int row = 0;
    for (const auto& o : App().document.outages) {
        LvAddRow(App().hOutage, row, FmtW(L"%d", row + 1));
        LvSet(App().hOutage, row, 1, U8ToW(fmtTime(o.start, "MD")));
        if (o.recovered) {
            LvSet(App().hOutage, row, 2, U8ToW(fmtTime(o.end, "MD")));
            LvSet(App().hOutage, row, 3, U8ToW(fmtDur(o.dur)));
            g_ogColors.push_back(o.dur > 60 ? th::critical : (o.dur > 30 ? th::rowWarn : th::inkPri));
        } else {
            LvSet(App().hOutage, row, 2, L"未恢复");
            LvSet(App().hOutage, row, 3, L"?");
            g_ogColors.push_back(th::critical);
        }
        row++;
    }
}

static void RenderTags() {
    ListView_DeleteAllItems(App().hTags);
    std::map<std::string, int> tc;
    for (const LogLine* l : App().document.filtered) {
        const std::string& tag = l->tagText();
        tc[tag.empty() ? "(无标签)" : tag]++;
    }
    int mx = 1;
    for (auto& kv : tc) mx = std::max(mx, kv.second);
    std::vector<std::pair<std::string,int>> v(tc.begin(), tc.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    int row = 0;
    for (auto& kv : v) {
        LvAddRow(App().hTags, row, U8ToW(kv.first));
        LvSet(App().hTags, row, 1, FmtW(L"%d", kv.second));
        LvSet(App().hTags, row, 2, std::wstring((size_t)std::max(0, kv.second * 50 / mx), L'█'));
        row++;
    }
}

static void AppendRawLine(std::string& output, const LogLine& line) {
    output += std::to_string(line.lineNo);
    output += "  ";
    output += line.ts;
    const std::string& level = line.levelText();
    const std::string& tag = line.tagText();
    if (line.fmt == FMT_SEAS && !level.empty()) { output += " ["; output += level; output += "]"; }
    if (!tag.empty())                             { output += " ["; output += tag; output += "]"; }
    output += " "; output += line.msg; output += "\r\n";
}

static void RenderRaw() {
    if (g_rawTargetLine) {
        const auto found = std::find_if(App().document.lines.begin(), App().document.lines.end(),
            [](const LogLine& line) { return line.lineNo == g_rawTargetLine; });
        if (found != App().document.lines.end()) {
            const size_t index = static_cast<size_t>(found - App().document.lines.begin());
            const size_t first = index > 40 ? index - 40 : 0;
            const size_t last = std::min(App().document.lines.size(), index + 41);
            std::wstring text = FmtW(L"已定位到原始第 %d 行 · 显示前后上下文\r\n\r\n",
                                     static_cast<int>(g_rawTargetLine));
            g_rawSelectionStart = g_rawSelectionEnd = 0;
            for (size_t i = first; i < last; ++i) {
                std::string narrow;
                AppendRawLine(narrow, App().document.lines[i]);
                std::wstring line = U8ToW(narrow);
                if (i == index) g_rawSelectionStart = static_cast<LONG>(text.size());
                text += line;
                if (i == index) g_rawSelectionEnd = static_cast<LONG>(text.size());
            }
            SetWindowTextW(App().hRaw, text.c_str());
            SendMessageW(App().hRaw, EM_SETSEL, g_rawSelectionStart, g_rawSelectionEnd);
            SendMessageW(App().hRaw, EM_SCROLLCARET, 0, 0);
            return;
        }
        g_rawTargetLine = 0;
    }

    const size_t CAP = 5000;
    std::string s;
    s.reserve(256 * 1024);
    size_t n = 0;
    for (const LogLine* item : App().document.filtered) {
        AppendRawLine(s, *item);
        if (++n >= CAP) { s += "\r\n… 已截断,仅显示前 5000 行(用筛选缩小范围)\r\n"; break; }
    }
    SetWindowTextW(App().hRaw, U8ToW(s).c_str());
}

static void RenderUnparsed() {
    ListView_DeleteAllItems(App().hUnparsed);
    int row = 0;
    for (const auto& u : App().document.audit.samples) {
        LvAddRow(App().hUnparsed, row, FmtW(L"%d", (int)u.lineNo));
        LvSet(App().hUnparsed, row, 1, U8ToW(u.text.substr(0, 300)));
        row++;
    }
}

void MarkAllPagesDirty() {
    g_rawTargetLine = 0;
    std::fill(std::begin(g_pageDirty), std::end(g_pageDirty), true);
    RefreshNavigation();
}

// OWNERDATA 控件只保存行数,数据仍属于下列 C++ 模型。模型即将重建/释放时必须先把
// 行数归零,避免控件在重绘通知中索引已经失效的 App().document.metrics / App().document.timelineRows。
void ResetVirtualTables() {
    if (App().hTimeline) ListView_SetItemCountEx(App().hTimeline, 0, LVSICF_NOSCROLL);
    if (App().hMetric)   ListView_SetItemCountEx(App().hMetric,   0, LVSICF_NOSCROLL);
    App().document.timelineRows.clear();
}

// 关闭/替换日志的冷路径:不仅清空元素,还释放所有大 vector 的 capacity。
// 顺序是安全边界的一部分:OWNERDATA 行数和两个借用视图必须先失效,最后才能释放
// App().document.lines。普通筛选仍使用 clear()/赋值复用容量,避免每次点击“应用”都重新分配。
void ReleaseLoadedData() {
    g_rawTargetLine = 0;
    g_rawSelectionStart = g_rawSelectionEnd = 0;
    ResetVirtualTables();

    // 非 OWNERDATA 控件自己持有单元格文本；替换日志前也立即丢掉旧内容，避免隐藏页
    // 一直保留到用户下次切换页签才释放。
    if (App().hOutage)   ListView_DeleteAllItems(App().hOutage);
    if (App().hTags)     ListView_DeleteAllItems(App().hTags);
    if (App().hUnparsed) ListView_DeleteAllItems(App().hUnparsed);
    if (App().hRaw)      SetWindowTextW(App().hRaw, L"");

    App().document.release();

    releaseVector(g_ogColors);
    ReleaseOverviewPageData();
    ReleaseChartPageData();
}

// 数据更新时只刷新当前页；其它页保留脏标记,用户首次切过去时再生成控件内容。
// 时间线/指标还使用 OWNERDATA 虚拟表,这里只设置行数,滚动到可见单元格时才转 UTF-16。
void RenderPage(int page) {
    if (page < 0 || page >= 8 || !g_pageDirty[page]) return;
    switch (page) {
    case 0: RenderSummary(); InvalidateRect(App().hDash, nullptr, TRUE); break;
    case 1: RenderFindings(); break;
    case 2: RenderTimeline(); break;
    case 3: RenderOutages(); break;
    case 4: RenderMetrics(); break;
    case 5: RenderTags(); break;
    case 6: RenderRaw(); break;
    case 7: RenderUnparsed(); break;
    }
    g_pageDirty[page] = false;
}

void ShowPage(int page) {
    // 页序:0总览 1结论 2时间线 3断网 4指标 5标签 6原始行 7未识别行
    if (page < 0 || page >= 8) return;
    g_curPage = page;
    const wchar_t* titles[] = {L"概览", L"诊断结论", L"事件时间线", L"断网记录",
                               L"信号指标", L"标签统计", L"原始日志", L"未识别行"};
    if (App().hPageTitle) SetWindowTextW(App().hPageTitle, titles[page]);
    SetNavigationPage(page);
    RenderPage(page);   // 页仍隐藏时填充,减少 ListView 大批插入时的可见闪烁
    struct { HWND* h; int page; } items[] = {
        { &App().hDash, 0 }, { &App().hSummary, 0 }, { &App().hFindings, 1 }, { &App().hTimeline, 2 }, { &App().hOutage, 3 },
        { &App().hChart, 4 }, { &App().hMetric, 4 },
        { &App().hTags, 5 }, { &App().hRaw, 6 }, { &App().hUnparsed, 7 },
    };
    for (auto& it : items) {
        bool on = (it.page == page);
        ShowWindow(*it.h, on ? SW_SHOW : SW_HIDE);
        if (on) SetWindowPos(*it.h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    ShowWindow(App().hExport, page == 4 ? SW_SHOW : SW_HIDE);
    SendMessageW(App().hMain, WM_APP_SHELL_LAYOUT, 0, 0);
}


int CurrentPage() {
    return g_curPage;
}

void JumpToRawLine(size_t lineNo) {
    if (!lineNo || App().document.lines.empty()) return;
    const auto found = std::find_if(App().document.lines.begin(), App().document.lines.end(),
        [lineNo](const LogLine& line) { return line.lineNo == lineNo; });
    if (found == App().document.lines.end()) {
        ShowModernNotice(L"无法定位该证据", L"对应原始行未被解析，可在“未识别行”页面复核。",
                         ModernNoticeKind::Warning);
        return;
    }
    g_rawTargetLine = lineNo;
    g_pageDirty[6] = true;
    ShowPage(6);
    SendMessageW(App().hRaw, EM_SETSEL, g_rawSelectionStart, g_rawSelectionEnd);
    SendMessageW(App().hRaw, EM_SCROLLCARET, 0, 0);
    SetFocus(App().hRaw);
    ShowModernNotice(L"已定位原始证据",
                     FmtW(L"第 %d 行已选中；按 Ctrl+C 可直接复制。", static_cast<int>(lineNo)).c_str(),
                     ModernNoticeKind::Info, 4500);
}

static void DrawListEmptyState(HWND list, HDC dc) {
    const bool loaded = !App().document.lines.empty();
    const wchar_t* title = loaded ? L"当前范围内没有数据" : L"还没有加载日志";
    const wchar_t* detail = loaded ? L"调整筛选条件，或切换到其他页面查看。"
                                    : L"拖入日志文件，或使用右上角“打开日志”。";
    bool success = false;
    if (list == App().hOutage && loaded) {
        title = L"没有发现断网记录"; detail = L"当前筛选范围内未检测到完整的断网事件。"; success = true;
    } else if (list == App().hMetric && loaded) {
        title = L"没有信号指标"; detail = L"当前日志中没有可绘制的心跳或信号采样。";
    } else if (list == App().hTimeline && loaded) {
        title = L"没有关键事件"; detail = L"当前范围内没有状态迁移、切换或恢复事件。"; success = true;
    } else if (list == App().hTags && loaded) {
        title = L"没有标签统计"; detail = L"当前筛选结果为空，请尝试清空筛选条件。";
    } else if (list == App().hUnparsed && loaded) {
        title = L"全部日志均已识别"; detail = L"解析覆盖完整，没有需要人工复核的原始行。"; success = true;
    }

    RECT client{}; GetClientRect(list, &client);
    if (HWND header = ListView_GetHeader(list)) {
        RECT hr{}; GetWindowRect(header, &hr);
        client.top += hr.bottom - hr.top;
    }
    const int width = std::min(S(440), std::max(S(260), static_cast<int>(client.right) - S(64)));
    const int height = S(126);
    const int x = (client.right - width) / 2;
    const int y = client.top + std::max(S(24), (static_cast<int>(client.bottom - client.top) - height) / 2);
    RECT card{x, y, x + width, y + height};
    FillRound(dc, card, S(12), th::page, th::border);
    const COLORREF tone = success ? th::good : th::accent;
    RECT icon{x + S(22), y + S(31), x + S(66), y + S(75)};
    FillRound(dc, icon, S(22), th::accentSoft, th::accentSoft);
    HGDIOBJ oldFont = SelectObject(dc, App().hFontSect);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, tone);
    DrawTextW(dc, success ? L"✓" : L"…", -1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT titleRect{x + S(82), y + S(26), card.right - S(18), y + S(53)};
    SelectObject(dc, App().hFontSect); SetTextColor(dc, th::inkPri);
    DrawTextW(dc, title, -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT detailRect{x + S(82), y + S(57), card.right - S(18), y + S(100)};
    SelectObject(dc, App().hFontUI); SetTextColor(dc, th::inkSec);
    DrawTextW(dc, detail, -1, &detailRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    SelectObject(dc, oldFont);
}

bool HandlePageNotify(LPARAM lparam, LRESULT& result) {
    LPNMHDR hdr = reinterpret_cast<LPNMHDR>(lparam);
    if (hdr->code == LVN_GETDISPINFOW &&
        (hdr->hwndFrom == App().hTimeline || hdr->hwndFrom == App().hMetric)) {
        NMLVDISPINFOW* di = reinterpret_cast<NMLVDISPINFOW*>(lparam);
        if ((di->item.mask & LVIF_TEXT) && di->item.pszText && di->item.cchTextMax > 0 &&
            di->item.iItem >= 0 && di->item.iSubItem >= 0) {
            const size_t row = static_cast<size_t>(di->item.iItem);
            const size_t col = static_cast<size_t>(di->item.iSubItem);
            std::string text;
            if (hdr->hwndFrom == App().hTimeline && row < App().document.timelineRows.size() &&
                col < kTimelineColumnCount) {
                text = timelineCellText(*App().document.timelineRows[row], col);
            } else if (hdr->hwndFrom == App().hMetric && row < App().document.metrics.size() &&
                       col < kMetricColumnCount) {
                text = metricCellText(App().document.metrics[row], col);
            }
            const std::wstring wide = U8ToW(text);
            lstrcpynW(di->item.pszText, wide.c_str(), di->item.cchTextMax);
        }
        result = 0;
        return true;
    }

    if (hdr->code != NM_CUSTOMDRAW ||
        (hdr->hwndFrom != App().hTimeline && hdr->hwndFrom != App().hOutage &&
         hdr->hwndFrom != App().hMetric && hdr->hwndFrom != App().hTags &&
         hdr->hwndFrom != App().hUnparsed)) {
        return false;
    }

    LPNMLVCUSTOMDRAW draw = reinterpret_cast<LPNMLVCUSTOMDRAW>(lparam);
    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
        result = CDRF_NOTIFYITEMDRAW;
        if (ListView_GetItemCount(hdr->hwndFrom) == 0) result |= CDRF_NOTIFYPOSTPAINT;
        return true;
    }
    if (draw->nmcd.dwDrawStage == CDDS_POSTPAINT && ListView_GetItemCount(hdr->hwndFrom) == 0) {
        DrawListEmptyState(hdr->hwndFrom, draw->nmcd.hdc);
        result = CDRF_DODEFAULT;
        return true;
    }
    if (hdr->hwndFrom == App().hTimeline || hdr->hwndFrom == App().hOutage) {
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            const size_t row = static_cast<size_t>(draw->nmcd.dwItemSpec);
            if (hdr->hwndFrom == App().hTimeline) {
                if (row < App().document.timelineRows.size())
                    draw->clrText = RowColor(*App().document.timelineRows[row]);
            } else if (row < g_ogColors.size()) {
                draw->clrText = g_ogColors[row];
            }
            draw->clrTextBk = (row & 1) ? th::zebra : th::surface;
        }
        result = CDRF_DODEFAULT;
        return true;
    }

    if (hdr->hwndFrom == App().hTags || hdr->hwndFrom == App().hUnparsed) {
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            const size_t row = static_cast<size_t>(draw->nmcd.dwItemSpec);
            draw->clrText = th::inkPri;
            draw->clrTextBk = (row & 1) ? th::zebra : th::surface;
        }
        result = CDRF_DODEFAULT;
        return true;
    }

    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        result = CDRF_NOTIFYSUBITEMDRAW;
        return true;
    }
    if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
        const size_t row = static_cast<size_t>(draw->nmcd.dwItemSpec);
        const int column = draw->iSubItem;
        draw->clrText = th::inkPri;
        draw->clrTextBk = (row & 1) ? th::zebra : th::surface;
        if (row < App().document.metrics.size()) {
            const MetricRow& metric = App().document.metrics[row];
            if (column == 6 && metric.drx == 0)
                draw->clrTextBk = th::cellStall;
            else if (column == 2 && metric.csqVal >= 0 && metric.csqVal < 10)
                draw->clrTextBk = th::cellWeak;
            else if (column == 9 && metric.snr10 != 100000 && metric.snr10 <= 0)
                draw->clrTextBk = th::cellSnrLow;
        }
    }
    result = CDRF_DODEFAULT;
    return true;
}

} // namespace dl
