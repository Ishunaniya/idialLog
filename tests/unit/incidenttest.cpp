#include "incidentmodel.h"
#include "incident_export.h"
#include "rssi_summary.h"
#include "log_parser.h"
#include "log_analysis.h"
#include "log_filter.h"
#include "log_time.h"
#include "tablemodel.h"
#include "text_catalog.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>
using namespace dl;
static int failures=0;
static void ok(bool value,const char* what){std::printf("%s %s\n",value?"PASS":"FAIL",what);if(!value)++failures;}
static LogView view(const std::vector<LogLine>& lines){LogView result;for(const auto& l:lines)result.push_back(&l);return result;}
static std::map<std::string,std::string> unzip(const std::string& bytes) {
    std::map<std::string,std::string> files;mz_zip_archive zip{};
    if(!mz_zip_reader_init_mem(&zip,bytes.data(),bytes.size(),0))return files;
    for(mz_uint i=0;i<mz_zip_reader_get_num_files(&zip);++i){mz_zip_archive_file_stat st{};if(!mz_zip_reader_file_stat(&zip,i,&st))continue;
        size_t size=0;void* data=mz_zip_reader_extract_to_heap(&zip,i,&size,0);
        if(data){files[st.m_filename]=std::string(static_cast<char*>(data),size);mz_free(data);}}
    mz_zip_reader_end(&zip);return files;
}
int main() {
    std::vector<LogLine> lines;std::vector<std::string> sessions;
    std::vector<std::string> raw={
        "=== Dial Log Opened [2026-09-01 10:00:00] ===","",
        "[2026-09-01 10:00:00] [HEARTBEAT] CH:SIM | CSQ:18 | RSRP:-95 | RSRQ:-8 | SNR:120 | RSSI:-65",
        "[2026-09-01 10:00:10] [NET] Network outage started",
        "[2026-09-01 10:00:12] [RECOVERY L2] AT+CFUN=0 rsp: OK",
        "[2026-09-01 10:00:15] [NET] Network recovered after 5s",
        "[2026-09-01 10:00:17] [HEARTBEAT] CH:SIM | CSQ:20"};
    parseLines(raw,lines,sessions,nullptr);auto catalog=buildIncidentCatalog(view(lines));
    ok(catalog.outages.size()==1,"one outage, recovery action is not a recovery edge");
    if(catalog.outages.empty())return 1;
    auto review=reviewIncident(catalog,0,2,2);
    ok(review.valid && review.outage.startLine==4 && review.outage.endLine==6 && review.outage.dur==5,
       "review preserves concrete start/recovery evidence and five seconds duration");
    ok(review.metrics.size()==1 && review.start==mkEpoch(2026,9,1,10,0,8) && review.end==mkEpoch(2026,9,1,10,0,17) && review.rows.size()==4,
       "two-second margins keep four rows and one post-recovery sample");
    ok(review.events.size()==3 && review.events[1]->lineNo==5 && review.metrics.size()==1,
       "three events keep intermediate CFUN action, one post-recovery signal sample");
    const auto filtered=collectOutages(LogView{&lines[1]});
    ok(filtered.size()==1 && !filtered.front().recovered && findIncident(catalog,filtered.front())==0,
       "filtered open incident resolves against full source scope");
    ok(!reviewIncident(catalog,1).valid && !incidentContainsLine(review,3) && incidentContainsLine(review,6),
       "invalid index and exact evidence membership");
    std::vector<LogLine> qengRows;std::vector<std::string> qengSessions;
    parseLines({"[2026-09-01 10:00:09] [AT] +QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,00,1D8DE0B,274,1300,3,5,5,272D,-104,-10,-65,25",
                "[2026-09-01 10:00:10] [OUTAGE] Network outage started",
                "[2026-09-01 10:00:10] [OUTAGE] Network recovered after 0s",
                "[2026-09-01 10:00:11] [HEARTBEAT] CH:SIM | CSQ:20"},qengRows,qengSessions,nullptr);
    auto qr=reviewIncident(buildIncidentCatalog(view(qengRows)),0,0,2);
    ok(qr.valid && qr.rows.size()==3 && qr.metrics.size()==1 && qr.metrics[0].rsrp==-104 &&
       qr.metrics[0].rssiVal==-65 && qr.metrics[0].cellId=="1D8DE0B",
       "short review keeps exact QENG signal/cell snapshot from before the observation window");
    MetricRow edge;edge.t=review.outage.start;edge.lineNo=3;
    ok(incidentSamplePhase(review,edge)==IncidentPhase::Before,"same-second sample before onset proof row is before fault");
    edge.t=review.outage.end;edge.lineNo=5;
    ok(incidentSamplePhase(review,edge)==IncidentPhase::During,"same-second sample before recovery proof row is during fault");
    edge.lineNo=6;
    ok(incidentSamplePhase(review,edge)==IncidentPhase::After,"recovery edge sample is classified with recovered observations");
    // Streamed source offsets include headers, empty lines and unparsed input.
    std::vector<LogLine> merged;std::vector<std::string> mergedSessions;StreamingLogParser parser(merged,mergedSessions);
    parser.beginFile();for(const auto& r:raw)parser.pushLine(r);
    parser.beginFile();parser.pushLine("");parser.pushLine("unparsed heading");
    parser.pushLine("[2026-09-01 10:00:20] [HEARTBEAT] CH:SIM | CSQ:8");parser.finish();
    const auto source=evidenceOrigin({{"A.log",1,3,7,0},{"B.log",2,10,10,7}},10);
    ok(merged.back().lineNo==10 && source.index==2 && source.line==3 && source.label=="B.log",
       "global row 10 maps to source B original row 3, not parsed-row ordinal");
    const auto missing=evidenceOrigin({},10);ok(missing.index==0 && missing.line==0,"missing origin remains unknown");
    IncidentExportContext context;context.scope="source A";context.version="1.12.0";context.build="test";
    ok(incidentSummaryText(review,context).find("RSSI：当前范围无有效上报") != std::string::npos &&
       rssiSummaryText(observedRssi(qr.metrics)).find("-65 / -65.0 / -65 dBm") != std::string::npos,
       "review RSSI uses its own window and carries actual QENG values, never outside samples");
    context.sources={{"A.log",1,3,7,0},{"B.log",2,10,10,7}};
    context.bookmarks={{6,"reviewed\n\"evidence\""},{3,"outside window"}};
    lines[2].msg+="\n=cmd|\"test\"\t"; // Controls and quotes in an action's original evidence.
    auto zip=buildIncidentZip(review,context);std::ofstream("build/tests/unit/incident-fixture.zh.zip",std::ios::binary).write(zip.data(),zip.size());
    auto files=unzip(zip);
    ok(files.size()==6 && files.count("manifest.json") && files.count("parsed-evidence.jsonl"),"six readable ZIP entries, including explicit provenance");
    ok(files["manifest.json"].find("\"original_bytes_included\":false")!=std::string::npos &&
       files["manifest.json"].find("\"start_line\":4")!=std::string::npos && files["manifest.json"].find("\"samples\":1")!=std::string::npos,
       "manifest identifies parsed evidence, exact boundaries and sample count");
    ok(files["parsed-evidence.jsonl"].find("\\u000a=cmd|\\\"test\\\"\\u0009")!=std::string::npos &&
       files["bookmarks.jsonl"].find("outside window")==std::string::npos &&
       files["bookmarks.jsonl"].find("\"merged_line\":6")!=std::string::npos,"JSON escaping preserves evidence; only window bookmarks are exported");
    SetEnglish(true);
    ok(incidentSummaryText(qr,context).find("Valid RSSI samples: 1") != std::string::npos &&
       incidentSummaryText(review,context).find("RSSI: no valid readings") != std::string::npos,
       "English RSSI summary and missing-data state follow the selected language");
    auto englishZip=buildIncidentZip(review,context);std::ofstream("build/tests/unit/incident-fixture.en.zip",std::ios::binary).write(englishZip.data(),englishZip.size());
    auto english=unzip(englishZip);
    ok(english["report.md"].find("Incident review")!=std::string::npos && english["manifest.json"].find("en-US")!=std::string::npos &&
       english["parsed-evidence.jsonl"]==files["parsed-evidence.jsonl"],"language switch translates report but leaves raw evidence intact");SetEnglish(false);
    // Self-reported recovery-only incident keeps a derived start with one proof row.
    std::vector<LogLine> reported;std::vector<std::string> unused;
    parseLines({"[2026-09-01 10:00:15] [NET] Network Recovered. Down: 5s"},reported,unused,nullptr);
    auto reportedCatalog=buildIncidentCatalog(view(reported));auto rr=reviewIncident(reportedCatalog,0,0,0);
    ok(rr.valid && rr.outage.reportedDuration && rr.outage.startLine==rr.outage.endLine && rr.events.size()==1 &&
       incidentSummaryText(rr,context).find("推算")!=std::string::npos,"recovery-only duration does not invent independent fault evidence");
    // An unsynchronised open incident must not consume wall-clock observations.
    std::vector<LogLine> clock;std::vector<std::string> clockSessions;
    parseLines({"[1970-01-01 00:00:10] [NET] Network outage started",
                "[1970-01-01 00:00:20] [RECOVERY L2] AT+CFUN=0 rsp: OK",
                "[2026-09-01 10:00:15] [HEARTBEAT] CH:SIM | CSQ:18"},clock,clockSessions,nullptr);
    auto cr=reviewIncident(buildIncidentCatalog(view(clock)),0,300,300);
    ok(cr.valid && !cr.outage.recovered && cr.clockLimited && cr.end==20 && cr.rows.size()==2,
       "1970-to-wall-clock jump stays open with a 20-second observation endpoint");
    // Core time-base detection allows one day around 2000-01-01 for time zones.
    // A recovery across UTC midnight must stay in the same review clock domain.
    std::vector<LogLine> boundary;std::vector<std::string> boundarySessions;
    parseLines({"[1999-12-31 23:59:55] [NET] Network outage started",
                "[2000-01-01 00:00:00] [HEARTBEAT] CH:SIM | CSQ:18 | RSSI:-65",
                "[2000-01-01 00:00:05] [NET] Network recovered after 10s"},
               boundary,boundarySessions,nullptr);
    auto boundaryCatalog=buildIncidentCatalog(view(boundary));
    auto boundaryReview=reviewIncident(boundaryCatalog,0,0,0);
    ok(timeBaseOf({"[1999-12-31 23:59:55] [NET] before"})==TB_WALL &&
       boundaryCatalog.outages.size()==1 && boundaryCatalog.outages[0].recovered &&
       boundaryReview.valid && !boundaryReview.clockLimited && boundaryReview.rows.size()==3 &&
       boundaryReview.events.size()==2 && boundaryReview.metrics.size()==1 &&
       incidentContainsLine(boundaryReview,boundaryReview.outage.endLine),
       "review uses the parser's time-zone allowance and preserves recovery across 2000 midnight");
    // Concurrent sources with the same timestamps cannot supply each other's end.
    auto independent=buildIncidentCatalog(LogView{&merged.back()});
    ok(independent.outages.empty(),"unrelated heartbeat-only source has no incident");
    std::vector<LogLine> devices;std::vector<std::string> deviceSessions;
    StreamingLogParser deviceParser(devices,deviceSessions);
    deviceParser.beginFile();deviceParser.pushLine("[2026-09-01 10:00:10] [OUTAGE] Network outage started");
    deviceParser.beginFile();deviceParser.pushLine("[2026-09-01 10:00:15] [OUTAGE] Network recovered after 5s");deviceParser.finish();
    const auto deviceA=buildIncidentCatalog(LogView{&devices[0]});const auto deviceB=buildIncidentCatalog(LogView{&devices[1]});
    ok(deviceA.outages.size()==1 && !deviceA.outages.front().recovered && deviceB.outages.empty(),
       "independent A onset cannot close against an unrelated B recovery at the same timestamps");
    const auto continued=buildIncidentCatalog(view(devices));
    ok(continued.outages.size()==1 && continued.outages.front().recovered && continued.outages.front().dur==5,
       "explicit same-device continuation preserves the existing core's cross-file pairing");
    std::vector<LogLine> interfaceRows;std::vector<std::string> interfaceSessions;
    parseLines({"[2026-09-01 10:00:00] [HEARTBEAT-NET] IF=ccinet0",
                "[2026-09-01 10:00:10] [HEARTBEAT-NET] IF=(none)",
                "[2026-09-01 10:00:15] [HEARTBEAT-NET] IF=ccinet0"},interfaceRows,interfaceSessions,nullptr);
    auto interfaceReview=reviewIncident(buildIncidentCatalog(view(interfaceRows)),0,0,0);
    ok(interfaceReview.valid && interfaceReview.outage.dur==5 && interfaceReview.events.size()==2 &&
       interfaceReview.events.front()->lineNo==2 && interfaceReview.events.back()->lineNo==3,
       "interface boundary heartbeats remain evidence even when excluded by the generic timeline classifier");
    std::ifstream sample("samples/rtms_eg25/dial_20260630_000026.log");std::vector<std::string> sampleRaw;std::string text;
    while(std::getline(sample,text))sampleRaw.push_back(text);
    std::vector<LogLine> real;std::vector<std::string> realSessions;parseLines(sampleRaw,real,realSessions,nullptr);
    const auto realCatalog=buildIncidentCatalog(view(real));
    ok(realCatalog.outages.size()==36,"real EG25 log keeps its 36 known outages");
    const std::size_t firstProof[][5]={{25,29,45,9,22},{103,108,66,35,20},{139,160,83,41,25}};
    for(std::size_t i=0;i<realCatalog.outages.size();++i){auto r=reviewIncident(realCatalog,i);
        if(i<3)ok(r.outage.startLine==firstProof[i][0] && r.outage.endLine==firstProof[i][1] &&
                  r.rows.size()==firstProof[i][2] && r.events.size()==firstProof[i][3] && r.metrics.size()==firstProof[i][4],
                  "first three real windows have exact independently pinned boundaries and row/sample counts");
        ok(r.valid && incidentContainsLine(r,r.outage.startLine) && incidentContainsLine(r,r.outage.endLine),"real incident window contains both exact proof rows");
        std::printf("baseline incident %zu start=%zu end=%zu dur=%d rows=%zu events=%zu metrics=%zu\n",i,r.outage.startLine,r.outage.endLine,r.outage.dur,r.rows.size(),r.events.size(),r.metrics.size());}
    if(!realCatalog.outages.empty()) {
        auto realReview=reviewIncident(realCatalog,0);auto realExport=unzip(buildIncidentZip(realReview,context));
        ok(incidentSummaryText(realReview,context).find("记录时长: 30s · 边沿时间差: 20s")!=std::string::npos,
           "real incident summary explains both duration values without replacing the recorded value");
        ok(realExport["manifest.json"].find("\"duration_seconds\":30")!=std::string::npos &&
           realExport["manifest.json"].find("\"edge_elapsed_seconds\":20")!=std::string::npos,
           "real first incident keeps device's 30 seconds distinct from the 20-second evidence timestamp difference");
    }
    // Oversize message must fail before ZIP construction, never silently truncate.
    std::vector<LogLine> large(1);large[0].lineNo=1;large[0].msg.assign(64*1024*1024,'x');IncidentReview lr;
    lr.valid=true;lr.rows=view(large);bool limited=false;try{buildIncidentZip(lr,context);}catch(const std::length_error&){limited=true;}
    ok(limited,"oversize evidence is rejected rather than silently truncated");
    std::printf("incidenttest: %d failures\n",failures);return failures?1:0;
}
