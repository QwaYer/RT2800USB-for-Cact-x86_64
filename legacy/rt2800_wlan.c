/*
 * /dev/wlan0 control surface — the driver as a dumb radio.
 *
 * Association, authentication and WPA2 live in the userspace wljoin utility;
 * this file only gives it the hardware:
 *   - SCAN keeps the driver's AP list (read(/dev/wlan0) serves it as text);
 *   - TX/RX move raw 802.11 MPDUs (management, EAPOL, data);
 *   - SET_CHANNEL / SET_BSSID / SET_KEY / STATUS expose the link knobs;
 *   - received management and EAPOL frames are queued here, not acted on.
 *
 * Frames are copied across the user/kernel boundary with the exported
 * validate/copy primitives (see ksym.c), so a bad pointer returns -EFAULT
 * instead of faulting.
 */

#include "rt2800_local.h"

extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);
extern int   validate_user_ptr(const void *ptr, uint32_t size);
extern int   copy_from_user(void *dst, const void *src, uint32_t size);
extern int   copy_to_user(void *dst, const void *src, uint32_t size);

/* Errors go back to userspace as negative return values (RT_E* in
 * rt2800_local.h): the connection utility prints them, the driver does not. */

/* Raw RX queue: management and EAPOL frames for wljoin.  One device, one
 * consumer, so a plain ring with head/tail is enough; a full ring drops. */
#define RT_RX_RING       8
#define RT_RX_RING_FRAME CACT_WLAN_FRAME_MAX

static u8  rt_rx_ring[RT_RX_RING][RT_RX_RING_FRAME];
static u16 rt_rx_len[RT_RX_RING];
static u16 rt_rx_head;   /* producer (RX path) */
static u16 rt_rx_tail;   /* consumer (wljoin) */

static void rt_rx_enqueue(const u8 *f, int len) {
    u16 next = (u16)((rt_rx_head + 1) % RT_RX_RING);

    if (next == rt_rx_tail)
        return;                          /* ring full: drop */
    memcpy(rt_rx_ring[rt_rx_head], f, (unsigned)len);
    rt_rx_len[rt_rx_head] = (u16)len;
    rt_rx_head = next;
}

static int rt_rx_dequeue(u8 *out, int cap) {
    int n;

    if (rt_rx_tail == rt_rx_head)
        return 0;                        /* empty */
    n = rt_rx_len[rt_rx_tail];
    if (n > cap)
        n = cap;
    memcpy(out, rt_rx_ring[rt_rx_tail], (unsigned)n);
    rt_rx_tail = (u16)((rt_rx_tail + 1) % RT_RX_RING);
    return n;
}

/* Every received 802.11 frame passes here (rt2800_rx_poll).  Beacons and probe
 * responses maintain the driver's AP list; data frames go to the network stack;
 * management and EAPOL frames are queued for wljoin. */
void rt2800_rx_dispatch(struct rt_dev *rt, u8 *f, int len) {
    u8 type;
    u8 sub;

    if (len < 2)
        return;

    type = (u8)((f[0] >> 2) & 0x3);
    sub  = (u8)((f[0] >> 4) & 0xf);

    if (type == 0 && (sub == 8 || sub == 5))
        rt2800_scan_frame(rt, f, len);   /* keep the AP list current */

    /* A beacon from the AP we are joined to is the link-liveness signal the
     * beacon-loss watchdog watches; beacons are broadcast, so the RX filter
     * lets them through. */
    if (type == 0 && sub == 8 && len >= 24 &&
        memcmp(f + 16, rt->bssid, 6) == 0)
        rt->last_beacon_us = ktime_get_usec();

    if (type == 2 && !rt2800_frame_is_eapol(f, len)) {
        rt2800_rx_data(rt, f, len);
        return;
    }
    if (len > RT_RX_RING_FRAME)
        return;
    rt_rx_enqueue(f, len);
}

/* Copy a frame from userspace into a kernel buffer (frames are larger than a
 * comfortable stack object, so the buffer is allocated). */
