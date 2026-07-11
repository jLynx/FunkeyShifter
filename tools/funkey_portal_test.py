#!/usr/bin/env python3
"""Host-side USB tester for the FunkeyShifter portal firmware."""

from __future__ import annotations

import argparse
import sys
from typing import Iterable

try:
    import usb.core
    import usb.util
except ImportError as exc:  # pragma: no cover - depends on host setup
    raise SystemExit("PyUSB is required: py -m pip install pyusb libusb-package") from exc


# Temporary hardware-test VID/PID currently used in src/main.c.
# For the final remake/private-test VID/PID, pass --vid 0x1209 --pid 0x0001
# or change these defaults when the firmware IDs change.
DEFAULT_VID = 0x0E4C
DEFAULT_PID = 0x7288
PORTAL_INTERFACE = 0
PORTAL_EP_IN = 0x81
REPORT_LEN = 8

REQ_INIT = 0x00
REQ_GET_REPORT = 0x01
REQ_SET_REPORT = 0x02
REQ_REMOVE = 0x03

TYPE_INIT_IN = 0xA0
TYPE_VENDOR_IN = 0xC0
TYPE_VENDOR_OUT = 0x40

try:
    import libusb_package
except ImportError:  # pragma: no cover - host-specific optional helper
    libusb_package = None

NAMES = {
    "flurry": 0x50,
    "webley": 0x5C,
    "wasabi": 0x2F,
    "chimchim": 0x92,
    "speed": 0x93,
    "speedracer": 0x93,
    "speedracergp": 0x93,
}


def normalize_name(text: str) -> str:
    return "".join(ch for ch in text.lower() if ch.isalnum())


def report_from_id(funkey_id: int) -> bytes:
    if not 0 <= funkey_id <= 0xFF:
        raise ValueError("Funkey short id must fit in one byte")
    return bytes((0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, funkey_id))


def parse_report(text: str) -> bytes:
    name = normalize_name(text)
    if name in NAMES:
        return report_from_id(NAMES[name])

    digits = "".join(ch for ch in text if ch.lower() in "0123456789abcdef")
    if len(digits) in (3, 9, 17) and digits[0] == "0":
        digits = digits[1:]

    if len(digits) in (1, 2):
        return report_from_id(int(digits, 16))

    if len(digits) == 8:
        return bytes((0xFF, 0xFF, 0xFF, 0xF0)) + bytes.fromhex(digits)

    if len(digits) == 16:
        return bytes.fromhex(digits)

    raise ValueError("expected a known name, 1-byte id, 8-hex suffix, or 16-hex report")


def hex_report(data: Iterable[int]) -> str:
    return "".join(f"{byte:02X}" for byte in data)


def find_device(vid: int, pid: int):
    backend = libusb_package.get_libusb1_backend() if libusb_package else None
    try:
        dev = usb.core.find(idVendor=vid, idProduct=pid, backend=backend)
    except usb.core.NoBackendError as exc:
        raise SystemExit("No PyUSB backend available: py -m pip install pyusb libusb-package") from exc
    if dev is None:
        raise SystemExit(f"Device {vid:04X}:{pid:04X} not found")
    return dev


def claim_portal_interface(dev) -> None:
    try:
        if dev.is_kernel_driver_active(PORTAL_INTERFACE):
            dev.detach_kernel_driver(PORTAL_INTERFACE)
    except (NotImplementedError, usb.core.USBError):
        pass

    try:
        dev.set_configuration()
    except usb.core.USBError:
        # The OS may already have configured the composite device.
        pass

    usb.util.claim_interface(dev, PORTAL_INTERFACE)


def main() -> int:
    parser = argparse.ArgumentParser(description="Test a FunkeyShifter USB portal")
    parser.add_argument("--vid", type=lambda value: int(value, 0), default=DEFAULT_VID)
    parser.add_argument("--pid", type=lambda value: int(value, 0), default=DEFAULT_PID)
    parser.add_argument("--set", metavar="FUNKEY_OR_HEX", help="Set the current report")
    parser.add_argument("--remove", action="store_true", help="Set the no-figure report")
    parser.add_argument("--poll", type=int, default=0, metavar="N", help="Read N interrupt reports")
    parser.add_argument("--read-control", action="store_true", help="Read the current report with EP0")
    parser.add_argument("--no-init", action="store_true", help="Skip the 0xA0/0x00 init request")
    args = parser.parse_args()

    dev = find_device(args.vid, args.pid)
    print(f"Opened {args.vid:04X}:{args.pid:04X}")

    if not args.no_init:
        init = dev.ctrl_transfer(TYPE_INIT_IN, REQ_INIT, 0, 0, 4, timeout=1000)
        print(f"INIT {hex_report(init)}")

    if args.set:
        report = parse_report(args.set)
        dev.ctrl_transfer(TYPE_VENDOR_OUT, REQ_SET_REPORT, 0, 0, report, timeout=1000)
        print(f"SET {hex_report(report)}")

    if args.remove:
        dev.ctrl_transfer(TYPE_VENDOR_OUT, REQ_REMOVE, 0, 0, b"", timeout=1000)
        print("REMOVE FFFFFFF000000000")

    if args.read_control:
        report = dev.ctrl_transfer(TYPE_VENDOR_IN, REQ_GET_REPORT, 0, 0, REPORT_LEN, timeout=1000)
        print(f"CONTROL {hex_report(report)}")

    if args.poll > 0:
        claim_portal_interface(dev)
        try:
            for _ in range(args.poll):
                report = dev.read(PORTAL_EP_IN, REPORT_LEN, timeout=1500)
                print(f"INTERRUPT {hex_report(report)}")
        finally:
            usb.util.release_interface(dev, PORTAL_INTERFACE)

    return 0


if __name__ == "__main__":
    sys.exit(main())
