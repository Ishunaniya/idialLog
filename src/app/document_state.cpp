// document_state.cpp — 文档模型安全释放顺序
#include "document_state.h"
#include <algorithm>

#include "memoryutil.h"
#include "log_analysis.h"

namespace dl {

void DocumentState::swap(DocumentState& other) noexcept {
    std::swap(timeRange, other.timeRange);
    std::swap(sourceMode, other.sourceMode);
    std::swap(selectedSource, other.selectedSource);
    comparisons.swap(other.comparisons);
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
    sourceMode = SourceMode::Independent;
    selectedSource = 0;
    releaseVector(comparisons);
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

void DocumentState::restrictToSource(LogView& view, std::size_t source) const {
    if (sources.empty()) return;
    if (source >= sources.size()) { view.clear(); return; }
    const auto& bounds = sources[source];
    view.erase(std::remove_if(view.begin(), view.end(), [&](const LogLine* line) {
        const std::size_t index = static_cast<std::size_t>(line - lines.data());
        return index < bounds.first || index >= bounds.last;
    }), view.end());
}

void DocumentState::rebuildComparisons(const LogView& scoped) {
    comparisons.clear();
    // One pass distributes rows; avoid scanning N rows once per source.
    std::vector<LogView> views(sources.size());
    for (const LogLine* line : scoped) {
        const auto index = static_cast<std::size_t>(line - lines.data());
        const auto found = std::upper_bound(sources.begin(), sources.end(), index,
            [](std::size_t value, const SourceSummary& source) { return value < source.last; });
        if (found != sources.end() && index >= found->first)
            views[static_cast<std::size_t>(found - sources.begin())].push_back(line);
    }
    for (std::size_t i = 0; i < views.size(); ++i) {
        SourceComparison stats; stats.source = i; stats.lines = views[i].size();
        if (!views[i].empty()) {
            stats.firstTime = stats.lastTime = views[i].front()->t;
            for (const auto* line : views[i]) {
                stats.firstTime = std::min(stats.firstTime, line->t);
                stats.lastTime = std::max(stats.lastTime, line->t);
            }
        }
        const auto metricsForSource = buildMetrics(views[i]);
        const auto outagesForSource = collectOutages(views[i]);
        stats.samples = metricsForSource.size(); stats.outages = outagesForSource.size();
        stats.observedSeconds = observationStats(views[i]).observedSpan;
        stats.availability = availabilityStats(views[i], outagesForSource);
        for (const auto& outage : outagesForSource) {
            stats.outageSeconds += outage.dur;
            if (!outage.recovered) ++stats.unrecovered;
        }
        long long rsrpTotal = 0, csqTotal = 0;
        for (const auto& metric : metricsForSource) {
            if (metric.rsrp < 0) {
                if (!stats.rsrpSamples) stats.rsrpMin = stats.rsrpMax = metric.rsrp;
                stats.rsrpMin = std::min(stats.rsrpMin, metric.rsrp);
                stats.rsrpMax = std::max(stats.rsrpMax, metric.rsrp);
                rsrpTotal += metric.rsrp; ++stats.rsrpSamples;
            }
            if (metric.csqVal >= 0 && metric.csqVal <= 31) {
                if (!stats.csqSamples) stats.csqMin = stats.csqMax = metric.csqVal;
                stats.csqMin = std::min(stats.csqMin, metric.csqVal);
                stats.csqMax = std::max(stats.csqMax, metric.csqVal);
                csqTotal += metric.csqVal; ++stats.csqSamples;
            }
        }
        if (stats.rsrpSamples) stats.rsrpAverage = static_cast<int>(rsrpTotal / stats.rsrpSamples);
        if (stats.csqSamples) stats.csqAverage = static_cast<int>(csqTotal / stats.csqSamples);
        comparisons.push_back(stats);
    }
}

} // namespace dl
