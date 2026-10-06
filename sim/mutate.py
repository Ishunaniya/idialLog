#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mutate.py — 变异测试:把已知正确的行为**故意改错**,看测试抓不抓得住。

为什么必须做:测试全绿说明不了什么 —— 可能是代码对,也可能是**测试没牙齿**。
变异测试是唯一能证明"测试真的在测"的手段。

实证:最初 10 个变异 **5 个存活**(测试只验"结论出现"、不验具体数字);
补 baselinetest 把真机数字钉死后全灭。又加 4 个解析器边界变异,再抓到 1 个洞
(ANSI CSI 剥离,补 artery 残留断言后堵上)。

每个变异对应一个**真实修过的 bug** 或**真实的行为约束**。存活 = 测试有洞。

用 Python 而非 shell:变异串里全是 C++ 代码(引号、括号、反斜杠),
shell 的多层引号转义极易出错(踩过:'~'/'.'  在 heredoc 里转义后匹配不上,
误报"变异点不存在")。Python 里就是普通字符串,无转义地狱。

优化:变异只改临时目录里的对应核心源文件副本，再与其余核心模块一起链接靶向测试。
工作树从不被改写,所以能安全验证尚未提交的正当改动,进程被杀也不会留下变异源码。
用法: python3 sim/mutate.py
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE_SOURCES = [
    "src/core/log_time.cpp",
    "src/core/log_parser.cpp",
    "src/core/log_analysis.cpp",
    "src/core/log_filter.cpp",
    "src/core/archive_reader.cpp",
]
INCLUDE_DIRS = [
    "src/core",
    "src/app",
    "src/presentation",
    "src/win32",
    "third_party/miniz",
    ".",
]
INCLUDE_FLAGS = [flag for directory in INCLUDE_DIRS
                 for flag in ("-I", os.path.join(ROOT, directory))]
TEST_SOURCES = {
    "selftest": "tests/unit/selftest.cpp",
    "archivetest": "tests/unit/archivetest.cpp",
    "boundarytest": "tests/unit/boundarytest.cpp",
    "tabletest": "tests/unit/tabletest.cpp",
    "charttest": "tests/unit/charttest.cpp",
    "simtest": "tests/regression/simtest.cpp",
    "hostruntest": "tests/regression/hostruntest.cpp",
    "baselinetest": "tests/regression/baselinetest.cpp",
    "mergetest": "tests/regression/mergetest.cpp",
    "rk3506jtest": "tests/regression/rk3506jtest.cpp",
}
# 变异测试只验证行为,不做性能基准。-O0 可降低重复编译成本。
CXX  = ["g++", "-std=c++17", "-O0"]
RUN_TIMEOUT_SECONDS = 120
COMPILE_TIMEOUT_SECONDS = 300


def run_cmd(cmd, **kwargs):
    """所有编译/测试都有上限；超时由调用方作为验证异常处理。"""
    try:
        return subprocess.run(cmd, timeout=kwargs.pop("timeout", RUN_TIMEOUT_SECONDS), **kwargs)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124)

