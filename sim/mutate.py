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

优化:变异只改临时目录里的 logmodel.cpp 副本 → 编一次 .o,多个测试共享链接。
工作树从不被改写,所以能安全验证尚未提交的正当改动,进程被杀也不会留下变异源码。
用法: python3 sim/mutate.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "logmodel.cpp")
# 变异测试只验证行为,不做性能基准。-O0 可把 44 次重复编译从数十分钟压到可接受范围。
CXX  = ["g++", "-std=c++17", "-O0"]
RUN_TIMEOUT_SECONDS = 120


def run_cmd(cmd, **kwargs):
    """所有编译/测试都有上限；超时按“变异被抓住”处理,避免整轮永久卡住。"""
    try:
        return subprocess.run(cmd, timeout=RUN_TIMEOUT_SECONDS, **kwargs)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124)

# 每个变异:(名字, 源码里的原片段, 改坏成什么)。片段取**唯一**的核心串,避免上下文差异。
MUTATIONS = [
    # ── 分类/结论逻辑 ──
    ("CP dump 退回'见标签就报'",
     'l.tagText() == "CPDUMP" && icontains(l.msg, "existing CP dump")',
     'l.tagText() == "CPDUMP" && true'),
    ("恢复阶梯退回按行计数",
     'if (!v.empty() && l.t - v.back()->t <= 30) return;',
     'if (false) return;'),
    ("续行退回丢弃(不并入上一条)",
     "if (e2 != std::string::npos && prev[e2] == ':') {",
     "if (false) {"),
    ("续行规则退回'无时间戳即续行'",
     "if (e2 != std::string::npos && prev[e2] == ':') {",
     "if (true) {"),
    # 注意:必须用**唯一**片段。"(c >= 'A' ...)" 在 isIdentChar(:14)与 splitTag(:122)
    # 各出现一次,replace(...,1) 会改到无关的 isIdentChar → 变异无效。用 splitTag 独有的
    # "!((c >= 'A'" 前缀锁定标签识别那处。
    ("标签退回只认大写(丢 [NetCheck])",
     "!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||",
     "!((c >= 'A' && c <= 'Z') ||"),
    ("'数据服务未就绪'退回死分支",
     'icontains(l.msg, "data_call_init failed") ||\n            icontains(l.msg, "data_call_init retrying"))',
     'false)'),
    ("审计不计未识别行(自洽等式该崩)",
     "ad.unparsed++;",
     "ad.unparsed += 0;"),
    ("弱信号阈值 <10 改成 <2",
     "bool weakByCsq  = (minCsq < 10 && mWeak);",
     "bool weakByCsq  = (minCsq < 2 && mWeak);"),
    ("never-connected 匹配串改错",
     'icontains(l.msg, "never-connected")',
     'icontains(l.msg, "never-connectedZZ")'),
    ("数据假死(ΔRX=0)判定失效",
     "else if (sawZeroRx && mZero)",
     "else if (false && mZero)"),
    # ── 解析器边界 ──
    ("SD 时间戳 ']' 位置判定改错",
     "line[20] != ']'",
     "line[20] != 'X'"),
    ("seas 毫秒 '.' 位置判定改错",
     "line0[19] != '.'",
     "line0[19] != 'X'"),
    ("ANSI CSI 终止范围收窄",
     "s[j] >= '@' && s[j] <= '~'",
     "s[j] >= '@' && s[j] <= 'A'"),
    ("会话标记匹配串改错",
     'line.find("Dial Log Opened")',
     'line.find("Dial Log OpenedZZ")'),
    # ── 多文件合并定序(orderByTime / firstTimestamp)——mergetest 的靶子 ──
    ('定序退回不排序(拖入顺序直接拼)',
     'std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {',
     'if (false) std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {'),
    ('定序丢掉 stable(同秒起头顺序不再保证)',
     'std::stable_sort(keys.begin(), keys.end()',
     'std::sort(keys.begin(), keys.end()'),
    ('无时间戳的 chunk 退回排最前',
     'if (a.hasT != b.hasT) return a.hasT;',
     'if (a.hasT != b.hasT) return !a.hasT;'),
    ('firstTimestamp 漏认会话标记',
     'if (line.compare(0, 3, "===") == 0 &&\n            (line.find("Dial Log Opened") != std::string::npos ||\n             line.find("Dial Program Started") != std::string::npos)) {',
     'if (false) {'),
    # ── 跨时基混合防护(timeBaseOf / detectMix)——mergetest T11 的靶子 ──
    ('时基阈值退回 0(1970 被当墙钟,混合不再被拦)',
     'return (t < 946598400LL) ? TB_UNSYNCED : TB_WALL;',
     'return (t < 0LL) ? TB_UNSYNCED : TB_WALL;'),
    ('detectMix 永不报混合(mixed 恒 false)',
     'r.mixed = !r.wallIdx.empty() && !r.unsyncedIdx.empty();',
     'r.mixed = false;'),
    ('未同步误分到墙钟批(排除失效)',
     'case TB_UNSYNCED: r.unsyncedIdx.push_back(i); break;',
     'case TB_UNSYNCED: r.wallIdx.push_back(i); break;'),
    # ── 压缩包直读 / BOM 剥离(archiveKindOf / extractArchive / stripBom)——archivetest 靶子 ──
    ('stripBom 不剥离(BOM 残留破坏首戳)',
     '        buf.erase(0, 3);',
     '        (void)0;'),
    ('gzip 魔数判错(第二字节)',
     '(unsigned char)buf[0] == 0x1F && (unsigned char)buf[1] == 0x8B) return ARC_GZIP;',
     '(unsigned char)buf[0] == 0x1F && (unsigned char)buf[1] == 0x8C) return ARC_GZIP;'),
    ('zip 魔数判错',
     "buf[0] == 'P' && buf[1] == 'K' &&",
     "buf[0] == 'Q' && buf[1] == 'K' &&"),
    ('tar 大小字段进制读错(八进制当十进制)',
     'long long fsize = std::strtoll(szbuf, &sizeEnd, 8);',
     'long long fsize = std::strtoll(szbuf, &sizeEnd, 10);'),
    ('tar 魔数判错(ustar)',
     'std::memcmp(d.data() + 257, "ustar", 5) == 0;',
     'std::memcmp(d.data() + 257, "ustaX", 5) == 0;'),
    # ── 跨文件续行防御 / 时钟跳变检测 —— boundarytest 靶子 ──
    ('跨文件续行防御失效(atFileStart 恒 false)',
     'if (!out.empty() && !atFileStart) {',
     'if (!out.empty() && !false) {'),
    ('时钟跳变阈值错(2000边界退回0)',
     'bool prevUnsynced = prevT < 946598400LL;   // <2000-01-01(与 timeBaseOf 同阈值)',
     'bool prevUnsynced = prevT < 0LL;   // <2000-01-01(与 timeBaseOf 同阈值)'),
    ('时钟跳变检测关闭(clockJump 永不置位)',
     'if (prevUnsynced != curUnsynced) {',
     'if (false) {'),
    # ── 公共文本切行 —— boundarytest T8 的靶子 ──
    ('CRLF退回按两个换行处理(每行多造一个空行)',
     "if (buf[i] == '\\r' && i + 1 < buf.size() && buf[i + 1] == '\\n') ++i;",
     "if (false && i + 1 < buf.size() && buf[i + 1] == '\\n') ++i;"),
    # ── gzip 完整性与头边界 —— archivetest T7 的靶子 ──
    ('gzip FNAME边界退回按整个buf而非trailer判断',
     '''while (z < trailer && buf[z]) ++z;
            if (z >= trailer) { err = std::string("gzip ") + field + " 越界"; return false; }
            p = z + 1;''',
     '''while (z < buf.size() && buf[z]) ++z;
            if (z >= buf.size()) { err = std::string("gzip ") + field + " 越界"; return false; }
            p = z + 1;'''),
    ('gzip CRC32校验被关闭',
     'if ((uint32_t)actualCrc != expectedCrc) { err = "gzip CRC32 校验失败"; return false; }',
     'if (false) { err = "gzip CRC32 校验失败"; return false; }'),
    # ── 断网引擎 open_dial 'Down:' 格式识别 —— baselinetest 靶子 ──
    ('断网漏认 open_dial 的 Down: 格式(回退只认 after)',
     'if (lo.find("network recovered") != std::string::npos) {',
     'if (false) {'),
    ('Down: 时长提取位置错(偏移5改0)',
     'size_t q = d + 5;',
     'size_t q = d + 0;'),
    # ── open_dial 会话标记识别 —— baselinetest 靶子 ──
    ('会话标记漏认 open_dial 的 Dial Program Started',
     'bool isOpened = line.find("Dial Log Opened") != std::string::npos ||\n                        line.find("Dial Program Started") != std::string::npos;',
     'bool isOpened = line.find("Dial Log Opened") != std::string::npos ||\n                        line.find("Dial Program StartedZZ") != std::string::npos;'),
    # ── RSRP/RSRQ 提取 —— baselinetest 靶子 ──
    ('RSRP 提取失效(字段键改错)',
     'else if (k == "RSRP")        f.rsrpUpper = v;',
     'else if (k == "RSRPZZ")      f.rsrpUpper = v;'),
    ('RSRP 负值过滤反向(只收正数→全废)',
     'if (parseLong(firstOf(f.rsrpUpper, f.rsrpLower), number) &&\n            number >= INT_MIN && number < 0) m.rsrp = (int)number;',
     'if (parseLong(firstOf(f.rsrpUpper, f.rsrpLower), number) &&\n            number >= INT_MIN && number > 0) m.rsrp = (int)number;'),
    # ── RSRP 断网分类 + 信号劣化结论 —— baselinetest 靶子 ──
    ('RSRP断网分类失效(阈值-110退回不可能值)',
     'bool weakByRsrp = (minRsrp <= -110);',
     'bool weakByRsrp = (minRsrp <= -99999);'),
    ('信号劣化结论阈值反向(≤-100退回≥)',
     'if (avg <= -100 && mWorst) {',
     'if (avg >= -100 && mWorst) {'),
    # ── 新 SDK 心跳字段 SNR / DENY —— baselinetest 精确值与结论边界的靶子 ──
    ('SNR原始0.1dB被误除10(246不再精确保留)',
     'm.snr10 = (int)number;',
     'm.snr10 = (int)number / 10;'),
    ('SNR推断提示被关闭',
     'if (n >= 5 && nonPositive * 2 >= n && mWorst) {',
     'if (false && mWorst) {'),
    ('SDK DENY拒绝证据被漏掉',
     'if (m.srvVal >= 0 && m.srvVal != 2 && m.denyVal > 0) evSdkDeny.push_back(&m);',
     'if (m.srvVal >= 0 && m.srvVal != 2 && m.denyVal > 999) evSdkDeny.push_back(&m);'),
    # ── SDK L0 短断网归类 —— baselinetest 靶子 ──
    ('SDK L0标志失效(不认(L0))',
     'o.l0Recovered = (l.msg.find("(L0)") != std::string::npos);',
     'o.l0Recovered = (l.msg.find("(L0)ZZ") != std::string::npos);'),
    ('SDK L0归类分支删除(退回未能归类)',
     'else if (o.l0Recovered)          { c = C_SDK_L0; }',
     'else if (false)                  { c = C_SDK_L0; }'),
]

