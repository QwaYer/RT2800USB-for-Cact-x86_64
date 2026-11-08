/*
 * rt2800usb — Ralink RT28xx/RT30xx USB Wi-Fi driver (.cctk module).
 *
 * Loaded with `modload /lib/rt2800usb.cctk`; usb_driver_init() registers the
 * driver for the Ralink USB IDs and probe() runs the bring-up.  Transfers go
 * through the usb_device_t's host-controller ops; the firmware blob is staged
 * out of cctkfs with request_firmware().  Registered drivers are offered the
 * devices already enumerated when the module is loaded, so the dongle may be
 * plugged in before or after modload.
 *
 * Ported from the Linux rt2x00 driver (Projects/linux import/linux/linux-master/
 * drivers/net/wireless/ralink/rt2x00): the USB vendor-request register access
 * from rt2x00usb.{c,h}, the 8051 firmware upload from rt2800usb.c/rt2800lib.c,
 * the MCU mailbox request, and (rt2800_init.c) the BBP/RF indirect access.
 * rt2800_regs.h is a verbatim copy of the Linux rt2800.h.
 */

#include <stdint.h>
#include <stddef.h>
#include "rt2800_local.h"

/* Kernel entry points resolved through the ksym table. */
extern int   usb_driver_register(usb_driver_t *drv);
extern int   usb_driver_unregister(usb_driver_t *drv);
extern int   request_firmware(const char *name, const uint8_t **data, uint32_t *len);
extern void  release_firmware(const uint8_t *data);
extern void  ktime_busy_wait_us(uint64_t us);
extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *dst, const void *src, unsigned int n);

/* ------------------------------------------------------------------ */
/* USB vendor requests (rt2x00usb.h)                                   */
/* ------------------------------------------------------------------ */

#define USB_DEVICE_MODE      1
#define USB_MULTI_WRITE      6
#define USB_MULTI_READ       7
#define USB_EEPROM_READ      9

#define USB_MODE_AUTORUN     17
#define USB_MODE_FIRMWARE    8
#define USB_MODE_WAKEUP      9

#define USB_VENDOR_REQUEST_IN   0xc0
#define USB_VENDOR_REQUEST_OUT  0x40

#define REGISTER_TIMEOUT            100
#define REGISTER_TIMEOUT_FIRMWARE   1000
#define EEPROM_TIMEOUT              2000

#define CSR_CHUNK_SIZE  64

#define FIRMWARE_IMAGE_BASE   0x3000

/* ------------------------------------------------------------------ */
/* Chip / driver tables                                                */
/* ------------------------------------------------------------------ */

#define RT_VENDOR_RALINK  0x148f
#define RT_FW_NAME        "rt2870.bin"

static const uint16_t rt2800usb_ids[] = {
    0x3070,  /* RT3070  */
    0x3071,  /* RT3071  */
    0x3072,  /* RT3072  */
    0x2870,  /* RT2870  */
    0x2770,  /* RT2770  */
    0x3572,  /* RT3572  */
    0x3370,  /* RT3370  */
};

#define RT_NUM_IDS  (sizeof(rt2800usb_ids) / sizeof(rt2800usb_ids[0]))

static usb_driver_t   rt2800usb_drivers[RT_NUM_IDS];
static const uint8_t *rt_fw;
static uint32_t       rt_fw_len;
static int            rt_registered;

static uint8_t      rt_mac[6];

/* ------------------------------------------------------------------ */
/* USB transport                                                       */
/* ------------------------------------------------------------------ */

int rt_vendor_request(usb_device_t *dev, uint8_t request,
                             uint8_t requesttype, uint16_t offset,
                             uint16_t value, void *buffer, uint16_t length,
                             uint32_t timeout_ms) {
    usb_setup_pkt_t setup = {
        .bmRequestType = requesttype,
        .bRequest      = request,
        .wValue        = value,
        .wIndex        = offset,
        .wLength       = length,
    };

    if (!dev || !dev->hc || !dev->hc->control_transfer)
        return -1;

    uint32_t attempts = timeout_ms ? timeout_ms : 1;
    int rc = -1;
    for (uint32_t i = 0; i < attempts; i++) {
        rc = dev->hc->control_transfer(dev->hc, dev, &setup, buffer, length);
        if (rc >= 0)
            return 0;
        ktime_busy_wait_us(1000);
    }
    return rc;
}

