#include "workspace_tools.h"
#include "json_value.h"
#include "sha256.h"
#include "tablemodel.h"
#include "log_analysis.h"
#include "log_time.h"
#include "text_catalog.h"
#include <algorithm>
#include <set>
#include <sstream>
#include <iomanip>
#include <locale>
#include <stdexcept>

namespace dl {
namespace {
void textBound(const std::string& text, std::size_t limit = 4096) {
    if (text.size() > limit || text.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid session text");
}

void validateSession(const WorkspaceSession& s) {
    for (const auto* value : {&s.name, &s.tag, &s.message, &s.since, &s.until, &s.cell, &s.rat, &s.channel,
                              &s.comparison.a, &s.comparison.b, &s.trendScope, &s.boardScope, &s.boardQuery})
        textBound(*value);
    for (const auto value : {s.start, s.end, s.comparison.aStart, s.comparison.aEnd, s.comparison.bStart,
                             s.comparison.bEnd, s.trendStart, s.trendEnd})
        if (value < 0 || value > 4102444799LL)
            throw std::invalid_argument("Session dates must be in 1970–2099");

    if (s.comparison.viewActive) {
        const auto lo = s.comparison.relative ? 0 : std::min(s.comparison.aStart, s.comparison.bStart);
        const auto hi = s.comparison.relative
                            ? std::max(s.comparison.aEnd - s.comparison.aStart, s.comparison.bEnd - s.comparison.bStart)
                            : std::max(s.comparison.aEnd, s.comparison.bEnd);
        if (s.comparison.viewStart < lo || s.comparison.viewEnd > hi)
            throw std::invalid_argument("Invalid comparison view range");
    }

    if (s.boardStatus < -1 || s.boardStatus > 2 || s.boardSort < 0 || s.boardSort > 1 || s.boardMinimum < 0 ||
        s.start > s.end || s.comparison.aStart > s.comparison.aEnd || s.comparison.bStart > s.comparison.bEnd ||
        s.trendStart > s.trendEnd || s.trendStep < 1 || s.trendStep > 366LL * 86400 || s.page < 0 || s.page > 9 ||
        s.workPage < 0 || s.workPage > 4 || s.comparison.metric < 0 || s.comparison.metric > 4 ||
        (s.comparison.viewActive && s.comparison.viewStart > s.comparison.viewEnd) || s.deny < -1 || s.deny > 65535 ||
        s.columns >= (1u << 22) || !(s.columns & 1))
        throw std::invalid_argument("Invalid session settings");
}

std::string origin(const DocumentState& doc, std::size_t line) {
    for (const auto& source : doc.sources)
        if (source.first < source.last && line >= doc.lines[source.first].lineNo &&
            line <= doc.lines[source.last - 1].lineNo)
            return source.workspaceKey() + ":" + std::to_string(line - source.rawLineOffset);
    return "unknown:" + std::to_string(line);
}

std::string utf8(const std::wstring& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        unsigned c = text[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.size()) {
            unsigned d = text[i + 1];
            if (d >= 0xdc00 && d <= 0xdfff) {
                c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
                ++i;
            }
        }

        if (c < 0x80)
            out += char(c);
        else if (c < 0x800) {
            out += char(0xc0 | (c >> 6));
            out += char(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out += char(0xe0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 63));
            out += char(0x80 | (c & 63));
        } else {
            out += char(0xf0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 63));
            out += char(0x80 | ((c >> 6) & 63));
            out += char(0x80 | (c & 63));
        }
    }

    return out;
}
}  // namespace

