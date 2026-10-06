#pragma once
#include "log_types.h"
#include "text_catalog.h"
#include <iomanip>
#include <locale>
#include <sstream>

namespace dl {
inline RssiObservation observedRssi(const std::vector<MetricRow>& metrics) {
    RssiObservation result;
    for (const auto& row : metrics)
        result.add(row.rssiVal);
    return result;
}

inline RssiObservation observedRssi(const MetricView& metrics) {
    RssiObservation result;
    for (const auto* row : metrics)
        result.add(row->rssiVal);
    return result;
}

inline std::string rssiRangeText(const RssiObservation& value) {
    if (!value.samples)
        return "-";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << value.minimum << " / " << std::fixed << std::setprecision(1) << value.mean() << " / " << value.maximum;
    return out.str();
}

inline std::string rssiSummaryText(const RssiObservation& value) {
    if (!value.samples)
        return UiText8(TextId::rssi_no_observation);
    return std::string(UiText8(TextId::rssi_range)) + ": " + rssiRangeText(value) + " dBm · " +
           UiText8(TextId::rssi_samples) + ": " + std::to_string(value.samples);
}
}  // namespace dl
