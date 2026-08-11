// log_filter.h — 日志轻量筛选 API
#pragma once

#include "log_types.h"

namespace dl {

// ---- 过滤 ----
// tag: "A,B" 逗号分隔; grep: 正则(忽略大小写); since/until: "HH:MM[:SS]" 或 "MM-DD HH:MM"
// grepBad 置为 true 表示正则非法(调用方可提示)
LogView applyFilterView(const std::vector<LogLine>& lines,
                        const std::string& tag, const std::string& grep,
                        const std::string& since, const std::string& until,
                        bool* grepBad = nullptr);

// 兼容旧调用:返回拥有数据的副本。新 UI 应优先使用 applyFilterView 避免复制正文。
std::vector<LogLine> applyFilters(const std::vector<LogLine>& lines,
                                  const std::string& tag, const std::string& grep,
                                  const std::string& since, const std::string& until,
                                  bool* grepBad = nullptr);

} // namespace dl
