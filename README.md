# dialLog — 拨号日志分析工具 (Windows 原生 / MinGW)

纯 Win32 API 的原生 Windows GUI 程序,**静态链接,无任何运行时依赖**(不需要 .NET,
不需要 MinGW 的 DLL),编译产物 `dialLog.exe` 单文件约 1.8MiB,拷到 Windows 双击即用。

**目标:把日志塞进去就出结论**——不只给统计值,还给根因判断与处置建议,
且每条结论都附带可追溯的日志证据(行号 + 时间戳)。

## 支持的日志格式

| 格式 | 产生方 | 行样式 |
|---|---|---|
| `FMT_SD` | `logger_sd.c` 的 `dial_log()`,**modem_mng 与 open_dial 完全一致** | `[YYYY-MM-DD HH:MM:SS] [TAG] message` |
| `FMT_SEAS` | `open_dial_for_artery` 的 `src/seas_log/seas_log.c` | `YYYY-MM-DD HH:MM:SS.mmm [LEVEL] <ESC[0m>func (file:line) - message` |
| `FMT_ANDROID` | Android/logcat 输出 | `YYYY-MM-DD HH:MM:SS.mmm pid tid L TAG: message` |
| `FMT_SYSLOG`(RFC3339) | 通用 syslog 转储 | `YYYY-MM-DDTHH:MM:SS host app[pid]: message` |
| `FMT_SYSLOG`(BusyBox RFC3164) | `modem_mng_v2` 的主要部署日志 `/var/log/messages` | `Mon DD HH:MM:SS host user.info modem_mng_v2[pid]: message` |
| `FMT_CONSOLE` | 裸控制台输出；含 `modem_mng_v2` 的 stderr 调试镜像 | `[INFO]/[ERR]/[WARN]/[NOTICE]/[DBG] message`(无自身时间) |

`FMT_SEAS` 的细节(源码实证,`seas_log.c:233-296`):当前 `SEAS_DISPLAY_COLOR=0`
故**不输出颜色码**,但 `SEAS_DISPLAY_RESET=1` 故每行在级别与函数名之间**必带一个
`\x1B[0m`**;解析时统一剥离 ANSI CSI 序列,有无都能解析。日志级别在
`main.c` 配成 `SEAS_LEVEL_INFO`,故 `[DEBUG]` 不会出现在文件里。

`modem_mng_v2` 固定调用 `openlog("modem_mng_v2", LOG_PID|LOG_CONS, LOG_USER)`；正常启动脚本
会丢弃 stdout/stderr，所以应优先导入 BusyBox syslogd 写出的 `/var/log/messages`。其 `log_*`
宏虽会把带 `[INFO]` 等前缀的副本镜像到 stderr，送入 syslog 的正文却**不含这个前缀**，
级别取自 `user.info/user.err/...` 或可选 `<PRI>`。RFC3164 不携带年份：解析器优先借同文件
的明确年份，否则按当地当前年推定，并在原始日志时间前加 `~`；文件内 Dec→Jan 会按跨年处理，
但脱离上下文的历史片段无法恢复真实年份。纯 stderr 片段没有可用于断网时长的可靠时基。

### 平台自动识别

打开日志即自动判定来源平台并显示在标题栏/状态栏/结论页,判据均为源码实证的特征:

| 平台 | 判据 | 源码出处 |
|---|---|---|
| modem_mng_v2 | syslog 应用名为 `modem_mng_v2`，或出现 v2 固定启动/状态机原文；模组再由 `Module detected: EC200A/EG25` 动态识别 | `modem_mng_v2/src/log/log.c:22`、`src/modem/modem.c:73,147` |
| 新版平台化版本标识 | `DIAL Version: dial_eg25_*` / `dial_ec200a_*`，或 `Modem_mng Version: rtms_<platform>_*`；直接识别 artery、open_dial、RTMS AG35/EC200A/EG25/IMX6ULL/RK3506J | 2026-08-31 三个产品仓库的版本标识提交 |
| artery | 行格式为 seas_log | `seas_log.c:210` |
| AG35 | 心跳含 `SLOT:` 或出现 `[SLOT]` 标签 | `ec200a/dial/dial.cpp:1071-1075`(AG35-only `#ifdef`) |
| EG25 | 心跳含 `CH:`/`RL_FAIL`/`RX_PKT`,或 `[ROAMLINK]` 标签 | `eg25/diag/diag.c:109-131` |
| EC200A | 心跳为 `SIM_AT:`/`SIM_CB:` 且无 `SLOT:` | `ec200a/dial/dial.cpp:1077` |

