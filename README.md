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

- Drive the panel at its native **1024x600 @ 60 Hz** on Linux as a normal DRM/KMS display. Verified with the framebuffer (`/dev/fbN`), KMS (`modetest`), and as a regular desktop output under **Hyprland 0.57** (with `aq_evdi_compat=1`, see below). X11 is untested.
- Work over a single USB cable from either USB-C port on the monitor. Tested on the host's own USB port and through a Surface Dock. No DisplayPort Alt Mode is needed.
- Survive hot-plugging and suspend/resume: tested under Hyprland with unplug and replug and `systemctl suspend`. After DPMS off/on the display was working again, though the blanking itself wasn't watched.
- **Pace the compositor to USB speed.** The page-flip completes only when a transfer buffer is free (waiting up to 1 s), so the compositor isn't driven faster than USB 2.0 can carry frames. Upstream dropped any frame that found both buffers busy, which could leave the last update unsent. Now a frame is only lost if a transfer times out, and then the damage is resent with the next update.
- **Recover from a wedged chip.** After 3 consecutive failed frame transfers, or a failed power-on or mode set, the driver resets the USB device. That unbinds and re-probes it, so the compositor sees an ordinary hotplug. Resets are limited to 1 per 30 s and 3 per 5 min. Tested with a forced reset under Hyprland.
- **Full-range color** on integrated panels. Limited-range YUV made black grey and the picture washed out.
- Build as a DKMS module, so it rebuilds automatically on kernel updates.
- Mostly leave other ms912x devices alone. Exactly what changes for them:
  - The 1024x600 fallback timing is only added when the chip reports a digital (integrated) port, its flash has no custom timing, and its EDID asks for 1024x600.
  - Digital-port devices with any custom timing are limited to those custom modes.
  - The probe-time 720x480 reset is skipped for **all** digital-port devices.
  - The 3 s boot delay applies to **all** `345f:*` devices (`9132`, `9133`, `9135`), including HDMI/VGA adapters with that vendor ID.
  - The probe retries its first chip reads (3 attempts, 200 ms apart) on transient USB errors. It fails if the video-port register still can't be read; upstream would have carried on.
  - Shorter USB timeouts (1 s instead of 5 s), frame pacing, and automatic reset recovery apply to **all** devices.
  - YUV range is full for digital-port devices and limited for HDMI/VGA, as upstream does. Override with `full_range`.

## What it can't do (yet)

- **No touch.** The panel has no touch hardware.
- **No software brightness control.** Use the buttons on the monitor.
- **Limited frame rate.** Every frame is converted to YUV 4:2:2 on the CPU and pushed over USB 2.0, about 1.2 MB per full frame. Moving windows is noticeably slower than on a normal monitor but usable. Fine for text, terminals and dashboards; video and fast motion will be choppy. The vendor quotes 25 FPS.
- **Through a USB hub or dock it may bounce on plug-in and resume.** Through a Surface Dock the monitor sometimes dropped off the bus once or twice a few seconds after the backlight came on, before staying up. It never did that on the host's own port. **Insufficient bus power** is suspected but not verified: the panel draws ~2.4 W, and lcdwiki recommends a USB 3.0 port or enough power on USB 2.0. Powering the monitor's second USB-C port from a charger may help. The driver copes with the bounces (it re-probes each time), but expect a few seconds of flicker.
- **Only 1024x600.** Other timings leave the panel dark, so the driver doesn't offer them. Scale in your compositor if you need a different logical size.
- **Tested only on** Arch Linux, kernel 7.2.15 (linux-surface), Surface Pro 6. The DKMS config refuses to build for kernels older than 7.2. Upstream has branches for other kernels (5.10-6.18), but this patch has only been applied to and tested on the `kernel-7.2` base.
- **Not in mainline.** It's an out-of-tree module. With Secure Boot enabled you must sign it yourself.
- **Hyprland / aquamarine needs `aq_evdi_compat=1`.** aquamarine (0.16) only treats a GPU-less KMS device as display-only if its DRM driver is named `evdi` (DisplayLink). For any other name it tries to create an EGL renderer on the device itself, fails (`initMgpu: no renderer`), and never lights the output. With `aq_evdi_compat=1` the driver reports the name `evdi` and Hyprland drives it normally, including on hotplug. This is a workaround: anything else that keys on the `evdi` name (DisplayLink Manager, `libevdi`) could get confused, so don't enable it if you use DisplayLink devices. The proper fix belongs in aquamarine; see [`AQUAMARINE-UPSTREAM.md`](AQUAMARINE-UPSTREAM.md). Other compositors (wlroots, KWin, Mutter) are untested.
- **Software cursor only** on this output under aquamarine's display-only path.
- macOS and Windows are out of scope (Windows has the vendor driver).

