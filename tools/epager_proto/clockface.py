"""Reference implementation of clock faces (contracts/asset-formats.md, "Clock faces").

build_face   a description (the logical form of the registry struct `face_root`) plus the image pixels -> a file
parse_face   a file -> a Face, refusing exactly what the board refuses (IMAGE_INVALID)
resolve_opts stored (id, value) pairs -> one value per option of the face
evaluate     a Face, its option values and a data snapshot -> what every element draws

The C component components/clock_face must give the same answers for every case of
contracts/test-vectors/clock_faces.json. The tools (tools/clock_face) build faces with build_face.
"""
from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field
from typing import Any

from . import codec, transfer
from . import registry as R

ASSET_TYPE_CLOCK_FACE, FORMAT_CLOCK_FACE_V1 = 6, 7
HEADER_LEN = 16
MAX_IMAGES, MAX_OPTIONS, MAX_CHOICES, MAX_ELEMENTS, MAX_SETS, MAX_CONDS = 32, 8, 8, 48, 8, 2
MAX_OPT_PAIRS = 16
GLYPH_MAX = 16
SLOT_COLON, SLOT_AM, SLOT_PM, SLOT_MINUS, SLOT_DEGREE, SLOT_PERCENT = 10, 11, 12, 13, 14, 15
CONTRACT = (0, 15, 0)

reg = R.load()
ENUM = {name: {v["name"]: v["value"] for v in e["values"]} for name, e in reg.enums.items()}
_E = ENUM["face_element"]
_S = ENUM["face_source"]
_F = ENUM["face_format"]
_FMT = ENUM["asset_format"]


class FaceError(Exception):
    """The board answers IMAGE_INVALID (or TOO_LARGE for a file above max_size); `reason` is for people."""

    def __init__(self, reason: str, status: str = "IMAGE_INVALID"):
        super().__init__(reason)
        self.reason = reason
        self.status = status


@dataclass(frozen=True)
class Limits:
    width: int
    height: int
    max_file: int
    formats: frozenset[int]
    arcs: bool


PAPER = Limits(200, 200, 65536, frozenset({_FMT["mono1"]}), False)
LCD = Limits(296, 240, 1 << 20, frozenset({_FMT["mono1"], _FMT["rgb565"], _FMT["rgb565a8"]}), True)
LIMITS = {"paper": PAPER, "lcd": LCD}


