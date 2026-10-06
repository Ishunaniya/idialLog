// 旧版 artery 的真实日志与反例：状态监测、SDK 事件、证据边界及未知小区。
#include "logmodel.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
using namespace dl;
static int failures = 0, checks = 0;

static void check(bool ok, const char* name) {
    ++checks;
    if (!ok)
        ++failures;
    std::printf("  [%s] %s\n", ok ? "通过" : "失败", name);
}

static std::string at(int seconds, const std::string& message) {
    char prefix[100];
    std::snprintf(prefix, sizeof(prefix), "2026-09-21 04:%02d:%02d.000 [INFO] main (main.c:237) - ", seconds / 60,
                  seconds % 60);
    return prefix + message;
}

static std::string hb(int seconds, const std::string& state = "start_call", const std::string& extra = "") {
    return at(seconds, "[HEARTBEAT] state=" + state + " csq=18 tcp_fail=0 " + extra);
}

struct Log {
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    std::vector<Outage> outages;
    std::vector<MetricRow> metrics;
    std::vector<Finding> findings;
    ArteryDiagnostics diagnostics;
    AvailabilityStats availability;
    CellAnalysis cells;

    Log(const std::vector<std::string>& raw, const std::vector<size_t>& boundaries = {}) {
        parseLines(raw, lines, sessions, &audit, boundaries);
        outages = collectOutages(lines);
        metrics = buildMetrics(lines);
        diagnostics = collectArteryDiagnostics(lines);
        availability = availabilityStats(lines, outages);
        cells = analyzeCells(lines, metrics, outages);
        findings = analyze(lines, outages, metrics, detectPlatform(lines), audit, &cells);
    }

    const Finding* finding(const std::string& name) const {
        for (const auto& f : findings)
            if (f.title.find(name) != std::string::npos)
                return &f;
        return nullptr;
    }
};

static void stateAndEvents() {
    std::puts("== 状态与旧回调 ==");
    Log normal({at(0, "DIAL Version: 1.29.15"), at(1, "state: pre_start_call -> start_call"), hb(10),
                at(11, "Net Connected"), at(12, "state: wait_for_connect -> net_connected"), hb(360, "net_connected")});
    check(normal.diagnostics.startCallStalls.empty() && !normal.finding("长期停留"), "正常拨号不报长期停留");
    check(!normal.availability.evidenceLimited() && normal.availability.runtimePercent() == 100,
          "正常日志原可用率数值和可信边界保持");
    check(normal.lines[1].tagText() == "STATE" && normal.lines[2].msg.find("state=start_call") != std::string::npos &&
              isEventLine(normal.lines[1]) && normal.lines[3].tagText() == "SDK" && isEventLine(normal.lines[3]),
          "历史文案仅补标签、正文不改，时间线可见");
    Log stall({at(0, "DIAL Version: 1.29.15"), at(1, "policy=force_sim: SIM failed, redialing"),
               at(2, "state: pre_start_call -> start_call"), hb(62), hb(122), at(130, "Net Connected"), hb(182),
               hb(242), hb(302), hb(362)});
    check(stall.diagnostics.startCallStalls.size() == 1 &&
              stall.diagnostics.startCallStalls[0].end - stall.diagnostics.startCallStalls[0].start == 360 &&
              stall.diagnostics.startCallStalls[0].heartbeatCount == 6,
          "连续心跳确定观察时段和数量");
    check(stall.finding("长期停留") && stall.finding("长期停留")->severity == 2 &&
              stall.diagnostics.startCallStalls[0].redialLine == 2 &&
              stall.diagnostics.startCallStalls[0].sdkConnectedLine == 6,
          "TCP重拨与SDK联网留证据，SDK联网不结束应用停留");
    check(stall.outages.empty() && stall.availability.evidenceLimited() && stall.availability.arteryStateStall &&
              availabilityEvidenceNote(stall.availability).find("不能据此确认") != std::string::npos,
          "状态停留不伪造业务断网，可用率标记证据不足");
    Log callbacks({at(0, "Net disconnected, and reason code 0x0"), at(2, "Net Connected"),
                   at(4, "Net disconnected, and reason code 0xd"),
                   at(5, "[SDK] DataCall disconnected | initiator=APP_STOP | reason=FORCE_SIM_TCP_REDIAL"),
                   at(6, "[SDK] DataCall disconnected | initiator=SDK_URC | reason=UNSOLICITED")});
    const auto stats = collectDataCallStats(callbacks.lines);
    check(stats.disconnected == 4 && stats.legacy == 2 && stats.appStop == 1 && stats.sdkUrc == 1 &&
              stats.unsolicited == 1,
          "旧SDK回调与新版APP_STOP/SDK_URC分类不混淆");
    check(stats.reasons.at("0x0") == 1 && stats.reasons.at("0xd") == 1 && callbacks.availability.legacyArteryEvents,
          "旧回调保留原原因码，并限制可用率解释");
    Log decoys({at(0, "explain state: a -> b"), at(1, "Net Connected later"),
                at(2, "Net disconnected, and reason code 0xzz"),
                "[2026-09-21 04:00:03] [INFO] Net disconnected, and reason code 0xd"});
    check(collectDataCallStats(decoys.lines).disconnected == 0 && decoys.diagnostics.legacyDisconnected == 0 &&
              decoys.lines[0].tagText().empty() && decoys.lines[1].tagText().empty(),
          "说明文字、坏原因码和非artery格式不匹配历史规则");
    Log licenses({at(0, "license pending: SIM TCP failed, redialing SIM"), at(1, "state: pre_start_call -> start_call"),
                  hb(61), hb(121), hb(181), hb(241), hb(301)});
    check(licenses.diagnostics.startCallStalls.size() == 1 && licenses.diagnostics.startCallStalls[0].redialLine == 1,
          "license_pending同类TCP路径可记录，但不从普通状态推定license问题");
}

