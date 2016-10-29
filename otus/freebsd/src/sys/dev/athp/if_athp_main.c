/*-
 * Copyright (c) 2015 Adrian Chadd <adrian@FreeBSD.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Playground for QCA988x chipsets.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_wlan.h"

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/firmware.h>
#include <sys/module.h>
#include <sys/taskqueue.h>
#include <sys/condvar.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_regdomain.h>
#include <net80211/ieee80211_radiotap.h>
#include <net80211/ieee80211_ratectl.h>
#include <net80211/ieee80211_input.h>
#ifdef	IEEE80211_SUPPORT_SUPERG
#include <net80211/ieee80211_superg.h>
#endif

#include "hal/linux_compat.h"
#include "hal/targaddrs.h"
#include "hal/htc.h"
#include "hal/wmi.h"
#include "hal/hw.h"

#include "if_athp_debug.h"
#include "if_athp_regio.h"
#include "if_athp_stats.h"
#include "if_athp_wmi.h"
#include "if_athp_desc.h"
#include "if_athp_core.h"
#include "if_athp_htc.h"
#include "if_athp_var.h"
#include "if_athp_hif.h"
#include "if_athp_bmi.h"
#include "if_athp_mac.h"
#include "if_athp_mac2.h"

#include "if_athp_main.h"
#include "if_athp_taskq.h"

MALLOC_DEFINE(M_ATHPDEV, "athpdev", "athp memory");

/*
 * These are the net80211 facing implementation pieces.
 */

/*
 * 2GHz channel list for ath10k.
 */
static uint8_t chan_list_2ghz[] =
    { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
static uint8_t chan_list_5ghz[] =
    { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104,
      108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149,
      153, 157, 161, 165 };

static int
athp_tx_tag_crypto(struct ath10k *ar, struct ieee80211_node *ni,
    struct mbuf *m0)
{
	struct ieee80211_frame *wh;
	struct ieee80211_key *k;
	int iswep;

	wh = mtod(m0, struct ieee80211_frame *);
	iswep = wh->i_fc[1] & IEEE80211_FC1_PROTECTED;

	if (iswep) {
		/*
		 * Construct the 802.11 header+trailer for an encrypted
		 * frame. The only reason this can fail is because of an
		 * unknown or unsupported cipher/key type.
		 */
		k = ieee80211_crypto_encap(ni, m0);
		if (k == NULL) {
			/*
			 * This can happen when the key is yanked after the
			 * frame was queued.  Just discard the frame; the
			 * 802.11 layer counts failures and provides
			 * debugging/diagnostics.
			 */
			return (0);
		}
	}

	return (1);
}
/*
 * Raw frame transmission - this is "always" 802.11.
 *
 * Free the mbuf if we fail, but don't deref the node.
 * That's the callers job.
 *
 * XXX TODO: use ieee80211_free_mbuf() so fragment lists get freed.
 */
static int
athp_raw_xmit(struct ieee80211_node *ni, struct mbuf *m0,
    const struct ieee80211_bpf_params *params)
{
	struct ieee80211com *ic = ni->ni_ic;
	struct ieee80211vap *vap = ni->ni_vap;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vap);
	struct ath10k *ar = ic->ic_softc;
	struct athp_buf *pbuf;
	struct ath10k_skb_cb *cb;
	struct ieee80211_frame *wh;
	struct mbuf *m = NULL;
	int is_wep, is_qos;

	wh = mtod(m0, struct ieee80211_frame *);
	is_wep = !! wh->i_fc[1] & IEEE80211_FC1_PROTECTED;
	is_qos = IEEE80211_IS_QOS(wh);

	ath10k_dbg(ar, ATH10K_DBG_XMIT,
	    "%s: called; ni=%p, m=%p, len=%d, fc0=0x%x, fc1=0x%x, ni.macaddr=%6D, is_wep=%d, is_qos=%d\n",
	    __func__,
	    ni, m0, m0->m_pkthdr.len, wh->i_fc[0], wh->i_fc[1], ni->ni_macaddr, ":", is_wep, is_qos);

	ATHP_CONF_LOCK(ar);

	/* XXX station mode hacks - don't xmit until we plumb up a BSS context */
	if (vap->iv_opmode == IEEE80211_M_STA) {
		if (arvif->is_stabss_setup == 0) {
			ATHP_CONF_UNLOCK(ar);
			ath10k_warn(ar,
			    "%s: stabss not setup; don't xmit\n",
			    __func__);
			m_freem(m0);
			return (ENXIO);
		}
	}
	ATHP_CONF_UNLOCK(ar);

	if (! athp_tx_tag_crypto(ar, ni, m0)) {
		ar->sc_stats.xmit_fail_crypto_encap++;
		m_freem(m0);
		return (ENXIO);
	}

	/*
	 * For now, the ath10k linux side doesn't handle multi-segment
	 * mbufs.  The firmware/hardware supports it, but the tx path
	 * assumes everything is a single linear mbuf.
	 *
	 * So, try to defrag.  If we fail, return ENOBUFS.
	 */
	m = m_defrag(m0, M_NOWAIT);
	if (m == NULL) {
		ar->sc_stats.xmit_fail_mbuf_defrag++;
		ath10k_err(ar, "%s: failed to m_defrag\n", __func__);
		m_freem(m0);
		return (ENOBUFS);
	}
	m0 = NULL;

	/* Allocate a TX mbuf */
	pbuf = athp_getbuf_tx(ar, &ar->buf_tx);
	if (pbuf == NULL) {
		ar->sc_stats.xmit_fail_get_pbuf++;
		ath10k_err(ar, "%s: failed to get TX pbuf\n", __func__);
		m_freem(m);
		return (ENOBUFS);
	}

	/* Put the mbuf into the given pbuf */
	athp_buf_give_mbuf(ar, &ar->buf_tx, pbuf, m);

	/* The node reference is ours to free upon xmit, so .. */
	cb = ATH10K_SKB_CB(pbuf);
	cb->ni = ni;

	if (ieee80211_radiotap_active_vap(vap)) {
		ar->sc_txtapu.th.wt_flags = 0;
		if (is_wep)
			ar->sc_txtapu.th.wt_flags |= IEEE80211_RADIOTAP_F_WEP;
		ieee80211_radiotap_tx(vap, m);
	}

	/* Transmit */
	ath10k_tx(ar, ni, pbuf);

	return (0);
}

