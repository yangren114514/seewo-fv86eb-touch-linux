# nbm-touchd

**Userspace Linux touch driver for the Seewo FV86EB interactive flat panel (CVTE `1ff7:0f26` HID touch controller).**

The in-tree `hid-multitouch` driver binds to this panel and creates an input
device with all the right `ABS_MT_*` axes — but it never emits a single event.
`/dev/hidraw` shows the controller is happily streaming 62-byte reports. This
project reads those reports directly and injects standard multi-touch events
through `/dev/uinput`, which `libinput`/KDE/GNOME/X11 accept as a normal
touchscreen.

No kernel patch, no initramfs rebuild, no kernel module.

---

## Table of contents

- [The problem](#the-problem)
- [The protocol](#the-protocol)
- [How it works](#how-it-works)
- [Install](#install)
- [Usage](#usage)
- [Verification](#verification)
- [Capturing data from Windows](#capturing-data-from-windows)
- [How this was reverse engineered](#how-this-was-reverse-engineered)
- [Known limitations](#known-limitations)
- [Tested on](#tested-on)
- [License](#license)
- [中文说明](#中文说明)

---

## The problem

On a Seewo FV86EB (86" interactive flat panel) running Debian 13 / kernel
6.12, the touch controller enumerates as:

```
ID 1ff7:0f26 CVTE Touch Device
  iManufacturer: CVTE
  iProduct:      Touch Device
  bcdDevice 1.00, full speed (12 Mbps), HID 1.11

  Interface 0: HID, 2 endpoints
    EP 0x81  IN   interrupt, 64 bytes, bInterval 1
    EP 0x02  OUT  interrupt, 64 bytes, bInterval 1
    Report descriptor length: 1177 bytes

  Interface 1: HID, 0 endpoints   <-- decoy interface, see note below
```

The kernel does everything right:

```
hid-multitouch 0003:1FF7:0F26.0002: input,hiddev0,hidraw2: USB HID v1.11
/proc/bus/input/devices:
  N: Name="CVTE Touch Device"
  H: Handlers=mouse0 event5
  B: PROP=2                     # INPUT_PROP_DIRECT
  B: KEY=400                    # BTN_TOUCH
  B: ABS=273800000000003        # ABS_X/Y, ABS_MT_SLOT, ABS_MT_POSITION_X/Y,
                                # ABS_MT_TRACKING_ID, ABS_MT_TOUCH_MAJOR/MINOR,
                                # ABS_MT_ORIENTATION
```

…and yet:

```
cat /dev/hidraw2 | hexdump -C    -> data flows (~50 reports/s while touching)
cat /dev/input/event5            -> absolutely nothing, ever
```

The device reports, the kernel parses nothing into events. That is the bug this
project works around.

> **Note on interface 1.** `usbhid 1-3.1:1.1: couldn't find an input interrupt
> endpoint` is printed at probe time. It is harmless: interface 1 declares
> `bNumEndpoints = 0`, so it is a decoy descriptor block. It is not the cause of
> the missing events.

---

## The protocol

The panel uses the standard **Windows 8 compatible multi-touch** report format.
The 500-byte report descriptor for the touch collection decodes to:

```
Usage Page (Digitizer, 0x0D)
Usage (Touch Screen, 0x04)
Collection (Application)
  Report ID (2)
  Finger collection, repeated 6 times:
    Usage (Tip Switch)       1 bit
    Usage (In Range)         1 bit
    Usage (Confidence)       1 bit
    padding                  5 bit
    Usage (Contact Identifier)  8 bit
    Usage (X)               16 bit   logical 0..32767
    Usage (Y)               16 bit   logical 0..32767
    Usage (Width)           16 bit
    Usage (Height)          16 bit
  Usage (Contact Count)      8 bit
  Report ID (3)              Feature, 1 byte, Usage (Contact Max)
End Collection
```

So each **input report is 62 bytes**:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Report ID = `0x02` |
| 1 + n*10 | 1 | flags — bit0 `Tip Switch`, bit1 `In Range`, bit2 `Confidence` |
| 2 + n*10 | 1 | contact identifier |
| 3 + n*10 | 2 | **X**, uint16 little-endian, 0..32767 |
| 5 + n*10 | 2 | **Y**, uint16 little-endian, 0..32767 |
| 7 + n*10 | 2 | width |
| 9 + n*10 | 2 | height |
| 61 | 1 | contact count |

`n` runs 0..5 (six concurrent contacts).

Physical size encoded in the descriptor: X max = 8687 (86.87 cm),
Y max = 4983 (49.83 cm) — i.e. an 86" 16:9 panel.

### Coordinate orientation

Measured on the real panel with known touch positions:

| Touched | X range | Y range |
|---|---|---|
| top-left | 309 – 1207 | 756 – 2750 |
| top-right | 31446 – 32767 | 1225 – 7215 |
| bottom-left | 0 – 705 | 29719 – 32767 |
| bottom-right | 31412 – 32188 | 29580 – 31869 |
| centre | 15568 – 16533 | 15109 – 18004 |

Direct mapping, no swap, no inversion.

### Feature report

Report ID 3 (1 byte, `Usage 0x55 Contact Max`) must be written to the device.
Without it the controller stays in a low-rate mode: observed **~0.36 reports/s**
before, **~53 reports/s** after. `nbm-touchd` sends it immediately after opening
the hidraw node.

---

## How it works

```
┌──────────────────┐
│  /dev/hidrawN    │  62-byte report ID 2
│  (1ff7:0f26)     │
└────────┬─────────┘
         │ read()
         ▼
┌──────────────────┐
│   nbm-touchd     │  parse 6 finger records,
│                  │  map contact id -> MT slot
└────────┬─────────┘
         │ write()
         ▼
┌──────────────────┐
│  /dev/uinput     │  virtual MT touchscreen
│                  │  ABS_MT_SLOT / TRACKING_ID /
│                  │  POSITION_X / POSITION_Y /
│                  │  TOUCH_MAJOR / WIDTH_MAJOR,
│                  │  BTN_TOUCH, INPUT_PROP_DIRECT
└────────┬─────────┘
         │
         ▼
   libinput / KDE / GNOME / X11
```

The daemon also:

- retries every 2 s if the device is not present yet (USB re-enumeration safe)
- re-opens the hidraw node if the device disappears
- re-sends the feature report if no frames arrive for 10 s
- logs a report rate every 10 s
- caps its log file at 1 MB with rotation

---

## Install

```bash
git clone https://github.com/yangren114514/seewo-fv86eb-touch-linux.git
cd seewo-fv86eb-touch-linux
make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now nbm-touchd
```

Requires `gcc` and kernel headers (`linux/uinput.h`, `linux/hidraw.h`).

Check it came up:

```bash
systemctl status nbm-touchd
journalctl -u nbm-touchd -n 30
```

Expected log lines:

```
[info] found hidraw /dev/hidraw2 for 1ff7:0f26
[info] sent feature report 3 = 0x0A (10)
[info] uinput device created: "NBM Virtual Touchscreen"
[info] rate: 512 frames in 10.0s (51.1 fps)
```

---

## Usage

```
nbm-touchd [options]

  --debug                  verbose per-contact logging
  --probe                  probe the device, then exit
  --probe-seconds N        probe duration (default 15)
  --replay FILE            replay a capture file through the parser + uinput
  --replay-delay-ms N      delay between replayed frames
  --feature-value N        value for HID feature report 3 (default 10)
  --selftest               create the uinput device and inject synthetic events
  --help
```

Environment variable `NBM_TOUCH_FEATURE_VALUE` sets the feature report value when
`--feature-value` is not given.

### `--probe` — the first thing to run on a new panel

```bash
sudo nbm-touchd --probe
```

```
[probe] looking for hidraw 1ff7:0f26 ...
[probe] found /dev/hidraw2
[probe] HIDIOCSFEATURE(report 3) ok, value=0x0A
[probe] 15.0s: 780 frames (52.0 fps)
[probe] report lengths seen: 62 x780
[probe] first frames:
  02 07 00 35 01 f4 02 88 00 ee 00 ...
```

If the frame rate is low, try other feature values:

```bash
sudo nbm-touchd --probe --feature-value 5
sudo nbm-touchd --probe --feature-value 1
sudo nbm-touchd --probe --feature-value 127
```

### `--replay` — offline validation

Replays a capture file produced by the Windows capture tool (see below) through
the exact same parse + uinput path, without needing the hardware:

```bash
sudo nbm-touchd --replay capture.txt
# in another terminal
sudo evtest        # pick "NBM Virtual Touchscreen"
```

This is useful for regression testing on a machine that does not have the panel.

---

## Verification

```bash
# the virtual device should appear as a touchscreen
sudo libinput list-devices | grep -A8 -i 'NBM Virtual'

# raw events
sudo evtest                  # select "NBM Virtual Touchscreen"

# check that libinput classified it as touch, not touchpad
sudo libinput debug-events
```

`evtest` should show, for each finger:

```
Event: type 3 (EV_ABS), code 57 (ABS_MT_TRACKING_ID), value 0
Event: type 3 (EV_ABS), code 53 (ABS_MT_POSITION_X), value 309
Event: type 3 (EV_ABS), code 54 (ABS_MT_POSITION_Y), value 756
Event: type 3 (EV_ABS), code 48 (ABS_MT_TOUCH_MAJOR), value 238
Event: type 3 (EV_ABS), code 50 (ABS_MT_WIDTH_MAJOR), value 136
Event: type 1 (EV_KEY), code 330 (BTN_TOUCH), value 1
Event: type 0 (EV_SYN), code 0 (SYN_REPORT), value 0
```

---

## Capturing data from Windows

You cannot reverse engineer a touch protocol from Linux alone when the Linux
driver emits nothing — you do not know whether a byte means X, Y, pressure, or
nothing at all. The trick used here:

1. Boot the same panel under Windows, where touch works.
2. Capture the raw HID reports while touching **known screen positions**
   (four corners + centre).
3. Correlate timestamps with positions to identify the coordinate fields.

On Windows the touch collection cannot be opened with `CreateFile` — the
Windows touch driver holds it exclusively (`ERROR_SHARING_VIOLATION` / `err=32`).
Use the **Raw Input API** instead, which taps the report stream at the system
level:

```
RegisterRawInputDevices(usUsagePage=0x0D, usUsage=0x04, RIDEV_INPUTSINK)
  -> WM_INPUT
  -> GetRawInputData(RID_INPUT) -> RAWHID { dwSizeHid, dwCount, bRawData[] }
```

The raw report descriptor can be obtained on Windows with
`HidSharp.HidDevice.GetRawReportDescriptor()`;
`DeviceIoControl(IOCTL_HID_GET_REPORT_DESCRIPTOR)` returns
`ERROR_INVALID_FUNCTION` from user mode on this device.

See [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md) for the full
walkthrough.

---

## How this was reverse engineered

Short version:

1. `lsusb -v` → interface layout, endpoint sizes, report descriptor length.
2. `/proc/bus/input/devices` → what the kernel created, `ABS` capability bitmap.
3. **Key fork in the road:** hidraw has data but evdev has no events
   → the transport is fine, the *parsing* is wrong.
4. Grab the HID report descriptor (`/sys/kernel/debug/hid/*/rdesc` on Linux,
   HidSharp on Windows) → the field layout is standard Win8 multi-touch.
5. Capture raw reports in Windows while touching known positions → confirm the
   coordinate offsets and orientation.
6. Write a `uinput` userspace driver.
7. Validate offline with `--replay` against the real capture before touching
   hardware again.

Full write-up: [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md).
Protocol reference: [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

---

## Known limitations

- Verified on **one** panel model (Seewo FV86EB, CVTE `1ff7:0f26`). Other CVTE
  panels with the same controller family will probably work; other vendors will
  not.
- Maximum 6 concurrent contacts — that is a hardware limit encoded in the
  descriptor, not a driver limit.
- The feature report value (`10`) was chosen empirically. The descriptor only
  says "Contact Max, logical max 128"; the panel ignores values it does not
  like, so `--probe --feature-value N` is provided to experiment.
- If a future kernel learns to parse this panel correctly, this daemon becomes
  unnecessary. That would be the better outcome — patches welcome upstream.
- No pressure/altitude support (the descriptor does not expose them).

---

## Tested on

| Item | Value |
|---|---|
| Device | CVTE Touch Device `1ff7:0f26` |
| Panel | **Seewo FV86EB** (86", 3840×2160) |
| Mainboard | MT61A-JHA |
| OS | Debian 13 (trixie) |
| Kernel | 6.12.107+deb13-amd64 |
| Display server | KDE Plasma 6 — Wayland **and** X11 |
| Build | gcc 14.2.0 |

---

## License

GPL-2.0. See [LICENSE](LICENSE).

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.

---

## 中文说明

**用途**：希沃 FV86EB（86 寸交互白板）的触摸屏在 Linux 下无法工作 —— 内核
`hid-multitouch` 能识别设备、能创建 input 节点、各种 `ABS_MT_*` 轴都齐全，但
一条事件都产生不出来；而 `/dev/hidraw` 里明明有数据在流动。

本项目绕开内核驱动，直接用用户态程序读取 `/dev/hidraw`，按标准 Windows 8 多点
触摸协议解析，再通过 `/dev/uinput` 注入标准触摸事件。**不需要打内核补丁、不需要
重建 initramfs、不需要编译内核模块。**

**安装**：

```bash
make
sudo make install
sudo systemctl enable --now nbm-touchd
```

**排查**：

```bash
sudo nbm-touchd --probe        # 看设备在不在、上报率多少
sudo evtest                     # 选 "NBM Virtual Touchscreen" 看事件
journalctl -u nbm-touchd -f     # 看日志（每 10 秒打印一次上报率）
```

**协议要点**：Report ID 2，62 字节，6 个手指记录 × 10 字节，
X 在 +2..3（小端，0..32767），Y 在 +4..5，坐标直接映射无需翻转。
开机后要先发 Feature Report ID 3，否则设备只有 0.36 帧/秒。
