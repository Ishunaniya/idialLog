// charttest.cpp — 图表时间排序、峰谷降采样与二分悬停回归。
#include "chartmodel.h"

#include <algorithm>
#include <climits>
#include <cstdio>

using namespace dl;

static int g_fail = 0;
static void ok(bool condition, const char* what) {
    std::printf("  %s %s\n", condition ? "[通过]" : "[失败]", what);
    if (!condition) ++g_fail;
}

static bool contains(const ChartSeries& series, ChartPoint point) {
    return std::find(series.begin(), series.end(), point) != series.end();
}

int main() {
    std::puts("== T1 时间排序与二分悬停 ==");
    ChartSeries unordered{{30, 3}, {20, 2}, {10, 1}, {20, 4}};
    sortChartSeriesByTime(unordered);
    ok(unordered == ChartSeries({{10, 1}, {20, 2}, {20, 4}, {30, 3}}),
       "稳定排序且相同时间戳保持原顺序");
    long long distance = -1;
    const ChartPoint* p = nearestChartPoint(unordered, 20, &distance);
    ok(p && *p == ChartPoint(20, 2) && distance == 0,
       "精确命中同秒多点时仍取第一项");
    p = nearestChartPoint(unordered, 25, &distance);
    ok(p && *p == ChartPoint(20, 2) && distance == 5,
       "等距时取较早时间并保持旧线性扫描语义");
    p = nearestChartPoint(unordered, 29, &distance);
    ok(p && *p == ChartPoint(30, 3) && distance == 1, "靠近右侧时返回后一采样");
    ChartSeries empty;
    ok(!nearestChartPoint(empty, 0, &distance) && distance == LLONG_MAX,
       "空序列安全返回且距离为最大值");

    std::puts("== T2 像素桶保留端点与峰谷 ==");
    ChartSeries dense;
    for (int i = 0; i < 400; ++i) dense.push_back({i, 100});
    for (int bucket = 0; bucket < 10; ++bucket) {
        dense[(size_t)bucket * 40 + 5].second = -1000 - bucket;
        dense[(size_t)bucket * 40 + 10].second = 1000 + bucket;
    }
    ChartSeries sampled;
    downsampleChartSeries(dense, 0, 399, 10, sampled);
    ok(sampled.size() <= 22, "10 像素输出不超过 2*10+2 个点");
    ok(!sampled.empty() && sampled.front() == dense.front() && sampled.back() == dense.back(),
       "全局首尾点不丢失");
    bool extrema = true;
    for (int bucket = 0; bucket < 10; ++bucket) {
        extrema = extrema && contains(sampled, dense[(size_t)bucket * 40 + 5]) &&
                  contains(sampled, dense[(size_t)bucket * 40 + 10]);
    }
    ok(extrema, "每个像素桶的峰值和谷值均保留");
    ok(std::is_sorted(sampled.begin(), sampled.end(),
                      [](const ChartPoint& a, const ChartPoint& b) { return a.first < b.first; }),
       "降采样结果仍按时间排列");
    ChartSeries small{{1, 1}, {2, 2}, {3, 3}}, smallOut;
    downsampleChartSeries(small, 1, 3, 10, smallOut);
    ok(smallOut == small, "数据量已低于像素预算时完全保真");
    downsampleChartSeries(small, 1, 3, 0, smallOut);
    ok(smallOut.empty(), "零宽度图表不产生绘制点");

    std::puts("== T3 百万点规模与退化时间范围 ==");
    ChartSeries million;
    million.reserve(1000000);
    for (int i = 0; i < 1000000; ++i) million.push_back({i * 2LL, i % 31});
    million[123456].second = -500;
    million[654321].second = 500;
    downsampleChartSeries(million, million.front().first, million.back().first, 1920, sampled);
    ok(sampled.size() <= 3842, "百万点在 1920 像素下压到最多 3842 点");
    ok(contains(sampled, million[123456]) && contains(sampled, million[654321]),
       "百万点中的极端尖峰仍被保留");
    p = nearestChartPoint(million, 1000001, &distance);
    ok(p && p->first == 1000000 && distance == 1, "百万点悬停二分查询结果正确");

    ChartSeries sameTime;
    sameTime.reserve(10000);
    for (int i = 0; i < 10000; ++i) sameTime.push_back({42, i % 101 - 50});
    downsampleChartSeries(sameTime, 42, 42, 1920, sampled);
    auto mm = std::minmax_element(sameTime.begin(), sameTime.end(),
        [](const ChartPoint& a, const ChartPoint& b) { return a.second < b.second; });
    ok(sampled.size() <= 4 && contains(sampled, *mm.first) && contains(sampled, *mm.second),
       "时间范围退化为同一秒时仍压缩并保留极值");

    std::printf("\n%s 失败 %d 项\n", g_fail ? "**" : "==", g_fail);
    return g_fail ? 1 : 0;
}
