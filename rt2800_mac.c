/*
 * rt2800 MAC initialisation, transcribed from the Linux rt2x00 driver
 * (rt2800lib.c: rt2800_init_registers, rt2800_wait_wpdma_ready,
 * rt2800_config_wcid, rt2800_delete_wcid_attr, rt2800_config_filter, and
 * rt2800usb.c: rt2800usb_init_registers).  Bodies are kept close to the
 * reference; only rt2x00dev -> struct rt_dev * and the locking differ.
 */

#include "rt2800_local.h"

#define ETH_ALEN 6

/* USB vendor request bits (rt2x00usb.h) used by rt2800usb_init_registers. */
#define USB_DEVICE_MODE         1
#define USB_MODE_RESET          1
#define USB_VENDOR_REQUEST_OUT  0x40
#define REGISTER_TIMEOUT        100

extern int rt_vendor_request(usb_device_t *dev, u8 request, u8 requesttype,
                             u16 offset, u16 value, void *buffer,
                             u16 length, u32 timeout_ms);

/* Filter flags (include/linux/netdevice.h). */
#define FIF_FCSFAIL   0x00000001
#define FIF_PLCPFAIL  0x00000002
#define FIF_CONTROL   0x00000004
#define FIF_PSPOLL    0x00001000

#define AGGREGATION_SIZE 3840
#define IEEE80211_MAX_RTS_THRESHOLD 2353

int rt2800_wait_wpdma_ready(struct rt_dev *rt) {
    unsigned int i;
    u32 reg = 0;

    for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
        reg = rt2800_register_read(rt, WPDMA_GLO_CFG);
        if (!rt2x00_get_field32(reg, WPDMA_GLO_CFG_TX_DMA_BUSY) &&
            !rt2x00_get_field32(reg, WPDMA_GLO_CFG_RX_DMA_BUSY))
            return 0;
        msleep(10);
    }
    (void)reg;
    return -1;
}

/* rt2800usb_init_registers(): reset the MAC/BBP via USB vendor request. */
static int rt2800_usb_init_registers(struct rt_dev *rt) {
    u32 reg;

    if (rt_wait_csr_ready(rt->usb))
        return -1;

    reg = rt2800_register_read(rt, PBF_SYS_CTRL);
    rt2800_register_write(rt, PBF_SYS_CTRL, reg & ~0x00002000);

    reg = 0;
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_RESET_CSR, 1);
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_RESET_BBP, 1);
    rt2800_register_write(rt, MAC_SYS_CTRL, reg);

    rt_vendor_request(rt->usb, USB_DEVICE_MODE, USB_VENDOR_REQUEST_OUT, 0,
                      USB_MODE_RESET, NULL, 0, REGISTER_TIMEOUT);

    rt2800_register_write(rt, MAC_SYS_CTRL, 0x00000000);

    return 0;
}

static void rt2800_config_wcid(struct rt_dev *rt, const u8 *address, int wcid) {
    struct mac_wcid_entry wcid_entry;
    u32 offset;

    offset = MAC_WCID_ENTRY(wcid);

    memset(&wcid_entry, 0xff, sizeof(wcid_entry));
    if (address)
        memcpy(wcid_entry.mac, address, ETH_ALEN);

    rt2800_register_multiwrite(rt, offset, &wcid_entry, sizeof(wcid_entry));
}

static void rt2800_delete_wcid_attr(struct rt_dev *rt, int wcid) {
    u32 offset = MAC_WCID_ATTR_ENTRY(wcid);
    rt2800_register_write(rt, offset, 0);
}

