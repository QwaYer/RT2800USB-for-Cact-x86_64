/*
 * rt2800 channel configuration, transcribed from the Linux rt2x00 driver
 * (rt2800lib.c: rf_vals_3x, rt2800_txpower_to_dev, rt2800_config_channel_rf3xxx
 * and the RT3070/1T1R part of rt2800_config_channel).
 *
 * Covers 2.4 GHz (channels 1..14) for the RF3020/3xxx front end, which is what
 * the RT3070 dongle uses.
 */

#include "rt2800_local.h"

struct rf_channel {
    int  channel;
    u32  rf1;
    u32  rf2;
    u32  rf3;
    u32  rf4;
};

/* rf_vals_3x (2.4 GHz subset). */
static const struct rf_channel rt2800_rf_vals_3x[] = {
    { 1,  241, 2, 2 }, { 2,  241, 2, 7 }, { 3,  242, 2, 2 }, { 4,  242, 2, 7 },
    { 5,  243, 2, 2 }, { 6,  243, 2, 7 }, { 7,  244, 2, 2 }, { 8,  244, 2, 7 },
    { 9,  245, 2, 2 }, { 10, 245, 2, 7 }, { 11, 246, 2, 2 }, { 12, 246, 2, 7 },
    { 13, 247, 2, 2 }, { 14, 248, 2, 4 },
};

static s8 rt2800_txpower_to_dev(unsigned int channel, s8 txpower) {
    (void)channel;
    if (txpower < MIN_G_TXPOWER)
        txpower = MIN_G_TXPOWER;
    if (txpower > MAX_G_TXPOWER)
        txpower = MAX_G_TXPOWER;
    return txpower;
}

/* rt2800_config_channel_rf3xxx() */
static void rt2800_config_channel_rf3xxx(struct rt_dev *rt,
                                         const struct rf_channel *rf,
                                         s8 default_power1, s8 default_power2) {
    u8 rfcsr, calib_tx, calib_rx;

    rt2800_rfcsr_write(rt, 2, (u8)rf->rf1);

    rfcsr = rt2800_rfcsr_read(rt, 3);
    rt2x00_set_field8(&rfcsr, RFCSR3_K, (u8)rf->rf3);
    rt2800_rfcsr_write(rt, 3, rfcsr);

    rfcsr = rt2800_rfcsr_read(rt, 6);
    rt2x00_set_field8(&rfcsr, RFCSR6_R1, (u8)rf->rf2);
    rt2800_rfcsr_write(rt, 6, rfcsr);

    rfcsr = rt2800_rfcsr_read(rt, 12);
    rt2x00_set_field8(&rfcsr, RFCSR12_TX_POWER, (u8)default_power1);
    rt2800_rfcsr_write(rt, 12, rfcsr);

    rfcsr = rt2800_rfcsr_read(rt, 13);
    rt2x00_set_field8(&rfcsr, RFCSR13_TX_POWER, (u8)default_power2);
    rt2800_rfcsr_write(rt, 13, rfcsr);

    rfcsr = rt2800_rfcsr_read(rt, 1);
    rt2x00_set_field8(&rfcsr, RFCSR1_RX0_PD, 0);
    rt2x00_set_field8(&rfcsr, RFCSR1_RX1_PD, rt->rx_chain_num <= 1);
    rt2x00_set_field8(&rfcsr, RFCSR1_RX2_PD, rt->rx_chain_num <= 2);
    rt2x00_set_field8(&rfcsr, RFCSR1_TX0_PD, 0);
    rt2x00_set_field8(&rfcsr, RFCSR1_TX1_PD, rt->tx_chain_num <= 1);
    rt2x00_set_field8(&rfcsr, RFCSR1_TX2_PD, rt->tx_chain_num <= 2);
    rt2800_rfcsr_write(rt, 1, rfcsr);

    rfcsr = rt2800_rfcsr_read(rt, 23);
    rt2x00_set_field8(&rfcsr, RFCSR23_FREQ_OFFSET, rt->freq_offset);
    rt2800_rfcsr_write(rt, 23, rfcsr);

    calib_tx = rt->calibration_bw20;
    calib_rx = rt->calibration_bw20;

    rfcsr = rt2800_rfcsr_read(rt, 24);
    rt2x00_set_field8(&rfcsr, RFCSR24_TX_CALIB, calib_tx);
    rt2800_rfcsr_write(rt, 24, rfcsr);

    rfcsr = rt2800_rfcsr_read(rt, 31);
    rt2x00_set_field8(&rfcsr, RFCSR31_RX_CALIB, calib_rx);
    rt2800_rfcsr_write(rt, 31, rfcsr);

    rfcsr = rt2800_rfcsr_read(rt, 7);
    rt2x00_set_field8(&rfcsr, RFCSR7_RF_TUNING, 1);
    rt2800_rfcsr_write(rt, 7, rfcsr);

    rfcsr = rt2800_rfcsr_read(rt, 30);
    rt2x00_set_field8(&rfcsr, RFCSR30_RF_CALIBRATION, 1);
    rt2800_rfcsr_write(rt, 30, rfcsr);

    udelay(1000);

    rt2x00_set_field8(&rfcsr, RFCSR30_RF_CALIBRATION, 0);
    rt2800_rfcsr_write(rt, 30, rfcsr);
}

