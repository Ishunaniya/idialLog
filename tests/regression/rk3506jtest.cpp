// 指定 RTMS 8f9aaa19..461b2b2f 源码派生的行为回归；不冒称新版真机验证。
#include "logmodel.h"
#include "tablemodel.h"

#include <cmath>
#include <cstdio>
#include <fstream>

using namespace dl;
static int failures = 0;
static void check(bool value, const char* message) {
    std::printf("  %s %s\n", value ? "[通过]" : "[失败]", message);
    if (!value) ++failures;
}
static std::string at(int seconds, const std::string& message) {
    char prefix[40];
    std::snprintf(prefix, sizeof(prefix), "[2026-09-16 10:%02d:%02d] ", seconds / 60, seconds % 60);
    return prefix + message;
}
static std::string banner(int seconds = 0, const std::string& version = "1.28.6") {
    return at(seconds, "Modem_mng Version: rtms_rk3506j_" + version);
}
static std::string ready(int seconds) {
    return at(seconds, "[INTERNET-READY] ===== PUBLIC PING OK ===== target=223.5.5.5 "
                       "if=eth1 ip=192.0.2.2 path=EC200A-full-dial retained=none probe_ms=380 "
                       "process_elapsed_ms=10000 boot_ms=20000");
}
struct Log {
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    std::vector<Outage> outages;
    std::vector<MetricRow> metrics;
    std::vector<Finding> findings;
    PlatformInfo platform;
    AvailabilityStats availability;
    Log(const std::vector<std::string>& raw, const std::vector<size_t>& boundaries = {}) {
        parseLines(raw, lines, sessions, &audit, boundaries);
        platform = detectPlatform(lines);
        outages = collectOutages(lines); metrics = buildMetrics(lines);
        availability = availabilityStats(lines, outages);
        findings = analyze(lines, outages, metrics, platform, audit);
    }
    const Finding* finding(const std::string& title) const {
        for (const auto& f : findings) if (f.title.find(title) != std::string::npos) return &f;
        return nullptr;
    }
    bool rkFault() const {
        for (const auto& f : findings) if (f.severity >= 1 && f.title.find("RK3506J") != std::string::npos) return true;
        return false;
    }
};

static void firstInternetAndRecovery() {
    std::printf("== 首次公网成功、截段与恢复边沿 ==\n");
    Log first({banner(), ready(10), at(30, "[HB30] online=1 | csq=26"), at(60, "[SERVICE] 4G interface=eth1")});
    check(first.availability.longestStartupSeconds == 10 && first.availability.fullObservedSeconds == 60 &&
          first.availability.fullUnavailableSeconds == 10 && first.availability.runtimeObservedSeconds == 50 &&
          std::abs(first.availability.fullPercent() - 83.3333333333) < 1e-6,
          "公网公告早于 HB30：等待10s、全程不可用10/60s、运行期50s");
    Log cut({banner(), ready(10), at(20, "[SERVICE] 4G interface=eth1")});
    check(cut.availability.neverConnectedStartupSegments == 0 && cut.availability.connectedStartupSegments == 1 &&
          cut.availability.fullPercent() == 50 && cut.outages.empty(),
          "无后续心跳仍确认已联网，全程可达率50%，不误报启动断网");
    Log down({banner(), ready(10), at(20, "[HB30] online=0 | csq=26"), at(50, "[HB30] online=1 | csq=26")});
    check(down.outages.size() == 1 && down.outages[0].dur == 30 && down.outages[0].startLine == 3 &&
          down.outages[0].endLine == 4 && down.availability.longestStartupSeconds == 10 &&
          down.availability.runtimeUnavailableSeconds == 30,
          "公网公告建立曾在线门控，后续真实断网配成30s");
    for (const auto* tag : {"FULL-DIAL", "EC200A", "EG912"}) {
        Log recovered({banner(), at(10, "[HB30] online=1"), at(20, "[HB30] online=0"),
                       at(25, "[HB30] online=0"), at(40, std::string("[") + tag + "] connectivity restored"),
                       at(50, "[HB30] online=1")});
        check(recovered.outages.size() == 1 && recovered.outages[0].dur == 20 &&
              recovered.outages[0].endLine == 5 && recovered.availability.runtimeUnavailableSeconds == 20,
              "旧EC200A、新FULL-DIAL及EG912明确恢复均早于下一心跳闭合；重复offline不改起点");
    }
    Log phase({banner(), at(12, "[STARTUP] phase=internet_ready process_elapsed_ms=12000 boot_ms=22000"),
               at(30, "[HB30] online=1")});
    check(phase.availability.longestStartupSeconds == 12, "internet_ready阶段作为公告缺失时的直接公网成功证据");
    Log milestones({banner(), at(2, "[STARTUP] phase=usb_topology_ready process_elapsed_ms=2000 boot_ms=12000"),
                    at(3, "[STARTUP] phase=ecm_ready process_elapsed_ms=3000 boot_ms=13000"),
                    at(4, "[STARTUP] phase=host_network_ready process_elapsed_ms=4000 boot_ms=14000"),
                    at(5, "[WAKE] startup SOFT_RECOVERY completed"), at(6, "[FULL-DIAL] state=SUCCESS"),
                    at(20, "[HB30] online=0")});
    check(milestones.availability.neverConnectedStartupSegments == 1 && milestones.outages.empty(),
          "USB/ECM/IP就绪、恢复阶段完成及SUCCESS状态不能替代公网成功");
    Log recurring({banner(), ready(10), at(20, "[HB30] online=0"), at(40, "[FULL-DIAL] connectivity restored"),
                   at(50, "[HB30] online=0"), at(60, "[EG912] connectivity restored"), at(70, "[HB30] online=1")});
    check(recurring.outages.size() == 2 && recurring.outages[0].dur == 20 && recurring.outages[1].dur == 10,
          "只打印一次的公告不影响后续多轮运行期恢复边沿");
    Log terminal({banner(), ready(10), at(20, "[HB30] online=0"), at(50, "[SERVICE] 4G interface=eth1")});
    check(terminal.outages.size() == 1 && !terminal.outages[0].recovered &&
          terminal.availability.terminalOutages == 1 && terminal.availability.runtimeUnavailableSeconds == 30,
          "公告后末尾未恢复断网仍计入运行期30s不可用");
}

