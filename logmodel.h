// logmodel.h — modem_mng / open_dial / open_dial_for_artery 拨号日志解析·分析·结论层
// 纯标准 C++17,不含任何 Win32 依赖,便于单独测试(make selftest)。
//
// 支持两种行格式(均由源码实证,见 logmodel.cpp 各处的 文件:行号 注释):
//   FMT_SD   : "[YYYY-MM-DD HH:MM:SS] [TAG] message"      logger_sd.c dial_log()
//   FMT_SEAS : "YYYY-MM-DD HH:MM:SS.mmm [LEVEL] <ESC>[0m func (file:line) - message"
//              open_dial_for_artery/src/seas_log/seas_log.c seas_emit_log()
#pragma once
#include <string>
#include <vector>
#include <map>

namespace dl {

// 行格式
enum Fmt { FMT_UNKNOWN = 0, FMT_SD, FMT_SEAS };

// 日志来源平台
enum Platform {
    PLAT_UNKNOWN = 0,
    PLAT_EC200A,    // modem_mng EC200A 或 open_dial(上游,格式相同)
    PLAT_AG35,      // modem_mng AG35(EC200A 源码路径 + [SLOT] 双卡)
    PLAT_EG25,      // modem_mng EG25
    PLAT_ARTERY,    // open_dial_for_artery(seas_log)
    PLAT_IMX        // 仅占位:IMX 无 dial_log,不产此类日志(见 logmodel.cpp 说明)
};

// 一条已解析的日志行
struct LogLine {
    long long   t = 0;      // epoch 秒(按字面时间解释,不做时区换算)
    int Y=0, Mo=0, D=0, h=0, mi=0, s=0;
    int  ms = -1;           // 毫秒;仅 FMT_SEAS 有,-1=无
    std::string ts;         // 原始时间戳 "YYYY-MM-DD HH:MM:SS"
    std::string tag;        // 首个 [TAG] 内容(可能含空格,如 "CELL CHANGE"/"RECOVERY L1")
    std::string msg;        // 标签之后的正文(UTF-8,已剥离 ANSI 转义码)
    // 以下仅 FMT_SEAS 有值
    std::string level;      // INFO / ERROR / WARNING / NOTICE / ALL / FATAL(不含方括号)
    std::string func;       // 打日志的函数名
    std::string srcfile;    // 源文件名
    int  srcline = 0;       // 源码行号
    Fmt  fmt = FMT_UNKNOWN;
    size_t lineNo = 0;      // 原始文件行号(1 基),供结论证据溯源
};

// 未识别行(审计用)
struct UnparsedLine {
    size_t lineNo = 0;
    std::string text;
};

// 解析审计:证明"没漏消息"的硬证据。rawTotal = parsed+session+blank+unparsed
struct ParseAudit {
    size_t rawTotal  = 0;
    size_t parsed    = 0;   // 成功解析为 LogLine
    size_t session   = 0;   // "=== Dial Log Opened/Program Exit ===" 会话标记
    size_t blank     = 0;   // 空行/纯空白
    size_t unparsed  = 0;   // 未识别 ← 审计目标
    std::vector<UnparsedLine> samples;              // 未识别样例(上限 kMaxSamples)
    std::map<std::string, size_t> unparsedKinds;    // 未识别行的粗分类 → 计数
    static const size_t kMaxSamples = 200;
    double unparsedRatio() const {                  // 占原始行比例(0..1)
        return rawTotal ? double(unparsed) / double(rawTotal) : 0.0;
    }
};

// 平台识别结果
struct PlatformInfo {
    Platform plat = PLAT_UNKNOWN;
    std::string name;       // 展示名,如 "EG25 (modem_mng)"
    std::string evidence;   // 判定依据(实证的日志片段)
    size_t evidenceLine = 0;
};

// 一次断网。recovered=false 表示日志结束时仍未恢复(end/dur 无效)
struct Outage {
    long long start = 0;
    long long end   = 0;
    int  dur        = 0;
    bool recovered  = false;
    size_t startLine = 0, endLine = 0;   // 证据行号
};

// 一段 RX_PKT 停滞(数据链路假死征兆)
struct Stall {
    long long start = 0, end = 0, dur = 0;
    size_t startLine = 0, endLine = 0;
};

// 心跳指标行(供“指标”页)
struct MetricRow {
    long long t = 0;
    std::string ts, ch, csq, tmax, cf, rx, drx;
    int  csqVal  = -1;      // -1=无效/99
    bool drxZero = false;   // ΔRX == 0 → 数据不通征兆
    size_t lineNo = 0;
};

// ---- 结论引擎(第 4 节)----
struct Evidence {
    size_t lineNo = 0;      // 原始行号
    std::string ts;         // 时间戳
    std::string text;       // 证据原文(截断)
};
struct Finding {
    int severity = 0;               // 0=信息 1=告警 2=严重
    std::string title;              // 结论
    std::string detail;             // 依据说明
    std::string advice;             // 处置建议
    std::vector<Evidence> ev;       // 支撑证据(无证据不得输出)
};

// ---- 解析 ----
// 返回解析出的行;sessions 收集会话标记时间戳(进程重启次数);audit 给出未识别行审计。
void parseLines(const std::vector<std::string>& raw,
                std::vector<LogLine>& out,
                std::vector<std::string>& sessions,
                ParseAudit* audit = nullptr);

// 心跳/诊断字段解析:同时支持 "K:V" 与 "K=V",'|' 与空格分隔;非字段行返回空 map
std::map<std::string, std::string> hbFields(const std::string& msg);

// 剥离 ANSI 转义码(CSI 序列)
std::string stripAnsi(const std::string& s);

// 平台识别(基于已解析行的实证特征)
PlatformInfo detectPlatform(const std::vector<LogLine>& lines);

// ---- 分析 ----
std::vector<Outage> collectOutages(const std::vector<LogLine>& lines);
std::vector<Stall>  detectRxStall(const std::vector<std::pair<long long,long long>>& rxs,
                                  long long minStallSec = 120);
std::vector<MetricRow> buildMetrics(const std::vector<LogLine>& lines);

// 结论引擎:每条结论必须带证据(ev 非空),否则不输出
std::vector<Finding> analyze(const std::vector<LogLine>& lines,
                             const std::vector<Outage>& outs,
                             const std::vector<MetricRow>& mets,
                             const PlatformInfo& pi,
                             const ParseAudit& audit);

// ---- 过滤 ----
// tag: "A,B" 逗号分隔; grep: 正则(忽略大小写); since/until: "HH:MM[:SS]" 或 "MM-DD HH:MM"
// grepBad 置为 true 表示正则非法(调用方可提示)
std::vector<LogLine> applyFilters(const std::vector<LogLine>& lines,
                                  const std::string& tag, const std::string& grep,
                                  const std::string& since, const std::string& until,
                                  bool* grepBad = nullptr);

// ---- 判定/工具 ----
bool isFaultStart(const std::string& msg);          // "Ping failed ... fault timer started"
bool isRecovered(const std::string& msg, int* durSec); // "Network recovered after Ns"
bool isEventTag(const std::string& tag);            // 状态变化类标签(时间线保留)
bool isErrTag(const std::string& tag);              // ERROR/WARN/FATAL/ALARM
// 整行级判定:除标签外还认 seas_log 的级别字段(artery 的报错不带 [TAG],
// 严重度在 level 里,只看 tag 会把 [ERROR] 行漏掉)
bool isErrLine(const LogLine& l);
bool isEventLine(const LogLine& l);

std::string fmtDur(long long sec);                  // 42s / 3m20s / 1h05m
std::string fmtTime(long long epoch, const char* fmt); // fmt: "MD" -> MM-DD HH:MM:SS, "HM" -> HH:MM, "FULL"
long long   mkEpoch(int Y, int Mo, int D, int h, int mi, int s);

} // namespace dl
