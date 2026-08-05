// sdk_stubs.cpp —— **自动生成**:签名逐字取自 SDK 真头文件,不是我手写的。
// 生成方式见 sim/hostrun/README.md。这些桩一律返回成功(0)/空,
// 具体"装病"由 fakebin/ 的假 serial_atcmd + 环境变量控制。
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ql_atc.h"

#include "ql_data_call.h"

#include "ql_nw.h"
#include "ql_sim.h"

extern "C" {

static int sim_int(const char *name, int fallback) {
    const char *s = getenv(name);
    return (s && *s) ? atoi(s) : fallback;
}
static int sim_registered(void) {
    return !sim_int("SIM_CARD_ABSENT", 0) && sim_int("SIM_CEREG", 1) != 0;
}

int ql_atc_init() { return 0; }
/* ql_atc_send:不返回空 —— 转发给 PATH 上的假 serial_atcmd,
   这样 oper.cpp 等模块的 AT 应答解析跑的仍是真代码。 */
int ql_atc_send(char *req_buf, char *rsp_buf, int rsp_len) {
    char cmd[512]; snprintf(cmd, sizeof cmd, "serial_atcmd %s", req_buf ? req_buf : "");
    FILE* p = popen(cmd, "r");
    if (!p) { if (rsp_buf && rsp_len) rsp_buf[0] = 0; return -1; }
    size_t n = fread(rsp_buf, 1, (size_t)rsp_len - 1, p);
    rsp_buf[n] = 0; pclose(p); return 0;
}
int ql_data_call_param_get_reconnect_interval(ql_data_call_param_t *param, int *time_list, int *p_num) { return 0; }
int ql_data_call_param_get_reconnect_mode(ql_data_call_param_t *param, int *p_mode) { return 0; }
int ql_nw_deinit() { return 0; }
int ql_nw_get_cell_access_status(QL_NW_CELL_ACCESS_STATE_TYPE_E *p_info) { return 0; }
int ql_nw_get_cell_info(ql_nw_cell_info_t *p_info) { return 0; }
int ql_nw_get_data_reg_status(ql_nw_reg_status_info_t *p_info) {
    if (!p_info) return -1;
    memset(p_info, 0, sizeof(*p_info));
    int registered = sim_registered();
    p_info->tech_domain = QL_NW_TECH_DOMAIN_3GPP;
    p_info->radio_tech = QL_NW_RADIO_TECH_LTE;
    p_info->reg_state = (QL_NW_SERVICE_TYPE_E)(registered ? QL_NW_SERVICE_FULL : 0);
    p_info->deny_reason = (QL_NW_DENY_REASON_TYPE_E)sim_int("SIM_DENY", registered ? 0 : 6);
    return 0;
}
int ql_nw_get_etws_config(uint8_t* p_enable_etws) { return 0; }
int ql_nw_get_mobile_operator_name(ql_nw_mobile_operator_name_info_t *p_info) {
    if (!p_info) return -1;
    memset(p_info, 0, sizeof(*p_info));
    snprintf(p_info->short_eons, sizeof(p_info->short_eons), "%s", getenv("SIM_OPERATOR") ? getenv("SIM_OPERATOR") : "SIMNET");
    snprintf(p_info->mcc, sizeof(p_info->mcc), "%s", "460");
    snprintf(p_info->mnc, sizeof(p_info->mnc), "%s", "00");
    return 0;
}
int ql_nw_get_nitz_time_info(ql_nw_nitz_time_info_t *p_info) { return 0; }
int ql_nw_get_pref_nwmode_roaming(ql_nw_pref_nwmode_roaming_info_t *p_info) { return 0; }
int ql_nw_get_signal_strength(ql_nw_signal_strength_info_t *p_info, QL_NW_SIGNAL_STRENGTH_LEVEL_E* p_level) {
    if (!p_info) return -1;
    memset(p_info, 0, sizeof(*p_info));
    int csq = sim_int("SIM_CSQ", 18);
    if (!sim_int("SIM_LTE_VALID", csq != 99)) return 0;
    int weak = csq < 10;
    p_info->has_lte = 1;
    p_info->lte.rsrp = (int16_t)sim_int("SIM_RSRP", weak ? -115 : -88);
    p_info->lte.rsrq = (int8_t)sim_int("SIM_RSRQ", weak ? -19 : -10);
    p_info->lte.snr = (int16_t)sim_int("SIM_SNR", weak ? -20 : 180);
    p_info->lte.rssi = (int8_t)sim_int("SIM_RSSI", weak ? -100 : -65);
    if (p_level) *p_level = QL_NW_SIGNAL_STRENGTH_LEVEL_NONE;
    return 0;
}
int ql_nw_get_voice_reg_status(ql_nw_reg_status_info_t *p_info) { return 0; }
int ql_nw_get_wea_config(ql_nw_wea_config_t *p_config) { return 0; }
int ql_nw_init() { return 0; }
int ql_nw_network_scan(int *async_index, ql_nw_network_scan_async_cb async_cb) { return 0; }
int ql_nw_set_cell_access_status_ind_cb(ql_nw_cell_access_status_ind_cb cb_func) { return 0; }
int ql_nw_set_data_reg_ind_cb(ql_nw_data_reg_ind_cb cb_func) { return 0; }
int ql_nw_set_etws_alert_ind_cb(ql_nw_etws_reg_ind_cb cb_func) { return 0; }
int ql_nw_set_etws_config(uint8_t enable_etws) { return 0; }
int ql_nw_set_nitz_time_update_ind_cb(ql_nw_nitz_time_update_ind_cb cb_func) { return 0; }
int ql_nw_set_power_mode(uint8_t lower_mode) { return 0; }
int ql_nw_set_pref_nwmode_roaming(ql_nw_pref_nwmode_roaming_info_t *p_info) { return 0; }
int ql_nw_set_service_error_cb(ql_nw_service_error_cb_f cb) { return 0; }
int ql_nw_set_signal_strength_ind_cb(ql_nw_signal_strength_ind_cb cb_func) { return 0; }
int ql_nw_set_wea_alert_ind_cb(ql_nw_wea_reg_ind_cb cb_func) { return 0; }
int ql_nw_set_wea_config(int item, ql_nw_wea_config_t *p_info) { return 0; }
int ql_sim_deinit() { return 0; }
int ql_sim_get_iccid(QL_SIM_SLOT_E slot, char *iccid, int iccid_len) { return 0; }
int ql_sim_get_imsi(QL_SIM_SLOT_E slot, QL_SIM_APP_TYPE_E app_type, char *imsi, int imsi_len) { return 0; }
int ql_sim_init() { return 0; }

} // extern "C"

// —— 头文件里没有声明的少数符号,按调用点推断 ——
extern "C" {
const char* ql_data_call_state_str(int s) { return s ? "connected" : "disconnected"; }
// test_utils(open_dial 的测试辅助;dial.cpp 引了它的头)
int  t_get_int(const char* s)  { return s ? atoi(s) : 0; }
int  t_get_hex(const char* s)  { return s ? (int)strtol(s, 0, 16) : 0; }
const char* TAG = "modem_mng";
// diff:misc/diag 里的时间差辅助
long diff(long a, long b) { return a - b; }
}

// 真签名取自 ql_sim.h: int ql_sim_set_card_status_cb(ql_sim_card_status_cb_f cb);
extern "C" int ql_sim_set_card_status_cb(ql_sim_card_status_cb_f cb) { (void)cb; return 0; }
