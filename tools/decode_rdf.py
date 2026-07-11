#!/usr/bin/env python3
"""Decode U.B. Funkeys RDF data files.

Most RadicaGame ``*.rdf`` files are XML text wrapped in an 8-byte magic header
and XOR-obfuscated with an 8-byte repeating key.
"""

from __future__ import annotations

import argparse
import base64
import csv
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Iterable


MAGIC = bytes.fromhex("ff 00 ff ac eb 96 c4 2a")
XOR_KEY = bytes.fromhex("44 32 6c e5 88 79 7f 95")
TERMINATOR = b"\x00\x06"

RARITY_LABELS = {
    "0": "Common",
    "1": "Rare",
    "2": "Very Rare",
}


def xor_bytes(data: bytes, key_offset: int = 0) -> bytes:
    return bytes(byte ^ XOR_KEY[(index + key_offset) % len(XOR_KEY)] for index, byte in enumerate(data))


def strip_decoded_tail(data: bytes) -> bytes:
    if data.endswith(TERMINATOR):
        data = data[:-len(TERMINATOR)]

    last_tag = data.rfind(b">")
    if last_tag == -1:
        return data.rstrip(b"\x00")

    tail = data[last_tag + 1:]
    if all(byte in b" \t\r\n" or byte <= 0x06 for byte in tail):
        return data[:last_tag + 1]
    return data


def decode_base64_xor_bytes(data: bytes) -> bytes:
    compact = b"".join(data.split())
    payload = base64.b64decode(compact, validate=True)

    for key_offset in range(len(XOR_KEY)):
        decoded = xor_bytes(payload, key_offset)
        if decoded.lstrip().startswith(b"<"):
            return decoded

    raise ValueError("base64 RDF payload did not decode to XML-like text")


def decode_rdf_bytes(data: bytes) -> bytes:
    if data.startswith(MAGIC):
        return strip_decoded_tail(xor_bytes(data[len(MAGIC):]))

    return strip_decoded_tail(decode_base64_xor_bytes(data))


def decode_rdf_file(path: Path) -> bytes:
    return decode_rdf_bytes(path.read_bytes())


def write_text(path: Path, data: bytes, encoding: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data.decode(encoding), encoding="utf-8", newline="")


def funkey_rows(xml_bytes: bytes, encoding: str) -> list[dict[str, str]]:
    root = ET.fromstring(xml_bytes.decode(encoding))
    if root.tag != "funkeys":
        raise ValueError(f"expected <funkeys> root, got <{root.tag}>")

    rows = []
    for node in root.findall("funkey"):
        rarity = node.attrib.get("rarity", "")
        rows.append(
            {
                "id": node.attrib.get("id", ""),
                "name": node.attrib.get("name", ""),
                "rarity": rarity,
                "rarity_label": RARITY_LABELS.get(rarity, rarity),
                "series": node.attrib.get("series", ""),
                "url": node.attrib.get("url", ""),
                "zone": node.attrib.get("zone", ""),
                "mega": node.attrib.get("mega", ""),
                "sp": node.attrib.get("sp", ""),
                "codex": node.attrib.get("codex", ""),
            }
        )
    return rows


def write_rows(path: Path, rows: Iterable[dict[str, str]], delimiter: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = ["id", "name", "rarity", "rarity_label", "series", "url", "zone", "mega", "sp", "codex"]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, delimiter=delimiter)
        writer.writeheader()
        writer.writerows(rows)


def iter_inputs(path: Path, recursive: bool) -> list[Path]:
    if path.is_dir():
        return sorted(path.rglob("*.rdf") if recursive else path.glob("*.rdf"))
    return [path]


def output_path_for(input_root: Path, output_dir: Path, input_path: Path) -> Path:
    if input_root.is_dir():
        return output_dir / input_path.relative_to(input_root).with_suffix(".xml")
    return output_dir / input_path.with_suffix(".xml").name


def decode_many(input_root: Path, output_dir: Path, encoding: str, recursive: bool) -> tuple[int, list[str]]:
    decoded_count = 0
    errors = []

    for input_path in iter_inputs(input_root, recursive):
        try:
            decoded = decode_rdf_file(input_path)
            write_text(output_path_for(input_root, output_dir, input_path), decoded, encoding)
            decoded_count += 1
        except Exception as exc:
            errors.append(f"{input_path}: {exc}")

    return decoded_count, errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode RadicaGame RDF files")
    parser.add_argument("input", type=Path, help="Path to an encoded .rdf file")
    parser.add_argument("--output", "-o", type=Path, help="Write decoded XML/text to this file")
    parser.add_argument("--output-dir", type=Path, help="Decode an input file/directory into this directory")
    parser.add_argument("--recursive", action="store_true", help="With --output-dir and a directory input, decode recursively")
    parser.add_argument(
        "--encoding",
        default="cp1252",
        help="Decoded text encoding. funkeys.rdf uses cp1252; default: %(default)s",
    )
    parser.add_argument("--funkeys-csv", type=Path, help="Parse funkeys.rdf and write id/name rows as CSV")
    parser.add_argument("--funkeys-tsv", type=Path, help="Parse funkeys.rdf and write id/name rows as TSV")
    args = parser.parse_args()

    try:
        if args.output_dir:
            decoded_count, errors = decode_many(args.input, args.output_dir, args.encoding, args.recursive)
            print(f"Decoded {decoded_count} RDF file(s) into {args.output_dir}")
            for error in errors:
                print(f"decode_rdf.py: {error}", file=sys.stderr)
            return 1 if errors else 0

        if args.input.is_dir():
            raise ValueError("directory input requires --output-dir")

        decoded = decode_rdf_file(args.input)
        if args.output:
            write_text(args.output, decoded, args.encoding)

        if args.funkeys_csv or args.funkeys_tsv:
            rows = funkey_rows(decoded, args.encoding)
            if args.funkeys_csv:
                write_rows(args.funkeys_csv, rows, ",")
            if args.funkeys_tsv:
                write_rows(args.funkeys_tsv, rows, "\t")
            print(f"Decoded {len(rows)} funkey rows from {args.input}")
        elif not args.output:
            sys.stdout.write(decoded.decode(args.encoding))
    except Exception as exc:
        print(f"decode_rdf.py: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
