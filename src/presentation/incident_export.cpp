#include "incident_export.h"
#include "rssi_summary.h"
#include "log_time.h"
#include "tablemodel.h"
#include "text_catalog.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <memory>

namespace dl {
namespace {
constexpr std::size_t limit=64*1024*1024;
void append(std::string& out,const std::string& value) {
    if(value.size()>limit-out.size()) throw std::length_error("incident_export_limit");
    out+=value;
}
std::string json(const std::string& value) {
    std::string out="\"";
    const char* hex="0123456789abcdef";
    for(unsigned char c:value) {
        if(c=='"' || c=='\\') { append(out,"\\");append(out,std::string(1,c)); }
        else if(c<32) { std::string escape="\\u00";escape+=hex[c>>4];escape+=hex[c&15];append(out,escape); }
        else append(out,std::string(1,c));
    }
    append(out,"\"");return out;
}
std::string csv(const std::string& value,bool guard=false) {
    std::string out="\"";
    const auto first=value.find_first_not_of(" \t\r\n");
    if(guard && first!=std::string::npos && std::string("=+-@").find(value[first])!=std::string::npos)out+="'";
    for(char c:value) {if(c=='"')append(out,"\"");append(out,std::string(1,c));}
    append(out,"\"");return out;
}
std::string phase(const IncidentReview& r,const MetricRow& m) {
    const auto p=incidentSamplePhase(r,m);
    return UiText8(p==IncidentPhase::Before?TextId::incident_pre:
        p==IncidentPhase::After?TextId::incident_post:TextId::incident_during);
}
std::string role(const IncidentReview& r,const LogLine& row) {
    if(row.lineNo==r.outage.endLine && r.outage.recovered)return UiText8(TextId::incident_end);
    if(row.lineNo==r.outage.startLine)return UiText8(TextId::incident_start);
    return GeneratedText(timelineCellText(row,1));
}
std::string originJson(const EvidenceOrigin& o,std::size_t line) {
    return "\"source_index\":"+std::to_string(o.index)+",\"source_label\":"+json(o.label)+
        ",\"source_line\":"+std::to_string(o.line)+",\"merged_line\":"+std::to_string(line);
}
// Log-controlled strings are indented in Markdown, never allowed to create
// headings, links or instructions that masquerade as the generated report.
std::string block(const std::string& value) {
    std::string out="    ";for(char c:value) {if(c=='\n')append(out,"\n    ");else if(c!='\r')append(out,std::string(1,c));}
    append(out,"\n\n");return out;
}
}
EvidenceOrigin evidenceOrigin(const std::vector<EvidenceSourceSpan>& sources,std::size_t line) {
    for(const auto& s:sources)if(s.firstLine && line>=s.firstLine && line<=s.lastLine)
        return {s.label,s.index,line>s.rawLineOffset?line-s.rawLineOffset:0};
    return {"",0,0};
}
std::string incidentSummaryText(const IncidentReview& r,const IncidentExportContext& c) {
    if(!r.valid)return {};
    std::string text=std::string(UiText8(TextId::incident_title))+" #"+std::to_string(r.index+1)+"\n"+c.scope+"\n";
    text+=std::string(UiText8(TextId::incident_start))+": "+fmtTime(r.outage.start,"FULL")+"\n";
    if(r.outage.recovered)text+=std::string(UiText8(TextId::incident_end))+": "+fmtTime(r.outage.end,"FULL")+"\n"+
        UiText8(TextId::incident_duration)+": "+fmtDur(r.outage.dur)+" · "+
        UiText8(TextId::incident_edge_duration)+": "+fmtDur(r.outage.end-r.outage.start)+"\n";
    else text+=std::string(UiText8(TextId::incident_open))+"\n";
    for(auto line:{r.outage.startLine,r.outage.endLine}) if(line) {
        const auto o=evidenceOrigin(c.sources,line);
        text+=std::string(UiText8(TextId::incident_source))+": #"+std::to_string(o.index)+" "+o.label+" · "+
            UiText8(TextId::incident_local_line)+" "+std::to_string(o.line)+" · "+
            UiText8(TextId::incident_global_line)+" "+std::to_string(line)+"\n";
    }
    text+=std::string(UiText8(TextId::ui_0364))+GeneratedText(r.platform.name)+"\n";
    text+=std::string(UiText8(TextId::incident_window))+": "+fmtTime(r.start,"FULL")+" — "+fmtTime(r.end,"FULL")+"\n";
    text+=rssiSummaryText(observedRssi(r.metrics))+"\n";
    text+=UiText8(TextId::incident_scope);text+='\n';
    if(r.outage.reportedDuration && r.outage.startLine==r.outage.endLine)text+=std::string(UiText8(TextId::incident_reported))+"\n";
    if(r.resolvedOutsideFilter)text+=std::string(UiText8(TextId::incident_rebuilt))+"\n";
    if(r.clockLimited)text+=std::string(UiText8(TextId::incident_clock))+"\n";
    if(r.inferredTime)text+=std::string(UiText8(TextId::incident_inferred))+"\n";
    text+=std::string(UiText8(TextId::incident_boundary))+"\n"+UiText8(TextId::incident_evidence_note)+"\n"+UiText8(TextId::incident_signal_note)+"\n";
    return text;
}
std::string buildIncidentZip(const IncidentReview& r,const IncidentExportContext& c) {
    if(!r.valid)throw std::invalid_argument("Invalid incident");
    std::vector<std::pair<std::string,std::string>> files;
    std::size_t total=0;
    auto addFile=[&](const char* name,std::string bytes) {
        if(bytes.size()>limit-total)throw std::length_error("incident_export_limit");
        total+=bytes.size();files.emplace_back(name,std::move(bytes));
    };
    std::string report="# "+std::string(UiText8(TextId::incident_title))+"\n\n"+block(incidentSummaryText(r,c));
    if(r.platformEvidence) {
        const auto origin=evidenceOrigin(c.sources,r.platformEvidence->lineNo);
        append(report,block(std::string(UiText8(TextId::incident_platform_evidence))+" #"+std::to_string(origin.index)+" "+origin.label+
            " : "+std::to_string(origin.line)+" [merged #"+std::to_string(r.platformEvidence->lineNo)+"] "+r.platformEvidence->ts+" "+r.platformEvidence->msg));
    }
    append(report,std::string(UiText8(TextId::incident_rules_note))+"\n\n");
    if(r.findings.empty())append(report,UiText8(TextId::incident_no_rules));
    for(const auto& f:r.findings) {
        append(report,"## "+GeneratedText(f.title)+"\n\n");
        append(report,block(GeneratedText(f.detail)));append(report,block(GeneratedText(f.advice)));
        for(const auto& e:f.ev)append(report,block("#"+std::to_string(e.lineNo)+" "+e.ts+" "+e.text));
    }
    addFile("report.md",std::move(report));
    std::string events="\xEF\xBB\xBFtime,role,source_index,source,source_line,merged_line,message\r\n";
    for(const auto* row:r.events) {
        const auto o=evidenceOrigin(c.sources,row->lineNo);
        append(events,csv(row->ts,true)+","+csv(role(r,*row))+","+std::to_string(o.index)+","+csv(o.label,true)+","+
            std::to_string(o.line)+","+std::to_string(row->lineNo)+","+csv(row->msg,true)+"\r\n");
    }
    addFile("events.csv",std::move(events));
    std::string metrics="\xEF\xBB\xBFtime,ch,cell_id,pci,tac,csq,temp_max,consec_fail,rx,delta_rx,rsrp,rsrq,snr,rssi,srv,rat,deny,oper,quality,at_timeout,at_probe,at_stage,phase,source_index,source,source_line,merged_line\r\n";
    for(const auto& m:r.metrics) {
        for(std::size_t col=0;col<kMetricColumnCount;++col){if(col)append(metrics,",");append(metrics,csv(col>=18?GeneratedText(metricCsvCellText(m,col)):metricCsvCellText(m,col)));}
        const auto o=evidenceOrigin(c.sources,m.lineNo);
        append(metrics,","+csv(phase(r,m))+","+std::to_string(o.index)+","+csv(o.label,true)+","+std::to_string(o.line)+","+std::to_string(m.lineNo)+"\r\n");
    }
    addFile("metrics.csv",std::move(metrics));
    std::string evidence;
    for(const auto* row:r.rows) {
        const auto o=evidenceOrigin(c.sources,row->lineNo);
        append(evidence,"{"+originJson(o,row->lineNo)+",\"time\":"+json(row->ts)+",\"epoch\":"+std::to_string(row->t)+
            ",\"milliseconds\":"+(row->ms<0?std::string("null"):std::to_string(row->ms))+
            ",\"parser_source_id\":"+std::to_string(row->sourceId)+",\"inferred_time\":"+(row->inferredTime?"true":"false")+",\"level\":"+json(row->levelText())+
            ",\"tag\":"+json(row->tagText())+",\"message\":"+json(row->msg)+"}\n");
    }
    addFile("parsed-evidence.jsonl",std::move(evidence));
    std::vector<std::size_t> selected;selected.reserve(r.rows.size());for(const auto* row:r.rows)selected.push_back(row->lineNo);
    std::sort(selected.begin(),selected.end());
    std::string notes;
    for(const auto& note:c.bookmarks)if(std::binary_search(selected.begin(),selected.end(),note.line))
        append(notes,"{"+originJson(evidenceOrigin(c.sources,note.line),note.line)+",\"text\":"+json(note.text)+"}\n");
    addFile("bookmarks.jsonl",std::move(notes));
    std::string manifest="{\"schema\":1,\"tool\":\"dialLog\",\"version\":"+json(c.version)+",\"build\":"+json(c.build)+
        ",\"language\":"+json(IsEnglish()?"en-US":"zh-CN")+",\"scope\":"+json(c.scope)+",\"continuation\":"+(c.continuation?"true":"false")+
        ",\"original_bytes_included\":false,\"metrics_basis\":\"full-source-state-filtered-by-window\",\"evidence_format\":\"parsed-log-lines\",\"start\":"+std::to_string(r.outage.start)+
        ",\"end\":"+(r.outage.recovered?std::to_string(r.outage.end):"null")+",\"recovered\":"+(r.outage.recovered?"true":"false")+
        ",\"self_contained_reported_duration\":"+(r.outage.reportedDuration?"true":"false")+",\"duration_seconds\":"+(r.outage.recovered?std::to_string(r.outage.dur):"null")+
        ",\"edge_elapsed_seconds\":"+(r.outage.recovered?std::to_string(r.outage.end-r.outage.start):"null")+
        ",\"start_line\":"+std::to_string(r.outage.startLine)+",\"end_line\":"+std::to_string(r.outage.endLine)+
        ",\"window_start\":"+std::to_string(r.start)+",\"window_end\":"+std::to_string(r.end)+
        ",\"clock_limited\":"+(r.clockLimited?"true":"false")+",\"inferred_time\":"+(r.inferredTime?"true":"false")+
        ",\"rows\":"+std::to_string(r.rows.size())+",\"events\":"+std::to_string(r.events.size())+",\"samples\":"+std::to_string(r.metrics.size())+
        ",\"platform\":"+json(GeneratedText(r.platform.name))+",\"platform_evidence\":";
    if(r.platformEvidence)append(manifest,"{"+originJson(evidenceOrigin(c.sources,r.platformEvidence->lineNo),r.platformEvidence->lineNo)+
        ",\"time\":"+json(r.platformEvidence->ts)+",\"message\":"+json(r.platformEvidence->msg)+"}");
    else append(manifest,"null");
    append(manifest,",\"sources\":[");
    bool comma=false;for(const auto& s:c.sources) {
        const bool used=(r.platformEvidence && r.platformEvidence->lineNo>=s.firstLine && r.platformEvidence->lineNo<=s.lastLine) || std::any_of(selected.begin(),selected.end(),[&](std::size_t line){return line>=s.firstLine&&line<=s.lastLine;});
        if(!used)continue;
        if(comma)append(manifest,",");
        comma=true;
        append(manifest,"{\"index\":"+std::to_string(s.index)+",\"label\":"+json(s.label)+",\"raw_line_offset\":"+std::to_string(s.rawLineOffset)+"}");
    }
    append(manifest,"]}\n");addFile("manifest.json",std::move(manifest));
    
    mz_zip_archive zip{};
    if(!mz_zip_writer_init_heap(&zip,0,0))throw std::runtime_error("ZIP init failed");
    struct End {mz_zip_archive* zip;~End(){mz_zip_writer_end(zip);}} end{&zip};
    for(const auto& f:files)if(!mz_zip_writer_add_mem(&zip,f.first.c_str(),f.second.data(),f.second.size(),MZ_BEST_SPEED))
        throw std::runtime_error("ZIP write failed");
    void* bytes=nullptr;std::size_t size=0;
    if(!mz_zip_writer_finalize_heap_archive(&zip,&bytes,&size))throw std::runtime_error("ZIP finalise failed");
    std::unique_ptr<void,decltype(&mz_free)> data(bytes,mz_free);
    if(size>limit)throw std::length_error("incident_export_limit");
    return std::string(static_cast<const char*>(bytes),size);
}
} // namespace dl