std::string encodeSession(const WorkspaceSession& s) {
    validateSession(s);

    Json j = Json::dict();
    j.object["schema"] = Json(1LL);
    for (auto pair : {std::pair<const char*, std::string>{"name", s.name},
                      {"tag", s.tag},
                      {"message", s.message},
                      {"since", s.since},
                      {"until", s.until},
                      {"cell", s.cell},
                      {"rat", s.rat},
                      {"channel", s.channel},
                      {"a", s.comparison.a},
                      {"b", s.comparison.b},
                      {"trend_scope", s.trendScope},
                      {"board_scope", s.boardScope},
                      {"board_query", s.boardQuery}})
        j.object[pair.first] = Json(pair.second);
    for (auto pair : {std::pair<const char*, long long>{"start", s.start},
                      {"end", s.end},
                      {"page", s.page},
                      {"work_page", s.workPage},
                      {"columns", s.columns},
                      {"a_start", s.comparison.aStart},
                      {"a_end", s.comparison.aEnd},
                      {"b_start", s.comparison.bStart},
                      {"b_end", s.comparison.bEnd},
                      {"metric", s.comparison.metric},
                      {"view_start", s.comparison.viewStart},
                      {"view_end", s.comparison.viewEnd},
                      {"deny", s.deny},
                      {"trend_start", s.trendStart},
                      {"trend_end", s.trendEnd},
                      {"trend_step", s.trendStep},
                      {"board_status", s.boardStatus},
                      {"board_sort", s.boardSort},
                      {"board_minimum", s.boardMinimum}})
        j.object[pair.first] = Json(pair.second);

    j.object["has_deny"] = Json::flag(s.hasDeny);
    j.object["view_active"] = Json::flag(s.comparison.viewActive);
    j.object["range_active"] = Json::flag(s.rangeActive);
    j.object["relative"] = Json::flag(s.comparison.relative);
    return writeJson(j);
}

WorkspaceSession decodeSession(const std::string& bytes) {
    auto j = parseJson(bytes);
    if (j.at("schema").num() != 1)
        throw std::invalid_argument("Unsupported session");

    WorkspaceSession s;
    s.name = j.at("name").str();
    s.tag = j.at("tag").str();
    s.message = j.at("message").str();
    s.since = j.at("since").str();
    s.until = j.at("until").str();
    s.cell = j.at("cell").str();
    s.rat = j.at("rat").str();
    s.channel = j.at("channel").str();

    s.hasDeny = j.at("has_deny").flag();
    const auto deny = j.at("deny").num();
    if (deny < -1 || deny > 65535)
        throw std::invalid_argument("Invalid DENY filter");
    s.deny = int(deny);

    s.comparison.viewActive = j.at("view_active").flag();
    s.comparison.viewStart = j.at("view_start").num();
    s.comparison.viewEnd = j.at("view_end").num();

    s.start = j.at("start").num();
    s.end = j.at("end").num();
    s.rangeActive = j.at("range_active").flag();

    const auto page = j.at("page").num(), work = j.at("work_page").num(), columns = j.at("columns").num(),
               metric = j.at("metric").num();
    if (page < 0 || page > 9 || work < 0 || work > 4 || columns < 0 || columns >= (1LL << 22) || metric < 0 ||
        metric > 4)
        throw std::invalid_argument("Invalid session selectors");
    s.page = int(page);
    s.workPage = int(work);
    s.columns = unsigned(columns);
    s.comparison.metric = int(metric);

    s.comparison.a = j.at("a").str();
    s.comparison.b = j.at("b").str();
    s.comparison.aStart = j.at("a_start").num();
    s.comparison.aEnd = j.at("a_end").num();
    s.comparison.bStart = j.at("b_start").num();
    s.comparison.bEnd = j.at("b_end").num();
    s.comparison.relative = j.at("relative").flag();

    const auto status = j.at("board_status").num(), sort = j.at("board_sort").num();
    if (status < -1 || status > 2 || sort < 0 || sort > 1)
        throw std::invalid_argument("Invalid board selectors");
    s.boardStatus = int(status);
    s.boardSort = int(sort);
    s.boardMinimum = j.at("board_minimum").num();
    s.boardScope = j.at("board_scope").str();
    s.boardQuery = j.at("board_query").str();

    s.trendScope = j.at("trend_scope").str();
    s.trendStart = j.at("trend_start").num();
    s.trendEnd = j.at("trend_end").num();
    s.trendStep = j.at("trend_step").num();

    validateSession(s);
    return s;
}

std::string workspaceIncidentKey(const DocumentState& doc, const Outage& outage) {
    return sha256("incident:" + origin(doc, outage.startLine) + ":" + std::to_string(outage.start));
}

