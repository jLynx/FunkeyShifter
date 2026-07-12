#!/usr/bin/env python
"""Host-side USB tester for the FunkeyShifter portal firmware."""

from __future__ import annotations

import argparse
import sys
from typing import Iterable

try:
    import usb.core
    import usb.util
except ImportError as exc:  # pragma: no cover - depends on host setup
    raise SystemExit(f"PyUSB is required: py -m pip install pyusb libusb-package ({exc})") from exc


# Temporary hardware-test VID/PID currently used in src/main.c.
# For the final remake/private-test VID/PID, pass --vid 0x1209 --pid 0x0001
# or change these defaults when the firmware IDs change.
DEFAULT_VID = 0x0E4C
DEFAULT_PID = 0x7288
PORTAL_INTERFACE = 0
PORTAL_EP_IN = 0x81
REPORT_LEN = 8
RAW_PACKET_LEN = 7
BLE_STATUS_LEN = 8
CAPABILITIES_LEN = 8

REQ_INIT = 0x00
REQ_GET_REPORT = 0x01
REQ_SET_REPORT = 0x02
REQ_REMOVE = 0x03
REQ_SET_RAW_PACKET = 0x04
REQ_GET_BLE_STATUS = 0x05
REQ_GET_CAPABILITIES = 0x06

TYPE_INIT_IN = 0xA0
TYPE_VENDOR_IN = 0xC0
TYPE_VENDOR_OUT = 0x40

try:
    import libusb_package
except ImportError:  # pragma: no cover - host-specific optional helper
    libusb_package = None

FUNKEY_IDS = (
    ("Scratch", 0x47, 0x48, 0x49),
    ("Lotus", 0x4A, 0x4B, 0x4F),
    ("Drift", 0xAA, 0xB7, 0xC4),
    ("Waggs", 0xAB, 0xB8, 0xC5),
    ("Dot", 0xFC, 0xFD, 0x101),
    ("Holler", 0xCD, None, None),
    ("Gabby", 0xCB, None, None),
    ("Henchman", 0xCF, None, None),
    ("Master Lox", 0xF7, None, None),
    ("Mayor Sayso", 0xF8, None, None),
    ("Stitch", 0x26, 0x27, 0x28),
    ("Deuce", 0x29, 0x2A, 0x2E),
    ("Wasabi", 0x2F, 0x30, 0x31),
    ("Bones", 0x32, 0x33, 0x34),
    ("Xener", 0x35, 0x39, 0x3A),
    ("Fallout", 0x3B, 0x3C, 0x3D),
    ("Boggle", 0x3E, 0x3F, 0x40),
    ("Vroom", 0x44, 0x45, 0x46),
    ("Rom", 0xCE, None, None),
    ("Glub", 0x14, 0x18, 0x19),
    ("Sprout", 0x1A, 0x1B, 0x1C),
    ("Tiki", 0x1D, 0x1E, 0x1F),
    ("Twinx", 0x23, 0x24, 0x25),
    ("Flurry", 0x50, 0x51, 0x52),
    ("Nibble", 0x53, 0x54, 0x55),
    ("Sol", 0x56, 0x5A, 0x5B),
    ("Webley", 0x5C, 0x5D, 0x5E),
    ("Jerry", 0xF1, None, None),
    ("Pineapple King", 0xF2, None, None),
    ("Native", 0xF6, None, None),
    ("Rewind", 0xCC, None, None),
    ("Racer X", 0x5F, 0x60, 0x61),
    ("Trixie", 0x82, 0x83, 0x87),
    ("Cannonball Taylor", 0x88, 0x89, 0x8A),
    ("Snake Oiler", 0x8B, 0x8C, 0x8D),
    ("Speed Racer Pinball", 0x8E, 0x95, 0x99),
    ("Speed Racer", 0x93, 0x97, 0x9E),
    ("Chim-Chim", 0x92, 0x96, 0x9D),
    ("Taejo", 0x94, 0x98, 0x9F),
    ("Ptep", 0xA0, 0xAD, 0xBA),
    ("Sprocket", 0xA4, 0xB4, 0xC1),
    ("Vlurp", 0xA8, 0xB5, 0xC2),
    ("Snipe", 0xAC, 0xB9, 0xCA),
    ("Dyer", 0xA1, 0xAE, 0xBE),
    ("Lucky", 0xA2, 0xAF, 0xBF),
    ("Tank", 0xA3, 0xB3, 0xC0),
    ("Berger", 0xA9, 0xB6, 0xC3),
    ("Singe", 0x102, 0x106, 0x10D),
    ("Raj", 0x103, 0x107, 0x10E),
    ("Yang", 0x104, 0x108, 0x10F),
    ("Bomble", 0x105, 0x10C, 0x110),
    ("Maul", 0x11A, 0x11E, 0x125),
    ("Nectar", 0x119, 0x11D, 0x124),
    ("Rastro", 0x117, 0x11B, 0x122),
    ("Tadd", 0x118, 0x11C, 0x123),
    ("Mulch", 0x126, None, None),
    ("Ace", 0x127, None, None),
)


