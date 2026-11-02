"""The LZ4 block encoder that packs the colour emoji (tools/assets/lz4_block.py): round trips, the format's
end-of-block rules that LVGL's LZ4_decompress_safe relies on, and a strict decoder that rejects bad blocks."""
from __future__ import annotations

import random
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "assets"))
import lz4_block  # noqa: E402


@pytest.mark.parametrize("data", [
    b"", b"a", b"abcdefghijkl", b"abcdefghijklm", b"\x00" * 13, b"\x00" * 5000, bytes(range(256)) * 40,
    b"ab" * 1000 + b"xyz", bytes(random.Random(1).randrange(256) for _ in range(3000)),
])
def test_round_trip(data: bytes) -> None:
    packed = lz4_block.compress(data)
    assert lz4_block.decompress(packed, len(data)) == data


def test_end_of_block_rules() -> None:
    rng = random.Random(7)
    for _ in range(300):
        n = rng.randrange(0, 600)
        data = bytes(rng.choice(b"ab\x00\xff") for _ in range(n))
        packed = lz4_block.compress(data)
        assert lz4_block.decompress(packed, n) == data
        # the last token has no match part and its literals end the block
        assert packed[-1:] == data[-1:] or n == 0
    assert len(lz4_block.compress(b"\x00" * 5000)) < 40  # a run becomes one long match


def test_decoder_rejects_bad_blocks() -> None:
    packed = lz4_block.compress(b"hello hello hello hello hello")
    with pytest.raises(ValueError):
        lz4_block.decompress(packed, 30)  # wrong size
    with pytest.raises((ValueError, IndexError)):
        lz4_block.decompress(packed[:-3], 29)  # truncated
    with pytest.raises(ValueError):
        lz4_block.decompress(bytes([0x0F, 0x01, 0x00]), 4)  # a match before any output
