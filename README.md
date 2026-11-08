# 📶 RT2800USB-for-Cact

Ralink **RT28xx/RT30xx USB Wi-Fi** driver for CactOS, shipped as the kernel
module **`rt2800usb.cctk`** (Ralink `rt2800usb` family — the bring-up target is
an **RT3070** dongle).

The module is a relocatable ET_REL image loaded by the kernel's **USB module
loader** (`Cact/drivers/usb/loader/usb_mod.c`).  It exports `usb_driver_init()`,
which calls the ksym-exported `usb_driver_register()` with a `usb_driver_t` for
the Ralink USB IDs; the driver's `probe()` runs the device bring-up and drives
transfers through the `usb_device_t`'s host-controller ops.  Firmware
(`rt2870.bin`) is staged out of cctkfs with the kernel's `request_firmware()`.

> **Status:** hardware bring-up is in — MCU firmware upload, EEPROM/MAC read,
> MAC/BBP/RF init, 2.4 GHz channel tuning and the bulk-endpoint TX/RX rings.
> The driver exposes `/dev/wlan0` as a dumb radio (SCAN, SET_CHANNEL, SET_BSSID,
> SET_KEY, STATUS); association/MLME and WPA2 live in the userspace `wljoin`,
> with AES-CCMP implemented in software in this module.

---

## 🔨 Building

**Recommended — full workspace**

```sh
ninja -C CactOS-x86_64/build-meson stage
```

**Standalone**

```sh
meson setup build-meson --cross-file cross/x86_64-cact-clang.ini \
    -Dkern_root=../CactKernel-x86_64 -Dlocal_repo=../LocalRepoCactOS-x86_64
ninja -C build-meson          # → build-meson/rt2800usb.cctk
ninja -C build-meson stage    # copy into ../LocalRepoCactOS-x86_64/lib/
```

Override paths with `meson configure build-meson -Dkern_root=… -Dlocal_repo=…`.

---

## 🚀 Using it

```
modload /usr/lib/modules/rt2800usb.cctk
```

The dongle may be plugged in before or after `modload`: when the driver
registers, the USB core offers it every already-enumerated device that no other
driver owns.
