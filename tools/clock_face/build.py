"""Compile a clock face source (face.json and PNG files) into a clock_face_v1 file.

The source format is a tool convention (it can change without a contract bump); the file format is the contract
(contracts/asset-formats.md). One source describes every board: `meta` and `options` are shared, each entry of
`boards` has its own images and elements (the screens differ).

    {"meta": {"id": "me.big", "name": "Big", "version": "1.0.0"},
     "options": [{"id": 1, "type": "toggle", "label": "Weather", "default": true},
                 {"id": 2, "type": "colour", "label": "Accent", "choices": [{"label": "Red", "colour": "#e11d48"}]}],
     "boards": {"paper": {"images": [{"digit_set": "big", "format": "mono1", "dir": "digits-paper"},
                                     {"name": "logo", "file": "logo.png", "format": "mono1"}],
                          "elements": [{"kind": "number", "x": 10, "y": 20, "set": "big", "source": "time"}]}}}

Digit set files (in `dir`): 0.png ... 9.png, colon.png, am.png, pm.png, minus.png, degree.png, percent.png (the last
five optional). Elements use the registry's field names; enum values by name, colours as "#rrggbb", "black", "white" or
a number (RGB565), `image` by image name, `set` by digit set name, `visible_if` as
{"option": id, "equals": v} | {"has": source} | {"above_zero": source}, each with an optional "not": true.
"""
from __future__ import annotations

import json
import struct
from pathlib import Path
from typing import Any

from PIL import Image

from epager_proto import clockface as CF

ENUM = CF.ENUM
SLOT_NAMES = [str(d) for d in range(10)] + ["colon", "am", "pm", "minus", "degree", "percent"]
ENUM_FIELDS = {"kind": "face_element", "align": "face_align", "source": "face_source", "format": "face_format",
               "font": "face_font", "icon": "face_icon"}
NAMED_COLOURS = {"black": 0x0000, "white": 0xFFFF}


class SourceError(Exception):
    pass


