# USB Protocol

This is the FunkeyShifter remake protocol. It is deliberately not identified as
the original MegaByte hardware.

## Device

| Field | Value |
| --- | --- |
| VID | `0x0E4C` temporary local testing VID |
| PID | `0x7288` temporary local testing PID |
| Manufacturer | `Funkey Revival` |
| Product | `Funkey Shifter Portal` |
| Configuration | 1 |

For the published remake, replace the temporary test VID/PID in `src/main.c`
with the remake/private IDs and update this table.

## Portal Interface

| Field | Value |
| --- | --- |
| Interface | 0 |
| Class | vendor-specific, `0xFF` |
| Endpoint | `0x81` interrupt IN |
| Max packet size | 8 bytes |
| Observed transfer length | 7 bytes |
| Poll interval | 10 ms |

The real hub sends 7-byte raw packets even though the endpoint max packet size
is 8. The decoded `FFFFFFF000000050` style IDs are host/tool-level values, not
the raw interrupt payload.

## Report Format

Decoded EP0 control reports are 8 bytes:

```text
byte 0..3: prefix, normally FF FF FF F0
byte 4..7: figure suffix/id
```

Examples:

| Name | Hex report |
| --- | --- |
| Removed | `FFFFFFF000000000` |
| Wasabi | `FFFFFFF00000002F` |
| Xener Ultra Rare | `FFFFFFF00000003A` |
| Flurry | `FFFFFFF000000050` |
| Webley | `FFFFFFF00000005C` |
| Chim-Chim | `FFFFFFF000000092` |
| Speed Racer | `FFFFFFF000000093` |

The endpoint packet is generated from the numeric decoded ID. The monitor/game
formats that number as an 8-digit uppercase hex suffix, but the raw packet
stores the number as decimal digits:

```text
digit 0: ones
digit 1: tens
digit 2: hundreds
digit 3: checksum = (ones + tens + hundreds) % 10
```

Examples:

| Name | Displayed suffix | Numeric value | Encoded decimal digits |
| --- | --- | --- | --- |
| Wasabi | `0000002F` | 47 | `7, 4, 0, 1` |
| Xener Ultra Rare | `0000003A` | 58 | `8, 5, 0, 3` |
| Flurry | `00000050` | 80 | `0, 8, 0, 8` |
| Webley | `0000005C` | 92 | `2, 9, 0, 1` |
| Chim-Chim | `00000092` | 146 | `6, 4, 1, 1` |
| Speed Racer | `00000093` | 147 | `7, 4, 1, 2` |

Each digit is represented by an ADC-like value in one of ten value buckets.
The firmware uses a fixed baseline of `177` and bucket-center ADC values:

| Digit | ADC |
| --- | --- |
| 0 | 278 |
| 1 | 375 |
| 2 | 465 |
| 3 | 554 |
| 4 | 643 |
| 5 | 713 |
| 6 | 783 |
| 7 | 844 |
| 8 | 899 |
| 9 | 942 |

The 7-byte raw packet packs those four 10-bit ADC values plus the baseline:

```text
byte 0: adc0 low 8 bits
byte 1: adc1 low 8 bits
byte 2: adc2 low 8 bits
byte 3: adc3 low 8 bits
byte 4: adc0..adc3 high 2 bits, packed at bit positions 0, 2, 4, 6
byte 5: baseline low 8 bits
byte 6: baseline high 8 bits
```

The old captured stable packets were:

| Name | Decoded report | Raw endpoint packet |
| --- | --- | --- |
| Flurry | `FFFFFFF000000050` | `F5 85 F7 82 CC B1 00` |
| Webley | `FFFFFFF00000005C` | `D5 B1 0D 74 5D C9 00` |
| Wasabi | `FFFFFFF00000002F` | `52 84 07 6D 5B C2 00` |
| Chim-Chim | `FFFFFFF000000092` | `12 84 6B 6C 5B BF 00` |
| Speed Racer | `FFFFFFF000000093` | `4D 7F 60 C4 5B B1 00` |

The Flurry packet was first extracted from `usb_dump2.pcapng`. The other four
packets were extracted from `usb_dump3.pcapng` and used to confirm the decoder.
Generated packets do not need to byte-match the captures; they only need to land
inside the same digit buckets and pass the checksum. Removal uses the single
no-figure raw packet `FF FF FF FF FF 00 00`.

The raw protocol carries three decimal ID digits, so the current generated path
supports numeric IDs `0..999`. The known list currently tops out at `00000127`
hex, which is decimal `295`, so this covers the known figures.

## Control Requests

The portal accepts optional EP0 requests for host tools and remake code.