> **旧版 EC200A 与 open_dial 不做区分**:二者心跳字段完全相同；但含新版
> `dial_ec200a_*` / `rtms_ec200a_*` 展示版本时，工具按该直接证据区分来源。
>
> **IMX6ULL / RK3506J**:新版 RTMS 都输出 `[HB30]` / `[HB300]` 状态快照。
> 工具解析其 `online/downtime_s/cereg/pdp/CSQ/RSRP/RSRQ/SNR/RSSI/temp_c/at_timeout/at_probe`、详细快照中的
> `detailed_at_timeout/detailed_at_stage/serving_cell/PCI/TAC/rx_packets`，并以 `online=1→0→1` 或 `[RECOVERY]` 配对断网；
> 仅带 `downtime_s` 的 `[RECOVERY]` 才表示恢复完成；`class/level/action` 的 PDP、CFUN、硬件分级动作
> 仍属于断网中的恢复过程。`[FAILURE]/[RETRY]/[SIM]/[REG]/[PDP]/[DHCP]/[NET]/[DEVICE]/[AT]` 和新版
> `[RECOVERY]` 进入 IMX6ULL 状态机诊断。RK3506J 的外置 EC200A / EG912 ECM 状态机则识别
> `[EC200A]` / `[EG912]` 的失败重试、`[bringup]` 的 SIM/注册/PDP/DHCP 失败，以及
> `[DEVICE]` / `[AT]` 的拓扑和端口失败。陈旧的详细快照不会参与 RX 停滞计算。

### 心跳格式逐平台不同(不要假设相同)

| 平台 | 心跳字段 | 出处 |
|---|---|---|
| EC200A / open_dial | `SIM_AT / SIM_CB / REG / CSQ / TEMP / DownTime` + `SRV / RAT / DENY / RSRP / RSRQ / SNR / RSSI`;周期扩展行含 `OPER` | `ec200a/dial/dial.cpp` / `open_dial/dial.c` |
| AG35 | 同 EC200A + `SLOT` | `ec200a/dial/dial.cpp` |
| EG25 | `CH / SIM / [REG] / CSQ / Temp / DownTime / ConsecFail / [RL_FAIL] / [RX_PKT]` + `SRV / RAT / DENY / RSRP / RSRQ / SNR / RSSI`;<br>周期扩展行另有 `cereg= / ifname= / rx_packets= / OPER=`(**等号**赋值) | `eg25/diag/diag.c` |
| artery | `state= / csq= / tcp_fail= / rl_fail=` + `SRV / RAT / DENY / RSRP / RSRQ / SNR / RSSI`;扩展 `cereg= / ifname= / ip= / rx_packets= / OPER=`(空格分隔 k=v) | `main.c` |
| modem_mng_v2 | 无旧版 `HEARTBEAT`；READY 轮询输出 `[where] CSQ: ...`、`CEREG/CGREG stat=...` 和 `WAN ping OK/fail -> network_online=...` | `modem_mng_v2/src/modem/modem.c:1073,1443-1451,1772-1775` |

字段解析器同时支持 `K:V` 与 `K=V`,并按“空白 + 标识符 + 分隔符”切分,
否则 `RSRP:-104 RSRQ:-10`(`diag.c:33`)会把 RSRQ 吞进 RSRP 的值。

## 功能

- **拖拽打开**:把一个或多个 `dial_*.log` 拖进窗口(多文件自动合并),或用“打开日志…”。
  文件读取、解析和结论计算在后台完成，状态栏显示阶段进度；可随时点“取消加载”，旧分析结果
  会一直保留到新文档完整成功。多文件加载还会在概览与报告中给出逐来源的行数、断网、指标样本
  和平均 RSRP 对比。
- **粘贴分析 ★**:手上没有文件时(SSH 里 `cat` 日志直接选中复制、别人在聊天里发来一段),
  按 **Ctrl+V** 或点“粘贴日志”即可直接分析剪贴板文本。片段也能用:平台识别、未识别行审计
  照常工作。焦点在筛选输入框里时 Ctrl+V 仍是正常粘贴文字,不会误触发。
  粘贴内容若一行都认不出,会直接弹出支持格式说明,而不是留个空界面让你猜。
