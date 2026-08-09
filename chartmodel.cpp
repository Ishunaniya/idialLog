// chartmodel.cpp — 纯标准 C++ 图表模型，Win32 绘制层只消费压缩后的点。
#include "chartmodel.h"

#include <algorithm>
#include <climits>
#include <iterator>
#include <limits>

namespace dl {

static bool timeLess(const ChartPoint& a, const ChartPoint& b) {
    return a.first < b.first;
}

void sortChartSeriesByTime(ChartSeries& series) {
    if (!std::is_sorted(series.begin(), series.end(), timeLess))
        std::stable_sort(series.begin(), series.end(), timeLess);
}

static unsigned long long timeDistance(long long a, long long b) {
    // 转无符号后相减可覆盖 LLONG_MIN..LLONG_MAX 的完整距离，不触发有符号溢出。
    return a >= b ? (unsigned long long)a - (unsigned long long)b
                  : (unsigned long long)b - (unsigned long long)a;
}

static long long publicDistance(unsigned long long d) {
    return d > (unsigned long long)LLONG_MAX ? LLONG_MAX : (long long)d;
}

const ChartPoint* nearestChartPoint(const ChartSeries& series, long long target,
                                    long long* distance) {
    if (series.empty()) {
        if (distance) *distance = LLONG_MAX;
        return nullptr;
    }

    auto right = std::lower_bound(series.begin(), series.end(), target,
        [](const ChartPoint& p, long long t) { return p.first < t; });
    auto chosen = right;
    if (right == series.end()) {
        chosen = std::prev(series.end());
    } else if (right != series.begin()) {
        auto left = std::prev(right);
        if (timeDistance(left->first, target) <= timeDistance(right->first, target))
            chosen = left;
    }

    // prev(lower_bound) 可能落在同时间戳的最后一项；旧线性扫描会保留第一项。
    chosen = std::lower_bound(series.begin(), std::next(chosen), chosen->first,
        [](const ChartPoint& p, long long t) { return p.first < t; });
    if (distance) *distance = publicDistance(timeDistance(chosen->first, target));
    return &*chosen;
}

void downsampleChartSeries(const ChartSeries& input,
                           long long rangeStart, long long rangeEnd,
                           size_t pixelWidth, ChartSeries& output) {
    output.clear();
    if (input.empty() || pixelWidth == 0) return;

    const size_t maxSize = std::numeric_limits<size_t>::max();
    const size_t limit = pixelWidth > (maxSize - 2) / 2
        ? maxSize : pixelWidth * 2 + 2;
    if (input.size() <= limit) {
        output = input;
        return;
    }
    output.reserve(std::min(input.size(), limit));

    auto bucketOf = [&](long long t) -> size_t {
        if (rangeEnd <= rangeStart || t <= rangeStart) return 0;
        if (t >= rangeEnd) return pixelWidth - 1;
        const long double pos = ((long double)t - (long double)rangeStart) /
                                ((long double)rangeEnd - (long double)rangeStart);
        size_t bucket = (size_t)(pos * (long double)pixelWidth);
        return std::min(bucket, pixelWidth - 1);
    };

    size_t lastIndex = maxSize;
    auto append = [&](size_t index) {
        if (index != lastIndex) {
            output.push_back(input[index]);
            lastIndex = index;
        }
    };
    auto flush = [&](size_t minIndex, size_t maxIndex) {
        if (minIndex <= maxIndex) {
            append(minIndex);
            append(maxIndex);
        } else {
            append(maxIndex);
            append(minIndex);
        }
    };

    append(0);
    size_t bucket = bucketOf(input[0].first);
    size_t minIndex = 0, maxIndex = 0;
    for (size_t i = 1; i < input.size(); ++i) {
        const size_t nextBucket = bucketOf(input[i].first);
        if (nextBucket != bucket) {
            flush(minIndex, maxIndex);
            bucket = nextBucket;
            minIndex = maxIndex = i;
            continue;
        }
        if (input[i].second < input[minIndex].second) minIndex = i;
        if (input[i].second > input[maxIndex].second) maxIndex = i;
    }
    flush(minIndex, maxIndex);
    append(input.size() - 1);
}

} // namespace dl
