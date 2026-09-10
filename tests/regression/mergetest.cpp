// mergetest.cpp — 多文件合并定序(orderByTime / firstTimestamp)断言测试。
//
// ============================ 它能证明什么 ============================
// ✅ 最有牙齿的是 T1:拿**真机日志**(samples/rtms_eg25/dial_20260630_000026.log,2861 行)
//    切成 N 段、打乱顺序喂进去,断言定序后拼回来的结果**与原文逐行相同**。
//    真机日志的时间顺序是设备自己写出来的客观事实,不是我编的参照系。
// ✅ T2..T9 是边界断言(手写串),覆盖真机数据里碰不到的分支:无时间戳、全无时间戳、
//    同秒起头、扫描窗口越界、空输入。
// ⚠️ 手写串只能证明"我按自己的理解实现得对",证明不了真设备只产出这些形态。
//
// 构建运行:make mergetest && build/tests/regression/mergetest        (rc=0 全过,rc=1 有断言失败)
#include "logmodel.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace dl;

static int g_fail = 0;

static void ok(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[通过]" : "[失败]", what);
    if (!cond) g_fail++;
}

// 把 vector<size_t> 印成 "0,2,1" 便于断言时钉死具体顺序
static std::string joinOrd(const std::vector<size_t>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        s += std::to_string(v[i]);
    }
    return s;
}

static std::vector<std::string> lines(std::initializer_list<const char*> ls) {
    std::vector<std::string> v;
    for (const char* l : ls) v.push_back(l);
    return v;
}

// ============================ T1:真机日志切分还原 ============================
static void t1_real_log_roundtrip(const char* path) {
    std::printf("== T1 真机日志切分还原(%s)==\n", path);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("  [失败] 打不开真机夹具 —— 这个测试不能跳过\n");
        g_fail++;
        return;
    }
    std::vector<std::string> orig;
    std::string line;
    while (std::getline(f, line)) orig.push_back(line);
    ok(orig.size() == 2861, "真机夹具行数 == 2861(钉死,防夹具被换掉)");
    if (orig.size() < 100) return;

    // 切成 4 段(模拟按天/按会话分的 4 份文件)
    const size_t N = 4, seg = orig.size() / N;
    std::vector<std::vector<std::string>> parts;
    for (size_t i = 0; i < N; ++i) {
        size_t a = i * seg;
        size_t b = (i == N - 1) ? orig.size() : (i + 1) * seg;
        parts.push_back(std::vector<std::string>(orig.begin() + a, orig.begin() + b));
    }

    // 逆序喂进去(最坏情况:拖入顺序与时间顺序完全相反)
    std::vector<std::vector<std::string>> shuffled = { parts[3], parts[1], parts[2], parts[0] };
    std::vector<size_t> ord = orderByTime(shuffled);
    ok(joinOrd(ord) == "3,1,2,0", ("定序结果 == 3,1,2,0(实得 " + joinOrd(ord) + ")").c_str());

    // 按定序拼回来,应与原文逐行相同
    std::vector<std::string> merged;
    for (size_t i : ord) merged.insert(merged.end(), shuffled[i].begin(), shuffled[i].end());
    bool same = (merged.size() == orig.size());
    size_t firstDiff = 0;
    if (same) {
        for (size_t i = 0; i < merged.size(); ++i) {
            if (merged[i] != orig[i]) { same = false; firstDiff = i + 1; break; }
        }
    }
    ok(same, same ? "拼回结果与原文逐行相同(2861/2861)"
                  : ("拼回结果与原文不同,首个差异在第 " + std::to_string(firstDiff) + " 行").c_str());

    // 真正要保住的性质:解析后时间单调非减
    std::vector<LogLine> ll;
    std::vector<std::string> ss;
    parseLines(merged, ll, ss, nullptr);
    bool mono = true;
    for (size_t i = 1; i < ll.size(); ++i) if (ll[i].t < ll[i-1].t) { mono = false; break; }
    ok(mono, "合并后解析结果时间单调非减");

    // 有牙齿的对照:**错误顺序**必须被抓出来(否则上面的断言等于没测)
    std::vector<std::string> wrong;
    for (const auto& p : shuffled) wrong.insert(wrong.end(), p.begin(), p.end());
    std::vector<LogLine> ll2;
    parseLines(wrong, ll2, ss, nullptr);
    bool mono2 = true;
    for (size_t i = 1; i < ll2.size(); ++i) if (ll2[i].t < ll2[i-1].t) { mono2 = false; break; }
    ok(!mono2, "对照组:不定序直接拼接**确实**产生时间倒流(证明这个 bug 真实存在)");

    // 钉死错误顺序造成的后果(v1.3.0 的实际行为)
    auto oGood = collectOutages(ll);
    auto oBad  = collectOutages(ll2);
    std::printf("  [数据] 断网次数:定序后 %zu 次 / 不定序 %zu 次\n", oGood.size(), oBad.size());
    ok(oGood.size() == 36, ("定序后断网 == 36 次(与 README 记录的真机基线一致,实得 "
                            + std::to_string(oGood.size()) + ")").c_str());
}

