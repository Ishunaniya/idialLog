#include "incident_export.h"
#include "rssi_summary.h"
#include "sha256.h"
#include "json_value.h"
#include "log_time.h"
#include "tablemodel.h"
#include "text_catalog.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <memory>
#include <set>

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
    for(const auto& record:c.reviews)text+=reviewRecordText(record)+"\n";
    text+=std::string(UiText8(TextId::incident_boundary))+"\n"+UiText8(TextId::incident_evidence_note)+"\n"+UiText8(TextId::incident_signal_note)+"\n";
    return text;
}
std::string buildIncidentZip(const IncidentReview& r,const IncidentExportContext& c) {
    if(!r.valid)throw std::invalid_argument("Invalid incident");
    if(c.originals.size()>1000)throw std::invalid_argument("Evidence packages support at most 1000 original sources");
    std::vector<std::pair<std::string,std::string>> files;
    std::size_t total=0;
    const std::size_t packageLimit=c.originals.empty()?limit:512ULL*1024*1024;
    auto addFile=[&](const std::string& name,std::string bytes) {
        if(bytes.size()>packageLimit-total)throw std::length_error("incident_export_limit");
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
        ",\"original_bytes_included\":"+(c.originals.empty()?std::string("false"):std::string("true"))+",\"metrics_basis\":\"full-source-state-filtered-by-window\",\"evidence_format\":\"parsed-log-lines\",\"start\":"+std::to_string(r.outage.start)+
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
    
    if(!c.originals.empty()) {
        Json index=Json::dict();index.object["schema"]=Json(2LL);index.object["tool"]=Json("dialLog");
        index.object["start"]=Json(r.start);index.object["end"]=Json(r.end);index.object["continuation"]=Json::flag(c.continuation);
        index.object["selected_device"]=Json(c.selectedDevice);index.object["selected_source"]=Json(c.selectedSourceHash);
        Json bookmarks=Json::list();for(const auto& note:c.bookmarks){const auto origin=evidenceOrigin(c.sources,note.line);if(origin.index<1||origin.index>c.originals.size())continue;Json b=Json::dict();b.object["source_hash"]=Json(c.originals[origin.index-1].identity.empty()?c.originals[origin.index-1].hash:c.originals[origin.index-1].identity);b.object["source_line"]=Json(static_cast<long long>(origin.line));b.object["text"]=Json(note.text);bookmarks.array.push_back(std::move(b));}index.object["bookmarks"]=std::move(bookmarks);
        Json originals=Json::list(),digests=Json::dict();std::size_t number=0;
        for(const auto& original:c.originals){
            if(original.bytes.size()>256ULL*1024*1024)throw std::length_error("Original source exceeds 256 MiB");
            if(sha256(original.bytes)!=original.hash)throw std::invalid_argument("Original digest mismatch");
            const std::string path="originals/"+std::to_string(number++)+".log";
            Json item=Json::dict();item.object["path"]=Json(path);item.object["label"]=Json(original.label);item.object["sha256"]=Json(original.hash);item.object["identity"]=Json(original.identity.empty()?original.hash:original.identity);
            originals.array.push_back(std::move(item));addFile(path,original.bytes);
        }
        addFile("workspace.json",encodeWorkspace(c.workspace));
        for(const auto& file:files)digests.object[file.first]=Json(sha256(file.second));
        index.object["originals"]=std::move(originals);index.object["files"]=std::move(digests);
        addFile("package-index.json",writeJson(index)+"\n");
    }

    mz_zip_archive zip{};
    if(!mz_zip_writer_init_heap(&zip,0,0))throw std::runtime_error("ZIP init failed");
    struct End {mz_zip_archive* zip;~End(){mz_zip_writer_end(zip);}} end{&zip};
    for(const auto& f:files)if(!mz_zip_writer_add_mem(&zip,f.first.c_str(),f.second.data(),f.second.size(),MZ_BEST_SPEED))
        throw std::runtime_error("ZIP write failed");
    void* bytes=nullptr;std::size_t size=0;
    if(!mz_zip_writer_finalize_heap_archive(&zip,&bytes,&size))throw std::runtime_error("ZIP finalise failed");
    std::unique_ptr<void,decltype(&mz_free)> data(bytes,mz_free);
    if(size>packageLimit)throw std::length_error("incident_export_limit");
    return std::string(static_cast<const char*>(bytes),size);
}
ImportedEvidencePackage readEvidencePackage(const std::string& bytes) {
    constexpr std::size_t max=512ULL*1024*1024;
    if(bytes.size()>max)throw std::length_error("Package limit");
    mz_zip_archive zip{};if(!mz_zip_reader_init_mem(&zip,bytes.data(),bytes.size(),0))throw std::invalid_argument("Invalid ZIP");
    struct End {mz_zip_archive* zip;~End(){mz_zip_reader_end(zip);}} end{&zip};
    std::map<std::string,std::string> files;std::size_t total=0;const auto n=mz_zip_reader_get_num_files(&zip);
    if(n>1010)throw std::length_error("Package entries");
    for(mz_uint i=0;i<n;++i){mz_zip_archive_file_stat stat{};if(!mz_zip_reader_file_stat(&zip,i,&stat)||mz_zip_reader_is_file_a_directory(&zip,i))throw std::invalid_argument("Invalid package entry");
        std::string name=stat.m_filename;
        if(name.empty()||name.size()>200||name.front()=='/'||name.find("..")!=std::string::npos||name.find('\\')!=std::string::npos||name.find(':')!=std::string::npos||files.count(name)||stat.m_uncomp_size>max-total)throw std::invalid_argument("Invalid package path or size");
        std::size_t size=0;void* buffer=mz_zip_reader_extract_to_heap(&zip,i,&size,0);std::unique_ptr<void,decltype(&mz_free)> memory(buffer,mz_free);
        if(!buffer||size!=stat.m_uncomp_size)throw std::invalid_argument("Package CRC failure");
        total+=size;files.emplace(name,std::string(static_cast<const char*>(buffer),size));}
    ImportedEvidencePackage result;auto report=files.find("report.md");if(report==files.end())throw std::invalid_argument("No report");result.report=report->second;
    const auto found=files.find("package-index.json");
    if(found==files.end()){auto m=files.find("manifest.json");if(m==files.end()||parseJson(m->second).at("tool").str()!="dialLog")throw std::invalid_argument("Not a dialLog package");return result;}
    auto index=parseJson(found->second);if(index.at("schema").num()!=2||index.at("tool").str()!="dialLog")throw std::invalid_argument("Unsupported package");
    const auto& hashes=index.at("files");if(hashes.kind!=Json::Object||hashes.object.size()+1!=files.size())throw std::invalid_argument("Package inventory mismatch");
    for(const auto& pair:hashes.object){auto f=files.find(pair.first);if(f==files.end()||sha256(f->second)!=pair.second.str())throw std::invalid_argument("Package SHA-256 mismatch");}
    result.start=index.at("start").num();result.end=index.at("end").num();if(result.start>result.end)throw std::invalid_argument("Invalid package range");result.continuation=index.at("continuation").flag();
    result.selectedDevice=index.at("selected_device").str();result.selectedSourceHash=index.at("selected_source").str();
    const auto& originals=index.at("originals");if(originals.kind!=Json::Array||originals.array.empty()||originals.array.size()>1000)throw std::invalid_argument("No originals");
    std::set<std::string> paths, identities;
    for (const auto& item : originals.array) {
        const auto path = item.at("path").str();
        auto file = files.find(path);
        if (path.compare(0, 10, "originals/") != 0 || file == files.end() ||
            !paths.insert(path).second || file->second.size() > 256ULL * 1024 * 1024)
            throw std::invalid_argument("Invalid original inventory");
        const auto hash = item.at("sha256").str();
        if (sha256(file->second) != hash) throw std::invalid_argument("Original hash mismatch");
        const auto identity = item.at("identity").str();
        const auto label = item.at("label").str();
        if (identity.size() != 64 || identity.find_first_not_of("0123456789abcdef") != std::string::npos ||
            !identities.insert(identity).second || label.size() > 4096 || label.find('\0') != std::string::npos)
            throw std::invalid_argument("Invalid source identity or label");
        result.originals.push_back({path, label, std::move(file->second), hash, identity});
    }
    auto workspace=files.find("workspace.json");if(workspace==files.end())throw std::invalid_argument("No workspace");result.workspace=decodeWorkspace(workspace->second);
    for (const auto& device : result.workspace.devices)
        if (!identities.count(device.first)) throw std::invalid_argument("Unknown device source");
    if ((!result.selectedSourceHash.empty() && !identities.count(result.selectedSourceHash)) ||
        (!result.selectedDevice.empty() && !std::any_of(result.workspace.devices.begin(), result.workspace.devices.end(),
            [&](const auto& device) { return device.second.device == result.selectedDevice; })))
        throw std::invalid_argument("Unknown selected source or device");
    const auto& bookmarks=index.at("bookmarks");if(bookmarks.kind!=Json::Array||bookmarks.array.size()>10000)throw std::invalid_argument("Invalid bookmarks");for(const auto& b:bookmarks.array){auto line=b.at("source_line").num();auto hash=b.at("source_hash").str();auto text=b.at("text").str();if(line<=0||text.size()>32768||!std::any_of(result.originals.begin(),result.originals.end(),[&](const IncidentExportContext::Original& o){return o.identity==hash;}))throw std::invalid_argument("Invalid bookmark source");result.bookmarks.push_back({hash,text,static_cast<std::size_t>(line)});}
    return result;
}
} // namespace dl