/* Buffered request: 64-byte chunks, as rt2x00usb_vendor_request_buff(). */
static int rt_vendor_buff(usb_device_t *dev, uint8_t request,
                          uint8_t requesttype, uint16_t offset,
                          void *buffer, uint16_t length) {
    uint8_t *p   = (uint8_t *)buffer;
    uint16_t off = offset;
    uint16_t len = length;

    while (len) {
        uint16_t chunk = len > CSR_CHUNK_SIZE ? CSR_CHUNK_SIZE : len;
        int rc = rt_vendor_request(dev, request, requesttype, off, 0,
                                   p, chunk, REGISTER_TIMEOUT);
        if (rc < 0)
            return rc;
        p   += chunk;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

u32 rt_reg_read(usb_device_t *usb, u16 offset) {
    u32 reg = 0;
    rt_vendor_buff(usb, USB_MULTI_READ, USB_VENDOR_REQUEST_IN, offset, &reg, 4);
    return reg;
}

void rt_reg_write(usb_device_t *usb, u16 offset, u32 value) {
    rt_vendor_buff(usb, USB_MULTI_WRITE, USB_VENDOR_REQUEST_OUT, offset, &value, 4);
}

int rt_reg_multiwrite(usb_device_t *usb, u16 offset, const void *buf, u16 len) {
    return rt_vendor_buff(usb, USB_MULTI_WRITE, USB_VENDOR_REQUEST_OUT,
                          offset, (void *)buf, len);
}

int rt_eeprom_read(usb_device_t *usb, u16 *words, u16 nwords) {
    return rt_vendor_request(usb, USB_EEPROM_READ, USB_VENDOR_REQUEST_IN, 0, 0,
                             words, (uint16_t)(nwords * 2), EEPROM_TIMEOUT);
}

/* MCU mailbox request — rt2800_mcu_request(). */
void rt_mcu_request(usb_device_t *usb, u8 command, u8 token, u8 arg0, u8 arg1) {
    u32 reg = 0;
    unsigned i;

    /*
     * Wait until the MCU becomes available, afterwards we can safely write
     * the new data into the register (Linux WAIT_FOR_MCU: on timeout the
     * request is dropped rather than clobbering the mailbox).
     */
    for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
        reg = rt_reg_read(usb, H2M_MAILBOX_CSR);
        if (!rt2x00_get_field32(reg, H2M_MAILBOX_CSR_OWNER))
            break;
        udelay(REGISTER_BUSY_DELAY);
    }
    if (i == REGISTER_BUSY_COUNT)
        return;

    rt2x00_set_field32(&reg, H2M_MAILBOX_CSR_OWNER, 1);
    rt2x00_set_field32(&reg, H2M_MAILBOX_CSR_CMD_TOKEN, token);
    rt2x00_set_field32(&reg, H2M_MAILBOX_CSR_ARG0, arg0);
    rt2x00_set_field32(&reg, H2M_MAILBOX_CSR_ARG1, arg1);
    rt_reg_write(usb, H2M_MAILBOX_CSR, reg);

    reg = 0;
    rt2x00_set_field32(&reg, HOST_CMD_CSR_HOST_COMMAND, command);
    rt_reg_write(usb, HOST_CMD_CSR, reg);
}

int rt_wait_csr_ready(usb_device_t *usb) {
    for (unsigned i = 0; i < REGISTER_BUSY_COUNT; i++) {
        u32 reg = rt_reg_read(usb, MAC_CSR0);
        if (reg && reg != 0xffffffffu)
            return 0;
        msleep(1);
    }
    return -1;
}

void rt_disable_wpdma(usb_device_t *usb) {
    u32 reg = rt_reg_read(usb, WPDMA_GLO_CFG);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_TX_DMA, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_TX_DMA_BUSY, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_RX_DMA, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_RX_DMA_BUSY, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_TX_WRITEBACK_DONE, 1);
    rt_reg_write(usb, WPDMA_GLO_CFG, reg);
}

/* ------------------------------------------------------------------ */
/* Firmware                                                            */
/* ------------------------------------------------------------------ */

