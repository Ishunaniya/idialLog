// modemv2test.cpp — modem_mng_v2 从日志包络到诊断结论的端到端回归。
//
// 日志正文逐字取自 rtms_sdk/apps/modem_mng_v2/src/modem/modem.c；
// 仅时间、主机名、PID 和 printf 参数值为测试夹具填充值。
#include "logmodel.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace dl;

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "[OK]" : "[FAIL]", message);
    if (!condition) ++failures;
}

struct Result {
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    PlatformInfo platform;
    std::vector<Outage> outages;
    std::vector<MetricRow> metrics;
    std::vector<Finding> findings;
};

Result run(const std::vector<std::string>& raw,
           const std::vector<size_t>& fileBoundaries = {}) {
    Result result;
    parseLines(raw, result.lines, result.sessions, &result.audit, fileBoundaries);
    result.platform = detectPlatform(result.lines);
    result.outages = collectOutages(result.lines);
    result.metrics = buildMetrics(result.lines);
    result.findings = analyze(result.lines, result.outages, result.metrics,
                              result.platform, result.audit);
    return result;
}

bool auditConsistent(const ParseAudit& audit) {
    return audit.rawTotal == audit.parsed + audit.session + audit.blank +
           audit.continuation + audit.unparsed;
}

bool findingContains(const std::vector<Finding>& findings, const std::string& text) {
    for (const Finding& finding : findings)
        if (finding.title.find(text) != std::string::npos ||
            finding.detail.find(text) != std::string::npos)
            return true;
    return false;
}

bool everyFindingHasEvidence(const std::vector<Finding>& findings) {
    for (const Finding& finding : findings)
        if (finding.ev.empty()) return false;
    return true;
}

void testBusyBoxEnvelope() {
    std::puts("== BusyBox RFC3164 包络 ==");
    const std::vector<std::string> raw{
        "Aug 24 12:34:56 rk3576 user.info modem_mng_v2[123]: ===== modem_mng_v2 start =====",
        "Aug 24 12:34:57 rk3576 modem_mng_v2[123]: phase: INIT(0) -> READY(8) tty=/dev/ttyUSB2 module=EG25",
        "<11>Aug 24 12:34:58 rk3576 modem_mng_v2: No registered so long, reset modem",
        "Aug 24 12:34:59 rk3576 user.warning cron[8]: generic warning",
        "Aug 24 12:35:00 this is ordinary prose: not a syslog envelope",
        "Aug 24 12:35:01 rk3576 user.info modem_mng_v2[]: invalid empty pid",
    };
    Result result = run(raw);

    expect(result.lines.size() == raw.size(), "所有原始行均保留");
    expect(auditConsistent(result.audit), "RFC3164 场景解析审计自洽");
    expect(result.audit.parsed == 4 && result.audit.unparsed == 2,
           "facility、无 facility、PRI 共 4 条合法包络，2 条伪包络未识别");
    expect(result.lines[0].fmt == FMT_SYSLOG &&
           result.lines[0].tagText() == "MODEM_MNG_V2" &&
           result.lines[0].level == LEVEL_INFO,
           "user.info 与 modem_mng_v2 app 正确结构化");
    expect(result.lines[0].inferredTime && !result.lines[0].ts.empty() &&
           result.lines[0].ts.front() == '~',
           "RFC3164 缺失年份被明确标记为推定值");
    expect(result.lines[1].fmt == FMT_SYSLOG &&
           result.lines[1].tagText() == "MODEM_MNG_V2" &&
           result.lines[1].level == LEVEL_NONE,
           "无 facility 的 v2 syslog 可解析且不伪造级别");
    expect(result.lines[2].level == LEVEL_ERROR,
           "PRI=11 映射为 error severity");
    expect(result.lines[3].tagText() == "SYSLOG" &&
           result.lines[3].level == LEVEL_WARNING,
           "generic RFC3164 可解析但不误标为 modem_mng_v2");
    expect(result.lines[4].fmt == FMT_CONSOLE && result.lines[5].fmt == FMT_CONSOLE,
           "普通月份文本和非法空 PID 不误收为 RFC3164");

    long long first = 0;
    expect(firstTimestamp(raw, &first) && first == result.lines[0].t,
           "首时间戳定序已支持 RFC3164");
}