// ============================ T2..T9:边界 ============================
static void t2_basic_reorder() {
    std::printf("== T2 乱序三份按时间升序 ==\n");
    std::vector<std::vector<std::string>> c = {
        lines({ "[2026-06-30 12:00:00] [HEARTBEAT] CSQ:20" }),   // 0: 中
        lines({ "[2026-06-30 08:00:00] [HEARTBEAT] CSQ:20" }),   // 1: 早
        lines({ "[2026-06-30 20:00:00] [HEARTBEAT] CSQ:20" }),   // 2: 晚
    };
    std::string got = joinOrd(orderByTime(c));
    ok(got == "1,0,2", ("顺序 == 1,0,2(实得 " + got + ")").c_str());
}

static void t3_session_marker_wins() {
    std::printf("== T3 会话标记的时间戳被采纳(它比首条普通行早)==\n");
    // 标记 00:00:26,首条普通行 00:00:56 —— 若漏认标记,这份会被排到 00:00:30 那份之后
    std::vector<std::vector<std::string>> c = {
        lines({ "[2026-06-30 00:00:30] [HEARTBEAT] CSQ:20" }),
        lines({ "=== Dial Log Opened [2026-06-30 00:00:26] daykey=2026-06-30 ===",
                "[2026-06-30 00:00:56] [HEARTBEAT] CSQ:21" }),
    };
    std::string got = joinOrd(orderByTime(c));
    ok(got == "1,0", ("顺序 == 1,0(实得 " + got + ")").c_str());

    long long t = 0;
    ok(firstTimestamp(c[1], &t) && fmtTime(t, "FULL") == "2026-06-30 00:00:26",
       "取到的是标记里的 00:00:26,不是首条普通行的 00:00:56");
}

static void t4_seas_format() {
    std::printf("== T4 artery(FMT_SEAS)格式也能取到时间戳 ==\n");
    std::vector<std::vector<std::string>> c = {
        lines({ "2026-03-19 10:00:00.100 [INFO] \x1b[0m dial_task (dial.c:245) - CSQ: 20" }),
        lines({ "2026-03-19 02:23:09.899 [INFO] \x1b[0m dial_task (dial.c:245) - CSQ: 31" }),
    };
    std::string got = joinOrd(orderByTime(c));
    ok(got == "1,0", ("顺序 == 1,0(实得 " + got + ")").c_str());
}