/* rt2800usb_write_firmware(): RT3070 takes the first 4 KiB section. */
static int rt_write_firmware(usb_device_t *dev, const uint8_t *fw, uint32_t len) {
    u32 autorun = 0;
    if (rt_vendor_request(dev, USB_DEVICE_MODE, USB_VENDOR_REQUEST_IN, 0,
                          USB_MODE_AUTORUN, &autorun, 4,
                          REGISTER_TIMEOUT_FIRMWARE) == 0) {
        if ((autorun & 0x3) == 2)
            return 1;      /* NIC in AutoRun mode: firmware not required */
    }

    u32 length = len < 4096 ? len : 4096;
    if (rt_reg_multiwrite(dev, FIRMWARE_IMAGE_BASE, fw, (u16)length) < 0)
        return -1;

    rt_reg_write(dev, H2M_MAILBOX_CID, 0xffffffffu);
    rt_reg_write(dev, H2M_MAILBOX_STATUS, 0xffffffffu);

    if (rt_vendor_request(dev, USB_DEVICE_MODE, USB_VENDOR_REQUEST_OUT, 0,
                          USB_MODE_FIRMWARE, NULL, 0,
                          REGISTER_TIMEOUT_FIRMWARE) < 0)
        return -1;

    msleep(10);
    rt_reg_write(dev, H2M_MAILBOX_CSR, 0);
    return 0;
}

/* rt2800_load_firmware(): download, boot the MCU, wait for PBF ready. */
static int rt_load_firmware(usb_device_t *dev, const uint8_t *fw, uint32_t len) {
    rt_reg_write(dev, AUTOWAKEUP_CFG, 0x00000000);

    if (rt_wait_csr_ready(dev) != 0)
        return -1;

    rt_disable_wpdma(dev);

    int rc = rt_write_firmware(dev, fw, len);
    if (rc < 0)
        return rc;

    if (rc == 0) {
        unsigned i;
        for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
            u32 reg = rt_reg_read(dev, PBF_SYS_CTRL);
            if (rt2x00_get_field32(reg, PBF_SYS_CTRL_READY))
                break;
            msleep(1);
        }
        if (i == REGISTER_BUSY_COUNT)
            return -1;

        rt_disable_wpdma(dev);
        rt_reg_write(dev, H2M_BBP_AGENT, 0);
        rt_reg_write(dev, H2M_MAILBOX_CSR, 0);
        rt_reg_write(dev, H2M_INT_SRC, 0);
        rt_mcu_request(dev, MCU_BOOT_SIGNAL, 0, 0, 0);
        msleep(1);
    }
    return 0;
}

/* Program the station address (rt2800_config_intf / CONFIG_UPDATE_MAC): the
 * MAC into MAC_ADDR_DW0/DW1 with UNICAST_TO_ME_MASK set, exactly as Linux's
 * rt2800_config_intf does.
 *
 * It has to run AFTER rt2800_enable_radio(), because that calls
 * rt2800_init_registers() -> rt2800usb_init_registers(), which resets the MAC
 * CSR block (MAC_SYS_CTRL_RESET_CSR plus the USB_MODE_RESET vendor request) and
 * clears these registers.  Linux's order is the same: mac80211 calls drv_start()
 * ("radio on") before drv_add_interface(), and the address is written by
 * rt2x00mac_add_interface().
 *
 * Writing it earlier leaves the chip addressed as 00:00:00:00:00:00, and the
 * hardware then drops every frame addressed to us as "not to me": beacons and
 * other broadcast frames still arrive (so the scan looks healthy), while probe
 * responses, the AP's ACK of our own frames and the auth reply are all
 * discarded — "scan works, authentication never gets an answer". */
static void rt2800_program_mac(struct rt_dev *rt) {
    u32 dw0 = 0, dw1 = 0;
    u32 mac[2];
    u32 rd0, rd1;

    rt2x00_set_field32(&dw0, MAC_ADDR_DW0_BYTE0, rt_mac[0]);
    rt2x00_set_field32(&dw0, MAC_ADDR_DW0_BYTE1, rt_mac[1]);
    rt2x00_set_field32(&dw0, MAC_ADDR_DW0_BYTE2, rt_mac[2]);
    rt2x00_set_field32(&dw0, MAC_ADDR_DW0_BYTE3, rt_mac[3]);
    rt2x00_set_field32(&dw1, MAC_ADDR_DW1_BYTE4, rt_mac[4]);
    rt2x00_set_field32(&dw1, MAC_ADDR_DW1_BYTE5, rt_mac[5]);
    rt2x00_set_field32(&dw1, MAC_ADDR_DW1_UNICAST_TO_ME_MASK, 0xff);

    /* Linux writes the two words as one 8-byte block at MAC_ADDR_DW0
     * (rt2800_register_multiwrite(..., conf->mac, sizeof(conf->mac))), so that
     * the address and the unicast-to-me mask in DW1 are programmed together. */
    mac[0] = dw0;
    mac[1] = dw1;
    rt_reg_multiwrite(rt->usb, MAC_ADDR_DW0, mac, sizeof(mac));

    /* The hardware must accept frames addressed to us, or the AP's ACK and its
     * answer to the auth are both dropped as "not to me" while broadcast still
     * arrives — the scan looks fine and the connection never starts.  Check the
     * address read back and report a wipe instead of assuming it away. */
    rd0 = rt_reg_read(rt->usb, MAC_ADDR_DW0);
    rd1 = rt_reg_read(rt->usb, MAC_ADDR_DW1);
    if ((rd0 & 0xffffffffu) != dw0 || (rd1 & 0x0000ffffu) != (dw1 & 0x0000ffffu))
        rt->last_error = -RT_EIO;
}

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

