// version.h — 版本号单一来源(C++ 与 resource.rc 共用,避免两处各写一份对不上)
//
// 改版本只改这三个数字。resource.rc 的 VS_VERSION_INFO(exe 右键→属性→详细信息)
// 与标题栏显示都从这里取。
//
// 版本沿革:
//   1.0.0  首版:仅 FMT_SD 格式,只在一份 EG25 日志上验证
//   1.1.0  双格式(+artery seas_log)、平台自动识别、未识别行审计、结论引擎
//   1.2.0  剪贴板粘贴分析(按钮 / Ctrl+V / --paste)
//   1.3.0  修 CP dump 假阳性(正常设备曾被报"基带崩溃")、恢复阶梯改按事件计数
//   1.4.0  多文件合并按时间定序(修时间线/断网/可用率错算);
//          跨时基混合防护:墙钟与"时钟未同步"(1970)日志混合拖入时排除未同步批
//   1.5.0  压缩包直读(.zip/.tar.gz/.gz,内嵌 miniz)+ UTF-8 BOM 剥离
//   1.6.0  跨文件续行防御(多文件合并不串味)+ 时钟跳变检测报告(未授时日志中途授时)
//   1.7.0  UI:应用图标 + 行/断网着色改用 CVD 安全角色色(删违规裸RGB) + PerMonitorV2 DPI 缩放
//   1.7.1  修断网漏报:引擎原只认 modem_mng "recovered after Ns",漏 open_dial "Down: Ns"→报0次
//   1.7.2  修 open_dial 会话标记漏认:"=== Dial Program Started ===" 曾误计未识别
//   1.8.0  UI 卡片化 + 斑马纹;RSRP/RSRQ 提取与质量评价;结论纳入 RSRP;
//          新增"SDK 短断网(链路抖动)"断网归类
//   1.9.0  RSRP 曲线并入信号图
//   1.10.0  跟进四份产品的新心跳:SRV/RAT/DENY、RSRP/RSRQ/SNR/RSSI、OPER;
//           SNR 加入明细/CSV/总览/信号图及保守诊断,信号图改为上下两个单 Y 轴
//   1.10.1  修复 CRLF 重复空行;强化文件/压缩包边界和 gzip 完整性校验;
//           完善 Windows x64/x86 构建、自动回归与变异测试
//   1.10.2  隔离 x64/x86 构建目录;限制批量输入总量并减少合并复制;
//           CSV 改为原子写入,修复 DPI 切换后等宽字体丢失
//   1.10.3  筛选改为单次遍历;断网分析增加时间窗口索引和乱序回退;
//           页面按需渲染,新增百万行性能基准
//   1.10.4  筛选结果改为借用原始日志的轻量指针视图;
//           断网/指标/结论复用同一套拥有/借用分析实现,解析后提前释放原始文本
#pragma once

#define DL_VER_MAJOR 1
#define DL_VER_MINOR 10
#define DL_VER_PATCH 4

#define DL_STRINGIFY2(x) #x
#define DL_STRINGIFY(x)  DL_STRINGIFY2(x)

// "1.2.0"
#define DL_VER_STR   DL_STRINGIFY(DL_VER_MAJOR) "." DL_STRINGIFY(DL_VER_MINOR) "." DL_STRINGIFY(DL_VER_PATCH)
// "1.2.0.0" —— Windows 版本资源惯例是四段
#define DL_VER_STR4  DL_VER_STR ".0"

#define DL_WIDE2(x) L##x
#define DL_WIDE(x)  DL_WIDE2(x)
// L"1.2.0"
#define DL_VER_WSTR DL_WIDE(DL_VER_STR)

#define DL_APP_NAME   "dialLog"
#define DL_APP_NAME_W L"dialLog"

// "dialLog_v1.2.0.exe" —— 与 Makefile 生成的文件名保持一致(Makefile 从上面三个数字解析)
#define DL_EXE_NAME   DL_APP_NAME "_v" DL_VER_STR ".exe"
