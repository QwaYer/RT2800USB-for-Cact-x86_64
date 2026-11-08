/*
 * 802.3 <-> 802.11 data-path conversion for rt2800usb.
 *
 * TX: the network stack hands us an Ethernet frame; wrap it in an 802.11 data
 *     header (ToDS) plus the LLC/SNAP header the AP expects.
 * RX: frames from the AP (FromDS) carry Addr1 = our MAC, Addr3 = original
 *     sender; build the Ethernet frame back out of that.
 */

#include "rt2800_local.h"
#include "net.h"

#define ETH_HDR_LEN   14
#define LLC_SNAP_LEN  8
#define DOT11_HDR     24
#define DOT11_QOS_HDR 26

extern skb_t* skb_alloc(void);
extern uint8_t* skb_put(skb_t* skb, uint16_t len);
extern void   netif_rx(skb_t* skb);

/* True when a data frame carries EAPOL (ethertype 0x888e).  EAPOL belongs to
 * wljoin, so the RX dispatch must not hand it to the network stack.  A
 * protected frame has an encrypted LLC and is never EAPOL (the handshake runs
 * in the clear), so it correctly answers false. */
bool rt2800_frame_is_eapol(const u8 *f, int len) {
    u8 sub;
    int hdr;
    const u8 *llc;

    if (len < DOT11_HDR + LLC_SNAP_LEN)
        return false;
    if (((f[0] >> 2) & 0x3) != 2)          /* not a data frame */
        return false;
    if (!(f[1] & 0x02))                    /* not FromDS */
        return false;

    sub = (u8)((f[0] >> 4) & 0xf);
    hdr = (sub & 0x8) ? DOT11_QOS_HDR : DOT11_HDR;
    if (len < hdr + LLC_SNAP_LEN)
        return false;

    llc = f + hdr;
    if (!(llc[0] == 0xaa && llc[1] == 0xaa && llc[2] == 0x03))
        return false;
    return llc[6] == 0x88 && llc[7] == 0x8e;
}

int rt2800_tx_eth(struct rt_dev *rt, const u8 *eth, int len) {
    u8 f[1600];
    int n = 0;

    if (!rt->assoc || len < ETH_HDR_LEN)
        return -1;

    int plen = len - ETH_HDR_LEN;
    if (DOT11_HDR + LLC_SNAP_LEN + plen > (int)sizeof(f))
        return -1;

    f[n++] = 0x08; f[n++] = 0x01;          /* data, ToDS */
    f[n++] = 0x00; f[n++] = 0x00;          /* duration */
    memcpy(f + n, rt->bssid, 6); n += 6;   /* Addr1: BSSID */
    memcpy(f + n, rt->mac, 6);   n += 6;   /* Addr2: us */
    memcpy(f + n, eth, 6);       n += 6;   /* Addr3: destination */
    f[n++] = 0x00; f[n++] = 0x00;          /* sequence */

    /* LLC/SNAP: one 802.3 payload with the Ethernet type. */
    f[n++] = 0xaa; f[n++] = 0xaa; f[n++] = 0x03;
    f[n++] = 0x00; f[n++] = 0x00; f[n++] = 0x00;
    f[n++] = eth[12]; f[n++] = eth[13];

    memcpy(f + n, eth + ETH_HDR_LEN, plen); n += plen;

    int is_eapol = (eth[12] == 0x88 && eth[13] == 0x8e);
    int is_group = (eth[0] & 0x01) != 0;    /* broadcast/multicast destination */

    /* EAPOL frames stay in the clear; data frames are CCMP-encrypted with the
     * key the frame needs — the pairwise key (TK) for unicast, the group key
     * (GTK) for broadcast/multicast, with the key id the AP assigned so the
     * receiver picks the same key.  Keys are installed from userspace. */
    const u8 *key = is_group ? rt->gtk : rt->tk;
    u8 key_id = is_group ? rt->gtk_id : 0;
    int have_key = is_group ? rt->have_gtk : rt->have_tk;
    int rc;

    if (is_eapol) {
        rc = rt2800_tx_frame(rt, f, (u16)n, RT_QID_AC_BE);
    } else if (have_key) {
        u8 out[2048];
        int tot;
        /* Mark the frame protected before encrypting: an AP only decrypts a
         * data frame whose Protected bit is set, and the AAD the MIC is
         * computed over already carries Protected (ccmp_aad forces it), so the
         * header on the air has to agree. */
        f[1] |= 0x40;
        tot = rt2800_ccmp_encrypt(key, f, DOT11_HDR,
                                  f + DOT11_HDR, n - DOT11_HDR, key_id,
                                  out, sizeof(out));
        rc = (tot > 0) ? rt2800_tx_frame(rt, out, (u16)tot, RT_QID_AC_BE)
                       : -RT_EINVAL;
    } else {
        /* Sending it in the clear would only be dropped by the AP; the caller
         * (netdev send) cannot report it, but the reason is recorded. */
        rt->last_error = -RT_ENOKEY;
        rc = -RT_ENOKEY;
    }

    /* Diagnose the netdev data path through STATUS: what was asked to go out,
     * how much of it was group-addressed, and how much never reached the air. */
    if (!is_eapol) {
        rt->diag_tx_data++;
        if (is_group)
            rt->diag_tx_group++;
        if (rc != 0)
            rt->diag_tx_fail++;
    }
    return rc;
}