# 每个变异:(名字, 源码里的原片段, 改坏成什么)。片段取**唯一**的核心串,避免上下文差异。
MUTATIONS = [
    ("RK3506J e15公共流量故障诊断被删除",
     '        const bool rkSource = sourcePlatformAt(rkSourcePlatforms, l) == PLAT_RK3506J;\n        if (modemTrafficSkipped(l))\n            evTrafficSkipped.push_back(&l);\n',
     '        const bool rkSource = sourcePlatformAt(rkSourcePlatforms, l) == PLAT_RK3506J;\n        if (false)\n            evTrafficSkipped.push_back(&l);\n'),
    ("RK3506J e15允许的ECM停止拒绝误报选网失败",
     '        // QNETDEVCTL=0 在 ECM 已停止时可被拒绝；它不阻止后续 CFUN/选网。\n        const bool required = command == "AT+CFUN=0" || command == "AT+CFUN=1" || command == "AT+COPS?" ||\n                              startsWith(command, "AT+COPS=1,2,");\n        e.selectionFailed = required && rkField(msg, "ok") == "0" && !rkField(msg, "status").empty();\n',
     '        // QNETDEVCTL=0 在 ECM 已停止时可被拒绝；它不阻止后续 CFUN/选网。\n        const bool required = command == "AT+QNETDEVCTL=0" || command == "AT+CFUN=0" || command == "AT+CFUN=1" ||\n                              command == "AT+COPS?" || startsWith(command, "AT+COPS=1,2,");\n        e.selectionFailed = required && rkField(msg, "ok") == "0" && !rkField(msg, "status").empty();\n'),
    ("RK3506J e15旧候选OK误升级为严格PLMN验证",
     '        // 旧版本候选分支的 set operator OK 只检查字符串 COPS；不把它提升为严格验证。\n        e.selectionVerified = tag == "FULL-DIAL" && rkHistorySelectionVerified(msg);\n        e.eg912Cycle =\n',
     '        // 旧版本候选分支的 set operator OK 只检查字符串 COPS；不把它提升为严格验证。\n        e.selectionVerified =\n            tag == "FULL-DIAL" && (rkHistorySelectionVerified(msg) || startsWith(msg, "set operator OK -> "));\n        e.eg912Cycle =\n'),
    ("RK3506J e15恢复循环重启误当公网恢复",
     '    }\n    return rkDialTag(tag) && (line.msg == "connectivity restored" || line.msg == "startup adopted existing network");\n}\n',
     '    }\n    return rkDialTag(tag) && (startsWith(line.msg, "escalate: redial stage 4/4: restart EG912 recovery cycle") ||\n                              line.msg == "connectivity restored" || line.msg == "startup adopted existing network");\n}\n'),
    # ── RK3506J 首次公网、前缀兼容、初始化容错与启动会话 ──
    ("RK3506J 首次公网成功不进入可用率",
     'static bool isDataPathUp(const LogLine& line, bool rkSource) {\n    if (rkSource && rkTimedInternetUp(line))\n        return true;\n',
     'static bool isDataPathUp(const LogLine& line, bool rkSource) {\n    if (false && rkTimedInternetUp(line))\n        return true;\n'),
    ("RK3506J FULL-DIAL 前缀兼容被删除",
     'static bool rkDialTag(const std::string& tag) {\n    return tag == "FULL-DIAL" || tag == "EC200A" || tag == "EG912";\n}\n',
     'static bool rkDialTag(const std::string& tag) {\n    return tag == "EC200A" || tag == "EG912";\n}\n'),
    ("RK3506J CPIN transient 被当成永久SIM故障",
     '        e.cpinDeadline = startsWith(msg, "CPIN not ready ");\n        e.sim = startsWith(msg, "CPIN attempt=") && rkField(msg, "class") == "permanent" &&\n                rkField(msg, "state") != "<none>" && !rkField(msg, "state").empty();\n',
     '        e.cpinDeadline = startsWith(msg, "CPIN not ready ");\n        e.sim = startsWith(msg, "CPIN attempt=") && rkField(msg, "class") == "transient" &&\n                rkField(msg, "state") != "<none>" && !rkField(msg, "state").empty();\n'),
    ("RK3506J 未恢复断网跨启动会话累加",
     '            // 未恢复事件属于打开它的启动/时基会话，不可在重启后的会话重复累加。\n            if (!outage.recovered && (outage.startLine < segment.beginLine || outage.startLine > segment.endLine))\n                continue;\n',
     '            // 未恢复事件属于打开它的启动/时基会话，不可在重启后的会话重复累加。\n            if (false && (outage.startLine < segment.beginLine || outage.startLine > segment.endLine))\n                continue;\n'),
    # ── 分类/结论逻辑 ──
    ("CP dump 退回'见标签就报'",
     '        // "No existing CP dumps.",旧实现据此报出 [严重] 基带崩溃 —— 假阳性,会误导排查方向。\n        if (l.tagText() == "CPDUMP" && icontains(l.msg, "existing CP dump") && !icontains(l.msg, "No existing") &&\n            historicalCpInventories.insert(l.msg).second)\n',
     '        // "No existing CP dumps.",旧实现据此报出 [严重] 基带崩溃 —— 假阳性,会误导排查方向。\n        if (l.tagText() == "CPDUMP" && true && !icontains(l.msg, "No existing") &&\n            historicalCpInventories.insert(l.msg).second)\n'),
    ("恢复阶梯退回按行计数",
     '        auto pushEvent = [&l](std::vector<const LogLine*>& v) {\n            if (!v.empty() && l.t - v.back()->t <= 30)\n                return; // 同一次的后续行,不另计\n',
     '        auto pushEvent = [&l](std::vector<const LogLine*>& v) {\n            if (false)\n                return; // 同一次的后续行,不另计\n'),
    ("续行退回丢弃(不并入上一条)",
     '    // 老实计入未识别,由审计报出。\n    if (!out.empty() && out.back().fmt != FMT_CONSOLE && !atFileStart && continuationOpen) {\n        out.back().msg += " ⏎ ";\n',
     '    // 老实计入未识别,由审计报出。\n    if (!out.empty() && out.back().fmt != FMT_CONSOLE && !atFileStart && false) {\n        out.back().msg += " ⏎ ";\n'),
    ("续行规则退回'无时间戳即续行'",
     '    // 老实计入未识别,由审计报出。\n    if (!out.empty() && out.back().fmt != FMT_CONSOLE && !atFileStart && continuationOpen) {\n        out.back().msg += " ⏎ ";\n',
     '    // 老实计入未识别,由审计报出。\n    if (!out.empty() && out.back().fmt != FMT_CONSOLE && !atFileStart && true) {\n        out.back().msg += " ⏎ ";\n'),
    # 注意:必须用**唯一**片段。"(c >= 'A' ...)" 在 isIdentChar(:14)与 splitTag(:122)
    # 各出现一次,replace(...,1) 会改到无关的 isIdentChar → 变异无效。用 splitTag 独有的
    # "!((c >= 'A'" 前缀锁定标签识别那处。
    ("标签退回只认大写(丢 [NetCheck])",
     "        for (char c : tag)\n            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||\n                  c == ' ')) {\n                ok = false;\n",
     "        for (char c : tag)\n            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ')) {\n                ok = false;\n"),
    ("'数据服务未就绪'退回死分支",
     '         * 永远不可能触发(三种数据源全空不是"没样本",是它根本是死的)。 */\n        if (icontains(l.msg, "data_call_init failed") || icontains(l.msg, "data_call_init retrying"))\n            evNotReady.push_back(&l);\n',
     '         * 永远不可能触发(三种数据源全空不是"没样本",是它根本是死的)。 */\n        if (false)\n            evNotReady.push_back(&l);\n'),
    ("新版 NETWORK REJECTED 明确拒绝被漏掉",
     '        // 必须同时校验嵌套标记和固定措辞，不能见到普通的 "limited service" 就下结论。\n        const bool newNetworkRejected = icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "NETWORK REJECTED");\n        const bool limitedService = icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "LIMITED SERVICE");\n',
     '        // 必须同时校验嵌套标记和固定措辞，不能见到普通的 "limited service" 就下结论。\n        const bool newNetworkRejected = icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "NETWORK REJECTEDZZ");\n        const bool limitedService = icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "LIMITED SERVICE");\n'),
    ("新版 LIMITED SERVICE 受限状态被漏掉",
     '        const bool newNetworkRejected = icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "NETWORK REJECTED");\n        const bool limitedService = icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "LIMITED SERVICE");\n        const bool suspectedAccount = icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "SUSPECTED subscription");\n',
     '        const bool newNetworkRejected = icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "NETWORK REJECTED");\n        const bool limitedService = icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "LIMITED SERVICEZZ");\n        const bool suspectedAccount = icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "SUSPECTED subscription");\n'),
    ("新版 SUSPECTED subscription 疑似账户诊断被漏掉",
     '        const bool limitedService = icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "LIMITED SERVICE");\n        const bool suspectedAccount = icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "SUSPECTED subscription");\n        const bool regQueryFailed = icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "CEREG query/parse failed");\n',
     '        const bool limitedService = icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "LIMITED SERVICE");\n        const bool suspectedAccount = icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "SUSPECTED subscriptionZZ");\n        const bool regQueryFailed = icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "CEREG query/parse failed");\n'),
    ("新版 CEREG query/parse failed 被漏掉",
     '        const bool suspectedAccount = icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "SUSPECTED subscription");\n        const bool regQueryFailed = icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "CEREG query/parse failed");\n\n',
     '        const bool suspectedAccount = icontains(l.msg, "[SIM-ACCOUNT]") && icontains(l.msg, "SUSPECTED subscription");\n        const bool regQueryFailed = icontains(l.msg, "[SIM-REG]") && icontains(l.msg, "CEREG query/parse failedZZ");\n\n'),
    ("审计不计未识别行(自洽等式该崩)",
     '    // 未识别:计数 + 分类 + 留样(这是“没漏消息”的唯一硬证据)\n    ad.unparsed++;\n    ad.unparsedKinds[classifyUnparsed(line)]++;\n',
     '    // 未识别:计数 + 分类 + 留样(这是“没漏消息”的唯一硬证据)\n    ad.unparsed += 0;\n    ad.unparsedKinds[classifyUnparsed(line)]++;\n'),
    ("弱信号阈值 <10 改成 <2",
     '            bool weakByRsrp = (minRsrp <= -110);\n            bool weakByCsq = (minCsq < 10 && mWeak);\n            const LogLine* sw = nullptr;\n',
     '            bool weakByRsrp = (minRsrp <= -110);\n            bool weakByCsq = (minCsq < 2 && mWeak);\n            const LogLine* sw = nullptr;\n'),
    ("never-connected 匹配串改错",
     '        // 其门控是静默的(eg25/dial/dial.c:974 的 has_connected_once 条件)。\n        if (icontains(l.msg, "never-connected"))\n            evNeverConn.push_back(&l);\n',
     '        // 其门控是静默的(eg25/dial/dial.c:974 的 has_connected_once 条件)。\n        if (icontains(l.msg, "never-connectedZZ"))\n            evNeverConn.push_back(&l);\n'),
    ("数据假死(ΔRX=0)判定失效",
     '                evm = weakByCsq ? mWeak : mRsrp; // CSQ 命中优先用 CSQ 证据,否则用 RSRP\n            } else if (sawZeroRx && mZero) {\n                c = C_DATADEAD;\n',
     '                evm = weakByCsq ? mWeak : mRsrp; // CSQ 命中优先用 CSQ 证据,否则用 RSRP\n            } else if (false && mZero) {\n                c = C_DATADEAD;\n'),
    # ── 解析器边界 ──
    ("SD 时间戳 ']' 位置判定改错",
     "static bool parseSd(const std::string& line, LogLine& L) {\n    if (line.size() < 22 || line[0] != '[' || line[20] != ']')\n        return false;\n",
     "static bool parseSd(const std::string& line, LogLine& L) {\n    if (line.size() < 22 || line[0] != '[' || line[20] != 'X')\n        return false;\n"),
    ("seas 毫秒 '.' 位置判定改错",
     "        return false;\n    if (line0[19] != '.')\n        return false;\n",
     "        return false;\n    if (line0[19] != 'X')\n        return false;\n"),
    ("ANSI CSI 终止范围收窄",
     "            size_t j = i + 2;\n            while (j < s.size() && !((s[j] >= '@' && s[j] <= '~')))\n                j++;\n",
     "            size_t j = i + 2;\n            while (j < s.size() && !((s[j] >= '@' && s[j] <= 'A')))\n                j++;\n"),
    ("会话标记匹配串改错",
     '    //   会话开始,都带时间戳、都算一次进程重启。\n    bool isLogOpened = line.find("Dial Log Opened") != std::string::npos;\n    bool isProgramStarted = line.find("Dial Program Started") != std::string::npos;\n',
     '    //   会话开始,都带时间戳、都算一次进程重启。\n    bool isLogOpened = line.find("Dial Log OpenedZZ") != std::string::npos;\n    bool isProgramStarted = line.find("Dial Program Started") != std::string::npos;\n'),
    # ── 多文件合并定序(orderByTime / firstTimestamp)——mergetest 的靶子 ──
    ('定序退回不排序(拖入顺序直接拼)',
     '    // stable_sort:同一首时间戳(同一秒内起头的两份)保持输入顺序,不无端打乱。\n    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {\n        if (a.hasT != b.hasT)\n            return a.hasT; // 有时间戳的一律在前\n        if (!a.hasT)\n            return false; // 都没有 → 比较结果为“相等”,stable 保持原序\n        return a.t < b.t;\n    });\n    std::vector<size_t> out;\n',
     '    // stable_sort:同一首时间戳(同一秒内起头的两份)保持输入顺序,不无端打乱。\n    if (false)\n        std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {\n            if (a.hasT != b.hasT)\n                return a.hasT; // 有时间戳的一律在前\n            if (!a.hasT)\n                return false; // 都没有 → 比较结果为“相等”,stable 保持原序\n            return a.t < b.t;\n        });\n    std::vector<size_t> out;\n'),
    ('定序丢掉 stable(同秒起头顺序不再保证)',
     '    // stable_sort:同一首时间戳(同一秒内起头的两份)保持输入顺序,不无端打乱。\n    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {\n        if (a.hasT != b.hasT)\n',
     '    // stable_sort:同一首时间戳(同一秒内起头的两份)保持输入顺序,不无端打乱。\n    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {\n        if (a.hasT != b.hasT)\n'),
    ('无时间戳的 chunk 退回排最前',
     '        if (a.hasT != b.hasT)\n            return a.hasT; // 有时间戳的一律在前\n        if (!a.hasT)\n',
     '        if (a.hasT != b.hasT)\n            return !a.hasT; // 有时间戳的一律在前\n        if (!a.hasT)\n'),
    ('firstTimestamp 漏认会话标记',
     '        // 比后面第一条普通日志更早,是这份文件真正的起点。open_dial 用 "Dial Program Started"。\n        if (line.compare(0, 3, "===") == 0 && (line.find("Dial Log Opened") != std::string::npos ||\n                                               line.find("Dial Program Started") != std::string::npos)) {\n            size_t a = line.find(\'[\');\n',
     '        // 比后面第一条普通日志更早,是这份文件真正的起点。open_dial 用 "Dial Program Started"。\n        if (false) {\n            size_t a = line.find(\'[\');\n'),
    # ── 跨时基混合防护(timeBaseOf / detectMix)——mergetest T11 的靶子 ──
    ('时基阈值退回 0(1970 被当墙钟,混合不再被拦)',
     '    // 阈值取 946684800 - 86400 留一天余量(避免时区把 2000-01-01 本地时刻算到边界外)。\n    return (t < 946598400LL) ? TB_UNSYNCED : TB_WALL;\n}\n',
     '    // 阈值取 946684800 - 86400 留一天余量(避免时区把 2000-01-01 本地时刻算到边界外)。\n    return (t < 0LL) ? TB_UNSYNCED : TB_WALL;\n}\n'),
    ('detectMix 永不报混合(mixed 恒 false)',
     '    }\n    r.mixed = !r.wallIdx.empty() && !r.unsyncedIdx.empty();\n    return r;\n',
     '    }\n    r.mixed = false;\n    return r;\n'),
    ('未同步误分到墙钟批(排除失效)',
     '        case TB_UNSYNCED:\n            r.unsyncedIdx.push_back(i);\n            break;\n',
     '        case TB_UNSYNCED:\n            r.wallIdx.push_back(i);\n            break;\n'),
    # ── 压缩包直读 / BOM 剥离(archiveKindOf / extractArchive / stripBom)——archivetest 靶子 ──
    ('stripBom 不剥离(BOM 残留破坏首戳)',
     '        (unsigned char)buf[2] == 0xBF) {\n        buf.erase(0, 3);\n    }\n',
     '        (unsigned char)buf[2] == 0xBF) {\n        (void)0;\n    }\n'),
    ('gzip 魔数判错(第二字节)',
     'ArchiveKind archiveKindOf(const std::string& buf) {\n    if (buf.size() >= 2 && (unsigned char)buf[0] == 0x1F && (unsigned char)buf[1] == 0x8B)\n        return ARC_GZIP;\n',
     'ArchiveKind archiveKindOf(const std::string& buf) {\n    if (buf.size() >= 2 && (unsigned char)buf[0] == 0x1F && (unsigned char)buf[1] == 0x8C)\n        return ARC_GZIP;\n'),
    ('zip 魔数判错',
     "        return ARC_GZIP;\n    if (buf.size() >= 4 && buf[0] == 'P' && buf[1] == 'K' && (unsigned char)buf[2] == 0x03 &&\n        (unsigned char)buf[3] == 0x04)\n",
     "        return ARC_GZIP;\n    if (buf.size() >= 4 && buf[0] == 'Q' && buf[1] == 'K' && (unsigned char)buf[2] == 0x03 &&\n        (unsigned char)buf[3] == 0x04)\n"),
    ('tar 大小字段进制读错(八进制当十进制)',
     '        char* sizeEnd = nullptr;\n        long long fsize = std::strtoll(szbuf, &sizeEnd, 8);\n        if (sizeEnd == szbuf || fsize < 0 || (unsigned long long)fsize > kMaxArchiveEntrySize)\n',
     '        char* sizeEnd = nullptr;\n        long long fsize = std::strtoll(szbuf, &sizeEnd, 10);\n        if (sizeEnd == szbuf || fsize < 0 || (unsigned long long)fsize > kMaxArchiveEntrySize)\n'),
    ('tar 魔数判错(ustar)',
     '    // 这里用魔数做主判据(可靠),避免把普通文本误判成 tar。\n    return d.size() >= 262 && std::memcmp(d.data() + 257, "ustar", 5) == 0;\n}\n',
     '    // 这里用魔数做主判据(可靠),避免把普通文本误判成 tar。\n    return d.size() >= 262 && std::memcmp(d.data() + 257, "ustaX", 5) == 0;\n}\n'),
    # ── 跨文件续行防御 / 时钟跳变检测 —— boundarytest 靶子 ──
    ('跨文件续行防御失效(atFileStart 恒 false)',
     '    // 老实计入未识别,由审计报出。\n    if (!out.empty() && out.back().fmt != FMT_CONSOLE && !atFileStart && continuationOpen) {\n        out.back().msg += " ⏎ ";\n',
     '    // 老实计入未识别,由审计报出。\n    if (!out.empty() && out.back().fmt != FMT_CONSOLE && !false && continuationOpen) {\n        out.back().msg += " ⏎ ";\n'),
    ('时钟跳变阈值错(2000边界退回0)',
     '            long long prevT = out.back().t;\n            bool prevUnsynced = prevT < 946598400LL; // <2000-01-01(与 timeBaseOf 同阈值)\n            bool curUnsynced = L.t < 946598400LL;\n',
     '            long long prevT = out.back().t;\n            bool prevUnsynced = prevT < 0LL; // <2000-01-01(与 timeBaseOf 同阈值)\n            bool curUnsynced = L.t < 946598400LL;\n'),
    ('时钟跳变检测关闭(clockJump 永不置位)',
     '            bool curUnsynced = L.t < 946598400LL;\n            if (prevUnsynced != curUnsynced) {\n                ad.clockJump = true;\n',
     '            bool curUnsynced = L.t < 946598400LL;\n            if (false) {\n                ad.clockJump = true;\n'),
    # ── 公共文本切行 —— boundarytest T8 的靶子 ──
    ('CRLF退回按两个换行处理(每行多造一个空行)',
     "        out.push_back(buf.substr(start, i - start));\n        if (buf[i] == '\\r' && i + 1 < buf.size() && buf[i + 1] == '\\n')\n            ++i;\n",
     "        out.push_back(buf.substr(start, i - start));\n        if (false && i + 1 < buf.size() && buf[i + 1] == '\\n')\n            ++i;\n"),
    # ── gzip 完整性与头边界 —— archivetest T7 的靶子 ──
    ('gzip FNAME边界退回按整个buf而非trailer判断',
     '            size_t z = p;\n            while (z < trailer && buf[z])\n                ++z;\n            if (z >= trailer) {\n                err = std::string("gzip ") + field + " 越界";\n',
     '            size_t z = p;\n            while (z < buf.size() && buf[z])\n                ++z;\n            if (z >= buf.size()) {\n                err = std::string("gzip ") + field + " 越界";\n'),
    ('gzip CRC32校验被关闭',
     '            mz_crc32(MZ_CRC32_INIT, out.empty() ? nullptr : (const unsigned char*)out.data(), out.size());\n        if ((uint32_t)actualCrc != expectedCrc) {\n            err = "gzip CRC32 校验失败";\n',
     '            mz_crc32(MZ_CRC32_INIT, out.empty() ? nullptr : (const unsigned char*)out.data(), out.size());\n        if (false) {\n            err = "gzip CRC32 校验失败";\n'),
    # ── 断网引擎 open_dial 'Down:' 格式识别 —— baselinetest 靶子 ──
    ('断网漏认 open_dial 的 Down: 格式(回退只认 after)',
     '    //   必须同时含 "network recovered" 与 "down:",避免把无关的 "Down:" 行误判。\n    if (lo.find("network recovered") != std::string::npos) {\n        size_t d = lo.find("down:");\n',
     '    //   必须同时含 "network recovered" 与 "down:",避免把无关的 "Down:" 行误判。\n    if (false) {\n        size_t d = lo.find("down:");\n'),
    ('Down: 时长提取位置错(偏移5改0)',
     "        if (d != std::string::npos) {\n            size_t q = d + 5;\n            while (q < msg.size() && msg[q] == ' ')\n",
     "        if (d != std::string::npos) {\n            size_t q = d + 0;\n            while (q < msg.size() && msg[q] == ' ')\n"),
    # ── open_dial 会话标记识别 —— baselinetest 靶子 ──
    ('会话标记漏认 open_dial 的 Dial Program Started',
     '    bool isLogOpened = line.find("Dial Log Opened") != std::string::npos;\n    bool isProgramStarted = line.find("Dial Program Started") != std::string::npos;\n    bool isOpened = isLogOpened || isProgramStarted;\n',
     '    bool isLogOpened = line.find("Dial Log Opened") != std::string::npos;\n    bool isProgramStarted = line.find("Dial Program StartedZZ") != std::string::npos;\n    bool isOpened = isLogOpened || isProgramStarted;\n'),
    # ── RSRP/RSRQ 提取 —— baselinetest 靶子 ──
    ('RSRP 提取失效(字段键改错)',
     '            f.rxLower = v;\n        else if (k == "RSRP")\n            f.rsrpUpper = v;\n',
     '            f.rxLower = v;\n        else if (k == "RSRPZZ")\n            f.rsrpUpper = v;\n'),
    ('RSRP 负值过滤反向(只收正数→全废)',
     '        // 只接受负值,正数视为异常(1=无效标记)。\n        if (parseLong(firstOf(f.rsrpUpper, f.rsrpLower), number) && number >= INT_MIN && number < 0)\n            m.rsrp = (int)number;\n',
     '        // 只接受负值,正数视为异常(1=无效标记)。\n        if (parseLong(firstOf(f.rsrpUpper, f.rsrpLower), number) && number >= INT_MIN && number > 0)\n            m.rsrp = (int)number;\n'),
    # ── RSRP 断网分类 + 信号劣化结论 —— baselinetest 靶子 ──
    ('RSRP断网分类失效(阈值-110退回不可能值)',
     '            // CSQ 是 0-31 粗档,可能读到中间值,而 RSRP 已探底 —— 覆盖问题此时才现形。\n            bool weakByRsrp = (minRsrp <= -110);\n            bool weakByCsq = (minCsq < 10 && mWeak);\n',
     '            // CSQ 是 0-31 粗档,可能读到中间值,而 RSRP 已探底 —— 覆盖问题此时才现形。\n            bool weakByRsrp = (minRsrp <= -99999);\n            bool weakByCsq = (minCsq < 10 && mWeak);\n'),
    ('信号劣化结论阈值反向(≤-100退回≥)',
     '            int avg = (int)(sum / n);\n            if (avg <= -100 && mWorst) { // 均值 ≤ -100 命中 LTE 工程参考较差档\n                Finding f;\n',
     '            int avg = (int)(sum / n);\n            if (avg >= -100 && mWorst) { // 均值 ≤ -100 命中 LTE 工程参考较差档\n                Finding f;\n'),
    # ── 新 SDK 心跳字段 SNR / DENY —— baselinetest 精确值与结论边界的靶子 ──
    ('SNR原始0.1dB被误除10(246不再精确保留)',
     '        } else if (parseLong(firstOf(f.snrUpper, f.snrLower), number, true) && number >= -32768 && number <= 32767)\n            m.snr10 = (int)number;\n        if (parseLong(firstOf(f.rssiUpper, f.rssiLower), number, true) && number >= INT_MIN && number < 0)\n',
     '        } else if (parseLong(firstOf(f.snrUpper, f.snrLower), number, true) && number >= -32768 && number <= 32767)\n            m.snr10 = (int)number / 10;\n        if (parseLong(firstOf(f.rssiUpper, f.rssiLower), number, true) && number >= INT_MIN && number < 0)\n'),
    ('SNR推断提示被关闭',
     '        }\n        if (n >= 5 && nonPositive * 2 >= n && mWorst) {\n            int avg10 = (int)(sum / n);\n',
     '        }\n        if (false && mWorst) {\n            int avg10 = (int)(sum / n);\n'),
    ('SDK DENY拒绝证据被漏掉',
     '    for (const auto& m : mets)\n        if (m.srvVal >= 0 && m.srvVal != 2 && m.denyVal > 0)\n            evSdkDeny.push_back(&m);\n',
     '    for (const auto& m : mets)\n        if (m.srvVal >= 0 && m.srvVal != 2 && m.denyVal > 999)\n            evSdkDeny.push_back(&m);\n'),
    # ── SDK L0 短断网归类 —— baselinetest 靶子 ──
    ('SDK L0标志失效(不认(L0))',
     '            // 这类是短断网、链路抖动,设备自愈,与走 L1+ 阶梯的深层恢复区分。\n            o.l0Recovered = (l.msg.find("(L0)") != std::string::npos);\n            o.reportedDuration = reportedRecovery && selfContained;\n',
     '            // 这类是短断网、链路抖动,设备自愈,与走 L1+ 阶梯的深层恢复区分。\n            o.l0Recovered = (l.msg.find("(L0)ZZ") != std::string::npos);\n            o.reportedDuration = reportedRecovery && selfContained;\n'),
    ('SDK L0归类分支删除(退回未能归类)',
     '            // 未能归类是"查不出",这里是"查出来了——SDK 在 L0 就自愈,是网络侧瞬时抖动"。\n            else if (o.l0Recovered) {\n                c = C_SDK_L0;\n',
     '            // 未能归类是"查不出",这里是"查出来了——SDK 在 L0 就自愈,是网络侧瞬时抖动"。\n            else if (false) {\n                c = C_SDK_L0;\n'),
]

