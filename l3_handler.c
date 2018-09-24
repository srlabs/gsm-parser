#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <osmocom/gsm/rsl.h>
#include <osmocom/gsm/tlv.h>
#include <osmocom/gsm/gsm48.h>
#include <osmocom/gsm/gsm48_ie.h>
#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/gsm/protocol/gsm_04_11.h>
#include <assert.h>

#include "session.h"
#include "bit_func.h"
#include "assignment.h"
#include "address.h"
#include "sms.h"
#include "cell_info.h"
#include "output.h"
#include "umts_rrc.h"
#include "lte_nas_eps.h"

void handle_classmark(struct session_info *s, uint8_t *data, uint8_t type)
{
	struct gsm48_classmark2 *cm2 = (struct gsm48_classmark2 *)data;

	s->ms_cipher_mask |= !cm2->a5_1;

	if (type == 2) {
		s->ms_cipher_mask |= (cm2->a5_2 << 1);
		s->ms_cipher_mask |= (cm2->a5_3 << 2);
	}
}


void handle_lai(struct session_info *s, uint8_t *data, int cid)
{
	struct gsm48_loc_area_id *lai = (struct gsm48_loc_area_id *) data;

	s->mcc = get_mcc(lai->digits);
	s->mnc = get_mnc(lai->digits);
	s->lac = htons(lai->lac);

	if (cid >= 0) {
		s->cid = cid;
	}
}

void handle_mi(struct session_info *s, uint8_t *data, uint8_t len, uint8_t new_tmsi)
{
	char tmsi_str[9];
	uint8_t mi_type;

	if (len > GSM48_MI_SIZE) {
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (MI_LEN)");
		return;
	}

	mi_type = data[0] & GSM_MI_TYPE_MASK;
	switch (mi_type) {
	case GSM_MI_TYPE_NONE:
		break;

	case GSM_MI_TYPE_IMSI:
		bcd2str(data, s->imsi, len*2, 1);
		APPEND_MSG_INFO(s, ", IMSI %s", s->imsi);
		s->use_imsi = 1;
		break;

	case GSM_MI_TYPE_IMEI:
	case GSM_MI_TYPE_IMEISV:
		bcd2str(data, s->imei, 15, 1);
		APPEND_MSG_INFO(s, ", IMEI %s", s->imei);
		break;

	case GSM_MI_TYPE_TMSI:
		hex_bin2str(&data[1], tmsi_str, 4);
		tmsi_str[8] = 0;
		assert(s->new_msg);

		APPEND_MSG_INFO(s, ", TMSI %s", tmsi_str);
		if (new_tmsi) {
			if (!not_zero(s->new_tmsi, 4)) {
				memcpy(s->new_tmsi, &data[1], 4);
			}
		} else {
			if (!not_zero(s->old_tmsi, 4)) {
				memcpy(s->old_tmsi, &data[1], 4);
				s->use_tmsi = 1;
			}
		}
		break;

	default:
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (MI_TYPE)");
		return;
	}
}

void handle_cmreq(struct session_info *s, uint8_t *data)
{
	struct gsm48_service_request *cm = (struct gsm48_service_request *) data;

	switch (cm->cm_service_type) {
	case GSM48_CMSERV_EMERGENCY:
		s->call_presence = 1;
		strncpy(s->msisdn, "<emergency>", GSM48_MI_SIZE);
		/* fall-through */
	case GSM48_CMSERV_MO_CALL_PACKET:
		s->call = 1;
		break;
	case GSM48_CMSERV_SMS:
		s->sms = 1;
		break;
	case GSM48_CMSERV_SUP_SERV:
		s->ssa = 1;
		break;
	default:
		s->unknown = 1;
	}

	s->started = 1;
	s->closed = 0;

	s->initial_seq = cm->cipher_key_seq & 7;

	handle_classmark(s, ((uint8_t *) &cm->classmark)+1, 2);

	handle_mi(s, cm->mi, cm->mi_len, 0);
}

void handle_serv_req(struct session_info *s, uint8_t *data, unsigned len)
{
	s->started = 1;
	s->closed = 0;
	s->serv_req = 1;

	s->initial_seq = data[0] & 7;
	if (len >= 7) {
		handle_mi(s, &data[2], data[1], 0);
	}
}

void handle_pag_resp(struct session_info *s, uint8_t *data)
{
	struct gsm48_pag_resp *pr = (struct gsm48_pag_resp *) data;

	s->initial_seq = pr->key_seq;

	s->mt = 1;
	s->started = 1;
	s->closed = 0;

	handle_classmark(s, (uint8_t *) (&pr->classmark2) + 1, 2);

	s->pag_mi = pr->mi[0] & GSM_MI_TYPE_MASK;
	handle_mi(s, pr->mi, pr->mi_len, 0);
}

void handle_loc_upd_acc(struct session_info *s, uint8_t *data, unsigned len)
{
	SET_MSG_INFO(s, "LOC UPD ACCEPT");

	s->locupd = 1;
	s->mo = 1;
	s->lu_acc = 1;

	handle_lai(s, data, -1);

	APPEND_MSG_INFO(s, ", LAC %d", s->lac);

	if ((len > 11) && (data[5] == 0x17)) {
		s->tmsi_realloc = 1;
		handle_mi(s, &data[7], data[6], 1);
	}
}

void handle_id_req(struct session_info *s, uint8_t *data)
{
	switch (data[0] & GSM_MI_TYPE_MASK) {
	case GSM_MI_TYPE_IMSI:
		SET_MSG_INFO(s, "IDENTITY REQUEST, IMSI");
		if (s->cipher) {
			s->iden_imsi_ac = 1;
		} else {
			s->iden_imsi_bc = 1;
		}
		break;
	case GSM_MI_TYPE_IMEI:
		SET_MSG_INFO(s, "IDENTITY REQUEST, IMEI");
		if (s->cipher) {
			s->iden_imei_ac = 1;
		} else {
			s->iden_imei_bc = 1;
		}
		break;
	case GSM_MI_TYPE_IMEISV:
		SET_MSG_INFO(s, "IDENTITY REQUEST, IMEISV");
		if (s->cipher) {
			s->iden_imei_ac = 1;
		} else {
			s->iden_imei_bc = 1;
		}
		break;
	case GSM_MI_TYPE_TMSI:
		SET_MSG_INFO(s, "IDENTITY REQUEST, TMSI");
		break;
	}
}

