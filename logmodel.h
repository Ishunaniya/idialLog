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

// 解析审计:证明"没漏消息"的硬证据。
// 自洽等式:rawTotal = parsed + session + blank + continuation + unparsed
struct ParseAudit {
    size_t rawTotal      = 0;
    size_t parsed        = 0;   // 成功解析为 LogLine
    size_t session       = 0;   // "=== Dial Log Opened/Program Exit ===" 会话标记
    size_t blank         = 0;   // 空行/纯空白
    size_t continuation  = 0;   // 多行条目的续行(无时间戳,已并入上一条;非丢弃)
    size_t unparsed      = 0;   // 未识别 ← 审计目标
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

// ---- 多文件合并定序 ----
// 取一批原始行中**第一条带时间戳的行**的 epoch 秒。只扫前 scanLimit 行
// (日志文件头部必有时间戳,不必整份扫)。认三种来源:
//   "=== Dial Log Opened [ts] ... ===" 会话标记(FMT_SD 文件首行常是它,logger_sd.c:424)
//   FMT_SD / FMT_SEAS 的普通行
// 返回 false = 扫不到(整批都是噪声,或不是本工具支持的格式);此时 *t 不被写入。
bool firstTimestamp(const std::vector<std::string>& raw, long long* t, size_t scanLimit = 200);

// 多份日志拼接前定序:返回应当拼接的下标顺序(chunks 的下标)。
// 规则:有首时间戳的按时间升序在前;首时间戳相同的保持输入顺序(stable);
//       扫不到时间戳的一律排在最后,保持输入顺序。
// 为什么需要:拖入顺序(资源管理器多选)与文件对话框返回顺序**都不保证按时间**,
// 而 parseLines 全程不排序 —— v1.3.0 及之前直接顺序拼接,跨天多份日志一旦拖乱,
// 时间线、断网次数/时长、可用率会全部算错。这是正确性问题,不是显示问题。
std::vector<size_t> orderByTime(const std::vector<std::vector<std::string>>& chunks);

// ---- 时基(time base)判定:防止墙钟日志与"时钟未同步"日志混算 ----
// 设备 RTC 未授时时,首时间戳停在 epoch 起点(1970),此后是开机 uptime —— 单调递增、
// 秒差有效,所以**单份**未同步日志算断网/可用率是对的;但它与墙钟日志**不在同一坐标系**,
// 一旦混合拼接,跨度会被撑成几十年(实测 495212h),可用率从"差"翻成"良好",边界处还
// 多算一次假断网。这不是显示问题,是把两把不同的尺当同一把用。
enum TimeBase {
    TB_NONE = 0,   // 扫不到首时间戳(无法判定)
    TB_WALL,       // 真实墙钟(年份 >= 2000)
    TB_UNSYNCED    // 时钟未同步(年份 < 2000,RTC 未授时的 epoch/uptime)
};

// 判定一批原始行属于哪种时基。判据:首时间戳年份 —— 年份 < 2000 视为未同步。
// 年份是物理事实(嵌入设备墙钟不可能落在 1970s),跨 FMT_SD/FMT_SEAS 通用;
// daykey=unsynced 只作佐证,不作唯一判据(某些固件版本可能不写该字段)。
TimeBase timeBaseOf(const std::vector<std::string>& raw);

// 跨时基混合检测结果:同时含 TB_WALL 与 TB_UNSYNCED 时 mixed=true。
// 方案 A(拒绝混合):合并时**排除**未同步那批,只用墙钟批出结论,并把被排除的
// 下标交给调用方去提示用户(列出文件名/份数)。未同步批未被销毁,可单独再分析。
struct MixReport {
    bool mixed = false;                 // 是否检测到跨时基混合
    std::vector<size_t> wallIdx;        // 墙钟批的下标(保留参与合并)
    std::vector<size_t> unsyncedIdx;    // 未同步批的下标(方案A下被排除)
    std::vector<size_t> noneIdx;        // 扫不到时间戳的下标
};
MixReport detectMix(const std::vector<std::vector<std::string>>& chunks);

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
