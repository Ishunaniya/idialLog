// build/tests/ 下的程序与导出文件是可删除的本地测试产物；make check 会重新生成，不是应用运行依赖。
#include "version.h"
#include "workspace_state.h"
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
int main(){int failures=0;auto ok=[&](bool pass,const char* name){std::printf("%s %s\n",pass?"PASS":"FAIL",name);if(!pass)++failures;};auto rejects=[&](auto fn){try{fn();return false;}catch(const std::exception&){return true;}};
    ok(sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA-256 empty known vector");
    ok(sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 known abc vector");Sha256 stream;for(int i=0;i<1000000;++i)stream.update("a");ok(stream.finish()=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","SHA-256 streaming million-byte vector");
    auto unicode=parseJson("{\"x\":\"\\ud83d\\ude00\\n\",\"time\":9223372036854775807}");ok(unicode.at("x").str()=="\xf0\x9f\x98\x80\n"&&unicode.at("time").num()==LLONG_MAX,"JSON surrogate and lossless int64");
    for(const char* bad:{"{\"x\":1,\"x\":2}","[01]","[1.0]","[9223372036854775808]","[\"\\ud800\"]","[]trailing"})ok(rejects([&]{parseJson(bad);}),"reject malformed/duplicate/overflow JSON");
    WorkspaceState state;const auto aHash=sha256("source-a"),bHash=sha256("source-b"),key=sha256("incident-a");state.devices[aHash]={"设备一","FW-A","APN=现场\n=cmd"};state.devices[bHash]={"device-b","FW-B",""};state.reviews[key]={key,"真实断网", "人工复核\n<script>\"note\"",ReviewStatus::Confirmed};const auto encoded=encodeWorkspace(state);auto decoded=decodeWorkspace(encoded);
    ok(decoded.devices[aHash].device=="设备一"&&decoded.reviews[key].note==state.reviews[key].note&&decoded.reviews[key].status==ReviewStatus::Confirmed,"workspace round trip preserves manual content/status");
    auto invalid=parseJson(encoded);invalid.object["reviews"].array[0].object["status"]=Json(3LL);ok(rejects([&]{decodeWorkspace(writeJson(invalid));}),"unknown review status rejected atomically");
    DocumentState doc;std::vector<std::string> raw={"[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | CSQ:18 | RSRP:-95 RSRQ:-8 SNR:120 RSSI:-65", "[2026-08-03 10:00:01] [SDK] Ping failed, fault timer started", "[2026-08-03 10:00:03] [SDK] Network recovered after 2s", "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | CSQ:8 | RSRP:-115 RSRQ:-19 SNR:-20 RSSI:-100", "[2026-08-03 10:00:02] [SDK] Ping failed, fault timer started", "[2026-08-03 10:00:09] [SDK] Network recovered after 7s"};parseLines(raw,doc.lines,doc.sessions,&doc.audit,{0,3});
    SourceSummary source;source.label=L"A.log";source.first=0;source.last=3;source.originalHash=aHash;doc.sources.push_back(source);source.label=L"B.log";source.first=3;source.last=6;source.rawLineOffset=3;source.originalHash=bHash;doc.sources.push_back(source);doc.workspace=state;
    LogView rows;for(const auto& row:doc.lines)rows.push_back(&row);doc.sourceMode=DocumentState::SourceMode::Device;doc.selectedDevice="设备一";auto selected=rows;doc.restrictToSelection(selected);ok(selected.size()==3&&selected.front()==&doc.lines[0],"device group excludes other device despite overlapping times");doc.selectedDevice="";selected=rows;doc.restrictToSelection(selected);ok(selected.empty(),"unknown device cannot merge all sources");doc.workspace.devices[bHash].device="设备一";doc.selectedDevice="设备一";selected=rows;doc.restrictToSelection(selected);ok(selected.size()==6,"manual same-device grouping includes both sources");
    LogView a(rows.begin(),rows.begin()+3),b(rows.begin()+3,rows.end());auto as=comparePeriod(a,doc.lines[0].t,doc.lines[2].t),bs=comparePeriod(b,doc.lines[3].t,doc.lines[5].t);ok(as.outages==1&&bs.outages==1&&as.down==2&&bs.down==7&&as.rssi.mean==-65&&bs.rssi.mean==-100,"period comparison isolates real outage durations and RSSI");ok(as.snr.samples==1&&as.snr.mean==120&&bs.snr.mean==-20,"SNR distribution excludes missing sentinel");auto clipped=comparePeriod(b,doc.lines[4].t,doc.lines[4].t);ok(clipped.open==1&&clipped.rssi.samples==0,"clipped period retains open boundary and missing RSSI");
    SetEnglish(true);auto report=periodComparisonText(as,bs,"FW-A","FW-B",true);ok(report.find("-35.000")!=std::string::npos&&report.find("manually confirmed")!=std::string::npos&&report.find("does not establish")==std::string::npos,"comparison carries difference and manual relation without causality verdict");
    std::vector<LogLine> recoveryRows;std::vector<std::string> recoverySessions;
    parseLines({"[2026-08-03 10:00:00] [RECOVERY] class=DATA_PATH level=L2_PDP action=enter", "[2026-08-03 10:00:01] [RECOVERY] class=PDP level=L3_CFUN action=cycle"},recoveryRows,recoverySessions);
    LogView recoveryView;for(const auto& row:recoveryRows)recoveryView.push_back(&row);
    auto recoveryStats=comparePeriod(recoveryView,recoveryRows.front().t,recoveryRows.back().t);
    ok(recoveryStats.recoveryActions["RECOVERY / level=L2_PDP / action=enter"]==1&&recoveryStats.recoveryActions["RECOVERY / level=L3_CFUN / action=cycle"]==1,"reported recovery levels and actions remain distinct");
    std::vector<LogLine> v2Rows;std::vector<std::string> v2Sessions;
    parseLines({"2026-08-03T10:00:00 host modem_mng_v2[321]: CEREG stat=1 lac=1234 ci=01ABCDEF act=0", "2026-08-03T10:00:01 host modem_mng_v2[321]: [READY] CSQ: 20 (sim_present=1 online=1)"},v2Rows,v2Sessions);
    LogView v2View;for(const auto& row:v2Rows)v2View.push_back(&row);
    auto v2Stats=comparePeriod(v2View,v2Rows.back().t,v2Rows.back().t);
    ok(v2Stats.samples==1&&v2Stats.csq.samples==0,"period keeps pre-window non-LTE registration evidence");
    auto win=navigateChartWindow({100,200},{0,300},150,.5,0);ok(win.start==125&&win.end==175,"anchor-centred wheel zoom");win=navigateChartWindow(win,{0,300},150,1,10);ok(win.start==250&&win.end==300,"pan clamps to source bounds");win=navigateChartWindow({100,101},{0,300},100,.1,0);ok(win.end-win.start==1,"zoom minimum one second");
    ok(findingBrief("观察到 2 次失败。复述说明。后续【推断】无法确定原因。").find("后续【推断】无法确定原因。")!=std::string::npos,"concise view retains full late inference sentence");ok(findingBrief("Observed -99.5 dBm. Repeated guidance. It does not prove an outage.").find("Observed -99.5 dBm.")==0,"concise view keeps decimals and full negative qualifier");
    ok(findingBrief("Observed failures. Repeated description. Only reported samples are compared. Insufficient evidence to establish a cause.").find("Only reported samples")!=std::string::npos&&findingBrief("Observed failures. Repeated description. Insufficient evidence to establish a cause.").find("Insufficient evidence")!=std::string::npos,"concise view retains uppercase English scope and evidence limits");
    ok(findingBrief("观察到失败。重复说明。尚未确认根因，证据不足。") .find("尚未确认根因，证据不足。")!=std::string::npos,"concise view retains incomplete Chinese evidence qualifier");
    SetEnglish(false);auto catalog=buildIncidentCatalog(a);auto review=reviewIncident(catalog,0,300,300);IncidentExportContext context;context.version=DL_VER_STR;context.sources={{"A.log",1,1,3,0},{"B.log",2,4,6,3}};std::string aBytes="\xef\xbb\xbf";for(int i=0;i<3;++i)aBytes+=raw[i]+"\r\n";std::string bBytes;for(int i=3;i<6;++i)bBytes+=raw[i]+"\n";
    context.originals={{"","A.log",aBytes,sha256(aBytes),sha256(aBytes)},{"","B.log",bBytes,sha256(bBytes),sha256(bBytes)}};context.workspace=state;context.workspace.devices.clear();context.workspace.devices[sha256(aBytes)]=state.devices[aHash];context.workspace.devices[sha256(bBytes)]=state.devices[bHash];context.selectedSourceHash=sha256(aBytes);context.selectedDevice="设备一";context.bookmarks={{2,"现场备注"}};context.reviews.push_back(state.reviews[key]);auto bytes=buildIncidentZip(review,context);auto package=readEvidencePackage(bytes);
    ok(package.originals.size()==2&&package.originals[0].bytes==aBytes&&package.originals[1].bytes==bBytes,"evidence round trip preserves original BOM/CRLF bytes");ok(package.workspace.reviews[key].note==state.reviews[key].note&&package.selectedDevice=="设备一"&&package.bookmarks.size()==1&&package.bookmarks[0].line==2,"package restores manual reviews, group selection and source-local bookmarks");
    std::ofstream("build/tests/unit/workspace-package.zip",std::ios::binary).write(bytes.data(),bytes.size());
    auto invalidContext=context;invalidContext.originals[1].identity=invalidContext.originals[0].identity;
    ok(rejects([&]{readEvidencePackage(buildIncidentZip(review,invalidContext));}),"duplicate source identities rejected");
    invalidContext=context;invalidContext.selectedSourceHash=sha256("unknown");
    ok(rejects([&]{readEvidencePackage(buildIncidentZip(review,invalidContext));}),"unknown source selection rejected");
    invalidContext=context;invalidContext.workspace.devices[sha256("unknown")]={"device","",""};
    ok(rejects([&]{readEvidencePackage(buildIncidentZip(review,invalidContext));}),"unrelated device source rejected");
    auto tooManyOriginals=context;tooManyOriginals.originals.resize(1001);for(auto& original:tooManyOriginals.originals)if(original.hash.empty())original.hash=sha256(original.bytes);
    ok(rejects([&]{buildIncidentZip(review,tooManyOriginals);}),"export source count matches import limit");
    WorkspaceState tooManyDevices;for(int i=0;i<1001;++i)tooManyDevices.devices[sha256(std::to_string(i))]={};
    ok(rejects([&]{encodeWorkspace(tooManyDevices);}),"workspace writer rejects records its reader cannot reopen");
    context.originals[0].bytes+="changed";ok(rejects([&]{buildIncidentZip(review,context);}),"export rejects changed original");
    auto corrupted=bytes;corrupted[corrupted.size()/2]^=0x40;ok(rejects([&]{readEvidencePackage(corrupted);}),"corrupt ZIP rejected without partial workspace");
    auto makeZip=[](const std::vector<std::pair<std::string,std::string>>& files){mz_zip_archive z{};mz_zip_writer_init_heap(&z,0,0);for(const auto& f:files)mz_zip_writer_add_mem(&z,f.first.c_str(),f.second.data(),f.second.size(),MZ_BEST_SPEED);void* buffer=nullptr;std::size_t size=0;mz_zip_writer_finalize_heap_archive(&z,&buffer,&size);std::string out(static_cast<const char*>(buffer),size);mz_free(buffer);mz_zip_writer_end(&z);return out;};
    ok(rejects([&]{readEvidencePackage(makeZip({{"report.md","hello"},{"../outside","bad"}}));}),"traversal entry rejected without extraction");ok(rejects([&]{readEvidencePackage(makeZip({{"report.md","one"},{"report.md","two"}}));}),"duplicate ZIP paths rejected");
    ReportChartOptions chart;chart.start=0;chart.end=20;chart.title="RSSI";chart.unit="dBm";chart.low=-120;chart.high=-20;chart.color="#2563eb";chart.comparisonMode=true;chart.comparison={{0,-100},{10,-95}};chart.relative=true;chart.originalStart=doc.lines[0].t;chart.comparisonOriginalStart=doc.lines[3].t;auto html=renderReportChart({{0,-65},{10,-60}}, {},chart).html;ok(html.find("stroke-dasharray=\"7 4\"")!=std::string::npos&&html.find("\"second\":[[0,-100],[10,-95]]")!=std::string::npos&&html.find("1970-01-01")==std::string::npos,"dual report curves preserve original hover dates and relative axis");chart.comparison.clear();chart.title="RSRQ";auto empty=renderReportChart({}, {},chart).html;ok(empty.find("chart-data")!=std::string::npos&&empty.find("\"points\":[]")!=std::string::npos,"empty metrics participate in shared hover as missing samples");ok(html.find("B 采样 2")!=std::string::npos&&empty.find("B 采样 0")!=std::string::npos&&html.find("\"compare\":true")!=std::string::npos,"comparison captions and hover identify both sources even when B is missing");std::ofstream("build/tests/unit/workspace-comparison.html")<<"<!doctype html><meta charset=utf-8>"<<html<<empty<<reportChartScript(false);
    auto singleton=chart;singleton.start=singleton.end=10;singleton.title="RSSI";singleton.comparisonMode=false;singleton.relative=false;
    std::ofstream("build/tests/unit/workspace-singleton.html")<<"<!doctype html><meta charset=utf-8>"<<renderReportChart({{10,-65}}, {},singleton).html<<reportChartScript(false);
    DocumentState other;other.swap(doc);ok(other.workspace.reviews.size()==1&&other.selectedDevice=="设备一"&&doc.workspace.reviews.empty(),"document swap owns workspace safely");other.release();ok(other.workspace.reviews.empty()&&other.selectedDevice.empty(),"unload releases workspace metadata");
    return failures?1:0;
}
