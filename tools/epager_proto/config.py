"""Reference config value encoding and per-key validation (protocol.md section 5)."""
from __future__ import annotations

from typing import Any

from .registry import INT_WIDTH, SIGNED, Registry


def key_by_name(reg: Registry, name: str) -> dict:
    return next(k for k in reg.config_keys if k["name"] == name)


def key_by_id(reg: Registry, key_id: int) -> dict | None:
    return next((k for k in reg.config_keys if k["id"] == key_id), None)


def applies(key: dict, variant: str) -> bool:
    return "variants" not in key or variant in key["variants"]


def encode_value(key: dict, v: Any) -> bytes:
    t = key["type"]
    if t in INT_WIDTH:
        return int(v).to_bytes(INT_WIDTH[t], "little", signed=t in SIGNED)
    if t == "bool":
        return b"\x01" if v else b"\x00"
    if t == "bytes":
        return bytes.fromhex(v)
    return v.encode("utf-8")


def decode_value(key: dict, b: bytes) -> Any:
    t = key["type"]
    if t in INT_WIDTH:
        return int.from_bytes(b, "little", signed=t in SIGNED)
    if t == "bool":
        return bool(b[0])
    if t == "bytes":
        return b.hex()
    return b.decode("utf-8")


def validate(reg: Registry, key_id: int, value: bytes, variant: str) -> str:
    """Return a status name. Order: unknown key, read-only, width/length, UTF-8, range."""
    key = key_by_id(reg, key_id)
    if key is None or not applies(key, variant):
        return "UNKNOWN_KEY"
    if key.get("read_only"):
        return "READ_ONLY"
    t = key["type"]
    if t in INT_WIDTH:
        if len(value) != INT_WIDTH[t]:
            return "TYPE_MISMATCH"
        v = int.from_bytes(value, "little", signed=t in SIGNED)
        if "min" in key and v < key["min"] or "max" in key and v > key["max"]:
            return "OUT_OF_RANGE"
        if "enum" in key and v not in {e["value"] for e in reg.enums[key["enum"]]["values"]}:
            return "OUT_OF_RANGE"
        return "OK"
    if t == "bool":
        if len(value) != 1:
            return "TYPE_MISMATCH"
        return "OK" if value[0] <= 1 else "OUT_OF_RANGE"
    if len(value) > key["max_len"]:
        return "TOO_LARGE"
    if t == "bytes":
        return "OK"
    try:
        value.decode("utf-8")
    except UnicodeDecodeError:
        return "INVALID_UTF8"
    return "OK"