- **总览**:上半是**仪表盘** —— hero 数字(可用率,配状态标签)+ 指标卡(断网次数/最长断网/
  未识别行/CSQ 与 LTE 信号质量)+ 断网时长分布横条;下半是仪表盘装不下的明细(温度、通道占比、
  **RX_PKT 停滞**、报错/告警、关键事件计数)。DataCall 事件按 `APP_STOP`、
  `SDK_URC/UNSOLICITED` 和旧格式未归因分开统计，并汇总 `reason`。上下不重复。
- **结论 ★**(核心):自动根因 + 处置建议 + **每条结论的日志证据(行号/时间戳)**。
  覆盖:断网根因分类(弱信号 / 数据假死 / 切卡选网期间 / 注册与账户异常)、SDK `DENY`
  注册异常与 SNR 持续偏低提示；识别旧四产品首次初始化诊断中的明确网络拒绝、受限服务、
  疑似订阅异常和 CEREG 查询/解析失败，并识别 v2 的 SIM 未插入、长时间未注册、READY 仍离线、
  连续 ping 失败重初始化等明确故障动作，严格区分【源码直证】与【推断】，
  恢复阶梯 L1/L2/L3 是否触发及**被什么门控挡住**、CP dump、温度、解析覆盖率；
  artery 的 `APP_STOP` 仅作为主动停止信息，只有 `SDK_URC + UNSOLICITED` 作为异常断线证据。
  **无证据支撑的结论一律不输出**(宁可少说,不臆测)。
  证据可单击定位原始行、右键复制或加入书签；顶部“书签”菜单可快速回跳，`Ctrl+B` 可切换
  当前已定位行。导出菜单可生成 Markdown 报告，以及内嵌三张 SVG 趋势图、小区质量、断网和
  原始证据的单文件 HTML 报告（无外部脚本或网络资源）。
- **时间线**:剔除心跳/小区噪声,只留状态变化,按类型着色。
- **断网**:逐次断网表,时长超阈值标红；单击同步图表时间准线，双击定位原始证据。
- **小区分析**:按 Cell ID 汇总样本/占比、观测驻留、平均/最低 RSRP、平均 RSRQ/SNR/CSQ、
  切入切出与断网关联；识别 5 分钟内 A→B→A 乒乓及频繁切换。双击小区可直接筛选指标与图表。
  兼容各平台日志中的 `Cell`、`cellid`、`CID`、`CellID`、`ECI`、`NCI` 字段；状态栏与信号图
  始终显示最近有效小区 ID，日志未提供时也会明确提示。
- **指标 / 信号图**:按 **CSQ、RSRP/RSRQ、SNR** 三个独立图区纵向排列；中间图可点击切换
  RSRP/RSRQ，SNR 始终单独展示，避免量纲和变化趋势相互遮蔽。
  四项指标统一显示“较差 / 一般 / 良好 / 优秀”工程分档；图中直接绘制三条分界线并标注
  等级，悬停值、指标表“LTE 工程参考”列、总览、CSV、Markdown 与 HTML 报告使用同一口径。
  综合等级取四项中的最弱项，属于保守提示策略；这些分档不是 3GPP、运营商或模组统一的
  故障等级。明确标注为非 LTE 的记录不参与分档，旧日志未提供 RAT 时按现有 LTE 日志兼容；
  SNR 为模组 SDK 上报值，不等同于所有制式的标准化 SINR。
  页面支持“图表 / 分屏 / 表格全页”三种显示方式；分屏分隔条可拖动，表格保留合理列宽并
  支持横向滚动，不再把 19 列强行压缩到一屏。
  大日志按屏幕像素桶保留峰谷并缓存绘制点,悬停用二分查询,不会随采样数线性卡顿。
  指标表和 CSV 同步包含 `Cell ID / PCI / TAC / SNR(dB) / RSSI / SRV / RAT / DENY / OPER`;
  可用 Cell/RAT/CH/DENY 快捷筛选，表头点击排序，表格与图表双向定位；CSV 会中和来自日志文本的
  `= + - @` 公式前缀，同时保持负 RSRP/RSRQ 为可计算数值。
  总览会统计小区驻留样本与占比，明确无效小区时不会沿用旧 ID；
  **ΔRX=0** 单元格单独告警；CSQ、RSRP、RSRQ、SNR 数值及 LTE 工程参考评价按四档浅色高亮。
