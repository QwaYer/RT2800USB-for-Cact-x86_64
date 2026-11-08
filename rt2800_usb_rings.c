/*
 * rt2800usb USB TX/RX rings.
 *
 * The kernel's USB core has only synchronous transfers, so instead of the
 * rt2x00 URB rings this drives the bulk endpoints directly:
 *
 *   RX: one bulk-IN read returns | RXINFO(4) | RXWI(20) | 802.11 frame | ... |;
 *       RXINFO word0 carries the packet length (RXWI + frame), so the transfer
 *       length is not needed.
 *   TX: build | TXINFO(4) | TXWI(20) | 802.11 frame padded to 4 | and bulk-OUT
 *       it to the TX endpoint.
 *
 * Endpoint assignment mirrors rt2x00usb_find_endpoints: the first bulk-IN
 * endpoint serves RX, the first bulk-OUT endpoint serves TX.
 */

#include "rt2800_local.h"

/* USB DMA descriptors (rt2800usb.h). */
#define TXINFO_W0_USB_DMA_TX_PKT_LEN   FIELD32(0x0000ffff)
#define TXINFO_W0_WIV                  FIELD32(0x01000000)
#define TXINFO_W0_QSEL                 FIELD32(0x06000000)
#define RXINFO_W0_USB_DMA_RX_PKT_LEN   FIELD32(0x0000ffff)

#define RXINFO_DESC_SIZE   4
#define TXINFO_DESC_SIZE   4
/* TXWI and RXWI are each FOUR words (16 bytes) on RT3070:
 * rt2800_get_txwi_rxwi_size() (rt2800lib.c:603) returns the 4/4 pair for every
 * chip except RT3593/RT3883 (4/5) and RT5592/RT6352 (5/6), and RT3070 takes the
 * default.  The RXWI was confirmed on the air: with five words every delivered
 * frame decoded as the reserved 802.11 type 3, and an offset sweep put the true
 * frame control exactly four bytes earlier on every channel (hits == frames);
 * a pkt_len of 48 for our own loopback probe request is exactly the 32-byte
 * frame, where five words left it 4 bytes short.  The same five-word TXWI put
 * the 802.11 MPDU four bytes late in the bulk-OUT record, so the MAC read four
 * descriptor bytes as the frame and never completed a transmission. */
#define RXWI_SIZE          RXWI_DESC_SIZE_4WORDS
#define TXWI_SIZE          TXWI_DESC_SIZE_4WORDS

#define RX_BUFSZ   (RXINFO_DESC_SIZE + RXWI_SIZE + 2048 + 64)

/* USB transfer timeouts.  RX is polled in a tight loop, so a short timeout
 * keeps the bulk-IN pending almost continuously — which is what Linux's
 * URB-based rt2x00usb does by always having RX URBs submitted.  With one long
 * (1 s) poll per channel the chip's RX PBF fills up in between and overflows
 * (RX_STA ovf is non-zero on a busy channel), and an overflowed FIFO drops what
 * it holds.  TX carries a frame and keeps the long timeout. */
#define RT_RX_TIMEOUT_MS   20
#define RT_TX_TIMEOUT_MS   1000

/* How long to wait for the MAC's TX_STA_FIFO entry of a frame that required an
 * ACK.  The MAC defers a frame while the medium is busy (CCA / backoff) and the
 * entry only appears once it actually goes out; Linux accounts for that ("The
 * hw may delay sending the packet after DMA complete if the medium is busy,
 * thus the TX_STA_FIFO entry is also delayed") by re-reading on a timer and
 * treating an entry as dead only after two seconds
 * (rt2800_entry_txstatus_timeout, rt2800lib.c).  A 20 ms budget reported such
 * frames as never completed long before they had left the air. */
#define RT_TX_STATUS_TIMEOUT_MS  1000

static u8 rx_buf[RX_BUFSZ];

