// selftest.cpp — 解析/分析层自测:与 tools/diallog.py 的输出对拍。
// 因为 logmodel.* 不含 Win32 依赖,本机(Linux)g++ 即可编译运行:
//   g++ -std=c++17 -O2 -o selftest selftest.cpp logmodel.cpp && ./selftest <日志>
#include "logmodel.h"
#include <climits>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace dl;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "用法: selftest <dial_*.log>\n"); return 2; }

    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { std::fprintf(stderr, "打不开: %s\n", argv[1]); return 2; }
    std::vector<std::string> raw;
    std::string line;
    while (std::getline(f, line)) raw.push_back(line);

    std::vector<LogLine> lines;
    std::vector<std::string> sessions;
    ParseAudit audit;
    parseLines(raw, lines, sessions, &audit);
    if (lines.empty()) { std::printf("无可解析行\n"); return 1; }

    std::printf("== 解析 ==\n");
    std::printf("  原始行:%zu  已解析:%zu  会话:%zu\n", raw.size(), lines.size(), sessions.size());

    // 未识别行审计:证明“没漏”的硬证据(必须自洽:各类之和 == 原始行数)
    std::printf("== 未识别行审计 ==\n");
    std::printf("  已解析:%zu  会话标记:%zu  空行:%zu  续行:%zu  未识别:%zu  (占比 %.3f%%)\n",
                audit.parsed, audit.session, audit.blank, audit.continuation, audit.unparsed,
                audit.unparsedRatio() * 100.0);
    size_t sum = audit.parsed + audit.session + audit.blank + audit.continuation + audit.unparsed;
    std::printf("  自洽校验: %zu + %zu + %zu + %zu + %zu = %zu %s %zu(原始行)\n",
                audit.parsed, audit.session, audit.blank, audit.continuation, audit.unparsed,
                sum, sum == audit.rawTotal ? "==" : "!=", audit.rawTotal);
    if (sum != audit.rawTotal) { std::printf("  ** 审计不自洽,解析器有漏计 **\n"); return 1; }
    for (auto& kv : audit.unparsedKinds) std::printf("  未识别分类 %-22s %zu\n", kv.first.c_str(), kv.second);
    for (size_t i = 0; i < audit.samples.size() && i < 5; ++i)
        std::printf("  样例 L%zu: %s\n", audit.samples[i].lineNo, audit.samples[i].text.substr(0,100).c_str());

    // 平台识别
    PlatformInfo pi = detectPlatform(lines);
    std::printf("== 平台识别 ==\n  %s\n  依据(L%zu): %s\n",
                pi.name.c_str(), pi.evidenceLine, pi.evidence.c_str());
    std::printf("  跨度: %s -> %s (%s)\n",
                fmtTime(lines.front().t, "FULL").c_str(),
                fmtTime(lines.back().t, "FULL").c_str(),
                fmtDur(lines.back().t - lines.front().t).c_str());

    // 断网
    auto outs = collectOutages(lines);
    long long total = 0, longest = 0, longestAt = 0;
    int done = 0, b0=0,b1=0,b2=0,b3=0;
    for (auto& o : outs) {
        if (!o.recovered) continue;
        done++; total += o.dur;
        if (o.dur > longest) { longest = o.dur; longestAt = o.end; }
        if (o.dur <= 30) b0++; else if (o.dur <= 60) b1++; else if (o.dur <= 300) b2++; else b3++;
    }
    double span = double(lines.back().t - lines.front().t);
    std::printf("== 断网 ==\n");
    std::printf("  次数:%zu  累计:%s  可用率:%.3f%%\n", outs.size(), fmtDur(total).c_str(),
                span > 0 ? 100.0 * (1.0 - total / span) : 0.0);
    std::printf("  最长:%s @ %s\n", fmtDur(longest).c_str(), fmtTime(longestAt, "MD").c_str());
    std::printf("  分布: <=30s:%d 31-60s:%d 1-5m:%d >5m:%d\n", b0,b1,b2,b3);

    // 指标
    auto rows = buildMetrics(lines);
    long long csqSum = 0; int csqN = 0, csqMin = 999, csqMax = -1, weak = 0;
    int tmaxMax = -999; long long tSum = 0; int tN = 0;
    std::map<std::string,int> chans;
    std::vector<std::pair<long long,long long>> rxs;
    for (auto& m : rows) {
        if (m.csqVal >= 0) {
            csqSum += m.csqVal; csqN++;
            if (m.csqVal < csqMin) csqMin = m.csqVal;
            if (m.csqVal > csqMax) csqMax = m.csqVal;
            if (m.csqVal < 10) weak++;
        }
        if (m.tempMax != INT_MIN) { int v = m.tempMax; if (v > tmaxMax) tmaxMax = v; tSum += v; tN++; }
        if (!m.ch.empty()) chans[m.ch]++;
        if (m.rx != LLONG_MIN) rxs.push_back({m.t, m.rx});
    }
    std::printf("== 心跳 ==\n");
    std::printf("  指标行:%zu\n", rows.size());
    if (csqN) std::printf("  CSQ min/avg/max: %d / %.1f / %d  样本:%d  弱信号(<10):%d\n",
                          csqMin, double(csqSum)/csqN, csqMax, csqN, weak);
    if (tN) std::printf("  温度 max:%d  avg:%.0f\n", tmaxMax, double(tSum)/tN);
    int chTot = 0; for (auto& kv : chans) chTot += kv.second;
    std::printf("  通道:");
    for (auto& kv : chans) std::printf(" %s:%.0f%%", kv.first.c_str(), 100.0*kv.second/chTot);
    std::printf("\n");

    // RX 停滞
    auto stalls = detectRxStall(rxs);
    std::printf("== RX_PKT 停滞 ==\n  段数:%zu\n", stalls.size());
    for (size_t i = 0; i < stalls.size() && i < 5; ++i)
        std::printf("  卡住 %s  %s -> %s\n", fmtDur(stalls[i].dur).c_str(),
                    fmtTime(stalls[i].start, "MD").c_str(), fmtTime(stalls[i].end, "HM").c_str());

    // 标签
    std::map<std::string,int> tc;
    for (auto& l : lines) { const std::string& tag = l.tagText(); tc[tag.empty() ? "(无标签)" : tag]++; }
    std::printf("== 标签 ==\n");
    for (auto& kv : tc) std::printf("  %-14s %d\n", kv.first.c_str(), kv.second);

    // 关键事件
    int sw=0, disc=0, states=0, cells=0, evt=0;
    for (auto& l : lines) {
        if (l.msg.find("switching to SIM") != std::string::npos ||
            l.msg.find("switching to Roamlink") != std::string::npos) sw++;
        if (l.msg.find("DataCall disconnected") != std::string::npos) disc++;
        if (l.tagText() == "STATE") states++;
        if (l.tagText().compare(0, 4, "CELL") == 0) cells++;
        bool keep = isEventLine(l);
        if (l.tagText().compare(0, 9, "HEARTBEAT") == 0 && (isFaultStart(l.msg) || isRecovered(l.msg, nullptr))) keep = true;
        if (keep) evt++;
    }
    std::printf("== 关键事件 ==\n  通道切换:%d SDK断开:%d 状态迁移:%d 小区变更:%d 时间线事件:%d\n",
                sw, disc, states, cells, evt);

    // 结论引擎
    auto finds = analyze(lines, outs, rows, pi, audit);
    std::printf("== 结论 ==\n  条数:%zu\n", finds.size());
    for (auto& f : finds) {
        std::printf("  [%s] %s\n",
                    f.severity == 2 ? "严重" : (f.severity == 1 ? "告警" : "信息"), f.title.c_str());
        std::printf("      依据: %s\n", f.detail.c_str());
        std::printf("      建议: %s\n", f.advice.c_str());
        if (f.ev.empty()) { std::printf("      ** 无证据的结论,违反铁律 **\n"); return 1; }
        for (size_t i = 0; i < f.ev.size() && i < 2; ++i)
            std::printf("      证据 L%zu %s: %s\n", f.ev[i].lineNo, f.ev[i].ts.c_str(),
                        f.ev[i].text.substr(0, 90).c_str());
    }

    // 过滤自检
    bool bad = false;
    auto only = applyFilters(lines, "ROAMLINK", "switching|Policy", "", "", &bad);
    std::printf("== 过滤自检 ==\n  tag=ROAMLINK grep=switching|Policy -> %zu 行 (正则非法:%s)\n",
                only.size(), bad ? "是" : "否");
    auto win = applyFilters(lines, "", "", "00:23", "00:29", &bad);
    std::printf("  时间窗 00:23-00:29 -> %zu 行\n", win.size());
    auto badrx = applyFilters(lines, "", "((((", "", "", &bad);
    std::printf("  非法正则 '((((' -> 未崩溃, grepBad=%s\n", bad ? "是" : "否");

    // 轻量视图必须与拥有对象的旧 API 语义一致,且每个元素直接借用原始 lines。
    LogView allView = applyFilterView(lines, "", "", "", "", nullptr);
    bool ptrOk = allView.size() == lines.size();
    for (size_t i = 0; ptrOk && i < allView.size(); ++i) ptrOk = allView[i] == &lines[i];
    auto viewOuts = collectOutages(allView);
    auto viewRows = buildMetrics(allView);
    auto viewFinds = analyze(allView, viewOuts, viewRows, pi, audit);
    bool same = ptrOk && viewOuts.size() == outs.size() && viewRows.size() == rows.size() &&
                viewFinds.size() == finds.size();
    for (size_t i = 0; same && i < outs.size(); ++i)
        same = viewOuts[i].start == outs[i].start && viewOuts[i].end == outs[i].end &&
               viewOuts[i].dur == outs[i].dur && viewOuts[i].recovered == outs[i].recovered &&
               viewOuts[i].l0Recovered == outs[i].l0Recovered &&
               viewOuts[i].startLine == outs[i].startLine && viewOuts[i].endLine == outs[i].endLine;
    auto sameMetric = [](const MetricRow& a, const MetricRow& b) {
        return a.t == b.t && a.ch == b.ch && a.csqRaw == b.csqRaw &&
               a.tempMax == b.tempMax && a.consecFail == b.consecFail &&
               a.rx == b.rx && a.drx == b.drx && a.rat == b.rat && a.oper == b.oper &&
               a.csqVal == b.csqVal && a.rsrp == b.rsrp &&
               a.rsrq == b.rsrq && a.snr10 == b.snr10 && a.rssiVal == b.rssiVal &&
               a.srvVal == b.srvVal && a.denyVal == b.denyVal &&
               a.lineNo == b.lineNo;
    };
    for (size_t i = 0; same && i < rows.size(); ++i) same = sameMetric(viewRows[i], rows[i]);
    auto sameFinding = [](const Finding& a, const Finding& b) {
        if (a.severity != b.severity || a.title != b.title || a.detail != b.detail ||
            a.advice != b.advice || a.ev.size() != b.ev.size()) return false;
        for (size_t i = 0; i < a.ev.size(); ++i)
            if (a.ev[i].lineNo != b.ev[i].lineNo || a.ev[i].ts != b.ev[i].ts ||
                a.ev[i].text != b.ev[i].text) return false;
        return true;
    };
    for (size_t i = 0; same && i < finds.size(); ++i) same = sameFinding(viewFinds[i], finds[i]);
    auto onlyView = applyFilterView(lines, "ROAMLINK", "switching|Policy", "", "", nullptr);
    bool filterSame = onlyView.size() == only.size();
    std::map<size_t, const LogLine*> byLine;
    for (const LogLine* l : allView) byLine[l->lineNo] = l;
    for (size_t i = 0; filterSame && i < only.size(); ++i) {
        auto it = byLine.find(only[i].lineNo);
        filterSame = it != byLine.end() && it->second == onlyView[i] &&
                     onlyView[i]->lineNo == only[i].lineNo &&
                     onlyView[i]->msg == only[i].msg;
    }
    std::printf("== 轻量视图自检 ==\n  全量指针归属:%s  分析结果一致:%s  筛选结果一致:%s\n",
                ptrOk ? "是" : "否", same ? "是" : "否", filterSame ? "是" : "否");
    if (!same || !filterSame) return 1;

    return 0;
}