# `make check-full` 已先跑全部测试；每个变异这里只链接最能抓它的靶向测试。
# 若路由选错,该变异会“存活”并令整轮失败,不会被静默放过。
def target_test(mut_name):
    lower_name = mut_name.lower()
    if mut_name.startswith("RK3506J"):
        return "rk3506jtest"
    if any(k in mut_name for k in ("跨文件", "时钟跳变", "CRLF")):
        return "boundarytest"
    # 用 startswith 避免 "Started" / "trailer" 中间恰含 "tar" 而误路由。
    if (lower_name.startswith(("gzip", "zip", "tar", "stripbom")) or
            any(k in lower_name for k in ("bom", "miniz", "解压", "压缩", "魔数"))):
        return "archivetest"
    if any(k in mut_name for k in ("定序", "firstTimestamp", "chunk", "时基", "detectMix",
                                   "未同步误分", "stable")):
        return "mergetest"
    if "never-connected" in mut_name:
        return "hostruntest"
    if any(k in mut_name for k in ("NETWORK REJECTED", "LIMITED SERVICE",
                                   "SUSPECTED subscription", "CEREG query/parse failed")):
        return "simtest"
    return "baselinetest"


_MINIZ_CACHE = [None]   # miniz.o 只编一次(与变异无关),全局缓存路径