/* rt2800_get_gain_calibration_delta(): TSSI temperature compensation.  It is 0
 * unless the EEPROM enables external TX ALC, so most dongles skip the whole
 * TSSI block. */
static int rt2800_get_gain_calibration_delta(struct rt_dev *rt) {
    u8 tssi_bounds[9];
    u8 current_tssi, step;
    u16 eeprom;
    int i;

    eeprom = rt2800_eeprom_read(rt, EEPROM_NIC_CONF1);
    if (!rt2x00_get_field16(eeprom, EEPROM_NIC_CONF1_EXTERNAL_TX_ALC))
        return 0;

    /* 2.4 GHz bounds; array index 0..8 matches a delta of -4..+4. */
    eeprom = rt2800_eeprom_read(rt, EEPROM_TSSI_BOUND_BG1);
    tssi_bounds[0] = (u8)rt2x00_get_field16(eeprom, EEPROM_TSSI_BOUND_BG1_MINUS4);
    tssi_bounds[1] = (u8)rt2x00_get_field16(eeprom, EEPROM_TSSI_BOUND_BG1_MINUS3);
    eeprom = rt2800_eeprom_read(rt, EEPROM_TSSI_BOUND_BG2);
    tssi_bounds[2] = (u8)rt2x00_get_field16(eeprom, EEPROM_TSSI_BOUND_BG2_MINUS2);
    tssi_bounds[3] = (u8)rt2x00_get_field16(eeprom, EEPROM_TSSI_BOUND_BG2_MINUS1);
    eeprom = rt2800_eeprom_read(rt, EEPROM_TSSI_BOUND_BG3);
    tssi_bounds[4] = (u8)rt2x00_get_field16(eeprom, EEPROM_TSSI_BOUND_BG3_REF);
    tssi_bounds[5] = (u8)rt2x00_get_field16(eeprom, EEPROM_TSSI_BOUND_BG3_PLUS1);
    eeprom = rt2800_eeprom_read(rt, EEPROM_TSSI_BOUND_BG4);
    tssi_bounds[6] = (u8)rt2x00_get_field16(eeprom, EEPROM_TSSI_BOUND_BG4_PLUS2);
    tssi_bounds[7] = (u8)rt2x00_get_field16(eeprom, EEPROM_TSSI_BOUND_BG4_PLUS3);
    eeprom = rt2800_eeprom_read(rt, EEPROM_TSSI_BOUND_BG5);
    tssi_bounds[8] = (u8)rt2x00_get_field16(eeprom, EEPROM_TSSI_BOUND_BG5_PLUS4);
    step = (u8)rt2x00_get_field16(eeprom, EEPROM_TSSI_BOUND_BG5_AGC_STEP);

    if (tssi_bounds[4] == 0xff || step == 0xff)
        return 0;

    current_tssi = rt2800_bbp_read(rt, 49);

    for (i = 0; i <= 3; i++)
        if (current_tssi > tssi_bounds[i])
            break;

    if (i == 4) {
        for (i = 8; i >= 5; i--)
            if (current_tssi < tssi_bounds[i])
                break;
    }

    return (i - 4) * (int)step;
}