def rgb565(r: int, g: int, b: int) -> int:
    return (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3


def colour(v: Any) -> int:
    if isinstance(v, int):
        if not 0 <= v <= 0xFFFF:
            raise SourceError(f"colour {v} is not an RGB565 value")
        return v
    if v in NAMED_COLOURS:
        return NAMED_COLOURS[v]
    if isinstance(v, str) and len(v) == 7 and v[0] == "#":
        try:
            return rgb565(int(v[1:3], 16), int(v[3:5], 16), int(v[5:7], 16))
        except ValueError:
            pass
    raise SourceError(f"colour {v!r}: use #rrggbb, black, white or an RGB565 number")


def enum(kind: str, v: Any) -> int:
    if isinstance(v, int):
        return v
    try:
        return ENUM[kind][v]
    except KeyError:
        raise SourceError(f"{v!r} is not a {kind} (one of {', '.join(ENUM[kind])})") from None


# --------------------------------------------------------------------------- pixels

def load_png(path: Path, background: tuple[int, int, int] | None) -> Image.Image:
    try:
        im = Image.open(path)
        im.load()
    except OSError as e:
        raise SourceError(f"{path}: {e}") from None
    return im.convert("RGBA")


def pack_mono1(im: Image.Image) -> bytes:
    """Black pixels (luma < 128, transparent counts as white) are ink: bit 0; rows padded with 0 bits."""
    flat = Image.new("RGBA", im.size, (255, 255, 255, 255))
    flat.alpha_composite(im)
    return flat.convert("L").point(lambda v: 255 if v >= 128 else 0).convert("1", dither=Image.Dither.NONE).tobytes()


def pack_rgb565(im: Image.Image) -> bytes:
    flat = Image.new("RGBA", im.size, (0, 0, 0, 255))
    flat.alpha_composite(im)
    return b"".join(struct.pack("<H", rgb565(r, g, b)) for r, g, b, _ in flat.getdata())


def pack_rgb565a8(im: Image.Image) -> bytes:
    px = list(im.getdata())
    return b"".join(struct.pack("<H", rgb565(r, g, b) if a else 0) for r, g, b, a in px) + bytes(a for *_, a in px)


PACK = {"mono1": (CF._FMT["mono1"], pack_mono1), "rgb565": (CF._FMT["rgb565"], pack_rgb565),
        "rgb565a8": (CF._FMT["rgb565a8"], pack_rgb565a8)}


# --------------------------------------------------------------------------- source -> description

def _images(board: dict, root: Path) -> tuple[list[dict], list[bytes], dict[str, int], dict[str, int]]:
    imgs: list[dict] = []
    pix: list[bytes] = []
    by_name: dict[str, int] = {}
    sets: dict[str, int] = {}

    def add(path: Path, fmt_name: str, extra: dict) -> int:
        if fmt_name not in PACK:
            raise SourceError(f"image format {fmt_name!r}: use mono1, rgb565 or rgb565a8")
        fmt, pack = PACK[fmt_name]
        im = load_png(path, None)
        imgs.append({"format": fmt, "width": im.width, "height": im.height, **extra})
        pix.append(pack(im))
        return len(imgs) - 1

    for spec in board.get("images", []):
        fmt_name = spec.get("format", "mono1")
        if "digit_set" in spec:
            name = spec["digit_set"]
            if name in sets:
                raise SourceError(f"digit set {name!r} twice")
            sets[name] = len(sets) + 1
            folder = root / spec.get("dir", name)
            for slot, slot_name in enumerate(SLOT_NAMES):
                f = folder / f"{slot_name}.png"
                if f.exists():
                    add(f, fmt_name, {"set": sets[name], "slot": slot})
                elif slot < 10:
                    raise SourceError(f"digit set {name!r}: {f} is missing")
        else:
            name = spec["name"]
            if name in by_name:
                raise SourceError(f"image {name!r} twice")
            by_name[name] = add(root / spec["file"], fmt_name, {})
    return imgs, pix, by_name, sets


def _cond(c: dict) -> dict:
    out: dict[str, Any]
    if "option" in c:
        out = {"kind": ENUM["face_cond"]["option_eq"], "option": c["option"], "value": int(c["equals"])}
    elif "has" in c:
        out = {"kind": ENUM["face_cond"]["source_has"], "source": enum("face_source", c["has"])}
    elif "above_zero" in c:
        out = {"kind": ENUM["face_cond"]["source_gt0"], "source": enum("face_source", c["above_zero"])}
    else:
        raise SourceError(f"visible_if {c}: use option+equals, has or above_zero")
    if c.get("not"):
        out["negate"] = True
    return out


def _element(e: dict, by_name: dict[str, int], sets: dict[str, int]) -> dict:
    out: dict[str, Any] = {}
    for k, v in e.items():
        if k in ENUM_FIELDS:
            out[k] = enum(ENUM_FIELDS[k], v)
        elif k == "colour":
            out[k] = colour(v)
        elif k == "image":
            if v not in by_name:
                raise SourceError(f"element names image {v!r}, which is not in the image list")
            out[k] = by_name[v]
        elif k == "set":
            if v not in sets:
                raise SourceError(f"element names digit set {v!r}, which is not in the image list")
            out[k] = sets[v]
        elif k == "visible_if":
            out[k] = [_cond(c) for c in v]
        else:
            out[k] = v
    return out


def _options(options: list[dict]) -> list[dict]:
    out = []
    for o in options:
        t = enum("face_option_type", o["type"])
        d: dict[str, Any] = {"id": o["id"], "type": t, "label": o["label"], "default": int(o.get("default", 0))}
        if "label_vi" in o:
            d["label_vi"] = o["label_vi"]
        choices = []
        for c in o.get("choices", []):
            ch: dict[str, Any] = {"label": c["label"]}
            if "label_vi" in c:
                ch["label_vi"] = c["label_vi"]
            if "colour" in c:
                ch["colour"] = colour(c["colour"])
            choices.append(ch)
        if choices:
            d["choice"] = choices
        out.append(d)
    return out


def build(source: dict, root: Path, board: str) -> bytes:
    """The file for one board from the parsed face.json; `root` is the folder the image paths are relative to."""
    if board not in CF.LIMITS:
        raise SourceError(f"board {board!r}: use paper or lcd")
    spec = source.get("boards", {}).get(board)
    if spec is None:
        raise SourceError(f"the face has no entry for the board {board!r}")
    lim = CF.LIMITS[board]
    imgs, pix, by_name, sets = _images(spec, root)
    meta = dict(source["meta"])
    if "update_anchor" in meta:
        meta["update_anchor"] = enum("face_anchor", meta["update_anchor"])
    desc: dict[str, Any] = {"meta": meta, "image": imgs, "option": _options(source.get("options", [])),
                            "element": [_element(e, by_name, sets) for e in spec.get("elements", [])]}
    data = CF.build_face(desc, pix, lim.width, lim.height)
    CF.parse_face(data, lim, meta["id"])  # a face this tool writes must pass what the board checks
    return data


def build_file(face_dir: Path, board: str) -> bytes:
    src = json.loads((face_dir / "face.json").read_text())
    return build(src, face_dir, board)