static void t5_no_timestamp_goes_last() {
    std::printf("== T5 扫不到时间戳的排最后 ==\n");
    std::vector<std::vector<std::string>> c = {
        lines({ "this is not a dial log at all", "neither is this" }),   // 0: 无
        lines({ "[2026-06-30 12:00:00] [HEARTBEAT] CSQ:20" }),           // 1: 有
        lines({ "[2026-06-30 08:00:00] [HEARTBEAT] CSQ:20" }),           // 2: 有,更早
    };
    std::string got = joinOrd(orderByTime(c));
    ok(got == "2,1,0", ("顺序 == 2,1,0(实得 " + got + ")").c_str());
    ok(!firstTimestamp(c[0], nullptr), "无时间戳的 chunk firstTimestamp 返回 false");
}

static void t6_all_no_timestamp_keeps_order() {
    std::printf("== T6 全都扫不到时间戳 → 保持输入顺序 ==\n");
    std::vector<std::vector<std::string>> c = { lines({ "aaa" }), lines({ "bbb" }), lines({ "ccc" }) };
    std::string got = joinOrd(orderByTime(c));
    ok(got == "0,1,2", ("顺序 == 0,1,2(实得 " + got + ")").c_str());
}

static void t7_same_timestamp_is_stable() {
    std::printf("== T7 同一首时间戳 → 保持输入顺序(stable)==\n");
    // 【变异测试实证】此处必须用 **>16 份**。libstdc++ 的 std::sort 是 introsort:
    // 元素数 <= _S_threshold(16) 时直接退化成插入排序,而插入排序恰好是稳定的 ——
    // 于是把 stable_sort 改成 sort 的变异在 3 份输入下"碰巧"通过,变异存活、断言没牙齿。
    // 32 份会真正走到 quicksort 的 partition,相等元素的相对次序才会被打乱。
    const size_t N = 32;
    std::vector<std::vector<std::string>> c;
    std::vector<std::string> want;
    for (size_t i = 0; i < N; ++i) {
        // 时间戳全同,靠正文区分身份
        c.push_back(lines({ "[2026-06-30 08:00:00] [HEARTBEAT] chunk" }));
        c.back()[0] += std::to_string(i);
        want.push_back(std::to_string(i));
    }
    std::string expect;
    for (size_t i = 0; i < N; ++i) { if (i) expect += ','; expect += want[i]; }
    std::string got = joinOrd(orderByTime(c));
    ok(got == expect, ("32 份同首时间戳 → 顺序原封不动 0..31(实得 " + got + ")").c_str());
}

static void t8_empty_inputs() {
    std::printf("== T8 空输入 / 空 chunk 不崩 ==\n");
    std::vector<std::vector<std::string>> none;
    ok(orderByTime(none).empty(), "空输入 → 空输出");
    std::vector<std::vector<std::string>> c = {
        lines({ "[2026-06-30 08:00:00] [HEARTBEAT] A" }),
        {},                                   // 空 chunk
    };
    std::string got = joinOrd(orderByTime(c));
    ok(got == "0,1", ("空 chunk 视为无时间戳排最后,顺序 == 0,1(实得 " + got + ")").c_str());
}

static void t9_scan_limit() {
    std::printf("== T9 扫描窗口边界(scanLimit)==\n");
    std::vector<std::string> v;
    for (int i = 0; i < 250; ++i) v.push_back("noise line without any timestamp");
    v.push_back("[2026-06-30 08:00:00] [HEARTBEAT] CSQ:20");   // 第 251 行才有时间戳
    ok(!firstTimestamp(v, nullptr, 200), "默认只扫 200 行 → 第 251 行的时间戳扫不到(设计如此)");
    ok(firstTimestamp(v, nullptr, 300), "放宽到 300 行 → 能扫到");
}

