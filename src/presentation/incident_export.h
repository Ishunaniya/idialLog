#pragma once
#include "incidentmodel.h"
#include <string>

namespace dl {
struct EvidenceSourceSpan {
    std::string label;
    std::size_t index=0, firstLine=0, lastLine=0, rawLineOffset=0;
};
struct EvidenceOrigin { std::string label; std::size_t index=0, line=0; };
struct EvidenceNote { std::size_t line=0; std::string text; };
struct IncidentExportContext {
    std::string scope, version, build;
    bool continuation=false;
    std::vector<EvidenceSourceSpan> sources;
    std::vector<EvidenceNote> bookmarks;
};
EvidenceOrigin evidenceOrigin(const std::vector<EvidenceSourceSpan>& sources,std::size_t mergedLine);
std::string incidentSummaryText(const IncidentReview& review,const IncidentExportContext& context);
// UTF-8 files and an atomic-write-ready ZIP. Throws length_error above 64 MiB.
std::string buildIncidentZip(const IncidentReview& review,const IncidentExportContext& context);
} // namespace dl
