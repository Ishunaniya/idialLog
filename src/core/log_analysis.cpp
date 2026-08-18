// log_analysis.cpp — 平台识别、指标、断网分析与证据化结论引擎
#include "log_analysis.h"
#include "log_time.h"
#include "log_internal.h"
#include "radio_access.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace dl {

static std::string_view trimView(std::string_view s) {
    while (!s.empty() && (unsigned char)s.front() <= ' ') s.remove_prefix(1);
    while (!s.empty() && (unsigned char)s.back() <= ' ') s.remove_suffix(1);
    return s;
}

template <typename Fn>
static bool scanHbFields(const std::string& msg, Fn&& fn) {
    if (msg.find(':') == std::string::npos && msg.find('=') == std::string::npos) return false;
    bool found = false;
    size_t i = 0;
    while (i < msg.size()) {
        if (!isIdentChar(msg[i])) { i++; continue; }
        size_t j = i;
        while (j < msg.size() && isIdentChar(msg[j])) j++;
        if (j >= msg.size() || (msg[j] != ':' && msg[j] != '=')) { i = j; continue; }

        size_t k = j + 1;
        size_t e = msg.size();
        size_t bar = msg.find('|', k);
        if (bar != std::string::npos) e = bar;
        for (size_t q = k; q + 1 < e; ++q) {
            if (msg[q] != ' ' && msg[q] != '\t') continue;
            size_t r = q + 1;
            while (r < e && (msg[r] == ' ' || msg[r] == '\t')) r++;
            size_t t2 = r;
            while (t2 < e && isIdentChar(msg[t2])) t2++;
            if (t2 > r && t2 < e && (msg[t2] == ':' || msg[t2] == '=')) { e = q; break; }
        }
        if (e > k) {
            fn(std::string_view(msg.data() + i, j - i),
               trimView(std::string_view(msg.data() + k, e - k)));
            found = true;
        }
        i = (e > k) ? e : j;
    }
    return found;
}

std::map<std::string, std::string> hbFields(const std::string& msg) {
    std::map<std::string, std::string> d;
    scanHbFields(msg, [&](std::string_view key, std::string_view value) {
        d[std::string(key)] = std::string(value);
    });
    return d;
}

// ============================ 平台识别 ============================
// 全部基于源码实证的判别特征:
//  artery : 行格式为 seas_log(FMT_SEAS)                        seas_log.c:210
//  AG35   : 心跳含 "SLOT:" 或出现 [SLOT] 标签
//           (ec200a/dial/dial.cpp:1071-1075 #ifdef QL_MODULE_PLATFORM_AG35;
//            ec200a/slot/slot_mgr.c 整文件 AG35-only)
//  EC200A : 心跳含 SIM_AT:/SIM_CB: 且无 SLOT:                   ec200a/dial/dial.cpp:1077
//           (注:open_dial(B) dial.c:518 心跳与此**完全相同**,故 B 的日志
//            也会判为 EC200A —— 二者行格式与心跳字段无差异,不做无据区分)
//  EG25   : 心跳含 "CH:"/RL_FAIL/RX_PKT 或出现 [ROAMLINK] 标签  eg25/diag/diag.c:109-131
//  IMX    : 不可能出现 —— dialer_imx6ull.cpp 中 dial_log 调用数为 0(只用 printf),
//           CMakeLists.txt 的 IMX 分支也不链接 logger_sd.c,故 IMX 不产此类日志。
//           保留 PLAT_IMX 枚举仅为完整性,detectPlatform 永不返回它。
PlatformInfo detectPlatform(const std::vector<LogLine>& lines) {
    PlatformInfo pi;
    size_t seas = 0;
    const LogLine* seasEv = nullptr;
    const LogLine* ag35Ev = nullptr;
    const LogLine* ec200Ev = nullptr;
    const LogLine* eg25Ev = nullptr;

    for (const auto& l : lines) {
        if (l.fmt == FMT_SEAS) { seas++; if (!seasEv) seasEv = &l; }
        if (!ag35Ev && (l.tagText() == "SLOT" || l.msg.find("SLOT:") != std::string::npos)) ag35Ev = &l;
        if (!ec200Ev && l.msg.find("SIM_AT:") != std::string::npos &&
            l.msg.find("SIM_CB:") != std::string::npos) ec200Ev = &l;
        if (!eg25Ev && (l.tagText() == "ROAMLINK" ||
                        l.msg.find("CH:ROAMLINK") != std::string::npos ||
                        l.msg.find("CH:SIM") != std::string::npos ||
                        l.msg.find("RL_FAIL:") != std::string::npos)) eg25Ev = &l;
    }

    auto set = [&](Platform p, const char* nm, const LogLine* l, const char* why) {
        pi.plat = p; pi.name = nm;
        if (l) { pi.evidenceLine = l->lineNo; pi.evidence = std::string(why) + ":  " + l->msg.substr(0, 90); }
        else   { pi.evidence = why; }
    };

    if (seas > 0)      set(PLAT_ARTERY, "artery (open_dial_for_artery, seas_log)", seasEv, "行格式为 seas_log(时间带毫秒+级别+函数名)");
    else if (ag35Ev)   set(PLAT_AG35,   "AG35 (modem_mng, 双卡)",                  ag35Ev, "出现 AG35 专有的 SLOT 切卡痕迹");
    else if (eg25Ev)   set(PLAT_EG25,   "EG25 (modem_mng)",                        eg25Ev, "出现 EG25 专有的 ROAMLINK/CH 通道字段");
    else if (ec200Ev)  set(PLAT_EC200A, "EC200A (modem_mng 或 open_dial 上游)",    ec200Ev, "心跳为 SIM_AT/SIM_CB 格式且无 SLOT");
    else               set(PLAT_UNKNOWN, "未识别", nullptr, "无任何平台特征字段");
    return pi;
}

// ============================ 判定 ============================
bool isFaultStart(const std::string& msg) {
    return icontains(msg, "Ping failed") && icontains(msg, "fault timer started");
}

bool isRecovered(const std::string& msg, int* durSec) {
    std::string lo = lower(msg);
    // 形态一(modem_mng):"Network recovered after Ns" —— 时长在 after 后。
    {
        const char* key = "network recovered after ";
        size_t p = lo.find(key);
        if (p != std::string::npos) {
            size_t q = p + std::strlen(key), st = q;
            while (q < msg.size() && std::isdigit((unsigned char)msg[q])) q++;
            if (q > st) { if (durSec) *durSec = std::atoi(msg.substr(st, q - st).c_str()); return true; }
        }
    }
    // 形态二(open_dial/SDK):"Network Recovered ... Down: Ns" —— 恢复行自报停机时长。
    //   覆盖 "Network Recovered. Down: Ns" / "...in SDK phase (L0). Down: Ns" /
    //        "...via card-switch (now X). Down: Ns"。真机实证 3 变体。
    //   必须同时含 "network recovered" 与 "down:",避免把无关的 "Down:" 行误判。
    if (lo.find("network recovered") != std::string::npos) {
        size_t d = lo.find("down:");
        if (d != std::string::npos) {
            size_t q = d + 5;
            while (q < msg.size() && msg[q] == ' ') q++;
            size_t st = q;
            while (q < msg.size() && std::isdigit((unsigned char)msg[q])) q++;
            if (q > st) { if (durSec) *durSec = std::atoi(msg.substr(st, q - st).c_str()); return true; }
        }
    }
    return false;
}

// 时间线保留的“状态变化类”标签。取自三仓库 dial_log/SEAS_LOG 首参的穷举
// (modem_mng 357 处、open_dial 107 处、artery 4 处内嵌标签),多词标签按首段匹配:
//   "RECOVERY L1/L2/L3"→RECOVERY、"REG TIMEOUT"/"REG DIAG"→REG、
//   "PING OUT/FAIL/ERROR"→PING、"ZERO ADDR"→ZERO、"CELL CHANGE"→CELL(不在表内=噪声)
// 刻意排除:HEARTBEAT/CELL/CELL CHANGE(高频噪声,另有专门统计)、
//           LOGMIGR/LOGCLEAN/CLEANUP(日志自身维护,与网络无关)
static const char* kEventTags[] = {
    "STATE","SDK","ROAMLINK","SLOT","OPER","LED","CFUN","SIM","APN","INIT","MODEM",
    "RECOVERY","ERROR","WARN","WARNING","FATAL","INFO","EVENT","STATUS","ALARM","TZ","NANOMSG",
    // 以下为本次按源码穷举补齐(此前被静默丢弃)
    "PING","REG","ZERO","CPDUMP","COPS","SM","DIAG", nullptr
};
static const char* kErrTags[] = { "ERROR","WARN","WARNING","FATAL","ALARM", nullptr };

static bool inList(const char* const* list, const std::string& tag) {
    std::string t = lower(tag);
    for (int i = 0; list[i]; ++i) if (t == lower(list[i])) return true;
    return false;
}

bool isEventTag(const std::string& tag) {
    if (inList(kEventTags, tag)) return true;
    size_t sp = tag.find(' ');                      // "CELL CHANGE" → 取首段再判一次
    if (sp != std::string::npos) return inList(kEventTags, tag.substr(0, sp));
    return false;
}
bool isErrTag(const std::string& tag) { return inList(kErrTags, tag); }

// seas_log 的严重度在 level 字段(seas_log.c:239),不在标签里。
// 级别取值见 seas_log.h:66-121:[ALL]/[DEBUG]/[INFO]/[NOTICE]/[WARNING]/[ERROR]/[FATAL]
bool isErrLine(const LogLine& l) {
    if (isErrTag(l.tagText())) return true;
    if (l.fmt == FMT_SEAS) {
        return l.level == LEVEL_ERROR || l.level == LEVEL_WARNING ||
               l.level == LEVEL_FATAL || l.level == LEVEL_CRITICAL;
    }
    return false;
}

// 时间线保留:事件类标签,或 seas 的报错行(artery 大量日志无内嵌标签,
// 只按标签过滤会让 artery 的时间线几乎全空)
bool isEventLine(const LogLine& l) {
    if (isEventTag(l.tagText())) return true;
    if (l.fmt == FMT_SEAS) {
        if (isErrLine(l)) return true;
        return l.level == LEVEL_NOTICE;
    }
    return false;
}

