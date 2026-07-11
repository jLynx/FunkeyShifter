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

For the remake, the simplest path is:

1. Open `0E4C:7288`, or the remake/private VID/PID after it changes.
2. Claim interface 0.
3. Optionally perform `0xA0/0x00` and expect `00 00 00 00`.
4. Poll interrupt endpoint `0x81` for 7-byte raw hub packets.

## Live Control While The Game Runs

The game should own interface 0 and poll endpoint `0x81`. A separate controller
should not claim interface 0 at the same time.

The supported live-control path is EP0 vendor control:

- `0x40/0x02` with 8 data bytes sets the current report.
- `0x40/0x03` with no data removes the current figure.
- `0xC0/0x01` with length 8 reads the current report.

`tools/funkey_live_control.py` uses only those EP0 requests, so it can be used
for quick in-game switching without reading the game-facing interrupt endpoint.
The default number keys select five common figures. Run
`py -3 .\tools\funkey_live_control.py --list` to print the full known decoded
ID table.

The repository includes `tools/funkey_portal_test.py` as a reference host-side
implementation for the init, get, set, remove, and interrupt-read paths.
