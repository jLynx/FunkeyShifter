#!/usr/bin/env python3
"""Keyboard controller for changing FunkeyShifter reports while a game is running.

The default USB mode only uses EP0 vendor control transfers. It does not claim
interface 0 or read endpoint 0x81, so the game can keep polling the portal
interface. The optional game mode sends the same WM_COPYDATA message style used
by FunkeySelectorGUI.
"""

from __future__ import annotations

import argparse
import sys
import time

import usb.core
import usb.util

from funkey_portal_test import (
    DEFAULT_PID,
    DEFAULT_VID,
    FUNKEY_IDS,
    REPORT_LEN,
    REQ_GET_REPORT,
    REQ_REMOVE,
    REQ_SET_REPORT,
    TYPE_VENDOR_IN,
    TYPE_VENDOR_OUT,
    find_device,
    hex_report,
    parse_report,
)


WM_COPYDATA = 0x004A
GAME_COPYDATA_MAGIC = 238164658
DEFAULT_GAME_WINDOW_TITLES = ("FunkeyOne", "U.B. Funkeys", "OpenFK")

PRESETS = {
    "1": ("Flurry", "00000050"),
    "2": ("Webley", "0000005C"),
    "3": ("Wasabi", "0000002F"),
    "4": ("Chim-Chim", "00000092"),
    "5": ("Speed Racer", "00000093"),
    "6": ("Xener Ultra Rare", "0000003A"),
}


def print_menu() -> None:
    print("Funkey live control")
    print("-------------------")
    for key, (label, _) in PRESETS.items():
        print(f"{key}: {label}")
    print("0: remove figure")
    print("s: status")
    print("h: help")
    print("q: quit")
    print()


def read_key() -> str:
    if sys.platform == "win32":
        import msvcrt

        key = msvcrt.getwch()
        if key in ("\x00", "\xe0"):
            msvcrt.getwch()
            return ""
        print(key)
        return key.lower()

    return input("> ").strip().lower()[:1]


def open_device(vid: int, pid: int):
    return find_device(vid, pid)


def set_report(dev, report: bytes) -> None:
    dev.ctrl_transfer(TYPE_VENDOR_OUT, REQ_SET_REPORT, 0, 0, report, timeout=1000)


def remove_report(dev) -> None:
    dev.ctrl_transfer(TYPE_VENDOR_OUT, REQ_REMOVE, 0, 0, b"", timeout=1000)


def read_report(dev) -> bytes:
    return bytes(dev.ctrl_transfer(TYPE_VENDOR_IN, REQ_GET_REPORT, 0, 0, REPORT_LEN, timeout=1000))


def dispose_device(dev) -> None:
    try:
        usb.util.dispose_resources(dev)
    except usb.core.USBError:
        pass


def report_suffix_hex(report: bytes) -> str:
    return hex_report(report[4:])


def send_game_copydata(report: bytes, titles: list[str] | None, class_name: str | None) -> str:
    if sys.platform != "win32":
        raise RuntimeError("WM_COPYDATA game control is only available on Windows")

    import ctypes
    from ctypes import wintypes

    class COPYDATASTRUCT(ctypes.Structure):
        _fields_ = [
            ("dwData", ctypes.c_size_t),
            ("cbData", wintypes.DWORD),
            ("lpData", ctypes.c_void_p),
        ]

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.FindWindowW.restype = wintypes.HWND
    user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, ctypes.c_void_p]
    user32.SendMessageW.restype = ctypes.c_ssize_t

    search_titles = titles if titles else list(DEFAULT_GAME_WINDOW_TITLES)
    hwnd = 0
    used_title = None
    for title in search_titles:
        hwnd = user32.FindWindowW(class_name, title)
        if hwnd:
            used_title = title
            break

    if not hwnd:
        raise RuntimeError("game window not found")

    # This matches FunkeySelectorGUI's MegaByte mode: FFFFFFFF + 8-digit id.
    message = ("FFFFFFFF" + report_suffix_hex(report)).encode("ascii") + b"\0"
    message_buf = ctypes.create_string_buffer(message)
    copy_data = COPYDATASTRUCT(
        GAME_COPYDATA_MAGIC,
        len(message),
        ctypes.cast(message_buf, ctypes.c_void_p),
    )
    user32.SendMessageW(hwnd, WM_COPYDATA, 0, ctypes.byref(copy_data))
    return used_title or f"0x{hwnd:X}"


def emit_report(dev, label: str, report: bytes, mode: str, readback: bool,
                game_titles: list[str] | None, game_class: str | None) -> None:
    status_parts = []
    if mode in ("usb", "both"):
        set_report(dev, report)
        if readback:
            report = read_report(dev)
        status_parts.append(hex_report(report))

    if mode in ("game", "both"):
        title = send_game_copydata(report, game_titles, game_class)
        status_parts.append(f"game:{title}")

    print(f"SET {label}: {' '.join(status_parts)}")