- **标签**:标签直方图；支持表头排序、多选与 `Ctrl+C` 复制。
- **原始行**:不截断的虚拟列表，百万行仍按需取数；支持多选复制和证据定位上下文。
- **完整浏览与复制**:原始日志和事件时间线的消息列自动占满剩余页面，窗口较窄时可用横向
  滚动或 `Shift+滚轮` 浏览；原始日志、事件时间线、信号指标均可按 `Enter`/双击打开可拖动的
  完整详情区。`Ctrl+C` 复制所选行（TSV，可直接粘贴到 Excel），右键还可复制单元格、完整消息、
  整行或所选多行；详情区中的文字可自由选择复制。
- **未识别行 ★**:解析器**跳过**的行(行号 + 原文 + 粗分类)。这是“完完整整不漏消息”的
  唯一硬证据——未识别数与占比同时常显在状态栏,任何一页都看得见,不必翻页。
- **顶部筛选**(对所有页生效):标签、正则、起止时间。非法正则会在状态栏提示而非崩溃；
  最近 8 条已应用的消息正则保存在当前用户设置中，可从“历史”菜单复用或清除。
- **可访问性与窗口适配**:跟随系统浅色/深色/高对比度，导航与图表可用键盘操作，所有表格均有
  焦点和多选语义；支持 860×640 窄窗口及运行时 DPI 切换。`Ctrl+1..9` 可直达九个页面。
- **大日志内存生命周期**:关闭或替换日志时先解除虚拟表/指针视图，再释放原始行、
  指标、结论和图表容器的容量；普通筛选仍复用容量，兼顾卸载后的内存回收与筛选速度。
  普通文件按 1 MiB 分块逐行解析，多文件/压缩条目直接依次投喂解析器，不再先拼出完整
  `raw` 副本；常见标签用可释放的字典 ID，未知标签仍保留原文。

命令行:`dialLog.exe [--tab=N] [--paste] [a.log b.log ...]`,`N=0..8`
(0总览 1结论 2时间线 3断网 4指标 5标签 6原始行 7未识别行 8小区分析)。
`--paste` = 启动即分析剪贴板内容(复制完日志直接跑,不必先存文件)。

## 构建

### 交叉编译(Linux → Windows exe)

```bash
sudo apt-get install -y mingw-w64
make                     # x64: build/x64/dialLog_v1.11.2.exe
make windows-all         # 同时构建 build/x64 与 build/x86
make release             # 正式 x64 产物复制到仓库根目录
make version             # 只打印当前版本号
```

### Windows 本机 MinGW

```bat
mingw32-make CROSS=
```

本机构建输出到 `build/native/`,不会与交叉编译对象混用。

### 32 位

```bash
make windows-x86         # build/x86/dialLog_v1.11.2.exe
```

`CROSS` 同时派生 `CC/CXX/WINDRES`;每种工具链使用独立构建目录,连续切换架构
也不会复用上一架构的对象或误把旧 exe 判为最新。
三个编译器仍用 `:=` 固定,不受本机导出的 RK3576/buildroot `CC/CXX` 污染；
命令行显式传入的变量仍可覆盖。

## 自测

`src/core/` 与 `src/presentation/` 不含 Win32 依赖,可用本机 g++ 直接编译验证:

```bash
make check       # 核心全量回归:解析、场景、真代码、真机基线、合并、压缩、边界、虚拟表、图表、文档接管等
make check-full  # check + 48 个变异；靶向路由、默认并发2、带逐项进度与超时
make perf        # 10万/50万/100万行:分阶段计时 + 轻量视图/虚拟表/图表规模断言
make ui-smoke    # wine+xvfb 启动真实 exe，验证后台加载、9 页导航、虚拟原始行、小区页与窄窗布局
```

