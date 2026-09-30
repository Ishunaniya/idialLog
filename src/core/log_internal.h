// log_internal.h — logmodel 各实现文件共享的轻量内部工具
#pragma once

#include "log_types.h"

#include <cctype>
#include <string>

namespace dl {

inline bool isIdentChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline const LogLine& lineRef(const LogLine& line) { return line; }
inline const LogLine& lineRef(const LogLine* line) { return *line; }

inline bool icontains(const std::string& haystack, const char* needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

// 启动横幅是进程重启、会话切分和 RX/小区状态重置的共同证据，必须由解析和
// 分析共用同一规则。新版 RTMS 输出 "Modem_mng Version:"（首字母大写），
// 旧版可为 "modem_mng Version:"；这里兼容两种已发布原文，同时保留两套
// open_dial 与 artery 的既有横幅。此函数在逐行热路径上调用，故不为普通
// 心跳行分配大小写转换副本。
inline bool isProgramStartBanner(const std::string& msg) {
    return msg.find("DIAL Version:") != std::string::npos ||
           msg.find("Modem_mng Version:") != std::string::npos ||
           msg.find("modem_mng Version:") != std::string::npos ||
           msg.find("Program started. Version:") != std::string::npos ||
           msg.find("Program started. Main Version:") != std::string::npos;
}


// artery 的历史固定文案。共享识别规则，避免时间线与计数使用不同口径。
inline bool arteryStateTransition(const LogLine& line, std::string* from = nullptr,
                                  std::string* to = nullptr) {
    if (line.fmt != FMT_SEAS || line.msg.compare(0, 7, "state: ") != 0) return false;
    const size_t arrow = line.msg.find(" -> ", 7);
    if (arrow == std::string::npos) return false;
    const std::string left = line.msg.substr(7, arrow - 7);
    const std::string right = line.msg.substr(arrow + 4);
    const auto valid = [](const std::string& value) {
        if (value.empty()) return false;
        for (char c : value) if ((c < 'a' || c > 'z') && c != '_') return false;
        return true;
    };
    if ((left != "(start)" && !valid(left)) || !valid(right)) return false;
    if (from) *from = left;
    if (to) *to = right;
    return true;
}
inline bool arteryLegacyConnected(const LogLine& line) {
    return line.fmt == FMT_SEAS && line.msg == "Net Connected";
}
inline bool arteryLegacyDisconnected(const LogLine& line) {
    constexpr std::string_view prefix = "Net disconnected, and reason code 0x";
    if (line.fmt != FMT_SEAS || line.msg.compare(0, prefix.size(), prefix) != 0 ||
        line.msg.size() == prefix.size()) return false;
    for (size_t i = prefix.size(); i < line.msg.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(line.msg[i]))) return false;
    return true;
}

} // namespace dl
