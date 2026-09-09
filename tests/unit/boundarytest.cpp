// boundarytest.cpp — 跨文件续行防御(fileBoundaries)+ 时钟跳变检测(clockJump)断言测试。
//
// ============================ 证据分级(诚实声明)============================
// 【机制实证 / 合成输入】问题②跨文件续行:机制可精确复现(合成 A末冒号 + B首无ts),
//   但真机 72 种拼接组合 0 触发(日志通常正常结束)。所以本测试用合成输入证明**防御生效**,
//   不谎称真机踩过。
// 【无真机样本 / 合成输入】问题①时钟跳变:33 份真机夹具无一含跳变(两份 unsynced 全程
//   1970)。本测试用合成的"1970→2026 跳变"序列,只能证明**检测逻辑按设计工作**,
//   证明不了真设备就这样跳。一旦拿到真实含跳变日志,应换成真机夹具重验。
//
// 构建运行:make boundarytest && build/tests/unit/boundarytest      (rc=0 全过)
#include "logmodel.h"
#include <climits>
#include <cstdio>
#include <string>
#include <vector>

using namespace dl;

static int g_fail = 0;
static void ok(bool c, const char* what) {
    std::printf("  %s %s\n", c ? "[通过]" : "[失败]", what);
    if (!c) g_fail++;
}
static std::vector<std::string> L(std::initializer_list<const char*> xs) {
    std::vector<std::string> v; for (auto x : xs) v.push_back(x); return v;
}

// ============================ 问题②:跨文件续行防御 ============================
static void t1_cross_file_continuation() {
    std::printf("== T1 跨文件续行防御 ==\n");
    // A 末行以冒号结尾(宣告多行);B 首行无时间戳裸文本。拼接后 B 首行不得并入 A 末条。
    std::vector<std::string> fileA = L({
        "[2026-06-30 10:00:00] [HEARTBEAT] CSQ:20",
        "[2026-06-30 10:00:05] [RECOVERY L2] AT+CFUN=0 rsp:"   // 末行冒号
    });
    std::vector<std::string> fileB = L({
        "OK",                                                  // 首行无 ts 裸文本
        "[2026-06-30 11:00:03] [HEARTBEAT] CSQ:25"
    });
    std::vector<std::string> merged = fileA;
    merged.insert(merged.end(), fileB.begin(), fileB.end());

    // (a) 不给边界(旧行为):B 的 "OK" 会被并入 A 末条 —— 证明污染机制真实存在
    {
        std::vector<LogLine> ll; std::vector<std::string> ss; ParseAudit ad;
        parseLines(merged, ll, ss, &ad);   // 无 fileBoundaries
        bool polluted = false;
        for (auto& l : ll) if (l.msg.find("CFUN") != std::string::npos && l.msg.find("OK") != std::string::npos) polluted = true;
        ok(polluted, "对照:不给边界时 B 的\"OK\"确实被并入 A 末条(污染机制真实)");
        ok(ad.continuation == 1, "对照:续行合并计数 == 1");
    }

    // (b) 给出边界(B 从下标 2 开始):防御生效,不跨文件合并
    {
        std::vector<size_t> bounds = { 0, 2 };   // A 从 0,B 从 2
        std::vector<LogLine> ll; std::vector<std::string> ss; ParseAudit ad;
        parseLines(merged, ll, ss, &ad, bounds);
        bool polluted = false;
        for (auto& l : ll) if (l.msg.find("CFUN") != std::string::npos && l.msg.find("OK") != std::string::npos) polluted = true;
        ok(!polluted, "给边界后 B 的\"OK\"不再并入 A 末条(防御生效)");
        ok(ad.continuation == 0, "给边界后跨文件续行合并 == 0 次");
        // "OK" 应老实计入未识别(诚实暴露)
        ok(ad.unparsed >= 1, "被挡下的 \"OK\" 计入未识别(诚实,不静默吞掉)");
    }
}

// 同一文件内的**正常**续行必须仍然工作(边界不能误伤文件内多行条目)
static void t2_intra_file_continuation_still_works() {
    std::printf("== T2 文件内正常续行不受影响 ==\n");
    // 单文件:CFUN 应答分两行,应正常合并
    std::vector<std::string> one = L({
        "[2026-06-30 10:00:00] [HEARTBEAT] CSQ:20",
        "[2026-06-30 10:00:05] [RECOVERY L2] AT+CFUN=0 rsp:",
        "OK"                                                  // 同文件续行
    });
    std::vector<size_t> bounds = { 0 };   // 单文件,边界只有起点 0
    std::vector<LogLine> ll; std::vector<std::string> ss; ParseAudit ad;
    parseLines(one, ll, ss, &ad, bounds);
    bool merged = false;
    for (auto& l : ll) if (l.msg.find("CFUN") != std::string::npos && l.msg.find("OK") != std::string::npos) merged = true;
    ok(merged, "同文件内 \"OK\" 正常并入上一条(边界不误伤)");
    ok(ad.continuation == 1, "文件内续行合并 == 1 次");
}

// 空边界 = 旧行为完全一致(回归保护)
static void t3_empty_boundaries_is_legacy() {
    std::printf("== T3 空边界 == 旧行为(回归) ==\n");
    std::vector<std::string> raw = L({
        "[2026-06-30 10:00:00] [HEARTBEAT] rsp:",
        "OK"
    });
    std::vector<LogLine> a1, a2; std::vector<std::string> s1, s2; ParseAudit d1, d2;
    parseLines(raw, a1, s1, &d1);                    // 不传边界
    parseLines(raw, a2, s2, &d2, std::vector<size_t>{});  // 传空边界
    ok(d1.continuation == d2.continuation && d1.continuation == 1,
       "不传边界 与 传空边界 行为一致(都合并)");
}

// ============================ 问题①:时钟跳变检测 ============================
static void t4_clock_jump_detection() {
    std::printf("== T4 时钟跳变检测(合成输入,无真机样本)==\n");
    // 合成:前段 1970(未授时),中途跳到 2026(授时成功)
    std::vector<std::string> raw = L({
        "[1970-01-01 00:00:10] [HEARTBEAT] CSQ:15",
        "[1970-01-01 00:00:20] [OUTAGE] Network outage started",
        "[2026-06-30 12:00:00] [OUTAGE] Network recovered after 1786696972s", // 跳变
        "[2026-06-30 12:00:10] [HEARTBEAT] CSQ:21"
    });
    std::vector<LogLine> ll; std::vector<std::string> ss; ParseAudit ad;
    parseLines(raw, ll, ss, &ad);
    ok(ad.clockJump, "检测到时钟跳变");
    ok(ad.jumpAtLine == 3, ("跳变行号 == 3(实得 " + std::to_string(ad.jumpAtLine) + ")").c_str());
    ok(fmtTime(ad.jumpFromT, "FULL") == "1970-01-01 00:00:20", "跳变前时间正确(1970)");
    ok(fmtTime(ad.jumpToT, "FULL") == "2026-06-30 12:00:00", "跳变后时间正确(2026)");
    const ObservationStats observation = observationStats(ll);
    ok(observation.observedSpan == 20 && observation.clockDiscontinuities == 1,
       "同 source 授时跳变切成 10s+10s，不把 56 年计入实际观测");
    ok(collectOutages(ll).empty(), "跨 1970→2026 时基的 start/recovery 不配成断网");
}

