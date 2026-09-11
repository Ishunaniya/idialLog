// log_parser.cpp — 日志格式解析、增量状态机与多文件定序
#include "log_parser.h"
#include "log_time.h"
#include "log_internal.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <utility>

namespace dl {

// ============================ ANSI ============================
// 剥离 CSI 转义序列 ESC '[' ... 终止字符(@-~)。
// 实证:open_dial_for_artery/src/seas_log/seas_log.c:54 SEAS_DISPLAY_COLOR=0(不发颜色码),
// 但 :63 SEAS_DISPLAY_RESET=1 → :241 每行在 level 与 func 之间必定插入一个 "\x1B[0m"
// (SEAS_COLOR_RESET,seas_log.c:46)。若将来把 COLOR 打开,前缀色码也一并被本函数剥掉。
std::string stripAnsi(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1B' && i + 1 < s.size() && s[i+1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && !((s[j] >= '@' && s[j] <= '~'))) j++;
            i = (j < s.size()) ? j : s.size() - 1;   // 跳过终止字符
            continue;
        }
        o += s[i];
    }
    return o;
}

// ============================ 解析 ============================
// 高频标签字典。顺序把百万行基准/真机最常见值放前面,线性匹配通常 1~3 次即命中；
// 新固件出现未知标签时 customTag 仍原样保留,不会为了省内存静默丢信息。
static const std::string kKnownTags[] = {
    "", "HEARTBEAT", "HEARTBEAT-NET", "TRACE", "STATE", "CELL CHANGE", "CELL", "SDK", "ROAMLINK",
    "DIAG", "SLOT", "OPER", "RECOVERY L1", "RECOVERY L2", "RECOVERY L3",
    "ERROR", "WARN", "WARNING", "FATAL", "INFO", "ALARM", "CFUN", "SIM", "APN",
    "INIT", "MODEM", "EVENT", "STATUS", "LED", "TZ", "NANOMSG", "PING", "PING OUT",
    "PING FAIL", "PING ERROR", "REG", "REG TIMEOUT", "REG DIAG", "ZERO", "ZERO ADDR",
    "CPDUMP", "COPS", "SM", "LOGMIGR", "LOGCLEAN", "CLEANUP", "NetCheck",
    "LOG_E", "LOG_I", "LOG_D", "CONSOLE", "SYSLOG", "MODEM_MNG_V2",
    // IMX6ULL modem_mng 1.25+: 30s 状态采样、5min 详细采样及故障状态机。
    "HB", "HB30", "HB300", "FAILURE", "RETRY", "PDP", "NET", "DHCP", "DEVICE", "USB", "POWER", "EXIT", "SERVICE", "PLMN", "AT", "VERSION",
    // IMX6ULL 1.25.1: registration diagnostics and terminal configuration state.
    "REG", "CONFIG_ERROR"
};

LogLine::LogLine(const LogLine& o)
    : t(o.t), lineNo(o.lineNo), ts(o.ts), msg(o.msg),
      customTag(o.customTag ? std::make_unique<std::string>(*o.customTag) : nullptr),
      ms(o.ms), tagId(o.tagId), sourceId(o.sourceId), fmt(o.fmt), level(o.level),
      inferredTime(o.inferredTime) {}

LogLine& LogLine::operator=(const LogLine& o) {
    if (this == &o) return *this;
    t = o.t; lineNo = o.lineNo; ts = o.ts; msg = o.msg;
    customTag = o.customTag ? std::make_unique<std::string>(*o.customTag) : nullptr;
    ms = o.ms; tagId = o.tagId; sourceId = o.sourceId; fmt = o.fmt; level = o.level;
    inferredTime = o.inferredTime;
    return *this;
}

void LogLine::setTag(std::string tag) {
    for (std::uint16_t i = 0; i < sizeof(kKnownTags) / sizeof(kKnownTags[0]); ++i) {
        if (tag == kKnownTags[i]) { tagId = i; customTag.reset(); return; }
    }
    tagId = 0;
    customTag = std::make_unique<std::string>(std::move(tag));
}

const std::string& LogLine::tagText() const {
    if (customTag) return *customTag;
    if (tagId < sizeof(kKnownTags) / sizeof(kKnownTags[0])) return kKnownTags[tagId];
    return kKnownTags[0];
}

void LogLine::setLevel(const std::string& text) {
    std::string v = lower(text);
    if      (v == "all")      level = LEVEL_ALL;
    else if (v == "debug")    level = LEVEL_DEBUG;
    else if (v == "info")     level = LEVEL_INFO;
    else if (v == "notice")   level = LEVEL_NOTICE;
    else if (v == "warning")  level = LEVEL_WARNING;
    else if (v == "error")    level = LEVEL_ERROR;
    else if (v == "fatal")    level = LEVEL_FATAL;
    else if (v == "critical") level = LEVEL_CRITICAL;
    else if (!v.empty())       level = LEVEL_OTHER;
    else                       level = LEVEL_NONE;
}

const std::string& LogLine::levelText() const {
    static const std::string names[] = {
        "", "ALL", "DEBUG", "INFO", "NOTICE", "WARNING", "ERROR", "FATAL", "CRITICAL", "OTHER"
    };
    return names[level <= LEVEL_OTHER ? level : LEVEL_NONE];
}

