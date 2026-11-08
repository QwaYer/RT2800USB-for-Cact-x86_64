/*
 * rt2800 BBP + RF initialisation and radio enable, transcribed from the Linux
 * rt2x00 driver (rt2800lib.c): rt2800_wait_bbp_rf_ready, rt2800_wait_bbp_ready,
 * rt2800_bbp4_mac_if_ctrl, rt2800_init_freq_calibration,
 * rt2800_disable_unused_dac_adc, rt2800_init_bbp_early, rt2800_init_bbp_30xx,
 * rt2800_init_bbp, rt2800_init_rx_filter, rt2800_rx_filter_calibration,
 * rt2800_rf_init_calibration, rt2800_led_open_drain_enable,
 * rt2800_normal_mode_setup_3xxx, rt2800_init_rfcsr_30xx, rt2800_init_rfcsr,
 * rt2800_enable_radio.
 */

#include "rt2800_local.h"

static int rt2800_wait_bbp_rf_ready(struct rt_dev *rt,
                                    const struct rt2x00_field32 mask) {
    unsigned int i;
    u32 reg;

    for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
        reg = rt2800_register_read(rt, MAC_STATUS_CFG);
        if (!rt2x00_get_field32(reg, mask))
            return 0;
        udelay(REGISTER_BUSY_DELAY);
    }
    rt2x00_err(rt, "BBP/RF register access failed, aborting\n");
    return -1;
}

static int rt2800_wait_bbp_ready(struct rt_dev *rt) {
    unsigned int i;
    u8 value;

    rt2800_register_write(rt, H2M_BBP_AGENT, 0);
    rt2800_register_write(rt, H2M_MAILBOX_CSR, 0);
    msleep(1);

    for (i = 0; i < REGISTER_BUSY_COUNT; i++) {
        value = rt2800_bbp_read(rt, 0);
        if ((value != 0xff) && (value != 0x00))
            return 0;
        udelay(REGISTER_BUSY_DELAY);
    }
    rt2x00_err(rt, "BBP register access failed, aborting\n");
    return -1;
}

static void rt2800_bbp4_mac_if_ctrl(struct rt_dev *rt) {
    u8 value = rt2800_bbp_read(rt, 4);
    rt2x00_set_field8(&value, BBP4_MAC_IF_CTRL, 1);
    rt2800_bbp_write(rt, 4, value);
}

static void rt2800_init_freq_calibration(struct rt_dev *rt) {
    rt2800_bbp_write(rt, 142, 1);
    rt2800_bbp_write(rt, 143, 57);
}

static void rt2800_disable_unused_dac_adc(struct rt_dev *rt) {
    u16 eeprom;
    u8 value;

    value = rt2800_bbp_read(rt, 138);
    eeprom = rt2800_eeprom_read(rt, EEPROM_NIC_CONF0);
    if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_TXPATH) == 1)
        value |= 0x20;
    if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_RXPATH) == 1)
        value &= ~0x02;
    rt2800_bbp_write(rt, 138, value);
}

static void rt2800_init_bbp_early(struct rt_dev *rt) {
    rt2800_bbp_write(rt, 65, 0x2C);
    rt2800_bbp_write(rt, 66, 0x38);
    rt2800_bbp_write(rt, 68, 0x0B);
    rt2800_bbp_write(rt, 69, 0x12);
    rt2800_bbp_write(rt, 70, 0x0a);
    rt2800_bbp_write(rt, 73, 0x10);
    rt2800_bbp_write(rt, 81, 0x37);
    rt2800_bbp_write(rt, 82, 0x62);
    rt2800_bbp_write(rt, 83, 0x6A);
    rt2800_bbp_write(rt, 84, 0x99);
    rt2800_bbp_write(rt, 86, 0x00);
    rt2800_bbp_write(rt, 91, 0x04);
    rt2800_bbp_write(rt, 92, 0x00);
    rt2800_bbp_write(rt, 103, 0x00);
    rt2800_bbp_write(rt, 105, 0x05);
    rt2800_bbp_write(rt, 106, 0x35);
}

