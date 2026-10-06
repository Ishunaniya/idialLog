// charttest.cpp — 图表时间排序、峰谷降采样与二分悬停回归。
#include "chartmodel.h"
#include "signal_chart.h"

#include <algorithm>
#include <climits>
#include <cstdio>

using namespace dl;

static int g_fail = 0;

static void ok(bool condition, const char* what) {
    std::printf("  %s %s\n", condition ? "[通过]" : "[失败]", what);
    if (!condition)
        ++g_fail;
}

static bool contains(const ChartSeries& series, ChartPoint point) {
    return std::find(series.begin(), series.end(), point) != series.end();
}

int main() {
    std::puts("== 原始 RSSI 信号图 ==");
    MetricRow a, b, c;
    a.t = 30;
    a.csqVal = 18;
    b.t = 10;
    b.rssiVal = -100;
    c.t = 20;
    c.rssiVal = -65;
    const auto rssi = reportedRssiSeries({&a, &b, &c});
    ok(rssi == ChartSeries({{10, -100}, {20, -65}}), "RSSI 使用上报值与原时间排序，CSQ 单独存在不产生 RSSI");
    ok(rssiDisplayBounds(rssi) == std::pair<int, int>{-120, -20}, "RSSI 默认显示轴覆盖真实样本，不混入质量阈值");
    ok(rssiDisplayBounds({{10, -155}, {20, -10}}) == std::pair<int, int>{-160, -5},
       "RSSI 超出默认范围时扩展坐标轴，不裁掉极值");
    ok(rssiDisplayBounds({{10, INT_MIN}}).first == INT_MIN, "极端负 RSSI 轴边界不会溢出");
    std::puts("== 选区时间映射 ==");
    ok(chartTimeAtPixel(100, 200, 50, 150, 75) == 125 && chartTimeAtPixel(100, 200, 50, 150, 125) == 175,
       "选区端点按可见轴精确映射，支持反向拖动");
    ok(chartTimeAtPixel(86340, 86580, 0, 240, 120) == 86460, "跨午夜选区保留日期，不退回第一天的时分秒");
    ok(chartTimeAtPixel(100, 200, 50, 150, -1) == 100 && chartTimeAtPixel(100, 200, 50, 150, 200) == 200,
       "拖出图表时钳制到轴端点");
    ok(chartTimeAtPixel(42, 42, 0, 100, 50) == 42 && chartTimeAtPixel(42, 100, 50, 50, 50) == 42,
       "单秒与零宽度安全退化");
    ok(chartTimeAtPixel(LLONG_MIN, LLONG_MAX, 0, 100, 0) == LLONG_MIN &&
           chartTimeAtPixel(LLONG_MIN, LLONG_MAX, 0, 100, 100) == LLONG_MAX &&
           chartTimeAtPixel(LLONG_MIN, LLONG_MAX, 0, 100, 50) == 0,
       "极端时间区间不溢出");
    std::puts("== T0 自适应时间轴 ==");
    ok(chartTimeTicks(100, 175, 600, 120) == std::vector<long long>({100, 120, 135, 150, 175}),
       "75 秒日志具有秒级刻度并保留起止时间");
    ok(chartTimeTicks(86370, 86550, 600, 120) == std::vector<long long>({86370, 86460, 86550}),
       "跨午夜的三分钟日志刻度精确且不会拥挤");
    ok(chartTimeTicks(0, 259200, 600, 120) == std::vector<long long>({0, 86400, 172800, 259200}),
       "多日日志按天标注，不受旧 24 小时步长上限限制");
    ok(chartTimeTicks(42, 42, 600, 120) == std::vector<long long>({42}), "单秒采样保留唯一时间");
    ok(chartTimeTicks(100, 175, 80, 120) == std::vector<long long>({100, 175}), "窄图仅保留首尾");
    ok(chartTimeTicks(100, 175, 0, 120).empty() && chartTimeTicks(175, 100, 600, 120).empty(),
       "无空间和倒置范围不生成无效刻度");
    auto extremes = chartTimeTicks(LLONG_MIN, LLONG_MAX, 600, 120);
    ok(!extremes.empty() && extremes.front() == LLONG_MIN && extremes.back() == LLONG_MAX && extremes.size() <= 7 &&
           std::is_sorted(extremes.begin(), extremes.end()),
       "极端时间范围不溢出或无限循环");
    ok(chartSampleGaps({{0, 1}, {600, 2}, {1201, 3}, {1201, 4}, {1801, 5}}, 600) ==
           std::vector<ChartGap>({{600, 1201}}),
       "仅超过 10 分钟的真实采样空缺断线，边界/重复时间不误断");
    ok(chartSampleGaps({{LLONG_MIN, 1}, {LLONG_MAX, 2}}, 600) == std::vector<ChartGap>({{LLONG_MIN, LLONG_MAX}}),
       "跨时基极大空缺仍可识别，不发生溢出");
    std::puts("== T1 时间排序与二分悬停 ==");
    ChartSeries unordered{{30, 3}, {20, 2}, {10, 1}, {20, 4}};
    sortChartSeriesByTime(unordered);
    ok(unordered == ChartSeries({{10, 1}, {20, 2}, {20, 4}, {30, 3}}), "稳定排序且相同时间戳保持原顺序");
    long long distance = -1;
    const ChartPoint* p = nearestChartPoint(unordered, 20, &distance);
    ok(p && *p == ChartPoint(20, 2) && distance == 0, "精确命中同秒多点时仍取第一项");
    p = nearestChartPoint(unordered, 25, &distance);
    ok(p && *p == ChartPoint(20, 2) && distance == 5, "等距时取较早时间并保持旧线性扫描语义");
    p = nearestChartPoint(unordered, 29, &distance);
    ok(p && *p == ChartPoint(30, 3) && distance == 1, "靠近右侧时返回后一采样");
    ChartSeries empty;
    ok(!nearestChartPoint(empty, 0, &distance) && distance == LLONG_MAX, "空序列安全返回且距离为最大值");

    std::puts("== T2 像素桶保留端点与峰谷 ==");
    ChartSeries dense;
    for (int i = 0; i < 400; ++i)
        dense.push_back({i, 100});
    for (int bucket = 0; bucket < 10; ++bucket) {
        dense[(size_t)bucket * 40 + 5].second = -1000 - bucket;
        dense[(size_t)bucket * 40 + 10].second = 1000 + bucket;
    }
    ChartSeries sampled;
    downsampleChartSeries(dense, 0, 399, 10, sampled);
    ok(sampled.size() <= 22, "10 像素输出不超过 2*10+2 个点");
    ok(!sampled.empty() && sampled.front() == dense.front() && sampled.back() == dense.back(), "全局首尾点不丢失");
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
    for (int i = 0; i < 1000000; ++i)
        million.push_back({i * 2LL, i % 31});
    million[123456].second = -500;
    million[654321].second = 500;
    downsampleChartSeries(million, million.front().first, million.back().first, 1920, sampled);
    ok(sampled.size() <= 3842, "百万点在 1920 像素下压到最多 3842 点");
    ok(contains(sampled, million[123456]) && contains(sampled, million[654321]), "百万点中的极端尖峰仍被保留");
    p = nearestChartPoint(million, 1000001, &distance);
    ok(p && p->first == 1000000 && distance == 1, "百万点悬停二分查询结果正确");

    ChartSeries sameTime;
    sameTime.reserve(10000);
    for (int i = 0; i < 10000; ++i)
        sameTime.push_back({42, i % 101 - 50});
    downsampleChartSeries(sameTime, 42, 42, 1920, sampled);
    auto mm = std::minmax_element(sameTime.begin(), sameTime.end(),
                                  [](const ChartPoint& a, const ChartPoint& b) { return a.second < b.second; });
    ok(sampled.size() <= 4 && contains(sampled, *mm.first) && contains(sampled, *mm.second),
       "时间范围退化为同一秒时仍压缩并保留极值");

    std::printf("\n%s 失败 %d 项\n", g_fail ? "**" : "==", g_fail);
    return g_fail ? 1 : 0;
}