void rt2800_config_filter(struct rt_dev *rt, const unsigned int filter_flags) {
    u32 reg;

    reg = rt2800_register_read(rt, RX_FILTER_CFG);
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_CRC_ERROR,
                       !(filter_flags & FIF_FCSFAIL));
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_PHY_ERROR,
                       !(filter_flags & FIF_PLCPFAIL));
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_NOT_TO_ME, 1);
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_NOT_MY_BSSD, 0);
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_VER_ERROR, 1);
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_MULTICAST,
                       !(filter_flags & FIF_ALLMULTI));
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_BROADCAST, 0);
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_DUPLICATE, 1);
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_CF_END_ACK,
                       !(filter_flags & FIF_CONTROL));
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_CF_END,
                       !(filter_flags & FIF_CONTROL));
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_ACK,
                       !(filter_flags & FIF_CONTROL));
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_CTS,
                       !(filter_flags & FIF_CONTROL));
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_RTS,
                       !(filter_flags & FIF_CONTROL));
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_PSPOLL,
                       !(filter_flags & FIF_PSPOLL));
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_BA, 0);
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_BAR,
                       !(filter_flags & FIF_CONTROL));
    rt2x00_set_field32(&reg, RX_FILTER_CFG_DROP_CNTL,
                       !(filter_flags & FIF_CONTROL));
    rt2800_register_write(rt, RX_FILTER_CFG, reg);
}