static void rt2800_init_bbp_30xx(struct rt_dev *rt) {
    rt2800_bbp_write(rt, 65, 0x2c);
    rt2800_bbp_write(rt, 66, 0x38);

    rt2800_bbp_write(rt, 69, 0x12);
    rt2800_bbp_write(rt, 73, 0x10);

    rt2800_bbp_write(rt, 70, 0x0a);

    rt2800_bbp_write(rt, 79, 0x13);
    rt2800_bbp_write(rt, 80, 0x05);
    rt2800_bbp_write(rt, 81, 0x33);

    rt2800_bbp_write(rt, 82, 0x62);
    rt2800_bbp_write(rt, 83, 0x6a);
    rt2800_bbp_write(rt, 84, 0x99);
    rt2800_bbp_write(rt, 86, 0x00);
    rt2800_bbp_write(rt, 91, 0x04);
    rt2800_bbp_write(rt, 92, 0x00);

    if (rt2x00_rt_rev_gte(rt, RT3070, REV_RT3070F) ||
        rt2x00_rt_rev_gte(rt, RT3071, REV_RT3071E) ||
        rt2x00_rt_rev_gte(rt, RT3090, REV_RT3090E))
        rt2800_bbp_write(rt, 103, 0xc0);
    else
        rt2800_bbp_write(rt, 103, 0x00);

    rt2800_bbp_write(rt, 105, 0x05);
    rt2800_bbp_write(rt, 106, 0x35);

    if (rt2x00_rt(rt, RT3071) || rt2x00_rt(rt, RT3090))
        rt2800_disable_unused_dac_adc(rt);
}

static void rt2800_init_bbp(struct rt_dev *rt) {
    unsigned int i;
    u16 eeprom;
    u8 reg_id;
    u8 value;

    switch (rt->rt) {
    case RT3070:
    case RT3071:
    case RT3090:
        rt2800_init_bbp_30xx(rt);
        break;
    default:
        break;
    }

    for (i = 0; i < EEPROM_BBP_SIZE; i++) {
        eeprom = rt2800_eeprom_read_from_array(rt, EEPROM_BBP_START, i);

        if (eeprom != 0xffff && eeprom != 0x0000) {
            reg_id = rt2x00_get_field16(eeprom, EEPROM_BBP_REG_ID);
            value = rt2x00_get_field16(eeprom, EEPROM_BBP_VALUE);
            rt2800_bbp_write(rt, reg_id, value);
        }
    }
}

static u8 rt2800_init_rx_filter(struct rt_dev *rt, bool bw40, u8 filter_target) {
    unsigned int i;
    u8 bbp;
    u8 rfcsr;
    u8 passband = 0;
    u8 stopband = 0;
    u8 overtuned = 0;
    u8 rfcsr24 = (bw40) ? 0x27 : 0x07;

    rt2800_rfcsr_write(rt, 24, rfcsr24);

    bbp = rt2800_bbp_read(rt, 4);
    rt2x00_set_field8(&bbp, BBP4_BANDWIDTH, 2 * bw40);
    rt2800_bbp_write(rt, 4, bbp);

    rfcsr = rt2800_rfcsr_read(rt, 31);
    rt2x00_set_field8(&rfcsr, RFCSR31_RX_H20M, bw40);
    rt2800_rfcsr_write(rt, 31, rfcsr);

    rfcsr = rt2800_rfcsr_read(rt, 22);
    rt2x00_set_field8(&rfcsr, RFCSR22_BASEBAND_LOOPBACK, 1);
    rt2800_rfcsr_write(rt, 22, rfcsr);

    /* Set power & frequency of passband test tone. */
    rt2800_bbp_write(rt, 24, 0);
    for (i = 0; i < 100; i++) {
        rt2800_bbp_write(rt, 25, 0x90);
        msleep(1);
        passband = rt2800_bbp_read(rt, 55);
        if (passband)
            break;
    }

    /* Set power & frequency of stopband test tone. */
    rt2800_bbp_write(rt, 24, 0x06);
    for (i = 0; i < 100; i++) {
        rt2800_bbp_write(rt, 25, 0x90);
        msleep(1);

        stopband = rt2800_bbp_read(rt, 55);
        if ((passband - stopband) <= filter_target) {
            rfcsr24++;
            overtuned += ((passband - stopband) == filter_target);
        } else {
            break;
        }
        rt2800_rfcsr_write(rt, 24, rfcsr24);
    }

    rfcsr24 -= !!overtuned;
    rt2800_rfcsr_write(rt, 24, rfcsr24);
    return rfcsr24;
}

