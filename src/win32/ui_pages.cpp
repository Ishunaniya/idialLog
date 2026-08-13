// ui_pages.cpp — 页签渲染、自绘视图与虚拟列表适配
#include "ui_pages.h"

#include <commctrl.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <set>
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
static bool g_pageDirty[9] = { true, true, true, true, true, true, true, true, true };
static size_t g_rawTargetLine = 0;
static LogView g_rawContext;
static std::vector<const CellSummary*> g_cellRows;
static std::vector<std::size_t> g_outageOrder;
static std::vector<std::pair<std::string, int>> g_tagRows;
static std::vector<EvidenceBookmark> g_bookmarks;
static int g_metricSortColumn = -1, g_outageSortColumn = -1, g_tagSortColumn = 1, g_cellSortColumn = -1;
static bool g_metricSortAscending = true, g_outageSortAscending = true;
static bool g_tagSortAscending = false, g_cellSortAscending = true;

struct MetricQuickFilter {
    std::string cell, rat, ch;
    int deny = -1;
    bool hasDeny = false;
};
static MetricQuickFilter g_metricFilter;

static void RefreshBookmarkButton() {
    if (!App().hBookmarks) return;
    std::wstring label = L"书签";
    if (!g_bookmarks.empty()) label += L" (" + std::to_wstring(g_bookmarks.size()) + L")";
    SetWindowTextW(App().hBookmarks, label.c_str());
    EnableWindow(App().hBookmarks, !g_bookmarks.empty());
}

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

static void SetSortIndicator(HWND list, int sortedColumn, bool ascending) {
    HWND header = ListView_GetHeader(list);
    const int count = header ? Header_GetItemCount(header) : 0;
    for (int column = 0; column < count; ++column) {
        HDITEMW item{}; item.mask = HDI_FORMAT;
        if (!Header_GetItem(header, column, &item)) continue;
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (column == sortedColumn) item.fmt |= ascending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, column, &item);
    }
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
    g_outageOrder.resize(App().document.outages.size());
    for (std::size_t i = 0; i < g_outageOrder.size(); ++i) g_outageOrder[i] = i;
    if (g_outageSortColumn >= 0) {
        std::stable_sort(g_outageOrder.begin(), g_outageOrder.end(), [](std::size_t left, std::size_t right) {
            const Outage& a = App().document.outages[left];
            const Outage& b = App().document.outages[right];
            long long av = 0, bv = 0;
            if (g_outageSortColumn == 0) { av = static_cast<long long>(left); bv = static_cast<long long>(right); }
            else if (g_outageSortColumn == 1) { av = a.start; bv = b.start; }
            else if (g_outageSortColumn == 2) { av = a.recovered ? a.end : LLONG_MAX; bv = b.recovered ? b.end : LLONG_MAX; }
            else { av = a.recovered ? a.dur : LLONG_MAX; bv = b.recovered ? b.dur : LLONG_MAX; }
            return g_outageSortAscending ? av < bv : av > bv;
        });
    }
    int row = 0;
    for (std::size_t original : g_outageOrder) {
        const Outage& o = App().document.outages[original];
        LvAddRow(App().hOutage, row, FmtW(L"%d", static_cast<int>(original + 1)));
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
    g_tagRows.assign(tc.begin(), tc.end());
    std::stable_sort(g_tagRows.begin(), g_tagRows.end(), [](const auto& a, const auto& b) {
        if (g_tagSortColumn == 0) return g_tagSortAscending ? a.first < b.first : a.first > b.first;
        if (a.second != b.second) return g_tagSortAscending ? a.second < b.second : a.second > b.second;
        return a.first < b.first;
    });
    int row = 0;
    for (auto& kv : g_tagRows) {
        LvAddRow(App().hTags, row, U8ToW(kv.first));
        LvSet(App().hTags, row, 1, FmtW(L"%d", kv.second));
        LvSet(App().hTags, row, 2, std::wstring((size_t)std::max(0, kv.second * 50 / mx), L'█'));
        row++;
    }
}

