// log_filter.cpp — 单遍日志筛选与轻量借用视图
#include "log_filter.h"
#include "log_time.h"
#include "log_internal.h"

#include <algorithm>
#include <cstdio>
#include <regex>

namespace dl {

// ============================ 过滤 ============================
static bool parseBound(const std::string& s, const LogLine& base, long long& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    int baseY = 0, baseMo = 0, baseD = 0;
    // RFC3164 缺失年份时，解析器会在推定时间前加 '~' 明示证据边界。
    // 筛选基准仍应使用其推定出的年月日，不能因审计标记而静默忽略时间窗。
    const char* baseTs = base.ts.c_str();
    if (*baseTs == '~') ++baseTs;
    if (std::sscanf(baseTs, "%4d-%2d-%2d", &baseY, &baseMo, &baseD) != 3)
        return false;

    // 含 '-' 视为 "MM-DD HH:MM"(年份取日志基准行的年)
    if (t.find('-') != std::string::npos) {
        int M = 0, D = 0, h = 0, mi = 0;
        if (std::sscanf(t.c_str(), "%d-%d %d:%d", &M, &D, &h, &mi) == 4) {
            out = mkEpoch(baseY, M, D, h, mi, 0);
            return true;
        }
        return false;
    }

    // "HH:MM:SS" / "HH:MM" —— 贴到基准行那一天
    int h = 0, mi = 0, sec = 0;
    int n = std::sscanf(t.c_str(), "%d:%d:%d", &h, &mi, &sec);
    if (n >= 2) {
        out = mkEpoch(baseY, baseMo, baseD, h, mi, (n == 3 ? sec : 0));
        return true;
    }
    return false;
}

LogView applyFilterView(const std::vector<LogLine>& lines,
                        const std::string& tag, const std::string& grep,
                        const std::string& since, const std::string& until,
                        bool* grepBad)
{
    if (grepBad) *grepBad = false;

    // 条件只预处理一次。旧实现先完整复制 lines,再为标签/正则/时间各建一份 next,
    // 大日志会反复复制 LogLine 里的多个字符串；这里在一次遍历中完成全部判断。
    std::vector<std::string> want;
    std::string tg = trim(tag);
    if (!tg.empty()) {
        size_t a = 0;
        while (a <= tg.size()) {
            size_t b = tg.find(',', a);
            std::string piece = trim(tg.substr(a, (b == std::string::npos ? tg.size() : b) - a));
            if (!piece.empty()) want.push_back(lower(piece));
            if (b == std::string::npos) break;
            a = b + 1;
        }
    }

    std::regex rx;
    bool useRegex = false;
    std::string gp = trim(grep);
    if (!gp.empty()) {
        try {
            rx = std::regex(gp, std::regex::icase);
            useRegex = true;
        } catch (const std::regex_error&) {
            if (grepBad) *grepBad = true;   // 非法正则:忽略该条件,由调用方提示
        }
    }

    const bool needBounds = !trim(since).empty() || !trim(until).empty();
    bool boundsReady = !needBounds;
    bool hasLo = false, hasHi = false;
    long long lo = 0, hi = 0;

    LogView out;
    const bool noConditions = want.empty() && !useRegex && !needBounds;
    out.reserve(noConditions ? lines.size() : std::min<size_t>(lines.size(), 65536));
    for (const auto& l : lines) {
        if (!want.empty()) {
            std::string lt = lower(l.tagText());
            std::string first = lt;
            size_t sp = lt.find(' ');
            if (sp != std::string::npos) first.resize(sp);
            if (std::find(want.begin(), want.end(), lt) == want.end() &&
                std::find(want.begin(), want.end(), first) == want.end())
                continue;
        }
        if (useRegex && !std::regex_search(l.msg, rx)) continue;

        // 与旧语义一致:HH:MM 的年月日取“标签+正则筛选后第一行”,不是最终时间窗首行。
        if (!boundsReady) {
            hasLo = parseBound(since, l, lo);
            hasHi = parseBound(until, l, hi);
            boundsReady = true;
        }
        if (hasLo && l.t < lo) continue;
        if (hasHi && l.t > hi) continue;
        out.push_back(&l);
    }
    return out;
}

std::vector<LogLine> applyFilters(const std::vector<LogLine>& lines,
                                  const std::string& tag, const std::string& grep,
                                  const std::string& since, const std::string& until,
                                  bool* grepBad)
{
    LogView view = applyFilterView(lines, tag, grep, since, until, grepBad);
    std::vector<LogLine> out;
    out.reserve(view.size());
    for (const LogLine* l : view) out.push_back(*l);
    return out;
}


} // namespace dl