static void boundaries() {
    std::puts("== 观测边界与混流 ==");
    const std::vector<std::string> shortHbs = {hb(0), hb(60), hb(120), hb(180)};
    Log shortLog(shortHbs);
    check(shortLog.diagnostics.startCallStalls.empty(), "不足300秒不诊断长期停留");
    Log sparse({hb(0), hb(100), hb(360)});
    check(sparse.diagnostics.startCallStalls.empty(), "心跳空洞不冒充持续停留");
    Log gap({hb(0), hb(60), hb(120), hb(180), hb(240), hb(300), hb(600), hb(660), hb(720), hb(780), hb(840), hb(900)});
    check(gap.diagnostics.startCallStalls.size() == 2 &&
              gap.diagnostics.startCallStalls[0].end - gap.diagnostics.startCallStalls[0].start == 300,
          "长停留经空洞分成两个独立区间");
    Log restart({hb(0), hb(60), hb(120), hb(180), at(200, "DIAL Version: 1.29.15"), hb(240), hb(300), hb(360)});
    check(restart.diagnostics.startCallStalls.empty(), "重启横幅禁止跨会话拼接");
    Log exit({at(0, "state: pre_start_call -> start_call"), hb(60), hb(120),
              at(130, "state: start_call -> wait_for_connect"), hb(360)});
    check(exit.diagnostics.startCallStalls.empty(), "退出start_call后旧心跳不归入新状态");
    Log backward({hb(0), hb(60), hb(120), hb(180), hb(20), hb(80), hb(140), hb(200)});
    check(backward.diagnostics.startCallStalls.empty(), "时钟回退截段");
    Log sources({hb(0), hb(60), hb(120), hb(180), hb(240), hb(300), hb(360)}, {4});
    check(sources.diagnostics.startCallStalls.empty(), "不同来源不拼接心跳");
    Log mixed({at(0, "state: wait_for_connect -> net_connected"), hb(60), hb(70, "net_connected"), hb(120),
               hb(130, "net_connected"), hb(180)});
    check(mixed.finding("状态证据冲突") && mixed.diagnostics.contradictoryStateEvidence.size() == 3 &&
              mixed.diagnostics.startCallStalls.empty() && mixed.availability.mixedArteryStates,
          "新状态与旧心跳交错只报证据冲突，不擅自确定实例数或制造新停留");
    Log transient({at(0, "state: wait_for_connect -> net_connected"), hb(1), hb(5), hb(60, "net_connected")});
    check(!transient.finding("状态证据冲突"), "短暂线程交错不报混流");
}