// ============================ 分析 ============================
template <typename Lines>
static std::vector<Outage> collectOutagesImpl(const Lines& lines) {
    std::vector<Outage> outs;
    bool have = false;
    long long start = 0;
    size_t startLine = 0;
    for (const auto& item : lines) {
        const LogLine& l = lineRef(item);
        if (isFaultStart(l.msg) && !have) { have = true; start = l.t; startLine = l.lineNo; }
        int dur = 0;
        if (isRecovered(l.msg, &dur)) {
            Outage o;
            o.start = have ? start : l.t;
            o.startLine = have ? startLine : l.lineNo;
            o.end = l.t; o.dur = dur; o.recovered = true;
            o.endLine = l.lineNo;
            // SDK L0 自愈:恢复行含 "(L0)"(open_dial "Network Recovered in SDK phase (L0)")。
            // 这类是短断网、链路抖动,设备自愈,与走 L1+ 阶梯的深层恢复区分。
            o.l0Recovered = (l.msg.find("(L0)") != std::string::npos);
            outs.push_back(o);
            have = false;
        }
    }
    if (have) {
        Outage o; o.start = start; o.startLine = startLine; o.recovered = false;
        outs.push_back(o);
    }
    return outs;
}

std::vector<Outage> collectOutages(const std::vector<LogLine>& lines) {
    return collectOutagesImpl(lines);
}

std::vector<Outage> collectOutages(const LogView& lines) {
    return collectOutagesImpl(lines);
}

std::vector<Stall> detectRxStall(const std::vector<std::pair<long long,long long>>& rxs,
                                 long long minStallSec)
{
    std::vector<Stall> stalls;
    bool haveRun = false;
    long long runStart = 0;
    bool haveLast = false;
    std::pair<long long,long long> last{0,0};

    for (const auto& cur : rxs) {
        if (haveLast && cur.second == last.second) {
            if (!haveRun) { haveRun = true; runStart = last.first; }
        } else {
            if (haveRun) {
                long long dur = last.first - runStart;
                if (dur >= minStallSec) stalls.push_back(Stall{runStart, last.first, dur});
                haveRun = false;
            }
        }
        last = cur; haveLast = true;
    }
    if (haveRun && haveLast) {
        long long dur = last.first - runStart;
        if (dur >= minStallSec) stalls.push_back(Stall{runStart, last.first, dur});
    }
    return stalls;
}

// 各平台字段名不同(已逐一核对源码,不假设相同),故按候选名依次取:
//   温度  EC200A/AG35 "TEMP"(大写,ec200a/dial/dial.cpp:1074/1077)
//         EG25        "Temp"(小写,eg25/diag/diag.c:118)
//   通道  EG25 "CH"(diag.c:110);AG35 "SLOT"(dial.cpp:1074);artery 由 state 推导
//   失败数 EG25 "ConsecFail"(diag.c:118);artery "tcp_fail"(main.c:217)
//   收包  EG25 基础心跳 "RX_PKT"(仅 roamlink 通道,diag.c:130)
//         EG25 扩展心跳 "rx_packets"(仅 SIM 通道,**等号**赋值,diag.c:157)
//         artery "rx_packets"(main.c:239)
//   信号  EG25/EC200A "CSQ";artery "csq"(main.c:217)
// buildMetrics 的热路径不再构造 map/string:只保存原消息里的 string_view,随后立即转成
// 紧凑数值或三个确需保留原文的字段。公共 hbFields 仍复用同一扫描器保持兼容。
struct HeartbeatFields {
    bool any = false;
    std::string_view ch, slot, state, csqUpper, csqLower, tempTitle, tempUpper;
    std::string_view failTitle, failLower, rxUpper, rxLower;
    std::string_view rsrpUpper, rsrpLower, rsrqUpper, rsrqLower;
    std::string_view snrUpper, snrLower, rssiUpper, rssiLower;
    std::string_view srv, rat, deny, oper, cell, pci, tac;
};

static HeartbeatFields heartbeatFields(const std::string& msg) {
    HeartbeatFields f;
    f.any = scanHbFields(msg, [&](std::string_view k, std::string_view v) {
        if      (k == "CH")          f.ch = v;
        else if (k == "SLOT")        f.slot = v;
        else if (k == "state")       f.state = v;
        else if (k == "CSQ")         f.csqUpper = v;
        else if (k == "csq")         f.csqLower = v;
        else if (k == "Temp")        f.tempTitle = v;
        else if (k == "TEMP")        f.tempUpper = v;
        else if (k == "ConsecFail")  f.failTitle = v;
        else if (k == "tcp_fail")    f.failLower = v;
        else if (k == "RX_PKT")      f.rxUpper = v;
        else if (k == "rx_packets")  f.rxLower = v;
        else if (k == "RSRP")        f.rsrpUpper = v;
        else if (k == "rsrp")        f.rsrpLower = v;
        else if (k == "RSRQ")        f.rsrqUpper = v;
        else if (k == "rsrq")        f.rsrqLower = v;
        else if (k == "SNR")         f.snrUpper = v;
        else if (k == "snr")         f.snrLower = v;
        else if (k == "RSSI")        f.rssiUpper = v;
        else if (k == "rssi")        f.rssiLower = v;
        else if (k == "SRV")         f.srv = v;
        else if (k == "RAT")         f.rat = v;
        else if (k == "DENY")        f.deny = v;
        else if (k == "OPER")        f.oper = v;
        // 同一含义在各平台日志里的名字并不统一：EG25 使用 Cell/cellid，
        // EC200A、AG35 真机心跳使用 CID；部分模组版本使用 CellID/ECI/NCI。
        // 统一落入 cell，保证指标表、概览、小区分析与导出使用同一取值。
        else if (k == "Cell" || k == "cell" || k == "cellid" || k == "CellID" ||
                 k == "CELLID" || k == "cell_id" || k == "CELL_ID" ||
                 k == "CID" || k == "cid" || k == "ECI" || k == "eci" ||
                 k == "NCI" || k == "nci") f.cell = v;
        else if (k == "pci")         f.pci = v;
        else if (k == "tac")         f.tac = v;
    });
    return f;
}

static std::string_view firstOf(std::string_view a, std::string_view b) {
    return !a.empty() ? a : b;
}

static bool parseLong(std::string_view text, long long& value, bool requireWhole = false) {
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last = first + text.size();
    // std::strtoll 接受显式正号；from_chars 不接受。保留旧解析器对 "+20" 的兼容。
    if (*first == '+') {
        if (++first == last) return false;
    }
    auto result = std::from_chars(first, last, value, 10);
    return result.ptr != first && result.ec == std::errc{} && (!requireWhole || result.ptr == last);
}