/* rt2800_init_registers() — the MAC register block. */
int rt2800_init_registers(struct rt_dev *rt) {
    u32 reg;
    u16 eeprom;
    unsigned int i;
    int ret;

    rt_disable_wpdma(rt->usb);

    ret = rt2800_usb_init_registers(rt);
    if (ret)
        return ret;

    if (rt2x00_rt(rt, RT6352)) {
        rt2800_register_write(rt, MAC_SYS_CTRL, 0x01);

        u8 bbp = rt2800_bbp_read(rt, 21);
        bbp |= 0x01;
        rt2800_bbp_write(rt, 21, bbp);
        bbp = rt2800_bbp_read(rt, 21);
        bbp &= (~0x01);
        rt2800_bbp_write(rt, 21, bbp);

        rt2800_register_write(rt, MAC_SYS_CTRL, 0x00);
    }

    rt2800_register_write(rt, LEGACY_BASIC_RATE, 0x0000013f);
    rt2800_register_write(rt, HT_BASIC_RATE, 0x00008003);

    rt2800_register_write(rt, MAC_SYS_CTRL, 0x00000000);

    reg = rt2800_register_read(rt, BCN_TIME_CFG);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_BEACON_INTERVAL, 1600);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_TSF_TICKING, 0);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_TSF_SYNC, 0);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_TBTT_ENABLE, 0);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_BEACON_GEN, 0);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_TX_TIME_COMPENSATE, 0);
    rt2800_register_write(rt, BCN_TIME_CFG, reg);

    rt2800_config_filter(rt, FIF_ALLMULTI);

    reg = rt2800_register_read(rt, BKOFF_SLOT_CFG);
    rt2x00_set_field32(&reg, BKOFF_SLOT_CFG_SLOT_TIME, 9);
    rt2x00_set_field32(&reg, BKOFF_SLOT_CFG_CC_DELAY_TIME, 2);
    rt2800_register_write(rt, BKOFF_SLOT_CFG, reg);

    if (rt2x00_rt(rt, RT3290)) {
        reg = rt2800_register_read(rt, WLAN_FUN_CTRL);
        if (rt2x00_get_field32(reg, WLAN_EN) == 1) {
            rt2x00_set_field32(&reg, PCIE_APP0_CLK_REQ, 1);
            rt2800_register_write(rt, WLAN_FUN_CTRL, reg);
        }

        reg = rt2800_register_read(rt, CMB_CTRL);
        if (!(rt2x00_get_field32(reg, LDO0_EN) == 1)) {
            rt2x00_set_field32(&reg, LDO0_EN, 1);
            rt2x00_set_field32(&reg, LDO_BGSEL, 3);
            rt2800_register_write(rt, CMB_CTRL, reg);
        }

        reg = rt2800_register_read(rt, OSC_CTRL);
        rt2x00_set_field32(&reg, OSC_ROSC_EN, 1);
        rt2x00_set_field32(&reg, OSC_CAL_REQ, 1);
        rt2x00_set_field32(&reg, OSC_REF_CYCLE, 0x27);
        rt2800_register_write(rt, OSC_CTRL, reg);

        reg = rt2800_register_read(rt, COEX_CFG0);
        rt2x00_set_field32(&reg, COEX_CFG_ANT, 0x5e);
        rt2800_register_write(rt, COEX_CFG0, reg);

        reg = rt2800_register_read(rt, COEX_CFG2);
        rt2x00_set_field32(&reg, BT_COEX_CFG1, 0x00);
        rt2x00_set_field32(&reg, BT_COEX_CFG0, 0x17);
        rt2x00_set_field32(&reg, WL_COEX_CFG1, 0x93);
        rt2x00_set_field32(&reg, WL_COEX_CFG0, 0x7f);
        rt2800_register_write(rt, COEX_CFG2, reg);

        reg = rt2800_register_read(rt, PLL_CTRL);
        rt2x00_set_field32(&reg, PLL_CONTROL, 1);
        rt2800_register_write(rt, PLL_CTRL, reg);
    }

    if (rt2x00_rt(rt, RT3071) || rt2x00_rt(rt, RT3090) ||
        rt2x00_rt(rt, RT3290) || rt2x00_rt(rt, RT3390)) {

        if (rt2x00_rt(rt, RT3290))
            rt2800_register_write(rt, TX_SW_CFG0, 0x00000404);
        else
            rt2800_register_write(rt, TX_SW_CFG0, 0x00000400);

        rt2800_register_write(rt, TX_SW_CFG1, 0x00000000);
        if (rt2x00_rt_rev_lt(rt, RT3071, REV_RT3071E) ||
            rt2x00_rt_rev_lt(rt, RT3090, REV_RT3090E) ||
            rt2x00_rt_rev_lt(rt, RT3390, REV_RT3390E)) {
            eeprom = rt2800_eeprom_read(rt, EEPROM_NIC_CONF1);
            if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF1_DAC_TEST))
                rt2800_register_write(rt, TX_SW_CFG2, 0x0000002c);
            else
                rt2800_register_write(rt, TX_SW_CFG2, 0x0000000f);
        } else {
            rt2800_register_write(rt, TX_SW_CFG2, 0x00000000);
        }
    } else if (rt2x00_rt(rt, RT3070)) {
        rt2800_register_write(rt, TX_SW_CFG0, 0x00000400);

        if (rt2x00_rt_rev_lt(rt, RT3070, REV_RT3070F)) {
            rt2800_register_write(rt, TX_SW_CFG1, 0x00000000);
            rt2800_register_write(rt, TX_SW_CFG2, 0x0000002c);
        } else {
            rt2800_register_write(rt, TX_SW_CFG1, 0x00080606);
            rt2800_register_write(rt, TX_SW_CFG2, 0x00000000);
        }
    } else if (rt2x00_rt(rt, RT3352)) {
        rt2800_register_write(rt, TX_SW_CFG0, 0x00000402);
        rt2800_register_write(rt, TX_SW_CFG1, 0x00080606);
        rt2800_register_write(rt, TX_SW_CFG2, 0x00000000);
    } else if (rt2x00_rt(rt, RT3572)) {
        rt2800_register_write(rt, TX_SW_CFG0, 0x00000400);
        rt2800_register_write(rt, TX_SW_CFG1, 0x00080606);
    } else {
        rt2800_register_write(rt, TX_SW_CFG0, 0x00000000);
        rt2800_register_write(rt, TX_SW_CFG1, 0x00080606);
    }

    reg = rt2800_register_read(rt, TX_LINK_CFG);
    rt2x00_set_field32(&reg, TX_LINK_CFG_REMOTE_MFB_LIFETIME, 32);
    rt2x00_set_field32(&reg, TX_LINK_CFG_MFB_ENABLE, 0);
    rt2x00_set_field32(&reg, TX_LINK_CFG_REMOTE_UMFS_ENABLE, 0);
    rt2x00_set_field32(&reg, TX_LINK_CFG_TX_MRQ_EN, 0);
    rt2x00_set_field32(&reg, TX_LINK_CFG_TX_RDG_EN, 0);
    rt2x00_set_field32(&reg, TX_LINK_CFG_TX_CF_ACK_EN, 1);
    rt2x00_set_field32(&reg, TX_LINK_CFG_REMOTE_MFB, 0);
    rt2x00_set_field32(&reg, TX_LINK_CFG_REMOTE_MFS, 0);
    rt2800_register_write(rt, TX_LINK_CFG, reg);

    reg = rt2800_register_read(rt, TX_TIMEOUT_CFG);
    rt2x00_set_field32(&reg, TX_TIMEOUT_CFG_MPDU_LIFETIME, 9);
    rt2x00_set_field32(&reg, TX_TIMEOUT_CFG_RX_ACK_TIMEOUT, 32);
    rt2x00_set_field32(&reg, TX_TIMEOUT_CFG_TX_OP_TIMEOUT, 10);
    rt2800_register_write(rt, TX_TIMEOUT_CFG, reg);

    reg = rt2800_register_read(rt, MAX_LEN_CFG);
    rt2x00_set_field32(&reg, MAX_LEN_CFG_MAX_MPDU, AGGREGATION_SIZE);
    rt->max_psdu = 3;   /* USB */
    rt2x00_set_field32(&reg, MAX_LEN_CFG_MAX_PSDU, rt->max_psdu);
    rt2x00_set_field32(&reg, MAX_LEN_CFG_MIN_PSDU, 10);
    rt2x00_set_field32(&reg, MAX_LEN_CFG_MIN_MPDU, 10);
    rt2800_register_write(rt, MAX_LEN_CFG, reg);

    reg = rt2800_register_read(rt, LED_CFG);
    rt2x00_set_field32(&reg, LED_CFG_ON_PERIOD, 70);
    rt2x00_set_field32(&reg, LED_CFG_OFF_PERIOD, 30);
    rt2x00_set_field32(&reg, LED_CFG_SLOW_BLINK_PERIOD, 3);
    rt2x00_set_field32(&reg, LED_CFG_R_LED_MODE, 3);
    rt2x00_set_field32(&reg, LED_CFG_G_LED_MODE, 3);
    rt2x00_set_field32(&reg, LED_CFG_Y_LED_MODE, 3);
    rt2x00_set_field32(&reg, LED_CFG_LED_POLAR, 1);
    rt2800_register_write(rt, LED_CFG, reg);

    rt2800_register_write(rt, PBF_MAX_PCNT, 0x1f3fbf9f);

    reg = rt2800_register_read(rt, TX_RTY_CFG);
    rt2x00_set_field32(&reg, TX_RTY_CFG_SHORT_RTY_LIMIT, 2);
    rt2x00_set_field32(&reg, TX_RTY_CFG_LONG_RTY_LIMIT, 2);
    rt2x00_set_field32(&reg, TX_RTY_CFG_LONG_RTY_THRE, 2000);
    rt2x00_set_field32(&reg, TX_RTY_CFG_NON_AGG_RTY_MODE, 0);
    rt2x00_set_field32(&reg, TX_RTY_CFG_AGG_RTY_MODE, 0);
    rt2x00_set_field32(&reg, TX_RTY_CFG_TX_AUTO_FB_ENABLE, 1);
    rt2800_register_write(rt, TX_RTY_CFG, reg);

    reg = rt2800_register_read(rt, AUTO_RSP_CFG);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_AUTORESPONDER, 1);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_BAC_ACK_POLICY, 1);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_CTS_40_MMODE, 1);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_CTS_40_MREF, 0);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_AR_PREAMBLE, 0);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_DUAL_CTS_EN, 0);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_ACK_CTS_PSM_BIT, 0);
    rt2800_register_write(rt, AUTO_RSP_CFG, reg);

    reg = rt2800_register_read(rt, CCK_PROT_CFG);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_PROTECT_RATE, 3);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_PROTECT_CTRL, 0);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_CCK, 1);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_MM40, 0);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_TX_OP_ALLOW_GF40, 0);
    rt2x00_set_field32(&reg, CCK_PROT_CFG_RTS_TH_EN, 0);
    rt2800_register_write(rt, CCK_PROT_CFG, reg);

    reg = rt2800_register_read(rt, OFDM_PROT_CFG);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_PROTECT_RATE, 3);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_PROTECT_CTRL, 0);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_CCK, 1);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_MM40, 0);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_TX_OP_ALLOW_GF40, 0);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_RTS_TH_EN, 0);
    rt2800_register_write(rt, OFDM_PROT_CFG, reg);

    reg = rt2800_register_read(rt, MM20_PROT_CFG);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_PROTECT_RATE, 0x4004);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_PROTECT_CTRL, 1);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_CCK, 0);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_MM40, 0);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_TX_OP_ALLOW_GF40, 0);
    rt2x00_set_field32(&reg, MM20_PROT_CFG_RTS_TH_EN, 0);
    rt2800_register_write(rt, MM20_PROT_CFG, reg);

    reg = rt2800_register_read(rt, MM40_PROT_CFG);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_PROTECT_RATE, 0x4084);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_PROTECT_CTRL, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_CCK, 0);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_MM40, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_TX_OP_ALLOW_GF40, 1);
    rt2x00_set_field32(&reg, MM40_PROT_CFG_RTS_TH_EN, 0);
    rt2800_register_write(rt, MM40_PROT_CFG, reg);

    reg = rt2800_register_read(rt, GF20_PROT_CFG);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_PROTECT_RATE, 0x4004);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_PROTECT_CTRL, 1);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_CCK, 0);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_MM40, 0);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_TX_OP_ALLOW_GF40, 0);
    rt2x00_set_field32(&reg, GF20_PROT_CFG_RTS_TH_EN, 0);
    rt2800_register_write(rt, GF20_PROT_CFG, reg);

    reg = rt2800_register_read(rt, GF40_PROT_CFG);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_PROTECT_RATE, 0x4084);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_PROTECT_CTRL, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_PROTECT_NAV_SHORT, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_CCK, 0);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_OFDM, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_MM20, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_MM40, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_GF20, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_TX_OP_ALLOW_GF40, 1);
    rt2x00_set_field32(&reg, GF40_PROT_CFG_RTS_TH_EN, 0);
    rt2800_register_write(rt, GF40_PROT_CFG, reg);

    rt2800_register_write(rt, PBF_CFG, 0xf40006);

    reg = rt2800_register_read(rt, WPDMA_GLO_CFG);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_TX_DMA, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_TX_DMA_BUSY, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_RX_DMA, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_RX_DMA_BUSY, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_WP_DMA_BURST_SIZE, 3);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_TX_WRITEBACK_DONE, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_BIG_ENDIAN, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_RX_HDR_SCATTER, 0);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_HDR_SEG_LEN, 0);
    rt2800_register_write(rt, WPDMA_GLO_CFG, reg);

    reg = rt2800_register_read(rt, TXOP_CTRL_CFG);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_TIMEOUT_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_AC_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_TXRATEGRP_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_USER_MODE_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_MIMO_PS_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_RESERVED_TRUN_EN, 1);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_LSIG_TXOP_EN, 0);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_EXT_CCA_EN, 0);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_EXT_CCA_DLY, 88);
    rt2x00_set_field32(&reg, TXOP_CTRL_CFG_EXT_CWMIN, 0);
    rt2800_register_write(rt, TXOP_CTRL_CFG, reg);

    reg = rt2x00_rt(rt, RT5592) ? 0x00000082 : 0x00000002;
    rt2800_register_write(rt, TXOP_HLDR_ET, reg);

    reg = rt2800_register_read(rt, TX_RTS_CFG);
    rt2x00_set_field32(&reg, TX_RTS_CFG_AUTO_RTS_RETRY_LIMIT, 7);
    rt2x00_set_field32(&reg, TX_RTS_CFG_RTS_THRES, IEEE80211_MAX_RTS_THRESHOLD);
    rt2x00_set_field32(&reg, TX_RTS_CFG_RTS_FBK_EN, 1);
    rt2800_register_write(rt, TX_RTS_CFG, reg);

    rt2800_register_write(rt, EXP_ACK_TIME, 0x002400ca);

    reg = rt2800_register_read(rt, XIFS_TIME_CFG);
    rt2x00_set_field32(&reg, XIFS_TIME_CFG_CCKM_SIFS_TIME, 16);
    rt2x00_set_field32(&reg, XIFS_TIME_CFG_OFDM_SIFS_TIME, 16);
    rt2x00_set_field32(&reg, XIFS_TIME_CFG_OFDM_XIFS_TIME, 4);
    rt2x00_set_field32(&reg, XIFS_TIME_CFG_EIFS, 314);
    rt2x00_set_field32(&reg, XIFS_TIME_CFG_BB_RXEND_ENABLE, 1);
    rt2800_register_write(rt, XIFS_TIME_CFG, reg);

    rt2800_register_write(rt, PWR_PIN_CFG, 0x00000003);

    /* Clear the encryption key tables. */
    for (i = 0; i < 4; i++)
        rt2800_register_write(rt, SHARED_KEY_MODE_ENTRY(i), 0);

    for (i = 0; i < 256; i++) {
        rt2800_config_wcid(rt, NULL, i);
        rt2800_delete_wcid_attr(rt, i);
    }

    for (i = 0; i < 256; i++)
        rt2800_register_write(rt, MAC_IVEIV_ENTRY(i), 0);

    /* Beacons are not cleared here yet (they matter only for AP mode). */

    reg = rt2800_register_read(rt, US_CYC_CNT);
    rt2x00_set_field32(&reg, US_CYC_CNT_CLOCK_CYCLE, 30);   /* USB */
    rt2800_register_write(rt, US_CYC_CNT, reg);

    reg = rt2800_register_read(rt, HT_FBK_CFG0);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS0FBK, 0);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS1FBK, 0);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS2FBK, 1);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS3FBK, 2);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS4FBK, 3);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS5FBK, 4);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS6FBK, 5);
    rt2x00_set_field32(&reg, HT_FBK_CFG0_HTMCS7FBK, 6);
    rt2800_register_write(rt, HT_FBK_CFG0, reg);

    reg = rt2800_register_read(rt, HT_FBK_CFG1);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS8FBK, 8);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS9FBK, 8);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS10FBK, 9);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS11FBK, 10);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS12FBK, 11);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS13FBK, 12);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS14FBK, 13);
    rt2x00_set_field32(&reg, HT_FBK_CFG1_HTMCS15FBK, 14);
    rt2800_register_write(rt, HT_FBK_CFG1, reg);

    reg = rt2800_register_read(rt, LG_FBK_CFG0);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS0FBK, 8);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS1FBK, 8);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS2FBK, 9);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS3FBK, 10);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS4FBK, 11);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS5FBK, 12);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS6FBK, 13);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_OFDMMCS7FBK, 14);
    rt2800_register_write(rt, LG_FBK_CFG0, reg);

    reg = rt2800_register_read(rt, LG_FBK_CFG1);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_CCKMCS0FBK, 0);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_CCKMCS1FBK, 0);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_CCKMCS2FBK, 1);
    rt2x00_set_field32(&reg, LG_FBK_CFG0_CCKMCS3FBK, 2);
    rt2800_register_write(rt, LG_FBK_CFG1, reg);

    reg = rt2800_register_read(rt, AMPDU_BA_WINSIZE);
    rt2x00_set_field32(&reg, AMPDU_BA_WINSIZE_FORCE_WINSIZE_ENABLE, 0);
    rt2x00_set_field32(&reg, AMPDU_BA_WINSIZE_FORCE_WINSIZE, 0);
    rt2800_register_write(rt, AMPDU_BA_WINSIZE, reg);

    reg = rt2800_register_read(rt, RX_STA_CNT0);
    reg = rt2800_register_read(rt, RX_STA_CNT1);
    reg = rt2800_register_read(rt, RX_STA_CNT2);
    reg = rt2800_register_read(rt, TX_STA_CNT0);
    reg = rt2800_register_read(rt, TX_STA_CNT1);
    reg = rt2800_register_read(rt, TX_STA_CNT2);

    reg = rt2800_register_read(rt, INT_TIMER_CFG);
    rt2x00_set_field32(&reg, INT_TIMER_CFG_PRE_TBTT_TIMER, 6 << 4);
    rt2800_register_write(rt, INT_TIMER_CFG, reg);

    reg = rt2800_register_read(rt, CH_TIME_CFG);
    rt2x00_set_field32(&reg, CH_TIME_CFG_EIFS_BUSY, 1);
    rt2x00_set_field32(&reg, CH_TIME_CFG_NAV_BUSY, 1);
    rt2x00_set_field32(&reg, CH_TIME_CFG_RX_BUSY, 1);
    rt2x00_set_field32(&reg, CH_TIME_CFG_TX_BUSY, 1);
    rt2x00_set_field32(&reg, CH_TIME_CFG_TMR_EN, 1);
    rt2800_register_write(rt, CH_TIME_CFG, reg);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Link configuration for a station                                    */
