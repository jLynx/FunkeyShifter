# 🎮 [FunkeyShifter](https://funkeyshifter.jlynx.workers.dev/)

[![FunkeyShifter Live](https://img.shields.io/badge/Live-funkeyshifter.jlynx.workers.dev-success?style=for-the-badge&logo=cloudflare)](https://funkeyshifter.jlynx.workers.dev/)
[![ESP32-S3](https://img.shields.io/badge/ESP32--S3-Firmware-red?style=for-the-badge&logo=espressif)](docs/toolchain.md)
[![Web Bluetooth](https://img.shields.io/badge/Web%20Bluetooth-Controller-blue?style=for-the-badge&logo=bluetooth)](docs/usb-protocol.md)

FunkeyShifter is an ESP32-S3 remake portal for U.B. Funkeys. It lets the game
see a MegaByte-style USB hub while a browser controls which Funkey is currently
present.

## What It Does

The ESP32 presents itself to the computer as a portal-style USB device. The game
polls the ESP32 over USB and receives the same kind of raw MegaByte hub packets
that the original hardware produced.

The controller website talks to the ESP32 over Bluetooth Low Energy instead of
opening the USB device. That means the game can keep using the USB portal while
the browser changes the active Funkey in the background.

When a Funkey is selected in the website, the app sends the decoded Funkey ID to
the ESP32 over BLE. The firmware converts that ID into the raw hub packet format
and serves it to the game on the USB endpoint. Removing a Funkey works the same
way: the website sends a remove command, and the ESP32 reports the no-figure
state to the game.

## Using The Website

1. Flash the current firmware to the ESP32-S3.
2. Plug the ESP32 into the computer running the game.
3. Open [funkeyshifter.jlynx.workers.dev](https://funkeyshifter.jlynx.workers.dev/)
   in Chrome or Edge.
4. Click `Connect BLE`.
5. Select `Funkey Shifter` from the browser Bluetooth picker.
6. Choose a Funkey from the website to change what the game sees.

Web Bluetooth requires Chrome or Edge and a secure browser context. The deployed
site uses HTTPS, so it is ready to connect directly.

## Project Areas

- `src/`: ESP32-S3 firmware that emulates the portal USB device and exposes the
  BLE control service.
- `web/`: Next.js Web Bluetooth controller deployed to Cloudflare Workers.
- `docs/`: Protocol, firmware, and toolchain notes.
- `tools/`: Development and reference utilities.

## Documentation

- [Web app notes](web/README.md)
- [Firmware toolchain](docs/toolchain.md)
- [USB and BLE protocol](docs/usb-protocol.md)
- [RDF format notes](docs/rdf-format.md)
