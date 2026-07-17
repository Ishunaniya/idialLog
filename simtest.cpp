// simtest.cpp — 场景模拟器 + 结论引擎断言测试
//
// ============================ 它能证明什么、不能证明什么 ============================
//
// ❌ **不能验证解析器**。本文件由我照源码写,解析器也由我照同一份源码写 ——
//    我若读错源码,模拟器就会生成解析器正好期待的东西,然后全绿通过。
//    那是拿自己的答案批自己的卷子。解析器的正确性**只能靠真机日志**证明
//    (实证:真机日志抓出 5 个 bug,合成夹具抓出 0 个)。
//
// ✅ **能验证结论引擎**。因为断言是独立参照系:每个场景声明"这份日志喂进去,
//    **应该**得出什么结论、**不该**得出什么结论"。结论引擎有一半分支零真机覆盖,
//    这是唯一能让它们不至于裸奔的手段。
//    实证价值:今天那个 CP dump 假阳性(正常设备被报基带崩溃),就是被
//    S_CPDUMP_NONE 这类"不该报"的断言抓住的类型。
//
// 为降低循环论证:**所有日志串逐字照抄源码**(每条都标了 file:line),不是我复述。
//
// 构建运行:make simtest && ./simtest        (rc=0 全过,rc=1 有断言失败)
// 生成的场景日志会写到 samples/sim/,可直接拖进 GUI 看。
#include "logmodel.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace dl;

struct Scenario {
    std::string name;
    std::string file;                       // 写到 samples/sim/<file>
    std::string log;                        // 日志内容(串逐字照抄源码)
    std::vector<std::string> expect;        // 结论标题**须包含**这些子串
    std::vector<std::string> forbid;        // 结论标题**不得包含**这些子串(抓假阳性)
    Platform expectPlat = PLAT_UNKNOWN;     // 期望识别出的平台(PLAT_UNKNOWN=不检查)
    size_t   expectUnparsed = 0;            // 期望未识别行数
};

// —— 各场景的日志串来源(逐字照抄,勿改措辞;改了就不再是源码实证)——
// ec200a/dial/dial.cpp:1615  "[INFO] never-connected, policy recovery (L1/L2/L3) gated. "
// ec200a/dial/dial.cpp:1082  "[WARNING] Registration Denied! Code %d.\n"
// ec200a/dial/dial.cpp:1701  "[RECOVERY L3] FATAL: Network down 35mins. Exiting for watchdog/init to reinitialize.\n"
// eg25/dial/dial.c:893       "[RECOVERY L3] FATAL: Network down 30mins. Exiting for watchdog to reinitialize.\n"
// eg25/dial/dial.c:875       "[RECOVERY L2] AT+CFUN=0 rsp: %s\n"
// ec200a/diag/diag.c:145     "[CPDUMP] Found %d existing CP dump(s): %s\n"
// ec200a/diag/diag.c         "[CPDUMP] No existing CP dumps.\n"
// open_dial/dial.c           "[RECOVERY L3] FATAL: Network down 35mins. Exiting for start_prog to reinitialize.\n"

