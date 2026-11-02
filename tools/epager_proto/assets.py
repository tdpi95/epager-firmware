"""Asset helpers (contracts/asset-formats.md): the app icon id rule, mono1, rgb565 and rgb565a8 packing.

The app computes `app_icon_id` to name the icon it uploads; the board computes it to find the icon
of a notification. Vectors: contracts/test-vectors/app_icons.json.
"""
from __future__ import annotations

import hashlib
import struct

from . import transfer

ASSET_TYPE_APP_ICON = 5
APP_ICON_ID_BYTES = 16  # hex of the first 16 SHA-256 bytes = 32 chars, the `id` type's maximum
BLACK, WHITE = "#", "."


def app_icon_id(app_id: str) -> str:
    """Asset id of an app's icon: lower-case hex of SHA-256(app_id as UTF-8)[:16]. No normalisation:
    the app_id is hashed exactly as the board reports it (case-sensitive)."""
    return hashlib.sha256(app_id.encode("utf-8")).digest()[:APP_ICON_ID_BYTES].hex()


def mono1_pack(rows: list[str]) -> bytes:
    """Rows of '#' (black, bit 0) and '.' (white, bit 1), top to bottom; MSB = leftmost pixel; each
    row is ceil(width / 8) bytes and its padding bits are 0."""
    width = len(rows[0])
    out = bytearray()
    for row in rows:
        assert len(row) == width and set(row) <= {BLACK, WHITE}, row
        line = bytearray((width + 7) // 8)
        for x, px in enumerate(row):
            if px == WHITE:
                line[x // 8] |= 0x80 >> (x % 8)
        out += line
    return bytes(out)


def app_icon(rows: list[str]) -> bytes:
    """An `app_icon` + `mono1` asset file (16-byte container header + packed rows)."""
    return transfer.container(ASSET_TYPE_APP_ICON, transfer.FORMAT_MONO1, mono1_pack(rows), len(rows[0]), len(rows))


Rgba = tuple[int, int, int, int]


def rgb565(r: int, g: int, b: int) -> int:
    """8-bit channels to RGB565 (bits 15-11 red, 10-5 green, 4-0 blue), by truncation."""
    return (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3


def rgb565_pack(pixels: list[list[tuple[int, int, int]]]) -> bytes:
    """Rows top to bottom of (r, g, b): 2 bytes per pixel, little-endian RGB565, no padding."""
    return b"".join(struct.pack("<H", rgb565(*px)) for row in pixels for px in row)


def rgb565a8_pack(pixels: list[list[Rgba]]) -> bytes:
    """Rows top to bottom of (r, g, b, a): the RGB565 plane (little-endian) then the alpha plane."""
    return rgb565_pack([[px[:3] for px in row] for row in pixels]) + bytes(px[3] for row in pixels for px in row)


def app_icon_rgb565a8(pixels: list[list[Rgba]]) -> bytes:
    """An `app_icon` + `rgb565a8` asset file (16-byte container header + both planes)."""
    return transfer.container(ASSET_TYPE_APP_ICON, transfer.FORMAT_RGB565A8, rgb565a8_pack(pixels),
                              len(pixels[0]), len(pixels))