// 从 rest 中剥出首个 "[TAG] ":首字符须大写字母或下划线,其余为 [A-Z0-9_ ]。
// 多词标签实证存在(modem_mng/open_dial 源码穷举):RECOVERY L1/L2/L3、CELL CHANGE、
// REG TIMEOUT、ZERO ADDR、PING OUT/FAIL/ERROR、REG DIAG 等。
static void splitTag(const std::string& rest, LogLine& L) {
    if (rest.empty() || rest[0] != '[') { L.msg = rest; return; }
    size_t e = rest.find(']');
    if (e == std::string::npos) { L.msg = rest; return; }
    std::string tag = rest.substr(1, e - 1);
    // 首字符须字母/下划线,其余允许字母数字下划线、连字符和空格。
    // open_dial 的 [HEARTBEAT-NET] 是正式结构化状态行；此前连字符使其
    // 退化为无标签普通消息，虽不丢行，但分析层无法识别数据接口状态。
    // 【源码穷举】标签绝大多数全大写,但 modem_mng 有且仅有一个混合大小写的:[NetCheck]
    // (真机 AG35 1.32.16 控制台日志实证);故不能只认 [A-Z]。
    bool ok = !tag.empty() &&
              (((tag[0] >= 'A' && tag[0] <= 'Z') || (tag[0] >= 'a' && tag[0] <= 'z') || tag[0] == '_'));
    if (ok) for (char c : tag)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ')) { ok = false; break; }
    if (!ok) { L.msg = rest; return; }
    L.setTag(trim(tag));
    size_t q = e + 1;
    while (q < rest.size() && rest[q] == ' ') q++;
    L.msg = rest.substr(q);
}

// FMT_SD: "[YYYY-MM-DD HH:MM:SS] <msg>"
// 实证:logger_sd.c:544 strftime("%Y-%m-%d %H:%M:%S") + :570 fprintf("[%s] ",time_str)
// A(modem_mng)与 B(open_dial)的 logger_sd.c 行格式完全一致(逐字节 diff 仅管道差异)。
static bool parseSd(const std::string& line, LogLine& L) {
    if (line.size() < 22 || line[0] != '[' || line[20] != ']') return false;
    int Y=0, Mo=0, D=0, h=0, mi=0, s=0;
    if (std::sscanf(line.c_str() + 1, "%4d-%2d-%2d %2d:%2d:%2d",
                    &Y, &Mo, &D, &h, &mi, &s) != 6) return false;
    L.ts  = line.substr(1, 19);
    L.t   = mkEpoch(Y, Mo, D, h, mi, s);
    L.fmt = FMT_SD;
    size_t p = 21;
    while (p < line.size() && line[p] == ' ') p++;
    splitTag(line.substr(p), L);
    return true;
}

// FMT_SEAS: "YYYY-MM-DD HH:MM:SS.mmm [LEVEL] <ESC[0m>func (file:line) - <msg>"
// 实证(open_dial_for_artery/src/seas_log/seas_log.c seas_emit_log 逐段 asprintf):
//   :233 时间 "%s.%03ld" + " "        (SEAS_DISPLAY_TIME=1)
//   :237 颜色 ""                      (SEAS_DISPLAY_COLOR=0 → 无色码)
//   :239 级别 "%2s" + " "             (level 形如 "[INFO]")
//   :241 复位 "\x1B[0m"               (SEAS_DISPLAY_RESET=1 → 必有)
//   :245 函数 "%s" + " "              (SEAS_DISPLAY_FUNC=1)
//   :259 "(" + file + ":" + line + ") "(SEAS_DISPLAY_FILE/LINE=1)
//   :270 边框 "-" + " "               (SEAS_DISPLAY_BORDER=1)
//   :289 消息, :296 "\n"
// 级别取值:main.c:48 seas_log_config level=SEAS_LEVEL_INFO → [DEBUG] 被抑制,
// 可见 [INFO]/[NOTICE]/[WARNING]/[ERROR]/[ALL]/[FATAL]。
// 容错:各 SEAS_DISPLAY_* 开关可被改;故 func/(file:line)/边框 均按“有则吃、无则跳”解析。
// 注:本分支未经真机日志实证(全机未找到 artery 真机日志;仅
// open_dial_for_artery/md/analyse/open_dial_roamlink_analysis_v2.md 内 1 行文档示例
// "2024-01-15 10:25:03.899 [INFO] dial_task (dial.c:245) - CSQ: 20",且该示例已被
// 文档剥掉 ESC 码)。故此处对 ESC[0m 有无都能解析。
static bool parseSeas(const std::string& line0, LogLine& L) {
    // 时间 + 毫秒
    if (line0.size() < 24) return false;
    int Y=0, Mo=0, D=0, h=0, mi=0, s=0, ms=0;
    if (std::sscanf(line0.c_str(), "%4d-%2d-%2d %2d:%2d:%2d.%3d",
                    &Y, &Mo, &D, &h, &mi, &s, &ms) != 7) return false;
    if (line0[19] != '.') return false;
    L.ts  = line0.substr(0, 19);
    L.ms  = ms;
    L.t   = mkEpoch(Y, Mo, D, h, mi, s);
    L.fmt = FMT_SEAS;

    std::string line = stripAnsi(line0);   // 去掉 ESC[0m(及潜在色码)
    size_t p = 23;                          // "YYYY-MM-DD HH:MM:SS.mmm"
    while (p < line.size() && line[p] == ' ') p++;

    // [LEVEL]
    if (p < line.size() && line[p] == '[') {
        size_t e = line.find(']', p);
        if (e != std::string::npos) {
            L.setLevel(line.substr(p + 1, e - p - 1));
            p = e + 1;
            while (p < line.size() && line[p] == ' ') p++;
        }
    }
    // func(到空格或 '(' 为止)
    while (p < line.size() && line[p] != ' ' && line[p] != '(') p++;
    while (p < line.size() && line[p] == ' ') p++;

    // (file:line)
    if (p < line.size() && line[p] == '(') {
        size_t e = line.find(')', p);
        if (e != std::string::npos) {
            p = e + 1;
            while (p < line.size() && line[p] == ' ') p++;
        }
    }
    // 边框 "- "
    if (p + 1 < line.size() && line[p] == '-' && line[p+1] == ' ') p += 2;
    else if (p < line.size() && line[p] == '-' && p + 1 == line.size()) p += 1;

    splitTag(line.substr(p), L);   // 消息内可再带 [TAG](实证:[INIT]/[HEARTBEAT]/[ROAMLINK]/[ZERO ADDR])
    return true;
}

