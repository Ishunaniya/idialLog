# sim/hostrun_artery — 让 **open_dial_for_artery 的真实拨号代码**在本机跑起来

**第四份**,四家齐了:

| 目录 | 代码 | 行数 |
|---|---|---|
| `../hostrun` | modem_mng `ec200a/dial/dial.cpp`(EC200A + AG35) | 1756 |
| `../hostrun_open_dial` | open_dial `dial.c` | 880 |
| `../hostrun_eg25` | modem_mng `eg25/dial/dial.c` | 2116 |
| **`.`(本目录)** | **artery `src/dial/dial.c`** | **2049** |

## artery 的特点

- **日志体系不同**:`seas_log`(不是 `dial_log`)。真代码产出的日志带**毫秒 + 级别 + 函数名 +
  真实 ESC 字节**(实测 746 个),正是 `FMT_SEAS` 格式。
- **连通判定是第四种**:不是 ping 而是 **TCP**(`tcp_fail_count`,`main.c:228`)。
- 与 modem_mng EG25 **同一套 Quectel SDK** → include 链、`fakemodem.so`(PTY 假模组)
  直接复用,14 个真实模块**一次全过**(EG25 那轮踩的坑全省了)。

## 四家的连通判定各不相同(全是实测,不是猜的)

| | 判定依据 |
|---|---|
| **EC200A** | 匹配 `"1 packets transmitted, 1 received"`(`NetworkMonitor.cpp:9`) |
| **open_dial** | 匹配 `"ttl="`(`misc.c:600`) |
| **EG25** | `system(...)` **只看返回码**(`dial.c:319`) |
| **artery** | **TCP**,不是 ping(`tcp_fail_count`) |

## 用法

```bash
make && ./run_scenario.sh    # → ../../samples/sim/hostrun_artery/*.log
```
依赖 `../hostrun_eg25/fakemodem.so`(PTY 假模组)+ `../hostrun/fastclock.so`(时间加速)。

## 边界

同前三份:能抓"我读错源码",**抓不到"真设备超出源码字面"**。真机日志不可替代。