// 正常日志(全程墙钟)不得误报跳变
static void t5_no_false_jump_on_normal() {
    std::printf("== T5 正常日志不误报跳变 ==\n");
    std::vector<std::string> raw = L({
        "[2026-06-30 23:59:50] [HEARTBEAT] CSQ:20",
        "[2026-07-01 00:00:10] [HEARTBEAT] CSQ:21",   // 正常跨天,不是跳变
        "[2026-07-01 00:00:20] [HEARTBEAT] CSQ:22"
    });
    std::vector<LogLine> ll; std::vector<std::string> ss; ParseAudit ad;
    parseLines(raw, ll, ss, &ad);
    ok(!ad.clockJump, "正常跨天不误报为跳变");
}

// 纯 1970 日志(设备整段未授时,如真机 unsynced 夹具)不报跳变
static void t6_pure_unsynced_no_jump() {
    std::printf("== T6 纯未授时(全程1970)不报跳变 ==\n");
    std::vector<std::string> raw = L({
        "[1970-01-01 15:10:16] [HEARTBEAT] CSQ:15",
        "[1970-01-01 15:10:26] [HEARTBEAT] CSQ:16",
        "[1970-01-01 15:10:36] [HEARTBEAT] CSQ:17"
    });
    std::vector<LogLine> ll; std::vector<std::string> ss; ParseAudit ad;
    parseLines(raw, ll, ss, &ad);
    ok(!ad.clockJump, "全程 1970 无跳变(与真机 unsynced 夹具行为一致)");
}

// 真机 unsynced 夹具:确认它确实不含跳变(锚定"无真机样本"这个事实)
static void t7_real_unsynced_fixture_no_jump() {
    std::printf("== T7 真机 unsynced 夹具确认无跳变 ==\n");
    const char* paths[] = {
        "samples/rtms_eg25/real_eg25_1.31.15_unsynced.log",
        "samples/dial_ec200a/real_ec200a_1.28.4_unsynced.log"
    };
    for (const char* p : paths) {
        std::vector<std::string> raw;
        std::FILE* f = std::fopen(p, "rb");
        if (!f) { std::printf("  [失败] 打不开 %s\n", p); g_fail++; continue; }
        char buf[4096];
        std::string acc;
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) acc.append(buf, n);
        std::fclose(f);
        splitTextLines(std::move(acc), raw);
        std::vector<LogLine> ll; std::vector<std::string> ss; ParseAudit ad;
        parseLines(raw, ll, ss, &ad);
        std::string base = p; size_t sl = base.find_last_of('/');
        if (sl != std::string::npos) base = base.substr(sl + 1);
        ok(!ad.clockJump, (base + " 无跳变(证实真机无此样本,检测也不误报)").c_str());
    }
}

// 文件与 Windows 剪贴板共用的切行规则。CRLF 必须只产生一个行边界。
static void t8_text_line_endings() {
    std::printf("== T8 LF / CRLF / CR 统一切行 ==\n");
    std::vector<std::string> lf, crlf, cr, blank, bom, empty;
    splitTextLines("a\nb\n", lf);
    splitTextLines("a\r\nb\r\n", crlf);
    splitTextLines("a\rb\r", cr);
    splitTextLines("a\r\n\r\nb", blank);
    splitTextLines("\xEF\xBB\xBF" "a\r\nb", bom);
    splitTextLines("", empty);
    ok(lf == L({"a", "b"}), "LF:末尾换行不额外造空行");
    ok(crlf == L({"a", "b"}), "CRLF:每行只切一次(Windows 粘贴回归)");
    ok(cr == L({"a", "b"}), "CR:老式终端换行可识别");
    ok(blank == L({"a", "", "b"}), "正文中的真实空行被保留");
    ok(bom == L({"a", "b"}), "BOM + CRLF 同时正确处理");
    ok(empty.empty(), "空文本不产生伪造行");
}

// 时间索引只对单调数据使用；时钟倒退时必须退回全扫描,不能漏掉窗口内证据。
static void t9_unsorted_analysis_fallback() {
    std::printf("== T9 分析时间乱序时回退全扫描 ==\n");
    Outage outage;
    outage.start = 150; outage.end = 170; outage.dur = 20; outage.recovered = true;
    outage.startLine = 2; outage.endLine = 3;
    PlatformInfo pi;
    ParseAudit audit;

    auto hasTitle = [](const std::vector<Finding>& fs, const char* needle) {
        for (const auto& f : fs)
            if (f.title.find(needle) != std::string::npos) return true;
        return false;
    };
    auto findTitle = [](const std::vector<Finding>& fs, const char* needle) -> const Finding* {
        for (const auto& f : fs)
            if (f.title.find(needle) != std::string::npos) return &f;
        return nullptr;
    };

    // 指标顺序 300→160(倒退),弱信号证据在窗口内的第二项。
    LogLine a, b;
    a.t = 300; a.ts = "300"; a.lineNo = 1; a.msg = "outside";
    b.t = 100; b.ts = "100"; b.lineNo = 2; b.msg = "before";
    MetricRow outside, weak;
    outside.t = 300; outside.csqVal = 20; outside.csqRaw = 20; outside.lineNo = 1;
    weak.t = 160; weak.csqVal = 5; weak.csqRaw = 5; weak.lineNo = 2;
    auto weakFs = analyze({a, b}, {outage}, {outside, weak}, pi, audit);
    ok(hasTitle(weakFs, "断网根因分类:弱信号"),
       "乱序指标回退全扫描,窗口内弱信号未漏掉");

    // 事件顺序同样倒退；窗口内 SLOT 必须仍能归类为切卡/选网。
    LogLine slotOutside, slotInside, operInside, cfunInside;
    slotOutside.t = 300; slotOutside.ts = "300"; slotOutside.lineNo = 1;
    slotOutside.setTag("SLOT"); slotOutside.msg = "outside switch";
    slotInside.t = 160; slotInside.ts = "160"; slotInside.lineNo = 2;
    slotInside.setTag("SLOT"); slotInside.msg = "switch in outage";
    operInside.t = 161; operInside.ts = "161"; operInside.lineNo = 3;
    operInside.setTag("OPER"); operInside.msg = "operator change in outage";
    cfunInside.t = 162; cfunInside.ts = "162"; cfunInside.lineNo = 4;
    cfunInside.setTag("CFUN"); cfunInside.msg = "CFUN reset in outage";
    auto switchFs = analyze({slotOutside, slotInside, operInside, cfunInside},
                            {outage}, {}, pi, audit);
    ok(hasTitle(switchFs, "断网根因分类:切卡/选网/CFUN 期间"),
       "乱序事件回退全扫描,窗口内切卡事件未漏掉");
    const Finding* switchFinding =
        findTitle(switchFs, "断网根因分类:切卡/选网/CFUN 期间");
    ok(switchFinding && !switchFinding->ev.empty() && switchFinding->ev[0].lineNo == 4,
       "同窗 SLOT/OPER/CFUN 保持旧证据顺序,CFUN 覆盖前两类");
}