变异并发数可用 `DL_MUTATE_JOBS=1..4` 调整。
性能基准校验数据规模、分析结果和轻量视图归属,并钉死紧凑记录的尺寸上限:
x64 `LogLine` 由 248 B 降至 104 B、`MetricRow` 在加入 Cell ID/PCI/TAC 后仍控制在 192 B;
百万行 + 10 万指标样本时，两类结构主数组理论占用由约 277.7 MiB 降至 117.5 MiB。
基准还对比指针视图与对象数组的结构字节数,
统计虚拟时间线/指标表省去的预写单元格数、1920px 信号图降采样后的绘制点数,
并模拟关闭日志验证所有大 vector 的主数组容量归零;
不设置依赖机器负载的墙钟阈值。

自测会做**审计自洽校验**(`已解析 + 会话标记 + 空行 + 续行 + 未识别 == 原始行数`),
不自洽即退出码 1;并校验“每条结论都有证据”,无证据的结论同样判失败。

输入防御:单个文件最大 512MiB,一次多选展开后的日志文本总量最大 512MiB；
压缩包单条目最大 256MiB、总解压最大 512MiB、最多 1000 个条目。
gzip 会校验头部边界、ISIZE 与 CRC32；损坏包会明确报错,不会静默当普通文本分析。

## 已验证 / 未验证(事实与推断分开)

### ✅ 已在**真机日志**上验证
- `dial_20260630_000026.log`(EG25,2861 行,现网真实日志):平台识别=EG25;
  未识别 0 行(2860+1+0+0=2861 自洽);断网 36 次 / 累计 34m02s / 可用率 95.166% /
  最长 3m20s / CSQ 2-17.9-28 / 弱信号 84 次 / 温度 78°C / 通道 ROAMLINK 88%-SIM 12%。
  以上与本工具早期版本、与 `tools/diallog.py` 的结果**逐项一致**。
- GUI:9 个页签由 wine 自动烟测覆盖(含虚拟原始行、小区分析与三张信号图)。
- 粘贴路径:wine 下实测(剪贴板塞入 80 行真实日志片段 → `--paste` 载入),
  平台正确识别为 EG25、审计自洽(79 解析+1 会话标记=80)、总览正常渲染。

> **RX_PKT 停滞由 17 段变为 18 段(最长 13m34s → 4m57s),这是修正而非回归**:
> 早期只认 roamlink 通道的 `RX_PKT`,SIM 通道整段没有 RX 样本,于是相邻两个 roamlink
> 样本会跨越整个 SIM 通道时段被误判成一大段“假死”。补入 `rx_packets=`(与 `RX_PKT`
> 同源,都来自 `nw_get_rmnet_rx_packets_sum`,`eg25/nw/nw.c:404-413`)后,
> 那段 13m34s 被真实样本拆成 4m31s + 3m30s。

### ⚠️ 仅源码实证 + 合成夹具,**未经真机日志验证**
- **modem_mng_v2**:BusyBox RFC3164 包络、EC200A/EG25 动态识别、CSQ/注册/联网状态、断网配对
  及 SIM/注册/ping 诊断均由产品源码、源码输出审计和合成回归覆盖；当前**没有 v2 真机日志**，
  合成夹具只证明工具按源码预期工作，不能把运行期格式或诊断升级为【样本实证】。
- **四份产品新增心跳字段**:`SRV/RAT/DENY`、`RSRP/RSRQ/SNR/RSSI` 与 `OPER` 已由产品源码、
  SDK 头文件和四套 host-run 真代码输出共同验证；AG35 1.32.0 SD 真机日志还逐字段验证了
  `SRV=2 RAT=LTE DENY=0`、LTE 信号值和 `OPER/CID/IP`。其余产品真机样本尚未覆盖全部新字段。
  SDK 头文件直证 `SNR` 原始单位为 0.1 dB;“至少 5 个样本且半数 `SNR <= 0 dB`”仅为
  **【推断】提示**,不会单独归因断网。不同 SDK 的 `DENY` 枚举表不同,工具保留原始码而不跨产品套名称。
- **四份产品首次初始化 SIM 诊断**:`NETWORK REJECTED`、`LIMITED SERVICE`、
  `SUSPECTED subscription issue` 与 `CEREG query/parse failed` 已由 2026-08-18 产品源码和
  四条逐字场景覆盖；其中 `SUSPECTED` 已在 EC200A/AG35、EG25、artery 三套 host-run 真代码
  场景中实际输出。open_dial 的现有桩无法满足该诊断前置条件，仍由源码覆盖审计和逐字场景兜底。
  新增的 AG35 1.32.0 真机样本是正常 REG=5 场景且并非最新版本，未触发上述异常；目前仍无
  真实设备日志命中这些新消息。
