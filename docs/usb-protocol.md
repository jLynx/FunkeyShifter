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
| Speed Racer GP | `FFFFFFF000000093` |

The only raw mapping currently captured is Flurry. Its raw endpoint `0x81`
sequence was extracted from `usb_dump2.pcapng` and is replayed by the firmware
when the decoded control report is `FFFFFFF000000050`.

## Control Requests

The portal accepts optional EP0 requests for host tools and remake code.

| bmRequestType | bRequest | Direction | Data | Meaning |
| --- | --- | --- | --- | --- |
| `0xA0` | `0x00` | IN | 4 bytes | Init/readiness response, currently `00 00 00 00` |
| vendor IN | `0x01` | IN | 8 bytes | Read current report |
| vendor OUT | `0x02` | OUT | 8 bytes | Set current report |
| vendor OUT | `0x03` | OUT | 0 bytes | Remove current figure |

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

The repository includes `tools/funkey_portal_test.py` as a reference host-side
implementation for the init, get, set, remove, and interrupt-read paths.
