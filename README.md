# FunkeyShifter

ESP32-S3 revival portal firmware for a U.B. Funkeys-compatible remake.

This project intentionally should not use Radica's USB VID/PID for a published
remake. The source currently uses `0E4C:7288` because that is the temporary test
VID/PID being used against the old tooling. Before sharing hardware or
distributing firmware, switch `FUNKEY_USB_VID` and `FUNKEY_USB_PID` in
`src/main.c` to the remake/private IDs.

## Current USB Shape

- Framework: ESP-IDF through PlatformIO.
- Board profile: `esp32_s3_super_mini` in PlatformIO.
- PlatformIO base board: `esp32-s3-devkitm-1`, with 4 MB flash override.
- Product string: `Funkey Shifter Portal`.
- VID/PID: `0E4C:7288` for temporary local testing.
- Interface 0: vendor-specific portal report interface.
- Endpoint `0x81`: interrupt IN, max packet 8, 10 ms interval.
- Game-facing packets are 7-byte raw MegaByte hub packets.
- Control is via EP0 vendor requests from the Python helper.

The EP0 control helper accepts the decoded 8-byte game-side ID form seen in
your logs, for example:

```text
FF FF FF F0 00 00 00 5C
```

That prints as `FFFFFFF00000005C`. The firmware converts the numeric decoded
ID into the raw MegaByte interrupt packet format on endpoint `0x81`.

The removed/no-figure report is:

```text
FF FF FF F0 00 00 00 00
```

## Decoded IDs

Host tools can set the current decoded report by name or by hex ID:

```text
SET FFFFFFF00000005C
SET 5C
SET 0x5C
Webley
Flurry
Wasabi
Chim-Chim
Speed Racer
```

IDs are expanded to `FFFFFFF0` plus an 8-digit suffix. For example, `5C`
becomes `FFFFFFF00000005C`, and `00000127` becomes `FFFFFFF000000127`.

The original hub packet encoding stores the numeric ID as decimal digits plus a
checksum, while the monitor/game displays that number as hex. For example,
Webley is displayed as `0000005C`, but the raw packet encodes decimal `92`.
The current encoder supports numeric IDs from `0x00000000` through decimal
`999`, which covers the known toy IDs through `00000127`.

## Build

Toolchain setup and generated-file notes are in
[`docs/toolchain.md`](docs/toolchain.md).

Use the bundled PlatformIO executable if `pio` is not on `PATH`:

```powershell
C:\Users\jLynx\.platformio\penv\Scripts\pio.exe run
```

Upload:

```powershell
C:\Users\jLynx\.platformio\penv\Scripts\pio.exe run -t upload
```

Monitor:

```powershell
C:\Users\jLynx\.platformio\penv\Scripts\pio.exe device monitor
```

## USB Host Testing

For live in-game switching, use the EP0 control helper. It does not claim
interface 0 or read endpoint `0x81`, so the game can keep the portal interface
open:

```powershell
py -m pip install pyusb libusb-package
py .\tools\funkey_live_control.py
```

Keys:

```text
1 Flurry
2 Webley
3 Wasabi
4 Chim-Chim
5 Speed Racer
0 remove figure
s status
q quit
```

Numbered keys send a short remove pulse before the selected figure by default.
Use `--no-pulse` to disable that, or `--pulse-delay-ms 500` to make the
remove/place gap longer.

The numbered presets are shortcuts only. The Python tool sends the decoded ID
over EP0, and the firmware derives the game-facing raw packet from that ID. Run
this to list the larger decoded ID table supported by the host tools:

```powershell
py .\tools\funkey_live_control.py --list
```

Any listed ID in the supported numeric range can be emitted through the original
USB path. You can also set one directly:

```powershell
py .\tools\funkey_live_control.py --set Webley
py .\tools\funkey_live_control.py --set 0x127
```

For game/remake-side testing of the vendor USB interface, use the PyUSB helper:

```powershell
py -m pip install pyusb libusb-package
py .\tools\funkey_portal_test.py --set Webley --read-control --poll 3
py .\tools\funkey_portal_test.py --remove --read-control
```

Do not use `--poll` while the game is running. Polling claims/reads the same
game-facing endpoint that the game should own.

## Dependencies

`src/idf_component.yml` declares `espressif/esp_tinyusb`. ESP-IDF downloads that
component into `managed_components/`, which is generated build state and is
ignored by git. `dependencies.lock` records exact resolved versions and should
normally be committed so builds are repeatable.

`platformio.ini` and `sdkconfig.defaults` are configured for the ESP32-S3 Super
Mini's 4 MB flash. PlatformIO's `esp32-s3-devkitm-1` board profile defaults to
8 MB, so this project overrides the upload flash size and maximum image size.
