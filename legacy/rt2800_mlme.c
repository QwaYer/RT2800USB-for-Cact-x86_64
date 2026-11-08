/*
 * rt2800usb scan: walk the 2.4 GHz channels, send probe requests and keep the
 * AP list.
 *
 * The driver no longer associates or runs WPA2 — that is wljoin's job in
 * userspace (see /dev/wlan0 CACT_WLANCTL_* and CactUserBins/wljoin).  The scan
 * and the AP list stay here: read(/dev/wlan0) serves the list as text, and the
 * scan is also what primes the radio's per-channel state.
 */

#include "rt2800_local.h"

#define MAX_AP 64

struct ap_info {
    u8 bssid[6];
    u8 ssid[33];
    u8 ssid_len;
    u8 channel;
    s8 rssi;
    u8 rsn[64];
    u8 rsn_len;
    u8 privacy;
};

static struct ap_info aps[MAX_AP];
static int ap_count;

int rt2800_scan_count(void) { return ap_count; }

static void rt2800_ap_record(const u8 *bssid, const u8 *ssid, u8 ssid_len,
                             u8 channel, s8 rssi, const u8 *rsn, u8 rsn_len,
                             u8 privacy) {
    if (ssid_len > 32)
        ssid_len = 32;

    for (int i = 0; i < ap_count; i++) {
        if (memcmp(aps[i].bssid, bssid, 6) == 0) {
            aps[i].channel = channel;
            aps[i].rssi = rssi;
            aps[i].privacy = privacy;
            /* A hidden SSID beacon (SSID length 0) must not pin the entry to an
             * empty name for good: a later frame that carries the real SSID
             * fills it in. */
            if (aps[i].ssid_len == 0 && ssid_len > 0) {
                memcpy(aps[i].ssid, ssid, ssid_len);
                aps[i].ssid[ssid_len] = 0;
                aps[i].ssid_len = ssid_len;
            }
            if (rsn_len && rsn_len <= 64) {
                memcpy(aps[i].rsn, rsn, rsn_len);
                aps[i].rsn_len = rsn_len;
            }
            return;
        }
    }
    if (ap_count >= MAX_AP)
        return;

    struct ap_info *a = &aps[ap_count++];
    memcpy(a->bssid, bssid, 6);
    memset(a->ssid, 0, sizeof(a->ssid));
    memcpy(a->ssid, ssid, ssid_len);
    a->ssid_len = ssid_len;
    a->channel = channel;
    a->rssi = rssi;
    a->privacy = privacy;
    if (rsn_len && rsn_len <= 64) {
        memcpy(a->rsn, rsn, rsn_len);
        a->rsn_len = rsn_len;
    }
}

/* Update the AP list from a beacon / probe response (IE 0 = SSID, IE 3 =
 * channel, IE 48 = RSN). */
void rt2800_scan_frame(struct rt_dev *rt, const u8 *f, int len) {
    const u8 *bssid = f + 16;
    const u8 *ssid = NULL;
    const u8 *rsn = NULL;
    u8 ssid_len = 0, rsn_len = 0, channel = rt->scan_channel, privacy;
    int off = 24 + 12;

    if (len < 24 + 12)
        return;
    privacy = (u8)((f[34] & 0x10) != 0);   /* capability: privacy bit */

    while (off + 2 <= len) {
        u8 id = f[off];
        u8 ilen = f[off + 1];
        if (off + 2 + ilen > len)
            break;
        if (id == 0) {
            ssid = f + off + 2;
            ssid_len = ilen;
        } else if (id == 3 && ilen >= 1) {
            channel = f[off + 2];
        } else if (id == 48) {
            rsn = f + off + 2;
            rsn_len = ilen;
        }
        off += 2 + ilen;
    }

    if (!ssid)
        return;
    rt2800_ap_record(bssid, ssid, ssid_len, channel, 0, rsn, rsn_len, privacy);
}

/* Per-channel dwell of the scan.  Rather than polling a fixed number of times,
 * frames are drained until the channel goes quiet: a beacon arrives about every
 * 100 ms, so a channel with an AP keeps resetting the counter, while an empty
 * channel costs only the quiet window. */
#define SCAN_POLLS_MAX 50       /* safety cap: at most ~1 s on a busy channel */
#define SCAN_QUIET     5        /* ~100 ms without a frame ends the dwell */
#define SCAN_REPROBE   10       /* re-send the probe request every ~200 ms */

int rt2800_scan(struct rt_dev *rt) {
    ap_count = 0;

    /* Let only frames that passed the hardware FCS check through. */
    rt2800_config_filter(rt, FIF_ALLMULTI);

    for (u8 ch = 1; ch <= 13; ch++) {
        rt->scan_channel = ch;
        int rc = rt2800_set_channel(rt, ch);
        if (rc < 0)
            return rc;          /* the radio is not usable: report, do not scan */
        rt2800_send_probe_request(rt);

        int quiet = 0;
        for (int i = 0; i < SCAN_POLLS_MAX && quiet < SCAN_QUIET; i++) {
            /* A busy AP may not answer a single probe request; re-asking during
             * the dwell is free on an idle channel and rescues discovery on a
             * crowded one. */
            if (i && (i % SCAN_REPROBE) == 0)
                rt2800_send_probe_request(rt);
            quiet = (rt2800_rx_poll(rt) > 0) ? 0 : quiet + 1;
        }
    }

    return ap_count;
}

/* The scan list as text, one AP per line — what /dev/wlan0's read() serves.
 * wljoin parses the same frames itself; this is for a human `cat`. */
int rt2800_scan_list(char *buf, int size) {
    int n = 0;

    for (int i = 0; i < ap_count; i++) {
        struct ap_info *a = &aps[i];
        int w = snprintf(buf + n, (size_t)(size - n),
                         "%02x:%02x:%02x:%02x:%02x:%02x ch=%u %s %s\n",
                         a->bssid[0], a->bssid[1], a->bssid[2],
                         a->bssid[3], a->bssid[4], a->bssid[5],
                         (unsigned)a->channel, a->privacy ? "WPA2" : "open",
                         a->ssid);
        if (w <= 0 || w >= size - n)
            break;
        n += w;
    }
    return n;
}
