"""Extract character thumbnails from U.B. Funkeys SWF files.

The game XML maps each Funkey ID to a SWF. The character thumbnail is the
first character-sized bitmap in the SWF, usually a 55x62 DefineBitsJPEG3 tag.
Some later files use DefineBitsLossless2 instead, so this script handles both.
"""

from __future__ import annotations

import argparse
import io
import re
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path
from xml.etree import ElementTree

from PIL import Image


DEFAULT_SWF_DIR = Path(r"C:\Users\jLynx\Documents\U.B. Funkeys\RadicaGame\funkeys")
DEFAULT_XML = Path("decoded_rdf/system/funkeys.xml")
DEFAULT_OUTPUT_DIR = Path("web/public/funkeys")


@dataclass(frozen=True)
class FunkeyAsset:
    funkey_id: str
    name: str
    url: str
    output_name: str


@dataclass(frozen=True)
class ImageTag:
    code: int
    character_id: int
    image: Image.Image


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xml", type=Path, default=DEFAULT_XML, help="Path to decoded funkeys.xml")
    parser.add_argument("--swf-dir", type=Path, default=DEFAULT_SWF_DIR, help="Directory containing source SWF files")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUTPUT_DIR, help="Output directory for PNG files")
    return parser.parse_args()


def load_assets(xml_path: Path) -> list[FunkeyAsset]:
    root = ElementTree.parse(xml_path).getroot()
    assets: list[FunkeyAsset] = []

    for node in root.findall("funkey"):
        funkey_id = (node.get("id") or "").strip()
        url = (node.get("url") or "").strip()
        if not funkey_id or not url:
            continue

        name = (node.get("name") or funkey_id).strip()
        assets.append(
            FunkeyAsset(
                funkey_id=funkey_id,
                name=name,
                url=url,
                output_name=output_name_for_id(funkey_id),
            )
        )

    return assets


def output_name_for_id(funkey_id: str) -> str:
    if re.fullmatch(r"[0-9a-fA-F]+", funkey_id):
        return f"{int(funkey_id, 16):08X}.png"

    safe_id = re.sub(r"[^A-Za-z0-9_-]+", "-", funkey_id).strip("-")
    return f"{safe_id or 'unknown'}.png"


def read_swf_body(path: Path) -> bytes:
    data = path.read_bytes()
    signature = data[:3]

    if signature == b"CWS":
        return zlib.decompress(data[8:])

    if signature == b"FWS":
        return data[8:]

    raise ValueError(f"unsupported SWF signature {signature!r}")


def first_tag_offset(body: bytes) -> int:
    rect_nbits = body[0] >> 3
    rect_bits = 5 + rect_nbits * 4
    rect_bytes = (rect_bits + 7) // 8
    return rect_bytes + 4  # RECT + FrameRate + FrameCount


def iter_tags(body: bytes):
    offset = first_tag_offset(body)

    while offset + 2 <= len(body):
        header = struct.unpack_from("<H", body, offset)[0]
        offset += 2
        code = header >> 6
        length = header & 0x3F

        if length == 0x3F:
            if offset + 4 > len(body):
                return
            length = struct.unpack_from("<I", body, offset)[0]
            offset += 4

        payload = body[offset : offset + length]
        offset += length
        yield code, payload

        if code == 0:
            return


def decode_jpeg(payload: bytes, has_alpha: bool) -> ImageTag | None:
    if len(payload) < 2:
        return None

    character_id = struct.unpack_from("<H", payload, 0)[0]

    if has_alpha:
        if len(payload) < 6:
            return None
        alpha_offset = struct.unpack_from("<I", payload, 2)[0]
        image_data = payload[6 : 6 + alpha_offset]
        alpha_data = payload[6 + alpha_offset :]
    else:
        image_data = payload[2:]
        alpha_data = b""

    image = Image.open(io.BytesIO(image_data)).convert("RGBA")

    if alpha_data:
        alpha = zlib.decompress(alpha_data)
        expected_alpha_size = image.width * image.height
        if len(alpha) == expected_alpha_size:
            image.putalpha(Image.frombytes("L", image.size, alpha))

    return ImageTag(code=35 if has_alpha else 21, character_id=character_id, image=image.copy())


def decode_lossless(payload: bytes, has_alpha: bool) -> ImageTag | None:
    if len(payload) < 7:
        return None

    character_id, bitmap_format, width, height = struct.unpack_from("<HBHH", payload, 0)
    data_offset = 7
    color_table_size = 0

    if bitmap_format == 3:
        if len(payload) < 8:
            return None
        color_table_size = payload[7] + 1
        data_offset = 8

    bitmap_data = zlib.decompress(payload[data_offset:])

    if bitmap_format == 3:
        return ImageTag(
            code=36 if has_alpha else 20,
            character_id=character_id,
            image=decode_colormapped(bitmap_data, width, height, color_table_size, has_alpha),
        )

    if bitmap_format == 4:
        return ImageTag(
            code=36 if has_alpha else 20,
            character_id=character_id,
            image=decode_rgb555(bitmap_data, width, height),
        )

    if bitmap_format == 5:
        return ImageTag(
            code=36 if has_alpha else 20,
            character_id=character_id,
            image=decode_argb(bitmap_data, width, height, has_alpha),
        )

    return None