/* Record a bring-up failure; the engine reads it back through the ops table and
 * reports it in CACT_WLANCTL_STATUS (the driver never prints). */
static void rt_fail(struct rt_dev *rt, int err) {
    rt->last_error = err;
}

static int rt2800usb_probe(usb_device_t *dev) {
    struct rt_dev *rt = (struct rt_dev *)kmalloc(sizeof(struct rt_dev));
    if (!rt)
        return -1;
    memset(rt, 0, sizeof(*rt));
    rt->usb = dev;
    dev->driver_priv = rt;

    if (!rt_fw && request_firmware(RT_FW_NAME, &rt_fw, &rt_fw_len) != 0) {
        rt_fail(rt, -RT_ENOENT);          /* firmware not in the image */
        return 0;
    }
    if (!rt_fw) {
        rt_fail(rt, -RT_ENOENT);
        return 0;
    }

    int rc = rt_load_firmware(dev, rt_fw, rt_fw_len);
    if (rc < 0) {
        rt_fail(rt, rc);
        return 0;
    }
    rt->booted = 1;

    u32 csr0 = rt_reg_read(dev, MAC_CSR0);
    rt->rev = (u16)rt2x00_get_field32(csr0, MAC_CSR0_REVISION);
    rt->rt  = (u16)rt2x00_get_field32(csr0, MAC_CSR0_CHIPSET);

    if (rt_eeprom_read(dev, rt->eeprom, RT_EEPROM_WORDS) != 0) {
        rt_fail(rt, -RT_EIO);             /* no EEPROM (efuse-only part?) */
        return 0;
    }

    u16 m0 = rt2800_eeprom_read(rt, EEPROM_MAC_ADDR_0);
    u16 m1 = rt2800_eeprom_read(rt, EEPROM_MAC_ADDR_1);
    u16 m2 = rt2800_eeprom_read(rt, EEPROM_MAC_ADDR_2);
    rt_mac[0] = (uint8_t)(m0 & 0xff);
    rt_mac[1] = (uint8_t)(m0 >> 8);
    rt_mac[2] = (uint8_t)(m1 & 0xff);
    rt_mac[3] = (uint8_t)(m1 >> 8);
    rt_mac[4] = (uint8_t)(m2 & 0xff);
    rt_mac[5] = (uint8_t)(m2 >> 8);

    /* The station address is NOT written here: rt2800_enable_radio() resets the
     * MAC CSR block and would clear it.  See rt2800_program_mac(). */
    memcpy(rt->mac, rt_mac, 6);

    /* Register with the in-kernel 802.11 engine as soon as the address is known:
     * /dev/wlan0 then exists even if the radio bring-up below fails, so the
     * failure stays visible through CACT_WLANCTL_STATUS (the engine owns the
     * node; the driver never prints). */
    if (rt2800_wlan_register(rt) != 0)
        rt->last_error = -RT_EIO;

    u16 tmbg = rt2800_eeprom_read(rt, EEPROM_TXMIXER_GAIN_BG);
    rt->txmixer_gain_24g = (u8)rt2x00_get_field16(tmbg, EEPROM_TXMIXER_GAIN_BG_VAL);
    u16 nc1 = rt2800_eeprom_read(rt, EEPROM_NIC_CONF1);
    rt->external_lna_bg =
        rt2x00_get_field16(nc1, EEPROM_NIC_CONF1_EXTERNAL_LNA_2G) != 0;

    /* LNA gain (2.4 GHz): feeds the per-channel RX AGC BBP62/63/64. */
    u16 lna = rt2800_eeprom_read(rt, EEPROM_LNA);
    rt->lna_gain = (s8)rt2x00_get_field16(lna, EEPROM_LNA_BG);

    u16 nc0 = rt2800_eeprom_read(rt, EEPROM_NIC_CONF0);
    /* EEPROM_NIC_CONF0_TXPATH/RXPATH already hold the chain *count*
     * (rt2800.h: "TXPATH: 1: 1T, 2: 2T, 3: 3T") and Linux uses them as-is.
     * Adding one here turned this 1T1R RF3020 (rf_type=5, both fields 1) into
     * "2T2R", which made rt2800_config_channel_rf3xxx leave RFCSR1's RX1/TX1
     * chains powered up — chains the front end does not have. */
    rt->tx_chain_num = (u8)rt2x00_get_field16(nc0, EEPROM_NIC_CONF0_TXPATH);
    rt->rx_chain_num = (u8)rt2x00_get_field16(nc0, EEPROM_NIC_CONF0_RXPATH);

    /* Crystal frequency offset: EEPROM_FREQ's low byte, programmed into
     * RFCSR23_FREQ_OFFSET on every channel change (rt2800_config_channel_rf3xxx).
     * Linux reads it during probe (rt2800lib.c:11302) after its EEPROM
     * validator turns a blank 0xff into 0.  Hardcoding 0 here left the LO off by
     * the dongle's own crystal trim — the receiver hears energy but never locks
     * a PLCP, and the AP never decodes our probe requests. */
    u16 freq = rt2800_eeprom_read(rt, EEPROM_FREQ);
    rt->freq_offset = (u8)rt2x00_get_field16(freq, EEPROM_FREQ_OFFSET);
    if (rt->freq_offset == 0xff)
        rt->freq_offset = 0;

    /* Wake the MCU the way rt2800usb_set_state(STATE_AWAKE) does: an
     * MCU_WAKEUP mailbox request, not a USB_DEVICE_MODE vendor request. */
    rt_mcu_request(rt->usb, MCU_WAKEUP, 0xff, 0, 2);
    ktime_busy_wait_us(1000);

    if (rt2800_enable_radio(rt) != 0) {
        rt_fail(rt, -RT_EIO);             /* radio stays down; STATUS says so */
        return 0;
    }

    /* Linux's interface-up order: radio on, then the interface type (TSF sync
     * to the AP), the antenna, the station address — which has to come after
     * enable_radio(), whose CSR reset would otherwise wipe it — and finally the
     * medium-access parameters and retry limits mac80211 configures. */
    rt2800_config_intf_sta(rt);
    rt2800_config_edca(rt);
    rt2800_config_ant(rt);
    rt2800_program_mac(rt);
    rt2800_config_retry_limit(rt, RT_SHORT_RETRY_LIMIT, RT_LONG_RETRY_LIMIT);

    if (rt2800_find_endpoints(rt) != 0) {
        rt_fail(rt, -RT_ENODEV);          /* no bulk endpoints: no rings */
        return 0;
    }

    /* Let every frame that passed the FCS check through, so the engine's scan
     * sees beacons and probe responses.  The engine is already registered (see
     * above, right after the MAC read); this only arms the RX filter. */
    rt2800_config_filter(rt, FIF_ALLMULTI);

    return 0;
}

