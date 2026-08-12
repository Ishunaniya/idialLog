// tablemodel.cpp — 虚拟表格的行选择与单元格格式化。
#include "tablemodel.h"

#include "log_analysis.h"
#include "log_time.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdio>

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
    case 2: return line.msg.substr(0, 200);
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
    default: return {};
    }
}

} // namespace dl
