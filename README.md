# dialLog — 拨号日志分析工具 (Windows 原生 / MinGW)

纯 Win32 API 的原生 Windows GUI 程序,**静态链接,无任何运行时依赖**(不需要 .NET,
不需要 MinGW 的 DLL),编译产物 `dialLog.exe` 单文件约 1.2MB,拷到 Windows 双击即用。

**目标:把日志塞进去就出结论**——不只给统计值,还给根因判断与处置建议,
且每条结论都附带可追溯的日志证据(行号 + 时间戳)。

## 支持的日志格式

| 格式 | 产生方 | 行样式 |
|---|---|---|
| `FMT_SD` | `logger_sd.c` 的 `dial_log()`,**modem_mng 与 open_dial 完全一致** | `[YYYY-MM-DD HH:MM:SS] [TAG] message` |
| `FMT_SEAS` | `open_dial_for_artery` 的 `src/seas_log/seas_log.c` | `YYYY-MM-DD HH:MM:SS.mmm [LEVEL] <ESC[0m>func (file:line) - message` |

`FMT_SEAS` 的细节(源码实证,`seas_log.c:233-296`):当前 `SEAS_DISPLAY_COLOR=0`
故**不输出颜色码**,但 `SEAS_DISPLAY_RESET=1` 故每行在级别与函数名之间**必带一个
`\x1B[0m`**;解析时统一剥离 ANSI CSI 序列,有无都能解析。日志级别在
`main.c` 配成 `SEAS_LEVEL_INFO`,故 `[DEBUG]` 不会出现在文件里。

### 平台自动识别

打开日志即自动判定来源平台并显示在标题栏/状态栏/结论页,判据均为源码实证的特征:

| 平台 | 判据 | 源码出处 |
|---|---|---|
| artery | 行格式为 seas_log | `seas_log.c:210` |
| AG35 | 心跳含 `SLOT:` 或出现 `[SLOT]` 标签 | `ec200a/dial/dial.cpp:1071-1075`(AG35-only `#ifdef`) |
| EG25 | 心跳含 `CH:`/`RL_FAIL`/`RX_PKT`,或 `[ROAMLINK]` 标签 | `eg25/diag/diag.c:109-131` |
| EC200A | 心跳为 `SIM_AT:`/`SIM_CB:` 且无 `SLOT:` | `ec200a/dial/dial.cpp:1077` |

> **EC200A 与 open_dial 不做区分**:二者行格式与心跳字段完全相同(`open_dial/dial.c:518`
> 与 `ec200a/dial/dial.cpp:1077` 逐字段一致),无据可分,故统一报为 EC200A。
>
> **IMX 不在列**:`dialer_imx6ull.cpp` 中 `dial_log` 调用数为 **0**(只用 `printf`),
> `CMakeLists.txt` 的 IMX 分支也不链接 `logger_sd.c` —— IMX 不产此类日志。

### 心跳格式逐平台不同(不要假设相同)

| 平台 | 心跳字段 | 出处 |
|---|---|---|
| EC200A | `SIM_AT / SIM_CB / REG / CSQ / **TEMP**(大写) / DownTime` | `ec200a/dial/dial.cpp:1077` |
| AG35 | 同上 + `SLOT` | `ec200a/dial/dial.cpp:1074` |
| EG25 | `CH / SIM / [REG] / CSQ / **Temp**(小写) / DownTime / ConsecFail / [RL_FAIL] / [RX_PKT]`;<br>5min 扩展行另有 `cereg= / ifname= / rx_packets=`(**等号**赋值) | `eg25/diag/diag.c:109-157` |
| artery | `state= / csq= / tcp_fail= / rl_fail=`;扩展 `cereg= / ifname= / ip= / rx_packets=`(空格分隔 k=v) | `main.c:217/239` |

字段解析器同时支持 `K:V` 与 `K=V`,并按“空白 + 标识符 + 分隔符”切分,
否则 `RSRP:-104 RSRQ:-10`(`diag.c:33`)会把 RSRQ 吞进 RSRP 的值。

## 功能

