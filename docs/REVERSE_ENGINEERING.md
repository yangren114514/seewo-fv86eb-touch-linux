# Reverse engineering walkthrough

How the protocol in [`PROTOCOL.md`](PROTOCOL.md) was figured out. Written up
because the *method* generalises to any HID device whose in-tree driver
misbehaves.

---

## 0. Symptom

On Debian 13 / kernel 6.12, a Seewo FV86EB interactive panel:

- touch does nothing at all
- `dmesg` shows the device probing cleanly
- `/proc/bus/input/devices` shows an input node named `CVTE Touch Device` with
  62-byte interrupts and a full set of `ABS_MT_*` axes
- `usbhid ... couldn't find an input interrupt endpoint` appears once (red
  herring — see below)

The first instinct is "driver missing". `modprobe hid_multitouch` is already
loaded, and it already claimed the device. So the driver is *not* missing.

---

## 1. Establish what the kernel actually thinks

```bash
lsusb
lsusb -d 1ff7:0f26 -v

cat /proc/bus/input/devices
```

Key fields to read from `/proc/bus/input/devices`:

```
I: Bus=0003 Vendor=1ff7 Product=0f26 Version=0111
N: Name="CVTE Touch Device"
S: Sysfs=.../0003:1FF7:0F26.0002/input/input15
H: Handlers=mouse0 event5            <-- node names
B: PROP=2                            <-- bitmap, see note
B: KEY=400 0 0 0 0 0                 <-- BTN_TOUCH present
B: ABS=273800000000003               <-- capability bitmap
```

### Reading the bitmaps

`B: ABS=` is a **hexadecimal bitmap**, least-significant bit = `ABS_X` (0).

```
0x273800000000003
  bit  0  ABS_X
  bit  1  ABS_Y
  bit 47  ABS_MT_SLOT
  bit 48  ABS_MT_TOUCH_MAJOR
  bit 49  ABS_MT_TOUCH_MINOR
  bit 52  ABS_MT_ORIENTATION
  bit 53  ABS_MT_POSITION_X
  bit 54  ABS_MT_POSITION_Y
  bit 57  ABS_MT_TRACKING_ID
```

`B: PROP=` is also a bitmap of property *indices*, not values:

```
bit 0  INPUT_PROP_POINTER
bit 1  INPUT_PROP_DIRECT      <-- what a touchscreen needs
bit 2  INPUT_PROP_BUTTONPAD
```

So `PROP=2` (binary `10`) means **bit 1 = INPUT_PROP_DIRECT** — correct for a
touchscreen. (It is easy to misread `PROP=2` as "no properties"; it is the
opposite.)

**Conclusion at this step:** the kernel built a perfect-looking touchscreen
device. Do not touch the kernel yet.

---

## 2. Find the layer where things break

The three candidate layers are:

```
device  ->  HID transport  ->  HID parser  ->  evdev  ->  libinput/DE
```

Test each:

```bash
# HID transport: does the device send bytes at all?
sudo cat /dev/hidraw2 | hexdump -C

# parser: does the kernel emit events?
sudo cat /dev/input/event5      # or: sudo evtest
```

Observed:

| layer | result |
|---|---|
| hidraw | **data flows**, ~50 reports/s while touching |
| evdev | **completely empty**, zero bytes |

**This is the decisive fork.** The device works, USB works, the kernel's HID
transport works. Only the *interpretation* is wrong. Therefore:

- do not chase USB, firmware, power management, or "the panel needs enabling"
- do not write a kernel module yet — a userspace driver is cheaper
- you need the **report descriptor**, which defines the field layout

---

## 3. Get the HID report descriptor

### Linux

```bash
sudo mount -t debugfs none /sys/kernel/debug
ls /sys/kernel/debug/hid/
sudo cat /sys/kernel/debug/hid/0003:1FF7:0F26.0002/rdesc > rdesc.bin
hexdump -C rdesc.bin
```

If debugfs is not available, `usbhid-dump -e descriptor` works too.

### Windows

Two options, in order of preference:

1. **HidSharp** (used here):

   ```csharp
   var dev = DeviceList.Local.GetHidDevices(0x1FF7, 0x0F26).First();
   byte[] rdesc = dev.GetRawReportDescriptor();
   ```

2. `DeviceIoControl(hDevice, IOCTL_HID_GET_REPORT_DESCRIPTOR, ...)` — this
   **fails from user mode** with `ERROR_INVALID_FUNCTION` (1) on this device.
   Only kernel-mode clients may use it.

