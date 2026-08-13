// radio_access.h — 无分配的无线制式判断辅助函数。
#pragma once

#include <string_view>

namespace dl {

inline char upperAscii(char value) {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A') : value;
}

inline bool ratEquals(std::string_view value, std::string_view expected) {
    if (value.size() != expected.size()) return false;
    for (std::size_t index = 0; index < value.size(); ++index)
        if (upperAscii(value[index]) != expected[index]) return false;
    return true;
}

inline bool ratContains(std::string_view value, std::string_view expected) {
    if (expected.empty() || value.size() < expected.size()) return false;
    for (std::size_t offset = 0; offset + expected.size() <= value.size(); ++offset) {
        bool same = true;
        for (std::size_t index = 0; index < expected.size(); ++index)
            if (upperAscii(value[offset + index]) != expected[index]) { same = false; break; }
        if (same) return true;
    }
    return false;
}

// 旧日志往往没有 RAT 字段，但当前四个产品的这些信号字段均来自 LTE SDK，继续兼容。
// 一旦日志明确给出其它制式，就不套用 LTE 工程分档。LTE-M/eMTC/NB-IoT 仍属于 LTE 家族。
inline bool usesLteEngineeringReference(std::string_view rat) {
    while (!rat.empty() && static_cast<unsigned char>(rat.front()) <= ' ') rat.remove_prefix(1);
    while (!rat.empty() && static_cast<unsigned char>(rat.back()) <= ' ') rat.remove_suffix(1);
    if (rat.empty()) return true;
    return ratContains(rat, "LTE") || ratEquals(rat, "4G") || ratEquals(rat, "EMTC") ||
           ratEquals(rat, "CAT-M") || ratEquals(rat, "CATM") || ratEquals(rat, "NB-IOT") ||
           ratEquals(rat, "NBIOT");
}

} // namespace dl
