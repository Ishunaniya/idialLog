// build/tests/ 下的程序与导出文件是可删除的本地测试产物；make check 会重新生成，不是应用运行依赖。
#include "version.h"
#include "workspace_state.h"
#include "workspace_tools.h"
#include "log_time.h"
#include "document_state.h"
#include "sha256.h"
#include "json_value.h"
#include "incident_export.h"
#include "findingmodel.h"
#include "logmodel.h"
#include "chartmodel.h"
#include "report_chart.h"
#include <cstdio>
#include <fstream>
#include <limits>
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
using namespace dl;

int main() {
    int failures = 0;
    auto ok = [&](bool pass, const char* name) {
        std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
        if (!pass)
            ++failures;
    };
    auto rejects = [&](auto fn) {
        try {
            fn();
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    ok(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 empty known vector");
    ok(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known abc vector");
    Sha256 stream;
    for (int i = 0; i < 1000000; ++i)
        stream.update("a");
    ok(stream.finish() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
       "SHA-256 streaming million-byte vector");
    auto unicode = parseJson("{\"x\":\"\\ud83d\\ude00\\n\",\"time\":9223372036854775807}");
    ok(unicode.at("x").str() == "\xf0\x9f\x98\x80\n" && unicode.at("time").num() == LLONG_MAX,
       "JSON surrogate and lossless int64");
    for (const char* bad :
         {"{\"x\":1,\"x\":2}", "[01]", "[1.0]", "[9223372036854775808]", "[\"\\ud800\"]", "[]trailing"})
        ok(rejects([&] { parseJson(bad); }), "reject malformed/duplicate/overflow JSON");
    WorkspaceState state;
    const auto aHash = sha256("source-a"), bHash = sha256("source-b"), key = sha256("incident-a");
    state.devices[aHash] = {"设备一", "FW-A", "APN=现场\n=cmd"};
    state.devices[bHash] = {"device-b", "FW-B", ""};
    state.reviews[key] = {key, "真实断网", "人工复核\n<script>\"note\"", ReviewStatus::Confirmed};
    const auto encoded = encodeWorkspace(state);
    auto decoded = decodeWorkspace(encoded);
    ok(decoded.devices[aHash].device == "设备一" && decoded.reviews[key].note == state.reviews[key].note &&
           decoded.reviews[key].status == ReviewStatus::Confirmed,
       "workspace round trip preserves manual content/status");
    auto invalid = parseJson(encoded);
    invalid.object["reviews"].array[0].object["status"] = Json(3LL);
    ok(rejects([&] { decodeWorkspace(writeJson(invalid)); }), "unknown review status rejected atomically");
    DocumentState doc;
    std::vector<std::string> raw = {
        "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | CSQ:18 | RSRP:-95 RSRQ:-8 SNR:120 RSSI:-65",
        "[2026-08-03 10:00:01] [SDK] Ping failed, fault timer started",
        "[2026-08-03 10:00:03] [SDK] Network recovered after 2s",
        "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | CSQ:8 | RSRP:-115 RSRQ:-19 SNR:-20 RSSI:-100",
        "[2026-08-03 10:00:02] [SDK] Ping failed, fault timer started",
        "[2026-08-03 10:00:09] [SDK] Network recovered after 7s"};
    parseLines(raw, doc.lines, doc.sessions, &doc.audit, {0, 3});
    SourceSummary source;
    source.label = L"A.log";
    source.first = 0;
    source.last = 3;
    source.originalHash = aHash;
    doc.sources.push_back(source);
    source.label = L"B.log";
    source.first = 3;
    source.last = 6;
    source.rawLineOffset = 3;
    source.originalHash = bHash;
    doc.sources.push_back(source);
    doc.workspace = state;
    LogView rows;
    for (const auto& row : doc.lines)
        rows.push_back(&row);
    doc.sourceMode = DocumentState::SourceMode::Device;
    doc.selectedDevice = "设备一";
    auto selected = rows;
    doc.restrictToSelection(selected);
    ok(selected.size() == 3 && selected.front() == &doc.lines[0],
       "device group excludes other device despite overlapping times");
    doc.selectedDevice = "";
    selected = rows;
    doc.restrictToSelection(selected);
    ok(selected.empty(), "unknown device cannot merge all sources");
    doc.workspace.devices[bHash].device = "设备一";
    doc.selectedDevice = "设备一";
    selected = rows;
    doc.restrictToSelection(selected);
    ok(selected.size() == 6, "manual same-device grouping includes both sources");
    LogView a(rows.begin(), rows.begin() + 3), b(rows.begin() + 3, rows.end());
    auto as = comparePeriod(a, doc.lines[0].t, doc.lines[2].t), bs = comparePeriod(b, doc.lines[3].t, doc.lines[5].t);
    ok(as.outages == 1 && bs.outages == 1 && as.down == 2 && bs.down == 7 && as.rssi.mean == -65 &&
           bs.rssi.mean == -100,
       "period comparison isolates real outage durations and RSSI");
    ok(as.snr.samples == 1 && as.snr.mean == 120 && bs.snr.mean == -20, "SNR distribution excludes missing sentinel");
    auto clipped = comparePeriod(b, doc.lines[4].t, doc.lines[4].t);
    ok(clipped.open == 1 && clipped.rssi.samples == 0, "clipped period retains open boundary and missing RSSI");
    auto events = workspaceEvents(doc);
    ok(events.size() == 2 && events[0].outage.dur == 2 && events[1].outage.dur == 7,
       "board enumerates assigned device events without cross-source fusion");
    ok(workspaceEvents(doc, "source:" + aHash).size() == 1 &&
           workspaceEvents(doc, "source:" + aHash)[0].device == "设备一" &&
           workspaceEvents(doc, "device:设备一").size() == 2 && workspaceScopeRows(doc, "device:missing").empty(),
       "board device and source scopes select exact originals");
    ok(events[0].key == sha256("incident:" + aHash + ":2:" + std::to_string(doc.lines[1].t)) &&
           events[0].review.status == ReviewStatus::Pending,
       "board keys match existing source-local review identities");
    doc.workspace.reviews[events[0].key] = {events[0].key, "confirmed", "manual only", ReviewStatus::Handled};
    events = workspaceEvents(doc);
    ok(events[0].review.note == "manual only" && events[0].review.status == ReviewStatus::Handled,
       "board manual annotation attaches only to its incident");
    auto fullMetrics = buildMetrics(b);
    MetricView cached;
    for (const auto& m : fullMetrics)
        cached.push_back(&m);
    auto cachedStats = comparePeriodCached(b, cached, doc.lines[3].t, doc.lines[5].t);
    ok(cachedStats.rssi.samples == bs.rssi.samples && cachedStats.rssi.p50 == bs.rssi.p50 &&
           cachedStats.down == bs.down && cachedStats.outages == bs.outages,
       "cached period calculation retains real signal and outage statistics");
    WorkspaceSession session;
    session.name = "现场会话 <&>";
    session.tag = "HEARTBEAT";
    session.message = "CSQ";
    session.rangeActive = true;
    session.start = doc.lines[0].t;
    session.end = doc.lines[2].t;
    session.columns = 1u | 32u;
    session.cell = "01ABCDEF";
    session.hasDeny = true;
    session.deny = 15;
    session.comparison.a = "source:" + aHash;
    session.comparison.b = "device:设备一";
    session.comparison.aStart = session.comparison.bStart = doc.lines[0].t;
    session.comparison.aEnd = doc.lines[2].t;
    session.comparison.bEnd = doc.lines[5].t;
    session.comparison.viewActive = true;
    session.comparison.viewStart = 1;
    session.comparison.viewEnd = 3;
    session.comparison.metric = 4;
    session.boardStatus = 2;
    session.boardQuery = "manual";
    session.boardMinimum = 2;
    session.boardScope = "device:设备一";
    session.trendScope = "source:" + aHash;
    session.trendStart = doc.lines[0].t;
    session.trendEnd = doc.lines[2].t;
    auto restored = decodeSession(encodeSession(session));
    ok(restored.name == session.name && restored.comparison.metric == 4 && restored.comparison.viewStart == 1 &&
           restored.cell == session.cell && restored.boardMinimum == 2 && restored.boardStatus == 2 &&
           restored.columns == 33,
       "session round trip preserves filters, selectors and plot view");
    const auto sessionJson = parseJson(encodeSession(session));
    for (const auto* field : {"page", "work_page", "columns", "metric", "board_status", "board_sort", "deny"}) {
        auto bad = sessionJson;
        bad.object[field] = Json(LLONG_MAX);
        ok(rejects([&] { decodeSession(writeJson(bad)); }), "session rejects overflowing selectors before narrowing");
    }
    auto invalidSession = session;
    invalidSession.comparison.viewEnd = 10;
    ok(rejects([&] { encodeSession(invalidSession); }), "session rejects plot views outside their comparison periods");
    invalidSession = session;
    invalidSession.trendStep = 0;
    ok(rejects([&] { encodeSession(invalidSession); }), "session rejects zero period length");
    invalidSession = session;
    invalidSession.comparison.aStart = LLONG_MIN;
    ok(rejects([&] { encodeSession(invalidSession); }), "session rejects unsafe date values");
    std::vector<LogLine> chainRows;
    std::vector<std::string> chainSessions;
    parseLines({"[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | CSQ:18 | RSRP:-95 RSRQ:-8 SNR:120 RSSI:-65",
                "[2026-08-03 10:00:01] [SDK] Ping failed, fault timer started",
                "[2026-08-03 10:00:02] [RECOVERY] class=PDP level=L3_CFUN action=cycle",
                "[2026-08-03 10:00:03] [SDK] Network recovered after 2s",
                "[2026-08-03 10:00:04] [HEARTBEAT] CH:SIM | CSQ:18 | RX:100 DRX:10",
                "[2026-08-03 10:00:05] [APP_PROBE] result=failed", "[2026-08-03 10:00:06] [BUSINESS_PROBE] result=ok"},
               chainRows, chainSessions);
    LogView chainView;
    for (const auto& row : chainRows)
        chainView.push_back(&row);
    auto chainCatalog = buildIncidentCatalog(chainView);
    auto chainReview = reviewIncident(chainCatalog, 0, 300, 300);
    auto chain = incidentEvidenceChain(chainReview);
    ok(chain.size() == 4 && chain[0].stage == ChainStage::Fault && chain[1].stage == ChainStage::Action &&
           chain[2].stage == ChainStage::Network && chain[3].stage == ChainStage::Business && chain[3].line == 7,
       "evidence chain follows explicit fault/action/network/business source rows");
    chainView.pop_back();
    chainCatalog = buildIncidentCatalog(chainView);
    chain = incidentEvidenceChain(reviewIncident(chainCatalog, 0, 300, 300));
    ok(chain.size() == 3 && chain.back().stage == ChainStage::Network,
       "traffic increase and failed app probes do not prove business recovery");
    auto periods = workspaceTrend(chainView, chainRows[0].t, chainRows[0].t + 15, 4);
    ok(periods.size() == 4 && periods[0].start == chainRows[0].t && periods[0].end + 1 == periods[1].start &&
           periods[2].stats.samples == 0 && periods[3].stats.samples == 0,
       "trends keep disjoint inclusive boundaries and missing periods");
    ok(periods[0].starts == 1 && periods[1].starts == 0 && periods[0].stats.rssi.samples == 1 &&
           periods[1].stats.rssi.samples == 0,
       "trends separate incident starts from missing metric values");
    ok(workspaceTrendText(periods, "A").find("RECOVERY / level=L3_CFUN / action=cycle") != std::string::npos,
       "trend output retains real recovery levels rather than only a total");
    const auto carriedPeriods = workspaceTrend(b, doc.lines[3].t, doc.lines[5].t, 3);
    ok(carriedPeriods[0].starts == 1 && carriedPeriods[1].carried == 1 && carriedPeriods[2].carried == 1 &&
           carriedPeriods[3].starts == 0,
       "incident carry-over is counted without repeating its start");
    ok(rejects([&] { workspaceTrend(chainView, 0, 3660, 1); }) &&
           rejects([&] { workspaceTrend(chainView, 10, 0, 1); }) &&
           rejects([&] { workspaceTrend(chainView, 0, 10, 0); }),
       "trend rejects excessive/reversed/zero-step ranges");
    auto edge = workspaceTrend({}, LLONG_MAX, LLONG_MAX, 1);
    ok(edge.size() == 1 && edge[0].end == LLONG_MAX && edge[0].stats.samples == 0,
       "single extreme timestamp has no arithmetic overflow or fabricated data");
    SetEnglish(true);
    auto report = periodComparisonText(as, bs, "FW-A", "FW-B", true);
    ok(report.find("-35.000") != std::string::npos && report.find("manually confirmed") != std::string::npos &&
           report.find("does not establish") == std::string::npos,
       "comparison carries difference and manual relation without causality verdict");
    std::vector<LogLine> recoveryRows;
    std::vector<std::string> recoverySessions;
    parseLines({"[2026-08-03 10:00:00] [RECOVERY] class=DATA_PATH level=L2_PDP action=enter",
                "[2026-08-03 10:00:01] [RECOVERY] class=PDP level=L3_CFUN action=cycle"},
               recoveryRows, recoverySessions);
    LogView recoveryView;
    for (const auto& row : recoveryRows)
        recoveryView.push_back(&row);
    auto recoveryStats = comparePeriod(recoveryView, recoveryRows.front().t, recoveryRows.back().t);
    ok(recoveryStats.recoveryActions["RECOVERY / level=L2_PDP / action=enter"] == 1 &&
           recoveryStats.recoveryActions["RECOVERY / level=L3_CFUN / action=cycle"] == 1,
       "reported recovery levels and actions remain distinct");
    std::vector<LogLine> v2Rows;
    std::vector<std::string> v2Sessions;
    parseLines({"2026-08-03T10:00:00 host modem_mng_v2[321]: CEREG stat=1 lac=1234 ci=01ABCDEF act=0",
                "2026-08-03T10:00:01 host modem_mng_v2[321]: [READY] CSQ: 20 (sim_present=1 online=1)"},
               v2Rows, v2Sessions);
    LogView v2View;
    for (const auto& row : v2Rows)
        v2View.push_back(&row);
    auto v2Stats = comparePeriod(v2View, v2Rows.back().t, v2Rows.back().t);
    ok(v2Stats.samples == 1 && v2Stats.csq.samples == 0, "period keeps pre-window non-LTE registration evidence");
    auto win = navigateChartWindow({100, 200}, {0, 300}, 150, .5, 0);
    ok(win.start == 125 && win.end == 175, "anchor-centred wheel zoom");
    win = navigateChartWindow(win, {0, 300}, 150, 1, 10);
    ok(win.start == 250 && win.end == 300, "pan clamps to source bounds");
    win = navigateChartWindow({100, 101}, {0, 300}, 100, .1, 0);
    ok(win.end - win.start == 1, "zoom minimum one second");
    ok(findingBrief("观察到 2 次失败。复述说明。后续【推断】无法确定原因。").find("后续【推断】无法确定原因。") !=
           std::string::npos,
       "concise view retains full late inference sentence");
    ok(findingBrief("Observed -99.5 dBm. Repeated guidance. It does not prove an outage.")
               .find("Observed -99.5 dBm.") == 0,
       "concise view keeps decimals and full negative qualifier");
    ok(findingBrief("Observed failures. Repeated description. Only reported samples are compared. Insufficient "
                    "evidence to establish a cause.")
                   .find("Only reported samples") != std::string::npos &&
           findingBrief("Observed failures. Repeated description. Insufficient evidence to establish a cause.")
                   .find("Insufficient evidence") != std::string::npos,
       "concise view retains uppercase English scope and evidence limits");
    ok(findingBrief("观察到失败。重复说明。尚未确认根因，证据不足。").find("尚未确认根因，证据不足。") !=
           std::string::npos,
       "concise view retains incomplete Chinese evidence qualifier");
    SetEnglish(false);
    auto catalog = buildIncidentCatalog(a);
    auto review = reviewIncident(catalog, 0, 300, 300);
    IncidentExportContext context;
    context.version = DL_VER_STR;
    context.sources = {{"A.log", 1, 1, 3, 0}, {"B.log", 2, 4, 6, 3}};
    std::string aBytes = "\xef\xbb\xbf";
    for (int i = 0; i < 3; ++i)
        aBytes += raw[i] + "\r\n";
    std::string bBytes;
    for (int i = 3; i < 6; ++i)
        bBytes += raw[i] + "\n";
    context.originals = {{"", "A.log", aBytes, sha256(aBytes), sha256(aBytes)},
                         {"", "B.log", bBytes, sha256(bBytes), sha256(bBytes)}};
    context.workspace = state;
    context.workspace.devices.clear();
    context.workspace.devices[sha256(aBytes)] = state.devices[aHash];
    context.workspace.devices[sha256(bBytes)] = state.devices[bHash];
    context.selectedSourceHash = sha256(aBytes);
    context.selectedDevice = "设备一";
    context.bookmarks = {{2, "现场备注"}};
    context.reviews.push_back(state.reviews[key]);
    auto bytes = buildIncidentZip(review, context);
    auto package = readEvidencePackage(bytes);
    ok(package.originals.size() == 2 && package.originals[0].bytes == aBytes && package.originals[1].bytes == bBytes,
       "evidence round trip preserves original BOM/CRLF bytes");
    ok(package.workspace.reviews[key].note == state.reviews[key].note && package.selectedDevice == "设备一" &&
           package.bookmarks.size() == 1 && package.bookmarks[0].line == 2,
       "package restores manual reviews, group selection and source-local bookmarks");
    std::ofstream("build/tests/unit/workspace-package.zip", std::ios::binary).write(bytes.data(), bytes.size());
    auto invalidContext = context;
    invalidContext.originals[1].identity = invalidContext.originals[0].identity;
    ok(rejects([&] { readEvidencePackage(buildIncidentZip(review, invalidContext)); }),
       "duplicate source identities rejected");
    invalidContext = context;
    invalidContext.selectedSourceHash = sha256("unknown");
    ok(rejects([&] { readEvidencePackage(buildIncidentZip(review, invalidContext)); }),
       "unknown source selection rejected");
    invalidContext = context;
    invalidContext.workspace.devices[sha256("unknown")] = {"device", "", ""};
    ok(rejects([&] { readEvidencePackage(buildIncidentZip(review, invalidContext)); }),
       "unrelated device source rejected");
    auto tooManyOriginals = context;
    tooManyOriginals.originals.resize(1001);
    for (auto& original : tooManyOriginals.originals)
        if (original.hash.empty())
            original.hash = sha256(original.bytes);
    ok(rejects([&] { buildIncidentZip(review, tooManyOriginals); }), "export source count matches import limit");
    WorkspaceState tooManyDevices;
    for (int i = 0; i < 1001; ++i)
        tooManyDevices.devices[sha256(std::to_string(i))] = {};
    ok(rejects([&] { encodeWorkspace(tooManyDevices); }), "workspace writer rejects records its reader cannot reopen");
    WorkspaceSession packageSession = session;
    packageSession.comparison.a = "source:" + sha256(aBytes);
    packageSession.comparison.b = "device:设备一";
    packageSession.trendScope = "source:" + sha256(aBytes);
    packageSession.boardScope = "device:设备一";
    auto sessionBytes = buildWorkspaceSessionZip(context, packageSession);
    auto reopened = readEvidencePackage(sessionBytes);
    ok(reopened.hasSession && reopened.originals.size() == 2 && reopened.session.boardStatus == 2 &&
           reopened.session.cell == session.cell && reopened.originals[0].bytes == aBytes,
       "full session package restores original bytes and all user selections");
    std::ofstream("build/tests/unit/workspace-session.zip", std::ios::binary)
        .write(sessionBytes.data(), sessionBytes.size());
    ok(hasEvidencePackageIndex(sessionBytes) && !hasEvidencePackageIndex("ordinary log"),
       "package recognition reads the ZIP index before generic archive limits");
    IncidentExportContext boundaryContext;
    boundaryContext.version = DL_VER_STR;
    for (int i = 0; i < 1000; ++i) {
        auto body = raw[0] + " #" + std::to_string(i) + "\n";
        auto hash = sha256(body);
        boundaryContext.sources.push_back(
            {"source" + std::to_string(i), std::size_t(i + 1), std::size_t(i + 1), std::size_t(i + 1), std::size_t(i)});
        boundaryContext.originals.push_back({"", "source" + std::to_string(i), body, hash, hash});
    }
    auto boundaryZip = buildWorkspaceSessionZip(boundaryContext, WorkspaceSession{});
    ok(hasEvidencePackageIndex(boundaryZip) && readEvidencePackage(boundaryZip).originals.size() == 1000,
       "1000-source session includes index and metadata without hitting the ordinary archive entry cap");
    auto unknownSession = packageSession;
    unknownSession.boardScope = "source:" + sha256("unknown");
    ok(rejects([&] { readEvidencePackage(buildWorkspaceSessionZip(context, unknownSession)); }),
       "session package rejects unknown board source identity");
    IncidentExportContext noOutage;
    noOutage.version = DL_VER_STR;
    noOutage.sources = {{"pasted.log", 1, 1, 1, 0}};
    noOutage.originals = {{"", "pasted.log", raw[0] + "\n", sha256(raw[0] + "\n"), sha256(raw[0] + "\n")}};
    WorkspaceSession cleanSession;
    auto clean = readEvidencePackage(buildWorkspaceSessionZip(noOutage, cleanSession));
    ok(clean.hasSession && clean.originals.size() == 1 && clean.originals[0].bytes == raw[0] + "\n",
       "work session needs no outage and preserves pasted input");
    context.originals[0].bytes += "changed";
    ok(rejects([&] { buildIncidentZip(review, context); }), "export rejects changed original");
    auto corrupted = bytes;
    corrupted[corrupted.size() / 2] ^= 0x40;
    ok(rejects([&] { readEvidencePackage(corrupted); }), "corrupt ZIP rejected without partial workspace");
    auto makeZip = [](const std::vector<std::pair<std::string, std::string>>& files) {
        mz_zip_archive z{};
        mz_zip_writer_init_heap(&z, 0, 0);
        for (const auto& f : files)
            mz_zip_writer_add_mem(&z, f.first.c_str(), f.second.data(), f.second.size(), MZ_BEST_SPEED);
        void* buffer = nullptr;
        std::size_t size = 0;
        mz_zip_writer_finalize_heap_archive(&z, &buffer, &size);
        std::string out(static_cast<const char*>(buffer), size);
        mz_free(buffer);
        mz_zip_writer_end(&z);
        return out;
    };
    ok(rejects([&] { readEvidencePackage(makeZip({{"report.md", "hello"}, {"../outside", "bad"}})); }),
       "traversal entry rejected without extraction");
    ok(rejects([&] { readEvidencePackage(makeZip({{"report.md", "one"}, {"report.md", "two"}})); }),
       "duplicate ZIP paths rejected");
    ReportChartOptions aggregate;
    aggregate.title = "RSSI period P50";
    aggregate.unit = "dBm";
    aggregate.start = 0;
    aggregate.end = 259199;
    aggregate.low = -120;
    aggregate.high = -20;
    aggregate.color = "#2563eb";
    aggregate.gapSeconds = 86400;
    aggregate.periods = {{0, 86399, 2, -65}, {86400, 172799, 0, 0}, {172800, 259199, 3, -80}};
    auto aggregateReport = renderReportChart({{0, -65}, {172800, -80}}, {}, aggregate);
    ok(aggregateReport.segments == 2 &&
           aggregateReport.html.find("\"periods\":[[0,86399,2,-65],[86400,172799,0,0]") != std::string::npos &&
           aggregateReport.html.find("P50 (n=2)") != std::string::npos,
       "aggregate chart retains missing periods and interval/sample-count hover semantics");
    std::ofstream("build/tests/unit/workspace-trend.html")
        << "<!doctype html><html><meta charset=utf-8>" << aggregateReport.html << reportChartScript(true) << "</html>";
    ReportChartOptions chart;
    chart.start = 0;
    chart.end = 20;
    chart.title = "RSSI";
    chart.unit = "dBm";
    chart.low = -120;
    chart.high = -20;
    chart.color = "#2563eb";
    chart.comparisonMode = true;
    chart.comparison = {{0, -100}, {10, -95}};
    chart.relative = true;
    chart.originalStart = doc.lines[0].t;
    chart.comparisonOriginalStart = doc.lines[3].t;
    auto html = renderReportChart({{0, -65}, {10, -60}}, {}, chart).html;
    ok(html.find("stroke-dasharray=\"7 4\"") != std::string::npos &&
           html.find("\"second\":[[0,-100],[10,-95]]") != std::string::npos &&
           html.find("1970-01-01") == std::string::npos,
       "dual report curves preserve original hover dates and relative axis");
    chart.comparison.clear();
    chart.title = "RSRQ";
    auto empty = renderReportChart({}, {}, chart).html;
    ok(empty.find("chart-data") != std::string::npos && empty.find("\"points\":[]") != std::string::npos,
       "empty metrics participate in shared hover as missing samples");
    ok(html.find("B 采样 2") != std::string::npos && empty.find("B 采样 0") != std::string::npos &&
           html.find("\"compare\":true") != std::string::npos,
       "comparison captions and hover identify both sources even when B is missing");
    std::ofstream("build/tests/unit/workspace-comparison.html")
        << "<!doctype html><meta charset=utf-8>" << html << empty << reportChartScript(false);
    auto singleton = chart;
    singleton.start = singleton.end = 10;
    singleton.title = "RSSI";
    singleton.comparisonMode = false;
    singleton.relative = false;
    std::ofstream("build/tests/unit/workspace-singleton.html")
        << "<!doctype html><meta charset=utf-8>" << renderReportChart({{10, -65}}, {}, singleton).html
        << reportChartScript(false);
    DocumentState other;
    other.swap(doc);
    ok(other.workspace.reviews.size() == 2 && other.workspace.reviews.count(events[0].key) == 1 &&
           other.selectedDevice == "设备一" && doc.workspace.reviews.empty(),
       "document swap owns workspace safely");
    other.release();
    ok(other.workspace.reviews.empty() && other.selectedDevice.empty(), "unload releases workspace metadata");
    return failures ? 1 : 0;
}