static size_t skipField(const std::string& line, size_t p) {
    while (p < line.size() && line[p] == ' ') ++p;
    while (p < line.size() && line[p] != ' ') ++p;
    return p;
}

static bool splitSyslogApp(const std::string& field, std::string& app);

// Android logcat -v threadtime：
//   YYYY-MM-DD HH:MM:SS.mmm pid tid I TAG: msg（部分采集器补全年）
//   MM-DD HH:MM:SS.mmm pid tid I TAG: msg（Android 原生 threadtime）
static bool parseAndroid(const std::string& line0, LogLine& L, int yearHint) {
    int Y=0, Mo=0, D=0, h=0, mi=0, s=0, ms=0;
    size_t p = 0;
    bool fullYear = line0.size() >= 24 && line0[4] == '-' && line0[7] == '-';
    if (fullYear) {
        if (std::sscanf(line0.c_str(), "%4d-%2d-%2d %2d:%2d:%2d.%3d",
                        &Y,&Mo,&D,&h,&mi,&s,&ms) != 7) return false;
        p = 23;
    } else {
        if (line0.size() < 19 || line0[2] != '-' || line0[5] != ' ') return false;
        if (std::sscanf(line0.c_str(), "%2d-%2d %2d:%2d:%2d.%3d",
                        &Mo,&D,&h,&mi,&s,&ms) != 6) return false;
        Y = yearHint ? yearHint : 1970;
        p = 18;
    }
    // pid、tid、单字母级别。
    p = skipField(line0, p);
    p = skipField(line0, p);
    while (p < line0.size() && line0[p] == ' ') ++p;
    if (p >= line0.size() || std::string("VDIWEF").find(line0[p]) == std::string::npos)
        return false;
    char level = line0[p++];
    while (p < line0.size() && line0[p] == ' ') ++p;
    size_t colon = line0.find(':', p);
    if (colon == std::string::npos || colon == p) return false;
    std::string outerTag = trim(line0.substr(p, colon - p));
    p = colon + 1;
    while (p < line0.size() && line0[p] == ' ') ++p;

    char ts[32];
    std::snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d", Y,Mo,D,h,mi,s);
    L.ts = ts; L.t = mkEpoch(Y,Mo,D,h,mi,s); L.ms = ms; L.fmt = FMT_ANDROID;
    if      (level == 'V') L.setLevel("ALL");
    else if (level == 'D') L.setLevel("DEBUG");
    else if (level == 'I') L.setLevel("INFO");
    else if (level == 'W') L.setLevel("WARNING");
    else if (level == 'E') L.setLevel("ERROR");
    else if (level == 'F') L.setLevel("FATAL");
    L.setTag(outerTag);
    splitTag(line0.substr(p), L); // 正文中的 [RECOVERY]/[INIT] 优先
    return true;
}

// RFC3339 syslog：YYYY-MM-DDTHH:MM:SS[.mmm] host app[pid]: msg。
static bool parseRfc3339Syslog(const std::string& line0, LogLine& L) {
    if (line0.size() < 21 || line0[4] != '-' || line0[7] != '-' || line0[10] != 'T')
        return false;
    int Y=0,Mo=0,D=0,h=0,mi=0,s=0,ms=-1;
    if (std::sscanf(line0.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                    &Y,&Mo,&D,&h,&mi,&s) != 6) return false;
    size_t p = 19;
    if (p < line0.size() && line0[p] == '.') {
        int parsedMs = 0;
        if (std::sscanf(line0.c_str() + p + 1, "%3d", &parsedMs) == 1) ms = parsedMs;
        while (p < line0.size() && line0[p] != ' ') ++p;
    }
    while (p < line0.size() && line0[p] == ' ') ++p;
    p = skipField(line0, p); // host
    while (p < line0.size() && line0[p] == ' ') ++p;
    const size_t appStart = p;
    size_t colon = line0.find(':', p);
    if (colon == std::string::npos) return false;
    std::string app;
    const bool v2App = splitSyslogApp(line0.substr(appStart, colon - appStart), app) &&
                       app == "modem_mng_v2";
    p = colon + 1;
    while (p < line0.size() && line0[p] == ' ') ++p;
    char ts[32];
    std::snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d", Y,Mo,D,h,mi,s);
    L.ts = ts; L.t = mkEpoch(Y,Mo,D,h,mi,s); L.ms = ms; L.fmt = FMT_SYSLOG;
    L.setTag(v2App ? "MODEM_MNG_V2" : "SYSLOG");
    // v2 的外层 syslog ident 是可靠的进程身份；正文中形如 "[%s]" 的 where
    // 前缀只是调用位置。若按通用内层标签覆盖，会让仅含 CSQ/ping 片段的日志丢失
    // modem_mng_v2 身份，进而漏掉平台、指标和断网分析。
    if (v2App) L.msg = line0.substr(p);
    else splitTag(line0.substr(p), L);
    return true;
}

static bool decimalChar(char c) { return c >= '0' && c <= '9'; }

static int monthNumber(const std::string& line, size_t p) {
    static const char* names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (p + 3 > line.size()) return 0;
    for (int i = 0; i < 12; ++i)
        if (line.compare(p, 3, names[i]) == 0) return i + 1;
    return 0;
}

static bool validSyslogName(const std::string& text) {
    if (text.empty()) return false;
    for (char c : text) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.' || c == '/'))
            return false;
    }
    return true;
}

// app-name / app-name[pid]。严格限制 PID 为数字，防止把正文里的冒号误当 syslog 头。
static bool splitSyslogApp(const std::string& field, std::string& app) {
    if (field.empty()) return false;
    size_t nameEnd = field.size();
    if (field.back() == ']') {
        size_t open = field.rfind('[');
        if (open == std::string::npos || open == 0 || open + 2 >= field.size()) return false;
        for (size_t i = open + 1; i + 1 < field.size(); ++i)
            if (!decimalChar(field[i])) return false;
        nameEnd = open;
    }
    app = field.substr(0, nameEnd);
    return validSyslogName(app);
}

