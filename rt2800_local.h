#ifndef RT2800_LOCAL_H
#define RT2800_LOCAL_H

/*
 * Shared context and access shims for the rt2800 routines transcribed from the
 * Linux rt2x00 driver.  Keeping the Linux names (rt2800_register_read,
 * rt2x00_set_field32, ...) as macros over this module's primitives lets the
 * transcribed bodies stay close to the reference.
 */

#include "rt2x00_compat.h"
#include "usb.h"
#include "rt2800_regs.h"
#include "cact80211.h"

#define REGISTER_BUSY_COUNT      100
#define REGISTER_USB_BUSY_COUNT  20
#define REGISTER_BUSY_DELAY      100

#define RT_EEPROM_WORDS          256   /* EEPROM_SIZE (0x200) / 2 */

/* Maximum number of bulk-OUT endpoints a dongle may expose (the RT3070 has
 * six: the four AC queues plus MGMT and beacon). */
#define RT_MAX_TX_EPS            6

/* TX queue ids (rt2x00queue.h: enum data_queue_qid).  The value goes into the
 * TXWI's 2-bit PACKETID_QUEUE field exactly as in Linux (rt2800_write_tx_data
 * writes entry->queue->qid), and selects which bulk-OUT endpoint the frame is
 * written to — rt2x00usb_find_endpoints() assigns the OUT endpoints, in
 * descriptor order, to VO, VI, BE, BK, MGMT, beacon. */
#define RT_QID_AC_VO             0
#define RT_QID_AC_VI             1
#define RT_QID_AC_BE             2
#define RT_QID_AC_BK             3
#define RT_QID_MGMT              13

/* Failures are reported to userspace as negative ioctl return values, never as
 * driver output: the connection utility (wljoin) prints what it gets back. */
#define RT_EPERM                 1
#define RT_ENOENT                2
#define RT_EIO                   5
#define RT_ENOMEM                12
#define RT_ENODEV                19
#define RT_EFAULT                14
#define RT_EBUSY                 16
#define RT_EINVAL                22
#define RT_ECOMM                 70   /* transmitted, but no acknowledgement */
#define RT_ENOKEY                126  /* the key a frame needs is not installed */
#define RT_ETIMEDOUT             110

/* mac80211's default retry counts: short frames are retried more often. */
#define RT_SHORT_RETRY_LIMIT     7
#define RT_LONG_RETRY_LIMIT      4

/* Beacon-loss watchdog: treat the link as gone after this long without a beacon
 * from the associated BSSID.  Beacons are ~100 ms apart; the wide margin covers
 * the synchronous RX poll dropping a few under load. */
#define RT_BEACON_LOSS_US        3000000ULL

struct rt_dev {
    usb_device_t *usb;
    u16           rt;       /* chipset id (RT3070, ...) */
    u16           rev;      /* chip revision (REV_RT3070F, ...) */
    u8            rf;       /* RF chip id (RF3020, ...) */
    u8            booted;
    u16           eeprom[RT_EEPROM_WORDS];
    u8            rf_words[64];   /* rt2x00_rf_write() bookkeeping */
    u8            max_psdu;
    u8            txpath;
    u8            rxpath;
    u16           freq;     /* configured channel frequency (MHz) */
    u8            calibration_bw20;
    u8            calibration_bw40;
    u8            bbp25;
    u8            bbp26;
    u8            txmixer_gain_24g;
    bool          external_lna_bg;
    u8            rx_ep;
    u8            tx_ep;
    u8            tx_eps[RT_MAX_TX_EPS];  /* bulk-OUT endpoints, descriptor order */
    u8            tx_ep_count;
    u16           rx_maxpkt;
    u16           tx_maxpkt;
    u32           rx_count;
    u32           tx_count;
    /* Data-path counters for bring-up diagnosis via CACT_WLANCTL_STATUS (the
     * driver never prints): what the netdev handed down, what came back up, and
     * where frames were lost. */
    u32           diag_tx_data;
    u32           diag_tx_group;
    u32           diag_tx_fail;
    u32           diag_rx_data;
    u32           diag_rx_group;
    u32           diag_rx_bad;
    u32           diag_rx_nokey;
    u32           diag_rx_other;
    u8            tx_chain_num;
    u8            rx_chain_num;
    u8            freq_offset;
    s8            lna_gain;      /* EEPROM_LNA_BG, used by per-channel BBP gains */
    u8            mac[6];
    u8            scan_channel;
    u8            bssid[6];
    int           linked;
    int           assoc;
    /* Beacon-loss watchdog: ktime_get_usec() of the last beacon heard from
     * rt->bssid while linked.  If the AP goes quiet the driver clears `linked`
     * so wifid sees the drop and re-runs wljoin, and netdev TX closes. */
    u64           last_beacon_us;
    /* CCMP keys installed from userspace (CACT_WLANCTL_SET_KEY): the 4-way
     * handshake and key derivation live in the wljoin utility, not here.  The
     * pairwise key (TK) protects unicast, the group key (GTK) broadcast and
     * multicast, and the CCMP header's key id says which one a frame used. */
    u8            tk[16];
    u8            gtk[16];
    int           have_tk;
    int           have_gtk;
    u8            gtk_id;
    /* What the AP's beacon advertises (CACT_WLANCTL_SET_RATES): the basic-rate
     * set that management/EAPOL frames must use and the ERP timings.  Set from
     * userspace before the first frame goes out, like mac80211's bss_conf. */
    u16           basic_rates;
    u16           erp_flags;
    /* Last failure, for the errors that have no ioctl to return from (the
     * radio bring-up in probe): read back through CACT_WLANCTL_STATUS. */
    int           last_error;
    /* Result of the engine's CCMP known-answer test, exposed through
     * CACT_WLANCTL_STATUS (the engine runs the test, not the driver). */
    int           ccmp_ok;
    /* Our registration with the in-kernel 802.11 engine (rt2800_cact80211.c):
     * the cact80211_hw the engine hands back to cact80211_rx. */
    void         *hw;
};