// ============================ T10:时钟未同步(1970)真机日志 ============================
// samples/rtms_eg25/real_eg25_1.31.15_unsynced.log 首行:
//   "=== Dial Log Opened [1970-01-01 15:10:16] daykey=unsynced ==="
// 这是设备开机 RTC 未同步时的真实产物。先把**实际行为**测出来,不预判对错。
static void t10_unsynced_observed(const char* unsyncedPath, const char* normalPath) {
    std::printf("== T10 时钟未同步日志(1970)与正常日志混合 —— 观察实际行为 ==\n");
    auto readAll = [](const char* p) {
        std::vector<std::string> v;
        std::ifstream f(p, std::ios::binary);
        std::string l;
        while (std::getline(f, l)) v.push_back(l);
        return v;
    };
    std::vector<std::string> uns = readAll(unsyncedPath), nor = readAll(normalPath);
    if (uns.empty() || nor.empty()) { std::printf("  [失败] 夹具读不到\n"); g_fail++; return; }

    long long tu = 0, tn = 0;
    firstTimestamp(uns, &tu);
    firstTimestamp(nor, &tn);
    std::printf("  [数据] unsynced 首时间戳 = %s\n", fmtTime(tu, "FULL").c_str());
    std::printf("  [数据] 正常日志首时间戳 = %s\n", fmtTime(tn, "FULL").c_str());

    std::vector<std::vector<std::string>> c = { nor, uns };   // 正常在前拖入
    std::vector<size_t> ord = orderByTime(c);
    std::printf("  [数据] 定序结果 = %s(0=正常日志, 1=unsynced)\n", joinOrd(ord).c_str());

    std::vector<std::string> merged;
    for (size_t i : ord) merged.insert(merged.end(), c[i].begin(), c[i].end());
    std::vector<LogLine> ll;
    std::vector<std::string> ss;
    parseLines(merged, ll, ss, nullptr);
    if (ll.size() < 2) { std::printf("  [失败] 合并后解析不出行\n"); g_fail++; return; }
    long long span = ll.back().t - ll.front().t;
    std::printf("  [数据] 合并后时间跨度 = %s (%lld 秒)\n", fmtDur(span).c_str(), span);
    std::printf("  [数据] 跨度起止 = %s -> %s\n",
                fmtTime(ll.front().t, "FULL").c_str(), fmtTime(ll.back().t, "FULL").c_str());
}

