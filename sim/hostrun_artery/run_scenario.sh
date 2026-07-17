#!/bin/bash
# run_scenario.sh — 故障注入驱动 **artery 的真实 dial_task**。
# 复用 ../hostrun_eg25/fakemodem.so(PTY 假模组;artery 的 AT 口同为 /dev/smd8+O_NONBLOCK)
# 与 ../hostrun/fastclock.so(时间加速)、../hostrun/fakebin(假 ping)。
#
# 注意 artery 的连通判定是**第四种**:不是 ping 而是 **TCP**(tcp_fail_count,main.c:228)。
set -u
cd "$(dirname "$0")"
HERE=$PWD; EG=$PWD/../hostrun_eg25; SH=$PWD/../hostrun
OUT=${1:-../../samples/sim/hostrun_artery}
mkdir -p "$OUT"
[ -x ./driver ] || { echo "先 make"; exit 1; }
[ -f "$EG/fakemodem.so" ] || (cd "$EG" && make fakemodem.so >/dev/null 2>&1)
[ -f "$SH/fastclock.so" ] || (cd "$SH" && make fastclock.so >/dev/null 2>&1)
run() {
    local name=$1 secs=$2; shift 2
    printf "  %-24s " "$name"
    rm -f /tmp/.sim_ping_t0.*
    ( export PATH="$SH/fakebin:$PATH" LD_PRELOAD="$EG/fakemodem.so:$SH/fastclock.so"
      timeout $((secs / 100 + 60)) env "$@" ./driver "$secs" ) > "$OUT/$name.log" 2>/dev/null
    printf "%4d 行\n" "$(wc -l < "$OUT/$name.log")"
}
echo "════ artery 真实 dial_task 场景(日志全部由真代码产出,seas_log 格式)════"
run artery_all_normal      600  SIM_TIME_SCALE=60  SIM_CEREG=1 SIM_CSQ=22
run artery_reg_down       1500 SIM_TIME_SCALE=150 SIM_CEREG=0 SIM_CREG=0 SIM_CSQ=15
run artery_weak_signal     900 SIM_TIME_SCALE=100 SIM_CEREG=1 SIM_CSQ=3
run artery_reg_timeout    2400 SIM_TIME_SCALE=150 SIM_CEREG=0 SIM_CSQ=18
echo; echo "日志已落到 $OUT/"
