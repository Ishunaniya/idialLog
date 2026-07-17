# samples/ — 测试夹具

按**日志来源**分目录。`make selftest && ./selftest <文件>` 可逐个跑。

| 目录 | 来源 | 格式 | 有无样本 | 证据等级 |
|---|---|---|---|---|
| `rtms_eg25/` | `rtms_sdk/apps/modem_mng` EG25 | `FMT_SD` | ✅ `dial_20260630_000026.log`(2861 行) | **真机实证**(现网日志) |
| `rtms_ag35/` | `rtms_sdk/apps/modem_mng` AG35(双卡) | `FMT_SD` | ⚠️ 仅合成 | **仅源码实证**,未见过真机日志 |
| `rtms_ec200a/` | `rtms_sdk/apps/modem_mng` EC200A | `FMT_SD` | ⚠️ 仅合成 | **仅源码实证**,未见过真机日志 |
| `dial_ec200a/` | `/home/tronlong/lyp/code/open_dial`(EC200A 上游) | `FMT_SD` | ❌ 无 | 与 `rtms_ec200a` **行格式与心跳字段逐字段相同**(`open_dial/dial.c:518` vs `ec200a/dial/dial.cpp:1077`),故复用其夹具即可 |
| `dial_eg25/` | `/home/tronlong/lyp/code/open_dial_for_artery`(EG25 上游) | `FMT_SEAS` | ⚠️ 仅合成 | **仅源码实证**,全机无 artery 真机日志 |

## 为什么没有 `rtms_imx6ull/`

**IMX 不产这类日志**,不是"样本还没收集":

- `dialer_imx6ull.cpp` 中 `dial_log` 调用数为 **0**(只用 `printf`)
- `CMakeLists.txt` 的 IMX 分支**不链接** `logger_sd.c`

建一个永远空的目录会误导成"待补样本",故不建。

## 合成夹具的效力边界(重要)

`*_synthetic.log` 是**照着源码格式拼的**,不是真机日志。它能证明:

- ✅ 解析器**按源码预期**工作

它**不能**证明:

- ❌ 真机日志确实长这样(源码理解可能有偏差、运行期可能有未预料的格式)

所以 `rtms_ag35` / `rtms_ec200a` / `dial_eg25` 三个分支的结论,证据等级都只是 **【源码实证】**,
不是【样本实证】。**拿到任何一份真机日志,请立刻丢进对应目录跑一遍** —— 那才是把这三个分支
从"推断"升级为"实证"的唯一途径。

真实覆盖率不看这里的断言,看工具的**「未识别行」页 + 状态栏占比**——那是实测。
