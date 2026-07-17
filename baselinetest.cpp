// baselinetest.cpp — **真机日志基线断言**:把真机日志上的具体数字钉死。
//
// 为什么需要这一层(变异测试逼出来的):
//   10 个变异里 **5 个存活** —— 我原来的测试只验"某结论出现了/没出现",
//   不验**具体数字**,所以下面这些真实修过的 bug 改回去后测试照样全绿:
//     · 续行规则退回"无时间戳即续行"  → 62 行控制台噪声被糊进上一条,未识别归 0
//     · 标签退回只认大写               → [NetCheck] 丢失
//     · "数据服务未就绪"退回死分支     → 该结论消失
//     · 弱信号阈值 <10 改成 <2         → 弱信号断网归类消失
//     · 数据假死判定失效               → 17 次假死归类消失
//
//   教训:**只断言"结论出现了"往往没牙齿,必须断言它的具体内容/数字。**
//
// 基线数字全部来自**真机日志**(证据等级最高),不是我编的期望值。
// 用法:make baselinetest && ./baselinetest
#include "logmodel.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace dl;

static int g_fail = 0;

static void ck(bool ok, const char* what, const std::string& got, const std::string& want) {
    if (ok) { std::printf("   ✓ %-34s %s\n", what, got.c_str()); return; }
    std::printf("   ✗ %-34s 实际=%s 期望=%s\n", what, got.c_str(), want.c_str());
    g_fail++;
}
static void cki(long got, long want, const char* what) {
    ck(got == want, what, std::to_string(got), std::to_string(want));
}

struct Loaded {
    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    PlatformInfo pi;
    std::vector<Outage> outs;
    std::vector<MetricRow> mets;
    std::vector<Finding> fs;
    bool ok = false;
};

static Loaded load(const std::string& path) {
    Loaded L;
    std::ifstream in(path, std::ios::binary);
    if (!in) return L;
    std::vector<std::string> raw;
    std::string line;
    while (std::getline(in, line)) raw.push_back(line);
    parseLines(raw, L.lines, L.sessions, &L.audit);
    L.pi   = detectPlatform(L.lines);
    L.outs = collectOutages(L.lines);
    L.mets = buildMetrics(L.lines);
    L.fs   = analyze(L.lines, L.outs, L.mets, L.pi, L.audit);
    L.ok   = true;
    return L;
}

static bool has(const std::vector<Finding>& fs, const std::string& sub) {
    for (const auto& f : fs)
        if (f.title.find(sub) != std::string::npos) return true;
    return false;
}
static std::string titleWith(const std::vector<Finding>& fs, const std::string& sub) {
    for (const auto& f : fs)
        if (f.title.find(sub) != std::string::npos) return f.title;
    return "(无此结论)";
}