static bool setFacilityLevel(const std::string& field, LogLine& L) {
    const size_t dot = field.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == field.size()) return false;
    const std::string facility = lower(field.substr(0, dot));
    const std::string severity = lower(field.substr(dot + 1));
    bool knownFacility = facility == "auth" || facility == "authpriv" ||
        facility == "cron" || facility == "daemon" || facility == "ftp" ||
        facility == "kern" || facility == "lpr" || facility == "mail" ||
        facility == "mark" || facility == "news" || facility == "security" ||
        facility == "syslog" || facility == "user" || facility == "uucp" ||
        (facility.size() == 6 && facility.compare(0, 5, "local") == 0 &&
         facility[5] >= '0' && facility[5] <= '7');
    if (!knownFacility) return false;

    if (severity == "emerg" || severity == "alert" || severity == "crit")
        L.setLevel("CRITICAL");
    else if (severity == "err" || severity == "error") L.setLevel("ERROR");
    else if (severity == "warn" || severity == "warning") L.setLevel("WARNING");
    else if (severity == "notice") L.setLevel("NOTICE");
    else if (severity == "info") L.setLevel("INFO");
    else if (severity == "debug") L.setLevel("DEBUG");
    else return false;
    return true;
}

static void setPriLevel(int pri, LogLine& L) {
    switch (pri & 7) {
        case 0: case 1: case 2: L.setLevel("CRITICAL"); break;
        case 3: L.setLevel("ERROR"); break;
        case 4: L.setLevel("WARNING"); break;
        case 5: L.setLevel("NOTICE"); break;
        case 6: L.setLevel("INFO"); break;
        case 7: L.setLevel("DEBUG"); break;
    }
}

// RFC3164 本身没有年份。有同文件年份线索时优先使用；否则以当地当前年
// 作可审计的推定，且将明显落在未来（>31 天）的日期视为上一年的归档日志。
// 这不能恢复历史文件的真实年份，因此调用者必须保留 inferredTime/~ 标记。
static int inferRfc3164Year(int Mo, int D, int h, int mi, int s) {
    std::time_t nowValue = std::time(nullptr);
    std::tm* now = std::localtime(&nowValue);
    if (!now) return 1970;
    int Y = now->tm_year + 1900;
    const long long candidate = mkEpoch(Y, Mo, D, h, mi, s);
    const long long current = mkEpoch(Y, now->tm_mon + 1, now->tm_mday,
                                      now->tm_hour, now->tm_min, now->tm_sec);
    if (candidate - current > 31LL * 86400LL) --Y;
    return Y;
}

// BusyBox/RFC3164：
//   Aug 24 12:34:56 host user.info modem_mng_v2[123]: msg
//   Aug 24 12:34:56 host modem_mng_v2[123]: msg       (facility 可省)
// 亦接受网络报文常见的 <PRI> 前缀。语法故意严格：必须有 host 和合法 app:，
// 避免把普通的 "Aug ..." 文本误收为日志包络。
static bool parseRfc3164Syslog(const std::string& line0, LogLine& L,
                               int yearHint, int previousYear, int previousMonth,
                               int* parsedYear, int* parsedMonth) {
    size_t p = 0;
    int pri = -1;
    if (!line0.empty() && line0[0] == '<') {
        size_t close = line0.find('>');
        if (close == std::string::npos || close < 2 || close > 4) return false;
        pri = 0;
        for (size_t i = 1; i < close; ++i) {
            if (!decimalChar(line0[i])) return false;
            pri = pri * 10 + (line0[i] - '0');
        }
        if (pri > 191) return false;
        p = close + 1;
    }

    const int Mo = monthNumber(line0, p);
    if (!Mo || p + 4 >= line0.size() || line0[p + 3] != ' ') return false;
    p += 4;
    while (p < line0.size() && line0[p] == ' ') ++p;
    size_t dayStart = p;
    int D = 0;
    while (p < line0.size() && decimalChar(line0[p]) && p - dayStart < 2)
        D = D * 10 + (line0[p++] - '0');
    if (p == dayStart || D < 1 || D > 31 || p >= line0.size() || line0[p] != ' ')
        return false;
    while (p < line0.size() && line0[p] == ' ') ++p;
    if (p + 8 > line0.size() || !decimalChar(line0[p]) || !decimalChar(line0[p+1]) ||
        line0[p+2] != ':' || !decimalChar(line0[p+3]) || !decimalChar(line0[p+4]) ||
        line0[p+5] != ':' || !decimalChar(line0[p+6]) || !decimalChar(line0[p+7]))
        return false;
    int h = (line0[p]-'0')*10 + line0[p+1]-'0';
    int mi = (line0[p+3]-'0')*10 + line0[p+4]-'0';
    int s = (line0[p+6]-'0')*10 + line0[p+7]-'0';
    if (h > 23 || mi > 59 || s > 60) return false;
    p += 8;
    if (p >= line0.size() || line0[p] != ' ') return false;
    while (p < line0.size() && line0[p] == ' ') ++p;

    // hostname
    size_t hostStart = p;
    while (p < line0.size() && line0[p] != ' ') ++p;
    if (p == hostStart || !validSyslogName(line0.substr(hostStart, p - hostStart))) return false;
    while (p < line0.size() && line0[p] == ' ') ++p;
    if (p >= line0.size()) return false;

    // BusyBox -S/-O 组合可决定是否输出 facility.severity。
    size_t possibleFacilityStart = p;
    while (p < line0.size() && line0[p] != ' ') ++p;
    LogLine facilityProbe;
    if (setFacilityLevel(line0.substr(possibleFacilityStart, p - possibleFacilityStart), facilityProbe)) {
        L.level = facilityProbe.level;
        while (p < line0.size() && line0[p] == ' ') ++p;
    } else {
        p = possibleFacilityStart;
    }
    if (p >= line0.size()) return false;

    const size_t colon = line0.find(':', p);
    if (colon == std::string::npos) return false;
    // app 是单个字段；冒号前出现空格说明它已经进入正文。
    for (size_t i = p; i < colon; ++i) if (line0[i] == ' ') return false;
    std::string app;
    if (!splitSyslogApp(line0.substr(p, colon - p), app)) return false;
    p = colon + 1;
    while (p < line0.size() && line0[p] == ' ') ++p;

    int Y = previousYear ? previousYear : (yearHint ? yearHint : inferRfc3164Year(Mo,D,h,mi,s));
    // 文件内按时间正序解析；Dec -> Jan 是 RFC3164 跨年的唯一可靠线索。
    if (previousYear && previousMonth >= 10 && Mo <= 3) ++Y;
    char ts[32];
    std::snprintf(ts, sizeof ts, "~%04d-%02d-%02d %02d:%02d:%02d", Y,Mo,D,h,mi,s);
    L.ts = ts;
    L.t = mkEpoch(Y,Mo,D,h,mi,s);
    L.fmt = FMT_SYSLOG;
    L.inferredTime = true;
    if (pri >= 0 && L.level == LEVEL_NONE) setPriLevel(pri, L);
    const bool v2App = app == "modem_mng_v2";
    L.setTag(v2App ? "MODEM_MNG_V2" : "SYSLOG");
    if (v2App) L.msg = line0.substr(p);
    else splitTag(line0.substr(p), L);
    if (parsedYear) *parsedYear = Y;
    if (parsedMonth) *parsedMonth = Mo;
    return true;
}

