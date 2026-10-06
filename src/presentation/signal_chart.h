#pragma once
#include "chartmodel.h"
#include "log_types.h"
#include "signal_quality.h"
#include <algorithm>
#include <climits>
#include <utility>

namespace dl {
struct SignalChartSeries {
    ChartSeries csq, rsrp, rsrq, snr10, rssi;
};

// Both the screen and the report consume the current metric view. Unknown CSQ
// and non-LTE engineering metrics must not become valid plotted samples.
inline SignalChartSeries reportedSignalSeries(const MetricView& metrics) {
    SignalChartSeries result;
    for (const auto* row : metrics) {
        if (usesLteEngineeringReference(row->rat)) {
            if (row->csqVal >= 0 && row->csqVal <= 31)
                result.csq.emplace_back(row->t, row->csqVal);
            if (row->rsrp < 0)
                result.rsrp.emplace_back(row->t, row->rsrp);
            if (row->rsrq < 0)
                result.rsrq.emplace_back(row->t, row->rsrq);
            if (row->snr10 != 100000)
                result.snr10.emplace_back(row->t, row->snr10);
        }
        if (row->rssiVal < 0)
            result.rssi.emplace_back(row->t, row->rssiVal);
    }
    for (auto* series : {&result.csq, &result.rsrp, &result.rsrq, &result.snr10, &result.rssi})
        sortChartSeriesByTime(*series);
    return result;
}

inline ChartSeries reportedRssiSeries(const MetricView& metrics) {
    ChartSeries result;
    for (const auto* row : metrics)
        if (row->rssiVal < 0)
            result.emplace_back(row->t, row->rssiVal);
    sortChartSeriesByTime(result);
    return result;
}

// A display axis derived from actual values; no universal RSSI quality thresholds.
inline std::pair<int, int> rssiDisplayBounds(const ChartSeries& series) {
    long long lo = -120, hi = -20;
    for (const auto& point : series) {
        lo = std::min(lo, static_cast<long long>(point.second) - 5);
        hi = std::max(hi, static_cast<long long>(point.second) + 5);
    }
    return {static_cast<int>(std::max(lo, static_cast<long long>(INT_MIN))),
            static_cast<int>(std::min(hi, static_cast<long long>(INT_MAX)))};
}
}  // namespace dl