static void t10_streaming_parser_equivalence() {
    std::printf("== T10 增量解析与批量入口等价 ==\n");
    std::vector<std::string> raw = L({
        "=== Dial Log Opened [2026-06-30 10:00:00] daykey=20260630 ===",
        "[2026-06-30 10:00:01] [RECOVERY L2] AT+CFUN=0 rsp:",
        "OK",
        "[2026-06-30 10:00:03] [HEARTBEAT] CH:SIM | CSQ:20 | RX_PKT:100"
    });
    std::vector<LogLine> batch, stream;
    std::vector<std::string> batchSessions, streamSessions;
    ParseAudit batchAudit, streamAudit;
    parseLines(raw, batch, batchSessions, &batchAudit, {2});

    StreamingLogParser parser(stream, streamSessions, raw.size());
    parser.beginFile();
    parser.pushLine(raw[0]);
    parser.pushLine(raw[1]);
    parser.beginFile();
    parser.pushLine(raw[2]);
    parser.pushLine(raw[3]);
    parser.finish(&streamAudit);

    bool sameLines = batch.size() == stream.size();
    for (size_t i = 0; sameLines && i < batch.size(); ++i) {
        sameLines = batch[i].t == stream[i].t &&
                    batch[i].lineNo == stream[i].lineNo &&
                    batch[i].ts == stream[i].ts &&
                    batch[i].tagText() == stream[i].tagText() &&
                    batch[i].msg == stream[i].msg &&
                    batch[i].sourceId == stream[i].sourceId &&
                    batch[i].fmt == stream[i].fmt &&
                    batch[i].level == stream[i].level;
    }
    bool sameAudit = batchAudit.rawTotal == streamAudit.rawTotal &&
                     batchAudit.parsed == streamAudit.parsed &&
                     batchAudit.session == streamAudit.session &&
                     batchAudit.blank == streamAudit.blank &&
                     batchAudit.continuation == streamAudit.continuation &&
                     batchAudit.unparsed == streamAudit.unparsed &&
                     batchAudit.unparsedKinds == streamAudit.unparsedKinds;
    ok(sameLines && batchSessions == streamSessions && sameAudit,
       "逐文件 pushLine 的行、会话、审计与 parseLines 完全一致");
}

static void t11_compact_record_boundaries() {
    std::printf("== T11 紧凑记录数值与未知标签边界 ==\n");
    std::vector<std::string> raw = L({
        "[2026-06-30 10:00:00] [HEARTBEAT] CSQ:+20 | Temp:-2000,+50 | RX_PKT:9223372036854775807",
        "[2026-06-30 10:00:01] [HEARTBEAT] CSQ:21 | RSRP:-2147483649 | RX_PKT:-9223372036854775808",
        "[2026-06-30 10:00:02] [RuntimeMixedTag] future firmware payload"
    });
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    parseLines(raw, lines, sessions);
    auto metrics = buildMetrics(lines);
    ok(metrics.size() == 2 && metrics[0].csqRaw == 20 && metrics[0].tempMax == 50,
       "显式正号与多温度仍按旧语义解析");
    ok(metrics.size() == 2 && metrics[1].drx == LLONG_MIN && metrics[1].rsrp == 1,
       "RX 差值溢出与超 int 信号值保持无效标记");

    bool unknownPreserved = lines.size() == 3 &&
                            lines[2].tagText() == "RuntimeMixedTag";
    LogLine copied = lines.size() == 3 ? lines[2] : LogLine{};
    if (lines.size() == 3) lines[2].setTag("ChangedAfterCopy");
    ok(unknownPreserved && copied.tagText() == "RuntimeMixedTag",
       "未知标签保留原文且 LogLine 深拷贝不悬空");
}

static void t12_cell_quality_and_correlation() {
    std::printf("== T12 小区质量、乒乓与断网关联 ==\n");
    std::vector<LogLine> lines;
    std::vector<MetricRow> metrics;
    const long long base = 1785556800;
    const char* cells[] = {"A", "A", "A", "A", "A", "B", "A", "B", "A", "B", "A", "B"};
    for (std::size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); ++i) {
        LogLine line;
        line.t = base + static_cast<long long>(i) * 10;
        line.lineNo = i + 1;
        line.sourceId = 1;
        line.ts = fmtTime(line.t, "FULL");
        line.setTag("HEARTBEAT");
        line.msg = std::string("Cell:") + cells[i];
        lines.push_back(std::move(line));

        MetricRow metric;
        metric.t = base + static_cast<long long>(i) * 10;
        metric.lineNo = i + 1;
        metric.cellId = cells[i];
        metric.csqVal = cells[i][0] == 'A' ? 5 : 20;
        metric.rsrp = cells[i][0] == 'A' ? -115 : -85;
        metric.rsrq = cells[i][0] == 'A' ? -18 : -8;
        metric.snr10 = cells[i][0] == 'A' ? -10 : 100;
        metrics.push_back(std::move(metric));
    }
    LogLine outageLine;
    outageLine.t = base + 105;
    outageLine.lineNo = 13;
    outageLine.sourceId = 1;
    outageLine.ts = fmtTime(outageLine.t, "FULL");
    outageLine.msg = "Ping failed, fault timer started";
    lines.push_back(std::move(outageLine));

    LogLine nextSource;
    nextSource.t = base + 120;
    nextSource.lineNo = 14;
    nextSource.sourceId = 2;
    nextSource.ts = fmtTime(nextSource.t, "FULL");
    nextSource.msg = "Cell:C";
    lines.push_back(std::move(nextSource));
    MetricRow sourceTwoMetric;
    sourceTwoMetric.t = base + 120;
    sourceTwoMetric.lineNo = 14;
    sourceTwoMetric.cellId = "C";
    metrics.push_back(std::move(sourceTwoMetric));

    Outage outage;
    outage.start = base + 105;
    outage.startLine = 13;
    const CellAnalysis analysis = analyzeCells(lines, metrics, {outage});
    const CellSummary* cellA = nullptr;
    for (const CellSummary& cell : analysis.cells)
        if (cell.cellId == "A") cellA = &cell;
    ok(analysis.switchCount == 7, "跨来源不造伪切换，来源内切换共 7 次");
    ok(analysis.pingPongCount == 6, "5 分钟内 A/B 往返识别为 6 次乒乓");
    ok(cellA && cellA->samples == 8 && cellA->observedDwellSec == 40,
       "小区样本与连续观测驻留时间正确");
    ok(cellA && cellA->rsrpAvg10 == -1150 && cellA->snrAvg10 == -10,
       "RSRP 与 SNR 平均值单位正确");
    ok(cellA && cellA->outageStarts == 1 && cellA->firstOutageLine == 13,
       "断网关联到同来源、10 分钟内最近小区");

    ParseAudit audit;
    auto findings = analyze(lines, {outage}, metrics, PlatformInfo{}, audit, &analysis);
    auto hasTitle = [&](const char* title) {
        for (const Finding& finding : findings)
            if (finding.title.find(title) != std::string::npos && !finding.ev.empty()) return true;
        return false;
    };
    ok(hasTitle("频繁小区切换"), "频繁切换结论带原始行证据");
    ok(hasTitle("疑似小区乒乓"), "乒乓结论带原始行证据");
    ok(hasTitle("疑似弱覆盖小区"), "弱覆盖与断网相关性结论带证据");

    std::vector<LogLine> oddLines(lines.begin(), lines.begin() + 3);
    std::vector<MetricRow> oddMetrics(metrics.begin(), metrics.begin() + 3);
    const CellAnalysis odd = analyzeCells(oddLines, oddMetrics, {});
    ok(odd.cells.size() == 1 && odd.cells[0].rsrpAvg10 == -1150 && odd.cells[0].snrAvg10 == -10,
       "非 2 次幂样本的负值平均不发生有符号/无符号提升错误");
}

static void t13_console_android_syslog_retention() {
    std::printf("== T13 全打印保留 + Android/syslog 格式 ==\n");
    std::vector<std::string> raw = L({
        "startup raw printf",
        "2026-07-17 10:00:00.123  1000  1001 I DIAL: [INIT] android full-year",
        "07-17 10:00:01.456  1000  1001 E DIAL: android threadtime",
        "raw printf after timestamp",
        "2026-07-17T10:00:02 device modem_mng[1000]: [WARN] syslog message"
    });
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);
    ok(lines.size() == 5, "5 条输入全部保留，没有只留未识别样例");
    ok(audit.parsed == 3 && audit.unparsed == 2,
       "Android/syslog 3 条结构化解析，2 条裸输出诚实计未识别");
    ok(lines[0].fmt == FMT_CONSOLE && lines[0].inferredTime &&
       lines[0].t == lines[1].t && lines[0].ts == "~2026-07-17 10:00:00",
       "文件头裸输出回填首个真实时间并显式标记为推定");
    ok(lines[1].fmt == FMT_ANDROID && lines[1].tagText() == "INIT" &&
       lines[1].ms == 123, "全年 Android threadtime 的级别/标签/毫秒正确");
    ok(lines[2].fmt == FMT_ANDROID && lines[2].level == LEVEL_ERROR &&
       lines[2].ts == "2026-07-17 10:00:01",
       "缺年份 Android threadtime 只借同文件已知年份");
    ok(lines[3].fmt == FMT_CONSOLE && lines[3].inferredTime &&
       lines[3].t == lines[2].t, "中段裸输出沿用上一条时间且保留原文");
    ok(lines[4].fmt == FMT_SYSLOG && lines[4].tagText() == "WARN",
       "RFC3339 syslog 被解析且正文标签优先");
}

