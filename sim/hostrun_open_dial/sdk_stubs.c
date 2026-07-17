/* sdk_stubs.c —— 自动生成:签名逐字取自 SDK 真头。open_dial 与 modem_mng 用同一套 Quectel SDK。
   桩一律返回成功;"装病"由 fakebin/ 的假 serial_atcmd/ping + 环境变量控制。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ql_data_call.h"
#include "ql_nw.h"
#include "ql_sim.h"
#include "ql_atc.h"

int ql_data_call_config(int call_id, ql_data_call_param_t *param) { return 0; }
int ql_data_call_create(int call_id, const char *call_name, int is_background) { return 0; }
int ql_data_call_deinit() { return 0; }
int ql_data_call_get_list(ql_data_call_item_t *list, int *list_len) { return 0; }
int ql_data_call_get_status(int call_id, ql_data_call_status_t *p_sta) { return 0; }
int ql_data_call_init() { return 0; }
int ql_data_call_param_free(ql_data_call_param_t *param) { return 0; }
int ql_data_call_param_get_apn_id(ql_data_call_param_t *param, int *apn_id) { return 0; }
int ql_data_call_param_get_ip_version(ql_data_call_param_t *param, int *p_ver) { return 0; }
int ql_data_call_param_get_reconnect_interval(ql_data_call_param_t *param, int *time_list, int *p_num) { return 0; }
int ql_data_call_param_get_reconnect_mode(ql_data_call_param_t *param, int *p_mode) { return 0; }
int ql_data_call_param_set_apn_id(ql_data_call_param_t *param, int apn_id) { return 0; }
int ql_data_call_param_set_ip_version(ql_data_call_param_t *param, int ip_ver) { return 0; }
int ql_data_call_param_set_reconnect_interval(ql_data_call_param_t *param, int *time_list, int num) { return 0; }
int ql_data_call_param_set_reconnect_mode(ql_data_call_param_t *param, int reconnect_mode) { return 0; }
int ql_data_call_set_apn_config(int apn_id, ql_data_call_apn_config_t *p_info) { return 0; }
int ql_data_call_set_service_error_cb(ql_data_call_service_error_cb_f cb) { return 0; }
int ql_data_call_set_status_ind_cb(ql_data_call_status_ind_cb_f cb) { return 0; }
int ql_data_call_start(int call_id) { return 0; }
int ql_data_call_stop(int call_id) { return 0; }
int ql_nw_deinit() { return 0; }
int ql_nw_get_cell_access_status(QL_NW_CELL_ACCESS_STATE_TYPE_E *p_info) { return 0; }
int ql_nw_get_cell_info(ql_nw_cell_info_t *p_info) { return 0; }
int ql_nw_get_data_reg_status(ql_nw_reg_status_info_t *p_info) { return 0; }
int ql_nw_get_etws_config(uint8_t* p_enable_etws) { return 0; }
int ql_nw_get_mobile_operator_name(ql_nw_mobile_operator_name_info_t *p_info) { return 0; }
int ql_nw_get_nitz_time_info(ql_nw_nitz_time_info_t *p_info) { return 0; }
int ql_nw_get_pref_nwmode_roaming(ql_nw_pref_nwmode_roaming_info_t *p_info) { return 0; }
int ql_nw_get_signal_strength(ql_nw_signal_strength_info_t *p_info, QL_NW_SIGNAL_STRENGTH_LEVEL_E* p_level) { return 0; }
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
int ql_sim_get_card_info(QL_SIM_SLOT_E slot, ql_sim_card_info_t *p_info) { return 0; }
int ql_sim_get_iccid(QL_SIM_SLOT_E slot, char *iccid, int iccid_len) { return 0; }
int ql_sim_get_imsi(QL_SIM_SLOT_E slot, QL_SIM_APP_TYPE_E app_type, char *imsi, int imsi_len) { return 0; }
int ql_sim_init() { return 0; }
int ql_sim_set_card_status_cb(ql_sim_card_status_cb_f cb) { return 0; }

/* —— 头里无声明的 2 个,按调用点推断 —— */
void* ql_data_call_param_alloc(void) { static int d; return &d; }
const char* ql_data_call_state_str(int s) { return s ? "connected" : "disconnected"; }

/* —— json-c 桩:只是 apn.json 的解析库,不是拨号逻辑。
      返回"找不到"→ 真代码自己走 "iccid not matched in apn.json, fall back to
      auto/default APN" 分支 —— 那个选择仍是真代码做的。 —— */
void* json_tokener_parse(const char* s) { (void)s; return 0; }
int   json_object_object_get_ex(void* a, const char* b, void** c) { (void)a;(void)b;(void)c; return 0; }
void* json_object_array_get_idx(void* a, size_t i) { (void)a;(void)i; return 0; }
int   json_object_array_length(void* a) { (void)a; return 0; }
const char* json_object_get_string(void* a) { (void)a; return ""; }
int   json_object_put(void* a) { (void)a; return 0; }

/* —— 杂项外部符号 —— */
int  __android_log_print(int p, const char* t, const char* f, ...) { (void)p;(void)t;(void)f; return 0; }
long diff(long a, long b) { return a - b; }
int  t_get_int(const char* s) { return s ? atoi(s) : 0; }
int  t_get_hex(const char* s) { return s ? (int)strtol(s, 0, 16) : 0; }
