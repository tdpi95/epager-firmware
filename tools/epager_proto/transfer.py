"""Reference model of the board's transfer state machine (protocol.md section 7).

Used by tools/codegen/vectors.py to compute the board's answers in transfer.json. The C
implementation (components/transfer) with its in-memory fakes (blob_mem, kv_mem, fw_mem) must
produce byte-identical messages for every scenario. Storage semantics mirrored from the fakes:
free = total - sum(file sizes); asset data `<type>_<id>`, temp `<type>_<id>~`, version `<type>_<id>@`.
"""
from __future__ import annotations

import copy
import hashlib
import re
import struct
import zlib
from typing import Any

from . import codec
from .registry import Registry

KIND_ASSET, KIND_FIRMWARE = 1, 2
ST_IDLE, ST_RECEIVING, ST_VERIFYING, ST_COMPLETE, ST_FAILED = range(5)
FORMAT_MONO1, FORMAT_RGB565, FORMAT_FONT_V1, FORMAT_RGB565A8 = 1, 2, 3, 6
ASSET_TYPE_BACKGROUND, ASSET_TYPE_FONT = 2, 1
HEADER_LEN = 16
LIST_DEFAULT_LIMIT = 16

DEFAULT_BOARD: dict[str, Any] = {
    "max_chunk": 64,
    "ack_window": 128,
    "journal_every": 100,
    "storage_total": 2048,
    "display": {"width": 200, "height": 200},
    "asset_support": [{"asset_type": 3, "format": 4, "max_size": 512},
                      {"asset_type": 1, "format": 3, "max_size": 1024},
                      {"asset_type": 5, "format": 1, "max_size": 88, "width": 24, "height": 24}],
    "firmware": {"slot_size": 1024, "running_version": "0.1.0", "allow_downgrade": False,
                 "low_battery_pct": 30, "battery": {"pct": 80, "external_power": 0}, "image_status": "OK"},
}

# --------------------------------------------------------------------------- semver

_SEMVER = re.compile(r"^([0-9]{1,9})\.([0-9]{1,9})\.([0-9]{1,9})(?:-([0-9A-Za-z.-]+))?(?:\+([0-9A-Za-z.-]+))?$")


def parse_version(v: str | None) -> tuple | None:
    """MAJOR.MINOR.PATCH[-pre][+build]. Numeric parts have at most 9 digits; pre-release and build
    identifiers are non-empty, and numeric pre-release identifiers have at most 9 digits.
    Returns a comparable key or None if invalid."""
    if v is None:
        return None
    m = _SEMVER.match(v)
    if not m:
        return None
    ids = m.group(4).split(".") if m.group(4) is not None else []
    build = m.group(5).split(".") if m.group(5) is not None else []
    if "" in ids or "" in build or any(i.isdigit() and len(i) > 9 for i in ids):
        return None
    return int(m.group(1)), int(m.group(2)), int(m.group(3)), ids


def _cmp_id(x: str, y: str) -> int:
    xd, yd = x.isdigit(), y.isdigit()
    if xd and yd and int(x) != int(y):
        return -1 if int(x) < int(y) else 1
    if xd != yd:
        return -1 if xd else 1  # numeric identifiers sort before alphanumeric ones
    bx, by = x.encode(), y.encode()  # same value written differently ("01" vs "1"): by text
    return (bx > by) - (bx < by)


def compare_versions(a: tuple, b: tuple) -> int:
    """Semver precedence; build metadata ignored. Returns -1, 0 or 1."""
    if a[:3] != b[:3]:
        return -1 if a[:3] < b[:3] else 1
    if not a[3] or not b[3]:
        return (not a[3]) - (not b[3])  # a release is newer than its pre-releases
    for x, y in zip(a[3], b[3]):
        c = _cmp_id(x, y)
        if c:
            return c
    return (len(a[3]) > len(b[3])) - (len(a[3]) < len(b[3]))


# --------------------------------------------------------------------------- model


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


