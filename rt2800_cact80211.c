/*
 * rt2800usb <-> cact80211 glue.
 *
 * The driver is a dumb radio: this file registers it with the in-kernel 802.11
 * engine (cact80211), which owns the netdev (wlan0), the scan, open-system
 * auth/association and the CCMP datapath.  The engine drives the radio through
 * the ops table below and hands every received MPDU to it through
 * cact80211_rx() (see rt2800_usb_rings.c).
 */

#include "rt2800_local.h"

extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *dst, const void *src, unsigned int n);

/* Program the BSSID the engine has associated with (rt2800_config_intf's
 * CONFIG_UPDATE_BSSID): MAC_BSSID_DW0/DW1, in 8-BSSID mode with beacon count 0,
 * exactly as the old CACT_WLANCTL_SET_BSSID did. */
static void rt2800_program_bssid(struct rt_dev *rt, const u8 *bssid) {
    u32 dw0 = 0, dw1 = 0;

    rt2x00_set_field32(&dw0, MAC_BSSID_DW0_BYTE0, bssid[0]);
    rt2x00_set_field32(&dw0, MAC_BSSID_DW0_BYTE1, bssid[1]);
    rt2x00_set_field32(&dw0, MAC_BSSID_DW0_BYTE2, bssid[2]);
    rt2x00_set_field32(&dw0, MAC_BSSID_DW0_BYTE3, bssid[3]);
    rt2x00_set_field32(&dw1, MAC_BSSID_DW1_BYTE4, bssid[4]);
    rt2x00_set_field32(&dw1, MAC_BSSID_DW1_BYTE5, bssid[5]);
    rt2x00_set_field32(&dw1, MAC_BSSID_DW1_BSS_ID_MASK, 3);
    rt2x00_set_field32(&dw1, MAC_BSSID_DW1_BSS_BCN_NUM, 0);
    rt2800_register_write(rt, MAC_BSSID_DW0, dw0);
    rt2800_register_write(rt, MAC_BSSID_DW1, dw1);

    memcpy(rt->bssid, bssid, 6);
}

/* Transmit one raw 802.11 MPDU on the queue the engine asked for. */
static int rt_cc_tx(void *priv, const u8 *mpdu, int len, u8 qid) {
    struct rt_dev *rt = (struct rt_dev *)priv;
    u8 hwq = (qid == CACT80211_Q_MGMT) ? RT_QID_MGMT : RT_QID_AC_BE;

    if (!rt)
        return -RT_EINVAL;
    return rt2800_tx_frame(rt, mpdu, (u16)len, hwq);
}

/* Apply the engine's view of the medium: channel, the AP's basic rates/ERP and,
 * on association, the BSSID.  The engine calls this on every scan channel and
 * once more when it joins. */
static void rt_cc_config(void *priv, const struct cact80211_conf *conf) {
    struct rt_dev *rt = (struct rt_dev *)priv;

    if (!rt || !conf)
        return;
    rt2800_set_channel(rt, conf->channel);
    rt->basic_rates = conf->basic_rates;
    rt->erp_flags = conf->erp_flags;
    if (conf->basic_rates)
        rt2800_config_erp(rt, conf->basic_rates, conf->erp_flags);
    if (conf->assoc)
        rt2800_program_bssid(rt, conf->bssid);
}

/* One receive poll; rt2800_rx_poll() hands each frame to cact80211_rx(). */
static int rt_cc_rx_poll(void *priv) {
    return rt2800_rx_poll((struct rt_dev *)priv);
}

static struct cact80211_ops rt_cc_ops = {
    .tx      = rt_cc_tx,
    .config  = rt_cc_config,
    .rx_poll = rt_cc_rx_poll,
};

int rt2800_wlan_register(struct rt_dev *rt) {
    struct cact80211_hw *hw;
    int rc;

    if (!rt)
        return -RT_EINVAL;

    hw = (struct cact80211_hw *)kmalloc(sizeof(*hw));
    if (!hw)
        return -RT_ENOMEM;
    memset(hw, 0, sizeof(*hw));
    hw->ops  = &rt_cc_ops;
    hw->priv = rt;
    memcpy(hw->mac, rt->mac, 6);
    hw->name = "wlan0";

    /* Set before registration: the engine may poll the radio as soon as it is
     * registered, and the RX path needs rt->hw to hand frames back. */
    rt->hw = hw;

    rc = cact80211_register(hw);
    if (rc != 0) {
        rt->hw = NULL;
        kfree(hw);
        return rc;
    }
    return 0;
}

void rt2800_wlan_unregister(struct rt_dev *rt) {
    if (!rt || !rt->hw)
        return;
    cact80211_unregister((struct cact80211_hw *)rt->hw);
    kfree(rt->hw);
    rt->hw = NULL;
}