LogView workspaceScopeRows(const DocumentState& doc, const std::string& scope) {
    LogView rows;
    for (const auto& source : doc.sources) {
        auto metadata = doc.workspace.devices.find(source.workspaceKey());
        const bool matches = scope.empty() || scope == "source:" + source.workspaceKey() ||
                             (metadata != doc.workspace.devices.end() && !metadata->second.device.empty() &&
                              scope == "device:" + metadata->second.device);
        if (matches)
            for (std::size_t i = source.first; i < source.last; ++i)
                rows.push_back(&doc.lines[i]);
    }

    return rows;
}

std::vector<WorkspaceEvent> workspaceEvents(const DocumentState& doc, const std::string& selected) {
    std::set<std::string> scopes;
    for (const auto& source : doc.sources) {
        auto m = doc.workspace.devices.find(source.workspaceKey());
        const std::string scope = m != doc.workspace.devices.end() && !m->second.device.empty()
                                      ? "device:" + m->second.device
                                      : "source:" + source.workspaceKey();
        if (selected.empty())
            scopes.insert(scope);
    }

    if (!selected.empty())
        scopes.insert(selected);
    std::vector<WorkspaceEvent> events;
    for (const auto& scope : scopes)
        for (const auto& outage : collectOutages(workspaceScopeRows(doc, scope))) {
            WorkspaceEvent e;
            e.outage = outage;
            e.scope = scope;
            e.key = workspaceIncidentKey(doc, outage);
            e.device = scope.compare(0, 7, "device:") == 0 ? scope.substr(7) : "";
            for (const auto& source : doc.sources)
                if (source.first < source.last && outage.startLine >= doc.lines[source.first].lineNo &&
                    outage.startLine <= doc.lines[source.last - 1].lineNo) {
                    e.source = utf8(source.label);
                    auto metadata = doc.workspace.devices.find(source.workspaceKey());
                    if (e.device.empty() && metadata != doc.workspace.devices.end())
                        e.device = metadata->second.device;
                    break;
                }
            auto record = doc.workspace.reviews.find(e.key);
            e.review = record == doc.workspace.reviews.end()
                           ? ReviewRecord{e.key, fmtTime(outage.start, "FULL"), "", ReviewStatus::Pending}
                           : record->second;
            events.push_back(std::move(e));
        }
    std::stable_sort(events.begin(), events.end(),
                     [](const WorkspaceEvent& a, const WorkspaceEvent& b) { return a.outage.start < b.outage.start; });
    return events;
}

std::vector<ChainNode> incidentEvidenceChain(const IncidentReview& review) {
    std::vector<ChainNode> nodes;
    if (!review.valid)
        return nodes;
    for (const auto* row : review.rows) {
        ChainStage stage;
        if (row->lineNo == review.outage.startLine)
            stage = ChainStage::Fault;
        else if (review.outage.recovered && row->lineNo == review.outage.endLine)
            stage = ChainStage::Network;
        else if (row->t >= review.outage.start && row->t <= review.outage.end &&
                 logEventKind(*row) == LogEventKind::RecoveryAction)
            stage = ChainStage::Action;
        else {
            const auto& tag = row->tagText();
            if (tag != "BUSINESS_PROBE" && tag != "APP_PROBE")
                continue;
            const auto f = hbFields(row->msg);
            auto result = f.find("result");
            if (!review.outage.recovered || row->t < review.outage.end || result == f.end() ||
                (result->second != "ok" && result->second != "OK" && result->second != "success"))
                continue;
            stage = ChainStage::Business;
        }

        nodes.push_back({stage, row->t, row->lineNo, row->msg});
    }

    return nodes;
}