# `make check-full` 已先跑全部测试；每个变异这里只链接最能抓它的靶向测试。
# 若路由选错,该变异会“存活”并令整轮失败,不会被静默放过。
def target_test(mut_name):
    lower_name = mut_name.lower()
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
    return "baselinetest"


_MINIZ_CACHE = [None]   # miniz.o 只编一次(与变异无关),全局缓存路径


def _miniz_obj():
    """编译 miniz.o 一次并缓存返回路径;失败返回 None。"""
    if _MINIZ_CACHE[0] and os.path.exists(_MINIZ_CACHE[0]):
        return _MINIZ_CACHE[0]
    import tempfile
    path = os.path.join(tempfile.gettempdir(), "dl_mutate_miniz.o")
    if run_cmd(["gcc", "-std=c11", "-O2", "-DMINIZ_NO_STDIO", "-DMINIZ_NO_TIME",
                "-c", "miniz.c", "-o", path], cwd=ROOT,
               stderr=subprocess.DEVNULL).returncode != 0:
        return None
    _MINIZ_CACHE[0] = path
    return path


def run_tests(tmp, source, mut_name=""):
    """编变异 logmodel 对象,只链接并运行对应靶向测试。全绿=变异存活=测试有洞。"""
    test = target_test(mut_name)
    exe = os.path.join(tmp, test)
    if test == "archivetest":
        obj = os.path.join(tmp, "lm_mz.o")
        mzobj = _miniz_obj()
        if mzobj is None or run_cmd(CXX + ["-DDL_HAVE_MINIZ", "-I", ROOT,
                                              "-c", source, "-o", obj], cwd=ROOT,
                                       stderr=subprocess.DEVNULL).returncode != 0:
            return False
        cmd = CXX + ["-DDL_HAVE_MINIZ", "-o", exe, test + ".cpp", obj, mzobj]
    else:
        obj = os.path.join(tmp, "lm.o")
        if run_cmd(CXX + ["-I", ROOT, "-c", source, "-o", obj], cwd=ROOT,
                   stderr=subprocess.DEVNULL).returncode != 0:
            return False
        cmd = CXX + ["-o", exe, test + ".cpp", obj]
    if run_cmd(cmd, cwd=ROOT, stderr=subprocess.DEVNULL).returncode != 0:
        return False
    return run_cmd([exe], cwd=ROOT, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL).returncode == 0