static int rt_wlan_tx(struct rt_dev *rt, void *arg) {
    cact_wlan_frame_t *f;
    int rc;

    if (!arg || !validate_user_ptr(arg, sizeof(*f)))
        return -RT_EFAULT;
    f = (cact_wlan_frame_t *)kmalloc(sizeof(*f));
    if (!f)
        return -RT_ENOMEM;
    if (copy_from_user(f, arg, sizeof(*f)) != 0) {
        kfree(f);
        return -RT_EFAULT;
    }
    rc = -RT_EINVAL;
    if (f->len > 0 && f->len <= sizeof(f->data))
        rc = rt2800_tx_frame(rt, f->data, (u16)f->len,
                             rt2800_tx_qid(f->data, (u16)f->len));
    kfree(f);
    return rc;
}

static int rt_wlan_rx(struct rt_dev *rt, void *arg) {
    cact_wlan_frame_t *f;
    int n, rc;

    if (!arg || !validate_user_ptr(arg, sizeof(*f)))
        return -RT_EFAULT;
    f = (cact_wlan_frame_t *)kmalloc(sizeof(*f));
    if (!f)
        return -RT_ENOMEM;

    n = rt_rx_dequeue(f->data, (int)sizeof(f->data));
    if (n == 0) {
        /* Nothing queued.  This ioctl is the userspace half of the radio's RX
         * path: with no frame waiting it performs one bulk-IN itself.  A caller
         * that loops on it (wljoin's scan and 4-way handshake) is then paced by
         * the air — RT_RX_TIMEOUT_MS (20 ms) per empty poll — instead of
         * spinning, which is what wljoin's poll budgets assume.  The background
         * netdev poller stays off the radio while this runs. */
        rt2800_rx_userspace_begin();
        rt2800_rx_poll(rt);
        rt2800_rx_userspace_end();
        n = rt_rx_dequeue(f->data, (int)sizeof(f->data));
    }
    f->len = (uint32_t)n;
    rc = copy_to_user(arg, f, sizeof(*f));
    kfree(f);
    return rc != 0 ? -RT_EFAULT : 0;
}

static int rt_wlan_set_bssid(struct rt_dev *rt, void *arg) {
    cact_wlan_bssid_t b;
    u32 dw0 = 0, dw1 = 0;

    if (!arg || copy_from_user(&b, arg, sizeof(b)) != 0)
        return -RT_EFAULT;

    rt2x00_set_field32(&dw0, MAC_BSSID_DW0_BYTE0, b.bssid[0]);
    rt2x00_set_field32(&dw0, MAC_BSSID_DW0_BYTE1, b.bssid[1]);
    rt2x00_set_field32(&dw0, MAC_BSSID_DW0_BYTE2, b.bssid[2]);
    rt2x00_set_field32(&dw0, MAC_BSSID_DW0_BYTE3, b.bssid[3]);
    rt2x00_set_field32(&dw1, MAC_BSSID_DW1_BYTE4, b.bssid[4]);
    rt2x00_set_field32(&dw1, MAC_BSSID_DW1_BYTE5, b.bssid[5]);
    /* rt2800_config_intf() writes 8-BSSID mode and beacon count 0 together with
     * the BSSID. */
    rt2x00_set_field32(&dw1, MAC_BSSID_DW1_BSS_ID_MASK, 3);
    rt2x00_set_field32(&dw1, MAC_BSSID_DW1_BSS_BCN_NUM, 0);
    rt2800_register_write(rt, MAC_BSSID_DW0, dw0);
    rt2800_register_write(rt, MAC_BSSID_DW1, dw1);

    memcpy(rt->bssid, b.bssid, 6);
    rt->assoc = 1;
    return 0;
}

static int rt_wlan_set_key(struct rt_dev *rt, void *arg) {
    cact_wlan_key_t k;
    u8 *dst;
    u32 n;

    if (!arg || copy_from_user(&k, arg, sizeof(k)) != 0)
        return -RT_EFAULT;
    if (k.key_len == 0 || k.key_len > sizeof(k.key))
        return -RT_EINVAL;

    n = k.key_len < 16 ? k.key_len : 16;
    if (k.kind == CACT_WLAN_KEY_GROUP) {
        dst = rt->gtk;
        /* The AP picks the group key id; every group frame we send carries it
         * in the CCMP header, and that is how received ones select the key. */
        rt->gtk_id = (u8)(k.key_id & 0x3);
        rt->have_gtk = 1;
    } else {
        dst = rt->tk;
        rt->have_tk = 1;
    }
    memcpy(dst, k.key, n);
    return 0;
}

