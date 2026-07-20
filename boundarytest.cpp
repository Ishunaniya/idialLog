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
// 构建运行:make boundarytest && ./boundarytest      (rc=0 全过)
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
        "[1970-01-01 00:00:20] [HEARTBEAT] CSQ:16",
        "[2026-06-30 12:00:00] [HEARTBEAT] CSQ:20",   // ← 第3行:跳变
        "[2026-06-30 12:00:10] [HEARTBEAT] CSQ:21"
    });
    std::vector<LogLine> ll; std::vector<std::string> ss; ParseAudit ad;
    parseLines(raw, ll, ss, &ad);
    ok(ad.clockJump, "检测到时钟跳变");
    ok(ad.jumpAtLine == 3, ("跳变行号 == 3(实得 " + std::to_string(ad.jumpAtLine) + ")").c_str());
    ok(fmtTime(ad.jumpFromT, "FULL") == "1970-01-01 00:00:20", "跳变前时间正确(1970)");
    ok(fmtTime(ad.jumpToT, "FULL") == "2026-06-30 12:00:00", "跳变后时间正确(2026)");
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
        size_t a = 0;
        while (a <= acc.size()) {
            size_t b = acc.find('\n', a);
            if (b == std::string::npos) { if (a < acc.size()) raw.push_back(acc.substr(a)); break; }
            raw.push_back(acc.substr(a, b - a)); a = b + 1;
        }
        std::vector<LogLine> ll; std::vector<std::string> ss; ParseAudit ad;
        parseLines(raw, ll, ss, &ad);
        std::string base = p; size_t sl = base.find_last_of('/');
        if (sl != std::string::npos) base = base.substr(sl + 1);
        ok(!ad.clockJump, (base + " 无跳变(证实真机无此样本,检测也不误报)").c_str());
    }
}

int main() {
    t1_cross_file_continuation();
    t2_intra_file_continuation_still_works();
    t3_empty_boundaries_is_legacy();
    t4_clock_jump_detection();
    t5_no_false_jump_on_normal();
    t6_pure_unsynced_no_jump();
    t7_real_unsynced_fixture_no_jump();
    std::printf("\n%s 失败 %d 项\n", g_fail ? "**" : "==", g_fail);
    return g_fail ? 1 : 0;
}
