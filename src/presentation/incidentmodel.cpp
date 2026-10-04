#include "incidentmodel.h"
#include "log_analysis.h"
#include "tablemodel.h"
#include <algorithm>
#include <limits>

namespace dl {
// Same one-day time-zone allowance as the parser and outage analysis.
static constexpr long long kReviewWallClockEpoch = 946598400LL;
IncidentCatalog buildIncidentCatalog(LogView fullScope) {
    IncidentCatalog result; result.rows=std::move(fullScope);
    result.outages=collectOutages(result.rows);
    result.platform=detectPlatform(result.rows);
    result.metrics=buildMetrics(result.rows);
    std::stable_sort(result.outages.begin(),result.outages.end(),[](const Outage& a,const Outage& b) {
        return a.start==b.start?a.startLine<b.startLine:a.start<b.start;
    });
    return result;
}
std::size_t findIncident(const IncidentCatalog& catalog,const Outage& selected) {
    for (std::size_t i=0;i<catalog.outages.size();++i)
        if (catalog.outages[i].startLine==selected.startLine) return i;
    // A filtered recovery-only record may use its recovery line as the start.
    if (selected.endLine) for (std::size_t i=0;i<catalog.outages.size();++i)
        if (catalog.outages[i].endLine==selected.endLine) return i;
    return catalog.outages.size();
}
static long long addMargin(long long time,int margin,bool before) {
    const long long delta=std::clamp(margin,0,86400);
    if (before) return time<std::numeric_limits<long long>::min()+delta?
        std::numeric_limits<long long>::min():time-delta;
    return time>std::numeric_limits<long long>::max()-delta?
        std::numeric_limits<long long>::max():time+delta;
}
IncidentReview reviewIncident(const IncidentCatalog& catalog,std::size_t index,int before,int after) {
    IncidentReview result;
    if (index>=catalog.outages.size() || catalog.rows.empty()) return result;
    result.index=index;result.outage=catalog.outages[index];result.platform=catalog.platform;
    for(const auto* row:catalog.rows)if(row->lineNo==catalog.platform.evidenceLine){result.platformEvidence=row;break;}
    // Match the core's unsynchronised/wall-clock boundary. An open outage must
    // not become a decades-long review because the device received its clock.
    const bool wallClock=result.outage.start>=kReviewWallClockEpoch;
    long long first=std::numeric_limits<long long>::max(),last=std::numeric_limits<long long>::min();
    for (const auto* row:catalog.rows) {
        if ((row->t>=kReviewWallClockEpoch)!=wallClock) { result.clockLimited=true;continue; }
        first=std::min(first,row->t);last=std::max(last,row->t);
    }
    const long long incidentEnd=result.outage.recovered?result.outage.end:last;
    result.start=std::max(first,addMargin(result.outage.start,before,true));
    result.end=std::min(last,addMargin(incidentEnd,after,false));
    if (result.end<result.start) return result;
    for (const auto* row:catalog.rows) if ((row->t>=kReviewWallClockEpoch)==wallClock && row->t>=result.start && row->t<=result.end) {
        result.rows.push_back(row);result.inferredTime=result.inferredTime || row->inferredTime;
    }
    if (result.rows.empty()) return result;
    // Keep boundary evidence even when a generic event classifier cannot label
    // a product-specific interface/WAN edge. Neither actions nor nearby "up"
    // messages are used to invent a recovery boundary.
    buildTimelineView(result.rows,result.events);
    for (const auto* row:result.rows) if ((row->lineNo==result.outage.startLine ||
        row->lineNo==result.outage.endLine) && std::find(result.events.begin(),result.events.end(),row)==result.events.end())
        result.events.push_back(row);
    std::stable_sort(result.events.begin(),result.events.end(),[](const LogLine* a,const LogLine* b) {
        return a->t==b->t?a->lineNo<b->lineNo:a->t<b->t;
    });
    // Keep full-source radio/cell snapshots that preceded the short window.
    for(const auto& metric:catalog.metrics)if((metric.t>=kReviewWallClockEpoch)==wallClock && metric.t>=result.start && metric.t<=result.end)
        result.metrics.push_back(metric);
    std::stable_sort(result.metrics.begin(),result.metrics.end(),[](const MetricRow& a,const MetricRow& b){
        return a.t==b.t?a.lineNo<b.lineNo:a.t<b.t;
    });
    // Run the existing rules on the window itself. File-level parse audit is
    // deliberately not fabricated from already-parsed rows.
    const auto windowOutages=collectOutages(result.rows);
    result.findings=analyze(result.rows,windowOutages,result.metrics,result.platform,ParseAudit{});
    result.valid=true;return result;
}
bool incidentContainsLine(const IncidentReview& review,std::size_t line) {
    return std::any_of(review.rows.begin(),review.rows.end(),[&](const LogLine* row){return row->lineNo==line;});
}
IncidentPhase incidentSamplePhase(const IncidentReview& r,const MetricRow& m) {
    if(m.t<r.outage.start || (m.t==r.outage.start && m.lineNo<r.outage.startLine))return IncidentPhase::Before;
    if(r.outage.recovered && (m.t>r.outage.end || (m.t==r.outage.end && m.lineNo>=r.outage.endLine)))return IncidentPhase::After;
    return IncidentPhase::During;
}
} // namespace dl