static std::vector<Scenario> scenarios() {
    std::vector<Scenario> v;

    // ---- 1. 从未联网:恢复阶梯被 has_connected_once 门控(EC200A)----
    v.push_back({
        "EC200A 从未联网,阶梯被门控",
        "ec200a_never_connected.log",
        "=== Dial Log Opened [2026-07-17 09:00:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:00:00] Program started. Version: 1.31.3\n"
        "[2026-07-17 09:00:01] [INIT] ICCID: 89860000000000000000\n"
        "[2026-07-17 09:00:02] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:0 | CSQ:12 | TEMP:35 | DownTime:0s\n"
        "[2026-07-17 09:05:02] [HEARTBEAT] Ping failed 3 consecutive times, fault timer started\n"
        "[2026-07-17 09:05:02] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:0 | CSQ:12 | TEMP:35 | DownTime:300s\n"
        "[2026-07-17 09:10:02] [INFO] never-connected, policy recovery (L1/L2/L3) gated. \n"
        "[2026-07-17 09:15:02] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:0 | CSQ:12 | TEMP:35 | DownTime:900s\n",
        { "从未成功联网" },
        { "CP dump", "L3 已触发" },
        PLAT_EC200A, 0
    });

    // ---- 2. 注册被拒 ----
    v.push_back({
        "EC200A 注册被拒(Registration Denied)",
        "ec200a_reg_denied.log",
        "=== Dial Log Opened [2026-07-17 09:00:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:00:00] Program started. Version: 1.31.3\n"
        "[2026-07-17 09:00:02] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:0 | CSQ:20 | TEMP:35 | DownTime:0s\n"
        "[2026-07-17 09:00:03] [WARNING] Registration Denied! Code 3.\n"
        "[2026-07-17 09:05:03] [HEARTBEAT] Ping failed 3 consecutive times, fault timer started\n"
        "[2026-07-17 09:05:03] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:0 | CSQ:20 | TEMP:35 | DownTime:300s\n",
        { "注册被拒" },
        { "CP dump" },
        PLAT_EC200A, 0
    });

    // ---- 3. L3 触发(EC200A 措辞:35mins / watchdog/init)----
    v.push_back({
        "EC200A L3 触发(进程 exit)",
        "ec200a_recovery_l3.log",
        "=== Dial Log Opened [2026-07-17 09:00:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:00:00] Program started. Version: 1.31.3\n"
        "[2026-07-17 09:00:01] [PING] Ping 8.8.8.8 OK\n"
        "[2026-07-17 09:00:02] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:20 | TEMP:35 | DownTime:0s\n"
        "[2026-07-17 09:01:00] [HEARTBEAT] Ping failed 3 consecutive times, fault timer started\n"
        "[2026-07-17 09:01:00] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:20 | TEMP:35 | DownTime:60s\n"
        "[2026-07-17 09:06:00] [RECOVERY L1] LastErr: +CEER: 0,-1\n"
        "[2026-07-17 09:11:00] [RECOVERY L2] AT+CFUN=0 rsp: \nOK\n"
        "[2026-07-17 09:36:00] [RECOVERY L3] FATAL: Network down 35mins. Exiting for watchdog/init to reinitialize.\n",
        { "L3 已触发", "恢复阶梯已生效" },
        { "恢复阶梯一次都没触发", "CP dump" },
        PLAT_EC200A, 0
    });

    // ---- 4. 真有 CP dump ----
    v.push_back({
        "EC200A 基带崩溃(确有 CP dump)",
        "ec200a_cpdump_found.log",
        "=== Dial Log Opened [2026-07-17 09:00:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:00:00] [CPDUMP] Bind mounted /media/sdcard -> /sdcard for CP dump capture.\n"
        "[2026-07-17 09:00:01] [CPDUMP] Found 2 existing CP dump(s): dump_001.bin dump_002.bin\n"
        "[2026-07-17 09:00:02] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:20 | TEMP:35 | DownTime:0s\n",
        { "CP dump" },
        {},
        PLAT_EC200A, 0
    });

    // ---- 5. 假阳性守卫:正常设备只打例行 CPDUMP,绝不该报崩溃 ----
    //      (2026-07-17 真机 EC200A 1.28.4 就是栽在这里 —— 曾被误报 [严重] 基带崩溃)
    v.push_back({
        "EC200A 正常设备(例行 CPDUMP)—— 假阳性守卫",
        "ec200a_cpdump_none.log",
        "=== Dial Log Opened [2026-07-17 09:00:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:00:00] [CPDUMP] Bind mounted /media/sdcard -> /sdcard for CP dump capture.\n"
        "[2026-07-17 09:00:01] [CPDUMP] No existing CP dumps.\n"
        "[2026-07-17 09:00:02] [PING] Ping 8.8.8.8 OK\n"
        "[2026-07-17 09:00:03] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:20 | TEMP:35 | DownTime:0s\n",
        {},
        { "CP dump", "基带崩溃" },       // ← 一条结论都不该有
        PLAT_EC200A, 0
    });

    // ---- 6. AG35 切卡期间断网 ----
    v.push_back({
        "AG35 切卡期间断网(应归因为切卡代价)",
        "ag35_slot_switch.log",
        "=== Dial Log Opened [2026-07-17 09:00:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:00:00] Program started. Version: 1.32.16\n"
        "[2026-07-17 09:00:01] [SLOT] init: enable=1 primary_phy=1(eSIM) switch_after=600s cooldown=120s budget=3\n"
        "[2026-07-17 09:00:02] [PING] Ping 8.8.8.8 OK\n"
        "[2026-07-17 09:00:03] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:18 | TEMP:32 | DownTime:0s | SLOT:Phy\n"
        "[2026-07-17 09:10:00] [HEARTBEAT] Ping failed 3 consecutive times, fault timer started\n"
        "[2026-07-17 09:10:01] [SLOT] trigger(no-net): down 600s REG=0 CSQ=5 count=1/3 -> switch card\n"
        "[2026-07-17 09:10:02] [SLOT] switch to eSIM\n"
        "[2026-07-17 09:10:08] [SLOT] active now eSIM (card READY)\n"
        "[2026-07-17 09:10:20] [HEARTBEAT] Network recovered after 20s\n"
        "[2026-07-17 09:10:21] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:20 | TEMP:32 | DownTime:0s | SLOT:eSIM\n",
        { "切卡" },
        { "CP dump", "从未成功联网" },
        PLAT_AG35, 0
    });

    // ---- 7. 进程反复重启(多会话)----
    v.push_back({
        "EC200A 进程反复重启(多会话)",
        "ec200a_multi_session.log",
        "=== Dial Log Opened [2026-07-17 09:00:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:00:00] Program started. Version: 1.31.3\n"
        "[2026-07-17 09:00:01] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:20 | TEMP:35 | DownTime:0s\n"
        "=== Dial Log Opened [2026-07-17 09:10:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:10:00] Program started. Version: 1.31.3\n"
        "[2026-07-17 09:10:01] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:20 | TEMP:35 | DownTime:0s\n"
        "=== Dial Log Opened [2026-07-17 09:20:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:20:00] Program started. Version: 1.31.3\n"
        "[2026-07-17 09:20:01] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:20 | TEMP:35 | DownTime:0s\n",
        {},
        { "CP dump" },
        PLAT_EC200A, 0
    });

    // ---- 8. 温度过高 ----
    v.push_back({
        "EC200A 温度过高(≥85℃)",
        "ec200a_thermal.log",
        "=== Dial Log Opened [2026-07-17 09:00:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:00:00] Program started. Version: 1.31.3\n"
        "[2026-07-17 09:00:01] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:20 | TEMP:88 | DownTime:0s\n"
        "[2026-07-17 09:00:31] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:20 | TEMP:91 | DownTime:0s\n",
        { "温度偏高" },
        { "CP dump" },
        PLAT_EC200A, 0
    });

    // ---- 9. EG25 L3(措辞与 EC200A 不同:30mins / watchdog)----
    v.push_back({
        "EG25 L3 触发(措辞:30mins/watchdog)",
        "eg25_recovery_l3.log",
        "=== Dial Log Opened [2026-07-17 09:00:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:00:00] EG25 modem_mng Version: 1.31.15\n"
        "[2026-07-17 09:00:01] [ROAMLINK] network_select = 4\n"
        "[2026-07-17 09:00:02] [HEARTBEAT] CH:SIM | SIM:1 | REG:1 | CSQ:20 | Temp:40,35,35 | DownTime:0s | ConsecFail:0 | RL_FAIL:0\n"
        "[2026-07-17 09:01:00] [HEARTBEAT] Ping failed 3 consecutive times, fault timer started\n"
        "[2026-07-17 09:01:00] [HEARTBEAT] CH:SIM | SIM:1 | REG:1 | CSQ:20 | Temp:40,35,35 | DownTime:60s | ConsecFail:3 | RL_FAIL:0\n"
        "[2026-07-17 09:02:00] [RECOVERY L1] LastErr: +CEER: 0,-1\n"
        "[2026-07-17 09:07:00] [RECOVERY L2] AT+CFUN=0 rsp: \nOK\n"
        "[2026-07-17 09:31:00] [RECOVERY L3] FATAL: Network down 30mins. Exiting for watchdog to reinitialize.\n",
        { "L3 已触发", "恢复阶梯已生效" },
        { "恢复阶梯一次都没触发" },
        PLAT_EG25, 0
    });

    // ---- 10. 恢复阶梯次数必须按**事件**算,不按**行**算 ----
    //  变异测试证明:只断言"恢复阶梯已生效"出现是不够的 —— 把计数退回按行,
    //  测试照样全绿。必须断言**具体次数**才有牙齿。
    //  本场景照真机 EG25 1.31.15 的多行形态构造:
    //    一次 L1 = 2 行(LastErr + "REG down, skip redial",同秒)
    //    一次 L2 = 3 行(LastErr + CFUN=0 rsp + CFUN=1 rsp,跨 3 秒)
    //  共 2 次 L1(4 行)+ 1 次 L2(3 行)→ 必须报 "L1 触发 2 次,L2 触发 1 次"
    v.push_back({
        "恢复阶梯多行条目:次数须按事件不按行",
        "eg25_recovery_multiline_count.log",
        "=== Dial Log Opened [2026-07-17 09:00:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:00:00] EG25 modem_mng Version: 1.31.15\n"
        "[2026-07-17 09:00:01] [ROAMLINK] network_select = 4\n"
        "[2026-07-17 09:00:02] [HEARTBEAT] CH:SIM | SIM:1 | REG:1 | CSQ:20 | Temp:40,35,35 | DownTime:0s | ConsecFail:0 | RL_FAIL:0\n"
        "[2026-07-17 09:01:00] [HEARTBEAT] Ping failed 3 consecutive times, fault timer started\n"
        "[2026-07-17 09:01:00] [HEARTBEAT] CH:SIM | SIM:1 | REG:0 | CSQ:20 | Temp:40,35,35 | DownTime:60s | ConsecFail:3 | RL_FAIL:0\n"
        // 第 1 次 L1 —— 2 行同秒
        "[2026-07-17 09:02:00] [RECOVERY L1] LastErr: +CEER: 0,-1 | PDP: +CGACT: 1,1\n"
        "[2026-07-17 09:02:00] [RECOVERY L1] REG down, skip redial (downtime=120s)\n"
        // 第 2 次 L1 —— 2 行同秒(距上次 63s > 30s 合并窗)
        "[2026-07-17 09:03:03] [RECOVERY L1] LastErr: +CEER: 0,-1 | PDP: +CGACT: 1,1\n"
        "[2026-07-17 09:03:03] [RECOVERY L1] REG down, skip redial (downtime=183s)\n"
        // 第 1 次 L2 —— 3 行跨 3 秒
        "[2026-07-17 09:06:00] [RECOVERY L2] LastErr: +CEER: 0,-1 | PDP: +CGACT: 1,1\n"
        "[2026-07-17 09:06:01] [RECOVERY L2] AT+CFUN=0 rsp: \nOK\n"
        "[2026-07-17 09:06:03] [RECOVERY L2] AT+CFUN=1 rsp: \nOK\n"
        "[2026-07-17 09:06:20] [HEARTBEAT] Network recovered after 320s\n"
        "[2026-07-17 09:06:21] [HEARTBEAT] CH:SIM | SIM:1 | REG:1 | CSQ:20 | Temp:40,35,35 | DownTime:0s | ConsecFail:0 | RL_FAIL:0\n",
        { "L1 触发 2 次", "L2 触发 1 次" },   // ← 断言具体次数,这才挡得住"按行计数"回归
        { "L3 已触发", "恢复阶梯一次都没触发" },
        PLAT_EG25, 0
    });

    // ---- 11. open_dial 的 L3(措辞又不同:start_prog)----
    v.push_back({
        "open_dial L3 触发(措辞:start_prog)",
        "open_dial_recovery_l3.log",
        "=== Dial Log Opened [2026-07-17 09:00:00] daykey=2026-07-17 ===\n"
        "[2026-07-17 09:00:00] Program started. Main Version: 1.28.4\n"
        "[2026-07-17 09:00:01] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:17 | TEMP:25 | DownTime:0s\n"
        "[2026-07-17 09:01:00] [HEARTBEAT] Ping failed 3 consecutive times, fault timer started\n"
        "[2026-07-17 09:01:00] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:1 | CSQ:17 | TEMP:25 | DownTime:60s\n"
        "[2026-07-17 09:36:00] [RECOVERY L3] FATAL: Network down 35mins. Exiting for start_prog to reinitialize.\n",
        { "L3 已触发" },
        { "CP dump" },
        PLAT_EC200A, 0
    });

    return v;
}

