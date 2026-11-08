#ifndef RT2X00_COMPAT_H
#define RT2X00_COMPAT_H

/*
 * Freestanding shims so the vendored rt2800 register header (rt2800_regs.h)
 * and the routines transcribed from the Linux rt2x00 driver compile inside a
 * Cact kernel module without any Linux headers.
 */

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;

typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint16_t __be16;
typedef uint32_t __be32;

#ifndef __cplusplus
typedef int bool;
#define true  1
#define false 0
#endif

#define BIT(n)        (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define __ffs(x)      ((unsigned)__builtin_ctz((unsigned)(x)))

/* Attribute shims used by the vendored register header. */
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif

/* The transcribed routines keep their rt2x00_info/warn/err call sites, but the
 * driver does not print: failures travel back to userspace as negative ioctl
 * return values (see the RT_E* codes in rt2800_local.h).  These compile away. */
#define rt2x00_info(dev, fmt, ...) do { (void)(dev); } while (0)
#define rt2x00_warn(dev, fmt, ...) do { (void)(dev); } while (0)
#define rt2x00_err(dev, fmt, ...)  do { (void)(dev); } while (0)

/* Exported to modules (see the kernel's ksym.c); used to build the /dev/wlan0
 * scan list. */
extern int  snprintf(char *buf, size_t size, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* Field access (mirrors rt2x00reg.h)                                  */
/* ------------------------------------------------------------------ */

struct rt2x00_field8  { u8  bit_offset; u8  bit_mask; };
struct rt2x00_field16 { u16 bit_offset; u16 bit_mask; };
struct rt2x00_field32 { u32 bit_offset; u32 bit_mask; };

#define FIELD8(__mask)  ((struct rt2x00_field8){  (u8)__ffs(__mask),  (u8)(__mask)  })
#define FIELD16(__mask) ((struct rt2x00_field16){ (u16)__ffs(__mask), (u16)(__mask) })
#define FIELD32(__mask) ((struct rt2x00_field32){ (u32)__ffs(__mask), (u32)(__mask) })

static inline void rt2x00_set_field32(u32 *reg, struct rt2x00_field32 f, u32 value) {
    *reg = (*reg & ~f.bit_mask) | ((value << f.bit_offset) & f.bit_mask);
}

static inline u32 rt2x00_get_field32(u32 reg, struct rt2x00_field32 f) {
    return (reg & f.bit_mask) >> f.bit_offset;
}

static inline void rt2x00_set_field16(u16 *reg, struct rt2x00_field16 f, u16 value) {
    *reg = (u16)((*reg & ~f.bit_mask) | ((value << f.bit_offset) & f.bit_mask));
}

static inline u16 rt2x00_get_field16(u16 reg, struct rt2x00_field16 f) {
    return (u16)((reg & f.bit_mask) >> f.bit_offset);
}

static inline void rt2x00_set_field8(u8 *reg, struct rt2x00_field8 f, u8 value) {
    *reg = (u8)((*reg & ~f.bit_mask) | ((value << f.bit_offset) & f.bit_mask));
}

static inline u8 rt2x00_get_field8(u8 reg, struct rt2x00_field8 f) {
    return (u8)((reg & f.bit_mask) >> f.bit_offset);
}

/* ------------------------------------------------------------------ */
/* Chip ids (rt2x00.h)                                                 */
/* ------------------------------------------------------------------ */

#define RT2460   0x2460
#define RT2560   0x2560
#define RT2570   0x2570
#define RT2661   0x2661
#define RT2573   0x2573
#define RT2860   0x2860
#define RT2872   0x2872
#define RT2883   0x2883
#define RT3070   0x3070
#define RT3071   0x3071
#define RT3090   0x3090
#define RT3290   0x3290
#define RT3352   0x3352
#define RT3390   0x3390
#define RT3572   0x3572
#define RT3593   0x3593
#define RT3883   0x3883
#define RT5350   0x5350
#define RT5390   0x5390
#define RT5392   0x5392
#define RT5592   0x5592
#define RT6352   0x6352

/* ------------------------------------------------------------------ */
/* Device helpers — operate on the module's own context (rt2800_local.h) */
/* ------------------------------------------------------------------ */

#define rt2x00_rt(rd, chip)            ((rd)->rt == (chip))
#define rt2x00_rf(rd, chip)            ((rd)->rf == (chip))
#define rt2x00_rt_rev(rd, chip, revision)   ((rd)->rt == (chip) && (rd)->rev == (revision))
#define rt2x00_rt_rev_lt(rd, chip, revision)  ((rd)->rt == (chip) && (rd)->rev < (revision))
#define rt2x00_rt_rev_gte(rd, chip, revision) ((rd)->rt == (chip) && (rd)->rev >= (revision))
#define rt2x00_is_usb(rd)  (1)
#define rt2x00_is_pci(rd)  (0)
#define rt2x00_is_soc(rd)  (0)
#define rt2x00_is_pcie(rd) (0)

#endif /* RT2X00_COMPAT_H */
