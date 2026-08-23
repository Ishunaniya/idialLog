// log_types.h — 日志核心数据类型及所有权约束
#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dl {

// 行格式
enum Fmt : unsigned char {
    FMT_UNKNOWN = 0, FMT_SD, FMT_SEAS, FMT_ANDROID, FMT_SYSLOG, FMT_CONSOLE
};

// seas_log 级别来自固定枚举,不必让每一行都常驻一个 32B std::string。
enum LogLevel : unsigned char {
    LEVEL_NONE = 0, LEVEL_ALL, LEVEL_DEBUG, LEVEL_INFO, LEVEL_NOTICE,
    LEVEL_WARNING, LEVEL_ERROR, LEVEL_FATAL, LEVEL_CRITICAL, LEVEL_OTHER
};

// 日志来源平台
enum Platform {
    PLAT_UNKNOWN = 0,
    PLAT_EC200A,    // modem_mng EC200A 或 open_dial(上游,格式相同)
    PLAT_AG35,      // modem_mng AG35(EC200A 源码路径 + [SLOT] 双卡)
    PLAT_EG25,      // modem_mng EG25
    PLAT_ARTERY,    // open_dial_for_artery(seas_log)
    PLAT_IMX,       // 仅占位:IMX 无 dial_log,不产此类日志(见 log_analysis.cpp 说明)
    // RK3576 重构版使用 syslog/控制台文本，状态机和恢复策略均不同于旧 modem_mng；
    // 单列平台，避免把 v2 的事件套入旧 EG25/EC200A 恢复阶梯阈值。
    PLAT_MODEM_MNG_V2
};

// 一条已解析的日志行
struct LogLine {
    long long   t = 0;      // epoch 秒(按字面时间解释,不做时区换算)
    size_t lineNo = 0;      // 原始文件行号(1 基),供结论证据溯源
    std::string ts;         // 原始时间戳 "YYYY-MM-DD HH:MM:SS"
    std::string msg;        // 标签之后的正文(UTF-8,已剥离 ANSI 转义码)
    // 常见标签存字典 ID；只有未知/新标签才分配字符串。不会使用进程级永久池，关闭日志
    // 后自定义标签照常释放。func/srcfile/srcline 解析后从未被消费,不再逐行保存。
    std::unique_ptr<std::string> customTag;
    int  ms = -1;           // 毫秒;SEAS/Android/syslog 可有,-1=无
    std::uint16_t tagId = 0;
    std::uint16_t sourceId = 0; // 合并来源编号；防止 Cell ID 等状态跨文件串联
    Fmt  fmt = FMT_UNKNOWN;
    LogLevel level = LEVEL_NONE;
    bool inferredTime = false; // 时间含推定成分：RFC3164 缺年份，或裸控制台沿用相邻时间

    LogLine() = default;
    ~LogLine() = default;
    LogLine(const LogLine& other);
    LogLine& operator=(const LogLine& other);
    LogLine(LogLine&&) noexcept = default;
    LogLine& operator=(LogLine&&) noexcept = default;

    void setTag(std::string tag);
    const std::string& tagText() const;
    void setLevel(const std::string& text);
    const std::string& levelText() const;
};

// 筛选后的轻量视图:只保存指向原始 LogLine 的指针,不复制时间戳/标签/正文。
// 所有指针只在来源 vector<LogLine> 未清空、未增删、未触发重新分配时有效。
using LogView = std::vector<const LogLine*>;

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
    size_t logOpened     = 0;   // Dial Log Opened：日志文件打开/轮转，不等价于进程启动
    size_t programStarted = 0;  // 版本横幅或 Dial Program Started 提供的启动证据（原始信号数）
    size_t programExited = 0;   // Program Exit 正常退出标记
    size_t nulBytes      = 0;   // 输入中的 NUL 字节；文本解析成功也必须单独暴露完整性损伤
    size_t nulLines      = 0;   // 含 NUL 的逻辑行数
    // 时钟跳变检测(问题①):一份日志内部时间戳大幅跳跃 —— 通常是设备开机 RTC 未授时
    // (1970 起点)后中途联网授时,时间从 1970 跳到真实年份。此时该日志的时间轴前后
    // 不在同一坐标系,断网时长/可用率跨越跳变点会算错。
    // 【无真机样本】现有 33 份夹具无一含此跳变(两份 unsynced 全程 1970,设备整段未授时)。
    // 故此处**只检测并报告,不臆测正确行为**(修复需知道正确时间,只能靠猜)——与"未识别行
    // 审计"同路子:诚实暴露异常,由用户判断,不假装解决。
    bool   clockJump     = false;  // 是否检测到跳变
    long long jumpFromT  = 0;      // 跳变前一行的时间
    long long jumpToT    = 0;      // 跳变后一行的时间
    size_t jumpAtLine    = 0;      // 跳变发生的原始行号(1-based)
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
    bool l0Recovered = false;            // true=SDK 在 L0 阶段自愈(短断网,链路抖动);
                                         // false=走了 L1+ 恢复阶梯或普通恢复
    size_t startLine = 0, endLine = 0;   // 证据行号
};

