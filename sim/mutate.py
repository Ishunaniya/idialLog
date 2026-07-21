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

优化:变异只改 logmodel.cpp → 编一次 .o,4 个测试共享链接(全量编译 12s→链接秒级)。
用法: python3 sim/mutate.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "logmodel.cpp")
CXX  = ["g++", "-std=c++17", "-O2"]

# 每个变异:(名字, 源码里的原片段, 改坏成什么)。片段取**唯一**的核心串,避免上下文差异。
MUTATIONS = [
    # ── 分类/结论逻辑 ──
    ("CP dump 退回'见标签就报'",
     'l.tag == "CPDUMP" && icontains(l.msg, "existing CP dump")',
     'l.tag == "CPDUMP" && true'),
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
     "else if (minCsq < 10 && mWeak)",
     "else if (minCsq < 2 && mWeak)"),
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
     'if (line.compare(0, 3, "===") == 0 && line.find("Dial Log Opened") != std::string::npos) {',
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
     'long long fsize = std::strtoll(szbuf, nullptr, 8);',
     'long long fsize = std::strtoll(szbuf, nullptr, 10);'),
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
    # ── 断网引擎 open_dial 'Down:' 格式识别 —— baselinetest 靶子 ──
    ('断网漏认 open_dial 的 Down: 格式(回退只认 after)',
     'if (lo.find("network recovered") != std::string::npos) {',
     'if (false) {'),
    ('Down: 时长提取位置错(偏移5改0)',
     'size_t q = d + 5;',
     'size_t q = d + 0;'),
]

# 变异后跑的测试(全绿=变异存活=测试有洞)
TESTS = ["simtest", "hostruntest", "baselinetest", "mergetest", "archivetest", "boundarytest"]
SELFTEST_LOGS = [
    "samples/rtms_eg25/dial_20260630_000026.log",
    "samples/rtms_eg25/real_eg25_1.31.15_unsynced.log",
    "samples/rtms_ag35/real_ag35_1.32.16_console.log",
    "samples/dial_eg25/real_artery_1.29.13.log",
]


_MINIZ_CACHE = [None]   # miniz.o 只编一次(与变异无关),全局缓存路径


def _miniz_obj():
    """编译 miniz.o 一次并缓存返回路径;失败返回 None。"""
    if _MINIZ_CACHE[0] and os.path.exists(_MINIZ_CACHE[0]):
        return _MINIZ_CACHE[0]
    import tempfile
    path = os.path.join(tempfile.gettempdir(), "dl_mutate_miniz.o")
    if subprocess.run(["gcc", "-std=c11", "-O2", "-DMINIZ_NO_STDIO", "-DMINIZ_NO_TIME",
                       "-c", "miniz.c", "-o", path], cwd=ROOT,
                      stderr=subprocess.DEVNULL).returncode != 0:
        return None
    _MINIZ_CACHE[0] = path
    return path


