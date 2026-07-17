# samples/ — 测试夹具

按**日志来源**分目录。`make selftest && ./selftest <文件>` 可逐个跑。

| 目录 | 来源 | 格式 | 样本 | 证据等级 |
|---|---|---|---|---|
| `rtms_eg25/` | `rtms_sdk/apps/modem_mng` EG25 | `FMT_SD` | ✅ `dial_20260630_000026.log`(2861 行,ROAMLINK 通道)<br>✅ `real_eg25_1.31.15_unsynced.log`(133 行,SIM 通道 + 完整 L1/L2 恢复阶梯) | **真机实证** |
| `dial_ec200a/` | `/home/tronlong/lyp/code/open_dial`(老框架 EC200A) | `FMT_SD` | ✅ `real_ec200a_1.28.4_unsynced.log`(36 行) | **真机实证**(2026-07-17 补入) |
| `dial_eg25/` | `/home/tronlong/lyp/code/open_dial_for_artery`(老框架 EG25) | `FMT_SEAS` | ✅ `real_artery_1.29.13.log`(71 行,**含真实 ESC 字节**)<br>⚠️ `artery_seas_synthetic.log` | **真机实证**(2026-07-17 补入) |
| `rtms_ag35/` | `rtms_sdk/apps/modem_mng` AG35(双卡) | `FMT_SD` | ⚠️ 仅合成 | **仅源码实证** —— 唯一仍无真机日志的分支 |
| `rtms_ec200a/` | `rtms_sdk/apps/modem_mng` EC200A | `FMT_SD` | ⚠️ 仅合成 | 行格式与心跳字段和 `dial_ec200a` **逐字段相同**(`open_dial/dial.c:518` vs `ec200a/dial/dial.cpp:1077`),后者已有真机实证 |

## 真机日志抓出了合成夹具抓不到的两个 bug(2026-07-17)

合成夹具**每级恢复只写一行、也没有多行条目**,所以以下问题从未暴露 —— 这就是"合成夹具不能替代真机日志"的实证:

1. **恢复阶梯次数虚高**:真机一次 L1 打 2 行(`LastErr` + `REG down, skip redial`)、
   一次 L2 打 3 行(`LastErr` + `CFUN=0 rsp` + `CFUN=1 rsp`)。原实现按**行**计数,
   把 4 次 L1 报成 8 次、1 次 L2 报成 3 次。已改为按**事件**计数(30s 内同级行合并)。
2. **多行条目的续行被当成"未识别"**:AT 应答分行写,裸 `OK` 行没有时间戳。
   原实现丢弃并计入未识别;现并入上一条(`⏎` 分隔),审计新增 `continuation` 计数。
   那个 `OK` 是 CFUN 是否成功的证据,丢掉就是漏诊断信息。

## 为什么没有 `rtms_imx6ull/`

**IMX 不产这类日志**,不是"样本还没收集":

- `dialer_imx6ull.cpp` 中 `dial_log` 调用数为 **0**(只用 `printf`)
- `CMakeLists.txt` 的 IMX 分支**不链接** `logger_sd.c`

建一个永远空的目录会误导成"待补样本",故不建。

## 合成夹具的效力边界(重要 —— 已被上面的事实印证)

`*_synthetic.log` 是**照着源码格式拼的**,不是真机日志。它能证明:

- ✅ 解析器**按源码预期**工作

它**不能**证明:

- ❌ 真机日志确实长这样(源码理解可能有偏差、运行期可能有未预料的格式)

所以 `rtms_ag35` / `rtms_ec200a` / `dial_eg25` 三个分支的结论,证据等级都只是 **【源码实证】**,
不是【样本实证】。**拿到任何一份真机日志,请立刻丢进对应目录跑一遍** —— 那才是把这三个分支
从"推断"升级为"实证"的唯一途径。

真实覆盖率不看这里的断言,看工具的**「未识别行」页 + 状态栏占比**——那是实测。
