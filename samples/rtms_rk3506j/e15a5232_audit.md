# e15a5232 补充核验

源仓库 `/home/tronlong/lyp/code/rtms_sdk` 的提交
`e15a52329cf9cf8e19983c92da6f1124fb3d76e7` 是 `461b2b2f` 的直接后续提交。
变更共 10 文件、667 行新增、403 行删除，RTMS 版本号仍为 1.28.6。
核验依据提交对象及完整差异，未使用源仓库未提交内容。提交快照与差异保存于
`build/source-audit/e15a5232/`。

| 源文件（相对 apps/modem_mng） | 改动与本工具适用性 |
| --- | --- |
| nanomsg_json_frame.hpp | 按 NN_MSG 字节长度解析 JSON，允许一个尾部 NUL，拒绝多帧/垃圾；设备侧解析机制，无需移植 |
| nanomsg_process.cpp | 精确接口与 ifindex 双读、64 位字节计数、旧 int 接口饱和转换及新消息帧调用；既有 NANOMSG 日志和控制台 JSON 回显保留，不能将字节累计值解析为包计数 |
| nanomsg_process.hpp | 共享流量监控累计值/线程/恢复及事务改写，新增 TRAFFIC；需公共告警、错误筛选和时间线，不能只归为 RK 故障 |
| rk3506j/at/quectel_parser.cpp | 严格手动数字 PLMN、LTE AcT=7、完整行匹配；命令回显、其他 PLMN 和部分行不证明已选中目标 |
| rk3506j/at/quectel_parser.hpp | 严格匹配接口声明；无需移植设备解析器 |
| rk3506j/dial/rk3506j_dialer.cpp | 新 COPS 事务日志、历史验证失败后扫描、候选失败后下一候选；EG912 第四级改回 FAILURE_RETRY 循环，需诊断并保留实际公网恢复边沿 |
| rk3506j/dial/rk3506j_dialer.hpp | 严格匹配同时要求终端 OK 且无终端 ERROR；旧版相同的候选 set operator OK 仅做字符串检查，不能凭未变版本号或该旧日志提升证据 |
| traffic_sql/sql_traffic.hpp | 数据库就绪、原子初始化、合法持久值/时间戳及错误结果；无需复制 SQLite schema/事务机制，相关失败按 TRAFFIC reason 呈现 |
| traffic_sql/traffic_accounting.hpp | 64 位累加、首次基线、接口替换/计数重置、可逆月份及保存月份保护；设备侧计费逻辑，不改变 HB300 包计数 schema |
| traffic_sql/traffic_sql_interface.cpp | 回传时间数组明确 NUL 终止；未产生新日志，无额外解析适配 |

本仓库增加公共流量缺失告警，保留监控累计失败 count 和稀疏输出的证据边界。
接口读失败表示缺失采样；数据库写失败可保留内存累计，不据此认定公网断开或累计流量归零。
RK 选网事务失败/未验证仅报告对应失败，ECM 停止被拒绝不单独报选网失败。
仅新 `history PLMN verified` 直证生成严格历史验证提示；旧候选 OK 仍作为流程事件。
选网、WRITE_TO_MODEM 和 EG912 循环都不作为公网成功，断网继续按心跳及公网恢复直证配对。

新增验证位于 `tests/regression/rk3506jtest.cpp` 的 `e15TrafficAndSelection()`，包括
五个平台共享流量、固定和异常 reason、count 格式/限流、来源隔离、准确证据行、
ECM 停止容错、CFUN/候选失败、旧候选弱检查、五/六位历史 PLMN、无公网成功、
EG912 循环和既有坏请求/64 位 JSON 回显。增加 4 项行为变异，整轮变异扩至 56 项。

尚无该提交的新真机样本；回归及控制台输入是源码派生验证，不能替代现场验证。
最终验证：13 个常规测试程序全部通过，56 项变异全部被抓住，0 存活、0 片段失配、
0 验证异常；10 万、50 万、100 万行性能检查通过，Windows x64/x86 构建通过。
按用户要求，当前源码将两轮适配统一为 1.11.10，完整保留上述功能和回归。
上述双架构构建验证在统一版本号前完成，既有产物保留当时的版本号；
统一版本后的可执行文件由用户自行编译，输出名称为 `dialLog_v1.11.10.exe`。
最终检查结果保存于 `build/rk3506j-e15-check-full-verified.log`、
`build/rk3506j-e15-perf.log` 和 `build/rk3506j-e15-windows-verified.log`。
早先未带 `verified` 的全量/构建日志记录了编译异常或进程 Hangup，未作为通过依据；
重跑完整全量检查和双架构构建均以退出码 0 完成。