void handle_id_resp(struct session_info *s, uint8_t *data, unsigned len)
{
	SET_MSG_INFO(s, "IDENTITY RESPONSE");

	switch (data[1] & GSM_MI_TYPE_MASK) {
	case GSM_MI_TYPE_IMSI:
		if (s->cipher) {
			s->iden_imsi_ac = 1;
		} else {
			s->iden_imsi_bc = 1;
		}
		break;
	case GSM_MI_TYPE_IMEI:
	case GSM_MI_TYPE_IMEISV:
		if (s->cipher) {
			s->iden_imei_ac = 1;
		} else {
			s->iden_imei_bc = 1;
		}
		break;
	case GSM_MI_TYPE_TMSI:
		break;
	}

	handle_mi(s, &data[1], data[0], 0);
}

void handle_loc_upd_req(struct session_info *s, uint8_t *data)
{
	struct gsm48_loc_upd_req *lu = (struct gsm48_loc_upd_req *) data;

	s->mo = 1;
	s->locupd = 1;
	s->started = 1;
	s->closed = 0;

	s->lu_type = lu->type & 3;
	s->initial_seq = lu->key_seq;
	s->lu_mcc = get_mcc(lu->lai.digits);
	s->lu_mnc = get_mnc(lu->lai.digits);
	s->lu_lac = htons(lu->lai.lac);
	APPEND_MSG_INFO(s, ", LAI %d-%d-%04x", s->lu_mcc, s->lu_mnc, s->lu_lac);

	handle_classmark(s, (uint8_t *) &lu->classmark1, 1);

	handle_mi(s, lu->mi, lu->mi_len, 0);
}

void handle_detach(struct session_info *s, uint8_t *data)
{
	struct gsm48_imsi_detach_ind *idi = (struct gsm48_imsi_detach_ind *) data;

	s->started = 1;
	s->closed = 0;

	handle_classmark(s, (uint8_t *) &idi->classmark1, 1);

	handle_mi(s, idi->mi, idi->mi_len, 0);
}

void handle_cc(struct session_info *s, struct gsm48_hdr *dtap, unsigned len, uint8_t ul)
{
	struct tlv_parsed tp;

	s->call = 1;

	switch (dtap->msg_type & 0x3f) {
	case 0x01:
		SET_MSG_INFO(s, "CALL ALERTING");
		break;
	case 0x02:
		SET_MSG_INFO(s, "CALL PROCEEDING");
		if (s->cipher && !s->fc.enc_rand && !ul)
			s->fc.predict++;
		if (!ul)
			s->mo = 1;
		break;
	case 0x03:
		SET_MSG_INFO(s, "CALL PROGRESS");
		break;
	case 0x05:
		s->call_presence = 1;
		SET_MSG_INFO(s, "CALL SETUP");
		if (!ul)
			s->mt = 1;
		else
			s->mo = 1;

		/* get MSISDN */
		tlv_parse(&tp, &gsm48_att_tlvdef, dtap->data, len-2, 0, 0);
		if (TLVP_PRESENT(&tp, GSM48_IE_CALLING_BCD)) {
			uint8_t *v = (uint8_t *) TLVP_VAL(&tp, GSM48_IE_CALLING_BCD);
			uint8_t v_len = TLVP_LEN(&tp, GSM48_IE_CALLING_BCD);
			handle_address(v, v_len, s->msisdn, 0);
		}
		if (TLVP_PRESENT(&tp, GSM48_IE_CALLED_BCD)) {
			uint8_t *v = (uint8_t *) TLVP_VAL(&tp, GSM48_IE_CALLED_BCD);
			uint8_t v_len = TLVP_LEN(&tp, GSM48_IE_CALLED_BCD);
			handle_address(v, v_len, s->msisdn, 0);
		}

		break;
	case 0x07:
		SET_MSG_INFO(s, "CALL CONNECT");
		break;
	case 0x08:
		SET_MSG_INFO(s, "CALL CONFIRMED");
		if (ul)
			s->mt = 1;
		else
			s->mo = 1;
		break;
	case 0x0f:
		SET_MSG_INFO(s, "CALL CONNECT ACK");
		break;
	case 0x25:
		SET_MSG_INFO(s, "CALL DISCONNECT");
		break;
	case 0x2a:
		SET_MSG_INFO(s, "CALL RELEASE COMPLETE");
		break;
	case 0x2d:
		SET_MSG_INFO(s, "CALL RELEASE");
		break;
	case 0x3a:
		SET_MSG_INFO(s, "CALL FACILITY");
		break;
	case 0x3d:
		SET_MSG_INFO(s, "CALL STATUS");
		break;
	case 0x3e:
		SET_MSG_INFO(s, "CALL NOTIFY");
		break;
	default:
		SET_MSG_INFO(s, "UNKNOWN CC (%02x)", dtap->msg_type & 0x3f);
		s->unknown = 1;
	}
}