- **EC200A / AG35 分支**:格式取自源码,夹具见 `samples/`(按来源分目录,见 `samples/README.md`)
  (`ec200a_synthetic.log` / `ag35_synthetic.log`)。已有 AG35 控制台真机日志和 open_dial EC200A
  真机日志，但仍无 modem_mng EC200A 独立真机日志。
- **artery(seas_log)分支**:已有 1.29.13/1.29.14 两份真机日志；1.29.16 新增的 SIM 诊断
  尚无真机样本。合成夹具 `artery_seas_synthetic.log` 含真实 ESC 字节。
- 切通道时 `roamlink_rx_packets` 清零(`eg25/dial/dial.c:1857/1875`)对停滞检测的影响:
  仅源码推断,未在真实日志上观察到。

### 覆盖率的说法只信审计,不信断言

截至 2026-08-24，审计器会遍历四个仓库的全部本地/远端分支，按真实构建范围解析
跨行调用、相邻字符串、条件编译和 C/C++ 输出入口；每个调用实例写入 `calls.tsv`，
去重后的输出形态写入 `manifest.tsv`。五份产品的源码输出形态为：

| 产品代码 | 唯一输出形态 |
|---|---:|
| open_dial | 431 |
| modem_mng EC200A/AG35（共享实现） | 662 |
| modem_mng EG25 | 570 |
| open_dial_for_artery | 291 |
| modem_mng_v2（EC200A/EG25 运行时动态识别） | 143 |
| **合计** | **2097** |

其中带时间/通道包络的结构化输出必须全部解析；裸 `printf`、`perror`、`iostream`
没有可靠时间戳，解析器会逐行原样保留为 `CONSOLE`，若借用前一条时间会用 `~` 明示。
v2 的 143 种形态进一步分为 136 种持久 syslog 和 7 种直接控制台输出，未分类入口为 0；
其 syslog 夹具使用真实 BusyBox RFC3164 包络，而不是把 stderr 的 `[LEVEL]` 前缀伪装进正文。
统计按正常部署可留存的 syslog 通道计数：每个 `log_*` 调用固定产生的 stderr 镜像，以及
`system | logger` 同时写往 stderr 的副本，不重复扩成第二套形态；五种 stderr 级别包络另有独立回归。
少数运行时格式串入口也会逐调用点列出，但静态穷举不能证明 `%s` 等运行时参数不会出现
新内容。因此这里声称的是“源码调用点与静态输出形态无遗漏”，不是“所有真机参数值可预知”；
真实设备仍以**“未识别行”页 + 状态栏占比**审计。

## 版本

版本号**只在 `version.h` 里改**这一个地方,三处自动同步(已实测:改成 9.9.9 三处全跟着变):

| 处 | 表现 |
|---|---|
| **exe 文件名** | `dialLog_v1.11.8.exe`(Makefile 从 `version.h` 解析) |
| exe 版本资源 | 右键→属性→详细信息:`FileVersion` / `OriginalFilename` |
| 标题栏 | `dialLog v1.11.8 — 拨号日志分析` |

文件名自带版本号:发给别人、存档、收截图时都不会搞混是哪个 build。
`make clean` 只清理 `build/`；测试程序也位于 `build/tests/`，不会污染根目录或误删已提交的发布 exe。

