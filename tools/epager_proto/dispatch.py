"""Reference model of the board's message dispatch (protocol.md 2.1, 2.4 routing and security).

Covers what happens between a characteristic write and a handler: per-characteristic reassembly,
header checks, routing, the security gate, body validation and segmenting the answer at the link's
MTU. The only handler is PING; every other valid request is out of scope (its behaviour is covered
by messages.json and transfer.json) and raises. Used by tools/codegen/vectors.py for dispatch.json.
"""
from __future__ import annotations

from . import codec, segments
from .registry import Registry

# Characteristic ids as in the TCP transport (protocol.md 10)
CHARS = {"capabilities": 1, "config": 2, "notification": 3, "transfer": 4, "status": 5}
WRITABLE = ("config", "notification", "transfer")


def segment_max(mtu: int) -> int:
    """Bytes per write (protocol.md 2.1): min(ATT_MTU - 3, 512)."""
    return min(mtu - 3, 512)


class OutOfScope(Exception):
    """A valid request that needs a real handler."""


class Dispatcher:
    def __init__(self, reg: Registry, mtu: int = 517, secure: bool = True):
        self.reg = reg
        self.mtu = mtu
        self.secure = secure
        self.rx = {c: segments.Reassembler(reg.limits[f"max_message_{c}"]) for c in WRITABLE}

    def write(self, char: str, seg: bytes) -> list[tuple[str, bytes]]:
        """One characteristic write; returns the notified segments as (char, bytes)."""
        out: list[tuple[str, bytes]] = []
        for kind, val in self.rx[char].feed(seg):
            msg = self._error(val, 0, None) if kind == "error" else self._message(char, val)
            if msg is not None:
                out += [(char, s) for s in segments.split(msg, segment_max(self.mtu))]
        return out

    # ------------------------------------------------------------------ internals

    def _error(self, status: str, req_id: int, msg_type: int | None) -> bytes:
        fields: dict = {"status": self.reg.status_by_name[status]}
        if msg_type is not None:
            fields["msg_type"] = msg_type
        return codec.encode_message(self.reg, {"ver": self.reg.proto_major, "type": "ERROR", "req_id": req_id,
                                               "fields": fields})

    def _status_only(self, req, req_id: int, status: str, body: bytes) -> bytes:
        st = self.reg.status_by_name[status]
        if req.name == "XFER_CHUNK":
            handle = 0
            if status != "INSUFFICIENT_SECURITY":
                handle = _top_level_u8(body, 1)
            return codec.encode_message(self.reg, {"ver": self.reg.proto_major, "type": "XFER_ACK", "req_id": 0,
                                                   "fields": {"handle": handle, "offset": 0, "status": st}})
        rsp = self.reg.msg_by_id[req.id | 0x80]
        return codec.encode_message(self.reg, {"ver": self.reg.proto_major, "type": rsp.name, "req_id": req_id,
                                               "fields": {"status": st}})

    def _message(self, char: str, buf: bytes) -> bytes | None:
        if len(buf) < codec.HEADER_LEN:
            return self._error("MALFORMED", 0, buf[1] if len(buf) >= 2 else None)
        ver, mtype, req_id = buf[0], buf[1], int.from_bytes(buf[2:4], "little")
        if ver != self.reg.proto_major:
            return self._error("UNSUPPORTED_VERSION", req_id, mtype)
        msg = self.reg.msg_by_id.get(mtype)
        if msg is None:
            return self._error("UNKNOWN_MESSAGE", req_id, mtype)
        if msg.kind != "request" or msg.channel not in ("any", char):
            return self._error("UNKNOWN_MESSAGE", req_id, mtype)
        body = buf[codec.HEADER_LEN:]
        if not self.secure:
            return self._status_only(msg, req_id, "INSUFFICIENT_SECURITY", body)
        try:
            fields = codec.decode_fields(self.reg, msg.fields, body)
        except codec.ProtoError as e:
            return self._status_only(msg, req_id, e.status, body)
        if msg.name != "PING":
            raise OutOfScope(msg.name)
        rsp: dict = {"status": 0}
        if "payload" in fields:
            rsp["payload"] = fields["payload"]
        return codec.encode_message(self.reg, {"ver": self.reg.proto_major, "type": "PING_RSP", "req_id": req_id,
                                               "fields": rsp})


def _top_level_u8(body: bytes, tag: int) -> int:
    """Value of a 1-byte top-level field if the TLVs up to it are well formed, else 0
    (an invalid XFER_CHUNK is acked with the handle as sent when it can be read)."""
    i = 0
    while len(body) - i >= 3:
        t, ln = body[i], int.from_bytes(body[i + 1:i + 3], "little")
        i += 3
        if t == 0 or i + ln > len(body):
            return 0
        if t == tag:
            return body[i] if ln == 1 else 0
        i += ln
    return 0