static void t14_tbox_confirmed_boundaries() {
    std::printf("== T14 TBOX 真机问题边界：孤立恢复/覆盖/会话/NUL/RX/门控 ==\n");

    std::vector<std::string> raw{
        "=== Dial Log Opened [2026-08-14 08:44:25] daykey=2026-08-14 ===",
        "[2026-08-14 08:44:25] EG25 modem_mng Version: 1.31.15",
        "[2026-08-14 08:44:28] [HEARTBEAT] Network recovered after 1786696972s",
        std::string(899, '\0'),
        "=== Dial Log Opened [2026-08-21 08:44:25] daykey=2026-08-21 ===",
        "[2026-08-21 08:44:25] [HEARTBEAT] CH:SIM | rx_packets=10",
    };
    std::vector<LogLine> lines; std::vector<std::string> sessions; ParseAudit audit;
    parseLines(raw, lines, sessions, &audit, {0, 4});
    ok(audit.logOpened == 2 && sessions.size() == 1,
       "2 次日志打开只形成 1 个有版本横幅支撑的进程启动");
    ok(audit.nulBytes == 899 && audit.nulLines == 1 && audit.unparsed == 1,
       "899 个 NUL 被字节审计并作为可见损伤行保留");

    auto outages = collectOutages(lines);
    ok(outages.empty(), "无 fault start 的 1786696972s 恢复行不形成断网");
    PlatformInfo eg25; eg25.plat = PLAT_EG25;
    auto metrics = buildMetrics(lines);
    auto findings = analyze(lines, outages, metrics, eg25, audit);
    bool anomaly = false;
    for (const Finding& finding : findings)
        anomaly = anomaly || finding.title.find("恢复记录缺少可信故障起点") != std::string::npos;
    ok(anomaly, "孤立恢复仍作为时钟/日志异常保留证据，不被静默丢弃");

    std::vector<std::string> coverageRaw = L({
        "[2026-08-14 08:00:00] [HEARTBEAT] CH:SIM",
        "[2026-08-14 08:01:00] [HEARTBEAT] CH:SIM",
        "[2026-08-21 08:00:00] [HEARTBEAT] CH:SIM",
        "[2026-08-21 08:01:00] [HEARTBEAT] CH:SIM"
    });
    std::vector<LogLine> coverageLines; std::vector<std::string> coverageSessions;
    parseLines(coverageRaw, coverageLines, coverageSessions, nullptr, {0, 2});
    const ObservationStats observation = observationStats(coverageLines);
    ok(observation.observedSpan == 120 && observation.calendarSpan == 604860,
       "相隔七天的两份日志只累计各自 60s，不跨空档计算观测时长");

    std::vector<std::string> rxRaw = L({
        "[2026-08-14 08:00:00] [HEARTBEAT] CH:SIM | rx_packets=100",
        "[2026-08-21 08:00:00] [HEARTBEAT] CH:SIM | rx_packets=10"
    });
    std::vector<LogLine> rxLines; std::vector<std::string> rxSessions;
    parseLines(rxRaw, rxLines, rxSessions, nullptr, {0, 1});
    const auto rxMetrics = buildMetrics(rxLines);
    ok(rxMetrics.size() == 2 && rxMetrics[0].drx == LLONG_MIN && rxMetrics[1].drx == LLONG_MIN,
       "新日志来源重置 RX 基线，不制造 -90 伪负增量");

    std::vector<std::string> gateRaw = L({
        "[2026-08-14 09:00:00] [ROAMLINK] probe NO_PACKAGE: RBMaster missing, force SIM, policy=4",
        "[2026-08-14 09:00:01] [SDK] DataCall connected | profile=1",
        "[2026-08-14 09:00:02] [HEARTBEAT] CH:SIM | DownTime:0s",
        "[2026-08-14 09:01:00] [HEARTBEAT] Ping failed 3 consecutive times, fault timer started",
        "[2026-08-14 09:03:00] [HEARTBEAT] Network recovered after 120s"
    });
    std::vector<LogLine> gateLines; std::vector<std::string> gateSessions; ParseAudit gateAudit;
    parseLines(gateRaw, gateLines, gateSessions, &gateAudit);
    const auto gateOutages = collectOutages(gateLines);
    const auto gateFindings = analyze(gateLines, gateOutages, buildMetrics(gateLines), eg25, gateAudit);
    bool enabled = false, oldFalseClaim = false;
    for (const Finding& finding : gateFindings) {
        enabled = enabled || finding.detail.find("可见门控条件均满足") != std::string::npos;
        oldFalseClaim = oldFalseClaim || finding.detail.find("本日志的策略/通道不满足") != std::string::npos;
    }
    ok(enabled && !oldFalseClaim, "policy=4、CH=SIM、已联网不再被误判为阶梯结构性关闭");
}

static void t15_artery_persistent_outage_events() {
    std::printf("== T15 artery 标准化断网持久事件 ==\n");
    std::vector<std::string> raw = L({
        "2026-08-23 10:00:00.100 [INFO] dial_status_update (dial.c:780) - [OUTAGE] Network outage started | state=reg_check channel=SWITCHING",
        "2026-08-23 10:01:15.200 [INFO] dial_status_update (dial.c:790) - [OUTAGE] Network recovered after 75s | state=net_connected channel=SIM"
    });
    std::vector<LogLine> lines; std::vector<std::string> sessions; ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);
    const auto outages = collectOutages(lines);
    ok(audit.unparsed == 0 && lines.size() == 2 && lines[0].tagText() == "OUTAGE",
       "artery seas_log 的 [OUTAGE] 起止事件完整结构化解析");
    ok(outages.size() == 1 && outages[0].recovered && outages[0].dur == 75,
       "同 source 标准事件配成 1 次可信 75s 断网");
}

/* Both products emit a +COPS query snapshot, but artery uses untagged
 * seas_log text for the restore while RTMS EG25 uses [REG TIMEOUT].  The
 * analyzer must require a confirmed COPS=0 response and a later registration
 * success; neither a manual-select command nor an unconfirmed COPS=0 is enough. */