static const LogView& RawRows() {
    return g_rawContext.empty() ? App().document.filtered : g_rawContext;
}

static void RenderRaw() {
    g_rawContext.clear();
    std::size_t targetRow = std::numeric_limits<std::size_t>::max();
    if (g_rawTargetLine) {
        const auto filtered = std::find_if(App().document.filtered.begin(), App().document.filtered.end(),
            [](const LogLine* line) { return line->lineNo == g_rawTargetLine; });
        if (filtered != App().document.filtered.end()) {
            targetRow = static_cast<std::size_t>(filtered - App().document.filtered.begin());
        } else {
            const auto found = std::find_if(App().document.lines.begin(), App().document.lines.end(),
                [](const LogLine& line) { return line.lineNo == g_rawTargetLine; });
            if (found != App().document.lines.end()) {
                const std::size_t index = static_cast<std::size_t>(found - App().document.lines.begin());
                const std::size_t first = index > 40 ? index - 40 : 0;
                const std::size_t last = std::min(App().document.lines.size(), index + 41);
                g_rawContext.reserve(last - first);
                for (std::size_t i = first; i < last; ++i) g_rawContext.push_back(&App().document.lines[i]);
                targetRow = index - first;
            }
        }
    }
    ListView_SetItemCountEx(App().hRaw, static_cast<int>(RawRows().size()),
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (targetRow < RawRows().size()) {
        ListView_SetItemState(App().hRaw, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(App().hRaw, static_cast<int>(targetRow), LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(App().hRaw, static_cast<int>(targetRow), FALSE);
    }
    InvalidateRect(App().hRaw, nullptr, TRUE);
}

static void RenderCells() {
    g_cellRows.clear();
    g_cellRows.reserve(App().document.cellAnalysis.cells.size());
    for (const CellSummary& cell : App().document.cellAnalysis.cells) g_cellRows.push_back(&cell);
    if (g_cellSortColumn >= 0) {
        std::stable_sort(g_cellRows.begin(), g_cellRows.end(), [](const CellSummary* a, const CellSummary* b) {
            const std::string av = cellSummaryCellText(*a, static_cast<std::size_t>(g_cellSortColumn));
            const std::string bv = cellSummaryCellText(*b, static_cast<std::size_t>(g_cellSortColumn));
            if (g_cellSortColumn >= 1 && g_cellSortColumn <= 13) {
                auto numeric = [](const CellSummary& cell, int column) -> long long {
                    switch (column) {
                    case 1: return cell.pci;
                    case 2: return cell.tac == UINT32_MAX ? -1 : cell.tac;
                    case 3: return cell.samples;
                    case 4: return cell.sampleSharePermille;
                    case 5: return cell.observedDwellSec;
                    case 6: return cell.rsrpAvg10;
                    case 7: return cell.rsrpMin;
                    case 8: return cell.rsrqAvg10;
                    case 9: return cell.snrAvg10;
                    case 10: return cell.csqAvg10;
                    case 11: return cell.switchesIn;
                    case 12: return cell.switchesOut;
                    case 13: return cell.outageStarts;
                    default: return 0;
                    }
                };
                const long long an = numeric(*a, g_cellSortColumn), bn = numeric(*b, g_cellSortColumn);
                return g_cellSortAscending ? an < bn : an > bn;
            }
            return g_cellSortAscending ? av < bv : av > bv;
        });
    }
    ListView_SetItemCountEx(App().hCells, static_cast<int>(g_cellRows.size()),
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(App().hCells, nullptr, TRUE);
}

static void UpdateMetricFilterButton() {
    if (!App().hMetricFilter) return;
    int count = !g_metricFilter.cell.empty() + !g_metricFilter.rat.empty() +
                !g_metricFilter.ch.empty() + g_metricFilter.hasDeny;
    std::wstring text = L"指标筛选 ▾";
    if (count) text = L"指标筛选 (" + std::to_wstring(count) + L") ▾";
    SetWindowTextW(App().hMetricFilter, text.c_str());
    SetModernButtonActive(App().hMetricFilter, count > 0);
}

static bool sameText(const std::string& left, const std::string& right) {
    return _stricmp(left.c_str(), right.c_str()) == 0;
}

static long long metricNumericValue(const MetricRow& metric, int column) {
    switch (column) {
    case 3: return metric.pci;
    case 4: return metric.tac == UINT32_MAX ? -1 : metric.tac;
    case 5: return metric.csqRaw;
    case 6: return metric.tempMax;
    case 7: return metric.consecFail;
    case 8: return metric.rx;
    case 9: return metric.drx;
    case 10: return metric.rsrp;
    case 11: return metric.rsrq;
    case 12: return metric.snr10;
    case 13: return metric.rssiVal;
    case 14: return metric.srvVal;
    case 16: return metric.denyVal;
    default: return 0;
    }
}

void RebuildMetricQuickFilterView() {
    App().document.metricView.clear();
    App().document.metricView.reserve(App().document.metrics.size());
    for (const MetricRow& metric : App().document.metrics) {
        if (!g_metricFilter.cell.empty() && !sameText(metric.cellId.str(), g_metricFilter.cell)) continue;
        if (!g_metricFilter.rat.empty() && !sameText(metric.rat, g_metricFilter.rat)) continue;
        if (!g_metricFilter.ch.empty() && !sameText(metric.ch, g_metricFilter.ch)) continue;
        if (g_metricFilter.hasDeny && metric.denyVal != g_metricFilter.deny) continue;
        App().document.metricView.push_back(&metric);
    }
    if (g_metricSortColumn >= 0) {
        std::stable_sort(App().document.metricView.begin(), App().document.metricView.end(),
            [](const MetricRow* a, const MetricRow* b) {
                if (g_metricSortColumn == 0)
                    return g_metricSortAscending ? a->t < b->t : a->t > b->t;
                const std::string av = metricCellText(*a, static_cast<std::size_t>(g_metricSortColumn));
                const std::string bv = metricCellText(*b, static_cast<std::size_t>(g_metricSortColumn));
                const bool numeric = (g_metricSortColumn >= 3 && g_metricSortColumn <= 14) ||
                                     g_metricSortColumn == 16;
                if (numeric) {
                    const long long an = metricNumericValue(*a, g_metricSortColumn);
                    const long long bn = metricNumericValue(*b, g_metricSortColumn);
                    if (an != bn) return g_metricSortAscending ? an < bn : an > bn;
                }
                return g_metricSortAscending ? av < bv : av > bv;
            });
    }
    UpdateMetricFilterButton();
}

void ClearMetricQuickFilters(bool refresh) {
    g_metricFilter = MetricQuickFilter{};
    RebuildMetricQuickFilterView();
    if (refresh) {
        g_pageDirty[4] = true;
        if (CurrentPage() == 4) RenderPage(4);
    }
}

void ShowMetricQuickFilterMenu(HWND anchor) {
    std::set<std::string> cellSet, ratSet, chSet;
    std::set<int> denySet;
    for (const MetricRow& metric : App().document.metrics) {
        if (!metric.cellId.empty()) cellSet.insert(metric.cellId.str());
        if (!metric.rat.empty()) ratSet.insert(metric.rat);
        if (!metric.ch.empty()) chSet.insert(metric.ch);
        if (metric.denyVal >= 0) denySet.insert(metric.denyVal);
    }
    std::vector<std::string> cells(cellSet.begin(), cellSet.end());
    std::vector<std::string> rats(ratSet.begin(), ratSet.end());
    std::vector<std::string> channels(chSet.begin(), chSet.end());
    std::vector<int> denies(denySet.begin(), denySet.end());
    HMENU menu = CreatePopupMenu();
    HMENU cellMenu = CreatePopupMenu(), ratMenu = CreatePopupMenu();
    HMENU channelMenu = CreatePopupMenu(), denyMenu = CreatePopupMenu();
    auto appendStrings = [](HMENU target, const std::vector<std::string>& values, UINT base,
                            const std::string& selected) {
        const std::size_t count = std::min<std::size_t>(values.size(), 80);
        for (std::size_t i = 0; i < count; ++i)
            AppendMenuW(target, MF_STRING | (values[i] == selected ? MF_CHECKED : 0), base + i,
                        U8ToW(values[i]).c_str());
        if (values.empty()) AppendMenuW(target, MF_STRING | MF_DISABLED, 0, L"暂无数据");
    };
    appendStrings(cellMenu, cells, 30100, g_metricFilter.cell);
    appendStrings(ratMenu, rats, 30200, g_metricFilter.rat);
    appendStrings(channelMenu, channels, 30300, g_metricFilter.ch);
    for (std::size_t i = 0; i < denies.size() && i < 80; ++i)
        AppendMenuW(denyMenu, MF_STRING | (g_metricFilter.hasDeny && denies[i] == g_metricFilter.deny ? MF_CHECKED : 0),
                    30400 + i, std::to_wstring(denies[i]).c_str());
    if (denies.empty()) AppendMenuW(denyMenu, MF_STRING | MF_DISABLED, 0, L"暂无数据");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(cellMenu), L"Cell ID");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(ratMenu), L"RAT");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(channelMenu), L"CH");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(denyMenu), L"DENY");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 30000, L"清空指标快捷筛选");
    RECT rect{}; GetWindowRect(anchor, &rect);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                        rect.left, rect.bottom + S(4), 0, App().hMain, nullptr);
    if (command == 30000) g_metricFilter = MetricQuickFilter{};
    else if (command >= 30100 && command < 30180 && command - 30100 < cells.size()) {
        const std::string& value = cells[command - 30100];
        g_metricFilter.cell = g_metricFilter.cell == value ? std::string{} : value;
    } else if (command >= 30200 && command < 30280 && command - 30200 < rats.size()) {
        const std::string& value = rats[command - 30200];
        g_metricFilter.rat = g_metricFilter.rat == value ? std::string{} : value;
    } else if (command >= 30300 && command < 30380 && command - 30300 < channels.size()) {
        const std::string& value = channels[command - 30300];
        g_metricFilter.ch = g_metricFilter.ch == value ? std::string{} : value;
    } else if (command >= 30400 && command < 30480 && command - 30400 < denies.size()) {
        const int value = denies[command - 30400];
        if (g_metricFilter.hasDeny && g_metricFilter.deny == value) {
            g_metricFilter.hasDeny = false; g_metricFilter.deny = -1;
        } else {
            g_metricFilter.deny = value; g_metricFilter.hasDeny = true;
        }
    }
    DestroyMenu(menu);
    if (command) {
        RebuildMetricQuickFilterView();
        g_pageDirty[4] = true;
        RenderPage(4);
        ShowPage(4);
    }
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
    g_rawContext.clear();
    std::fill(std::begin(g_pageDirty), std::end(g_pageDirty), true);
    RefreshNavigation();
}