/* rt2800_compensate_txpower(), the no-CAPABILITY_POWER_LIMIT branch: the EIRP
 * adjustment needs a regulatory TX power level this driver does not have, and
 * rt2800_get_txpower_reg_delta() is 0 in exactly that case. */
static u8 rt2800_compensate_txpower(u8 txpower, int delta) {
    int v = (int)txpower + delta;

    if (v < 0)
        v = 0;
    if (v > 0xc)
        v = 0xc;
    return (u8)v;
}

/* rt2800_config_txpower() (rt2800_config_txpower_rt28xx): load the per-rate TX
 * power from EEPROM_TXPOWER_BYRATE into MAC TX_PWR_CFG_0..4 and set BBP1's TX
 * power control.  Linux runs this on every channel change — rt2800_config()
 * calls it right after rt2800_config_channel() — and without it the chip keeps
 * its power-on values, transmitting far below the EEPROM's calibrated per-rate
 * power: the receiver (scan) works while the AP never hears our TX, which is
 * what "authentication has no answer" looked like.
 *
 * Deliberately not ported: the HT40 delta (rt2800_get_txpower_bw_comp — this
 * driver is 20 MHz), the delta from user/regulatory power settings
 * (rt2800_get_txpower_reg_delta, a no-op here), the EIRP power-limit branch of
 * rt2800_compensate_txpower, and the TX_PWR_CFG_7/8/9 copies of
 * OFDM54/MCS7/MCS15 (rates this driver never sends). */
static void rt2800_config_txpower(struct rt_dev *rt) {
    int delta = rt2800_get_gain_calibration_delta(rt);
    int power_ctrl;
    u32 reg, offset;
    u8 r1;

    /* BBP R1 saves power in -12/-6/0 dBm steps; +6 dBm stays unused, as in
     * Linux. */
    if (delta <= -12) {
        power_ctrl = 2;
        delta += 12;
    } else if (delta <= -6) {
        power_ctrl = 1;
        delta += 6;
    } else {
        power_ctrl = 0;
    }

    r1 = rt2800_bbp_read(rt, 1);
    rt2x00_set_field8(&r1, BBP1_TX_POWER_CTRL, (u8)power_ctrl);
    rt2800_bbp_write(rt, 1, r1);

    offset = TX_PWR_CFG_0;
    for (unsigned i = 0; i < EEPROM_TXPOWER_BYRATE_SIZE; i += 2) {
        u16 e;

        if (offset > TX_PWR_CFG_4)     /* TX_PWR_CFG_5.. are not for this band */
            break;

        reg = rt2800_register_read(rt, offset);

        /* The first word carries the CCK/low-OFDM rates (is_rate_b), the
         * second the matching MCS/low rates into RATE4..7. */
        e = rt2800_eeprom_read_from_array(rt, EEPROM_TXPOWER_BYRATE, i);
        rt2x00_set_field32(&reg, TX_PWR_CFG_RATE0,
            rt2800_compensate_txpower(
                (u8)rt2x00_get_field16(e, EEPROM_TXPOWER_BYRATE_RATE0), delta));
        rt2x00_set_field32(&reg, TX_PWR_CFG_RATE1,
            rt2800_compensate_txpower(
                (u8)rt2x00_get_field16(e, EEPROM_TXPOWER_BYRATE_RATE1), delta));
        rt2x00_set_field32(&reg, TX_PWR_CFG_RATE2,
            rt2800_compensate_txpower(
                (u8)rt2x00_get_field16(e, EEPROM_TXPOWER_BYRATE_RATE2), delta));
        rt2x00_set_field32(&reg, TX_PWR_CFG_RATE3,
            rt2800_compensate_txpower(
                (u8)rt2x00_get_field16(e, EEPROM_TXPOWER_BYRATE_RATE3), delta));

        e = rt2800_eeprom_read_from_array(rt, EEPROM_TXPOWER_BYRATE, i + 1);
        rt2x00_set_field32(&reg, TX_PWR_CFG_RATE4,
            rt2800_compensate_txpower(
                (u8)rt2x00_get_field16(e, EEPROM_TXPOWER_BYRATE_RATE0), delta));
        rt2x00_set_field32(&reg, TX_PWR_CFG_RATE5,
            rt2800_compensate_txpower(
                (u8)rt2x00_get_field16(e, EEPROM_TXPOWER_BYRATE_RATE1), delta));
        rt2x00_set_field32(&reg, TX_PWR_CFG_RATE6,
            rt2800_compensate_txpower(
                (u8)rt2x00_get_field16(e, EEPROM_TXPOWER_BYRATE_RATE2), delta));
        rt2x00_set_field32(&reg, TX_PWR_CFG_RATE7,
            rt2800_compensate_txpower(
                (u8)rt2x00_get_field16(e, EEPROM_TXPOWER_BYRATE_RATE3), delta));

        rt2800_register_write(rt, offset, reg);
        offset += 4;
    }
}