static bool viewContainsIgnoreCase(std::string_view text, std::string_view needle) {
    if (needle.size() > text.size()) return false;
    for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower((unsigned char)text[i + j]) !=
                std::tolower((unsigned char)needle[j])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static void assignView(std::string& out, std::string_view value) {
    if (!value.empty()) out.assign(value.data(), value.size());
}

static bool usableCell(std::string_view value) {
    value = trimView(value);
    auto equalsIgnoreCase = [&](std::string_view expected) {
        if (value.size() != expected.size()) return false;
        for (size_t index = 0; index < value.size(); ++index)
            if (std::tolower(static_cast<unsigned char>(value[index])) !=
                std::tolower(static_cast<unsigned char>(expected[index]))) return false;
        return true;
    };
    return !value.empty() && value != "-" && !equalsIgnoreCase("N/A") &&
           !equalsIgnoreCase("FFFFFFFF") && !equalsIgnoreCase("init");
}

static bool parseUnsignedField(std::string_view value, unsigned base, std::uint32_t& output) {
    value = trimView(value);
    if (value.empty()) return false;
    if (base == 16 && value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
        value.remove_prefix(2);
    if (value.empty()) return false;
    std::uint32_t result = 0;
    for (char character : value) {
        unsigned digit = character >= '0' && character <= '9' ? static_cast<unsigned>(character - '0') :
                         character >= 'a' && character <= 'f' ? static_cast<unsigned>(character - 'a' + 10) :
                         character >= 'A' && character <= 'F' ? static_cast<unsigned>(character - 'A' + 10) : base;
        if (digit >= base || result > (UINT32_MAX - digit) / base) return false;
        result = result * base + digit;
    }
    output = result;
    return true;
}

struct CellState {
    std::string id;
    int pci = -1;
    std::uint32_t tac = UINT32_MAX;
    std::uint8_t tacDigits = 0;

    void clear() { id.clear(); pci = -1; tac = UINT32_MAX; tacDigits = 0; }
    void setId(std::string_view value) {
        value = trimView(value);
        if (id.size() != value.size() || !std::equal(id.begin(), id.end(), value.begin())) {
            pci = -1; tac = UINT32_MAX; tacDigits = 0;
        }
        id.assign(value.data(), value.size());
    }
};

static std::string_view unquote(std::string_view value) {
    value = trimView(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1); value.remove_suffix(1);
    }
    return value;
}

// Quectel +QENG servingcell 的 LTE/WCDMA/GSM/NR5G-SA 形态都把 Cell ID 放在
// 第 7 个 CSV 字段。LTE 的 PCI/TAC 分别位于第 8/13 个字段，NR5G-SA 的 TAC
// 位于第 9 个字段。这里只接纳明确的 servingcell 证据，不从邻区或数字位置猜测。
static bool qengCellState(const std::string& message, CellState& state) {
    const size_t marker = message.find("+QENG:");
    if (marker == std::string::npos) return false;
    std::array<std::string_view, 24> fields{};
    size_t count = 0, first = marker + 6;
    bool quoted = false;
    for (size_t index = first; index <= message.size() && count < fields.size(); ++index) {
        const char character = index < message.size() ? message[index] : ',';
        if (character == '"') quoted = !quoted;
        if (character == ',' && !quoted) {
            fields[count++] = unquote(std::string_view(message.data() + first, index - first));
            first = index + 1;
        }
    }
    if (count <= 6 || !viewContainsIgnoreCase(fields[0], "servingcell")) return false;
    if (!usableCell(fields[6])) { state.clear(); return true; }

    state.setId(fields[6]);
    state.pci = -1; state.tac = UINT32_MAX; state.tacDigits = 0;
    std::uint32_t parsed = 0;
    const bool lte = fields[2] == "LTE";
    const bool nrSa = fields[2] == "NR5G-SA";
    if ((lte || nrSa) && count > 7 && parseUnsignedField(fields[7], 10, parsed) &&
        parsed <= static_cast<std::uint32_t>(INT_MAX))
        state.pci = static_cast<int>(parsed);
    const size_t tacIndex = lte ? 12 : nrSa ? 8 : fields.size();
    if (tacIndex < count && parseUnsignedField(fields[tacIndex], 16, parsed)) {
        state.tac = parsed;
        std::string_view tac = trimView(fields[tacIndex]);
        if (tac.size() > 2 && tac[0] == '0' && (tac[1] == 'x' || tac[1] == 'X')) tac.remove_prefix(2);
        state.tacDigits = static_cast<std::uint8_t>(std::min<std::size_t>(tac.size(), 8));
    }
    return true;
}

static bool cellAfterChange(const LogLine& line, std::string& cell) {
    if (line.tagText() != "CELL CHANGE") return false;
    const size_t arrow = line.msg.find("->");
    if (arrow == std::string::npos) return false;
    size_t first = arrow + 2;
    while (first < line.msg.size() && static_cast<unsigned char>(line.msg[first]) <= ' ') ++first;
    size_t last = line.msg.find('|', first);
    if (last == std::string::npos) last = line.msg.size();
    while (last > first && static_cast<unsigned char>(line.msg[last - 1]) <= ' ') --last;
    const std::string_view value(line.msg.data() + first, last - first);
    cell = usableCell(value) ? std::string(value) : std::string{};
    return true;
}

static void updateCellState(const LogLine& line, CellState& state) {
    std::string changed;
    if (cellAfterChange(line, changed)) {
        if (changed.empty()) state.clear();
        else state.setId(changed);
    }
    if (qengCellState(line.msg, state)) return;
    if (line.tagText() == "DIAG") {
        const HeartbeatFields fields = heartbeatFields(line.msg);
        const std::string_view cell = fields.cell;
        if (!cell.empty()) {
            if (usableCell(cell)) state.setId(cell);
            else state.clear();
        }
    }
}

template <typename Lines>
static std::vector<MetricRow> buildMetricsImpl(const Lines& lines) {
    std::vector<MetricRow> rows;
    bool haveLastRx = false;
    long long lastRx = 0;
    CellState currentCell;
    std::uint16_t currentSource = 0;
    bool haveSource = false;

    for (const auto& item : lines) {
        const LogLine& l = lineRef(item);
        if (haveSource && l.sourceId != currentSource) currentCell.clear();
        currentSource = l.sourceId;
        haveSource = true;
        updateCellState(l, currentCell);
        if (l.tagText().compare(0, 9, "HEARTBEAT") != 0) continue;
        HeartbeatFields f = heartbeatFields(l.msg);
        if (!f.any) continue;

        MetricRow m;
        m.t  = l.t;
        m.lineNo = l.lineNo;
        if (!f.ch.empty()) assignView(m.ch, f.ch);
        else if (!f.slot.empty()) {                                    // AG35
            m.ch = "SLOT";
            m.ch.append(f.slot.data(), f.slot.size());
        } else if (!f.state.empty()) {                                 // artery
            m.ch = viewContainsIgnoreCase(f.state, "roamlink") ? "ROAMLINK" : "SIM";
        }
        assignView(m.rat, f.rat);
        assignView(m.oper, f.oper);
        const std::string_view explicitCell = f.cell;
        if (!explicitCell.empty() && usableCell(explicitCell)) {
            currentCell.setId(explicitCell);
        } else if (!explicitCell.empty()) {
            // 明确上报无效小区时清空驻留状态，避免把旧 Cell ID 错带到后续样本。
            currentCell.clear();
        }
        std::uint32_t compact = 0;
        if (!f.pci.empty()) {
            currentCell.pci = parseUnsignedField(f.pci, 10, compact) &&
                              compact <= static_cast<std::uint32_t>(INT_MAX)
                                  ? static_cast<int>(compact) : -1;
        }
        if (!f.tac.empty()) {
            currentCell.tac = UINT32_MAX; currentCell.tacDigits = 0;
            if (parseUnsignedField(f.tac, 16, compact)) {
                currentCell.tac = compact;
                std::string_view tac = trimView(f.tac);
                if (tac.size() > 2 && tac[0] == '0' && (tac[1] == 'x' || tac[1] == 'X')) tac.remove_prefix(2);
                currentCell.tacDigits = static_cast<std::uint8_t>(std::min<std::size_t>(tac.size(), 8));
            }
        }
        if (!currentCell.id.empty()) m.cellId.assign(currentCell.id);
        m.pci = currentCell.pci;
        if (currentCell.tac != UINT32_MAX) {
            m.tac = currentCell.tac;
            m.tacDigits = currentCell.tacDigits;
        }

        std::string_view temp = firstOf(f.tempTitle, f.tempUpper);
        if (!temp.empty()) {
            int best = INT_MIN; bool ok = false;
            size_t a = 0;
            while (a <= temp.size()) {
                size_t b = temp.find(',', a);
                std::string_view piece = trimView(temp.substr(a, (b == std::string_view::npos ? temp.size() : b) - a));
                if (!piece.empty()) {
                    long long value = 0;
                    if (parseLong(piece, value) && value >= INT_MIN && value <= INT_MAX) {
                        if ((int)value > best) best = (int)value;
                        ok = true;
                    }
                }
                if (b == std::string_view::npos) break;
                a = b + 1;
            }
            if (ok) m.tempMax = best;
        }
        long long number = 0;
        if (parseLong(firstOf(f.failTitle, f.failLower), number) &&
            number >= INT_MIN && number <= INT_MAX) m.consecFail = (int)number;

        // RX_PKT 与 rx_packets 是**同一个计数器**的两个打印点,故合并为一条序列:
        // 两者都来自 nw_get_rmnet_rx_packets_sum()(遍历 /sys/.../rmnet_data*/statistics/rx_packets
        // 求和,eg25/nw/nw.c:404-413)。RX_PKT 仅 roamlink 通道打(diag.c:130,值取自
        // p_dial_mng->roamlink_rx_packets,由 dial.c:776/788 以“只增不减”的方式更新);
        // rx_packets 仅 SIM 通道的 5min 扩展心跳打(diag.c:157,瞬时值)。
        // 合并的意义:此前只认 RX_PKT,SIM 通道整段没有 RX 样本,于是相邻两个 roamlink 样本
        // 会跨越整个 SIM 通道时段被误判成一大段“假死”(真实日志上曾误报 13m34s 一段,
        // 补入 rx_packets 样本后该段被真实数据拆成 4m31s+3m30s)。
        // 注意(仅源码推断,未经实证):切通道时 roamlink_rx_packets 会被清零
        // (dial.c:1857/1875),故切换点附近可能出现一次负增量;负增量不等于 0,
        // 不会被 drxZero 误判为假死。
        bool haveRx = parseLong(firstOf(f.rxUpper, f.rxLower), m.rx);
        if (haveRx && haveLastRx) {
            const bool overflow = (lastRx > 0 && m.rx < LLONG_MIN + lastRx) ||
                                  (lastRx < 0 && m.rx > LLONG_MAX + lastRx);
            if (!overflow) m.drx = m.rx - lastRx;
        }
        if (haveRx) { lastRx = m.rx; haveLastRx = true; }

        if (parseLong(firstOf(f.csqUpper, f.csqLower), number) &&
            number >= INT_MIN && number <= INT_MAX) {
            m.csqRaw = (int)number;
            if (number != 99) m.csqVal = (int)number;               // 99 = AT+CSQ 未知
        }
        // RSRP/RSRQ:dBm 精确信号值(负数)。真机两种分隔 hbFields 均能切出:
        //   open_dial "RSRP:-94 | RSRQ:-18"(竖线) / modem_mng "RSRP:-104 RSRQ:-10"(空格)。
        // 只接受负值,正数视为异常(1=无效标记)。
        if (parseLong(firstOf(f.rsrpUpper, f.rsrpLower), number) &&
            number >= INT_MIN && number < 0) m.rsrp = (int)number;
        if (parseLong(firstOf(f.rsrqUpper, f.rsrqLower), number) &&
            number >= INT_MIN && number < 0) m.rsrq = (int)number;
        // 两套 SDK 的 LTE SNR 都是 int16_t 原值,单位 0.1dB(真头示例:246=24.6dB)。
        // 0 与正值均有效,不能沿用 RSRP/RSRQ 的“只收负值”规则。
        if (parseLong(firstOf(f.snrUpper, f.snrLower), number, true) &&
            number >= -32768 && number <= 32767) m.snr10 = (int)number;
        if (parseLong(firstOf(f.rssiUpper, f.rssiLower), number, true) &&
            number >= INT_MIN && number < 0)
            m.rssiVal = (int)number;
        if (parseLong(f.srv, number, true) && number >= 0 && number <= 2)
            m.srvVal = (int)number;
        if (parseLong(f.deny, number, true) && number >= 0 && number <= INT_MAX)
            m.denyVal = (int)number;
        rows.push_back(std::move(m));
    }
    return rows;
}

std::vector<MetricRow> buildMetrics(const std::vector<LogLine>& lines) {
    return buildMetricsImpl(lines);
}

std::vector<MetricRow> buildMetrics(const LogView& lines) {
    return buildMetricsImpl(lines);
}

// ============================ 结论引擎 ============================
// 铁律:每条结论必须能追溯到具体日志证据(行号+时间戳),ev 为空的结论一律不输出。
// 各判据的源码出处逐条标注;凡"仅源码推断、未经真机实证"的分支已明确标注。

static Evidence mkEv(const LogLine& l) {
    Evidence e;
    e.lineNo = l.lineNo;
    e.ts     = l.ts;
    const std::string& tag = l.tagText();
    e.text   = (tag.empty() ? "" : "[" + tag + "] ") + l.msg.substr(0, 160);
    return e;
}

static std::string fmtSnr10(int raw) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f", raw / 10.0);
    return b;
}

