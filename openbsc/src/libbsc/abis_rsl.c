/* GSM Radio Signalling Link messages on the A-bis interface
 * 3GPP TS 08.58 version 8.6.0 Release 1999 / ETSI TS 100 596 V8.6.0 */

/* (C) 2008-2010 by Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <openbsc/gsm_data.h>
#include <openbsc/gsm_04_08.h>
#include <osmocore/gsm_utils.h>
#include <openbsc/abis_rsl.h>
#include <openbsc/chan_alloc.h>
#include <openbsc/bsc_rll.h>
#include <openbsc/debug.h>
#include <osmocore/tlv.h>
#include <openbsc/paging.h>
#include <openbsc/signal.h>
#include <openbsc/meas_rep.h>
#include <openbsc/rtp_proxy.h>
#include <osmocore/rsl.h>

#include <osmocore/talloc.h>

#define RSL_ALLOC_SIZE		1024
#define RSL_ALLOC_HEADROOM	128

#define MAX(a, b) (a) >= (b) ? (a) : (b)

static int rsl_send_imm_assignment(struct gsm_lchan *lchan);

static void send_lchan_signal(int sig_no, struct gsm_lchan *lchan,
			      struct gsm_meas_rep *resp)
{
	struct lchan_signal_data sig;
	sig.lchan = lchan;
	sig.mr = resp;
	dispatch_signal(SS_LCHAN, sig_no, &sig);
}

static u_int8_t mdisc_by_msgtype(u_int8_t msg_type)
{
	/* mask off the transparent bit ? */
	msg_type &= 0xfe;

	if ((msg_type & 0xf0) == 0x00)
		return ABIS_RSL_MDISC_RLL;
	if ((msg_type & 0xf0) == 0x10) {
		if (msg_type >= 0x19 && msg_type <= 0x22)
			return ABIS_RSL_MDISC_TRX;
		else
			return ABIS_RSL_MDISC_COM_CHAN;
	}
	if ((msg_type & 0xe0) == 0x20)
		return ABIS_RSL_MDISC_DED_CHAN;
	
	return ABIS_RSL_MDISC_LOC;
}

static inline void init_dchan_hdr(struct abis_rsl_dchan_hdr *dh,
				  u_int8_t msg_type)
{
	dh->c.msg_discr = mdisc_by_msgtype(msg_type);
	dh->c.msg_type = msg_type;
	dh->ie_chan = RSL_IE_CHAN_NR;
}

/* determine logical channel based on TRX and channel number IE */
struct gsm_lchan *lchan_lookup(struct gsm_bts_trx *trx, u_int8_t chan_nr)
{
	struct gsm_lchan *lchan;
	u_int8_t ts_nr = chan_nr & 0x07;
	u_int8_t cbits = chan_nr >> 3;
	u_int8_t lch_idx;
	struct gsm_bts_trx_ts *ts = &trx->ts[ts_nr];

	if (cbits == 0x01) {
		lch_idx = 0;	/* TCH/F */	
		if (ts->pchan != GSM_PCHAN_TCH_F &&
		    ts->pchan != GSM_PCHAN_PDCH &&
		    ts->pchan != GSM_PCHAN_TCH_F_PDCH)
			LOGP(DRSL, LOGL_ERROR, "chan_nr=0x%02x but pchan=%u\n",
				chan_nr, ts->pchan);
	} else if ((cbits & 0x1e) == 0x02) {
		lch_idx = cbits & 0x1;	/* TCH/H */
		if (ts->pchan != GSM_PCHAN_TCH_H)
			LOGP(DRSL, LOGL_ERROR, "chan_nr=0x%02x but pchan=%u\n",
				chan_nr, ts->pchan);
	} else if ((cbits & 0x1c) == 0x04) {
		lch_idx = cbits & 0x3;	/* SDCCH/4 */
		if (ts->pchan != GSM_PCHAN_CCCH_SDCCH4)
			LOGP(DRSL, LOGL_ERROR, "chan_nr=0x%02x but pchan=%u\n",
				chan_nr, ts->pchan);
	} else if ((cbits & 0x18) == 0x08) {
		lch_idx = cbits & 0x7;	/* SDCCH/8 */
		if (ts->pchan != GSM_PCHAN_SDCCH8_SACCH8C)
			LOGP(DRSL, LOGL_ERROR, "chan_nr=0x%02x but pchan=%u\n",
				chan_nr, ts->pchan);
	} else if (cbits == 0x10 || cbits == 0x11 || cbits == 0x12) {
		lch_idx = 0;
		if (ts->pchan != GSM_PCHAN_CCCH &&
		    ts->pchan != GSM_PCHAN_CCCH_SDCCH4)
			LOGP(DRSL, LOGL_ERROR, "chan_nr=0x%02x but pchan=%u\n",
				chan_nr, ts->pchan);
		/* FIXME: we should not return first sdcch4 !!! */
	} else {
		LOGP(DRSL, LOGL_ERROR, "unknown chan_nr=0x%02x\n", chan_nr);
		return NULL;
	}

	lchan = &ts->lchan[lch_idx];
	log_set_context(BSC_CTX_LCHAN, lchan);
	if (lchan->conn)
		log_set_context(BSC_CTX_SUBSCR, lchan->conn->subscr);

	return lchan;
}

/* See Table 10.5.25 of GSM04.08 */
static u_int8_t ts2chan_nr(const struct gsm_bts_trx_ts *ts, uint8_t lchan_nr)
{
	u_int8_t cbits, chan_nr;

	switch (ts->pchan) {
	case GSM_PCHAN_TCH_F:
	case GSM_PCHAN_PDCH:
	case GSM_PCHAN_TCH_F_PDCH:
		cbits = 0x01;
		break;
	case GSM_PCHAN_TCH_H:
		cbits = 0x02;
		cbits += lchan_nr;
		break;
	case GSM_PCHAN_CCCH_SDCCH4:
		cbits = 0x04;
		cbits += lchan_nr;
		break;
	case GSM_PCHAN_SDCCH8_SACCH8C:
		cbits = 0x08;
		cbits += lchan_nr;
		break;
	default:
	case GSM_PCHAN_CCCH:
		cbits = 0x10;
		break;
	}

	chan_nr = (cbits << 3) | (ts->nr & 0x7);

	return chan_nr;
}

u_int8_t lchan2chan_nr(const struct gsm_lchan *lchan)
{
	return ts2chan_nr(lchan->ts, lchan->nr);
}

/* As per TS 03.03 Section 2.2, the IMSI has 'not more than 15 digits' */
u_int64_t str_to_imsi(const char *imsi_str)
{
	u_int64_t ret;

	ret = strtoull(imsi_str, NULL, 10);

	return ret;
}

/* Table 5 Clause 7 TS 05.02 */
unsigned int n_pag_blocks(int bs_ccch_sdcch_comb, unsigned int bs_ag_blks_res)
{
	if (!bs_ccch_sdcch_comb)
		return 9 - bs_ag_blks_res;
	else
		return 3 - bs_ag_blks_res;
}

/* Chapter 6.5.2 of TS 05.02 */
unsigned int get_ccch_group(u_int64_t imsi, unsigned int bs_cc_chans,
			    unsigned int n_pag_blocks)
{
	return (imsi % 1000) % (bs_cc_chans * n_pag_blocks) / n_pag_blocks;
}

/* Chapter 6.5.2 of TS 05.02 */
unsigned int get_paging_group(u_int64_t imsi, unsigned int bs_cc_chans,
			      int n_pag_blocks)
{
	return (imsi % 1000) % (bs_cc_chans * n_pag_blocks) % n_pag_blocks;
}

static struct msgb *rsl_msgb_alloc(void)
{
	return msgb_alloc_headroom(RSL_ALLOC_SIZE, RSL_ALLOC_HEADROOM,
				   "RSL");
}

#define MACBLOCK_SIZE	23
static void pad_macblock(u_int8_t *out, const u_int8_t *in, int len)
{
	memcpy(out, in, len);

	if (len < MACBLOCK_SIZE)
		memset(out+len, 0x2b, MACBLOCK_SIZE-len);
}

/* Chapter 9.3.7: Encryption Information */
static int build_encr_info(u_int8_t *out, struct gsm_lchan *lchan)
{
	*out++ = lchan->encr.alg_id & 0xff;
	if (lchan->encr.key_len)
		memcpy(out, lchan->encr.key, lchan->encr.key_len);
	return lchan->encr.key_len + 1;
}

static void print_rsl_cause(int lvl, const u_int8_t *cause_v, u_int8_t cause_len)
{
	int i;

	LOGPC(DRSL, lvl, "CAUSE=0x%02x(%s) ",
		cause_v[0], rsl_err_name(cause_v[0]));
	for (i = 1; i < cause_len-1; i++)
		LOGPC(DRSL, lvl, "%02x ", cause_v[i]);
}

/* Send a BCCH_INFO message as per Chapter 8.5.1 */
int rsl_bcch_info(struct gsm_bts_trx *trx, u_int8_t type,
		  const u_int8_t *data, int len)
{
	struct abis_rsl_dchan_hdr *dh;
	struct msgb *msg = rsl_msgb_alloc();

	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof*dh);
	init_dchan_hdr(dh, RSL_MT_BCCH_INFO);
	dh->chan_nr = RSL_CHAN_BCCH;

	msgb_tv_put(msg, RSL_IE_SYSINFO_TYPE, type);
	msgb_tlv_put(msg, RSL_IE_FULL_BCCH_INFO, len, data);

	msg->trx = trx;

	return abis_rsl_sendmsg(msg);
}

int rsl_sacch_filling(struct gsm_bts_trx *trx, u_int8_t type,
		      const u_int8_t *data, int len)
{
	struct abis_rsl_common_hdr *ch;
	struct msgb *msg = rsl_msgb_alloc();

	ch = (struct abis_rsl_common_hdr *) msgb_put(msg, sizeof(*ch));
	ch->msg_discr = ABIS_RSL_MDISC_TRX;
	ch->msg_type = RSL_MT_SACCH_FILL;

	msgb_tv_put(msg, RSL_IE_SYSINFO_TYPE, type);
	msgb_tl16v_put(msg, RSL_IE_L3_INFO, len, data);

	msg->trx = trx;

	return abis_rsl_sendmsg(msg);
}