static void t15b_manual_cops_restore_correlation() {
    std::printf("== T15b EG25 手动选网解锁恢复关联 ==\n");
    const auto hasTitle = [](const std::vector<Finding>& fs, const char* needle) {
        for (const Finding& f : fs)
            if (f.title.find(needle) != std::string::npos) return true;
        return false;
    };
    const auto hasDetail = [](const std::vector<Finding>& fs, const char* needle) {
        for (const Finding& f : fs)
            if (f.detail.find(needle) != std::string::npos) return true;
        return false;
    };

    std::vector<std::string> arteryRaw = L({
        "2026-09-09 03:30:50.000 [INFO] dial_task (dial.c:1668) - AT+COPS=1,2,\"46001\"",
        "2026-09-09 03:31:15.304 [INFO] main (main.c:191) - [INIT] COPS: +COPS: 1 (mode=1 manual) | CEREG: +CEREG: 0,0",
        "2026-09-09 03:31:21.464 [INFO] dial_task (dial.c:1011) - state: sim_op -> reg_check",
        "2026-09-09 03:34:16.178 [INFO] main (main.c:378) - [INIT][SIM-ACCOUNT] SIM=SIM1 SUSPECTED subscription issue: SIM READY, CSQ>=10 and REG=0 continuously for >=120s",
        "2026-09-09 03:36:22.067 [INFO] dial_task (dial.c:2201) - reg timeout: AT+COPS=0 unlock OK, back to auto operator",
        "2026-09-09 03:36:38.512 [INFO] dial_task (dial.c:1011) - state: sim_op -> reg_check",
        "2026-09-09 03:37:17.589 [ERROR] dial_task (dial.c:1144) - Dial_st: dial_stat_reg_check failed",
        "2026-09-09 03:44:15.089 [INFO] dial_task (dial.c:1011) - state: reg_check -> cereg_check"
    });
    std::vector<LogLine> arteryLines; std::vector<std::string> arterySessions; ParseAudit arteryAudit;
    parseLines(arteryRaw, arteryLines, arterySessions, &arteryAudit);
    const PlatformInfo artery = detectPlatform(arteryLines);
    const auto arteryFindings = analyze(arteryLines, collectOutages(arteryLines),
                                        buildMetrics(arteryLines), artery, arteryAudit);
    ok(arteryAudit.unparsed == 0 && artery.plat == PLAT_ARTERY,
       "artery 的实际 seas_log 包络完整解析");
    ok(hasTitle(arteryFindings, "手动选网解除后恢复注册") &&
       hasDetail(arteryFindings, "强关联推断"),
       "artery: COPS=1 → COPS=0成功 → 注册成功生成保守关联结论");
    ok(hasTitle(arteryFindings, "账户/订阅异常提示已降级"),
       "artery: 有手动选网恢复链时 SUSPECTED 不再作为主归因");
    ok(hasTitle(arteryFindings, "冷启动注册阻塞") &&
       hasTitle(arteryFindings, "注册等待期存在日志空洞") &&
       hasDetail(arteryFindings, "应用自身的 AT+COPS=1") &&
       hasDetail(arteryFindings, "12m54s"),
       "artery: 首次冷启动时长不被 CFUN 后的第二轮 reg_check 缩短，日志空洞和应用选网来源均被保留");

    std::vector<std::string> incompleteRaw = L({
        "2026-09-09 03:31:15.304 [INFO] main (main.c:191) - [INIT] COPS: +COPS: 1 (mode=1 manual) | CEREG: +CEREG: 0,0",
        "2026-09-09 03:36:22.067 [INFO] dial_task (dial.c:2201) - reg timeout: AT+COPS=0 unlock OK, back to auto operator"
    });
    std::vector<LogLine> incompleteLines; std::vector<std::string> incompleteSessions; ParseAudit incompleteAudit;
    parseLines(incompleteRaw, incompleteLines, incompleteSessions, &incompleteAudit);
    const auto incompleteFindings = analyze(incompleteLines, collectOutages(incompleteLines),
                                            buildMetrics(incompleteLines), detectPlatform(incompleteLines), incompleteAudit);
    ok(hasTitle(incompleteFindings, "手动选网已解锁但未见后续注册成功"),
       "证据强度分级: 只有 COPS=0 成功、没有恢复注册时不得报强关联");

    std::vector<std::string> rtmsRaw = L({
        "[2026-09-09 03:31:15] [INIT] COPS: +COPS: 1 (mode=1 manual) | CEREG: +CEREG: 0,0",
        "[2026-09-09 03:31:20] [HEARTBEAT] CH:SIM | SIM:1 | REG:0 | CSQ:16 | DownTime:0s",
        "[2026-09-09 03:31:21] [STATE] sim_op -> reg_check",
        "[2026-09-09 03:36:22] [REG TIMEOUT] COPS not auto (+COPS: 1), forcing COPS=0",
        "[2026-09-09 03:36:23] [REG TIMEOUT] AT+COPS=0 rsp:",
        "OK",
        "[2026-09-09 03:44:15] [STATE] reg_check -> cereg_check"
    });
    std::vector<LogLine> rtmsLines; std::vector<std::string> rtmsSessions; ParseAudit rtmsAudit;
    parseLines(rtmsRaw, rtmsLines, rtmsSessions, &rtmsAudit);
    const PlatformInfo rtms = detectPlatform(rtmsLines);
    const auto rtmsFindings = analyze(rtmsLines, collectOutages(rtmsLines),
                                      buildMetrics(rtmsLines), rtms, rtmsAudit);
    ok(rtmsAudit.unparsed == 0 && rtmsAudit.continuation == 1 && rtms.plat == PLAT_EG25,
       "RTMS EG25 的 [REG TIMEOUT] COPS=0 多行应答完整解析");
    ok(hasTitle(rtmsFindings, "手动选网解除后恢复注册"),
       "RTMS EG25: COPS=1 → COPS=0成功 → 注册成功生成同一结论");
    ok(hasTitle(rtmsFindings, "冷启动注册阻塞"),
       "RTMS EG25: 启动注册等待独立于运行期断网统计");
}

static void t16_eg25_persistent_failure_diagnostics() {
    std::printf("== T16 EG25 新增落盘故障：初始化退出/重启/APN/Start ==\n");
    std::vector<std::string> raw = L({
        "[2026-08-24 09:00:00] EG25 modem_mng Version: 1.31.18",
        "[2026-08-24 09:00:10] [SDK] Initialization data call failure, ret=-77",
        "[2026-08-24 09:00:10] [FATAL][PROCESS EXIT] QL_Data_Call_Init failed | ret=-77 | pid=4321",
        "[2026-08-24 09:00:15] EG25 modem_mng Version: 1.31.18",
        "[2026-08-24 09:00:16] [APN] json_root is NULL",
        "[2026-08-24 09:00:17] [SDK] profile 1 start data call failure: 0x2a",
        "[2026-08-24 09:00:18] [LOG_E] Failed to read /proc/uptime"
    });
    std::vector<LogLine> lines; std::vector<std::string> sessions; ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);
    ok(audit.unparsed == 0 && lines.size() == raw.size(),
       "新增 SD 日志全部结构化解析，无未识别行");
    ok(lines[2].tagText() == "FATAL" &&
       lines[2].msg.find("[PROCESS EXIT]") == 0,
       "嵌套 FATAL/PROCESS EXIT 保留退出动作正文");
    ok(lines[6].tagText() == "LOG_E" && isErrLine(lines[6]),
       "兼容当前 [LOG_E] 标题并纳入错误时间线");

    PlatformInfo eg25; eg25.plat = PLAT_EG25;
    const auto findings = analyze(lines, collectOutages(lines), buildMetrics(lines), eg25, audit);
    bool initRestart = false, exactRetPid = false, apn = false, start = false;
    for (const Finding& finding : findings) {
        initRestart = initRestart ||
            (finding.title.find("QL_Data_Call_Init 初始化失败") != std::string::npos &&
             finding.title.find("进程主动退出") != std::string::npos &&
             finding.title.find("随后检测到重新启动") != std::string::npos);
        apn = apn || finding.title.find("APN 配置文件读取/解析失败 1 次") != std::string::npos;
        start = start || finding.title.find("数据调用启动失败 1 次") != std::string::npos;
        for (const Evidence& evidence : finding.ev)
            if (evidence.text.find("ret=-77") != std::string::npos &&
                evidence.text.find("pid=4321") != std::string::npos)
                exactRetPid = true;
    }
    ok(initRestart && exactRetPid,
       "致命退出与后续版本横幅关联，并钉死 ret=-77/pid=4321 证据");
    ok(apn && start, "APN 解析失败和 Data Call Start 失败分别生成有边界的结论");
}