def _miniz_obj():
    """编译 miniz.o 一次并缓存返回路径;失败返回 None。"""
    if _MINIZ_CACHE[0] and os.path.exists(_MINIZ_CACHE[0]):
        return _MINIZ_CACHE[0]
    import tempfile
    path = os.path.join(tempfile.gettempdir(), "dl_mutate_miniz.o")
    if run_cmd(["gcc", "-std=c11", "-O2", "-DMINIZ_NO_STDIO", "-DMINIZ_NO_TIME",
                "-c", os.path.join(ROOT, "third_party/miniz/miniz.c"), "-o", path], cwd=ROOT,
               timeout=COMPILE_TIMEOUT_SECONDS, stderr=subprocess.DEVNULL).returncode != 0:
        return None
    _MINIZ_CACHE[0] = path
    return path


def build_shared_objects(tmp, originals):
    """从本轮捕获的源码编译公共对象；每个变异只重编真正改变的模块。"""
    shared = {}
    directory = os.path.join(tmp, "shared")
    for source, content in originals.items():
        snapshot = os.path.join(directory, source)
        os.makedirs(os.path.dirname(snapshot), exist_ok=True)
        with open(snapshot, "w", encoding="utf-8") as output:
            output.write(content)
        stem = os.path.splitext(os.path.basename(source))[0]
        modes = (False, True) if stem == "archive_reader" else (False,)
        for archive_enabled in modes:
            obj = os.path.join(directory, stem + ("_miniz" if archive_enabled else "") + ".o")
            flags = ["-DDL_HAVE_MINIZ"] if archive_enabled else []
            if run_cmd(CXX + flags + INCLUDE_FLAGS + ["-c", snapshot, "-o", obj], cwd=ROOT,
                       timeout=COMPILE_TIMEOUT_SECONDS, stderr=subprocess.DEVNULL).returncode != 0:
                raise RuntimeError(f"公共模块 {stem} 编译失败或超时")
            shared[(stem, archive_enabled)] = obj
    table = os.path.join(directory, "tablemodel.o")
    if run_cmd(CXX + INCLUDE_FLAGS + ["-c", os.path.join(ROOT, "src/presentation/tablemodel.cpp"),
                                     "-o", table], cwd=ROOT, timeout=COMPILE_TIMEOUT_SECONDS,
               stderr=subprocess.DEVNULL).returncode != 0:
        raise RuntimeError("公共表格模块编译失败或超时")
    shared[("tablemodel", False)] = table
    return shared