| 版本 | 内容 |
|---|---|
| 1.0.0 | 首版:仅 `FMT_SD` 格式,只在一份 EG25 日志上验证 |
| 1.1.0 | 双格式(+artery `seas_log`)、平台自动识别、未识别行审计、结论引擎 |
| 1.2.0 | 剪贴板粘贴分析(按钮 / Ctrl+V / `--paste`) |
| 1.3.0 | 修 CP dump 假阳性(正常设备曾被报"基带崩溃")、恢复阶梯改按**事件**计数(4 次曾被报成 8 次)、多行条目续行并入上一条、混合大小写标签 `[NetCheck]`、接活死分支"数据服务未就绪" |
| 1.4.0–1.7.2 | 多文件定序、压缩包直读、跨文件/时钟跳变防御、DPI/配色与 open_dial 断网兼容修复 |
| 1.8.0 | RSRP/RSRQ 提取、质量评价与 SDK 短断网归类 |
| 1.9.0–1.9.3 | 信号曲线与后续修订 |
| 1.10.0 | 跟进四份产品的新心跳字段;SNR 加入表格、CSV、总览、图表和保守诊断;信号图改为上下两个单 Y 轴 |
| 1.10.1 | 修复 CRLF 重复空行;强化文件/压缩包边界和 gzip 完整性校验;完善 Windows 双架构构建、CI 与变异测试 |
| 1.10.2 | 隔离 x64/x86 构建目录;限制批量输入总量并减少合并复制;CSV 原子写入;修复 DPI 切换后的等宽字体 |
| 1.10.3 | 单遍筛选;断网时间窗口索引及乱序回退;页面按需渲染;新增百万行性能基准 |
| 1.10.4 | 筛选结果改为轻量指针视图;复用同一套分析实现;解析后提前释放原始文本;新增内存断言 |
| 1.10.5 | 时间线和指标表改为虚拟列表,只按需格式化可见单元格;新增表格格式回归和百万行免预写统计 |
| 1.10.6 | 信号图按像素桶保留峰谷并缓存绘制点;悬停改为二分查询;新增百万点图表回归 |
| 1.10.7 | 关闭/替换日志时按借用顺序释放模型及容器容量;降低连续分析大日志的驻留与切换峰值内存;新增百万行卸载容量回归 |
| 1.10.8 | 紧凑日志/指标记录与常见标签字典;心跳字段单遍低分配扫描;普通文件分块流式解析;多文件/压缩条目取消合并 `raw` 副本;新增增量等价与结构尺寸回归 |
| 1.10.9 | 修复 CSV 完整时间戳在 Excel 中显示为井号和信号图标题重叠;补齐 `+QENG servingcell` 的 Cell ID/PCI/TAC 解析;诊断报告新增日志起止时间 |
| 1.10.10 | 完善小区 ID 展示;统一 CSQ/RSRP/RSRQ/SNR 的 LTE 工程参考分档与阈值线;原始日志、事件时间线和信号指标新增全页/分屏浏览、可拖动详情及完整复制交互 |
| 1.10.11 | 跟进四产品首次初始化 SIM 诊断;区分明确网络拒绝、受限服务、疑似订阅异常与 CEREG 查询失败，并纳入断网根因证据 |
| 1.10.12 | 穷举四产品全部输出入口（含跨行、条件分支、printf/LOG/QLOG/iostream）；Android/syslog 结构化解析，裸控制台输出逐行保留且推定时间显式标记 |
| 1.10.13 | 修复孤立/异常恢复污染断网与可用率；按来源实际观测时长计算覆盖；区分日志打开和进程启动；EG25 门控改按 policy/CH/联网实证；补 NUL、跨来源 RX、QENG 信号审计和同 source 授时跳变切段；接入 artery 标准化断网起止事件 |
| 1.11.0 | 加入 modem_mng_v2：BusyBox RFC3164 syslog 与 stderr 调试镜像解析、EC200A/EG25 动态识别、v2 CSQ/状态/断网和保守诊断；源码输出审计扩为五产品；RFC3164 推定年份以 `~` 明示 |
| 1.11.1 | 接入 EG25 新增落盘日志：诊断 `QL_Data_Call_Init` 致命退出、Data Call Start 失败和 APN 文件/JSON 失败；仅以 5 分钟内版本横幅关联退出后的重新启动，并兼容 `[LOG_E]/[LOG_I]/[LOG_D]` 时间线 |
| 1.11.2 | 区分 artery DataCall 的 `APP_STOP` 与 `SDK_URC/UNSOLICITED`，将后者标为异常断线证据并汇总 `reason`；兼容原 `DataCall disconnected \| profile=...` 格式；接入 EG25 `[SYSTEM]/[RECOVERY]/[APN]` 业务标题，补充 artery 样例和回归测试 |
| 1.11.3 | 接入 IMX6ULL modem_mng：HB30/HB300 指标、小区与流量解析，online/RECOVERY 断网配对和状态机故障诊断；排除陈旧详细快照导致的 RX 假阳性 |
| 1.11.4 | 适配 IMX6ULL RTMS 1.25.1 的分级 PDP/CFUN/硬件恢复、AT 超时确认探测和可配置探测端点；避免进行中的 RECOVERY 动作提前结束断网 |
| 1.11.5 | 识别 artery 与 RTMS EG25 的“COPS 手动锁网→COPS=0 成功→恢复注册”证据链；独立统计冷启动注册等待与日志空洞，CFUN 重试不缩短首次等待 |
| 1.11.6 | 可用率拆分为首次联网后运行期可用率与全程服务可达率；启动期未联网和日志末尾未恢复断网纳入不可用统计，避免首次联网失败误显 100% |
| 1.11.7 | 完整解析 open_dial `HEARTBEAT-NET` 数据面状态；跨日文件和 L3 进程重拉合并同一设备主事故，并按现场阈值输出结论 |
| 1.11.8 | 接入 RK3506J RTMS 1.28.1：兼容 `sample_ms`、`temp_c=(unavailable)`、压缩 AT 应答和按变更输出的 PLMN；新版 HB300 流量计数持续纳入分析 |