/* Per-channel TX power byte from the EEPROM TX power arrays.
 *
 * rt2800_probe_hw() fills its per-channel table from
 * `default_power1 = rt2800_eeprom_addr(EEPROM_TXPOWER_BG1)` as a *byte* array
 * indexed by (channel - 1) — one byte per channel, two channels per EEPROM
 * word.  The port always took word 0x29's low byte, i.e. channel 1's RF gain,
 * and used it on every channel. */
static u8 rt2800_txpower_byte(struct rt_dev *rt, const unsigned int word,
                              u8 channel) {
    unsigned int idx = (channel >= 1) ? (unsigned int)(channel - 1) : 0;
    u16 w = rt2800_eeprom_read_from_array(rt, word, idx / 2);

    return (u8)((idx & 1) ? (w >> 8) : (w & 0xff));
}

int rt2800_set_channel(struct rt_dev *rt, u8 channel) {
    const struct rf_channel *rf = NULL;
    u32 reg, tx_pin;
    u8 bbp;

    for (unsigned i = 0; i < ARRAY_SIZE(rt2800_rf_vals_3x); i++) {
        if (rt2800_rf_vals_3x[i].channel == channel) {
            rf = &rt2800_rf_vals_3x[i];
            break;
        }
    }
    if (!rf)
        return -1;

    /* rt2800_config_channel(): the RF gain for this channel, taken from the
     * per-channel EEPROM entry (not always channel 1's). */
    u8 tpbg1 = rt2800_txpower_byte(rt, EEPROM_TXPOWER_BG1, channel);
    u8 tpbg2 = rt2800_txpower_byte(rt, EEPROM_TXPOWER_BG2, channel);
    s8 pw1 = rt2800_txpower_to_dev(channel, (s8)tpbg1);
    s8 pw2 = rt2800_txpower_to_dev(channel, (s8)tpbg2);

    rt2800_config_channel_rf3xxx(rt, rf, pw1, pw2);

    /* Deliberately no RFCSR3_VCOCAL_EN / RFCSR30_TX_H20M here.  Linux applies
     * that block only to the RF3070 family (RF3070/3290/3322/535x/539x);
     * rf_vals_3x chips such as this RF3020 go through
     * rt2800_config_channel_rf3xxx, which calibrates via RFCSR7_RF_TUNING and
     * RFCSR30_RF_CALIBRATION above.  Setting VCOCAL_EN unconditionally left the
     * VCO recalibrating after every channel change. */

    /* BBP changes for the channel (rt2800_config_channel): the RX AGC/filter
     * gains must track the channel or the receiver cannot demodulate frames.
     * These BBP registers are written nowhere else. */
    rt2800_bbp_write(rt, 62, (u8)(0x37 - rt->lna_gain));
    rt2800_bbp_write(rt, 63, (u8)(0x37 - rt->lna_gain));
    rt2800_bbp_write(rt, 64, (u8)(0x37 - rt->lna_gain));
    rt2800_bbp_write(rt, 86, 0);

    if (rf->channel <= 14) {
        if (rt->external_lna_bg) {
            rt2800_bbp_write(rt, 82, 0x62);
            rt2800_bbp_write(rt, 82, 0x62);
            rt2800_bbp_write(rt, 75, 0x46);
        } else {
            rt2800_bbp_write(rt, 82, 0x84);
            rt2800_bbp_write(rt, 75, 0x50);
        }
    }

    reg = rt2800_register_read(rt, TX_BAND_CFG);
    rt2x00_set_field32(&reg, TX_BAND_CFG_A, rf->channel > 14);
    rt2x00_set_field32(&reg, TX_BAND_CFG_BG, rf->channel <= 14);
    rt2800_register_write(rt, TX_BAND_CFG, reg);

    /* TX/RX pin config (rt2800_config_channel): Linux builds this register from
     * zero and then enables the PAs/LNAs of the chains the board has, so the
     * polarity and the unused bits keep their reset values instead of leaking
     * into the enable pins. */
    tx_pin = 0;
    if (rt->tx_chain_num >= 3) {
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_A2_EN, (rf->channel > 14));
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_G2_EN, (rf->channel <= 14));
    }
    if (rt->tx_chain_num >= 2) {
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_A1_EN, (rf->channel > 14));
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_G1_EN, (rf->channel <= 14));
    }
    if (rt->tx_chain_num >= 1) {
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_A0_EN, (rf->channel > 14));
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_PA_PE_G0_EN, (rf->channel <= 14));
    }
    if (rt->rx_chain_num >= 3) {
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_A2_EN, 1);
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_G2_EN, 1);
    }
    if (rt->rx_chain_num >= 2) {
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_A1_EN, 1);
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_G1_EN, 1);
    }
    if (rt->rx_chain_num >= 1) {
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_A0_EN, 1);
        rt2x00_set_field32(&tx_pin, TX_PIN_CFG_LNA_PE_G0_EN, 1);
    }
    rt2x00_set_field32(&tx_pin, TX_PIN_CFG_RFTR_EN, 1);
    rt2x00_set_field32(&tx_pin, TX_PIN_CFG_TRSW_EN, 1);
    rt2800_register_write(rt, TX_PIN_CFG, tx_pin);

    /* This register carries the PA/LNA enables, the RF transceiver and the T/R
     * switch: the whole gate between the RF chain and the antenna.  Verify that
     * what was written is what the chip holds — a PA or the T/R switch silently
     * left off turns the transmitter inaudible while receive stays perfect, and
     * that is exactly the kind of failure the caller must hear about.  The
     * channel is still configured; the result is reported at the end. */
    int tx_pin_ok = (rt2800_register_read(rt, TX_PIN_CFG) & tx_pin) == tx_pin;

    /* Bandwidth: 20 MHz. */
    bbp = rt2800_bbp_read(rt, 4);
    rt2x00_set_field8(&bbp, BBP4_BANDWIDTH, 0);
    rt2800_bbp_write(rt, 4, bbp);

    bbp = rt2800_bbp_read(rt, 3);
    rt2x00_set_field8(&bbp, BBP3_HT40_MINUS, 0);
    rt2800_bbp_write(rt, 3, bbp);

    udelay(1000);

    /* rt2800_config(): the per-rate TX power follows the channel. */
    rt2800_config_txpower(rt);

    rt->freq = (channel == 14) ? 2484 : (2407 + channel * 5);

    if (!tx_pin_ok) {
        rt->last_error = -RT_EIO;
        return -RT_EIO;              /* the TX_PIN_CFG write did not stick */
    }
    return 0;
}