def emit_remove(dev, mode: str, readback: bool, game_titles: list[str] | None, game_class: str | None) -> None:
    report = parse_report("00000000")
    status_parts = []
    if mode in ("usb", "both"):
        remove_report(dev)
        if readback:
            report = read_report(dev)
        status_parts.append(hex_report(report))

    if mode in ("game", "both"):
        title = send_game_copydata(report, game_titles, game_class)
        status_parts.append(f"game:{title}")

    print(f"REMOVE: {' '.join(status_parts)}")


def apply_value(dev, label: str, value: str, mode: str, readback: bool,
                game_titles: list[str] | None, game_class: str | None) -> None:
    report = parse_report(value)
    emit_report(dev, label, report, mode, readback, game_titles, game_class)


def apply_key(dev, key: str, mode: str, readback: bool, pulse_remove: bool, pulse_delay_s: float,
              game_titles: list[str] | None, game_class: str | None) -> None:
    if key in PRESETS:
        label, value = PRESETS[key]
        report = parse_report(value)
        if pulse_remove and mode in ("usb", "both"):
            remove_report(dev)
            time.sleep(pulse_delay_s)
        emit_report(dev, label, report, mode, readback, game_titles, game_class)
        return

    if key == "0":
        emit_remove(dev, mode, readback, game_titles, game_class)
        return

    if key == "s":
        if mode == "game":
            print("STATUS is only available in usb/both mode")
        else:
            print(f"STATUS: {hex_report(read_report(dev))}")
        return

    if key == "h":
        print_menu()
        return

    if key:
        print("Unknown key. Press h for help.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Live keyboard controller for FunkeyShifter")
    parser.add_argument("--vid", type=lambda value: int(value, 0), default=DEFAULT_VID)
    parser.add_argument("--pid", type=lambda value: int(value, 0), default=DEFAULT_PID)
    parser.add_argument("--mode", choices=("usb", "game", "both"), default="usb",
                        help="usb controls the ESP32 portal; game sends WM_COPYDATA; both does both")
    parser.add_argument("--game-window-title", action="append", dest="game_titles",
                        help="Game window title for --mode game/both; can be repeated")
    parser.add_argument("--game-window-class", default=None,
                        help="Optional game window class for --mode game/both")
    parser.add_argument("--no-readback", action="store_true", help="Do not read EP0 status after setting a report")
    parser.add_argument("--no-pulse", action="store_true", help="Do not send a remove event before numbered presets")
    parser.add_argument("--pulse-delay-ms", type=int, default=250, help="Delay between remove and set for numbered presets")
    parser.add_argument("--reopen-each-command", action="store_true", help="Open and close the USB device for each key")
    parser.add_argument("--once", metavar="KEY", help="Send one key command and exit, for example --once 2")
    parser.add_argument("--set", metavar="FUNKEY_OR_HEX", help="Set a known Funkey name or hex id and exit")
    parser.add_argument("--list", action="store_true", help="List known Funkey ids and exit")
    args = parser.parse_args()

    if args.list:
        for label, common, rare, very_rare in FUNKEY_IDS:
            values = [f"common={common:08X}"]
            if rare is not None:
                values.append(f"rare={rare:08X}")
            if very_rare is not None:
                values.append(f"very_rare={very_rare:08X}")
            print(f"{label}: {', '.join(values)}")
        return 0

    readback = not args.no_readback
    pulse_remove = not args.no_pulse
    pulse_delay_s = max(args.pulse_delay_ms, 0) / 1000.0
    needs_usb = args.mode in ("usb", "both")

    if args.set:
        dev = open_device(args.vid, args.pid) if needs_usb else None
        try:
            apply_value(dev, args.set, args.set, args.mode, readback, args.game_titles, args.game_window_class)
        finally:
            if dev is not None:
                dispose_device(dev)
        return 0

    if args.once:
        dev = open_device(args.vid, args.pid) if needs_usb else None
        try:
            apply_key(dev, args.once.strip().lower()[:1], args.mode, readback, pulse_remove, pulse_delay_s,
                      args.game_titles, args.game_window_class)
        finally:
            if dev is not None:
                dispose_device(dev)
        return 0

    print_menu()
    if needs_usb:
        print(f"Opening {args.vid:04X}:{args.pid:04X}")
    if args.mode in ("game", "both"):
        print("Game message window titles: " + ", ".join(args.game_titles or DEFAULT_GAME_WINDOW_TITLES))
    dev = None if args.reopen_each_command or not needs_usb else open_device(args.vid, args.pid)

    try:
        while True:
            key = read_key()
            if key == "q":
                print("Quit")
                return 0

            command_dev = dev
            if args.reopen_each_command and needs_usb:
                command_dev = open_device(args.vid, args.pid)

            try:
                apply_key(command_dev, key, args.mode, readback, pulse_remove, pulse_delay_s,
                          args.game_titles, args.game_window_class)
            except (RuntimeError, usb.core.USBError) as exc:
                print(f"Error: {exc}")
            finally:
                if args.reopen_each_command and command_dev is not None:
                    dispose_device(command_dev)
    finally:
        if dev is not None:
            dispose_device(dev)


if __name__ == "__main__":
    sys.exit(main())