// OWNERDATA 控件只保存行数,数据仍属于下列 C++ 模型。模型即将重建/释放时必须先把
// 行数归零,避免控件在重绘通知中索引已经失效的 App().document.metrics / App().document.timelineRows。
void ResetVirtualTables() {
    if (App().hTimeline) ListView_SetItemCountEx(App().hTimeline, 0, LVSICF_NOSCROLL);
    if (App().hMetric)   ListView_SetItemCountEx(App().hMetric,   0, LVSICF_NOSCROLL);
    if (App().hRaw)      ListView_SetItemCountEx(App().hRaw,      0, LVSICF_NOSCROLL);
    if (App().hCells)    ListView_SetItemCountEx(App().hCells,    0, LVSICF_NOSCROLL);
    App().document.timelineRows.clear();
    App().document.metricView.clear();
    g_rawContext.clear();
    g_cellRows.clear();
}

// 关闭/替换日志的冷路径:不仅清空元素,还释放所有大 vector 的 capacity。
// 顺序是安全边界的一部分:OWNERDATA 行数和两个借用视图必须先失效,最后才能释放
// App().document.lines。普通筛选仍使用 clear()/赋值复用容量,避免每次点击“应用”都重新分配。
void ReleaseLoadedData() {
    g_rawTargetLine = 0;
    ResetVirtualTables();

    // 非 OWNERDATA 控件自己持有单元格文本；替换日志前也立即丢掉旧内容，避免隐藏页
    // 一直保留到用户下次切换页签才释放。
    if (App().hOutage)   ListView_DeleteAllItems(App().hOutage);
    if (App().hTags)     ListView_DeleteAllItems(App().hTags);
    if (App().hUnparsed) ListView_DeleteAllItems(App().hUnparsed);

    App().document.release();
    g_metricFilter = MetricQuickFilter{};
    UpdateMetricFilterButton();
    ClearEvidenceBookmarks();

    releaseVector(g_ogColors);
    ReleaseOverviewPageData();
    ReleaseChartPageData();
}

