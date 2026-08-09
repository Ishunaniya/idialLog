# CLAUDE.md

给 Claude Code 用的仓库指南。这是 **dialLog** —— 一个分析 4G 拨号守护进程日志的 Windows 原生工具。

## 这是什么

`dialLog.exe`(Win32/MinGW,静态链接,单文件无依赖)读取三个仓库的拨号日志,做**解析 + 平台识别 + 未识别行审计 + 结论引擎**(根因判断 + 处置建议 + 证据溯源)。目标:**把日志塞进去就出结论**。

分析的日志来自这四份产品代码(**不在本仓库**,是被分析对象):
- `rtms_sdk/apps/modem_mng` 的 `ec200a/dial/dial.cpp`(EC200A **和** AG35 共用,靠 `#ifdef QL_MODULE_PLATFORM_AG35` 分)
- `rtms_sdk/apps/modem_mng` 的 `eg25/dial/dial.c`(EG25)
- `/home/tronlong/lyp/code/open_dial` 的 `dial.c`(老框架 EC200A)
- `/home/tronlong/lyp/code/open_dial_for_artery` 的 `src/dial/dial.c`(老框架 EG25,**seas_log 格式**)

## 铁律(用户反复强调,违反即失败)

1. **不许臆测,不许片面**。每条结论标注证据等级:【源码直证】/【样本实证】/【推断】。全称断言("全部/都/不漏")必须**穷举证明**,做不到就写"未穷举,已覆盖 X/Y"。
2. **"结论出现了"不等于"结论对"** —— 断言必须钉死**具体数字/内容**,否则没牙齿(变异测试实证:只验"出现"时 10 个变异 5 个存活)。
3. **真机日志不可替代**。合成夹具/主机模拟只能证明"我按源码理解的那样工作",证明不了"真设备真这样"。至今真机日志抓出 ~7 个 bug,合成夹具抓出 0 个。
4. 编过了 ≠ 编进去了。`#ifdef` 平台宏关掉的文件会静默变成**空 TU**,必须 `nm` 验符号真的在(AG35 的 slot_mgr 栽过)。

## 完整测试(改任何东西后全跑一遍)

```bash
make check       # 七个测试程序:解析/场景/真代码/真机基线/合并/压缩/边界
make check-full  # check + 44 个变异；须 0 存活、0 片段失配
make perf        # 10万/50万/100万行性能与结果规模基准
```

`hostruntest`/`baselinetest` 依赖 `sim/hostrun*/` 已生成的日志(见下)。
**mutate.py 是 Python 不是 shell** —— shell 版有引号转义 bug 已删,别复活。

## 核心分层(`logmodel.*` 是命脉)

- `logmodel.h/.cpp` —— **纯标准 C++17,零 Win32 依赖**。解析 + 分析 + 结论引擎全在这。因为无 Win32 依赖,**本机 g++ 直接编译测试**,不必等 MinGW/wine。改逻辑改这里。
- `ui.cpp` —— Win32 界面层(8 页签、自绘 CSQ 图、拖拽、粘贴、导出 CSV)。
- `theme.h` —— 界面配色单一来源(校验过的 CVD 安全调色板,按角色命名,别写裸 hex;改界面前先读它顶部的硬规矩)。
- `version.h` —— **版本号唯一来源**。改一个数字,exe 文件名(`dialLog_v1.3.0.exe`)/ 版本资源 / 标题栏三处自动同步。**exe 内容变了就必须升版本**(约定:只在升版本时提交 exe)。

## 两种日志格式(解析器的根基)

- `FMT_SD`:`[YYYY-MM-DD HH:MM:SS] [TAG] msg` —— modem_mng 与 open_dial **完全一致**(`logger_sd.c`)。
- `FMT_SEAS`:`时间.毫秒 [LEVEL] <ESC[0m>func (file:line) - msg` —— 仅 artery(`seas_log.c`)。带 ESC 字节,解析时剥离。

## 主机模拟(`sim/hostrun*/`)—— 让真代码在本机跑

