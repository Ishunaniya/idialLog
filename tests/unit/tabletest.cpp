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
        cells = cells && timelineCellText(*l, 0) == fmtTime(l->t, "FULL") &&
                timelineCellText(*l, 2) == l->tagText() &&
                timelineCellText(*l, 3) == l->msg;
    }
    ok(borrowed, "时间线行全部借用筛选视图,不复制 LogLine");
    ok(cells, "时间线显示完整年月日，标签与消息保持原文");
    ok(audit.logOpened == 1 && !all.empty() && all.front()->lineNo == 2 &&
       rawCellText(*all.front(), 0) == "2", "真机第 1 行为文件打开标记，原始行号保留第 2 行而不重新编号");
    LogLine event;
    event.setTag("RECOVERY L2"); event.msg = "AT+CFUN=0 rsp: OK";
    ok(timelineCellText(event, 1) == "恢复动作", "恢复步骤不会标为已恢复联网");
    event.msg = "Net Fail Duration: 60s";
    ok(timelineCellText(event, 1) == "故障", "恢复标签中的故障信息优先显示故障");
    event.msg = "Network Recovered in SDK phase";
    ok(timelineCellText(event, 1) == "已恢复", "明确的 SDK 恢复日志单独标注已恢复");
    event.msg = "AT command timed out"; event.setLevel("ERROR");
    ok(timelineCellText(event, 1) == "错误", "错误级别优先于普通恢复步骤");
    event.setLevel("INFO"); event.setTag("CFUN"); event.msg = "AT+CFUN=1";
    ok(timelineCellText(event, 1) == "状态变化", "CFUN 标签本身不构成错误证据");
    Outage status; status.recovered = true;
    status.dur = 30; ok(outageStatusText(status) == "已恢复 · ≤30s", "30 秒恢复事件状态明确");
    status.dur = 31; ok(outageStatusText(status) == "已恢复 · 31–60s", "31 秒进入第二显示档");
    status.dur = 60; ok(outageStatusText(status) == "已恢复 · 31–60s", "60 秒仍在第二显示档");
    status.dur = 61; ok(outageStatusText(status) == "已恢复 · >60s", "长事件仍明确显示已恢复");
    status.recovered = false; ok(outageStatusText(status) == "未恢复", "未闭合事件单独显示未恢复");

    std::puts("== T2 指标虚拟列格式 ==");
    MetricRow m;
    m.t = mkEpoch(2026, 6, 30, 0, 0, 0); m.ch = "SIM"; m.cellId = "D17C148"; m.pci = 496; m.tac = 0x272D; m.tacDigits = 4;
    m.csqRaw = 20; m.csqVal = 20; m.tempMax = 60;
    m.consecFail = 0; m.rx = 100; m.drx = 5; m.rsrp = -104; m.rsrq = -10;
    m.snr10 = -25; m.rssiVal = -65; m.srvVal = 2; m.rat = "LTE";
    m.denyVal = 0; m.oper = "CMCC 46000";
    m.atTelemetryTimeout = 1; m.atBasicProbe = 0; m.detailedAtTimeout = 1;
    m.detailedAtStage = "serving_cell";
    const std::string expected[kMetricColumnCount] = {
        "2026-06-30 00:00:00", "SIM", "D17C148", "496", "272D", "20", "60", "0", "100", "5",
        "-104", "-10", "-2.5", "-65", "2", "LTE", "0", "CMCC 46000",
        "较差 · RSRP/SNR", "是", "失败", "serving_cell"
    };
    bool metricCells = true;
    for (size_t i = 0; i < kMetricColumnCount; ++i)
        metricCells = metricCells && metricCellText(m, i) == expected[i];
    ok(metricCells, "22 列文本逐列精确一致（含 LTE 工程参考与 IMX AT 健康字段）");
    m.inferredTime = true;
    ok(metricCellText(m, 0) == "~2026-06-30 00:00:00" &&
       metricCsvCellText(m, 0) == "=\"~2026-06-30 00:00:00\"", "推定年月日在指标表和 CSV 中保留 ~ 标记");
    m.inferredTime = false;
    LogLine estimated; estimated.t = m.t; estimated.ts = "~2026-06-30 00:00:00"; estimated.inferredTime = true;
    ok(timelineCellText(estimated, 0) == "~2026-06-30 00:00:00", "时间线保留推定年份提示");
    std::vector<LogLine> estimatedHeartbeat(1);
    estimatedHeartbeat[0] = estimated; estimatedHeartbeat[0].setTag("HEARTBEAT");
    estimatedHeartbeat[0].msg = "CH:SIM | CSQ:18";
    auto estimatedMetrics = buildMetrics(estimatedHeartbeat);
    ok(estimatedMetrics.size() == 1 && estimatedMetrics[0].inferredTime,
       "心跳提取不会丢失原始行的推定时间标记");
    m.rsrp = 1; m.rsrq = 1; m.snr10 = 100000; m.rssiVal = 1;
    ok(metricCellText(m, 10) == "-" && metricCellText(m, 11) == "-" &&
       metricCellText(m, 12) == "-" && metricCellText(m, 13) == "-",
       "无效信号值仍显示 '-'");
    ok(metricCellText(m, 18) == "优秀 · CSQ", "工程参考评价只列出决定当前等级的指标");
    m.rat = "NR5G-SA";
    ok(metricOverallSignalQuality(m) == SignalQuality::Unknown && metricCellText(m, 18) == "-",
       "明确的非 LTE 制式不套用 LTE 工程分档");
    m.rat = "LTE";

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
    ok(timelineCellText(longLine, 3).size() == 250, "时间线保留完整消息供滚动、详情与复制");
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
       qengMetrics[0].tacDigits == 4 && qengMetrics[0].rsrp == -101 &&
       qengMetrics[0].rsrq == -6 && qengMetrics[0].rssiVal == -75 &&
       qengMetrics[0].snr10 == 230,
       "[CELL] +QENG 的 Cell/PCI/TAC/RSRP/RSRQ/RSSI/SINR 会延续到后续心跳");

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
