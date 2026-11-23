# Protocol reference — CVTE `1ff7:0f26` touch controller

Everything below was read from the device itself (HID report descriptor +
observed reports), not from vendor documentation.

---

## USB interface layout

```
ID 1ff7:0f26  CVTE Touch Device
  iManufacturer : CVTE
  iProduct      : Touch Device
  iSerial       : (13-char panel serial)
  bcdUSB        : 1.10  (full speed, 12 Mbps)
  bcdDevice     : 1.00

  Configuration 1, 2 interfaces

  Interface 0
    bInterfaceClass    = 3 (HID)
    bInterfaceSubClass = 0
    bInterfaceProtocol = 0
    bNumEndpoints      = 2
      EP 0x81  Interrupt IN    wMaxPacketSize 64   bInterval 1
      EP 0x02  Interrupt OUT   wMaxPacketSize 64   bInterval 1
    HID descriptor:
      bcdHID             = 1.11
      bNumDescriptors    = 1
      bDescriptorType    = 0x22 (Report)
      wDescriptorLength  = 1177

  Interface 1
    bInterfaceClass    = 3 (HID)
    bNumEndpoints      = 0        <-- no endpoints at all
```

Interface 1 has zero endpoints. `usbhid` still tries to bind it and logs
`couldn't find an input interrupt endpoint`. This is cosmetic and unrelated to
the missing touch events.

---

## Report descriptor — touch collection (500 bytes)

```
05 0D              Usage Page (Digitizer)
09 04              Usage (Touch Screen)
A1 01              Collection (Application)
85 02                Report ID (2)

09 22                Usage (Finger)
A1 02                Collection (Logical)          <- finger 1
  09 42                Usage (Tip Switch)
  15 00                Logical Minimum (0)
  25 01                Logical Maximum (1)
  35 00                Physical Minimum (0)
  45 01                Physical Maximum (1)
  65 00                Unit (None)
  55 00                Unit Exponent (0)
  75 01                Report Size (1)
  95 01                Report Count (1)
  81 02                Input (Data,Var,Abs)        -> Tip Switch, 1 bit
  09 32                Usage (In Range)
  81 02                Input                       -> In Range, 1 bit
  09 47                Usage (Confidence)
  81 02                Input                       -> Confidence, 1 bit
  95 05                Report Count (5)
  81 03                Input (Const,Var,Abs)       -> 5 bits padding

  09 51                Usage (Contact Identifier)
  46 FF 7F             Physical Maximum (32767)
  75 08                Report Size (8)
  95 01                Report Count (1)
  81 02                Input                       -> contact id, 1 byte

  05 01                Usage Page (Generic Desktop)
  09 30                Usage (X)
  26 FF 7F             Logical Maximum (32767)
  46 EF 21             Physical Maximum (8687)     -> 86.87 cm
  65 11                Unit (SI Linear : cm)
  55 0E                Unit Exponent (-2)
  75 10                Report Size (16)
  81 02                Input                       -> X, 2 bytes

  09 31                Usage (Y)
  46 77 13             Physical Maximum (4983)     -> 49.83 cm
  81 02                Input                       -> Y, 2 bytes

  05 0D                Usage Page (Digitizer)
  09 48                Usage (Width)
  81 02                Input                       -> width, 2 bytes
  09 49                Usage (Height)
  81 02                Input                       -> height, 2 bytes
C1 00                End Collection                <- end finger 1

09 00  A1 02  ...  C1 00     <- finger 2 (identical, Usage 0x00 = undefined)
09 00  A1 02  ...  C1 00     <- finger 3
09 00  A1 02  ...  C1 00     <- finger 4
09 00  A1 02  ...  C1 00     <- finger 5
09 00  A1 02  ...  C1 00     <- finger 6

09 54                Usage (Contact Count)
25 FF                Logical Maximum (255)
75 08                Report Size (8)
81 02                Input                       -> contact count, 1 byte

85 03                Report ID (3)
09 55                Usage (Contact Max)
25 80                Logical Maximum (128)
B1 02                Feature (Data,Var,Abs)      -> feature report, 1 byte

C1 00              End Collection  (Application)
```

Byte accounting for the input report:

```
1 (report id) + 6 * 10 (fingers) + 1 (contact count) = 62 bytes
```

which matches `InputReportByteLength = 62` reported by the OS.

---

## Input report layout

```
byte  0        : 0x02  (report ID)

finger n (n = 0..5), base = 1 + n*10:
  base + 0     : flags
                   bit 0  Tip Switch
                   bit 1  In Range
                   bit 2  Confidence
                   bit 3..7 unused
  base + 1     : contact identifier (0..255)
  base + 2..3  : X, uint16 little-endian, 0..32767
  base + 4..5  : Y, uint16 little-endian, 0..32767
  base + 6..7  : width,  uint16 little-endian
  base + 8..9  : height, uint16 little-endian

byte 61        : contact count (0..255)
```

### Observed flag values

| value | meaning |
|---|---|
| `0x04` | hovering / approaching, Tip Switch clear |
| `0x07` | finger down (Tip + In Range + Confidence) |

### Example frame (finger down, top-left area)

```
02 07 00 35 01 F4 02 88 00 EE 00 ...
│  │  │  └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘
│  │  │    │     │     │     └─ height = 0x00EE = 238
│  │  │    │     │     └─ width  = 0x0088 = 136
│  │  │    │     └─ Y = 0x02F4 = 756
│  │  │    └─ X = 0x0135 = 309
│  │  └─ contact id = 0
│  └─ flags = 0x07
└─ report ID
```

---

## Feature report

```
Report ID 3, 1 byte, Usage 0x55 (Contact Max), Logical Maximum 128.
```

Writing this report after opening the device changes the controller's reporting
behaviour drastically:

| | before feature report | after |
|---|---|---|
| report rate while touching | ~0.36 /s | ~53 /s |

`nbm-touchd` sends value `0x0A` (10) by default and re-sends it if no frames
arrive for 10 s. The descriptor allows values up to 128; if the default does not
produce a good frame rate, try `--feature-value 5 / 1 / 127`.

---

## Coordinate orientation (measured)

Touching known screen positions on a Seewo FV86EB produced:

| Touched position | X range | Y range |
|---|---|---|
| top-left | 309 – 1207 | 756 – 2750 |
| top-right | 31446 – 32767 | 1225 – 7215 |
| bottom-left | 0 – 705 | 29719 – 32767 |
| bottom-right | 31412 – 32188 | 29580 – 31869 |
| centre | 15568 – 16533 | 15109 – 18004 |

So the controller's coordinate system matches the display orientation:

```
(0,0)                 (32767,0)
   +-----------------------+
   |                       |
   |                       |
   +-----------------------+
(0,32767)          (32767,32767)
```

**No swap, no inversion, no offset.** Values are mapped straight through to
`ABS_MT_POSITION_X/Y` with the same 0..32767 range.

Small deviations from the ideal values (e.g. top-left X up to 1207) are the
fingers' physical size plus the panel bezel — the touch area does not extend to
the absolute display edge.

---

## Other HID collections on the same device

The device exposes 11 top-level HID collections besides the touch screen
(keyboard, consumer control, system control, digitizer stylus, vendor-defined
blocks, …). Only the collection described above carries touch data. On Linux all
of them share the same `hidraw` node, so a reader must filter on
`report ID == 2` and on the report length (62 bytes).

```c
if (n != 62 || buf[0] != 0x02)
    continue;
```
