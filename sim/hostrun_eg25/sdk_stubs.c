/* sdk_stubs.c —— 自动生成:签名逐字取自 EG25 SDK 真头(ql-ol-extsdk/include)。
 * EG25 与 EC200A 不是同一套 SDK(QL_APN_* / QL_MCM_* 是 EG25 特有)。
 * 生成时先剥掉注释再匹配 —— EG25 的多行声明里嵌了 ///< 参数注释,不剥会把参数列表切坏。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ql_oe.h>

static int sim_int(const char *name, int fallback) {
    const char *s = getenv(name);
    return (s && *s) ? atoi(s) : fallback;
}
static int sim_registered(void) {
    return !sim_int("SIM_CARD_ABSENT", 0) && sim_int("SIM_CEREG", 1) != 0;
}

int QL_APN_Add(ql_apn_add_s *apn, unsigned char *profile_idx) { return 0; }
int QL_APN_Del(unsigned char profile_idx) { return 0; }
int QL_APN_Get_Lists(ql_apn_info_list_s *apn_list) { return 0; }
int QL_APN_Set(ql_apn_info_s *apn) { return 0; }
int QL_Data_Call_Get_Default_Profile(ql_data_call_default_profile_s *profile) { return 0; }
int QL_Data_Call_Info_Get(char profile_idx, ql_data_call_ip_family_e ip_family, ql_data_call_info_s *info, ql_data_call_error_e *err) { return 0; }
int QL_Data_Call_Init(ql_data_call_evt_cb_t evt_cb) {
    (void)evt_cb;
    return sim_int("SIM_DATACALL_INIT_RET", 0);
}
int QL_Data_Call_Init_Precondition() { return 0; }
int QL_Data_Call_Set_Default_Profile(ql_data_call_default_profile_s *profile) { return 0; }
int QL_Data_Call_Start(ql_data_call_s *data_call, ql_data_call_error_e *err) { return 0; }
int QL_Data_Call_Stop(char profile_idx, ql_data_call_ip_family_e ip_family, ql_data_call_error_e *err) { return 0; }
E_QL_ERROR_CODE_T QL_MCM_NW_Client_Init(nw_client_handle_type *ph_nw) { return 0; }
E_QL_ERROR_CODE_T QL_MCM_NW_GetRegStatus(nw_client_handle_type h_nw, QL_MCM_NW_REG_STATUS_INFO_T *pt_info) {
    int registered;
    (void)h_nw;
    if (!pt_info) return (E_QL_ERROR_CODE_T)-1;
    memset(pt_info, 0, sizeof(*pt_info));
    registered = sim_registered();
    pt_info->data_registration_valid = 1;
    pt_info->data_registration.tech_domain = E_QL_MCM_NW_TECH_DOMAIN_3GPP;
    pt_info->data_registration.radio_tech = E_QL_MCM_NW_RADIO_TECH_LTE;
    pt_info->data_registration.registration_state = (E_QL_MCM_NW_SERVICE_TYPE_T)(registered ? E_QL_MCM_NW_SERVICE_FULL : 0);
    pt_info->data_registration.deny_reason = (E_QL_MCM_NW_DENY_REASON_TYPE_T)sim_int("SIM_DENY", registered ? 0 : 6);
    return (E_QL_ERROR_CODE_T)0;
}
E_QL_ERROR_CODE_T QL_MCM_NW_GetSignalStrength(nw_client_handle_type h_nw, QL_MCM_NW_SIGNAL_STRENGTH_INFO_T *pt_info) {
    int csq, weak;
    (void)h_nw;
    if (!pt_info) return (E_QL_ERROR_CODE_T)-1;
    memset(pt_info, 0, sizeof(*pt_info));
    csq = sim_int("SIM_CSQ", 18);
    if (!sim_int("SIM_LTE_VALID", csq != 99)) return (E_QL_ERROR_CODE_T)0;
    weak = csq < 10;
    pt_info->lte_sig_info_valid = 1;
    pt_info->lte_sig_info.rsrp = (int16_t)sim_int("SIM_RSRP", weak ? -115 : -88);
    pt_info->lte_sig_info.rsrq = (int8_t)sim_int("SIM_RSRQ", weak ? -19 : -10);
    pt_info->lte_sig_info.snr = (int16_t)sim_int("SIM_SNR", weak ? -20 : 180);
    pt_info->lte_sig_info.rssi = (int8_t)sim_int("SIM_RSSI", weak ? -100 : -65);
    return (E_QL_ERROR_CODE_T)0;
}
E_QL_ERROR_CODE_T QL_MCM_NW_GetOperatorName(nw_client_handle_type h_nw, QL_MCM_NW_OPERATOR_NAME_INFO_T *pt_info) {
    (void)h_nw;
    if (!pt_info) return (E_QL_ERROR_CODE_T)-1;
    memset(pt_info, 0, sizeof(*pt_info));
    snprintf(pt_info->short_eons, sizeof(pt_info->short_eons), "%s", getenv("SIM_OPERATOR") ? getenv("SIM_OPERATOR") : "SIMNET");
    snprintf(pt_info->mcc, sizeof(pt_info->mcc), "%s", "460");
    snprintf(pt_info->mnc, sizeof(pt_info->mnc), "%s", "00");
    return (E_QL_ERROR_CODE_T)0;
}
int QL_MCM_SIM_Client_Deinit(sim_client_handle_type h_sim) { return 0; }
int QL_MCM_SIM_Client_Init(sim_client_handle_type *ph_sim) { return 0; }
E_QL_ERROR_CODE_T QL_MCM_SIM_GetCardStatus(sim_client_handle_type h_sim, E_QL_MCM_SIM_SLOT_ID_TYPE_T simId, QL_MCM_SIM_CARD_STATUS_INFO_T *pt_info) {
    (void)h_sim; (void)simId;
    if (!pt_info) return (E_QL_ERROR_CODE_T)-1;
    memset(pt_info, 0, sizeof(*pt_info));
    pt_info->card_app_info.app_3gpp.app_state = sim_int("SIM_CARD_ABSENT", 0)
        ? E_QL_MCM_SIM_APP_STATE_UNKNOWN : E_QL_MCM_SIM_APP_STATE_READY;
    return (E_QL_ERROR_CODE_T)0;
}
E_QL_ERROR_CODE_T QL_MCM_SIM_GetICCID(sim_client_handle_type h_sim, E_QL_MCM_SIM_SLOT_ID_TYPE_T simId, char *iccid, size_t iccidLen) { return 0; }
E_QL_ERROR_CODE_T QL_MCM_SIM_GetIMSI(sim_client_handle_type h_sim, QL_SIM_APP_ID_INFO_T *pt_info, char *imsi, size_t imsiLen) { return 0; }

/* 头里没有声明的,按调用点补 */
void set_eg25_iccid(const char* s) { (void)s; }
void set_eg25_imsi(const char* s) { (void)s; }
const char* TAG = "modem_mng";