namespace {

struct CellAccumulator {
    CellSummary summary;
    long long csqSum = 0;
    long long rsrpSum = 0;
    long long rsrqSum = 0;
    long long snrSum10 = 0;
};

template <typename Lines>
static std::vector<std::uint16_t> metricSourceIds(const Lines& lines,
                                                   const std::vector<MetricRow>& metrics) {
    std::vector<std::uint16_t> result(metrics.size(), 0);
    std::size_t lineIndex = 0;
    for (std::size_t metricIndex = 0; metricIndex < metrics.size(); ++metricIndex) {
        while (lineIndex < lines.size() &&
               lineRef(lines[lineIndex]).lineNo < metrics[metricIndex].lineNo) ++lineIndex;
        if (lineIndex < lines.size() &&
            lineRef(lines[lineIndex]).lineNo == metrics[metricIndex].lineNo)
            result[metricIndex] = lineRef(lines[lineIndex]).sourceId;
    }
    return result;
}

template <typename Lines>
static std::uint16_t sourceIdAtLine(const Lines& lines, std::size_t lineNo) {
    auto it = std::lower_bound(lines.begin(), lines.end(), lineNo,
        [](const auto& item, std::size_t number) { return lineRef(item).lineNo < number; });
    return it != lines.end() && lineRef(*it).lineNo == lineNo ? lineRef(*it).sourceId : 0;
}

template <typename Lines>
static CellAnalysis analyzeCellsImpl(const Lines& lines,
                                     const std::vector<MetricRow>& metrics,
                                     const std::vector<Outage>& outages) {
    CellAnalysis result;
    result.totalSamples = metrics.size();
    if (metrics.empty()) return result;

    const auto sourceIds = metricSourceIds(lines, metrics);
    std::vector<CellAccumulator> accumulators;
    std::map<std::string, std::size_t> cellIndexes;
    std::map<std::pair<std::string, std::string>, std::size_t> transitionIndexes;

    auto cellIndex = [&](std::string_view id) -> std::size_t {
        const std::string key(id);
        auto found = cellIndexes.find(key);
        if (found != cellIndexes.end()) return found->second;
        const std::size_t index = accumulators.size();
        CellAccumulator accumulator;
        accumulator.summary.cellId = key;
        accumulators.push_back(std::move(accumulator));
        cellIndexes.emplace(key, index);
        return index;
    };

    auto transitionIndex = [&](const std::string& from, const std::string& to) -> std::size_t {
        const auto key = std::make_pair(from, to);
        auto found = transitionIndexes.find(key);
        if (found != transitionIndexes.end()) return found->second;
        const std::size_t index = result.transitions.size();
        CellTransition transition;
        transition.fromCell = from;
        transition.toCell = to;
        result.transitions.push_back(std::move(transition));
        transitionIndexes.emplace(key, index);
        return index;
    };

    std::string previousCell, cellBeforePrevious;
    std::uint16_t previousSource = 0;
    long long previousTime = 0, previousSwitchTime = 0;
    for (std::size_t index = 0; index < metrics.size(); ++index) {
        const MetricRow& metric = metrics[index];
        if (metric.cellId.empty()) continue;
        const std::string id = metric.cellId.str();
        CellAccumulator& accumulator = accumulators[cellIndex(id)];
        CellSummary& summary = accumulator.summary;
        if (summary.samples == 0) {
            summary.first = metric.t;
            summary.firstEvidenceLine = metric.lineNo;
            summary.pci = metric.pci;
            summary.tac = metric.tac;
            summary.tacDigits = metric.tacDigits;
        }
        summary.last = metric.t;
        summary.samples++;
        result.samplesWithCell++;
        if (result.first == 0 || metric.t < result.first) result.first = metric.t;
        if (metric.t > result.last) result.last = metric.t;

        if (metric.pci >= 0) summary.pci = metric.pci;
        if (metric.tac != UINT32_MAX) {
            summary.tac = metric.tac;
            summary.tacDigits = metric.tacDigits;
        }
        const bool lteReference = usesLteEngineeringReference(metric.rat);
        if (lteReference && metric.csqVal >= 0) {
            accumulator.csqSum += metric.csqVal;
            summary.csqSamples++;
            summary.csqMin = std::min(summary.csqMin, metric.csqVal);
            summary.csqMax = std::max(summary.csqMax, metric.csqVal);
        }
        if (lteReference && metric.rsrp < 0) {
            accumulator.rsrpSum += metric.rsrp;
            summary.rsrpSamples++;
            summary.rsrpMin = std::min(summary.rsrpMin, metric.rsrp);
            summary.rsrpMax = std::max(summary.rsrpMax, metric.rsrp);
        }
        if (lteReference && metric.rsrq < 0) {
            accumulator.rsrqSum += metric.rsrq;
            summary.rsrqSamples++;
            summary.rsrqMin = std::min(summary.rsrqMin, metric.rsrq);
            summary.rsrqMax = std::max(summary.rsrqMax, metric.rsrq);
        }
        if (lteReference && metric.snr10 != 100000) {
            accumulator.snrSum10 += metric.snr10;
            summary.snrSamples++;
            summary.snrMin10 = std::min(summary.snrMin10, metric.snr10);
            summary.snrMax10 = std::max(summary.snrMax10, metric.snr10);
        }

        const bool sameSource = previousCell.empty() || sourceIds[index] == previousSource;
        if (!sameSource) {
            previousCell.clear();
            cellBeforePrevious.clear();
            previousSwitchTime = 0;
        }
        if (!previousCell.empty() && id == previousCell) {
            // 相邻同小区样本才累加观测驻留；超过 10 分钟的采样空洞不冒充连续驻留。
            const long long delta = metric.t - previousTime;
            if (delta >= 0 && delta <= 600)
                accumulators[cellIndex(previousCell)].summary.observedDwellSec += delta;
        } else if (!previousCell.empty()) {
            CellSummary& from = accumulators[cellIndex(previousCell)].summary;
            from.switchesOut++;
            summary.switchesIn++;
            result.switchCount++;
            CellTransition& transition = result.transitions[transitionIndex(previousCell, id)];
            if (transition.count == 0) {
                transition.first = metric.t;
                transition.firstEvidenceLine = metric.lineNo;
            }
            transition.last = metric.t;
            transition.count++;

            // A→B 后 5 分钟内又 B→A 记为一次乒乓；这是工程观察规则，不等同网络根因。
            if (!cellBeforePrevious.empty() && id == cellBeforePrevious &&
                previousSwitchTime > 0 && metric.t >= previousSwitchTime &&
                metric.t - previousSwitchTime <= 300) {
                transition.pingPongCount++;
                transition.pingPongEvidenceLine = metric.lineNo;
                transition.pingPongEvidenceTime = metric.t;
                result.pingPongCount++;
            }
            cellBeforePrevious = previousCell;
            previousSwitchTime = metric.t;
        }
        previousCell = id;
        previousSource = sourceIds[index];
        previousTime = metric.t;
    }

    // 将每次断网关联到同来源、断网前 10 分钟内最近一次有小区 ID 的指标。
    // 按证据行归并推进，避免“每次断网倒扫全部指标”的 O(断网×指标) 开销。
    std::vector<const Outage*> orderedOutages;
    orderedOutages.reserve(outages.size());
    for (const Outage& outage : outages) orderedOutages.push_back(&outage);
    std::stable_sort(orderedOutages.begin(), orderedOutages.end(),
        [](const Outage* a, const Outage* b) { return a->startLine < b->startLine; });
    std::map<std::uint16_t, std::size_t> latestMetric;
    std::size_t metricCursor = 0;
    for (const Outage* outagePtr : orderedOutages) {
        const Outage& outage = *outagePtr;
        while (metricCursor < metrics.size() && metrics[metricCursor].lineNo <= outage.startLine) {
            if (!metrics[metricCursor].cellId.empty()) latestMetric[sourceIds[metricCursor]] = metricCursor;
            ++metricCursor;
        }
        const std::uint16_t sourceId = sourceIdAtLine(lines, outage.startLine);
        auto latest = latestMetric.find(sourceId);
        if (latest != latestMetric.end()) {
            const std::size_t index = latest->second;
            const MetricRow& metric = metrics[index];
            const long long age = outage.start - metric.t;
            // 行号在前但时钟略晚时，退回同来源更早样本，不能让未来样本遮住有效关联。
            std::size_t fallback = index;
            while (age < 0 && fallback > 0) {
                --fallback;
                if (sourceIds[fallback] != sourceId || metrics[fallback].cellId.empty()) continue;
                const long long fallbackAge = outage.start - metrics[fallback].t;
                if (fallbackAge >= 0) {
                    const MetricRow& candidate = metrics[fallback];
                    if (fallbackAge <= 600) {
                        CellSummary& summary = accumulators[cellIndex(candidate.cellId.view())].summary;
                        summary.outageStarts++;
                        if (summary.firstOutageLine == 0) {
                            summary.firstOutageLine = outage.startLine;
                            summary.firstOutageTime = outage.start;
                        }
                    }
                    break;
                }
            }
            if (age >= 0 && age <= 600) {
                CellSummary& summary = accumulators[cellIndex(metric.cellId.view())].summary;
                summary.outageStarts++;
                if (summary.firstOutageLine == 0) {
                    summary.firstOutageLine = outage.startLine;
                    summary.firstOutageTime = outage.start;
                }
            }
        }
    }

    result.cells.reserve(accumulators.size());
    for (CellAccumulator& accumulator : accumulators) {
        CellSummary& summary = accumulator.summary;
        if (summary.csqSamples) summary.csqAvg10 = int(accumulator.csqSum * 10 /
            static_cast<long long>(summary.csqSamples));
        if (summary.rsrpSamples) summary.rsrpAvg10 = int(accumulator.rsrpSum * 10 /
            static_cast<long long>(summary.rsrpSamples));
        if (summary.rsrqSamples) summary.rsrqAvg10 = int(accumulator.rsrqSum * 10 /
            static_cast<long long>(summary.rsrqSamples));
        if (summary.snrSamples) summary.snrAvg10 = int(accumulator.snrSum10 /
            static_cast<long long>(summary.snrSamples));
        if (result.samplesWithCell)
            summary.sampleSharePermille = int(summary.samples * 1000 / result.samplesWithCell);
        result.cells.push_back(std::move(summary));
    }
    std::stable_sort(result.cells.begin(), result.cells.end(), [](const CellSummary& a, const CellSummary& b) {
        if (a.outageStarts != b.outageStarts) return a.outageStarts > b.outageStarts;
        return a.samples > b.samples;
    });
    std::stable_sort(result.transitions.begin(), result.transitions.end(),
        [](const CellTransition& a, const CellTransition& b) {
            if (a.pingPongCount != b.pingPongCount) return a.pingPongCount > b.pingPongCount;
            return a.count > b.count;
        });
    return result;
}

} // namespace