static void
athp_scan_curchan(struct ieee80211_scan_state *ss, unsigned long maxdwell)
{
}

static void
athp_scan_mindwell(struct ieee80211_scan_state *ss)
{
}

static void
athp_scan_start(struct ieee80211com *ic)
{
	struct ath10k *ar = ic->ic_softc;
	struct ieee80211vap *vap;
	int ret;

	/* XXX TODO: yes, scan should just freaking pass in a vap */
	vap = TAILQ_FIRST(&ic->ic_vaps);
	if (vap == NULL)
		return;

	/*
	 * For now - active scan, hard-coded 200ms active/passive dwell times.
	 */
	ret = ath10k_hw_scan(ar, vap, 200, 200);

	if (ret != 0) {
		ath10k_err(ar, "%s: ath10k_hw_scan failed; ret=%d\n",
		    __func__, ret);
	}
}

static void
athp_scan_end(struct ieee80211com *ic)
{
}

static void
athp_set_channel(struct ieee80211com *ic)
{
	struct ath10k *ar = ic->ic_softc;
	struct ath10k_vif *arvif;
	struct ieee80211vap *vap;
	int ret;

	/* If we have a monitor vap then set that channel */
	ATHP_CONF_LOCK(ar);
	if (ar->monitor_arvif == NULL) {
		goto finish;
	}

	/* XXX TODO: maybe we don't need to do this when in RUN state? */

	arvif = ar->monitor_arvif;
	vap = (void *) arvif;
	ath10k_vif_bring_down(vap);
	ret = ath10k_vif_bring_up(vap, ic->ic_curchan);
	if (ret != 0) {
		ath10k_err(ar, "%s: error calling vif_up; ret=%d\n",
		    __func__,
		    ret);
	}

finish:
	ATHP_CONF_UNLOCK(ar);
	return;
}

/*
 * data transmission.  For now this is 802.11, but once we get this
 * driver up it could just as easy be 802.3 frames so we can bypass
 * almost /all/ of the net80211 side handling.
 *
 * Unlike the raw path - if we fail, we don't free the buffer.
 *
 * XXX TODO: use ieee80211_free_mbuf() so fragment lists get freed.
 *
 * XXX TODO: handle fragmented frame list
 *
 * XXX TODO: we shouldn't transmit a frame if there's no peer setup
 * for it!
 */
static int
athp_transmit(struct ieee80211com *ic, struct mbuf *m0)
{
	struct ath10k *ar = ic->ic_softc;
	struct ieee80211vap *vap;
	struct ath10k_vif *arvif;
	struct athp_buf *pbuf;
	struct ath10k_skb_cb *cb;
	struct ieee80211_node *ni;
	struct mbuf *m = NULL;
	struct ieee80211_frame *wh;
	int is_wep, is_qos;

	wh = mtod(m0, struct ieee80211_frame *);
	is_wep = !! wh->i_fc[1] & IEEE80211_FC1_PROTECTED;
	is_qos = !! IEEE80211_IS_QOS(wh);

	ni = (struct ieee80211_node *) m0->m_pkthdr.rcvif;
	ath10k_dbg(ar, ATH10K_DBG_XMIT,
	    "%s: called; ni=%p, m=%p; ni.macaddr=%6D; iswep=%d, isqos=%d\n",
	    __func__, ni, m0, ni->ni_macaddr, ":", is_wep,
	    is_qos);

	vap = ni->ni_vap;
	arvif = ath10k_vif_to_arvif(vap);

	ATHP_CONF_LOCK(ar);
	/* XXX station mode hacks - don't xmit until we plumb up a BSS context */
	if (vap->iv_opmode == IEEE80211_M_STA) {
		if (arvif->is_stabss_setup == 0) {
			ATHP_CONF_UNLOCK(ar);
			ath10k_warn(ar, "%s: stabss not setup; don't xmit\n", __func__);
			return (ENXIO);
		}
	}
	ATHP_CONF_UNLOCK(ar);

	if (! athp_tx_tag_crypto(ar, ni, m0)) {
		ar->sc_stats.xmit_fail_crypto_encap++;
		return (ENXIO);
	}

	/*
	 * For now, the ath10k linux side doesn't handle multi-segment
	 * mbufs.  The firmware/hardware supports it, but the tx path
	 * assumes everything is a single linear mbuf.
	 *
	 * So, try to defrag.  If we fail, return ENOBUFS.
	 */
	m = m_defrag(m0, M_NOWAIT);
	if (m == NULL) {
		ar->sc_stats.xmit_fail_mbuf_defrag++;
		ath10k_err(ar, "%s: failed to m_defrag\n", __func__);
		return (ENOBUFS);
	}
	m0 = NULL;

	/* Allocate a TX mbuf */
	pbuf = athp_getbuf_tx(ar, &ar->buf_tx);
	if (pbuf == NULL) {
		ar->sc_stats.xmit_fail_get_pbuf++;
		ath10k_err(ar, "%s: failed to get TX pbuf\n", __func__);
		return (ENOBUFS);
	}

	/* Put the mbuf into the given pbuf */
	athp_buf_give_mbuf(ar, &ar->buf_tx, pbuf, m);

	m->m_pkthdr.rcvif = NULL;

	/* The node reference is ours to free upon xmit, so .. */
	cb = ATH10K_SKB_CB(pbuf);
	cb->ni = ni;

	if (ieee80211_radiotap_active_vap(vap)) {
		ar->sc_txtapu.th.wt_flags = 0;
		if (is_wep)
			ar->sc_txtapu.th.wt_flags |= IEEE80211_RADIOTAP_F_WEP;
		ieee80211_radiotap_tx(vap, m);
	}

	/* Transmit */
	ath10k_tx(ar, ni, pbuf);

	return (0);
}