static int rt_wlan_set_link(struct rt_dev *rt, void *arg) {
    int up;

    if (!arg || copy_from_user(&up, arg, sizeof(up)) != 0)
        return -RT_EFAULT;
    rt->linked = up ? 1 : 0;
    /* Start the beacon clock at link-up, so the watchdog cannot fire before the
     * AP's first beacon has had a chance to arrive. */
    if (rt->linked)
        rt->last_beacon_us = ktime_get_usec();
    return 0;
}

/* The AP's basic-rate set and ERP timings, from its beacon (rt2x00lib's
 * BSS_CHANGED_BASIC_RATES / BSS_CHANGED_ERP_* handling). */
static int rt_wlan_set_rates(struct rt_dev *rt, void *arg) {
    cact_wlan_rates_t r;

    if (!arg || copy_from_user(&r, arg, sizeof(r)) != 0)
        return -RT_EFAULT;
    rt2800_config_erp(rt, r.basic, r.flags);
    return 0;
}

static int rt_wlan_status(struct rt_dev *rt, void *arg) {
    cact_wlan_status_t st;

    if (!arg || !validate_user_ptr(arg, sizeof(st)))
        return -RT_EFAULT;
    memset(&st, 0, sizeof(st));
    st.linked     = rt->linked;
    st.channel    = rt->scan_channel;
    st.last_error = rt->last_error;
    st.ccmp_selftest = rt->ccmp_ok;
    memcpy(st.mac, rt->mac, 6);
    memcpy(st.bssid, rt->bssid, 6);
    st.tx_data  = rt->diag_tx_data;
    st.tx_group = rt->diag_tx_group;
    st.tx_fail  = rt->diag_tx_fail;
    st.rx_data  = rt->diag_rx_data;
    st.rx_group = rt->diag_rx_group;
    st.rx_bad   = rt->diag_rx_bad;
    st.rx_nokey = rt->diag_rx_nokey;
    st.rx_other = rt->diag_rx_other;
    return copy_to_user(arg, &st, sizeof(st)) != 0 ? -RT_EFAULT : 0;
}

/* Beacon-loss watchdog — called from the netdev poller (every ~100 ms).  While
 * linked, every beacon from the AP refreshes last_beacon_us; once none has
 * arrived for RT_BEACON_LOSS_US the association is gone, so drop `linked`.
 * wifid reads that through CACT_WLANCTL_STATUS and re-runs wljoin, and the
 * netdev send() path (which gates on `linked`) closes with it. */
void rt2800_link_watchdog(struct rt_dev *rt) {
    if (!rt || !rt->linked)
        return;
    if (ktime_get_usec() - rt->last_beacon_us > RT_BEACON_LOSS_US)
        rt->linked = 0;
}

int rt_wlan_ioctl(void *priv, uint32_t cmd, void *arg) {
    struct rt_dev *rt = (struct rt_dev *)priv;

    switch (cmd) {
    case CACT_WLANCTL_SCAN: {
        int rc;

        rt2800_rx_userspace_begin();
        rc = rt2800_scan(rt);
        rt2800_rx_userspace_end();
        return rc;
    }

    case CACT_WLANCTL_TX:
        return rt_wlan_tx(rt, arg);

    case CACT_WLANCTL_RX:
        return rt_wlan_rx(rt, arg);

    case CACT_WLANCTL_SET_CHANNEL: {
        cact_wlan_channel_t c;
        if (!arg || copy_from_user(&c, arg, sizeof(c)) != 0)
            return -RT_EFAULT;
        return rt2800_set_channel(rt, (u8)c.channel);
    }

    case CACT_WLANCTL_SET_BSSID:
        return rt_wlan_set_bssid(rt, arg);

    case CACT_WLANCTL_SET_KEY:
        return rt_wlan_set_key(rt, arg);

    case CACT_WLANCTL_STATUS:
        return rt_wlan_status(rt, arg);

    case CACT_WLANCTL_SET_LINK:
        return rt_wlan_set_link(rt, arg);

    case CACT_WLANCTL_SET_RATES:
        return rt_wlan_set_rates(rt, arg);
    }
    return -RT_EINVAL;
}
