// tablemodel.cpp — 虚拟表格的行选择与单元格格式化。
#include "tablemodel.h"

#include "log_analysis.h"
#include "log_time.h"
#include "signal_quality.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cctype>

namespace dl {

void buildTimelineView(const LogView& lines, LogView& timeline) {
    timeline.clear();
    // 事件通常远少于原始日志,不要为百万行稀疏时间线预留百万个指针。
    // 事件密集时 vector 仍会按需增长,这里只限制典型场景的预留上限。
    timeline.reserve(std::min<size_t>(lines.size(), 65536));
    for (const LogLine* line : lines) {
        bool keep = isEventLine(*line);
        if (line->tagText().compare(0, 9, "HEARTBEAT") == 0 &&
            (isFaultStart(line->msg) || isRecovered(line->msg, nullptr)))
            keep = true;
        if (keep) timeline.push_back(line);
    }
}

std::string timelineCellText(const LogLine& line, size_t column) {
    switch (column) {
    case 0: return fmtTime(line.t, "MD");
    case 1: return line.tagText();
    // ListView 只会请求可见行，无需在模型层截断。保留完整消息才能让横向滚动、
    // 详情面板与复制操作拿到同一份原文，而不是复制界面上的省略版本。
    case 2: return line.msg;
    default: return {};
    }
}

std::string metricCellText(const MetricRow& m, size_t column) {
    switch (column) {
    case 0:  return fmtTime(m.t, "MD");
    case 1:  return m.ch.empty() ? "-" : m.ch;
    case 2:  return m.cellId.empty() ? "-" : m.cellId.str();
    case 3:  return m.pci < 0 ? "-" : std::to_string(m.pci);
    case 4: {
        if (m.tac == UINT32_MAX) return "-";
        char value[16]{};
        const int width = std::max(1, std::min(8, static_cast<int>(m.tacDigits)));
        std::snprintf(value, sizeof(value), "%0*X", width, m.tac);
        return value;
    }
    case 5:  return m.csqRaw >= 0 ? std::to_string(m.csqRaw) : "-";
    case 6:  return m.tempMax != INT_MIN ? std::to_string(m.tempMax) : "-";
    case 7:  return m.consecFail != INT_MIN ? std::to_string(m.consecFail) : "-";
    case 8:  return m.rx != LLONG_MIN ? std::to_string(m.rx) : "-";
    case 9:  return m.drx != LLONG_MIN ? std::to_string(m.drx) : "-";
    case 10: return m.rsrp < 0 ? std::to_string(m.rsrp) : "-";
    case 11: return m.rsrq < 0 ? std::to_string(m.rsrq) : "-";
    case 12: {
        if (m.snr10 == 100000) return "-";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", m.snr10 / 10.0);
        return buf;
    }
    case 13: return m.rssiVal < 0 ? std::to_string(m.rssiVal) : "-";
    case 14: return m.srvVal >= 0 ? std::to_string(m.srvVal) : "-";
    case 15: return m.rat.empty() ? "-" : m.rat;
    case 16: return m.denyVal >= 0 ? std::to_string(m.denyVal) : "-";
    case 17: return m.oper.empty() ? "-" : m.oper;
    case 18: {
        const SignalQuality worst = metricOverallSignalQuality(m);
        if (worst == SignalQuality::Unknown) return "-";
        std::string metrics;
        auto add = [&](const char* name, SignalQuality quality) {
            if (quality != worst) return;
            if (!metrics.empty()) metrics += "/";
            metrics += name;
        };
        add("CSQ", metricSignalQuality(m, 5));
        add("RSRP", metricSignalQuality(m, 10));
        add("RSRQ", metricSignalQuality(m, 11));
        add("SNR", metricSignalQuality(m, 12));
        return std::string(signalQualityName(worst)) + " · " + metrics;
    }
    default: return {};
    }
}

std::string rawCellText(const LogLine& line, size_t column) {
    switch (column) {
    case 0: return std::to_string(line.lineNo);
    case 1: return line.ts.empty() ? fmtTime(line.t, "FULL") : line.ts;
    case 2: return line.levelText();
    case 3: return line.tagText();
    case 4: return line.msg;
    default: return {};
    }
}

static std::string tenth(int value) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.1f", value / 10.0);
    return text;
}

std::string cellSummaryCellText(const CellSummary& cell, size_t column) {
    switch (column) {
    case 0: return cell.cellId;
    case 1: return cell.pci < 0 ? "-" : std::to_string(cell.pci);
    case 2: {
        if (cell.tac == UINT32_MAX) return "-";
        char value[16]{};
        const int width = std::max(1, std::min(8, static_cast<int>(cell.tacDigits)));
        std::snprintf(value, sizeof(value), "%0*X", width, cell.tac);
        return value;
    }
    case 3: return std::to_string(cell.samples);
    case 4: return tenth(cell.sampleSharePermille);
    case 5: return fmtDur(cell.observedDwellSec);
    case 6: return cell.rsrpSamples ? tenth(cell.rsrpAvg10) : "-";
    case 7: return cell.rsrpSamples ? std::to_string(cell.rsrpMin) : "-";
    case 8: return cell.rsrqSamples ? tenth(cell.rsrqAvg10) : "-";
    case 9: return cell.snrSamples ? tenth(cell.snrAvg10) : "-";
    case 10: return cell.csqSamples ? tenth(cell.csqAvg10) : "-";
    case 11: return std::to_string(cell.switchesIn);
    case 12: return std::to_string(cell.switchesOut);
    case 13: return std::to_string(cell.outageStarts);
    case 14:
        if (cell.outageStarts && ((cell.rsrpSamples >= 5 && cell.rsrpAvg10 <= -1100) ||
                                  (cell.snrSamples >= 5 && cell.snrAvg10 <= 0))) return "疑似弱覆盖";
        if ((cell.rsrpSamples >= 5 && cell.rsrpAvg10 <= -1000) ||
            (cell.snrSamples >= 5 && cell.snrAvg10 <= 0)) return "需关注";
        return "正常";
    default: return {};
    }
}

static std::string csvSafeText(std::string value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    if (first < value.size() && (value[first] == '=' || value[first] == '+' ||
                                 value[first] == '-' || value[first] == '@'))
        value.insert(value.begin(), '\'');
    return value;
}

std::string metricCsvCellText(const MetricRow& metric, size_t column) {
    if (column == 0) return "=\"" + fmtTime(metric.t, "FULL") + "\"";
    std::string value = metricCellText(metric, column);
    if (column == 1 || column == 2 || column == 15 || column == 17 || column == 18)
        value = csvSafeText(std::move(value));
    return value;
}

} // namespace dl
