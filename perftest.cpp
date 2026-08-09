// perftest.cpp — 大日志性能基准。默认跑 10万/50万/100万行。
//
// 这不是墙钟阈值测试(共享 CI 机器的耗时会波动),而是可重复的数据规模与结果校验:
// 每轮打印解析/筛选/分析耗时,同时钉死行数、筛选数、断网数和审计自洽。
#include "logmodel.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dl;
using Clock = std::chrono::steady_clock;

static long long ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

static std::string makeLine(size_t i) {
    // 默认最大 100 万秒≈12天,固定在 2026-01 内,时间严格单调。
    size_t day = 1 + i / 86400;
    size_t sod = i % 86400;
    size_t h = sod / 3600, m = (sod / 60) % 60, s = sod % 60;
    char ts[32];
    std::snprintf(ts, sizeof(ts), "2026-01-%02zu %02zu:%02zu:%02zu", day, h, m, s);

    std::string line = "[";
    line += ts;
    if (i % 10000 == 0) {
        line += "] [HEARTBEAT] Ping failed, fault timer started";
    } else if (i % 10000 == 30) {
        line += "] [HEARTBEAT] Network recovered after 30s";
    } else if (i % 10 == 0) {
        line += "] [HEARTBEAT] CH:SIM | CSQ:20 | Temp:60 | ConsecFail:0 | RX_PKT:";
        line += std::to_string(i * 7 + 1);
    } else {
        line += "] [TRACE] periodic diagnostic noise ";
        line += std::to_string(i);
    }
    return line;
}

static bool runOne(size_t n) {
    std::vector<std::string> raw;
    raw.reserve(n);
    for (size_t i = 0; i < n; ++i) raw.push_back(makeLine(i));

    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    auto t0 = Clock::now();
    parseLines(raw, lines, sessions, &audit);
    PlatformInfo platform = detectPlatform(lines);
    auto t1 = Clock::now();

    // app 在解析后不再需要 raw；基准也释放它,避免把生成器缓存算进筛选峰值。
    std::vector<std::string>().swap(raw);

    bool grepBad = false;
    std::vector<LogLine> view = applyFilters(lines, "HEARTBEAT", "", "", "", &grepBad);
    auto t2 = Clock::now();

    std::vector<Outage> outages = collectOutages(view);
    auto t3 = Clock::now();
    std::vector<MetricRow> metrics = buildMetrics(view);
    auto t4 = Clock::now();
    std::vector<Finding> findings = analyze(view, outages, metrics, platform, audit);
    auto t5 = Clock::now();

    size_t expectedView = (n + 9) / 10;       // HEARTBEAT 恰好每 10 行一条
    size_t expectedOutages = (n + 9999) / 10000;
    bool ok = lines.size() == n && audit.rawTotal == n && audit.parsed == n &&
              audit.unparsed == 0 && view.size() == expectedView && !grepBad &&
              outages.size() == expectedOutages && !metrics.empty() && !findings.empty();

    std::printf("%8zu 行 | 解析+平台 %6lld ms | 筛选 %6lld ms"
                " | 断网 %4lld ms 指标 %6lld ms 结论 %6lld ms"
                " | 视图 %zu 指标 %zu 断网 %zu | %s\n",
                n, ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), ms(t4, t5),
                view.size(), metrics.size(), outages.size(), ok ? "通过" : "失败");
    return ok;
}

int main(int argc, char** argv) {
    std::vector<size_t> sizes;
    for (int i = 1; i < argc; ++i) {
        char* end = nullptr;
        unsigned long long n = std::strtoull(argv[i], &end, 10);
        if (!end || *end || n == 0) {
            std::fprintf(stderr, "无效行数: %s\n", argv[i]);
            return 2;
        }
        sizes.push_back((size_t)n);
    }
    if (sizes.empty()) sizes = {100000, 500000, 1000000};

    std::puts("== 大日志性能基准 ==");
    bool ok = true;
    for (size_t n : sizes) ok = runOne(n) && ok;
    return ok ? 0 : 1;
}
