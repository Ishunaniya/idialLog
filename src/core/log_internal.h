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

} // namespace dl