Note the descriptor is **per top-level collection**. On this device Windows
exposes 11 collections, and `GetRawReportDescriptor()` must be called per
device, not once for the whole VID/PID. The touch collection is the 500-byte
one with `Usage Page 0x0D / Usage 0x04`.

---

## 4. Decode the descriptor

Walk it with the HID item encoding (`prefix | tag | size`, short items):

```
05 0D      -> Usage Page (Digitizer)
09 04      -> Usage (Touch Screen)
A1 01      -> Collection (Application)
85 02      -> Report ID (2)
09 22      -> Usage (Finger)
A1 02      -> Collection (Logical)
...
```

For each `Input` item, accumulate `Report Size x Report Count` into a bit
cursor; that gives the byte offsets within the report.

The result here is a bog-standard Windows 8 multi-touch layout — 6 fingers,
10 bytes each, plus a contact count. Full table in
[`PROTOCOL.md`](PROTOCOL.md).

**Why did `hid-multitouch` fail then?** The layout is standard, so the in-tree
driver *should* have handled it. The observed behaviour (node with correct axes,
zero events) suggests the driver's per-collection state machine does not accept
this particular combination — possibly because the six finger collections are
declared with `Usage 0x00` (undefined) for fingers 2..6 instead of a repeated
`Usage (Finger)`, which trips the driver's collection bookkeeping. That
hypothesis was not verified against kernel sources; the userspace driver made it
moot.

---

## 5. Find the coordinate mapping empirically

The descriptor tells you *where* X and Y are, but not their orientation,
scaling, or whether some vendor quirk flips them. The reliable way is to touch
**known positions** and record the raw reports with timestamps.

You cannot do this on Linux — the whole point is that Linux touch does not work
in a way you can trust. Do it on Windows, where the panel behaves.

### Why `CreateFile` on the HID collection fails

The first attempt opened each HID collection with `CreateFile` / HidSharp. Most
collections failed:

```
open failed: ...col04...  err=32    (ERROR_SHARING_VIOLATION)
open failed: ...col01...  err=5     (ERROR_ACCESS_DENIED)
```

`col04` is exactly the touch collection (`InputReportByteLength = 62`). Windows'
touch driver owns it exclusively; a normal open cannot get a handle.

### Use Raw Input instead

Raw Input taps the report stream at the system level, after the OS driver has
consumed it:

```csharp
RAWINPUTDEVICE rid = new RAWINPUTDEVICE {
    usUsagePage = 0x0D,      // Digitizer
    usUsage     = 0x04,      // Touch Screen
    dwFlags     = 0x00000100, // RIDEV_INPUTSINK (receive in background)
    hwndTarget  = form.Handle
};
RegisterRawInputDevices(new[]{ rid }, 1, Marshal.SizeOf<RAWINPUTDEVICE>());
```

Then in `WndProc`, on `WM_INPUT` (0x00FF):

```csharp
uint size = 0;
GetRawInputData(lParam, RID_INPUT, IntPtr.Zero, ref size, headerSize);
IntPtr buf = Marshal.AllocHGlobal((int)size);
GetRawInputData(lParam, RID_INPUT, buf, ref size, headerSize);
```

The returned buffer is a `RAWINPUT` header followed by `RAWHID`:

```
offset 0   dwType      uint32   (2 = RIM_TYPEHID)
offset 4   dwSize      uint32
offset 8   hDevice     uint64
offset 16  wParam      uint64
offset 24  dwSizeHid   uint32   bytes per report
offset 28  dwCount     uint32   number of reports
offset 32  bRawData[]           dwSizeHid * dwCount bytes
```

**`bRawData` is several concatenated reports** — split it by `dwSizeHid`, do not
treat it as one report.

### Guided capture

The capture tool displays, full screen and in large type, the sequence
*top-left → top-right → bottom-left → bottom-right → centre*, giving 8 seconds
per position with a countdown, and writes phase markers into the log:

```
===PHASE 1 LEFT-TOP start 8.105===
10.982 raw0 0204003501F4028800EE0000FF...
10.983 raw0 0207003501F4028800EE0000FF...
===PHASE 1 LEFT-TOP end 16.105===
```

Then per-phase min/max of the candidate fields gives the mapping directly:

| phase | X | Y |
|---|---|---|
| LEFT-TOP | 309 – 1207 | 756 – 2750 |
| RIGHT-TOP | 31446 – 32767 | 1225 – 7215 |
| BOTTOM-LEFT | 0 – 705 | 29719 – 32767 |
| BOTTOM-RIGHT | 31412 – 32188 | 29580 – 31869 |
| CENTER | 15568 – 16533 | 15109 – 18004 |

