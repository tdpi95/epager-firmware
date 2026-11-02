"""LZ4 block format (no frame), enough for the colour emoji (PLAN D59, D60): a greedy compressor and a
reference decompressor that checks every block. LVGL decodes the blocks with its bundled
LZ4_decompress_safe (LV_USE_LZ4_INTERNAL) when it draws a compressed image.

Format rules kept (lz4 docs/lz4_Block_format.md): matches are at least 4 bytes, the last 5 bytes are
literals, the last match starts at least 12 bytes before the end, offsets are 1-65535.
"""
from __future__ import annotations

MIN_MATCH = 4
LAST_LITERALS = 5
MF_LIMIT = 12
MAX_OFFSET = 65535


def _length(out: bytearray, n: int) -> None:
    while n >= 255:
        out.append(255)
        n -= 255
    out.append(n)


def _sequence(out: bytearray, literals: bytes, offset: int = 0, match: int = 0) -> None:
    lit, ml = len(literals), match - MIN_MATCH
    out.append(min(lit, 15) << 4 | (min(ml, 15) if match else 0))
    if lit >= 15:
        _length(out, lit - 15)
    out += literals
    if match:
        out += offset.to_bytes(2, "little")
        if ml >= 15:
            _length(out, ml - 15)


def compress(src: bytes) -> bytes:
    n, out, table = len(src), bytearray(), {}
    anchor = i = 0
    while i + MF_LIMIT <= n:
        key = src[i:i + MIN_MATCH]
        cand = table.get(key)
        table[key] = i
        if cand is not None and i - cand <= MAX_OFFSET:
            m = MIN_MATCH
            while i + m < n - LAST_LITERALS and src[cand + m] == src[i + m]:
                m += 1
            _sequence(out, src[anchor:i], i - cand, m)
            i += m
            anchor = i
        else:
            i += 1
    _sequence(out, src[anchor:])
    return bytes(out)


def decompress(src: bytes, size: int) -> bytes:
    """Strict: raises ValueError on anything LZ4_decompress_safe would reject for this size."""
    out, i = bytearray(), 0
    while True:
        if i >= len(src):
            raise ValueError("truncated")
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i == len(src):
            break  # the last sequence: literals only
        offset = src[i] | src[i + 1] << 8
        i += 2
        if offset == 0 or offset > len(out):
            raise ValueError("bad offset")
        m = token & 15
        if m == 15:
            while True:
                b = src[i]
                i += 1
                m += b
                if b != 255:
                    break
        for _ in range(m + MIN_MATCH):
            out.append(out[-offset])
    if len(out) != size:
        raise ValueError(f"size {len(out)} != {size}")
    return bytes(out)