| bmRequestType | bRequest | Direction | Data | Meaning |
| --- | --- | --- | --- | --- |
| `0xA0` | `0x00` | IN | 4 bytes | Init/readiness response, currently `00 00 00 00` |
| vendor IN | `0x01` | IN | 8 bytes | Read current report |
| vendor OUT | `0x02` | OUT | 8 bytes | Set current report |
| vendor OUT | `0x03` | OUT | 0 bytes | Remove current figure |
| vendor OUT | `0x04` | OUT | 7 bytes | Debug: force a stable raw endpoint packet |
| vendor IN | `0x05` | IN | 8 bytes | Read BLE diagnostic state |
| vendor IN | `0x06` | IN | 8 bytes | Read portal capability flags |

Capability response `0x06` is an 8-byte block:

| Byte(s) | Meaning |
| --- | --- |
| `0..3` | ASCII magic `FSH1` |
| `4` | Capability protocol version, currently `0x01` |
| `5` | Feature flags |
| `6..7` | Reserved, currently zero |

Feature flags:

| Bit | Meaning |
| --- | --- |
| `0` | Managed catalog/profile expected by the game |
| `1` | BLE control service available |
| `2` | Raw packet override request available |
| `3` | Physical resistor-pad reader enabled |

For the remake, the simplest path is:

1. Open `0E4C:7288`, or the remake/private VID/PID after it changes.
2. Claim interface 0.
3. Optionally perform `0xA0/0x00` and expect `00 00 00 00`.
4. Optionally perform vendor IN `0x06` and, when the response starts with
   `FSH1` and feature bit `0` is set, enable the managed in-game catalog.
5. Poll interrupt endpoint `0x81` for 7-byte raw hub packets.

## Debug Host Tools

Normal control is handled by the Next.js Web Bluetooth app in `web/`. The game
should own interface 0 and poll endpoint `0x81`; a separate controller should
not claim interface 0 at the same time.

The firmware still exposes EP0 vendor control for development and reference
tools:

- `0x40/0x02` with 8 data bytes sets the current report.
- `0x40/0x03` with no data removes the current figure.
- `0xC0/0x01` with length 8 reads the current report.
- `0xC0/0x06` with length 8 reads the portal capability flags.

`tools/funkey_live_control.py` is a debug helper that uses only those EP0
requests. `tools/funkey_portal_test.py` is a reference host-side implementation
for the init, capability, get, set, remove, and interrupt-read paths.

## Physical Funkey Reader

The firmware can also read a real Funkey resistor board. Pad 1/common connects
to GND and the other four pads connect to the GPIO8..GPIO11 header block. Each
GPIO line needs its own external `220k` pull-up to 3.3 V:

| Funkey pad | Meaning | Default ESP32-S3 ADC |
| --- | --- | --- |
| 1 | Common | GND |
| 2 | R4 checksum | ADC1 channel 7 / GPIO8 |
| 3 | R1 ones | ADC1 channel 8 / GPIO9 |
| 4 | R2 tens | ADC1 channel 9 / GPIO10 |
| 5 | R3 hundreds | ADC2 channel 0 / GPIO11 |

GPIO12 and GPIO13 are not used by the default reader mapping; they remain free
in the same header block. ESP32 internal pull-ups are useful for rough contact
testing only; reliable ID decoding needs the four external `220k` pull-ups.
The ADC bucket math keeps the physical pull-up value at `220k` and applies a
separate ADC target scale, currently `900/1000`, based on measured raw values
from known figures with the external pull-up harness fitted.

The physical board encodes `R3 R2 R1 R4`, where `R3` is hundreds, `R2` is
tens, `R1` is ones, and `R4` is `(R1 + R2 + R3) % 10`. The firmware converts a
stable physical placement into the same decoded report used by BLE/software
control.

Last action wins: BLE/software set/remove commands update the current report
immediately, and the physical reader only updates the report when the physical
state changes. Leaving a physical Funkey sitting on the reader does not
continuously override a later BLE selection; removing or replacing it does.

## BLE Browser Control

The ESP32 also exposes a Web Bluetooth control path so the browser does not need
to open the USB device or require a WinUSB driver binding on Windows.

| Field | Value |
| --- | --- |
| Device name | `Funkey Shifter` |
| Service UUID | `8a8f9f85-0d1c-4e54-9f54-1f2e2a94d839` |
| Report characteristic | `8a8f9f86-0d1c-4e54-9f54-1f2e2a94d839`, read/notify |
| Command characteristic | `8a8f9f87-0d1c-4e54-9f54-1f2e2a94d839`, write/write without response |
| Physical report characteristic | `8a8f9f88-0d1c-4e54-9f54-1f2e2a94d839`, read/notify |

The report and physical report characteristic values are the same decoded
8-byte report documented above. The report characteristic is the current active
figure. The physical report characteristic is only the reader's physical
placement state. Command payloads are:

| Payload | Meaning |
| --- | --- |
| `02` plus 8 report bytes | Set current decoded report |
| `03` | Remove current figure |
| `04` | Set current figure to the latest physical report |

The browser app in `web/` uses this BLE path. The USB interface can remain bound
to `libusbK` for the game.
