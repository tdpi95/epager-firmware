"""Reference message codec (protocol.md sections 2.2-2.4).

Decoding follows the validation order of protocol.md 2.4 exactly; the C library must
produce the same status for every vector.
"""
from __future__ import annotations

import re
import struct
from typing import Any

from .registry import INT_WIDTH, SIGNED, Field, Registry

HEADER_LEN = 4
_ID_RE = re.compile(rb"^[a-z0-9._-]+$")


class ProtoError(Exception):
    def __init__(self, status: str, detail: str = ""):
        super().__init__(f"{status}: {detail}" if detail else status)
        self.status = status


# --------------------------------------------------------------------------- decode

def decode_message(reg: Registry, buf: bytes) -> dict:
    """Return the logical form {ver, type, req_id, fields}. Raises ProtoError."""
    if len(buf) < HEADER_LEN:
        raise ProtoError("MALFORMED", "short header")
    ver, mtype, req_id = buf[0], buf[1], int.from_bytes(buf[2:4], "little")
    if ver != reg.proto_major:
        raise ProtoError("UNSUPPORTED_VERSION")
    msg = reg.msg_by_id.get(mtype)
    if msg is None:
        raise ProtoError("UNKNOWN_MESSAGE")
    fields = decode_fields(reg, msg.fields, buf[HEADER_LEN:])
    return {"ver": ver, "type": msg.name, "req_id": req_id, "fields": fields}


def decode_fields(reg: Registry, descs: list[Field], body: bytes) -> dict:
    by_tag = {f.tag: f for f in descs}
    out: dict[str, Any] = {}
    seen: set[int] = set()
    i = 0
    while i < len(body):
        if len(body) - i < 3:
            raise ProtoError("MALFORMED", "truncated TLV header")
        tag = body[i]
        ln = int.from_bytes(body[i + 1:i + 3], "little")
        i += 3
        if tag == 0:
            raise ProtoError("MALFORMED", "tag 0")
        if i + ln > len(body):
            raise ProtoError("MALFORMED", "TLV value past end")
        val = body[i:i + ln]
        i += ln
        f = by_tag.get(tag)
        if f is None:
            continue  # unknown tags are skipped
        if not f.repeated and tag in seen:
            raise ProtoError("MALFORMED", f"duplicate {f.name}")
        seen.add(tag)
        v = decode_value(reg, f, val)
        if f.repeated:
            out.setdefault(f.name, []).append(v)
        else:
            out[f.name] = v
    for f in sorted(descs, key=lambda d: d.tag):
        if f.required and f.tag not in seen:
            raise ProtoError("MISSING_FIELD", f.name)
    return out


def decode_value(reg: Registry, f: Field, val: bytes) -> Any:
    t = f.type
    if t in INT_WIDTH:
        if len(val) != INT_WIDTH[t]:
            raise ProtoError("MALFORMED", f"{f.name}: width")
        v = int.from_bytes(val, "little", signed=t in SIGNED)
        if (f.min is not None and v < f.min) or (f.max is not None and v > f.max):
            raise ProtoError("OUT_OF_RANGE", f.name)
        return v
    if t == "bool":
        if len(val) != 1 or val[0] > 1:
            raise ProtoError("MALFORMED", f"{f.name}: bool")
        return bool(val[0])
    if t == "struct":
        return decode_fields(reg, reg.structs[f.struct], val)
    # variable length
    if f.eff_max_len is not None and len(val) > f.eff_max_len:
        raise ProtoError("TOO_LARGE", f.name)
    if f.eff_min_len is not None and len(val) < f.eff_min_len:
        raise ProtoError("MALFORMED", f"{f.name}: min_len")
    if t == "bytes":
        return val.hex()
    if t == "text":
        return val.decode("utf-8", errors="replace")
    try:
        s = val.decode("utf-8")
    except UnicodeDecodeError:
        raise ProtoError("INVALID_UTF8", f.name) from None
    if t == "id" and (not _ID_RE.match(val) or val.startswith(b".")):
        raise ProtoError("INVALID_ID", f.name)
    return s


# --------------------------------------------------------------------------- encode

def encode_message(reg: Registry, logical: dict) -> bytes:
    """Encode the logical form canonically (ascending tags). Does not validate ranges."""
    msg = reg.msg_by_name[logical["type"]]
    hdr = bytes([logical.get("ver", reg.proto_major), msg.id]) + struct.pack("<H", logical.get("req_id", 0))
    return hdr + encode_fields(reg, msg.fields, logical.get("fields", {}))


def encode_fields(reg: Registry, descs: list[Field], values: dict) -> bytes:
    by_name = {f.name: f for f in descs}
    unknown = set(values) - set(by_name)
    if unknown:
        raise KeyError(f"unknown field(s) {sorted(unknown)}")
    out = bytearray()
    for f in sorted(descs, key=lambda d: d.tag):
        if f.name not in values:
            continue
        items = values[f.name] if f.repeated else [values[f.name]]
        for item in items:
            val = encode_value(reg, f, item)
            out += tlv(f.tag, val)
    return bytes(out)


def encode_value(reg: Registry, f: Field, v: Any) -> bytes:
    t = f.type
    if t in INT_WIDTH:
        return int(v).to_bytes(INT_WIDTH[t], "little", signed=t in SIGNED)
    if t == "bool":
        return b"\x01" if v else b"\x00"
    if t == "struct":
        return encode_fields(reg, reg.structs[f.struct], v)
    if t == "bytes":
        return bytes.fromhex(v)
    return v.encode("utf-8")


def tlv(tag: int, val: bytes) -> bytes:
    return bytes([tag]) + struct.pack("<H", len(val)) + val


def header(reg: Registry, type_name: str, req_id: int = 0, ver: int | None = None) -> bytes:
    msg = reg.msg_by_name[type_name]
    return bytes([reg.proto_major if ver is None else ver, msg.id]) + struct.pack("<H", req_id)