static void rt2800_rx_filter_calibration(struct rt_dev *rt) {
    u8 filter_tgt_bw20;
    u8 filter_tgt_bw40;
    u8 rfcsr, bbp;

    if (rt2x00_rt(rt, RT3070)) {
        filter_tgt_bw20 = 0x16;
        filter_tgt_bw40 = 0x19;
    } else {
        filter_tgt_bw20 = 0x13;
        filter_tgt_bw40 = 0x15;
    }

    rt->calibration_bw20 = rt2800_init_rx_filter(rt, false, filter_tgt_bw20);
    rt->calibration_bw40 = rt2800_init_rx_filter(rt, true, filter_tgt_bw40);

    rt->bbp25 = rt2800_bbp_read(rt, 25);
    rt->bbp26 = rt2800_bbp_read(rt, 26);

    rt2800_bbp_write(rt, 24, 0);

    rfcsr = rt2800_rfcsr_read(rt, 22);
    rt2x00_set_field8(&rfcsr, RFCSR22_BASEBAND_LOOPBACK, 0);
    rt2800_rfcsr_write(rt, 22, rfcsr);

    bbp = rt2800_bbp_read(rt, 4);
    rt2x00_set_field8(&bbp, BBP4_BANDWIDTH, 0);
    rt2800_bbp_write(rt, 4, bbp);
}

static void rt2800_rf_init_calibration(struct rt_dev *rt,
                                       const unsigned int rf_reg) {
    u8 rfcsr = rt2800_rfcsr_read(rt, rf_reg);
    rt2x00_set_field8(&rfcsr, FIELD8(0x80), 1);
    rt2800_rfcsr_write(rt, rf_reg, rfcsr);
    msleep(1);
    rt2x00_set_field8(&rfcsr, FIELD8(0x80), 0);
    rt2800_rfcsr_write(rt, rf_reg, rfcsr);
}

static void rt2800_led_open_drain_enable(struct rt_dev *rt) {
    u32 reg = rt2800_register_read(rt, OPT_14_CSR);
    rt2x00_set_field32(&reg, OPT_14_CSR_BIT0, 1);
    rt2800_register_write(rt, OPT_14_CSR, reg);
}

static void rt2800_normal_mode_setup_3xxx(struct rt_dev *rt) {
    u8 min_gain, rfcsr, bbp;
    u16 eeprom;

    rfcsr = rt2800_rfcsr_read(rt, 17);

    rt2x00_set_field8(&rfcsr, RFCSR17_TX_LO1_EN, 0);
    if (rt2x00_rt(rt, RT3070) ||
        rt2x00_rt_rev_lt(rt, RT3071, REV_RT3071E) ||
        rt2x00_rt_rev_lt(rt, RT3090, REV_RT3090E) ||
        rt2x00_rt_rev_lt(rt, RT3390, REV_RT3390E)) {
        if (!rt->external_lna_bg)
            rt2x00_set_field8(&rfcsr, RFCSR17_R, 1);
    }

    min_gain = rt2x00_rt(rt, RT3070) ? 1 : 2;
    if (rt->txmixer_gain_24g >= min_gain) {
        rt2x00_set_field8(&rfcsr, RFCSR17_TXMIXER_GAIN,
                          rt->txmixer_gain_24g);
    }

    rt2800_rfcsr_write(rt, 17, rfcsr);

    if (rt2x00_rt(rt, RT3090)) {
        bbp = rt2800_bbp_read(rt, 138);
        eeprom = rt2800_eeprom_read(rt, EEPROM_NIC_CONF0);
        if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_RXPATH) == 1)
            rt2x00_set_field8(&bbp, BBP138_RX_ADC1, 0);
        if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF0_TXPATH) == 1)
            rt2x00_set_field8(&bbp, BBP138_TX_DAC1, 1);
        rt2800_bbp_write(rt, 138, bbp);
    }

    if (rt2x00_rt(rt, RT3070)) {
        rfcsr = rt2800_rfcsr_read(rt, 27);
        if (rt2x00_rt_rev_lt(rt, RT3070, REV_RT3070F))
            rt2x00_set_field8(&rfcsr, RFCSR27_R1, 3);
        else
            rt2x00_set_field8(&rfcsr, RFCSR27_R1, 0);
        rt2x00_set_field8(&rfcsr, RFCSR27_R2, 0);
        rt2x00_set_field8(&rfcsr, RFCSR27_R3, 0);
        rt2x00_set_field8(&rfcsr, RFCSR27_R4, 0);
        rt2800_rfcsr_write(rt, 27, rfcsr);
    }
}

