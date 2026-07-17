#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_all_prints.py — 从源码**穷举**某仓库能打出的全部 dial_log 串,生成"全打印"覆盖夹具。

目的:回答"工具是不是把该仓库所有日志都能解析" —— 用可度量的覆盖率,而不是嘴上说。
做法:git grep 出所有 dial_log("...") 的格式串(**逐字取自源码,非我复述**),
      跨该仓库全部分支取并集,填充占位符后逐条生成一行日志,喂给解析器。
      解析器必须做到 **0 未识别**;否则就是真漏了。

⚠️ 循环论证的边界(必须清楚):
  - ✅ **格式串本身**取自源码,不是我复述 —— 这部分不循环。
  - ⚠️ **占位符(%s/%d/...)的替换值是我选的**。这是有实据的风险点:真机日志里
       "[RECOVERY L2] AT+CFUN=0 rsp: %s" 的 %s 实际是**多行**的 "\\nOK\\n",
       而我原先没料到,导致续行被当成未识别丢弃(2026-07-17 由真机日志抓出)。
       故本脚本对 "rsp: %s"/"response: %s" 专门还原多行形态。
       但仍不能排除还有别的 %s 在真机上是我没想到的形态 —— **真机日志不可替代**。

用法:
    python3 sim/gen_all_prints.py /home/tronlong/lyp/code/open_dial samples/sim/open_dial_all_prints.log
"""
import re
import subprocess
import sys
from pathlib import Path


def branches(repo: str):
    out = subprocess.run(
        ["git", "-C", repo, "for-each-ref", "--format=%(refname:short)",
         "refs/heads", "refs/remotes"],
        capture_output=True, text=True).stdout.split()
    return [b for b in out if "HEAD" not in b]


# 两套日志体系(实证):
#   dial_log(...)        —— modem_mng / open_dial(logger_sd.c)
#   SEAS_LOG_XXX(...)    —— open_dial_for_artery(src/seas_log/seas_log.c),dial_log 数为 0
# 注意两套正则不能混用:git grep -E 是 **POSIX ERE**,不支持 (?:...) 和 \s,
# 传进去会直接 fatal: Invalid preceding regular expression 且静默 0 命中。
GREP_ERE = r'(dial_log|SEAS_LOG_[A-Z]+)[[:space:]]*\("'          # 给 git grep -E
PY_RE    = r'(dial_log|SEAS_LOG_[A-Z]+)\s*\("((?:[^"\\]|\\.)*)"'  # 给 python re


def extract(repo: str):
    """跨全部分支取日志格式串的并集。返回 {串: 最早见到它的 branch:file:line}"""
    found = {}
    for br in branches(repo):
        r = subprocess.run(
            ["git", "-C", repo, "grep", "-nE", GREP_ERE, br],
            capture_output=True, text=True)
        for line in r.stdout.splitlines():
            # <ref>:<file>:<lineno>:<code>
            parts = line.split(":", 3)
            if len(parts) < 4:
                continue
            ref, path, lineno, code = parts
            for m in re.finditer(PY_RE, code):
                s = m.group(2)
                if s and s not in found:
                    found[s] = f"{ref}:{path}:{lineno}"
    return found


def fill(fmt: str) -> str:
    """把 C 格式串还原成一条具体日志。替换值是本脚本的选择(见顶部风险说明)。"""
    s = fmt.replace("\\r\\n", "\n").replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"')

    # AT 应答:真机上这个 %s 是**多行**的("\nOK\n")—— 由真机日志实证,不可简化成单行。
    # 必须只替换紧跟在 "rsp: "/"response: " 后面的那个 %s。
    # (踩过的坑:早先用 re.sub('%s', ..., count=1) 替换"第一个"%s,
    #  遇到 "[OPER] Selected operator %s, response: %s"(eg25/dial/dial.c:1568)这种
    #  有两个 %s 的串时,把多行值塞进了运营商名那个,生成出畸形日志 ——
    #  是生成器的 bug,不是解析器的。)
    s = re.sub(r'((?:rsp|response):\s*)%s', r'\1\nOK', s)

    # 其余占位符:按类型给一个形态合理的值
    s = re.sub(r'%lds', '60s', s)
    s = re.sub(r'%ld',  '60',  s)
    s = re.sub(r'%dms', '10000ms', s)
    s = re.sub(r'%ds',  '10s',  s)
    s = re.sub(r'%02d', '01',  s)
    s = re.sub(r'%[-0-9.]*d', '1', s)
    s = re.sub(r'%[-0-9.]*u', '1', s)
    s = re.sub(r'%[-0-9.]*[fg]', '1.0', s)
    s = re.sub(r'%[-0-9.]*x', '0x1a', s)
    s = re.sub(r'%[-0-9.]*s', 'VALUE', s)
    s = re.sub(r'%%', '%', s)
    return s.rstrip("\n")


def main():
    if len(sys.argv) < 3:
        sys.exit("用法: gen_all_prints.py <repo> <out.log>")
    repo, out = sys.argv[1], sys.argv[2]

    strings = extract(repo)
    if not strings:
        sys.exit(f"没从 {repo} 提取到任何 dial_log 串")

    name = Path(repo).name
    # artery(seas_log)行格式与 dial_log 完全不同,须按其真实格式生成:
    #   "YYYY-MM-DD HH:MM:SS.mmm [LEVEL] \x1b[0m func (file:line) - msg"
    #   (seas_log.c:233-296;SEAS_DISPLAY_COLOR=0 无色码,但 RESET=1 故必带一个 ESC[0m)
    seas = "for_artery" in name
    lines = [] if seas else [f"=== Dial Log Opened [2026-07-17 00:00:00] daykey=2026-07-17 ==="]
    t = 0
    for fmt in sorted(strings):
        body = fill(fmt)
        if not body.strip():
            continue
        h, m, sec = t // 3600, (t % 3600) // 60, t % 60
        first, *rest = body.split("\n")
        if seas:
            ts = f"2026-07-17 {h:02d}:{m:02d}:{sec:02d}.{t % 1000:03d}"
            lines.append(f"{ts} [INFO] \x1b[0mdial_task (dial.c:{100 + t}) - {first}")
        else:
            ts = f"2026-07-17 {h:02d}:{m:02d}:{sec:02d}"
            lines.append(f"[{ts}] {first}")
        lines.extend(rest)          # 多行条目的续行(无时间戳),真机就是这样
        t += 1

    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"仓库      : {repo}")
    print(f"分支      : {len(branches(repo))} 个(取并集)")
    print(f"唯一日志串: {len(strings)} 条 —— 逐字取自源码")
    print(f"生成       : {out}({len(lines)} 行)")
    print(f"\n用 ./selftest {out} 验证:未识别必须为 0,否则就是真漏了。")


if __name__ == "__main__":
    main()