def run_tests(tmp, sources, mut_name="", shared_objects=None, changed_source=None):
    """编译拆分后的核心对象并运行靶向测试。全绿=变异存活=测试有洞。"""
    test = target_test(mut_name)
    exe = os.path.join(tmp, test)
    archive_enabled = test == "archivetest"
    objects = []
    for source in sources:
        stem = os.path.splitext(os.path.basename(source))[0]
        obj = os.path.join(tmp, stem + ".o")
        flags = ["-DDL_HAVE_MINIZ"] if archive_enabled and stem == "archive_reader" else []
        if shared_objects is not None and os.path.relpath(source, tmp) != changed_source:
            objects.append(shared_objects[(stem, bool(flags))])
            continue
        if run_cmd(CXX + flags + INCLUDE_FLAGS + ["-c", source, "-o", obj], cwd=ROOT,
                   timeout=COMPILE_TIMEOUT_SECONDS, stderr=subprocess.DEVNULL).returncode != 0:
            raise RuntimeError("核心编译失败或超时")
        objects.append(obj)

    link_flags = ["-DDL_HAVE_MINIZ"] if archive_enabled else []
    test_source = os.path.join(ROOT, TEST_SOURCES[test])
    cmd = CXX + link_flags + INCLUDE_FLAGS + ["-o", exe, test_source] + objects
    if test == "rk3506jtest":
        cmd.append(shared_objects[("tablemodel", False)] if shared_objects is not None else
                   os.path.join(ROOT, "src/presentation/tablemodel.cpp"))
    if archive_enabled:
        mzobj = _miniz_obj()
        if mzobj is None:
            raise RuntimeError("miniz 编译失败或超时")
        cmd.append(mzobj)
    if run_cmd(cmd, cwd=ROOT, timeout=COMPILE_TIMEOUT_SECONDS, stderr=subprocess.DEVNULL).returncode != 0:
        raise RuntimeError("靶向测试编译/链接失败或超时")
    result = run_cmd([exe], cwd=ROOT, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)
    if result.returncode == 124 or result.returncode < 0:
        raise RuntimeError("靶向测试超时或被信号中止")
    return result.returncode == 0


