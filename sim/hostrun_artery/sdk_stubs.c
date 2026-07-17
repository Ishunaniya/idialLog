/* sdk_stubs.c —— 自动生成:签名逐字取自 EG25 SDK 真头(artery 与 modem_mng EG25 同一套 SDK)。
 * 生成时先剥注释再匹配(SDK 头的多行声明嵌了 ///< 参数注释)。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ql_oe.h>

int QL_APN_Add(ql_apn_add_s *apn, unsigned char *profile_idx) { return 0; }
int QL_APN_Del(unsigned char profile_idx) { return 0; }
int QL_APN_Get_Lists(ql_apn_info_list_s *apn_list) { return 0; }
int QL_APN_Set(ql_apn_info_s *apn) { return 0; }
int QL_Data_Call_Info_Get(char profile_idx, ql_data_call_ip_family_e ip_family, ql_data_call_info_s *info, ql_data_call_error_e *err) { return 0; }
int QL_Data_Call_Init(ql_data_call_evt_cb_t evt_cb) { return 0; }
int QL_Data_Call_Init_Precondition() { return 0; }
int QL_Data_Call_Set_Default_Profile(ql_data_call_default_profile_s *profile) { return 0; }
int QL_Data_Call_Start(ql_data_call_s *data_call, ql_data_call_error_e *err) { return 0; }
int QL_Data_Call_Stop(char profile_idx, ql_data_call_ip_family_e ip_family, ql_data_call_error_e *err) { return 0; }
E_QL_ERROR_CODE_T QL_MCM_NW_Client_Init(nw_client_handle_type *ph_nw) { return 0; }
E_QL_ERROR_CODE_T QL_MCM_NW_GetRegStatus(nw_client_handle_type h_nw, QL_MCM_NW_REG_STATUS_INFO_T *pt_info) { return 0; }
int QL_MCM_SIM_Client_Deinit(sim_client_handle_type h_sim) { return 0; }
int QL_MCM_SIM_Client_Init(sim_client_handle_type *ph_sim) { return 0; }
E_QL_ERROR_CODE_T QL_MCM_SIM_GetCardStatus(sim_client_handle_type h_sim, E_QL_MCM_SIM_SLOT_ID_TYPE_T simId, QL_MCM_SIM_CARD_STATUS_INFO_T *pt_info) { return 0; }
E_QL_ERROR_CODE_T QL_MCM_SIM_GetICCID(sim_client_handle_type h_sim, E_QL_MCM_SIM_SLOT_ID_TYPE_T simId, char *iccid, size_t iccidLen) { return 0; }
E_QL_ERROR_CODE_T QL_MCM_SIM_GetIMSI(sim_client_handle_type h_sim, QL_SIM_APP_ID_INFO_T *pt_info, char *imsi, size_t imsiLen) { return 0; }
