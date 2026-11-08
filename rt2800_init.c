/*
 * rt2800 register primitives, transcribed from the Linux rt2x00 driver
 * (rt2800lib.c: rt2800_regbusy_read via rt2x00usb, rt2800_bbp_read/write,
 * rt2800_rfcsr_read/write, rt2800_rf_write, rt2800_eeprom_read) so the rest of
 * the bring-up can be ported verbatim (same names through rt2800_local.h).
 */

#include "rt2800_local.h"

/* Busy-wait indirect register access — rt2x00usb_regbusy_read(). */
int rt2800_regbusy_read(struct rt_dev *dev, const unsigned int offset,
                        const struct rt2x00_field32 field, u32 *reg) {
    for (unsigned int i = 0; i < REGISTER_USB_BUSY_COUNT; i++) {
        *reg = rt2800_register_read_lock(dev, offset);
        if (!rt2x00_get_field32(*reg, field))
            return 1;
        udelay(REGISTER_BUSY_DELAY);
    }
    *reg = ~0;
    return 0;
}

void rt2800_bbp_write(struct rt_dev *dev, const unsigned int word, const u8 value) {
    u32 reg;

    if (rt2800_regbusy_read(dev, BBP_CSR_CFG, BBP_CSR_CFG_BUSY, &reg)) {
        reg = 0;
        rt2x00_set_field32(&reg, BBP_CSR_CFG_VALUE, value);
        rt2x00_set_field32(&reg, BBP_CSR_CFG_REGNUM, word);
        rt2x00_set_field32(&reg, BBP_CSR_CFG_BUSY, 1);
        rt2x00_set_field32(&reg, BBP_CSR_CFG_READ_CONTROL, 0);
        rt2x00_set_field32(&reg, BBP_CSR_CFG_BBP_RW_MODE, 1);

        rt2800_register_write_lock(dev, BBP_CSR_CFG, reg);
    }
}

u8 rt2800_bbp_read(struct rt_dev *dev, const unsigned int word) {
    u32 reg = 0;
    u8 value;

    if (rt2800_regbusy_read(dev, BBP_CSR_CFG, BBP_CSR_CFG_BUSY, &reg)) {
        reg = 0;
        rt2x00_set_field32(&reg, BBP_CSR_CFG_REGNUM, word);
        rt2x00_set_field32(&reg, BBP_CSR_CFG_BUSY, 1);
        rt2x00_set_field32(&reg, BBP_CSR_CFG_READ_CONTROL, 1);
        rt2x00_set_field32(&reg, BBP_CSR_CFG_BBP_RW_MODE, 1);

        rt2800_register_write_lock(dev, BBP_CSR_CFG, reg);

        rt2800_regbusy_read(dev, BBP_CSR_CFG, BBP_CSR_CFG_BUSY, &reg);
    }

    value = (u8)rt2x00_get_field32(reg, BBP_CSR_CFG_VALUE);
    return value;
}

void rt2800_rfcsr_write(struct rt_dev *dev, const unsigned int word, const u8 value) {
    u32 reg;

    if (rt2800_regbusy_read(dev, RF_CSR_CFG, RF_CSR_CFG_BUSY, &reg)) {
        reg = 0;
        rt2x00_set_field32(&reg, RF_CSR_CFG_DATA, value);
        rt2x00_set_field32(&reg, RF_CSR_CFG_REGNUM, word);
        rt2x00_set_field32(&reg, RF_CSR_CFG_WRITE, 1);
        rt2x00_set_field32(&reg, RF_CSR_CFG_BUSY, 1);

        rt2800_register_write_lock(dev, RF_CSR_CFG, reg);
    }
}

u8 rt2800_rfcsr_read(struct rt_dev *dev, const unsigned int word) {
    u32 reg = 0;
    u8 value;

    if (rt2800_regbusy_read(dev, RF_CSR_CFG, RF_CSR_CFG_BUSY, &reg)) {
        reg = 0;
        rt2x00_set_field32(&reg, RF_CSR_CFG_REGNUM, word);
        rt2x00_set_field32(&reg, RF_CSR_CFG_WRITE, 0);
        rt2x00_set_field32(&reg, RF_CSR_CFG_BUSY, 1);

        rt2800_register_write_lock(dev, RF_CSR_CFG, reg);

        rt2800_regbusy_read(dev, RF_CSR_CFG, RF_CSR_CFG_BUSY, &reg);
    }

    value = (u8)rt2x00_get_field32(reg, RF_CSR_CFG_DATA);
    return value;
}