static void adoptionAndIsolation() {
    std::printf("== 保留网络接管、来源/重启/时基隔离 ==\n");
    for (const auto* tag : {"STARTUP", "FAST-BOOT"}) for (int internet : {0, 1}) {
        const auto message = std::string("[") + tag + (std::string(tag) == "STARTUP"
            ? "] adopted existing PDP if=eth1 action=host-recovery internet="
            : "] adopted retained=active-pdp if=eth1 action=dhcp ip=1 route=1 internet=") +
            std::to_string(internet) + " elapsed_ms=800";
        Log log({banner(0, std::string(tag) == "STARTUP" ? "1.28.2" : "1.28.4"), at(10, message),
                 at(30, "[SERVICE] 4G interface=eth1")});
        check(log.availability.connectedStartupSegments == static_cast<size_t>(internet) &&
              log.availability.neverConnectedStartupSegments == static_cast<size_t>(1 - internet) &&
              log.availability.fullUnavailableSeconds == (internet ? 10 : 30),
              "1.28.2/3 STARTUP与1.28.4+ FAST-BOOT均严格区分internet=0/1");
    }
    Log mqtt({banner(), at(10, "[FAST-BOOT] retained=connected-mqtt mqtt_state=3 registered=1 pdp_active=1 qnet=1 cid=2"),
              at(20, "[FULL-DIAL] existing PDP adopted; internet probe pending"), at(30, "[HB30] online=0")});
    check(mqtt.availability.neverConnectedStartupSegments == 1 && mqtt.outages.empty(),
          "MQTT/PDP/ECM保留和internet probe pending不算主机公网可达");
    Log twoSources({banner(), ready(10), at(20, "[HB30] online=0"), banner(30), ready(40), at(50, "[HB30] online=1")}, {0, 3});
    check(twoSources.outages.size() == 1 && !twoSources.outages[0].recovered &&
          twoSources.availability.runtimeUnavailableSeconds == 0,
          "另一来源的公网成功不关闭旧来源断网，也不跨来源累加不可用时间");
    Log reboot({banner(), ready(10), at(20, "[HB30] online=0"), at(25, "[SERVICE] 4G interface=eth1"),
                banner(30), ready(40), at(50, "[HB30] online=1")});
    check(reboot.outages.size() == 1 && !reboot.outages[0].recovered &&
          reboot.availability.startupSegments == 2 && reboot.availability.runtimeUnavailableSeconds == 5 &&
          reboot.availability.fullUnavailableSeconds == 25,
          "同来源重启关闭旧状态周期：旧会话未恢复5s，不污染新会话");
    Log sameSecond({banner(), ready(10), at(20, "[HB30] online=0"), banner(20), ready(30),
                    at(40, "[HB30] online=1")});
    check(sameSecond.outages.size() == 1 && !sameSecond.outages[0].recovered &&
          sameSecond.availability.runtimeUnavailableSeconds == 0,
          "掉线与重启同一秒时按证据行隔离，不把旧断网累计到新进程");
    Log crossed({"[1970-01-01 00:00:00] Modem_mng Version: rtms_rk3506j_1.28.6",
                 "[1970-01-01 00:00:10] [INTERNET-READY] ===== PUBLIC PING OK ===== target=223.5.5.5 if=eth1",
                 at(20, "[HB30] online=0"), at(50, "[FULL-DIAL] connectivity restored")});
    check(crossed.audit.clockJump && crossed.outages.empty(), "跨1970/墙钟时基清理曾在线门控，不制造巨大或虚假断网");
    Log mixed({banner(), at(10, "[HB30] online=1"), at(20, "[HB30] online=0"),
               at(30, "Modem_mng Version: rtms_imx6ull_1.25.1"), ready(40), at(50, "[SERVICE] program=imx")}, {0, 3});
    check(mixed.outages.size() == 1 && !mixed.outages[0].recovered &&
          mixed.availability.neverConnectedStartupSegments == 1,
          "有明确IMX横幅的来源不接受RK公网标签，RK诊断也不跨来源归类");
    Log changedPlatform({banner(), ready(10), at(20, "Modem_mng Version: rtms_imx6ull_1.25.1"), ready(30),
                         at(40, "[SERVICE] program=imx")});
    check(changedPlatform.availability.neverConnectedStartupSegments == 1,
          "同来源下一启动切换平台后也拒绝RK公网成功证据");
    Log cutIdentity({ready(10), at(20, "[HB30] online=0"), at(40, "[FULL-DIAL] connectivity restored"),
                     at(50, "[HB300] online=1 | csq=26 | traffic_valid=1 | rx_packets=12 | sample_ms=100")});
    check(cutIdentity.platform.plat == PLAT_RK3506J && cutIdentity.outages.size() == 1 &&
          cutIdentity.outages[0].dur == 20 && !cutIdentity.availability.fullValid(),
          "缺少横幅的截段可由固定RK证据识别和配对断网，但不虚构启动统计");
    check(cutIdentity.metrics.size() == 2 && cutIdentity.metrics[0].ch == "RK3506J" &&
          cutIdentity.metrics.back().ch == "RK3506J" && cutIdentity.metrics.back().rx == 12,
          "截段心跳与公网证据统一识别RK3506J，sample_ms流量样本仍有效");
    Log untimed({banner(), "[INTERNET-READY] ===== PUBLIC PING OK ===== target=223.5.5.5 if=eth1",
                 at(20, "[HB30] online=0"), at(30, "[SERVICE] 4G interface=eth1")});
    check(untimed.availability.neverConnectedStartupSegments == 1 && untimed.outages.empty(),
          "裸控制台公网镜像没有自身时间，不借相邻时间制造联网或断网边沿");
    for (const auto& message : {
        "[INTERNET-READY] ===== PUBLIC PING OK ===== target=223.5.5.5 if=eth1 response=internet=1",
        "[FAST-BOOT] adopted retained=active-pdp if=eth1 action=none ip=1 route=1 internet=10 elapsed_ms=900",
        "[FAST-BOOT] adopted retained=active-pdp if=eth1 action=none ip=1 route=1 response=internet=1",
        "[FAST-BOOT] adopted retained=active-pdp if=eth1 action=\"internet=1\" ip=1 route=1 internet=0 elapsed_ms=900",
        "[STARTUP] phase=internet_ready process_elapsed_ms=12x boot_ms=20000"}) {
        Log invalid({banner(), at(10, message), at(20, "[SERVICE] 4G interface=eth1")});
        // 第一条是有效公告，response中的噪声不覆盖公告直证；其余均无公网成功。
        const bool genuine = std::string(message).find("[INTERNET-READY]") == 0;
        check(invalid.availability.connectedStartupSegments == static_cast<size_t>(genuine),
              "字段精确取值，原始response和引号正文不能伪造internet=1");
    }
}