static void rt2800usb_remove(usb_device_t *dev) {
    struct rt_dev *rt = (struct rt_dev *)dev->driver_priv;

    if (rt) {
        rt2800_wlan_unregister(rt);
        kfree(rt);
        dev->driver_priv = NULL;
    }
}

int usb_driver_init(void) {
    if (rt_registered)
        return 0;

    for (unsigned i = 0; i < RT_NUM_IDS; i++) {
        usb_driver_t *d = &rt2800usb_drivers[i];
        d->name       = "rt2800usb";
        d->class_code = USB_CLASS_VENDOR;
        d->subclass   = 0xFF;
        d->protocol   = 0xFF;
        d->probe      = rt2800usb_probe;
        d->remove     = rt2800usb_remove;
        d->id_vendor  = RT_VENDOR_RALINK;
        d->id_product = rt2800usb_ids[i];
        usb_driver_register(d);
    }
    rt_registered = 1;
    return 0;
}

void usb_driver_exit(void) {
    for (unsigned i = 0; i < RT_NUM_IDS; i++)
        usb_driver_unregister(&rt2800usb_drivers[i]);
    rt_registered = 0;

    if (rt_fw) {
        release_firmware(rt_fw);
        rt_fw = NULL;
        rt_fw_len = 0;
    }
}
