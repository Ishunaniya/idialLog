#pragma once
#include "chartmodel.h"
#include "log_types.h"
#include <algorithm>
#include <climits>
#include <utility>

namespace dl {
inline ChartSeries reportedRssiSeries(const MetricView& metrics) {
    ChartSeries result;
    for (const auto* row : metrics)
        if (row->rssiVal < 0) result.emplace_back(row->t,row->rssiVal);
    sortChartSeriesByTime(result);
    return result;
}
// A display axis derived from actual values; no universal RSSI quality thresholds.
inline std::pair<int,int> rssiDisplayBounds(const ChartSeries& series) {
    long long lo=-120, hi=-20;
    for (const auto& point : series) {
        lo=std::min(lo,static_cast<long long>(point.second)-5);
        hi=std::max(hi,static_cast<long long>(point.second)+5);
    }
    return {static_cast<int>(std::max(lo,static_cast<long long>(INT_MIN))),
            static_cast<int>(std::min(hi,static_cast<long long>(INT_MAX)))};
}
}