static bool titlesHave(const std::vector<Finding>& fs, const std::string& sub) {
    for (const auto& f : fs)
        if (f.title.find(sub) != std::string::npos || f.detail.find(sub) != std::string::npos)
            return true;
    return false;
}

int main() {
    auto scs = scenarios();
    int fail = 0;

    for (const auto& sc : scs) {
        // 写出场景日志(可直接拖进 GUI 复现)
        std::string path = "samples/sim/" + sc.file;
        { std::ofstream o(path, std::ios::binary); o << sc.log; }

        // 切行
        std::vector<std::string> raw;
        std::string cur;
        for (char c : sc.log) {
            if (c == '\n') { raw.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) raw.push_back(cur);

        std::vector<LogLine> lines;
        std::vector<std::string> sessions;
        ParseAudit audit;
        parseLines(raw, lines, sessions, &audit);
        PlatformInfo pi = detectPlatform(lines);
        auto outs = collectOutages(lines);
        auto mets = buildMetrics(lines);
        auto fs   = analyze(lines, outs, mets, pi, audit);

        std::printf("── %s\n", sc.name.c_str());
        bool ok = true;

        // 审计自洽(任何时候都必须成立)
        size_t sum = audit.parsed + audit.session + audit.blank + audit.continuation + audit.unparsed;
        if (sum != audit.rawTotal) {
            std::printf("   ✗ 审计不自洽: %zu != %zu\n", sum, audit.rawTotal); ok = false;
        }
        if (audit.unparsed != sc.expectUnparsed) {
            std::printf("   ✗ 未识别行 %zu,期望 %zu\n", audit.unparsed, sc.expectUnparsed); ok = false;
        }
        if (sc.expectPlat != PLAT_UNKNOWN && pi.plat != sc.expectPlat) {
            std::printf("   ✗ 平台识别为 %s,不符期望\n", pi.name.c_str()); ok = false;
        }
        // 该报的必须报
        for (const auto& e : sc.expect)
            if (!titlesHave(fs, e)) { std::printf("   ✗ 缺少应有的结论:%s\n", e.c_str()); ok = false; }
        // 不该报的绝不能报(假阳性守卫)
        for (const auto& f : sc.forbid)
            if (titlesHave(fs, f)) { std::printf("   ✗ 出现不该有的结论(假阳性):%s\n", f.c_str()); ok = false; }
        // 铁律:结论必带证据
        for (const auto& f : fs)
            if (f.ev.empty()) { std::printf("   ✗ 无证据的结论:%s\n", f.title.c_str()); ok = false; }

        if (ok) std::printf("   ✓ 通过(平台=%s 结论%zu条 未识别%zu)\n",
                            pi.name.c_str(), fs.size(), audit.unparsed);
        else fail++;
    }

    std::printf("\n===== %zu 个场景,%d 个失败 =====\n", scs.size(), fail);
    std::printf("注:本测试只验证**结论引擎**,不验证解析器 —— 日志由我照源码生成,\n"
                "    解析器也由我照同一份源码写,同源错误无法自证。解析器只能靠真机日志验证。\n");
    return fail ? 1 : 0;
}
