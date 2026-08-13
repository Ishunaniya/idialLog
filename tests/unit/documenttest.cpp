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
    DocumentState active;
    active.swap(pending);
    ok(active.lines.size() == 3 && active.filtered.size() == 3,
       "完整模型从后台结果交换到活动文档");
    ok(active.filtered.front() == first && active.filtered.front() == &active.lines.front(),
       "vector 交换后借用指针仍指向活动文档拥有的行");
    ok(!active.metricView.empty() && active.metricView.front() == &active.metrics.front(),
       "vector 交换后指标视图仍指向活动文档拥有的指标");
    ok(pending.lines.empty() && pending.filtered.empty(), "接管后临时文档不再拥有新模型");

    active.release();
    ok(active.lines.capacity() == 0 && active.filtered.capacity() == 0 &&
       active.metrics.capacity() == 0 && active.metricView.capacity() == 0 &&
       active.cellAnalysis.cells.capacity() == 0 && active.findings.capacity() == 0,
       "卸载按借用顺序释放全部主要容量");

    std::printf("\n%s 失败 %d 项\n", failures ? "**" : "==", failures);
    return failures ? 1 : 0;
}