/* rt2800_set_ant_diversity(): boards with antenna diversity switch the RX
 * antenna through the MCU.  rt2x00reg.h's enum antenna: A = 1, B = 2. */
#define RT_ANTENNA_A 1
#define RT_ANTENNA_B 2

static void rt2800_set_ant_diversity(struct rt_dev *rt, u8 ant) {
    u8 eesk_pin = (ant == RT_ANTENNA_A) ? 1 : 0;
    u8 gpio_bit3 = (ant == RT_ANTENNA_A) ? 0 : 1;
    u32 reg;

    /* USB parts route the antenna select through the MCU (PCI/SoC parts write
     * E2PROM_CSR instead — not a path this module takes). */
    rt_mcu_request(rt->usb, MCU_ANT_SELECT, 0xff, eesk_pin, 0);

    reg = rt2800_register_read(rt, GPIO_CTRL);
    rt2x00_set_field32(&reg, GPIO_CTRL_DIR3, 0);
    rt2x00_set_field32(&reg, GPIO_CTRL_VAL3, gpio_bit3);
    rt2800_register_write(rt, GPIO_CTRL, reg);
}

/* rt2800_config_ant(): BBP1/BBP3 carry the TX/RX antenna (chain) select and
 * Linux programs them when the interface comes up.  Nothing else in the driver
 * writes them, so without this the chip keeps its power-on values — on this
 * dongle BBP1_TX_ANTENNA and BBP3_RX_ANTENNA both read back as 2, i.e. a chain
 * a 1T1R RF3020 does not have.  Only the paths this module's chips take are
 * ported (no RT3572/RT3593/RT3883, no BT coexistence). */