def build_name_table() -> dict[str, int]:
    names: dict[str, int] = {}
    for label, common, rare, very_rare in FUNKEY_IDS:
        base = normalize_name(label)
        names[base] = common
        names[f"{base}common"] = common
        if rare is not None:
            names[f"{base}r"] = rare
            names[f"{base}rare"] = rare
        if very_rare is not None:
            names[f"{base}vr"] = very_rare
            names[f"{base}veryrare"] = very_rare
            names[f"{base}ur"] = very_rare
            names[f"{base}ultrarare"] = very_rare

    names["speed"] = 0x93
    names["speedracergp"] = 0x93
    names["speedracerpinballgp"] = 0x8E
    names["removed"] = 0x00000000
    names["none"] = 0x00000000
    return names


def normalize_name(text: str) -> str:
    return "".join(ch for ch in text.lower() if ch.isalnum())


NAMES = build_name_table()


def report_from_id(funkey_id: int) -> bytes:
    if not 0 <= funkey_id <= 0xFFFFFFFF:
        raise ValueError("Funkey id must fit in four bytes")
    return bytes((0xFF, 0xFF, 0xFF, 0xF0)) + funkey_id.to_bytes(4, "big")


def parse_report(text: str) -> bytes:
    name = normalize_name(text)
    if name in NAMES:
        return report_from_id(NAMES[name])

    digits = "".join(ch for ch in text if ch.lower() in "0123456789abcdef")
    if 1 <= len(digits) <= 8:
        return report_from_id(int(digits, 16))

    if len(digits) == 16:
        return bytes.fromhex(digits)

    raise ValueError("expected a known name, 1- to 8-hex id suffix, or 16-hex report")


def parse_raw_packet(text: str) -> bytes:
    digits = "".join(ch for ch in text if ch.lower() in "0123456789abcdef")
    if len(digits) != RAW_PACKET_LEN * 2:
        raise ValueError("expected 14 hex digits for a 7-byte raw portal packet")
    return bytes.fromhex(digits)


def hex_report(data: Iterable[int]) -> str:
    return "".join(f"{byte:02X}" for byte in data)


def ble_error_name(code: int) -> str:
    host_errors = {
        0: "ok",
        1: "try_again",
        2: "already",
        3: "invalid_argument",
        4: "data_too_large",
        6: "out_of_memory",
        15: "busy",
        21: "no_ble_address",
        22: "host_not_synced",
        30: "disabled",
    }
    hci_errors = {
        0x07: "hci_memory_capacity_exceeded",
        0x0C: "hci_command_disallowed",
        0x11: "hci_unsupported_feature_or_parameter",
        0x12: "hci_invalid_command_parameters",
        0x3A: "hci_controller_busy",
    }

    if code in host_errors:
        return host_errors[code]

    if 0x200 <= code < 0x300:
        return hci_errors.get(code - 0x200, f"hci_error_0x{code - 0x200:02X}")

    return "unknown"