/* Register / EEPROM / MCU primitives — implemented in rt2800usb.c. */
u32  rt_reg_read (usb_device_t *usb, u16 offset);
void rt_reg_write(usb_device_t *usb, u16 offset, u32 value);
int  rt_reg_multiwrite(usb_device_t *usb, u16 offset, const void *buf, u16 len);
int  rt_eeprom_read(usb_device_t *usb, u16 *words, u16 nwords);
void rt_mcu_request(usb_device_t *usb, u8 command, u8 token, u8 arg0, u8 arg1);
int  rt_wait_csr_ready(usb_device_t *usb);
void rt_disable_wpdma(usb_device_t *usb);

extern void ktime_busy_wait_us(u64 us);
extern u64  ktime_get_usec(void);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *dst, const void *src, unsigned int n);
extern int   memcmp(const void *a, const void *b, unsigned int n);

#define msleep(ms) ktime_busy_wait_us((u64)(ms) * 1000)
#define udelay(us) ktime_busy_wait_us((u64)(us))

/* Linux-name shims over the primitives above. */
#define rt2800_register_read(dev, off)          rt_reg_read((dev)->usb, (off))
#define rt2800_register_write(dev, off, v)      rt_reg_write((dev)->usb, (off), (v))
#define rt2800_register_read_lock(dev, off)     rt_reg_read((dev)->usb, (off))
#define rt2800_register_write_lock(dev, off, v) rt_reg_write((dev)->usb, (off), (v))
#define rt2800_register_multiwrite(dev, off, buf, len) \
        rt_reg_multiwrite((dev)->usb, (off), (buf), (len))
#define rt2x00_rf_write(dev, word, val)         ((dev)->rf_words[(word) & 63] = (u8)(val))

/* EEPROM read through the word map (rt2800_init.c). */
u16 rt2800_eeprom_read(struct rt_dev *dev, const unsigned int word);
u16 rt2800_eeprom_read_from_array(struct rt_dev *dev, const unsigned int word,
                                  unsigned int index);

/* BBP / RF access + busy read (rt2800_init.c). */
int  rt2800_regbusy_read(struct rt_dev *dev, const unsigned int offset,
                         const struct rt2x00_field32 field, u32 *reg);
void rt2800_bbp_write(struct rt_dev *dev, const unsigned int word, const u8 value);
u8   rt2800_bbp_read (struct rt_dev *dev, const unsigned int word);
void rt2800_rfcsr_write(struct rt_dev *dev, const unsigned int word, const u8 value);
u8   rt2800_rfcsr_read (struct rt_dev *dev, const unsigned int word);
void rt2800_rf_write(struct rt_dev *dev, const unsigned int word, const u32 value);

/* USB transport + MAC init (rt2800usb.c / rt2800_mac.c). */
int  rt_vendor_request(usb_device_t *dev, u8 request, u8 requesttype,
                       u16 offset, u16 value, void *buffer, u16 length,
                       u32 timeout_ms);
int  rt2800_init_registers(struct rt_dev *dev);
int  rt2800_wait_wpdma_ready(struct rt_dev *dev);
/* Receive filter flags (rt2x00's FIF_*), shared by rt2800_mac.c and the MLME. */
#define FIF_ALLMULTI  0x00002000

void rt2800_config_filter(struct rt_dev *dev, const unsigned int filter_flags);
int  rt2800_enable_radio(struct rt_dev *dev);

/* Interface/link configuration (rt2800_mac.c): the STA part of Linux's
 * rt2800_config_intf, rt2800_config_retry_limit and rt2800_config_erp. */
void rt2800_config_intf_sta(struct rt_dev *dev);
void rt2800_config_retry_limit(struct rt_dev *dev, u8 short_rty, u8 long_rty);
void rt2800_config_erp(struct rt_dev *dev, u16 basic_rates, u16 erp_flags);
void rt2800_config_edca(struct rt_dev *dev);

/* USB TX/RX rings (rt2800_usb_rings.c). */
int  rt2800_find_endpoints(struct rt_dev *dev);
int  rt2800_rx_poll(struct rt_dev *dev);
int  rt2800_tx_frame(struct rt_dev *dev, const u8 *frame, u16 len, u8 qid);
int  rt2800_set_channel(struct rt_dev *dev, u8 channel);
void rt2800_config_ant(struct rt_dev *dev);

/* cact80211 glue (rt2800_cact80211.c): register the radio with the in-kernel
 * 802.11 engine.  The engine drives the radio through the ops table and owns the
 * netdev, the scan/auth/assoc MLME and the CCMP datapath; the driver is a dumb
 * radio. */
int  rt2800_wlan_register(struct rt_dev *dev);
void rt2800_wlan_unregister(struct rt_dev *dev);

#endif /* RT2800_LOCAL_H */