/*
 * Handle initial notifications about starting the interface here.
 */
static void
athp_parent(struct ieee80211com *ic)
{
	struct ath10k *ar = ic->ic_softc;

	/* XXX TODO: add conf lock */
	if (ic->ic_nrunning > 0) {
		/* Track if we're running already */
		if (ar->sc_isrunning == 0) {
			ieee80211_start_all(ic);
			ar->sc_isrunning = 1;
		}
	}

	if (ic->ic_nrunning == 0) {
		ar->sc_isrunning = 0;
	}
}

/*
 * TODO:
 *
 * + if we fail assoc and move to another bssid, do we go via INIT
 *   first?  Ie, how do we delete the existing bssid?
 */
static int
athp_vap_newstate(struct ieee80211vap *vap, enum ieee80211_state nstate, int arg)
{
	struct ath10k_vif *vif = ath10k_vif_to_arvif(vap);
	struct ieee80211com *ic = vap->iv_ic;
	struct ath10k *ar = ic->ic_softc;
	enum ieee80211_state ostate = vap->iv_state;
	int ret;
	int error = 0;
	struct ieee80211_node *bss_ni;

	ath10k_warn(ar, "%s: %s -> %s\n", __func__,
	    ieee80211_state_name[ostate],
	    ieee80211_state_name[nstate]);

	/* Grab bss node ref before unlocking */
	bss_ni = ieee80211_ref_node(vap->iv_bss);

	IEEE80211_UNLOCK(ic);

	switch (nstate) {
	case IEEE80211_S_RUN:
		/* RUN->RUN; ignore for now */
		if (ostate == IEEE80211_S_RUN)
			break;

		if (vap->iv_opmode == IEEE80211_M_STA) {
			ATHP_CONF_LOCK(ar);
			ret = ath10k_vif_restart(ar, vap, bss_ni, ic->ic_curchan);
			if (ret != 0) {
				ATHP_CONF_UNLOCK(ar);
				ath10k_err(ar,
				    "%s: ath10k_vdev_start failed; ret=%d\n",
				    __func__, ret);
				break;
			}
			ath10k_bss_update(ar, vap, bss_ni, 1, 1);
			ATHP_CONF_UNLOCK(ar);
		}

		/* For now, only start vdev on INIT->RUN */
		/* This should be ok for monitor, but not for station */
		if (vap->iv_opmode == IEEE80211_M_MONITOR) {
			if (ostate == IEEE80211_S_INIT) {
				ATHP_CONF_LOCK(ar);
				ret = ath10k_vif_bring_up(vap, ic->ic_curchan);
				ATHP_CONF_UNLOCK(ar);
				if (ret != 0) {
					ath10k_err(ar,
					    "%s: ath10k_vdev_start failed; ret=%d\n",
					    __func__, ret);
					break;
				}
			}
		}
		break;

	/* Transitioning to SCAN from RUN - is fine, you don't need to delete anything */
	case IEEE80211_S_SCAN:
		break;

	case IEEE80211_S_INIT:
		ATHP_CONF_LOCK(ar);
		if (vap->iv_opmode == IEEE80211_M_MONITOR) {
			/* Monitor mode - explicit down */
			ath10k_vif_bring_down(vap);
		}
		if (vap->iv_opmode == IEEE80211_M_STA) {

			/* Wait for xmit to finish before continuing */
			ath10k_tx_flush(ar, vap, 0, 1);

			/* This brings the interface down; delete the peer */
			if (vif->is_stabss_setup == 1) {
				ath10k_bss_update(ar, vap, bss_ni, 0, 0);
			}
		}
		ATHP_CONF_UNLOCK(ar);
		break;

	case IEEE80211_S_AUTH:
		/*
		 * When going SCAN->AUTH, we need to plumb up the initial
		 * BSS before we can send frames to it.
		 *
		 * For ASSOC, we do the same.
		 *
		 * Then for RUN we update the BSS configuration
		 * with whatever new information we've found.
		 */
		ATHP_CONF_LOCK(ar);
		ret = ath10k_vif_restart(ar, vap, bss_ni, ic->ic_curchan);
		if (ret != 0) {
			ATHP_CONF_UNLOCK(ar);
			ath10k_err(ar,
			    "%s: ath10k_vdev_start failed; ret=%d\n",
			    __func__, ret);
			break;
		}
		ath10k_bss_update(ar, vap, bss_ni, 1, 0);
		ATHP_CONF_UNLOCK(ar);
		break;
	case IEEE80211_S_ASSOC:
		/* Assuming we already went through AUTH */
#if 0
		ATHP_CONF_LOCK(ar);
		/* Update the association state */
		ath10k_bss_update(ar, vap, bss_ni, 1, 0);
		ATHP_CONF_UNLOCK(ar);
#endif
		break;
	default:
		ath10k_warn(ar, "%s: state %s not handled\n",
		    __func__,
		    ieee80211_state_name[nstate]);
		break;
	}
	IEEE80211_LOCK(ic);

	ieee80211_free_node(bss_ni);

	error = vif->av_newstate(vap, nstate, arg);
	return (error);
}

