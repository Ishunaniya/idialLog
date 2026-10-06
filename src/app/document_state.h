// document_state.h — 一次日志分析文档的所有权与借用视图边界
#pragma once

#include <string>

#include "log_types.h"
#include "workspace_state.h"

namespace dl {

struct SourceSummary {
    std::wstring label;
    std::wstring originalPath;
    std::string originalEntry, originalHash, identity, pastedBytes;

    const std::string& workspaceKey() const {
        return identity.empty() ? originalHash : identity;
    }

    std::size_t first = 0, last = 0;  // parsed-line range [first,last), including clock segments
    std::size_t rawLineOffset = 0;    // preceding raw lines in the ordered merged input
    std::size_t parsedLines = 0;
    std::size_t metricRows = 0;
    std::size_t outages = 0;
    int averageRsrp = 0;
    bool hasAverageRsrp = false;
};

struct SourceComparison {
    std::size_t source = 0, lines = 0, samples = 0, outages = 0, unrecovered = 0;
    long long firstTime = 0, lastTime = 0, observedSeconds = 0, outageSeconds = 0;
    AvailabilityStats availability;
    RssiObservation rssi;
    int rsrpSamples = 0, rsrpMin = 0, rsrpMax = 0, rsrpAverage = 0;
    int csqSamples = 0, csqMin = 0, csqMax = 0, csqAverage = 0;
};

class DocumentState {
  public:
    struct TimeRange {
        bool active = false;
        long long start = 0, end = 0;
    } timeRange;

    std::vector<LogLine> lines;
    LogView filtered;  // 借用 lines
    std::vector<std::string> sessions;
    std::vector<Outage> outages;
    std::vector<MetricRow> metrics;
    MetricView metricView;  // 借用 metrics；UI 快捷筛选/排序结果
    CellAnalysis cellAnalysis;
    LogView timelineRows;  // 借用 lines
    ParseAudit audit;
    PlatformInfo platform;
    std::vector<Finding> findings;
    std::vector<SourceSummary> sources;
    WorkspaceState workspace;
    std::string selectedDevice;
    void restrictToSelection(LogView& view) const;
    // Independent sources are never paired across files. Continuation requires user selection.
    enum class SourceMode { Independent, Continuation, Device } sourceMode = SourceMode::Independent;
    std::size_t selectedSource = 0;
    std::vector<SourceComparison> comparisons;

    DocumentState() = default;
    DocumentState(const DocumentState&) = delete;
    DocumentState& operator=(const DocumentState&) = delete;
    DocumentState(DocumentState&&) = delete;
    DocumentState& operator=(DocumentState&&) = delete;

    // 冷路径释放全部容量。借用视图总是在拥有者 lines 之前失效。
    void release();
    // 后台线程构建完整文档后，在 UI 线程常数时间接管所有权。
    void swap(DocumentState& other) noexcept;
    // 图表选区使用精确时间戳，不经过 HH:MM 输入框，保留跨日/跨年的日期。
    void selectTimeRange(long long start, long long end);
    void restrictToTimeRange(LogView& view) const;
    void restrictToSource(LogView& view, std::size_t source) const;
    RssiObservation rssi;
    void rebuildSignalObservations();
    void rebuildComparisons(const LogView& scoped);
};

}  // namespace dl