int rt2800_find_endpoints(struct rt_dev *rt) {
    usb_device_t *usb = rt->usb;

    rt->rx_ep = 0;
    rt->tx_ep = 0;
    rt->tx_ep_count = 0;

    for (u8 i = 0; i < usb->ep_count; i++) {
        usb_endpoint_t *ep = &usb->ep[i];

        if (ep->transfer_type != USB_TRANSFER_BULK)
            continue;

        if (ep->direction == USB_DIR_IN && !rt->rx_ep) {
            rt->rx_ep = ep->address;
            rt->rx_maxpkt = ep->max_packet;
        } else if (ep->direction == USB_DIR_OUT &&
                   rt->tx_ep_count < RT_MAX_TX_EPS) {
            /* In descriptor order, as rt2x00usb_find_endpoints() does: the
             * N-th bulk-OUT endpoint serves the N-th TX queue (VO, VI, BE, BK,
             * MGMT, beacon).  A frame is written to its queue's endpoint. */
            rt->tx_eps[rt->tx_ep_count++] = ep->address;
            if (!rt->tx_ep) {
                rt->tx_ep = ep->address;
                rt->tx_maxpkt = ep->max_packet;
            }
        }
    }

    return (rt->rx_ep && rt->tx_ep) ? 0 : -1;
}

/* Which bulk-OUT endpoint carries a TX queue (rt2x00usb's queue order).  A
 * dongle with fewer than six OUT endpoints falls back to the first one, like
 * rt2x00usb_find_endpoints() copies the last found endpoint to the remaining
 * queues. */
static u8 rt2800_tx_ep_for_qid(struct rt_dev *rt, u8 qid) {
    u8 idx;

    switch (qid) {
    case RT_QID_AC_VO: idx = 0; break;
    case RT_QID_AC_VI: idx = 1; break;
    case RT_QID_AC_BE: idx = 2; break;
    case RT_QID_AC_BK: idx = 3; break;
    case RT_QID_MGMT:  idx = 4; break;
    default:           idx = 5; break;    /* beacon */
    }
    return idx < rt->tx_ep_count ? rt->tx_eps[idx] : rt->tx_ep;
}

/* Read one frame from the RX bulk endpoint.  Returns the 802.11 frame length,
 * 0 when nothing arrived, negative on a bad descriptor. */
int rt2800_rx_poll(struct rt_dev *rt) {
    usb_device_t *usb = rt->usb;

    if (!rt->rx_ep)
        return 0;

    int n = usb->hc->bulk_transfer(usb->hc, usb, rt->rx_ep, USB_DIR_IN,
                                   rx_buf, sizeof(rx_buf), RT_RX_TIMEOUT_MS);
    if (n < 0)
        return 0;   /* no data (timeout) */

    u32 rxinfo;
    memcpy(&rxinfo, rx_buf, RXINFO_DESC_SIZE);
    int pkt_len = rxinfo & 0xffff;          /* RXWI + frame + pad + RXD */
    if (pkt_len <= RXWI_SIZE || pkt_len + RXINFO_DESC_SIZE > (int)sizeof(rx_buf))
        return -1;

    /* The 802.11 frame length lives in the RXWI, not in the record length: the
     * USB record is | RXINFO | RXWI | frame | pad | RXD |, so pkt_len -
     * RXWI_SIZE hands the parser the pad and the 4-byte RXD as well.  Those
     * stray bytes decode as a bogus trailing information element, which is how
     * an SSID silently ends up empty.  Linux reads the same field
     * (RXWI_W0_MPDU_TOTAL_BYTE_COUNT, rt2800_process_rxwi, rt2800lib.c:909). */
    u32 rxwi0;
    memcpy(&rxwi0, rx_buf + RXINFO_DESC_SIZE, 4);
    int flen = (int)rt2x00_get_field32(rxwi0, RXWI_W0_MPDU_TOTAL_BYTE_COUNT);
    if (flen <= 0 || flen > pkt_len - RXWI_SIZE)
        flen = pkt_len - RXWI_SIZE - 4;     /* no usable RXWI length */
    if (flen <= 0 ||
        RXINFO_DESC_SIZE + RXWI_SIZE + flen > (int)sizeof(rx_buf))
        return -1;

    u8 *frame = rx_buf + RXINFO_DESC_SIZE + RXWI_SIZE;

    /* Hand the MPDU to the in-kernel 802.11 engine, which owns the scan, the
     * datapath and CCMP. */
    if (rt->hw)
        cact80211_rx((struct cact80211_hw *)rt->hw, frame, flen);
    return flen;
}

/* The 802.11 rate the chip is asked to use for a frame.
 *
 * Linux sends management frames and EAPOL at a *basic* rate: mac80211 picks the
 * lowest rate of the BSS's basic set (bss_conf.basic_rates) and EAPOL bypasses
 * rate control the same way.  The engine parses the AP's beacon and programs the
 * set here through the config() op.  Data keeps the lowest OFDM rate until there
 * is a rate-control policy. */