def decode_colormapped(data: bytes, width: int, height: int, color_count: int, has_alpha: bool) -> Image.Image:
    palette_stride = 4 if has_alpha else 3
    palette_bytes = color_count * palette_stride
    palette_data = data[:palette_bytes]
    index_data = data[palette_bytes:]
    palette: list[tuple[int, int, int, int]] = []

    for index in range(color_count):
        start = index * palette_stride
        if has_alpha:
            red, green, blue, alpha = palette_data[start : start + 4]
        else:
            red, green, blue = palette_data[start : start + 3]
            alpha = 255
        palette.append((red, green, blue, alpha))

    row_stride = (width + 3) & ~3
    rgba = bytearray(width * height * 4)

    for y in range(height):
        row_start = y * row_stride
        for x in range(width):
            palette_index = index_data[row_start + x]
            red, green, blue, alpha = palette[palette_index]
            out = (y * width + x) * 4
            rgba[out : out + 4] = bytes((red, green, blue, alpha))

    return Image.frombytes("RGBA", (width, height), bytes(rgba))


def decode_rgb555(data: bytes, width: int, height: int) -> Image.Image:
    row_stride = ((width * 2 + 3) // 4) * 4
    rgba = bytearray(width * height * 4)

    for y in range(height):
        row_start = y * row_stride
        for x in range(width):
            packed = struct.unpack_from("<H", data, row_start + x * 2)[0]
            red = ((packed >> 10) & 0x1F) * 255 // 31
            green = ((packed >> 5) & 0x1F) * 255 // 31
            blue = (packed & 0x1F) * 255 // 31
            out = (y * width + x) * 4
            rgba[out : out + 4] = bytes((red, green, blue, 255))

    return Image.frombytes("RGBA", (width, height), bytes(rgba))


def decode_argb(data: bytes, width: int, height: int, has_alpha: bool) -> Image.Image:
    expected_size = width * height * 4
    if len(data) < expected_size:
        raise ValueError(f"bitmap data is too small for {width}x{height}")

    rgba = bytearray(expected_size)
    for index in range(width * height):
        alpha_or_reserved, red, green, blue = data[index * 4 : index * 4 + 4]
        alpha = alpha_or_reserved if has_alpha else 255
        rgba[index * 4 : index * 4 + 4] = bytes((red, green, blue, alpha))

    return Image.frombytes("RGBA", (width, height), bytes(rgba))


def decode_image_tag(code: int, payload: bytes) -> ImageTag | None:
    if code == 21:
        return decode_jpeg(payload, has_alpha=False)
    if code == 35:
        return decode_jpeg(payload, has_alpha=True)
    if code == 20:
        return decode_lossless(payload, has_alpha=False)
    if code == 36:
        return decode_lossless(payload, has_alpha=True)
    return None


def looks_like_character_thumbnail(image: Image.Image) -> bool:
    width, height = image.size
    return 40 <= width <= 85 and 50 <= height <= 95


def extract_character_image(path: Path) -> ImageTag:
    body = read_swf_body(path)
    first_image: ImageTag | None = None

    for code, payload in iter_tags(body):
        image_tag = decode_image_tag(code, payload)
        if image_tag is None:
            continue

        if first_image is None:
            first_image = image_tag

        if looks_like_character_thumbnail(image_tag.image):
            return image_tag

    if first_image is not None:
        return first_image

    raise ValueError("no supported bitmap tags found")


def main() -> int:
    args = parse_args()
    assets = load_assets(args.xml)
    args.out.mkdir(parents=True, exist_ok=True)

    image_cache: dict[str, ImageTag] = {}
    exported = 0
    missing: list[FunkeyAsset] = []
    failed: list[tuple[FunkeyAsset, str]] = []

    for asset in assets:
        swf_path = args.swf_dir / asset.url
        if not swf_path.exists():
            missing.append(asset)
            continue

        try:
            image_tag = image_cache.get(asset.url)
            if image_tag is None:
                image_tag = extract_character_image(swf_path)
                image_cache[asset.url] = image_tag

            image_tag.image.save(args.out / asset.output_name)
            exported += 1
        except Exception as error:  # noqa: BLE001 - report every file that fails.
            failed.append((asset, str(error)))

    print(f"Exported {exported} PNGs to {args.out}")

    if missing:
        print(f"Missing SWFs: {len(missing)}")
        for asset in missing:
            print(f"  {asset.funkey_id} {asset.name}: {asset.url}")

    if failed:
        print(f"Failed exports: {len(failed)}")
        for asset, reason in failed:
            print(f"  {asset.funkey_id} {asset.name}: {asset.url} ({reason})")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