def run_mutation(tmp_root, orig, idx, name, old, new, total):
    """在独立临时目录运行一个变异；返回 (状态,耗时)。"""
    started = time.monotonic()
    print(f"  [{idx:02d}/{total:02d}] {name}", flush=True)
    if old not in orig:
        return "bad", time.monotonic() - started
    tmp = os.path.join(tmp_root, f"m{idx:02d}")
    os.makedirs(tmp)
    source = os.path.join(tmp, "logmodel.cpp")
    with open(source, "w", encoding="utf-8") as f:
        f.write(orig.replace(old, new, 1))
    survived = run_tests(tmp, source, name)
    return ("survived" if survived else "caught"), time.monotonic() - started


def main():
    orig = open(SRC, encoding="utf-8").read()
    tmp = tempfile.mkdtemp(prefix="diallog_mutate_")
    caught = survived = bad = 0
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
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = {}
            for idx, (name, old, new) in enumerate(MUTATIONS, 1):
                fut = pool.submit(run_mutation, tmp, orig, idx, name, old, new, len(MUTATIONS))
                futures[fut] = name
            for fut in as_completed(futures):
                status, elapsed = fut.result()
                name = futures[fut]
                if status == "bad":
                    print(f"      ⚠ {name}:变异点不存在(片段对不上) ({elapsed:.1f}s)", flush=True)
                    bad += 1
                elif status == "survived":
                    print(f"      ❌ {name}:存活 —— 测试没抓住,有洞 ({elapsed:.1f}s)", flush=True)
                    survived += 1
                else:
                    print(f"      ✅ {name}:被抓住 ({elapsed:.1f}s)", flush=True)
                    caught += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    total_elapsed = time.monotonic() - all_started
    print(f"\n════ {len(MUTATIONS)} 个变异:{caught} 被抓住,{survived} 存活,{bad} 片段失配"
          f"，总耗时 {total_elapsed:.1f}s ════", flush=True)
    if survived:
        print("存活 = 测试有洞,必须补断言(不是代码没问题)")
    if bad:
        print("片段失配 = mutate.py 的 old 串和源码对不上,不是真跳过 —— 必须修")
    return 1 if (survived or bad) else 0


if __name__ == "__main__":
    sys.exit(main())