// ============================ T11:跨时基混合防护(方案A)============================
// 用**两份真机夹具**:未同步(1970)+ 正常墙钟。断言 detectMix 认出混合、
// 把未同步批挑出来排除、只留墙钟批;且排除后墙钟批单独算出的可用率 == 真值。
static void t11_mix_detection(const char* unsyncedPath, const char* wallPath) {
    std::printf("== T11 跨时基混合防护(真机 unsynced + 墙钟)==\n");
    auto readAll = [](const char* p) {
        std::vector<std::string> v;
        std::ifstream f(p, std::ios::binary);
        std::string l;
        while (std::getline(f, l)) v.push_back(l);
        return v;
    };
    std::vector<std::string> uns = readAll(unsyncedPath), wall = readAll(wallPath);
    if (uns.empty() || wall.empty()) { std::printf("  [失败] 夹具读不到\n"); g_fail++; return; }

    // 单份时基判定
    ok(timeBaseOf(uns) == TB_UNSYNCED, "未同步日志(1970)判为 TB_UNSYNCED");
    ok(timeBaseOf(wall) == TB_WALL, "墙钟日志(2026)判为 TB_WALL");
    ok(timeBaseOf({ std::string("garbage line"), std::string("no timestamp") }) == TB_NONE,
       "无时间戳判为 TB_NONE");

    // 混合检测:顺序无关 —— 墙钟在前 / 未同步在前都应认出混合并正确归类
    std::vector<std::vector<std::string>> c1 = { wall, uns };   // 墙钟(0) 未同步(1)
    MixReport m1 = detectMix(c1);
    ok(m1.mixed, "墙钟+未同步 → mixed=true");
    ok(m1.wallIdx.size() == 1 && m1.wallIdx[0] == 0, "墙钟批下标 == {0}");
    ok(m1.unsyncedIdx.size() == 1 && m1.unsyncedIdx[0] == 1, "未同步批下标 == {1}");

    std::vector<std::vector<std::string>> c2 = { uns, wall };   // 未同步(0) 墙钟(1)
    MixReport m2 = detectMix(c2);
    ok(m2.mixed && m2.wallIdx.size() == 1 && m2.wallIdx[0] == 1 && m2.unsyncedIdx[0] == 0,
       "顺序颠倒仍正确归类(墙钟=1 未同步=0)");

    // 非混合:两份都是墙钟 → mixed=false(不能误伤)
    std::vector<std::vector<std::string>> c3 = { wall, wall };
    ok(!detectMix(c3).mixed, "两份墙钟 → mixed=false(不误伤)");

    // 非混合:只有未同步 → mixed=false(纯未同步单独分析是合理的)
    std::vector<std::vector<std::string>> c4 = { uns };
    ok(!detectMix(c4).mixed, "纯未同步 → mixed=false(允许单独分析)");

    // 有牙齿的核心断言:排除未同步批后,墙钟批单独算的可用率 == 只有墙钟时的真值。
    // 若防护失效(未同步没被排除),下面的可用率会被 56 年跨度冲成 99.99%。
    auto avail = [](const std::vector<std::string>& raw) -> double {
        std::vector<LogLine> ll; std::vector<std::string> ss;
        parseLines(raw, ll, ss, nullptr);
        if (ll.size() < 2) return -1;
        auto o = collectOutages(ll);
        long long tot = 0; for (auto& x : o) if (x.recovered) tot += x.dur;
        double span = (double)(ll.back().t - ll.front().t);
        return span > 0 ? 100.0 * (1.0 - tot / span) : -1;
    };
    double truth = avail(wall);
    // 模拟 LoadFiles 方案A:mixed → 只保留 wallIdx 那批,定序后拼接
    MixReport m = detectMix(c1);
    std::vector<std::vector<std::string>> kept;
    for (size_t i : m.wallIdx) kept.push_back(c1[i]);
    std::vector<size_t> ord = orderByTime(kept);
    std::vector<std::string> merged;
    for (size_t i : ord) merged.insert(merged.end(), kept[i].begin(), kept[i].end());
    double got = avail(merged);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "排除未同步后可用率 %.4f%% == 真值 %.4f%%", got, truth);
    ok(got >= 0 && truth >= 0 && std::abs(got - truth) < 0.001, buf);

    // 对照:若**不**排除(直接混合),可用率必被冲高 —— 证明这个防护不是空测
    std::vector<size_t> ordBad = orderByTime(c1);
    std::vector<std::string> bad;
    for (size_t i : ordBad) bad.insert(bad.end(), c1[i].begin(), c1[i].end());
    double gotBad = avail(bad);
    std::snprintf(buf, sizeof(buf), "对照:不排除则可用率冲高到 %.4f%%(证明 bug 真实)", gotBad);
    ok(gotBad > truth + 1.0, buf);
}

// ============================ T12:旧拨号日志跨文件主事故 ============================
// 这是 2026-09 客诉的最小化、可重复场景：故障开始于第 1 份日志，L3 后进程
// 重启，次日第 2 份日志才恢复。旧实现按 sourceId 截断，漏掉整段主事故；同时
// [HEARTBEAT-NET] 的连字符此前不被解析成标签，无法作为数据面证据。
static bool hasFinding(const std::vector<Finding>& findings, const std::string& needle) {
    for (const auto& f : findings)
        if (f.title.find(needle) != std::string::npos || f.detail.find(needle) != std::string::npos)
            return true;
    return false;
}