std::vector<TrendPeriod> workspaceTrend(const LogView& rows, long long start, long long end, long long step) {
    if (start > end || step < 1 || step > 366LL * 86400 || (static_cast<long double>(end) - start) / step >= 3660)
        throw std::invalid_argument("Trend range exceeds 3660 periods");
    const auto outages = collectOutages(rows);
    const auto metrics = buildMetrics(rows);
    std::vector<TrendPeriod> periods;
    for (long long lo = start;;) {
        const auto distance = static_cast<unsigned long long>(end) - static_cast<unsigned long long>(lo);
        const auto hi = distance < static_cast<unsigned long long>(step) ? end : lo + step - 1;
        TrendPeriod p;
        p.start = lo;
        p.end = hi;
        periods.push_back(p);
        if (hi == end)
            break;
        lo = hi + 1;
    }

    std::vector<LogView> bucketRows(periods.size());
    std::vector<MetricView> bucketMetrics(periods.size());
    const auto bucket = [&](long long t) {
        return static_cast<std::size_t>((static_cast<unsigned long long>(t) - static_cast<unsigned long long>(start)) /
                                        static_cast<unsigned long long>(step));
    };

    for (const auto* row : rows)
        if (row->t >= start && row->t <= end)
            bucketRows[bucket(row->t)].push_back(row);
    for (const auto& metric : metrics)
        if (metric.t >= start && metric.t <= end)
            bucketMetrics[bucket(metric.t)].push_back(&metric);
    for (std::size_t i = 0; i < periods.size(); ++i) {
        auto& p = periods[i];
        p.stats = comparePeriodCached(std::move(bucketRows[i]), bucketMetrics[i], p.start, p.end);
        for (const auto& event : outages) {
            if (event.start >= p.start && event.start <= p.end)
                ++p.starts;
            else if (event.start < p.start && event.end >= p.start)
                ++p.carried;
        }
    }

    return periods;
}

std::string workspaceTrendText(const std::vector<TrendPeriod>& periods, const std::string& scope) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << UiText8(TextId::wb_trend_basis) << "\n" << scope << "\n";
    out << "start\tend\tobserved_s\tstarted_outages\tcarried_outages\twindow_outages\twindow_down_s\truntime_"
           "availability_pct\tfull_availability_pct";
    for (const char* name : {"CSQ", "RSRP", "RSRQ", "SNR_dB", "RSSI"}) {
        out << '\t' << name << "_n\t" << name << "_mean\t" << name << "_p10\t" << name << "_p50\t" << name << "_p90";
    }

    out << "\trecovery_actions\n";
    for (const auto& p : periods) {
        const auto& s = p.stats;
        out << fmtTime(p.start, "FULL") << '\t' << fmtTime(p.end, "FULL") << '\t' << s.observed << '\t' << p.starts
            << '\t' << p.carried << '\t' << s.outages << '\t' << s.down << '\t';
        if (s.availability.runtimeValid())
            out << std::fixed << std::setprecision(3) << s.availability.runtimePercent();
        else
            out << "—";
        out << '\t';
        if (s.availability.fullValid())
            out << std::fixed << std::setprecision(3) << s.availability.fullPercent();
        else
            out << "—";
        int i = 0;
        for (const auto* d : {&s.csq, &s.rsrp, &s.rsrq, &s.snr, &s.rssi}) {
            const double scale = i++ == 3 ? 10.0 : 1.0;
            out << '\t' << d->samples;
            if (d->samples)
                out << '\t' << std::fixed << std::setprecision(3) << d->mean / scale << '\t' << d->p10 / scale << '\t'
                    << d->p50 / scale << '\t' << d->p90 / scale;
            else
                out << "\t—\t—\t—\t—";
        }

        std::size_t actions = 0;
        for (const auto& a : s.recoveryActions)
            actions += a.second;
        out << '\t' << actions << '\n';
    }

    std::set<std::string> actions;
    for (const auto& p : periods)
        for (const auto& item : p.stats.recoveryActions)
            actions.insert(item.first);
    if (!actions.empty()) {
        out << "\n" << UiText8(TextId::compare_recovery) << "\nstart\tend";

        // Keep log-reported recovery stages/levels distinct. Total actions are not successes.
        for (auto action : actions) {
            for (char& c : action)
                if (c == '\t' || c == '\r' || c == '\n')
                    c = ' ';
            out << '\t' << action;
        }

        out << '\n';
        for (const auto& p : periods) {
            out << fmtTime(p.start, "FULL") << '\t' << fmtTime(p.end, "FULL");
            for (const auto& action : actions) {
                const auto count = p.stats.recoveryActions.find(action);
                out << '\t' << (count == p.stats.recoveryActions.end() ? 0 : count->second);
            }

            out << '\n';
        }
    }

    return out.str();
}
}  // namespace dl