void rt2800_rx_data(struct rt_dev *rt, u8 *f, int len) {
    if (len < DOT11_HDR + LLC_SNAP_LEN)
        return;

    u8 fc1 = f[1];
    if (!(fc1 & 0x02))                     /* not FromDS: ignore */
        return;

    /* Only accept frames from the AP we are joined to: for a FromDS frame A2
     * (f+10) is the BSSID.  The hardware RX filter does not drop other BSSIDs,
     * so without this every neighbouring AP's encrypted frames land here and
     * fail our CCMP, looking exactly like a key problem. */
    if (memcmp(f + 10, rt->bssid, 6) != 0) {
        rt->diag_rx_other++;
        return;
    }

    u8 sub = (f[0] >> 4) & 0xf;
    int hdr = (sub & 0x8) ? DOT11_QOS_HDR : DOT11_HDR;

    const u8 *llc;
    int plen2;

    if (fc1 & 0x40) {                      /* protected: CCMP */
        /* The CCMP header's key id selects the key, exactly as the sender
         * intended: 0 is the pairwise key, 1..3 the group key. */
        u8 ccmp_key_id = (u8)((f[hdr + 3] >> 6) & 0x3);
        const u8 *key = ccmp_key_id ? rt->gtk : rt->tk;

        if ((ccmp_key_id && !rt->have_gtk) || (!ccmp_key_id && !rt->have_tk)) {
            rt->diag_rx_nokey++;           /* protected, but the key is missing */
            return;
        }
        int n = rt2800_ccmp_decrypt(key, f, len, hdr);
        if (n < 0) {
            rt->diag_rx_bad++;             /* MIC / decrypt failure */
            return;
        }
        llc = f + hdr;
        plen2 = n;
    } else {
        if (len < hdr + LLC_SNAP_LEN)
            return;
        llc = f + hdr;
        plen2 = len - hdr;
    }

    if (!(llc[0] == 0xaa && llc[1] == 0xaa && llc[2] == 0x03))
        return;

    u16 ethertype = (u16)((llc[6] << 8) | llc[7]);
    const u8 *payload = llc + LLC_SNAP_LEN;
    int plen = plen2 - LLC_SNAP_LEN;
    if (plen <= 0 || plen > SKB_MAX_SIZE - ETH_HDR_LEN)
        return;

    /* EAPOL never reaches here: rt2800_rx_dispatch() routes it (and every
     * management frame) to the wljoin queue instead. */

    skb_t *skb = skb_alloc();
    if (!skb)
        return;

    u8 *d = skb_put(skb, (uint16_t)(ETH_HDR_LEN + plen));
    if (!d) {
        kfree_skb(skb);
        return;
    }

    memcpy(d,     f + 4,  6);              /* dst = Addr1 */
    memcpy(d + 6, f + 16, 6);              /* src = Addr3 */
    d[12] = (u8)(ethertype >> 8);
    d[13] = (u8)ethertype;
    memcpy(d + ETH_HDR_LEN, payload, plen);

    rt->diag_rx_data++;
    if (f[4] & 0x01)                       /* Addr1 = DA: broadcast/multicast */
        rt->diag_rx_group++;
    netif_rx(skb);
}
