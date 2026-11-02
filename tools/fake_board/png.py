"""Minimal 8-bit greyscale or RGB PNG writer and reader (stdlib zlib only). Deterministic: the same
pixels always give the same bytes with the same zlib. The reader handles what the writer produces."""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

_SIG = b"\x89PNG\r\n\x1a\n"
_COLOUR_TYPE = {1: 0, 3: 2}  # channels -> PNG colour type (greyscale, truecolour)


def _chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def encode(px: bytes, width: int, height: int, scale: int = 1, channels: int = 1) -> bytes:
    """`px`: rows top to bottom, `channels` bytes per pixel (1 = grey, 3 = RGB)."""
    if len(px) != width * height * channels:
        raise ValueError("pixel count does not match the size")
    stride = width * channels
    rows = []
    for y in range(height):
        row = px[y * stride:(y + 1) * stride]
        if scale > 1:
            row = b"".join(row[i:i + channels] * scale for i in range(0, stride, channels))
        rows += [b"\x00" + row] * scale
    ihdr = struct.pack(">IIBBBBB", width * scale, height * scale, 8, _COLOUR_TYPE[channels], 0, 0, 0)
    return _SIG + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + _chunk(b"IEND", b"")


def write(path: Path, px: bytes, width: int, height: int, scale: int = 1, channels: int = 1) -> None:
    Path(path).write_bytes(encode(px, width, height, scale, channels))


def decode(data: bytes) -> tuple[int, int, bytes]:
    """(width, height, pixels) of an 8-bit greyscale or RGB, non-interlaced PNG with filter type 0 rows."""
    if not data.startswith(_SIG):
        raise ValueError("not a PNG")
    pos, idat, width, height, channels = len(_SIG), b"", 0, 0, 1
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (0, 2) or interlace:
                raise ValueError("only 8-bit greyscale or RGB, non-interlaced PNGs are supported")
            channels = 1 if ctype == 0 else 3
        elif kind == b"IDAT":
            idat += body
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray()
    for y in range(height):
        line = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        if line[0] != 0:
            raise ValueError("only filter type 0 is supported")
        out += line[1:]
    return width, height, bytes(out)


def read(path: Path) -> tuple[int, int, bytes]:
    return decode(Path(path).read_bytes())
