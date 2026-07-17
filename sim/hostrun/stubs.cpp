// stubs.cpp — 让**真实的 modem_mng 拨号代码**能在本机 x86 跑起来的最小假外设层。
//
// 设计原则(决定这套东西有没有价值):
//   1. **只打桩"设备/外部库",绝不替代业务逻辑**。
//      dial.cpp / at.c / sim.c / apn.c / data_call.c / nw.c / diag.c / misc.c / slot_mgr.c
//      全部是**编进来的真代码**。它们打什么日志、走哪个分支,由它们自己决定。
//   2. 桩只负责"装病":SDK 说拨号成功还是失败、SIM 在不在 —— 由环境变量控制。
//      **真代码怎么反应,是真代码的事**,不是我写的。
//   3. AT 命令与 ping 不在这里打桩 —— 真代码是用 popen("serial_atcmd ...") /
//      system("ping ...") 调外部命令的,故用 PATH 上的假可执行文件拦截(见 fakebin/)。
//      那样连"怎么解析 AT 应答"都跑的是真代码。
//
// 能力边界:桩的**行为**(返回成功/失败)是我选的 → 这套东西能验"我有没有读错源码的控制流",
// **验不了**"真设备会不会干出源码字面之外的事"。后者只有真机日志能验。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "ql_sim.h"


extern "C" {

// ============================ 环境变量:故障注入开关 ============================
static int envi(const char* k, int dflt) {
    const char* v = getenv(k);
    return v ? atoi(v) : dflt;
}

// ============================ Quectel SDK 桩(15 个) ============================
// 真头文件来自 SDK sysroot;这里只给假实现。
// 回调保存下来,由 driver 在需要时触发,模拟 SDK 的异步通知。
typedef void (*status_ind_cb_t)(void*, int, int, void*);
static void* g_status_cb = nullptr;
static void* g_err_cb    = nullptr;
static void* g_sim_cb    = nullptr;
void* sim_get_status_cb() { return g_status_cb; }

int  ql_data_call_init(void)                        { return envi("SIM_DATACALL_INIT_RET", 0); }
int  ql_data_call_deinit(void)                      { return 0; }
void* ql_data_call_param_alloc(void)                { static int dummy; return &dummy; }
void ql_data_call_param_free(void*)                 {}
int  ql_data_call_param_set_apn_id(void*, int)      { return 0; }
int  ql_data_call_param_set_ip_version(void*, int)  { return 0; }
int  ql_data_call_param_set_reconnect_mode(void*, int)     { return 0; }
int  ql_data_call_param_set_reconnect_interval(void*, int) { return 0; }
int  ql_data_call_create(void*, const char*)        { return 0; }
int  ql_data_call_config(void*, void*)              { return 0; }
int  ql_data_call_release(void*)                    { return 0; }
int  ql_data_call_set_apn_config(int, int, const char*, const char*, const char*, int)
                                                    { return envi("SIM_SET_APN_RET", 0); }
int  ql_data_call_set_status_ind_cb(void* cb)       { g_status_cb = cb; return 0; }
int  ql_data_call_set_service_error_cb(void* cb)    { g_err_cb = cb; return 0; }
int  ql_data_call_start(const char*, int)           { return envi("SIM_DATACALL_START_RET", 0); }
int  ql_data_call_stop(const char*, int)            { return 0; }
/* ql_sim_get_card_info:签名与结构体都用 SDK 真头文件的
 * (int ql_sim_get_card_info(QL_SIM_SLOT_E, ql_sim_card_info_t*))。
 * 踩过的坑:早先按 void* + memset(info,0,64) 猜,结构体远大于 64 字节 → 段错误。
 * 教训:桩的**签名与类型**必须取自真头文件,不能猜。 */
int  ql_sim_get_card_info(QL_SIM_SLOT_E, ql_sim_card_info_t* p_info) {
    if (!p_info) return -1;
    *p_info = ql_sim_card_info_t{};                       // 真类型,尺寸由编译器算
    if (envi("SIM_CARD_ABSENT", 0)) return -1;            // 模拟拔卡 → 真代码自己决定怎么办
    p_info->app_3gpp.app_state = QL_SIM_APP_STATE_READY;  // 卡在位且就绪
    return 0;
}
// ql_sim_set_card_status_cb 的真签名带回调类型 → 交给 sdk_stubs.cpp
// (那份是从 SDK 真头**自动生成**的,签名不会错)。
// 教训:凡是我手写的桩签名,只要和真头对不上,编译器立刻打脸 —— 这是好事。

// ============================ appmng(看门狗心跳)============================
int cpactive_upt_atime(void*) { return 0; }
void* cpactive_init(const char*) { static int d; return &d; }
void cpactive_deinit(void*) {}

// ============================ LED 控制 ============================
void* LEDControl_create(void)                    { static int d; return &d; }
void  LEDControl_destroy(void*)                  {}
int   LEDControl_controlLight(void*, int id, int st) {
    // LED 也打条日志,方便核对 dial.cpp 的 [LED] 边沿去重逻辑
    fprintf(stderr, "[stub] LED id=%d state=%d\n", id, st);
    return 0;
}

// ============================ iniparser(tz.ini 等)============================
void* iniparser_load(const char*)                { return nullptr; }
void  iniparser_freedict(void*)                  {}
int   iniparser_getint(void*, const char*, int notfound) { return notfound; }
int   iniparser_set(void*, const char*, const char*)     { return 0; }
int   iniparser_dump_ini(void*, void*)           { return 0; }

} // extern "C"