void testYearAnchorAndRollover() {
    std::puts("== RFC3164 年份锚定与跨年 ==");
    const std::vector<std::string> raw{
        "2024-12-30T23:59:58 rk3576 collector[1]: explicit year anchor",
        "Dec 31 23:59:58 rk3576 user.info modem_mng_v2[2]: [READY] WAN ping OK -> network_online=1",
        "Dec 31 23:59:59 rk3576 user.info modem_mng_v2[2]: [READY] WAN ping fail -> network_online=0",
        "Jan  1 00:00:01 rk3576 user.info modem_mng_v2[2]: [READY] WAN ping OK -> network_online=1",
    };
    Result result = run(raw);

    expect(result.audit.parsed == 4 && result.audit.unparsed == 0,
           "明确年份与 RFC3164 可在同文件混合解析");
    expect(result.lines[2].ts == "~2024-12-31 23:59:59",
           "RFC3164 优先采用同文件明确年份锚点");
    expect(result.lines[3].ts == "~2025-01-01 00:00:01" &&
           result.lines[3].t - result.lines[2].t == 2,
           "Dec 到 Jan 自动跨年且秒差正确");
    expect(result.outages.size() == 1 && result.outages[0].recovered &&
           result.outages[0].dur == 2,
           "推定年份的 RFC3164 时间仍可配对 v2 断网");
}

void testStderrEnvelopeAndGuard() {
    std::puts("== stderr 前缀与防误判 ==");
    const std::vector<std::string> raw{
        "[INFO] ordinary program started",
        "[WARN] generic warning",
        "[ERR] generic failure",
        "[INFO] ===== modem_mng_v2 start =====",
        "[INFO]Version 2.1",
    };
    Result result = run(raw);

    expect(result.audit.parsed == 4 && result.audit.unparsed == 1,
           "只接受带固定空格的五种 v2 stderr 宏前缀");
    expect(result.lines[0].tagText() == "CONSOLE" &&
           result.lines[0].level == LEVEL_INFO,
           "普通 [INFO] 只提取级别，不无据认定 v2");
    expect(result.lines[1].tagText() == "CONSOLE" &&
           result.lines[1].level == LEVEL_WARNING &&
           result.lines[2].level == LEVEL_ERROR,
           "WARN/ERR 级别正确且通用正文不冒充 v2");
    expect(result.lines[3].tagText() == "MODEM_MNG_V2" &&
           result.platform.plat == PLAT_MODEM_MNG_V2,
           "含应用名的启动横幅是强 v2 身份证据");
    expect(result.sessions.empty() && result.audit.programStarted == 1,
           "无时间 stderr 记录启动信号但不伪造 1970 会话");

    Result consoleOutage = run({
        "[INFO] [READY] WAN ping fail -> network_online=0",
        "[INFO] [READY] WAN ping OK -> network_online=1",
    });
    expect(consoleOutage.outages.empty(),
           "没有可用时钟的纯 stderr 不伪造断网时长");

    Result borrowedConsoleTime = run({
        "Aug 24 09:00:00 rk3576 user.info modem_mng_v2[300]: ===== modem_mng_v2 start =====",
        "[INFO] [READY] WAN ping OK -> network_online=1",
        "[INFO] [READY] WAN ping fail -> network_online=0",
    });
    expect(borrowedConsoleTime.lines.size() == 3 &&
           borrowedConsoleTime.lines[1].fmt == FMT_CONSOLE &&
           borrowedConsoleTime.lines[2].fmt == FMT_CONSOLE &&
           borrowedConsoleTime.lines[1].inferredTime &&
           borrowedConsoleTime.lines[2].inferredTime &&
           borrowedConsoleTime.lines[1].t == borrowedConsoleTime.lines[0].t &&
           borrowedConsoleTime.lines[2].t == borrowedConsoleTime.lines[0].t,
           "syslog 后的 raw stderr 只借用显示时间并保持 FMT_CONSOLE");
    expect(borrowedConsoleTime.outages.empty(),
           "借到 syslog 时间的 raw stderr WAN OK/fail 仍不参与断网配对");
}