// 数据更新时只刷新当前页；其它页保留脏标记,用户首次切过去时再生成控件内容。
// 时间线/指标还使用 OWNERDATA 虚拟表,这里只设置行数,滚动到可见单元格时才转 UTF-16。
void RenderPage(int page) {
    if (page < 0 || page >= 9 || !g_pageDirty[page]) return;
    switch (page) {
    case 0: RenderSummary(); InvalidateRect(App().hDash, nullptr, TRUE); break;
    case 1: RenderFindings(); break;
    case 2: RenderTimeline(); break;
    case 3: RenderOutages(); break;
    case 4: RenderMetrics(); break;
    case 5: RenderTags(); break;
    case 6: RenderRaw(); break;
    case 7: RenderUnparsed(); break;
    case 8: RenderCells(); break;
    }
    g_pageDirty[page] = false;
}

void ShowPage(int page) {
    // 页序:0总览 1结论 2时间线 3断网 4指标 5标签 6原始行 7未识别行 8小区分析
    if (page < 0 || page >= 9) return;
    g_curPage = page;
    const wchar_t* titles[] = {L"概览", L"诊断结论", L"事件时间线", L"断网记录",
                               L"信号指标", L"标签统计", L"原始日志", L"未识别行", L"小区分析"};
    if (App().hPageTitle) SetWindowTextW(App().hPageTitle, titles[page]);
    SetNavigationPage(page);
    RenderPage(page);   // 页仍隐藏时填充,减少 ListView 大批插入时的可见闪烁
    struct { HWND* h; int page; } items[] = {
        { &App().hDash, 0 }, { &App().hSummary, 0 }, { &App().hFindings, 1 }, { &App().hTimeline, 2 }, { &App().hOutage, 3 },
        { &App().hChart, 4 }, { &App().hMetric, 4 },
        { &App().hTags, 5 }, { &App().hRaw, 6 }, { &App().hUnparsed, 7 },
        { &App().hCells, 8 },
    };
    for (auto& it : items) {
        bool on = (it.page == page);
        ShowWindow(*it.h, on ? SW_SHOW : SW_HIDE);
        if (on) SetWindowPos(*it.h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    ShowWindow(App().hExport, SW_SHOW);
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
    SetFocus(App().hRaw);
    ShowModernNotice(L"已定位原始证据",
                     FmtW(L"第 %d 行已选中；按 Ctrl+C 可直接复制。", static_cast<int>(lineNo)).c_str(),
                     ModernNoticeKind::Info, 4500);
}

bool ToggleEvidenceBookmark(size_t lineNo, const std::wstring& text) {
    if (!lineNo) return false;
    const auto found = std::find_if(g_bookmarks.begin(), g_bookmarks.end(),
        [lineNo](const EvidenceBookmark& bookmark) { return bookmark.lineNo == lineNo; });
    bool added = found == g_bookmarks.end();
    if (added) {
        if (g_bookmarks.size() >= 30) g_bookmarks.erase(g_bookmarks.begin());
        g_bookmarks.push_back(EvidenceBookmark{lineNo, text});
    } else {
        g_bookmarks.erase(found);
    }
    RefreshBookmarkButton();
    return added;
}

bool ToggleCurrentRawBookmark() {
    if (GetFocus() == App().hRaw) {
        const int row = ListView_GetNextItem(App().hRaw, -1, LVNI_SELECTED);
        if (row >= 0 && static_cast<std::size_t>(row) < RawRows().size())
            g_rawTargetLine = RawRows()[row]->lineNo;
    }
    if (!g_rawTargetLine) {
        ShowModernNotice(L"没有可标记的原始行", L"先选择或从诊断证据定位一行，再按 Ctrl+B 添加书签。",
                         ModernNoticeKind::Info);
        return false;
    }
    auto found = std::find_if(App().document.lines.begin(), App().document.lines.end(),
        [](const LogLine& line) { return line.lineNo == g_rawTargetLine; });
    if (found == App().document.lines.end()) return false;
    std::wstring text = U8ToW(found->ts + " [" + found->tagText() + "] " + found->msg);
    const bool added = ToggleEvidenceBookmark(g_rawTargetLine, text);
    ShowModernNotice(added ? L"证据书签已添加" : L"证据书签已移除",
                     FmtW(L"原始第 %d 行", static_cast<int>(g_rawTargetLine)).c_str(),
                     added ? ModernNoticeKind::Success : ModernNoticeKind::Info);
    return added;
}

void ClearEvidenceBookmarks() {
    releaseVector(g_bookmarks);
    RefreshBookmarkButton();
}

const std::vector<EvidenceBookmark>& EvidenceBookmarks() { return g_bookmarks; }

static bool CopyTextToClipboard(const std::wstring& text) {
    if (!OpenClipboard(App().hMain)) return false;
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) { CloseClipboard(); return false; }
    void* output = GlobalLock(memory);
    if (!output) { GlobalFree(memory); CloseClipboard(); return false; }
    std::memcpy(output, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory); CloseClipboard(); return false;
    }
    CloseClipboard();
    return true;
}

