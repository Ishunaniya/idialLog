// logmodel.cpp — 解析/分析实现。手写扫描代替正则(快且可预期);仅用户 --grep 走 std::regex。
#include "logmodel.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#ifdef DL_HAVE_MINIZ
// 只用 mz_ 前缀 API;关掉 zlib 兼容别名(inflate/crc32/... 那些 static inline),
// 否则它们在本 TU 里未被引用会触发 -Wunused-function(本项目 -Wall -Wextra 零告警)。
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
#endif


namespace dl {

// ============================ 小工具 ============================
static inline bool isIdentChar(char c) { // Python 侧是 [A-Za-z_]
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b-1] <= ' ') b--;
    return s.substr(a, b - a);
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// 大小写无关的子串查找(对齐 Python 的 re.IGNORECASE)
static bool icontains(const std::string& hay, const char* needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

// Howard Hinnant 的 days_from_civil:避免 mktime 的时区/夏令时干扰
static long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097 + (long long)doe - 719468;
}

// 与 days_from_civil 互逆
static void civil_from_days(long long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    const long long yy = (long long)yoe + era * 400;
    const unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    const unsigned mp = (5*doy + 2)/153;
    d = doy - (153*mp+2)/5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = (int)(yy + (m <= 2));
}

long long mkEpoch(int Y, int Mo, int D, int h, int mi, int s) {
    return days_from_civil(Y, (unsigned)Mo, (unsigned)D) * 86400LL + h*3600LL + mi*60LL + s;
}

std::string fmtDur(long long sec) {
    char buf[64];
    if (sec < 60)        std::snprintf(buf, sizeof buf, "%llds", (long long)sec);
    else if (sec < 3600) std::snprintf(buf, sizeof buf, "%lldm%02llds", sec/60, sec%60);
    else                 std::snprintf(buf, sizeof buf, "%lldh%02lldm", sec/3600, (sec%3600)/60);
    return buf;
}

std::string fmtTime(long long epoch, const char* fmt) {
    long long days = epoch / 86400;
    long long rem  = epoch % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    int Y; unsigned M, D;
    civil_from_days(days, Y, M, D);
    int h = (int)(rem / 3600), mi = (int)((rem % 3600) / 60), s = (int)(rem % 60);
    char buf[64];
    if (std::strcmp(fmt, "HM") == 0)
        std::snprintf(buf, sizeof buf, "%02d:%02d", h, mi);
    else if (std::strcmp(fmt, "FULL") == 0)
        std::snprintf(buf, sizeof buf, "%04d-%02u-%02u %02d:%02d:%02d", Y, M, D, h, mi, s);
    else // "MD"
        std::snprintf(buf, sizeof buf, "%02u-%02u %02d:%02d:%02d", M, D, h, mi, s);
    return buf;
}

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
// 从 rest 中剥出首个 "[TAG] ":首字符须大写字母或下划线,其余为 [A-Z0-9_ ]。
// 多词标签实证存在(modem_mng/open_dial 源码穷举):RECOVERY L1/L2/L3、CELL CHANGE、
// REG TIMEOUT、ZERO ADDR、PING OUT/FAIL/ERROR、REG DIAG 等。
static void splitTag(const std::string& rest, LogLine& L) {
    if (rest.empty() || rest[0] != '[') { L.msg = rest; return; }
    size_t e = rest.find(']');
    if (e == std::string::npos) { L.msg = rest; return; }
    std::string tag = rest.substr(1, e - 1);
    // 首字符须字母/下划线,其余允许字母数字下划线空格。
    // 【源码穷举】标签绝大多数全大写,但 modem_mng 有且仅有一个混合大小写的:[NetCheck]
    // (真机 AG35 1.32.16 控制台日志实证);故不能只认 [A-Z]。
    bool ok = !tag.empty() &&
              (((tag[0] >= 'A' && tag[0] <= 'Z') || (tag[0] >= 'a' && tag[0] <= 'z') || tag[0] == '_'));
    if (ok) for (char c : tag)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == ' ')) { ok = false; break; }
    if (!ok) { L.msg = rest; return; }
    L.tag = trim(tag);
    size_t q = e + 1;
    while (q < rest.size() && rest[q] == ' ') q++;
    L.msg = rest.substr(q);
}