CellAnalysis analyzeCells(const std::vector<LogLine>& lines,
                          const std::vector<MetricRow>& metrics,
                          const std::vector<Outage>& outages) {
    return analyzeCellsImpl(lines, metrics, outages);
}

CellAnalysis analyzeCells(const LogView& lines,
                          const std::vector<MetricRow>& metrics,
                          const std::vector<Outage>& outages) {
    return analyzeCellsImpl(lines, metrics, outages);
}

// 断网根因分类(取值即 Finding 的分组键)
enum Cause { C_WEAK, C_DATADEAD, C_SWITCHING, C_DENIED, C_NOTREADY, C_SDK_L0, C_UNKNOWN, C_N };
static const char* kCauseName[C_N] = {
    "弱信号", "数据假死(RX_PKT 停滞)", "切卡/选网/CFUN 期间",
    "注册被拒/受限/疑似账户问题", "数据服务未就绪",
    "SDK 短断网(链路抖动,非设备故障)", "未能归类"
};

static bool linePtrTimesSorted(const std::vector<const LogLine*>& v) {
    return std::is_sorted(v.begin(), v.end(),
                          [](const LogLine* a, const LogLine* b) { return a->t < b->t; });
}

// 正常日志按时间单调,可二分到断网窗口；若检测到时钟倒退,退回原顺序全扫，
// 保持旧行为及证据选择顺序。sorted 由调用方预先算一次,不在每次断网里重复 O(N)。
static const LogLine* firstLineInWindow(const std::vector<const LogLine*>& v,
                                        long long lo, long long hi, bool sorted) {
    if (sorted) {
        auto it = std::lower_bound(v.begin(), v.end(), lo,
                                   [](const LogLine* l, long long t) { return l->t < t; });
        return it != v.end() && (*it)->t <= hi ? *it : nullptr;
    }
    for (const auto* l : v)
        if (l->t >= lo && l->t <= hi) return l;
    return nullptr;
}

