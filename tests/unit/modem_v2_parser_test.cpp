// modem_mng_v2 日志包络解析隔离测试。
// 不依赖 Makefile，可单独编译：
//   g++ -std=c++17 -O2 -Wall -Wextra -Isrc/core tests/unit/modem_v2_parser_test.cpp
//       src/core/log_parser.cpp src/core/log_filter.cpp src/core/log_time.cpp
//       -o /tmp/modem_v2_parser_test
#include "log_filter.h"
#include "log_parser.h"
#include "log_time.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace dl;

static int failures = 0;

static void ok(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "[OK]" : "[FAIL]", message);
    if (!condition) ++failures;
}

static void testRfc3164Envelope() {
    std::puts("== RFC3164/BusyBox 包络 ==");
    const std::vector<std::string> raw{
        "Aug 24 12:34:56 rk3576 user.info modem_mng_v2[123]: Version 2.1",
        "Aug 24 12:34:57 rk3576 modem_mng_v2[123]: phase: INIT(0) -> READY(1)",
        "<11>Aug 24 12:34:58 rk3576 modem_mng_v2: modem reset",
        "Aug 24 12:34:59 rk3576 user.warn cron[8]: generic warning",
        "Aug 24 12:35:00 this is ordinary prose: not a syslog envelope",
        "Aug 24 12:35:01 rk3576 user.info modem_mng_v2[]: invalid empty pid",
    };
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);

    ok(lines.size() == raw.size(), "所有输入仍完整保留");
    ok(audit.parsed == 4 && audit.unparsed == 2,
       "4 条合法 RFC3164 结构化，2 条伪包络诚实计未识别");
    ok(lines[0].fmt == FMT_SYSLOG && lines[0].tagText() == "MODEM_MNG_V2" &&
       lines[0].level == LEVEL_INFO && lines[0].msg == "Version 2.1",
       "facility.severity、v2 app 与正文正确提取");
    ok(lines[0].inferredTime && !lines[0].ts.empty() && lines[0].ts[0] == '~',
       "RFC3164 推定年份显式标记为 ~ / inferredTime");
    ok(lines[1].fmt == FMT_SYSLOG && lines[1].tagText() == "MODEM_MNG_V2" &&
       lines[1].level == LEVEL_NONE,
       "无 facility 的 v2 syslog 仍可解析，不伪造级别");
    ok(lines[2].level == LEVEL_ERROR && lines[2].tagText() == "MODEM_MNG_V2",
       "<PRI> 在 facility 缺失时提供 severity");
    ok(lines[3].fmt == FMT_SYSLOG && lines[3].tagText() == "SYSLOG" &&
       lines[3].level == LEVEL_WARNING,
       "generic RFC3164 可解析但不会误标成 modem_mng_v2");
    ok(lines[4].fmt == FMT_CONSOLE && lines[5].fmt == FMT_CONSOLE,
       "普通月份文本和非法空 PID 不被误收为 syslog");

    long long first = 0;
    ok(firstTimestamp(raw, &first) && first == lines[0].t,
       "firstTimestamp 已接入 RFC3164");
}

static void testYearAnchorAndRollover() {
    std::puts("== RFC3164 年份锚定与跨年 ==");
    const std::vector<std::string> raw{
        "2024-12-30T23:59:58 rk3576 collector[1]: explicit year anchor",
        "Dec 31 23:59:59 rk3576 user.info modem_mng_v2[2]: before rollover",
        "Jan  1 00:00:01 rk3576 user.info modem_mng_v2[2]: after rollover",
    };
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);

    ok(audit.parsed == 3 && audit.unparsed == 0, "全年时间与 RFC3164 可混合解析");
    ok(lines[1].ts == "~2024-12-31 23:59:59",
       "优先采用同文件明确年份作为 RFC3164 锚点");
    ok(lines[2].ts == "~2025-01-01 00:00:01" &&
       lines[2].t - lines[1].t == 2,
       "Dec -> Jan 单调递增一年且秒差正确");
}

