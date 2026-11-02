"""Reference segmentation and reassembly (protocol.md 2.1)."""
from __future__ import annotations

FIRST = 0x80
LAST = 0x40
SEQ_MASK = 0x3F


def split(message: bytes, max_segment: int) -> list[bytes]:
    """Split into segments of at most max_segment bytes (header included)."""
    if max_segment < 2:
        raise ValueError("max_segment must be >= 2")
    room = max_segment - 1
    chunks = [message[i:i + room] for i in range(0, len(message), room)] or [b""]
    segs = []
    for n, c in enumerate(chunks):
        hdr = (n % 64) | (FIRST if n == 0 else 0) | (LAST if n == len(chunks) - 1 else 0)
        segs.append(bytes([hdr]) + c)
    return segs


class Reassembler:
    """Feed segments; each call returns a list of events:
    ("message", bytes) or ("error", status_name)."""

    IDLE, ASSEMBLING, DISCARDING = range(3)

    def __init__(self, max_message: int):
        self.max_message = max_message
        self.state = self.IDLE
        self.buf = bytearray()
        self.expected = 0

    def feed(self, seg: bytes) -> list[tuple[str, object]]:
        ev: list[tuple[str, object]] = []
        if not seg:
            if self.state == self.ASSEMBLING:
                self.state = self.DISCARDING
            ev.append(("error", "SEGMENT_ERROR"))
            return ev
        hdr, payload = seg[0], seg[1:]
        first, last, seq = hdr & FIRST, hdr & LAST, hdr & SEQ_MASK
        if first:
            if self.state == self.ASSEMBLING:
                ev.append(("error", "SEGMENT_ERROR"))  # previous partial message discarded
            if seq != 0:
                self.state = self.DISCARDING
                ev.append(("error", "SEGMENT_ERROR"))
                return ev
            self.buf = bytearray()
            self.expected = 0
            self.state = self.ASSEMBLING
        elif self.state == self.DISCARDING:
            return ev
        elif self.state == self.IDLE:
            self.state = self.DISCARDING
            ev.append(("error", "SEGMENT_ERROR"))
            return ev
        elif seq != self.expected:
            self.state = self.DISCARDING
            ev.append(("error", "SEGMENT_ERROR"))
            return ev
        if len(self.buf) + len(payload) > self.max_message:
            self.state = self.DISCARDING
            ev.append(("error", "TOO_LARGE"))
            return ev
        self.buf += payload
        self.expected = (self.expected + 1) % 64
        if last:
            self.state = self.IDLE
            if not self.buf:
                ev.append(("error", "SEGMENT_ERROR"))
            else:
                ev.append(("message", bytes(self.buf)))
        return ev