static void t12_legacy_cross_file_outage() {
    std::printf("== T12 旧拨号日志跨文件主事故与 HEARTBEAT-NET ==\n");
    std::vector<std::string> raw = {
        "[2026-09-09 12:27:45] [HEARTBEAT-NET] IF=ccinet1 | WINDOW=60s | TX_PKT=120(+10) RX_PKT=100(+8) | TX_IDLE=0s RX_IDLE=0s",
        "[2026-09-09 12:28:16] [HEARTBEAT-NET] IF=ccinet1 | WINDOW=60s | TX_PKT=130(+10) RX_PKT=100(+0) | TX_IDLE=0s RX_IDLE=10s",
        "[2026-09-09 12:28:16] [WARNING] Interface has no RX data for 10s: IF=ccinet1 TX_PKT_SINCE_RX=10",
        "[2026-09-09 12:58:46] [RECOVERY L3] FATAL: Network down 1830 secs (threshold 1830s). Exiting for start_prog to reinitialize.",
        "[2026-09-09 13:02:48] Program started. Main Version: 1.28.13",
        "[2026-09-09 13:08:29] [INFO] never-connected, policy recovery (L1/L2/L3) gated.",
        "[2026-09-09 13:08:32] [HEARTBEAT-NET] IF=(none) | DATA:N/A",
        "[2026-09-10 03:49:34] [HEARTBEAT-NET] IF=(none) | DATA:N/A",
        "[2026-09-10 05:39:04] Program started. Main Version: 1.28.13",
        "[2026-09-10 05:39:05] [HEARTBEAT-NET] IF=ccinet1 | WINDOW=60s | TX_PKT=1(+1) RX_PKT=1(+1) | TX_IDLE=0s RX_IDLE=0s",
        "[2026-09-10 05:39:06] [EVENT] Network Connected. Notification sent.",
    };
    // 第二个文件从下标 8 开始；必须保留真实文件边界以复现 sourceId 切换。
    std::vector<LogLine> parsed; std::vector<std::string> sessions; ParseAudit audit;
    parseLines(raw, parsed, sessions, &audit, {0, 8});
    ok(audit.unparsed == 0 && parsed.size() == raw.size(), "HEARTBEAT-NET 全部按结构化日志解析");
    ok(parsed.size() > 1 && parsed[1].tagText() == "HEARTBEAT-NET", "连字符标签被保留为 HEARTBEAT-NET");

    auto outages = collectOutages(parsed);
    ok(outages.size() == 1, ("跨文件主事故合并为 1 段(实得 " + std::to_string(outages.size()) + ")").c_str());
    if (outages.size() == 1) {
        ok(outages[0].recovered, "主事故由次日接口恢复/联网通知关闭");
        ok(outages[0].dur == 61849,
           ("主事故时长 == 17h10m49s(实得 " + fmtDur(outages[0].dur) + ")").c_str());
    }

    auto findings = analyze(parsed, outages, buildMetrics(parsed), detectPlatform(parsed), audit);
    ok(hasFinding(findings, "设备级主事故: 数据业务中断 17h10m49s"), "输出设备级主事故，而非历史统计替代结论");
    ok(hasFinding(findings, "进程重启后丢失既有联网上下文"), "never-connected 不再误报为设备从未联网");
    ok(hasFinding(findings, "数据接口不可用"), "IF=(none) 输出为数据面证据");
    ok(hasFinding(findings, "30m30s"), "L3 使用日志实报 threshold=1830s，不套用平台默认值");
}

int main(int argc, char** argv) {
    const char* real = (argc > 1) ? argv[1] : "samples/rtms_eg25/dial_20260630_000026.log";
    const char* uns  = (argc > 2) ? argv[2] : "samples/rtms_eg25/real_eg25_1.31.15_unsynced.log";

    t1_real_log_roundtrip(real);
    t2_basic_reorder();
    t3_session_marker_wins();
    t4_seas_format();
    t5_no_timestamp_goes_last();
    t6_all_no_timestamp_keeps_order();
    t7_same_timestamp_is_stable();
    t8_empty_inputs();
    t9_scan_limit();
    t10_unsynced_observed(uns, real);
    t11_mix_detection(uns, real);
    t12_legacy_cross_file_outage();

    std::printf("\n%s 失败 %d 项\n", g_fail ? "**" : "==", g_fail);
    return g_fail ? 1 : 0;
}
