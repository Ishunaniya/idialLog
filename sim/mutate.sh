#!/bin/bash
# mutate.sh — 变异测试:把已知正确的行为**故意改错**,看测试抓不抓得住。
#
# 为什么必须做:测试全绿说明不了什么 —— 可能是代码对,也可能是**测试没牙齿**。
# 变异测试是唯一能证明"测试真的在测"的手段。
#
# 实证:上一轮只做了 2 个变异,其中 **1 个没被抓住**(恢复阶梯退回按行计数),
# 说明当时的断言只验"某结论出现了"、没验"次数对不对" —— 补了断言才抓住。
# 2 个里漏 1 个,这个比例说明必须系统地做。
#
# 每个变异都对应一个**真实修过的 bug** 或**真实的行为约束**:变异 = 把它改回错的样子。
# 存活(SURVIVED)= 测试有洞,必须补断言。
#
# 用法: sim/mutate.sh
set -u
cd "$(dirname "$0")/.."
SRC=logmodel.cpp
BAK=$(mktemp); cp "$SRC" "$BAK"
trap 'cp "$BAK" "$SRC"; rm -f "$BAK"; make selftest simtest hostruntest baselinetest >/dev/null 2>&1' EXIT

pass=0; survived=0; total=0

# 跑三层测试,全绿返回 0
# 只编一次 logmodel.o(变异只改它),4 个测试共用它链接 —— 省掉 30 次重复全量编译
# (瓶颈实测:全量编译 logmodel.cpp 每次 12s;只链接则秒级)
CXX="g++ -std=c++17 -O2"
run_tests() {
    $CXX -c logmodel.cpp -o /tmp/mut_lm.o 2>/dev/null || return 2   # 编不过=变异被抓住
    for t in selftest simtest hostruntest baselinetest; do
        $CXX -o /tmp/mut_$t $t.cpp /tmp/mut_lm.o 2>/dev/null || return 2
    done
    /tmp/mut_simtest      >/dev/null 2>&1 || return 1
    /tmp/mut_hostruntest  >/dev/null 2>&1 || return 1
    /tmp/mut_baselinetest >/dev/null 2>&1 || return 1
    for f in samples/sim/hostrun/never_connected.log \
             samples/rtms_eg25/real_eg25_1.31.15_unsynced.log \
             samples/rtms_ag35/real_ag35_1.32.16_console.log; do
        [ -f "$f" ] || continue
        /tmp/mut_selftest "$f" >/dev/null 2>&1 || return 1
    done
    return 0
}

mutate() {
    local name="$1" old="$2" new="$3"
    total=$((total+1))
    cp "$BAK" "$SRC"
    if ! grep -qF "$old" "$SRC"; then
        printf "  %-46s ⚠ 变异点已不存在(代码变了?跳过)\n" "$name"
        return
    fi
    python3 - "$SRC" "$old" "$new" <<'PY'
import sys
p,o,n=sys.argv[1],sys.argv[2],sys.argv[3]
s=open(p,encoding='utf-8').read()
open(p,'w',encoding='utf-8').write(s.replace(o,n,1))
PY
    if run_tests; then
        printf "  %-46s ❌ 存活 —— 测试没抓住,有洞\n" "$name"
        survived=$((survived+1))
    else
        printf "  %-46s ✅ 被抓住\n" "$name"
        pass=$((pass+1))
    fi
}

echo "════ 变异测试:把修过的 bug 故意改回去,看测试抓不抓得住 ════"

# 1. CP dump 假阳性(真实 bug:正常 EC200A 曾被报"基带崩溃")
mutate "CP dump 退回'见标签就报'" \
  'if (l.tag == "CPDUMP" && icontains(l.msg, "existing CP dump") &&' \
  'if (l.tag == "CPDUMP" && true &&'

# 2. 恢复阶梯按行计数(真实 bug:4 次 L1 曾被报成 8 次)
mutate "恢复阶梯退回按行计数" \
  'if (!v.empty() && l.t - v.back()->t <= 30) return;' \
  'if (false) return;'

# 3. 续行被丢弃(真实 bug:AT 应答的 OK 曾被当垃圾)
mutate "续行退回丢弃(不并入上一条)" \
  'if (e2 != std::string::npos && prev[e2] == '"'"':'"'"') {' \
  'if (false) {'

# 4. 续行规则过宽(真实 bug:控制台 printf 曾被糊进上一条)
mutate "续行规则退回'无时间戳即续行'" \
  'if (e2 != std::string::npos && prev[e2] == '"'"':'"'"') {' \
  'if (true) {'

# 5. 混合大小写标签(真实 bug:[NetCheck] 曾认不出)
#    注意:必须改**后续字符**的判断才复现得了 —— 只改首字符时 'N' 本来就是大写,
#    后面的 'etCheck' 靠字符循环放行,变异无效(第一版就栽在这,不是测试没抓住)。
mutate "标签退回只认大写(丢 [NetCheck])" \
  "        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||" \
  "        if (!((c >= 'A' && c <= 'Z') ||"

# 6. 数据服务未就绪(真实 bug:曾是永不触发的死分支)
#    注意:判据是两个 || 条件("failed" 或 "retrying"),只改第一个时第二个仍会命中
#    → 变异无效(第一版栽在这)。必须把整个收集语句废掉。
mutate "'数据服务未就绪'退回死分支" \
  'if (icontains(l.msg, "data_call_init failed") ||
            icontains(l.msg, "data_call_init retrying"))    evNotReady.push_back(&l);' \
  'if (false) evNotReady.push_back(&l);'

# 7. 审计漏计未识别行 → 自洽等式必须崩
mutate "审计不计未识别行(自洽等式该崩)" \
  '        ad.unparsed++;' \
  '        ad.unparsed += 0;'

# 8. 弱信号阈值改错
mutate "弱信号阈值 <10 改成 <2" \
  'else if (minCsq < 10 && mWeak)   { c = C_WEAK;      evm = mWeak; }' \
  'else if (minCsq < 2 && mWeak)    { c = C_WEAK;      evm = mWeak; }'

# 9. never-connected 匹配串改错
mutate "never-connected 匹配串改错" \
  'if (icontains(l.msg, "never-connected"))            evNeverConn.push_back(&l);' \
  'if (icontains(l.msg, "never-connectedXX"))          evNeverConn.push_back(&l);'

# 10. 数据假死判定失效
mutate "数据假死(ΔRX=0)判定失效" \
  'else if (sawZeroRx && mZero)     { c = C_DATADEAD;  evm = mZero; }' \
  'else if (false && mZero)         { c = C_DATADEAD;  evm = mZero; }'

echo
echo "════ ${total} 个变异:${pass} 个被抓住,${survived} 个存活 ════"
[ "$survived" -gt 0 ] && echo "存活 = 测试有洞,必须补断言(不是代码没问题)"
exit 0
