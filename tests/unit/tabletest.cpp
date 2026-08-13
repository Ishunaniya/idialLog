// tabletest.cpp — 虚拟时间线/指标表格的数据源回归测试。
#include "logmodel.h"
#include "signal_quality.h"
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
    m.t = 1782758400; m.ch = "SIM"; m.cellId = "D17C148"; m.pci = 496; m.tac = 0x272D; m.tacDigits = 4;
    m.csqRaw = 20; m.csqVal = 20; m.tempMax = 60;
    m.consecFail = 0; m.rx = 100; m.drx = 5; m.rsrp = -104; m.rsrq = -10;
    m.snr10 = -25; m.rssiVal = -65; m.srvVal = 2; m.rat = "LTE";
    m.denyVal = 0; m.oper = "CMCC 46000";
    const std::string expected[kMetricColumnCount] = {
        fmtTime(m.t, "MD"), "SIM", "D17C148", "496", "272D", "20", "60", "0", "100", "5",
        "-104", "-10", "-2.5", "-65", "2", "LTE", "0", "CMCC 46000",
        "CSQ优秀 / RSRP较差 / RSRQ优秀 / SNR较差"
    };
    bool metricCells = true;
    for (size_t i = 0; i < kMetricColumnCount; ++i)
        metricCells = metricCells && metricCellText(m, i) == expected[i];
    ok(metricCells, "19 列文本逐列精确一致（含小区 ID 与信号评价）");
    m.rsrp = 1; m.rsrq = 1; m.snr10 = 100000; m.rssiVal = 1;
    ok(metricCellText(m, 10) == "-" && metricCellText(m, 11) == "-" &&
       metricCellText(m, 12) == "-" && metricCellText(m, 13) == "-",
       "无效信号值仍显示 '-'");

    ok(csqQuality(9) == SignalQuality::Poor && csqQuality(10) == SignalQuality::Fair &&
       csqQuality(15) == SignalQuality::Good && csqQuality(20) == SignalQuality::Excellent &&
       csqQuality(99) == SignalQuality::Unknown, "CSQ 四档边界与 99 未知值正确");
    ok(rsrpQuality(-101) == SignalQuality::Poor && rsrpQuality(-100) == SignalQuality::Fair &&
       rsrpQuality(-90) == SignalQuality::Good && rsrpQuality(-80) == SignalQuality::Excellent,
       "RSRP 四档边界正确");
    ok(rsrqQuality(-21) == SignalQuality::Poor && rsrqQuality(-20) == SignalQuality::Fair &&
       rsrqQuality(-15) == SignalQuality::Good && rsrqQuality(-10) == SignalQuality::Excellent,
       "RSRQ 四档边界正确");
    ok(snrQuality10(-1) == SignalQuality::Poor && snrQuality10(0) == SignalQuality::Poor &&
       snrQuality10(1) == SignalQuality::Fair &&
       snrQuality10(130) == SignalQuality::Good && snrQuality10(200) == SignalQuality::Excellent,
       "SNR 四档边界正确");

    std::puts("== T2b CSV 公式注入防护 ==");
    m.ch = "=cmd|' /C calc'!A0"; m.cellId = "+SUM(A1:A2)"; m.rat = " @evil";
    m.oper = "-2+3"; m.rsrp = -104;
    ok(metricCsvCellText(m, 1).front() == '\'' && metricCsvCellText(m, 2).front() == '\'' &&
       metricCsvCellText(m, 15).front() == '\'' && metricCsvCellText(m, 17).front() == '\'',
       "日志文本的 =,+,-,@ 前缀统一中和");
    ok(metricCsvCellText(m, 10) == "-104", "负数指标保持数值，不误加文本前缀");
    ok(metricCsvCellText(m, 0).compare(0, 2, "=\"") == 0,
       "时间列只使用程序生成的固定文本公式");

    LogLine rawCell; rawCell.lineNo = 42; rawCell.ts = "2026-08-03 10:00:00";
    rawCell.setLevel("WARNING"); rawCell.setTag("SDK"); rawCell.msg = "Network Down";
    ok(rawCellText(rawCell, 0) == "42" && rawCellText(rawCell, 3) == "SDK" &&
       rawCellText(rawCell, 4) == "Network Down", "原始日志虚拟表按需格式化五列");
    CellSummary cell; cell.cellId = "ABC"; cell.samples = 8; cell.sampleSharePermille = 625;
    cell.observedDwellSec = 120; cell.rsrpSamples = 8; cell.rsrpAvg10 = -1150;
    cell.rsrpMin = -120; cell.snrSamples = 8; cell.snrAvg10 = -10; cell.outageStarts = 2;
    ok(cellSummaryCellText(cell, 4) == "62.5" && cellSummaryCellText(cell, 5) == "2m00s" &&
       cellSummaryCellText(cell, 14) == "疑似弱覆盖", "小区画像虚拟表包含占比、驻留和质量判断");

    std::puts("== T3 边界与规模 ==");
    LogLine longLine; longLine.t = 0; longLine.msg.assign(250, 'x');
    ok(timelineCellText(longLine, 2).size() == 200, "时间线消息仍截断到 200 字节");
    ok(timelineCellText(longLine, kTimelineColumnCount).empty() &&
       metricCellText(m, kMetricColumnCount).empty(), "越界列返回空串");
    auto metrics = buildMetrics(all);
    ok(metrics.size() == 1399, "真机 EG25 指标仍为 1399 行");
    ok(!metrics.empty() && metrics.front().cellId == "1D8DE0B",
       "真机首条 Cell 字段进入指标模型");
    bool carriedCell = false;
    for (const auto& metric : metrics)
        if (metric.lineNo == 5 && metric.cellId == "1D8DE0D") carriedCell = true;
    ok(carriedCell, "小区变更后的 ID 可延续到后续心跳样本");

    std::vector<std::string> cellRaw{
        "2026-08-03 10:00:00.000 [INFO] main (main.c:1) - [HEARTBEAT] cellid=C5EAB21 pci=257 tac=41B rsrp=-107dBm rsrq=-20dB"
    };
    std::vector<LogLine> cellLines; std::vector<std::string> cellSessions;
    parseLines(cellRaw, cellLines, cellSessions, nullptr);
    auto cellMetrics = buildMetrics(cellLines);
    ok(cellMetrics.size() == 1 && cellMetrics[0].cellId == "C5EAB21" &&
       cellMetrics[0].pci == 257 && cellMetrics[0].tac == 0x41B && cellMetrics[0].tacDigits == 3,
       "artery cellid/pci/tac 精确进入指标模型");

    std::vector<std::string> qengRaw{
        "[2026-08-03 10:00:00] [CELL] Init: +QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,00,D17C148,496,1850,3,5,5,272D,-101,-6,-75,23,18",
        "[2026-08-03 10:00:01] [HEARTBEAT] CH:SIM | CSQ:18"
    };
    std::vector<LogLine> qengLines; std::vector<std::string> qengSessions;
    parseLines(qengRaw, qengLines, qengSessions, nullptr);
    auto qengMetrics = buildMetrics(qengLines);
    ok(qengMetrics.size() == 1 && qengMetrics[0].cellId == "D17C148" &&
       qengMetrics[0].pci == 496 && qengMetrics[0].tac == 0x272D &&
       qengMetrics[0].tacDigits == 4,
       "[CELL] +QENG 的 Cell ID/PCI/TAC 会延续到后续心跳");

    std::vector<std::string> invalidCellRaw{
        "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | Cell:1D8DE0B | CSQ:18",
        "[2026-08-03 10:00:01] [CELL CHANGE] 1D8DE0B -> FFFFFFFF | CSQ=99",
        "[2026-08-03 10:00:02] [HEARTBEAT] CH:SIM | CSQ:17",
        "[2026-08-03 10:00:03] [HEARTBEAT] CH:SIM | Cell:N/A | CSQ:16"
    };
    std::vector<LogLine> invalidCellLines; std::vector<std::string> invalidCellSessions;
    parseLines(invalidCellRaw, invalidCellLines, invalidCellSessions, nullptr);
    auto invalidCellMetrics = buildMetrics(invalidCellLines);
    ok(invalidCellMetrics.size() == 3 && invalidCellMetrics[0].cellId == "1D8DE0B" &&
       invalidCellMetrics[1].cellId.empty() && invalidCellMetrics[2].cellId.empty(),
       "无效小区变更/N/A 会清空旧 Cell ID，不把上一小区串到后续样本");

    std::vector<std::string> mergedCells{
        "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | Cell:AAA001 | CSQ:18",
        "[2026-08-03 11:00:00] [HEARTBEAT] CH:SIM | CSQ:17"
    };
    std::vector<LogLine> mergedCellLines; std::vector<std::string> mergedCellSessions;
    parseLines(mergedCells, mergedCellLines, mergedCellSessions, nullptr, {0, 1});
    auto mergedCellMetrics = buildMetrics(mergedCellLines);
    ok(mergedCellMetrics.size() == 2 && mergedCellMetrics[0].cellId == "AAA001" &&
       mergedCellMetrics[1].cellId.empty(), "Cell ID 驻留状态不会跨日志来源串联");

    std::printf("\n%s 失败 %d 项\n", g_fail ? "**" : "==", g_fail);
    return g_fail ? 1 : 0;
}