四个目录各把一份**真实拨号代码**(不拷贝,直接编原文件)在本机 x86 跑起来,日志由**真代码自己打**,不是我手写:

| 目录 | 真代码 | 覆盖 |
|---|---|---|
| `sim/hostrun` | modem_mng `ec200a/dial/dial.cpp` | EC200A;**加 `AG35=1 make` 才编入双卡** |
| `sim/hostrun_open_dial` | open_dial `dial.c` | 老 EC200A |
| `sim/hostrun_eg25` | modem_mng `eg25/dial/dial.c` | EG25 |
| `sim/hostrun_artery` | artery `src/dial/dial.c` | 老 EG25(seas_log) |

各 `make && ./run_scenario.sh`。故障注入靠环境变量(`SIM_PING_OK`/`SIM_CEREG`/`SIM_CSQ`/`SIM_CARD_ABSENT`/`SIM_DATACALL_INIT_RET`…),**源码一行不改**。

**这套东西能验什么、不能验什么(务必如实)**:能抓"**我读错了源码的控制流/措辞**",抓不到"**真设备干出源码字面之外的事**"(桩的行为仍是我选的)。

### 主机模拟的坑(都是被真代码/真头打脸打出来的)

- **桩签名/类型必须取自 SDK 真头,不能猜** —— 猜 `void*`/`memset(64)` → 段错误(被打脸 ≥4 次)。SDK 桩由脚本从真头自动生成(生成时先剥 `///<` 注释否则参数列表被切坏)。
- **桩"返回成功"还不够,得填出真代码要读的值** —— artery 的 `QL_MCM_SIM_GetCardStatus` 只 return 0 不填结构体 → 状态机永久卡 sim_check(745 次)。
- **ping 判定三家各不同**:EC200A 匹配 `"1 packets transmitted, 1 received"`、open_dial 匹配 `"ttl="`、EG25 只看返回码、artery 用 **TCP** 不是 ping。共用假 ping 三样都满足。
- **EG25/artery 的 AT 走真串口**(`open(/dev/smd8)`+select/read/write),用 `fakemodem.so`(LD_PRELOAD 拦 open 接 PTY);PTY slave 必须设 **O_NONBLOCK**,否则 `Ql_SendAT` 先 read 清残留时死锁。
- **`fastclock.so` 加速时钟必须同时加速 sleep/usleep/nanosleep**,否则日志内部时序失真(L2 的 CFUN=0→1 从真机 3s 变成 32min)。driver 参数是**逻辑秒**不是真实秒。
- **`roamlink.c` 里有 `reboot(RB_AUTOBOOT)`** —— fakemodem 已拦死,别靠"非 root 会失败"的侥幸。
- **场景之间要清 `/tmp/dial_retry_count`** 等状态文件,否则互相污染(快速失败计数累积会让下一轮秒退)。

## 本机构建/验证环境

- 交叉编译:`make`(默认 x64,输出 `build/x64/`);32 位用 `make windows-x86`；
  双架构用 `make windows-all`;正式根目录 x64 产物用 `make release`；
  Windows 本机用 `mingw32-make CROSS=`(输出 `build/native/`)。
- GUI 验证:`wine`(prefix 在 `$HOME/.wine-diag`,含 Noto CJK 字体 + FontSubstitutes 映射,否则中文是方块)+ Xvfb 无头截图。**GUI 从没在真 Windows 上跑过,只 wine 截图。**

## 仍未覆盖(别声称"全部")

- GUI 未在真 Windows 验证;32 位版本未编过。
- AG35 真机日志只有 fast-boot 命中,**真切卡路径**(`trigger→switch card`)无真机数据。
- **OPER 海外选网**真机 `oper_select=0`,整个模块从没在真机跑过。
- 主机模拟的桩行为是我选的,替代不了真机日志。

## 相关记忆

用户的持久记忆里有 `diaglog-tool-and-cron-20260716`(工具位置/能力/验证边界)、`feedback-fact-vs-inference`(事实与推断分开标注)、`ag35-build-recipe-gotchas`(AG35 构建三必备)。