void rt2800_config_ant(struct rt_dev *rt) {
    u8 r1 = rt2800_bbp_read(rt, 1);
    u8 r3 = rt2800_bbp_read(rt, 3);

    switch (rt->tx_chain_num) {
    case 1:
        rt2x00_set_field8(&r1, BBP1_TX_ANTENNA, 0);
        break;
    case 2:
    case 3:
        rt2x00_set_field8(&r1, BBP1_TX_ANTENNA, 2);
        break;
    }

    switch (rt->rx_chain_num) {
    case 1:
        /* A board whose EEPROM enables antenna diversity has its RX antenna
         * switched by the MCU (rt2800_config_ant does this on Linux too); the
         * EEPROM value picks the default antenna — 3 is B, everything else A. */
        {
            u8 div = (u8)rt2x00_get_field16(
                rt2800_eeprom_read(rt, EEPROM_NIC_CONF1),
                EEPROM_NIC_CONF1_ANT_DIVERSITY);

            if (div)
                rt2800_set_ant_diversity(rt,
                                         div == 3 ? RT_ANTENNA_B : RT_ANTENNA_A);
        }
        rt2x00_set_field8(&r3, BBP3_RX_ANTENNA, 0);
        break;
    case 2:
        rt2x00_set_field8(&r3, BBP3_RX_ANTENNA, 1);
        break;
    case 3:
        rt2x00_set_field8(&r3, BBP3_RX_ANTENNA, 2);
        break;
    }

    rt2800_bbp_write(rt, 3, r3);
    rt2800_bbp_write(rt, 1, r1);
}

/* Minimal MLME step: broadcast a probe request.  It needs no association, so it
 * exercises the TX ring and should draw probe responses (and beacons) into RX. */
int rt2800_send_probe_request(struct rt_dev *rt) {
    u8 frame[64];
    int n = 0;

    /* Frame control: management, probe request. */
    frame[n++] = 0x40; frame[n++] = 0x00;
    frame[n++] = 0x00; frame[n++] = 0x00;      /* duration */
    for (int i = 0; i < 6; i++) frame[n++] = 0xff;   /* DA: broadcast */
    for (int i = 0; i < 6; i++) frame[n++] = rt->mac[i];  /* SA */
    for (int i = 0; i < 6; i++) frame[n++] = 0xff;   /* BSSID: broadcast */
    frame[n++] = 0x00; frame[n++] = 0x00;      /* sequence */

    frame[n++] = 0;  frame[n++] = 0;           /* SSID: wildcard */
    frame[n++] = 1;  frame[n++] = 4;           /* supported rates */
    frame[n++] = 0x82; frame[n++] = 0x84; frame[n++] = 0x8b; frame[n++] = 0x96;

    return rt2800_tx_frame(rt, frame, (u16)n, RT_QID_MGMT);
}