void handle_mm(struct session_info *s, struct gsm48_hdr *dtap, unsigned dtap_len, uint32_t fn)
{
	if (dtap_len < sizeof(struct gsm48_hdr)) {
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (MM_LEN)");
		return;
	}

	switch (dtap->msg_type & 0x3f) {
	case 0x01:
		session_reset(s, 1);
		s->started = 1;
		SET_MSG_INFO(s, "IMSI DETACH");
		s->detach = 1;
		s->mo = 1;
		handle_detach(s, dtap->data);
		break;
	case 0x02:
		SET_MSG_INFO(s, "LOC UPD ACCEPT");
		handle_loc_upd_acc(s, dtap->data, dtap_len - 2);
		break;
	case 0x04:
		SET_MSG_INFO(s, "LOC UPD REJECT cause=%d", dtap->data[0]);
		s->locupd = 1;
		s->lu_reject = 1;
		s->lu_rej_cause = dtap->data[0];
		s->mo = 1;
		break;
	case 0x08:
		session_reset(s, 1);
		if (dtap_len < sizeof(struct gsm48_loc_upd_req)) {
			SET_MSG_INFO(s, "FAILED SANITY CHECKS (LUR_DTAP_SIZE)");
			break;
		}
		SET_MSG_INFO(s, "LOC UPD REQUEST");
		handle_loc_upd_req(s, dtap->data);
		break;
	case 0x12:
		if ((dtap_len > 19) && (dtap->data[17] == 0x20) && (dtap->data[18] == 0x10)) {
			SET_MSG_INFO(s, "AUTH REQUEST (UMTS)");
			s->auth = 2;
		} else {
			SET_MSG_INFO(s, "AUTH REQUEST (GSM)");
			s->auth = 1;
		}
		if (!s->auth_req_fn) {
			if (fn) {
				s->auth_req_fn = fn;
			} else {
				s->auth_req_fn = GSM_MAX_FN;
			}
		}
		break;
	case 0x14:
		if ((dtap_len > 6) && (dtap->data[4] == 0x21) && (dtap->data[5] == 0x04)) {
			SET_MSG_INFO(s, "AUTH RESPONSE (UMTS)");
			if (!s->auth) {
				s->auth = 2;
			}
		} else {
			SET_MSG_INFO(s, "AUTH RESPONSE (GSM)");
			if (!s->auth) {
				s->auth = 1;
			}
		}
		if (!s->auth_resp_fn) {
			if (fn) {
				s->auth_resp_fn = fn;
			} else {
				s->auth_resp_fn = GSM_MAX_FN;
			}
		}
		break;
	case 0x18:
		handle_id_req(s, dtap->data);
		break;
	case 0x19:
		handle_id_resp(s, dtap->data, dtap_len - 2);
		break;
	case 0x1a:
		SET_MSG_INFO(s, "TMSI REALLOC COMMAND");
		s->tmsi_realloc = 1;
		handle_lai(s, dtap->data, -1);
		handle_mi(s, &dtap->data[6], dtap->data[5], 1);
		break;
	case 0x1b:
		SET_MSG_INFO(s, "TMSI REALLOC COMPLETE");
		s->tmsi_realloc = 1;
		break;
	case 0x21:
		SET_MSG_INFO(s, "CM SERVICE ACCEPT");
		s->mo = 1;
		break;
	case 0x23:
		SET_MSG_INFO(s, "CM SERVICE ABORT");
		s->mo = 1;
		break;
	case 0x24:
		SET_MSG_INFO(s, "CM SERVICE REQUEST");
		/* Handle subsequent request without starting a new session */
		if ((s->started || s->call_presence || s->sms_presence || s->lu_acc) &&
			!s->closed &&
			s->last_msg &&
			!(s->last_msg->flags & MSG_BCCH) &&
			(s->new_msg->timestamp.tv_sec - s->last_msg->timestamp.tv_sec <= 1)) {
			if (msg_verbose > 0) {
				printf("New service request in already started transaction!\n");
			}
		} else {
			session_reset(s, 1);
		}
		s->started = 1;
		s->closed = 0;
		s->serv_req = 1;
		s->mo = 1;
		handle_cmreq(s, dtap->data);
		break;
	case 0x29:
		SET_MSG_INFO(s, "ABORT");
		s->abort = 1;
		break;
	case 0x32:
		SET_MSG_INFO(s, "MM INFORMATION");
		break;
	default:
		SET_MSG_INFO(s, "UNKNOWN MM (%02x)", dtap->msg_type & 0x3f);
		s->unknown = 1;
	}
}

void handle_rr(struct session_info *s, struct gsm48_hdr *dtap, unsigned len, uint32_t fn)
{
	struct gsm48_system_information_type_6 *si6;
	struct tlv_parsed tp;

	assert(s->new_msg);

	if (!len) {
		return;
	}

	switch (dtap->msg_type) {
	case GSM48_MT_RR_SYSINFO_1:
		SET_MSG_INFO(s, "SYSTEM INFO 1");
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_SYSINFO_2:
		SET_MSG_INFO(s, "SYSTEM INFO 2");
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_SYSINFO_2bis:
		SET_MSG_INFO(s, "SYSTEM INFO 2bis");
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_SYSINFO_2ter:
		SET_MSG_INFO(s, "SYSTEM INFO 2ter");
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_SYSINFO_2quater:
		SET_MSG_INFO(s, "SYSTEM INFO 2quater");
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_SYSINFO_3:
		SET_MSG_INFO(s, "SYSTEM INFO 3");
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_SYSINFO_4:
		SET_MSG_INFO(s, "SYSTEM INFO 4");
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_SYSINFO_5:
		SET_MSG_INFO(s, "SYSTEM INFO 5");
		rand_check((uint8_t *)dtap, 18, &s->si5, s->cipher);
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_SYSINFO_5bis:
		SET_MSG_INFO(s, "SYSTEM INFO 5bis");
		rand_check((uint8_t *)dtap, 18, &s->si5bis, s->cipher);
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_SYSINFO_5ter:
		SET_MSG_INFO(s, "SYSTEM INFO 5ter");
		rand_check((uint8_t *)dtap, 18, &s->si5ter, s->cipher);
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_SYSINFO_6:
		SET_MSG_INFO(s, "SYSTEM INFO 6");
		rand_check((uint8_t *)dtap, 18, &s->si6, s->cipher);
		si6 = (struct gsm48_system_information_type_6 *) dtap;
		handle_lai(s, (uint8_t*)&si6->lai, htons(si6->cell_identity));
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_SYSINFO_13:
		SET_MSG_INFO(s, "SYSTEM INFO 13");
		handle_sysinfo(s, dtap, len);
		break;
	case GSM48_MT_RR_CHAN_REL:
		SET_MSG_INFO(s, "CHANNEL RELEASE");
		if (s->cipher && !s->fc.enc_rand)
			s->fc.predict++;

		s->release = 1;
		s->rr_cause = dtap->data[0];
		if ((len > 3) && ((dtap->data[1] & 0xf0) == 0xc0))
			s->have_gprs = 1;

		session_reset(&s[0], 0);
		s->rat = RAT_GSM;
		if (auto_reset) {
			s[1].new_msg = NULL;
		}
		break;
	case GSM48_MT_RR_CLSM_ENQ:
		SET_MSG_INFO(s, "CLASSMARK ENQUIRY");
		break;
	case GSM48_MT_RR_MEAS_REP:
		SET_MSG_INFO(s, "MEASUREMENT REPORT");
		break;
	case GSM48_MT_RR_CLSM_CHG:
		SET_MSG_INFO(s, "CLASSMARK CHANGE");
		handle_classmark(s, &dtap->data[1], 2);
		break;
	case GSM48_MT_RR_PAG_REQ_1:
		SET_MSG_INFO(s, "PAGING REQ 1");
		handle_paging1((uint8_t *) dtap, len);
		break;
	case GSM48_MT_RR_PAG_REQ_2:
		SET_MSG_INFO(s, "PAGING REQ 2");
		handle_paging2((uint8_t *) dtap, len);
		break;
	case GSM48_MT_RR_PAG_REQ_3:
		SET_MSG_INFO(s, "PAGING REQ 3");
		handle_paging3();
		break;
	case GSM48_MT_RR_IMM_ASS:
		SET_MSG_INFO(s, "IMM ASSIGNMENT");
		break;
	case GSM48_MT_RR_IMM_ASS_EXT:
		SET_MSG_INFO(s, "IMM ASSIGNMENT EXT");
		break;
	case GSM48_MT_RR_IMM_ASS_REJ:
		SET_MSG_INFO(s, "IMM ASSIGNMENT REJECT");
		break;
	case GSM48_MT_RR_PAG_RESP:
		session_reset(s, 1);
		s->rat = RAT_GSM;
		SET_MSG_INFO(s, "PAGING RESPONSE");
		handle_pag_resp(s, dtap->data);
		break;
	case GSM48_MT_RR_HANDO_CMD:
		SET_MSG_INFO(s, "HANDOVER COMMAND");
		parse_assignment(dtap, len, s->cell_arfcns, &s->ga);
		s->handover = 1;
		s->use_jump = 2;
		break;
	case GSM48_MT_RR_HANDO_COMPL:
		SET_MSG_INFO(s, "HANDOVER COMPLETE");
		break;
	case GSM48_MT_RR_ASS_CMD:
		SET_MSG_INFO(s, "ASSIGNMENT COMMAND");
		if ((s->fc.enc-s->fc.enc_null-s->fc.enc_si) == 1)
			s->forced_ho = 1;
		parse_assignment(dtap, len, s->cell_arfcns, &s->ga);
		s->assignment = 1;
		s->use_jump = 1;
		break;
	case GSM48_MT_RR_ASS_COMPL:
		SET_MSG_INFO(s, "ASSIGNMENT COMPLETE");
		s->assign_complete = 1;
		break;
	case GSM48_MT_RR_CIPH_M_COMPL:
		SET_MSG_INFO(s, "CIPHER MODE COMPLETE");
		if (s->cipher_missing < 0) {
			s->cipher_missing = 0;
		} else {
			s->cipher_missing = 1;
		}

		if (!s->cm_comp_first_fn) {
			if (fn) {
				s->cm_comp_first_fn = fn;
			} else {
				s->cm_comp_first_fn = GSM_MAX_FN;
			}
		}

		if (fn) {
			s->cm_comp_last_fn = fn;
		} else {
			s->cm_comp_last_fn = GSM_MAX_FN;
		}

		s->cm_comp_count++;

		if (dtap->data[0] == 0x2b)
			return;

		/* get IMEISV */
		tlv_parse(&tp, &gsm48_rr_att_tlvdef, dtap->data, len-2, 0, 0);
		if (TLVP_PRESENT(&tp, GSM48_IE_MOBILE_ID)) {
			uint8_t *v = (uint8_t *) TLVP_VAL(&tp, GSM48_IE_MOBILE_ID);
			handle_mi(s, &v[1], v[0], 0);
			s->cmc_imeisv = 1;
		}
		break;
	case GSM48_MT_RR_GPRS_SUSP_REQ:
		SET_MSG_INFO(s, "GPRS SUSPEND");
		s->have_gprs = 1;
		//tlli
		//rai (lai+rac)
		break;
	case GSM48_MT_RR_CIPH_M_CMD:
		if (!s->cm_cmd_fn) {
			if (fn) {
				s->cm_cmd_fn = fn;
			} else {
				s->cm_cmd_fn = GSM_MAX_FN;
			}
		}

		if (dtap->data[0] & 1) {
			s->cipher = 1 + ((dtap->data[0]>>1) & 7);
			if (!not_zero(s->key, 8))
				s->decoded = 0;
		}
		SET_MSG_INFO(s, "CIPHER MODE COMMAND, A5/%u", s->cipher);
		if (dtap->data[0] & 0x10) {
			s->cmc_imeisv = 1;

			if (s->cipher && !s->fc.enc_rand)
				s->fc.predict++;
		}
		s->cipher_missing = -1;
		break;
	case 0x60:
		SET_MSG_INFO(s, "UTRAN CLASSMARK");
		break;
	default:
		SET_MSG_INFO(s, "UNKNOWN RR (%02x)", dtap->msg_type);
		s->unknown = 1;
	}
	s->rat = RAT_GSM;
}