static void t17_datacall_initiator_reason_classification() {
    std::printf("== T17 DataCall 发起方/reason 分类与新业务标题 ==\n");
    std::vector<std::string> raw = L({
        "2026-08-24 10:00:00.000 [INFO] dail_stop_data_call (dial.c:439) - [SDK] DataCall stop requested | reason=LICENSE_FAILURE_REDIAL profile=1",
        "2026-08-24 10:00:00.100 [INFO] data_call_state_callback (dial.c:378) - [SDK] DataCall disconnected | initiator=APP_STOP reason=LICENSE_FAILURE_REDIAL err=0x0",
        "2026-08-24 10:00:01.000 [INFO] dail_stop_data_call (dial.c:439) - [SDK] DataCall stop requested | reason=START_CALL_TIMEOUT profile=1",
        "2026-08-24 10:00:01.100 [INFO] data_call_state_callback (dial.c:378) - [SDK] DataCall disconnected | initiator=APP_STOP reason=START_CALL_TIMEOUT err=0x0",
        "2026-08-24 10:00:02.000 [INFO] dail_stop_data_call (dial.c:439) - [SDK] DataCall stop requested | reason=PREFER_ROAMLINK_RETRY profile=1",
        "2026-08-24 10:00:02.100 [INFO] data_call_state_callback (dial.c:378) - [SDK] DataCall disconnected | initiator=APP_STOP reason=PREFER_ROAMLINK_RETRY err=0x0",
        "2026-08-24 10:00:03.000 [INFO] dail_stop_data_call (dial.c:439) - [SDK] DataCall stop requested | reason=SIM_TCP_FAILURE_SWITCH profile=1",
        "2026-08-24 10:00:03.100 [INFO] data_call_state_callback (dial.c:378) - [SDK] DataCall disconnected | initiator=APP_STOP reason=SIM_TCP_FAILURE_SWITCH err=0x0",
        "2026-08-24 10:00:04.000 [INFO] dail_stop_data_call (dial.c:439) - [SDK] DataCall stop requested | reason=REGISTRATION_TIMEOUT_SWITCH profile=1",
        "2026-08-24 10:00:04.100 [INFO] data_call_state_callback (dial.c:378) - [SDK] DataCall disconnected | initiator=APP_STOP reason=REGISTRATION_TIMEOUT_SWITCH err=0x0",
        "2026-08-24 10:00:05.000 [INFO] data_call_state_callback (dial.c:381) - [SDK] DataCall disconnected | initiator=SDK_URC reason=UNSOLICITED err=0xd",
        "2026-08-24 10:00:06.000 [INFO] data_call_state_callback (dial.c:381) - [SDK] DataCall disconnected | profile=1 err=0xd",
        "[2026-08-24 10:00:07] [SDK] DataCall disconnected | profile=1 family=v4 err=0x0",
        "[2026-08-24 10:00:08] [SYSTEM] uptime read failed | path=/proc/uptime",
        "[2026-08-24 10:00:09] [RECOVERY] reset suppressed | reason=BOOT_GUARD uptime=57s threshold=2000s",
        "[2026-08-24 10:00:10] [APN] matched config | apn=internet.lte.cxn iccid_prefix=894642 username_set=user password_set=secret"
    });
    std::vector<LogLine> lines; std::vector<std::string> sessions; ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);
    const DataCallStats stats = collectDataCallStats(lines);
    ok(audit.unparsed == 0 && lines.size() == raw.size(),
       "artery 与 modem_mng 新旧 DataCall/业务标题全部结构化解析");
    ok(stats.stopRequested == 5 && stats.disconnected == 8 && stats.appStop == 5 &&
       stats.sdkUrc == 1 && stats.unsolicited == 1 && stats.legacy == 2 &&
       stats.otherInitiator == 0,
       "精确分类 Stop请求=5、APP_STOP=5、UNSOLICITED=1、旧格式=2");
    const char* reasons[] = { "LICENSE_FAILURE_REDIAL", "START_CALL_TIMEOUT",
        "PREFER_ROAMLINK_RETRY", "SIM_TCP_FAILURE_SWITCH",
        "REGISTRATION_TIMEOUT_SWITCH", "UNSOLICITED" };
    bool exactReasons = stats.reasons.size() == 6;
    for (const char* reason : reasons) {
        auto it = stats.reasons.find(reason);
        exactReasons = exactReasons && it != stats.reasons.end() && it->second == 1;
    }
    ok(exactReasons, "6 种 reason 均精确汇总为 1 次，不重复计算 Stop 请求");
    ok(!isErrLine(lines[1]) && isErrLine(lines[10]) && !isErrLine(lines[11]) &&
       isErrLine(lines[13]) && isEventLine(lines[14]) && isEventLine(lines[15]),
       "APP_STOP/旧格式不误报；UNSOLICITED 与 SYSTEM失败为告警，RECOVERY/APN进时间线");

    const PlatformInfo platform = detectPlatform(lines);
    const auto findings = analyze(lines, {}, {}, platform, audit);
    const Finding* app = nullptr; const Finding* urc = nullptr;
    for (const Finding& finding : findings) {
        if (finding.title.find("应用主动停止 DataCall 5 次") != std::string::npos) app = &finding;
        if (finding.title.find("SDK 非预期断线(SDK_URC/UNSOLICITED) 1 次") != std::string::npos) urc = &finding;
    }
    ok(app && app->severity == 0 && app->detail.find("START_CALL_TIMEOUT=1") != std::string::npos &&
       app->detail.find("SIM_TCP_FAILURE_SWITCH=1") != std::string::npos,
       "APP_STOP 仅生成信息结论并保留精确 reason 汇总");
    ok(urc && urc->severity == 1 && !urc->ev.empty() &&
       urc->ev[0].text.find("err=0xd") != std::string::npos,
       "SDK_URC/UNSOLICITED 生成告警并保留原始错误码证据");
}

static void t18_artery_datacall_exit_diagnostic() {
    std::printf("== T18 artery 1.29.18 DataCall 初始化退出诊断 ==\n");
    std::vector<std::string> raw = L({
        "2026-08-26 10:00:00.000 [INFO] \x1b[0mmain (main.c:208) - DIAL Version: 1.29.18",
        "2026-08-26 10:00:10.000 [ERROR] \x1b[0mdail_start_data_call (dial.c:398) - DataCall initialization failed; shutting down dial-owned RBMaster before exit(0)",
        "2026-08-26 10:00:10.100 [ERROR] \x1b[0mdail_start_data_call (dial.c:400) - DataCall initialization failure: exiting dial with status 0 for supervisor restart",
        "2026-08-26 10:00:20.000 [INFO] \x1b[0mmain (main.c:208) - DIAL Version: 1.29.18"
    });
    std::vector<LogLine> lines; std::vector<std::string> sessions; ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);
    const auto findings = analyze(lines, {}, {}, detectPlatform(lines), audit);
    const Finding* exitFinding = nullptr;
    for (const Finding& finding : findings) {
        if (finding.title.find("artery DataCall 初始化失败，进程主动退出 1 次") != std::string::npos)
            exitFinding = &finding;
    }
    bool cleanupEvidence = false;
    if (exitFinding) {
        for (const Evidence& evidence : exitFinding->ev)
            cleanupEvidence = cleanupEvidence ||
                evidence.text.find("shutting down dial-owned RBMaster") != std::string::npos;
    }
    ok(audit.unparsed == 0 && exitFinding &&
       exitFinding->title.find("随后检测到重新启动") != std::string::npos && cleanupEvidence,
       "SEAS 精确退出文案生成 artery 结论，并保留清理和同源重启证据");

    std::vector<std::string> negativeRaw = L({
        "[2026-08-26 11:00:00] [SDK] DataCall initialization failure: exiting dial with status 0 for supervisor restart",
        "2026-08-26 11:00:01.000 [ERROR] \x1b[0mdail_start_data_call (dial.c:398) - DataCall initialization failed; shutting down dial-owned RBMaster before exit(0)"
    });
    std::vector<LogLine> negativeLines; std::vector<std::string> negativeSessions; ParseAudit negativeAudit;
    parseLines(negativeRaw, negativeLines, negativeSessions, &negativeAudit);
    const auto negativeFindings = analyze(negativeLines, {}, {}, detectPlatform(negativeLines), negativeAudit);
    bool falsePositive = false;
    for (const Finding& finding : negativeFindings)
        falsePositive = falsePositive || finding.title.find("artery DataCall 初始化失败") != std::string::npos;
    ok(!falsePositive, "非 SEAS 或仅清理 RBMaster 的日志不触发 artery 初始化退出诊断");
}