template <typename Lines>
static std::vector<Finding> analyzeImpl(const Lines& lines,
                                        const std::vector<Outage>& outs,
                                        const std::vector<MetricRow>& mets,
                                        const PlatformInfo& pi,
                                        const ParseAudit& audit,
                                        const CellAnalysis* precomputedCells)
{
    std::vector<Finding> fs;
    if (lines.empty()) return fs;

    // ---- 预扫:各类特征行(全部留证据指针)----
    std::vector<const LogLine*> evNeverConn, evPolicy, evRecL1, evRecL2, evRecL3,
                                evDenied, evLimited, evSuspectedAccount, evRegQueryFail,
                                evRegistrationIssue, evCpdump, evSlot, evOper, evCfun,
                                evNotReady;
    for (const auto& item : lines) {
        const LogLine& l = lineRef(item);
        // EC200A 门控日志(ec200a/dial/dial.cpp:1615/1622)。EG25 无对应日志:
        // 其门控是静默的(eg25/dial/dial.c:974 的 has_connected_once 条件)。
        if (icontains(l.msg, "never-connected"))            evNeverConn.push_back(&l);
        if (icontains(l.msg, "Policy1:") || icontains(l.msg, "Policy2:") ||
            icontains(l.msg, "policy="))                    evPolicy.push_back(&l);
        // 按**事件**收,不是按行收:一次恢复会打多行。
        // 【样本实证】真机 EG25 1.31.15:一次 L1 打 2 行(LastErr + "REG down, skip redial",同秒);
        // 一次 L2 打 3 行(LastErr + "AT+CFUN=0 rsp" + "AT+CFUN=1 rsp",跨 3 秒)。
        // 按行计数会把 4 次 L1 报成 8 次、1 次 L2 报成 3 次。
        // 用时间邻近合并:恢复阶梯自身有 ≥60s 节流(eg25/dial/dial.c),故 30s 内的同级行必属同一次。
        auto pushEvent = [&l](std::vector<const LogLine*>& v) {
            if (!v.empty() && l.t - v.back()->t <= 30) return;   // 同一次的后续行,不另计
            v.push_back(&l);
        };
        if (l.tagText().compare(0, 11, "RECOVERY L1") == 0) pushEvent(evRecL1);
        if (l.tagText().compare(0, 11, "RECOVERY L2") == 0) pushEvent(evRecL2);
        if (l.tagText().compare(0, 11, "RECOVERY L3") == 0) pushEvent(evRecL3);
        // 2026-08-18 四份产品代码新增首次初始化 SIM 诊断。嵌套标记留在正文中:
        // FMT_SD 的外层标签是 INIT，FMT_SEAS 同样会先剥掉第一个 [INIT]。
        // 必须同时校验嵌套标记和固定措辞，不能见到普通的 "limited service" 就下结论。
        const bool newNetworkRejected =
            icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "NETWORK REJECTED");
        const bool limitedService =
            icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "LIMITED SERVICE");
        const bool suspectedAccount =
            icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "SUSPECTED subscription");
        const bool regQueryFailed =
            icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "CEREG query/parse failed");

        // 旧版 EC200A/open_dial 直出 Registration Denied；新版四产品改为带完整 AT
        // 证据的 NETWORK REJECTED。二者都只在明确 REG=3 时产生，属于同一类直证。
        if (icontains(l.msg, "Registration Denied") || newNetworkRejected) {
            evDenied.push_back(&l);
            evRegistrationIssue.push_back(&l);
        }
        if (limitedService) {
            evLimited.push_back(&l);
            evRegistrationIssue.push_back(&l);
        }
        if (suspectedAccount) {
            evSuspectedAccount.push_back(&l);
            evRegistrationIssue.push_back(&l);
        }
        if (regQueryFailed) evRegQueryFail.push_back(&l);
        /* 数据服务未就绪:AP 侧数据服务(ql_netd)没起来 → ql_data_call_init 失败。
         * 【源码穷举】真代码实际打的就这两句(rtms_sdk HEAD, apps/modem_mng):
         *   "[INIT] data_call_init failed, ret=%d"
         *   "[INIT] data_call_init retrying, remaining=%d"
         * 【样本实证】sim/hostrun 的 datacall_init_fail 场景(真代码产出)确实打出
         *   "data_call_init retrying, remaining=200/180/160..."。
         * 早先 C_NOTREADY 只在 enum 里有名字、**代码里从无任何赋值** —— 是死分支,
         * 永远不可能触发(三种数据源全空不是"没样本",是它根本是死的)。 */
        if (icontains(l.msg, "data_call_init failed") ||
            icontains(l.msg, "data_call_init retrying"))    evNotReady.push_back(&l);
        // 只认"真的发现了 dump"这一句,不能见 [CPDUMP] 标签就报基带崩溃。
        // 【穷举证明】源码里 [CPDUMP] 共 9 种消息(rtms_sdk + open_dial 的 HEAD),
        // 只有 "Found %d existing CP dump(s)" 表示确实崩过;其余 8 种是例行挂载/卸载/
        // "No existing CP dumps." 等。
        // 【样本实证】真机 EC200A 1.28.4(完全正常的设备)只打了 "Bind mounted ..." 和
        // "No existing CP dumps.",旧实现据此报出 [严重] 基带崩溃 —— 假阳性,会误导排查方向。
        if (l.tagText() == "CPDUMP" && icontains(l.msg, "existing CP dump") &&
            !icontains(l.msg, "No existing"))                evCpdump.push_back(&l);
        if (l.tagText() == "SLOT")                          evSlot.push_back(&l);
        if (l.tagText() == "OPER")                          evOper.push_back(&l);
        if (l.tagText() == "CFUN")                          evCfun.push_back(&l);
    }
    // SDK 注网摘要是 2026-07/08 四份产品代码新增字段。只有 SRV!=FULL 且 DENY>0
    // 才算拒绝证据；DENY=0 不臆测。两套 SDK 的 DENY 数字表不同,这里只保留原码。
    std::vector<const MetricRow*> evSdkDeny;
    for (const auto& m : mets)
        if (m.srvVal >= 0 && m.srvVal != 2 && m.denyVal > 0) evSdkDeny.push_back(&m);

    const bool metsTimeSorted = std::is_sorted(
        mets.begin(), mets.end(), [](const MetricRow& a, const MetricRow& b) { return a.t < b.t; });
    const bool slotTimeSorted = linePtrTimesSorted(evSlot);
    const bool operTimeSorted = linePtrTimesSorted(evOper);
    const bool cfunTimeSorted = linePtrTimesSorted(evCfun);
    const bool registrationIssueTimeSorted = linePtrTimesSorted(evRegistrationIssue);
    const bool notReadyTimeSorted = linePtrTimesSorted(evNotReady);

    // ---- 1. 从未联网(SIM/账户问题):恢复阶梯被 has_connected_once 门控 ----
    if (!evNeverConn.empty()) {
        Finding f;
        f.severity = 2;
        f.title  = "设备从未成功联网,L1/L2/L3 恢复阶梯被 has_connected_once 门控关闭";
        f.detail = "日志出现 never-connected 门控行。按设计(ec200a/dial/dial.cpp:1615),"
                   "从未 ping 通的设备只停留在 L0,不做软重拨/射频重置/退出——因为这类故障"
                   "多为 SIM 卡或账户问题,复位改变不了。";
        f.advice = "查 SIM 卡是否插好/欠费/未开通数据业务/APN 是否匹配;确认 REG 注册状态。"
                   "复位类恢复对该场景无效,不要靠重启设备解决。";
        for (size_t i = 0; i < evNeverConn.size() && i < 3; ++i) f.ev.push_back(mkEv(*evNeverConn[i]));
        fs.push_back(std::move(f));
    }

    // ---- 2. 注册与账户诊断 ----
    if (!evDenied.empty() || !evSdkDeny.empty()) {
        Finding f;
        f.severity = 2;
        f.title  = "网络注册被明确拒绝(REG=3 / SDK DENY)";
        f.detail = "【源码直证】产品首次初始化诊断输出 NETWORK REJECTED/旧版 Registration "
                   "Denied，或 SDK 摘要同时满足 SRV!=2(FULL) 且 DENY>0。DENY 是平台 SDK "
                   "原始码；EC200A 与 EG25 编码表不同，工具不跨平台套用名称。";
        f.advice = "核对卡状态(欠费/停机/未开通漫游或数据)、IMSI 与运营商签约是否一致;"
                   "海外场景确认是否需要选网(COPS)。";
        for (size_t i = 0; i < evDenied.size() && i < 3; ++i) f.ev.push_back(mkEv(*evDenied[i]));
        for (size_t i = 0; i < evSdkDeny.size() && f.ev.size() < 3; ++i) {
            Evidence e; e.lineNo = evSdkDeny[i]->lineNo; e.ts = fmtTime(evSdkDeny[i]->t, "MD");
            e.text = "SDK注网摘要 SRV=" + std::to_string(evSdkDeny[i]->srvVal) +
                     " RAT=" + (evSdkDeny[i]->rat.empty() ? "-" : evSdkDeny[i]->rat) +
                     " DENY=" + std::to_string(evSdkDeny[i]->denyVal);
            f.ev.push_back(std::move(e));
        }
        fs.push_back(std::move(f));
    }

    if (!evLimited.empty()) {
        Finding f;
        f.severity = 1;
        f.title = "SIM 注册处于受限服务(LIMITED SERVICE)";
        f.detail = "【源码直证】产品读取到 CEREG 受限状态(REG=6..13)并输出 LIMITED SERVICE。"
                   "这表示普通分组数据可能不可用，但不能仅凭该行断言欠费或停机。";
        f.advice = "核对证据中的 CEREG/COPS/CGATT/CEER 原始响应，并向运营商确认业务开通、"
                   "漫游和网络限制状态。";
        for (size_t i = 0; i < evLimited.size() && i < 3; ++i) f.ev.push_back(mkEv(*evLimited[i]));
        fs.push_back(std::move(f));
    }

    if (!evSuspectedAccount.empty()) {
        Finding f;
        f.severity = 1;
        f.title = "疑似 SIM 账户/订阅异常(SUSPECTED)";
        f.detail = "【推断】产品仅在 SIM READY、信号良好且 REG=0 连续至少 120 秒时输出该诊断。"
                   "标准 AT 无运营商后台停机字段，因此这不是欠费/停机的确定证据。";
        f.advice = "保留行内 CPIN/CEREG/COPS/CGATT/CEER 证据，向运营商核验账户、数据业务和"
                   "漫游权限；同时排除覆盖与选网问题。";
        for (size_t i = 0; i < evSuspectedAccount.size() && i < 3; ++i)
            f.ev.push_back(mkEv(*evSuspectedAccount[i]));
        fs.push_back(std::move(f));
    }

    if (!evRegQueryFail.empty()) {
        Finding f;
        f.severity = 1;
        f.title = "CEREG 查询/解析失败，注册状态未知";
        f.detail = "【源码直证】产品明确记录 CEREG query/parse failed；此时注册状态未知，"
                   "不得据此推断 SIM 或账户异常。";
        f.advice = "检查行内 Raw 响应、AT 端口和模组应答完整性；修复查询后再判断注册原因。";
        for (size_t i = 0; i < evRegQueryFail.size() && i < 3; ++i)
            f.ev.push_back(mkEv(*evRegQueryFail[i]));
        fs.push_back(std::move(f));
    }

    // ---- 3. 模组 CP 崩溃 ----
    /* 数据服务未就绪的**独立结论**(不依附于断网事件)。
     * 【实证】真代码模拟 datacall_init_fail 场景:进程从启动就卡在 data_call_init 重试,
     * **从未进入过已连接状态 → 一条断网记录都没有**(断网是"连上后又掉");
     * 而根因分类是挂在"每次断网"上遍历的 → 永远轮不到它。
     * 所以这个根因必须同时做成独立结论,否则最典型的场景反而报不出来。 */
    if (!evNotReady.empty()) {
        Finding f;
        f.severity = 2;
        f.title  = "数据服务未就绪:ql_data_call_init 失败/重试 " + std::to_string(evNotReady.size()) + " 次";
        f.detail = "AP 侧数据服务(ql_netd)没起来,不是射频或信号问题。此时 AT 命令照样能通"
                   "(那走 ql_atc),但数据业务起不来,表现为\"信号好好的却上不了网\"。";
        f.advice = "查 ql_netd 守护进程是否在跑(ps);重拨/CFUN/换卡对这类故障都无效 —— "
                   "它们治射频侧,而问题在 AP 侧的数据服务。";
        for (size_t i = 0; i < evNotReady.size() && i < 3; ++i) f.ev.push_back(mkEv(*evNotReady[i]));
        fs.push_back(std::move(f));
    }

    if (!evCpdump.empty()) {
        Finding f;
        f.severity = 2;
        f.title  = "检测到模组 CP dump(基带崩溃)";
        f.detail = "日志出现 \"[CPDUMP] Found N existing CP dump(s)\" —— 模组基带侧确实发生过崩溃转储。\n(注:仅 \"Found ... existing CP dump(s)\" 计入;例行的 bind mount / \"No existing CP dumps\" 不算。)";
        f.advice = "取回 dump 文件反馈模组厂商;纯应用层重拨/CFUN 无法根治固件崩溃。";
        for (size_t i = 0; i < evCpdump.size() && i < 3; ++i) f.ev.push_back(mkEv(*evCpdump[i]));
        fs.push_back(std::move(f));
    }

    // ---- 4. 逐次断网根因分类 ----
    // 判据(均为窗口内的实证特征,窗口 = 断网起点前 90s 至恢复点):
    //   弱信号     :窗口内心跳 CSQ 有效样本的最小值 < 10(CSQ<10 ≈ RSSI<-95dBm)
    //   数据假死   :窗口内出现 ΔRX==0(RX_PKT 不增长)
    //   切卡/选网  :窗口内出现 [SLOT]/[OPER]/[CFUN]
    //   注册/账户异常:窗口内出现 Registration Denied、NETWORK REJECTED、LIMITED SERVICE
    //                 或产品有边界的 SUSPECTED subscription issue；查询失败不作为根因。
    size_t causeCnt[C_N] = {0};
    std::vector<Evidence> causeEv[C_N];
    for (const auto& o : outs) {
        long long lo = o.start - 90, hi = o.recovered ? o.end : lineRef(lines.back()).t;
        int  minCsq = 999; bool sawZeroRx = false;
        int  minRsrp = 9999;                              // 窗口内最低 RSRP(dBm,越低越差)
        const MetricRow* mZero = nullptr; const MetricRow* mWeak = nullptr;
        const MetricRow* mRsrp = nullptr; const MetricRow* mDeny = nullptr;
        auto mb = mets.begin(), me = mets.end();
        if (metsTimeSorted) {
            mb = std::lower_bound(mets.begin(), mets.end(), lo,
                                  [](const MetricRow& m, long long t) { return m.t < t; });
            me = std::upper_bound(mb, mets.end(), hi,
                                  [](long long t, const MetricRow& m) { return t < m.t; });
        }
        for (auto it = mb; it != me; ++it) {
            const auto& m = *it;
            if (!metsTimeSorted && (m.t < lo || m.t > hi)) continue;
            const bool lteReference = usesLteEngineeringReference(m.rat);
            if (lteReference && m.csqVal >= 0 && m.csqVal < minCsq) { minCsq = m.csqVal; mWeak = &m; }
            if (lteReference && m.rsrp < 0 && m.rsrp < minRsrp) { minRsrp = m.rsrp; mRsrp = &m; }
            if (m.drx == 0) { sawZeroRx = true; if (!mZero) mZero = &m; }
            if (!mDeny && m.srvVal >= 0 && m.srvVal != 2 && m.denyVal > 0) mDeny = &m;
        }
        // RSRP ≤ -110 dBm 是本项目的 LTE 弱覆盖工程观察线。它比 CSQ<10 更灵敏:
        // CSQ 是 0-31 粗档,可能读到中间值,而 RSRP 已探底 —— 覆盖问题此时才现形。
        bool weakByRsrp = (minRsrp <= -110);
        bool weakByCsq  = (minCsq < 10 && mWeak);
        const LogLine* sw = nullptr;
        const std::vector<const LogLine*>* switchEvents[] = { &evSlot, &evOper, &evCfun };
        const bool switchSorted[] = { slotTimeSorted, operTimeSorted, cfunTimeSorted };
        // 保持旧证据选择顺序:后面的类别覆盖前面(SLOT < OPER < CFUN)。
        for (size_t i = 0; i < 3; ++i) {
            const LogLine* candidate =
                firstLineInWindow(*switchEvents[i], lo, hi, switchSorted[i]);
            if (candidate) sw = candidate;
        }
        const LogLine* dn = firstLineInWindow(evRegistrationIssue, lo, hi,
                                              registrationIssueTimeSorted);
        const LogLine* nr = firstLineInWindow(evNotReady, lo, hi, notReadyTimeSorted);

        Cause c = C_UNKNOWN;
        const LogLine* evl = nullptr;
        const MetricRow* evm = nullptr;
        if (dn)                          { c = C_DENIED;    evl = dn; }
        else if (mDeny)                  { c = C_DENIED;    evm = mDeny; }
        /* 服务未就绪排在信号/假死之前:数据服务没起来时,CSQ 再好也拨不上,
         * 归因成"弱信号"会把排查引偏(见 ec200a 数据服务未就绪诊断的教训)。 */
        else if (nr)                     { c = C_NOTREADY;  evl = nr; }
        else if (sw)                     { c = C_SWITCHING; evl = sw; }
        else if (weakByCsq || weakByRsrp) {
            c = C_WEAK;
            evm = weakByCsq ? mWeak : mRsrp;   // CSQ 命中优先用 CSQ 证据,否则用 RSRP
        }
        else if (sawZeroRx && mZero)     { c = C_DATADEAD;  evm = mZero; }
        // L0 自愈 + 无上述特征 → SDK 短断网(链路抖动)。区别于"未能归类":
        // 未能归类是"查不出",这里是"查出来了——SDK 在 L0 就自愈,是网络侧瞬时抖动"。
        else if (o.l0Recovered)          { c = C_SDK_L0; }
        causeCnt[c]++;
        if (causeEv[c].size() < 3) {
            Evidence e;
            if (evl) e = mkEv(*evl);
            else if (evm) {
                e.lineNo = evm->lineNo; e.ts = fmtTime(evm->t, "MD");
                if (c == C_DENIED) {
                    e.text = "SDK注网摘要 SRV=" + std::to_string(evm->srvVal) +
                             " RAT=" + (evm->rat.empty() ? "-" : evm->rat) +
                             " DENY=" + std::to_string(evm->denyVal) +
                             " (断网 " + fmtTime(o.start, "MD") + " 起)";
                } else {
                    std::string sig = "心跳 CSQ=" +
                                      (evm->csqRaw >= 0 ? std::to_string(evm->csqRaw) : "-");
                    if (evm->rsrp < 0) sig += " RSRP=" + std::to_string(evm->rsrp) + "dBm";
                    if (evm->snr10 != 100000) sig += " SNR=" + fmtSnr10(evm->snr10) + "dB";
                    sig += " ΔRX=" +
                           (evm->drx != LLONG_MIN ? std::to_string(evm->drx) : "-");
                    e.text = sig + " (断网 " + fmtTime(o.start, "MD") + " 起)";
                }
            } else {
                e.lineNo = o.startLine; e.ts = fmtTime(o.start, "FULL");
                e.text = "断网起点(无弱信号/无 ΔRX=0/无切卡选网痕迹)";
            }
            causeEv[c].push_back(e);
        }
    }
    for (int c = 0; c < C_N; ++c) {
        if (!causeCnt[c] || causeEv[c].empty()) continue;
        Finding f;
        f.severity = (c == C_UNKNOWN) ? 0 : 1;
        f.title = "断网根因分类:" + std::string(kCauseName[c]) + " —— " +
                  std::to_string(causeCnt[c]) + " 次 / 共 " + std::to_string(outs.size()) + " 次";
        switch (c) {
        case C_WEAK:
            f.detail = "断网窗口内 LTE 心跳 CSQ 最小值 < 10(≈RSSI<-95dBm),或 RSRP ≤ -110dBm,"
                       "命中本项目的弱覆盖工程观察线；该线不是 3GPP 统一故障等级。";
            f.advice = "查天线连接/馈线/安装位置;确认是否处于覆盖边缘或屏蔽环境。重拨无法解决覆盖问题。";
            break;
        case C_DATADEAD:
            f.detail = "断网窗口内 RX_PKT 不增长(ΔRX=0)但链路仍在,典型的数据面假死:"
                       "控制面看似正常,业务无数据。";
            f.advice = "这类故障靠软重拨(L1)通常无效,需射频重置(L2 CFUN=0/1)或切通道;"
                       "确认恢复阶梯是否被门控(见本页其它结论)。";
            break;
        case C_SWITCHING:
            f.detail = "断网窗口内出现 [SLOT]/[OPER]/[CFUN] 动作,断网发生在切卡/选网/射频重置期间,"
                       "属该动作的预期代价(切卡实测 ~6-8s 数据中断)。";
            f.advice = "若次数不多可视为正常;频繁发生则查切卡触发条件是否过于敏感。";
            break;
        case C_DENIED:
            f.detail = "断网窗口内出现注册明确拒绝、受限服务、产品有边界的疑似账户诊断，"
                       "或 SDK 摘要为 SRV!=2 且 DENY>0。证据等级以上方对应独立结论为准。";
            f.advice = "按注册/SIM/账户方向处理，并保留原始 AT 证据；SUSPECTED 不能当成停机实锤。";
            break;
        case C_NOTREADY:
            f.detail = "断网窗口内出现 data_call_init 失败/重试 —— **AP 侧数据服务(ql_netd)没起来**,"
                       "不是射频或信号问题。此时 AT 命令照样能通(那走 ql_atc),但数据业务起不来,"
                       "表现为\"信号好好的却上不了网\"。";
            f.advice = "查 ql_netd 守护进程是否在跑(ps);这类故障靠重拨/CFUN/换卡都无效 —— "
                       "它们治的是射频侧,而问题在 AP 侧的数据服务。";
            break;
        case C_SDK_L0:
            f.detail = "断网短暂,SDK auto-reconnect 在 L0 阶段即自愈(未升级到 L1 软重拨/L2 射频重置),"
                       "且窗口内信号正常、无数据假死、无切卡。这类是网络侧瞬时抖动(基站释放/PDP "
                       "会话超时/覆盖切换等),设备本身无故障。";
            f.advice = "无需排查设备:设备已自愈。若频率高或影响业务,查网络侧 —— 运营商专网策略、"
                       "基站 inactivity timer、PDP/承载超时;可要求运营商侧排查周期性断连。";
            break;
        default:
            f.detail = "该次断网窗口内未见弱信号、ΔRX=0、切卡/选网或注册被拒的痕迹,"
                       "证据不足以归类(工具不臆测根因)。";
            f.advice = "结合“时间线”页人工查看该时段;必要时提高日志级别复现。";
            break;
        }
        f.ev = causeEv[c];
        fs.push_back(std::move(f));
    }

    // ---- 5. 恢复阶梯是否生效 ----
    // 阈值实证:EC200A L1>5min/L2>10min/L3>35min(ec200a/dial/dial.cpp 恢复段);
    //           EG25   L1=60s/L2=5min/L3=30min(eg25/dial/dial.c:979)。
    if (!evRecL3.empty()) {
        Finding f;
        f.severity = 2;
        f.title  = "L3 已触发:进程主动 exit,交由 watchdog/init 重启";
        f.detail = "断网时长达到 L3 阈值(EC200A 35min / EG25 30min),阶梯已走到尽头。"
                   "L3 只是纯进程退出,清不掉挂死的 CP 固件(设计权衡)。";
        f.advice = "若 L3 反复出现,说明重启无法解决,应查 SIM/账户/覆盖或模组固件。";
        for (size_t i = 0; i < evRecL3.size() && i < 3; ++i) f.ev.push_back(mkEv(*evRecL3[i]));
        fs.push_back(std::move(f));
    }
    if (!evRecL1.empty() || !evRecL2.empty()) {
        Finding f;
        f.severity = 1;
        f.title  = "恢复阶梯已生效:L1 触发 " + std::to_string(evRecL1.size()) +
                   " 次,L2 触发 " + std::to_string(evRecL2.size()) + " 次";
        f.detail = "阶梯按断网时长逐级升级(L1 软重拨 → L2 射频重置 → L3 退出)。";
        f.advice = "若 L1 频繁但每次都靠 L2 才恢复,说明软重拨无效,可考虑下调 L2 阈值。";
        for (size_t i = 0; i < evRecL1.size() && i < 2; ++i) f.ev.push_back(mkEv(*evRecL1[i]));
        for (size_t i = 0; i < evRecL2.size() && i < 2; ++i) f.ev.push_back(mkEv(*evRecL2[i]));
        fs.push_back(std::move(f));
    }
    // 有"够长"的断网却一条恢复日志都没有 → 找出被什么挡住了
    if (evRecL1.empty() && evRecL2.empty() && evRecL3.empty()) {
        long long thr = (pi.plat == PLAT_EG25) ? 60 : 5 * 60;
        const Outage* lng = nullptr;
        for (const auto& o : outs) if (o.recovered && o.dur >= thr) { lng = &o; break; }
        if (lng) {
            Finding f;
            f.severity = 1;
            f.title  = "恢复阶梯一次都没触发,但存在超过 L1 阈值的断网";
            f.detail = "有断网时长 ≥ L1 阈值(EG25 60s / EC200A 5min)却无任何 [RECOVERY] 日志。";
            if (pi.plat == PLAT_EG25 && !evPolicy.empty()) {
                // 实证:eg25/dial/dial.c:974-977 —— 阶梯要求同时满足
                // has_connected_once && 通道非 roamlink && policy==NET_POLICY_FORCE_SIM(4)
                f.detail += " EG25 的阶梯(eg25/dial/dial.c:974)要求同时满足三个条件:"
                            "已联网过、当前不在 Roamlink 通道、且策略为 4(FORCE_SIM)。"
                            "本日志的策略/通道不满足,阶梯是被结构性关闭的,不是坏了。";
                f.advice = "这是设计行为:Roamlink 通道有自己的失败切换逻辑,不走 L1/L2/L3。"
                           "若期望 SIM 通道的阶梯生效,需把 network_select 设为 4。";
                for (size_t i = 0; i < evPolicy.size() && i < 2; ++i) f.ev.push_back(mkEv(*evPolicy[i]));
            } else {
                f.detail += " 未能从日志证据判定被何条件门控。";
                f.advice = "确认 has_connected_once 是否为 0(从未联网),或平台策略是否禁用了阶梯。";
            }
            Evidence e; e.lineNo = lng->startLine; e.ts = fmtTime(lng->start, "FULL");
            e.text = "该次断网时长 " + fmtDur(lng->dur) + ",已超过 L1 阈值 " + fmtDur(thr);
            f.ev.push_back(e);
            fs.push_back(std::move(f));
        }
    }

    // ---- 6. 温度 ----
    // 阈值 75℃ 为经验值(非源码常量),故仅作提示,不作判定
    {
        const MetricRow* hot = nullptr; int best = -999;
        for (const auto& m : mets) {
            if (m.tempMax == INT_MIN) continue;
            int v = m.tempMax;
            if (v > best) { best = v; hot = &m; }
        }
        if (hot && best >= 75) {
            Finding f;
            f.severity = 1;
            f.title  = "模组/CPU 温度偏高:峰值 " + std::to_string(best) + "℃";
            f.detail = "高温会导致射频性能下降甚至模组保护性降频。(75℃ 为经验提示阈值,非源码常量)";
            f.advice = "查散热与安装环境;若高温与断网时间吻合,优先排散热。";
            Evidence e; e.lineNo = hot->lineNo; e.ts = fmtTime(hot->t, "MD");
            e.text = "心跳温度峰值 " + std::to_string(hot->tempMax) + "℃";
            f.ev.push_back(e);
            fs.push_back(std::move(f));
        }
    }

    // ---- 6b. 信号质量劣化(RSRP 长期偏低)----
    // RSRP 是比 CSQ 更精确的信号指标。若整体均值就偏低,说明设备长期处于覆盖边缘,
    // 不只是偶发弱信号 —— 这是安装位置/天线的系统性问题,值得单独提示。
    {
        long long sum = 0; int n = 0, worst = 9999; const MetricRow* mWorst = nullptr;
        for (const auto& m : mets) {
            if (usesLteEngineeringReference(m.rat) && m.rsrp < 0) {
                sum += m.rsrp; n++;
                if (m.rsrp < worst) { worst = m.rsrp; mWorst = &m; }
            }
        }
        if (n >= 5) {                              // 需足够样本才下结论,不臆测
            int avg = (int)(sum / n);
            if (avg <= -100 && mWorst) {           // 均值 ≤ -100 命中 LTE 工程参考较差档
                Finding f;
                f.severity = 1;
                f.title  = "信号质量长期偏低:RSRP 均值 " + std::to_string(avg) + " dBm(共 " +
                           std::to_string(n) + " 样本)";
                f.detail = "RSRP 均值命中 LTE 工程参考较差档(≤-100dBm),最低 " + std::to_string(worst) +
                           " dBm。该分档不是 3GPP 统一故障等级；持续弱覆盖可能与断网/低速相关。";
                f.advice = "系统性排查:天线选型/安装位置/朝向、是否室内深处或金属屏蔽;"
                           "必要时加装外置天线或选覆盖更好的运营商。";
                Evidence e; e.lineNo = mWorst->lineNo; e.ts = fmtTime(mWorst->t, "MD");
                e.text = "最低 RSRP=" + std::to_string(worst) + "dBm (均值 " + std::to_string(avg) + "dBm)";
                f.ev.push_back(e);
                fs.push_back(std::move(f));
            }
        }
    }

    // ---- 6c. LTE SNR 非正值提示 ----
    // 【源码直证】两套 SDK 真头均定义 SNR 为 0.1dB 有符号整数。
    // 【推断】SNR<=0dB 表示期望信号功率不高于噪声+干扰；“至少5样本且半数命中”是
    // 工程提示门槛,不是产品源码/SDK 常量。无新版真机日志前不把它单独归为断网根因。
    {
        long long sum = 0; int n = 0, nonPositive = 0, worst = 100000;
        const MetricRow* mWorst = nullptr;
        for (const auto& m : mets) {
            if (!usesLteEngineeringReference(m.rat) || m.snr10 == 100000) continue;
            sum += m.snr10; n++;
            if (m.snr10 <= 0) nonPositive++;
            if (m.snr10 < worst) { worst = m.snr10; mWorst = &m; }
        }
        if (n >= 5 && nonPositive * 2 >= n && mWorst) {
            int avg10 = (int)(sum / n);
            Finding f;
            f.severity = 0;
            f.title = "LTE SNR偏低提示:非正值 " + std::to_string(nonPositive) + "/" +
                      std::to_string(n) + " 样本,均值 " + fmtSnr10(avg10) + " dB";
            f.detail = "【推断】至少5个有效样本且半数以上 SNR≤0dB。该门槛用于提示噪声/同频干扰,"
                       "不是 SDK 或产品源码故障阈值；尚无新版真机日志验证,不单独据此归因断网。";
            f.advice = "结合 RSRP/RSRQ、断网时段和安装环境复核；若 RSRP尚可但SNR持续非正,"
                       "重点排查同频干扰、天线位置及馈线。";
            Evidence e; e.lineNo = mWorst->lineNo; e.ts = fmtTime(mWorst->t, "MD");
            e.text = "最低 SNR=" + fmtSnr10(worst) + "dB (原值 " + std::to_string(worst) +
                     ",均值 " + fmtSnr10(avg10) + "dB)";
            f.ev.push_back(std::move(e));
            fs.push_back(std::move(f));
        }
    }

    // ---- 6d. 小区质量、频繁切换及乒乓观察 ----
    // 这些规则用于把“现象”和原始证据连起来，不把无线侧相关性冒充确定根因。
    {
        CellAnalysis computedCells;
        if (!precomputedCells) computedCells = analyzeCellsImpl(lines, mets, outs);
        const CellAnalysis& cells = precomputedCells ? *precomputedCells : computedCells;
        const long long span = cells.last > cells.first ? cells.last - cells.first : 0;
        const long long switchRate10 = span > 0
            ? static_cast<long long>(cells.switchCount) * 36000 / span : 0;
        if (cells.switchCount >= 6 && switchRate10 >= 60 && !cells.transitions.empty()) {
            const CellTransition& top = cells.transitions.front();
            Finding f;
            f.severity = 1;
            f.title = "频繁小区切换:" + std::to_string(cells.switchCount) + " 次,约 " +
                      fmtSnr10(static_cast<int>(switchRate10)) + " 次/小时";
            f.detail = "【工程观察】已排除跨日志来源的伪切换；短时高频切换可能放大链路抖动，"
                       "但不能单凭该统计认定为断网根因。最常见方向 " + top.fromCell + " → " +
                       top.toCell + " 共 " + std::to_string(top.count) + " 次。";
            f.advice = "结合小区分析页的 RSRP/RSRQ/SNR、断网关联数和切换方向，复核覆盖边缘、"
                       "天线位置及运营商邻区配置。";
            Evidence e;
            e.lineNo = top.firstEvidenceLine;
            e.ts = fmtTime(top.first, "MD");
            e.text = "观察到小区切换 " + top.fromCell + " → " + top.toCell;
            f.ev.push_back(std::move(e));
            fs.push_back(std::move(f));
        }
        if (cells.pingPongCount >= 2) {
            const CellTransition* top = nullptr;
            for (const CellTransition& transition : cells.transitions)
                if (transition.pingPongCount && (!top || transition.pingPongCount > top->pingPongCount))
                    top = &transition;
            if (top) {
                Finding f;
                f.severity = 1;
                f.title = "疑似小区乒乓:" + std::to_string(cells.pingPongCount) + " 次";
                f.detail = "【工程观察】A→B 后 5 分钟内又 B→A 记为一次乒乓。高频往返通常值得"
                           "检查覆盖重叠区，但该时间门槛不是模组 SDK 的故障常量。";
                f.advice = "对照断网时间与两侧小区信号质量；若集中发生在固定位置，优先复核"
                           "天线、遮挡和邻区切换参数。";
                Evidence e;
                e.lineNo = top->pingPongEvidenceLine;
                e.ts = fmtTime(top->pingPongEvidenceTime, "MD");
                e.text = "5 分钟内往返 " + top->fromCell + " → " + top->toCell +
                         ",该方向累计 " + std::to_string(top->pingPongCount) + " 次";
                f.ev.push_back(std::move(e));
                fs.push_back(std::move(f));
            }
        }

        std::vector<const CellSummary*> weakCells;
        for (const CellSummary& cell : cells.cells) {
            const bool weakRsrp = cell.rsrpSamples >= 5 && cell.rsrpAvg10 <= -1100;
            const bool poorSnr = cell.snrSamples >= 5 && cell.snrAvg10 <= 0;
            if (cell.samples >= 5 && cell.outageStarts > 0 && (weakRsrp || poorSnr))
                weakCells.push_back(&cell);
        }
        if (!weakCells.empty()) {
            Finding f;
            f.severity = 1;
            f.title = "疑似弱覆盖小区:" + std::to_string(weakCells.size()) + " 个与断网起点相关";
            f.detail = "【相关性提示】这些小区同时满足：至少 5 个样本、RSRP均值≤-110dBm或"
                       "SNR均值≤0dB，并在断网前 10 分钟内被观测到。关联不等同因果。";
            f.advice = "在小区分析页按断网关联数排序，优先复核对应 Cell ID 的安装位置、天线"
                       "及覆盖；再结合原始证据确认。";
            for (std::size_t i = 0; i < weakCells.size() && i < 3; ++i) {
                const CellSummary& cell = *weakCells[i];
                Evidence e;
                e.lineNo = cell.firstOutageLine ? cell.firstOutageLine : cell.firstEvidenceLine;
                e.ts = fmtTime(cell.firstOutageTime ? cell.firstOutageTime : cell.first, "MD");
                e.text = "Cell " + cell.cellId + ":断网关联 " + std::to_string(cell.outageStarts) +
                         " 次,RSRP均值 " + (cell.rsrpSamples ? fmtSnr10(cell.rsrpAvg10) : "-") +
                         "dBm,SNR均值 " + (cell.snrSamples ? fmtSnr10(cell.snrAvg10) : "-") + "dB";
                f.ev.push_back(std::move(e));
            }
            fs.push_back(std::move(f));
        }
    }

    // ---- 7. 解析覆盖率(未识别行占比过高 → 结论本身可能不完整)----
    if (audit.unparsed > 0 && audit.unparsedRatio() > 0.01 && !audit.samples.empty()) {
        Finding f;
        f.severity = 1;
        f.title  = "有 " + std::to_string(audit.unparsed) + " 行未被解析(占 " +
                   std::to_string((int)(audit.unparsedRatio() * 100 + 0.5)) + "%),以上结论可能不完整";
        f.detail = "解析器跳过了这些行,它们不参与任何统计与结论。占比越高,结论的覆盖面越窄。";
        f.advice = "到“未识别行”页查看样例;若是新格式或新标签,需要扩展解析器。";
        for (size_t i = 0; i < audit.samples.size() && i < 3; ++i) {
            Evidence e; e.lineNo = audit.samples[i].lineNo; e.ts = "-";
            e.text = audit.samples[i].text.substr(0, 120);
            f.ev.push_back(e);
        }
        fs.push_back(std::move(f));
    }

    // 严重度降序(稳定排序,保留同级内的生成顺序)
    std::stable_sort(fs.begin(), fs.end(),
                     [](const Finding& a, const Finding& b) { return a.severity > b.severity; });
    return fs;
}

std::vector<Finding> analyze(const std::vector<LogLine>& lines,
                             const std::vector<Outage>& outs,
                             const std::vector<MetricRow>& mets,
                             const PlatformInfo& pi,
                             const ParseAudit& audit,
                             const CellAnalysis* precomputedCells) {
    return analyzeImpl(lines, outs, mets, pi, audit, precomputedCells);
}

std::vector<Finding> analyze(const LogView& lines,
                             const std::vector<Outage>& outs,
                             const std::vector<MetricRow>& mets,
                             const PlatformInfo& pi,
                             const ParseAudit& audit,
                             const CellAnalysis* precomputedCells) {
    return analyzeImpl(lines, outs, mets, pi, audit, precomputedCells);
}


} // namespace dl