// 多来源日志的实际观测窗口。calendarSpan 只描述首尾日历跨度；observedSpan 为
// 各 source 内首末有效时间跨度之和，绝不跨文件间的无日志空档。
struct ObservationStats {
    long long calendarSpan = 0;
    long long observedSpan = 0;
    std::size_t sourceCount = 0;
    std::size_t clockDiscontinuities = 0; // 同一 source 内跨 2000 年边界，观测区间在此切断
    double coveragePercent = 0.0;
};

// 一段 RX_PKT 停滞(数据链路假死征兆)
struct Stall {
    long long start = 0, end = 0, dur = 0;
    size_t startLine = 0, endLine = 0;
};

// 小区 ID 是高频短字段；用定长内联文本避免每个指标样本再常驻一个 32B std::string。
template <std::size_t Capacity>
struct SmallText {
    char data[Capacity]{};

    void assign(std::string_view value) {
        const std::size_t count = value.size() < Capacity - 1 ? value.size() : Capacity - 1;
        for (std::size_t index = 0; index < count; ++index) data[index] = value[index];
        data[count] = '\0';
    }
    SmallText& operator=(std::string_view value) { assign(value); return *this; }
    SmallText& operator=(const char* value) { assign(value ? std::string_view(value) : std::string_view{}); return *this; }
    bool empty() const { return data[0] == '\0'; }
    std::string str() const { return data; }
    std::string_view view() const { return data; }
    bool operator==(std::string_view value) const { return view() == value; }
    bool operator==(const SmallText& value) const { return view() == value.view(); }
};

// 心跳指标行(供“指标”页)
struct MetricRow {
    long long t = 0;
    long long rx = LLONG_MIN;   // LLONG_MIN=无样本；允许 0
    long long drx = LLONG_MIN;  // LLONG_MIN=首样本/无样本；允许负增量(计数器重置)
    size_t lineNo = 0;
    // CH/RAT/OPER 不是可靠的固定枚举，保留原文；Cell ID 内联，PCI/TAC 数值化。
    std::string ch, rat, oper;
    SmallText<12> cellId;       // LTE ECI / NR NCI，最多保留 11 个原始字符
    int pci = -1;               // 物理小区 ID；-1=无效
    std::uint32_t tac = UINT32_MAX; // 跟踪区码（按十六进制展示）
    std::uint8_t tacDigits = 0; // 保留 TAC 的前导零位数
    int  csqRaw  = -1;      // 原始数值；99=AT+CSQ 未知，-1=缺失/非法
    int  csqVal  = -1;      // -1=无效/99
    int  tempMax = INT_MIN; // 多温度字段最大值；INT_MIN=无效
    int  consecFail = INT_MIN;
    int  rsrp    = 1;       // dBm,负值(约-70~-120,越大越好);1=无效(正数不可能是真值)
    int  rsrq    = 1;       // dB,负值(约-3~-20);1=无效
    int  snr10   = 100000;  // SDK 原值,单位 0.1dB;100000=无效(超出 int16_t 范围)
    int  rssiVal = 1;       // dBm,负值;1=无效
    int  srvVal  = -1;      // SDK 服务状态:0=NONE,1=LIMITED,2=FULL;-1=无效
    int  denyVal = -1;      // SDK 原始拒绝码;两套 SDK 编码不同,-1=无效
};

// 指标的轻量借用视图。用于 UI 快捷筛选/排序，不复制高频指标字符串。
// 指针只在来源 vector<MetricRow> 未清空、未增删、未触发重新分配时有效。
using MetricView = std::vector<const MetricRow*>;

// 单个小区的观测质量画像。平均值统一保存为原单位的 10 倍，避免核心层引入浮点误差。
struct CellSummary {
    std::string cellId;
    int pci = -1;
    std::uint32_t tac = UINT32_MAX;
    std::uint8_t tacDigits = 0;
    std::size_t samples = 0;
    std::size_t switchesIn = 0;
    std::size_t switchesOut = 0;
    std::size_t outageStarts = 0;
    long long first = 0;
    long long last = 0;
    long long observedDwellSec = 0;
    int sampleSharePermille = 0;

    std::size_t csqSamples = 0;
    int csqMin = INT_MAX, csqMax = INT_MIN, csqAvg10 = 0;
    std::size_t rsrpSamples = 0;
    int rsrpMin = INT_MAX, rsrpMax = INT_MIN, rsrpAvg10 = 0;
    std::size_t rsrqSamples = 0;
    int rsrqMin = INT_MAX, rsrqMax = INT_MIN, rsrqAvg10 = 0;
    std::size_t snrSamples = 0;
    int snrMin10 = INT_MAX, snrMax10 = INT_MIN, snrAvg10 = 0;
    size_t firstEvidenceLine = 0;
    size_t firstOutageLine = 0;
    long long firstOutageTime = 0;
};

struct CellTransition {
    std::string fromCell;
    std::string toCell;
    std::size_t count = 0;
    std::size_t pingPongCount = 0;
    long long first = 0;
    long long last = 0;
    size_t firstEvidenceLine = 0;
    size_t pingPongEvidenceLine = 0;
    long long pingPongEvidenceTime = 0;
};

struct CellAnalysis {
    std::vector<CellSummary> cells;
    std::vector<CellTransition> transitions;
    std::size_t totalSamples = 0;
    std::size_t samplesWithCell = 0;
    std::size_t switchCount = 0;
    std::size_t pingPongCount = 0;
    long long first = 0;
    long long last = 0;
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

} // namespace dl