int rsl_sacch_info_modify(struct gsm_lchan *lchan, u_int8_t type,
			  const u_int8_t *data, int len)
{
	struct abis_rsl_dchan_hdr *dh;
	struct msgb *msg = rsl_msgb_alloc();
	u_int8_t chan_nr = lchan2chan_nr(lchan);

	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_SACCH_INFO_MODIFY);
	dh->chan_nr = chan_nr;

	msgb_tv_put(msg, RSL_IE_SYSINFO_TYPE, type);
	msgb_tl16v_put(msg, RSL_IE_L3_INFO, len, data);

	msg->trx = lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}

int rsl_chan_bs_power_ctrl(struct gsm_lchan *lchan, unsigned int fpc, int db)
{
	struct abis_rsl_dchan_hdr *dh;
	struct msgb *msg;
	u_int8_t chan_nr = lchan2chan_nr(lchan);

	db = abs(db);
	if (db > 30)
		return -EINVAL;

	msg = rsl_msgb_alloc();

	lchan->bs_power = db/2;
	if (fpc)
		lchan->bs_power |= 0x10;
	
	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_BS_POWER_CONTROL);
	dh->chan_nr = chan_nr;

	msgb_tv_put(msg, RSL_IE_BS_POWER, lchan->bs_power);

	msg->trx = lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}

int rsl_chan_ms_power_ctrl(struct gsm_lchan *lchan, unsigned int fpc, int dbm)
{
	struct abis_rsl_dchan_hdr *dh;
	struct msgb *msg;
	u_int8_t chan_nr = lchan2chan_nr(lchan);
	int ctl_lvl;

	ctl_lvl = ms_pwr_ctl_lvl(lchan->ts->trx->bts->band, dbm);
	if (ctl_lvl < 0)
		return ctl_lvl;

	msg = rsl_msgb_alloc();

	lchan->ms_power = ctl_lvl;

	if (fpc)
		lchan->ms_power |= 0x20;
	
	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_MS_POWER_CONTROL);
	dh->chan_nr = chan_nr;

	msgb_tv_put(msg, RSL_IE_MS_POWER, lchan->ms_power);

	msg->trx = lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}

static int channel_mode_from_lchan(struct rsl_ie_chan_mode *cm,
				   struct gsm_lchan *lchan)
{
	memset(cm, 0, sizeof(cm));

	/* FIXME: what to do with data calls ? */
	if (lchan->ts->trx->bts->network->dtx_enabled)
		cm->dtx_dtu = 0x03;
	else
		cm->dtx_dtu = 0x00;

	/* set TCH Speech/Data */
	cm->spd_ind = lchan->rsl_cmode;

	if (lchan->rsl_cmode == RSL_CMOD_SPD_SIGN &&
	    lchan->tch_mode != GSM48_CMODE_SIGN)
		LOGP(DRSL, LOGL_ERROR, "unsupported: rsl_mode == signalling, "
			"but tch_mode != signalling\n");

	switch (lchan->type) {
	case GSM_LCHAN_SDCCH:
		cm->chan_rt = RSL_CMOD_CRT_SDCCH;
		break;
	case GSM_LCHAN_TCH_F:
		cm->chan_rt = RSL_CMOD_CRT_TCH_Bm;
		break;
	case GSM_LCHAN_TCH_H:
		cm->chan_rt = RSL_CMOD_CRT_TCH_Lm;
		break;
	case GSM_LCHAN_NONE:
	case GSM_LCHAN_UNKNOWN:
	default:
		return -EINVAL;
	}

	switch (lchan->tch_mode) {
	case GSM48_CMODE_SIGN:
		cm->chan_rate = 0;
		break;
	case GSM48_CMODE_SPEECH_V1:
		cm->chan_rate = RSL_CMOD_SP_GSM1;
		break;
	case GSM48_CMODE_SPEECH_EFR:
		cm->chan_rate = RSL_CMOD_SP_GSM2;
		break;
	case GSM48_CMODE_SPEECH_AMR:
		cm->chan_rate = RSL_CMOD_SP_GSM3;
		break;
	case GSM48_CMODE_DATA_14k5:
		cm->chan_rate = RSL_CMOD_SP_NT_14k5;
		break;
	case GSM48_CMODE_DATA_12k0:
		cm->chan_rate = RSL_CMOD_SP_NT_12k0;
		break;
	case GSM48_CMODE_DATA_6k0:
		cm->chan_rate = RSL_CMOD_SP_NT_6k0;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* Chapter 8.4.1 */
#if 0
int rsl_chan_activate(struct gsm_bts_trx *trx, u_int8_t chan_nr,
		      u_int8_t act_type,
		      struct rsl_ie_chan_mode *chan_mode,
		      struct rsl_ie_chan_ident *chan_ident,
		      u_int8_t bs_power, u_int8_t ms_power,
		      u_int8_t ta)
{
	struct abis_rsl_dchan_hdr *dh;
	struct msgb *msg = rsl_msgb_alloc();

	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_CHAN_ACTIV);
	dh->chan_nr = chan_nr;

	msgb_tv_put(msg, RSL_IE_ACT_TYPE, act_type);
	/* For compatibility with Phase 1 */
	msgb_tlv_put(msg, RSL_IE_CHAN_MODE, sizeof(*chan_mode),
		     (u_int8_t *) chan_mode);
	msgb_tlv_put(msg, RSL_IE_CHAN_IDENT, 4,
		     (u_int8_t *) chan_ident);
#if 0
	msgb_tlv_put(msg, RSL_IE_ENCR_INFO, 1,
		     (u_int8_t *) &encr_info);
#endif
	msgb_tv_put(msg, RSL_IE_BS_POWER, bs_power);
	msgb_tv_put(msg, RSL_IE_MS_POWER, ms_power);
	msgb_tv_put(msg, RSL_IE_TIMING_ADVANCE, ta);

	msg->trx = trx;

	return abis_rsl_sendmsg(msg);
}
#endif

int rsl_chan_activate_lchan(struct gsm_lchan *lchan, u_int8_t act_type,
			    u_int8_t ta, u_int8_t ho_ref)
{
	struct abis_rsl_dchan_hdr *dh;
	struct msgb *msg;
	int rc;
	uint8_t *len;

	u_int8_t chan_nr = lchan2chan_nr(lchan);
	struct rsl_ie_chan_mode cm;
	struct gsm48_chan_desc cd;

	rc = channel_mode_from_lchan(&cm, lchan);
	if (rc < 0)
		return rc;

	memset(&cd, 0, sizeof(cd));
	gsm48_lchan2chan_desc(&cd, lchan);

	msg = rsl_msgb_alloc();
	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_CHAN_ACTIV);
	dh->chan_nr = chan_nr;

	msgb_tv_put(msg, RSL_IE_ACT_TYPE, act_type);
	msgb_tlv_put(msg, RSL_IE_CHAN_MODE, sizeof(cm),
		     (u_int8_t *) &cm);

	/*
	 * The Channel Identification is needed for Phase1 phones
	 * and it contains the GSM48 Channel Description and the
	 * Mobile Allocation. The GSM 08.58 asks for the Mobile
	 * Allocation to have a length of zero. We are using the
	 * msgb_l3len to calculate the length of both messages.
	 */
	msgb_v_put(msg, RSL_IE_CHAN_IDENT);
	len = msgb_put(msg, 1);
	msgb_tlv_put(msg, GSM48_IE_CHANDESC_2, sizeof(cd), (const uint8_t *) &cd);

	if (lchan->ts->hopping.enabled)
		msgb_tlv_put(msg, GSM48_IE_MA_AFTER, lchan->ts->hopping.ma_len,
			     lchan->ts->hopping.ma_data);
	else
		msgb_tlv_put(msg, GSM48_IE_MA_AFTER, 0, NULL);

	/* update the calculated size */
	msg->l3h = len + 1;
	*len = msgb_l3len(msg);

	if (lchan->encr.alg_id > RSL_ENC_ALG_A5(0)) {
		u_int8_t encr_info[MAX_A5_KEY_LEN+2];
		rc = build_encr_info(encr_info, lchan);
		if (rc > 0)
			msgb_tlv_put(msg, RSL_IE_ENCR_INFO, rc, encr_info);
	}

	switch (act_type) {
	case RSL_ACT_INTER_ASYNC:
	case RSL_ACT_INTER_SYNC:
		msgb_tv_put(msg, RSL_IE_HANDO_REF, ho_ref);
		break;
	default:
		break;
	}

	msgb_tv_put(msg, RSL_IE_BS_POWER, lchan->bs_power);
	msgb_tv_put(msg, RSL_IE_MS_POWER, lchan->ms_power);
	msgb_tv_put(msg, RSL_IE_TIMING_ADVANCE, ta);

	if (lchan->tch_mode == GSM48_CMODE_SPEECH_AMR)
		msgb_tlv_put(msg, RSL_IE_MR_CONFIG, sizeof(lchan->mr_conf),
			     (u_int8_t *) &lchan->mr_conf);

	msg->trx = lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}

/* Chapter 8.4.9: Modify channel mode on BTS side */
int rsl_chan_mode_modify_req(struct gsm_lchan *lchan)
{
	struct abis_rsl_dchan_hdr *dh;
	struct msgb *msg;
	int rc;

	u_int8_t chan_nr = lchan2chan_nr(lchan);
	struct rsl_ie_chan_mode cm;

	rc = channel_mode_from_lchan(&cm, lchan);
	if (rc < 0)
		return rc;

	msg = rsl_msgb_alloc();
	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_MODE_MODIFY_REQ);
	dh->chan_nr = chan_nr;

	msgb_tlv_put(msg, RSL_IE_CHAN_MODE, sizeof(cm),
		     (u_int8_t *) &cm);

	if (lchan->encr.alg_id > RSL_ENC_ALG_A5(0)) {
		u_int8_t encr_info[MAX_A5_KEY_LEN+2];
		rc = build_encr_info(encr_info, lchan);
		if (rc > 0)
			msgb_tlv_put(msg, RSL_IE_ENCR_INFO, rc, encr_info);
	}

	if (lchan->tch_mode == GSM48_CMODE_SPEECH_AMR) {
		msgb_tlv_put(msg, RSL_IE_MR_CONFIG, sizeof(lchan->mr_conf),
			     (u_int8_t *) &lchan->mr_conf);
	}

	msg->trx = lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}