struct rt_tx_rate { u8 phymode; u8 mcs; };

/* bit0..11 of a basic-rate bitmap: 1, 2, 5.5, 11 Mbit/s CCK, then 6..54 OFDM. */
static const struct rt_tx_rate rt_rates_bg[12] = {
    { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 },
    { 1, 0 }, { 1, 1 }, { 1, 2 }, { 1, 3 },
    { 1, 4 }, { 1, 5 }, { 1, 6 }, { 1, 7 },
};

/* EAPOL on its way out (ToDS): LLC/SNAP with ethertype 0x888e.  The
 * FromDS-based rt2800_frame_is_eapol() only recognises received frames. */
static bool rt2800_tx_is_eapol(const u8 *f, u16 len) {
    return len >= 24 + 8 &&
           f[24] == 0xaa && f[25] == 0xaa && f[26] == 0x03 &&
           f[30] == 0x88 && f[31] == 0x8e;
}

static struct rt_tx_rate rt2800_tx_rate(struct rt_dev *rt, const u8 *f,
                                        u16 len) {
    u8 type = (u8)((f[0] >> 2) & 0x3);
    bool basic = (type == 0);                /* management */
    struct rt_tx_rate r;

    if (type == 2 && rt2800_tx_is_eapol(f, len))
        basic = true;                        /* EAPOL, as in mac80211 */

    if (!basic) {
        r.phymode = 1;                       /* RATE_MODE_OFDM */
        r.mcs     = 0;                       /* 6 Mbit/s */
        return r;
    }

    r.phymode = 0;                           /* RATE_MODE_CCK */
    r.mcs     = 0;                           /* 1 Mbit/s if nothing is known */
    for (int i = 0; i < 12; i++) {
        if (rt->basic_rates & (1u << i)) {
            r = rt_rates_bg[i];
            break;
        }
    }

    /* Short preamble is a CCK-only flag carried in the rate field (rt2x00 sets
     * mcs |= 0x08 for IEEE80211_TX_RC_USE_SHORT_PREAMBLE). */
    if (r.phymode == 0 && (rt->erp_flags & CACT80211_ERP_SHORT_PREAMBLE))
        r.mcs |= 0x08;
    return r;
}

/* The TX queue a frame belongs to — rt2x00mac_tx(): management frames go to the
 * MGMT queue, EAPOL and data to best effort.  The queue selects the bulk-OUT
 * endpoint and is reported in the TXWI's PACKETID field. */
u8 rt2800_tx_qid(const u8 *frame, u16 len) {
    u8 type = (u8)((frame[0] >> 2) & 0x3);

    (void)len;
    return type == 0 ? RT_QID_MGMT : RT_QID_AC_BE;
}

/* Ask the chip whether the frame that was just transmitted got its
 * acknowledgement.  The MAC writes one TX_STA_FIFO entry per transmission; for
 * a frame that required an ACK, TX_SUCCESS clear means the frame went out and
 * nothing came back — a failure the caller has to see, so it is returned as an
 * error for the userspace tool to report. */
static int rt2800_tx_wait_status(struct rt_dev *rt, int need_ack) {
    if (!need_ack)
        return 0;

    for (int i = 0; i < RT_TX_STATUS_TIMEOUT_MS / 2; i++) {
        u32 sta = rt2800_register_read(rt, TX_STA_FIFO);

        if (rt2x00_get_field32(sta, TX_STA_FIFO_VALID))
            return rt2x00_get_field32(sta, TX_STA_FIFO_TX_SUCCESS)
                   ? 0 : -RT_ECOMM;
        msleep(2);
    }
    return -RT_ETIMEDOUT;   /* no status at all: the frame never completed */
}

/* Transmit one 802.11 frame: | TXINFO | TXWI | frame | pad to 4 |.
 *
 * The frame goes to the bulk-OUT endpoint of its TX queue (qid), like
 * rt2x00usb's queue-to-endpoint binding.  Returns 0 when the chip transmitted
 * it and, for a frame that asked for an acknowledgement, got it; otherwise a
 * negative errno that the ioctl hands on to userspace. */
