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
// 构建运行:make simtest && build/tests/regression/simtest        (rc=0 全过,rc=1 有断言失败)
// 生成的场景日志会写到 samples/sim/,可直接拖进 GUI 看。
#include "logmodel.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

//
// 【场景退役记录 2026-07-18】原 11 个手写场景中,已被真代码日志(hostruntest)或
// 真机数字(baselinetest)等价覆盖的 5 个已退役(从没联网/L3×3/恢复阶梯多行计数)——
// 手写版被证明是残缺的,真代码版证据等级更高。
// **保留的都是真代码注入不了、或无等价覆盖的**:注册被拒(桩注不了 Registration Denied)、
// 真 CP dump(桩造不出)、正常设备例行 CPDUMP、AG35 切卡代价归因(hostruntest 的 ag35
// 场景没触发 C_SWITCHING)、多会话重启、温度过高(读 sysfs 桩注不了)。
using namespace dl;

struct Scenario {
    std::string name;
    std::string file;                       // 写到 samples/sim/<file>
    std::string log;                        // 日志内容(串逐字照抄源码)
    std::vector<std::string> expect;        // 结论标题**须包含**这些子串
    std::vector<std::string> forbid;        // 结论标题**不得包含**这些子串(抓假阳性)
    Platform expectPlat = PLAT_UNKNOWN;     // 期望识别出的平台(PLAT_UNKNOWN=不检查)
    size_t   expectUnparsed = 0;            // 期望未识别行数
    std::vector<std::string> expectEvidence;// 结论证据必须逐字包含这些关键值
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
        { "注册被明确拒绝" },
        { "CP dump" },
        PLAT_EC200A, 0, {}
    });

    // 2026-08-18 四产品新增首次初始化 SIM 注册/账户诊断；每条逐字保留产品措辞。
    v.push_back({
        "modem_mng EC200A/AG35 明确网络拒绝(REG=3)",
        "ag35_sim_account_rejected.log",
        "=== Dial Log Opened [2026-08-18 09:00:00] daykey=2026-08-18 ===\n"
        "[2026-08-18 09:00:00] Program started. Version: 1.32.0\n"
        "[2026-08-18 09:00:01] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:3 | CSQ:18 | TEMP:35 | DownTime:0s | SLOT:SIM2\n"
        "[2026-08-18 09:00:02] [HEARTBEAT] Ping failed 3 consecutive times, fault timer started\n"
        "[2026-08-18 09:00:03] [INIT][SIM-ACCOUNT] SIM=SIM2 NETWORK REJECTED registration: SIM READY, REG=3, CSQ=18. Possible suspended/inactive SIM, roaming or operator restriction; confirm with carrier. Evidence: CPIN:READY | +CEREG: 2,3 | +COPS: 0 | +CGATT: 0 | +CEER: EPS services not allowed\n"
        "[2026-08-18 09:00:20] [HEARTBEAT] Network recovered after 18s\n",
        { "注册被明确拒绝", "断网根因分类:注册被拒" },
        { "受限服务", "疑似 SIM", "CEREG 查询" },
        PLAT_AG35, 0,
        { "SIM=SIM2 NETWORK REJECTED registration", "REG=3, CSQ=18" }
    });

    v.push_back({
        "modem_mng EG25 CEREG 受限服务",
        "eg25_sim_limited_service.log",
        "=== Dial Log Opened [2026-08-18 10:00:00] daykey=2026-08-18 ===\n"
        "[2026-08-18 10:00:00] [HEARTBEAT] CH:SIM | SIM:1 | REG:6 | CSQ:18 | Temp:35 | DownTime:0s | ConsecFail:0\n"
        "[2026-08-18 10:00:01] [INIT][SIM-REG] SIM=SIM1 LIMITED SERVICE: SIM READY, REG=6, CSQ=18. Normal packet data may be unavailable; possible subscription or network restriction, confirm with carrier. Evidence: +CPIN: READY | +CEREG: 2,6 | +COPS: 0 | +CGATT: 0 | +CEER: Limited service\n",
        { "SIM 注册处于受限服务" },
        { "注册被明确拒绝", "疑似 SIM", "CEREG 查询" },
        PLAT_EG25, 0,
        { "SIM=SIM1 LIMITED SERVICE", "REG=6, CSQ=18" }
    });

    v.push_back({
        "open_dial 持续 REG=0 疑似订阅异常",
        "open_dial_sim_account_suspected.log",
        "=== Dial Log Opened [2026-08-18 11:00:00] daykey=2026-08-18 ===\n"
        "[2026-08-18 11:00:00] [HEARTBEAT] SIM_AT:READY | SIM_CB:READY | REG:0 | CSQ:20 | TEMP:35 | DownTime:0s\n"
        "[2026-08-18 11:02:01] [INIT][SIM-ACCOUNT] SIM=SIM1 SUSPECTED subscription issue: SIM READY with good signal but REG=0 for >=120s. Possible suspended/inactive SIM or subscription restriction; AT evidence is not definitive, confirm with carrier. Evidence: CPIN:READY | +CEREG: 2,0 | +COPS: 0 | +CGATT: 0 | +CEER: No report\n",
        { "疑似 SIM 账户/订阅异常", "【推断】", "不是欠费/停机的确定证据" },
        { "注册被明确拒绝", "受限服务", "CEREG 查询" },
        PLAT_EC200A, 0,
        { "SUSPECTED subscription issue", "REG=0 for >=120s" }
    });

    v.push_back({
        "artery CEREG 查询/解析失败(禁止账户推断)",
        "artery_cereg_query_failed.log",
        "2026-08-18 12:00:00.123 [INFO] \x1b[0mmain (main.c:281) - [INIT][SIM-REG] SIM=SIM1 CEREG query/parse failed; registration state is unknown. Raw: +CEREG: malformed\n"
        "2026-08-18 12:00:01.124 [INFO] \x1b[0mmain (main.c:282) - carrier text said NETWORK REJECTED but this is not a structured SIM diagnostic\n",
        { "CEREG 查询/解析失败", "不得据此推断 SIM 或账户异常" },
        { "注册被明确拒绝", "受限服务", "疑似 SIM" },
        PLAT_ARTERY, 0,
        { "registration state is unknown", "Raw: +CEREG: malformed" }
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
        PLAT_EC200A, 0, {}
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
        PLAT_EC200A, 0, {}
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
        PLAT_AG35, 0, {}
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
        PLAT_EC200A, 0, {}
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
        PLAT_EC200A, 0, {}
    });




    return v;
}

static bool titlesHave(const std::vector<Finding>& fs, const std::string& sub) {
    for (const auto& f : fs)
        if (f.title.find(sub) != std::string::npos || f.detail.find(sub) != std::string::npos)
            return true;
    return false;
}

static bool titleHas(const std::vector<Finding>& fs, const std::string& sub) {
    for (const auto& f : fs)
        if (f.title.find(sub) != std::string::npos) return true;
    return false;
}

static bool evidenceHas(const std::vector<Finding>& fs, const std::string& sub) {
    for (const auto& f : fs)
        for (const auto& e : f.ev)
            if (e.text.find(sub) != std::string::npos)
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
        // 新诊断不仅要“有结论”，还必须保留 SIM/REG/CSQ/AT 原始证据的具体内容。
        for (const auto& e : sc.expectEvidence)
            if (!evidenceHas(fs, e)) { std::printf("   ✗ 结论证据缺少关键值:%s\n", e.c_str()); ok = false; }
        // 不该报的绝不能报(假阳性守卫)
        for (const auto& f : sc.forbid)
            if (titleHas(fs, f)) { std::printf("   ✗ 出现不该有的结论(假阳性):%s\n", f.c_str()); ok = false; }
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