static bool parseSyslog(const std::string& line0, LogLine& L, int yearHint = 0,
                        int previousRfcYear = 0, int previousRfcMonth = 0,
                        int* parsedRfcYear = nullptr, int* parsedRfcMonth = nullptr) {
    return parseRfc3339Syslog(line0, L) ||
           parseRfc3164Syslog(line0, L, yearHint, previousRfcYear, previousRfcMonth,
                              parsedRfcYear, parsedRfcMonth);
}

static bool splitV2ConsoleEnvelope(const std::string& line, std::string& level,
                                   std::string& message) {
    struct Prefix { const char* text; const char* canonicalLevel; };
    static const Prefix prefixes[] = {
        {"[INFO]", "INFO"}, {"[ERR]", "ERROR"}, {"[WARN]", "WARNING"},
        {"[NOTICE]", "NOTICE"}, {"[DBG]", "DEBUG"}
    };
    for (const Prefix& prefix : prefixes) {
        const size_t n = std::char_traits<char>::length(prefix.text);
        if (line.compare(0, n, prefix.text) != 0) continue;
        if (line.size() != n && line[n] != ' ') return false;
        size_t p = n;
        if (p < line.size() && line[p] == ' ') ++p; // 只吃宏固定加的一个空格
        level = prefix.canonicalLevel;
        message = line.substr(p);
        return true;
    }
    return false;
}

static bool isV2StartBanner(const std::string& message0) {
    return trim(message0) == "===== modem_mng_v2 start =====";
}

static bool isV2ConsoleAnchor(const std::string& message0) {
    // "Version 2.1" 太通用，裸 stderr 中不能证明进程身份；只有包含应用名的
    // start 横幅是无外层 app 字段时足够强的 v2 证据。
    return isV2StartBanner(message0);
}

static std::string inferredTimestamp(const std::string& timestamp) {
    if (timestamp.empty()) return "(无时间戳)";
    return timestamp[0] == '~' ? timestamp : "~" + timestamp;
}

// 未识别行粗分类,供审计页展示“漏在哪”
static std::string classifyUnparsed(const std::string& s) {
    if (s.find("<NUL x") != std::string::npos) return "NUL/二进制损伤";
    if (s.find("+++") != std::string::npos || s.compare(0, 2, "AT") == 0) return "AT 命令/响应续行";
    if (!s.empty() && (s[0] == '+' || s[0] == '$')) return "模组 URC/响应续行";
    if (s.find("===") != std::string::npos) return "分隔/标记行(非会话头)";
    if (s.size() && (unsigned char)s[0] < 0x20) return "控制字符起始";
    return "其它(无时间戳)";
}

struct StreamingLogParser::Impl {
    std::vector<LogLine>& out;
    std::vector<std::string>& sessions;
    std::vector<long long> restartTs;
    ParseAudit ad;
    std::uint16_t sourceId = 0;
    bool nextIsFileStart = false;
    bool finished = false;
    long long lastT = 0;
    std::string lastTs;
    int yearHint = 0;
    int previousRfc3164Year = 0;
    int previousRfc3164Month = 0;
    std::vector<size_t> pendingConsole;
    // SD 日志的 AT 回显可能是“带时间戳的标题 + 响应 + 空行 + OK”。空行不应
    // 关闭这个条目，否则最后的 OK 会被误判为裸控制台输出。
    bool continuationOpen = false;

    Impl(std::vector<LogLine>& outRef, std::vector<std::string>& sessionRef, size_t reserveHint)
        : out(outRef), sessions(sessionRef) {
        out.clear();
        sessions.clear();
        out.reserve(reserveHint);
    }

    void pushLine(std::string line);
    void finish(ParseAudit* audit);
};