That is unambiguous: X little-endian at `+2`, Y at `+4`, 0..32767, direct
mapping.

> **Gotcha that cost an hour:** in PowerShell, `-shl` on a `[byte]` truncates to
> the operand's type. `[byte]1 -shl 8` is `0`, not `256`. Cast to `[int]` first,
> or every parsed coordinate silently loses its high byte (all values appear in
> 0..255).

---

## 6. Write the userspace driver

Minimal shape:

```c
/* 1. find the hidraw node for 1ff7:0f26 */
for each /sys/class/hidraw/hidrawN:
    read device/uevent, match HID_ID=0003:00001FF7:00000F26

/* 2. open it and send the feature report */
fd = open("/dev/hidrawN", O_RDONLY);
unsigned char feat[2] = { 0x03, 0x0A };
ioctl(fd, HIDIOCSFEATURE(2), feat);   /* length includes the report ID */

/* 3. create a virtual MT touchscreen */
ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
ioctl(ufd, UI_SET_EVBIT,  EV_ABS);
ioctl(ufd, UI_SET_EVBIT,  EV_KEY);
ioctl(ufd, UI_SET_EVBIT,  EV_SYN);
ioctl(ufd, UI_SET_ABSBIT, ABS_MT_SLOT);
ioctl(ufd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
ioctl(ufd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
ioctl(ufd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
ioctl(ufd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
ioctl(ufd, UI_SET_ABSBIT, ABS_MT_WIDTH_MAJOR);
ioctl(ufd, UI_SET_KEYBIT, BTN_TOUCH);
ioctl(ufd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);   /* touchscreen, not touchpad */

/* 4. per frame: emit slots, SYN_REPORT */
```

Two things that are easy to get wrong:

- **`INPUT_PROP_DIRECT`** — without it systemd's `input_id` classifies the device
  as a *touchpad*, and `libinput` will not treat absolute contacts as touches.
- **`ABS_MT_SLOT` bookkeeping** — assign a slot per contact id, send
  `ABS_MT_TRACKING_ID` when a slot appears, `-1` when it disappears. Sending
  only `ABS_MT_POSITION_X/Y` without slots works for one finger and breaks for
  more.

---

## 7. Validate offline before touching hardware again

Add a replay mode that feeds a recorded capture file through **the same** parse
and uinput path:

```
[replay] lines=3163 frames=3117 skipped=46 badlen=0
[replay] tip bbox: X 0..32767  Y 756..32767
```

and check the events on the other side with `evtest`:

```
type 3 (EV_ABS), code 57 (ABS_MT_TRACKING_ID), value 0
type 3 (EV_ABS), code 53 (ABS_MT_POSITION_X), value 309
type 3 (EV_ABS), code 54 (ABS_MT_POSITION_Y), value 756
type 0 (EV_SYN), code 0 (SYN_REPORT), value 0
```

`X=309 / Y=756` matching the first frame of the LEFT-TOP phase proves the whole
chain. Only then boot the panel.

---

## 8. Things that were red herrings

| Suspect | Why it looked guilty | Reality |
|---|---|---|
| Interface 1 with 0 endpoints | `usbhid` prints `couldn't find an input interrupt endpoint` | decoy descriptor block, harmless |
| `PROP=0` on a second MT node (`input19`) | looks like "no INPUT_PROP_DIRECT" | that node is not the main touchscreen; the real one (`input15`) has `PROP=2` |
| `04f3:0235` Elan device | second HID device on the bus | it is an *optical mouse*, not a second touch channel |
| Low report rate on Linux (~0.36/s) | looked like "device not enabled" | actually the missing HID feature report — but even after sending it, the kernel still parsed nothing, so the two problems were independent |
| `libinput` not installed | no `libinput list-devices` output | the driver had already been ruled out at the evdev layer |

---

## 9. Reusable checklist

For any HID device where Linux "sees it but nothing happens":

1. `lsusb -v` — interfaces, endpoints, `wDescriptorLength`.
2. `/proc/bus/input/devices` — what did the kernel create? Decode `ABS`, `KEY`,
   `PROP` as **bitmaps**.
3. `cat /dev/hidrawN` vs `cat /dev/input/eventN` — which layer is dead?
4. Dump the report descriptor; decode `Input` items into byte offsets.
5. If the layout is standard but the driver misbehaves, a `uinput` userspace
   driver is usually faster than a kernel patch.
6. Get ground truth from an OS where the device works, touching known positions.
7. Add a replay mode so you can iterate without the hardware.