static void rt2800_init_rfcsr_30xx(struct rt_dev *rt) {
    u8 rfcsr;
    u16 eeprom;
    u32 reg;

    /* XXX vendor driver do this only for 3070 */
    rt2800_rf_init_calibration(rt, 30);

    rt2800_rfcsr_write(rt, 4, 0x40);
    rt2800_rfcsr_write(rt, 5, 0x03);
    rt2800_rfcsr_write(rt, 6, 0x02);
    rt2800_rfcsr_write(rt, 7, 0x60);
    rt2800_rfcsr_write(rt, 9, 0x0f);
    rt2800_rfcsr_write(rt, 10, 0x41);
    rt2800_rfcsr_write(rt, 11, 0x21);
    rt2800_rfcsr_write(rt, 12, 0x7b);
    rt2800_rfcsr_write(rt, 14, 0x90);
    rt2800_rfcsr_write(rt, 15, 0x58);
    rt2800_rfcsr_write(rt, 16, 0xb3);
    rt2800_rfcsr_write(rt, 17, 0x92);
    rt2800_rfcsr_write(rt, 18, 0x2c);
    rt2800_rfcsr_write(rt, 19, 0x02);
    rt2800_rfcsr_write(rt, 20, 0xba);
    rt2800_rfcsr_write(rt, 21, 0xdb);
    rt2800_rfcsr_write(rt, 24, 0x16);
    rt2800_rfcsr_write(rt, 25, 0x03);
    rt2800_rfcsr_write(rt, 29, 0x1f);

    if (rt2x00_rt_rev_lt(rt, RT3070, REV_RT3070F)) {
        reg = rt2800_register_read(rt, LDO_CFG0);
        rt2x00_set_field32(&reg, LDO_CFG0_BGSEL, 1);
        rt2x00_set_field32(&reg, LDO_CFG0_LDO_CORE_VLEVEL, 3);
        rt2800_register_write(rt, LDO_CFG0, reg);
    } else if (rt2x00_rt(rt, RT3071) || rt2x00_rt(rt, RT3090)) {
        rt2800_rfcsr_write(rt, 31, 0x14);

        rfcsr = rt2800_rfcsr_read(rt, 6);
        rt2x00_set_field8(&rfcsr, RFCSR6_R2, 1);
        rt2800_rfcsr_write(rt, 6, rfcsr);

        reg = rt2800_register_read(rt, LDO_CFG0);
        rt2x00_set_field32(&reg, LDO_CFG0_BGSEL, 1);
        if (rt2x00_rt_rev_lt(rt, RT3071, REV_RT3071E) ||
            rt2x00_rt_rev_lt(rt, RT3090, REV_RT3090E)) {
            eeprom = rt2800_eeprom_read(rt, EEPROM_NIC_CONF1);
            if (rt2x00_get_field16(eeprom, EEPROM_NIC_CONF1_DAC_TEST))
                rt2x00_set_field32(&reg, LDO_CFG0_LDO_CORE_VLEVEL, 3);
            else
                rt2x00_set_field32(&reg, LDO_CFG0_LDO_CORE_VLEVEL, 0);
        }
        rt2800_register_write(rt, LDO_CFG0, reg);

        reg = rt2800_register_read(rt, GPIO_SWITCH);
        rt2x00_set_field32(&reg, GPIO_SWITCH_5, 0);
        rt2800_register_write(rt, GPIO_SWITCH, reg);
    }

    rt2800_rx_filter_calibration(rt);

    if (rt2x00_rt_rev_lt(rt, RT3070, REV_RT3070F) ||
        rt2x00_rt_rev_lt(rt, RT3071, REV_RT3071E) ||
        rt2x00_rt_rev_lt(rt, RT3090, REV_RT3090E))
        rt2800_rfcsr_write(rt, 27, 0x03);

    rt2800_led_open_drain_enable(rt);
    rt2800_normal_mode_setup_3xxx(rt);
}