// 2026-08-31 产品提交把启动横幅统一为带平台的展示版本。此处同时钉死：
// 1) 首字母大写的 Modem_mng 仍是启动证据；2) 重启后 RX 基线清空；
// 3) 展示版本可直接标识所有已发布平台，而无需等待心跳特征。
static void t19_display_version_platform_and_restart() {
    std::printf("== T19 平台化展示版本与启动边界 ==\n");
    std::vector<std::string> raw = L({
        "[2026-08-31 12:00:00] Modem_mng Version: rtms_eg25_1.31.19",
        "[2026-08-31 12:00:01] [HEARTBEAT] CH:SIM | CSQ:20 | rx_packets=100",
        "[2026-08-31 12:01:00] Modem_mng Version: rtms_eg25_1.31.19",
        "[2026-08-31 12:01:01] [HEARTBEAT] CH:SIM | CSQ:20 | rx_packets=10"
    });
    std::vector<LogLine> lines; std::vector<std::string> sessions; ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);
    const PlatformInfo eg25 = detectPlatform(lines);
    const auto metrics = buildMetrics(lines);
    ok(audit.programStarted == 2 && sessions.size() == 2,
       "大写 Modem_mng Version 作为两次独立启动证据");
    ok(eg25.plat == PLAT_EG25 && eg25.name.find("rtms") != std::string::npos,
       "rtms_eg25 展示版本直接识别 EG25");
    ok(metrics.size() == 2 && metrics[1].drx == LLONG_MIN,
       "新版横幅后的首个 RX 样本不继承上一进程基线");

    struct Case { const char* version; Platform platform; };
    const Case cases[] = {
        {"DIAL Version: dial_eg25_1.29.18", PLAT_ARTERY},
        {"DIAL Version: dial_ec200a_1.28.14", PLAT_EC200A},
        {"Modem_mng Version: rtms_ag35_1.32.0", PLAT_AG35},
        {"Modem_mng Version: rtms_ec200a_1.31.11", PLAT_EC200A},
        {"Modem_mng Version: rtms_eg25_1.31.19", PLAT_EG25},
        {"Modem_mng Version: rtms_imx6ull_1.24", PLAT_IMX},
        {"Modem_mng Version: rtms_rk3506j_1.28", PLAT_RK3506J},
    };
    bool allExact = true;
    for (const Case& c : cases) {
        const std::string line = std::string("[2026-08-31 12:00:00] ") + c.version;
        std::vector<LogLine> oneLine; std::vector<std::string> oneSession; ParseAudit oneAudit;
        parseLines(L({line.c_str()}), oneLine, oneSession, &oneAudit);
        const PlatformInfo platform = detectPlatform(oneLine);
        allExact = allExact && oneAudit.programStarted == 1 && oneSession.size() == 1 &&
                   platform.plat == c.platform &&
                   platform.evidence.find("展示版本") != std::string::npos;
    }
    ok(allExact, "七种新版展示版本均直接识别平台并计为启动证据");
}

// IMX6ULL 1.25.0 真机格式：HB30/HB300 是状态采样，online 边沿与 RECOVERY
// 形成断网；阶段失败标签必须进入证据化结论，而不能套旧 HEARTBEAT/L1-L3 规则。
static void t20_imx6ull_state_machine() {
    std::printf("== T20 IMX6ULL 心跳、断网与状态机 ==\n");
    std::vector<std::string> raw = L({
        "[2026-09-07 12:00:00] Modem_mng Version: rtms_imx6ull_1.25.0",
        "[2026-09-07 12:00:01] [HB30] online=1 | downtime_s=0 | cereg=5 | pdp=1 | csq=18 | RSRP:-104dBm | RSRQ:-8dB | SNR:8.4dB | RSSI:-56dBm | probe_ms=302 | temp_c=56 | sample_age_ms=1089",
        "[2026-09-07 12:00:01] [HB300] online=1 | downtime_s=0 | if=usb0 | cereg=5 | pdp=1 | csq=18 | RSRP:-104dBm | RSRQ:-8dB | SNR:8.4dB | RSSI:-56dBm | temp_c=56 | serving_cell=\"cell_id=99D991;pci=345;earfcn=100;band=1;tac=755C\" | traffic_valid=1 | rx_packets=411 | sample_age_ms=1000",
        "[2026-09-07 12:00:10] [HB30] online=0 | downtime_s=0 | state=CHECK_CONNECTION | cereg=5 | pdp=1 | csq=17 | RSRP:-105dBm | RSRQ:-9dB | SNR:8.0dB | RSSI:-57dBm | temp_c=57 | fail_streak=1 | retry=0 | reason=\"internet probe failed\"",
        "[2026-09-07 12:00:11] [FAILURE] state=FAILURE_RETRY pdn=1 if=usb0 reason=\"internet probe failed\"",
        "[2026-09-07 12:00:11] [RETRY] number=1 | wait_s=5 | downtime_s=1 | reason=\"internet probe failed\"",
        "[2026-09-07 12:01:00] [RECOVERY] downtime_s=50 | failures=1 | retries=1 | last_probe_ms=300 | reason=\"internet probe failed\"",
        "[2026-09-07 12:01:01] [HB30] online=1 | downtime_s=0 | cereg=5 | pdp=1 | csq=18 | RSRP:-103dBm | RSRQ:-7dB | SNR:8.5dB | RSSI:-55dBm | probe_ms=300 | temp_c=56 | sample_age_ms=1000",
        "[2026-09-07 12:02:00] [SIM] SIM not ready",
        "[2026-09-07 12:02:01] [REG] registration wait timed out CEREG=0",
        "[2026-09-07 12:02:02] [PDP] wait exhausted pdn=0 interface_present=0",
        "[2026-09-07 12:02:03] [DHCP] start failed interface=usb0",
        "[2026-09-07 12:02:04] [DEVICE] USB enumeration timed out vendor=2c7c product=0901 ports=0 net=(none)",
        "[2026-09-07 12:02:05] [AT] response timed out port=/dev/ttyUSB0 request=AT"
    });
    std::vector<LogLine> lines; std::vector<std::string> sessions; ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);
    const PlatformInfo platform = detectPlatform(lines);
    const auto metrics = buildMetrics(lines);
    const auto outages = collectOutages(lines);
    const auto findings = analyze(lines, outages, metrics, platform, audit);
    ok(platform.plat == PLAT_IMX && audit.programStarted == 1,
       "IMX6ULL 展示版本准确识别并形成启动边界");
    ok(metrics.size() == 4 && metrics[1].ch == "IMX6ULL" && metrics[1].cellId == "99D991" &&
       metrics[1].pci == 345 && metrics[1].tac == 0x755C && metrics[1].rx == 411 &&
       metrics[1].snr10 == 84 && metrics[1].tempMax == 56,
       "HB30/HB300 的信号、温度、流量和 serving_cell 字段进入指标模型");
    ok(outages.size() == 1 && outages[0].recovered && outages[0].dur == 50 &&
       outages[0].startLine == 4 && outages[0].endLine == 7,
       "IMX online=1→0 与 RECOVERY 配成可信 50s 断网");
    bool retry = false, sim = false, reg = false, pdp = false, network = false, device = false;
    for (const Finding& finding : findings) {
        retry = retry || finding.title.find("退避重试") != std::string::npos;
        sim = sim || finding.title.find("SIM 未就绪") != std::string::npos;
        reg = reg || finding.title.find("注册等待失败") != std::string::npos;
        pdp = pdp || finding.title.find("PDP 数据连接失败") != std::string::npos;
        network = network || finding.title.find("DHCP/IPv4/路由") != std::string::npos;
        device = device || finding.title.find("拓扑或 AT 通道") != std::string::npos;
    }
    ok(retry && sim && reg && pdp && network && device,
       "IMX 状态机的失败阶段全部生成有源码直证的结论");
}

