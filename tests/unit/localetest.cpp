#include "text_catalog.h"
#include "columnmodel.h"
#include "logmodel.h"
#include <cstdio>
#include <string>
#include <fstream>
#include <filesystem>
using namespace dl;
int main() {
    int failures=0;
    auto ok=[&](bool pass,const char* name) {std::printf("%s %s\n",pass?"PASS":"FAIL",name);if(!pass)++failures;};
    SetEnglish(false);
    ok(std::wstring(UiText(TextId::ui_0164))==L"概览","Chinese overview");
    SetEnglish(true);
    ok(std::wstring(UiText(TextId::ui_0164))==L"Overview","English overview");
    const std::string original="全量日志历史汇总:断网根因分类:弱信号 3 次 / 共 4 次";
    const auto rendered=GeneratedText(original);
    ok(rendered.find("Full-input history summary")!=std::string::npos &&
       rendered.find("Weak signal")!=std::string::npos && rendered.find("3")!=std::string::npos &&
       rendered.find("4")!=std::string::npos && rendered.find("弱信号")==std::string::npos,"finding parameters preserved");
    ok(original=="全量日志历史汇总:断网根因分类:弱信号 3 次 / 共 4 次","presentation leaves diagnosis unchanged");
    ok(GeneratedText("已恢复 · >60s")=="Recovered · >60s","generated table labels");
    ok(GeneratedText("RSRP=-115dBm")=="RSRP=-115dBm","units and numbers unchanged");
    ok(RelocalizeUiText(L"信号指标")==L"Signal metrics","controls relocalize");
    SetEnglish(false);
    ok(GeneratedText(original)==original && RelocalizeUiText(L"Signal metrics")==L"信号指标","switch back without losing source");
    for (int p=0;p<6;++p) {
        const auto mask=metricColumnMask(static_cast<MetricColumnPreset>(p));
        ok((mask&1u)!=0 && (mask&~kAllMetricColumns)==0,"column preset keeps timestamp and valid fields");
    }
    ok(metricColumnMask(MetricColumnPreset::AtHealth)==(1u|(1u<<19)|(1u<<20)|(1u<<21)),"AT task preset exact fields");
    ok(metricColumnMask(MetricColumnPreset::All)==kAllMetricColumns,"full export model retains 22 fields");
    SetEnglish(true);
    int sampleFiles=0, sampleFindings=0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator("samples")) {
        if (!entry.is_regular_file() || entry.path().extension()!=".log") continue;
        std::ifstream input(entry.path()); std::vector<std::string> raw; std::string line;
        while(std::getline(input,line)) raw.push_back(line);
        std::vector<LogLine> parsed;std::vector<std::string> sessions;ParseAudit audit;
        parseLines(raw,parsed,sessions,&audit);
        auto metrics=buildMetrics(parsed);auto outages=collectOutages(parsed);
        auto findings=analyze(parsed,outages,metrics,detectPlatform(parsed),audit);
        ++sampleFiles;
        for (const auto& finding : findings) {
            ++sampleFindings;
            for (const std::string* field : {&finding.title,&finding.detail,&finding.advice}) {
                const auto rendered=GeneratedText(*field);
                bool chinese=false;
                for (std::size_t i=0;i+2<rendered.size();++i) {
                    unsigned char first=rendered[i],second=rendered[i+1];
                    if ((first>=0xe4 && first<=0xe9) && (second>=0x80 && second<=0xbf)) chinese=true;
                }
                if (chinese) {std::printf("UNTRANSLATED %s: %s\n",entry.path().string().c_str(),rendered.c_str());++failures;}
            }
        }
    }
    std::printf("English diagnosis coverage: %d files, %d findings\n",sampleFiles,sampleFindings);
    ok(sampleFiles>=30 && sampleFindings>=100,"real/simulation sample English findings coverage");
    return failures?1:0;
}