/*                                                                     */
/* Linux runs these from mac80211's interface-up and BSS-info callbacks */
/* (rt2800_config_intf, rt2800_config_retry_limit, rt2800_config_erp); */
/* the port runs them when the radio comes up and when wljoin hands over */
/* the AP's beacon parameters.                                         */
/* ------------------------------------------------------------------ */

/* rt2x00reg.h: enum tsf_sync */
#define TSF_SYNC_INFRA  1

/* rt2800_config_intf(), the CONFIG_UPDATE_TYPE part for a station: sync the TSF
 * to the AP's beacons and tune the beacon-queue transmit parameters for the
 * non-AP case.  The station address is programmed by rt2800_program_mac and the
 * BSSID by the engine through the config() op (rt2800_cact80211.c). */
void rt2800_config_intf_sta(struct rt_dev *rt) {
    u32 reg;

    reg = rt2800_register_read(rt, BCN_TIME_CFG);
    rt2x00_set_field32(&reg, BCN_TIME_CFG_TSF_SYNC, TSF_SYNC_INFRA);
    rt2800_register_write(rt, BCN_TIME_CFG, reg);

    reg = rt2800_register_read(rt, TBTT_SYNC_CFG);
    rt2x00_set_field32(&reg, TBTT_SYNC_CFG_BCN_CWMIN, 4);
    rt2x00_set_field32(&reg, TBTT_SYNC_CFG_BCN_AIFSN, 2);
    rt2x00_set_field32(&reg, TBTT_SYNC_CFG_BCN_EXP_WIN, 32);
    rt2x00_set_field32(&reg, TBTT_SYNC_CFG_TBTT_ADJUST, 16);
    rt2800_register_write(rt, TBTT_SYNC_CFG, reg);
}