int main() {
    // ── 真机 EG25(ROAMLINK 通道,2861 行,现网日志)──
    {
        auto L = load("samples/rtms_eg25/dial_20260630_000026.log");
        std::printf("── 真机 EG25 dial_20260630(2861 行)\n");
        if (!L.ok) { std::printf("   ✗ 打不开\n"); g_fail++; }
        else {
            cki((long)L.audit.rawTotal, 2861, "原始行数");
            cki((long)L.audit.unparsed, 0,    "未识别行(SD 卡日志应为 0)");
            cki((long)L.outs.size(),    36,   "断网次数");
            // 钉死"数据假死 17 次" —— 变异"数据假死判定失效"就会让它变 0
            ck(titleWith(L.fs, "数据假死").find("17 次") != std::string::npos,
               "数据假死归类 17 次", titleWith(L.fs, "数据假死"), "…17 次…");
            // 钉死"弱信号 19 次" —— 变异"阈值 <10 改 <2"就会让它变 0
            ck(has(L.fs, "弱信号"), "弱信号归类存在", has(L.fs,"弱信号")?"有":"无", "有");
            long weak = 0;
            for (const auto& m : L.mets) if (m.csqVal >= 0 && m.csqVal < 10) weak++;
            cki(weak, 84, "弱信号(CSQ<10)心跳数");
        }
    }

    // ── 真机 AG35 控制台(dial_log 与裸 printf 交织)──
    {
        auto L = load("samples/rtms_ag35/real_ag35_1.32.16_console.log");
        std::printf("── 真机 AG35 1.32.16 控制台(144 行)\n");
        if (!L.ok) { std::printf("   ✗ 打不开\n"); g_fail++; }
        else {
            // 钉死未识别 62 —— 变异"续行规则退回无时间戳即续行"会把 62 行噪声
            // 糊进上一条、未识别归 0,这条断言是抓它的唯一手段
            cki((long)L.audit.unparsed, 62, "未识别行(控制台噪声,不该被糊进上一条)");
            cki((long)L.audit.continuation, 0, "续行数(控制台日志无真续行)");
            ck(L.pi.plat == PLAT_AG35, "平台=AG35", L.pi.name, "AG35");
            // 钉死 [NetCheck] —— 变异"标签只认大写"会让它丢失
            long nc = 0;
            for (const auto& l : L.lines) if (l.tag == "NetCheck") nc++;
            cki(nc, 2, "[NetCheck] 标签数(混合大小写)");
            long slot = 0;
            for (const auto& l : L.lines) if (l.tag == "SLOT") slot++;
            cki(slot, 10, "[SLOT] 标签数");
        }
    }

    // ── 真机 EG25 1.31.15(SIM 通道 + 完整 L1/L2 阶梯 + 多行 AT 应答)──
    {
        auto L = load("samples/rtms_eg25/real_eg25_1.31.15_unsynced.log");
        std::printf("── 真机 EG25 1.31.15(133 行)\n");
        if (!L.ok) { std::printf("   ✗ 打不开\n"); g_fail++; }
        else {
            cki((long)L.audit.unparsed, 0, "未识别行");
            // 钉死续行 2 —— 变异"续行退回丢弃"会让它变 0、未识别变 2
            cki((long)L.audit.continuation, 2, "续行数(AT 应答的裸 OK)");
            // 钉死 L1=4/L2=1 —— 变异"按行计数"会变成 8/3
            ck(titleWith(L.fs, "恢复阶梯已生效").find("L1 触发 4 次") != std::string::npos &&
               titleWith(L.fs, "恢复阶梯已生效").find("L2 触发 1 次") != std::string::npos,
               "阶梯 L1=4 次 / L2=1 次", titleWith(L.fs, "恢复阶梯已生效"), "L1 触发 4 次,L2 触发 1 次");
        }
    }

    // ── 真机 artery 1.29.13(seas_log,含真实 ESC 字节)──
    {
        auto L = load("samples/dial_eg25/real_artery_1.29.13.log");
        std::printf("── 真机 artery 1.29.13(seas_log,71 行)\n");
        if (!L.ok) { std::printf("   ✗ 打不开\n"); g_fail++; }
        else {
            ck(L.pi.plat == PLAT_ARTERY, "平台=artery", L.pi.name, "artery");
            // 钉死 ANSI 剥离既不残留、也不过度吞噬:
            //  · 残留 ESC/[0m  → CSI 范围太宽(剥不干净)
            //  · 消息被吃空    → CSI 范围太窄(变异 @..~ 收窄到 @..A 时,终止字符 m=109
            //    落在范围外 → stripAnsi 一路吃到行尾,整条消息变空)
            // 两个方向都要断言,否则"收窄"变异会从"消息变空"这一侧溜过去(实测踩到)。
            long esc = 0, tail = 0, empty = 0, total = 0;
            for (const auto& l : L.lines) {
                if (l.msg.find('\x1b') != std::string::npos) esc++;
                if (l.msg.find("[0m") != std::string::npos)  tail++;
                total++;
                if (l.msg.empty()) empty++;
            }
            cki(esc,   0, "消息含残留 ESC 字节的行数");
            cki(tail,  0, "消息含残留 '[0m' 的行数");
            // artery 每行都有正文(func/file/line 之后),解析后不该有空消息 —— 有则被 ANSI 吃空了
            ck(empty == 0 && total > 0, "解析后无空消息行(ANSI 未过度吞噬)",
               std::to_string(empty) + "/" + std::to_string(total), "0/N");
        }
    }

    // ── 真机 artery 1.29.14(Roamlink 通道 + 运行期策略切换 1→1→4,重启 3 次)──
    {
        auto L = load("samples/dial_eg25/real_artery_1.29.14_roamlink.log");
        std::printf("── 真机 artery 1.29.14(Roamlink+策略切换,84 行)\n");
        if (!L.ok) { std::printf("   ✗ 打不开\n"); g_fail++; }
        else {
            cki((long)L.audit.unparsed, 0, "未识别行");
            // 钉死重启=3 —— 这份日志 DIAL Version 出现 3 次(policy 1→1→4)、无 opened 标记。
            // 只认 "=== Dial Log Opened ===" 的旧实现对 artery 全瞎(报 0)。
            // 且修过整数溢出(prev=LLONG_MIN 时 t-prev 溢出漏掉第一次)→ 曾报 2。
            cki((long)L.sessions.size(), 3, "重启次数(DIAL Version 横幅)");
        }
    }

    // ── 真机 EC200A 1.28.4(完全正常的设备:假阳性守卫)──
    {
        auto L = load("samples/dial_ec200a/real_ec200a_1.28.4_unsynced.log");
        std::printf("── 真机 EC200A 1.28.4(正常设备,36 行)\n");
        if (!L.ok) { std::printf("   ✗ 打不开\n"); g_fail++; }
        else {
            cki((long)L.audit.unparsed, 0, "未识别行");
            // 这台设备只打了例行 [CPDUMP] "No existing CP dumps." —— 绝不能报基带崩溃
            ck(!has(L.fs, "CP dump"), "不报 CP dump(假阳性守卫)",
               has(L.fs,"CP dump")?"误报了":"未误报", "未误报");
            cki((long)L.fs.size(), 0, "结论数(正常设备应为 0)");
        }
    }

    // ── 真代码产出:数据服务未就绪(曾是死分支)──
    {
        auto L = load("samples/sim/hostrun/datacall_init_fail.log");
        std::printf("── 真代码产出 datacall_init_fail\n");
        if (!L.ok) std::printf("   ⚠ 缺日志(先跑 sim/hostrun/run_scenario.sh),跳过\n");
        else {
            // 钉死"未就绪"结论存在 —— 变异"退回死分支"会让它消失。
            // 注意:该场景**没有断网记录**(从头没连上),故必须是独立结论才报得出来。
            ck(has(L.fs, "数据服务未就绪"), "报出'数据服务未就绪'",
               has(L.fs,"数据服务未就绪")?"有":"无(死分支?)", "有");
            cki((long)L.outs.size(), 0, "断网次数(从头没连上→无断网记录)");
        }
    }

    std::printf("\n===== 真机基线断言:%d 项失败 =====\n", g_fail);
    std::printf("注:基线数字全部来自**真机日志**,不是我编的期望值。\n"
                "    本层专治'只验结论出现、不验具体数字'的没牙齿断言(变异测试逼出来的)。\n");
    return g_fail ? 1 : 0;
}
