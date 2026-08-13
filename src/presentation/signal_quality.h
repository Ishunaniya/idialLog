// signal_quality.h — 信号指标的统一工程分档。
//
// 3GPP / AT 手册定义指标的含义、单位与上报范围，但不会给所有现场一个通用的
// “好/坏”故障边界。下面的四档仅用于可视化和排障提示；核心结论仍使用有证据的
// 独立规则，不能只凭颜色或单个样本归因断网。
#pragma once

#include <cstddef>
#include <initializer_list>

#include "log_types.h"

namespace dl {

enum class SignalQuality {
    Unknown = -1,
    Poor = 0,
    Fair = 1,
    Good = 2,
    Excellent = 3
};

inline constexpr int kCsqFair = 10;
inline constexpr int kCsqGood = 15;
inline constexpr int kCsqExcellent = 20;
inline constexpr int kRsrpFair = -100;
inline constexpr int kRsrpGood = -90;
inline constexpr int kRsrpExcellent = -80;
inline constexpr int kRsrqFair = -20;
inline constexpr int kRsrqGood = -15;
inline constexpr int kRsrqExcellent = -10;
inline constexpr int kSnrFair10 = 0; // 必须严格大于该线才进入“一般”。
inline constexpr int kSnrGood10 = 130;
inline constexpr int kSnrExcellent10 = 200;

// CSQ:3GPP TS 27.007 / AT+CSQ 有效值 0..31，99 表示未知。
inline SignalQuality csqQuality(int value) {
    if (value < 0 || value > 31) return SignalQuality::Unknown;
    if (value >= kCsqExcellent) return SignalQuality::Excellent;
    if (value >= kCsqGood) return SignalQuality::Good;
    if (value >= kCsqFair) return SignalQuality::Fair;
    return SignalQuality::Poor;
}

// RSRP/RSRQ/SNR 四档是常用工程建议，越大越好；不是运营商或模组故障常量。
inline SignalQuality rsrpQuality(int dbm) {
    if (dbm >= 0) return SignalQuality::Unknown;
    if (dbm >= kRsrpExcellent) return SignalQuality::Excellent;
    if (dbm >= kRsrpGood) return SignalQuality::Good;
    if (dbm >= kRsrpFair) return SignalQuality::Fair;
    return SignalQuality::Poor;
}

inline SignalQuality rsrqQuality(int db) {
    if (db >= 0) return SignalQuality::Unknown;
    if (db >= kRsrqExcellent) return SignalQuality::Excellent;
    if (db >= kRsrqGood) return SignalQuality::Good;
    if (db >= kRsrqFair) return SignalQuality::Fair;
    return SignalQuality::Poor;
}

inline SignalQuality snrQuality10(int value10) {
    if (value10 == 100000) return SignalQuality::Unknown;
    if (value10 >= kSnrExcellent10) return SignalQuality::Excellent;
    if (value10 >= kSnrGood10) return SignalQuality::Good;
    if (value10 > kSnrFair10) return SignalQuality::Fair;
    return SignalQuality::Poor; // 保留项目既有的 SNR≤0 dB 低质量观察口径。
}

inline const char* signalQualityName(SignalQuality quality) {
    switch (quality) {
    case SignalQuality::Poor:      return "较差";
    case SignalQuality::Fair:      return "一般";
    case SignalQuality::Good:      return "良好";
    case SignalQuality::Excellent: return "优秀";
    default:                       return "未知";
    }
}

inline SignalQuality metricSignalQuality(const MetricRow& metric, std::size_t column) {
    switch (column) {
    case 5:  return csqQuality(metric.csqVal);
    case 10: return rsrpQuality(metric.rsrp);
    case 11: return rsrqQuality(metric.rsrq);
    case 12: return snrQuality10(metric.snr10);
    default: return SignalQuality::Unknown;
    }
}

inline SignalQuality metricOverallSignalQuality(const MetricRow& metric) {
    SignalQuality worst = SignalQuality::Unknown;
    for (std::size_t column : {std::size_t{5}, std::size_t{10}, std::size_t{11}, std::size_t{12}}) {
        const SignalQuality quality = metricSignalQuality(metric, column);
        if (quality != SignalQuality::Unknown &&
            (worst == SignalQuality::Unknown || static_cast<int>(quality) < static_cast<int>(worst)))
            worst = quality;
    }
    return worst;
}

} // namespace dl