void StreamingLogParser::Impl::pushLine(std::string line) {
        if (finished) return;
        const size_t idx = ad.rawTotal++;
        const bool atFileStart = nextIsFileStart;
        nextIsFileStart = false;
        // 去掉行尾 \r\n
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();

        // 文本解析前做字节级完整性审计。NUL 不能被 trim 静默当成空行；压缩连续 NUL
        // 为可见占位，既避免样例膨胀，也让“未识别行”页能直接看到损伤规模。
        {
            size_t nulCount = 0;
            for (char c : line) if (c == '\0') ++nulCount;
            if (nulCount) {
                std::string visible;
                visible.reserve(line.size() - nulCount + 32);
                size_t pos = 0;
                while (pos < line.size()) {
                    if (line[pos] != '\0') { visible.push_back(line[pos++]); continue; }
                    size_t end = pos + 1;
                    while (end < line.size() && line[end] == '\0') ++end;
                    visible += "<NUL x" + std::to_string(end - pos) + ">";
                    pos = end;
                }
                line.swap(visible);
                ad.nulBytes += nulCount;
                ad.nulLines++;
            }
        }

        if (trim(line).empty()) { ad.blank++; return; }

        // 会话标记(logger_sd.c:424 "=== Dial Log Opened [ts] daykey=... ===" (modem_mng)
        //           open_dial:  "=== Dial Program Started [ts] ===" (对等开场标记)
        //           logger_sd.c:527 "=== Program Exit [ts] ===")
        // 【真机实证】open_dial 日志开场是 "Dial Program Started",此前只认 "Dial Log Opened",
        //   导致 open_dial 首行被误计未识别(真机 954 行中恰 1 行)。二者语义对等:都是一次
        //   会话开始,都带时间戳、都算一次进程重启。
        bool isLogOpened = line.find("Dial Log Opened") != std::string::npos;
        bool isProgramStarted = line.find("Dial Program Started") != std::string::npos;
        bool isOpened = isLogOpened || isProgramStarted;
        bool isExited = line.find("Program Exit") != std::string::npos;
        if (line.compare(0, 3, "===") == 0 &&
            (isOpened || isExited)) {
            size_t a = line.find('[');
            size_t b = (a == std::string::npos) ? std::string::npos : line.find(']', a);
            if (a != std::string::npos && b != std::string::npos && b > a + 1 && isOpened) {
                std::string ts = line.substr(a + 1, b - a - 1);  // "YYYY-MM-DD HH:MM:SS"
                int Y,Mo,D,h,mi,s;
                if (isProgramStarted &&
                    std::sscanf(ts.c_str(), "%4d-%2d-%2d %2d:%2d:%2d",&Y,&Mo,&D,&h,&mi,&s)==6)
                    restartTs.push_back(mkEpoch(Y,Mo,D,h,mi,s));
            }
            if (isLogOpened) ad.logOpened++;
            if (isProgramStarted) ad.programStarted++;
            if (isExited) ad.programExited++;
            ad.session++;
            return;
        }

        LogLine L;
        L.lineNo = idx + 1;
        int parsedRfcYear = 0, parsedRfcMonth = 0;
        if (parseSd(line, L) || parseAndroid(line, L, yearHint) ||
            parseSeas(line, L) ||
            parseSyslog(line, L, yearHint, previousRfc3164Year, previousRfc3164Month,
                        &parsedRfcYear, &parsedRfcMonth)) {
            L.sourceId = sourceId;
            ad.parsed++;
            lastT = L.t;
            lastTs = L.ts;
            size_t dateOffset = (!L.ts.empty() && L.ts[0] == '~') ? 1 : 0;
            if (L.ts.size() >= dateOffset + 7 &&
                decimalChar(L.ts[dateOffset]) && decimalChar(L.ts[dateOffset+1]) &&
                decimalChar(L.ts[dateOffset+2]) && decimalChar(L.ts[dateOffset+3])) {
                yearHint = (L.ts[dateOffset]-'0')*1000 + (L.ts[dateOffset+1]-'0')*100 +
                           (L.ts[dateOffset+2]-'0')*10 + (L.ts[dateOffset+3]-'0');
                if (parsedRfcYear) {
                    previousRfc3164Year = parsedRfcYear;
                    previousRfc3164Month = parsedRfcMonth;
                } else if (decimalChar(L.ts[dateOffset+5]) &&
                           decimalChar(L.ts[dateOffset+6])) {
                    // 明示年份格式可作为随后 RFC3164 的同文件锚点。
                    previousRfc3164Year = yearHint;
                    previousRfc3164Month =
                        (L.ts[dateOffset+5]-'0')*10 + (L.ts[dateOffset+6]-'0');
                }
            }
            // 文件开头先出现裸 printf 时，等第一条真实时间到来后再回填；只在同一来源
            // 内回填，绝不跨文件借时间。
            std::vector<size_t> stillPending;
            for (size_t pos : pendingConsole) {
                if (pos < out.size() && out[pos].sourceId == sourceId) {
                    out[pos].t = L.t;
                    out[pos].ts = inferredTimestamp(L.ts);
                    out[pos].inferredTime = true;
                } else {
                    stillPending.push_back(pos);
                }
            }
            pendingConsole.swap(stillPending);
            // 进程重启横幅(每次启动恰一次,各产品措辞各异,全是正常日志行):
            //   RTMS 新版: "Modem_mng Version: rtms_<platform>_<version>"
            //   旧版 modem_mng/open_dial: "Program started. Version/Main Version:"
            //   artery/open_dial 新版: "DIAL Version:"(artery 无 opened 标记)
            // 【真机实证】real_artery_1.29.14 重启 3 次却只有 3 条 "DIAL Version",0 条 opened 标记
            //   —— 只认 opened 标记的话 artery/AG35控制台 的重启全漏(报 0 次)。
            if (isProgramStartBanner(L.msg)) {
                restartTs.push_back(L.t);
                ad.programStarted++;
            }
            // v2 的 Version 与 start 横幅同次出现，只取带 syslog 时间包络的显式
            // start 横幅作为启动信号，避免一轮启动重复计数。
            if (isV2StartBanner(L.msg)) {
                restartTs.push_back(L.t);
                ad.programStarted++;
            }
            // 时钟跳变检测:与上一条已解析行比较,若跨越 2000 年边界(一侧 <2000 一侧 >=2000)
            // 即认定跳变 —— 这是 RTC 未授时(1970)后中途授时的特征。只记录首次跳变(最有意义
            // 的那次:1970→真实时间)。阈值用 2000 年边界而非"差值大",避免把正常跨天误判。
            if (!ad.clockJump && !out.empty()) {
                long long prevT = out.back().t;
                bool prevUnsynced = prevT < 946598400LL;   // <2000-01-01(与 timeBaseOf 同阈值)
                bool curUnsynced  = L.t  < 946598400LL;
                if (prevUnsynced != curUnsynced) {
                    ad.clockJump  = true;
                    ad.jumpFromT  = prevT;
                    ad.jumpToT    = L.t;
                    ad.jumpAtLine = idx + 1;   // 1-based 原始行号
                }
            }
            out.push_back(std::move(L));
            const std::string& message = out.back().msg;
            const size_t last = message.find_last_not_of(" \t");
            continuationOpen = last != std::string::npos && message[last] == ':';
            return;
        }

        // modem_mng_v2 的 log.h 固定镜像到 stderr：
        //   [INFO]/[ERR]/[WARN]/[NOTICE]/[DBG] message
        // 前缀本身不是 v2 身份证明（别的程序也常用），因此普通行只标 CONSOLE +
        // 结构化 level；仅正文含 v2 强签名的该行标 MODEM_MNG_V2。
        {
            const std::string clean = stripAnsi(line);
            std::string level, message;
            if (splitV2ConsoleEnvelope(clean, level, message)) {
                L.lineNo = idx + 1;
                L.sourceId = sourceId;
                L.fmt = FMT_CONSOLE;
                L.t = lastT;
                L.ts = inferredTimestamp(lastTs);
                L.inferredTime = !lastTs.empty();
                L.setLevel(level);
                L.setTag(isV2ConsoleAnchor(message) ? "MODEM_MNG_V2" : "CONSOLE");
                L.msg = std::move(message);
                ad.parsed++;
                // 纯 stderr 没有源码时间，只记录原始启动信号；不伪造 epoch/session。
                if (isV2StartBanner(L.msg)) ad.programStarted++;
                out.push_back(std::move(L));
                continuationOpen = false;
                if (lastTs.empty()) pendingConsole.push_back(out.size() - 1);
                return;
            }
        }

        // 无时间戳但紧跟在已解析行之后 → 多行日志条目的**续行**,并入上一条,不算未识别。
        // 【样本实证】真机 EG25 1.31.15 / artery 1.29.13:AT 应答是分行写的 ——
        //   [15:22:26] [RECOVERY L2] AT+CFUN=0 rsp:
        //   OK                                        ← 本行无时间戳,属上一条
        // 这个"OK"是 CFUN 是否成功的证据,当成未识别丢掉就等于漏掉了诊断信息。
        // (合成夹具里没有多行条目,故此前一直没暴露。)
        // 【样本实证】真正的续行,其上一条必定**以冒号结尾**(在宣告"下面是多行内容"):
        //   EG25 1.31.15 : "[RECOVERY L2] AT+CFUN=0 rsp: " ⏎ "OK"
        //   artery 1.29.13: "cfun stop response: "          ⏎ "OK"
        // 不能只凭"无时间戳"就并入上一条 —— AG35 1.32.16 的**控制台**日志里,dial_log 与裸
        // printf(流量库 dump、"EXEC: serial_atcmd"、"call_id : 1" 等)交织,那样会把 60 行
        // 无关噪声糊进上一条 dial_log 的消息里。SD 卡日志文件只有 dial_log 写入、不存在此问题,
        // 但控制台捕获是用户会拿来分析的真实输入,必须挡住。
        // 挡不住的就老实计入未识别,由审计报出来 —— 那才是诚实的做法。
        // 跨文件防御:本行是某文件首行时,即使无时间戳也不并入上一条(那是上一个文件的),
        // 老实计入未识别,由审计报出。
        if (!out.empty() && out.back().fmt != FMT_CONSOLE && !atFileStart && continuationOpen) {
            out.back().msg += " ⏎ ";
            out.back().msg += line;
            ad.continuation++;
            return;
        }

        // 未识别:计数 + 分类 + 留样(这是“没漏消息”的唯一硬证据)
        ad.unparsed++;
        ad.unparsedKinds[classifyUnparsed(line)]++;
        if (ad.samples.size() < ParseAudit::kMaxSamples)
            ad.samples.push_back(UnparsedLine{idx + 1, line.substr(0, 400)});
        // 不再丢弃未识别/裸 printf：完整保留在时间线。若已有结构化时间，只能沿用
        // 上一条并明确打 inferredTime；这不是把推定时间冒充源码事实。
        L.lineNo = idx + 1;
        L.sourceId = sourceId;
        L.fmt = FMT_CONSOLE;
        L.t = lastT;
        L.ts = inferredTimestamp(lastTs);
        L.inferredTime = !lastTs.empty();
        L.setTag("CONSOLE");
        L.msg = stripAnsi(line);
        out.push_back(std::move(L));
        continuationOpen = false;
        if (lastTs.empty()) pendingConsole.push_back(out.size() - 1);
}