// ============================ json-c 桩 ============================
// 说明:json-c 只是 apn.json 的解析库(外部依赖),**不是拨号逻辑**。
// 本机未装 libjson-c-dev,故打桩。返回"找不到"→ 真代码走它自己的
// "iccid not matched in apn.json, fall back to auto/default APN" 分支 ——
// 那个分支的选择仍是真代码做的,不是我替它做的。
extern "C" {
void* json_tokener_parse(const char*)                    { return nullptr; }
int   json_object_object_get_ex(void*, const char*, void**) { return 0; }
void* json_object_array_get_idx(void*, size_t)           { return nullptr; }
int   json_object_array_length(void*)                    { return 0; }
const char* json_object_get_string(void*)                { return ""; }
int   json_object_put(void*)                             { return 0; }
// LED 补两个
int   LEDControl_blinkLight(void*, int, int, int)        { return 0; }
int   LEDControl_getLedIdType(void*, const char*)        { return 0; }
// android log(diag.c 的 logcat 抓取用)
int   __android_log_print(int, const char*, const char*, ...) { return 0; }
// SDK 补齐
int   ql_data_call_get_status(const char*, int, void*)   { return 0; }
int   ql_data_call_get_list(void*, int*)                 { return 0; }
int   ql_data_call_param_get_apn_id(void*)               { return 6; }
int   ql_data_call_param_get_ip_version(void*)           { return 0; }
}

// ============================ nanomsg 桩 ============================
// nanomsg 是**对外 IPC 通道**(38001 状态查询 / 48001 事件发布),不是拨号逻辑。
// 打桩 = 没人收消息,真代码照常发 —— 它发不发、发什么,仍是真代码决定的。
extern "C" {
int  nn_socket(int, int)                 { return 3; }
int  nn_close(int)                       { return 0; }
int  nn_bind(int, const char*)           { return 0; }
int  nn_connect(int, const char*)        { return 0; }
int  nn_send(int, const void*, size_t len, int)  { return (int)len; }
int  nn_recv(int, void*, size_t, int)    { return -1; }
int  nn_setsockopt(int, int, int, const void*, size_t) { return 0; }
int  nn_shutdown(int, int)               { return 0; }
int  nn_errno(void)                      { return 0; }
const char* nn_strerror(int)             { return "stub"; }
void nn_term(void)                       {}
}

// ============================ AG35 双卡专用桩 ============================
// 只有 -DQL_MODULE_PLATFORM_AG35 时 slot_mgr.c 才非空(否则整文件被 #ifdef 关掉,
// 编出来是 0 个函数的空 TU)。这两个 API 只在 AG35 SDK 里有。
// 签名逐字取自 AG35 SDK 真头(ql-sysroots/.../ql_sim.h):
//   int ql_sim_switch_slot(QL_SIM_SLOT_E log_slot, QL_SIM_PHY_SLOT_E phy_slot);
//   int ql_sim_get_active_slots(ql_sim_active_slots_t *p_active_slots);
// (教训:上次猜 ql_sim_get_card_info 的签名+结构体大小 → 段错误。签名必须取真头。)
extern "C" {
/* 用**真类型**,不用 int/void* 猜 —— 这是第三次被真头打脸了
 * (前两次:ql_sim_get_card_info 猜 void*+memset64 → 段错误;
 *          ql_sim_set_card_status_cb 猜 void* → 编译器拒绝)。 */
int ql_sim_switch_slot(QL_SIM_SLOT_E log_slot, QL_SIM_PHY_SLOT_E phy_slot) {
    fprintf(stderr, "[stub] ql_sim_switch_slot(log=%d, phy=%d)\n", (int)log_slot, (int)phy_slot);
    const char* f = getenv("SIM_SLOT_SWITCH_FAIL");
    if (f && atoi(f)) return -1;                       // 切卡失败 → 真代码自己决定怎么办
    setenv("SIM_ACTIVE_PHY", (int)phy_slot == 2 ? "2" : "1", 1);
    return 0;
}
int ql_sim_get_active_slots(ql_sim_active_slots_t* p) {
    if (!p) return -1;
    *p = ql_sim_active_slots_t{};                      // 真类型,尺寸由编译器算
    const char* v = getenv("SIM_ACTIVE_PHY");
    int phy = v ? atoi(v) : 1;
    /* 首个逻辑槽映射到当前物理槽;字段名以 AG35 SDK 头为准 */
    memcpy(p, &phy, sizeof(int));
    return 0;
}
/* EC200A 侧 AT 便捷封装(slot_mgr.c:338 发 AT+CFUN=0):转发给假 serial_atcmd */
int Ql_SendAT(const char* atCmd) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "serial_atcmd %s > /dev/null 2>&1", atCmd ? atCmd : "");
    return system(cmd);
}
}