/* Chapter 8.4.6: Send the encryption command with given L3 info */
int rsl_encryption_cmd(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *dh;
	struct gsm_lchan *lchan = msg->lchan;
	u_int8_t chan_nr = lchan2chan_nr(lchan);
	u_int8_t encr_info[MAX_A5_KEY_LEN+2];
	u_int8_t l3_len = msg->len;
	int rc;

	/* First push the L3 IE tag and length */
	msgb_tv16_push(msg, RSL_IE_L3_INFO, l3_len);

	/* then the link identifier (SAPI0, main sign link) */
	msgb_tv_push(msg, RSL_IE_LINK_IDENT, 0);

	/* then encryption information */
	rc = build_encr_info(encr_info, lchan);
	if (rc <= 0)
		return rc;
	msgb_tlv_push(msg, RSL_IE_ENCR_INFO, rc, encr_info);

	/* and finally the DCHAN header */
	dh = (struct abis_rsl_dchan_hdr *) msgb_push(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_ENCR_CMD);
	dh->chan_nr = chan_nr;

	msg->trx = lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}

/* Chapter 8.4.5 / 4.6: Deactivate the SACCH after 04.08 RR CHAN RELEASE */
int rsl_deact_sacch(struct gsm_lchan *lchan)
{
	struct abis_rsl_dchan_hdr *dh;
	struct msgb *msg = rsl_msgb_alloc();

	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_DEACTIVATE_SACCH);
	dh->chan_nr = lchan2chan_nr(lchan);

	msg->lchan = lchan;
	msg->trx = lchan->ts->trx;

	DEBUGP(DRSL, "%s DEACTivate SACCH CMD\n", gsm_lchan_name(lchan));

	return abis_rsl_sendmsg(msg);
}

static void error_timeout_cb(void *data)
{
	struct gsm_lchan *lchan = data;
	if (lchan->state != LCHAN_S_REL_ERR) {
		LOGP(DRSL, LOGL_ERROR, "%s error timeout but not in error state: %d\n",
		     gsm_lchan_name(lchan), lchan->state);
		return;
	}

	/* go back to the none state */
	LOGP(DRSL, LOGL_NOTICE, "%s is back in operation.\n", gsm_lchan_name(lchan));
	rsl_lchan_set_state(lchan, LCHAN_S_NONE);
}

static int rsl_rx_rf_chan_rel_ack(struct gsm_lchan *lchan);

/* Chapter 8.4.14 / 4.7: Tell BTS to release the radio channel */
static int rsl_rf_chan_release(struct gsm_lchan *lchan, int error)
{
	struct abis_rsl_dchan_hdr *dh;
	struct msgb *msg;
	int rc;

	if (lchan->state == LCHAN_S_REL_ERR) {
		LOGP(DRSL, LOGL_NOTICE, "%s is in error state not sending release.\n",
		     gsm_lchan_name(lchan));
		return -1;
	}

	msg = rsl_msgb_alloc();
	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_RF_CHAN_REL);
	dh->chan_nr = lchan2chan_nr(lchan);

	msg->lchan = lchan;
	msg->trx = lchan->ts->trx;

	DEBUGP(DRSL, "%s RF Channel Release CMD due error %d\n", gsm_lchan_name(lchan), error);

	if (error) {
		/*
		 * the nanoBTS sends RLL release indications after the channel release. This can
		 * be a problem when we have reassigned the channel to someone else and then can
		 * not figure out who used this channel.
		 */
		rsl_lchan_set_state(lchan, LCHAN_S_REL_ERR);
		lchan->error_timer.data = lchan;
		lchan->error_timer.cb = error_timeout_cb;
		bsc_schedule_timer(&lchan->error_timer,
				   msg->trx->bts->network->T3111 + 2, 0);
	}

	rc =  abis_rsl_sendmsg(msg);

	/* BTS will respond by RF CHAN REL ACK */
#ifdef HSL_SR_1_0
	/* The HSL Femto seems to 'forget' sending a REL ACK for TS1...TS7 */
	if (lchan->ts->trx->bts->type == GSM_BTS_TYPE_HSL_FEMTO && lchan->ts->nr != 0)
		rc = rsl_rx_rf_chan_rel_ack(lchan);
#endif

	return rc;
}

static int rsl_rx_rf_chan_rel_ack(struct gsm_lchan *lchan)
{

	DEBUGP(DRSL, "%s RF CHANNEL RELEASE ACK\n", gsm_lchan_name(lchan));

	if (lchan->state != LCHAN_S_REL_REQ && lchan->state != LCHAN_S_REL_ERR)
		LOGP(DRSL, LOGL_NOTICE, "%s CHAN REL ACK but state %s\n",
			gsm_lchan_name(lchan),
			gsm_lchans_name(lchan->state));
	bsc_del_timer(&lchan->T3111);
	/* we have an error timer pending to release that */
	if (lchan->state != LCHAN_S_REL_ERR)
		rsl_lchan_set_state(lchan, LCHAN_S_NONE);
	lchan_free(lchan);

	return 0;
}

int rsl_paging_cmd(struct gsm_bts *bts, u_int8_t paging_group, u_int8_t len,
		   u_int8_t *ms_ident, u_int8_t chan_needed)
{
	struct abis_rsl_dchan_hdr *dh;
	struct msgb *msg = rsl_msgb_alloc();

	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_PAGING_CMD);
	dh->chan_nr = RSL_CHAN_PCH_AGCH;

	msgb_tv_put(msg, RSL_IE_PAGING_GROUP, paging_group);
	msgb_tlv_put(msg, RSL_IE_MS_IDENTITY, len-2, ms_ident+2);
	msgb_tv_put(msg, RSL_IE_CHAN_NEEDED, chan_needed);

	msg->trx = bts->c0;

	return abis_rsl_sendmsg(msg);
}

int imsi_str2bcd(u_int8_t *bcd_out, const char *str_in)
{
	int i, len = strlen(str_in);

	for (i = 0; i < len; i++) {
		int num = str_in[i] - 0x30;
		if (num < 0 || num > 9)
			return -1;
		if (i % 2 == 0)
			bcd_out[i/2] = num;
		else
			bcd_out[i/2] |= (num << 4);
	}

	return 0;
}

/* Chapter 8.5.6 */
int rsl_imm_assign_cmd(struct gsm_bts *bts, u_int8_t len, u_int8_t *val)
{
	struct msgb *msg = rsl_msgb_alloc();
	struct abis_rsl_dchan_hdr *dh;
	u_int8_t buf[MACBLOCK_SIZE];

	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_IMMEDIATE_ASSIGN_CMD);
	dh->chan_nr = RSL_CHAN_PCH_AGCH;

	switch (bts->type) {
	case GSM_BTS_TYPE_BS11:
		msgb_tlv_put(msg, RSL_IE_IMM_ASS_INFO, len, val);
		break;
	default:
		/* If phase 2, construct a FULL_IMM_ASS_INFO */
		pad_macblock(buf, val, len);
		msgb_tlv_put(msg, RSL_IE_FULL_IMM_ASS_INFO, MACBLOCK_SIZE, buf);
		break;
	}

	msg->trx = bts->c0;

	return abis_rsl_sendmsg(msg);
}

/* Send Siemens specific MS RF Power Capability Indication */
int rsl_siemens_mrpci(struct gsm_lchan *lchan, struct rsl_mrpci *mrpci)
{
	struct msgb *msg = rsl_msgb_alloc();
	struct abis_rsl_dchan_hdr *dh;

	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_SIEMENS_MRPCI);
	dh->c.msg_discr = ABIS_RSL_MDISC_DED_CHAN;
	dh->chan_nr = lchan2chan_nr(lchan);
	msgb_tv_put(msg, RSL_IE_SIEMENS_MRPCI, *(u_int8_t *)mrpci);

	DEBUGP(DRSL, "%s TX Siemens MRPCI 0x%02x\n",
		gsm_lchan_name(lchan), *(u_int8_t *)mrpci);

	msg->trx = lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}


/* Send "DATA REQUEST" message with given L3 Info payload */
/* Chapter 8.3.1 */
int rsl_data_request(struct msgb *msg, u_int8_t link_id)
{
	if (msg->lchan == NULL) {
		LOGP(DRSL, LOGL_ERROR, "cannot send DATA REQUEST to unknown lchan\n");
		return -EINVAL;
	}

	rsl_rll_push_l3(msg, RSL_MT_DATA_REQ, lchan2chan_nr(msg->lchan),
			link_id, 1);

	msg->trx = msg->lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}

/* Send "ESTABLISH REQUEST" message with given L3 Info payload */
/* Chapter 8.3.1 */
int rsl_establish_request(struct gsm_lchan *lchan, u_int8_t link_id)
{
	struct msgb *msg;

	msg = rsl_rll_simple(RSL_MT_EST_REQ, lchan2chan_nr(lchan),
			     link_id, 0);
	msg->trx = lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}

/* Chapter 8.3.7 Request the release of multiframe mode of RLL connection.
   This is what higher layers should call.  The BTS then responds with
   RELEASE CONFIRM, which we in turn use to trigger RSL CHANNEL RELEASE,
   which in turn is acknowledged by RSL CHANNEL RELEASE ACK, which calls
   lchan_free() */
