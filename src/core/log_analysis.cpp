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
#include <set>
#include <string_view>
#include <utility>

namespace dl {

static constexpr long long kSaneClockEpoch = 946598400LL;

static std::string fmtDuration(long long seconds) {
    if (seconds < 0) return "-";
    const long long hours = seconds / 3600;
    const long long minutes = (seconds % 3600) / 60;
    const long long remain = seconds % 60;
    if (hours > 0) return std::to_string(hours) + "h" + std::to_string(minutes) + "m" + std::to_string(remain) + "s";
    if (minutes > 0) return std::to_string(minutes) + "m" + std::to_string(remain) + "s";
    return std::to_string(remain) + "s";
}

static bool crossesClockBase(long long first, long long second) {
    return (first < kSaneClockEpoch) != (second < kSaneClockEpoch);
}

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

// IMX6ULL 的在线状态来自 30s/300s 快照，而不是旧产品的 fault timer 文案。
// 只接受完整的 online=0/1，避免正文中的普通数字或诊断说明误触发断网边沿。
static bool imxHeartbeatOnline(const LogLine& line, bool& online) {
    const std::string& tag = line.tagText();
    if (tag != "HB30" && tag != "HB300") return false;
    bool found = false;
    scanHbFields(line.msg, [&](std::string_view key, std::string_view value) {
        if (found || key != "online" || value.size() != 1) return;
        if (value[0] == '0') { online = false; found = true; }
        else if (value[0] == '1') { online = true; found = true; }
    });
    return found;
}

// 1.25.1 的 [RECOVERY] 也会记录进入/退避/升级/软重建等进行中的动作。
// 只有原先 CHECK_CONNECTION 成功路径输出的 downtime_s 才能关闭 online=0
// 断网，不能把首次 "action=enter" 误当成恢复完成。
static bool imxRecoveryComplete(const LogLine& line) {
    if (line.tagText() != "RECOVERY") return false;
    bool complete = false;
    scanHbFields(line.msg, [&](std::string_view key, std::string_view value) {
        if (key != "downtime_s") return;
        long long seconds = 0;
        const char* first = value.data();
        const char* last = first + value.size();
        const auto parsed = std::from_chars(first, last, seconds);
        complete = parsed.ec == std::errc{} && parsed.ptr == last && seconds >= 0;
    });
    return complete;
}

static bool isImxHeartbeatDiagnostic(const LogLine& line) {
    const std::string& tag = line.tagText();
    if (tag != "HB" && tag != "HB300") return false;
    return icontains(line.msg, "snapshot stale") || icontains(line.msg, "snapshot delayed") ||
           icontains(line.msg, "diagnostic snapshot pending");
}

std::map<std::string, std::string> hbFields(const std::string& msg) {
    std::map<std::string, std::string> d;
    scanHbFields(msg, [&](std::string_view key, std::string_view value) {
        d[std::string(key)] = std::string(value);
    });
    return d;
}

static std::string dataCallField(const std::string& msg, std::string_view wanted) {
    std::string value;
    scanHbFields(msg, [&](std::string_view key, std::string_view fieldValue) {
        if (key == wanted && value.empty()) value.assign(fieldValue.data(), fieldValue.size());
    });
    return value;
}

static bool isUnsolicitedDataCallDisconnect(const LogLine& line) {
    if (line.msg.find("DataCall disconnected") == std::string::npos) return false;
    return dataCallField(line.msg, "initiator") == "SDK_URC" &&
           dataCallField(line.msg, "reason") == "UNSOLICITED";
}

// ============================ 平台识别 ============================
// modem_mng_v2 不再使用旧 modem_mng 的 HEARTBEAT/恢复阶梯语义。下列
// 特征均是 v2 源码中的固定原文；既支持 parser 识别出的应用标签，也支持
// 裁剪后只剩正文的控制台日志。
static bool isModemMngV2Envelope(const LogLine& line) {
    if (line.tagText() == "MODEM_MNG_V2") return true;
    const std::string& msg = line.msg;
    return msg.find("===== modem_mng_v2 start =====") != std::string::npos ||
           (msg.find("status update: phase=") != std::string::npos &&
            msg.find(" tty=") != std::string::npos && msg.find(" online=") != std::string::npos &&
            msg.find(" csq=") != std::string::npos && msg.find(" reg=") != std::string::npos) ||
           (msg.find("phase: ") != std::string::npos && msg.find(" -> ") != std::string::npos &&
            msg.find(" tty=") != std::string::npos && msg.find(" module=") != std::string::npos) ||
           (msg.find("Found AT port: ") != std::string::npos &&
            msg.find("(module=") != std::string::npos) ||
           (msg.find("Module detected: ") != std::string::npos &&
            msg.find("(type=") != std::string::npos) ||
           msg.find("===== modem READY =====") != std::string::npos;
}

static const char* modemMngV2Module(const LogLine& line) {
    const std::string& msg = line.msg;
    const bool carriesModule =
        msg.find("Module detected: ") != std::string::npos ||
        (msg.find("Found AT port: ") != std::string::npos && msg.find("(module=") != std::string::npos) ||
        msg.find("===== modem READY =====") != std::string::npos ||
        (msg.find("phase: ") != std::string::npos && msg.find(" module=") != std::string::npos);
    if (!carriesModule) return nullptr;
    if (msg.find("EC200A") != std::string::npos) return "EC200A";
    if (msg.find("EG25") != std::string::npos) return "EG25";
    return nullptr;
}

// 只将状态转换和故障动作纳入时间线。READY 的周期 CSQ/CEREG 采样进
// 指标/结论引擎，不在这里全量保留，避免长日志的时间线被轮询噪声淹没。
static bool isModemMngV2Event(const LogLine& line) {
    const std::string& msg = line.msg;
    return msg.find("===== modem_mng_v2 start =====") != std::string::npos ||
           msg.find("status update: phase=") != std::string::npos ||
           (msg.find("phase: ") != std::string::npos && msg.find(" -> ") != std::string::npos &&
            msg.find(" tty=") != std::string::npos) ||
           msg.find("Module detected: ") != std::string::npos ||
           msg.find("Found AT port: ") != std::string::npos ||
           msg.find("SIM not inserted (CME ") != std::string::npos ||
           msg.find("sim ok") != std::string::npos ||
           msg.find("===== modem READY =====") != std::string::npos ||
           msg.find("already online (ping OK), skip dial -> READY monitor") != std::string::npos ||
           msg.find("WAN ping OK -> network_online=1") != std::string::npos ||
           msg.find("WAN ping fail -> network_online=0") != std::string::npos ||
           (msg.find("ping failed ") != std::string::npos &&
            msg.find(" times, reinit modem!") != std::string::npos) ||
           msg.find("No registered so long, reset modem") != std::string::npos ||
           msg.find("modem err in reading, reset modem") != std::string::npos ||
           msg.find("soft-reset module AT+CFUN=1,1") != std::string::npos ||
           msg.find("failed to in ready state") != std::string::npos;
}

static bool isModemMngV2Failure(const LogLine& line) {
    const std::string& msg = line.msg;
    const bool structuredFailure = line.tagText() == "MODEM_MNG_V2" &&
        (line.level == LEVEL_ERROR || line.level == LEVEL_WARNING ||
         line.level == LEVEL_FATAL || line.level == LEVEL_CRITICAL);
    return structuredFailure ||
           msg.find("modem init failed, phase ") != std::string::npos ||
           msg.find("CSQ query failed (sim_present=") != std::string::npos ||
           msg.find("ping failed (") != std::string::npos ||
           (msg.find("ping failed ") != std::string::npos &&
            msg.find(" times, reinit modem!") != std::string::npos) ||
           msg.find("No registered so long, reset modem") != std::string::npos ||
           msg.find("modem err in reading, reset modem") != std::string::npos ||
           msg.find("failed to registered. Reset modem") != std::string::npos ||
           msg.find("failed to detect SIM card. Reset modem") != std::string::npos ||
           msg.find("usbnet still mode=") != std::string::npos ||
           msg.find("CHECK_USBNET retries exhausted") != std::string::npos;
}

// 正式 parser 为保留 syslog 应用身份会把 v2 正文开头的 [where] 留在 msg；
// 核心单元测试/旧调用者也可能直接提供已经拆掉 where 的正文。两种表示都
// 只接受完整固定文本，避免普通的 "ping fail" 消息误触发断网统计。
static bool v2BodyEquals(const std::string& message, const char* expected) {
    if (message == expected) return true;
    if (message.empty() || message.front() != '[') return false;
    const size_t close = message.find("] ");
    return close != std::string::npos && message.compare(close + 2, std::string::npos, expected) == 0;
}

static bool isModemMngV2WanDown(const LogLine& line) {
    return line.tagText() == "MODEM_MNG_V2" &&
           v2BodyEquals(line.msg, "WAN ping fail -> network_online=0");
}

static bool isModemMngV2WanUp(const LogLine& line) {
    return line.tagText() == "MODEM_MNG_V2" &&
           v2BodyEquals(line.msg, "WAN ping OK -> network_online=1");
}

static bool isModemMngV2Start(const LogLine& line) {
    return line.tagText() == "MODEM_MNG_V2" &&
           v2BodyEquals(line.msg, "===== modem_mng_v2 start =====");
}

// 全部基于源码实证的判别特征:
//  artery : 行格式为 seas_log(FMT_SEAS)                        seas_log.c:210
//  AG35   : 心跳含 "SLOT:" 或出现 [SLOT] 标签
//           (ec200a/dial/dial.cpp:1071-1075 #ifdef QL_MODULE_PLATFORM_AG35;
//            ec200a/slot/slot_mgr.c 整文件 AG35-only)
//  EC200A : 心跳含 SIM_AT:/SIM_CB: 且无 SLOT:                   ec200a/dial/dial.cpp:1077
//           (注:open_dial(B) dial.c:518 心跳与此**完全相同**,故 B 的日志
//            也会判为 EC200A —— 二者行格式与心跳字段无差异,不做无据区分)
//  EG25   : 心跳含 "CH:"/RL_FAIL/RX_PKT 或出现 [ROAMLINK] 标签  eg25/diag/diag.c:109-131
//  IMX/RK3506J: RTMS 最新版在启动时发布带平台名的展示版本；当前不假设其存在
//               旧版 HEARTBEAT 字段，只根据该直接证据标识平台。
struct DisplayVersionPlatform {
    const char* marker;
    Platform platform;
    const char* name;
};

static const DisplayVersionPlatform* displayVersionPlatform(const std::string& message) {
    static constexpr DisplayVersionPlatform kPlatforms[] = {
        {"dial version: dial_eg25_",         PLAT_ARTERY,    "artery (open_dial_for_artery, EG25)"},
        {"dial version: dial_ec200a_",       PLAT_EC200A,    "EC200A (open_dial)"},
        {"modem_mng version: rtms_ag35_",    PLAT_AG35,      "AG35 (modem_mng, rtms)"},
        {"modem_mng version: rtms_ec200a_",  PLAT_EC200A,    "EC200A (modem_mng, rtms)"},
        {"modem_mng version: rtms_eg25_",    PLAT_EG25,      "EG25 (modem_mng, rtms)"},
        {"modem_mng version: rtms_imx6ull_", PLAT_IMX,       "IMX6ULL (modem_mng, rtms)"},
        {"modem_mng version: rtms_rk3506j_", PLAT_RK3506J,   "RK3506J (modem_mng, rtms)"},
    };
    const std::string normalized = lower(message);
    for (const auto& platform : kPlatforms)
        if (normalized.find(platform.marker) != std::string::npos) return &platform;
    return nullptr;
}

PlatformInfo detectPlatform(const std::vector<LogLine>& lines) {
    PlatformInfo pi;
    size_t seas = 0;
    const LogLine* seasEv = nullptr;
    const LogLine* ag35Ev = nullptr;
    const LogLine* ec200Ev = nullptr;
    const LogLine* eg25Ev = nullptr;
    const LogLine* v2Ev = nullptr;
    const LogLine* v2ModuleEv = nullptr;
    const char* v2Module = nullptr;
    const LogLine* displayVersionEv = nullptr;
    const DisplayVersionPlatform* displayVersion = nullptr;

    for (const auto& l : lines) {
        if (!v2Ev && isModemMngV2Envelope(l)) v2Ev = &l;
        if (const char* module = modemMngV2Module(l)) {
            // 明确的模组检测行比启动时的 UNKNOWN 提示更有证据力。
            if (!v2ModuleEv || l.msg.find("Module detected: ") != std::string::npos) {
                v2ModuleEv = &l;
                v2Module = module;
            }
        }
        if (!displayVersion) {
            if (const DisplayVersionPlatform* p = displayVersionPlatform(l.msg)) {
                displayVersion = p;
                displayVersionEv = &l;
            }
        }
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

    if (v2Ev || v2ModuleEv) {
        const LogLine* evidence = v2ModuleEv ? v2ModuleEv : v2Ev;
        pi.plat = PLAT_MODEM_MNG_V2;
        pi.name = std::string("modem_mng_v2") +
                  (v2Module ? " (" + std::string(v2Module) + ")" : " (模组待识别)");
        pi.evidenceLine = evidence ? evidence->lineNo : 0;
        pi.evidence = std::string(v2Module ? "v2 原文明确给出模组:  " :
                                             "出现 modem_mng_v2 固定启动/状态机特征:  ") +
                      (evidence ? evidence->msg.substr(0, 90) : "");
    }
    else if (displayVersion) {
        pi.plat = displayVersion->platform;
        pi.name = displayVersion->name;
        pi.evidenceLine = displayVersionEv ? displayVersionEv->lineNo : 0;
        pi.evidence = std::string("新版展示版本标识:  ") +
                      (displayVersionEv ? displayVersionEv->msg.substr(0, 90) : "");
    }
    else if (seas > 0) set(PLAT_ARTERY, "artery (open_dial_for_artery, seas_log)", seasEv, "行格式为 seas_log(时间带毫秒+级别+函数名)");
    else if (ag35Ev)   set(PLAT_AG35,   "AG35 (modem_mng, 双卡)",                  ag35Ev, "出现 AG35 专有的 SLOT 切卡痕迹");
    else if (eg25Ev)   set(PLAT_EG25,   "EG25 (modem_mng)",                        eg25Ev, "出现 EG25 专有的 ROAMLINK/CH 通道字段");
    else if (ec200Ev)  set(PLAT_EC200A, "EC200A (modem_mng 或 open_dial 上游)",    ec200Ev, "心跳为 SIM_AT/SIM_CB 格式且无 SLOT");
    else               set(PLAT_UNKNOWN, "未识别", nullptr, "无任何平台特征字段");
    return pi;
}

// ============================ 判定 ============================
bool isFaultStart(const std::string& msg) {
    return (icontains(msg, "Ping failed") && icontains(msg, "fault timer started")) ||
           icontains(msg, "Network outage started") ||
           // open_dial 1.28.13 的数据面监视器在 10 秒无下行时打印这一固定告警；
           // 它是已创建接口上的直接故障边沿，不把普通 RX_IDLE 心跳当作断网。
           (icontains(msg, "Interface has no RX data for") && icontains(msg, "IF="));
}

// open_dial 的联网通知没有自报 Down: Ns，不能复用 isRecovered()；它仍是
// 设备级事故的明确恢复边沿。只接纳固定通知文本，避免普通说明误触发。
static bool isNetworkConnectedNotification(const std::string& msg) {
    return icontains(msg, "Network Connected. Notification sent");
}

// [HEARTBEAT-NET] 是 EC200A/open_dial 的数据面状态快照。IF=(none) 表示
// 没有可用数据接口；IF=ccinetX 表示接口已创建。它不能与“已注册”混为一谈。
static bool netHeartbeatInterface(const LogLine& line, bool& interfaceUp) {
    if (line.tagText() != "HEARTBEAT-NET") return false;
    const auto fields = hbFields(line.msg);
    const auto it = fields.find("IF");
    if (it == fields.end() || it->second.empty()) return false;
    const std::string value = lower(trim(it->second));
    interfaceUp = value != "(none)" && value != "none" && value != "(unknown)" && value != "unknown";
    return true;
}

static bool netHeartbeatTxWithoutRx(const LogLine& line) {
    if (line.tagText() != "HEARTBEAT-NET") return false;
    const auto fields = hbFields(line.msg);
    const auto tx = fields.find("TX_PKT");
    const auto rxIdle = fields.find("RX_IDLE");
    if (tx == fields.end() || rxIdle == fields.end()) return false;
    // 30 秒普通心跳带有 RX_IDLE=0s；只有本窗口确有 TX 增量且连续 10 秒无 RX，
    // 才是与固件 IF_TRAFFIC_IDLE_WARN_MS 一致的数据面异常样本。
    const size_t plus = tx->second.find("(+");
    if (plus == std::string::npos) return false;
    long long txDelta = 0, idleSeconds = 0;
    const char* txFirst = tx->second.data() + plus + 2;
    const char* txLast = tx->second.data() + tx->second.size();
    const auto txParsed = std::from_chars(txFirst, txLast, txDelta);
    const char* idleFirst = rxIdle->second.data();
    const char* idleLast = idleFirst + rxIdle->second.size();
    const auto idleParsed = std::from_chars(idleFirst, idleLast, idleSeconds);
    return txParsed.ec == std::errc{} && txDelta > 0 &&
           idleParsed.ec == std::errc{} && idleSeconds >= 10;
}

static int loggedL3ThresholdSeconds(const std::string& message) {
    const std::string text = lower(message);
    const size_t marker = text.find("threshold ");
    if (marker == std::string::npos) return -1;
    size_t first = marker + std::strlen("threshold ");
    size_t last = first;
    while (last < text.size() && std::isdigit(static_cast<unsigned char>(text[last]))) ++last;
    if (last == first || last >= text.size() || text[last] != 's') return -1;
    const long long value = std::strtoll(text.substr(first, last - first).c_str(), nullptr, 10);
    return value > 0 && value <= INT_MAX ? static_cast<int>(value) : -1;
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

/* COPS mode is a one-digit 3GPP field.  Keep this deliberately narrow: a
 * command such as "AT+COPS=1" is not evidence that the modem is presently
 * manual; only a +COPS query response is. */
static bool hasCopsMode(const std::string& msg, char wanted) {
    const std::string lo = lower(msg);
    size_t p = lo.find("+cops:");
    while (p != std::string::npos) {
        p += 6;
        while (p < lo.size() && (lo[p] == ' ' || lo[p] == '\t')) ++p;
        if (p < lo.size() && lo[p] == wanted &&
            (p + 1 == lo.size() || lo[p + 1] == ',' ||
             lo[p + 1] == ' ' || lo[p + 1] == '\t' ||
             lo[p + 1] == '\r' || lo[p + 1] == '\n'))
            return true;
        p = lo.find("+cops:", p);
    }
    return false;
}

/* The two EG25 products use different envelopes, but both log a successful
 * COPS=0 restore explicitly.  Do not treat a mere COPS=0 command as success. */
static bool isCopsAutoRestoreOk(const std::string& msg) {
    const std::string lo = lower(msg);
    if (lo.find("reg timeout: at+cops=0 unlock ok") != std::string::npos)
        return true;                              // artery (seas_log)
    return lo.find("at+cops=0 rsp:") != std::string::npos &&
           lo.find("ok") != std::string::npos;  // RTMS EG25 ([REG TIMEOUT])
}

/* Registration progress, not merely a COPS command.  These strings are the
 * successful state/connection records emitted by artery and RTMS EG25. */
static bool isRegistrationRecovered(const std::string& msg) {
    const std::string lo = lower(msg);
    return lo.find("state: reg_check -> cereg_check") != std::string::npos ||
           lo.find("reg_check -> cereg_check") != std::string::npos ||
           lo.find("net connected") != std::string::npos ||
           lo.find("datacall connected") != std::string::npos ||
           lo.find("network recovered after ") != std::string::npos ||
           (lo.find("network recovered") != std::string::npos &&
            lo.find("down:") != std::string::npos);
}

static bool isRegCheckEntered(const std::string& msg) {
    return lower(msg).find("sim_op -> reg_check") != std::string::npos;
}

/* This identifies an application-originated manual selection attempt, not an
 * external AT client.  Absence is deliberately reported as "source unknown". */
static bool isManualSelectionCommand(const std::string& msg) {
    const std::string lo = lower(msg);
    return lo.find("at+cops=1,2,") != std::string::npos ||
           lo.find("[oper] selected operator") != std::string::npos;
}

// open_dial 的 "Network Recovered ... Down: Ns" 是自包含事件：产品在恢复行中
// 同时给出停机时长，即使日志截段没有 fault start 也可统计。modem_mng 的
// "recovered after Ns" 不是该语义，仍必须与 fault start 配对。
static bool isSelfContainedRecovery(const std::string& msg) {
    const std::string lo = lower(msg);
    return lo.find("network recovered") != std::string::npos &&
           lo.find("down:") != std::string::npos;
}


// 时间线保留的“状态变化类”标签。取自三仓库 dial_log/SEAS_LOG 首参的穷举
// (modem_mng 357 处、open_dial 107 处、artery 4 处内嵌标签),多词标签按首段匹配:
//   "RECOVERY L1/L2/L3"→RECOVERY、"REG TIMEOUT"/"REG DIAG"→REG、
//   "PING OUT/FAIL/ERROR"→PING、"ZERO ADDR"→ZERO、"CELL CHANGE"→CELL(不在表内=噪声)
// 刻意排除:HEARTBEAT/CELL/CELL CHANGE(高频噪声,另有专门统计)、
//           LOGMIGR/LOGCLEAN/CLEANUP(日志自身维护,与网络无关)
static const char* kEventTags[] = {
    "STATE","SDK","ROAMLINK","SLOT","OPER","LED","CFUN","SIM","APN","INIT","MODEM",
    "RECOVERY","OUTAGE","ERROR","WARN","WARNING","FATAL","INFO","EVENT","STATUS","ALARM","TZ","NANOMSG","SYSTEM",
    "LOG_E","LOG_I","LOG_D",
    // 以下为本次按源码穷举补齐(此前被静默丢弃)
    "PING","REG","ZERO","CPDUMP","COPS","SM","DIAG",
    // IMX6ULL 状态机：HB30/HB300 是高频指标，不放时间线；其余均为阶段或故障动作。
    "FAILURE","RETRY","PDP","NET","DHCP","DEVICE","USB","POWER","EXIT","SERVICE","PLMN","AT","VERSION", nullptr
};
static const char* kErrTags[] = { "ERROR","WARN","WARNING","FATAL","ALARM","LOG_E","FAILURE", nullptr };

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
    if (isModemMngV2Failure(l)) return true;
    if (isUnsolicitedDataCallDisconnect(l)) return true;
    if (l.tagText() == "SYSTEM" &&
        l.msg.find("uptime read failed") != std::string::npos) return true;
    if (l.tagText() == "MODEM_MNG_V2" || l.fmt == FMT_CONSOLE) {
        return l.level == LEVEL_ERROR || l.level == LEVEL_WARNING ||
               l.level == LEVEL_FATAL || l.level == LEVEL_CRITICAL;
    }
    if (l.fmt == FMT_SEAS) {
        return l.level == LEVEL_ERROR || l.level == LEVEL_WARNING ||
               l.level == LEVEL_FATAL || l.level == LEVEL_CRITICAL;
    }
    return false;
}

// 时间线保留:事件类标签,或 seas 的报错行(artery 大量日志无内嵌标签,
// 只按标签过滤会让 artery 的时间线几乎全空)
bool isEventLine(const LogLine& l) {
    if (isModemMngV2Event(l) || isModemMngV2Failure(l)) return true;
    if (isImxHeartbeatDiagnostic(l)) return true;
    // RK3506J 1.28.1 只在运营商/PLMN 实际变化时输出这条 INIT 记录；它不是
    // 周期快照，保留到时间线可直接定位切网证据。
    if (l.tagText() == "INIT" && icontains(l.msg, "operator changed:")) return true;
    // [HEARTBEAT-NET] 是高频采样；只在接口消失或有发无收时进入时间线，
    // 既保留数据面故障证据，又不让正常心跳淹没事件。
    bool netInterfaceUp = false;
    if (netHeartbeatInterface(l, netInterfaceUp) && !netInterfaceUp) return true;
    if (netHeartbeatTxWithoutRx(l)) return true;
    if (isEventTag(l.tagText())) return true;
    if (isErrLine(l)) return true;
    if (l.fmt == FMT_SEAS) {
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
    std::uint16_t source = 0;
    bool legacySeenInterface = false;
    bool legacyInterfaceDown = false;
    struct V2Open {
        long long start = 0;
        size_t line = 0;
    };
    std::map<std::uint16_t, V2Open> v2Open;
    std::map<std::uint16_t, bool> v2SeenOnline;
    // IMX6ULL 1.25 使用 [HB30]/[HB300] 的 online 边沿。与 v2 一样，必须先
    // 观察到在线，才把后续 online=0 视为“掉线”，避免把启动拨号阶段误算断网。
    struct ImxOpen {
        long long start = 0;
        size_t line = 0;
    };
    std::map<std::uint16_t, ImxOpen> imxOpen;
    std::map<std::uint16_t, bool> imxSeenOnline;
    auto closeImxOutage = [&](std::uint16_t sourceId, const LogLine& end) {
        const auto down = imxOpen.find(sourceId);
        if (down == imxOpen.end()) return;
        const long long duration = end.t - down->second.start;
        if (end.t > 0 && duration >= 0 && duration <= INT_MAX &&
            !crossesClockBase(down->second.start, end.t)) {
            Outage outage;
            outage.start = down->second.start;
            outage.startLine = down->second.line;
            outage.end = end.t;
            outage.endLine = end.lineNo;
            outage.dur = static_cast<int>(duration);
            outage.recovered = true;
            outs.push_back(outage);
        }
        imxOpen.erase(down);
    };
    for (const auto& item : lines) {
        const LogLine& l = lineRef(item);
        const bool usableV2Clock = l.fmt == FMT_SYSLOG && l.t > 0;

        // v2 的 WAN 状态日志是状态边沿，而非旧产品的 fault timer/recovery
        // 自报时长。每个 source 必须先观察到一次 OK，避免把未插卡/启动拨号
        // 阶段的 fail 算作“从在线掉线”；之后保存第一个 down，重复 down 只是
        // 轮询，不能重置起点。只接受 FMT_SYSLOG：RFC3164 虽缺年份，月日时分秒
        // 仍可用于同文件差值；console 即使借到上一条 t 也不是该行采样时刻，必须排除。
        // 精确的 v2 启动横幅即使来自无时钟 stderr，也能证明旧进程状态已经失效。
        // 它只作为状态屏障，不为 outage 提供恢复时间或时长。
        if (isModemMngV2Start(l)) {
            auto stale = v2Open.find(l.sourceId);
            if (stale != v2Open.end()) {
                Outage outage;
                outage.start = stale->second.start;
                outage.startLine = stale->second.line;
                outage.recovered = false;
                outs.push_back(outage);
                v2Open.erase(stale);
            }
            v2SeenOnline[l.sourceId] = false;
        }
        if (usableV2Clock && isModemMngV2WanUp(l)) {
            auto down = v2Open.find(l.sourceId);
            if (down != v2Open.end()) {
                const long long duration = l.t - down->second.start;
                if (l.t > 0 && duration >= 0 && duration <= INT_MAX &&
                    !crossesClockBase(down->second.start, l.t)) {
                    Outage outage;
                    outage.start = down->second.start;
                    outage.startLine = down->second.line;
                    outage.end = l.t;
                    outage.endLine = l.lineNo;
                    outage.dur = static_cast<int>(duration);
                    outage.recovered = true;
                    outs.push_back(outage);
                }
                // 即使时钟无效，这条 up 也终止该状态周期；不能拿旧 down 与
                // 更晚的另一次 up 勉强配出一个看似合理的时长。
                v2Open.erase(down);
            }
            v2SeenOnline[l.sourceId] = true;
        } else if (usableV2Clock && isModemMngV2WanDown(l) && v2SeenOnline[l.sourceId]) {
            v2Open.emplace(l.sourceId, V2Open{l.t, l.lineNo});
        }

        // 新启动不能让上个 IMX 进程的 offline 状态跨会话延续。
        if (isProgramStartBanner(l.msg)) {
            const auto stale = imxOpen.find(l.sourceId);
            if (stale != imxOpen.end()) {
                Outage outage;
                outage.start = stale->second.start;
                outage.startLine = stale->second.line;
                outage.recovered = false;
                outs.push_back(outage);
                imxOpen.erase(stale);
            }
            imxSeenOnline[l.sourceId] = false;
        }
        bool imxOnline = false;
        if (imxHeartbeatOnline(l, imxOnline)) {
            if (imxOnline) {
                closeImxOutage(l.sourceId, l);
                imxSeenOnline[l.sourceId] = true;
            } else if (imxSeenOnline[l.sourceId]) {
                imxOpen.emplace(l.sourceId, ImxOpen{l.t, l.lineNo});
            }
        } else if (imxRecoveryComplete(l)) {
            // RECOVERY 在探测刚成功时即打印，比随后一个 30s 上报更接近实际恢复点。
            closeImxOutage(l.sourceId, l);
            imxSeenOnline[l.sourceId] = true;
        }

        // 日切文件、L3 exit 后重拉和快速失败重启会更换 sourceId，但设备级数据
        // 故障并不会因此结束。保留最早的未恢复起点，直到看到数据接口恢复或联网通知；
        // 过去在 sourceId 切换时强制关闭，导致长事故被拆丢。
        if (isFaultStart(l.msg)) {
            // 同一进程再次声明 fault start，保持旧行为：上一状态已被内部重置，
            // 新起点才能同本进程恢复行配对。跨文件/重拉则保留原始设备级起点。
            if (!have || l.sourceId == source) {
                have = true; start = l.t; startLine = l.lineNo; source = l.sourceId;
                legacyInterfaceDown = false;
            }
        }
        bool netInterfaceUp = false;
        const bool hasNetHeartbeat = netHeartbeatInterface(l, netInterfaceUp);
        if (hasNetHeartbeat && netInterfaceUp) legacySeenInterface = true;
        if (!have && hasNetHeartbeat && !netInterfaceUp) {
            // 只在此前已观察到接口在线时，才将 IF=(none) 作为事故起点；避免把
            // 单独截取的冷启动拨号阶段误判为“从在线掉线”。
            if (legacySeenInterface) {
                have = true; start = l.t; startLine = l.lineNo; source = l.sourceId;
                legacyInterfaceDown = true;
            }
        }
        if (have && hasNetHeartbeat && !netInterfaceUp) legacyInterfaceDown = true;
        int dur = 0;
        const bool reportedRecovery = isRecovered(l.msg, &dur);
        // 故障开始同一秒的末尾心跳仍可能带旧 IF=ccinetX；它只能说明接口当时
        // 还存在，不能关闭刚刚由“无 RX”打开的事故。只有本事故已明确见过
        // IF=(none) 后，后续 IF 回来才是接口恢复边沿。
        const bool interfaceRecovery = have && legacyInterfaceDown && hasNetHeartbeat && netInterfaceUp;
        const bool notificationRecovery = isNetworkConnectedNotification(l.msg);
        if (reportedRecovery || interfaceRecovery || notificationRecovery) {
            const bool selfContained = isSelfContainedRecovery(l.msg);
            if (!have && !selfContained) continue;
            const long long wallDuration = have ? l.t - start : dur;
            const long long duration = have && reportedRecovery && l.sourceId == source ? dur : wallDuration;
            const bool sameClockBase = !have || !crossesClockBase(start, l.t);
            const bool plausible = sameClockBase && duration >= 0 && duration <= INT_MAX &&
                                   (!reportedRecovery || dur >= 0);
            if (!plausible) { have = false; continue; }
            Outage o;
            o.start = have ? start : l.t - dur;
            o.startLine = have ? startLine : l.lineNo;
            o.end = l.t; o.dur = static_cast<int>(duration); o.recovered = true;
            o.endLine = l.lineNo;
            // SDK L0 自愈:恢复行含 "(L0)"(open_dial "Network Recovered in SDK phase (L0)")。
            // 这类是短断网、链路抖动,设备自愈,与走 L1+ 阶梯的深层恢复区分。
            o.l0Recovered = (l.msg.find("(L0)") != std::string::npos);
            outs.push_back(o);
            have = false;
            legacyInterfaceDown = false;
        }
    }
    if (have) {
        Outage o; o.start = start; o.startLine = startLine; o.recovered = false;
        outs.push_back(o);
    }
    for (const auto& entry : v2Open) {
        Outage outage;
        outage.start = entry.second.start;
        outage.startLine = entry.second.line;
        outage.recovered = false;
        outs.push_back(outage);
    }
    for (const auto& entry : imxOpen) {
        Outage outage;
        outage.start = entry.second.start;
        outage.startLine = entry.second.line;
        outage.recovered = false;
        outs.push_back(outage);
    }
    std::stable_sort(outs.begin(), outs.end(),
                     [](const Outage& a, const Outage& b) {
                         return a.startLine < b.startLine;
                     });
    return outs;
}

std::vector<Outage> collectOutages(const std::vector<LogLine>& lines) {
    return collectOutagesImpl(lines);
}

std::vector<Outage> collectOutages(const LogView& lines) {
    return collectOutagesImpl(lines);
}

template <typename Lines>
static DataCallStats collectDataCallStatsImpl(const Lines& lines) {
    DataCallStats stats;
    for (const auto& item : lines) {
        const LogLine& line = lineRef(item);
        if (line.msg.find("DataCall stop requested") != std::string::npos) {
            ++stats.stopRequested;
            continue;
        }
        if (line.msg.find("DataCall disconnected") == std::string::npos) continue;

        ++stats.disconnected;
        const std::string initiator = dataCallField(line.msg, "initiator");
        const std::string reason = dataCallField(line.msg, "reason");
        if (initiator.empty()) {
            ++stats.legacy;
        } else if (initiator == "APP_STOP") {
            ++stats.appStop;
        } else if (initiator == "SDK_URC") {
            ++stats.sdkUrc;
            if (reason == "UNSOLICITED") ++stats.unsolicited;
        } else {
            ++stats.otherInitiator;
        }
        if (!reason.empty()) ++stats.reasons[reason];
    }
    return stats;
}

DataCallStats collectDataCallStats(const std::vector<LogLine>& lines) {
    return collectDataCallStatsImpl(lines);
}

DataCallStats collectDataCallStats(const LogView& lines) {
    return collectDataCallStatsImpl(lines);
}

template <typename Lines>
static ObservationStats observationStatsImpl(const Lines& lines) {
    ObservationStats stats;
    if (lines.empty()) return stats;
    struct SourceWindow {
        bool have = false;
        long long first = 0;
        long long last = 0;
        long long previous = 0;
    };
    std::map<std::uint16_t, SourceWindow> windows;
    bool haveCalendar = false;
    long long calendarFirst = 0, calendarLast = 0;
    for (const auto& item : lines) {
        const LogLine& line = lineRef(item);
        if (!haveCalendar) {
            calendarFirst = calendarLast = line.t;
            haveCalendar = true;
        } else {
            calendarFirst = std::min(calendarFirst, line.t);
            calendarLast = std::max(calendarLast, line.t);
        }
        SourceWindow& window = windows[line.sourceId];
        if (!window.have) {
            window.have = true;
            window.first = window.last = window.previous = line.t;
        } else if (crossesClockBase(window.previous, line.t)) {
            stats.observedSpan += std::max(0LL, window.last - window.first);
            ++stats.clockDiscontinuities;
            window.first = window.last = window.previous = line.t;
        } else {
            window.first = std::min(window.first, line.t);
            window.last = std::max(window.last, line.t);
            window.previous = line.t;
        }
    }
    stats.calendarSpan = std::max(0LL, calendarLast - calendarFirst);
    stats.sourceCount = windows.size();
    for (const auto& item : windows)
        stats.observedSpan += std::max(0LL, item.second.last - item.second.first);
    if (stats.calendarSpan > 0)
        stats.coveragePercent = 100.0 * static_cast<double>(stats.observedSpan) /
                               static_cast<double>(stats.calendarSpan);
    return stats;
}

ObservationStats observationStats(const std::vector<LogLine>& lines) {
    return observationStatsImpl(lines);
}

ObservationStats observationStats(const LogView& lines) {
    return observationStatsImpl(lines);
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
    std::string_view tempImx, failTitle, failLower, failImx, rxUpper, rxLower;
    std::string_view rsrpUpper, rsrpLower, rsrqUpper, rsrqLower;
    std::string_view snrUpper, snrLower, rssiUpper, rssiLower;
    std::string_view srv, rat, deny, oper, cell, pci, tac, servingCell, trafficValid, sampleAge, sampleMs;
    std::string_view atTimeout, atProbe, detailedAtTimeout, detailedAtStage;
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
        else if (k == "temp_c")      f.tempImx = v;
        else if (k == "ConsecFail")  f.failTitle = v;
        else if (k == "tcp_fail")    f.failLower = v;
        else if (k == "fail_streak") f.failImx = v;
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
        else if (k == "serving_cell") f.servingCell = v;
        else if (k == "traffic_valid") f.trafficValid = v;
        else if (k == "sample_age_ms") f.sampleAge = v;
        // RK3506J RTMS 1.28.1 将详细快照的时效字段改为本轮采样耗时。遗漏它会
        // 令新版 HB300 的 rx_packets 被静默排除在流量分析之外。
        else if (k == "sample_ms") f.sampleMs = v;
        else if (k == "at_timeout") f.atTimeout = v;
        else if (k == "at_probe") f.atProbe = v;
        else if (k == "detailed_at_timeout") f.detailedAtTimeout = v;
        else if (k == "detailed_at_stage") f.detailedAtStage = v;
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

// IMX 日志把信号以人可读 dB/dBm 输出，例如 SNR:8.4dB；MetricRow 统一用 0.1dB。
// 只在 IMX 路径调用，避免改变旧 SDK 的整数原始值语义。
static bool parseImxSnr10(std::string_view text, int& value) {
    text = trimView(text);
    size_t pos = 0;
    bool negative = false;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        negative = text[pos] == '-';
        ++pos;
    }
    const size_t wholeBegin = pos;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos == wholeBegin) return false;
    long long whole = 0;
    if (!parseLong(text.substr(wholeBegin, pos - wholeBegin), whole, true)) return false;
    int tenth = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) return false;
        tenth = text[pos++] - '0';
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
    }
    if (text.substr(pos) != "dB") return false;
    const long long scaled = whole * 10 + tenth;
    const long long signedScaled = negative ? -scaled : scaled;
    if (signedScaled < -32768 || signedScaled > 32767) return false;
    value = static_cast<int>(signedScaled);
    return true;
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

static std::string_view modemMngV2Payload(const std::string& message) {
    std::string_view payload(message);
    if (!payload.empty() && payload.front() == '[') {
        const size_t close = payload.find("] ");
        if (close != std::string_view::npos) payload.remove_prefix(close + 2);
    }
    return payload;
}

static bool consumeV2Literal(std::string_view text, size_t& pos, std::string_view literal) {
    if (pos > text.size() || text.substr(pos, literal.size()) != literal) return false;
    pos += literal.size();
    return true;
}

static bool consumeV2Integer(std::string_view text, size_t& pos, long long& value) {
    const size_t first = pos;
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
    const size_t digits = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    return pos > digits && parseLong(text.substr(first, pos - first), value, true);
}

// log_info("[%s] CSQ: %d (sim_present=%d online=%d)", ...)。严格消费整行，
// 防止旧 HEARTBEAT 中同名 CSQ 字段或普通应用文本被重复生成指标。0..99
// 保留原始值供审计；AT+CSQ 只有 0..31 有效，99/32..98 都不得进入 csqVal。
static bool parseModemMngV2Csq(const std::string& message, int& csq) {
    const std::string_view text = modemMngV2Payload(message);
    size_t pos = 0;
    long long parsedCsq = 0, simPresent = 0, online = 0;
    if (!consumeV2Literal(text, pos, "CSQ: ") ||
        !consumeV2Integer(text, pos, parsedCsq) ||
        !consumeV2Literal(text, pos, " (sim_present=") ||
        !consumeV2Integer(text, pos, simPresent) ||
        !consumeV2Literal(text, pos, " online=") ||
        !consumeV2Integer(text, pos, online) ||
        !consumeV2Literal(text, pos, ")") || pos != text.size()) return false;
    if (parsedCsq < 0 || parsedCsq > 99 || (simPresent != 0 && simPresent != 1) ||
        (online != 0 && online != 1)) return false;
    csq = static_cast<int>(parsedCsq);
    return true;
}

struct ModemMngV2Registration {
    std::string cell;
    std::string rat;
    std::uint32_t tac = UINT32_MAX;
    std::uint8_t tacDigits = 0;
};

static const char* modemMngV2Rat(int act) {
    // Quectel +CEREG/+CGREG 的 AcT 枚举。只映射语义稳定的制式；未知扩展值
    // 留空，避免把厂商未来新增枚举套成错误的 LTE 工程阈值。
    switch (act) {
        case 0: return "GSM";
        case 1: return "GSM Compact";
        case 2: return "UMTS";
        case 3: return "EDGE";
        case 4: return "HSDPA";
        case 5: return "HSUPA";
        case 6: return "HSPA";
        case 7: return "LTE";
        case 8: return "EC-GSM-IoT";
        case 9: return "NB-IoT";
        default: return "";
    }
}

// log_info("CEREG stat=%d lac=%s ci=%s act=%d", ...) / CGREG 同形态。
// 只有 stat=1/5 的已注册结果才继承 Cell；未注册结果会清空旧状态。
static bool updateModemMngV2Registration(const std::string& message,
                                         ModemMngV2Registration& state) {
    const std::string_view text = modemMngV2Payload(message);
    const bool cereg = text.compare(0, 11, "CEREG stat=") == 0;
    const bool cgreg = text.compare(0, 11, "CGREG stat=") == 0;
    if (!cereg && !cgreg) return false;
    size_t pos = 11;
    long long stat = 0, act = 0;
    if (!consumeV2Integer(text, pos, stat) || !consumeV2Literal(text, pos, " lac=")) return false;
    const size_t lacFirst = pos;
    const size_t ciMarker = text.find(" ci=", lacFirst);
    if (ciMarker == std::string_view::npos) return false;
    const std::string_view lac = text.substr(lacFirst, ciMarker - lacFirst);
    pos = ciMarker;
    if (!consumeV2Literal(text, pos, " ci=")) return false;
    const size_t ciFirst = pos;
    const size_t actMarker = text.find(" act=", ciFirst);
    if (actMarker == std::string_view::npos) return false;
    const std::string_view ci = text.substr(ciFirst, actMarker - ciFirst);
    pos = actMarker;
    if (!consumeV2Literal(text, pos, " act=") ||
        !consumeV2Integer(text, pos, act) || pos != text.size() ||
        stat < 0 || stat > INT_MAX || act < -1 || act > INT_MAX) return false;

    state.cell.clear();
    state.rat.clear();
    state.tac = UINT32_MAX;
    state.tacDigits = 0;
    if (stat != 1 && stat != 5) return true;
    if (act < 0) return true;
    state.rat = modemMngV2Rat(static_cast<int>(act));
    if (state.rat.empty()) return true;
    if (usableCell(ci)) state.cell.assign(ci.data(), ci.size());
    // CEREG 的 lac 字段是 LTE/EPS TAC；CGREG 在非 LTE 制式下是 LAC，不能
    // 冒充 MetricRow::tac。只有已注册且 AcT 已知时才继承，避免错配制式。
    std::uint32_t tac = 0;
    if (cereg && parseUnsignedField(lac, 16, tac)) {
        state.tac = tac;
        std::string_view digits = trimView(lac);
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
            digits.remove_prefix(2);
        state.tacDigits = static_cast<std::uint8_t>(std::min<std::size_t>(digits.size(), 8));
    }
    return true;
}

struct CellState {
    std::string id;
    int pci = -1;
    std::uint32_t tac = UINT32_MAX;
    std::uint8_t tacDigits = 0;
    int rsrp = 1, rsrq = 1, rssi = 1, snr10 = 100000;
    long long signalT = 0;

