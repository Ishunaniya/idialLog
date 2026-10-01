// document_state.cpp — 文档模型安全释放顺序
#include "document_state.h"
#include <algorithm>

#include "memoryutil.h"

namespace dl {

void DocumentState::swap(DocumentState& other) noexcept {
    std::swap(timeRange, other.timeRange);
    using std::swap;
    lines.swap(other.lines);
    filtered.swap(other.filtered);
    sessions.swap(other.sessions);
    outages.swap(other.outages);
    metrics.swap(other.metrics);
    metricView.swap(other.metricView);
    swap(cellAnalysis, other.cellAnalysis);
    timelineRows.swap(other.timelineRows);
    swap(audit, other.audit);
    swap(platform, other.platform);
    findings.swap(other.findings);
    sources.swap(other.sources);
}

void DocumentState::release() {
    timeRange = TimeRange{};
    releaseVector(timelineRows);
    releaseVector(filtered);
    releaseVector(metricView);

    releaseVector(outages);
    releaseVector(metrics);
    releaseVector(cellAnalysis.cells);
    releaseVector(cellAnalysis.transitions);
    cellAnalysis = CellAnalysis{};
    releaseVector(findings);
    releaseVector(sources);

    // 所有借用 lines 的指针均已解除，现在才可释放拥有者。
    releaseVector(lines);
    releaseVector(sessions);
    audit = ParseAudit{};
    platform = PlatformInfo{};
}

void DocumentState::selectTimeRange(long long start, long long end) {
    timeRange = TimeRange{true, std::min(start, end), std::max(start, end)};
}

void DocumentState::restrictToTimeRange(LogView& view) const {
    if (!timeRange.active) return;
    view.erase(std::remove_if(view.begin(), view.end(), [&](const LogLine* line) {
        return line->t < timeRange.start || line->t > timeRange.end;
    }), view.end());
}

} // namespace dl