static void cellsAndDeny() {
    std::puts("== 未知小区与DENY证据级别 ==");
    Log unknown({hb(0, "net_connected", "cellid=A"), hb(60, "net_connected", "cellid=NA"),
                 hb(120, "net_connected", "cellid=B"), hb(180, "net_connected", "cellid=N/A"),
                 hb(240, "net_connected", "cellid=FFFFFFFF"), hb(300, "net_connected", "cellid=B")});
    check(unknown.cells.cells.size() == 2 && unknown.cells.switchCount == 0 && unknown.cells.pingPongCount == 0,
          "NA/N/A/FFFFFFFF不是小区，未知样本终止切换连续性");
    check(unknown.metrics[1].cellId.empty() && unknown.metrics[3].cellId.empty() && unknown.metrics[4].cellId.empty(),
          "无效小区不继承旧ID");
    Log healthy({hb(0, "net_connected", "cellid=A"), hb(60, "net_connected", "cellid=B"),
                 hb(120, "net_connected", "cellid=A")});
    check(healthy.cells.switchCount == 2 && healthy.cells.pingPongCount == 1, "连续有效A→B→A保留原切换/乒乓统计");
    Log gap({hb(0, "net_connected", "cellid=A"), hb(601, "net_connected", "cellid=B")});
    check(gap.cells.switchCount == 0, "超过600秒空洞不拼接确定切换");
    Log deny({hb(0, "net_connected", "SRV:0 RAT:HSPA+ DENY:10"), hb(60, "net_connected", "SRV:2 RAT:LTE DENY:21")});
    const auto* f = deny.finding("SDK DENY");
    check(f && f->severity == 1 && !deny.finding("明确拒绝") &&
              f->ev[0].text.find("NETWORK_FAILURE") != std::string::npos,
          "artery DENY10是SDK网络失败线索，不提升为明确拒绝");
    Log full({hb(0, "net_connected", "SRV:2 RAT:LTE DENY:10")});
    check(!full.finding("SDK DENY"), "FULL时非零DENY不直接报当前拒绝");
    Log rejected({at(0, "[INIT] [SIM-ACCOUNT] NETWORK REJECTED (REG=3)")});
    check(rejected.finding("明确拒绝") && rejected.finding("明确拒绝")->severity == 2,
          "真实REG3明确拒绝仍保留严重诊断");
    Log other({"[2026-09-21 04:00:00] DIAL Version: dial_ec200a_1.29.18",
               "[2026-09-21 04:00:01] [HEARTBEAT] SRV:0 RAT:LTE DENY:10"});
    const auto* otherDeny = other.finding("SDK DENY");
    check(otherDeny && otherDeny->ev[0].text.find("EG25 SDK") == std::string::npos, "非EG25 SDK不套用原因名称");
}

static void realLog() {
    std::puts("== 真机1.29.15日志 ==");
    std::ifstream input("samples/dial_eg25/real_artery_1.29.15_start_call_stall.log", std::ios::binary);
    check(bool(input), "真机样本必须存在，不能跳过");
    if (!input)
        return;
    std::vector<std::string> raw;
    std::string line;
    while (std::getline(input, line))
        raw.push_back(line);
    Log log(raw);
    check(raw.size() == 16810 && log.audit.parsed == 16790 && log.audit.unparsed == 2, "完整文件行数和未识别审计固定");
    const auto stats = collectDataCallStats(log.lines);
    check(stats.disconnected == 11 && stats.legacy == 11 && stats.appStop == 0 && stats.sdkUrc == 0,
          "11次旧SDK断开正确计数且不猜测发起方");
    size_t states = 0;
    for (const auto& l : log.lines)
        if (l.tagText() == "STATE")
            ++states;
    check(states == 37, "37条历史状态迁移进入时间线与总览");
    check(log.diagnostics.startCallStalls.size() == 1 && log.diagnostics.startCallStalls[0].entryLine == 367 &&
              log.diagnostics.startCallStalls[0].lastLine == 16728 &&
              log.diagnostics.startCallStalls[0].end - log.diagnostics.startCallStalls[0].start == 771335 &&
              log.diagnostics.startCallStalls[0].heartbeatCount == 12802,
          "最长应用停留L367→L16728，771335秒、12802条心跳，重启截段");
    check(log.outages.empty() && log.availability.evidenceLimited() && log.availability.arteryStateStall &&
              log.availability.legacyArteryEvents && log.availability.mixedArteryStates,
          "原日志不伪造9天业务断网，业务可用率有证据限制");
    check(log.finding("长期停留") && log.finding("状态证据冲突") && log.finding("SDK DENY") && !log.finding("明确拒绝"),
          "主异常和混流被发现，原明确拒绝误报消除");
    bool unknown = false;
    for (const auto& c : log.cells.cells)
        if (c.cellId == "NA" || c.cellId == "N/A" || c.cellId == "FFFFFFFF")
            unknown = true;
    check(!unknown && log.cells.cells.size() == 22 && !log.finding("小区切换频繁"),
          "未知ID不计真实小区，原频繁切换误报消除");
    LogView view;
    for (const auto& l : log.lines)
        view.push_back(&l);
    const auto diag = collectArteryDiagnostics(view);
    const auto availability = availabilityStats(view, log.outages);
    check(diag.startCallStalls.size() == log.diagnostics.startCallStalls.size() && diag.legacyDisconnected == 11 &&
              availability.evidenceLimited() && availability.mixedArteryStates,
          "GUI轻量视图与完整模型共享诊断及可信边界");
    bool allEvidence = true;
    for (const auto& f : log.findings)
        if (f.ev.empty())
            allEvidence = false;
    check(allEvidence, "所有新旧结论都有可跳转行号证据");
    std::printf("  真机统计：小区%zu、连续观测切换%zu、乒乓%zu\n", log.cells.cells.size(), log.cells.switchCount,
                log.cells.pingPongCount);
}

int main() {
    stateAndEvents();
    boundaries();
    cellsAndDeny();
    realLog();
    std::printf("== arterytest：%d项，失败%d ==\n", checks, failures);
    return failures ? 1 : 0;
}