# Whitespace is not a mutation target. Keep literals/comments exact while
# allowing clang-format to separate functions, logical blocks and comments.
_CPP_FRAGMENT_TOKEN = re.compile(
    r'(?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)")'
    r'|//[^\n]*|/\*.*?\*/|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"'
    r"|(?:u8|u|U|L)?'(?:\\.|[^'\\])*'|[A-Za-z_]\w*|[0-9]+|[^\s]",
    re.DOTALL,
)


def fragment_matches(text, fragment):
    tokens = [match.group() for match in _CPP_FRAGMENT_TOKEN.finditer(fragment)]
    pattern = r"\s*".join(re.escape(token) for token in tokens)
    # Refuse ambiguous locators rather than quietly mutating another occurrence.
    return list(re.finditer(pattern, text))


def run_mutation(tmp_root, originals, idx, name, old, new, total, shared_objects=None):
    """在独立临时目录运行一个变异；返回 (状态,耗时)。"""
    started = time.monotonic()
    print(f"  [{idx:02d}/{total:02d}] {name}", flush=True)
    targets = [(source, match) for source, text in originals.items()
               for match in fragment_matches(text, old)]
    if len(targets) != 1:
        return "bad", time.monotonic() - started
    tmp = os.path.join(tmp_root, f"m{idx:02d}")
    os.makedirs(tmp)
    target, match = targets[0]
    sources = []
    for source, text in originals.items():
        copy = os.path.join(tmp, source)
        os.makedirs(os.path.dirname(copy), exist_ok=True)
        with open(copy, "w", encoding="utf-8") as f:
            f.write(text[:match.start()] + new + text[match.end():] if source == target else text)
        sources.append(copy)
    try:
        survived = run_tests(tmp, sources, name, shared_objects, target)
    except RuntimeError as err:
        print(f"      ⚠ {name}:{err}", flush=True)
        return "error", time.monotonic() - started
    return ("survived" if survived else "caught"), time.monotonic() - started