bool CopySelectedPageRows() {
    HWND list = GetFocus();
    const bool supported = list == App().hMetric || list == App().hOutage || list == App().hTags ||
                           list == App().hRaw || list == App().hCells || list == App().hTimeline ||
                           list == App().hUnparsed;
    if (!supported) return false;
    const int columns = Header_GetItemCount(ListView_GetHeader(list));
    std::wstring output;
    int row = -1, copied = 0;
    while ((row = ListView_GetNextItem(list, row, LVNI_SELECTED)) >= 0) {
        for (int column = 0; column < columns; ++column) {
            std::string text;
            if (list == App().hMetric && static_cast<std::size_t>(row) < App().document.metricView.size())
                text = metricCellText(*App().document.metricView[row], column);
            else if (list == App().hRaw && static_cast<std::size_t>(row) < RawRows().size())
                text = rawCellText(*RawRows()[row], column);
            else if (list == App().hCells && static_cast<std::size_t>(row) < g_cellRows.size())
                text = cellSummaryCellText(*g_cellRows[row], column);
            else if (list == App().hTimeline && static_cast<std::size_t>(row) < App().document.timelineRows.size())
                text = timelineCellText(*App().document.timelineRows[row], column);
            else {
                wchar_t value[2048]{};
                ListView_GetItemText(list, row, column, value, 2048);
                text = WToU8(value);
            }
            for (char& ch : text) if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
            if (column) output += L'\t';
            output += U8ToW(text);
        }
        output += L"\r\n";
        ++copied;
    }
    if (!copied) return false;
    const bool success = CopyTextToClipboard(output);
    if (success)
        ShowModernNotice(L"已复制所选行", FmtW(L"共 %d 行，使用制表符分列。", copied).c_str(),
                         ModernNoticeKind::Success, 3000);
    return success;
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
    } else if (list == App().hCells && loaded) {
        title = L"没有小区画像"; detail = L"当前日志未提供 Cell ID，无法按小区聚合信号质量。";
    } else if (list == App().hRaw && loaded) {
        title = L"筛选结果为空"; detail = L"清空筛选条件即可恢复全部原始日志。";
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
    if (hdr->code == LVN_COLUMNCLICK) {
        const int column = reinterpret_cast<NMLISTVIEW*>(lparam)->iSubItem;
        auto toggle = [column](int& current, bool& ascending) {
            if (current == column) ascending = !ascending;
            else { current = column; ascending = true; }
        };
        if (hdr->hwndFrom == App().hMetric) {
            toggle(g_metricSortColumn, g_metricSortAscending);
            SetSortIndicator(App().hMetric, g_metricSortColumn, g_metricSortAscending);
            RebuildMetricQuickFilterView(); RenderMetrics();
        } else if (hdr->hwndFrom == App().hOutage) {
            toggle(g_outageSortColumn, g_outageSortAscending);
            SetSortIndicator(App().hOutage, g_outageSortColumn, g_outageSortAscending); RenderOutages();
        } else if (hdr->hwndFrom == App().hTags) {
            toggle(g_tagSortColumn, g_tagSortAscending);
            SetSortIndicator(App().hTags, g_tagSortColumn, g_tagSortAscending); RenderTags();
        } else if (hdr->hwndFrom == App().hCells) {
            toggle(g_cellSortColumn, g_cellSortAscending);
            SetSortIndicator(App().hCells, g_cellSortColumn, g_cellSortAscending); RenderCells();
        } else return false;
        result = 0; return true;
    }
    if (hdr->code == LVN_ITEMCHANGED && hdr->hwndFrom == App().hMetric) {
        const NMLISTVIEW* change = reinterpret_cast<NMLISTVIEW*>(lparam);
        if ((change->uNewState & LVIS_SELECTED) && change->iItem >= 0 &&
            static_cast<std::size_t>(change->iItem) < App().document.metricView.size())
            SetChartFocusTime(App().document.metricView[change->iItem]->t);
        return false;
    }
    if (hdr->code == NM_CLICK && hdr->hwndFrom == App().hOutage) {
        const int row = reinterpret_cast<NMITEMACTIVATE*>(lparam)->iItem;
        if (row >= 0 && static_cast<std::size_t>(row) < g_outageOrder.size())
            SetChartFocusTime(App().document.outages[g_outageOrder[row]].start);
        return false;
    }
    if (hdr->code == NM_DBLCLK) {
        const int row = reinterpret_cast<NMITEMACTIVATE*>(lparam)->iItem;
        if (row < 0) return false;
        if (hdr->hwndFrom == App().hMetric && static_cast<std::size_t>(row) < App().document.metricView.size())
            JumpToRawLine(App().document.metricView[row]->lineNo);
        else if (hdr->hwndFrom == App().hOutage && static_cast<std::size_t>(row) < g_outageOrder.size())
            JumpToRawLine(App().document.outages[g_outageOrder[row]].startLine);
        else if (hdr->hwndFrom == App().hCells && static_cast<std::size_t>(row) < g_cellRows.size()) {
            g_metricFilter.cell = g_cellRows[row]->cellId;
            RebuildMetricQuickFilterView(); g_pageDirty[4] = true; ShowPage(4);
        } else if (hdr->hwndFrom == App().hRaw && static_cast<std::size_t>(row) < RawRows().size()) {
            g_rawTargetLine = RawRows()[row]->lineNo;
            ToggleCurrentRawBookmark();
        } else return false;
        result = 0; return true;
    }
    if (hdr->code == LVN_GETDISPINFOW &&
        (hdr->hwndFrom == App().hTimeline || hdr->hwndFrom == App().hMetric ||
         hdr->hwndFrom == App().hRaw || hdr->hwndFrom == App().hCells)) {
        NMLVDISPINFOW* di = reinterpret_cast<NMLVDISPINFOW*>(lparam);
        if ((di->item.mask & LVIF_TEXT) && di->item.pszText && di->item.cchTextMax > 0 &&
            di->item.iItem >= 0 && di->item.iSubItem >= 0) {
            const size_t row = static_cast<size_t>(di->item.iItem);
            const size_t col = static_cast<size_t>(di->item.iSubItem);
            std::string text;
            if (hdr->hwndFrom == App().hTimeline && row < App().document.timelineRows.size() &&
                col < kTimelineColumnCount) {
                text = timelineCellText(*App().document.timelineRows[row], col);
            } else if (hdr->hwndFrom == App().hMetric && row < App().document.metricView.size() &&
                       col < kMetricColumnCount) {
                text = metricCellText(*App().document.metricView[row], col);
            } else if (hdr->hwndFrom == App().hRaw && row < RawRows().size() &&
                       col < kRawColumnCount) {
                text = rawCellText(*RawRows()[row], col);
            } else if (hdr->hwndFrom == App().hCells && row < g_cellRows.size() &&
                       col < kCellColumnCount) {
                text = cellSummaryCellText(*g_cellRows[row], col);
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
         hdr->hwndFrom != App().hUnparsed && hdr->hwndFrom != App().hRaw &&
         hdr->hwndFrom != App().hCells)) {
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
    if (hdr->hwndFrom == App().hTimeline || hdr->hwndFrom == App().hOutage ||
        hdr->hwndFrom == App().hRaw) {
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            const size_t row = static_cast<size_t>(draw->nmcd.dwItemSpec);
            if (hdr->hwndFrom == App().hTimeline) {
                if (row < App().document.timelineRows.size())
                    draw->clrText = RowColor(*App().document.timelineRows[row]);
            } else if (hdr->hwndFrom == App().hOutage && row < g_ogColors.size()) {
                draw->clrText = g_ogColors[row];
            } else if (hdr->hwndFrom == App().hRaw && row < RawRows().size()) {
                draw->clrText = RowColor(*RawRows()[row]);
            }
            draw->clrTextBk = (row & 1) ? th::zebra : th::surface;
        }
        result = CDRF_DODEFAULT;
        return true;
    }

    if (hdr->hwndFrom == App().hTags || hdr->hwndFrom == App().hUnparsed ||
        hdr->hwndFrom == App().hCells) {
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            const size_t row = static_cast<size_t>(draw->nmcd.dwItemSpec);
            draw->clrText = th::inkPri;
            draw->clrTextBk = (row & 1) ? th::zebra : th::surface;
            if (hdr->hwndFrom == App().hCells && row < g_cellRows.size()) {
                const CellSummary& cell = *g_cellRows[row];
                if (cell.outageStarts && ((cell.rsrpSamples >= 5 && cell.rsrpAvg10 <= -1100) ||
                                          (cell.snrSamples >= 5 && cell.snrAvg10 <= 0)))
                    draw->clrTextBk = th::cellWeak;
            }
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
        if (row < App().document.metricView.size()) {
            const MetricRow& metric = *App().document.metricView[row];
            if (column == 9 && metric.drx == 0)
                draw->clrTextBk = th::cellStall;
            else if (column == 5 && metric.csqVal >= 0 && metric.csqVal < 10)
                draw->clrTextBk = th::cellWeak;
            else if (column == 12 && metric.snr10 != 100000 && metric.snr10 <= 0)
                draw->clrTextBk = th::cellSnrLow;
        }
    }
    result = CDRF_DODEFAULT;
    return true;
}

} // namespace dl
