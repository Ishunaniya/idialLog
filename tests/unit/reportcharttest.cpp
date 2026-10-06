// build/tests/ 下的程序与导出文件是可删除的本地测试产物；make check 会重新生成，不是应用运行依赖。
#include "report_chart.h"
#include "signal_chart.h"
#include "log_parser.h"
#include "log_analysis.h"
#include "log_time.h"
#include <climits>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace dl;
static int failures = 0;

static void ok(bool value, const char* text) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", text);
    if (!value)
        ++failures;
}

static void save(const char* name, const std::string& text) {
    std::ofstream(name) << text;
}

int main() {
    MetricRow a, b, c, d;
    a.t = 30;
    a.csqVal = 99;
    a.rsrp = -90;
    a.rsrq = -8;
    a.snr10 = 130;
    b.t = 10;
    b.csqVal = 18;
    b.rssiVal = -65;
    c.t = 20;
    c.csqVal = 10;
    c.rat = "NR";
    c.rsrp = -100;
    c.rsrq = -12;
    c.snr10 = 50;
    c.rssiVal = -80;
    d.t = 20;
    d.csqVal = -1;
    const auto signals = reportedSignalSeries({&a, &b, &c, &d});
    ok(signals.csq == ChartSeries({{10, 18}}),
       "screen/report share valid CSQ 0..31 and LTE filtering, excluding unknown 99");
    ok(signals.rsrp == ChartSeries({{30, -90}}) && signals.rsrq == ChartSeries({{30, -8}}) &&
           signals.snr10 == ChartSeries({{30, 130}}),
       "non-LTE values are excluded from LTE reference plots");
    ok(signals.rssi == ChartSeries({{10, -65}, {20, -80}}),
       "RSSI retains real non-LTE readings without deriving CSQ or inventing values");
    ReportChartOptions o;
    o.title = "CSQ <test> & \"trace\"";
    o.color = "#2a78d6";
    o.start = 0;
    o.end = 7;
    auto small = renderReportChart({{0, 10}, {2, 15}, {7, 20}}, {}, o);
    ok(small.samples == 3 && small.drawn == 3 && small.segments == 1 && small.html.find("278.29") != std::string::npos,
       "small series retain every sample with fractional coordinates");
    ok(small.html.find("&lt;test&gt; &amp; &quot;trace&quot;") != std::string::npos &&
           small.html.find("原始点 3 · 绘制点 3") != std::string::npos,
       "labels are escaped and actual sample/draw counts are explicit");
    save("build/tests/unit/report-chart-small.html", small.html);
    o.start = 0;
    o.end = 2000;
    const auto broken = renderReportChart({{0, 15}, {1, 18}, {1000, 10}, {1001, 20}}, {}, o);
    ok(broken.samples == 4 && broken.drawn == 4 && broken.segments == 2,
       "original ten-minute gaps split SVG paths without interpolated measurements");
    save("build/tests/unit/report-chart-gaps.html", broken.html);
    o.start = 42;
    o.end = 42;
    o.english = true;
    const auto single = renderReportChart({{42, 18}}, {}, o);
    ok(single.drawn == 1 && single.html.find("r=\"4\"") != std::string::npos &&
           single.html.find("Samples 1 · Drawn 1") != std::string::npos,
       "single timestamp remains a visible point and supports English counts");
    o.start = 0;
    o.end = 10;
    ok(renderReportChart({{11, 18}}, {}, o).samples == 0, "selected chart range does not plot outside samples");
    ChartSeries dense;
    dense.reserve(1000000);
    for (int i = 0; i < 1000000; ++i)
        dense.emplace_back(i, i % 31);
    dense[123456].second = -100;
    dense[654321].second = 100;
    o.start = 0;
    o.end = 999999;
    auto big = renderReportChart(dense, {}, o);
    ok(big.samples == 1000000 && big.drawn <= 8194 && big.drawn > 1842,
       "million-point report uses a bounded 4096-bucket budget, above the previous 920-bucket budget");
    ok(big.html.find("1970-01-02 10:17:36 · -100") != std::string::npos &&
           big.html.find("1970-01-08 13:45:21 · 100") != std::string::npos,
       "dense reduction retains true extreme sample timestamps/values");
    save("build/tests/unit/report-chart-dense.html", big.html);
    // Same-second duplicates and extreme axes must not divide by zero/overflow.
    o.start = LLONG_MIN;
    o.end = LLONG_MAX;
    o.low = INT_MIN;
    o.high = INT_MAX;
    auto extreme = renderReportChart({{LLONG_MIN, INT_MIN}, {LLONG_MAX, INT_MAX}}, {}, o);
    ok(extreme.drawn == 2 && extreme.segments == 2 && extreme.html.find("nan") == std::string::npos &&
           extreme.html.find("inf") == std::string::npos,
       "coordinate math handles extreme time/value bounds");

    std::ifstream input("samples/rtms_eg25/dial_20260630_000026.log");
    std::vector<std::string> raw;
    std::string line;
    while (std::getline(input, line))
        raw.push_back(line);
    std::vector<LogLine> rows;
    std::vector<std::string> sessions;
    parseLines(raw, rows, sessions);
    const auto metrics = buildMetrics(rows);
    MetricView view;
    for (const auto& metric : metrics)
        view.push_back(&metric);
    const auto real = reportedSignalSeries(view);
    std::printf("Real EG25: metrics=%zu CSQ=%zu RSRP=%zu RSRQ=%zu SNR=%zu RSSI=%zu\n", metrics.size(), real.csq.size(),
                real.rsrp.size(), real.rsrq.size(), real.snr10.size(), real.rssi.size());
    ok(!real.csq.empty() && !real.rssi.empty(), "real EG25 produces both CSQ and actual reported RSSI");
    MetricView selected;
    for (const auto& metric : metrics)
        if (metric.t >= mkEpoch(2026, 6, 30, 0, 0, 26) && metric.t <= mkEpoch(2026, 6, 30, 0, 34, 41))
            selected.push_back(&metric);
    const auto selectedSignals = reportedSignalSeries(selected);
    ok(selected.size() == 68 && selectedSignals.csq.size() == 67 && selectedSignals.rsrp.size() == 30 &&
           selectedSignals.rssi.size() == 30,
       "real 68-row selected range has 67 valid CSQ points and 30 RF points; missing CSQ stays missing");
    o = {};
    o.start = real.csq.front().first;
    o.end = real.csq.back().first;
    o.title = "CSQ";
    o.color = "#2a78d6";
    const auto realChart = renderReportChart(real.csq, collectOutages(rows), o);
    ok(realChart.samples == real.csq.size() && realChart.drawn == real.csq.size(),
       "real EG25 CSQ retains every actual plotted sample");
    save("build/tests/unit/report-chart-real.html", realChart.html);
    std::printf("reportcharttest: %d failures\n", failures);
    return failures ? 1 : 0;
}