int rsl_release_request(struct gsm_lchan *lchan, u_int8_t link_id, u_int8_t reason)
{

	struct msgb *msg;

	msg = rsl_rll_simple(RSL_MT_REL_REQ, lchan2chan_nr(lchan),
			     link_id, 0);
	/* 0 is normal release, 1 is local end */
	msgb_tv_put(msg, RSL_IE_RELEASE_MODE, reason);

	/* FIXME: start some timer in case we don't receive a REL ACK ? */

	msg->trx = lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}

int rsl_lchan_set_state(struct gsm_lchan *lchan, int state)
{
	lchan->state = state;
	return 0;
}

/* Chapter 8.4.2: Channel Activate Acknowledge */
static int rsl_rx_chan_act_ack(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *rslh = msgb_l2(msg);

	/* BTS has confirmed channel activation, we now need
	 * to assign the activated channel to the MS */
	if (rslh->ie_chan != RSL_IE_CHAN_NR)
		return -EINVAL;

	if (msg->lchan->state != LCHAN_S_ACT_REQ)
		LOGP(DRSL, LOGL_NOTICE, "%s CHAN ACT ACK, but state %s\n",
			gsm_lchan_name(msg->lchan),
			gsm_lchans_name(msg->lchan->state));
	rsl_lchan_set_state(msg->lchan, LCHAN_S_ACTIVE);

	if (msg->lchan->rqd_ref) {
		rsl_send_imm_assignment(msg->lchan);
		talloc_free(msg->lchan->rqd_ref);
		msg->lchan->rqd_ref = NULL;
		msg->lchan->rqd_ta = 0;
	}

	send_lchan_signal(S_LCHAN_ACTIVATE_ACK, msg->lchan, NULL);

	return 0;
}

/* Chapter 8.4.3: Channel Activate NACK */
static int rsl_rx_chan_act_nack(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *dh = msgb_l2(msg);
	struct tlv_parsed tp;

	LOGP(DRSL, LOGL_ERROR, "%s CHANNEL ACTIVATE NACK",
		gsm_lchan_name(msg->lchan));

	/* BTS has rejected channel activation ?!? */
	if (dh->ie_chan != RSL_IE_CHAN_NR)
		return -EINVAL;

	rsl_tlv_parse(&tp, dh->data, msgb_l2len(msg)-sizeof(*dh));
	if (TLVP_PRESENT(&tp, RSL_IE_CAUSE)) {
		const u_int8_t *cause = TLVP_VAL(&tp, RSL_IE_CAUSE);
		print_rsl_cause(LOGL_ERROR, cause,
				TLVP_LEN(&tp, RSL_IE_CAUSE));
		if (*cause != RSL_ERR_RCH_ALR_ACTV_ALLOC)
			rsl_lchan_set_state(msg->lchan, LCHAN_S_NONE);
	} else
		rsl_lchan_set_state(msg->lchan, LCHAN_S_NONE);

	LOGPC(DRSL, LOGL_ERROR, "\n");

	send_lchan_signal(S_LCHAN_ACTIVATE_NACK, msg->lchan, NULL);

	lchan_free(msg->lchan);
	return 0;
}

/* Chapter 8.4.4: Connection Failure Indication */
static int rsl_rx_conn_fail(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *dh = msgb_l2(msg);
	struct tlv_parsed tp;

	/* FIXME: print which channel */
	LOGP(DRSL, LOGL_NOTICE, "%s CONNECTION FAIL: RELEASING ",
	     gsm_lchan_name(msg->lchan));

	rsl_tlv_parse(&tp, dh->data, msgb_l2len(msg)-sizeof(*dh));

	if (TLVP_PRESENT(&tp, RSL_IE_CAUSE))
		print_rsl_cause(LOGL_NOTICE, TLVP_VAL(&tp, RSL_IE_CAUSE),
				TLVP_LEN(&tp, RSL_IE_CAUSE));

	LOGPC(DRSL, LOGL_NOTICE, "\n");
	/* FIXME: only free it after channel release ACK */
	counter_inc(msg->lchan->ts->trx->bts->network->stats.chan.rf_fail);
	return rsl_rf_chan_release(msg->lchan, 1);
}

static void print_meas_rep_uni(struct gsm_meas_rep_unidir *mru,
				const char *prefix)
{
	DEBUGPC(DMEAS, "RXL-FULL-%s=%3ddBm RXL-SUB-%s=%3ddBm ",
		prefix, rxlev2dbm(mru->full.rx_lev),
		prefix, rxlev2dbm(mru->sub.rx_lev));
	DEBUGPC(DMEAS, "RXQ-FULL-%s=%d RXQ-SUB-%s=%d ",
		prefix, mru->full.rx_qual, prefix, mru->sub.rx_qual);
}

static void print_meas_rep(struct gsm_meas_rep *mr)
{
	int i;

	DEBUGP(DMEAS, "MEASUREMENT RESULT NR=%d ", mr->nr);

	if (mr->flags & MEAS_REP_F_DL_DTX)
		DEBUGPC(DMEAS, "DTXd ");

	print_meas_rep_uni(&mr->ul, "ul");
	DEBUGPC(DMEAS, "BS_POWER=%d ", mr->bs_power);
	if (mr->flags & MEAS_REP_F_MS_TO)
		DEBUGPC(DMEAS, "MS_TO=%d ", mr->ms_timing_offset);

	if (mr->flags & MEAS_REP_F_MS_L1) {
		DEBUGPC(DMEAS, "L1_MS_PWR=%3ddBm ", mr->ms_l1.pwr);
		DEBUGPC(DMEAS, "L1_FPC=%u ",
			mr->flags & MEAS_REP_F_FPC ? 1 : 0);
		DEBUGPC(DMEAS, "L1_TA=%u ", mr->ms_l1.ta);
	}

	if (mr->flags & MEAS_REP_F_UL_DTX)
		DEBUGPC(DMEAS, "DTXu ");
	if (mr->flags & MEAS_REP_F_BA1)
		DEBUGPC(DMEAS, "BA1 ");
	if (!(mr->flags & MEAS_REP_F_DL_VALID))
		DEBUGPC(DMEAS, "NOT VALID ");
	else
		print_meas_rep_uni(&mr->dl, "dl");

	DEBUGPC(DMEAS, "NUM_NEIGH=%u\n", mr->num_cell);
	if (mr->num_cell == 7)
		return;
	for (i = 0; i < mr->num_cell; i++) {
		struct gsm_meas_rep_cell *mrc = &mr->cell[i];
		DEBUGP(DMEAS, "IDX=%u ARFCN=%u BSIC=%u => %d dBm\n",
			mrc->neigh_idx, mrc->arfcn, mrc->bsic, rxlev2dbm(mrc->rxlev));
	}
}

static int rsl_rx_meas_res(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *dh = msgb_l2(msg);
	struct tlv_parsed tp;
	struct gsm_meas_rep *mr = lchan_next_meas_rep(msg->lchan);
	u_int8_t len;
	const u_int8_t *val;
	int rc;

	/* check if this channel is actually active */
	/* FIXME: maybe this check should be way more generic/centralized */
	if (msg->lchan->state != LCHAN_S_ACTIVE) {
		LOGP(DRSL, LOGL_DEBUG, "%s: MEAS RES for inactive channel\n",
			gsm_lchan_name(msg->lchan));
		return 0;
	}

	memset(mr, 0, sizeof(*mr));
	mr->lchan = msg->lchan;

	rsl_tlv_parse(&tp, dh->data, msgb_l2len(msg)-sizeof(*dh));

	if (!TLVP_PRESENT(&tp, RSL_IE_MEAS_RES_NR) ||
	    !TLVP_PRESENT(&tp, RSL_IE_UPLINK_MEAS) ||
	    !TLVP_PRESENT(&tp, RSL_IE_BS_POWER))
		return -EIO;

	/* Mandatory Parts */
	mr->nr = *TLVP_VAL(&tp, RSL_IE_MEAS_RES_NR);

	len = TLVP_LEN(&tp, RSL_IE_UPLINK_MEAS);
	val = TLVP_VAL(&tp, RSL_IE_UPLINK_MEAS);
	if (len >= 3) {
		if (val[0] & 0x40)
			mr->flags |= MEAS_REP_F_DL_DTX;
		mr->ul.full.rx_lev = val[0] & 0x3f;
		mr->ul.sub.rx_lev = val[1] & 0x3f;
		mr->ul.full.rx_qual = val[2]>>3 & 0x7;
		mr->ul.sub.rx_qual = val[2] & 0x7;
	}

	mr->bs_power = *TLVP_VAL(&tp, RSL_IE_BS_POWER);

	/* Optional Parts */
	if (TLVP_PRESENT(&tp, RSL_IE_MS_TIMING_OFFSET))
		mr->ms_timing_offset =
			*TLVP_VAL(&tp, RSL_IE_MS_TIMING_OFFSET);

	if (TLVP_PRESENT(&tp, RSL_IE_L1_INFO)) {
		val = TLVP_VAL(&tp, RSL_IE_L1_INFO);
		mr->flags |= MEAS_REP_F_MS_L1;
		mr->ms_l1.pwr = ms_pwr_dbm(msg->trx->bts->band, val[0] >> 3);
		if (val[0] & 0x04)
			mr->flags |= MEAS_REP_F_FPC;
		mr->ms_l1.ta = val[1];
	}
	if (TLVP_PRESENT(&tp, RSL_IE_L3_INFO)) {
		msg->l3h = (u_int8_t *) TLVP_VAL(&tp, RSL_IE_L3_INFO);
		rc = gsm48_parse_meas_rep(mr, msg);
		if (rc < 0)
			return rc;
	}

	print_meas_rep(mr);

	send_lchan_signal(S_LCHAN_MEAS_REP, msg->lchan, mr);

	return 0;
}