static void diagnosisAndTimeline() {
    std::printf("== 故障分层、合法容错与时间线 ==\n");
    for (const auto* tag : {"FULL-DIAL", "EC200A", "EG912"}) {
        Log log({banner(), at(10, std::string("[") + tag + "] Unable to ping google, attempt 1/5"),
                 at(20, std::string("[") + tag + "] state=FAILURE_RETRY: AT redial failed"),
                 at(30, "[SERVICE] 4G interface=eth1")});
        const auto* ping = log.finding("连通性探测失败"); const auto* retry = log.finding("失败重试状态");
        check(ping && retry && ping->severity == 1 && retry->severity == 2 &&
              ping->ev.size() == 1 && ping->ev[0].lineNo == 2 && retry->ev[0].lineNo == 3,
              "三个拨号前缀均生成对应PING/FAILURE_RETRY诊断及准确证据行");
        check(isEventLine(log.lines[1]) && isEventLine(log.lines[2]) && isErrLine(log.lines[2]),
              "PING失败及失败重试进入真实时间线，失败重试进入错误筛选");
    }
    Log redial({banner(), at(10, "[FULL-DIAL] CPIN query failed: read_timed_out -> REDIAL_AT"), at(20, "[HB30] online=0")});
    check(redial.finding("AT 通道不可用") && !redial.finding("失败重试状态"),
          "传输失败转向REDIAL_AT可诊断AT问题，不误称已进入FAILURE_RETRY");
    Log tolerated({banner(), at(2, "[AT-READY] CPIN attempt=1 port=/dev/ttyUSB1 status=completed ok=0 class=transient state=CME 10 elapsed_ms=20 total_ms=20 response=+CME ERROR: 10"),
                   at(3, "[AT-READY] CPIN attempt=2 port=/dev/ttyUSB1 status=read_timed_out ok=0 class=ready state=READY elapsed_ms=1000 total_ms=1020 response=+CPIN: READY"),
                   at(4, "[AT-READY] CGACT attempt=1 port=/dev/ttyUSB1 status=read_timed_out ok=0 usable=1 elapsed_ms=1000 total_ms=1000 response=+CGACT: 1,1"),
                   at(5, "[AT-READY] CEREG attempt=1 port=/dev/ttyUSB1 status=read_timed_out ok=0 registered=1 stat=5 elapsed_ms=1000 total_ms=1000 response=+CEREG: 0,5"),
                   at(6, "[AT-READY] QNETDEVCTL attempt=1 port=/dev/ttyUSB1 status=read_timed_out ok=0 connected=1 state=type=3 cid=1 urc=1 status=1 elapsed_ms=1000 total_ms=1000 response=+QNETDEVCTL: 3,1,1,1"),
                   at(7, "[AT] response stream resynchronized port=/dev/ttyUSB1 discarded=4 elapsed_ms=250"), ready(10), at(30, "[HB30] online=1")});
    check(!tolerated.rkFault() && tolerated.availability.longestStartupSeconds == 10,
          "SIM初始化CME10后READY及只读完整状态行缺OK均不误报永久SIM/激活/AT故障");
    check(isEventLine(tolerated.lines[2]) && !isErrLine(tolerated.lines[2]),
          "容错READY保留到时间线但不染成错误");
    Log exhausted({banner(), at(15, "[AT-READY] basic AT not ready port=/dev/ttyUSB1 attempts=10; startup readiness deadline exhausted"),
                   at(16, "[AT-READY] CPIN not ready port=/dev/ttyUSB1 attempts=10 elapsed_ms=15000; startup readiness deadline exhausted"),
                   at(20, "[HB30] online=0")});
    check(exhausted.finding("AT 通道不可用") && exhausted.finding("SIM 就绪等待耗尽") &&
          !exhausted.finding("SIM 未就绪") && exhausted.outages.empty(),
          "基本AT/CPIN窗口耗尽有明确诊断，但不猜缺卡、不算启动掉线");
    Log locked({banner(), at(3, "[AT-READY] CPIN attempt=1 port=/dev/ttyUSB1 status=completed ok=1 class=permanent state=SIM PIN elapsed_ms=20 total_ms=20 response=+CPIN: SIM PIN | OK"), at(20, "[HB30] online=0")});
    check(locked.finding("SIM 未就绪") && locked.finding("SIM 未就绪")->ev[0].lineNo == 2,
          "明确SIM PIN锁保留为SIM故障证据");
    Log attach({banner(), at(5, "[FAST-BOOT] retained=active-pdp but ECM attach failed; preserve hard-recovery guard elapsed_ms=900"),
                at(6, "[FAST-BOOT] ECM connected but interface eth1 is absent elapsed_ms=900"),
                at(7, "[FAST-BOOT] retained network exists but fast attach failed; enter SOFT_RECOVERY"),
                at(8, "[WAKE] hard_recovery=allowed reason=SOFT_RECOVERY exhausted"),
                at(9, "[WAKE] startup soft recovery exhausted; enter HARD_RECOVERY"), at(20, "[HB30] online=0")});
    check(attach.finding("PDP/ECM") && attach.finding("DHCP/IPv4") && attach.finding("启动软恢复") &&
          attach.finding("硬恢复阶段") && attach.finding("硬恢复阶段")->ev.size() == 1 &&
          attach.finding("硬恢复阶段")->ev[0].lineNo == 6,
          "接管分别诊断ECM/主机层与软硬升级，allowed不当作执行硬恢复");
    Log guard({banner(), at(1, "[WAKE] destructive_recovery=forbidden reason=startup mode initialized: boot reason unavailable"),
               at(2, "[AT-CACHE] ignore /usr/dial/rk3506j_at_port.cache: cached USB identity does not match the attached modem"),
               at(3, "[NANOMSG] PUB unavailable; dialing continues, retry_ms=1000 attempts=1 error=nn_bind failed"),
               at(4, "[NANOMSG] REQ/REP unavailable; dialing continues, retry_ms=1000 attempts=1 error=nn_bind failed"), ready(10), at(30, "[HB30] online=1")});
    check(!guard.rkFault() && guard.finding("保留网络保护") && guard.finding("缓存未采用") &&
          guard.finding("本地 IPC") && guard.finding("本地 IPC")->severity == 0 &&
          guard.finding("本地 IPC")->ev.size() == 2 && guard.availability.longestStartupSeconds == 10,
          "保护/缓存拒绝/本地IPC后台重试均是提示，不升级成公网故障");
    Log fallback({banner(), at(2, "[bringup] CGACT state unavailable after bounded retries; attempt idempotent CID1 activation"),
                  at(3, "[bringup] QNETDEVCTL start not confirmed; verify connected state"),
                  at(4, "[AT-READY] QNETDEVCTL attempt=1 port=/dev/ttyUSB1 status=completed ok=1 connected=1 elapsed_ms=20 response=+QNETDEVCTL: 3,1,1,1 | OK"),
                  at(5, "[bringup] DHCP failed, trying static IP from CGCONTRDP"),
                  at(6, "[bringup] static IP 192.0.2.2/24 gw 192.0.2.1 on eth1; DNS remains managed by the system"), ready(10), at(30, "[HB30] online=1")});
    check(!fallback.rkFault() && fallback.finding("回退验证") && fallback.finding("静态 IP 回退") &&
          !fallback.finding("DHCP/IPv4 配置失败"),
          "未确认命令后验证成功、DHCP回退后静态IP成功不报最终激活/IPv4失败");
    Log hostFailed({banner(), at(5, "[bringup] DHCP and CGCONTRDP fallback did not provide an IPv4 address"), at(20, "[HB30] online=0")});
    check(hostFailed.finding("DHCP/IPv4 配置失败") && hostFailed.finding("DHCP/IPv4 配置失败")->ev[0].lineNo == 2,
          "明确最终IPv4回退失败仍生成告警和准确证据");
    Log cleanQueries({banner(), at(1, "[AT-READY] CGACT attempt=1 port=/dev/ttyUSB1 status=completed ok=1 usable=1 elapsed_ms=20 response=+CGACT: 1,1 | OK"), ready(10), at(30, "[HB30] online=1")});
    LogView view; for (const auto& line : cleanQueries.lines) view.push_back(&line);
    LogView timeline; buildTimelineView(view, timeline);
    check(timeline.size() == 1 && timeline[0]->tagText() == "INTERNET-READY" &&
          timelineCellText(*timeline[0], 3).find("process_elapsed_ms=10000 boot_ms=20000") != std::string::npos,
          "真实时间线包含公网时序字段，成功逐次查询/HB30不淹没时间线");
    const auto* announcement = cleanQueries.finding("首次公网 PING 成功");
    check(announcement && announcement->ev[0].text.find("process_elapsed_ms=10000 boot_ms=20000") != std::string::npos,
          "诊断证据保留公告行尾的进程/开机毫秒字段，复制时不被160字符截断");
    const auto viewOutages = collectOutages(view); const auto viewStats = availabilityStats(view, viewOutages);
    const auto viewFindings = analyze(view, viewOutages, buildMetrics(view), cleanQueries.platform, cleanQueries.audit);
    check(viewStats.longestStartupSeconds == 10 && viewStats.fullUnavailableSeconds == 10 &&
          viewFindings.size() == cleanQueries.findings.size(), "拥有/借用分析路径结果一致，UI筛选视图使用同一语义");
    Log power({banner(), at(1, "[WAKE] hard_recovery=allowed reason=SOFT_RECOVERY exhausted"),
               at(2, "[power] physical PWRKEY shutdown disabled; modem remains in CFUN=0 soft-off state"),
               at(3, "[power] module power-off not confirmed; skip 300s power-on cooldown"), at(20, "[HB30] online=0")});
    check(!power.finding("硬恢复阶段") && power.finding("关机/上电未确认") &&
          power.finding("关机/上电未确认")->ev.size() == 1 && power.finding("关机/上电未确认")->ev[0].lineNo == 4,
          "硬恢复许可和正常CFUN软关机不报已断电，未确认关机只用具体证据告警");
}