static void rt2800_init_rfcsr(struct rt_dev *rt) {
    switch (rt->rt) {
    case RT3070:
    case RT3071:
    case RT3090:
        rt2800_init_rfcsr_30xx(rt);
        break;
    default:
        break;
    }
}

int rt2800_enable_radio(struct rt_dev *rt) {
    u32 reg;
    u16 word;

    if (rt2800_wait_wpdma_ready(rt))
        return -1;

    /* rt2800usb_enable_radio(): bring up the USB DMA engine (bulk RX/TX and
     * the aggregation settings) before the radio init. */
    reg = 0;
    rt2x00_set_field32(&reg, USB_DMA_CFG_PHY_CLEAR, 0);
    rt2x00_set_field32(&reg, USB_DMA_CFG_RX_BULK_AGG_EN, 0);
    rt2x00_set_field32(&reg, USB_DMA_CFG_RX_BULK_AGG_TIMEOUT, 128);
    /* Linux computes ((rx->limit * DATA_FRAME_SIZE) / 1024) - 3; for rt2800usb
     * rx->limit is 128 and DATA_FRAME_SIZE is 2432, giving 301.  The port had
     * a hardcoded 0x50 (80), too small for the RX PBF allocation. */
    rt2x00_set_field32(&reg, USB_DMA_CFG_RX_BULK_AGG_LIMIT, 301);
    rt2x00_set_field32(&reg, USB_DMA_CFG_RX_BULK_EN, 1);
    rt2x00_set_field32(&reg, USB_DMA_CFG_TX_BULK_EN, 1);
    rt2800_register_write(rt, USB_DMA_CFG, reg);

    if (rt2800_init_registers(rt))
        return -1;

    if (rt2800_wait_bbp_rf_ready(rt, MAC_STATUS_CFG_BBP_RF_BUSY))
        return -1;

    rt2800_register_write(rt, H2M_BBP_AGENT, 0);
    rt2800_register_write(rt, H2M_MAILBOX_CSR, 0);
    rt2800_register_write(rt, H2M_INT_SRC, 0);
    rt_mcu_request(rt->usb, MCU_BOOT_SIGNAL, 0, 0, 0);
    msleep(1);

    if (rt2800_wait_bbp_ready(rt))
        return -1;

    rt2800_init_bbp(rt);
    rt2800_init_rfcsr(rt);

    if (rt2x00_rt(rt, RT3070) || rt2x00_rt(rt, RT3071) ||
        rt2x00_rt(rt, RT3572)) {
        udelay(200);
        rt_mcu_request(rt->usb, MCU_CURRENT, 0, 0, 0);
        udelay(10);
    }

    reg = rt2800_register_read(rt, MAC_SYS_CTRL);
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_ENABLE_TX, 1);
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_ENABLE_RX, 0);
    rt2800_register_write(rt, MAC_SYS_CTRL, reg);

    udelay(50);

    reg = rt2800_register_read(rt, WPDMA_GLO_CFG);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_TX_DMA, 1);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_ENABLE_RX_DMA, 1);
    rt2x00_set_field32(&reg, WPDMA_GLO_CFG_TX_WRITEBACK_DONE, 1);
    rt2800_register_write(rt, WPDMA_GLO_CFG, reg);

    reg = rt2800_register_read(rt, MAC_SYS_CTRL);
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_ENABLE_TX, 1);
    rt2x00_set_field32(&reg, MAC_SYS_CTRL_ENABLE_RX, 1);
    rt2800_register_write(rt, MAC_SYS_CTRL, reg);

    word = rt2800_eeprom_read(rt, EEPROM_LED_AG_CONF);
    rt_mcu_request(rt->usb, MCU_LED_AG_CONF, 0xff, word & 0xff, (word >> 8) & 0xff);

    word = rt2800_eeprom_read(rt, EEPROM_LED_ACT_CONF);
    rt_mcu_request(rt->usb, MCU_LED_ACT_CONF, 0xff, word & 0xff, (word >> 8) & 0xff);

    word = rt2800_eeprom_read(rt, EEPROM_LED_POLARITY);
    rt_mcu_request(rt->usb, MCU_LED_LED_POLARITY, 0xff, word & 0xff, (word >> 8) & 0xff);

    return 0;
}