/* Chapter 8.4.7 */
static int rsl_rx_hando_det(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *dh = msgb_l2(msg);
	struct tlv_parsed tp;

	DEBUGP(DRSL, "%s HANDOVER DETECT ", gsm_lchan_name(msg->lchan));

	rsl_tlv_parse(&tp, dh->data, msgb_l2len(msg)-sizeof(*dh));

	if (TLVP_PRESENT(&tp, RSL_IE_ACCESS_DELAY))
		DEBUGPC(DRSL, "access delay = %u\n",
			*TLVP_VAL(&tp, RSL_IE_ACCESS_DELAY));
	else
		DEBUGPC(DRSL, "\n");

	send_lchan_signal(S_LCHAN_HANDOVER_DETECT, msg->lchan, NULL);

	return 0;
}

static int abis_rsl_rx_dchan(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *rslh = msgb_l2(msg);
	int rc = 0;
	char *ts_name;

	msg->lchan = lchan_lookup(msg->trx, rslh->chan_nr);
	ts_name = gsm_lchan_name(msg->lchan);

	switch (rslh->c.msg_type) {
	case RSL_MT_CHAN_ACTIV_ACK:
		DEBUGP(DRSL, "%s CHANNEL ACTIVATE ACK\n", ts_name);
		rc = rsl_rx_chan_act_ack(msg);
		break;
	case RSL_MT_CHAN_ACTIV_NACK:
		rc = rsl_rx_chan_act_nack(msg);
		break;
	case RSL_MT_CONN_FAIL:
		rc = rsl_rx_conn_fail(msg);
		break;
	case RSL_MT_MEAS_RES:
		rc = rsl_rx_meas_res(msg);
		break;
	case RSL_MT_HANDO_DET:
		rc = rsl_rx_hando_det(msg);
		break;
	case RSL_MT_RF_CHAN_REL_ACK:
		rc = rsl_rx_rf_chan_rel_ack(msg->lchan);
		break;
	case RSL_MT_MODE_MODIFY_ACK:
		DEBUGP(DRSL, "%s CHANNEL MODE MODIFY ACK\n", ts_name);
		break;
	case RSL_MT_MODE_MODIFY_NACK:
		LOGP(DRSL, LOGL_ERROR, "%s CHANNEL MODE MODIFY NACK\n", ts_name);
		break;
	case RSL_MT_IPAC_PDCH_ACT_ACK:
		DEBUGPC(DRSL, "%s IPAC PDCH ACT ACK\n", ts_name);
		msg->lchan->ts->flags |= TS_F_PDCH_MODE;
		break;
	case RSL_MT_IPAC_PDCH_ACT_NACK:
		LOGP(DRSL, LOGL_ERROR, "%s IPAC PDCH ACT NACK\n", ts_name);
		break;
	case RSL_MT_IPAC_PDCH_DEACT_ACK:
		DEBUGP(DRSL, "%s IPAC PDCH DEACT ACK\n", ts_name);
		msg->lchan->ts->flags &= ~TS_F_PDCH_MODE;
		break;
	case RSL_MT_IPAC_PDCH_DEACT_NACK:
		LOGP(DRSL, LOGL_ERROR, "%s IPAC PDCH DEACT NACK\n", ts_name);
		break;
	case RSL_MT_PHY_CONTEXT_CONF:
	case RSL_MT_PREPROC_MEAS_RES:
	case RSL_MT_TALKER_DET:
	case RSL_MT_LISTENER_DET:
	case RSL_MT_REMOTE_CODEC_CONF_REP:
	case RSL_MT_MR_CODEC_MOD_ACK:
	case RSL_MT_MR_CODEC_MOD_NACK:
	case RSL_MT_MR_CODEC_MOD_PER:
		LOGP(DRSL, LOGL_NOTICE, "%s Unimplemented Abis RSL DChan "
			"msg 0x%02x\n", ts_name, rslh->c.msg_type);
		break;
	default:
		LOGP(DRSL, LOGL_NOTICE, "%s unknown Abis RSL DChan msg 0x%02x\n",
			ts_name, rslh->c.msg_type);
		return -EINVAL;
	}

	return rc;
}

static int rsl_rx_error_rep(struct msgb *msg)
{
	struct abis_rsl_common_hdr *rslh = msgb_l2(msg);
	struct tlv_parsed tp;

	LOGP(DRSL, LOGL_ERROR, "%s ERROR REPORT ", gsm_trx_name(msg->trx));

	rsl_tlv_parse(&tp, rslh->data, msgb_l2len(msg)-sizeof(*rslh));

	if (TLVP_PRESENT(&tp, RSL_IE_CAUSE))
		print_rsl_cause(LOGL_ERROR, TLVP_VAL(&tp, RSL_IE_CAUSE),
				TLVP_LEN(&tp, RSL_IE_CAUSE));

	LOGPC(DRSL, LOGL_ERROR, "\n");

	return 0;
}

static int abis_rsl_rx_trx(struct msgb *msg)
{
	struct abis_rsl_common_hdr *rslh = msgb_l2(msg);
	int rc = 0;

	switch (rslh->msg_type) {
	case RSL_MT_ERROR_REPORT:
		rc = rsl_rx_error_rep(msg);
		break;
	case RSL_MT_RF_RES_IND:
		/* interference on idle channels of TRX */
		//DEBUGP(DRSL, "%s RF Resource Indication\n", gsm_trx_name(msg->trx));
		break;
	case RSL_MT_OVERLOAD:
		/* indicate CCCH / ACCH / processor overload */
		LOGP(DRSL, LOGL_ERROR, "%s CCCH/ACCH/CPU Overload\n",
		     gsm_trx_name(msg->trx));
		break;
	default:
		LOGP(DRSL, LOGL_NOTICE, "%s Unknown Abis RSL TRX message "
			"type 0x%02x\n", gsm_trx_name(msg->trx), rslh->msg_type);
		return -EINVAL;
	}
	return rc;
}

/* If T3101 expires, we never received a response to IMMEDIATE ASSIGN */
static void t3101_expired(void *data)
{
	struct gsm_lchan *lchan = data;

	rsl_rf_chan_release(lchan, 1);
}

/* If T3111 expires, we will send the RF Channel Request */
static void t3111_expired(void *data)
{
	struct gsm_lchan *lchan = data;

	rsl_rf_chan_release(lchan, 0);
}

#define GSM48_LEN2PLEN(a)	(((a) << 2) | 1)

/* Format an IMM ASS REJ according to 04.08 Chapter 9.1.20 */
static int rsl_send_imm_ass_rej(struct gsm_bts *bts,
				unsigned int num_req_refs,
				struct gsm48_req_ref *rqd_refs,
				uint8_t wait_ind)
{
	uint8_t buf[GSM_MACBLOCK_LEN];
	struct gsm48_imm_ass_rej *iar = (struct gsm48_imm_ass_rej *)buf;

	/* create IMMEDIATE ASSIGN REJECT 04.08 message */
	memset(iar, 0, sizeof(*iar));
	iar->proto_discr = GSM48_PDISC_RR;
	iar->msg_type = GSM48_MT_RR_IMM_ASS;
	iar->page_mode = GSM48_PM_SAME;

	memcpy(&iar->req_ref1, &rqd_refs[0], sizeof(iar->req_ref1));
	iar->wait_ind1 = wait_ind;

	if (num_req_refs >= 2)
		memcpy(&iar->req_ref2, &rqd_refs[1], sizeof(iar->req_ref2));
	else
		memcpy(&iar->req_ref2, &rqd_refs[0], sizeof(iar->req_ref2));
	iar->wait_ind2 = wait_ind;

	if (num_req_refs >= 3)
		memcpy(&iar->req_ref3, &rqd_refs[2], sizeof(iar->req_ref3));
	else
		memcpy(&iar->req_ref3, &rqd_refs[0], sizeof(iar->req_ref3));
	iar->wait_ind3 = wait_ind;

	if (num_req_refs >= 4)
		memcpy(&iar->req_ref4, &rqd_refs[3], sizeof(iar->req_ref4));
	else
		memcpy(&iar->req_ref4, &rqd_refs[0], sizeof(iar->req_ref4));
	iar->wait_ind4 = wait_ind;

	return rsl_imm_assign_cmd(bts, sizeof(iar), (uint8_t *) iar);
}

