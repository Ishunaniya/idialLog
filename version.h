// version.h — 版本号单一来源(C++ 与 resource.rc 共用,避免两处各写一份对不上)
//
// 改版本只改这三个数字。resource.rc 的 VS_VERSION_INFO(exe 右键→属性→详细信息)
// 与标题栏显示都从这里取。
//
// 版本沿革:
//   1.0.0  首版:仅 FMT_SD 格式,只在一份 EG25 日志上验证
//   1.1.0  双格式(+artery seas_log)、平台自动识别、未识别行审计、结论引擎
//   1.2.0  剪贴板粘贴分析(按钮 / Ctrl+V / --paste)
#pragma once

#define DL_VER_MAJOR 1
#define DL_VER_MINOR 2
#define DL_VER_PATCH 0

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