void handle_ss(struct session_info *s, struct gsm48_hdr *dtap, unsigned len)
{
	assert(s != NULL);
	assert(dtap != NULL);

	if (!len) {
		return;
	}

	s->ssa = 1;

	switch (dtap->msg_type & 0x3f) {
	case 0x2a:
		SET_MSG_INFO(s, "SS RELEASE COMPLETE");
		break;
	case 0x3a:
		SET_MSG_INFO(s, "SS FACILITY");
		break;
	case 0x3b:
		SET_MSG_INFO(s, "SS REGISTER");
		break;
	default:
		SET_MSG_INFO(s, "UNKNOWN SS (%02x)", dtap->msg_type & 0x3f);
		s->unknown = 1;
	}
}

void handle_attach_acc(struct session_info *s, uint8_t *data, unsigned len)
{
	s->attach = 1;
	s->att_acc = 1;

	if (len < 9) {
		return;
	}
	handle_lai(s, &data[3], data[8]);

	if (len > 18 && data[13] == 0x18) {
		handle_mi(s, &data[15], data[14], 1);
	}
}

void handle_ra_upd_acc(struct session_info *s, uint8_t *data, unsigned len)
{
	s->raupd = 1;
	s->lu_acc = 1;

	handle_lai(s, &data[2], data[7]);

	if (data[8] == 0x18) {
		handle_mi(s, &data[10], data[9], 1);
	}
}

void handle_attach_req(struct session_info *s, uint8_t *data, unsigned len)
{
	uint8_t offset;
	struct gsm48_loc_area_id *lai;

	s->attach = 1;
	s->started = 1;
	s->closed = 0;

	/* Get MS capabilities length */
	offset = 1 + data[0];
	if (offset >= len) {
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (MS_CAP_LEN)");
		return;
	}

	if (offset + 4 >= len) {
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (NO_DATA_ATT)");
		return;
	}
	s->lu_type = data[offset] & 7;
	s->initial_seq = (data[offset] >> 4) & 7;
	offset++;

	/* Skip DRX */
	offset += 2;

	if (offset + data[offset] + 1 >= len) {
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (NO_DATA_MI)");
		return;
	}
	/* Get current mobile identity */
	handle_mi(s, &data[offset+1], data[offset], 0);
	offset += 1 + data[offset];

	if (offset + 3 >= len) {
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (NO_DATA_LAI)");
		return;
	}
	/* Get old LAI */
	lai = (struct gsm48_loc_area_id*) &data[offset];
        s->lu_mcc = get_mcc(lai->digits);
        s->lu_mnc = get_mnc(lai->digits);
        s->lu_lac = htons(lai->lac);
}