/* MS has requested a channel on the RACH */
static int rsl_rx_chan_rqd(struct msgb *msg)
{
	struct gsm_bts *bts = msg->trx->bts;
	struct abis_rsl_dchan_hdr *rqd_hdr = msgb_l2(msg);
	struct gsm48_req_ref *rqd_ref;
	enum gsm_chan_t lctype;
	enum gsm_chreq_reason_t chreq_reason;
	struct gsm_lchan *lchan;
	u_int8_t rqd_ta;
	int is_lu;

	u_int16_t arfcn;
	u_int8_t ts_number, subch;

	/* parse request reference to be used in immediate assign */
	if (rqd_hdr->data[0] != RSL_IE_REQ_REFERENCE)
		return -EINVAL;

	rqd_ref = (struct gsm48_req_ref *) &rqd_hdr->data[1];

	/* parse access delay and use as TA */
	if (rqd_hdr->data[sizeof(struct gsm48_req_ref)+1] != RSL_IE_ACCESS_DELAY)
		return -EINVAL;
	rqd_ta = rqd_hdr->data[sizeof(struct gsm48_req_ref)+2];

	/* determine channel type (SDCCH/TCH_F/TCH_H) based on
	 * request reference RA */
	lctype = get_ctype_by_chreq(bts->network, rqd_ref->ra);
	chreq_reason = get_reason_by_chreq(rqd_ref->ra, bts->network->neci);

	counter_inc(bts->network->stats.chreq.total);

	/*
	 * We want LOCATION UPDATES to succeed and will assign a TCH
	 * if we have no SDCCH available.
	 */
	is_lu = !!(chreq_reason == GSM_CHREQ_REASON_LOCATION_UPD);

	/* check availability / allocate channel */
	lchan = lchan_alloc(bts, lctype, is_lu);
	if (!lchan) {
		LOGP(DRSL, LOGL_NOTICE, "BTS %d CHAN RQD: no resources for %s 0x%x\n",
		     msg->lchan->ts->trx->bts->nr, gsm_lchant_name(lctype), rqd_ref->ra);
		counter_inc(bts->network->stats.chreq.no_channel);
		/* FIXME gather multiple CHAN RQD and reject up to 4 at the same time */
		if (bts->network->T3122)
			rsl_send_imm_ass_rej(bts, 1, rqd_ref, bts->network->T3122 & 0xff);
		return -ENOMEM;
	}

	if (lchan->state != LCHAN_S_NONE)
		LOGP(DRSL, LOGL_NOTICE, "%s lchan_alloc() returned channel "
		     "in state %s\n", gsm_lchan_name(lchan),
		     gsm_lchans_name(lchan->state));
	rsl_lchan_set_state(lchan, LCHAN_S_ACT_REQ);

	/* save the RACH data as we need it after the CHAN ACT ACK */
	lchan->rqd_ref = talloc_zero(bts, struct gsm48_req_ref);
	if (!lchan->rqd_ref) {
		LOGP(DRSL, LOGL_ERROR, "Failed to allocate gsm48_req_ref.\n");
		lchan_free(lchan);
		return -ENOMEM;
	}

	memcpy(lchan->rqd_ref, rqd_ref, sizeof(*rqd_ref));
	lchan->rqd_ta = rqd_ta;

	ts_number = lchan->ts->nr;
	arfcn = lchan->ts->trx->arfcn;
	subch = lchan->nr;
	
	lchan->encr.alg_id = RSL_ENC_ALG_A5(0);	/* no encryption */
	lchan->ms_power = ms_pwr_ctl_lvl(bts->band, bts->ms_max_power);
	lchan->bs_power = 0; /* 0dB reduction, output power = Pn */
	lchan->rsl_cmode = RSL_CMOD_SPD_SIGN;
	lchan->tch_mode = GSM48_CMODE_SIGN;

	/* FIXME: Start another timer or assume the BTS sends a ACK/NACK? */
	rsl_chan_activate_lchan(lchan, 0x00, rqd_ta, 0);

	DEBUGP(DRSL, "%s Activating ARFCN(%u) SS(%u) lctype %s "
		"r=%s ra=0x%02x\n", gsm_lchan_name(lchan), arfcn, subch,
		gsm_lchant_name(lchan->type), gsm_chreq_name(chreq_reason),
		rqd_ref->ra);
	return 0;
}

static int rsl_send_imm_assignment(struct gsm_lchan *lchan)
{
	struct gsm_bts *bts = lchan->ts->trx->bts;
	u_int8_t buf[GSM_MACBLOCK_LEN];
	struct gsm48_imm_ass *ia = (struct gsm48_imm_ass *) buf;

	/* create IMMEDIATE ASSIGN 04.08 messge */
	memset(ia, 0, sizeof(*ia));
	/* we set ia->l2_plen once we know the length of the MA below */
	ia->proto_discr = GSM48_PDISC_RR;
	ia->msg_type = GSM48_MT_RR_IMM_ASS;
	ia->page_mode = GSM48_PM_SAME;
	gsm48_lchan2chan_desc(&ia->chan_desc, lchan);

	/* use request reference extracted from CHAN_RQD */
	memcpy(&ia->req_ref, lchan->rqd_ref, sizeof(ia->req_ref));
	ia->timing_advance = lchan->rqd_ta;
	if (!lchan->ts->hopping.enabled) {
		ia->mob_alloc_len = 0;
	} else {
		ia->mob_alloc_len = lchan->ts->hopping.ma_len;
		memcpy(ia->mob_alloc, lchan->ts->hopping.ma_data, ia->mob_alloc_len);
	}
	/* we need to subtract 1 byte from sizeof(*ia) since ia includes the l2_plen field */
	ia->l2_plen = GSM48_LEN2PLEN((sizeof(*ia)-1) + ia->mob_alloc_len);

	/* Start timer T3101 to wait for GSM48_MT_RR_PAG_RESP */
	lchan->T3101.cb = t3101_expired;
	lchan->T3101.data = lchan;
	bsc_schedule_timer(&lchan->T3101, bts->network->T3101, 0);

	/* send IMMEDIATE ASSIGN CMD on RSL to BTS (to send on CCCH to MS) */
	return rsl_imm_assign_cmd(bts, sizeof(*ia)+ia->mob_alloc_len, (u_int8_t *) ia);
}

/* MS has requested a channel on the RACH */
static int rsl_rx_ccch_load(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *rslh = msgb_l2(msg);
	u_int16_t pg_buf_space;
	u_int16_t rach_slot_count = -1;
	u_int16_t rach_busy_count = -1;
	u_int16_t rach_access_count = -1;

	switch (rslh->data[0]) {
	case RSL_IE_PAGING_LOAD:
		pg_buf_space = rslh->data[1] << 8 | rslh->data[2];
		if (is_ipaccess_bts(msg->trx->bts) && pg_buf_space == 0xffff) {
			/* paging load below configured threshold, use 50 as default */
			pg_buf_space = 50;
		}
		paging_update_buffer_space(msg->trx->bts, pg_buf_space);
		break;
	case RSL_IE_RACH_LOAD:
		if (msg->data_len >= 7) {
			rach_slot_count = rslh->data[2] << 8 | rslh->data[3];
			rach_busy_count = rslh->data[4] << 8 | rslh->data[5];
			rach_access_count = rslh->data[6] << 8 | rslh->data[7];
		}
		break;
	default:
		break;
	}

	return 0;
}

static int abis_rsl_rx_cchan(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *rslh = msgb_l2(msg);
	int rc = 0;

	msg->lchan = lchan_lookup(msg->trx, rslh->chan_nr);

	switch (rslh->c.msg_type) {
	case RSL_MT_CHAN_RQD:
		/* MS has requested a channel on the RACH */
		rc = rsl_rx_chan_rqd(msg);
		break;
	case RSL_MT_CCCH_LOAD_IND:
		/* current load on the CCCH */
		rc = rsl_rx_ccch_load(msg);
		break;
	case RSL_MT_DELETE_IND:
		/* CCCH overloaded, IMM_ASSIGN was dropped */
	case RSL_MT_CBCH_LOAD_IND:
		/* current load on the CBCH */
		LOGP(DRSL, LOGL_NOTICE, "Unimplemented Abis RSL TRX message "
			"type 0x%02x\n", rslh->c.msg_type);
		break;
	default:
		LOGP(DRSL, LOGL_NOTICE, "Unknown Abis RSL TRX message type "
			"0x%02x\n", rslh->c.msg_type);
		return -EINVAL;
	}

	return rc;
}

static int rsl_rx_rll_err_ind(struct msgb *msg)
{
	struct abis_rsl_rll_hdr *rllh = msgb_l2(msg);
	u_int8_t *rlm_cause = rllh->data;

	LOGP(DRLL, LOGL_ERROR, "%s ERROR INDICATION cause=%s\n",
		gsm_lchan_name(msg->lchan),
		rsl_rlm_cause_name(rlm_cause[1]));

	rll_indication(msg->lchan, rllh->link_id, BSC_RLLR_IND_ERR_IND);

	if (rlm_cause[1] == RLL_CAUSE_T200_EXPIRED) {
		counter_inc(msg->lchan->ts->trx->bts->network->stats.chan.rll_err);
		return rsl_rf_chan_release(msg->lchan, 1);
	}

	return 0;
}

static void rsl_handle_release(struct gsm_lchan *lchan)
{
	int sapi;
	struct gsm_bts *bts;

	/* maybe we have only brought down one RLL */
	if (lchan->state != LCHAN_S_REL_REQ)
		return;

	for (sapi = 0; sapi < ARRAY_SIZE(lchan->sapis); ++sapi) {
		if (lchan->sapis[sapi] == LCHAN_SAPI_UNUSED)
			continue;
		LOGP(DRSL, LOGL_DEBUG, "%s waiting for SAPI=%d to be released.\n",
		     gsm_lchan_name(lchan), sapi);
		return;
	}



	/* wait a bit to send the RF Channel Release */
	lchan->T3111.cb = t3111_expired;
	lchan->T3111.data = lchan;
	bts = lchan->ts->trx->bts;
	bsc_schedule_timer(&lchan->T3111, bts->network->T3111, 0);
}

/*	ESTABLISH INDICATION, LOCATION AREA UPDATE REQUEST
	0x02, 0x06,
	0x01, 0x20,
	0x02, 0x00,
	0x0b, 0x00, 0x0f, 0x05, 0x08, ... */

