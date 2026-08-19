#!/bin/bash
# 四产品全打印覆盖：完整源码调用清单 ↔ 生成夹具 ↔ 解析结果三方对账。
set -eu
REPO="${1:?用法: sim/check_coverage.sh <repo路径> [out.log]}"
NAME=$(basename "$REPO")
OUT="${2:-samples/sim/${NAME}_all_prints.log}"
MANIFEST="${OUT}.manifest.tsv"
CALLS_MANIFEST="${OUT}.calls.tsv"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

SELFTEST="build/tests/unit/selftest"
test -x "$SELFTEST" || make selftest >/dev/null

echo "════ 全打印调用点覆盖校验: $NAME ════"
python3 sim/gen_all_prints.py "$REPO" "$OUT" --manifest "$MANIFEST" \
    --calls-manifest "$CALLS_MANIFEST" | sed 's/^/  /'
echo

CALLS=$(($(wc -l < "$CALLS_MANIFEST") - 1))
if [ "$CALLS" -le 0 ]; then
    echo "  ❌ 逐调用点清单为空"
    exit 1
fi
echo "  ✅ $CALLS 个分支调用实例逐项落入 calls.tsv"

UNCLASSIFIED=$(awk -F '\t' 'NR>1 && $2=="unclassified" {n++} END {print n+0}' "$MANIFEST")
if [ "$UNCLASSIFIED" != "0" ]; then
    echo "  ❌ 发现 $UNCLASSIFIED 个疑似输出 API 尚未分类:"
    awk -F '\t' 'NR>1 && $2=="unclassified" {print "     "$5":"$6":"$7" "$3}' "$MANIFEST"
    exit 1
fi

OUTPUT=$("$SELFTEST" "$OUT" 2>&1) || {
    echo "❌ selftest 执行失败"
    echo "$OUTPUT"
    exit 1
}
echo "$OUTPUT" | grep -E '自洽校验' | sed 's/^/  /'

STRUCTURED=$(awk -F '\t' 'NR>1 && $2!="console" {n++} END {print n+0}' "$MANIFEST")
PARSED=$(echo "$OUTPUT" | sed -n '/== 未识别行审计 ==/{n;p;}' |
    grep -oE '已解析:[0-9]+' | tr -dc '0-9')
if [ "$PARSED" != "$STRUCTURED" ]; then
    echo "  ❌ 结构化输出对账失败:源码 $STRUCTURED 种,解析器认出 ${PARSED:-0} 种"
    exit 1
fi
echo "  ✅ 结构化输出 $STRUCTURED/$STRUCTURED 全部解析"

# 裸 printf/iostream 没有真实时间戳，不能伪造成结构化日志；但每一行都保留。
UNPARSED=$(echo "$OUTPUT" | sed -n '/== 未识别行审计 ==/{n;p;}' |
    grep -oE '未识别:[0-9]+' | tr -dc '0-9')
CONSOLE_RETAINED=$(echo "$OUTPUT" | sed -n '/== 标签 ==/,/== 关键事件/p' |
    awk '$1=="CONSOLE" {print $2}')
if [ "${CONSOLE_RETAINED:-0}" != "${UNPARSED:-0}" ]; then
    echo "  ❌ 裸输出保留对账失败:未识别 ${UNPARSED:-0},CONSOLE 保留 ${CONSOLE_RETAINED:-0}"
    exit 1
fi
echo "  ✅ 裸输出 ${CONSOLE_RETAINED:-0} 行全部保留（时间推定有显式标记）"

# 标签从完整 manifest 提取，不再用会漏跨行/误算注释的 git-grep 正则。
python3 -c '
import csv,re,sys
tags=set()
with open(sys.argv[1], encoding="utf-8") as f:
    for row in csv.DictReader(f, delimiter="\t"):
        if row["channel"] not in ("sd","seas","android","syslog"): continue
        m=re.match(r"\[([A-Za-z][A-Za-z0-9_ ]*)\]", row["format"])
        if m: tags.add(m.group(1).strip())
print("\n".join(sorted(tags)))
' "$MANIFEST" > "$TMP/src.txt"
echo "$OUTPUT" | sed -n '/== 标签 ==/,/== 关键事件/p' |
    sed -nE 's/^[[:space:]]+(.+)[[:space:]]+[0-9]+[[:space:]]*$/\1/p' |
    sed -E 's/[[:space:]]+$//' |
    grep -v -E '^\(无标签\)$' | sort -u > "$TMP/tool.txt"
MISS=$(comm -23 "$TMP/src.txt" "$TMP/tool.txt")
if [ -n "$MISS" ]; then
    echo "  ❌ 以下源码标签未被保留:"
    echo "$MISS" | sed 's/^/     /'
    exit 1
fi
echo "  ✅ 源码标签 $(wc -l < "$TMP/src.txt") 个全部保留"

DYNAMIC=$(awk -F '\t' 'NR>1 && $4==1 {n++} END {print n+0}' "$MANIFEST")
echo "  ℹ 动态格式入口 $DYNAMIC 个已逐调用点列入 manifest；运行时正文由通道包络保留"
echo "════ $NAME 全打印覆盖校验通过 ════"
