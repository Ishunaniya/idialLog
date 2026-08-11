#!/bin/bash
# check_coverage.sh — 全打印覆盖校验:某仓库能打出的**每一条** dial_log 串,
# 工具是否都能解析、标签是否都能认出。
#
# 这是对"是不是全部了"的**可度量**回答,不是嘴上说。
#
# 边界(见 gen_all_prints.py 顶部):格式串逐字取自源码(不循环);但占位符的替换值
# 由脚本选择(有循环风险,实证:真机 "rsp: %s" 的 %s 是多行的,曾被猜漏)。
# 故本校验能证明"源码里的串工具都认得",**不能**证明"真机日志工具都认得"。
#
# 用法: sim/check_coverage.sh <repo路径> [<out.log>]
set -u
REPO="${1:?用法: sim/check_coverage.sh <repo路径> [out.log]}"
NAME=$(basename "$REPO")
OUT="${2:-samples/sim/${NAME}_all_prints.log}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

SELFTEST="build/tests/unit/selftest"
test -x "$SELFTEST" || make selftest >/dev/null 2>&1 || { echo "selftest 构建失败"; exit 1; }

echo "════ 全打印覆盖校验: $NAME ════"
python3 sim/gen_all_prints.py "$REPO" "$OUT" | sed 's/^/  /'
echo

OUTPUT=$("$SELFTEST" "$OUT" 2>&1) || { echo "❌ selftest 执行失败"; exit 1; }

# 1) 未识别必须为 0
UNP=$(echo "$OUTPUT" | grep -oE '未识别:[0-9]+' | head -1 | tr -dc '0-9')
echo "$OUTPUT" | grep -E '自洽校验' | sed 's/^/  /'
if [ "${UNP:-1}" != "0" ]; then
    echo "  ❌ 有 $UNP 条源码日志串解析不了 —— 真漏了"
    echo "$OUTPUT" | grep -A5 '未识别分类' | sed 's/^/     /'
    exit 1
fi
echo "  ✅ 未识别 0 —— 源码里的每一条串都能解析"

# 2) 标签必须全认出
git -C "$REPO" for-each-ref --format='%(refname:short)' refs/heads refs/remotes 2>/dev/null \
  | grep -v HEAD | while read -r br; do
      git -C "$REPO" grep -ohE '(dial_log|SEAS_LOG_[A-Z]+)[[:space:]]*\("\[[A-Za-z0-9_ ]+\]' "$br" 2>/dev/null
    done | sed -E 's/^.*\("\[//; s/\]$//' | sort -u > "$TMP/src.txt"

echo "$OUTPUT" | sed -n '/== 标签 ==/,/== 关键事件/p' | grep -E '^  \S' \
  | sed -E 's/[[:space:]]+[0-9]+[[:space:]]*$//; s/^  //' | grep -v '^(无标签)' | sort -u > "$TMP/tool.txt"

MISS=$(comm -23 "$TMP/src.txt" "$TMP/tool.txt")
echo "  源码标签 $(wc -l < "$TMP/src.txt") 个 / 工具认出 $(wc -l < "$TMP/tool.txt") 个"
if [ -n "$MISS" ]; then
    echo "  ❌ 以下标签工具认不出:"; echo "$MISS" | sed 's/^/     /'
    exit 1
fi
echo "  ✅ 标签全部认出,无遗漏"
echo
echo "════ $NAME 覆盖校验通过 ════"
