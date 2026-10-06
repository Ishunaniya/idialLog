#pragma once
#include <map>
#include <string>
#include "log_types.h"

namespace dl {
struct DeviceMetadata {
    std::string device, firmware, configuration;
};
enum class ReviewStatus { Pending, Confirmed, Handled };

struct ReviewRecord {
    std::string key, title, note;
    ReviewStatus status = ReviewStatus::Pending;
};

struct WorkspaceState {
    std::map<std::string, DeviceMetadata>
        devices;  // Stable source identity (content hash + source label/occurrence) -> manual device.
    std::map<std::string, ReviewRecord> reviews;
};

std::string encodeWorkspace(const WorkspaceState& state);
WorkspaceState decodeWorkspace(const std::string& data);
std::string reviewStatusText(ReviewStatus status);
std::string reviewRecordText(const ReviewRecord& record);

struct SignalDistribution {
    std::size_t samples = 0;
    double mean = 0;
    int minimum = 0, p10 = 0, p50 = 0, p90 = 0, maximum = 0;
};

struct PeriodStats {
    std::size_t lines = 0, samples = 0, outages = 0, open = 0;
    long long first = 0, last = 0, observed = 0, down = 0;
    AvailabilityStats availability;
    SignalDistribution csq, rsrp, rsrq, snr, rssi;
    std::map<std::string, std::size_t> recoveryActions;
};

PeriodStats comparePeriod(LogView rows, long long start, long long end);
// Metrics must be built from the complete scope before clipping; borrowed pointers remain owned by caller.
PeriodStats comparePeriodCached(LogView rows, const MetricView& metrics, long long start, long long end);
std::string periodComparisonText(const PeriodStats& a, const PeriodStats& b, const std::string& aLabel,
                                 const std::string& bLabel, bool sameDevice);
}  // namespace dl