static int abis_rsl_rx_rll(struct msgb *msg)
{
	struct abis_rsl_rll_hdr *rllh = msgb_l2(msg);
	int rc = 0;
	char *ts_name;
	u_int8_t sapi = rllh->link_id & 7;

	msg->lchan = lchan_lookup(msg->trx, rllh->chan_nr);
	ts_name = gsm_lchan_name(msg->lchan);
	DEBUGP(DRLL, "%s SAPI=%u ", ts_name, sapi);
	
	switch (rllh->c.msg_type) {
	case RSL_MT_DATA_IND:
		DEBUGPC(DRLL, "DATA INDICATION\n");
		if (msgb_l2len(msg) >
		    sizeof(struct abis_rsl_common_hdr) + sizeof(*rllh) &&
		    rllh->data[0] == RSL_IE_L3_INFO) {
			msg->l3h = &rllh->data[3];
			return gsm0408_rcvmsg(msg, rllh->link_id);
		}
		break;
	case RSL_MT_EST_IND:
		DEBUGPC(DRLL, "ESTABLISH INDICATION\n");
		/* lchan is established, stop T3101 */
		msg->lchan->sapis[rllh->link_id & 0x7] = LCHAN_SAPI_MS;
		bsc_del_timer(&msg->lchan->T3101);
		if (msgb_l2len(msg) >
		    sizeof(struct abis_rsl_common_hdr) + sizeof(*rllh) &&
		    rllh->data[0] == RSL_IE_L3_INFO) {
			msg->l3h = &rllh->data[3];
			return gsm0408_rcvmsg(msg, rllh->link_id);
		}
		break;
	case RSL_MT_EST_CONF:
		DEBUGPC(DRLL, "ESTABLISH CONFIRM\n");
		msg->lchan->sapis[rllh->link_id & 0x7] = LCHAN_SAPI_NET;
		rll_indication(msg->lchan, rllh->link_id,
				  BSC_RLLR_IND_EST_CONF);
		break;
	case RSL_MT_REL_IND:
		/* BTS informs us of having received  DISC from MS */
		DEBUGPC(DRLL, "RELEASE INDICATION\n");
		msg->lchan->sapis[rllh->link_id & 0x7] = LCHAN_SAPI_UNUSED;
		rll_indication(msg->lchan, rllh->link_id,
				  BSC_RLLR_IND_REL_IND);
		rsl_handle_release(msg->lchan);
		rsl_lchan_rll_release(msg->lchan, rllh->link_id);
		break;
	case RSL_MT_REL_CONF:
		/* BTS informs us of having received UA from MS,
		 * in response to DISC that we've sent earlier */
		DEBUGPC(DRLL, "RELEASE CONFIRMATION\n");
		msg->lchan->sapis[rllh->link_id & 0x7] = LCHAN_SAPI_UNUSED;
		rsl_handle_release(msg->lchan);
		rsl_lchan_rll_release(msg->lchan, rllh->link_id);
		break;
	case RSL_MT_ERROR_IND:
		rc = rsl_rx_rll_err_ind(msg);
		break;
	case RSL_MT_UNIT_DATA_IND:
		LOGP(DRLL, LOGL_NOTICE, "unimplemented Abis RLL message "
			"type 0x%02x\n", rllh->c.msg_type);
		break;
	default:
		LOGP(DRLL, LOGL_NOTICE, "unknown Abis RLL message "
			"type 0x%02x\n", rllh->c.msg_type);
	}
	return rc;
}

static u_int8_t ipa_smod_s_for_lchan(struct gsm_lchan *lchan)
{
	switch (lchan->tch_mode) {
	case GSM48_CMODE_SPEECH_V1:
		switch (lchan->type) {
		case GSM_LCHAN_TCH_F:
			return 0x00;
		case GSM_LCHAN_TCH_H:
			return 0x03;
		default:
			break;
		}
	case GSM48_CMODE_SPEECH_EFR:
		switch (lchan->type) {
		case GSM_LCHAN_TCH_F:
			return 0x01;
		/* there's no half-rate EFR */
		default:
			break;
		}
	case GSM48_CMODE_SPEECH_AMR:
		switch (lchan->type) {
		case GSM_LCHAN_TCH_F:
			return 0x02;
		case GSM_LCHAN_TCH_H:
			return 0x05;
		default:
			break;
		}
	default:
		break;
	}
	LOGP(DRSL, LOGL_ERROR, "Cannot determine ip.access speech mode for "
		"tch_mode == 0x%02x\n", lchan->tch_mode);
	return 0;
}

static u_int8_t ipa_rtp_pt_for_lchan(struct gsm_lchan *lchan)
{
	struct gsm_network *net = lchan->ts->trx->bts->network;

	/* allow to hardcode the rtp payload */
	if (net->hardcoded_rtp_payload != 0)
		return net->hardcoded_rtp_payload;

	switch (lchan->tch_mode) {
	case GSM48_CMODE_SPEECH_V1:
		switch (lchan->type) {
		case GSM_LCHAN_TCH_F:
			return RTP_PT_GSM_FULL;
		case GSM_LCHAN_TCH_H:
			return RTP_PT_GSM_HALF;
		default:
			break;
		}
	case GSM48_CMODE_SPEECH_EFR:
		switch (lchan->type) {
		case GSM_LCHAN_TCH_F:
			return RTP_PT_GSM_EFR;
		/* there's no half-rate EFR */
		default:
			break;
		}
	case GSM48_CMODE_SPEECH_AMR:
		switch (lchan->type) {
		case GSM_LCHAN_TCH_F:
			return RTP_PT_AMR_FULL;
		case GSM_LCHAN_TCH_H:
			return RTP_PT_AMR_HALF;
		default:
			break;
		}
	default:
		break;
	}
	LOGP(DRSL, LOGL_ERROR, "Cannot determine ip.access rtp payload type for "
		"tch_mode == 0x%02x\n & lchan_type == %d",
		lchan->tch_mode, lchan->type);
	return 0;
}

/* ip.access specific RSL extensions */
static void ipac_parse_rtp(struct gsm_lchan *lchan, struct tlv_parsed *tv)
{
	struct in_addr ip;
	u_int16_t port, conn_id;

	if (TLVP_PRESENT(tv, RSL_IE_IPAC_LOCAL_IP)) {
		ip.s_addr = *((u_int32_t *) TLVP_VAL(tv, RSL_IE_IPAC_LOCAL_IP));
		DEBUGPC(DRSL, "LOCAL_IP=%s ", inet_ntoa(ip));
		lchan->abis_ip.bound_ip = ntohl(ip.s_addr);
	}

	if (TLVP_PRESENT(tv, RSL_IE_IPAC_LOCAL_PORT)) {
		port = *((u_int16_t *) TLVP_VAL(tv, RSL_IE_IPAC_LOCAL_PORT));
		port = ntohs(port);
		DEBUGPC(DRSL, "LOCAL_PORT=%u ", port);
		lchan->abis_ip.bound_port = port;
	}

	if (TLVP_PRESENT(tv, RSL_IE_IPAC_CONN_ID)) {
		conn_id = *((u_int16_t *) TLVP_VAL(tv, RSL_IE_IPAC_CONN_ID));
		conn_id = ntohs(conn_id);
		DEBUGPC(DRSL, "CON_ID=%u ", conn_id);
		lchan->abis_ip.conn_id = conn_id;
	}

	if (TLVP_PRESENT(tv, RSL_IE_IPAC_RTP_PAYLOAD2)) {
		lchan->abis_ip.rtp_payload2 =
				*TLVP_VAL(tv, RSL_IE_IPAC_RTP_PAYLOAD2);
		DEBUGPC(DRSL, "RTP_PAYLOAD2=0x%02x ",
			lchan->abis_ip.rtp_payload2);
	}

	if (TLVP_PRESENT(tv, RSL_IE_IPAC_SPEECH_MODE)) {
		lchan->abis_ip.speech_mode =
				*TLVP_VAL(tv, RSL_IE_IPAC_SPEECH_MODE);
		DEBUGPC(DRSL, "speech_mode=0x%02x ",
			lchan->abis_ip.speech_mode);
	}

	if (TLVP_PRESENT(tv, RSL_IE_IPAC_REMOTE_IP)) {
		ip.s_addr = *((u_int32_t *) TLVP_VAL(tv, RSL_IE_IPAC_REMOTE_IP));
		DEBUGPC(DRSL, "REMOTE_IP=%s ", inet_ntoa(ip));
		lchan->abis_ip.connect_ip = ntohl(ip.s_addr);
	}

	if (TLVP_PRESENT(tv, RSL_IE_IPAC_REMOTE_PORT)) {
		port = *((u_int16_t *) TLVP_VAL(tv, RSL_IE_IPAC_REMOTE_PORT));
		port = ntohs(port);
		DEBUGPC(DRSL, "REMOTE_PORT=%u ", port);
		lchan->abis_ip.connect_port = port;
	}
}

int rsl_ipacc_crcx(struct gsm_lchan *lchan)
{
	struct msgb *msg = rsl_msgb_alloc();
	struct abis_rsl_dchan_hdr *dh;

	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_IPAC_CRCX);
	dh->c.msg_discr = ABIS_RSL_MDISC_IPACCESS;
	dh->chan_nr = lchan2chan_nr(lchan);

	/* 0x1- == receive-only, 0x-1 == EFR codec */
	lchan->abis_ip.speech_mode = 0x10 | ipa_smod_s_for_lchan(lchan);
	lchan->abis_ip.rtp_payload = ipa_rtp_pt_for_lchan(lchan);
	msgb_tv_put(msg, RSL_IE_IPAC_SPEECH_MODE, lchan->abis_ip.speech_mode);
	msgb_tv_put(msg, RSL_IE_IPAC_RTP_PAYLOAD, lchan->abis_ip.rtp_payload);

	DEBUGP(DRSL, "%s IPAC_BIND speech_mode=0x%02x RTP_PAYLOAD=%d\n",
		gsm_lchan_name(lchan), lchan->abis_ip.speech_mode,
		lchan->abis_ip.rtp_payload);

	msg->trx = lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}

int rsl_ipacc_mdcx(struct gsm_lchan *lchan, u_int32_t ip, u_int16_t port,
		   u_int8_t rtp_payload2)
{
	struct msgb *msg = rsl_msgb_alloc();
	struct abis_rsl_dchan_hdr *dh;
	u_int32_t *att_ip;
	struct in_addr ia;

	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, RSL_MT_IPAC_MDCX);
	dh->c.msg_discr = ABIS_RSL_MDISC_IPACCESS;
	dh->chan_nr = lchan2chan_nr(lchan);

	/* we need to store these now as MDCX_ACK does not return them :( */
	lchan->abis_ip.rtp_payload2 = rtp_payload2;
	lchan->abis_ip.connect_port = port;
	lchan->abis_ip.connect_ip = ip;

	/* 0x0- == both directions, 0x-1 == EFR codec */
	lchan->abis_ip.speech_mode = 0x00 | ipa_smod_s_for_lchan(lchan);
	lchan->abis_ip.rtp_payload = ipa_rtp_pt_for_lchan(lchan);

	ia.s_addr = htonl(ip);
	DEBUGP(DRSL, "%s IPAC_MDCX IP=%s PORT=%d RTP_PAYLOAD=%d RTP_PAYLOAD2=%d "
		"CONN_ID=%d speech_mode=0x%02x\n", gsm_lchan_name(lchan),
		inet_ntoa(ia), port, lchan->abis_ip.rtp_payload, rtp_payload2,
		lchan->abis_ip.conn_id, lchan->abis_ip.speech_mode);

	msgb_tv16_put(msg, RSL_IE_IPAC_CONN_ID, lchan->abis_ip.conn_id);
	msgb_v_put(msg, RSL_IE_IPAC_REMOTE_IP);
	att_ip = (u_int32_t *) msgb_put(msg, sizeof(ip));
	*att_ip = ia.s_addr;
	msgb_tv16_put(msg, RSL_IE_IPAC_REMOTE_PORT, port);
	msgb_tv_put(msg, RSL_IE_IPAC_SPEECH_MODE, lchan->abis_ip.speech_mode);
	msgb_tv_put(msg, RSL_IE_IPAC_RTP_PAYLOAD, lchan->abis_ip.rtp_payload);
	if (rtp_payload2)
		msgb_tv_put(msg, RSL_IE_IPAC_RTP_PAYLOAD2, rtp_payload2);
	
	msg->trx = lchan->ts->trx;

	return abis_rsl_sendmsg(msg);
}

