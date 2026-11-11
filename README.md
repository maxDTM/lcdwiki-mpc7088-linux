# lcdwiki-mpc7088-linux

Linux kernel driver for the **lcdwiki 7" PC Sub Display (SKU MPC7088)**, a 1024x600 USB "sub display" that is sold as Windows-only.

This is a fork of [rhgndf/ms912x](https://github.com/rhgndf/ms912x) (branch `kernel-7.2`), an open-source DRM driver for MacroSilicon MS912x USB display chips. Upstream already recognizes this monitor's USB ID, but the screen stays dark: red LED, no backlight. This fork adds the fixes that make the integrated panel work. See [What was wrong](#what-was-wrong).

## The panel

| | |
|---|---|
| Product | 7inch PC Sub Display, SKU **MPC7088** ([lcdwiki listing](https://www.lcdwiki.com/7inch_PC_Sub_Display)) |
| Also sold as | Hosyond 7" 1024x600 portable secondary display ([Amazon B0F32HRYDP](https://www.amazon.com/Hosyond-1024x600-Portable-Secondary-Windows7/dp/B0F32HRYDP)) |
| Panel | 7" IPS, 1024x600, 350 cd/m², no touch |
| Active area | 154.08 x 85.92 mm |
| Module size | 171.8 x 108.6 x 9.0 mm |
| Interface | USB-C (data + 5 V power), connects to any USB 2.0/3.0 host port. Two USB-C ports on the same edge; both work. |
| Power | 2.39 W |
| Backlight | adjustable via buttons on the unit |
| Status LED | red = no valid output, green = displaying |
| Vendor-quoted frame rate | 25 FPS |
| USB ID | `345f:9132` "USB Display / usb extscreen", runs at USB 2.0 High Speed |
| Chip | MacroSilicon MS912x family (chip ID `b7 16 0a` at reg `0xf000`), video port type 6 (digital / integrated panel) |
| EDID | manufacturer `HJW`, name `USBDISPLAY`, one detailed timing 1024x600 @ 60.004 Hz, 51.21 MHz |
| Vendor driver (Windows) | [WinUSBDisplay_Windows_V3.1.4.68_QD.zip](https://qdtft-1353618751.cos.ap-beijing.myqcloud.com/WinUSBDisplay_Windows_V3.1.4.68_QD.zip) |
| Original Linux driver | [github.com/rhgndf/ms912x](https://github.com/rhgndf/ms912x) |

## What this project can do

- Drive the panel at its native **1024x600 @ 60 Hz** on Linux as a normal DRM/KMS display. It works under Wayland compositors (tested with Hyprland), X11, and the framebuffer console (`/dev/fbN`).
- Work over a single USB cable from either USB-C port on the monitor, on a plain USB-A or USB-C host port. No dock or DisplayPort Alt Mode is needed.
- Survive hot-plugging: unplug and replug the monitor and it comes back.
- Build as a DKMS module, so it rebuilds automatically on kernel updates.
- Leave other ms912x devices (HDMI/VGA adapters) unaffected. The panel-specific behavior only applies when the chip reports an integrated digital panel, its flash has no timing, and its EDID asks for 1024x600.

## What it can't do (yet)

- **No touch.** The panel has no touch hardware.
- **No software brightness control.** Use the buttons on the monitor.
- **Limited frame rate.** Every frame is compressed to YUV 4:2:2 and pushed over USB 2.0. Fine for text, terminals and dashboards; video and fast motion will be choppy. The vendor quotes 25 FPS.
- **Colors may look slightly washed out.** The driver sends limited-range (16-235) BT.601 YUV. The panel may expect full range. Not yet addressed.
- **Only 1024x600.** Other timings leave the panel dark, so the driver doesn't offer them. Scale in your compositor if you need a different logical size.
- **Tested only on** Arch Linux, kernel 7.2.15 (linux-surface), Surface Pro 6, Hyprland 0.57. Upstream has branches for other kernels (5.10-6.18), but this patch has only been applied to and tested on the `kernel-7.2` base.
- **Suspend/resume** has code paths for it but hasn't been tested yet.
- **Not in mainline.** It's an out-of-tree module. With Secure Boot enabled you must sign it yourself.
- **Compositor hotplug.** A Wayland compositor that was already running when the module first loaded may grab the device without creating an output for it. Restart the compositor (log out and back in) once after installing.
- macOS and Windows are out of scope (Windows has the vendor driver).

## Install (DKMS)

```sh
# Arch: sudo pacman -S dkms linux-headers   (or your kernel's -headers package)
git clone https://github.com/maxDTM/lcdwiki-mpc7088-linux.git
sudo cp -r lcdwiki-mpc7088-linux /usr/src/ms912x-0.1
sudo dkms install ms912x/0.1
sudo modprobe ms912x
```

Then plug in the monitor. After about 3 seconds the LED turns green and a new output appears. In `/sys/class/drm` it shows up as `card*-DPI-1`, and in `modetest -M ms912x -c`.

To quickly test without installing:

```sh
make
sudo insmod ms912x.ko
```

### Hyprland example

```ini
# hyprland.conf: adjust the position to your layout
monitor = DPI-1, 1024x600@60, 3840x0, 1
```

### Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `boot_delay_ms` | `3000` | Time to wait after USB connect (and after a reset-resume) before talking to `345f:*` chips. Max 10000. |

## What was wrong

The investigation, in short:

1. **No usable mode.** The panel's EDID asks for 1024x600, but upstream's mode table, which was measured on a different MS912x chip (`534d:6021`), has no 1024x600 entry, and this unit's flash custom-timing area is blank (`0xff`). So the driver only offered 800x600, which the panel can't show.
   The vendor's `config.ini` lists the panel timing as VIC 150: total 1344x635, sync 40/5, offsets 160/23, 51.21 MHz. Selecting chip mode code `0x96` and reading back registers `0xf398..` confirmed that the chip has exactly this timing built in. The fork adds it as a fallback custom mode.
2. **The probe-time reset kills the panel.** Upstream sends a 720x480 (code `0x02`) mode at probe "to stop weird behavior". On an integrated panel this leaves the output dead (red LED) until the monitor is power-cycled. The fork skips that reset for digital-panel devices.
3. **Chip boot window.** For about 1-2 s after power-up the chip runs its own firmware (LED briefly green). Any command sent in that window wedges the panel output. Upstream probes immediately, so a cold plug-in always failed, even though a later soft re-enumeration worked. The fork waits `boot_delay_ms` after connect before talking to the chip.

The diagnosis used register read-backs and a stepped power/mode test in the driver, while watching the status LED.

## License

GPL-2.0, same as upstream (see `LICENSE`). Upstream's original README is kept as [`README.upstream.md`](README.upstream.md).

Not affiliated with lcdwiki, Hosyond, or MacroSilicon.