void handle_gmm(struct session_info *s, struct gsm48_hdr *dtap, unsigned len)
{
	assert(s != NULL);
	assert(dtap != NULL);

	if (!len) {
		return;
	}

	if (s->domain != DOMAIN_PS) {
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (GMM_IN_CS)");
		return;
	}

	s->new_msg->domain = DOMAIN_PS;

	switch (dtap->msg_type & 0x3f) {
	case 0x01:
		session_reset(s, 1);
		SET_MSG_INFO(s, "ATTACH REQUEST");
		handle_attach_req(s, dtap->data, len-2);
		break;
	case 0x02:
		SET_MSG_INFO(s, "ATTACH ACCEPT");
		handle_attach_acc(s, dtap->data, len-2);
		break;
	case 0x03:
		SET_MSG_INFO(s, "ATTACH COMPLETE");
		s->att_acc = 1;
		break;
	case 0x04:
		SET_MSG_INFO(s, "ATTACH REJECT");
		s->att_acc = -1;
		break;
	case 0x05:
		SET_MSG_INFO(s, "DETACH REQUEST");
		s->started = 1;
		break;
	case 0x06:
		SET_MSG_INFO(s, "DETACH ACCEPT");
		break;
	case 0x08:
		session_reset(s, 1);
		SET_MSG_INFO(s, "RA UPDATE REQUEST");
		s->raupd = 1;
		s->mo = 1;
		s->started = 1;
		s->closed = 0;
		s->initial_seq = (dtap->data[0] >> 4) & 7;
		break;
	case 0x09:
		SET_MSG_INFO(s, "RA UPDATE ACCEPT");
		handle_ra_upd_acc(s, dtap->data, len - 2);
		break;
	case 0x0a:
		SET_MSG_INFO(s, "RA UPDATE COMPLETE");
		s->raupd = 1;
		break;
	case 0x0b:
		SET_MSG_INFO(s, "RA UPDATE REJECT");
		s->raupd = -1;
		break;
	case 0x0c:
		session_reset(s, 1);
		SET_MSG_INFO(s, "SERVICE REQUEST");
		handle_serv_req(s, dtap->data, len - 2);
		break;
	case 0x0d:
		SET_MSG_INFO(s, "SERVICE ACCEPT");
		s->serv_req = 2;
		break;
	case 0x0e:
		SET_MSG_INFO(s, "SERVICE REJECT");
		s->serv_req = -1;
		break;
	case 0x10:
		SET_MSG_INFO(s, "PTMSI REALLOC COMMAND");
		s->tmsi_realloc = 1;
		break;
	case 0x11:
		SET_MSG_INFO(s, "PTMSI REALLOC COMPLETE");
		s->tmsi_realloc = 1;
		break;
	case 0x12:
		SET_MSG_INFO(s, "AUTH AND CIPHER REQUEST");
		if (!s->cipher) {
			s->cipher = dtap->data[0] & 7;
		}
		if (s->rat == RAT_GSM) {
			APPEND_MSG_INFO(s, ", GEA/%d", s->cipher);
		}
		s->cmc_imeisv = !!(dtap->data[0] & 0x70);
		if ((len > (2 + 20)) && (dtap->data[20] == 0x28)) {
			s->auth = 2;
		} else {
			s->auth = 1;
		}
		break;
	case 0x13:
		SET_MSG_INFO(s, "AUTH AND CIPHER RESPONSE");
		if (!s->auth) {
			s->auth = 1;
		}
		/* Check if IMEISV is included */
		if ((len > (2 + 15)) && (dtap->data[6] == 0x23)) {
			s->cmc_imeisv = 1;
			handle_mi(s, &dtap->data[8], dtap->data[7], 0);
		}
		break;
	case 0x14:
		SET_MSG_INFO(s, "AUTH AND CIPHER REJECT");
		s->auth = 1;
		break;
	case 0x15:
		handle_id_req(s, dtap->data);
		break;
	case 0x16:
		handle_id_resp(s, dtap->data, len - 2);
		break;
	case 0x20:
		SET_MSG_INFO(s, "GMM STATUS");
		break;
	case 0x21:
		SET_MSG_INFO(s, "GMM INFORMATION");
		break;
	default:
		SET_MSG_INFO(s, "UNKNOWN GMM (%02x)", dtap->msg_type & 0x3f);
	}
}

void handle_pdp_accept(struct session_info *s, uint8_t *data, unsigned len)
{
	uint8_t offset;

	/* Skip LLC NSAPI */
	offset = 1;

	/* Skip QoS and Radio priority */
	offset += 1 + data[offset] + 1;
	if (offset >= len) {
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (QOS_LEN_OVER)");
		return;
	}
	/* Check if there is a PDP address */
	if (data[offset++] != 0x2b) {
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (NO_PDP_ADDR)");
		return;
	}
	/* Check if compatible with IPv4 */
	if ((offset + 7 < len) && (data[offset] == 6)) {
		struct in_addr *in = (struct in_addr *) (&data[offset+3]);
		strncpy(s->pdp_ip, inet_ntoa(*in), sizeof(s->pdp_ip));
		s->pdp_ip[15] = 0;
	}
}

void handle_sm(struct session_info *s, struct gsm48_hdr *dtap, unsigned len)
{
	assert(s != NULL);
	assert(dtap != NULL);

	if (len < 2) {
		return;
	}

	if (s->domain != DOMAIN_PS) {
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (SM_IN_CS)");
		return;
	}

	s->new_msg->domain = DOMAIN_PS;

	switch (dtap->msg_type & 0x3f) {
	case 0x01:
		SET_MSG_INFO(s, "ACTIVATE PDP REQUEST");
		s->pdp_activate = 1;
		break;
	case 0x02:
		SET_MSG_INFO(s, "ACTIVATE PDP ACCEPT");
		handle_pdp_accept(s, dtap->data, len-2);
		break;
	case 0x03:
		SET_MSG_INFO(s, "ACTIVATE PDP REJECT");
		break;
	case 0x04:
		SET_MSG_INFO(s, "REQUEST PDP ACTIVATION");
		s->pdp_activate = 1;
		break;
	case 0x05:
		SET_MSG_INFO(s, "REQUEST PDP ACT REJECT");
		break;
	case 0x06:
		SET_MSG_INFO(s, "DEACTIVATE PDP REQUEST");
		break;
	case 0x07:
		SET_MSG_INFO(s, "DEACTIVATE PDP ACCEPT");
		break;
	case 0x08:
		SET_MSG_INFO(s, "MODIFY PDP REQUEST");
		break;
	case 0x09:
		SET_MSG_INFO(s, "MODIFY PDP ACCEPT (MS)");
		break;
	case 0x0a:
		SET_MSG_INFO(s, "MODIFY PDP REQUEST (MS)");
		break;
	case 0x0b:
		SET_MSG_INFO(s, "MODIFY PDP ACCEPT");
		break;
	case 0x0c:
		SET_MSG_INFO(s, "MODIFY PDP REJECT");
		break;
	case 0x0d:
		SET_MSG_INFO(s, "ACTIVATE 2ND PDP REQUEST");
		break;
	case 0x0e:
		SET_MSG_INFO(s, "ACTIVATE 2ND PDP ACCEPT");
		break;
	case 0x0f:
		SET_MSG_INFO(s, "ACTIVATE 2ND PDP REJECT");
		break;
	case 0x15:
		SET_MSG_INFO(s, "SM STATUS");
		break;
	case 0x1b:
		SET_MSG_INFO(s, "REQUEST 2ND PDP ACTIVATION");
		break;
	case 0x1c:
		SET_MSG_INFO(s, "REQUEST 2ND PDP ACT REJECT");
		break;
	default:
		SET_MSG_INFO(s, "UNKNOWN SM (%02x)", dtap->msg_type & 0x3f);
	}
}