def main():
    originals = {}
    for source in CORE_SOURCES:
        with open(os.path.join(ROOT, source), encoding="utf-8") as f:
            originals[source] = f.read()
    tmp = tempfile.mkdtemp(prefix="diallog_mutate_")
    caught = survived = bad = errors = 0
    all_started = time.monotonic()
    try:
        requested_jobs = int(os.environ.get("DL_MUTATE_JOBS", "2"))
    except ValueError:
        requested_jobs = 2
    jobs = min(4, max(1, requested_jobs))
    print(f"════ 变异测试:把修过的 bug 故意改回去,看测试抓不抓得住 "
          f"(并发 {jobs}) ════", flush=True)
    try:
        # 避免多个 archive 变异同时争抢同一个 miniz 缓存文件。
        _miniz_obj()
        shared_objects = build_shared_objects(tmp, originals)
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = {}
            for idx, (name, old, new) in enumerate(MUTATIONS, 1):
                fut = pool.submit(run_mutation, tmp, originals, idx, name, old, new,
                                  len(MUTATIONS), shared_objects)
                futures[fut] = name
            for fut in as_completed(futures):
                status, elapsed = fut.result()
                name = futures[fut]
                if status == "bad":
                    print(f"      ⚠ {name}:变异点不存在(片段对不上) ({elapsed:.1f}s)", flush=True)
                    bad += 1
                elif status == "error":
                    errors += 1
                elif status == "survived":
                    print(f"      ❌ {name}:存活 —— 测试没抓住,有洞 ({elapsed:.1f}s)", flush=True)
                    survived += 1
                else:
                    print(f"      ✅ {name}:被抓住 ({elapsed:.1f}s)", flush=True)
                    caught += 1
    except RuntimeError as err:
        print(f"公共构建验证异常:{err}", flush=True)
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    total_elapsed = time.monotonic() - all_started
    print(f"\n════ {len(MUTATIONS)} 个变异:{caught} 被抓住,{survived} 存活,{bad} 片段失配,{errors} 验证异常"
          f"，总耗时 {total_elapsed:.1f}s ════", flush=True)
    if survived:
        print("存活 = 测试有洞,必须补断言(不是代码没问题)")
    if bad:
        print("片段失配 = mutate.py 的 old 串和源码对不上,不是真跳过 —— 必须修")
    if errors:
        print("验证异常 = 没有有效断言结果，不能算作变异被抓住")
    return 1 if (survived or bad or errors) else 0


if __name__ == "__main__":
    sys.exit(main())
