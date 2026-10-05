#include "report_chart.h"
#include "log_time.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace dl {
namespace {
std::string escape(const std::string& text) {
    std::string result;
    for (char ch : text) {
        switch (ch) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default: result += ch; break;
        }
    }
    return result;
}
std::string number(long double value, int precision = 2) {
    std::ostringstream stream; stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}
}
ReportChartResult renderReportChart(const ChartSeries& input,
                                   const std::vector<Outage>& outages,
                                   const ReportChartOptions& o) {
    ReportChartResult result;
    const auto first = std::lower_bound(input.begin(),input.end(),o.start,
        [](const ChartPoint& p,long long t){return p.first<t;});
    const auto last = o.end < o.start ? first : std::upper_bound(first,input.end(),o.end,
        [](long long t,const ChartPoint& p){return t<p.first;});
    const ChartSeries visible(first,last);
    result.samples = visible.size();
    ChartSeries points;
    constexpr std::size_t buckets = 4096;
    downsampleChartSeries(visible,o.start,o.end,buckets,points);
    result.drawn = points.size();
    result.html = "<figure class=\"signal-chart\" data-samples=\"" + std::to_string(result.samples) +
        "\" data-drawn=\"" + std::to_string(result.drawn) + "\"><figcaption><strong>" + escape(o.title) +
        "</strong><span class=\"muted\">" + (o.english ? "Samples " : "原始点 ") + std::to_string(result.samples) +
        (o.english ? " · Drawn " : " · 绘制点 ") + std::to_string(result.drawn) + "</span></figcaption>";
    const bool comparing = o.comparisonMode || !o.comparison.empty();
    if (comparing) {
        const auto countB = std::count_if(o.comparison.begin(), o.comparison.end(), [&](const ChartPoint& point) {
            return point.first >= o.start && point.first <= o.end;
        });
        const std::string labelA = o.english ? "A samples " : "A 采样 ";
        const std::string labelB = o.english ? "B samples " : "B 采样 ";
        const auto captionEnd = result.html.find("</figcaption>");
        result.html.insert(captionEnd, "<span class=\"muted\">" + labelA + std::to_string(visible.size()) +
            " <i aria-hidden=\"true\" style=\"display:inline-block;width:24px;vertical-align:middle;border-top:2px solid " + escape(o.color) +
            "\"></i> · " + labelB + std::to_string(countB) +
            " <i aria-hidden=\"true\" style=\"display:inline-block;width:24px;vertical-align:middle;border-top:2px dashed #008300\"></i></span>");
    }
    auto data=[&](const ChartSeries& values){std::string s="[";bool comma=false;for(const auto& p:values){if(p.first<o.start||p.first>o.end)continue;if(comma)s+=',';comma=true;s+="["+std::to_string(p.first)+","+std::to_string(p.second)+"]";}return s+"]";};
    const std::string payload="<script type=\"application/json\" class=\"chart-data\">{\"start\":"+std::to_string(o.start)+",\"end\":"+std::to_string(o.end)+",\"low\":"+std::to_string(o.low)+",\"high\":"+std::to_string(o.high)+",\"scale\":"+(o.scaled10?std::string("10"):std::string("1"))+",\"compare\":"+(comparing?std::string("true"):std::string("false"))+",\"relative\":"+(o.relative?std::string("true"):std::string("false"))+",\"originalStart\":"+std::to_string(o.originalStart)+",\"secondStart\":"+std::to_string(o.comparisonOriginalStart)+",\"color\":\""+escape(o.color)+"\",\"unit\":\""+escape(o.unit)+"\",\"points\":"+data(visible)+",\"second\":"+data(o.comparison)+"}</script>";
    if ((visible.empty() && o.comparison.empty()) || o.high <= o.low) {
        result.html += "<p class=\"muted\">" + std::string(o.english ? "No samples in this range." : "此区间暂无样本。") + "</p>" + payload + "</figure>";
        return result;
    }
    constexpr int left=64, right=814, top=18, bottom=246;
    const long double span=std::max(1.0L,static_cast<long double>(o.end)-o.start);
    auto x=[&](long long t){return left+(static_cast<long double>(t)-o.start)/span*(right-left);};
    auto y=[&](int value){return bottom-(static_cast<long double>(std::clamp(value,o.low,o.high))-o.low)/
        (static_cast<long double>(o.high)-o.low)*(bottom-top);};
    auto valueText=[&](int value){return o.scaled10?number(value/10.0L,1):std::to_string(value);};
    std::string& out=result.html;
    out += "<div class=\"chart-scroll\"><svg viewBox=\"0 0 1000 310\" role=\"img\" aria-label=\"" +
        escape(o.title) + "\"><title>" + escape(o.title+" · "+(o.relative?std::to_string(o.start)+" s":fmtTime(o.start,"FULL"))+" → "+(o.relative?std::to_string(o.end)+" s":fmtTime(o.end,"FULL"))) + "</title>";
    for (const auto& outage : outages) {
        const long long end=outage.recovered?outage.end:o.end;
        if(end<o.start || outage.start>o.end || end<outage.start)continue;
        const auto a=x(std::max(o.start,outage.start)),b=x(std::min(o.end,end));
        out += "<rect class=\"outage-band\" data-event-start=\""+std::to_string(outage.start)+"\" data-event-end=\""+std::to_string(end)+"\" tabindex=\"0\" x=\""+number(a)+"\" y=\"18\" width=\""+number(std::max(1.5L,b-a))+
            "\" height=\"228\"><title>"+escape((o.english?"Outage: ":"断网: ")+fmtTime(outage.start,"FULL")+" → "+
            (outage.recovered?fmtTime(outage.end,"FULL"):(o.english?"Unrecovered":"未恢复")))+"</title></rect>";
    }
    for(int i=0;i<=4;++i) {
        const int value=static_cast<int>(o.high-(static_cast<long long>(o.high)-o.low)*i/4);
        out += "<line class=\"chart-grid\" x1=\"64\" x2=\"814\" y1=\""+number(y(value))+"\" y2=\""+number(y(value))+"\"/>"+
            "<text class=\"chart-label\" x=\"56\" y=\""+number(y(value)+4)+"\" text-anchor=\"end\">"+valueText(value)+"</text>";
    }
    const auto ticks=chartTimeTicks(o.start,o.end,right-left,135);
    for(std::size_t i=0;i<ticks.size();++i) {
        const auto time=o.relative?std::to_string(ticks[i])+" s":fmtTime(ticks[i],"FULL");
        const auto split=o.relative?std::string::npos:time.find(' ');
        const auto date=split==std::string::npos?time:time.substr(0,split);
        const auto clock=split==std::string::npos?"":time.substr(split+1);
        const auto at=number(x(ticks[i]));
        out += "<line class=\"chart-grid\" x1=\""+at+"\" x2=\""+at+"\" y1=\"18\" y2=\"246\"/>"+
            "<text class=\"chart-label\" x=\""+at+"\" y=\"268\" text-anchor=\""+
            (i==0?"start":i+1==ticks.size()?"end":"middle")+"\"><tspan>"+escape(date)+"</tspan><tspan x=\""+at+
            "\" dy=\"17\">"+escape(clock)+"</tspan></text>";
    }
    out += "<rect class=\"chart-axis\" x=\"64\" y=\"18\" width=\"750\" height=\"228\"/>";
    for(std::size_t i=0;i<o.guides.size();++i) {
        const auto& guide=o.guides[o.guides.size()-1-i];
        if(guide.value>=o.low && guide.value<=o.high)
            out += "<line x1=\"64\" x2=\"814\" y1=\""+number(y(guide.value))+"\" y2=\""+number(y(guide.value))+
                "\" stroke=\""+escape(guide.color)+"\" stroke-dasharray=\"5 4\" opacity=\".7\"/>";
        const auto row=std::to_string(30+static_cast<int>(i)*24);
        out += "<line x1=\"828\" x2=\"844\" y1=\""+row+"\" y2=\""+row+"\" stroke=\""+escape(guide.color)+
            "\" stroke-width=\"2\"/><text class=\"chart-label\" x=\"851\" y=\""+std::to_string(34+static_cast<int>(i)*24)+"\">"+
            escape(guide.label)+"</text>";
    }
    const auto gaps=chartSampleGaps(visible,600);
    std::string path,dots;
    long long previous=0;
    for(std::size_t i=0;i<points.size();++i) {
        const auto& point=points[i];
        const auto gap=std::lower_bound(gaps.begin(),gaps.end(),previous,
            [](const ChartGap& g,long long t){return g.first<t;});
        const bool start=i==0 || (gap!=gaps.end() && gap->first<point.first);
        if(start)++result.segments;
        const auto px=number(x(point.first)),py=number(y(point.second));
        path += std::string(start?"M":"L")+px+","+py+" ";
        const bool marker=start || i+1==points.size() || visible.size()<=60;
        // Hidden dense-point targets reveal the exact retained timestamp/value
        // on hover; sparse and isolated samples are always visible.
        dots += "<circle class=\"sample "+std::string(marker?"visible":"dense")+"\" cx=\""+px+"\" cy=\""+py+
            "\" r=\""+(points.size()==1?"4":"2.5")+"\"><title>"+escape(fmtTime(o.relative?point.first+o.originalStart:point.first,"FULL")+" · "+valueText(point.second)+
            (o.unit.empty()?"":" "+o.unit))+"</title></circle>";
        previous=point.first;
    }
    out += "<g fill=\""+escape(o.color)+"\"><path class=\"signal-curve\" fill=\"none\" stroke=\""+escape(o.color)+
        "\" stroke-width=\"1.8\" stroke-linejoin=\"round\" stroke-linecap=\"round\" d=\""+path+"\"/>"+dots+"</g>";
    if(!o.comparison.empty()){auto second=o;second.comparison.clear();second.comparisonMode=false;second.color="#008300";second.originalStart=o.comparisonOriginalStart;auto html=renderReportChart(o.comparison,{},second).html;auto begin=html.find("<g fill=");auto finish=html.find("</g>",begin);if(begin!=std::string::npos&&finish!=std::string::npos){auto curve=html.substr(begin,finish+4-begin);auto dash=curve.find("class=\"signal-curve\"");if(dash!=std::string::npos)curve.insert(dash,"stroke-dasharray=\"7 4\" ");out+=curve;}}
    out+="</svg></div>";
    out+=payload+"</figure>";
    return result;
}
}