## Install (DKMS)

```sh
# Arch: sudo pacman -S dkms linux-headers   (or your kernel's -headers package)
git clone https://github.com/maxDTM/lcdwiki-mpc7088-linux.git
sudo dkms install ./lcdwiki-mpc7088-linux     # copies to /usr/src/ms912x-<version> and builds
# Hyprland (aquamarine) users:
echo 'options ms912x aq_evdi_compat=1' | sudo tee /etc/modprobe.d/ms912x.conf
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
# hyprland.conf: adjust the position to your layout. Scale 4/3 (768x450 logical) makes text
# more readable; 1024x600 only divides cleanly at a few scales (1, 4/3, 1.6, 2).
monitor = DPI-1, 1024x600@60, 3840x0, 1.333333
# portrait, USB-C side up:
# monitor = DPI-1, 1024x600@60, 3840x0, 1.333333, transform, 1
```

### Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `boot_delay_ms` | `3000` | Time to wait after USB connect, a reset-resume, or a recovery reset before talking to `345f:*` chips. Max 10000. |
| `aq_evdi_compat` | `0` | Report DRM driver name `evdi` so aquamarine/Hyprland drive the display. See limitations above. |
| `full_range` | `-1` | YUV range: `-1` auto (full for integrated panels, limited otherwise), `0` limited, `1` full. |

## What was wrong

The investigation, in short:

1. **No usable mode.** The panel's EDID asks for 1024x600, but upstream's mode table, which was measured on a different MS912x chip (`534d:6021`), has no 1024x600 entry, and this unit's flash custom-timing area is blank (`0xff`). So the driver only offered 800x600, which the panel can't show.
   The vendor's `config.ini` lists the panel timing as VIC 150: total 1344x635, sync 40/5, offsets 160/23, 51.21 MHz. Selecting chip mode code `0x96` and reading back registers `0xf398..` confirmed that the chip has exactly this timing built in. The fork adds it as a fallback custom mode.
2. **The probe-time reset kills the panel.** Upstream sends a 720x480 (code `0x02`) mode at probe "to stop weird behavior". On an integrated panel this leaves the output dead (red LED) until the monitor is power-cycled. The fork skips that reset for digital-panel devices.
3. **Chip boot window.** For about 1-2 s after power-up the chip runs its own firmware (LED briefly green). Any command sent in that window wedges the panel output. Upstream probes immediately, so a cold plug-in always failed, even though a later soft re-enumeration worked. The fork waits `boot_delay_ms` after connect before talking to the chip.

The diagnosis used register read-backs and a stepped power/mode test in the driver, while watching the status LED.

4. **Compositor integration.** Even with the panel working, Hyprland never showed it. A debug log showed aquamarine treating the device as a secondary GPU and failing to create a renderer on it, because its display-only path is reserved for DisplayLink's `evdi`. Hence `aq_evdi_compat`.

## License

GPL-2.0, same as upstream (see `LICENSE`). Upstream's original README is kept as [`README.upstream.md`](README.upstream.md).

Not affiliated with lcdwiki, Hosyond, or MacroSilicon.
