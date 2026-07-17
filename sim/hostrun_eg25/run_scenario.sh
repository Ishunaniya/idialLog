#!/bin/bash
# run_scenario.sh — 故障注入驱动 **EG25 的真实 dial_task**。
# 复用 ../hostrun 的 fakebin(假 ping)与 fastclock.so;AT 走本目录的 fakemodem.so(PTY 假模组)。
# 注意 EG25 的 ping 是 system("ping ... > /dev/null") **只看返回码**(dial.c:319),
# 与 EC200A(看输出文本)、open_dial(看 "ttl=")都不同 —— 假 ping 三样都满足。
set -u
cd "$(dirname "$0")"
HERE=$PWD; SHARED=$PWD/../hostrun
OUT=${1:-../../samples/sim/hostrun_eg25}
mkdir -p "$OUT"
[ -x ./driver ] || { echo "先 make"; exit 1; }
[ -f "$SHARED/fastclock.so" ] || (cd "$SHARED" && make fastclock.so >/dev/null 2>&1)
run() {
    local name=$1 secs=$2; shift 2
    printf "  %-24s " "$name"
    rm -f /tmp/.sim_ping_t0.* /tmp/dial_retry_count /tmp/cfun_count.txt /tmp/cfun_last_call.txt
    ( export PATH="$SHARED/fakebin:$PATH" LD_PRELOAD="$HERE/fakemodem.so:$SHARED/fastclock.so"
      timeout $((secs / 100 + 60)) env "$@" ./driver "$secs" ) > "$OUT/$name.log" 2>/dev/null
    printf "%4d 行\n" "$(wc -l < "$OUT/$name.log")"
}
echo "════ EG25 真实 dial_task 场景(日志全部由真代码产出)════"
run eg25_all_normal      600  SIM_TIME_SCALE=60  SIM_PING_OK=1 SIM_CEREG=1 SIM_CSQ=22
run eg25_never_connected 1500 SIM_TIME_SCALE=150 SIM_PING_OK=0 SIM_CEREG=1 SIM_CSQ=18
run eg25_recovery_ladder 2400 SIM_TIME_SCALE=150 SIM_RUN_ID=eg SIM_PING_FAIL_AFTER=4 SIM_CEREG=1 SIM_CSQ=18
run eg25_weak_signal     900  SIM_TIME_SCALE=100 SIM_PING_OK=0 SIM_CEREG=1 SIM_CSQ=3
run eg25_reg_down        900  SIM_TIME_SCALE=100 SIM_PING_OK=0 SIM_CEREG=0 SIM_CREG=0 SIM_CSQ=15
echo; echo "日志已落到 $OUT/"