void update_counters(struct session_info *s, uint8_t *data, unsigned len, unsigned data_len, uint32_t fn, uint8_t ul)
{
	int is_random = 0;
	struct frame_count *f = &s->fc;
	unsigned pad_len;
	uint8_t *pad_data;

	if (ul)
		s->uplink++;

	pad_len = len - data_len;
	pad_data = &data[data_len];

	if (pad_len > 20) {
		fprintf(stderr, "s = %d: error in pad_len %d\n", s->id, pad_len);
		return;
	}

	//TODO: handle uplink statistics
	if (ul)
		return;

	/* check padding randomization */
	if (pad_len) {
		switch (len) {
		case 20: /* SDCCH */
			if (data_len == 0) {
				/* null frames */
				is_random = rand_check(&pad_data[1], pad_len-1, &s->null, s->cipher);
			} else {
				/* other downlink */
				is_random = rand_check(&pad_data[1], pad_len-1, &s->other_sdcch, s->cipher);
			}
			break;
		case 18: /* SACCH */
			is_random = rand_check(&pad_data[1], pad_len-1, &s->other_sacch, s->cipher);
			break;
		}
	}

	if (s->cipher) {
		f->enc++;
		if (is_random) {
			f->enc_rand++;
		}
		if (!data_len) {
			f->enc_null++;
			if (is_random) {
				f->enc_null_rand++;
			} else {
				f->predict++;
			}
		} else if (len == 18) {
			f->enc_si++;
			if (is_random)
				f->enc_si_rand++;
			else
				f->predict++;
		}
	} else {
		f->unenc++;
		if (is_random) {
			f->unenc_rand++;
		}
	}
}

int is_double(struct session_info *s, uint8_t *msg, size_t len)
{
	int min_len;
	uint8_t ul;

	if (len > sizeof(s->last_dtap)) {
		len = sizeof(s->last_dtap);
	}

	min_len = len > s->last_dtap_len ? s->last_dtap_len : len;

	/* Match with previous msg, if available */
	if (min_len && !memcmp(msg, s->last_dtap, min_len)) {
		ul = !!(s->new_msg->bb.arfcn[0] & ARFCN_UPLINK);
		if (ul) {
			if ((s->last_dtap_rat == RAT_GSM) &&
			    (s->new_msg->rat != RAT_GSM)) {
				// set real rat and discard
				s->rat = s->new_msg->rat;
				s->new_msg->flags &= ~MSG_DECODED;
				return 1;
			}
		} else {
			if ((s->last_dtap_rat != RAT_GSM) &&
			    (s->new_msg->rat == RAT_GSM)) {
				// discard as we already processed the 3G one
				s->new_msg->flags &= ~MSG_DECODED;
				return 1;
			}
		}
	}

	/* store current */
	memcpy(s->last_dtap, msg, len);
	s->last_dtap_len = len;
	s->last_dtap_rat = s->new_msg->rat;

	return 0;
}

void handle_dtap(struct session_info *s, uint8_t *msg, size_t len, uint32_t fn, uint8_t ul)
{
	struct gsm48_hdr *dtap;

	assert(s != NULL);
	assert(s->new_msg != NULL);
	assert(msg != NULL);

	dtap = (struct gsm48_hdr *) msg;
	s->new_msg->info[0] = 0;

	if (len == 0) {
		SET_MSG_INFO(s, "<ZERO LENGTH>");
		return;
	}

	if (is_double(s, msg, len)) {
		SET_MSG_INFO(s, "<DOUBLE MSG>");
		return;
	}

	switch (dtap->proto_discr & GSM48_PDISC_MASK) {
	case GSM48_PDISC_CC:
		handle_cc(s, dtap, len, ul);
		break;
	case GSM48_PDISC_MM:
		handle_mm(s, dtap, len, fn);
		break;
	case GSM48_PDISC_RR:
		handle_rr(s, dtap, len, fn);
		break;
	case GSM48_PDISC_MM_GPRS:
		if (auto_reset) {
			handle_gmm(&s[1], dtap, len);
		} else {
			handle_gmm(s, dtap, len);
		}
		break;
	case GSM411_PDISC_SMS:
		handle_sms(s, dtap, len);
		break;
	case GSM48_PDISC_SM_GPRS:
		if (auto_reset) {
			handle_sm(&s[1], dtap, len);
		} else {
			handle_sm(s, dtap, len);
		}
		break;
	case GSM48_PDISC_NC_SS:
		handle_ss(s, dtap, len);
		break;
	case GSM48_PDISC_GROUP_CC:
		SET_MSG_INFO(s, "GCC");
		break;
	case GSM48_PDISC_BCAST_CC:
		SET_MSG_INFO(s, "BCC");
		break;
	case GSM48_PDISC_PDSS1:
		SET_MSG_INFO(s, "PDSS1");
		break;
	case GSM48_PDISC_PDSS2:
		SET_MSG_INFO(s, "PDSS2");
		break;
	case GSM48_PDISC_LOC:
		SET_MSG_INFO(s, "LCS");
		break;
	default:
		SET_MSG_INFO(s, "Unknown proto_discr %s: %s", (ul ? "UL" : "DL"),
			 osmo_hexdump_nospc((uint8_t *)dtap, len));
	}
}