/*
 * Keys aren't allocated in slots; we should have enough
 * slots based on the total number of peers available.
 *
 * Groupwise keys just use the key index that has been
 * provided - the firmware handles this per-vdev for us.
 *
 * For now, always allocate keyidx 0 for first pairwise
 * key.  Later on we could attempt to say, alternate the
 * hardware key indexes as appropriate.  I'm not yet
 * sure what's supposed to be driving the default
 * transmit key and such.
 *
 * This is very, very focused on STA mode right now - I'm
 * not sure what the hostap side of group versus unicast
 * key will look like.  I'll worry about that next.
 */
static int
athp_key_alloc(struct ieee80211vap *vap, struct ieee80211_key *k,
    ieee80211_keyix *keyix, ieee80211_keyix *rxkeyix)
{
	struct ath10k *ar = vap->iv_ic->ic_softc;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vap);

	/*
	 * This is a "bit" racy.  It's just a check to make sure
	 * we don't get called during the vap free/destroy path.
	 *
	 * Since we don't hold the conf lock for the whole
	 * duration of this function (XXX can we? Not likely
	 * whilst the net80211 com/node lock is held) we can't
	 * guarantee it's non-racy.
	 *
	 * It "should" be okay though.  Should.
	 */
	ATHP_CONF_LOCK(ar);
	if (! arvif->is_setup) {
		ATHP_CONF_UNLOCK(ar);
		return (1);
	}
	ATHP_CONF_UNLOCK(ar);

	ath10k_dbg(ar, ATH10K_DBG_KEYCACHE,
	    "%s: k=%p, keyix=%d; mac=%6D\n",
	    __func__, k, k->wk_keyix, k->wk_macaddr, ":");

	if (!(&vap->iv_nw_keys[0] <= k &&
	     k < &vap->iv_nw_keys[IEEE80211_WEP_NKID])) {
		ath10k_dbg(ar, ATH10K_DBG_KEYCACHE,
		    "%s: Pairwise key allocation\n", __func__);
		if (k->wk_flags & IEEE80211_KEY_GROUP)
			return (0);
		*keyix = ATHP_PAIRWISE_KEY_IDX;
	} else {
		*keyix = k - vap->iv_nw_keys;
	}
	*rxkeyix = *keyix;

	/*
	 * Management frames require IV.  Not yet sure about TKIP MIC.
	 * Other frames don't require IV/MIC.
	 *
	 * To be clear, ath10k does this:
	 *
	 * CCMP - GENERATE_IV_MGMT
	 * TKIP - nothing (ie, no MIC, etc)
	 * raw mode - always generate IVs
	 *
	 * XXX of course, we should really check this assumption
	 * XXX of course, we should finish configuring keys as appropriate,
	 *     rather than the below.
	 */
	if (! arvif->nohwcrypt) {
		k->wk_flags |= IEEE80211_KEY_NOIV;
		k->wk_flags |= IEEE80211_KEY_NOMIC;
	}

	return (1);
}

static void
athp_key_update_cb(struct ath10k *ar, struct athp_taskq_entry *e, int flush)
{
	struct ath10k_vif *arvif;
	struct athp_key_update *ku;
	int ret;

	ku = athp_taskq_entry_to_ptr(e);

	/* Yes, it's badly named .. */
	if (flush == 0)
		return;

	arvif = ath10k_vif_to_arvif(ku->vap);

	ATHP_CONF_LOCK(ar);
	ret = ath10k_install_key(arvif, &ku->k, ku->wmi_add, ku->wmi_macaddr,
	    ku->wmi_flags);

	ath10k_dbg(ar, ATH10K_DBG_KEYCACHE,
	    "%s: keyix=%d, wmi_add=%d, flags=0x%08x, mac=%6D; ret=%d,"
	    " wmimac=%6D\n",
	    __func__,
	    ku->k.wk_keyix, ku->wmi_add,
	    ku->k.wk_flags, ku->k.wk_macaddr, ":",
	    ret, ku->wmi_macaddr, ":");

	if (ku->wmi_add == 1) {
		/* Note: this only matters for WEP. */
		ret = ath10k_set_key_h_def_keyidx(ar, arvif, 1, &ku->k);
		ath10k_dbg(ar, ATH10K_DBG_KEYCACHE,
		    "%s: TODO: gk update=%d\n", __func__, ret);
	}

	ATHP_CONF_UNLOCK(ar);

}

/*
 * For raw mode operation (software crypto), we don't need to program
 * in keys.
 *
 * For hardware encryption mode, we need to program in keys to allow
 * the firmware to both encrypt frames and also gate the EAPOL frame
 * exchange.  Yes, it gates the PM4 exchange (the first encrypted one)
 * until a key is programmed in.
 *
 * We can't do QoS and software encryption + native wifi right now -
 * it seems the firmware/hardware does messy things with deleting and
 * re-inserting a QoS header and that causes "issues" with the
 * software encryption.
 *
 * XXX TODO - no, we actually kinda have to push this into a deferred
 * context and run it on the taskqueue.  net80211 holds locks that
 * we shouldn't be sleeping through.
 */
