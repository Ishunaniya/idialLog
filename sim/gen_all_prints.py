#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""穷举四套产品真实构建源码中的全部输出调用，生成覆盖夹具和可追溯清单。

完整解析跨行调用和相邻 C 字符串；剥除注释、预处理指令及 #if 0 死代码。
覆盖 dial_log/SEAS_LOG_*、LOG_*/QLOG*/ALOG*、Android/syslog、
printf/perror/stdout/stderr 及 C++ iostream；同时输出逐调用点和唯一形态清单。
"""
from __future__ import annotations

import argparse
import ast
import dataclasses
import re
import subprocess
import sys
from pathlib import Path
from typing import Sequence

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
APIS = {
    "dial_log", "print", "printf", "vprintf", "fprintf", "vfprintf", "puts", "perror",
    "fputs", "dprintf", "vdprintf", "syslog", "__android_log_print",
    "ql_sys_log_print",
}
LOG_API = re.compile(r"(?:SEAS_LOG_[A-Z0-9_]+|LOG_[A-Z0-9_]+|QLOG[A-Z0-9_]*|ALOG[A-Z0-9_]*)\Z")
SUSPECT_API = re.compile(r".*(?:log|print|trace|debug|warn|error|fatal|dump).*", re.I)
# 反向扫描中明确判定为“返回/格式化/配置/包装”，而非新的输出通道。
NON_OUTPUT_SUSPECTS = {
    "asprintf", "vasprintf", "snprintf", "vsnprintf", "sprintf", "ferror",
    "strerror", "gai_strerror", "nn_strerror", "get_error_msg", "cJSON_GetErrorPtr",
    "cJSON_Print", "cJSON_PrintBuffered", "cJSON_PrintPreallocated",
    "cJSON_PrintUnformatted", "runtime_error", "dictionary_dump", "iniparser_dump",
    "iniparser_dump_ini", "iniparser_dumpsection_ini", "SEAS_LOG_ENABLED",
    "diag_report_modem_log_media", "dial_log_heartbeat", "dial_log_init_static_info",
    "dial_log_modem_info", "if_traffic_monitor_log_heartbeat", "migrate_legacy_flat_logs",
    "open_log_for_today", "pid_is_logcat", "remove_logdir", "setup_cp_dump_capture",
    "teardown_cp_dump_capture", "write_version_log", "seas_emit_log", "seas_log_config",
    "seas_log_config_init", "seas_log_enable_log_file", "seas_log_get_level",
    "seas_log_get_max_level", "seas_log_get_min_level", "seas_log_init", "seas_log_lock",
    "seas_log_release", "seas_log_set_file_path", "seas_log_set_level",
    "seas_log_set_max_file_size", "seas_log_uninit", "seas_log_unlock",
    "seas_open_log_file", "seas_rotate_log_file", "data_call_service_error_cb",
    "item_ql_data_call_set_service_error_cb", "item_ql_nw_set_service_error_cb",
    "ql_data_call_set_service_error_cb", "ql_nw_set_service_error_cb",
    "log_close", "log_init", "log_recovery_snapshot", "log_sim_disconnect_diag",
    "log_sim_recovery_diag", "log_to_file", "print_config", "print_init_info",
    "logSetTag", "log_bad_request", "printTrafficData", "print_array", "print_number",
    "print_object", "print_string", "print_string_ptr", "print_value",
}


@dataclasses.dataclass(frozen=True)
class Token:
    value: str
    line: int
    kind: str


@dataclasses.dataclass(frozen=True)
class Call:
    product: str
    ref: str
    path: str
    line: int
    api: str
    channel: str
    fmt: str
    dynamic: bool

    def shape(self):
        return self.product, self.channel, self.api, self.fmt


def git(repo: str, *args: str) -> str:
    p = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(p.stderr.strip() or "git 命令失败")
    return p.stdout


def all_refs(repo: str) -> list[str]:
    values = [x for x in git(repo, "for-each-ref", "--format=%(refname:short)",
                             "refs/heads", "refs/remotes").splitlines()
              if x and not x.endswith("/HEAD")]
    head = git(repo, "branch", "--show-current").strip()
    if head in values:
        values.remove(head)
        values.insert(0, head)
    return values


def product_of(repo: str) -> str:
    name = Path(repo).name
    names = {"modem_mng": "modem_mng", "open_dial": "open_dial",
             "open_dial_for_artery": "artery"}
    if name not in names:
        raise ValueError(f"不支持的产品仓库：{repo}")
    return names[name]


def in_scope(product: str, path: str) -> bool:
    if Path(path).suffix.lower() not in SOURCE_SUFFIXES:
        return False
    if path.endswith((".bak", "_todel")) or "/tests/" in path or "/sim/" in path:
        return False
    if product == "modem_mng":
        if path in {"dialer_imx6ull.cpp", "imx_led.hpp"}:
            return False
        return (path.startswith(("ec200a/", "eg25/", "roamlink/", "status/",
                                 "cc_deque/", "traffic_sql/", "fault_report/")) or
                path in {"dialer_ec200a.cpp", "dialer_eg25.c", "logger_sd.c",
                         "nanomsg_process.cpp", "nanomsg_process_wraper.cpp"} or
                ("/" not in path and Path(path).suffix.lower() in {".h", ".hh", ".hpp", ".hxx"}))
    if product == "artery":
        return path == "main.c" or path.startswith((
            "src/sim/", "src/nw/", "src/apn/", "src/at/", "src/dial/",
            "src/cc_deque/", "src/tz/", "src/opt_iniparser/", "src/seas_log/",
            "src/json/", "src/roamlink/", "src/status/"))
    return not path.startswith(("thirdparty/", "third_party/"))


def variants(product: str, path: str) -> list[str]:
    if product != "modem_mng":
        return [product]
    if path.startswith("ec200a/") or path.startswith("fault_report/") or path == "dialer_ec200a.cpp":
        return ["modem_ec200a_ag35"]
    if (path.startswith(("eg25/", "roamlink/", "status/", "cc_deque/")) or
            path == "dialer_eg25.c"):
        return ["modem_eg25"]
    # logger/nanomsg/traffic_sql 及公共头同时编入两个 target。
    return ["modem_ec200a_ag35", "modem_eg25"]


def mask_comments(text: str) -> str:
    out = list(text)
    i, state = 0, "code"
    while i < len(text):
        c = text[i]
        if state == "code":
            if c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            elif text.startswith("//", i):
                out[i] = out[i + 1] = " "
                i += 1
                state = "line"
            elif text.startswith("/*", i):
                out[i] = out[i + 1] = " "
                i += 1
                state = "block"
        elif state in {"string", "char"}:
            if c == "\\" and i + 1 < len(text):
                i += 1
            elif (state == "string" and c == '"') or (state == "char" and c == "'"):
                state = "code"
        elif state == "line":
            if c == "\n":
                state = "code"
            else:
                out[i] = " "
        elif state == "block":
            if text.startswith("*/", i):
                out[i] = out[i + 1] = " "
                i += 1
                state = "code"
            elif c != "\n":
                out[i] = " "
        i += 1
    return "".join(out)


def mask_preprocessor(text: str) -> str:
    lines = text.splitlines(keepends=True)
    # -1=这个常量分支确定不编译，0=条件未知（两边都取），1=确定编译。
    branch_state: list[int] = []
    continuation = False
    out = []
    for raw in lines:
        s = raw.lstrip()
        directive = continuation or s.startswith("#")
        if s.startswith("#"):
            body = s[1:].strip()
            if re.match(r"if\s+0(?:\D|$)", body):
                branch_state.append(-1)
            elif re.match(r"if\s+1(?:\D|$)", body):
                branch_state.append(1)
            elif re.match(r"if(?:def|ndef)?\b", body):
                branch_state.append(0)
            elif re.match(r"else\b", body) and branch_state:
                branch_state[-1] = -branch_state[-1]
            elif re.match(r"elif\b", body) and branch_state:
                if branch_state[-1] == -1:
                    branch_state[-1] = 0
                elif branch_state[-1] == 1:
                    branch_state[-1] = -1
            elif re.match(r"endif\b", body) and branch_state:
                branch_state.pop()
        dead = any(state == -1 for state in branch_state)
        out.append("\n" if (directive or dead) and raw.endswith("\n")
                   else ("" if directive or dead else raw))
        continuation = directive and raw.rstrip("\r\n").endswith("\\")
    return "".join(out)


def tokenize(text: str) -> list[Token]:
    text = mask_preprocessor(mask_comments(text))
    out: list[Token] = []
    i = 0
    line = 1
    while i < len(text):
        c = text[i]
        if c.isspace():
            line += c == "\n"
            i += 1
            continue
        start_line = line
        prefix = 2 if text.startswith('u8"', i) else (
            1 if c in "uUL" and i + 1 < len(text) and text[i + 1] == '"' else 0)
        if c == '"' or prefix:
            start = i
            i += prefix + 1
            while i < len(text):
                if text[i] == "\\" and i + 1 < len(text):
                    line += text[i + 1] == "\n"
                    i += 2
                elif text[i] == '"':
                    i += 1
                    break
                else:
                    line += text[i] == "\n"
                    i += 1
            out.append(Token(text[start:i], start_line, "string"))
        elif c == "'":
            start = i
            i += 1
            while i < len(text):
                if text[i] == "\\" and i + 1 < len(text):
                    i += 2
                elif text[i] == "'":
                    i += 1
                    break
                else:
                    i += 1
            out.append(Token(text[start:i], start_line, "char"))
        elif c.isalpha() or c == "_":
            start = i
            i += 1
            while i < len(text) and (text[i].isalnum() or text[i] == "_"):
                i += 1
            out.append(Token(text[start:i], start_line, "ident"))
        else:
            out.append(Token(c, start_line, "punct"))
            i += 1
    return out


def arguments(tokens: Sequence[Token], open_at: int):
    args: list[list[Token]] = [[]]
    stack = [")"]
    pairs = {"(": ")", "[": "]", "{": "}"}
    i = open_at + 1
    while i < len(tokens):
        v = tokens[i].value
        if v in pairs:
            stack.append(pairs[v])
            args[-1].append(tokens[i])
        elif v == stack[-1]:
            stack.pop()
            if not stack:
                return args, i
            args[-1].append(tokens[i])
        elif v == "," and len(stack) == 1:
            args.append([])
        else:
            args[-1].append(tokens[i])
        i += 1
    return None


def decode_string(value: str) -> str:
    value = re.sub(r"^(?:u8|u|U|L)", "", value)
    try:
        return ast.literal_eval(value)
    except (SyntaxError, ValueError):
        return value[1:-1]


def get_format(arg: Sequence[Token]) -> tuple[str, bool]:
    pieces = []
    started = False
    for token in arg:
        if token.kind == "string":
            pieces.append(decode_string(token.value))
            started = True
        elif started and token.kind == "ident" and re.fullmatch(
                r"PRI[diouxX](?:8|16|32|64|MAX|PTR)", token.value):
            pieces.append("%lld")
        elif started and token.value not in {"(", ")"}:
            break
    if pieces:
        return "".join(pieces), False
    raw = "".join(t.value for t in arg)
    return f"<dynamic:{raw[:120]}>", True


def iostream_call(product: str, ref: str, path: str, tokens: Sequence[Token],
                  start: int) -> tuple[Call, int] | None:
    """解析 std::cout/cerr/clog << ...；返回完整拼接形态而非只取首段。"""
    i = start
    if (i + 3 < len(tokens) and tokens[i].value == "std" and
            tokens[i + 1].value == ":" and tokens[i + 2].value == ":" and
            tokens[i + 3].value in {"cout", "cerr", "clog"}):
        stream = tokens[i + 3].value
        i += 4
    elif tokens[i].value in {"cout", "cerr", "clog"}:
        stream = tokens[i].value
        i += 1
    else:
        return None
    if i + 1 >= len(tokens) or tokens[i].value != "<" or tokens[i + 1].value != "<":
        return None
    operands: list[list[Token]] = [[]]
    depth = 0
    i += 2
    while i < len(tokens):
        value = tokens[i].value
        if value in {"(", "[", "{"}:
            depth += 1
        elif value in {")", "]", "}"} and depth:
            depth -= 1
        if value == ";" and depth == 0:
            break
        if (i + 1 < len(tokens) and value == "<" and tokens[i + 1].value == "<"
                and depth == 0):
            operands.append([])
            i += 2
            continue
        operands[-1].append(tokens[i])
        i += 1
    pieces = []
    for operand in operands:
        strings = [decode_string(t.value) for t in operand if t.kind == "string"]
        if strings:
            pieces.append("".join(strings))
        elif any(t.value == "endl" for t in operand):
            pieces.append("\n")
        elif operand:
            pieces.append("%s")
    fmt = "".join(pieces)
    dynamic = False
    if not fmt:
        fmt = "<dynamic:iostream>"
        dynamic = True
    return Call(product, ref, path, tokens[start].line, f"std::{stream}",
                "console", fmt, dynamic), i


def has(arg: Sequence[Token], *names: str) -> bool:
    return any(t.value in names for t in arg)


def spec(api: str, args: Sequence[Sequence[Token]]):
    if api == "dial_log":
        return "sd", 0
    if api.startswith("SEAS_LOG_"):
        return "seas", 0
    if api in {"print", "printf", "vprintf", "puts", "perror"}:
        return "console", 0
    if api in {"fprintf", "vfprintf", "fputs"}:
        if not args or not has(args[0], "stdout", "stderr"):
            return None
        return "console", 0 if api == "fputs" else 1
    if api in {"dprintf", "vdprintf"}:
        if not args or not has(args[0], "1", "2", "STDOUT_FILENO", "STDERR_FILENO"):
            return None
        return "console", 1
    if api == "syslog":
        return "syslog", 1
    if api == "__android_log_print":
        return "android", 2
    if api == "ql_sys_log_print":
        return "android", 1
    if api.startswith("QLOG"):
        return "android", 1
    if api.startswith("ALOG"):
        return "android", 0
    if api.startswith("LOG_"):
        return ("android", 1) if args and has(args[0], "NW_LOG_TAG", "LOG_TAG") else ("android", 0)
    return None


def extract_text(product: str, ref: str, path: str, text: str) -> list[Call]:
    tokens = tokenize(text)
    out = []
    i = 0
    while i + 1 < len(tokens):
        stream_call = iostream_call(product, ref, path, tokens, i)
        if stream_call is not None:
            call, close = stream_call
            out.append(call)
            i = close + 1
            continue
        api = tokens[i].value
        is_call = tokens[i].kind == "ident" and tokens[i + 1].value == "("
        known = api in APIS or LOG_API.fullmatch(api)
        suspicious = is_call and SUSPECT_API.fullmatch(api)
        if not is_call or (not known and not suspicious):
            i += 1
            continue
        parsed = arguments(tokens, i + 1)
        if parsed is None:
            i += 1
            continue
        args, close = parsed
        # 函数定义不是调用点（旧脚本会把 dial_log(const char *fmt, ...) 自身算进去）。
        if close + 1 < len(tokens) and tokens[close + 1].value == "{":
            i = close + 1
            continue
        if not known:
            if api not in NON_OUTPUT_SUSPECTS:
                out.append(Call(product, ref, path, tokens[i].line, api,
                                "unclassified", f"<unclassified:{api}>", True))
            i = close + 1
            continue
        selected = spec(api, args)
        if selected is not None and selected[1] < len(args):
            channel, index = selected
            fmt, dynamic = get_format(args[index])
            out.append(Call(product, ref, path, tokens[i].line, api, channel, fmt, dynamic))
        i = close + 1
    return out


def extract(repo: str) -> list[Call]:
    product = product_of(repo)
    out = []
    prefix = git(repo, "rev-parse", "--show-prefix").strip()
    for ref in all_refs(repo):
        paths = []
        for relative in git(repo, "ls-tree", "-r", "--name-only", ref).splitlines():
            if in_scope(product, relative):
                full_path = prefix + relative
                paths.append((full_path, relative))
        for full_path, path in paths:
            raw = subprocess.run(["git", "-C", repo, "show", f"{ref}:{full_path}"],
                                 capture_output=True)
            if not raw.returncode:
                text = raw.stdout.decode("utf-8", errors="replace")
                for variant in variants(product, path):
                    out.extend(extract_text(variant, ref, path, text))
    return out


PRINTF = re.compile(
    r"%(?:\d+\$)?[-+ #0']*(?:\*|\d+)?(?:\.(?:\*|\d+))?"
    r"(?:hh|h|ll|l|j|z|t|L)?([diuoxXfFeEgGaAcspn%])")


def fill(fmt: str) -> str:
    if fmt.startswith("<dynamic:"):
        return "DYNAMIC OUTPUT (format determined at runtime)"
    fmt = re.sub(r"((?:rsp|response):\s*)%[-+ #0'.*0-9hljztL]*s", r"\1\nOK", fmt)

    def value(m):
        k = m.group(1)
        if k == "%": return "%"
        if k in "diuoxX": return "1"
        if k in "fFeEgGaA": return "1.0"
        if k == "c": return "X"
        if k == "s": return "VALUE"
        if k == "p": return "0x1"
        return ""
    return PRINTF.sub(value, fmt).rstrip("\r\n")


def shapes(calls: Sequence[Call]) -> list[Call]:
    unique = {}
    for call in calls:
        unique.setdefault(call.shape(), call)
    return sorted(unique.values(), key=lambda c: c.shape())


def write_manifest(path: Path, calls: Sequence[Call]) -> None:
    rows = ["product\tchannel\tapi\tdynamic\tref\tpath\tline\tformat"]
    for c in calls:
        f = (c.fmt.replace("\\", "\\\\").replace("\t", "\\t")
             .replace("\r", "\\r").replace("\n", "\\n"))
        rows.append(f"{c.product}\t{c.channel}\t{c.api}\t{int(c.dynamic)}\t"
                    f"{c.ref}\t{c.path}\t{c.line}\t{f}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")


def generate(calls: Sequence[Call], out: Path):
    lines = []
    counts = {}
    for t, call in enumerate(shapes(calls)):
        body = fill(call.fmt)
        physical = body.split("\n")
        first, rest = physical[0], physical[1:]
        h, minute, second = (t // 3600) % 24, (t % 3600) // 60, t % 60
        counts[call.channel] = counts.get(call.channel, 0) + 1
        if call.channel == "sd":
            lines.append(f"[2026-07-17 {h:02d}:{minute:02d}:{second:02d}] {first}")
            lines.extend(rest)
        elif call.channel == "seas":
            lines.append(f"2026-07-17 {h:02d}:{minute:02d}:{second:02d}.{t % 1000:03d} "
                         f"[INFO] \x1b[0msource_audit ({Path(call.path).name}:{call.line}) - {first}")
            lines.extend(rest)
        elif call.channel == "android":
            lines.append(f"2026-07-17 {h:02d}:{minute:02d}:{second:02d}.000  "
                         f"1000  1000 I DIAL: {first}")
            lines.extend(rest)
        elif call.channel == "syslog":
            lines.append(f"2026-07-17T{h:02d}:{minute:02d}:{second:02d} "
                         f"device modem_mng[1000]: {first}")
            lines.extend(rest)
        else:
            lines.extend(physical)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(lines), counts


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo")
    parser.add_argument("out")
    parser.add_argument("--manifest")
    parser.add_argument("--calls-manifest")
    args = parser.parse_args(argv)
    repo = str(Path(args.repo).resolve())
    try:
        calls = extract(repo)
    except (RuntimeError, ValueError) as exc:
        parser.error(str(exc))
    if not calls:
        parser.error("没有提取到产品输出调用")
    unique = shapes(calls)
    manifest = Path(args.manifest or (args.out + ".manifest.tsv"))
    write_manifest(manifest, unique)
    calls_manifest = Path(args.calls_manifest or (args.out + ".calls.tsv"))
    write_manifest(calls_manifest, sorted(
        calls, key=lambda c: (c.product, c.ref, c.path, c.line, c.api, c.fmt)))
    line_count, counts = generate(calls, Path(args.out))
    print(f"仓库          : {repo}")
    print(f"产品          : {product_of(repo)}")
    print(f"分支          : {len(all_refs(repo))} 个（取条件/分支并集）")
    print(f"输出调用实例  : {len(calls)} 个（跨分支，未去重）")
    print(f"唯一输出形态  : {len(unique)} 个")
    by_product = {}
    for call in unique:
        by_product[call.product] = by_product.get(call.product, 0) + 1
    print("四份代码      : " + ", ".join(f"{k}={v}" for k, v in sorted(by_product.items())))
    print("通道          : " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    print(f"动态格式      : {sum(c.dynamic for c in unique)} 个")
    print(f"生成          : {args.out}（{line_count} 行）")
    print(f"唯一形态清单  : {manifest}")
    print(f"逐调用点清单  : {calls_manifest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