def image_size(fmt: int, w: int, h: int) -> int:
    return {_FMT["mono1"]: (w + 7) // 8 * h, _FMT["rgb565"]: 2 * w * h, _FMT["rgb565a8"]: 3 * w * h}[fmt]


# --------------------------------------------------------------------------- build

def build_face(desc: dict, pixels: list[bytes], width: int, height: int) -> bytes:
    """A face file. `desc` is the logical form of face_root (names as in the registry; enum values as numbers);
    `pixels` the packed pixels of desc["image"] in order."""
    tlv = codec.encode_fields(reg, reg.structs["face_root"], desc)
    pad = -(20 + len(tlv)) % 4
    payload = struct.pack("<I", len(tlv)) + tlv + bytes(pad) + b"".join(pixels)
    return transfer.container(ASSET_TYPE_CLOCK_FACE, FORMAT_CLOCK_FACE_V1, payload, width, height)


# --------------------------------------------------------------------------- parse

@dataclass
class Face:
    width: int
    height: int
    meta: dict
    images: list[dict]            # each with "data": bytes
    options: list[dict]
    elements: list[dict]
    sets: dict[int, dict[int, int]] = field(default_factory=dict)   # set -> slot -> image index


def _fail(reason: str) -> FaceError:
    return FaceError(reason)


def _semver_tuple(v: str) -> tuple[int, int, int] | None:
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", v)
    return (int(m[1]), int(m[2]), int(m[3])) if m else None


def parse_face(file: bytes, limits: Limits, expected_id: str | None = None, contract: tuple[int, int, int] = CONTRACT) -> Face:
    if len(file) > limits.max_file:
        raise FaceError("file above max_size", "TOO_LARGE")
    if len(file) < HEADER_LEN + 4:
        raise _fail("short file")
    magic, hver, atype, fmt, flags, w, h, plen = struct.unpack("<4sBBBBHHI", file[:HEADER_LEN])
    if magic != b"ESPA" or hver != 1 or atype != ASSET_TYPE_CLOCK_FACE or fmt != FORMAT_CLOCK_FACE_V1 or flags != 0:
        raise _fail("bad container header")
    if plen != len(file) - HEADER_LEN:
        raise _fail("payload_len")
    if (w, h) != (limits.width, limits.height):
        raise _fail("screen size")
    tlv_len = struct.unpack("<I", file[HEADER_LEN:HEADER_LEN + 4])[0]
    tlv_end = 20 + tlv_len
    if tlv_end > len(file):
        raise _fail("tlv_len past the end")
    data_start = tlv_end + (-tlv_end % 4)
    if data_start > len(file) or any(file[tlv_end:data_start]):
        raise _fail("padding")
    try:
        root = codec.decode_fields(reg, reg.structs["face_root"], file[20:tlv_end])
    except codec.ProtoError as e:
        raise _fail(f"tlv: {e}") from None
    images = root.get("image", [])
    options = root.get("option", [])
    elements = root.get("element", [])
    meta = root["meta"]
    if len(images) > MAX_IMAGES or len(options) > MAX_OPTIONS or len(elements) > MAX_ELEMENTS:
        raise _fail("too many images, options or elements")
    if expected_id is not None and meta["id"] != expected_id:
        raise _fail("meta.id differs from the asset id")
    if "min_contract" in meta:
        need = _semver_tuple(meta["min_contract"])
        if need is None or need > contract:
            raise _fail("min_contract")
    face = Face(w, h, meta, images, options, elements)
    _check_images(face, file[data_start:], limits)
    _check_options(face)
    _check_elements(face, limits)
    return face


def _check_images(face: Face, data: bytes, limits: Limits) -> None:
    pos = 0
    for i, im in enumerate(face.images):
        if im["format"] not in limits.formats:
            raise _fail(f"image {i}: format")
        n = image_size(im["format"], im["width"], im["height"])
        im["data"] = data[pos:pos + n]
        im["offset"] = pos
        pos += n
        if ("set" in im) != ("slot" in im):
            raise _fail(f"image {i}: set and slot go together")
        if "set" in im:
            slots = face.sets.setdefault(im["set"], {})
            if im["slot"] in slots:
                raise _fail(f"image {i}: slot repeated in set {im['set']}")
            slots[im["slot"]] = i
    if pos != len(data):
        raise _fail("data area length")


def _check_options(face: Face) -> None:
    ids = set()
    for o in face.options:
        if o["id"] in ids:
            raise _fail(f"option {o['id']} repeated")
        ids.add(o["id"])
        n = len(o.get("choice", []))
        t = o["type"]
        d = o.get("default", 0)
        if t == ENUM["face_option_type"]["toggle"]:
            if n or d > 1:
                raise _fail(f"option {o['id']}: toggle")
        elif t in (ENUM["face_option_type"]["choice"], ENUM["face_option_type"]["colour"]):
            if not 1 <= n <= MAX_CHOICES or d >= n:
                raise _fail(f"option {o['id']}: choices")
            if t == ENUM["face_option_type"]["colour"] and any("colour" not in c for c in o["choice"]):
                raise _fail(f"option {o['id']}: colour choice without a colour")
        # an unknown option type is kept, never offered, and its value is its default


def _digit_set_ok(face: Face, set_id: int) -> int | None:
    """The digit height of a complete set, else None."""
    slots = face.sets.get(set_id)
    if not slots or any(d not in slots for d in range(10)):
        return None
    heights = {face.images[slots[d]]["height"] for d in range(10)}
    return heights.pop() if len(heights) == 1 else None


def _check_elements(face: Face, limits: Limits) -> None:
    W, H = face.width, face.height
    for i, e in enumerate(face.elements):
        if len(e.get("visible_if", [])) > MAX_CONDS:
            raise _fail(f"element {i}: more than {MAX_CONDS} conditions")
        kind = e["kind"]
        x, y, w, h = e.get("x", 0), e.get("y", 0), e.get("w", 0), e.get("h", 0)

        def box(bx, by, bw, bh):
            if bx < 0 or by < 0 or bx + bw > W or by + bh > H:
                raise _fail(f"element {i}: box leaves the screen")

        if kind == _E["rect"]:
            if w < 1 or h < 1:
                raise _fail(f"element {i}: rect size")
            box(x, y, w, h)
        elif kind == _E["line"]:
            if w == 0 and h == 0:
                raise _fail(f"element {i}: line of no length")
            box(x, y, w, h)
        elif kind == _E["arc"]:
            if not limits.arcs:
                raise _fail(f"element {i}: arcs not supported")
            if w < 8 or w != h or e.get("source", 0) == 0:
                raise _fail(f"element {i}: arc")
            box(x, y, w, h)
        elif kind == _E["image"]:
            idx = e.get("image", 0)
            if idx >= len(face.images):
                raise _fail(f"element {i}: image index")
            box(x, y, face.images[idx]["width"], face.images[idx]["height"])
        elif kind == _E["number"]:
            dh = _digit_set_ok(face, e.get("set", 0))
            if dh is None or e.get("source", 0) == 0:
                raise _fail(f"element {i}: number needs a complete digit set and a source")
            box(x, y, w, dh)
        elif kind == _E["text"]:
            if e.get("font", 0) == 0 or w < 1 or h < 1 or (e.get("source", 0) == 0 and not e.get("literal")):
                raise _fail(f"element {i}: text")
            box(x, y, w, h)
        elif kind == _E["icon"]:
            if e.get("icon", 0) == 0 or w < 1 or h < 1:
                raise _fail(f"element {i}: icon")
            box(x, y, w, h)
        # an unknown kind is skipped


# --------------------------------------------------------------------------- options

def option_default(o: dict) -> int:
    return o.get("default", 0)


def option_range(o: dict) -> int:
    """Number of values of an option (0 for an unknown type: only the default is valid)."""
    t = o["type"]
    if t == ENUM["face_option_type"]["toggle"]:
        return 2
    if t in (ENUM["face_option_type"]["choice"], ENUM["face_option_type"]["colour"]):
        return len(o["choice"])
    return 0


def resolve_opts(face: Face, pairs: bytes) -> list[int]:
    """One value per option, in table order. Of repeated ids the last pair wins."""
    stored: dict[int, int] = {}
    for i in range(0, len(pairs) - len(pairs) % 2, 2):
        stored[pairs[i]] = pairs[i + 1]
    out = []
    for o in face.options:
        v = stored.get(o["id"])
        out.append(v if v is not None and v < option_range(o) else option_default(o))
    return out


def pack_opts(face: Face, values: list[int]) -> bytes:
    """Pairs for the options that differ from their default, in table order."""
    out = bytearray()
    for o, v in zip(face.options, values):
        if v != option_default(o):
            out += bytes([o["id"], v])
    return bytes(out)


# --------------------------------------------------------------------------- evaluate

WDAY_SHORT = {"en": ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"], "vi": ["CN", "T2", "T3", "T4", "T5", "T6", "T7"]}
WDAY_LONG = {"en": ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"],
             "vi": ["Chủ nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy"]}
MONTH_SHORT = {"en": ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"],
               "vi": [f"T{m}" for m in range(1, 13)]}
MONTH_LONG = {"en": ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
                     "November", "December"], "vi": [f"Tháng {m}" for m in range(1, 13)]}

# Numeric sources and their kinds: the value an arc fills, a condition tests
_NUMERIC = {"hour", "minute", "day", "month", "year", "weather_temp", "weather_high", "weather_low", "weather_rain",
            "room_temp", "room_humidity", "battery_pct", "notif_count"}
_BOOL = {"charging", "ble_connected", "wifi_connected", "update_available"}
_SOURCE_NAME = {v: k for k, v in _S.items()}


def _cdiv(a: int, b: int) -> int:
    """C integer division (truncates toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def _round10(v: int) -> int:
    return _cdiv(v + 5 if v >= 0 else v - 5, 10)


def _shown_c10(c10: int, fahrenheit: bool) -> int:
    """Whole degrees of the shown unit from tenths of a degree Celsius."""
    if fahrenheit:
        c10 = _cdiv(c10 * 9, 5) + 320
    return _round10(c10)


def _unit(d: dict) -> str:
    return "°F" if d.get("fahrenheit") else "°C"


def source_text(src: str, fmt: int, d: dict) -> tuple[str | None, int | None]:
    """(text, number) of a source for a data snapshot; text None = no value. `number` is the value an arc
    fills or a condition tests (None for text-only and for missing values)."""
    lang = d.get("language", "en")
    known = d.get("time_known", False)
    pad2 = fmt == _F["pad2"]
    plain = fmt == _F["plain"]
    unit = fmt != _F["nounit"]
    wx = d.get("wx", {})
    room = d.get("room")
    if src == "hour":
        if not known:
            return "--", None
        h = d["hour"]
        if not d.get("clock_24h", True):
            h = h % 12 or 12
            p = pad2
        else:
            p = not plain
        return (f"{h:02d}" if p else str(h)), h
    if src == "minute":
        if not known:
            return "--", None
        return (str(d["minute"]) if plain else f"{d['minute']:02d}"), d["minute"]
    if src == "ampm":
        if not known or d.get("clock_24h", True):
            return None, None
        return ("AM" if d["hour"] < 12 else "PM"), None
    if src == "time":
        if not known:
            return "--:--", None
        h = d["hour"]
        if not d.get("clock_24h", True):
            h = h % 12 or 12
            return f"{h}:{d['minute']:02d}", None
        return f"{h:02d}:{d['minute']:02d}", None
    if not known and src in ("day", "month", "year", "weekday"):
        return None, None
    if src == "day":
        return (f"{d['day']:02d}" if pad2 else str(d["day"])), d["day"]
    if src == "month":
        m = d["month"]
        if fmt == _F["short"]:
            return MONTH_SHORT[lang][m - 1], m
        if fmt == _F["long"]:
            return MONTH_LONG[lang][m - 1], m
        return (f"{m:02d}" if pad2 else str(m)), m
    if src == "year":
        y = d["year"]
        return (f"{y % 100:02d}" if fmt == _F["year2"] else str(y)), y
    if src == "weekday":
        w = d["wday"]
        return (WDAY_LONG if fmt == _F["long"] else WDAY_SHORT)[lang][w], None
    if src == "date":
        return d.get("date", ""), None
    if src in ("weather_temp", "weather_high", "weather_low"):
        key = {"weather_temp": "temp_c10", "weather_high": "high_c10", "weather_low": "low_c10"}[src]
        if key not in wx:
            return None, None
        v = _shown_c10(wx[key], bool(d.get("fahrenheit")))
        return f"{v}{_unit(d) if unit else ''}", v
    if src == "weather_rain":
        if "rain" not in wx or wx["rain"] > 100:
            return None, None
        return f"{wx['rain']}%" if unit else str(wx["rain"]), wx["rain"]
    if src == "weather_condition":
        return None, wx["cond"] if "cond" in wx else None
    if src == "weather_place":
        return (wx.get("place") or None), None
    if src == "room_temp":
        if room is None:
            return None, None
        centi = room["temp_c100"]
        if d.get("fahrenheit"):
            centi = _cdiv(centi * 9, 5) + 3200
        t10 = _cdiv(centi + 5 if centi >= 0 else centi - 5, 10)
        a = abs(t10)
        return f"{'-' if t10 < 0 else ''}{a // 10}.{a % 10}{_unit(d) if unit else ''}", _cdiv(t10, 10)
    if src == "room_humidity":
        if room is None:
            return None, None
        rh = _cdiv(room["rh_c100"] + 50, 100)
        return f"{rh}%" if unit else str(rh), rh
    if src == "battery_pct":
        b = d.get("battery", 255)
        if b > 100:
            return None, None
        return f"{b}%" if unit else str(b), b
    if src == "notif_count":
        n = d.get("notif", 0)
        return str(n), n
    if src == "charging":
        return None, int(bool(d.get("charging")))
    if src == "ble_connected":
        return None, int(bool(d.get("ble")))
    if src == "wifi_connected":
        return None, int(bool(d.get("wifi")))
    if src == "update_available":
        return None, int(bool(d.get("update")))
    return None, None


def source_has_value(src: str, d: dict) -> bool:
    if src in _BOOL:
        return True
    if src == "weather_condition":
        return "cond" in d.get("wx", {})
    if src in ("hour", "minute", "time"):
        return bool(d.get("time_known"))
    t, n = source_text(src, 0, d)
    return t is not None or n is not None


def source_gt0(src: str, d: dict) -> bool:
    if src not in _NUMERIC and src not in _BOOL:
        return False
    _, n = source_text(src, 0, d)
    return n is not None and n > 0


def _cond_holds(face: Face, values: list[int], c: dict, d: dict) -> bool:
    kind = c["kind"]
    ok = False
    if kind == ENUM["face_cond"]["option_eq"]:
        for o, v in zip(face.options, values):
            if o["id"] == c.get("option", 0):
                ok = v == c.get("value", 0)
    elif kind == ENUM["face_cond"]["source_has"]:
        ok = source_has_value(_SOURCE_NAME.get(c.get("source", 0), "none"), d)
    elif kind == ENUM["face_cond"]["source_gt0"]:
        ok = source_gt0(_SOURCE_NAME.get(c.get("source", 0), "none"), d)
    return ok != bool(c.get("negate", False))


def _glyphs(text: str) -> list[int]:
    out: list[int] = []
    i = 0
    while i < len(text) and len(out) < GLYPH_MAX:
        ch = text[i]
        if text.startswith(("AM", "PM"), i):
            out.append(SLOT_AM if ch == "A" else SLOT_PM)
            i += 2
            continue
        if ch.isascii() and ch.isdigit():
            out.append(int(ch))
        elif ch == ":":
            out.append(SLOT_COLON)
        elif ch == "-":
            out.append(SLOT_MINUS)
        elif ch == "°":
            out.append(SLOT_DEGREE)
        elif ch == "%":
            out.append(SLOT_PERCENT)
        i += 1
    return out


def _colour(face: Face, values: list[int], e: dict) -> int:
    opt = e.get("colour_option")
    if opt is not None:
        for o, v in zip(face.options, values):
            if o["id"] == opt and o["type"] == ENUM["face_option_type"]["colour"]:
                return o["choice"][v]["colour"]
    return e.get("colour", 0)


def evaluate(face: Face, values: list[int], d: dict) -> list[dict]:
    """What each element draws: {"visible": False}, or visible with the colour and, by kind, "text" and "glyphs"
    (number), "text" (text), "value" (arc, icon) and "aux" (weather icon: 1 by day)."""
    out = []
    for e in face.elements:
        kind = e["kind"]
        if kind not in (_E["rect"], _E["line"], _E["arc"], _E["image"], _E["number"], _E["text"], _E["icon"]):
            out.append({"visible": False})
            continue
        if not all(_cond_holds(face, values, c, d) for c in e.get("visible_if", [])):
            out.append({"visible": False})
            continue
        r: dict[str, Any] = {"visible": True, "colour": _colour(face, values, e)}
        src_id = e.get("source", 0)
        src = _SOURCE_NAME.get(src_id)
        fmt = e.get("format", 0)
        if kind in (_E["number"], _E["text"]):
            if src_id == 0:
                text, _ = (e.get("literal") or None), None
            elif src is None:
                text = None  # a source this contract does not know
            else:
                text, _ = source_text(src, fmt, d)
            if not text or src in ("weather_condition",) or src in _BOOL:
                out.append({"visible": False})
                continue
            if kind == _E["text"] and e["font"] > 4:
                out.append({"visible": False})
                continue
            r["text"] = text
            if kind == _E["number"]:
                r["glyphs"] = _glyphs(text)
        elif kind == _E["arc"]:
            _, n = source_text(src, fmt, d) if src else (None, None)
            if n is None:
                out.append({"visible": False})
                continue
            r["value"] = max(0, min(100, n))
        elif kind == _E["icon"]:
            icon = e["icon"]
            names = ENUM["face_icon"]
            if icon == names["weather"]:
                wx = d.get("wx", {})
                if "cond" not in wx:
                    out.append({"visible": False})
                    continue
                r["value"] = wx["cond"]
                r["aux"] = int(bool(wx.get("day", True)))
            elif icon == names["ble"]:
                r["value"] = int(bool(d.get("ble")))
            elif icon == names["wifi"]:
                if not d.get("wifi"):
                    out.append({"visible": False})
                    continue
                r["value"] = 1
            elif icon == names["charging"]:
                if not d.get("charging"):
                    out.append({"visible": False})
                    continue
                r["value"] = 1
            else:
                out.append({"visible": False})
                continue
        out.append(r)
    return out


def draws_source(face: Face, name: str) -> bool:
    """True when an element reads the source `name` (a face that draws update_available gets no firmware mark)."""
    sid = _S[name]
    for e in face.elements:
        if e.get("source") == sid:
            return True
        if any(c.get("source") == sid for c in e.get("visible_if", [])):
            return True
    return False
