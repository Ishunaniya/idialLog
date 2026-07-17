# sim/hostrun_eg25 — 让 **modem_mng EG25 的真实拨号代码**在本机跑起来

第三份(前两份:`../hostrun` = EC200A/AG35,`../hostrun_open_dial` = open_dial)。
证据等级同为【**源码执行实证**】:日志由真 `dial_task`(`eg25/dial/dial.c:540`)自己打。

## EG25 比前两份难一档,难在哪(都是实测踩出来的)

| 坑 | 真相 | 修法 |
|---|---|---|
| **第三套 SDK** | EG25 用 `ql-ol-extsdk`,与 EC200A 的 `ql-sdk` 完全不同;头链层层套:`ql_oe.h`→`comdef.h`→`qmi_port_defs.h`→… | 逐个补 `-I` 试出来 |
| **ARM 头污染主机** | EG25 的 ARM sysroot 里有自己的 `stdint.h`,和主机 x86 的打架(`__INT64_C` 重定义) | 改用 `-idirafter`,让主机头优先 |
| **Android 日志头** | `tbox-common/logger.h` 的 `LOG_I` 用 `__android_log_print(ANDROID_LOG_INFO,...)`,主机没这头 | 造最小假 `fakeinc/android/log.h` + `-include` 强制插入(**不改真代码一行**) |
| **AT 走真串口** | 不像 EC200A 用 `popen("serial_atcmd")`,EG25 是 `open("/dev/smd8")` + `select/read/write`(`eg25/at/at.c:179`) | `fakemodem.so`:LD_PRELOAD 拦 `open`,接到 PTY 上的假模组 |
| **⭐ 死锁** | `Ql_SendAT` **第一步是 `read()` 清串口残留,再 `write()` 发命令**(`at.c:47-49`)。真串口是 `O_NONBLOCK` 读空即返回;`openpty` 的 slave **默认阻塞** → 真代码永久死等,假模组也在等命令 —— **互相死等,一行日志都出不来** | PTY slave 设 `O_NONBLOCK`,与真串口一致 |
| **dial_task 要真构造** | 传 NULL 直接段错误;它要 `dial_mng_new()`(真代码自己的构造函数)建的 `dial_mng_t` | driver 照 `dialer_eg25.c:89` 的真实流程来 |
| **iniparser 重复定义** | 我既编了真的(`eg25/opt_iniparser/`)又打了桩 | **真代码存在就编真的**,只有外设才打桩 |
| **roamlink 会动主机** | `roamlink.c:215` 有 `reboot(RB_AUTOBOOT)`,还有 `fork`+`execl(RBMaster)` | `fakemodem.so` 拦死 `reboot`(不靠"非 root 会失败"这种未验证的侥幸) |

> ⭐ 那个死锁**只有真代码跑起来才暴露**:光读源码我完全没注意到 `at_init` 会先 `read`。
> 这正是"跑真代码"相对"手写场景"的价值。

## 三家的 ping 判定各不相同(实证)

| | 判定依据 |
|---|---|
| **EG25** | `system("ping -c 1 8.8.8.8 > /dev/null")` **只看返回码**(`dial.c:319`) |
| **EC200A** | `PingNetworkChecker` 匹配 **`"1 packets transmitted, 1 received"`**(`NetworkMonitor.cpp:9`) |
| **open_dial** | `test_can_ping_google` 匹配 **`"ttl="`**(`misc.c:600`) |

共用的假 ping 三样都满足,故通用。

## 构成

**真代码**(直接编原文件,不拷贝):`dial.c`(2116 行)、`at.c` `nw.c` `apn.c` `sim.c` `tz.c`
`diag.c` `roamlink.c` `dial_status.c` `logger_sd.c` + `cc_deque` + 真 `iniparser` + 真 cJSON。
**假的**:18 个 SDK 桩(照 EG25 真头自动生成,生成时须先剥 `///<` 注释否则参数列表被切坏)、
LED/appmng 桩、`fakemodem.so`(PTY 假模组 + reboot 闸)、`fastclock.so`。

## 用法

```bash
make && ./run_scenario.sh      # → ../../samples/sim/hostrun_eg25/*.log
```

## 边界

同前两份:能抓"我读错源码",**抓不到"真设备超出源码字面"**。真机日志不可替代。