def run_tests(tmp, mut_name=""):
    """编 logmodel.o + 链接测试 + 跑。全绿返回 True(=变异存活)。
    优化:archivetest 每次要带 miniz 重编 logmodel(慢)。只有触及 archive/BOM 代码的
    变异才需要它 —— 非 archive 变异即使 archivetest 不跑也不影响结论(它抓不到这些洞)。
    靠变异名里的关键词判断是否 archive 相关。"""
    obj = os.path.join(tmp, "lm.o")
    if subprocess.run(CXX + ["-c", SRC, "-o", obj], cwd=ROOT,
                      stderr=subprocess.DEVNULL).returncode != 0:
        return False  # 编不过 = 变异被抓住
    # 是否 archive 相关变异(名字含这些词) → 才编 archivetest
    arch_kw = ("gzip", "zip", "tar", "bom", "miniz", "解压", "压缩", "魔数", "BOM")
    is_arch = any(k.lower() in mut_name.lower() for k in arch_kw)
    obj_mz = os.path.join(tmp, "lm_mz.o")
    mzobj = _miniz_obj()
    have_mz = (is_arch and "archivetest" in TESTS and mzobj is not None
               and subprocess.run(CXX + ["-DDL_HAVE_MINIZ", "-c", SRC, "-o", obj_mz], cwd=ROOT,
                                  stderr=subprocess.DEVNULL).returncode == 0)
    run_list = [t for t in TESTS if not (t == "archivetest" and not is_arch)]
    for t in run_list + ["selftest"]:
        exe = os.path.join(tmp, t)
        if t == "archivetest":
            if not have_mz:
                return False   # archivetest 该跑却编不出 miniz obj → 视为抓住(保守)
            cmd = CXX + ["-DDL_HAVE_MINIZ", "-o", exe, t + ".cpp", obj_mz, mzobj]
        else:
            cmd = CXX + ["-o", exe, t + ".cpp", obj]
        if subprocess.run(cmd, cwd=ROOT, stderr=subprocess.DEVNULL).returncode != 0:
            return False
    for t in run_list:
        if subprocess.run([os.path.join(tmp, t)], cwd=ROOT,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
            return False
    self_exe = os.path.join(tmp, "selftest")
    for log in SELFTEST_LOGS:
        if not os.path.exists(os.path.join(ROOT, log)):
            continue
        if subprocess.run([self_exe, log], cwd=ROOT,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
            return False
    return True


PRISTINE = os.path.join(os.path.dirname(__file__), ".logmodel.pristine")


def _git_head_src():
    """从 git HEAD 取 SRC 的干净内容(权威基线)。失败返回 None。"""
    rel = os.path.relpath(SRC, ROOT)
    r = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return r.stdout.decode("utf-8") if r.returncode == 0 else None


def main():
    # 崩溃安全:变异会改 SRC,进程若被 SIGKILL(超时 kill 无法捕获)会把变异态留在磁盘,
    # 下次 git add -A 就可能提交坏解析器。
    #
    # 【教训】旧自愈只在"SRC 含特定标记(ZZ 等)"时才还原 —— 非标记型变异(如把
    # `return a.hasT` 改成 `return !a.hasT`)残留时认不出,且会把坏内容再存进 PRISTINE,
    # 污染叠加。现改为:
    #   ① 优先信任 git HEAD 作为基线(权威,不会被上次运行污染);
    #   ② 只要磁盘 SRC 与基线**逐字节不一致**就还原,不再匹配任何标记;
    #   ③ PRISTINE 仅作 git 不可用时的兜底,且写入前先用 git 校验过。
    baseline = _git_head_src()
    if baseline is not None:
        disk = open(SRC, encoding="utf-8").read()
        if disk != baseline:
            # 与 HEAD 不一致:可能是上次残留,也可能是**未提交的正当改动**。
            # 无法区分,保守起见提示并中止,让用户自己确认 —— 绝不静默覆盖用户改动。
            print("⚠ SRC 与 git HEAD 不一致。若这是上次变异残留,请手动还原:")
            print(f"    git checkout -- {os.path.relpath(SRC, ROOT)}")
            print("  若这是你未提交的正当改动,请先 commit 或 stash 再跑变异测试。")
            sys.exit(2)
    elif os.path.exists(PRISTINE):
        # git 不可用的兜底:PRISTINE 是上次由本脚本(经校验后)落盘的
        disk = open(SRC, encoding="utf-8").read()
        pris = open(PRISTINE, encoding="utf-8").read()
        if disk != pris:
            print("⚠ 检测到 SRC 与 PRISTINE 不一致,还原(git 不可用,用兜底副本)")
            open(SRC, "w", encoding="utf-8").write(pris)

    orig = open(SRC, encoding="utf-8").read()
    open(PRISTINE, "w", encoding="utf-8").write(orig)   # 落盘原始副本

    import signal
    def _restore(*_):
        open(SRC, "w", encoding="utf-8").write(orig)
        os._exit(2)
    signal.signal(signal.SIGTERM, _restore)
    signal.signal(signal.SIGINT, _restore)

    tmp = tempfile.mkdtemp()
    caught = survived = bad = 0
    print("════ 变异测试:把修过的 bug 故意改回去,看测试抓不抓得住 ════")
    try:
        for name, old, new in MUTATIONS:
            if old not in orig:
                print(f"  {name:<40s} ⚠ 变异点不存在(片段对不上,修 mutate.py)")
                bad += 1
                continue
            open(SRC, "w", encoding="utf-8").write(orig.replace(old, new, 1))
            survived_now = run_tests(tmp, name)
            open(SRC, "w", encoding="utf-8").write(orig)  # 立即还原
            if survived_now:
                print(f"  {name:<40s} ❌ 存活 —— 测试没抓住,有洞")
                survived += 1
            else:
                print(f"  {name:<40s} ✅ 被抓住")
                caught += 1
    finally:
        open(SRC, "w", encoding="utf-8").write(orig)  # 兜底还原
        shutil.rmtree(tmp, ignore_errors=True)
        try: os.remove(PRISTINE)  # 正常结束才删副本;异常退出时保留它供下次自愈
        except OSError: pass

    print(f"\n════ {len(MUTATIONS)} 个变异:{caught} 被抓住,{survived} 存活,{bad} 片段失配 ════")
    if survived:
        print("存活 = 测试有洞,必须补断言(不是代码没问题)")
    if bad:
        print("片段失配 = mutate.py 的 old 串和源码对不上,不是真跳过 —— 必须修")
    return 1 if (survived or bad) else 0


if __name__ == "__main__":
    sys.exit(main())