/* tell BTS to connect RTP stream to our local RTP socket */
int rsl_ipacc_mdcx_to_rtpsock(struct gsm_lchan *lchan)
{
	struct rtp_socket *rs = lchan->abis_ip.rtp_socket;
	int rc;

	rc = rsl_ipacc_mdcx(lchan, ntohl(rs->rtp.sin_local.sin_addr.s_addr),
				ntohs(rs->rtp.sin_local.sin_port),
			/* FIXME: use RTP payload of bound socket, not BTS*/
				lchan->abis_ip.rtp_payload2);

	return rc;
}

int rsl_ipacc_pdch_activate(struct gsm_bts_trx_ts *ts, int act)
{
	struct msgb *msg = rsl_msgb_alloc();
	struct abis_rsl_dchan_hdr *dh;
	u_int8_t msg_type;

	if (act)
		msg_type = RSL_MT_IPAC_PDCH_ACT;
	else
		msg_type = RSL_MT_IPAC_PDCH_DEACT;

	dh = (struct abis_rsl_dchan_hdr *) msgb_put(msg, sizeof(*dh));
	init_dchan_hdr(dh, msg_type);
	dh->c.msg_discr = ABIS_RSL_MDISC_DED_CHAN;
	dh->chan_nr = ts2chan_nr(ts, 0);

	DEBUGP(DRSL, "%s IPAC_PDCH_%sACT\n", gsm_ts_name(ts),
		act ? "" : "DE");

	msg->trx = ts->trx;

	return abis_rsl_sendmsg(msg);
}

static int abis_rsl_rx_ipacc_crcx_ack(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *dh = msgb_l2(msg);
	struct tlv_parsed tv;
	struct gsm_lchan *lchan = msg->lchan;

	/* the BTS has acknowledged a local bind, it now tells us the IP
	* address and port number to which it has bound the given logical
	* channel */

	rsl_tlv_parse(&tv, dh->data, msgb_l2len(msg)-sizeof(*dh));
	if (!TLVP_PRESENT(&tv, RSL_IE_IPAC_LOCAL_PORT) ||
	    !TLVP_PRESENT(&tv, RSL_IE_IPAC_LOCAL_IP) ||
	    !TLVP_PRESENT(&tv, RSL_IE_IPAC_CONN_ID)) {
		LOGP(DRSL, LOGL_NOTICE, "mandatory IE missing");
		return -EINVAL;
	}

	ipac_parse_rtp(lchan, &tv);

	dispatch_signal(SS_ABISIP, S_ABISIP_CRCX_ACK, msg->lchan);

	return 0;
}

static int abis_rsl_rx_ipacc_mdcx_ack(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *dh = msgb_l2(msg);
	struct tlv_parsed tv;
	struct gsm_lchan *lchan = msg->lchan;

	/* the BTS has acknowledged a remote connect request and
	 * it now tells us the IP address and port number to which it has
	 * connected the given logical channel */

	rsl_tlv_parse(&tv, dh->data, msgb_l2len(msg)-sizeof(*dh));
	ipac_parse_rtp(lchan, &tv);
	dispatch_signal(SS_ABISIP, S_ABISIP_MDCX_ACK, msg->lchan);

	return 0;
}

static int abis_rsl_rx_ipacc_dlcx_ind(struct msgb *msg)
{
	struct abis_rsl_dchan_hdr *dh = msgb_l2(msg);
	struct tlv_parsed tv;

	rsl_tlv_parse(&tv, dh->data, msgb_l2len(msg)-sizeof(*dh));

	if (TLVP_PRESENT(&tv, RSL_IE_CAUSE))
		print_rsl_cause(LOGL_DEBUG, TLVP_VAL(&tv, RSL_IE_CAUSE),
				TLVP_LEN(&tv, RSL_IE_CAUSE));

	dispatch_signal(SS_ABISIP, S_ABISIP_DLCX_IND, msg->lchan);

	return 0;
}

static int abis_rsl_rx_ipacc(struct msgb *msg)
{
	struct abis_rsl_rll_hdr *rllh = msgb_l2(msg);
	char *ts_name;
	int rc = 0;

	msg->lchan = lchan_lookup(msg->trx, rllh->chan_nr);
	ts_name = gsm_lchan_name(msg->lchan);
	
	switch (rllh->c.msg_type) {
	case RSL_MT_IPAC_CRCX_ACK:
		DEBUGP(DRSL, "%s IPAC_CRCX_ACK ", ts_name);
		rc = abis_rsl_rx_ipacc_crcx_ack(msg);
		break;
	case RSL_MT_IPAC_CRCX_NACK:
		/* somehow the BTS was unable to bind the lchan to its local
		 * port?!? */
		LOGP(DRSL, LOGL_ERROR, "%s IPAC_CRCX_NACK\n", ts_name);
		break;
	case RSL_MT_IPAC_MDCX_ACK:
		/* the BTS tells us that a connect operation was successful */
		DEBUGP(DRSL, "%s IPAC_MDCX_ACK ", ts_name);
		rc = abis_rsl_rx_ipacc_mdcx_ack(msg);
		break;
	case RSL_MT_IPAC_MDCX_NACK:
		/* somehow the BTS was unable to connect the lchan to a remote
		 * port */
		LOGP(DRSL, LOGL_ERROR, "%s IPAC_MDCX_NACK\n", ts_name);
		break;
	case RSL_MT_IPAC_DLCX_IND:
		DEBUGP(DRSL, "%s IPAC_DLCX_IND ", ts_name);
		rc = abis_rsl_rx_ipacc_dlcx_ind(msg);
		break;
	default:
		LOGP(DRSL, LOGL_NOTICE, "Unknown ip.access msg_type 0x%02x\n",
			rllh->c.msg_type);
		break;
	}
	DEBUGPC(DRSL, "\n");

	return rc;
}


/* Entry-point where L2 RSL from BTS enters */
int abis_rsl_rcvmsg(struct msgb *msg)
{
	struct abis_rsl_common_hdr *rslh;
	int rc = 0;

	if (!msg) {
		DEBUGP(DRSL, "Empty RSL msg?..\n");
		return -1;
	}

	if (msgb_l2len(msg) < sizeof(*rslh)) {
		DEBUGP(DRSL, "Truncated RSL message with l2len: %u\n", msgb_l2len(msg));
		return -1;
	}

	rslh = msgb_l2(msg);

	switch (rslh->msg_discr & 0xfe) {
	case ABIS_RSL_MDISC_RLL:
		rc = abis_rsl_rx_rll(msg);
		break;
	case ABIS_RSL_MDISC_DED_CHAN:
		rc = abis_rsl_rx_dchan(msg);
		break;
	case ABIS_RSL_MDISC_COM_CHAN:
		rc = abis_rsl_rx_cchan(msg);
		break;
	case ABIS_RSL_MDISC_TRX:
		rc = abis_rsl_rx_trx(msg);
		break;
	case ABIS_RSL_MDISC_LOC:
		LOGP(DRSL, LOGL_NOTICE, "unimplemented RSL msg disc 0x%02x\n",
			rslh->msg_discr);
		break;
	case ABIS_RSL_MDISC_IPACCESS:
		rc = abis_rsl_rx_ipacc(msg);
		break;
	default:
		LOGP(DRSL, LOGL_NOTICE, "unknown RSL message discriminator "
			"0x%02x\n", rslh->msg_discr);
		return -EINVAL;
	}
	msgb_free(msg);
	return rc;
}

/* From Table 10.5.33 of GSM 04.08 */
int rsl_number_of_paging_subchannels(struct gsm_bts *bts)
{
	if (bts->si_common.chan_desc.ccch_conf == RSL_BCCH_CCCH_CONF_1_C) {
		return MAX(1, (3 - bts->si_common.chan_desc.bs_ag_blks_res))
			* (bts->si_common.chan_desc.bs_pa_mfrms + 2);
	} else {
		return (9 - bts->si_common.chan_desc.bs_ag_blks_res)
			* (bts->si_common.chan_desc.bs_pa_mfrms + 2);
	}
}

int rsl_sms_cb_command(struct gsm_bts *bts, uint8_t chan_number,
		       uint8_t cb_command, const uint8_t *data, int len)
{
	struct abis_rsl_dchan_hdr *dh;
	struct msgb *cb_cmd;

	cb_cmd = rsl_msgb_alloc();
	if (!cb_cmd)
		return -1;

	dh = (struct abis_rsl_dchan_hdr *) msgb_put(cb_cmd, sizeof*dh);
	init_dchan_hdr(dh, RSL_MT_SMS_BC_CMD);
	dh->chan_nr = RSL_CHAN_SDCCH4_ACCH; /* TODO: check the chan config */

	msgb_tv_put(cb_cmd, RSL_IE_CB_CMD_TYPE, cb_command);
	msgb_tlv_put(cb_cmd, RSL_IE_SMSCB_MSG, len, data);

	cb_cmd->trx = bts->c0;

	return abis_rsl_sendmsg(cb_cmd);
}