/* rt2800_config_retry_limit(): the retry counts mac80211 configures. */
void rt2800_config_retry_limit(struct rt_dev *rt, u8 short_rty, u8 long_rty) {
    u32 reg = rt2800_register_read(rt, TX_RTY_CFG);

    rt2x00_set_field32(&reg, TX_RTY_CFG_SHORT_RTY_LIMIT, short_rty);
    rt2x00_set_field32(&reg, TX_RTY_CFG_LONG_RTY_LIMIT, long_rty);
    rt2800_register_write(rt, TX_RTY_CFG, reg);
}

/* rt2800_config_erp(): what the AP's beacon changes about the MAC's timings.
 * basic_rates is the AP's basic-rate bitmap (bit0 = 1 Mbit/s) and erp_flags
 * carries CACT80211_ERP_*. */
void rt2800_config_erp(struct rt_dev *rt, u16 basic_rates, u16 erp_flags) {
    u32 reg;

    if (basic_rates) {
        /* The rates the AP declares basic, plus the fixed HT basic set. */
        rt2800_register_write(rt, LEGACY_BASIC_RATE, 0xff0 | basic_rates);
        rt2800_register_write(rt, HT_BASIC_RATE, 0x00008003);
        rt->basic_rates = basic_rates;
    }

    reg = rt2800_register_read(rt, AUTO_RSP_CFG);
    rt2x00_set_field32(&reg, AUTO_RSP_CFG_AR_PREAMBLE,
                       (erp_flags & CACT80211_ERP_SHORT_PREAMBLE) ? 1 : 0);
    rt2800_register_write(rt, AUTO_RSP_CFG, reg);

    reg = rt2800_register_read(rt, OFDM_PROT_CFG);
    rt2x00_set_field32(&reg, OFDM_PROT_CFG_PROTECT_CTRL,
                       (erp_flags & CACT80211_ERP_CTS_PROT) ? 2 : 0);
    rt2800_register_write(rt, OFDM_PROT_CFG, reg);

    reg = rt2800_register_read(rt, BKOFF_SLOT_CFG);
    rt2x00_set_field32(&reg, BKOFF_SLOT_CFG_SLOT_TIME,
                       (erp_flags & CACT80211_ERP_SHORT_SLOT) ? 9 : 20);
    rt2800_register_write(rt, BKOFF_SLOT_CFG, reg);

    rt->erp_flags = erp_flags;
}

