# Funkey Shifter Web

Next.js Web Bluetooth controller for the ESP32 FunkeyShifter portal.

The browser path uses BLE so Windows can keep the USB `0E4C:7288` device bound
to `libusbK` for the game. You do not need Zadig or a WinUSB binding for the web
controller.

## Run

```powershell
cd .\web
npm.cmd install
npm.cmd run dev
```

Open the local URL printed by Next.js in Chrome or Edge. Web Bluetooth requires
a secure browser context, so use `http://localhost:3000`,
`http://127.0.0.1:3000`, or HTTPS.

## BLE Path

The firmware advertises as `Funkey Shifter` and exposes this GATT surface:

- service `8a8f9f85-0d1c-4e54-9f54-1f2e2a94d839`
- report characteristic `8a8f9f86-0d1c-4e54-9f54-1f2e2a94d839`, read/notify
- command characteristic `8a8f9f87-0d1c-4e54-9f54-1f2e2a94d839`, write

Command payloads:

- `02` plus 8 report bytes: set decoded report
- `03`: remove current figure

The report value is still the same decoded 8-byte form used by the Python tool,
for example `FFFFFFF00000005C` for Webley and `FFFFFFF000000000` for removed.

## Browser Notes

Click `Connect BLE`, select `Funkey Shifter` from the Bluetooth picker, then use
the catalog or custom input to switch figures. If the device is not listed,
reflash the BLE-enabled firmware, reset or replug the ESP32, and make sure
Bluetooth is enabled on the PC.

Normal phone or Windows Bluetooth pairing screens may hide custom BLE GATT
peripherals. For raw advertising checks, use the Chrome/Edge Bluetooth picker or
a BLE scanner app rather than the OS pairing screen.