static void sourceFixture() {
    std::printf("== 提交派生完整夹具 ==\n");
    std::ifstream input("samples/rtms_rk3506j/rtms_1.28.6_synthetic.log");
    check(input.good(), "1.28.6源码派生夹具可加载");
    std::vector<std::string> raw; std::string line;
    while (std::getline(input, line)) raw.push_back(line);
    Log log(raw);
    check(log.audit.rawTotal == 22 && log.audit.unparsed == 0 && log.lines.size() == 22 &&
          log.platform.plat == PLAT_RK3506J && log.sessions.size() == 1,
          "22行源码派生SD包络全部保留，准确识别RK3506J和单次启动");
    check(log.outages.size() == 1 && log.outages[0].dur == 25 && log.outages[0].startLine == 16 &&
          log.outages[0].endLine == 20 && log.availability.longestStartupSeconds == 10 &&
          log.availability.fullUnavailableSeconds == 35 && log.availability.runtimeUnavailableSeconds == 25 &&
          log.availability.runtimeObservedSeconds == 80,
          "夹具统计等待10s、断网25s、全程不可用35/90s、运行期不可用25/80s");
    check(log.metrics.size() == 4 && log.metrics[0].ch == "RK3506J" && log.metrics[0].csqVal == 26 &&
          log.metrics.back().rx == 12 && hbFields(log.lines.back().msg)["tx_packets"] == "34" &&
          !log.finding("SIM 未就绪") && !log.finding("PDP/ECM") && !log.finding("AT 通道不可用"),
          "心跳信号/流量保持兼容，初始化瞬态与查询容错不产生错误根因");
    check(log.finding("连通性探测失败") && log.finding("连通性探测失败")->ev[0].lineNo == 15 &&
          log.finding("本地 IPC") && log.finding("本地 IPC")->severity == 0,
          "夹具只按实际公网失败证据诊断；本地IPC仍为提示");
    for (const auto& l : log.lines) if (l.tagText() == "STARTUP" || l.tagText() == "INTERNET-READY" ||
                                     l.tagText() == "AT-CACHE" || l.tagText() == "FULL-DIAL")
        check(!l.customTag && isEventLine(l), "新常用标签驻留字典，并进入关键事件时间线");
}