static int
athp_key_set(struct ieee80211vap *vap, const struct ieee80211_key *k)
{
	struct ath10k *ar = vap->iv_ic->ic_softc;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vap);
	struct ieee80211_node *ni;
	struct athp_taskq_entry *e;
	struct athp_key_update *ku;

	/*
	 * This is a "bit" racy.  It's just a check to make sure
	 * we don't get called during the vap free/destroy path.
	 *
	 * Since we don't hold the conf lock for the whole
	 * duration of this function (XXX can we? Not likely
	 * whilst the net80211 com/node lock is held) we can't
	 * guarantee it's non-racy.
	 *
	 * It "should" be okay though.  Should.
	 */
	ATHP_CONF_LOCK(ar);
	if (! arvif->is_setup) {
		ATHP_CONF_UNLOCK(ar);
		return (1);
	}
	ATHP_CONF_UNLOCK(ar);

	if (k->wk_flags & IEEE80211_KEY_SWCRYPT)
		return (1);

	ni = ieee80211_ref_node(vap->iv_bss);

	/*
	 * For STA mode keys, we program in the MAC address
	 * of the peer.  No, we don't program in the 'ff:ff:ff:ff:ff:ff'
	 * address, sigh.
	 */

	/*
	 * Ideally there'd be a "pairwise or not" routine/flag,
	 * but .. there isn't.  Sigh.
	 */

	/*
	 * Allocate a callback function state.
	 */
	e = athp_taskq_entry_alloc(ar, sizeof(struct athp_key_update));
	if (e == NULL) {
		ath10k_err(ar, "%s: failed to allocate key-update\n",
		    __func__);
		return (0);
	}
	ku = athp_taskq_entry_to_ptr(e);

	/* WMI_KEY_TX_USAGE is for static WEP */
#if 0
	if (k->wk_flags & IEEE80211_KEY_XMIT)
		flags |= WMI_KEY_TX_USAGE;
#endif

	if (k->wk_flags & IEEE80211_KEY_GROUP)
		ku->wmi_flags |= WMI_KEY_GROUP;
	if (k->wk_keyix == 0)
		ku->wmi_flags |= WMI_KEY_PAIRWISE;

	/* Which MAC to feed to the command */
	memcpy(&ku->wmi_macaddr, ni->ni_macaddr, ETH_ALEN);

	/* Copy the whole key contents for now; which is dirty.. */
	memcpy(&ku->k, k, sizeof(struct ieee80211_key));

	/* Add */
	ku->wmi_add = 1;

	/* XXX ugh */
	ku->vap = vap;

	/* schedule */
	(void) athp_taskq_queue(ar, e, "athp_key_set", athp_key_update_cb);

	ieee80211_free_node(ni);
	return (1);
}

/*
 * Just delete the allocated key.
 *
 * Again, STA oriented, WPA oriented (not WEP yet.)
 *
 * We actually kinda have to push this into a deferred
 * context and run it on the taskqueue.  net80211 holds locks that
 * we shouldn't be sleeping through - eg, the node table lock when
 * ieee80211_delucastkey() is called.
 *
 * XXX: Note And, we can't grab our conflock here without causing a LOR
 * because this path is sometimes called whilst the node table lock is held.
 */
static int
athp_key_delete(struct ieee80211vap *vap, const struct ieee80211_key *k)
{
	struct ieee80211_node *ni;
	struct ath10k *ar = vap->iv_ic->ic_softc;
	struct ath10k_vif *arvif = ath10k_vif_to_arvif(vap);
	struct athp_taskq_entry *e;
	struct athp_key_update *ku;

	/*
	 * This is a "bit" racy.  It's just a check to make sure
	 * we don't get called during the vap free/destroy path.
	 *
	 * Since we don't hold the conf lock for the whole
	 * duration of this function (XXX can we? Not likely
	 * whilst the net80211 com/node lock is held) we can't
	 * guarantee it's non-racy.
	 *
	 * It "should" be okay though.  Should.
	 */
	ATHP_CONF_LOCK(ar);
	if (! arvif->is_setup) {
		ATHP_CONF_UNLOCK(ar);
		return (1);
	}
	ATHP_CONF_UNLOCK(ar);

	/*
	 * For now, we don't do any work for software encryption.
	 *
	 * Later on we can experiment with using CLEAR keys
	 * if we can get it working.
	 */
	if (k->wk_flags & IEEE80211_KEY_SWCRYPT)
		return (1);

	/*
	 * Allocate a callback function state.
	 */
	e = athp_taskq_entry_alloc(ar, sizeof(struct athp_key_update));
	if (e == NULL) {
		ath10k_err(ar, "%s: failed to allocate key-update\n",
		    __func__);
		return (0);
	}
	ku = athp_taskq_entry_to_ptr(e);

	ni = ieee80211_ref_node(vap->iv_bss);

	/* Again, this is for WEP */
#if 0
	if (k->wk_flags & IEEE80211_KEY_XMIT)
		flags |= WMI_KEY_TX_USAGE;
#endif
	if (k->wk_flags & IEEE80211_KEY_GROUP)
		ku->wmi_flags |= WMI_KEY_GROUP;
	if (k->wk_keyix == 0)
		ku->wmi_flags |= WMI_KEY_PAIRWISE;

	/* Which MAC to feed to the command */
	memcpy(&ku->wmi_macaddr, ni->ni_macaddr, ETH_ALEN);

	/* Copy the whole key contents for now; which is dirty.. */
	memcpy(&ku->k, k, sizeof(struct ieee80211_key));

	/* Delete */
	ku->wmi_add = 0;

	/* XXX ugh */
	ku->vap = vap;

	/* schedule */
	(void) athp_taskq_queue(ar, e, "athp_key_del", athp_key_update_cb);

	ieee80211_free_node(ni);
	return (1);
}