// FMT_SD: "[YYYY-MM-DD HH:MM:SS] <msg>"
// 实证:logger_sd.c:544 strftime("%Y-%m-%d %H:%M:%S") + :570 fprintf("[%s] ",time_str)
// A(modem_mng)与 B(open_dial)的 logger_sd.c 行格式完全一致(逐字节 diff 仅管道差异)。
static bool parseSd(const std::string& line, LogLine& L) {
    if (line.size() < 22 || line[0] != '[' || line[20] != ']') return false;
    if (std::sscanf(line.c_str() + 1, "%4d-%2d-%2d %2d:%2d:%2d",
                    &L.Y, &L.Mo, &L.D, &L.h, &L.mi, &L.s) != 6) return false;
    L.ts  = line.substr(1, 19);
    L.t   = mkEpoch(L.Y, L.Mo, L.D, L.h, L.mi, L.s);
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
    int ms = 0;
    if (std::sscanf(line0.c_str(), "%4d-%2d-%2d %2d:%2d:%2d.%3d",
                    &L.Y, &L.Mo, &L.D, &L.h, &L.mi, &L.s, &ms) != 7) return false;
    if (line0[19] != '.') return false;
    L.ts  = line0.substr(0, 19);
    L.ms  = ms;
    L.t   = mkEpoch(L.Y, L.Mo, L.D, L.h, L.mi, L.s);
    L.fmt = FMT_SEAS;

    std::string line = stripAnsi(line0);   // 去掉 ESC[0m(及潜在色码)
    size_t p = 23;                          // "YYYY-MM-DD HH:MM:SS.mmm"
    while (p < line.size() && line[p] == ' ') p++;

    // [LEVEL]
    if (p < line.size() && line[p] == '[') {
        size_t e = line.find(']', p);
        if (e != std::string::npos) {
            L.level = line.substr(p + 1, e - p - 1);
            p = e + 1;
            while (p < line.size() && line[p] == ' ') p++;
        }
    }
    // func(到空格或 '(' 为止)
    size_t fs = p;
    while (p < line.size() && line[p] != ' ' && line[p] != '(') p++;
    if (p > fs) L.func = line.substr(fs, p - fs);
    while (p < line.size() && line[p] == ' ') p++;

    // (file:line)
    if (p < line.size() && line[p] == '(') {
        size_t e = line.find(')', p);
        if (e != std::string::npos) {
            std::string inside = line.substr(p + 1, e - p - 1);
            size_t c = inside.rfind(':');
            if (c != std::string::npos) {
                L.srcfile = inside.substr(0, c);
                L.srcline = std::atoi(inside.c_str() + c + 1);
            } else {
                L.srcfile = inside;
            }
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

// 未识别行粗分类,供审计页展示“漏在哪”
static std::string classifyUnparsed(const std::string& s) {
    if (s.find("+++") != std::string::npos || s.compare(0, 2, "AT") == 0) return "AT 命令/响应续行";
    if (!s.empty() && (s[0] == '+' || s[0] == '$')) return "模组 URC/响应续行";
    if (s.find("===") != std::string::npos) return "分隔/标记行(非会话头)";
    if (s.size() && (unsigned char)s[0] < 0x20) return "控制字符起始";
    return "其它(无时间戳)";
}

void parseLines(const std::vector<std::string>& raw,
                std::vector<LogLine>& out,
                std::vector<std::string>& sessions,
                ParseAudit* audit,
                const std::vector<size_t>& fileBoundaries)
{
    out.clear();
    sessions.clear();
    out.reserve(raw.size());
    std::vector<long long> restartTs;   // 重启时刻(opened 标记 + 版本横幅,末尾合并去重)
    ParseAudit ad;
    ad.rawTotal = raw.size();
    // 文件边界集合(升序下标转成 set 便于 O(1) 查):当前行下标 == 某文件起点时,
    // 禁止把它续到上一条(那属于上一个文件)。留空 = 无边界。
    size_t nextBoundaryPos = 0;   // 指向 fileBoundaries 中下一个待命中的边界

    for (size_t idx = 0; idx < raw.size(); ++idx) {
        // 是否是某个文件的起始行(多文件合并时)。是 → 本行不得续到上一条(属上一个文件)。
        bool atFileStart = false;
        while (nextBoundaryPos < fileBoundaries.size() && fileBoundaries[nextBoundaryPos] < idx)
            ++nextBoundaryPos;
        if (nextBoundaryPos < fileBoundaries.size() && fileBoundaries[nextBoundaryPos] == idx)
            atFileStart = true;

        // 去掉行尾 \r\n
        std::string line = raw[idx];
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();

        if (trim(line).empty()) { ad.blank++; continue; }

        // 会话标记(logger_sd.c:424 "=== Dial Log Opened [ts] daykey=... ===" (modem_mng)
        //           open_dial:  "=== Dial Program Started [ts] ===" (对等开场标记)
        //           logger_sd.c:527 "=== Program Exit [ts] ===")
        // 【真机实证】open_dial 日志开场是 "Dial Program Started",此前只认 "Dial Log Opened",
        //   导致 open_dial 首行被误计未识别(真机 954 行中恰 1 行)。二者语义对等:都是一次
        //   会话开始,都带时间戳、都算一次进程重启。
        bool isOpened = line.find("Dial Log Opened") != std::string::npos ||
                        line.find("Dial Program Started") != std::string::npos;
        if (line.compare(0, 3, "===") == 0 &&
            (isOpened || line.find("Program Exit") != std::string::npos)) {
            size_t a = line.find('[');
            size_t b = (a == std::string::npos) ? std::string::npos : line.find(']', a);
            if (a != std::string::npos && b != std::string::npos && b > a + 1 && isOpened) {
                std::string ts = line.substr(a + 1, b - a - 1);  // "YYYY-MM-DD HH:MM:SS"
                int Y,Mo,D,h,mi,s;
                if (std::sscanf(ts.c_str(), "%4d-%2d-%2d %2d:%2d:%2d",&Y,&Mo,&D,&h,&mi,&s)==6)
                    restartTs.push_back(mkEpoch(Y,Mo,D,h,mi,s));
            }
            ad.session++;
            continue;
        }

        LogLine L;
        L.lineNo = idx + 1;
        if (parseSd(line, L) || parseSeas(line, L)) {
            ad.parsed++;
            // 进程重启横幅(每次启动恰一次,三家措辞各异,全是正常日志行):
            //   modem_mng: "Program started. Version:" / "EG25 modem_mng Version:"
            //   open_dial: "Program started. Main Version:"
            //   artery   : "DIAL Version:"(seas_log,无 "=== Dial Log Opened ===" 标记)
            // 【真机实证】real_artery_1.29.14 重启 3 次却只有 3 条 "DIAL Version",0 条 opened 标记
            //   —— 只认 opened 标记的话 artery/AG35控制台 的重启全漏(报 0 次)。
            if (L.msg.find("DIAL Version:") != std::string::npos ||
                L.msg.find("modem_mng Version:") != std::string::npos ||
                L.msg.find("Program started. Version:") != std::string::npos ||
                L.msg.find("Program started. Main Version:") != std::string::npos)
                restartTs.push_back(L.t);
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
            continue;
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
        if (!out.empty() && !atFileStart) {
            const std::string& prev = out.back().msg;
            size_t e2 = prev.find_last_not_of(" \t");
            if (e2 != std::string::npos && prev[e2] == ':') {
                out.back().msg += " ⏎ ";
                out.back().msg += line;
                ad.continuation++;
                continue;
            }
        }

        // 未识别:计数 + 分类 + 留样(这是“没漏消息”的唯一硬证据)
        ad.unparsed++;
        ad.unparsedKinds[classifyUnparsed(line)]++;
        if (ad.samples.size() < ParseAudit::kMaxSamples)
            ad.samples.push_back(UnparsedLine{idx + 1, line.substr(0, 400)});
    }
    // 合并两种重启信号:同一次重启在 modem_mng 上既有 opened 标记又有版本横幅
    // (相隔几秒),去重防重复计数;artery/AG35控制台 只有其一。10s 内视为同一次。
    std::sort(restartTs.begin(), restartTs.end());
    long long prev = -1000000000LL;   // 不用 LLONG_MIN:t-prev 会整数溢出,第一个永远被漏掉
    for (long long t : restartTs) {
        if (t - prev > 10) sessions.push_back(fmtTime(t, "FULL"));
        prev = t;
    }

    if (audit) *audit = std::move(ad);
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
        if (parseSd(line, L) || parseSeas(line, L)) {
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
std::map<std::string, std::string> hbFields(const std::string& msg) {
    std::map<std::string, std::string> d;
    if (msg.find(':') == std::string::npos && msg.find('=') == std::string::npos) return d;

    size_t i = 0;
    while (i < msg.size()) {
        if (!isIdentChar(msg[i])) { i++; continue; }
        size_t j = i;
        while (j < msg.size() && isIdentChar(msg[j])) j++;
        if (j >= msg.size() || (msg[j] != ':' && msg[j] != '=')) { i = j; continue; }

        size_t k = j + 1;
        // 找值的终点
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
        if (e > k) d[msg.substr(i, j - i)] = trim(msg.substr(k, e - k));
        i = (e > k) ? e : j;
    }
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
        if (!ag35Ev && (l.tag == "SLOT" || l.msg.find("SLOT:") != std::string::npos)) ag35Ev = &l;
        if (!ec200Ev && l.msg.find("SIM_AT:") != std::string::npos &&
            l.msg.find("SIM_CB:") != std::string::npos) ec200Ev = &l;
        if (!eg25Ev && (l.tag == "ROAMLINK" ||
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
    if (isErrTag(l.tag)) return true;
    if (l.fmt == FMT_SEAS) {
        std::string lv = lower(l.level);
        return lv == "error" || lv == "warning" || lv == "fatal" || lv == "critical";
    }
    return false;
}

// 时间线保留:事件类标签,或 seas 的报错行(artery 大量日志无内嵌标签,
// 只按标签过滤会让 artery 的时间线几乎全空)
bool isEventLine(const LogLine& l) {
    if (isEventTag(l.tag)) return true;
    if (l.fmt == FMT_SEAS) {
        if (isErrLine(l)) return true;
        std::string lv = lower(l.level);
        return lv == "notice";
    }
    return false;
}

// ============================ 分析 ============================
std::vector<Outage> collectOutages(const std::vector<LogLine>& lines) {
    std::vector<Outage> outs;
    bool have = false;
    long long start = 0;
    size_t startLine = 0;
    for (const auto& l : lines) {
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
static const std::string* pick(const std::map<std::string,std::string>& f,
                               std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        auto it = f.find(k);
        if (it != f.end()) return &it->second;
    }
    return nullptr;
}

std::vector<MetricRow> buildMetrics(const std::vector<LogLine>& lines) {
    std::vector<MetricRow> rows;
    bool haveLastRx = false;
    long long lastRx = 0;

    for (const auto& l : lines) {
        if (l.tag.compare(0, 9, "HEARTBEAT") != 0) continue;
        auto f = hbFields(l.msg);
        if (f.empty()) continue;

        MetricRow m;
        m.t  = l.t;
        m.lineNo = l.lineNo;
        m.ts = fmtTime(l.t, "MD");

        const std::string* v;
        m.ch = "-";
        if ((v = pick(f, {"CH"})))               m.ch = *v;
        else if ((v = pick(f, {"SLOT"})))        m.ch = "SLOT" + *v;   // AG35
        else if ((v = pick(f, {"state"}))) {                            // artery
            m.ch = icontains(*v, "roamlink") ? "ROAMLINK" : "SIM";
        }
        v = pick(f, {"CSQ", "csq"});             m.csq = v ? *v : "-";
        v = pick(f, {"ConsecFail", "tcp_fail"}); m.cf  = v ? *v : "-";

        m.tmax = "-";
        if ((v = pick(f, {"Temp", "TEMP"}))) {
            int best = -1000; bool ok = false;
            std::string t = *v;
            size_t a = 0;
            while (a <= t.size()) {
                size_t b = t.find(',', a);
                std::string piece = trim(t.substr(a, (b == std::string::npos ? t.size() : b) - a));
                if (!piece.empty()) {
                    char* endp = nullptr;
                    long v = std::strtol(piece.c_str(), &endp, 10);
                    if (endp != piece.c_str()) { if ((int)v > best) best = (int)v; ok = true; }
                }
                if (b == std::string::npos) break;
                a = b + 1;
            }
            if (ok) m.tmax = std::to_string(best);
        }

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
        bool haveRx = false; long long rx = 0;
        if ((v = pick(f, {"RX_PKT", "rx_packets"}))) {
            char* endp = nullptr;
            long long r = std::strtoll(v->c_str(), &endp, 10);
            if (endp != v->c_str()) { haveRx = true; rx = r; }
        }
        m.rx = haveRx ? std::to_string(rx) : "-";
        if (haveRx && haveLastRx) {
            long long d = rx - lastRx;
            m.drx = std::to_string(d);
            m.drxZero = (d == 0);
        } else {
            m.drx = "-";
        }
        if (haveRx) { lastRx = rx; haveLastRx = true; }

        m.csqVal = -1;
        if ((v = pick(f, {"CSQ", "csq"}))) {
            char* endp = nullptr;
            long c = std::strtol(v->c_str(), &endp, 10);
            if (endp != v->c_str() && c != 99) m.csqVal = (int)c;   // 99 = AT+CSQ 未知
        }
        // RSRP/RSRQ:dBm 精确信号值(负数)。真机两种分隔 hbFields 均能切出:
        //   open_dial "RSRP:-94 | RSRQ:-18"(竖线) / modem_mng "RSRP:-104 RSRQ:-10"(空格)。
        // 只接受负值,正数视为异常(1=无效标记)。
        if ((v = pick(f, {"RSRP", "rsrp"}))) {
            char* endp = nullptr;
            long r = std::strtol(v->c_str(), &endp, 10);
            if (endp != v->c_str() && r < 0) m.rsrp = (int)r;
        }
        if ((v = pick(f, {"RSRQ", "rsrq"}))) {
            char* endp = nullptr;
            long r = std::strtol(v->c_str(), &endp, 10);
            if (endp != v->c_str() && r < 0) m.rsrq = (int)r;
        }
        rows.push_back(std::move(m));
    }
    return rows;
}

// ============================ 结论引擎 ============================
// 铁律:每条结论必须能追溯到具体日志证据(行号+时间戳),ev 为空的结论一律不输出。
// 各判据的源码出处逐条标注;凡"仅源码推断、未经真机实证"的分支已明确标注。

static Evidence mkEv(const LogLine& l) {
    Evidence e;
    e.lineNo = l.lineNo;
    e.ts     = l.ts;
    e.text   = (l.tag.empty() ? "" : "[" + l.tag + "] ") + l.msg.substr(0, 160);
    return e;
}

// 断网根因分类(取值即 Finding 的分组键)
enum Cause { C_WEAK, C_DATADEAD, C_SWITCHING, C_DENIED, C_NOTREADY, C_SDK_L0, C_UNKNOWN, C_N };
static const char* kCauseName[C_N] = {
    "弱信号", "数据假死(RX_PKT 停滞)", "切卡/选网/CFUN 期间",
    "注册被拒(SIM 或账户问题)", "数据服务未就绪",
    "SDK 短断网(链路抖动,非设备故障)", "未能归类"
};

std::vector<Finding> analyze(const std::vector<LogLine>& lines,
                             const std::vector<Outage>& outs,
                             const std::vector<MetricRow>& mets,
                             const PlatformInfo& pi,
                             const ParseAudit& audit)
{
    std::vector<Finding> fs;
    if (lines.empty()) return fs;

    // ---- 预扫:各类特征行(全部留证据指针)----
    std::vector<const LogLine*> evNeverConn, evPolicy, evRecL1, evRecL2, evRecL3,
                                evDenied, evCpdump, evSlot, evOper, evCfun, evNotReady;
    for (const auto& l : lines) {
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
        if (l.tag.compare(0, 11, "RECOVERY L1") == 0)       pushEvent(evRecL1);
        if (l.tag.compare(0, 11, "RECOVERY L2") == 0)       pushEvent(evRecL2);
        if (l.tag.compare(0, 11, "RECOVERY L3") == 0)       pushEvent(evRecL3);
        // ec200a/dial/dial.cpp:1082 "[WARNING] Registration Denied! Code %d."
        if (icontains(l.msg, "Registration Denied"))        evDenied.push_back(&l);
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
        if (l.tag == "CPDUMP" && icontains(l.msg, "existing CP dump") &&
            !icontains(l.msg, "No existing"))                evCpdump.push_back(&l);
        if (l.tag == "SLOT")                                evSlot.push_back(&l);
        if (l.tag == "OPER")                                evOper.push_back(&l);
        if (l.tag == "CFUN")                                evCfun.push_back(&l);
    }

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

    // ---- 2. 注册被拒 ----
    if (!evDenied.empty()) {
        Finding f;
        f.severity = 2;
        f.title  = "网络注册被拒绝(Registration Denied)";
        f.detail = "REG 状态为 3(拒绝)。运营商侧拒绝该 IMSI 接入,属 SIM/账户/位置区限制,"
                   "不是信号问题。";
        f.advice = "核对卡状态(欠费/停机/未开通漫游或数据)、IMSI 与运营商签约是否一致;"
                   "海外场景确认是否需要选网(COPS)。";
        for (size_t i = 0; i < evDenied.size() && i < 3; ++i) f.ev.push_back(mkEv(*evDenied[i]));
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
    //   注册被拒   :窗口内出现 Registration Denied
    size_t causeCnt[C_N] = {0};
    std::vector<Evidence> causeEv[C_N];
    for (const auto& o : outs) {
        long long lo = o.start - 90, hi = o.recovered ? o.end : lines.back().t;
        int  minCsq = 999; bool sawZeroRx = false;
        int  minRsrp = 9999;                              // 窗口内最低 RSRP(dBm,越低越差)
        const MetricRow* mZero = nullptr; const MetricRow* mWeak = nullptr;
        const MetricRow* mRsrp = nullptr;
        for (const auto& m : mets) {
            if (m.t < lo || m.t > hi) continue;
            if (m.csqVal >= 0 && m.csqVal < minCsq) { minCsq = m.csqVal; mWeak = &m; }
            if (m.rsrp < 0 && m.rsrp < minRsrp) { minRsrp = m.rsrp; mRsrp = &m; }
            if (m.drxZero) { sawZeroRx = true; if (!mZero) mZero = &m; }
        }
        // RSRP < -110 dBm = 3GPP 极差覆盖(基本不可用)。比 CS<10 更灵敏:
        // CSQ 是 0-31 粗档,可能读到中间值,而 RSRP 已探底 —— 覆盖问题此时才现形。
        bool weakByRsrp = (minRsrp <= -110);
        bool weakByCsq  = (minCsq < 10 && mWeak);
        const LogLine* sw = nullptr;
        for (const auto* v : { &evSlot, &evOper, &evCfun })
            for (const auto* l : *v)
                if (l->t >= lo && l->t <= hi) { sw = l; break; }
        const LogLine* dn = nullptr;
        for (const auto* l : evDenied) if (l->t >= lo && l->t <= hi) { dn = l; break; }
        const LogLine* nr = nullptr;
        for (const auto* l : evNotReady) if (l->t >= lo && l->t <= hi) { nr = l; break; }

        Cause c = C_UNKNOWN;
        const LogLine* evl = nullptr;
        const MetricRow* evm = nullptr;
        if (dn)                          { c = C_DENIED;    evl = dn; }
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
                e.lineNo = evm->lineNo; e.ts = evm->ts;
                std::string sig = "心跳 CSQ=" + evm->csq;
                if (evm->rsrp < 0) sig += " RSRP=" + std::to_string(evm->rsrp) + "dBm";
                sig += " ΔRX=" + evm->drx;
                e.text = sig + " (断网 " + fmtTime(o.start, "MD") + " 起)";
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
            f.detail = "断网窗口内心跳 CSQ 最小值 < 10(≈RSSI<-95dBm),或 RSRP ≤ -110dBm"
                       "(3GPP 极差覆盖),信号覆盖不足。";
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
            f.detail = "断网窗口内出现 Registration Denied。";
            f.advice = "按 SIM/账户问题处理,见上方结论。";
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
            if (m.tmax == "-") continue;
            int v = std::atoi(m.tmax.c_str());
            if (v > best) { best = v; hot = &m; }
        }
        if (hot && best >= 75) {
            Finding f;
            f.severity = 1;
            f.title  = "模组/CPU 温度偏高:峰值 " + std::to_string(best) + "℃";
            f.detail = "高温会导致射频性能下降甚至模组保护性降频。(75℃ 为经验提示阈值,非源码常量)";
            f.advice = "查散热与安装环境;若高温与断网时间吻合,优先排散热。";
            Evidence e; e.lineNo = hot->lineNo; e.ts = hot->ts;
            e.text = "心跳温度峰值 " + hot->tmax + "℃";
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
            if (m.rsrp < 0) { sum += m.rsrp; n++; if (m.rsrp < worst) { worst = m.rsrp; mWorst = &m; } }
        }
        if (n >= 5) {                              // 需足够样本才下结论,不臆测
            int avg = (int)(sum / n);
            if (avg <= -100 && mWorst) {           // 均值 ≤ -100(3GPP"较差"以下)判长期劣化
                Finding f;
                f.severity = 1;
                f.title  = "信号质量长期偏低:RSRP 均值 " + std::to_string(avg) + " dBm(共 " +
                           std::to_string(n) + " 样本)";
                f.detail = "RSRP 均值处于 3GPP\"较差\"档(≤-100dBm),最低 " + std::to_string(worst) +
                           " dBm。设备长期处于覆盖边缘,非偶发 —— 断网/低速大概率与此相关。";
                f.advice = "系统性排查:天线选型/安装位置/朝向、是否室内深处或金属屏蔽;"
                           "必要时加装外置天线或选覆盖更好的运营商。";
                Evidence e; e.lineNo = mWorst->lineNo; e.ts = mWorst->ts;
                e.text = "最低 RSRP=" + std::to_string(worst) + "dBm (均值 " + std::to_string(avg) + "dBm)";
                f.ev.push_back(e);
                fs.push_back(std::move(f));
            }
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

// ============================ 过滤 ============================
static bool parseBound(const std::string& s, const LogLine& base, long long& out) {
    std::string t = trim(s);
    if (t.empty()) return false;

    // 含 '-' 视为 "MM-DD HH:MM"(年份取日志基准行的年)
    if (t.find('-') != std::string::npos) {
        int M = 0, D = 0, h = 0, mi = 0;
        if (std::sscanf(t.c_str(), "%d-%d %d:%d", &M, &D, &h, &mi) == 4) {
            out = mkEpoch(base.Y, M, D, h, mi, 0);
            return true;
        }
        return false;
    }

    // "HH:MM:SS" / "HH:MM" —— 贴到基准行那一天
    int h = 0, mi = 0, sec = 0;
    int n = std::sscanf(t.c_str(), "%d:%d:%d", &h, &mi, &sec);
    if (n >= 2) {
        out = mkEpoch(base.Y, base.Mo, base.D, h, mi, (n == 3 ? sec : 0));
        return true;
    }
    return false;
}

std::vector<LogLine> applyFilters(const std::vector<LogLine>& lines,
                                  const std::string& tag, const std::string& grep,
                                  const std::string& since, const std::string& until,
                                  bool* grepBad)
{
    if (grepBad) *grepBad = false;
    std::vector<LogLine> cur(lines.begin(), lines.end());

    // 标签
    std::string tg = trim(tag);
    if (!tg.empty()) {
        std::vector<std::string> want;
        size_t a = 0;
        while (a <= tg.size()) {
            size_t b = tg.find(',', a);
            std::string piece = trim(tg.substr(a, (b == std::string::npos ? tg.size() : b) - a));
            if (!piece.empty()) want.push_back(lower(piece));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (!want.empty()) {
            std::vector<LogLine> next;
            for (const auto& l : cur) {
                std::string lt = lower(l.tag);
                std::string first = lt;
                size_t sp = lt.find(' ');
                if (sp != std::string::npos) first = lt.substr(0, sp);
                if (std::find(want.begin(), want.end(), lt) != want.end() ||
                    std::find(want.begin(), want.end(), first) != want.end())
                    next.push_back(l);
            }
            cur.swap(next);
        }
    }

    // 正则
    std::string gp = trim(grep);
    if (!gp.empty()) {
        try {
            std::regex rx(gp, std::regex::icase);
            std::vector<LogLine> next;
            for (const auto& l : cur)
                if (std::regex_search(l.msg, rx)) next.push_back(l);
            cur.swap(next);
        } catch (const std::regex_error&) {
            if (grepBad) *grepBad = true;   // 非法正则:忽略该条件,由调用方提示
        }
    }

    // 时间段(以筛选后首行的年月日为基准补齐)
    if ((!trim(since).empty() || !trim(until).empty()) && !cur.empty()) {
        LogLine base = cur.front();
        long long lo = 0, hi = 0;
        bool hasLo = parseBound(since, base, lo);
        bool hasHi = parseBound(until, base, hi);
        if (hasLo || hasHi) {
            std::vector<LogLine> next;
            for (const auto& l : cur) {
                if (hasLo && l.t < lo) continue;
                if (hasHi && l.t > hi) continue;
                next.push_back(l);
            }
            cur.swap(next);
        }
    }
    return cur;
}

// ============================ BOM 剥离 / 压缩包直读 ============================
void stripBom(std::string& buf) {
    if (buf.size() >= 3 &&
        (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        buf.erase(0, 3);
    }
}

ArchiveKind archiveKindOf(const std::string& buf) {
    if (buf.size() >= 2 &&
        (unsigned char)buf[0] == 0x1F && (unsigned char)buf[1] == 0x8B) return ARC_GZIP;
    if (buf.size() >= 4 &&
        buf[0] == 'P' && buf[1] == 'K' &&
        (unsigned char)buf[2] == 0x03 && (unsigned char)buf[3] == 0x04) return ARC_ZIP;
    return ARC_NONE;
}

// ---- tar 拆分(gzip 解出来若是 tar,再拆成多条目)----
// tar 是 512 字节块:每个文件一个 512 头(name[0..99], size 在 124..135 八进制),
// 紧跟 ceil(size/512) 个数据块。两个全零块表示结束。只取普通文件(typeflag '0' 或 '\0')。
// 仅在编入 miniz 时才需要(只被 gzip 分支调用);不定义 DL_HAVE_MINIZ 时不编译,
// 避免 -Wunused-function(本项目 -Wall -Wextra 零告警)。
#ifdef DL_HAVE_MINIZ
static bool looksLikeTar(const std::string& d) {
    // POSIX tar 在偏移 257 有 "ustar" 魔数;GNU tar 也是。老式 v7 tar 无魔数,
    // 这里用魔数做主判据(可靠),避免把普通文本误判成 tar。
    return d.size() >= 262 && std::memcmp(d.data() + 257, "ustar", 5) == 0;
}

static bool splitTar(const std::string& d, std::vector<ArchiveEntry>& out) {
    size_t off = 0;
    while (off + 512 <= d.size()) {
        const char* h = d.data() + off;
        // 全零块 = 结束
        bool allZero = true;
        for (int i = 0; i < 512; ++i) if (h[i]) { allZero = false; break; }
        if (allZero) break;

        // 文件名(可能不足 100 字节,以 NUL 结尾)
        size_t nameLen = 0;
        while (nameLen < 100 && h[nameLen]) nameLen++;
        std::string name(h, nameLen);

        // 大小:偏移 124,11 位八进制 + 可能的空格/NUL
        char szbuf[13] = {0};
        std::memcpy(szbuf, h + 124, 12);
        long long fsize = std::strtoll(szbuf, nullptr, 8);
        if (fsize < 0) return false;

        char typeflag = h[156];
        off += 512;   // 跳过头
        if (off + (size_t)fsize > d.size()) break;   // 数据不完整,停

        // 普通文件('0' 或 '\0');目录('5')/其它类型跳过数据
        if (typeflag == '0' || typeflag == '\0') {
            if (fsize > 0) out.push_back(ArchiveEntry{ name, d.substr(off, (size_t)fsize) });
        }
        // 跳到下一个 512 对齐
        off += ((size_t)fsize + 511) & ~((size_t)511);
    }
    return true;
}
#endif  // DL_HAVE_MINIZ (tar helpers)

bool extractArchive(const std::string& buf, std::vector<ArchiveEntry>& entries, std::string& err) {
    ArchiveKind k = archiveKindOf(buf);
    if (k == ARC_NONE) { err = "不是已知压缩格式"; return false; }

#ifdef DL_HAVE_MINIZ
    if (k == ARC_GZIP) {
        // gzip 解压:miniz 的 tinfl 只做裸 DEFLATE,gzip 需先跳过头、末尾无 adler。
        // 用 mz_inflate 走 raw deflate,gzip 头(10字节固定 + 可选字段)手工跳过。
        if (buf.size() < 18) { err = "gzip 数据过短"; return false; }
        size_t p = 10;
        unsigned char flg = (unsigned char)buf[3];
        if (flg & 0x04) {   // FEXTRA
            if (p + 2 > buf.size()) { err = "gzip FEXTRA 越界"; return false; }
            unsigned xlen = (unsigned char)buf[p] | ((unsigned char)buf[p+1] << 8);
            p += 2 + xlen;
        }
        if (flg & 0x08) { while (p < buf.size() && buf[p]) p++; p++; }   // FNAME
        if (flg & 0x10) { while (p < buf.size() && buf[p]) p++; p++; }   // FCOMMENT
        if (flg & 0x02) p += 2;                                          // FHCRC
        if (p >= buf.size()) { err = "gzip 头解析越界"; return false; }

        // gzip 末 8 字节是 CRC32 + ISIZE;ISIZE 给出原始大小,预分配。
        uint32_t isize = (unsigned char)buf[buf.size()-4] |
                         ((unsigned char)buf[buf.size()-3] << 8) |
                         ((unsigned char)buf[buf.size()-2] << 16) |
                         ((uint32_t)(unsigned char)buf[buf.size()-1] << 24);
        std::string out;
        size_t cap = isize ? isize : (buf.size() * 4 + 1024);
        out.resize(cap);

        mz_stream s; std::memset(&s, 0, sizeof(s));
        if (mz_inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) { err = "inflate 初始化失败"; return false; }
        s.next_in = (const unsigned char*)buf.data() + p;
        s.avail_in = (unsigned)(buf.size() - p - 8);
        s.next_out = (unsigned char*)&out[0];
        s.avail_out = (unsigned)out.size();
        int r = mz_inflate(&s, MZ_FINISH);
        if (r != MZ_STREAM_END && r != MZ_OK) {
            // 输出缓冲可能不够(isize 是 mod 2^32,超 4GB 才会错;这里日志远小于此)
            mz_inflateEnd(&s); err = "gzip 解压失败"; return false;
        }
        out.resize(s.total_out);
        mz_inflateEnd(&s);

        if (looksLikeTar(out)) {
            if (!splitTar(out, entries) || entries.empty()) { err = "tar 拆分为空"; return false; }
        } else {
            entries.push_back(ArchiveEntry{ "", std::move(out) });
        }
        return true;
    }

    if (k == ARC_ZIP) {
        mz_zip_archive z; mz_zip_zero_struct(&z);
        if (!mz_zip_reader_init_mem(&z, buf.data(), buf.size(), 0)) { err = "zip 打开失败"; return false; }
        mz_uint n = mz_zip_reader_get_num_files(&z);
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            if (mz_zip_reader_is_file_a_directory(&z, i)) continue;
            size_t outSz = 0;
            void* p = mz_zip_reader_extract_to_heap(&z, i, &outSz, 0);
            if (!p) continue;
            entries.push_back(ArchiveEntry{ st.m_filename, std::string((char*)p, outSz) });
            mz_free(p);
        }
        mz_zip_reader_end(&z);
        if (entries.empty()) { err = "zip 内无可读文件"; return false; }
        return true;
    }
    err = "未支持的压缩格式";
    return false;
#else
    (void)entries;
    err = "本次构建未编入解压支持(DL_HAVE_MINIZ 未定义)";
    return false;
#endif
}

} // namespace dl
