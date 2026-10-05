#pragma once
#include "incidentmodel.h"
#include <string>
#include "workspace_state.h"

namespace dl {
struct EvidenceSourceSpan {
    std::string label;
    std::size_t index=0, firstLine=0, lastLine=0, rawLineOffset=0;
};
struct EvidenceOrigin { std::string label; std::size_t index=0, line=0; };
struct EvidenceNote { std::size_t line=0; std::string text; };
struct IncidentExportContext {
    std::string scope, version, build,selectedDevice,selectedSourceHash;
    bool continuation=false;
    std::vector<EvidenceSourceSpan> sources;
    std::vector<EvidenceNote> bookmarks;
    struct Original {std::string name,label,bytes,hash,identity;};
    std::vector<Original> originals;
    WorkspaceState workspace;
    std::vector<ReviewRecord> reviews;
};
struct ImportedEvidencePackage {
    std::vector<IncidentExportContext::Original> originals;
    WorkspaceState workspace;
    std::string report;
    long long start=0,end=0;
    bool continuation=false;
    std::string selectedDevice,selectedSourceHash;
    struct Bookmark {std::string sourceHash,text;std::size_t line=0;};
    std::vector<Bookmark> bookmarks;
};
ImportedEvidencePackage readEvidencePackage(const std::string& bytes);
EvidenceOrigin evidenceOrigin(const std::vector<EvidenceSourceSpan>& sources,std::size_t mergedLine);
std::string incidentSummaryText(const IncidentReview& review,const IncidentExportContext& context);
// UTF-8 evidence and an atomic-write-ready ZIP. Basic packages: 64 MiB;
// complete packages: 512 MiB total and 256 MiB per original. No truncation.
std::string buildIncidentZip(const IncidentReview& review,const IncidentExportContext& context);
} // namespace dl