- **拖拽打开**:把一个或多个 `dial_*.log` 拖进窗口(多文件自动合并),或用“打开日志…”。
- **粘贴分析 ★**:手上没有文件时(SSH 里 `cat` 日志直接选中复制、别人在聊天里发来一段),
  按 **Ctrl+V** 或点“粘贴日志”即可直接分析剪贴板文本。片段也能用:平台识别、未识别行审计
  照常工作。焦点在筛选输入框里时 Ctrl+V 仍是正常粘贴文字,不会误触发。
  粘贴内容若一行都认不出,会直接弹出两种支持格式说明,而不是留个空界面让你猜。
- **总览**:时间跨度、进程重启次数、断网统计 + 可用率、CSQ/温度、通道占比、
  **RX_PKT 停滞(数据假死征兆)**、报错/告警、关键事件计数。
- **结论 ★**(核心):自动根因 + 处置建议 + **每条结论的日志证据(行号/时间戳)**。
  覆盖:断网根因分类(弱信号 / 数据假死 / 切卡选网期间 / 注册被拒)、
  恢复阶梯 L1/L2/L3 是否触发及**被什么门控挡住**、CP dump、温度、解析覆盖率。
  **无证据支撑的结论一律不输出**(宁可少说,不臆测)。
- **时间线**:剔除心跳/小区噪声,只留状态变化,按类型着色。
- **断网**:逐次断网表,时长超阈值标红。
- **指标 / 信号图**:上方自绘 **CSQ 折线 + 断网红带 + 弱信号阈值线**;
  下方指标表(**ΔRX=0 单元格粉色高亮 = 数据不通**,弱信号 CSQ 黄色高亮),可**导出 CSV**。
- **标签**:标签直方图。
- **原始行**:筛选后的原始日志(截断前 5000 行)。
- **未识别行 ★**:解析器**跳过**的行(行号 + 原文 + 粗分类)。这是“完完整整不漏消息”的
  唯一硬证据——未识别数与占比同时常显在状态栏,任何一页都看得见,不必翻页。
- **顶部筛选**(对所有页生效):标签、正则、起止时间。非法正则会在状态栏提示而非崩溃。

命令行:`dialLog.exe [--tab=N] [--paste] [a.log b.log ...]`,`N=0..7`
(0总览 1结论 2时间线 3断网 4指标 5标签 6原始行 7未识别行)。
`--paste` = 启动即分析剪贴板内容(复制完日志直接跑,不必先存文件)。

## 构建

### 交叉编译(Linux → Windows exe)

```bash
sudo apt-get install -y mingw-w64
make                     # 产物: dialLog.exe
```

### Windows 本机 MinGW

```bat
mingw32-make CXX=g++ WINDRES=windres
```

### 32 位

```bash
make CXX=i686-w64-mingw32-g++ WINDRES=i686-w64-mingw32-windres
```

> **注意**:Makefile 里编译器用 `:=` 而非 `?=`。本机环境常导出 `CC`/`CXX`
> (RK3576/buildroot 交叉链),`?=` 对已由环境定义的变量不生效,会误用 aarch64 编译器。
> 命令行 `make CXX=...` 仍可正常覆盖。

## 自测(解析层)

`logmodel.*` 不含 Win32 依赖,可用本机 g++ 直接编译验证:

```bash
make selftest
./selftest ../../rtms_sdk/apps/modem_mng/dial_20260630_000026.log
```

自测会做**审计自洽校验**(`已解析 + 会话标记 + 空行 + 未识别 == 原始行数`),
不自洽即退出码 1;并校验“每条结论都有证据”,无证据的结论同样判失败。

## 已验证 / 未验证(事实与推断分开)

### ✅ 已在**真机日志**上验证
- `dial_20260630_000026.log`(EG25,2861 行,现网真实日志):平台识别=EG25;
  未识别 0 行(2860+1+0+0=2861 自洽);断网 36 次 / 累计 34m02s / 可用率 95.166% /
  最长 3m20s / CSQ 2-17.9-28 / 弱信号 84 次 / 温度 78°C / 通道 ROAMLINK 88%-SIM 12%。
  以上与本工具早期版本、与 `tools/diallog.py` 的结果**逐项一致**。
