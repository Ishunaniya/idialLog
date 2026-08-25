# samples/ — 测试夹具

按**日志来源**分目录。`make selftest && build/tests/unit/selftest <文件>` 可逐个跑。

> 尚无真机日志包含 `NETWORK REJECTED`、`LIMITED SERVICE`、`SUSPECTED subscription issue`
> 或 `CEREG query/parse failed`。AG35 1.32.0 的 2026-08-18 正常日志已覆盖新版心跳字段，
> 但该版本并非最新且全程 REG=5，不能验证异常诊断。新诊断当前有产品源码、四条逐字场景
> 实证；其中 `SUSPECTED` 另有三套 host-run 真代码执行实证，但仍不等同于真实设备日志。
>
> `modem_mng_v2` 当前也**没有真机样本**；其目录只放按源码生成的 RFC3164 合成夹具，
> 不借旧版 modem_mng 的真机日志提升证据等级。

| 目录 | 来源 | 格式 | 样本 | 证据等级 |
|---|---|---|---|---|
| `rtms_eg25/` | `rtms_sdk/apps/modem_mng` EG25 | `FMT_SD` | ✅ `dial_20260630_000026.log`(2861 行,ROAMLINK 通道)<br>✅ `real_eg25_1.31.15_unsynced.log`(133 行,SIM 通道 + 完整 L1/L2 恢复阶梯) | **真机实证** |
| `dial_ec200a/` | `/home/tronlong/lyp/code/open_dial`(老框架 EC200A) | `FMT_SD` | ✅ `real_ec200a_1.28.4_unsynced.log`(36 行) | **真机实证**(2026-07-17 补入) |
| `dial_eg25/` | `/home/tronlong/lyp/code/open_dial_for_artery`(老框架 EG25) | `FMT_SEAS` | ✅ `real_artery_1.29.13.log`(71 行,**含真实 ESC 字节**)<br>✅ `real_artery_1.29.15_license_timeout.log`(1533 行,多启动会话、license 下载超时并降级 FORCE_SIM)<br>⚠️ `artery_seas_synthetic.log` | **真机实证** |
| `rtms_ag35/` | `rtms_sdk/apps/modem_mng` AG35(双卡) | `FMT_SD` | ✅ `real_ag35_1.32.16_console.log`(144 行,**控制台捕获**:SD 未挂载,dial_log 与裸 printf 交织)<br>✅ `real_ag35_1.32.0_sd.log`(333 行,2h03m 正常 eSIM 漫游,新版心跳字段)<br>⚠️ `ag35_synthetic.log` | **真机实证** |
| `rtms_ec200a/` | `rtms_sdk/apps/modem_mng` EC200A | `FMT_SD` | ⚠️ 仅合成 | 行格式与心跳字段和 `dial_ec200a` **逐字段相同**(`open_dial/dial.c:518` vs `ec200a/dial/dial.cpp:1077`),后者已有真机实证 |
| `rtms_v2/` | `rtms_sdk/apps/modem_mng_v2`，运行时动态支持 EC200A/EG25 | BusyBox RFC3164 `FMT_SYSLOG` | ⚠️ `modem_mng_v2_synthetic.log`；年份显示为推定 `~YYYY-...` | **源码实证**，尚无 v2 真机日志 |

## modem_mng_v2 应采哪份日志

正常部署的启动脚本会把 stdout/stderr 重定向到 `/dev/null`，`log_*` 的持久输出落在 BusyBox
syslogd 管理的 `/var/log/messages`。可保留完整文件，或用应用名筛出相关行：

```bash
grep 'modem_mng_v2' /var/log/messages > modem_mng_v2.log
```

典型包络为 `Aug 24 12:34:56 host user.info modem_mng_v2[123]: body`；它没有年份，工具只能
按同文件线索/当前年推定，并在原始日志时间前加 `~`。直接前台运行时见到的 `[INFO]/[ERR]/...` 是 stderr 调试镜像，
没有自身时间戳，适合看消息和级别，不适合验证断网时长或可用率。

## 真机日志抓出了合成夹具抓不到的四类 bug(2026-07-17)

合成夹具**每级恢复只写一行、也没有多行条目**,所以以下问题从未暴露 —— 这就是"合成夹具不能替代真机日志"的实证:

1. **恢复阶梯次数虚高**:真机一次 L1 打 2 行(`LastErr` + `REG down, skip redial`)、
   一次 L2 打 3 行(`LastErr` + `CFUN=0 rsp` + `CFUN=1 rsp`)。原实现按**行**计数,
   把 4 次 L1 报成 8 次、1 次 L2 报成 3 次。已改为按**事件**计数(30s 内同级行合并)。
2. **多行条目的续行被当成"未识别"**:AT 应答分行写,裸 `OK` 行没有时间戳。
   原实现丢弃并计入未识别;现并入上一条(`⏎` 分隔),审计新增 `continuation` 计数。
   那个 `OK` 是 CFUN 是否成功的证据,丢掉就是漏诊断信息。
3. **CP dump 假阳性**:正常 EC200A 只打了 `[CPDUMP] No existing CP dumps.`,
   旧实现见 `[CPDUMP]` 标签就报「[严重] 基带崩溃」。已改为只认 `Found N existing CP dump(s)`。
4. **续行规则过宽 + 漏认混合大小写标签**(AG35 控制台日志暴露):
   "无时间戳即续行"会把 60 行控制台 printf 噪声糊进上一条;已收紧为
   **仅当上一条以冒号结尾**(那是"下面是多行内容"的宣告)才吃续行。
   另:`[NetCheck]` 是 modem_mng **唯一**含小写字母的标签(源码穷举),旧的 `[A-Z0-9_ ]`
   规则认不出它。

## 控制台日志 vs SD 卡日志(两种真实输入,别混为一谈)

- **SD 卡日志文件**(`/media/sdcard/dial_log/...`):只有 `dial_log` 往里写 → 未识别应为 0。
- **控制台捕获**(SD 未挂载时 `Logging to console only`,或直接 `./modem_mng &` 看输出):
  `dial_log` 与**裸 `printf`**(流量库 dump、SDK 回调、`EXEC: serial_atcmd` 等)**交织**
  → 未识别占比会很高(AG35 真机样本:62/144 = 43%),**这是正常的** ——
  那 62 行本来就不是 dial_log 输出。工具老实报出来,而不是硬塞进上一条。

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

因此只能把**合成夹具新增覆盖的具体行为**标为【源码实证】，不能借同目录中旧版真机日志
把新字段或新诊断升级为【样本实证】。拿到新版真机日志后，应按产品目录补入并钉死具体数字。

真实覆盖率不看这里的断言,看工具的**「未识别行」页 + 状态栏占比**——那是实测。
