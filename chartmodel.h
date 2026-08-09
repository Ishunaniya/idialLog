// chartmodel.h — 大数据图表的时间排序、像素降采样与最近点查询。
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace dl {

using ChartPoint = std::pair<long long, int>; // 时间戳秒,指标原值
using ChartSeries = std::vector<ChartPoint>;

// 按时间稳定排序；相同时间戳保持原始先后，悬停语义与旧线性扫描一致。
void sortChartSeriesByTime(ChartSeries& series);

// 把已按时间排序的序列压到屏幕像素桶。每桶保留最小/最大值且按原时间顺序输出，
// 另保留全局首尾点，所以输出最多 2*pixelWidth+2 个点，不会吃掉瞬时峰谷。
void downsampleChartSeries(const ChartSeries& input,
                           long long rangeStart, long long rangeEnd,
                           size_t pixelWidth, ChartSeries& output);

// 已按时间排序序列上的最近点二分查询。等距时取较早时间；同一时间有多点时
// 取最先出现者，与旧版从头线性扫描、只在距离严格变小时更新的行为一致。
const ChartPoint* nearestChartPoint(const ChartSeries& series, long long target,
                                    long long* distance = nullptr);

} // namespace dl
