#pragma once
#include "log_types.h"

namespace dl {
// Catalog rows come from the full, explicitly selected source scope. Borrowed
// rows must be released before that document is replaced or closed.
struct IncidentCatalog {
    LogView rows;
    std::vector<Outage> outages;
    PlatformInfo platform;
    std::vector<MetricRow> metrics;
};
struct IncidentReview {
    bool valid = false, resolvedOutsideFilter = false, inferredTime = false, clockLimited = false;
    std::size_t index = 0;
    Outage outage;
    PlatformInfo platform;
    const LogLine* platformEvidence=nullptr; // May be outside the observation window.
    long long start = 0, end = 0;
    LogView rows, events;
    std::vector<MetricRow> metrics;
    std::vector<Finding> findings; // Observation-window rules, not incident attribution.
};
IncidentCatalog buildIncidentCatalog(LogView fullScope);
std::size_t findIncident(const IncidentCatalog& catalog, const Outage& selected);
IncidentReview reviewIncident(const IncidentCatalog& catalog, std::size_t index,
                             int beforeSeconds = 300, int afterSeconds = 300);
bool incidentContainsLine(const IncidentReview& review, std::size_t line);
enum class IncidentPhase { Before, During, After };
IncidentPhase incidentSamplePhase(const IncidentReview& review,const MetricRow& sample);
} // namespace dl
