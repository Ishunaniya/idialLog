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
    t16_eg25_persistent_failure_diagnostics();
    std::printf("\n%s 失败 %d 项\n", g_fail ? "**" : "==", g_fail);
    return g_fail ? 1 : 0;
}