def format_ble_status(data: Iterable[int]) -> str:
    status = bytes(data)
    if len(status) != BLE_STATUS_LEN or status[0] != ord("B"):
        return f"unexpected:{hex_report(status)}"

    flags = status[2]
    names = []
    if flags & 0x01:
        names.append("init_ok")
    if flags & 0x02:
        names.append("synced")
    if flags & 0x04:
        names.append("advertising")
    if flags & 0x08:
        names.append("connected")
    if flags & 0x10:
        names.append("notify")
    if not names:
        names.append("none")

    last_error = int.from_bytes(status[4:6], "little", signed=False)
    report_handle = int.from_bytes(status[6:8], "little", signed=False)
    return (
        f"v={status[1]} flags={','.join(names)} "
        f"addr_type={status[3]} last_error={last_error}({ble_error_name(last_error)}) "
        f"report_handle=0x{report_handle:04X}"
    )


def format_capabilities(data: Iterable[int]) -> str:
    capabilities = bytes(data)
    if len(capabilities) != CAPABILITIES_LEN or capabilities[:4] != b"FSH1":
        return f"unexpected:{hex_report(capabilities)}"

    flags = capabilities[5]
    names = []
    if flags & 0x01:
        names.append("managed_catalog")
    if flags & 0x02:
        names.append("ble_control")
    if flags & 0x04:
        names.append("raw_packet_set")
    if flags & 0x08:
        names.append("physical_reader")
    if not names:
        names.append("none")

    return f"v={capabilities[4]} flags={','.join(names)} raw={hex_report(capabilities)}"


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
    parser.add_argument("--raw-packet", metavar="HEX", help="Set a stable 7-byte raw endpoint packet")
    parser.add_argument("--remove", action="store_true", help="Set the no-figure report")
    parser.add_argument("--poll", type=int, default=0, metavar="N", help="Read N interrupt reports")
    parser.add_argument("--read-control", action="store_true", help="Read the current report with EP0")
    parser.add_argument("--ble-status", action="store_true", help="Read firmware BLE state with EP0")
    parser.add_argument("--capabilities", action="store_true", help="Read firmware capability flags with EP0")
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

    if args.raw_packet:
        raw_packet = parse_raw_packet(args.raw_packet)
        dev.ctrl_transfer(TYPE_VENDOR_OUT, REQ_SET_RAW_PACKET, 0, 0, raw_packet, timeout=1000)
        print(f"RAW {hex_report(raw_packet)}")

    if args.remove:
        dev.ctrl_transfer(TYPE_VENDOR_OUT, REQ_REMOVE, 0, 0, b"", timeout=1000)
        print("REMOVE FFFFFFF000000000")

    if args.read_control:
        report = dev.ctrl_transfer(TYPE_VENDOR_IN, REQ_GET_REPORT, 0, 0, REPORT_LEN, timeout=1000)
        print(f"CONTROL {hex_report(report)}")

    if args.ble_status:
        try:
            status = dev.ctrl_transfer(TYPE_VENDOR_IN, REQ_GET_BLE_STATUS, 0, 0, BLE_STATUS_LEN, timeout=1000)
        except usb.core.USBError as exc:
            raise SystemExit("BLE status request failed; reflash the diagnostic firmware first") from exc
        print(f"BLE {format_ble_status(status)}")

    if args.capabilities:
        try:
            capabilities = dev.ctrl_transfer(TYPE_VENDOR_IN, REQ_GET_CAPABILITIES, 0, 0, CAPABILITIES_LEN, timeout=1000)
        except usb.core.USBError as exc:
            raise SystemExit("Capabilities request failed; firmware may not support request 0x06") from exc
        print(f"CAPS {format_capabilities(capabilities)}")

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
