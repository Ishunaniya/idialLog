#pragma once
#include <array>
#include <cstdint>

namespace dl {
constexpr std::uint32_t kAllMetricColumns = (1u << 22) - 1;
enum class MetricColumnPreset { Common, Radio, Registration, Traffic, AtHealth, All };
constexpr std::uint32_t metricColumnMask(MetricColumnPreset preset) {
    switch (preset) {
    case MetricColumnPreset::Common: return 1u | (1u<<1) | (1u<<2) | (1u<<5) | (1u<<9) | (1u<<10) | (1u<<11) | (1u<<12) | (1u<<18);
    case MetricColumnPreset::Radio: return 1u | (1u<<2) | (1u<<3) | (1u<<4) | (1u<<5) | (1u<<10) | (1u<<11) | (1u<<12) | (1u<<13) | (1u<<18);
    case MetricColumnPreset::Registration: return 1u | (1u<<1) | (1u<<2) | (1u<<14) | (1u<<15) | (1u<<16) | (1u<<17);
    case MetricColumnPreset::Traffic: return 1u | (1u<<1) | (1u<<6) | (1u<<7) | (1u<<8) | (1u<<9);
    case MetricColumnPreset::AtHealth: return 1u | (1u<<19) | (1u<<20) | (1u<<21);
    default: return kAllMetricColumns;
    }
}
constexpr std::array<int, 22> kMetricColumnWidths{186,82,105,58,70,58,58,86,105,76,68,68,78,68,52,76,58,145,155,88,76,112};
}
