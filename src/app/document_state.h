// document_state.h — 一次日志分析文档的所有权与借用视图边界
#pragma once

#include <string>

#include "log_types.h"

namespace dl {

struct SourceSummary {
    std::wstring label;
    std::size_t parsedLines = 0;
    std::size_t metricRows = 0;
    std::size_t outages = 0;
    int averageRsrp = 0;
    bool hasAverageRsrp = false;
};

class DocumentState {
public:
    std::vector<LogLine> lines;
    LogView filtered;                 // 借用 lines
    std::vector<std::string> sessions;
    std::vector<Outage> outages;
    std::vector<MetricRow> metrics;
    LogView timelineRows;             // 借用 lines
    ParseAudit audit;
    PlatformInfo platform;
    std::vector<Finding> findings;
    std::vector<SourceSummary> sources;

    DocumentState() = default;
    DocumentState(const DocumentState&) = delete;
    DocumentState& operator=(const DocumentState&) = delete;
    DocumentState(DocumentState&&) = delete;
    DocumentState& operator=(DocumentState&&) = delete;

    // 冷路径释放全部容量。借用视图总是在拥有者 lines 之前失效。
    void release();
    // 后台线程构建完整文档后，在 UI 线程常数时间接管所有权。
    void swap(DocumentState& other) noexcept;
};

} // namespace dl