static struct ieee80211vap *
athp_vap_create(struct ieee80211com *ic, const char name[IFNAMSIZ], int unit,
    enum ieee80211_opmode opmode, int flags,
    const uint8_t bssid[IEEE80211_ADDR_LEN],
    const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct ath10k *ar = ic->ic_softc;
	struct ath10k_vif *uvp;
	struct ieee80211vap *vap;
	int ret;

	/* XXX for now, one vap */
	if (! TAILQ_EMPTY(&ic->ic_vaps))
		return (NULL);

	/* XXX TODO: figure out what we need to implement! */
	device_printf(ar->sc_dev, "%s: called\n", __func__);

	/* We have to bring up the hardware if it isn't yet */
	if (TAILQ_EMPTY(&ic->ic_vaps)) {
		/*
		 * XXX TODO: sigh, this path actually goes and re-re-re-re
		 * re-inits everything; which includes the memory allocations,
		 * and the /mutexes/, and the /tasks/, and the /callouts/.
		 *
		 * This .. can't happen, as it completely breaks how
		 * FreeBSD expects things to work.
		 *
		 * Trouble is, sigh, a whole bunch of WMI setup really seems
		 * to assume that we've completely powered off/reset the
		 * target CPU before its reinit'ed.  So, I may have to
		 * review each and every one of those pieces and fix
		 * the whole thing up.
		 *
		 * Ugh.
		 */
		ret = ath10k_start(ar);
		if (ret != 0) {
			ath10k_err(ar,
			    "%s: ath10k_start failed; ret=%d\n",
			    __func__, ret);
			return (NULL);
		}
	}

	uvp = malloc(sizeof(struct ath10k_vif), M_80211_VAP, M_WAITOK | M_ZERO);
	if (uvp == NULL)
		return (NULL);
	vap = (void *) uvp;

	if (ieee80211_vap_setup(ic, vap, name, unit, opmode,
	    flags | IEEE80211_CLONE_NOBEACONS, bssid) != 0) {
		free(uvp, M_80211_VAP);
		return (NULL);
	}

	/* Override vap methods */
	uvp->av_newstate = vap->iv_newstate;
	vap->iv_newstate = athp_vap_newstate;
	vap->iv_key_alloc = athp_key_alloc;
	vap->iv_key_set = athp_key_set;
	vap->iv_key_delete = athp_key_delete;

	/* Complete setup - so we can correctly tear it down if we need to */
	ieee80211_vap_attach(vap, ieee80211_media_change,
	    ieee80211_media_status, mac);
	/* XXX ew */
	ic->ic_opmode = opmode;

	/* call into driver; setup state */
	ret = ath10k_add_interface(ar, vap, opmode, flags, bssid, mac);
	if (ret != 0) {
		ath10k_err(ar, "%s: ath10k_add_interface failed; ret=%d\n",
		    __func__, ret);
		/*
		 * For now, we can't abort here - too much state needs
		 * to be setup before we call the linux ath10k mac.c
		 * routine.
		 */
		return (vap);
	}

	/* Get here - we're okay */
	uvp->is_setup = 1;

	return (vap);
}

static void
athp_vap_delete(struct ieee80211vap *vap)
{
	struct ieee80211com *ic = vap->iv_ic;
	struct ath10k *ar = ic->ic_softc;
	struct ath10k_vif *uvp = ath10k_vif_to_arvif(vap);

	device_printf(ar->sc_dev, "%s: called\n", __func__);

	/*
	 * Mark the VAP as dying.  This is to ensure we don't
	 * queue new frames, keycache modifications, etc after
	 * this point.
	 */
	ATHP_CONF_LOCK(ar);
	uvp->is_dying = 1;
	ATHP_CONF_UNLOCK(ar);

	/*
	 * Ideally we'd stop both TX and RX so we can ensure nothing
	 * is referencing a now-dead VAP.
	 */

	/*
	 * Only deinit the hardware/driver state if we did successfully
	 * set it up earlier.
	 */
	if (uvp->is_setup) {

		/* Wait for xmit to finish before continuing */
		ath10k_tx_flush(ar, vap, 0, 1);

		/*
		 * Flush/stop any pending taskq operations.
		 *
		 * Now, this is dirty and very single-VAP oriented; it's like
		 * this because unfortunately we don't know which entries
		 * reference this VAP or not.
		 *
		 * That all needs to, like, die.
		 */
		athp_taskq_flush(ar, 0);

		ATHP_CONF_LOCK(ar);
		ath10k_vdev_stop(uvp);
		ath10k_remove_interface(ar, vap);
		uvp->is_setup = 0;
		ATHP_CONF_UNLOCK(ar);
	}

	/*
	 * At this point the ath10k VAP no longer exists, so we can't
	 * queue things to the vdev anymore.
	 */

	/*
	 * XXX for now, we only support a single VAP.
	 * Later on, we need to check if any other VAPs are left and if
	 * not, we can power down.
	 */
	ath10k_stop(ar);

	/*
	 * Detaching the VAP at this point may generate other events,
	 * such as key deletions, sending last second frames, etc.
	 * So we have to make sure that any callbacks that occur
	 * at this point doesn't crash things.
	 */
	ieee80211_vap_detach(vap);

	/*
	 * Point of no return!
	 */
	free(uvp, M_80211_VAP);

	ath10k_warn(ar, "%s: finished!\n", __func__);
}

static int
athp_wme_update(struct ieee80211com *ic)
{

	return (0);
}

static void
athp_update_slot(struct ieee80211com *ic)
{

}

static void
athp_update_promisc(struct ieee80211com *ic)
{

}

static void
athp_update_mcast(struct ieee80211com *ic)
{

}

static struct ieee80211_node *
athp_node_alloc(struct ieee80211vap *vap,
    const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct ieee80211com *ic = vap->iv_ic;
	struct ath10k *ar = ic->ic_softc;
	struct athp_node *an;

	device_printf(ar->sc_dev, "%s: called; mac=%6D\n", __func__, mac, ":");

	an = malloc(sizeof(struct athp_node), M_80211_NODE, M_NOWAIT | M_ZERO);
	if (! an)
		return (NULL);

	/* XXX TODO: Create peer if it's not our MAC address */
	if (memcmp(mac, vap->iv_myaddr, ETHER_ADDR_LEN) != 0) {
		device_printf(ar->sc_dev,
		    "%s: TODO: add peer for MAC %6D\n",
		    __func__, mac, ":");
	}

	return (&an->ni);
}