class BoardModel:
    def __init__(self, reg: Registry, board: dict):
        self.reg = reg
        self.b = copy.deepcopy(board)
        self.files: dict[str, bytearray] = {}
        self.journal: dict | None = None
        self.fw = bytearray()          # firmware slot content
        self.fw_written = 0            # high-water mark since the last fw begin
        self._boot()

    # ---- helpers

    def _boot(self) -> None:
        self.session: dict | None = None
        self.next_handle = 1
        keep = self._temp_name(self.journal) if self.journal and self.journal["kind"] == KIND_ASSET else None
        for name in sorted(self.files):
            if name.endswith("~") and name != keep:
                del self.files[name]
            elif name.endswith("@") and name[:-1] not in self.files:
                del self.files[name]

    @staticmethod
    def _final_name(t: dict) -> str:
        return f"{t['asset_type']}_{t['id']}"

    def _temp_name(self, t: dict) -> str:
        return self._final_name(t) + "~"

    def _used(self) -> int:
        return sum(len(v) for v in self.files.values())

    def _free(self) -> int:
        return self.b["storage_total"] - self._used()

    def _blob_write(self, name: str, off: int, data: bytes) -> str:
        cur = self.files.get(name, bytearray())
        grow = max(0, off + len(data) - len(cur))
        if off > len(cur):
            return "INTERNAL"
        if grow > self._free():
            return "NO_SPACE"
        cur[off:off + len(data)] = data
        self.files[name] = cur
        return "OK"

    def _max_size(self, asset_type: int) -> int | None:
        sizes = [a["max_size"] for a in self.b["asset_support"] if a["asset_type"] == asset_type]
        return max(sizes) if sizes else None

    def _battery_ok(self) -> bool:
        fw = self.b["firmware"]
        bat = fw.get("battery")
        if bat is None:
            return True  # no battery reader
        if bat.get("external_power") == 1:
            return True
        return bat.get("pct") is not None and bat["pct"] >= fw["low_battery_pct"]

    def _target(self, f: dict) -> tuple:
        return f["kind"], f.get("asset_type", 0), f["id"]

    def _journal_now(self) -> None:
        s = self.session
        self.journal = {k: s[k] for k in ("kind", "asset_type", "id", "size", "crc32", "sha256", "version")}
        self.journal.update(offset=s["offset"], prefix_crc=s["crc"])
        s["journaled"] = s["offset"]

    def _discard_journaled(self) -> None:
        j = self.journal
        if j is None:
            return
        if j["kind"] == KIND_ASSET:
            self.files.pop(self._temp_name(j), None)
        else:
            self.fw_written = 0
        self.journal = None

    def _stored(self, s: dict) -> bytes:
        if s["kind"] == KIND_ASSET:
            return bytes(self.files.get(self._temp_name(s), b""))[:s["size"]]
        return bytes(self.fw[:s["size"]])

    def _asset_check(self, s: dict, data: bytes) -> str:
        """Container header (asset-formats.md) and the stable per-format rules."""
        if len(data) < HEADER_LEN:
            return "IMAGE_INVALID"
        magic, hver, atype, fmt, flags, w, h, plen = struct.unpack_from("<4sBBBBHHI", data)
        entry = next((a for a in self.b["asset_support"]
                      if a["asset_type"] == s["asset_type"] and a["format"] == fmt), None)
        if (magic != b"ESPA" or hver != 1 or atype != s["asset_type"] or flags != 0 or entry is None
                or plen != len(data) - HEADER_LEN):
            return "IMAGE_INVALID"
        # images: the size the asset_support entry advertises (app_icon), otherwise the display size
        fixed = "width" in entry or "height" in entry
        disp = self.b["display"]
        size = (entry.get("width", 0), entry.get("height", 0)) if fixed else (disp["width"], disp["height"])
        if fmt == FORMAT_MONO1 and ((w, h) != size or plen != (w + 7) // 8 * h):
            return "IMAGE_INVALID"
        if fmt == FORMAT_RGB565 and ((w, h) != size or plen != 2 * w * h):
            return "IMAGE_INVALID"
        if fmt == FORMAT_RGB565A8 and ((w, h) != size or plen != 3 * w * h):
            return "IMAGE_INVALID"
        if fmt == FORMAT_FONT_V1 and (w != 0 or h != 0):
            return "IMAGE_INVALID"
        return "OK"

    def _rsp(self, req: dict, fields: dict) -> dict:
        name = req["type"] + "_RSP"
        return {"ver": self.reg.proto_major, "type": name, "req_id": req["req_id"], "fields": fields}

    def _ack(self, handle: int, offset: int, status: str) -> dict:
        return {"ver": self.reg.proto_major, "type": "XFER_ACK", "req_id": 0,
                "fields": {"handle": handle, "offset": offset, "status": self.reg.status(status)}}

    def _st(self, req: dict, status: str) -> dict:
        return self._rsp(req, {"status": self.reg.status(status)})

    # ---- external events

    def disconnect(self) -> None:
        s = self.session
        if s and s["state"] == ST_RECEIVING:
            self._journal_now()
        self.session = None

    def reboot(self) -> None:
        self._boot()

    def set_battery(self, battery: dict | None) -> None:
        self.b["firmware"]["battery"] = battery

    # ---- requests

    def handle(self, req: dict) -> list[dict]:
        """Process one request (logical form, already valid). Returns the messages the board sends."""
        f = req["fields"]
        t = req["type"]
        if t == "XFER_BEGIN":
            return [self._begin(req, f)]
        if t == "XFER_CHUNK":
            return self._chunk(f)
        if t == "XFER_FINISH":
            return [self._finish(req, f)]
        if t == "XFER_STATUS":
            s = self.session
            if not s or s["handle"] != f["handle"]:
                return [self._st(req, "NO_SESSION")]
            return [self._rsp(req, {"status": 0, "state": s["state"], "offset": s["offset"], "prefix_crc32": s["crc"]})]
        if t == "XFER_ABORT":
            s = self.session
            if not s or s["handle"] != f["handle"]:
                return [self._st(req, "NO_SESSION")]
            if s["state"] == ST_RECEIVING:
                self._discard_journaled()
            self.session = None
            return [self._st(req, "OK")]
        if t == "STORAGE_INFO":
            return [self._rsp(req, {"status": 0, "total": self.b["storage_total"], "free": self._free(),
                                    "asset_count": len(self._assets())})]
        if t == "ASSET_LIST":
            return [self._asset_list(req, f)]
        if t == "ASSET_DELETE":
            name = self._final_name(f)
            if name not in self.files:
                return [self._st(req, "NOT_FOUND")]
            del self.files[name]
            self.files.pop(name + "@", None)
            return [self._st(req, "OK")]
        raise ValueError(f"not a transfer request: {t}")

    def _assets(self) -> list[str]:
        return sorted(n for n in self.files if not n.endswith(("~", "@")))

    def _asset_list(self, req: dict, f: dict) -> dict:
        names = self._assets()
        off, limit = f.get("offset", 0), f.get("limit", LIST_DEFAULT_LIMIT)
        fields: dict[str, Any] = {"status": 0, "total": len(names)}
        entries: list[dict] = []
        for name in names[off:off + limit]:
            atype, aid = name.split("_", 1)
            e = {"asset_type": int(atype), "id": aid, "size": len(self.files[name])}
            if name + "@" in self.files:
                ver, _, origin = self.files[name + "@"].partition(b"\0")  # the version file: version [NUL origin]
                if ver:
                    e["version"] = ver.decode()
                if origin:
                    e["origin"] = origin[0]
            trial = {**fields, "asset": entries + [e]}
            if len(codec.encode_message(self.reg, self._rsp(req, trial))) > self.reg.limits["max_message_transfer"]:
                break
            entries.append(e)
        if entries:
            fields["asset"] = entries
        return self._rsp(req, fields)

    def _budget_ok(self, f: dict) -> bool:
        """The local limits of an asset type (`budget` bytes in all, `max_count` assets); the asset being replaced
        does not count."""
        entry = next((a for a in self.b["asset_support"] if a["asset_type"] == f["asset_type"]
                      and (a.get("budget") or a.get("max_count"))), None)
        if entry is None:
            return True
        mine = self._final_name(f)
        others = [n for n in self._assets() if n.startswith(f"{f['asset_type']}_") and n != mine]
        if entry.get("max_count") and mine not in self.files and len(others) >= entry["max_count"]:
            return False
        return not entry.get("budget") or sum(len(self.files[n]) for n in others) + f["size"] <= entry["budget"]

    def _begin(self, req: dict, f: dict) -> dict:
        s = self.session
        kind = f["kind"]
        if s and s["state"] == ST_RECEIVING and self._target(s) != self._target(f):
            return self._st(req, "BUSY")
        fw = self.b.get("firmware")
        if kind == KIND_ASSET and self.b["asset_support"]:
            if "asset_type" not in f:
                return self._st(req, "MISSING_FIELD")
            limit = self._max_size(f["asset_type"])
            if limit is None:
                return self._st(req, "UNSUPPORTED")
        elif kind == KIND_FIRMWARE and fw is not None:
            if f["id"] != "firmware":
                return self._st(req, "INVALID_ID")
            limit = fw["slot_size"]
        else:
            return self._st(req, "UNSUPPORTED")
        if f["size"] > limit:
            return self._st(req, "TOO_LARGE")
        if kind == KIND_ASSET:
            j = self.journal
            held = len(self.files.get(self._temp_name(j), b"")) if j and j["kind"] == KIND_ASSET else 0
            if f["size"] > self._free() + held:
                return self._st(req, "NO_SPACE")
            if not self._budget_ok(f):
                return self._st(req, "NO_SPACE")
        else:
            new = parse_version(f.get("version"))
            cur = parse_version(fw["running_version"])
            if new is None or (not fw["allow_downgrade"] and compare_versions(new, cur) <= 0):
                return self._st(req, "VERSION_REJECTED")
            if not self._battery_ok():
                return self._st(req, "LOW_BATTERY")

        # accepted: detach the current session as a disconnect would, then resume or start fresh
        self.disconnect()
        t = {"kind": kind, "asset_type": f.get("asset_type", 0), "id": f["id"], "size": f["size"],
             "crc32": f["crc32"], "sha256": f["sha256"], "version": f.get("version"),
             "origin": f.get("origin") if kind == KIND_ASSET else None}  # not journaled: the BEGIN that finishes wins
        j = self.journal
        same = j is not None and all(j[k] == t[k] for k in ("kind", "asset_type", "id", "size", "crc32", "sha256"))
        if same and not f.get("restart", False):
            if kind == KIND_ASSET:
                present = j["offset"] == 0 or len(self.files.get(self._temp_name(j), b"")) >= j["offset"]
            else:
                present = self.fw_written >= j["offset"]
            same = present
        else:
            same = False
        if same:
            offset, crc = j["offset"], j["prefix_crc"]
        else:
            self._discard_journaled()
            if kind == KIND_ASSET:
                self.files.pop(self._temp_name(t), None)
            else:
                self.fw_written = 0
            offset, crc = 0, 0
        handle = self.next_handle
        self.next_handle = self.next_handle % 255 + 1
        self.session = {**t, "handle": handle, "offset": offset, "crc": crc, "last_ack": offset,
                        "state": ST_RECEIVING}
        self._journal_now()
        return self._rsp(req, {"status": 0, "handle": handle, "offset": offset, "prefix_crc32": crc,
                               "max_chunk": self.b["max_chunk"], "ack_window": self.b["ack_window"]})

    def _chunk(self, f: dict) -> list[dict]:
        s = self.session
        if not s or s["handle"] != f["handle"] or s["state"] != ST_RECEIVING:
            return [self._ack(f["handle"], 0, "NO_SESSION")]
        data = bytes.fromhex(f["data"])
        if f["offset"] != s["offset"]:
            return [self._ack(s["handle"], s["offset"], "BAD_OFFSET")]
        if f["offset"] + len(data) > s["size"]:
            return [self._ack(s["handle"], s["offset"], "TOO_LARGE")]
        if s["kind"] == KIND_ASSET:
            st = self._blob_write(self._temp_name(s), f["offset"], data)
            if st != "OK":
                return [self._ack(s["handle"], s["offset"], st)]
        else:
            if len(self.fw) < f["offset"] + len(data):
                self.fw.extend(bytes(f["offset"] + len(data) - len(self.fw)))
            self.fw[f["offset"]:f["offset"] + len(data)] = data
            self.fw_written = max(self.fw_written, f["offset"] + len(data))
        s["offset"] += len(data)
        s["crc"] = zlib.crc32(data, s["crc"]) & 0xFFFFFFFF
        out = []
        if s["offset"] - s["journaled"] >= self.b["journal_every"] or s["offset"] == s["size"]:
            self._journal_now()
        if s["offset"] - s["last_ack"] >= self.b["ack_window"] or s["offset"] == s["size"]:
            s["last_ack"] = s["offset"]
            out.append(self._ack(s["handle"], s["offset"], "OK"))
        return out

    def _finish(self, req: dict, f: dict) -> dict:
        s = self.session
        if not s or s["handle"] != f["handle"] or s["state"] != ST_RECEIVING:
            return self._st(req, "NO_SESSION")
        if s["offset"] < s["size"]:
            return self._st(req, "BAD_OFFSET")
        if s["kind"] == KIND_FIRMWARE and not self._battery_ok():
            return self._st(req, "LOW_BATTERY")
        data = self._stored(s)
        if crc32(data) != s["crc32"]:
            status = "CRC_MISMATCH"
        elif hashlib.sha256(data).hexdigest() != s["sha256"]:
            status = "HASH_MISMATCH"
        elif s["kind"] == KIND_ASSET:
            status = self._asset_check(s, data)
        else:
            status = self.b["firmware"].get("image_status", "OK")
        if status != "OK":
            self._discard_journaled()
            s["state"] = ST_FAILED
            return self._st(req, status)
        if s["kind"] == KIND_ASSET:
            final = self._final_name(s)
            self.files.pop(final + "@", None)
            self.files[final] = self.files.pop(self._temp_name(s))
            meta = (s["version"] or "").encode()
            if s.get("origin") is not None:
                meta += bytes([0, s["origin"]])
            if meta:
                self._blob_write(final + "@", 0, meta)  # best effort: no space = no version
        self.journal = None
        s["state"] = ST_COMPLETE
        return self._st(req, "OK")


def container(asset_type: int, fmt: int, payload: bytes, width: int = 0, height: int = 0, *,
              magic: bytes = b"ESPA", header_version: int = 1, flags: int = 0, payload_len: int | None = None) -> bytes:
    """Build an asset file with the asset-formats.md container header."""
    plen = len(payload) if payload_len is None else payload_len
    return struct.pack("<4sBBBBHHI", magic, header_version, asset_type, fmt, flags, width, height, plen) + payload
