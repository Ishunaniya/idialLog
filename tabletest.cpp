// tabletest.cpp — 虚拟时间线/指标表格的数据源回归测试。
#include "logmodel.h"
#include "tablemodel.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace dl;

static int g_fail = 0;
static void ok(bool condition, const char* what) {
    std::printf("  %s %s\n", condition ? "[通过]" : "[失败]", what);
    if (!condition) ++g_fail;
}

int main() {
    std::ifstream input("samples/rtms_eg25/dial_20260630_000026.log", std::ios::binary);
    std::vector<std::string> raw;
    std::string line;
    while (std::getline(input, line)) raw.push_back(line);
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);
    LogView all = applyFilterView(lines, "", "", "", "", nullptr);

    std::puts("== T1 真机时间线虚拟行 ==");
    LogView timeline;
    buildTimelineView(all, timeline);
    ok(timeline.size() == 777, "真机 EG25 时间线仍为 777 行");
    std::map<size_t, const LogLine*> byLine;
    for (const LogLine* l : all) byLine[l->lineNo] = l;
    bool borrowed = true, cells = true;
    for (const LogLine* l : timeline) {
        auto it = byLine.find(l->lineNo);
        borrowed = borrowed && it != byLine.end() && it->second == l;
        cells = cells && timelineCellText(*l, 0) == fmtTime(l->t, "MD") &&
                timelineCellText(*l, 1) == l->tagText() &&
                timelineCellText(*l, 2) == l->msg.substr(0, 200);
    }
    ok(borrowed, "时间线行全部借用筛选视图,不复制 LogLine");
    ok(cells, "三列文本与旧 RenderTimeline 语义一致");

    std::puts("== T2 指标虚拟列格式 ==");
    MetricRow m;
    m.t = 1782758400; m.ch = "SIM"; m.csqRaw = 20; m.tempMax = 60;
    m.consecFail = 0; m.rx = 100; m.drx = 5; m.rsrp = -104; m.rsrq = -10;
    m.snr10 = -25; m.rssiVal = -65; m.srvVal = 2; m.rat = "LTE";
    m.denyVal = 0; m.oper = "CMCC 46000";
    const std::string expected[kMetricColumnCount] = {
        fmtTime(m.t, "MD"), "SIM", "20", "60", "0", "100", "5",
        "-104", "-10", "-2.5", "-65", "2", "LTE", "0", "CMCC 46000"
    };
    bool metricCells = true;
    for (size_t i = 0; i < kMetricColumnCount; ++i)
        metricCells = metricCells && metricCellText(m, i) == expected[i];
    ok(metricCells, "15 列文本逐列精确一致");
    m.rsrp = 1; m.rsrq = 1; m.snr10 = 100000; m.rssiVal = 1;
    ok(metricCellText(m, 7) == "-" && metricCellText(m, 8) == "-" &&
       metricCellText(m, 9) == "-" && metricCellText(m, 10) == "-",
       "无效信号值仍显示 '-'");

    std::puts("== T3 边界与规模 ==");
    LogLine longLine; longLine.t = 0; longLine.msg.assign(250, 'x');
    ok(timelineCellText(longLine, 2).size() == 200, "时间线消息仍截断到 200 字节");
    ok(timelineCellText(longLine, kTimelineColumnCount).empty() &&
       metricCellText(m, kMetricColumnCount).empty(), "越界列返回空串");
    auto metrics = buildMetrics(all);
    ok(metrics.size() == 1399, "真机 EG25 指标仍为 1399 行");

    std::printf("\n%s 失败 %d 项\n", g_fail ? "**" : "==", g_fail);
    return g_fail ? 1 : 0;
}