> `dialLog.exe` **有意入库**(方便直接取用,不必装 MinGW)。代价是每次提交都往 git 历史塞约 1.8MiB
> 且永久留存。**约定:只在升版本号时提交 exe**,日常改源码不要跟着提交,否则仓库会被二进制撑爆。

## 界面配色

配色收敛在 `src/win32/theme.h`,取自经过校验的参考调色板(CVD 色盲安全 + 对比度达标),按**角色**命名,
代码里不写裸 hex。几条硬规矩(改界面前先读):

- **文字只用 ink 系**(primary/secondary/muted),**绝不用数据色** —— 浅色系列色当文字看不清;
  身份靠文字**旁边**的色块承载(如 hero 旁的状态点)。
- **status 四色是保留色**,不得挪用作"第 N 条序列";浅底上 warning/serious 对比度不足 3:1
  是设计如此,**必须配文字标签**(所以 hero 旁写着"良好/偏低/差",不是只有个红点)。
- **网格线/轴:一档灰、1px 实线、退让**;不要虚线。
- **绝不画双 Y 轴**。两个量纲不同的量(如 CSQ 和温度)→ 两张图,不是两个 Y 轴。
- 条形:≤24px 厚,数据端 4px 圆角、基线端方角;单序列不配图例(标题已说明画的是什么)。

## 源码结构

| 文件 | 说明 |
|---|---|
| `src/core/` | 纯 C++ 核心：类型、时间、解析、分析、筛选和压缩读取；`logmodel.h` 是兼容聚合头 |
| `src/app/` | 文档状态/释放顺序与 Win32 应用上下文 |
| `src/presentation/` | 虚拟表格和图表数据模型(纯标准 C++17,可单独测试) |
| `src/win32/ui.cpp` | 主窗口创建、布局、消息循环与程序入口 |
| `src/win32/ui_pages.*` | 页签编排、虚拟列表通知和页面按需渲染 |
| `src/win32/overview_page.*` / `chart_page.*` | 总览/结论与信号图自绘 |
| `src/win32/load_controller.*` | 后台打开/取消、拖拽、粘贴、分析刷新及 CSV/Markdown/HTML 导出流程 |
| `src/win32/win_file_io.*` / `win_text.*` | Win32 文件、压缩来源、原子写入与 UTF-8/UTF-16 边界 |
| `tests/unit/` / `regression/` / `performance/` / `ui/` | 单元、行为/真机基线、百万行性能与 GUI 烟测 |
| `third_party/miniz/` | 第三方压缩实现 |
| `resources/windows/` | Windows 图标、manifest 与版本资源脚本 |
| `version.h` | **版本号单一来源**(C++ 与 resource.rc 共用);改版本只改这里 |
| `samples/` | 测试夹具,按来源分目录(rtms_eg25 / rtms_ag35 / rtms_ec200a / rtms_v2 / dial_ec200a / dial_eg25);见其 README |
| `src/win32/theme.h` | **界面配色单一来源**(校验过的调色板,按角色命名) |
| `Makefile` | 构建 |

依赖方向为 `src/win32 -> src/app + src/presentation -> src/core`；核心层不依赖 Win32。