// RTMS 1.25.1 将 RECOVERY 扩展为整个分级恢复过程的结构化记录。开始/重试/
// 升级绝不能关闭 outage；只有 CHECK_CONNECTION 成功路径的 downtime_s 才是恢复完成。
static void t21_imx6ull_1251_recovery_and_at_health() {
    std::printf("== T21 IMX6ULL 1.25.1 分级恢复与 AT 健康 ==\n");
    std::vector<std::string> raw = L({
        "[2026-09-08 10:00:00] Modem_mng Version: rtms_imx6ull_1.25.1",
        "[2026-09-08 10:00:01] [HB30] online=1 | downtime_s=0 | cereg=5 | pdp=1 | csq=20 | at_timeout=0 | at_probe=ok | sample_age_ms=3",
        "[2026-09-08 10:00:10] [HB30] online=0 | downtime_s=0 | state=CHECK_CONNECTION | cereg=5 | pdp=1 | csq=19 | reason=\"internet probe failed\"",
        "[2026-09-08 10:00:11] [RECOVERY] class=DATA_PATH level=L2_PDP action=enter next=RECOVER_PDP reason=\"connection failed after 5 interface-bound internet checks\"",
        "[2026-09-08 10:00:12] [RECOVERY] class=DATA_PATH level=L2_PDP action=soft-rebuild attempt=1/3 reason=\"connection failed after 5 interface-bound internet checks\"",
        "[2026-09-08 10:00:20] [RECOVERY] class=PDP level=L3_CFUN action=escalate reason=\"PDP soft-rebuild limit exhausted\"",
        "[2026-09-08 10:00:21] [RECOVERY] class=PDP level=L3_CFUN action=cycle attempt=1 reason=\"PDP soft-rebuild limit exhausted\"",
        "[2026-09-08 10:00:30] [AT] timeout event request=registration generation=7; basic AT probe failed streak=3/3",
        "[2026-09-08 10:00:31] [RECOVERY] class=AT level=L4_HARDWARE action=enter next=RECOVER_HARDWARE reason=\"three consecutive telemetry-triggered basic AT probes failed\"",
        "[2026-09-08 10:00:32] [RECOVERY] class=AT level=L4_HARDWARE action=power-cycle attempt=1 if=usb0 ip=(none) gw=(none) route=0 dhcp=0 reason=\"three consecutive telemetry-triggered basic AT probes failed\"",
        "[2026-09-08 10:00:40] [HB300] online=0 | downtime_s=30 | if=usb0 | cereg=0 | pdp=0 | csq=99 | at_timeout=1 | at_probe=fail | detailed_at_timeout=1 | detailed_at_stage=regular_telemetry | traffic_valid=0 | sample_age_ms=5",
        "[2026-09-08 10:01:40] [RECOVERY] downtime_s=90 | failures=2 | retries=3 | last_probe_ms=301 | reason=\"connection failed after 5 interface-bound internet checks\"",
        "[2026-09-08 10:01:41] [HB30] online=1 | downtime_s=0 | cereg=5 | pdp=1 | csq=20 | at_timeout=0 | at_probe=ok | sample_age_ms=4",
        "[2026-09-08 10:02:00] [RECOVERY] class=CONFIGURATION level=NONE action=enter next=CONFIG_ERROR reason=\"RTMS_MODEM_PDP_TYPE is invalid\""
    });
    std::vector<LogLine> lines; std::vector<std::string> sessions; ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);
    const auto metrics = buildMetrics(lines);
    const auto outages = collectOutages(lines);
    const auto findings = analyze(lines, outages, metrics, detectPlatform(lines), audit);
    bool telemetry = false, probe = false, pdp = false, cfun = false, hardware = false, config = false;
    for (const Finding& finding : findings) {
        telemetry = telemetry || finding.title.find("AT 遥测命令超时") != std::string::npos;
        probe = probe || finding.title.find("AT 基础确认探测失败") != std::string::npos;
        pdp = pdp || finding.title.find("PDP 软重建") != std::string::npos;
        cfun = cfun || finding.title.find("CFUN 射频恢复") != std::string::npos;
        hardware = hardware || finding.title.find("硬件级模组恢复") != std::string::npos;
        config = config || finding.title.find("拨号配置错误") != std::string::npos;
    }
    ok(audit.unparsed == 0 && lines[3].tagText() == "RECOVERY" && lines[13].tagText() == "RECOVERY",
       "1.25.1 结构化恢复记录及 CONFIG_ERROR 目标状态均被完整保留");
    ok(outages.size() == 1 && outages[0].recovered && outages[0].dur == 90 &&
       outages[0].startLine == 3 && outages[0].endLine == 12,
       "进行中的 L2/L3/L4 RECOVERY 不提前结束断网，仅 downtime_s=90 完成配对");
    ok(metrics.size() == 4 && metrics[2].atTelemetryTimeout == 1 &&
       metrics[2].atBasicProbe == 0 && metrics[2].detailedAtTimeout == 1 &&
       metrics[2].detailedAtStage == "regular_telemetry",
       "HB300 的 AT 超时、确认探测和详细遥测超时状态进入指标模型");
    ok(telemetry && probe && pdp && cfun && hardware && config,
       "AT 健康、PDP/CFUN/硬件分级恢复和配置终止均生成对应结论");
}

int main() {
    t1_cross_file_continuation();
    t2_intra_file_continuation_still_works();
    t3_empty_boundaries_is_legacy();
    t4_clock_jump_detection();
    t5_no_false_jump_on_normal();
    t6_pure_unsynced_no_jump();
    t7_real_unsynced_fixture_no_jump();
    t8_text_line_endings();
    t9_unsorted_analysis_fallback();
    t10_streaming_parser_equivalence();
    t11_compact_record_boundaries();
    t12_cell_quality_and_correlation();
    t13_console_android_syslog_retention();
    t14_tbox_confirmed_boundaries();
    t15_artery_persistent_outage_events();
    t15b_manual_cops_restore_correlation();
    t16_eg25_persistent_failure_diagnostics();
    t17_datacall_initiator_reason_classification();
    t18_artery_datacall_exit_diagnostic();
    t19_display_version_platform_and_restart();
    t20_imx6ull_state_machine();
    t21_imx6ull_1251_recovery_and_at_health();
    std::printf("\n%s 失败 %d 项\n", g_fail ? "**" : "==", g_fail);
    return g_fail ? 1 : 0;
}