static void
athp_newassoc(struct ieee80211_node *ni, int isnew)
{
	/* XXX TODO */
	struct ieee80211com *ic = ni->ni_vap->iv_ic;
	struct ath10k *ar = ic->ic_softc;
	device_printf(ar->sc_dev,
	    "%s: called; mac=%6D; isnew=%d\n",
	    __func__, ni->ni_macaddr, ":", isnew);
}

static void
athp_node_free(struct ieee80211_node *ni)
{

	/* XXX TODO */
	struct ieee80211com *ic = ni->ni_vap->iv_ic;
	struct ath10k *ar = ic->ic_softc;

	device_printf(ar->sc_dev,
	    "%s: called; mac=%6D\n",
	    __func__, ni->ni_macaddr, ":");

	/* XXX TODO: delete peer */
	if (memcmp(ni->ni_macaddr, ni->ni_vap->iv_myaddr, ETHER_ADDR_LEN) != 0) {
		device_printf(ar->sc_dev,
		    "%s: TODO: add peer for MAC %6D\n",
		    __func__, ni->ni_macaddr, ":");
	}

	ar->sc_node_free(ni);
}

static void
athp_update_chw(struct ieee80211com *ic)
{

}

static int
athp_ampdu_enable(struct ieee80211_node *ni, struct ieee80211_tx_ampdu *tap)
{

	return (0);
}

/*
 * XXX TODO: we don't need to send probe requests, and I don't think
 * we send association requests either?  Should check.
 */
static int
athp_send_mgmt(struct ieee80211_node *ni, int type, int arg)
{
	struct ieee80211vap *vap = ni->ni_vap;
	struct ieee80211com *ic = vap->iv_ic;
	struct ath10k *ar = ic->ic_softc;

	/* Don't send probe requests - I think the firmware does it during scanning */
	/* XXX TODO: maybe only don't do it when we're scanning? */
	if (type == IEEE80211_FC0_SUBTYPE_PROBE_REQ)
		return (ENOTSUP);

	/* Scanning sends out QoS-NULL frames too, which we don't want */
	if (type == IEEE80211_FC0_SUBTYPE_QOS_NULL)
		return (ENOTSUP);
	if (type == IEEE80211_FC0_SUBTYPE_NODATA)
		return (ENOTSUP);

	/*
	 * XXX TODO: once scan offload/powersave offload in net80211 is
	 * done, re-enable these - we may need it for eg testing if
	 * a device is still there.
	 */

	/* Send the rest */
	ath10k_dbg(ar, ATH10K_DBG_XMIT,
	    "%s: sending type=0x%x (%d)\n", __func__, type, type);

	return (ieee80211_send_mgmt(ni, type, arg));

}

/*
 * TODO: this doesn't yet take the regulatory domain into account.
 */
static void
athp_setup_channels(struct ath10k *ar)
{
	struct ieee80211com *ic = &ar->sc_ic;
	struct ieee80211_channel *chans = ic->ic_channels;
	uint8_t bands[howmany(IEEE80211_MODE_MAX, 8)];
	int *nchans = &ic->ic_nchans;
	int ht40 = 0;

	memset(bands, 0, sizeof(bands));

	if (ar->phy_capability & WHAL_WLAN_11G_CAPABILITY) {
		setbit(bands, IEEE80211_MODE_11B);
		setbit(bands, IEEE80211_MODE_11G);
		ieee80211_add_channel_list_2ghz(chans, IEEE80211_CHAN_MAX,
		    nchans, chan_list_2ghz, nitems(chan_list_2ghz),
		    bands, ht40);
	}
	if (ar->phy_capability & WHAL_WLAN_11A_CAPABILITY) {
		setbit(bands, IEEE80211_MODE_11A);
		ieee80211_add_channel_list_5ghz(chans, IEEE80211_CHAN_MAX,
		    nchans, chan_list_5ghz, nitems(chan_list_5ghz),
		    bands, ht40);
	}
}

void
athp_attach_sysctl(struct ath10k *ar)
{
	struct sysctl_oid *tree = device_get_sysctl_tree(ar->sc_dev);
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(ar->sc_dev);
	struct sysctl_oid_list *child = SYSCTL_CHILDREN(tree);

	SYSCTL_ADD_QUAD(ctx, child, OID_AUTO, "debug",
	    CTLFLAG_RW | CTLFLAG_RWTUN,
	    &ar->sc_debug, "debug control");
	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "hwcrypt_mode",
	    CTLFLAG_RW | CTLFLAG_RWTUN,
	    &ar->sc_conf_crypt_mode, 0, "software/hardware crypt mode");

	SYSCTL_ADD_QUAD(ctx, child, OID_AUTO, "stats_rx_msdu_invalid_len", CTLFLAG_RD,
	    &ar->sc_stats.rx_msdu_invalid_len, "");
	SYSCTL_ADD_QUAD(ctx, child, OID_AUTO, "stats_rx_pkt_short_len", CTLFLAG_RD,
	    &ar->sc_stats.rx_pkt_short_len, "");
	SYSCTL_ADD_QUAD(ctx, child, OID_AUTO, "stats_rx_pkt_zero_len", CTLFLAG_RD,
	    &ar->sc_stats.rx_pkt_zero_len, "");
	SYSCTL_ADD_QUAD(ctx, child, OID_AUTO, "stats_xmit_fail_crypto_encap", CTLFLAG_RD,
	    &ar->sc_stats.xmit_fail_crypto_encap, "");
	SYSCTL_ADD_QUAD(ctx, child, OID_AUTO, "stats_xmit_fail_mbuf_defrag", CTLFLAG_RD,
	    &ar->sc_stats.xmit_fail_mbuf_defrag, "");
	SYSCTL_ADD_QUAD(ctx, child, OID_AUTO, "stats_xmit_fail_get_pbuf", CTLFLAG_RD,
	    &ar->sc_stats.xmit_fail_get_pbuf, "");

	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "rx_wmi", CTLFLAG_RW,
	    &ar->sc_rx_wmi, 0, "RX WMI frames");
	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "rx_htt", CTLFLAG_RW,
	    &ar->sc_rx_htt, 0, "RX HTT frames");

	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "dbglog_module_mask",
	    CTLFLAG_RW, &ar->sc_dbglog_module, 0, "Debuglog module mask");
	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "dbglog_module_level",
	    CTLFLAG_RW, &ar->sc_dbglog_level, 0, "Debuglog module level");
}