    void clear() {
        id.clear(); pci = -1; tac = UINT32_MAX; tacDigits = 0;
        rsrp = rsrq = rssi = 1; snr10 = 100000; signalT = 0;
    }
    void setId(std::string_view value) {
        value = trimView(value);
        if (id.size() != value.size() || !std::equal(id.begin(), id.end(), value.begin())) {
            pci = -1; tac = UINT32_MAX; tacDigits = 0;
            rsrp = rsrq = rssi = 1; snr10 = 100000; signalT = 0;
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

// IMX [HB300] 的小区明细是 serving_cell="cell_id=...;pci=...;...;tac=..."。
// 它不是 AT +QENG 原文，故不能复用其字段下标；只读取明确命名的四个字段。
static void updateImxServingCell(std::string_view value, CellState& state) {
    value = unquote(value);
    std::string_view cell, pci, tac;
    size_t first = 0;
    while (first <= value.size()) {
        const size_t last = value.find(';', first);
        const std::string_view part = trimView(value.substr(first,
            (last == std::string_view::npos ? value.size() : last) - first));
        const size_t equal = part.find('=');
        if (equal != std::string_view::npos) {
            const std::string_view key = trimView(part.substr(0, equal));
            const std::string_view field = trimView(part.substr(equal + 1));
            if (key == "cell_id") cell = field;
            else if (key == "pci") pci = field;
            else if (key == "tac") tac = field;
        }
        if (last == std::string_view::npos) break;
        first = last + 1;
    }
    if (!cell.empty()) {
        if (usableCell(cell)) state.setId(cell);
        else state.clear();
    }
    std::uint32_t parsed = 0;
    if (!pci.empty())
        state.pci = parseUnsignedField(pci, 10, parsed) && parsed <= static_cast<std::uint32_t>(INT_MAX)
                        ? static_cast<int>(parsed) : -1;
    if (!tac.empty()) {
        state.tac = UINT32_MAX;
        state.tacDigits = 0;
        if (parseUnsignedField(tac, 16, parsed)) {
            state.tac = parsed;
            if (tac.size() > 2 && tac[0] == '0' && (tac[1] == 'x' || tac[1] == 'X')) tac.remove_prefix(2);
            state.tacDigits = static_cast<std::uint8_t>(std::min<std::size_t>(tac.size(), 8));
        }
    }
}

// Quectel +QENG servingcell 的 LTE/WCDMA/GSM/NR5G-SA 形态都把 Cell ID 放在
// 第 7 个 CSV 字段。LTE 的 PCI/TAC 分别位于第 8/13 个字段，NR5G-SA 的 TAC
// 位于第 9 个字段。这里只接纳明确的 servingcell 证据，不从邻区或数字位置猜测。
static bool qengCellState(const std::string& message, long long sampleTime, CellState& state) {
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
    // LTE QENG: fields[13..16] = RSRP/RSRQ/RSSI/SINR，SINR 原始单位为 dB；
    // MetricRow 统一保存 0.1dB，故乘 10。仅在字段完整且数值语义有效时继承。
    long long signal = 0;
    if (lte && count > 16) {
        if (parseLong(fields[13], signal, true) && signal < 0 && signal >= INT_MIN)
            state.rsrp = static_cast<int>(signal);
        if (parseLong(fields[14], signal, true) && signal < 0 && signal >= INT_MIN)
            state.rsrq = static_cast<int>(signal);
        if (parseLong(fields[15], signal, true) && signal < 0 && signal >= INT_MIN)
            state.rssi = static_cast<int>(signal);
        if (parseLong(fields[16], signal, true) && signal >= -3276 && signal <= 3276)
            state.snr10 = static_cast<int>(signal * 10);
        state.signalT = sampleTime;
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
    if (qengCellState(line.msg, line.t, state)) return;
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
    std::map<std::uint16_t, ModemMngV2Registration> v2Registration;
    std::map<std::uint16_t, Platform> platformBySource;

    for (const auto& item : lines) {
        const LogLine& l = lineRef(item);
        if (const DisplayVersionPlatform* display = displayVersionPlatform(l.msg))
            platformBySource[l.sourceId] = display->platform;
        // 版本横幅是进程启动的直接证据。即使日志文件把多次启动串在一起，
        // 新进程的网卡计数器、小区驻留状态也不能继承给上一会话；否则首个
        // HEARTBEAT 会凭旧 rx_packets 算出假的 ΔRX=0/负增量，进而误报数据停滞。
        if (isProgramStartBanner(l.msg)) {
            currentCell.clear();
            haveLastRx = false;
            lastRx = 0;
        }
        ModemMngV2Registration& registration = v2Registration[l.sourceId];
        if (l.msg.find("===== modem_mng_v2 start =====") != std::string::npos)
            registration = ModemMngV2Registration{};
        updateModemMngV2Registration(l.msg, registration);
        int v2Csq = -1;
        if (parseModemMngV2Csq(l.msg, v2Csq)) {
            MetricRow metric;
            metric.t = l.t;
            metric.lineNo = l.lineNo;
            metric.ch = "MODEM_V2";
            metric.rat = registration.rat;
            if (!registration.cell.empty()) metric.cellId.assign(registration.cell);
            if (registration.tac != UINT32_MAX) {
                metric.tac = registration.tac;
                metric.tacDigits = registration.tacDigits;
            }
            metric.csqRaw = v2Csq;
            if (v2Csq >= 0 && v2Csq <= 31) metric.csqVal = v2Csq;
            rows.push_back(std::move(metric));
            continue;
        }
        if (haveSource && l.sourceId != currentSource) {
            currentCell.clear();
            haveLastRx = false;
            lastRx = 0;
        }
        currentSource = l.sourceId;
        haveSource = true;
        updateCellState(l, currentCell);
        const bool imxHeartbeat = l.tagText() == "HB30" || l.tagText() == "HB300";
        if (l.tagText().compare(0, 9, "HEARTBEAT") != 0 && !imxHeartbeat) continue;
        HeartbeatFields f = heartbeatFields(l.msg);
        if (!f.any) continue;

        MetricRow m;
        m.t  = l.t;
        m.lineNo = l.lineNo;
        if (imxHeartbeat) {
            // HB30/HB300 的 state=CHECK_CONNECTION/SUCCESS 不是数据通道；它们
            // 必须优先显示所属 RTMS 平台，而不能被 artery 的 state→SIM 规则截获。
            m.ch = platformBySource[l.sourceId] == PLAT_RK3506J ? "RK3506J" : "IMX6ULL";
        } else if (!f.ch.empty()) assignView(m.ch, f.ch);
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

        if (imxHeartbeat && !f.servingCell.empty()) updateImxServingCell(f.servingCell, currentCell);
        if (!currentCell.id.empty()) m.cellId.assign(currentCell.id);
        m.pci = currentCell.pci;
        if (currentCell.tac != UINT32_MAX) {
            m.tac = currentCell.tac;
            m.tacDigits = currentCell.tacDigits;
        }

        std::string_view temp = !f.tempTitle.empty() ? f.tempTitle :
                                !f.tempUpper.empty() ? f.tempUpper : f.tempImx;
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
        const std::string_view fail = !f.failTitle.empty() ? f.failTitle :
                                      !f.failLower.empty() ? f.failLower : f.failImx;
        if (parseLong(fail, number) &&
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
        // HB300 的诊断快照可能是数分钟前的缓存。它仍可说明当时的信号/小区，
        // 但把陈旧 rx_packets 当“当前采样”会制造假的 ΔRX=0 数据停滞。仅接收
        // traffic_valid=1 且快照不超过一个 HB30 周期(45s)的 IMX 流量计数。
        bool imxFreshTraffic = true;
        if (imxHeartbeat) {
            long long sampleAge = 0;
            // IMX6ULL 使用 sample_age_ms；RK3506J 1.28.1 改为 sample_ms（本轮
            // 采样耗时）。旧 RK3506J 日志仍优先使用 sample_age_ms。
            const std::string_view freshness = !f.sampleAge.empty() ? f.sampleAge :
                (platformBySource[l.sourceId] == PLAT_RK3506J ? f.sampleMs : std::string_view{});
            imxFreshTraffic = parseLong(f.trafficValid, number, true) && number == 1 &&
                              parseLong(freshness, sampleAge, true) && sampleAge >= 0 && sampleAge <= 45000;
        }
        bool haveRx = imxFreshTraffic && parseLong(firstOf(f.rxUpper, f.rxLower), m.rx);
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
        if (imxHeartbeat) {
            if (parseLong(f.atTimeout, number, true) && (number == 0 || number == 1))
                m.atTelemetryTimeout = static_cast<int>(number);
            if (parseLong(f.detailedAtTimeout, number, true) && (number == 0 || number == 1))
                m.detailedAtTimeout = static_cast<int>(number);
            if (!f.detailedAtStage.empty()) m.detailedAtStage.assign(f.detailedAtStage);
            if (f.atProbe == "ok") m.atBasicProbe = 1;
            else if (f.atProbe == "fail") m.atBasicProbe = 0;
            else if (f.atProbe == "not_run") m.atBasicProbe = 2;
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
        if (imxHeartbeat) {
            int snr10 = 0;
            if (parseImxSnr10(firstOf(f.snrUpper, f.snrLower), snr10)) m.snr10 = snr10;
        } else if (parseLong(firstOf(f.snrUpper, f.snrLower), number, true) &&
                   number >= -32768 && number <= 32767) m.snr10 = (int)number;
        if (parseLong(firstOf(f.rssiUpper, f.rssiLower), number, true) &&
            number >= INT_MIN && number < 0)
            m.rssiVal = (int)number;
        // 1.31.15 等旧固件的普通心跳不带完整 LTE 四元组，但紧邻的 QENG 已提供；
        // 只有心跳字段缺失时才继承，绝不覆盖新版固件的明确值。
        const bool adjacentQeng = currentCell.signalT > 0 && m.t >= currentCell.signalT &&
                                  m.t - currentCell.signalT <= 2;
        if (adjacentQeng && m.rsrp >= 0 && currentCell.rsrp < 0) m.rsrp = currentCell.rsrp;
        if (adjacentQeng && m.rsrq >= 0 && currentCell.rsrq < 0) m.rsrq = currentCell.rsrq;
        if (adjacentQeng && m.rssiVal >= 0 && currentCell.rssi < 0) m.rssiVal = currentCell.rssi;
        if (adjacentQeng && m.snr10 == 100000 && currentCell.snr10 != 100000)
            m.snr10 = currentCell.snr10;
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

// “已注册”不足以代表业务可用；这里只接受产品明确记录的数据通道/WAN 已通，
// 或旧产品的 Network Recovered 事件。这个标志既用于全程服务可达率，也用于
// 将首次联网前的失败与运行期掉线分开。
static bool isDataPathUp(const LogLine& line) {
    if (isModemMngV2WanUp(line) || imxRecoveryComplete(line)) return true;
    bool imxOnline = false;
    if (imxHeartbeatOnline(line, imxOnline) && imxOnline) return true;
    int recoveredSeconds = 0;
    if (isRecovered(line.msg, &recoveredSeconds)) return true;
    const std::string lo = lower(line.msg);
    return lo.find("net connected") != std::string::npos ||
           lo.find("data call connected") != std::string::npos ||
           lo.find("datacall connected") != std::string::npos ||
           lo.find("wait_for_connect -> net_connected") != std::string::npos;
}

template <typename Lines>
static AvailabilityStats availabilityStatsImpl(const Lines& lines,
                                               const std::vector<Outage>& outages) {
    AvailabilityStats stats;
    struct Segment {
        std::uint16_t sourceId = 0;
        long long begin = 0;
        long long end = 0;
        long long firstUp = -1;
        bool startKnown = false;
    };
    std::vector<Segment> segments;
    std::map<std::uint16_t, std::size_t> active;

    for (const auto& item : lines) {
        const LogLine& line = lineRef(item);
        auto it = active.find(line.sourceId);
        // v2 的 syslog 启动记录没有传统 Version 文案，但 isModemMngV2Start()
        // 已严格限定应用身份和完整正文，是与版本横幅等价的进程边界证据。
        const bool banner = isProgramStartBanner(line.msg) || isModemMngV2Start(line);
        const bool clockSplit = it != active.end() &&
                                crossesClockBase(segments[it->second].end, line.t);
        if (it == active.end() || banner || clockSplit) {
            Segment segment;
            segment.sourceId = line.sourceId;
            segment.begin = segment.end = line.t;
            segment.startKnown = banner;
            segments.push_back(segment);
            active[line.sourceId] = segments.size() - 1;
            it = active.find(line.sourceId);
        }
        Segment& segment = segments[it->second];
        segment.end = std::max(segment.end, line.t);
        if (segment.firstUp < 0 && isDataPathUp(line)) segment.firstUp = line.t;
    }

    std::vector<long long> outageSeconds(segments.size(), 0);
    for (const Outage& outage : outages) {
        const std::uint16_t sourceId = sourceIdAtLine(lines, outage.startLine);
        bool countedTerminal = false;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const Segment& segment = segments[i];
            if (segment.sourceId != sourceId || segment.firstUp < 0) continue;
            const long long outageEnd = outage.recovered ? outage.end : segment.end;
            const long long begin = std::max(std::max(outage.start, segment.begin), segment.firstUp);
            const long long end = std::min(outageEnd, segment.end);
            if (end > begin) {
                outageSeconds[i] += end - begin;
                if (!outage.recovered) countedTerminal = true;
            }
        }
        if (countedTerminal) ++stats.terminalOutages;
    }

    for (std::size_t i = 0; i < segments.size(); ++i) {
        const Segment& segment = segments[i];
        if (!segment.startKnown || segment.end <= segment.begin) continue;
        const long long span = segment.end - segment.begin;
        ++stats.startupSegments;
        stats.fullObservedSeconds += span;
        if (segment.firstUp < 0) {
            ++stats.neverConnectedStartupSegments;
            stats.fullUnavailableSeconds += span;
            continue;
        }
        ++stats.connectedStartupSegments;
        const long long startup = std::max(0LL, segment.firstUp - segment.begin);
        stats.longestStartupSeconds = std::max(stats.longestStartupSeconds, startup);
        stats.runtimeObservedSeconds += std::max(0LL, segment.end - segment.firstUp);
        stats.runtimeUnavailableSeconds += std::min(outageSeconds[i],
                                                    std::max(0LL, segment.end - segment.firstUp));
        stats.fullUnavailableSeconds += std::min(span, startup + outageSeconds[i]);
    }
    stats.fullUnavailableSeconds = std::min(stats.fullUnavailableSeconds, stats.fullObservedSeconds);
    stats.runtimeUnavailableSeconds = std::min(stats.runtimeUnavailableSeconds,
                                               stats.runtimeObservedSeconds);
    return stats;
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

AvailabilityStats availabilityStats(const std::vector<LogLine>& lines,
                                    const std::vector<Outage>& outages) {
    return availabilityStatsImpl(lines, outages);
}

AvailabilityStats availabilityStats(const LogView& lines,
                                    const std::vector<Outage>& outages) {
    return availabilityStatsImpl(lines, outages);
}

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
    const bool v2Platform = pi.plat == PLAT_MODEM_MNG_V2;

    // ---- 预扫:各类特征行(全部留证据指针)----
    std::vector<const LogLine*> evNeverConn, evPolicy, evRecL1, evRecL2, evRecL3,
                                evDenied, evLimited, evSuspectedAccount, evRegQueryFail,
                                evRegistrationIssue, evHardRegistrationIssue, evCpdump, evSlot, evOper, evCfun,
                                evManualCops, evCopsAutoRestore, evRegistrationRecovered,
                                evManualSelectionCommand,
                                evNotReady, evOrphanRecovery, evImpossibleRecovery,
                                evDataCallInitFailed, evDataCallFatalExit, evArteryDataCallCleanup, evArteryDataCallExit,
                                evDataCallStartFailed, evDataCallAppStop,
                                evDataCallUnsolicited, evApnLoadFailed, evProgramStart,
                                evLicenseMissing, evLicensePending, evLicenseTimeout,
                                evLicenseBackupFailed, evLicenseAtomicallyBackedUp,
                                evV2NoSim, evV2LongUnregistered, evV2PingReinit,
                                evV2ReadyOffline, evV2ReadyFailed,
                                evImxFailure, evImxRetry, evImxSimNotReady, evImxRegWait,
                                evImxPdp, evImxDhcp, evImxNetwork, evImxDevice, evImxAt,
                                evImxAtTelemetryTimeout, evImxAtProbeFailed,
                                evImxRecoverPdp, evImxRecoverCfun, evImxRecoverHardware,
                                evImxConfigError,
                                evRkRetry, evRkSimNotReady, evRkRegistration, evRkPdp,
                                evRkNetwork, evRkDeviceAt, evRkPing;
    std::vector<const LogLine*> evNetInterfaceNone, evNetTxWithoutRx;
    std::map<std::string, size_t> appStopReasons, unsolicitedReasons;
    std::map<std::uint16_t, std::pair<long long, long long>> sourceBounds;
    std::map<std::uint16_t, bool> faultOpen;
    std::map<std::uint16_t, long long> faultStartTime;
    for (const auto& item : lines) {
        const LogLine& line = lineRef(item);
        auto inserted = sourceBounds.emplace(line.sourceId, std::make_pair(line.t, line.t));
        if (!inserted.second) {
            inserted.first->second.first = std::min(inserted.first->second.first, line.t);
            inserted.first->second.second = std::max(inserted.first->second.second, line.t);
        }
    }
    for (const auto& item : lines) {
        const LogLine& l = lineRef(item);
        if (isProgramStartBanner(l.msg)) evProgramStart.push_back(&l);
        // artery 的 license 流程使用固定的 SEAS_LOG 原文。只把“缺失 → 下载等待
        // → 超时降级”这一完整、明确的产品动作作为结论；单条 license missing
        // 可能随后从备份恢复，不能单独当作下载失败。
        if (icontains(l.msg, "LICENSE_MISSING")) evLicenseMissing.push_back(&l);
        if (icontains(l.msg, "SIM connected, starting RBMaster for license download"))
            evLicensePending.push_back(&l);
        if (icontains(l.msg, "license download timeout") &&
            icontains(l.msg, "FORCE_SIM mode until next reboot"))
            evLicenseTimeout.push_back(&l);
        // artery 与 EG25 均在备份失败时明确说明“延后重启”，而不是下载失败或
        // 已经降级。两种格式分别来自 SEAS_LOG 和 [ROAMLINK]，故只依赖这段
        // 产品共用的完整文案，避免把普通文件写入错误误判为 license 流程。
        if (icontains(l.msg, "license backup failed; reboot postponed"))
            evLicenseBackupFailed.push_back(&l);
        if (icontains(l.msg, "license atomically backed up to "))
            evLicenseAtomicallyBackedUp.push_back(&l);
        // artery 1.29.18 没有 SD 侧 SDK/FATAL 标签；只接受完整退出文案作为触发，
        // RBMaster 清理文案仅作为补充证据，避免把其他产品误归为进程级退出。
        if (l.fmt == FMT_SEAS &&
            l.msg.find("DataCall initialization failed; shutting down dial-owned RBMaster before exit(0)")
                != std::string::npos)
            evArteryDataCallCleanup.push_back(&l);
        if (l.fmt == FMT_SEAS &&
            l.msg.find("DataCall initialization failure: exiting dial with status 0 for supervisor restart")
                != std::string::npos)
            evArteryDataCallExit.push_back(&l);
        if (v2Platform) {
            if (l.msg.find("SIM not inserted (CME ") != std::string::npos)
                evV2NoSim.push_back(&l);
            if (l.msg.find("No registered so long, reset modem") != std::string::npos ||
                l.msg.find("failed to registered. Reset modem") != std::string::npos)
                evV2LongUnregistered.push_back(&l);
            if (l.msg.find("ping failed ") != std::string::npos &&
                l.msg.find(" times, reinit modem!") != std::string::npos)
                evV2PingReinit.push_back(&l);
            if (l.msg.find("===== modem READY =====") != std::string::npos &&
                l.msg.find(" online=0") != std::string::npos)
                evV2ReadyOffline.push_back(&l);
            if (l.msg.find("failed to in ready state") != std::string::npos)
                evV2ReadyFailed.push_back(&l);
        }
        // IMX6ULL 的拨号状态机把阶段失败和退避动作拆成固定标签；仅在已经由
        // rtms_imx6ull_ 横幅直接识别的平台上采集，避免把别的平台同名标签混入。
        if (pi.plat == PLAT_IMX) {
            const std::string& tag = l.tagText();
            if (tag == "FAILURE") evImxFailure.push_back(&l);
            if (tag == "RETRY") evImxRetry.push_back(&l);
            if (tag == "SIM" && icontains(l.msg, "SIM not ready"))
                evImxSimNotReady.push_back(&l);
            if (tag == "REG" && (icontains(l.msg, "wait timed out") ||
                                 icontains(l.msg, "response unparseable")))
                evImxRegWait.push_back(&l);
            if (tag == "PDP" && (icontains(l.msg, "rejected") ||
                                 icontains(l.msg, "wait exhausted")))
                evImxPdp.push_back(&l);
            if (tag == "DHCP" && (icontains(l.msg, "failed") ||
                                  icontains(l.msg, "ended unexpectedly") ||
                                  icontains(l.msg, "process remains")))
                evImxDhcp.push_back(&l);
            if (tag == "NET" && (icontains(l.msg, "no IPv4 address") ||
                                 icontains(l.msg, "connection failed") ||
                                 icontains(l.msg, "default route missing")))
                evImxNetwork.push_back(&l);
            if (tag == "DEVICE" && (icontains(l.msg, "initial discovery failed") ||
                                    icontains(l.msg, "USB enumeration timed out")))
                evImxDevice.push_back(&l);
            if (tag == "AT" && (icontains(l.msg, "open failed") ||
                                icontains(l.msg, "read EOF") ||
                                icontains(l.msg, "basic AT probe failed")))
                evImxAt.push_back(&l);
            if ((tag == "HB30" || tag == "HB300") &&
                dataCallField(l.msg, "at_timeout") == "1")
                evImxAtTelemetryTimeout.push_back(&l);
            if ((tag == "HB30" || tag == "HB300") &&
                dataCallField(l.msg, "at_probe") == "fail")
                evImxAtProbeFailed.push_back(&l);
            if (tag == "AT" && icontains(l.msg, "basic AT probe failed"))
                evImxAtProbeFailed.push_back(&l);
            if (tag == "RECOVERY") {
                const std::string level = dataCallField(l.msg, "level");
                const std::string action = dataCallField(l.msg, "action");
                if (level == "L2_PDP" &&
                    (action == "soft-rebuild" || action == "escalate")) evImxRecoverPdp.push_back(&l);
                if (level == "L3_CFUN" &&
                    (action == "cycle" || action == "escalate")) evImxRecoverCfun.push_back(&l);
                if (level == "L4_HARDWARE" &&
                    (action == "power-cycle" || action == "enter")) evImxRecoverHardware.push_back(&l);
                if (dataCallField(l.msg, "class") == "CONFIGURATION" ||
                    (action == "enter" && dataCallField(l.msg, "next") == "CONFIG_ERROR"))
                    evImxConfigError.push_back(&l);
            }
        }
        // RK3506J 不使用 IMX 的 FAILURE/RETRY/RECOVERY 标签；EC200A 与 EG912
        // 两条实际构建路径均把状态机前缀写入正文。以下均为 rk3506j_dialer.cpp
        // 的固定日志，不以普通 online=0 或 CSQ 值猜测故障。
        if (pi.plat == PLAT_RK3506J) {
            const std::string& tag = l.tagText();
            if ((tag == "EC200A" || tag == "EG912") &&
                (icontains(l.msg, "state=FAILURE_RETRY") ||
                 icontains(l.msg, "-> FAILURE_RETRY")))
                evRkRetry.push_back(&l);
            if (tag == "bringup" && icontains(l.msg, "SIM is not ready"))
                evRkSimNotReady.push_back(&l);
            if (tag == "bringup" && icontains(l.msg, "LTE/EPS is not registered"))
                evRkRegistration.push_back(&l);
            if (tag == "bringup" &&
                (icontains(l.msg, "PDP profile configuration failed") ||
                 icontains(l.msg, "failed to configure PDP") ||
                 icontains(l.msg, "CGACT activate") ||
                 icontains(l.msg, "QNETDEVCTL did not reach") ||
                 icontains(l.msg, "QNETDEVCTL wait timed out")))
                evRkPdp.push_back(&l);
            if (tag == "bringup" &&
                (icontains(l.msg, "DHCP") || icontains(l.msg, "IPv4") ||
                 icontains(l.msg, "static IP")))
                evRkNetwork.push_back(&l);
            if ((tag == "DEVICE" || tag == "AT" || tag == "usb") &&
                (icontains(l.msg, "missing") || icontains(l.msg, "failed") ||
                 icontains(l.msg, "not found") || icontains(l.msg, "unavailable")))
                evRkDeviceAt.push_back(&l);
            if ((tag == "EC200A" || tag == "EG912") &&
                (icontains(l.msg, "Unable to ping google, attempt") ||
                 icontains(l.msg, "ping failed, attempt")))
                evRkPing.push_back(&l);
        }
        if (isFaultStart(l.msg)) {
            faultOpen[l.sourceId] = true;
            faultStartTime[l.sourceId] = l.t;
        }
        int recoveryDuration = 0;
        if (isRecovered(l.msg, &recoveryDuration)) {
            const auto bound = sourceBounds.find(l.sourceId);
            const long long sourceSpan = bound == sourceBounds.end() ? 0 :
                                         bound->second.second - bound->second.first;
            const bool selfContained = isSelfContainedRecovery(l.msg);
            if (!faultOpen[l.sourceId] && !selfContained) evOrphanRecovery.push_back(&l);
            else if (faultOpen[l.sourceId] &&
                     crossesClockBase(faultStartTime[l.sourceId], l.t))
                evImpossibleRecovery.push_back(&l);
            else if (recoveryDuration < 0 ||
                     static_cast<long long>(recoveryDuration) > sourceSpan + 60)
                evImpossibleRecovery.push_back(&l);
            faultOpen[l.sourceId] = false;
        }
        // EC200A 门控日志(ec200a/dial/dial.cpp:1615/1622)。EG25 无对应日志:
        // 其门控是静默的(eg25/dial/dial.c:974 的 has_connected_once 条件)。
        if (icontains(l.msg, "never-connected"))            evNeverConn.push_back(&l);
        bool netUp = false;
        if (netHeartbeatInterface(l, netUp) && !netUp) evNetInterfaceNone.push_back(&l);
        if (netHeartbeatTxWithoutRx(l)) evNetTxWithoutRx.push_back(&l);
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
            evHardRegistrationIssue.push_back(&l);
        }
        if (limitedService) {
            evLimited.push_back(&l);
            evRegistrationIssue.push_back(&l);
            evHardRegistrationIssue.push_back(&l);
        }
        if (suspectedAccount) {
            evSuspectedAccount.push_back(&l);
            evRegistrationIssue.push_back(&l);
        }
        if (regQueryFailed) evRegQueryFail.push_back(&l);
        if (hasCopsMode(l.msg, '1'))              evManualCops.push_back(&l);
        if (isCopsAutoRestoreOk(l.msg))           evCopsAutoRestore.push_back(&l);
        if (isRegistrationRecovered(l.msg))       evRegistrationRecovered.push_back(&l);
        if (isManualSelectionCommand(l.msg))      evManualSelectionCommand.push_back(&l);
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

        // EG25 2026-08 新增的持久化诊断。两行分别钉死 SDK 返回值和退出动作：
        //   [SDK] Initialization data call failure, ret=N
        //   [FATAL][PROCESS EXIT] QL_Data_Call_Init failed | ret=N | pid=P
        // 第二行经 FMT_SD 拆分后 tag=FATAL，msg 仍以 [PROCESS EXIT] 开头。
        if (l.tagText() == "SDK" &&
            icontains(l.msg, "Initialization data call failure"))
            evDataCallInitFailed.push_back(&l);
        if (l.tagText() == "FATAL" &&
            icontains(l.msg, "[PROCESS EXIT]") &&
            icontains(l.msg, "QL_Data_Call_Init failed"))
            evDataCallFatalExit.push_back(&l);
        if (l.tagText() == "SDK" &&
            icontains(l.msg, "start data call failure"))
            evDataCallStartFailed.push_back(&l);
        if (l.msg.find("DataCall disconnected") != std::string::npos) {
            const std::string initiator = dataCallField(l.msg, "initiator");
            const std::string reason = dataCallField(l.msg, "reason");
            if (initiator == "APP_STOP") {
                evDataCallAppStop.push_back(&l);
                ++appStopReasons[reason.empty() ? "UNSPECIFIED" : reason];
            } else if (initiator == "SDK_URC" && reason == "UNSOLICITED") {
                evDataCallUnsolicited.push_back(&l);
                ++unsolicitedReasons[reason];
            }
        }
        if (l.tagText() == "APN" &&
            (icontains(l.msg, "fp is NULL") ||
             icontains(l.msg, "fread error") ||
             icontains(l.msg, "json_root is NULL") ||
             icontains(l.msg, "json_apn_array error")))
            evApnLoadFailed.push_back(&l);
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
    if (!evOrphanRecovery.empty() || !evImpossibleRecovery.empty()) {
        Finding f;
        f.severity = 1;
        f.title = "恢复记录缺少可信故障起点或时长不可能，已排除出断网统计";
        f.detail = "恢复行必须与同一日志来源内的 fault timer started 配对；声明时长还必须"
                   "不超过该来源的实际观测跨度。未满足时通常表示日志缺失、授时跳变或残留"
                   "状态，不能据此计算断网和可用率。";
        f.advice = "补充同进程的 unsynced/前序日志并核对系统授时记录；产品持续时间应使用"
                   "CLOCK_MONOTONIC。工具已保留原始恢复值作为异常证据。";
        for (const LogLine* line : evOrphanRecovery) {
            if (f.ev.size() >= 3) break;
            f.ev.push_back(mkEv(*line));
        }
        for (const LogLine* line : evImpossibleRecovery) {
            if (f.ev.size() >= 3) break;
            f.ev.push_back(mkEv(*line));
        }
        fs.push_back(std::move(f));
    }

    // ---- modem_mng_v2 专属状态机诊断 ----
    // v2 没有旧产品的 HEARTBEAT、fault timer 与 L1/L2/L3 阶梯。这里只使用
    // modem.c 的固定动作原文下结论，不从普通 ping fail/低 CSQ 猜根因。
    if (v2Platform) {
        auto addV2Finding = [&](int severity, std::string title, std::string detail,
                                std::string advice, const std::vector<const LogLine*>& evidence) {
            if (evidence.empty()) return;
            Finding finding;
            finding.severity = severity;
            finding.title = std::move(title);
            finding.detail = std::move(detail);
            finding.advice = std::move(advice);
            for (size_t i = 0; i < evidence.size() && i < 3; ++i)
                finding.ev.push_back(mkEv(*evidence[i]));
            if (!finding.ev.empty()) fs.push_back(std::move(finding));
        };

        addV2Finding(
            2,
            "SIM 卡未插入，v2 正在低频轮询",
            "日志明确返回 CME SIM not inserted。v2 对该分支采用 5s→10s→15s 的慢轮询，"
            "不会因卡缺失耗尽普通 CPIN 重试并复位模组。",
            "检查 SIM 卡槽、卡片方向和接触；插卡后确认出现 sim ok，再继续排查注网/APN。",
            evV2NoSim);
        addV2Finding(
            2,
            "网络长期未注册，v2 已触发模组复位",
            "日志出现 No registered so long 或注册阶段失败后的 Reset modem 固定动作，"
            "说明不是一次尚在等待的 CEREG/CGREG 采样。",
            "核对同一时段的 CEREG/CGREG stat、LAC/CI/AcT、SIM 状态和运营商覆盖；"
            "复位反复出现时优先查注册条件，而不是继续缩短复位周期。",
            evV2LongUnregistered);
        addV2Finding(
            2,
            "连续 ping 失败触发重初始化",
            "v2 的 keepalive 重试次数已经达到配置上限，源码随后将状态转入 MD_ERROR，"
            "重新执行模组初始化；单条 ping failed(x/y) 不会触发本结论。",
            "结合 WAN down/up 断网区间和 CSQ/CEREG 指标判断是覆盖、注册还是数据面故障；"
            "若频繁重初始化，保留完整周期日志。",
            evV2PingReinit);
        addV2Finding(
            1,
            "进入 READY 时 WAN 仍离线",
            "READY 横幅明确记录 online=0。v2 会继续在 READY 周期内刷新注册、CSQ 和 WAN，"
            "该行证明当时尚未联网，但不等同于进程初始化失败。",
            "查看随后是否出现 WAN ping OK；若持续离线，再结合注册状态和 SIM/CSQ 证据处理。",
            evV2ReadyOffline);
        addV2Finding(
            2,
            "READY 状态失败并复位",
            "日志出现 failed to in ready state，表明状态机从 MD_READY 进入错误处理并调用模组复位。",
            "回看此前连续 ping、WAN online、CEREG/CGREG 与 CSQ；区分网络不可达和模组/AT 端口异常。",
            evV2ReadyFailed);
    }

    // IMX6ULL 1.25.x 状态机诊断。每项均对应 imx6ull_dialer.cpp 的固定日志动作；
    // 不从 online=0、低 CSQ 或普通 NET 信息猜测根因。
    if (pi.plat == PLAT_IMX) {
        auto addImxFinding = [&](int severity, std::string title, std::string detail,
                                 std::string advice, const std::vector<const LogLine*>& evidence) {
            if (evidence.empty()) return;
            Finding finding;
            finding.severity = severity;
            finding.title = std::move(title);
            finding.detail = std::move(detail);
            finding.advice = std::move(advice);
            for (size_t i = 0; i < evidence.size() && i < 3; ++i) finding.ev.push_back(mkEv(*evidence[i]));
            fs.push_back(std::move(finding));
        };
        if (!evImxFailure.empty() || !evImxRetry.empty()) {
            std::vector<const LogLine*> evidence = evImxFailure;
            for (const LogLine* line : evImxRetry)
                if (evidence.size() < 3) evidence.push_back(line);
            addImxFinding(
                2,
                "IMX6ULL 拨号失败，已进入退避重试",
                "【源码直证】旧版以 FAILURE/RETRY 记录退避；1.25.1 以 RECOVERY 的 class、level、"
                "action、attempt 和 wait_s 记录同一动作。它们是恢复进行中的证据，不是联网恢复。",
                "按证据中的 state、pdn、if 和 reason 分流排查；保留完整失败—恢复周期，确认重试是否反复耗尽。",
                evidence);
        }
        addImxFinding(2, "IMX6ULL SIM 未就绪",
                      "【源码直证】AT+CPIN 检查未返回 READY，状态机转入 FAILURE_RETRY。",
                      "检查卡槽、卡接触、PIN 锁状态和 CPIN 原始响应；SIM 未就绪前继续拨号没有意义。",
                      evImxSimNotReady);
        addImxFinding(2, "IMX6ULL 网络注册等待失败",
                      "【源码直证】CEREG 响应无法解析或在注册等待窗口内超时，状态机未进入正常数据拨号。",
                      "核对 CEREG、CSQ、运营商选择和 AT 端口响应；区分无注册、响应损坏和覆盖问题。",
                      evImxRegWait);
        addImxFinding(2, "IMX6ULL PDP 数据连接失败",
                      "【源码直证】CID1 请求被拒、PDP 等待耗尽或状态不可解析，数据面尚未就绪。",
                      "核对 APN/PDP 类型、QNETDEVCTL/QNETDEVSTATUS 原始响应和模组侧 PDP 上下文。",
                      evImxPdp);
        addImxFinding(2, "IMX6ULL DHCP/IPv4/路由配置失败",
                      "【源码直证】DHCP 客户端失败或网卡没有 IPv4/默认路由，问题位于 AP 侧网络配置或链路可达性。",
                      "检查接口是否存在、udhcpc 进程与租约、IP/网关/路由/DNS；再复核探测端点可达性。",
                      evImxDhcp.empty() ? evImxNetwork : evImxDhcp);
        addImxFinding(2, "IMX6ULL 模组拓扑或 AT 通道不可用",
                      "【源码直证】USB 模组拓扑发现超时/失败，或已选 AT 端口打开、读写响应失败。",
                      "检查 USB 枚举、option 驱动绑定、AT 端口节点及供电；确认网卡与 AT 口来自同一模组。",
                      evImxDevice.empty() ? evImxAt : evImxDevice);
        addImxFinding(1, "IMX6ULL AT 遥测命令超时",
                      "【日志直证】心跳的 at_timeout=1 表示本轮遥测 AT 命令没有获得终止响应。"
                      "该字段本身不等同 AT 端口完全不可用；新版会另以 at_probe 或 [AT] 基础探测确认。",
                      "查看同一心跳的 at_probe 及后续 [AT] timeout event；probe=ok 时优先保留现场观察，"
                      "probe=fail 或连续失败达到阈值时检查 AT 口、USB 与模组供电。",
                      evImxAtTelemetryTimeout);
        addImxFinding(2, "IMX6ULL AT 基础确认探测失败",
                      "【源码直证】遥测超时后，拨号循环用裸 AT 探测确认控制面；连续失败达到配置阈值才升级恢复。",
                      "核对 failed streak/limit、AT 口节点和 USB 枚举；不要仅凭单条遥测超时直接判定模组失联。",
                      evImxAtProbeFailed);
        addImxFinding(1, "IMX6ULL 已执行 PDP 软重建恢复",
                      "【源码直证】RECOVERY level=L2_PDP 的 soft-rebuild 会重建数据呼叫；达到上限才升级 CFUN。",
                      "核对 PDP 上下文、APN 和 QNETDEV 状态；若反复出现，保留每次 attempt 与 reason。",
                      evImxRecoverPdp);
        addImxFinding(1, "IMX6ULL 已执行 CFUN 射频恢复",
                      "【源码直证】RECOVERY level=L3_CFUN action=cycle 执行 CFUN=0/1，并受冷却时间限制。",
                      "结合注册状态与 cooldown 观察；频繁升级说明 PDP 软重建未解决根因。",
                      evImxRecoverCfun);
        addImxFinding(2, "IMX6ULL 已升级硬件级模组恢复",
                      "【源码直证】RECOVERY level=L4_HARDWARE 进入后会执行 power-cycle，并持久化冷却时间。",
                      "检查 USB/供电/模组硬件和此前 AT 失败；保留冷却期内的完整日志，避免把延迟动作误判为卡死。",
                      evImxRecoverHardware);
        addImxFinding(2, "IMX6ULL 拨号配置错误，恢复已停止",
                      "【源码直证】CONFIGURATION 类故障进入 CONFIG_ERROR，而非继续重拨或复位模组。",
                      "修正 APN、PDP 类型等配置后重启服务；重拨、CFUN 和硬件复位不能修复配置值。",
                      evImxConfigError);
    }

    // RK3506J 的两个实际构建分支（外置 EC200A / EG912 minimal）共用下列
    // bringup 与状态机文案。仅在平台横幅已确认时输出，避免把其它产品的普通
    // “failed” 文本错归入 RK3506。
    if (pi.plat == PLAT_RK3506J) {
        auto addRkFinding = [&](int severity, std::string title, std::string detail,
                                std::string advice, const std::vector<const LogLine*>& evidence) {
            if (evidence.empty()) return;
            Finding finding;
            finding.severity = severity;
            finding.title = std::move(title);
            finding.detail = std::move(detail);
            finding.advice = std::move(advice);
            for (size_t index = 0; index < evidence.size() && index < 3; ++index)
                finding.ev.push_back(mkEv(*evidence[index]));
            fs.push_back(std::move(finding));
        };
        addRkFinding(2, "RK3506J SIM 未就绪",
                     "【源码直证】ECM bringup 的 AT+CPIN 检查未返回 READY，拨号不会继续进入 PDP 激活。",
                     "检查卡槽、卡接触和 PIN 锁；保留 CPIN 原始应答，确认不是 AT 通道超时。",
                     evRkSimNotReady);
        addRkFinding(2, "RK3506J LTE/EPS 未注册",
                     "【源码直证】注册等待耗尽后明确记录 LTE/EPS is not registered，当前尚不能建立数据连接。",
                     "核对 CEREG、运营商选择、覆盖和 SIM 数据权限；不要仅凭一次 CSQ 数值归因。",
                     evRkRegistration);
        addRkFinding(2, "RK3506J PDP/ECM 数据激活失败",
                     "【源码直证】PDP profile、CGACT 或 QNETDEVCTL 的固定 bringup 失败文案表明模组侧数据面未就绪。",
                     "核对 APN、PDP 类型、CGACT 与 QNETDEVCTL 原始响应，再检查模组 profile。",
                     evRkPdp);
        addRkFinding(2, "RK3506J DHCP/IPv4 配置失败",
                     "【源码直证】DHCP 或 CGCONTRDP 静态 IPv4 回退失败，问题位于 AP 侧接口配置或模组返回的地址信息。",
                     "检查接口存在性、udhcpc、IP/网关/默认路由和 CGCONTRDP 响应。",
                     evRkNetwork);
        addRkFinding(2, "RK3506J 模组拓扑或 AT 通道不可用",
                     "【源码直证】设备发现/AT 端口日志明确报告缺失、未找到或操作失败。",
                     "检查 USB 枚举、option 驱动绑定、AT 端口节点及供电，并确认 AT 口和网卡属于同一模组。",
                     evRkDeviceAt);
        addRkFinding(1, "RK3506J 连通性探测失败",
                     "【日志直证】状态机记录了接口绑定的 Google ping 连续失败尝试；单次失败不等同于已完成重拨。",
                     "结合随后 HB30 的 online、fail_streak/retry 和 FAILURE_RETRY 状态判断是否升级恢复。",
                     evRkPing);
        addRkFinding(2, "RK3506J 已进入失败重试状态",
                     "【源码直证】EC200A/EG912 状态机明确进入 FAILURE_RETRY；这不是普通心跳中的离线采样。",
                     "从同一轮前序 bringup、注册、PDP、DHCP 和 AT 证据定位失败层级，保留恢复完成前的完整日志。",
                     evRkRetry);
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
    const bool hardRegistrationIssueTimeSorted = linePtrTimesSorted(evHardRegistrationIssue);
    const bool suspectedAccountTimeSorted = linePtrTimesSorted(evSuspectedAccount);
    const bool notReadyTimeSorted = linePtrTimesSorted(evNotReady);

    /* Every successful COPS=0 attempt gets its own cycle.  Do not collapse a
     * repeated restore loop into one event: failed restore attempts are useful
     * evidence, while only a later registration result makes it high strength. */
    struct ManualCopsCycle {
        const LogLine* manual = nullptr;
        const LogLine* restored = nullptr;
        const LogLine* recovered = nullptr;
        const LogLine* selectCommand = nullptr;
    };
    std::vector<ManualCopsCycle> manualCopsCycles;
    std::set<std::uint16_t> manualCopsRecoveredSources;
    for (const LogLine* restored : evCopsAutoRestore) {
        const LogLine* manual = nullptr;
        const LogLine* selected = nullptr;
        const LogLine* recovered = nullptr;
        for (const LogLine* candidate : evManualCops)
            if (candidate->sourceId == restored->sourceId && candidate->lineNo < restored->lineNo &&
                (!manual || candidate->lineNo > manual->lineNo)) manual = candidate;
        if (!manual) continue;
        for (const LogLine* candidate : evManualSelectionCommand)
            if (candidate->sourceId == restored->sourceId && candidate->lineNo < manual->lineNo &&
                (!selected || candidate->lineNo > selected->lineNo)) selected = candidate;
        for (const LogLine* candidate : evRegistrationRecovered)
            if (candidate->sourceId == restored->sourceId && candidate->lineNo > restored->lineNo) {
                recovered = candidate;
                break;
            }
        manualCopsCycles.push_back({manual, restored, recovered, selected});
        if (recovered) manualCopsRecoveredSources.insert(restored->sourceId);
    }

    /* Cold-start registration duration and expected reg_check retry logging.
     * This is intentionally separate from an outage: no successful data path
     * has been observed yet. */
    struct StartupRegistrationBlock { const LogLine* begin; const LogLine* end; long long seconds; };
    struct RegistrationLogGap { const LogLine* before; const LogLine* after; long long seconds; };
    std::vector<StartupRegistrationBlock> startupRegistrationBlocks;
    std::vector<RegistrationLogGap> registrationLogGaps;
    std::map<std::uint16_t, const LogLine*> pendingStart;
    std::map<std::uint16_t, const LogLine*> activeReg;
    std::map<std::uint16_t, const LogLine*> previousRegLine;
    std::map<std::uint16_t, RegistrationLogGap> largestGap;
    std::set<std::uint16_t> connectedSources;
    for (const auto& item : lines) {
        const LogLine& l = lineRef(item);
        if (isProgramStartBanner(l.msg)) {
            connectedSources.erase(l.sourceId); pendingStart.erase(l.sourceId);
            activeReg.erase(l.sourceId); previousRegLine.erase(l.sourceId); largestGap.erase(l.sourceId);
        }
        if (isRegCheckEntered(l.msg)) {
            activeReg[l.sourceId] = &l; previousRegLine[l.sourceId] = &l;
            largestGap.erase(l.sourceId);
            /* A CFUN retry can enter reg_check again before the first attach.
             * Preserve the very first attempt so the reported cold-start wait
             * is not shortened by an intermediate recovery action. */
            if (!connectedSources.count(l.sourceId) && !pendingStart.count(l.sourceId))
                pendingStart[l.sourceId] = &l;
            continue;
        }
        auto active = activeReg.find(l.sourceId);
        if (active != activeReg.end()) {
            const LogLine* previous = previousRegLine[l.sourceId];
            if (previous && l.t >= previous->t && l.t - previous->t > 120) {
                RegistrationLogGap gap{previous, &l, l.t - previous->t};
                auto old = largestGap.find(l.sourceId);
                if (old == largestGap.end() || gap.seconds > old->second.seconds) largestGap[l.sourceId] = gap;
            }
            previousRegLine[l.sourceId] = &l;
            if (isRegistrationRecovered(l.msg)) {
                auto start = pendingStart.find(l.sourceId);
                if (start != pendingStart.end() && l.t >= start->second->t &&
                    l.t - start->second->t >= 60)
                    startupRegistrationBlocks.push_back({start->second, &l, l.t - start->second->t});
                auto gap = largestGap.find(l.sourceId);
                if (gap != largestGap.end()) registrationLogGaps.push_back(gap->second);
                activeReg.erase(l.sourceId); previousRegLine.erase(l.sourceId);
                largestGap.erase(l.sourceId); pendingStart.erase(l.sourceId);
            }
        }
        if (isRegistrationRecovered(l.msg)) connectedSources.insert(l.sourceId);
    }

    // ---- 1. 从未联网(SIM/账户问题):恢复阶梯被 has_connected_once 门控 ----
    if (!evNeverConn.empty()) {
        bool connectedBeforeGate = false;
        const long long firstGate = evNeverConn.front()->t;
        for (const auto& item : lines) {
            const LogLine& line = lineRef(item);
            if (line.t >= firstGate) break;
            bool interfaceUp = false;
            if (isNetworkConnectedNotification(line.msg) ||
                (netHeartbeatInterface(line, interfaceUp) && interfaceUp)) {
                connectedBeforeGate = true;
                break;
            }
        }
        Finding f;
        f.severity = 2;
        if (connectedBeforeGate) {
            f.title = "进程重启后丢失既有联网上下文,L1/L2/L3 恢复被门控";
            f.detail = "门控行之前已有设备数据接口在线证据；因此 never-connected 仅描述新拨号进程，"
                       "不能表述为设备或 SIM 从未联网。重拉后 has_connected_once 未恢复，"
                       "进程停留在 L0 重拨，未继续执行射频重置或 L3。";
            f.advice = "修复重拉后的联网历史继承，或为 never-connected 门控设置最大持续时长；"
                       "同时保留 SIM/APN/运营商 PDP 证据以追查首次失败。";
        } else {
            f.title = "当前拨号进程未成功联网,L1/L2/L3 恢复被 has_connected_once 门控";
            f.detail = "日志出现 never-connected 门控行。该结论仅覆盖当前拨号进程，"
                       "不推断设备历史联网情况；从未 ping 通的进程只停留在 L0。";
            f.advice = "查 SIM 数据权限、APN 和注册状态；补充故障前日志确认设备历史联网状态。";
        }
        for (size_t i = 0; i < evNeverConn.size() && i < 3; ++i) f.ev.push_back(mkEv(*evNeverConn[i]));
        fs.push_back(std::move(f));
    }

    if (!evNetInterfaceNone.empty()) {
        Finding f;
        f.severity = 2;
        f.title = "数据接口不可用: [HEARTBEAT-NET] 记录 IF=(none) " +
                  std::to_string(evNetInterfaceNone.size()) + " 次";
        f.detail = "【日志直证】SIM/LTE 注册状态与数据接口是不同层次。IF=(none) 表示设备当时"
                   "没有可用数据接口；它可直接证明 PDP/数据呼叫未建立或已释放，不能单独定责"
                   "为运营商、SIM、APN 或模组。";
        if (!evNetTxWithoutRx.empty())
            f.detail += " 同期还记录到有发送但 RX 长时间空闲的接口样本，说明故障起点存在数据面异常。";
        f.advice = "结合 PDP 激活结果、CEER、CGACT/CGATT、实际 APN 和运营商 PDP/ESM 原因码定责；"
                   "不要仅以 REG=1 判断设备在线。";
        for (size_t i = 0; i < evNetInterfaceNone.size() && i < 3; ++i)
            f.ev.push_back(mkEv(*evNetInterfaceNone[i]));
        fs.push_back(std::move(f));
    }

    // ---- 2. 注册与账户诊断 ----
    if (!manualCopsCycles.empty()) {
        size_t complete = 0;
        const ManualCopsCycle* firstComplete = nullptr;
        const ManualCopsCycle* firstIncomplete = nullptr;
        for (const ManualCopsCycle& cycle : manualCopsCycles) {
            if (cycle.recovered) {
                ++complete;
                if (!firstComplete) firstComplete = &cycle;
            } else if (!firstIncomplete) firstIncomplete = &cycle;
        }
        if (firstComplete) {
            Finding f;
            f.severity = 1;
            f.title = "手动选网解除后恢复注册（强关联） " + std::to_string(complete) + " 次";
            f.detail = "【强关联推断】同一日志来源先由 AT+COPS? 确认处于手动选网(mode=1)，"
                       "随后成功执行 AT+COPS=0，之后才出现注册/连接成功。手动锁网是本次"
                       "未注册的首要本地侧嫌疑，但日志不能排除网络侧同时恢复，不能断言为唯一根因。";
            if (firstComplete->selectCommand)
                f.detail += " 此前存在应用自身的 AT+COPS=1/运营商选择日志，锁网来源可追溯至应用流程。";
            else
                f.detail += " 未见此前应用选网命令，锁网来源未知（可能来自旧会话或外部 AT 客户端）。";
            f.advice = "追查手动选网来源；若业务不要求固定运营商，启动发现 mode=1 且未注册时"
                       "应尽早切回 AT+COPS=0，并保留 COPS/CEREG 原始应答。";
            f.ev.push_back(mkEv(*firstComplete->manual));
            f.ev.push_back(mkEv(*firstComplete->restored));
            f.ev.push_back(mkEv(*firstComplete->recovered));
            fs.push_back(std::move(f));
        }
        if (firstIncomplete) {
            Finding f;
            f.severity = 0;
            f.title = "手动选网已解锁但未见后续注册成功 " +
                      std::to_string(manualCopsCycles.size() - complete) + " 次";
            f.detail = "【日志直证】已确认 mode=1 并收到 COPS=0 成功应答，但当前日志片段之后"
                       "没有注册/连接成功证据；不能把这类周期宣称为已恢复。";
            f.advice = "补充 COPS=0 后的 CEREG、状态迁移和 DataCall 日志；同时检查运营商覆盖与账户状态。";
            f.ev.push_back(mkEv(*firstIncomplete->manual));
            f.ev.push_back(mkEv(*firstIncomplete->restored));
            fs.push_back(std::move(f));
        }
    } else if (!evManualCops.empty()) {
        Finding f;
        f.severity = 0;
        f.title = "检测到手动选网模式，尚无完整恢复证据";
        f.detail = "【日志直证】AT+COPS? 返回 mode=1；仅有手动状态，尚无同源的 COPS=0 成功"
                   "及后续注册成功链，不能推断本次已由解锁恢复。";
        f.advice = "补充 COPS=0 应答与后续 CEREG/连接日志；若业务不要求固定运营商，检查手动选网来源。";
        f.ev.push_back(mkEv(*evManualCops.front()));
        fs.push_back(std::move(f));
    }

    if (!startupRegistrationBlocks.empty()) {
        const StartupRegistrationBlock* longest = &startupRegistrationBlocks.front();
        for (const auto& block : startupRegistrationBlocks)
            if (block.seconds > longest->seconds) longest = &block;
        Finding f;
        f.severity = longest->seconds >= 300 ? 1 : 0;
        f.title = "冷启动注册阻塞 " + std::to_string(startupRegistrationBlocks.size()) +
                  " 次，最长 " + fmtDuration(longest->seconds);
        f.detail = "【日志直证】从首次 sim_op -> reg_check 到首次注册/连接成功持续 " +
                   fmtDuration(longest->seconds) + "；此时尚未观察到已联网业务，因此不计为运行期断网。";
        f.advice = "结合 COPS 模式、CEREG、运营商选择与射频恢复动作排查；不要把冷启动注册耗时"
                   "混入已联网后的可用率或断网统计。";
        f.ev.push_back(mkEv(*longest->begin));
        f.ev.push_back(mkEv(*longest->end));
        fs.push_back(std::move(f));
    }

    if (!registrationLogGaps.empty()) {
        const RegistrationLogGap* longest = &registrationLogGaps.front();
        for (const auto& gap : registrationLogGaps)
            if (gap.seconds > longest->seconds) longest = &gap;
        Finding f;
        f.severity = 1;
        f.title = "注册等待期存在日志空洞 " + std::to_string(registrationLogGaps.size()) +
                  " 段，最大 " + fmtDuration(longest->seconds);
        f.detail = "【日志直证】reg_check 活动期间相邻已记录行相隔 " + fmtDuration(longest->seconds) +
                   "；无法仅凭该空洞判定进程阻塞、日志丢失或模组命令阻塞，恢复因果链存在观测盲区。";
        f.advice = "补充同时间段系统日志、进程/看门狗日志及存储 I/O 信息；产品侧可为 reg_check 重试"
                   "增加带单调时钟的周期性诊断。";
        f.ev.push_back(mkEv(*longest->before));
        f.ev.push_back(mkEv(*longest->after));
        fs.push_back(std::move(f));
    }

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
        bool allSuspectedSourcesHaveManualRecovery = !evSuspectedAccount.empty();
        for (const LogLine* line : evSuspectedAccount)
            if (!manualCopsRecoveredSources.count(line->sourceId))
                allSuspectedSourcesHaveManualRecovery = false;
        if (allSuspectedSourcesHaveManualRecovery) {
            f.severity = 0;
            f.title = "SIM 账户/订阅异常提示已降级：存在手动选网冲突";
            f.detail = "【推断】产品按 SIM READY、信号良好且 REG=0 持续至少 120 秒输出"
                       "SUSPECTED；但本日志另有手动选网(mode=1)并在 COPS=0 后恢复注册的"
                       "强关联证据。该提示不能作为本次故障的主归因，更不能据此断言欠费或停机。";
            f.advice = "优先处理手动选网来源；若之后仍发生 REG=0，再携带 CPIN/CEREG/COPS/"
                       "CGATT/CEER 原始应答向运营商核验账户、数据业务和漫游权限。";
        } else {
            f.severity = 1;
            f.title = "疑似 SIM 账户/订阅异常(SUSPECTED)";
            f.detail = "【推断】产品仅在 SIM READY、信号良好且 REG=0 连续至少 120 秒时输出该诊断。"
                       "标准 AT 无运营商后台停机字段，因此这不是欠费/停机的确定证据。";
            f.advice = "保留行内 CPIN/CEREG/COPS/CGATT/CEER 证据，向运营商核验账户、数据业务和"
                       "漫游权限；同时排除覆盖与选网问题。";
        }
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

    // EG25 的 QL_Data_Call_Init 失败路径是进程级故障：当前产品源码在持久化
    // [FATAL][PROCESS EXIT] 后 close 日志并 exit(EXIT_FAILURE)。后续版本横幅只证明
    // “进程后来又启动了”；由 sw_mng 拉起是产品源码直证，不冒充成单份日志直证。
    if (!evDataCallInitFailed.empty() || !evDataCallFatalExit.empty()) {
        const LogLine* restartAfterExit = nullptr;
        for (const LogLine* fatal : evDataCallFatalExit) {
            for (const LogLine* started : evProgramStart) {
                if (started->t <= fatal->t) continue;
                // 只关联 5 分钟内的启动。数小时/数天后的启动可能来自维护、整机重启或
                // 另一轮故障，不能仅凭先后顺序归到本次 Init 退出。
                if (started->t - fatal->t > 5 * 60) continue;
                if (!restartAfterExit || started->t < restartAfterExit->t)
                    restartAfterExit = started;
            }
        }

        Finding f;
        f.severity = 2;
        f.title = "QL_Data_Call_Init 初始化失败";
        if (!evDataCallFatalExit.empty()) f.title += "，进程主动退出";
        if (restartAfterExit) f.title += "，随后检测到重新启动";
        if (!evDataCallFatalExit.empty()) {
            f.detail = "【日志直证】[FATAL][PROCESS EXIT] 明确记录 SDK 初始化返回值和退出进程 PID。";
        } else {
            f.detail = "【日志直证】SDK 明确记录 Initialization data call failure；本段未见紧随其后的"
                       " [FATAL][PROCESS EXIT]，可能是旧固件或日志在两行之间截断。";
        }
        if (restartAfterExit) {
            f.detail += " 后续又出现版本启动横幅，证明进程后来重新启动；这与上层守护拉起路径相符，"
                        "但仅凭拨号日志不能证明守护进程名称。";
        }
        f.detail += " 【源码直证】当前 EG25 路径随后 log_close() 并 exit(EXIT_FAILURE)，"
                    "sw_mng 在进程缺失时启动 /usr/bin/modem_mng。";
        f.advice = "先查 AP 侧 ql_netd/数据服务是否就绪及 SDK 返回码；再对照 sw_mng 日志确认"
                   "拉起时间。CFUN、换卡和缩短信号恢复阈值不能修复 Data Call 服务初始化失败。";
        for (size_t i = 0; i < evDataCallInitFailed.size() && f.ev.size() < 2; ++i)
            f.ev.push_back(mkEv(*evDataCallInitFailed[i]));
        for (size_t i = 0; i < evDataCallFatalExit.size() && f.ev.size() < 3; ++i)
            f.ev.push_back(mkEv(*evDataCallFatalExit[i]));
        if (restartAfterExit && f.ev.size() < 3) f.ev.push_back(mkEv(*restartAfterExit));
        fs.push_back(std::move(f));
    }

    // artery 1.29.18 的 QL_Data_Call_Init 失败以 exit(0) 交给外部守护拉起，
    // 退出前新增了当前进程所创建 RBMaster 的回收；格式不同于 EG25 的 SDK/FATAL。
    if (!evArteryDataCallExit.empty()) {
        const LogLine* restartAfterExit = nullptr;
        for (const LogLine* exited : evArteryDataCallExit) {
            for (const LogLine* started : evProgramStart) {
                // 多文件合并时不能把另一来源的版本横幅关联到本次退出。
                if (started->sourceId != exited->sourceId || started->t <= exited->t) continue;
                if (started->t - exited->t > 5 * 60) continue;
                if (!restartAfterExit || started->t < restartAfterExit->t)
                    restartAfterExit = started;
            }
        }

        Finding f;
        f.severity = 2;
        f.title = "artery DataCall 初始化失败，进程主动退出 " +
                  std::to_string(evArteryDataCallExit.size()) + " 次";
        if (restartAfterExit) f.title += "，随后检测到重新启动";
        f.detail = "【日志直证】artery 明确记录 DataCall 初始化失败后以 status 0 退出，"
                   "交由 supervisor 重启；该版本会先尝试回收本进程启动的 RBMaster。";
        if (restartAfterExit)
            f.detail += " 同一日志来源在 5 分钟内出现新的 DIAL Version 横幅，证明进程随后重新启动。";
        f.advice = "检查 QL_Data_Call_Init 的调用条件、AP 侧数据服务及 supervisor 拉起记录；"
                   "同时核对紧邻的 RBMaster 回收日志是否为 SIGTERM 超时或 SIGKILL。";
        for (const LogLine* cleanup : evArteryDataCallCleanup) {
            if (cleanup->sourceId == evArteryDataCallExit.front()->sourceId) {
                f.ev.push_back(mkEv(*cleanup));
                break;
            }
        }
        for (const LogLine* exited : evArteryDataCallExit) {
            if (f.ev.size() >= 3) break;
            f.ev.push_back(mkEv(*exited));
        }
        if (restartAfterExit && f.ev.size() < 3) f.ev.push_back(mkEv(*restartAfterExit));
        fs.push_back(std::move(f));
    }

    if (!evLicenseBackupFailed.empty()) {
        Finding f;
        f.severity = 1;
        f.title = "Roamlink license 备份失败，重启已延后 " +
                  std::to_string(evLicenseBackupFailed.size()) + " 次";
        f.detail = "【日志直证】license 备份失败后，程序明确记录已延后重启。"
                   "【源码直证】当前 artery/EG25 会保留原有有效备份，并在 300 秒激活"
                   "窗口内每 30 秒重新检测和尝试备份；这不是已完成激活，也不是已降级"
                   " FORCE_SIM。";
        f.advice = "检查 license 主文件与备份目录的挂载、剩余空间、读写权限和文件系统 I/O"
                   " 错误；保留同一激活窗口内后续的备份成功、重启或超时降级日志。";
        if (!evLicensePending.empty()) f.ev.push_back(mkEv(*evLicensePending.back()));
        for (const LogLine* line : evLicenseBackupFailed) {
            if (f.ev.size() >= 3) break;
            f.ev.push_back(mkEv(*line));
        }
        fs.push_back(std::move(f));
    }

    if (!evLicenseAtomicallyBackedUp.empty()) {
        Finding f;
        f.severity = 0;
        f.title = "Roamlink license 已原子备份 " +
                  std::to_string(evLicenseAtomicallyBackedUp.size()) + " 次";
        f.detail = "【日志直证】license 已记录为原子备份完成。"
                   "【源码直证】当前实现先写入同目录临时文件并 fsync，再 rename 到正式"
                   "备份路径并同步父目录，因而写入中断不会截断既有有效备份。";
        f.advice = "这是备份持久化成功信息；若它属于首次下载激活流程，可继续查看随后"
                   "的重启及启动后的 license probe 日志确认完整闭环。";
        for (const LogLine* line : evLicenseAtomicallyBackedUp) {
            if (f.ev.size() >= 3) break;
            f.ev.push_back(mkEv(*line));
        }
        fs.push_back(std::move(f));
    }

    // artery 的 Roamlink license 缺失不是普通“断网”：产品刻意保持物理 SIM
    // 联网，让 RBMaster 下载 license；到 300s 仍未出现才停止 RBMaster、主动断开
    // DataCall 后重拨并保持 FORCE_SIM。该断开是应用处置动作，绝不能混入 SDK
    // 非预期掉线或旧产品的 fault timer 断网统计。
    if (!evLicenseTimeout.empty()) {
        Finding f;
        f.severity = 2;
        f.title = "Roamlink license 下载超时，已降级 FORCE_SIM " +
                  std::to_string(evLicenseTimeout.size()) + " 次";
        f.detail = "【日志直证】license 缺失且无可用备份后，设备先通过物理 SIM 联网并启动 "
                   "RBMaster 下载；日志明确记录等待上限届满后放弃下载。"
                   "【源码直证】此分支保持 FORCE_SIM、停止 RBMaster 释放 SIM 通道，并主动"
                   "重拨，避免 RBMaster 抢占通道造成地址/DNS 异常；因此紧随其后的 "
                   "Net disconnected 是应用处置动作，不作为非预期网络断线统计。";
        f.advice = "采集同一时间窗的 RBMaster、DNS/resolv.conf、许可证下载 URL 的 HTTP/TLS"
                   " 错误及服务端请求日志；同时检查 license 主文件和备份文件的写入、挂载与持久化。";
        if (!evLicenseMissing.empty()) f.ev.push_back(mkEv(*evLicenseMissing.front()));
        if (!evLicensePending.empty() && f.ev.size() < 3) f.ev.push_back(mkEv(*evLicensePending.back()));
        for (const LogLine* line : evLicenseTimeout) {
            if (f.ev.size() >= 3) break;
            f.ev.push_back(mkEv(*line));
        }
        fs.push_back(std::move(f));
    }

    if (!evDataCallStartFailed.empty()) {
        Finding f;
        f.severity = evDataCallStartFailed.size() >= 3 ? 2 : 1;
        f.title = "数据调用启动失败 " + std::to_string(evDataCallStartFailed.size()) + " 次";
        f.detail = "[SDK] start data call failure 保存了 profile 与原始十六进制错误码；"
                   "这是拨号 Start 阶段失败，不等同于 SIM 未注册，也不等同于 Init 导致进程退出。";
        f.advice = "按错误码核对 APN/profile、注册状态和数据服务；若连续失败，保留前序 CEREG、"
                   "APN 选择及随后状态机重试日志。";
        for (size_t i = 0; i < evDataCallStartFailed.size() && i < 3; ++i)
            f.ev.push_back(mkEv(*evDataCallStartFailed[i]));
        fs.push_back(std::move(f));
    }

    auto summarizeReasons = [](const std::map<std::string, size_t>& counts) {
        std::string text;
        for (const auto& entry : counts) {
            if (!text.empty()) text += ", ";
            text += entry.first + "=" + std::to_string(entry.second);
        }
        return text.empty() ? std::string("无 reason 字段") : text;
    };

    if (!evDataCallAppStop.empty()) {
        Finding f;
        f.severity = 0;
        f.title = "应用主动停止 DataCall " + std::to_string(evDataCallAppStop.size()) + " 次";
        f.detail = "【源码直证】当前 artery 在调用 QL_Data_Call_Stop 前保存 reason，并在 30 秒内按"
                   " profile/IP family 匹配断开回调后输出 initiator=APP_STOP。reason 汇总: " +
                   summarizeReasons(appStopReasons) +
                   "。这些事件是应用主动动作，不作为 SDK 非预期掉线或网络故障证据。";
        f.advice = "按 reason 回看对应状态机动作；START_CALL_TIMEOUT 重点检查拨号建立阶段，"
                   "其余切换/重拨原因结合 ROAMLINK、REG 和 TCP 诊断。";
        for (size_t i = 0; i < evDataCallAppStop.size() && i < 3; ++i)
            f.ev.push_back(mkEv(*evDataCallAppStop[i]));
        fs.push_back(std::move(f));
    }

    if (!evDataCallUnsolicited.empty()) {
        Finding f;
        f.severity = 1;
        f.title = "SDK 非预期断线(SDK_URC/UNSOLICITED) " +
                  std::to_string(evDataCallUnsolicited.size()) + " 次";
        f.detail = "【日志直证】断开回调明确记录 initiator=SDK_URC reason=UNSOLICITED。"
                   "【源码直证】当前 artery 仅在回调未命中 30 秒内同 profile/IP family 的应用 Stop"
                   " 原因时输出该组合，因此它是非预期链路断开证据；reason 汇总: " +
                   summarizeReasons(unsolicitedReasons) + "。";
        f.advice = "结合断线前后的注册状态、信号、CEER、QMI/SDK 错误码和恢复耗时判断网络侧"
                   "释放、覆盖波动或数据服务异常；不要与 APP_STOP 主动停拨混算。";
        for (size_t i = 0; i < evDataCallUnsolicited.size() && i < 3; ++i)
            f.ev.push_back(mkEv(*evDataCallUnsolicited[i]));
        fs.push_back(std::move(f));
    }

    if (!evApnLoadFailed.empty()) {
        Finding f;
        f.severity = 1;
        f.title = "APN 配置文件读取/解析失败 " + std::to_string(evApnLoadFailed.size()) + " 次";
        f.detail = "[APN] 明确记录文件打开、读取或 JSON 结构失败；代码随后可能回退默认 APN，"
                   "所以该事件本身不能证明最终拨号失败。";
        f.advice = "检查 APN JSON 路径、权限、文件完整性和 apn 数组结构；再看是否出现"
                   "“use default apn”以及后续 DataCall connected。";
        for (size_t i = 0; i < evApnLoadFailed.size() && i < 3; ++i)
            f.ev.push_back(mkEv(*evApnLoadFailed[i]));
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

    // ---- 4. 设备级主事故 ----
    const Outage* longestRecovered = nullptr;
    for (const auto& outage : outs) {
        if (outage.recovered && (!longestRecovered || outage.dur > longestRecovered->dur))
            longestRecovered = &outage;
    }
    if (longestRecovered && longestRecovered->dur >= 30 * 60) {
        Finding f;
        f.severity = 2;
        f.title = "设备级主事故: 数据业务中断 " + fmtDuration(longestRecovered->dur);
        f.detail = "该事故按设备连续状态跨日切文件和拨号进程重拉归并；恢复边沿为数据接口重新出现、"
                   "数据呼叫恢复或明确联网通知。其余断网分类为全量日志历史汇总，不能直接作为本事故根因。";
        f.advice = "优先围绕本事故窗口导出 PDP/ESM、APN、SIM 数据权限、CEER 和基带/系统日志；"
                   "不要用其他日期的短时弱信号或注册事件替代本事故定责。";
        for (const auto& item : lines) {
            const LogLine& line = lineRef(item);
            if (line.lineNo == longestRecovered->startLine && f.ev.empty()) f.ev.push_back(mkEv(line));
            if (line.lineNo == longestRecovered->endLine && f.ev.size() < 2) f.ev.push_back(mkEv(line));
        }
        if (!f.ev.empty()) fs.push_back(std::move(f));
    }

    // ---- 5. 逐次断网根因分类（全量历史汇总，不替代主事故定责） ----
    // 判据(均为窗口内的实证特征,窗口 = 断网起点前 90s 至恢复点):
    //   弱信号     :窗口内心跳 CSQ 有效样本的最小值 < 10(CSQ<10 ≈ RSSI<-95dBm)
    //   数据假死   :窗口内出现 ΔRX==0(RX_PKT 不增长)
    //   切卡/选网  :窗口内出现 [SLOT]/[OPER]/[CFUN]
    //   注册/账户异常:窗口内出现 Registration Denied、NETWORK REJECTED、LIMITED SERVICE
    //                 或产品有边界的 SUSPECTED subscription issue；查询失败不作为根因。
    size_t causeCnt[C_N] = {0};
    std::vector<Evidence> causeEv[C_N];
    if (!v2Platform) for (const auto& o : outs) {
        long long lo = o.start - 90, hi = o.recovered ? o.end : lineRef(lines.back()).t;
        const std::uint16_t outageSourceId = sourceIdAtLine(lines, o.startLine);
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
        /* An explicit DENY/limited-service indication remains authoritative.
         * A heuristic SUSPECTED account message does not: when the same outage
         * overlaps a complete manual-COPS -> COPS=0 -> recovery chain, report
         * that conflict instead of making SUSPECTED the root cause. */
        const LogLine* dn = firstLineInWindow(evHardRegistrationIssue, lo, hi,
                                              hardRegistrationIssueTimeSorted);
        const LogLine* suspected = firstLineInWindow(evSuspectedAccount, lo, hi,
                                                     suspectedAccountTimeSorted);
        bool completedManualCycleInWindow = false;
        for (const ManualCopsCycle& cycle : manualCopsCycles) {
            if (cycle.manual && cycle.recovered && cycle.manual->sourceId == outageSourceId &&
                cycle.manual->t <= hi && cycle.recovered->t >= lo) {
                completedManualCycleInWindow = true;
                break;
            }
        }
        if (!dn && suspected && !completedManualCycleInWindow) dn = suspected;
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
        f.title = "全量日志历史汇总:断网根因分类:" + std::string(kCauseName[c]) + " —— " +
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
    // 版本间阈值不同，且日志可直接带实际 threshold；结论优先使用该现场值，
    // 没有现场值时不套用可能过期的平台默认常量。
    if (!v2Platform && !evRecL3.empty()) {
        Finding f;
        f.severity = 2;
        f.title  = "L3 已触发:进程主动 exit,交由 watchdog/init 重启";
        const int loggedThreshold = loggedL3ThresholdSeconds(evRecL3.front()->msg);
        f.detail = loggedThreshold > 0
                   ? "该事件日志实际写明 L3 阈值为 " + fmtDuration(loggedThreshold) +
                     "；阶梯已走到尽头。L3 只是纯进程退出，清不掉挂死的 CP 固件。"
                   : "该事件已触发 L3；日志未提供可解析的实际阈值，工具不套用平台默认值。"
                     "L3 只是纯进程退出，清不掉挂死的 CP 固件。";
        f.advice = "若 L3 反复出现,说明重启无法解决,应查 SIM/账户/覆盖或模组固件。";
        for (size_t i = 0; i < evRecL3.size() && i < 3; ++i) f.ev.push_back(mkEv(*evRecL3[i]));
        fs.push_back(std::move(f));
    }
    if (!v2Platform && (!evRecL1.empty() || !evRecL2.empty())) {
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
    if (!v2Platform && evRecL1.empty() && evRecL2.empty() && evRecL3.empty()) {
        long long thr = (pi.plat == PLAT_EG25) ? 60 : 5 * 60;
        const Outage* lng = nullptr;
        for (const auto& o : outs) if (o.recovered && o.dur >= thr) { lng = &o; break; }
        if (lng) {
            Finding f;
            f.severity = 1;
            f.title  = "恢复阶梯一次都没触发,但存在超过 L1 阈值的断网";
            f.detail = "有断网时长 ≥ L1 阈值(EG25 60s / EC200A 5min)却无任何 [RECOVERY] 日志。";
            if (pi.plat == PLAT_EG25) {
                const std::uint16_t outageSource = sourceIdAtLine(lines, lng->startLine);
                int policy = -1;
                std::string channel;
                bool connectedBefore = false;
                const LogLine* policyEvidence = nullptr;
                const LogLine* channelEvidence = nullptr;
                for (const auto& item : lines) {
                    const LogLine& line = lineRef(item);
                    if (line.sourceId != outageSource || line.lineNo > lng->startLine) continue;
                    std::string lowerMessage = lower(line.msg);
                    size_t policyPos = lowerMessage.rfind("policy=");
                    if (policyPos == std::string::npos) policyPos = lowerMessage.rfind("policy:");
                    if (policyPos != std::string::npos) {
                        policyPos += 7;
                        while (policyPos < lowerMessage.size() && lowerMessage[policyPos] == ' ') ++policyPos;
                        if (policyPos < lowerMessage.size() && std::isdigit((unsigned char)lowerMessage[policyPos])) {
                            policy = lowerMessage[policyPos] - '0';
                            policyEvidence = &line;
                        }
                    }
                    const auto fields = hbFields(line.msg);
                    auto ch = fields.find("CH");
                    if (ch != fields.end() && !ch->second.empty()) {
                        channel = ch->second;
                        channelEvidence = &line;
                    }
                    if (icontains(line.msg, "DataCall connected") ||
                        icontains(line.msg, "net_connected") ||
                        icontains(line.msg, "Network recovered")) connectedBefore = true;
                }
                const bool forceSim = policy == 4;
                const bool simChannel = channel == "SIM" || channel == "sim";
                if ((policy >= 0 && !forceSim) || (!channel.empty() && !simChannel)) {
                    const std::string policyText = policy >= 0 ? std::to_string(policy) : "未知";
                    f.detail += " 日志在该断网前明确显示 policy=" + policyText +
                                "、CH=" + (channel.empty() ? "未知" : channel) +
                                "；其中至少一项不满足 EG25 FORCE_SIM 阶梯条件。";
                    f.advice = "这是有日志证据的门控结果；按当前策略检查对应的 Roamlink/切卡恢复链。";
                } else if (forceSim && simChannel && connectedBefore) {
                    f.detail += " 日志在该断网前明确显示 policy=4、CH=SIM 且已有联网证据，"
                                "可见门控条件均满足；不能把未触发解释成策略/通道关闭。";
                    f.advice = "检查恢复日志是否缺失、故障计时是否被状态机提前清零，或对应产品版本"
                               "是否仍使用受墙钟跳变影响的计时。";
                } else {
                    f.detail += " 未能从该断网同一来源的前序日志完整确定 policy、通道和"
                                "has_connected_once，不能推断为结构性关闭。";
                    f.advice = "补充该进程启动与故障前日志，确认 policy、CH 和首次联网证据。";
                }
                if (policyEvidence) f.ev.push_back(mkEv(*policyEvidence));
                if (channelEvidence && channelEvidence != policyEvidence)
                    f.ev.push_back(mkEv(*channelEvidence));
            } else {
                f.detail += " 未能从日志证据判定被何条件门控。";
                f.advice = "确认 has_connected_once、平台策略和故障期间状态机条件。";
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
