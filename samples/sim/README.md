# samples/sim/ — 模拟场景与源码输出审计夹具

**这些不是真机日志。** 场景夹具由 `../../tests/regression/simtest.cpp` 按源码格式生成；
`*_all_prints.log` 则由 `../../sim/gen_all_prints.py` 从产品源码输出入口生成。证据等级均为【源码实证】。

跑 `make simtest && build/tests/regression/simtest` 会重新生成场景日志并逐个断言；
`*_all_prints.log` 由下文的覆盖校验生成。两类文件均可直接拖进 GUI 检查解析结果。

## 为什么要有它:结论引擎有一半分支零真机覆盖

真机日志(见 `../README.md`)覆盖不到这些异常:never-connected 门控、注册被拒、
L3 触发、真有 CP dump、AG35 切卡、多会话重启、温度过高。
没被任何数据碰过的分支 = 可能出错的分支 —— 今天的 CP dump 假阳性就是实证。

## 它能证明什么、不能证明什么(重要)

| | |
|---|---|
| ❌ **验不了解析器** | 日志由我照源码生成,解析器也由我照同一份源码写。我若读错源码,模拟器会生成解析器正好期待的东西然后全绿 —— 拿自己的答案批自己的卷子。**解析器只能靠真机日志验证**(实证:真机日志抓出 5 个 bug,合成夹具抓出 0 个)。 |
| ✅ **能验结论引擎** | 断言是独立参照系:每个场景声明"应该得出什么结论"**和"不该得出什么结论"**。后者(假阳性守卫)是关键 —— 今天那个"正常设备被报基带崩溃"就属这一类。 |

降低循环论证的手段:**所有日志串逐字照抄源码**(`tests/regression/simtest.cpp` 顶部标了每条的 `file:line`),不是我复述。

## 断言必须有牙齿 —— 用变异测试证明

写完 10 个场景一次全绿,这本身就是"自己批自己卷子"的可疑信号。故做了变异测试
(把今天修好的 bug 故意改回去,看测试抓不抓得住):

| 变异 | 首轮结果 |
|---|---|
| CP dump 判据退回"见 `[CPDUMP]` 标签就报崩溃" | ✅ **被抓住**(假阳性守卫生效) |
| 恢复阶梯退回按**行**计数(而非按事件) | ❌ **没抓住** —— 测试有洞 |

第二个漏了,是因为场景只断言「恢复阶梯已生效」出现、**没断言次数**。已补场景
`eg25_recovery_multiline_count`(2 次 L1 共 4 行 + 1 次 L2 共 3 行),直接断言
「L1 触发 2 次,L2 触发 1 次」。补后变异 2 也被抓住。

**教训:只断言"某结论出现了"往往没有牙齿,得断言它的具体内容。**

## 全打印覆盖校验(`sim/check_coverage.sh`)

回答"是不是全部了"—— **用可度量的覆盖率,不是嘴上说**。

```bash
sim/check_coverage.sh /home/tronlong/lyp/code/open_dial
sim/check_coverage.sh /home/tronlong/lyp/code/rtms_sdk/apps/modem_mng
sim/check_coverage.sh /home/tronlong/lyp/code/rtms_sdk/apps/modem_mng_v2
sim/check_coverage.sh /home/tronlong/lyp/code/open_dial_for_artery
```

做法：遍历仓库**全部本地/远端分支**及真实构建范围，用 C/C++ 词法扫描完整调用，
处理跨行参数、相邻字符串、注释、`#if 0/1` 和其他条件编译；覆盖
`dial_log`、`SEAS_LOG_*`、`LOG_*/QLOG*/ALOG*`、Android/syslog、
`printf/perror/fprintf(stderr)` 与 C++ iostream。反向扫描还会把名字疑似输出、
却未分类的 API 作为失败项，防止只扩充白名单而漏掉新通道。

每个分支调用实例写入 `*.calls.tsv`，去重后的静态输出形态写入
`*.manifest.tsv`，填充占位符生成日志后再与解析结果逐项对账。截至
2026-08-24 的审计结果：

| 产品代码 | 唯一输出形态 | 结构化解析 | 裸输出保留 |
|---|---:|---:|---:|
| `open_dial` | **431** | 151/151 | 281 行 |
| `modem_mng` EC200A/AG35（共享实现） | **662** | 与下项合计 630/630 | 与下项合计 606 行 |
| `modem_mng` EG25 | **570** | 与上项合计 630/630 | 与上项合计 606 行 |
| `open_dial_for_artery` | **291** | 261/261 | 30 行 |
| `modem_mng_v2`（EC200A/EG25 动态识别） | **143** | 136/136 | 7 行 |

v2 表按正常部署可留存的 syslog 通道统计；`log_*` 和两个 `system | logger` 的确定性 stderr
镜像由五种级别包络回归单独覆盖，不在形态表中重复计数。目标 BusyBox 的警告包络为 `user.warn`。

调用实例清单分别为 `open_dial` 3823、`modem_mng` 24521、`modem_mng_v2` 324、
artery 1035 条（合计 29703）；
这是跨分支实例数，同一个源码调用在多个分支出现会分别列出。modem_mng 的标签
合计 38 个，open_dial 21 个，artery 6 个，全部由 manifest 对账保留。v2 的 syslog
正文通常没有 `[TAG]`，应用身份来自 `modem_mng_v2[pid]:` 包络，不把包络标签与正文标签混算。

分支间确有差异(实证):`open_dial` 的 `fix_cp_dump` 用 `[WARN] Status updated`,
另两个分支用 `[INFO] Status updated`;`Registration Denied` 的 code 一处是 `%d`、
一处硬编码 `3`。故必须取**全分支并集**,只看当前分支会漏。

### 这个校验能证明什么、不能证明什么

- ✅ **调用点与静态格式串**逐字取自源码，且 `calls.tsv` 可逐项追溯；结构化输出
  全部解析，无法结构化的裸输出也不会再丢弃。
- ⚠️ **占位符替换值是脚本选的**，6 个 modem、2 个 open_dial、1 个 artery 以及 2 个 v2
  动态格式入口的正文也只能在运行时确定，因此不能推出“所有真机参数值均已穷举”。
  实证:真机 `"[RECOVERY L2] AT+CFUN=0 rsp: %s"` 的 `%s` 是**多行**的 `\nOK\n`,
  我原先没料到 → 续行被当未识别丢弃(由真机日志抓出,非本校验)。
  故脚本对 `rsp:`/`response:` 后的 `%s` 专门还原多行形态,但**别的 %s 仍可能有我没想到的形态**。

**真机日志不可替代。** 本校验只是把"源码层面的漏"清零。
