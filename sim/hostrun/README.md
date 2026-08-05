# sim/hostrun — 让 **modem_mng 的真实拨号代码**在本机跑起来

## 这跟 `simtest.cpp` 有什么本质区别

| | `simtest.cpp` | `sim/hostrun`(本目录) |
|---|---|---|
| 日志谁产的 | **我照源码手写** | **真代码自己打的** |
| 能抓"我读错源码"吗 | ❌ 抓不到(日志和解析器同出我手,错法一致) | ✅ **能** |
| 证据等级 | 【源码阅读实证】 | 【**源码执行实证**】 |

**当场抓到的实证** —— 我手写的 vs 真代码打的:

```
我手写的:  [INFO] never-connected, policy recovery (L1/L2/L3) gated.
真代码打的:[INFO] never-connected, policy recovery (L1/L2/L3) gated. Downtime 602s,
          REG=1, data service OK but no data -- suspected SIM/account issue
          (e.g. suspended / no data plan).
```
**我漏掉了整个后半句** —— 而那才是诊断价值所在。原因:源码里那条 `dial_log(...)` 是跨行拼接的,
我 grep 只抓到第一个字符串字面量就当全部了。我的 simtest 一直拿残缺日志验结论引擎,还全绿。

真代码还打了这些我 11 个手写场景里**一条都没有**的:
- `[INFO] SDK auto-reconnect phase started (interval=25s). L1 at 300s, L2 at 600s, L3 at 2100s.`
- `[ALARM] Net Fail Duration: %d sec. Trigger Level %d recovery.`(阶梯升级时打的,我压根不知道)
- `[EVENT] Exit requested. Turning LED off and leaving dial_loop.`

## 构成:真的多、假的少

| 真代码(**直接编 modem_mng 原文件,不拷贝**) | 假的(只有外设) |
|---|---|
| `ec200a/dial/dial.cpp`(1756 行)、`oper.cpp` | 15 个 SDK 桩(**签名自动取自 SDK 真头**) |
| `at.c` `sim.c` `apn.c` `data_call.c` `nw.c` `diag.c` `misc.c` `slot_mgr.c` | LED / json-c / iniparser / nanomsg 桩 |
| `fault_report/NetworkMonitor.cpp`(真正执行 ping 的地方) | `fakebin/` 的假 `ping` / `serial_atcmd`(PATH 拦截) |
| `logger_sd.c`、真 cJSON | `fastclock.so`(LD_PRELOAD 加速时间) |

**故障注入不改一行源码**,全靠环境变量:
`SIM_PING_OK` `SIM_CEREG` `SIM_CREG` `SIM_CSQ` `SIM_SRV` `SIM_RAT` `SIM_DENY`
`SIM_RSRP` `SIM_RSRQ` `SIM_SNR` `SIM_RSSI` `SIM_OPER` `SIM_CARD_ABSENT`
`SIM_DATACALL_INIT_RET` `SIM_PING_FAIL_AFTER` `SIM_PING_FAIL_AFTER_CALLS`

## 用法

```bash
make                    # EC200A(不含双卡)
./run_scenario.sh       # 8 个场景 → ../../samples/sim/hostrun/*.log

AG35=1 make             # AG35(**编入双卡**)
AG35=1 ./run_scenario.sh   # 上面 8 个 + 3 个 AG35 双卡场景
```

### ⚠️ AG35 必须加 `AG35=1`,否则双卡代码根本没编进来

`ec200a/slot/slot_mgr.c` **整文件** `#ifdef QL_MODULE_PLATFORM_AG35`。不加宏时:

```
$ nm --defined-only slot_mgr.o | grep -c ' T '
0        ← 空 TU,24 个函数一个都没有
```

我早先只加了 `-DUSE_EC200A_DIAL`,却宣称"一份代码覆盖 EC200A + AG35" —— **那是错的**,
跑出来的日志里 `[SLOT]` 一条都没有。加上宏后 slot_mgr 有 24 个函数,真代码才会自己打出
`[SLOT] cold select: probing both physical slots` / `[SLOT] switch to Phy` /
`[SLOT] active now eSIM`,心跳里也才有 `SLOT:` 字段,dialLog 才会把平台识别成 AG35。

AG35 双卡还需两个 **AG35 SDK 专有**的桩(`ql_sim_switch_slot` / `ql_sim_get_active_slots`),
签名取自 AG35 SDK 真头 —— 我又先用 `int`/`void*` 猜了一次,**被编译器第三次打脸**
(前两次:`ql_sim_get_card_info` 猜 `void*`+`memset(64)` → 段错误;
`ql_sim_set_card_status_cb` 猜 `void*` → 拒绝)。

## 踩过的坑(都是真东西打脸打出来的,记下来免得重犯)

1. **桩的签名/类型必须取自真头,不能猜**:我按 `ql_sim_get_card_info(void*)` + `memset(info,0,64)`
   猜 → **段错误**(真结构体远大于 64 字节);`ql_sim_set_card_status_cb(void*)` → 真签名带回调类型,
   编译器当场拒绝。**每次我猜,真东西立刻打脸**。
2. **ping 不看返回码,看输出文本**:`PingNetworkChecker::checkNetwork()`
   (`fault_report/NetworkMonitor.cpp:9`)匹配的是 `"1 packets transmitted, 1 received"`。
   我早先只给返回码 → 真代码恒判"不通" → 所有场景塌缩成 never-connected。
   而且 `fault_report/` 一开始就漏编了。
3. **只加速时钟不加速 sleep → 时序失真**:代码里 CFUN=0 与 CFUN=1 之间 `sleep(5)`,
   时钟 ×150 但真睡 5 秒 → 日志里这两行**跨了 32 分钟**;真机 EG25 只差 **3 秒**。
   必须连 `sleep/usleep/nanosleep` 一起按比例缩短。
4. **场景之间会互相污染**:`/tmp/dial_retry_count`(真代码的快速失败计数)跨进程持久,
   不清 → 下一轮一进来就走 Fast-Fail、10 秒退出,跑不到 10 分钟的门控点。
5. **driver 参数是逻辑秒不是真实秒**:sleep 被加速后,按真实秒理解会让 driver 0.2 秒就退。

## 能力边界(别夸大)

- ✅ 能抓 **"我读错了源码的控制流/措辞"** —— 已实证抓到。
- ❌ **抓不到"真设备干出源码字面之外的事"**。桩的**行为**(拨号返不返回成功、ping 通不通)
  仍是我选的。今天真机日志抓出的 5 个 bug(多行 AT 应答、控制台 printf 交织、
  `No existing CP dumps` 的语义…)**没有一个**是跑源码逻辑能想到的。
- ⚠️ 时序**近似**真机但非等同(见坑 3);`all_normal` 等场景仍有少量未识别行,
  那是 SDK/流量库的裸 printf(控制台日志的正常现象)。

**真机日志不可替代。** 这套东西补的是"我对源码的理解可能有偏差",不是"设备的真实行为"。