void rt2800_rf_write(struct rt_dev *dev, const unsigned int word, const u32 value) {
    u32 reg;

    if (rt2800_regbusy_read(dev, RF_CSR_CFG0, RF_CSR_CFG0_BUSY, &reg)) {
        reg = 0;
        rt2x00_set_field32(&reg, RF_CSR_CFG0_REG_VALUE_BW, value);
        rt2x00_set_field32(&reg, RF_CSR_CFG0_STANDBYMODE, 0);
        rt2x00_set_field32(&reg, RF_CSR_CFG0_SEL, 0);
        rt2x00_set_field32(&reg, RF_CSR_CFG0_BUSY, 1);

        rt2800_register_write_lock(dev, RF_CSR_CFG0, reg);
        rt2x00_rf_write(dev, word, value);
    }
}

/* EEPROM word map (rt2800lib.c, standard map — RT3070 does not use the ext
 * map).  EEPROM_MAC_ADDR_0..2 live at words 2/3/4. */
static const unsigned int rt2800_eeprom_map[EEPROM_WORD_COUNT] = {
    [EEPROM_CHIP_ID]            = 0x0000,
    [EEPROM_VERSION]            = 0x0001,
    [EEPROM_MAC_ADDR_0]         = 0x0002,
    [EEPROM_MAC_ADDR_1]         = 0x0003,
    [EEPROM_MAC_ADDR_2]         = 0x0004,
    [EEPROM_NIC_CONF0]          = 0x001a,
    [EEPROM_NIC_CONF1]          = 0x001b,
    [EEPROM_FREQ]               = 0x001d,
    [EEPROM_LED_AG_CONF]        = 0x001e,
    [EEPROM_LED_ACT_CONF]       = 0x001f,
    [EEPROM_LED_POLARITY]       = 0x0020,
    [EEPROM_NIC_CONF2]          = 0x0021,
    [EEPROM_LNA]                = 0x0022,
    [EEPROM_RSSI_BG]            = 0x0023,
    [EEPROM_RSSI_BG2]           = 0x0024,
    [EEPROM_TXMIXER_GAIN_BG]    = 0x0024,
    [EEPROM_RSSI_A]             = 0x0025,
    [EEPROM_RSSI_A2]            = 0x0026,
    [EEPROM_TXMIXER_GAIN_A]     = 0x0026,
    [EEPROM_EIRP_MAX_TX_POWER]  = 0x0027,
    [EEPROM_TXPOWER_DELTA]      = 0x0028,
    [EEPROM_TXPOWER_BG1]        = 0x0029,
    [EEPROM_TXPOWER_BG2]        = 0x0030,
    [EEPROM_TSSI_BOUND_BG1]     = 0x0037,
    [EEPROM_TSSI_BOUND_BG2]     = 0x0038,
    [EEPROM_TSSI_BOUND_BG3]     = 0x0039,
    [EEPROM_TSSI_BOUND_BG4]     = 0x003a,
    [EEPROM_TSSI_BOUND_BG5]     = 0x003b,
    [EEPROM_TXPOWER_A1]         = 0x003c,
    [EEPROM_TXPOWER_A2]         = 0x0053,
    [EEPROM_TXPOWER_INIT]       = 0x0068,
    [EEPROM_TSSI_BOUND_A1]      = 0x006a,
    [EEPROM_TSSI_BOUND_A2]      = 0x006b,
    [EEPROM_TSSI_BOUND_A3]      = 0x006c,
    [EEPROM_TSSI_BOUND_A4]      = 0x006d,
    [EEPROM_TSSI_BOUND_A5]      = 0x006e,
    [EEPROM_TXPOWER_BYRATE]     = 0x006f,
    [EEPROM_BBP_START]          = 0x0078,
};

u16 rt2800_eeprom_read(struct rt_dev *dev, const unsigned int word) {
    return dev->eeprom[rt2800_eeprom_map[word]];
}

u16 rt2800_eeprom_read_from_array(struct rt_dev *dev, const unsigned int word,
                                  unsigned int index) {
    return dev->eeprom[rt2800_eeprom_map[word] + index];
}
