// log_analysis.h — 平台、指标、故障判定与结论引擎 API
#pragma once

#include "log_types.h"

#include <utility>

namespace dl {

// 心跳/诊断字段解析:同时支持 "K:V" 与 "K=V",'|' 与空格分隔;非字段行返回空 map
std::map<std::string, std::string> hbFields(const std::string& msg);
// 平台识别(基于已解析行的实证特征)
PlatformInfo detectPlatform(const std::vector<LogLine>& lines);

// ---- 分析 ----
std::vector<Outage> collectOutages(const std::vector<LogLine>& lines);
std::vector<Outage> collectOutages(const LogView& lines);
ObservationStats observationStats(const std::vector<LogLine>& lines);
ObservationStats observationStats(const LogView& lines);
std::vector<Stall>  detectRxStall(const std::vector<std::pair<long long,long long>>& rxs,
                                  long long minStallSec = 120);
std::vector<MetricRow> buildMetrics(const std::vector<LogLine>& lines);
std::vector<MetricRow> buildMetrics(const LogView& lines);
CellAnalysis analyzeCells(const std::vector<LogLine>& lines,
                          const std::vector<MetricRow>& metrics,
                          const std::vector<Outage>& outages);
CellAnalysis analyzeCells(const LogView& lines,
                          const std::vector<MetricRow>& metrics,
                          const std::vector<Outage>& outages);

// 结论引擎:每条结论必须带证据(ev 非空),否则不输出
std::vector<Finding> analyze(const std::vector<LogLine>& lines,
                             const std::vector<Outage>& outs,
                             const std::vector<MetricRow>& mets,
                             const PlatformInfo& pi,
                             const ParseAudit& audit,
                             const CellAnalysis* precomputedCells = nullptr);
std::vector<Finding> analyze(const LogView& lines,
                             const std::vector<Outage>& outs,
                             const std::vector<MetricRow>& mets,
                             const PlatformInfo& pi,
                             const ParseAudit& audit,
                             const CellAnalysis* precomputedCells = nullptr);

// ---- 判定/工具 ----
bool isFaultStart(const std::string& msg);          // "Ping failed ... fault timer started"
bool isRecovered(const std::string& msg, int* durSec); // "Network recovered after Ns"
bool isEventTag(const std::string& tag);            // 状态变化类标签(时间线保留)
bool isErrTag(const std::string& tag);              // ERROR/WARN/FATAL/ALARM
// 整行级判定:除标签外还认 seas_log 的级别字段(artery 的报错不带 [TAG],
// 严重度在 level 里,只看 tag 会把 [ERROR] 行漏掉)
bool isErrLine(const LogLine& l);
bool isEventLine(const LogLine& l);

} // namespace dl
