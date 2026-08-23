// log_parser.h — 日志解析、增量状态机与多来源定序 API
#pragma once

#include "log_types.h"

namespace dl {

// ---- 解析 ----
// 返回解析出的行；sessions 只收集有版本横幅/Dial Program Started 支撑的进程启动时间。
// Dial Log Opened 可能只是跨日/授时轮转，单独计入 audit.logOpened，不再冒充进程重启。
// fileBoundaries: 多文件合并时各文件在 raw 中的起始下标(升序)。用于阻止**跨文件续行**——
//   若 A 文件末行以冒号结尾、B 文件首行无时间戳,续行逻辑会把 B 首行误并入 A 末条(串味)。
//   真机 72 种拼接组合虽未触发(日志通常正常结束),但机制真实,此为零风险防御。
//   留空 = 单文件/剪贴板,无边界约束(行为与旧版完全一致)。
class StreamingLogParser {
public:
    StreamingLogParser(std::vector<LogLine>& out,
                       std::vector<std::string>& sessions,
                       size_t reserveHint = 0);
    ~StreamingLogParser();
    StreamingLogParser(const StreamingLogParser&) = delete;
    StreamingLogParser& operator=(const StreamingLogParser&) = delete;
    StreamingLogParser(StreamingLogParser&&) noexcept;
    StreamingLogParser& operator=(StreamingLogParser&&) noexcept;

    // 在下一行前建立文件边界,阻止该行被并入上一文件末行的续行。
    void beginFile();
    // 接受一行原始文本；允许带行尾 CR/LF。调用后不再保留参数内容。
    void pushLine(std::string line);
    // 完成会话去重并取回审计。finish 后不再接受新行。
    void finish(ParseAudit* audit = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void parseLines(const std::vector<std::string>& raw,
                std::vector<LogLine>& out,
                std::vector<std::string>& sessions,
                ParseAudit* audit = nullptr,
                const std::vector<size_t>& fileBoundaries = {});

// ---- 多文件合并定序 ----
// 取一批原始行中**第一条带时间戳的行**的 epoch 秒。只扫前 scanLimit 行
// (日志文件头部必有时间戳,不必整份扫)。认三种来源:
//   "=== Dial Log Opened [ts] ... ===" 会话标记(FMT_SD 文件首行常是它,logger_sd.c:424)
//   FMT_SD / FMT_SEAS / FMT_ANDROID / FMT_SYSLOG 的普通行
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
// 剥离 ANSI 转义码(CSI 序列)
std::string stripAnsi(const std::string& s);

} // namespace dl
