// document_state.cpp — 文档模型安全释放顺序
#include "document_state.h"

#include "memoryutil.h"

namespace dl {

void DocumentState::swap(DocumentState& other) noexcept {
    using std::swap;
    lines.swap(other.lines);
    filtered.swap(other.filtered);
    sessions.swap(other.sessions);
    outages.swap(other.outages);
    metrics.swap(other.metrics);
    timelineRows.swap(other.timelineRows);
    swap(audit, other.audit);
    swap(platform, other.platform);
    findings.swap(other.findings);
    sources.swap(other.sources);
}

void DocumentState::release() {
    releaseVector(timelineRows);
    releaseVector(filtered);

    releaseVector(outages);
    releaseVector(metrics);
    releaseVector(findings);
    releaseVector(sources);

    // 所有借用 lines 的指针均已解除，现在才可释放拥有者。
    releaseVector(lines);
    releaseVector(sessions);
    audit = ParseAudit{};
    platform = PlatformInfo{};
}

} // namespace dl