- GUI:8 个页签全部在 wine 下截图确认渲染正常(含自绘 CSQ 折线图)。
- 粘贴路径:wine 下实测(剪贴板塞入 80 行真实日志片段 → `--paste` 载入),
  平台正确识别为 EG25、审计自洽(79 解析+1 会话标记=80)、总览正常渲染。

> **RX_PKT 停滞由 17 段变为 18 段(最长 13m34s → 4m57s),这是修正而非回归**:
> 早期只认 roamlink 通道的 `RX_PKT`,SIM 通道整段没有 RX 样本,于是相邻两个 roamlink
> 样本会跨越整个 SIM 通道时段被误判成一大段“假死”。补入 `rx_packets=`(与 `RX_PKT`
> 同源,都来自 `nw_get_rmnet_rx_packets_sum`,`eg25/nw/nw.c:404-413`)后,
> 那段 13m34s 被真实样本拆成 4m31s + 3m30s。

### ⚠️ 仅源码实证 + 合成夹具,**未经真机日志验证**
- **EC200A / AG35 分支**:格式取自源码,夹具见 `samples/`
  (`ec200a_synthetic.log` / `ag35_synthetic.log`)。本机无 EC200A/AG35 真机日志。
- **artery(seas_log)分支**:全机**无 artery 真机日志**;仅
  `open_dial_for_artery/md/analyse/open_dial_roamlink_analysis_v2.md` 中有 1 行文档示例
  (`2024-01-15 10:25:03.899 [INFO] dial_task (dial.c:245) - CSQ: 20`,且该示例已被文档
  剥掉 ESC 码)。夹具 `artery_seas_synthetic.log` 按 `seas_log.c` 逐段拼接,含真实 ESC 字节。
- 切通道时 `roamlink_rx_packets` 清零(`eg25/dial/dial.c:1857/1875`)对停滞检测的影响:
  仅源码推断,未在真实日志上观察到。

### 覆盖率的说法只信审计,不信断言
本工具**不声称**“覆盖了全部标签”。三仓库 `dial_log` 首参标签已穷举(modem_mng 357 处 /
open_dial 107 处 / artery 4 处内嵌标签),但穷举证明不了运行时不出现新格式。
真实覆盖率以**“未识别行”页 + 状态栏占比**为准——那是实测,不是断言。

## 版本

版本号只在 `version.h` 里改(C++ 标题栏与 `resource.rc` 的 exe 版本资源共用同一来源,不会对不上)。
标题栏显示 `dialLog vX.Y.Z`;Windows 右键 exe →属性→详细信息也能看到。

| 版本 | 内容 |
|---|---|
| 1.0.0 | 首版:仅 `FMT_SD` 格式,只在一份 EG25 日志上验证 |
| 1.1.0 | 双格式(+artery `seas_log`)、平台自动识别、未识别行审计、结论引擎 |
| 1.2.0 | 剪贴板粘贴分析(按钮 / Ctrl+V / `--paste`) |

> `dialLog.exe` **有意入库**(方便直接取用,不必装 MinGW)。代价是每次提交都往 git 历史塞 1.2MB
> 且永久留存。**约定:只在升版本号时提交 exe**,日常改源码不要跟着提交,否则仓库会被二进制撑爆。

## 文件

| 文件 | 说明 |
|---|---|
| `logmodel.h/.cpp` | 解析 + 分析 + 结论引擎(纯标准 C++17,无 Win32 依赖,可单独测试) |
| `ui.cpp` | Win32 界面层(页签/列表/自绘 CSQ 图/拖拽/粘贴/导出) |
| `selftest.cpp` | 解析层自测(含审计自洽校验、结论必带证据校验) |
| `resource.rc` + `app.manifest` | 嵌入清单:comctl32 v6 现代控件外观 + DPI 感知 + 版本信息 |
| `version.h` | **版本号单一来源**(C++ 与 resource.rc 共用);改版本只改这里 |
| `Makefile` | 构建 |
