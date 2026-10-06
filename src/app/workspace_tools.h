#pragma once
#include "document_state.h"
#include "incidentmodel.h"

namespace dl {
struct ComparisonSelection {
    std::string a, b;
    long long aStart = 0, aEnd = 0, bStart = 0, bEnd = 0;
    bool relative = true;
    int metric = 0;
    bool viewActive = false;
    long long viewStart = 0, viewEnd = 0;
};

struct WorkspaceSession {
    std::string name, tag, message, since, until, cell, rat, channel;
    bool hasDeny = false;
    int deny = -1;
    bool rangeActive = false;
    long long start = 0, end = 0;
    int page = 9, workPage = 1;
    unsigned columns = (1u << 22) - 1;
    ComparisonSelection comparison;
    std::string boardScope, boardQuery;
    int boardStatus = -1, boardSort = 0;
    long long boardMinimum = 0;
    std::string trendScope;
    long long trendStart = 0, trendEnd = 0, trendStep = 86400;
};

std::string encodeSession(const WorkspaceSession& session);
WorkspaceSession decodeSession(const std::string& bytes);
std::string workspaceIncidentKey(const DocumentState& document, const Outage& outage);

struct WorkspaceEvent {
    Outage outage;
    std::string scope, key, device, source;
    ReviewRecord review;
};

// Empty scope enumerates all explicitly assigned devices and unassigned sources.
LogView workspaceScopeRows(const DocumentState& document, const std::string& scope);
std::vector<WorkspaceEvent> workspaceEvents(const DocumentState& document, const std::string& scope = {});
enum class ChainStage { Fault, Action, Network, Business };

struct ChainNode {
    ChainStage stage;
    long long time = 0;
    std::size_t line = 0;
    std::string text;
};

std::vector<ChainNode> incidentEvidenceChain(const IncidentReview& review);

struct TrendPeriod {
    long long start = 0, end = 0;
    std::size_t starts = 0, carried = 0;
    PeriodStats stats;
};

// Nonoverlapping inclusive periods; no fabricated observations in empty periods.
std::vector<TrendPeriod> workspaceTrend(const LogView& rows, long long start, long long end, long long step);
std::string workspaceTrendText(const std::vector<TrendPeriod>& periods, const std::string& scope);
}  // namespace dl