static void e15TrafficAndSelection() {
    std::printf("== e15a5232公共流量与RK选网恢复 ==\n");
    for (const auto* product : {"rtms_ec200a_1.0.0", "rtms_ag35_1.0.0", "rtms_eg25_1.0.0",
                                "rtms_imx6ull_1.25.1", "rtms_rk3506j_1.28.6"}) {
        Log shared({at(0, std::string("Modem_mng Version: ") + product),
                    at(2, "[TRAFFIC] sample/persist skipped count=1 reason=database not restored; monitoring continues in memory"),
                    at(20, "[SERVICE] 4G interface=eth1")});
        const auto* finding = shared.finding("流量采样/持久化已跳过");
        check(finding && finding->severity == 1 && finding->ev.size() == 1 &&
              finding->ev[0].lineNo == 2 && !shared.rkFault() && shared.outages.empty() &&
              isErrLine(shared.lines[1]) && isEventLine(shared.lines[1]) && !shared.lines[1].customTag,
              "TRAFFIC共享五个平台：告警/错误筛选/字典/时间线一致，不归为RK或公网故障");
    }
    for (const auto* reason : {"interface counters unavailable", "database unavailable or saved records invalid",
                              "system time unavailable", "clock is older than saved traffic records",
                              "database transaction unavailable", "database write failed; in-memory totals retained",
                              "unknown no-traffic monitor exception", "unknown traffic monitor exception",
                              "custom read exception"}) {
        Log skipped({banner(), ready(5), at(6, std::string("[TRAFFIC] sample/persist skipped count=10 reason=") + reason),
                     at(20, "[HB30] online=1 | csq=26")});
        check(skipped.finding("流量采样/持久化已跳过") && skipped.outages.empty() &&
              skipped.availability.longestStartupSeconds == 5 &&
              skipped.availability.runtimeUnavailableSeconds == 0 && !skipped.rkFault(),
              "固定原因和异常正文只说明流量采样/落盘缺失，不制造联网边沿或网络根因");
    }
    for (const auto* message : {"sample/persist skipped count=x reason=interface counters unavailable",
                               "sample/persist skipped count=-1 reason=interface counters unavailable",
                               "sample/persist skipped count=4294967296 reason=interface counters unavailable",
                               "sample/persist skipped count=10",
                               "sample/persist skipped count=10 response=reason=interface counters unavailable",
                               "text sample/persist skipped count=10 reason=interface counters unavailable"}) {
        Log malformed({banner(), at(2, std::string("[TRAFFIC] ") + message)});
        check(!malformed.finding("流量采样/持久化已跳过") && !isErrLine(malformed.lines[1]) &&
              !isEventLine(malformed.lines[1]), "TRAFFIC只接受真实包络和完整无符号count/reason，响应正文不冒充字段");
    }
    Log throttled({banner(), ready(5),
                   at(6, "[TRAFFIC] sample/persist skipped count=1 reason=interface counters unavailable"),
                   at(7, "[TRAFFIC] sample/persist skipped count=10 reason=database transaction unavailable"),
                   at(8, "[TRAFFIC] sample/persist skipped count=20 reason=system time unavailable"),
                   at(9, "[TRAFFIC] sample/persist skipped count=30 reason=database unavailable or saved records invalid"),
                   at(20, "[HB30] online=1")});
    const auto* sparse = throttled.finding("流量采样/持久化已跳过");
    check(sparse && sparse->ev.size() == 3 && sparse->ev[0].lineNo == 3 && sparse->ev[2].lineNo == 5 &&
          sparse->detail.find("累计失败次数") != std::string::npos && throttled.outages.empty(),
          "流量限流count不按稀疏行数求和或认定连续失败，证据仍保留准确行号");

    Log failed({banner(), ready(5),
                at(6, "[COPS] command=AT+QNETDEVCTL=0 status=completed ok=0 response=ERROR"),
                at(7, "[COPS] command=AT+CFUN=0 status=completed ok=1 response=OK"),
                at(8, "[COPS] command=AT+CFUN=1 status=read_timed_out ok=0 response="),
                at(9, "[FULL-DIAL] history PLMN verification failed -> operator scan"),
                at(10, "[FULL-DIAL] set operator FAIL -> try next candidate"),
                at(11, "[FULL-DIAL] history PLMN verified=46002 -> WRITE_TO_MODEM"),
                at(20, "[HB30] online=1")});
    const auto* selection = failed.finding("选网事务失败或结果未验证");
    check(selection && selection->severity == 1 && selection->ev.size() == 3 &&
          selection->ev[0].lineNo == 5 && selection->ev[1].lineNo == 6 && selection->ev[2].lineNo == 7 &&
          !isErrLine(failed.lines[2]) && isErrLine(failed.lines[4]) && isErrLine(failed.lines[6]) &&
          failed.outages.empty() && !failed.finding("已进入失败重试状态"),
          "CFUN/历史/候选失败按准确证据告警，ECM停止被拒绝和尝试下一候选不造公网掉线或FAILURE_RETRY");
    Log notInternet({banner(),
                     at(2, "[COPS] command=AT+COPS=1,2,\"46002\",7 status=completed ok=1 response=OK"),
                     at(3, "[COPS] command=AT+COPS? status=completed ok=1 response=+COPS: 1,2,\"46002\",7 OK"),
                     at(4, "[FULL-DIAL] set operator OK -> WRITE_TO_MODEM"),
                     at(5, "[FULL-DIAL] history PLMN verified=46002 -> WRITE_TO_MODEM"),
                     at(20, "[HB30] online=0")});
    const auto* verified = notInternet.finding("历史 PLMN 选网验证成功");
    check(verified && verified->severity == 0 && verified->ev[0].lineNo == 5 &&
          notInternet.availability.neverConnectedStartupSegments == 1 && notInternet.outages.empty() &&
          isEventLine(notInternet.lines[4]) && !isErrLine(notInternet.lines[4]),
          "严格历史PLMN验证只证明选网进入下一阶段，命令OK/COPS查询/WRITE_TO_MODEM均不证明公网成功");
    for (const auto* msg : {"set operator OK -> WRITE_TO_MODEM",
                           "history PLMN verified=4600 -> WRITE_TO_MODEM",
                           "history PLMN verified=4600x -> WRITE_TO_MODEM",
                           "history PLMN verified=4600012 -> WRITE_TO_MODEM",
                           "response=history PLMN verified=46002 -> WRITE_TO_MODEM"}) {
        Log oldOrInvalid({banner(), at(2, std::string("[FULL-DIAL] ") + msg), at(20, "[HB30] online=0")});
        check(!oldOrInvalid.finding("历史 PLMN 选网验证成功") &&
              oldOrInvalid.availability.neverConnectedStartupSegments == 1,
              "旧版set operator OK弱检查、畸形PLMN及响应回显不提升成严格选网验证或公网成功");
    }
    Log stopped({banner(), at(2, "[COPS] command=AT+QNETDEVCTL=0 status=completed ok=0 response=ERROR"),
                 at(3, "[COPS] command=AT+COPS? status=completed ok=1 response=ok=0 status=read_timed_out"),
                 at(4, "[FULL-DIAL] history PLMN verified=460001 -> WRITE_TO_MODEM")});
    check(!stopped.finding("选网事务失败或结果未验证") && stopped.finding("历史 PLMN 选网验证成功"),
          "停止拒绝及response中的伪失败字段不会覆盖COPS事务头部结果，六位PLMN仍有效");
    Log crop({at(2, "[FULL-DIAL] history PLMN verified=46002 -> WRITE_TO_MODEM"),
              at(5, "[HB30] online=0 | csq=26")});
    check(crop.platform.plat == PLAT_RK3506J && crop.finding("历史 PLMN 选网验证成功") &&
          crop.metrics.size() == 1 && crop.metrics[0].ch == "RK3506J" && !crop.availability.fullValid(),
          "新历史PLMN直证可识别截段平台和指标，仍不虚构启动或联网统计");
    Log foreign({at(0, "Modem_mng Version: rtms_imx6ull_1.25.1"),
                 at(2, "[FULL-DIAL] history PLMN verification failed -> operator scan"),
                 at(3, "[COPS] command=AT+CFUN=0 status=read_timed_out ok=0 response="),
                 at(4, "[FULL-DIAL] history PLMN verified=46002 -> WRITE_TO_MODEM")});
    check(!foreign.finding("RK3506J 选网") && !foreign.finding("历史 PLMN 选网验证成功"),
          "明确其它平台来源的COPS/选网正文不生成RK专属诊断");
    Log cycle({banner(), at(1, "[HB30] online=1"), at(10, "[HB30] online=0"),
               at(20, "[EG912] escalate: redial stage 4/4: restart EG912 recovery cycle -> state=7"),
               at(21, "[EG912] state=FAILURE_RETRY: redial stage 4/4: restart EG912 recovery cycle"),
               at(22, "[EG912] runtime recovery exhausted; enter HARD_RECOVERY"),
               at(30, "[EG912] connectivity restored"), at(40, "[HB30] online=1")});
    check(cycle.finding("EG912 恢复循环重新开始") && cycle.finding("EG912 恢复循环重新开始")->severity == 0 &&
          cycle.finding("EG912 恢复循环重新开始")->ev[0].lineNo == 4 && cycle.finding("已进入失败重试状态") &&
          !cycle.finding("选网事务失败") && cycle.outages.size() == 1 && cycle.outages[0].dur == 20 &&
          cycle.outages[0].endLine == 7 && isEventLine(cycle.lines[3]),
          "EG912第4级重启恢复循环后仍按实际公网边沿恢复，不误认切运营商或提前关闭断网");
    Log ipc({banner(), ready(5), at(6, "[NANOMSG] bad request: json_parse (cnt=1)"),
             at(7, "send back =========== >{\"cellular\":{\"rx\":4294967300,\"tx\":4294967400}}"),
             at(20, "[HB30] online=1")});
    check(ipc.outages.empty() && ipc.metrics.size() == 1 && !ipc.rkFault() &&
          ipc.lines[2].msg == "bad request: json_parse (cnt=1)" && isEventLine(ipc.lines[2]) &&
          ipc.lines[3].msg.find("4294967300") != std::string::npos,
          "消息帧修复保留既有NANOMSG日志与64位JSON回显，不把本地坏请求或字节累计值当公网/包计数故障");
}

int main() {
    firstInternetAndRecovery(); adoptionAndIsolation(); diagnosisAndTimeline(); sourceFixture(); e15TrafficAndSelection();
    std::printf("\n== 失败 %d 项\n", failures);
    return failures ? 1 : 0;
}