static void testInferredTimeFilter() {
    std::puts("== RFC3164 推定时间筛选 ==");
    const std::vector<std::string> raw{
        "Aug 24 12:34:56 rk3576 user.info modem_mng_v2[2]: before window",
        "Aug 24 12:34:57 rk3576 user.info modem_mng_v2[2]: inside window",
        "Aug 24 12:34:58 rk3576 user.info modem_mng_v2[2]: after window",
    };
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    parseLines(raw, lines, sessions);

    bool regexBad = false;
    LogView filtered = applyFilterView(lines, "", "", "12:34:57", "12:34:57",
                                       &regexBad);
    ok(!regexBad && filtered.size() == 1 &&
       filtered.front()->msg == "inside window",
       "前导 ~ 不会让 since/until 时间窗静默失效");
}

static void testV2OuterAppIdentity() {
    std::puts("== v2 外层应用身份 ==");
    const std::vector<std::string> raw{
        "Aug 24 12:34:56 rk3576 user.info modem_mng_v2[9]: [ready] CSQ: 20 (sim_present=1 online=1)",
        "2026-08-24T12:34:57 rk3576 modem_mng_v2[9]: [ready] WAN ping fail -> network_online=0",
    };
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);

    ok(audit.parsed == 2 && audit.unparsed == 0,
       "RFC3164 与 RFC3339 v2 包络均结构化");
    ok(lines[0].tagText() == "MODEM_MNG_V2" &&
       lines[1].tagText() == "MODEM_MNG_V2",
       "正文 where 前缀不会覆盖可靠的外层 app 身份");
    ok(lines[0].msg.find("[ready] CSQ:") == 0 &&
       lines[1].msg.find("[ready] WAN ping fail") == 0,
       "v2 正文原文（含 where 前缀）完整保留");
}

static void testConsoleEnvelope() {
    std::puts("== v2 stderr 包络与防误判 ==");
    const std::vector<std::string> raw{
        "[INFO] ordinary program started",
        "[INFO] Version 2.1",
        "[WARN] generic warning",
        "[ERR] modem init failed, phase 3",
        "[NOTICE] release client:1 for new socket:2",
        "[DBG] serial bytes",
        "[INFO] ===== modem_mng_v2 start =====",
        "[INFO]Version 2.1",
    };
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);

    ok(lines.size() == raw.size(), "stderr 与不合法相似行全部保留");
    ok(audit.parsed == 7 && audit.unparsed == 1,
       "只接受五种精确宏前缀；缺固定空格的相似行不误收");
    ok(lines[0].fmt == FMT_CONSOLE && lines[0].tagText() == "CONSOLE" &&
       lines[0].level == LEVEL_INFO && lines[0].msg == "ordinary program started",
       "普通 [INFO] 只结构化 level，不无据判定 v2");
    ok(lines[1].tagText() == "CONSOLE",
       "通用 Version 2.1 文本本身不足以证明 v2 身份");
    ok(lines[2].level == LEVEL_WARNING && lines[3].level == LEVEL_ERROR &&
       lines[4].level == LEVEL_NOTICE && lines[5].level == LEVEL_DEBUG,
       "WARN/ERR/NOTICE/DBG 映射正确");
    ok(lines[6].tagText() == "MODEM_MNG_V2",
       "包含应用名的 start 横幅可作为强 v2 证据");
    ok(audit.programStarted == 1 && sessions.empty(),
       "无时间 stderr 只计启动信号，不伪造 1970 session");
}

static void testConsoleTimeBackfillAndSyslogRestart() {
    std::puts("== 控制台时间回填与 v2 启动去重 ==");
    const std::vector<std::string> raw{
        "[INFO] Version 2.1",
        "Aug 24 12:34:56 rk3576 user.info modem_mng_v2[123]: Version 2.1",
        "Aug 24 12:34:57 rk3576 user.info modem_mng_v2[123]: ===== modem_mng_v2 start =====",
    };
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);

    ok(lines[0].inferredTime && lines[0].t == lines[1].t &&
       lines[0].ts == lines[1].ts && lines[0].ts.compare(0, 2, "~~") != 0,
       "首行无时间控制台由首个真实包络回填，且不会产生双 ~");
    ok(audit.programStarted == 1 && sessions.size() == 1,
       "syslog Version + start 横幅只形成一次启动证据/session");
}

int main() {
    testRfc3164Envelope();
    testYearAnchorAndRollover();
    testInferredTimeFilter();
    testV2OuterAppIdentity();
    testConsoleEnvelope();
    testConsoleTimeBackfillAndSyslogRestart();
    std::printf("== modem_v2_parser_test: %d failure(s) ==\n", failures);
    return failures ? 1 : 0;
}
