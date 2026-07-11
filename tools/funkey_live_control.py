#!/usr/bin/env python3
"""Keyboard controller for changing FunkeyShifter reports while a game is running.

This script only uses EP0 vendor control transfers. It does not claim interface 0
or read endpoint 0x81, so the game can keep polling the portal interface.
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


PRESETS = {
    "1": ("Flurry", "Flurry"),
    "2": ("Webley", "Webley"),
    "3": ("Wasabi", "Wasabi"),
    "4": ("Chim-Chim", "Chim-Chim"),
    "5": ("Speed Racer GP", "Speed Racer GP"),
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


def apply_key(dev, key: str, readback: bool, pulse_remove: bool, pulse_delay_s: float) -> None:
    if key in PRESETS:
        label, value = PRESETS[key]
        report = parse_report(value)
        if pulse_remove:
            remove_report(dev)
            time.sleep(pulse_delay_s)
        set_report(dev, report)
        if readback:
            report = read_report(dev)
        print(f"SET {label}: {hex_report(report)}")
        return

    if key == "0":
        remove_report(dev)
        report = read_report(dev) if readback else parse_report("00")
        print(f"REMOVE: {hex_report(report)}")
        return

    if key == "s":
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
    parser.add_argument("--no-readback", action="store_true", help="Do not read EP0 status after setting a report")
    parser.add_argument("--no-pulse", action="store_true", help="Do not send a remove event before numbered presets")
    parser.add_argument("--pulse-delay-ms", type=int, default=250, help="Delay between remove and set for numbered presets")
    parser.add_argument("--reopen-each-command", action="store_true", help="Open and close the USB device for each key")
    parser.add_argument("--once", metavar="KEY", help="Send one key command and exit, for example --once 2")
    args = parser.parse_args()

    readback = not args.no_readback
    pulse_remove = not args.no_pulse
    pulse_delay_s = max(args.pulse_delay_ms, 0) / 1000.0

    if args.once:
        dev = open_device(args.vid, args.pid)
        try:
            apply_key(dev, args.once.strip().lower()[:1], readback, pulse_remove, pulse_delay_s)
        finally:
            dispose_device(dev)
        return 0

    print_menu()
    print(f"Opening {args.vid:04X}:{args.pid:04X}")
    dev = None if args.reopen_each_command else open_device(args.vid, args.pid)

    try:
        while True:
            key = read_key()
            if key == "q":
                print("Quit")
                return 0

            command_dev = dev
            if args.reopen_each_command:
                command_dev = open_device(args.vid, args.pid)

            try:
                apply_key(command_dev, key, readback, pulse_remove, pulse_delay_s)
            except usb.core.USBError as exc:
                print(f"USB error: {exc}")
            finally:
                if args.reopen_each_command and command_dev is not None:
                    dispose_device(command_dev)
    finally:
        if dev is not None:
            dispose_device(dev)


if __name__ == "__main__":
    sys.exit(main())
