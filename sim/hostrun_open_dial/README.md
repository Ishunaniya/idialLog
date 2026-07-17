# sim/hostrun_open_dial — 让 **open_dial 的真实拨号代码**在本机跑起来

与 `../hostrun`(modem_mng)同法,证据等级同为【**源码执行实证**】:日志由真代码打,不是我手写。

## 构成

| 真代码(直接编 open_dial 原文件,不拷贝) | 假的(只有外设) |
|---|---|
| `dial.c`(**880 行**)、`misc.c`、`logger_sd.c` | 52 个 SDK 桩(**签名自动取自 SDK 真头**,50 个自动生成) |
| `apn.c` `at.c` `data_call.c` `nw.c` `sim.c` `diag.c` `dial_reboot_conf.c` | json-c 桩 |
| | 复用 `../hostrun/fakebin`(假 ping/serial_atcmd)与 `fastclock.so` |

open_dial 与 modem_mng 用**同一套 Quectel SDK**,故假外设通用。

## 用法

```bash
make && ./run_scenario.sh      # → ../../samples/sim/hostrun_open_dial/*.log
```

## 两家的真实差异(真代码跑出来的,不是我猜的)

| | open_dial | modem_mng |
|---|---|---|
| **ping 判定** | `test_can_ping_google`(`misc.c:600`)匹配 **`ttl=`/`TTL=`** | `PingNetworkChecker`(`NetworkMonitor.cpp:9`)匹配 **`"1 packets transmitted, 1 received"`** |
| **L3 措辞** | `Exiting for **start_prog** to reinitialize.` | `Exiting for **watchdog/init** to reinitialize.` |
| **SIM 状态行** | `[INIT] SIM init state: ...` | `[INIT] SIM state: ...` |
| `dial_loop` | 全局函数,`while(1)` 无退出开关 | 类方法,靠 `isExist` 退出 |

假 ping 两种文本都打,故两家通用 —— 这个设计是被 modem_mng 那边"只给返回码导致恒判不通"打脸后才明白的。

## 场景

`od_never_connected` / `od_recovery_ladder`(完整 L1→L2→L3)/ `od_weak_signal` /
`od_sim_absent` / `od_all_normal`(对照组)

## 边界

同 `../hostrun/README.md`:能抓"我读错源码",**抓不到"真设备超出源码字面"**。真机日志不可替代。
