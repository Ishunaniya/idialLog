#!/bin/bash
# run_scenario.sh — 用故障注入驱动 **open_dial 的真实 dial_loop**。
# 复用 ../hostrun 的 fakebin(假 ping/serial_atcmd)与 fastclock.so ——
# 两家用同一套 Quectel SDK,假外设通用。
#
# 注意两家的 ping 判定不同(实证):
#   open_dial  : test_can_ping_google(misc.c:600) 匹配 "ttl=" / "TTL="
#   modem_mng  : PingNetworkChecker(NetworkMonitor.cpp:9) 匹配 "1 packets transmitted, 1 received"
# 假 ping 两样都打,故通用。
set -u
cd "$(dirname "$0")"
HERE=$PWD
SHARED=$PWD/../hostrun          # 复用 fakebin/ 与 fastclock.so
OUT=${1:-../../samples/sim/hostrun_open_dial}
mkdir -p "$OUT"
[ -x ./driver ] || { echo "先 make"; exit 1; }
[ -f "$SHARED/fastclock.so" ] || (cd "$SHARED" && make fastclock.so >/dev/null 2>&1)

run() {
    local name=$1 secs=$2; shift 2
    printf "  %-26s " "$name"
    rm -f /tmp/.sim_ping_t0.* /tmp/cfun_count.txt /tmp/cfun_last_call.txt
    echo 9 > /tmp/dial_retry_count          # 进持久模式,别被 Fast-Fail 提前退掉
    ( export PATH="$SHARED/fakebin:$PATH" LD_PRELOAD="$SHARED/fastclock.so"
      timeout $((secs / 100 + 60)) env "$@" ./driver "$secs" ) > "$OUT/$name.log" 2>/dev/null
    printf "%4d 行\n" "$(wc -l < "$OUT/$name.log")"
}

echo "════ open_dial 真实 dial_loop 场景(日志全部由真代码产出)════"
run od_never_connected  1500 SIM_TIME_SCALE=150 SIM_PING_OK=0 SIM_CEREG=1 SIM_CSQ=18
run od_recovery_ladder  3300 SIM_TIME_SCALE=150 SIM_RUN_ID=od SIM_PING_FAIL_AFTER=4 SIM_CEREG=1 SIM_CSQ=18
run od_weak_signal       900 SIM_TIME_SCALE=100 SIM_PING_OK=0 SIM_CEREG=1 SIM_CSQ=3
run od_sim_absent        900 SIM_TIME_SCALE=100 SIM_CARD_ABSENT=1 SIM_PING_OK=0 SIM_CSQ=99
run od_all_normal        300 SIM_TIME_SCALE=60  SIM_PING_OK=1 SIM_CEREG=1 SIM_CSQ=22
echo
echo "日志已落到 $OUT/"
