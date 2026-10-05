#include "workspace_state.h"
#include "json_value.h"
#include "text_catalog.h"
#include "log_analysis.h"
#include "log_time.h"
#include "tablemodel.h"
#include "signal_chart.h"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
namespace dl {
namespace {
bool hashValid(const std::string& s){return s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos;}
void shortText(const std::string& s,std::size_t limit){if(s.size()>limit||s.find('\0')!=std::string::npos)throw std::invalid_argument("Invalid workspace text");}
SignalDistribution distribution(std::vector<int> values){SignalDistribution d;d.samples=values.size();if(values.empty())return d;std::sort(values.begin(),values.end());long long sum=0;for(int v:values)sum+=v;d.mean=double(sum)/values.size();d.minimum=values.front();d.maximum=values.back();d.p10=values[(values.size()-1)/10];d.p50=values[(values.size()-1)/2];d.p90=values[(values.size()-1)*9/10];return d;}
std::string decimal(double n){std::ostringstream out;out.imbue(std::locale::classic());out<<std::fixed<<std::setprecision(3)<<n;return out.str();}
}
std::string encodeWorkspace(const WorkspaceState& s){if(s.devices.size()>1000||s.reviews.size()>10000)throw std::length_error("Workspace record limit");Json j=Json::dict();j.object["schema"]=Json(1LL);auto devices=Json::dict(),reviews=Json::list();
    for(const auto& pair:s.devices){Json d=Json::dict();d.object["device"]=Json(pair.second.device);d.object["firmware"]=Json(pair.second.firmware);d.object["configuration"]=Json(pair.second.configuration);devices.object[pair.first]=std::move(d);}
    for(const auto& pair:s.reviews){Json r=Json::dict();r.object["key"]=Json(pair.first);r.object["title"]=Json(pair.second.title);r.object["note"]=Json(pair.second.note);r.object["status"]=Json(static_cast<long long>(pair.second.status));reviews.array.push_back(std::move(r));}
    j.object["devices"]=std::move(devices);j.object["reviews"]=std::move(reviews);auto bytes=writeJson(j)+"\n";if(bytes.size()>16*1024*1024)throw std::length_error("Workspace exceeds 16 MiB");return bytes;
}
WorkspaceState decodeWorkspace(const std::string& bytes){auto j=parseJson(bytes);if(j.at("schema").num()!=1)throw std::invalid_argument("Unsupported workspace schema");WorkspaceState s;const auto& devices=j.at("devices");const auto& reviews=j.at("reviews");if(devices.kind!=Json::Object||reviews.kind!=Json::Array||devices.object.size()>1000||reviews.array.size()>10000)throw std::invalid_argument("Workspace limits");
    for(const auto& pair:devices.object){if(!hashValid(pair.first))throw std::invalid_argument("Invalid source hash");DeviceMetadata d{pair.second.at("device").str(),pair.second.at("firmware").str(),pair.second.at("configuration").str()};shortText(d.device,512);shortText(d.firmware,512);shortText(d.configuration,4096);s.devices.emplace(pair.first,std::move(d));}
    for(const auto& entry:reviews.array){ReviewRecord r;r.key=entry.at("key").str();r.title=entry.at("title").str();r.note=entry.at("note").str();auto status=entry.at("status").num();if(!hashValid(r.key)||status<0||status>2)throw std::invalid_argument("Invalid review record");shortText(r.title,4096);shortText(r.note,32768);r.status=static_cast<ReviewStatus>(status);if(!s.reviews.emplace(r.key,std::move(r)).second)throw std::invalid_argument("Duplicate review key");}return s;}
std::string reviewStatusText(ReviewStatus s){return UiText8(s==ReviewStatus::Confirmed?TextId::review_confirmed:s==ReviewStatus::Handled?TextId::review_handled:TextId::review_pending);}
std::string reviewRecordText(const ReviewRecord& r){return std::string(UiText8(TextId::review_manual))+": "+reviewStatusText(r.status)+"\n"+GeneratedText(r.title)+"\n"+r.note+"\n";}
PeriodStats comparePeriod(LogView rows, long long start, long long end) {
    // Build source state before clipping: registration and cell evidence may
    // precede the selected period, but only in-period measurements are counted.
    auto metrics = buildMetrics(rows);
    metrics.erase(std::remove_if(metrics.begin(), metrics.end(), [&](const MetricRow& row) {
        return row.t < start || row.t > end;
    }), metrics.end());
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const LogLine* row) {
        return row->t < start || row->t > end;
    }), rows.end());
    PeriodStats stats;
    stats.lines = rows.size();
    if (rows.empty()) return stats;
    stats.first = stats.last = rows.front()->t;
    for (const auto* row : rows) {
        stats.first = std::min(stats.first, row->t);
        stats.last = std::max(stats.last, row->t);
        const auto& tag = row->tagText();
        std::string action;
        if (tag.compare(0, 8, "RECOVERY") == 0 || tag == "recovery" ||
            row->msg.find("_recovery_") != std::string::npos) {
            action = tag;
            const auto fields = hbFields(row->msg);
            for (const char* name : {"level", "action"}) {
                const auto found = fields.find(name);
                if (found != fields.end() && !found->second.empty()) action += " / " + std::string(name) + "=" + found->second;
            }
        }
        for (const char* stage : {"SOFT_RECOVERY", "HARD_RECOVERY"})
            if (row->msg.find(std::string("enter ") + stage) != std::string::npos) action = tag + " / " + stage;
        if (!action.empty()) ++stats.recoveryActions[action];
    }
    const auto outages = collectOutages(rows);
    stats.samples = metrics.size();
    stats.outages = outages.size();
    stats.observed = observationStats(rows).observedSpan;
    stats.availability = availabilityStats(rows, outages);
    for (const auto& outage : outages) {
        stats.down += outage.dur;
        if (!outage.recovered) ++stats.open;
        if (outage.l0Recovered) ++stats.recoveryActions["L0 / SDK recovery"];
    }
    std::vector<int> csq, rsrp, rsrq, snr, rssi;
    for (const auto& metric : metrics) {
        if (usesLteEngineeringReference(metric.rat)) {
            if (metric.csqVal >= 0 && metric.csqVal <= 31) csq.push_back(metric.csqVal);
            if (metric.rsrp < 0) rsrp.push_back(metric.rsrp);
            if (metric.rsrq < 0) rsrq.push_back(metric.rsrq);
            if (metric.snr10 != 100000) snr.push_back(metric.snr10);
        }
        if (metric.rssiVal < 0) rssi.push_back(metric.rssiVal);
    }
    stats.csq = distribution(std::move(csq));
    stats.rsrp = distribution(std::move(rsrp));
    stats.rsrq = distribution(std::move(rsrq));
    stats.snr = distribution(std::move(snr));
    stats.rssi = distribution(std::move(rssi));
    return stats;
}
std::string periodComparisonText(const PeriodStats& a,const PeriodStats& b,const std::string& al,const std::string& bl,bool sameDevice){std::string out=std::string(UiText8(TextId::compare_scope))+"\n"+(sameDevice?UiText8(TextId::compare_same):UiText8(TextId::compare_different))+"\n"+UiText8(TextId::compare_limits)+"\n\n";out+="A: "+al+"\n\nB: "+bl+"\n\n";out+=std::string(UiText8(TextId::compare_field))+"\tA\tB\tB − A\n";
    auto row=[&](const std::string& name,double x,double y){out+=name+"\t"+decimal(x)+"\t"+decimal(y)+"\t"+decimal(y-x)+"\n";};
    row(UiText8(TextId::compare_rows),a.lines,b.lines);row(UiText8(TextId::compare_samples),a.samples,b.samples);row(UiText8(TextId::compare_observed),a.observed,b.observed);row(UiText8(TextId::compare_outages),a.outages,b.outages);row(UiText8(TextId::compare_open),a.open,b.open);row(UiText8(TextId::compare_down),a.down,b.down);
    auto optional=[&](const std::string& name,bool av,double x,bool bv,double y){out+=name+"\t"+(av?decimal(x):"—")+"\t"+(bv?decimal(y):"—")+"\t"+(av&&bv?decimal(y-x):"—")+"\n";};
    optional(UiText8(TextId::compare_runtime),a.availability.runtimeValid(),a.availability.runtimePercent(),b.availability.runtimeValid(),b.availability.runtimePercent());optional(UiText8(TextId::compare_full),a.availability.fullValid(),a.availability.fullPercent(),b.availability.fullValid(),b.availability.fullPercent());optional(UiText8(TextId::compare_startup),a.availability.connectedStartupSegments,a.availability.longestStartupSeconds,b.availability.connectedStartupSegments,b.availability.longestStartupSeconds);
    auto signal=[&](const char* name,const SignalDistribution& x,const SignalDistribution& y,bool scaled){double scale=scaled?10:1;out+='\n';row(std::string(name)+" n",x.samples,y.samples);optional(std::string(name)+" mean",x.samples,x.mean/scale,y.samples,y.mean/scale);for(auto field:{std::pair<const char*,int SignalDistribution::*>{"min",&SignalDistribution::minimum},{"P10",&SignalDistribution::p10},{"P50",&SignalDistribution::p50},{"P90",&SignalDistribution::p90},{"max",&SignalDistribution::maximum}})optional(std::string(name)+" "+field.first,x.samples,(x.*field.second)/scale,y.samples,(y.*field.second)/scale);};
    signal("CSQ",a.csq,b.csq,false);signal("RSRP dBm",a.rsrp,b.rsrp,false);signal("RSRQ dB",a.rsrq,b.rsrq,false);signal("SNR dB",a.snr,b.snr,true);signal("RSSI dBm",a.rssi,b.rssi,false);
    out+="\n"+std::string(UiText8(TextId::compare_recovery))+"\n";std::map<std::string,int> tags;for(const auto& p:a.recoveryActions)tags[p.first]=0;for(const auto& p:b.recoveryActions)tags[p.first]=0;for(const auto& p:tags){auto x=a.recoveryActions.find(p.first),y=b.recoveryActions.find(p.first);row(p.first,x==a.recoveryActions.end()?0:x->second,y==b.recoveryActions.end()?0:y->second);}return out;
}
}
