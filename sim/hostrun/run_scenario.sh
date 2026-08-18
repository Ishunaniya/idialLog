#!/bin/bash
# run_scenario.sh — 用故障注入驱动**真实的 dial_loop**,把它自己打的日志收下来。
#
# 关键:这里**没有一行日志是我编的**。我只负责让假外设"装病"
# (ping 不通 / REG=0 / CSQ 低 / 拔卡 / 拨号失败),
# **真代码自己决定**打什么、走哪条恢复路径。
#
# 时间用 LD_PRELOAD 加速(fastclock.so),源码阈值一行不改:
#   EC200A 阶梯:L1@300s / L2@600s / L3@2100s(这几个数是真代码自己打出来的,见
#   "[INFO] SDK auto-reconnect phase started ... L1 at 300s, L2 at 600s, L3 at 2100s")
#   → ×150 加速下,20s 真实时间 ≈ 50 分钟逻辑时间,足以跑到 L3。
set -u
cd "$(dirname "$0")"
HERE=$PWD
OUT=${1:-../../samples/sim/hostrun}
mkdir -p "$OUT"

[ -x ./driver ] || { echo "先 make"; exit 1; }

# 第二个参数 = **逻辑秒**(driver 内部已按加速换算);真实耗时 ≈ 逻辑秒 / SIM_TIME_SCALE
# 场景之间必须清干净残留状态,否则会互相污染:
#   /tmp/dial_retry_count 是真代码的**快速失败重试计数**,跨进程持久。
#   不清 → 上一轮累积到 3/3,下一轮一进来就走 Fast-Fail 路径、10 秒就退,
#   根本跑不到 10 分钟的 never-connected 门控点。(这坑是真代码行为暴露的。)
# RETRY_MODE=persist : 预置成超过上限 → 进持久模式(长场景用)
# RETRY_MODE=fresh   : 清零 → 从 Attempt 1/3 开始(想验快速失败时用)
run() {
    local name=$1 secs=$2; shift 2
    printf "  %-28s " "$name"
    rm -f /tmp/.sim_ping_t0.* /tmp/.sim_ping_calls.* /tmp/cfun_count.txt /tmp/cfun_last_call.txt
    if [ "${RETRY_MODE:-persist}" = "persist" ]; then echo 9 > /tmp/dial_retry_count
    else rm -f /tmp/dial_retry_count; fi
    ( export PATH="$HERE/fakebin:$PATH" LD_PRELOAD="$HERE/fastclock.so"
      timeout $((secs / 100 + 60)) env "$@" ./driver "$secs" ) > "$OUT/$name.log" 2>/dev/null
    local n; n=$(wc -l < "$OUT/$name.log")
    printf "%4d 行\n" "$n"
}

echo "════ 用真实 dial_loop 跑场景(日志全部由真代码产出)════"

# 1) 从没连通过 → 真代码应自己走 has_connected_once 门控
run never_connected 3000 SIM_TIME_SCALE=100 SIM_PING_OK=0 SIM_CEREG=1 SIM_CSQ=18 SIM_TEMP=35

# 2) 连通过再断网 → 真代码应自己升 L1 → L2 → L3
#    第一次 ping 通(置 has_connected_once),第二次起失败；按调用次数可避免加速时钟调度抖动
run recovery_ladder 4200 SIM_TIME_SCALE=150 SIM_RUN_ID=ladder SIM_PING_FAIL_AFTER_CALLS=1 \
    SIM_CEREG=1 SIM_CSQ=18

# 3) 弱信号断网
run weak_signal 900 SIM_TIME_SCALE=100 SIM_PING_OK=0 SIM_CEREG=1 SIM_CSQ=3 SIM_TEMP=35

# 4) REG=0(没注册上)
run reg_down 900 SIM_TIME_SCALE=100 SIM_PING_OK=0 SIM_CEREG=0 SIM_CREG=0 SIM_CSQ=15 SIM_TEMP=35

# 5) 拔卡
run sim_absent 900 SIM_TIME_SCALE=100 SIM_CARD_ABSENT=1 SIM_PING_OK=0 SIM_CSQ=99 SIM_TEMP=35

# 6) 温度过高
run thermal 600 SIM_TIME_SCALE=100 SIM_PING_OK=1 SIM_CEREG=1 SIM_CSQ=20 SIM_TEMP=92

# 7) 数据服务起不来(ql_data_call_init 失败)
run datacall_init_fail 600 SIM_TIME_SCALE=100 SIM_DATACALL_INIT_RET=-1067 SIM_PING_OK=0 SIM_CSQ=18

# 8) 一切正常(对照组 —— 假阳性守卫:正常设备不该报任何严重结论)
run all_normal 900 SIM_TIME_SCALE=60 SIM_PING_OK=1 SIM_CEREG=1 SIM_CSQ=22 SIM_TEMP=35

# ---- AG35 双卡(需 AG35=1 重新编译:slot_mgr.c 整文件 #ifdef QL_MODULE_PLATFORM_AG35,
#      不加宏编出来是 0 个函数的空 TU,[SLOT] 一条都不会有)----
if [ "${AG35:-0}" = "1" ]; then
  echo
  echo "════ AG35 双卡场景(driver 须用 AG35=1 make 重编)════"
  run ag35_cold_select   600  SIM_TIME_SCALE=100 SIM_PING_OK=1 SIM_CEREG=1 SIM_CSQ=20
  run ag35_switch_fail   900  SIM_TIME_SCALE=100 SIM_PING_OK=0 SIM_CEREG=1 SIM_CSQ=4 SIM_SLOT_SWITCH_FAIL=1
  run ag35_weak_switch   900  SIM_TIME_SCALE=100 SIM_PING_OK=0 SIM_CEREG=1 SIM_CSQ=3
fi

echo
echo "日志已落到 $OUT/"