void handle_lapdm(struct session_info *s, struct lapdm_buf *mb_sapi, uint8_t *msg, unsigned len, uint32_t fn, uint8_t ul)
{
	uint8_t sapi, lpd_type, ea;
	uint8_t frame_type, nr, ns = 0;
	//uint8_t cr, pf;
	uint8_t data_len, more_frag, fo;
	uint8_t old_auth;
	uint8_t old_cipher;
	struct lapdm_buf *mb;
	uint8_t flags;

	/* extract all bit fields */
	lpd_type = (msg[0] >> 5) & 0x03;
	sapi = (msg[0] >> 2) & 0x07;
	//cr = (msg[0] >> 1) & 0x01;
	ea = msg[0] & 0x01;

	frame_type = msg[1] & 0x03;
	//pf = (msg[1] >> 4) & 0x01;

	data_len = (msg[2] >> 2) & 0x3f;
	more_frag = (msg[2] >> 1) & 0x1;
	fo = msg[2] & 0x1;

	s->new_msg->info[0] = 0;

	/* discard non-GSM */
	if (lpd_type) {
		SET_MSG_INFO(s, "non-GSM");
		return;
	}

	/* other sanity checks */
	if (!ea || !fo || ((data_len + 3) > len)) {
		SET_MSG_INFO(s, "FAILED SANITY CHECKS (LAPDm)");
		return;
	}

	/* discard unknown SAPIs */
	if ((sapi != 0) && (sapi != 3)) {
		SET_MSG_INFO(s, "Unknown SAPI: %u", sapi);
		return;
	}

	/* get SAPI state */
	mb = &mb_sapi[!!sapi];

	switch (frame_type) {
	case 0:
	case 2:
		/* I frame */
		nr = msg[1] >> 5;
		ns = (msg[1] >> 1) & 0x07;

		/* check sequence */
		//if mb->len is > 0 then we have already received a fragment. Otherwise, not.
		//BELOW: if start of message, we never check out-of-sequence
		if (mb->len
			&& (ns != ((mb->ns + 1) % 8) || mb->no_out_of_seq_sender_msgs >= 3)) {

			if (mb->last_out_of_seq_msg_number != ns
				&& ((mb->ns - ns) % 8) < 2
			) {
				mb->no_out_of_seq_sender_msgs++;
			}
			mb->last_out_of_seq_msg_number = ns;

			SET_MSG_INFO(s
				, "<OUT OF SEQUENCE> recv %d want %d no out-of-seq %u", ns
				, (mb->ns + 1)%8, mb->no_out_of_seq_sender_msgs
			);
			update_counters(s, &msg[3], len - 3, data_len, fn, ul);

			//Fragments end, reset everything
			if (!more_frag && mb->no_out_of_seq_sender_msgs >= 3) {
				SET_MSG_INFO(s
					, "<OUT OF SEQUENCE END> no out-of-seq %u, reset seq to %u"
					, mb->no_out_of_seq_sender_msgs, ns
				);

				mb->no_out_of_seq_sender_msgs = 0;
				mb->len = 0;
				mb->ns = ns;
			}
			return;
		}

		mb->no_out_of_seq_sender_msgs = 0;
		mb->last_out_of_seq_msg_number = 100;
		mb->nr = nr;
		mb->ns = ns;
		break;
	case 1:
		/* S frame */
		if (msg_verbose > 1) {
			fprintf(stdout, "<S-FRAME>\n");
		}
		data_len = 0;
		break;
	case 3:
		/* U (unnumbered) frame */
		flags = msg[1] & 0xec;
		//001. 11.. = Command: Set Asynchronous Balanced Mode
		if (flags == 0x2c) {
			if (msg_verbose > 1) {
				fprintf(stdout, "<SABM U-FRAME>\n");
			}

			mb->no_out_of_seq_sender_msgs = 0;
			mb->len = 0;
			mb->ns = 0;
		}
		break;

	}

	update_counters(s, &msg[3], len - 3, data_len, fn, ul);

	/* discard null frames */
	if (!data_len) {
		SET_MSG_INFO(s, "<NULL>");
		return;
	}

	/* append payload to buffer */
	if (mb->len == 0) {
		memcpy(mb->data, &msg[3], data_len);
		mb->len = data_len;
		memset(&mb->data[mb->len], 0x2b, sizeof(mb->data)-data_len);
	} else {
		memcpy(&mb->data[mb->len], &msg[3], data_len);
		mb->len += data_len;
	}

	/* store current state */
	old_auth = s->auth;
	old_cipher = s->cipher;

	/* more fragments? */
	if (more_frag) {
		SET_MSG_INFO(s, "<FRAGMENT %d>", ns);
	} else {
		/* call L3 handler */
		handle_dtap(s, mb->data, mb->len, fn, ul);

		mb->len = 0;
	}

	/* hack: update auth and cipher timestamps, when ul is not available  */
	if (s->auth && old_auth && !s->auth_resp_fn && (len > 21)
		 && ((sapi != 0) || (mb->data[0] != 5))) {
		if (fn) {
			s->auth_resp_fn = fn;
		} else {
			s->auth_resp_fn = GSM_MAX_FN;
		}
	}
	if (s->cipher && old_cipher && !s->cm_comp_last_fn && (len > 21)
		&& ((sapi != 0) || (mb->data[0] != 6))) {
		if (fn) {
			if (!s->cm_comp_first_fn) {
				s->cm_comp_first_fn = fn;
			}
			s->cm_comp_last_fn = fn;
		} else {
			if (!s->cm_comp_first_fn) {
				s->cm_comp_first_fn = GSM_MAX_FN;
			}
			s->cm_comp_last_fn = GSM_MAX_FN;
		}
	}
}

void update_timestamps(struct session_info *s)
{
	uint32_t fn;

	assert(s != NULL);

	if (!s->new_msg || !s->started)
		return;

	fn = s->new_msg->bb.fn[0];

	if (!s->first_fn) {
		if (fn) {
			s->first_fn = fn;
		} else {
			s->first_fn = GSM_MAX_FN;
		}
	}

	s->last_fn = fn;
}

