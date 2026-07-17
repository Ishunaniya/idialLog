// hostruntest.cpp — 对 **真代码产出的日志**(sim/hostrun*/)做结论断言。
//
// 与 simtest.cpp 的分工(重要):
//   simtest.cpp    : 日志**我手写** → 只保留真代码注入不了的场景(注册被拒 / 真 CP dump /
//                    多会话重启 / 温度过高 —— 这些桩造不出来)。证据等级【源码阅读实证】。
//   hostruntest.cpp: 日志由**真代码自己打**(sim/hostrun* 的 driver 跑出来的)。
//                    证据等级【源码执行实证】,比手写高一档。
//
// 为什么必须分开、且优先用这一份:
//   实证 —— 我手写的 never-connected 日志是
//     "[INFO] never-connected, policy recovery (L1/L2/L3) gated."
//   真代码实际打的是
//     "...gated. Downtime 604s, REG=1, data service OK but no data
//       -- suspected SIM/account issue (e.g. suspended / no data plan)."
//   **我漏了整个后半句**(源码里那条 dial_log 是跨行拼接,我 grep 只抓到第一个字面量)。
//   凡是真代码能产出的场景,就不该再用我手写的残缺版去"验证"。
//
// 前置:先跑 sim/hostrun*/run_scenario.sh 生成日志(本测试只读,不生成)。
// 用法:make hostruntest && ./hostruntest
#include "logmodel.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace dl;

struct Case {
    std::string file;                    // samples/sim/hostrun*/xxx.log
    std::vector<std::string> expect;     // 结论里**必须**出现的子串
    std::vector<std::string> forbid;     // **不得**出现的子串(假阳性守卫)
    Platform plat = PLAT_UNKNOWN;        // 期望平台(PLAT_UNKNOWN=不检查)
};

static std::vector<Case> cases() {
    return {
        // ── EC200A(sim/hostrun,真代码 ec200a/dial/dial.cpp 产出)──
        { "samples/sim/hostrun/never_connected.log",
          { "从未成功联网" }, { "CP dump", "L3 已触发" }, PLAT_EC200A },
        { "samples/sim/hostrun/recovery_ladder.log",
          { "L3 已触发", "恢复阶梯已生效" }, { "恢复阶梯一次都没触发" }, PLAT_EC200A },
        { "samples/sim/hostrun/datacall_init_fail.log",
          // 这条曾是**死分支**(C_NOTREADY 只在 enum 有名字、无任何赋值),
          // 且它没有断网记录(从头没连上),故必须是独立结论才报得出来
          { "数据服务未就绪" }, { "CP dump" }, PLAT_EC200A },
        { "samples/sim/hostrun/all_normal.log",
          {}, { "CP dump", "从未成功联网", "L3 已触发" }, PLAT_EC200A },   // 正常设备:零严重结论

        // ── AG35 双卡(须 AG35=1 重编 driver,否则 slot_mgr 是空 TU、[SLOT] 一条都没有)──
        { "samples/sim/hostrun/ag35_cold_select.log", {}, { "CP dump" }, PLAT_AG35 },
        { "samples/sim/hostrun/ag35_weak_switch.log", {}, { "CP dump" }, PLAT_AG35 },

        // ── open_dial(sim/hostrun_open_dial,真代码 dial.c 产出)──
        { "samples/sim/hostrun_open_dial/od_never_connected.log",
          { "从未成功联网" }, { "CP dump" }, PLAT_EC200A },
        { "samples/sim/hostrun_open_dial/od_recovery_ladder.log",
          { "L3 已触发" }, {}, PLAT_EC200A },
        { "samples/sim/hostrun_open_dial/od_all_normal.log",
          {}, { "CP dump", "从未成功联网" }, PLAT_EC200A },

        // ── EG25(sim/hostrun_eg25,真代码 eg25/dial/dial.c 产出)──
        { "samples/sim/hostrun_eg25/eg25_recovery_ladder.log",
          { "恢复阶梯已生效" }, {}, PLAT_EG25 },
        { "samples/sim/hostrun_eg25/eg25_weak_signal.log",
          { "弱信号" }, { "CP dump" }, PLAT_EG25 },

        // ── artery(sim/hostrun_artery,真代码 src/dial/dial.c 产出,seas_log 格式)──
        { "samples/sim/hostrun_artery/artery_all_normal.log", {}, { "CP dump" }, PLAT_ARTERY },
        { "samples/sim/hostrun_artery/artery_reg_down.log",   {}, { "CP dump" }, PLAT_ARTERY },
    };
}

static bool has(const std::vector<Finding>& fs, const std::string& sub) {
    for (const auto& f : fs)
        if (f.title.find(sub) != std::string::npos || f.detail.find(sub) != std::string::npos)
            return true;
    return false;
}

int main() {
    int fail = 0, skip = 0;
    for (const auto& c : cases()) {
        std::ifstream in(c.file, std::ios::binary);
        if (!in) {
            std::printf("── %-52s ⚠ 缺日志(先跑对应 run_scenario.sh)\n", c.file.c_str());
            skip++;
            continue;
        }
        std::vector<std::string> raw;
        std::string line;
        while (std::getline(in, line)) raw.push_back(line);

        std::vector<LogLine> lines;
        std::vector<std::string> sessions;
        ParseAudit audit;
        parseLines(raw, lines, sessions, &audit);
        PlatformInfo pi = detectPlatform(lines);
        auto outs = collectOutages(lines);
        auto mets = buildMetrics(lines);
        auto fs   = analyze(lines, outs, mets, pi, audit);

        bool ok = true;
        // 审计自洽:任何时候都必须成立
        size_t sum = audit.parsed + audit.session + audit.blank + audit.continuation + audit.unparsed;
        if (sum != audit.rawTotal) { std::printf("   ✗ 审计不自洽 %zu != %zu\n", sum, audit.rawTotal); ok = false; }
        if (c.plat != PLAT_UNKNOWN && pi.plat != c.plat) {
            std::printf("   ✗ 平台=%s,不符期望\n", pi.name.c_str()); ok = false;
        }
        for (const auto& e : c.expect)
            if (!has(fs, e)) { std::printf("   ✗ 缺应有结论:%s\n", e.c_str()); ok = false; }
        for (const auto& f : c.forbid)
            if (has(fs, f)) { std::printf("   ✗ 假阳性:%s\n", f.c_str()); ok = false; }
        for (const auto& f : fs)
            if (f.ev.empty()) { std::printf("   ✗ 无证据结论:%s\n", f.title.c_str()); ok = false; }

        std::printf("── %-52s %s (平台=%s 结论%zu 未识别%zu)\n", c.file.c_str(),
                    ok ? "✓" : "✗", pi.name.c_str(), fs.size(), audit.unparsed);
        if (!ok) fail++;
    }
    std::printf("\n===== %zu 例:%d 失败,%d 跳过 =====\n", cases().size(), fail, skip);
    std::printf("注:本测试用的是**真代码产出**的日志(证据等级【源码执行实证】),\n"
                "    比 simtest 的手写日志高一档 —— 手写版曾被证明是残缺的。\n");
    return fail ? 1 : 0;
}