/* rt2800_conf_tx(): the medium-access parameters of the four AC queues.
 *
 * Linux takes them from mac80211 — the AP's WMM element once QoS is negotiated,
 * otherwise the 802.11 defaults — and rt2x00mac_conf_tx() converts the real
 * contention-window values into the bit numbers the registers hold (fls()).
 * This port does not associate with QoS, so it programs the legacy 802.11b/g
 * rules mac80211 uses with enable_qos = false (802.11-2007 Table 7-37):
 * identical parameters on every AC — aifs 2, CWmin 15 slots (n = 4), CWmax 1023
 * (n = 10), no TXOP.  A QoS setup would take the per-AC (or the AP's) values
 * instead; the registers are already written one queue at a time for that.
 *
 * The MGMT queue is deliberately not touched: rt2800_conf_tx() returns early
 * for queue_idx >= 4, so on Linux too the management frames (the auth) go out
 * with the chip's power-on values. */
void rt2800_config_edca(struct rt_dev *rt) {
    u32 reg, offset;
    struct rt2x00_field32 field;

    for (u8 idx = 0; idx < 4; idx++) {
        const u8  aifs   = 2;
        const u8  cw_min = 4;      /* 2^4 - 1 = 15 slots */
        const u8  cw_max = 10;     /* 2^10 - 1 = 1023 slots */
        const u16 txop   = 0;

        /* WMM TXOP: two 16-bit slots per register. */
        offset = WMM_TXOP0_CFG + (4 * (!!(idx & 2)));
        field.bit_offset = (idx & 1) * 16;
        field.bit_mask   = 0xffff << field.bit_offset;
        reg = rt2800_register_read(rt, offset);
        rt2x00_set_field32(&reg, field, txop);
        rt2800_register_write(rt, offset, reg);

        /* AIFSN, CWmin and CWmax: one 4-bit slot per queue. */
        field.bit_offset = idx * 4;
        field.bit_mask   = 0xf << field.bit_offset;

        reg = rt2800_register_read(rt, WMM_AIFSN_CFG);
        rt2x00_set_field32(&reg, field, aifs);
        rt2800_register_write(rt, WMM_AIFSN_CFG, reg);

        reg = rt2800_register_read(rt, WMM_CWMIN_CFG);
        rt2x00_set_field32(&reg, field, cw_min);
        rt2800_register_write(rt, WMM_CWMIN_CFG, reg);

        reg = rt2800_register_read(rt, WMM_CWMAX_CFG);
        rt2x00_set_field32(&reg, field, cw_max);
        rt2800_register_write(rt, WMM_CWMAX_CFG, reg);

        /* ... and the per-queue EDCA register repeats the same set. */
        offset = EDCA_AC0_CFG + (4 * idx);
        reg = rt2800_register_read(rt, offset);
        rt2x00_set_field32(&reg, EDCA_AC0_CFG_TX_OP, txop);
        rt2x00_set_field32(&reg, EDCA_AC0_CFG_AIFSN, aifs);
        rt2x00_set_field32(&reg, EDCA_AC0_CFG_CWMIN, cw_min);
        rt2x00_set_field32(&reg, EDCA_AC0_CFG_CWMAX, cw_max);
        rt2800_register_write(rt, offset, reg);
    }
}