int rt2800_tx_frame(struct rt_dev *rt, const u8 *frame, u16 len, u8 qid) {
    usb_device_t *usb = rt->usb;

    if (!rt->tx_ep)
        return -RT_EIO;
    if (!frame || len < 10)
        return -RT_EINVAL;

    u16 padded = (u16)((len + 3) & ~3);
    u16 total  = TXINFO_DESC_SIZE + TXWI_SIZE + padded;
    if (total + 4 > sizeof(rx_buf))
        return -RT_EINVAL;

    static u8 tx_buf[RX_BUFSZ];
    memset(tx_buf, 0, total + 4);

    /* TXINFO word0: length of TXWI+frame+pads (excl. TXINFO), WIV=1
     * (not encrypting in hardware), QSEL=2. */
    u32 txinfo = 0;
    rt2x00_set_field32(&txinfo, TXINFO_W0_USB_DMA_TX_PKT_LEN, total - TXINFO_DESC_SIZE);
    rt2x00_set_field32(&txinfo, TXINFO_W0_WIV, 1);
    rt2x00_set_field32(&txinfo, TXINFO_W0_QSEL, 2);
    memset(tx_buf, 0, TXINFO_DESC_SIZE);
    memcpy(tx_buf, &txinfo, TXINFO_DESC_SIZE);

    /* TXWI: word0 rate (PHYMODE + MCS, rt2800_tx_rate), word1 ACK + sequence +
     * byte count + PACKETID (the queue and entry the frame belongs to, which
     * rt2800_write_tx_data fills from entry->queue->qid). */
    struct rt_tx_rate rate = rt2800_tx_rate(rt, frame, len);
    u32 w0 = 0, w1 = 0;
    rt2x00_set_field32(&w0, TXWI_W0_PHYMODE, rate.phymode);
    rt2x00_set_field32(&w0, TXWI_W0_MCS, rate.mcs);
    /* Broadcast/multicast frames are never acknowledged: asking for an ACK
     * makes the chip retry against a reply that never comes.  Linux sets
     * ENTRY_TXD_ACK only when !IEEE80211_TX_CTL_NO_ACK, which mac80211 clears
     * for group-addressed frames (rt2x00queue.c:407).  The group bit lives on
     * the *destination*: Addr3 for a ToDS frame (the engine always sends ToDS,
     * so its Addr1 is the AP's individual BSSID), Addr1 otherwise. */
    const u8 *da = (len >= 22 && (frame[1] & 0x01)) ? frame + 16 : frame + 4;
    int need_ack = !(da[0] & 0x01);
    rt2x00_set_field32(&w1, TXWI_W1_ACK, need_ack);
    rt2x00_set_field32(&w1, TXWI_W1_NSEQ, 1);
    rt2x00_set_field32(&w1, TXWI_W1_MPDU_TOTAL_BYTE_COUNT, len);
    rt2x00_set_field32(&w1, TXWI_W1_PACKETID_QUEUE, qid);
    rt2x00_set_field32(&w1, TXWI_W1_PACKETID_ENTRY, 1);
    memcpy(tx_buf + TXINFO_DESC_SIZE, &w0, 4);
    memcpy(tx_buf + TXINFO_DESC_SIZE + 4, &w1, 4);

    memcpy(tx_buf + TXINFO_DESC_SIZE + TXWI_SIZE, frame, len);

    /* Drop the statuses of earlier frames so the entry read back after this
     * transfer is this frame's. */
    if (need_ack) {
        for (int i = 0; i < 16; i++) {
            u32 s = rt2800_register_read(rt, TX_STA_FIFO);
            if (!rt2x00_get_field32(s, TX_STA_FIFO_VALID))
                break;
        }
    }

    u8 ep = rt2800_tx_ep_for_qid(rt, qid);
    int n = usb->hc->bulk_transfer(usb->hc, usb, ep, USB_DIR_OUT,
                                   tx_buf, (u16)(total + 4), RT_TX_TIMEOUT_MS);
    if (n < 0)
        return -RT_EIO;

    rt->tx_count++;

    /* Waiting for the chip's TX status costs a few milliseconds, which is worth
     * it for the frames userspace acts on — management (auth/assoc) and EAPOL
     * (the handshake) — but not on the netdev's send() path: there a lost frame
     * is the network stack's business to retransmit. */
    bool want_status = need_ack &&
        (((frame[0] >> 2) & 0x3) == 0 || rt2800_tx_is_eapol(frame, len));
    return rt2800_tx_wait_status(rt, want_status);
}
