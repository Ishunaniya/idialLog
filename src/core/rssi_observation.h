#pragma once
#include <algorithm>
#include <climits>
#include <cstddef>

namespace dl {
// Reported dBm only: missing/sentinel values never become zero samples.
// These observations do not define a signal grade or a fault threshold.
struct RssiObservation {
    std::size_t samples = 0;
    int minimum = INT_MAX, maximum = INT_MIN;
    long long sum = 0;

    void add(int value) {
        if (value >= 0)
            return;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        sum += value;
        ++samples;
    }

    double mean() const {
        return samples ? static_cast<double>(sum) / samples : 0.0;
    }
};
}  // namespace dl
