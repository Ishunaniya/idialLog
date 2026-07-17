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
]

# 变异后跑的测试(全绿=变异存活=测试有洞)
TESTS = ["simtest", "hostruntest", "baselinetest"]
SELFTEST_LOGS = [
    "samples/rtms_eg25/dial_20260630_000026.log",
    "samples/rtms_eg25/real_eg25_1.31.15_unsynced.log",
    "samples/rtms_ag35/real_ag35_1.32.16_console.log",
    "samples/dial_eg25/real_artery_1.29.13.log",
]


def run_tests(tmp):
    """编 logmodel.o + 链接 4 个测试 + 跑。全绿返回 True(=变异存活)。"""
    obj = os.path.join(tmp, "lm.o")
    if subprocess.run(CXX + ["-c", SRC, "-o", obj], cwd=ROOT,
                      stderr=subprocess.DEVNULL).returncode != 0:
        return False  # 编不过 = 变异被抓住
    for t in TESTS + ["selftest"]:
        exe = os.path.join(tmp, t)
        if subprocess.run(CXX + ["-o", exe, t + ".cpp", obj], cwd=ROOT,
                          stderr=subprocess.DEVNULL).returncode != 0:
            return False
    for t in TESTS:
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


def main():
    orig = open(SRC, encoding="utf-8").read()
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
            survived_now = run_tests(tmp)
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

    print(f"\n════ {len(MUTATIONS)} 个变异:{caught} 被抓住,{survived} 存活,{bad} 片段失配 ════")
    if survived:
        print("存活 = 测试有洞,必须补断言(不是代码没问题)")
    if bad:
        print("片段失配 = mutate.py 的 old 串和源码对不上,不是真跳过 —— 必须修")
    return 1 if (survived or bad) else 0


if __name__ == "__main__":
    sys.exit(main())