void handle_radio_msg(struct session_info *s, struct radio_message *m)
{
	static int num_called  = 0;
	if (msg_verbose > 1) {
		fprintf(stderr, "handle_radio_msg %d\n", num_called++);
	}

	assert(s != NULL);
	assert(m != NULL);

	uint8_t ul = !!(m->bb.arfcn[0] & ARFCN_UPLINK);

	m->info[0] = 0;
	m->flags |= MSG_DECODED;

	//s0 = CS (circuit switched) related transation
	//s1 = PS (packet switched) related transation
	int i;
	for(i = 0; i < 1 + !!auto_reset; i++) {
		assert(s[i].domain == i);
		s[i].new_msg = m;
	}

	switch (m->rat) {
	case RAT_GSM:
		switch (m->flags & 0x0f) {
		case MSG_SACCH: //slow associated control channel
			if (s->rat != RAT_GSM)
				break;

			if (msg_verbose > 1) {
				fprintf(stderr, "-> MSG_SACCH\n");
			}
			handle_lapdm(s, &s->chan_sacch[ul], &m->msg[2], m->msg_len-2, m->bb.fn[0], ul);
			break;
		case MSG_SDCCH: //standalone dedicated control channel
			if (s->rat != RAT_GSM)
				break;

			if (msg_verbose > 1) {
				fprintf(stderr, "-> MSG_SDCCH\n");
			}
			handle_lapdm(s, &s->chan_sdcch[ul], m->msg, m->msg_len, m->bb.fn[0], ul);
			break;
		case MSG_FACCH:
			if (msg_verbose > 1) {
				fprintf(stderr, "-> MSG_FACCH\n");
			}
			handle_lapdm(s, &s->chan_facch[ul], m->msg, m->msg_len, m->bb.fn[0], ul);
			break;
		case MSG_BCCH:
			if (msg_verbose > 1) {
				fprintf(stderr, "-> MSG_BCCH\n");
			}
			handle_dtap(s, &m->msg[1], m->msg_len-1, m->bb.fn[0], ul);
			break;
		default:
			if (msg_verbose > 1) {
				fprintf(stderr, "Wrong MSG flags %02x\n", m->flags);
			}
			printf("Wrong MSG flags %02x\n", m->flags);
			abort();
		}

		//if s->new_msg is not m, then we have freed it.
		if (msg_verbose && s->new_msg == m && m->flags & MSG_DECODED) {
			printf("GSM %s %s %u : %s\n", m->domain ? "PS" : "CS", ul ? "UL" : "DL",
				m->bb.fn[0], m->info[0] ? m->info : osmo_hexdump_nospc(m->msg, m->msg_len));
		}
		break;

	case RAT_UMTS:

		// if an LTE transaction was not closed
		if (s[0].rat == RAT_LTE && s[1].started == 1) {
			session_reset(s, 1);
		}
		if (m->flags & MSG_SDCCH) {
			s[0].rat = RAT_UMTS;
			s[1].rat = RAT_UMTS;

			if (ul) {
				handle_dcch_ul(s, m->bb.data, m->msg_len);
			} else {
				handle_dcch_dl(s, m->bb.data, m->msg_len);
			}
		} else if (m->flags & MSG_FACCH) {
			s[0].rat = RAT_UMTS;
			s[1].rat = RAT_UMTS;

			if (ul) {
				handle_ccch_ul(s, m->bb.data, m->msg_len);
			} else {
				handle_ccch_dl(s, m->bb.data, m->msg_len);
			}
		} else if (m->flags & MSG_BCCH) {
			handle_umts_bcch(s, m->bb.data, m->msg_len);
		} else {
			assert(0);
		}
		if (msg_verbose && s->new_msg == m && m->flags & MSG_DECODED) {
			printf("RRC %s %s %u : %s\n", m->domain ? "PS" : "CS", ul ? "UL" : "DL",
				m->bb.fn[0], m->info[0] ? m->info : osmo_hexdump_nospc(m->bb.data, m->msg_len));
		}
		break;

	case RAT_LTE:
		/* LTE NAS/EPS */
		if (m->flags & MSG_SDCCH) {
			s[0].rat = RAT_LTE;
			s[1].rat = RAT_LTE;
			/* Correct msg length for uplink */
			if (ul && m->msg_len > 6) {
				if (!not_zero(&m->bb.data[m->msg_len-6], 6)) {
					m->msg_len -= 6;
				}
			}
			handle_naseps(s, m->bb.data, m->msg_len);
		}
		if (msg_verbose && s->new_msg == m && m->flags & MSG_DECODED) {
			printf("LTE %s %u : %s\n", ul ? "UL" : "DL",
				m->bb.fn[0], m->info[0] ? m->info : osmo_hexdump_nospc(m->bb.data, m->msg_len));
		}
		break;

	default:
		return;
	}

	if (s->new_msg) {
		/* Keep fn timestamps updated */
		assert(m->domain < 2);
		update_timestamps(&s[m->domain]);

		if (s->new_msg->flags & MSG_DECODED) {
			assert(s->new_msg == m);
			link_to_msg_list(&s[m->domain], m);
			s->new_msg = NULL;
			net_send_msg(m);
		} else {
			free(m);
			s->new_msg = NULL;
		}
	}
}

unsigned encapsulate_lapdm(uint8_t *data, unsigned len, uint8_t ul, uint8_t sacch, uint8_t **output)
{
	if (!len)
		return 0;

	/* Prevent LAPDm length overflow */
	if (len > 63) {
		len = 63;
	}

	/* Select final message length */
	unsigned alloc_len;
	if (sacch) {
		alloc_len = 5 + (len < 18 ? 18 : len);
	} else {
		alloc_len = 3 + (len < 20 ? 20 : len);
	}

	/* Allocate message buffer */
	uint8_t *lapdm = malloc(alloc_len);
	if (lapdm == NULL) {
		*output = NULL;
		return 0;
	} else {
		*output = lapdm;
	}

	/* Fake SACCH L1 header */
	unsigned offset = 0;
	if (sacch) {
		lapdm[0] = 0x00;
		lapdm[1] = 0x00;
		offset = 2;
	}

	/* Fake LAPDm header */
	lapdm[offset+0] = (ul ? 0x01 : 0x03);
	lapdm[offset+1] = 0x03;
	lapdm[offset+2] = len << 2 | 0x01;
	offset += 3;

	/* Append actual payload */
	memcpy(&lapdm[offset], data, len);

	/* Add default padding */
	if (len + offset < alloc_len) {
		memset(&lapdm[len + offset], 0x2b, alloc_len - (len + offset));
	}

	return alloc_len;
}

struct radio_message * new_l2(uint8_t *data, uint8_t len, uint8_t rat, uint8_t domain, uint32_t fn, uint8_t ul, uint8_t flags)
{
	struct radio_message *m;

	assert(data != 0);

	m = (struct radio_message *) malloc(sizeof(struct radio_message));

	if (m == 0)
		return 0;

	memset(m, 0, sizeof(struct radio_message));

	m->rat = rat;
	m->domain = domain;
	switch (flags & 0x0f) {
	case MSG_SDCCH:
	case MSG_SACCH:
		m->chan_nr = 0x41;
		break;
	case MSG_FACCH:
		m->chan_nr = 0x08;
		break;
	case MSG_BCCH:
		m->chan_nr = 0x80;
	}
	m->flags = flags | MSG_DECODED;
	m->msg_len = len;
	m->bb.fn[0] = fn;
	m->bb.arfcn[0] = (ul ? ARFCN_UPLINK : 0);
	memcpy(m->msg, data, len);

	return m;
}

struct radio_message * new_l3(uint8_t *data, uint8_t len, uint8_t rat, uint8_t domain, uint32_t fn, uint8_t ul, uint8_t flags)
{
	assert(data != 0);

	unsigned lapdm_len;
	struct radio_message *m;

	if (len == 0)
		return 0;

	uint8_t *lapdm;
	if (flags & MSG_SACCH) {
		lapdm_len = encapsulate_lapdm(data, len, ul, 1, &lapdm);
	} else {
		lapdm_len = encapsulate_lapdm(data, len, ul, 0, &lapdm);
	}

	if (lapdm_len) {
		m = new_l2(lapdm, lapdm_len, rat, domain, fn, ul, flags);
		free(lapdm);
		return m;
	} else {
		return 0;
	}
}