void testNormalPlatformsAndMetrics() {
    std::puts("== v2 平台、模组、指标与正常场景 ==");
    Result eg25 = run({
        "Aug 24 10:00:00 rk3576 user.info modem_mng_v2[321]: ===== modem_mng_v2 start =====",
        "Aug 24 10:00:01 rk3576 user.info modem_mng_v2[321]: Module detected: EG25 (type=2)",
        "Aug 24 10:00:02 rk3576 user.info modem_mng_v2[321]: CEREG stat=1 lac=1234 ci=01ABCDEF act=7",
        "Aug 24 10:00:03 rk3576 user.info modem_mng_v2[321]: [CHECK_CSQ] CSQ: 20 (sim_present=1 online=1)",
        "Aug 24 10:00:04 rk3576 user.info modem_mng_v2[321]: [READY] CSQ: 99 (sim_present=1 online=1)",
        "Aug 24 10:00:05 rk3576 user.info modem_mng_v2[321]: ===== modem READY ===== tty=/dev/ttyUSB2 module=EG25 model=EG25G imei=867530900000001 csq=20 online=1",
        "Aug 24 10:00:06 rk3576 user.info modem_mng_v2[321]: [READY/enter] WAN ping OK -> network_online=1",
    });
    expect(eg25.platform.plat == PLAT_MODEM_MNG_V2 &&
           eg25.platform.name.find("EG25") != std::string::npos,
           "v2 EG25 平台与模组展示名正确");
    bool sawCsq20 = false, sawUnknownCsq = false;
    for (const MetricRow& metric : eg25.metrics) {
        if (metric.csqRaw == 20 && metric.csqVal == 20 && metric.lineNo == 4 &&
            metric.rat == "LTE" && metric.cellId == "01ABCDEF" &&
            metric.tac == 0x1234 && metric.tacDigits == 4)
            sawCsq20 = true;
        if (metric.csqRaw == 99 && metric.csqVal == -1) sawUnknownCsq = true;
    }
    expect(sawCsq20,
           "v2 CSQ 指标保留证据行号，并继承 CEREG 的 LTE/Cell/TAC");
    expect(sawUnknownCsq, "CSQ=99 保留原值但不冒充有效信号值");
    expect(eg25.findings.empty(), "正常 READY online=1 场景不产生故障结论");
    expect(auditConsistent(eg25.audit) && everyFindingHasEvidence(eg25.findings),
           "正常场景审计自洽且结论证据约束成立");

    Result ec200a = run({
        "Aug 24 11:00:00 rk3576 user.info modem_mng_v2[322]: ===== modem_mng_v2 start =====",
        "Aug 24 11:00:01 rk3576 user.info modem_mng_v2[322]: Module detected: EC200A (type=1)",
    });
    expect(ec200a.platform.plat == PLAT_MODEM_MNG_V2 &&
           ec200a.platform.name.find("EC200A") != std::string::npos,
           "v2 EC200A 模组可由源码固定检测行识别");
}

void testOutagePairing() {
    std::puts("== v2 WAN 断网配对 ==");
    Result result = run({
        "Aug 24 12:00:00 rk3576 user.info modem_mng_v2[400]: ===== modem_mng_v2 start =====",
        "Aug 24 12:00:01 rk3576 user.info modem_mng_v2[400]: [READY/enter] WAN ping OK -> network_online=1",
        "Aug 24 12:00:10 rk3576 user.info modem_mng_v2[400]: [READY] WAN ping fail -> network_online=0",
        "Aug 24 12:00:20 rk3576 user.info modem_mng_v2[400]: [READY] WAN ping fail -> network_online=0",
        "Aug 24 12:00:40 rk3576 user.info modem_mng_v2[400]: [READY] WAN ping OK -> network_online=1",
    });
    expect(result.outages.size() == 1, "重复 WAN fail 只形成一次断网");
    expect(result.outages.size() == 1 && result.outages[0].recovered &&
           result.outages[0].startLine == 3 && result.outages[0].endLine == 5 &&
           result.outages[0].dur == 30,
           "重复 fail 不重置起点，首次 fail 到 OK 为 30 秒");
    expect(auditConsistent(result.audit), "断网场景解析审计自洽");

    Result beforeOnline = run({
        "Aug 24 12:10:00 rk3576 user.info modem_mng_v2[401]: ===== modem_mng_v2 start =====",
        "Aug 24 12:10:01 rk3576 user.info modem_mng_v2[401]: [CHECK_CPIN/nosim] WAN ping fail -> network_online=0",
        "Aug 24 12:10:10 rk3576 user.info modem_mng_v2[401]: [READY] WAN ping OK -> network_online=1",
    });
    expect(beforeOnline.outages.empty(),
           "同来源首次 WAN OK 前的启动期 fail 不冒充掉线");

    Result otherApp = run({
        "Aug 24 12:15:00 rk3576 user.info health_agent[7]: WAN ping OK -> network_online=1",
        "Aug 24 12:15:01 rk3576 user.info health_agent[7]: WAN ping fail -> network_online=0",
        "Aug 24 12:15:02 rk3576 user.info health_agent[7]: WAN ping OK -> network_online=1",
    });
    expect(otherApp.outages.empty(),
           "其他 syslog 应用即使正文相同也不会冒充 v2 断网");

    Result consoleRestart = run({
        "Aug 24 12:16:00 rk3576 user.info modem_mng_v2[402]: [READY] WAN ping OK -> network_online=1",
        "Aug 24 12:16:10 rk3576 user.info modem_mng_v2[402]: [READY] WAN ping fail -> network_online=0",
        "[INFO] ===== modem_mng_v2 start =====",
        "Aug 24 12:16:30 rk3576 user.info modem_mng_v2[403]: [READY] WAN ping OK -> network_online=1",
    });
    expect(consoleRestart.outages.size() == 1 &&
           !consoleRestart.outages.front().recovered &&
           consoleRestart.outages.front().startLine == 2,
           "精确 console 启动横幅阻断跨进程 WAN 配对，但不伪造恢复时间");

    const std::vector<std::string> splitRaw{
        "Aug 24 12:20:00 rk3576 user.info modem_mng_v2[410]: ===== modem_mng_v2 start =====",
        "Aug 24 12:20:01 rk3576 user.info modem_mng_v2[410]: [READY] WAN ping OK -> network_online=1",
        "Aug 24 12:20:10 rk3576 user.info modem_mng_v2[410]: [READY] WAN ping fail -> network_online=0",
        "Aug 24 12:20:20 rk3576 user.info modem_mng_v2[420]: ===== modem_mng_v2 start =====",
        "Aug 24 12:20:40 rk3576 user.info modem_mng_v2[420]: [READY] WAN ping OK -> network_online=1",
    };
    Result splitSources = run(splitRaw, {3});
    expect(splitSources.outages.size() == 1 &&
           !splitSources.outages.front().recovered &&
           splitSources.outages.front().startLine == 3,
           "A 文件 fail 与 B 文件 OK 不跨 source 串联，A 保留为未恢复断网");
}