void StreamingLogParser::Impl::finish(ParseAudit* audit) {
    if (finished) {
        if (audit) *audit = ad;
        return;
    }
    // 合并两种重启信号:同一次重启在 modem_mng 上既有 opened 标记又有版本横幅
    // (相隔几秒),去重防重复计数;artery/AG35控制台 只有其一。10s 内视为同一次。
    std::sort(restartTs.begin(), restartTs.end());
    long long prev = -1000000000LL;   // 不用 LLONG_MIN:t-prev 会整数溢出,第一个永远被漏掉
    for (long long t : restartTs) {
        if (t - prev > 10) sessions.push_back(fmtTime(t, "FULL"));
        prev = t;
    }

    finished = true;
    if (audit) *audit = ad;
}

StreamingLogParser::StreamingLogParser(std::vector<LogLine>& out,
                                       std::vector<std::string>& sessions,
                                       size_t reserveHint)
    : impl_(new Impl(out, sessions, reserveHint)) {}

StreamingLogParser::~StreamingLogParser() = default;
StreamingLogParser::StreamingLogParser(StreamingLogParser&&) noexcept = default;
StreamingLogParser& StreamingLogParser::operator=(StreamingLogParser&&) noexcept = default;

void StreamingLogParser::beginFile() {
    if (impl_ && !impl_->finished) {
        impl_->nextIsFileStart = true;
        if (impl_->sourceId != UINT16_MAX) ++impl_->sourceId;
        impl_->lastT = 0;
        impl_->lastTs.clear();
        impl_->yearHint = 0;
        impl_->previousRfc3164Year = 0;
        impl_->previousRfc3164Month = 0;
    }
}

