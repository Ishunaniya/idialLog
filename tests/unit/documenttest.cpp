// documenttest.cpp — 后台构建文档在 UI 线程交换接管时的所有权/借用指针回归。
#include "document_state.h"
#include "logmodel.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace dl;

int main() {
    int failures = 0;
    auto ok = [&](bool condition, const char* text) {
        std::printf("  %s %s\n", condition ? "[通过]" : "[失败]", text);
        if (!condition) ++failures;
    };

    std::puts("== 后台文档常数时间接管 ==");
    std::vector<std::string> raw{
        "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | Cell:1D8DE0B | CSQ:18",
        "[2026-08-03 10:00:01] [SDK] Network Down: timeout",
        "[2026-08-03 10:00:03] [SDK] Network Recovered"
    };
    DocumentState pending;
    parseLines(raw, pending.lines, pending.sessions, &pending.audit);
    pending.filtered = applyFilterView(pending.lines, "", "", "", "", nullptr);
    pending.metrics = buildMetrics(pending.filtered);
    for (const MetricRow& metric : pending.metrics) pending.metricView.push_back(&metric);
    pending.outages = collectOutages(pending.filtered);
    pending.cellAnalysis = analyzeCells(pending.filtered, pending.metrics, pending.outages);
    pending.platform = detectPlatform(pending.lines);
    pending.findings = analyze(pending.filtered, pending.outages, pending.metrics,
                               pending.platform, pending.audit, &pending.cellAnalysis);

    const LogLine* first = pending.filtered.front();
    pending.selectTimeRange(first->t + 3, first->t + 1);
    DocumentState active;
    active.swap(pending);
    ok(active.lines.size() == 3 && active.filtered.size() == 3,
       "完整模型从后台结果交换到活动文档");
    ok(active.filtered.front() == first && active.filtered.front() == &active.lines.front(),
       "vector 交换后借用指针仍指向活动文档拥有的行");
    ok(!active.metricView.empty() && active.metricView.front() == &active.metrics.front(),
       "vector 交换后指标视图仍指向活动文档拥有的指标");
    ok(pending.lines.empty() && pending.filtered.empty(), "接管后临时文档不再拥有新模型");

    ok(active.timeRange.active && active.timeRange.start == first->t + 1 &&
       active.timeRange.end == first->t + 3 && !pending.timeRange.active,
       "反向选区归一化并随文档交换，不继承旧文档区间");
    LogView selected = active.filtered;
    active.restrictToTimeRange(selected);
    ok(selected.size() == 2 && selected.front()->lineNo == 2 && selected.back()->lineNo == 3,
       "时间范围含两端边界，借用原始行号不变");
    active.selectTimeRange(first->t + 2, first->t + 2);
    selected = active.filtered; active.restrictToTimeRange(selected);
    ok(selected.empty(), "无采样区间保持为空，不偷用范围外证据");
    active.timeRange = DocumentState::TimeRange{};
    selected = active.filtered; active.restrictToTimeRange(selected);
    ok(selected.size() == 3, "恢复全范围不丢失原始视图");
    active.selectTimeRange(first->t, first->t + 3);

    DocumentState calendar;
    parseLines({"[2025-12-31 23:59:50] [SDK] before",
                "[2025-12-31 23:59:59] [SDK] start",
                "[2026-01-01 00:00:01] [SDK] end",
                "[2026-01-01 23:59:59] [SDK] wrong day"},
               calendar.lines, calendar.sessions, &calendar.audit);
    calendar.selectTimeRange(calendar.lines[2].t, calendar.lines[1].t);
    LogView calendarView = applyFilterView(calendar.lines, "SDK", "", "", "");
    calendar.restrictToTimeRange(calendarView);
    ok(calendarView.size() == 2 && calendarView[0]->lineNo == 2 && calendarView[1]->lineNo == 3,
       "跨年跨日选区不混入另一日同一时分的证据");
    LogView regexView = applyFilterView(calendar.lines, "SDK", "end", "", "");
    calendar.restrictToTimeRange(regexView);
    ok(regexView.size() == 1 && regexView.front()->lineNo == 3, "选区与标签/消息条件求交，保留原筛选");

    active.release();
    ok(active.lines.capacity() == 0 && active.filtered.capacity() == 0 &&
       active.metrics.capacity() == 0 && active.metricView.capacity() == 0 &&
       active.cellAnalysis.cells.capacity() == 0 && active.findings.capacity() == 0 && !active.timeRange.active,
       "卸载按借用顺序释放全部主要容量");

    std::printf("\n%s 失败 %d 项\n", failures ? "**" : "==", failures);
    return failures ? 1 : 0;
}