void testEvidenceBackedFindings() {
    std::puts("== v2 故障结论与证据 ==");
    Result result = run({
        "Aug 24 13:00:00 rk3576 user.info modem_mng_v2[500]: ===== modem_mng_v2 start =====",
        "Aug 24 13:00:01 rk3576 user.info modem_mng_v2[500]: Module detected: EG25 (type=2)",
        "Aug 24 13:00:02 rk3576 user.info modem_mng_v2[500]: SIM not inserted (CME 10), next CPIN in 5s",
        "Aug 24 13:00:03 rk3576 user.info modem_mng_v2[500]: CEREG stat=2 lac= ci= act=-1",
        "Aug 24 13:02:03 rk3576 user.err modem_mng_v2[500]: No registered so long, reset modem",
        "Aug 24 13:02:04 rk3576 user.err modem_mng_v2[500]: ping failed 5 times, reinit modem!",
        "Aug 24 13:02:05 rk3576 user.info modem_mng_v2[500]: ===== modem READY ===== tty=/dev/ttyUSB2 module=EG25 model=EG25G imei=867530900000001 csq=20 online=0",
        "Aug 24 13:02:06 rk3576 user.err modem_mng_v2[500]: failed to in ready state",
    });
    expect(findingContains(result.findings, "SIM 卡未插入"),
           "SIM not inserted 生成 SIM 卡未插入结论");
    expect(findingContains(result.findings, "网络长期未注册"),
           "No registered so long 生成长期未注册结论");
    expect(findingContains(result.findings, "连续 ping 失败触发重初始化"),
           "ping 重试耗尽生成重初始化结论");
    expect(findingContains(result.findings, "进入 READY 时 WAN 仍离线"),
           "READY online=0 生成离线结论");
    expect(findingContains(result.findings, "READY 状态失败并复位"),
           "failed to in ready state 生成复位结论");
    expect(everyFindingHasEvidence(result.findings),
           "v2 场景输出的每条结论均带原始证据");
    expect(auditConsistent(result.audit), "故障场景解析审计自洽");
}

void testSyntheticSample() {
    std::puts("== 仓库合成样本 ==");
    std::ifstream input("samples/rtms_v2/modem_mng_v2_synthetic.log", std::ios::binary);
    expect(static_cast<bool>(input), "合成样本文件可读取");
    if (!input) return;
    std::vector<std::string> raw;
    std::string line;
    while (std::getline(input, line)) raw.push_back(line);
    Result result = run(raw);
    expect(result.audit.unparsed == 1 && !result.audit.samples.empty() &&
           result.audit.samples.front().text.find("【合成，非真机】") != std::string::npos,
           "样本在文件内显式标注合成性质，且审计诚实保留该说明");
    expect(result.platform.plat == PLAT_MODEM_MNG_V2 &&
           result.platform.name.find("EG25") != std::string::npos,
           "仓库样本可完成 v2/EG25 端到端识别");
    bool sawCsq20 = false;
    for (const MetricRow& metric : result.metrics)
        if (metric.csqRaw == 20 && metric.csqVal == 20) sawCsq20 = true;
    expect(sawCsq20,
           "仓库样本可生成 v2 CSQ 指标");
    expect(result.outages.size() == 1 && result.outages.front().dur == 30,
           "仓库样本可还原首次 fail 到恢复的 30 秒断网");
    expect(auditConsistent(result.audit) && everyFindingHasEvidence(result.findings),
           "仓库样本审计自洽且无无证据结论");
}

} // namespace

int main() {
    testBusyBoxEnvelope();
    testYearAnchorAndRollover();
    testStderrEnvelopeAndGuard();
    testNormalPlatformsAndMetrics();
    testOutagePairing();
    testEvidenceBackedFindings();
    testSyntheticSample();
    std::printf("== modemv2test: %d failure(s) ==\n", failures);
    return failures ? 1 : 0;
}