void StreamingLogParser::pushLine(std::string line) {
    if (impl_) impl_->pushLine(std::move(line));
}

void StreamingLogParser::finish(ParseAudit* audit) {
    if (impl_) impl_->finish(audit);
}

void parseLines(const std::vector<std::string>& raw,
                std::vector<LogLine>& out,
                std::vector<std::string>& sessions,
                ParseAudit* audit,
                const std::vector<size_t>& fileBoundaries)
{
    StreamingLogParser parser(out, sessions, raw.size());
    // 边界列表通常只列第二份及后续来源；首份隐含从 0 开始，也要建立来源编号。
    if (!fileBoundaries.empty() && fileBoundaries.front() != 0) parser.beginFile();
    size_t nextBoundaryPos = 0;
    for (size_t idx = 0; idx < raw.size(); ++idx) {
        while (nextBoundaryPos < fileBoundaries.size() &&
               fileBoundaries[nextBoundaryPos] < idx)
            ++nextBoundaryPos;
        if (nextBoundaryPos < fileBoundaries.size() &&
            fileBoundaries[nextBoundaryPos] == idx)
            parser.beginFile();
        parser.pushLine(raw[idx]);
    }
    parser.finish(audit);
}

// ============================ 多文件合并定序 ============================
// 只扫头部若干行取首时间戳。不复用 parseLines:那会把整份日志解析一遍(N 份文件 ×
// 全量行),而定序只需要头部一条。
bool firstTimestamp(const std::vector<std::string>& raw, long long* t, size_t scanLimit) {
    const size_t n = std::min(raw.size(), scanLimit);
    for (size_t i = 0; i < n; ++i) {
        std::string line = raw[i];
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (trim(line).empty()) continue;

        // 会话标记:FMT_SD 文件的首行通常就是它(logger_sd.c:424),它带的时间戳
        // 比后面第一条普通日志更早,是这份文件真正的起点。open_dial 用 "Dial Program Started"。
        if (line.compare(0, 3, "===") == 0 &&
            (line.find("Dial Log Opened") != std::string::npos ||
             line.find("Dial Program Started") != std::string::npos)) {
            size_t a = line.find('[');
            size_t b = (a == std::string::npos) ? std::string::npos : line.find(']', a);
            if (a != std::string::npos && b != std::string::npos && b > a + 1) {
                int Y, Mo, D, h, mi, s;
                if (std::sscanf(line.substr(a + 1, b - a - 1).c_str(),
                                "%4d-%2d-%2d %2d:%2d:%2d", &Y, &Mo, &D, &h, &mi, &s) == 6) {
                    if (t) *t = mkEpoch(Y, Mo, D, h, mi, s);
                    return true;
                }
            }
            continue;   // 是标记但时间戳残缺 → 继续往下找普通行
        }

        LogLine L;
        if (parseSd(line, L) || parseAndroid(line, L, 0) ||
            parseSeas(line, L) || parseSyslog(line, L)) {
            if (t) *t = L.t;
            return true;
        }
    }
    return false;
}

std::vector<size_t> orderByTime(const std::vector<std::vector<std::string>>& chunks) {
    struct Key { size_t idx; long long t; bool hasT; };
    std::vector<Key> keys;
    keys.reserve(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        long long t = 0;
        bool has = firstTimestamp(chunks[i], &t);
        keys.push_back(Key{ i, t, has });
    }
    // stable_sort:同一首时间戳(同一秒内起头的两份)保持输入顺序,不无端打乱。
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        if (a.hasT != b.hasT) return a.hasT;   // 有时间戳的一律在前
        if (!a.hasT) return false;             // 都没有 → 比较结果为“相等”,stable 保持原序
        return a.t < b.t;
    });
    std::vector<size_t> out;
    out.reserve(keys.size());
    for (const auto& k : keys) out.push_back(k.idx);
    return out;
}

// ---- 时基判定 ----
TimeBase timeBaseOf(const std::vector<std::string>& raw) {
    long long t = 0;
    if (!firstTimestamp(raw, &t)) return TB_NONE;
    // mkEpoch 以本地时间构造;这里只需要粗判年份,用 t 反推年份即可。
    // t < (2000-01-01 的 epoch) 即视为未同步。2000-01-01 00:00:00 UTC = 946684800。
    // 阈值取 946684800 - 86400 留一天余量(避免时区把 2000-01-01 本地时刻算到边界外)。
    return (t < 946598400LL) ? TB_UNSYNCED : TB_WALL;
}

MixReport detectMix(const std::vector<std::vector<std::string>>& chunks) {
    MixReport r;
    for (size_t i = 0; i < chunks.size(); ++i) {
        switch (timeBaseOf(chunks[i])) {
            case TB_WALL:     r.wallIdx.push_back(i);     break;
            case TB_UNSYNCED: r.unsyncedIdx.push_back(i); break;
            default:          r.noneIdx.push_back(i);     break;
        }
    }
    r.mixed = !r.wallIdx.empty() && !r.unsyncedIdx.empty();
    return r;
}

// 字段解析:同时支持 "K:V" 与 "K=V"。
// 值的终止:下一个 '|',或下一处 “空白 + 标识符 + [:=]”(否则
// "RSRP:-104 RSRQ:-10"(eg25/diag/diag.c:33)会把 RSRQ 吞进 RSRP 的值;
// artery "state=x csq=20 tcp_fail=0"(main.c:217)也全靠这一条拆开)。

} // namespace dl