/*
 * Process regulatory domain changes.
 *
 * XXX TODO: this ends up potentially sleeping on COMLOCK.
 * Maybe defer into a taskqueue later.
 */
static int
athp_set_regdomain(struct ieee80211com *ic, struct ieee80211_regdomain *reg,
    int nchans, struct ieee80211_channel *chans)
{
	struct ath10k *ar = ic->ic_softc;

	ath10k_warn(ar, "%s: called; rd %u cc %u location %c%s\n",
	    __func__,
	    reg->regdomain,
	    reg->country,
	    reg->location,
	    reg->ecm ? "ecm" : "");

	/*
	 * Program in the given channel set into the hardware.
	 */
	/* XXX locking! */
	IEEE80211_UNLOCK(ic);
	ATHP_CONF_LOCK(ar);
	if (ar->state == ATH10K_STATE_ON)
		(void) ath10k_regd_update(ar, nchans, chans);
	ATHP_CONF_UNLOCK(ar);
	IEEE80211_LOCK(ic);

	return (0);
}

/*
 * Attach time setup.
 *
 * This needs to be deferred until interrupts are enabled;
 * we can't run this code during probe as it does firmware messages
 * to set things up and that requires interrupts + sleeping.
 */
int
athp_attach_net80211(struct ath10k *ar)
{
	struct ieee80211com *ic = &ar->sc_ic;

	device_printf(ar->sc_dev, "%s: called\n", __func__);

	/* Setup net80211 state */
	ic->ic_softc = ar;
	ic->ic_name = device_get_nameunit(ar->sc_dev);
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;

	/* Setup device capabilities */
	ic->ic_caps =
	    IEEE80211_C_STA |
	    IEEE80211_C_BGSCAN |
	    IEEE80211_C_SHPREAMBLE |
	    IEEE80211_C_WME |
	    IEEE80211_C_SHSLOT |
	    IEEE80211_C_MONITOR |
	    IEEE80211_C_WPA;

	/* XXX crypto capabilities */
	if (ar->sc_conf_crypt_mode == ATH10K_CRYPT_MODE_HW) {
		ic->ic_cryptocaps |=
		    IEEE80211_CRYPTO_WEP |
		    IEEE80211_CRYPTO_AES_OCB |
		    IEEE80211_CRYPTO_AES_CCM |
		    IEEE80211_CRYPTO_CKIP |
		    IEEE80211_CRYPTO_TKIP |
		    IEEE80211_CRYPTO_TKIPMIC;
	}

	/* capabilities, etc */
	ic->ic_flags_ext |= IEEE80211_FEXT_SCAN_OFFLOAD;

	/* XXX 11n bits */

	/* XXX 11ac bits */

	/* Channels/regulatory */
	athp_setup_channels(ar);

	IEEE80211_ADDR_COPY(ic->ic_macaddr, ar->mac_addr);

	ieee80211_ifattach(ic);

	/* required 802.11 methods */
	ic->ic_raw_xmit = athp_raw_xmit;
	ic->ic_scan_start = athp_scan_start;
	ic->ic_scan_curchan = athp_scan_curchan;
	ic->ic_scan_mindwell = athp_scan_mindwell;
	ic->ic_scan_end = athp_scan_end;
	ic->ic_set_channel = athp_set_channel;
	ic->ic_transmit = athp_transmit;
	ic->ic_send_mgmt = athp_send_mgmt;
	ic->ic_parent = athp_parent;
	ic->ic_vap_create = athp_vap_create;
	ic->ic_vap_delete = athp_vap_delete;
	ic->ic_wme.wme_update = athp_wme_update;
	ic->ic_updateslot = athp_update_slot;
	ic->ic_update_promisc = athp_update_promisc;
	ic->ic_update_mcast = athp_update_mcast;
	ic->ic_node_alloc = athp_node_alloc;
	ic->ic_newassoc = athp_newassoc;
	ar->sc_node_free = ic->ic_node_free;
	ic->ic_node_free = athp_node_free;

	ic->ic_setregdomain = athp_set_regdomain;
#if 0
	ic->ic_getradiocaps = athp_get_radiocaps;
#endif

	/* 11n methods */
	ic->ic_update_chw = athp_update_chw;
	ic->ic_ampdu_enable = athp_ampdu_enable;

	/* radiotap attach */
	ieee80211_radiotap_attach(ic,
	    &ar->sc_txtapu.th.wt_ihdr, sizeof(ar->sc_txtapu), ATH10K_TX_RADIOTAP_PRESENT,
	    &ar->sc_rxtapu.th.wr_ihdr, sizeof(ar->sc_rxtapu), ATH10K_RX_RADIOTAP_PRESENT);

	// if (bootverbose)
		ieee80211_announce(ic);

	(void) athp_taskq_init(ar);

	return (0);
}

int
athp_detach_net80211(struct ath10k *ar)
{
	struct ieee80211com *ic = &ar->sc_ic;

	device_printf(ar->sc_dev, "%s: called\n", __func__);

	/* XXX Drain tasks from net80211 queue */

	/* stop/drain taskq entries */
	athp_taskq_flush(ar, 0);
	athp_taskq_free(ar);

	if (ic->ic_softc == ar)
		ieee80211_ifdetach(ic);

	return (0);
}
