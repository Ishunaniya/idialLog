// tablemodel.h — 时间线/指标虚拟 ListView 的纯 C++ 数据源。
// 不含 Win32 依赖,可在 Linux 主机上验证行选择与每列文本。
#pragma once

#include "log_types.h"

#include <cstddef>
#include <string>

namespace dl {

constexpr size_t kTimelineColumnCount = 4;
constexpr size_t kMetricColumnCount = 22;
constexpr size_t kRawColumnCount = 5;
constexpr size_t kCellColumnCount = 17;

// 时间线只保存指向筛选视图中事件行的指针,生命周期与 LogView 相同。
void buildTimelineView(const LogView& lines, LogView& timeline);

// 展示分类不参与断网配对或根因判断。恢复动作不等价于已经恢复联网。
enum class LogEventKind { Normal, Fault, Recovered, RecoveryAction, Error, Warning, State, Cell, Sdk };
LogEventKind logEventKind(const LogLine& line);
const char* logEventLabel(LogEventKind kind);
std::string outageStatusText(const Outage& outage);

// 返回 UTF-8 单元格文本；越界列返回空串。Windows 层只负责按需转 UTF-16。
std::string timelineCellText(const LogLine& line, size_t column);
std::string metricCellText(const MetricRow& metric, size_t column);
std::string rawCellText(const LogLine& line, size_t column);
std::string cellSummaryCellText(const CellSummary& cell, size_t column);

// CSV 中只有文本列需要防公式注入；数值列（含负 RSRP/RSRQ）保持可计算。
std::string metricCsvCellText(const MetricRow& metric, size_t column);

}  // namespace dl
