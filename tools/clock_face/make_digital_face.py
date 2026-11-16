"""Write components/ui_common/faces/digital/face.json, the built-in face "Digital" (a digital watch's display).

The face has two styles and every inked element exists once per style, so the source is written by this script
rather than by hand:
    python tools/clock_face/make_digital_face.py
Then tools/clock_face/gen_builtin.py compiles it with the other built-in faces.
"""
from __future__ import annotations

import json
from pathlib import Path

from PIL import Image

FACE = Path(__file__).resolve().parents[2] / "components/ui_common/faces/digital"
INFO = {"option": 1, "equals": 1}  # the bottom row: the room on the Paper, the weather on the LCD (it has no sensor)


def styled(els: list[dict], e: dict, inks: tuple[str, str], *conds: dict) -> None:
    """The element once per style (option 2: 0 = light, 1 = dark), in that style's ink."""
    for style, ink in enumerate(inks):
        els.append({**e, "colour": ink, "visible_if": [{"option": 2, "equals": style}, *conds]})


def paper() -> dict:
    els: list[dict] = [{"kind": "rect", "x": 0, "y": 0, "w": 200, "h": 200, "fill": True, "colour": "white"}]
    win = {"kind": "rect", "x": 4, "y": 4, "w": 192, "h": 168, "radius": 8, "colour": "black"}
    els.append({**win, "thickness": 3, "visible_if": [{"option": 2, "equals": 0}]})
    els.append({**win, "fill": True, "visible_if": [{"option": 2, "equals": 1}]})
    ink = ("black", "white")
    styled(els, {"kind": "text", "x": 14, "y": 12, "w": 52, "h": 20, "font": "small_bold", "source": "weekday"}, ink)
    styled(els, {"kind": "text", "x": 70, "y": 12, "w": 36, "h": 20, "font": "small_bold", "source": "ampm"}, ink)
    styled(els, {"kind": "number", "x": 120, "y": 11, "w": 66, "align": "right", "set": "small", "source": "day", "spacing": 2}, ink)
    styled(els, {"kind": "line", "x": 12, "y": 44, "w": 176, "h": 0, "thickness": 2}, ink)
    styled(els, {"kind": "number", "x": 8, "y": 53, "w": 184, "align": "center", "set": "seg", "source": "time", "spacing": 2}, ink)
    styled(els, {"kind": "line", "x": 12, "y": 130, "w": 176, "h": 0, "thickness": 2}, ink)
    styled(els, {"kind": "text", "x": 14, "y": 137, "w": 100, "h": 30, "font": "large_bold", "source": "room_temp"}, ink, INFO)
    styled(els, {"kind": "text", "x": 116, "y": 137, "w": 70, "h": 30, "font": "large_bold", "align": "right",
                 "source": "room_humidity"}, ink, INFO)
    els += [
        {"kind": "text", "x": 40, "y": 177, "w": 44, "h": 22, "font": "small_bold", "literal": "UPD",
         "visible_if": [{"above_zero": "update_available"}]},
        {"kind": "icon", "x": 94, "y": 180, "w": 16, "h": 16, "icon": "wifi"},
        {"kind": "icon", "x": 112, "y": 180, "w": 16, "h": 16, "icon": "ble"},
        {"kind": "icon", "x": 130, "y": 180, "w": 14, "h": 16, "icon": "charging"},
        {"kind": "text", "x": 146, "y": 177, "w": 48, "h": 22, "font": "small", "align": "right", "source": "battery_pct"},
    ]
    return {"images": [{"digit_set": "seg", "format": "mono1", "dir": "digits-paper"},
                       {"digit_set": "small", "format": "mono1", "dir": "small-paper"}], "elements": els}


def lcd() -> dict:
    page, grey = "#111827", "#9ca3af"
    glass, ink, ghost = ("#b7c2a8", "#0d130e"), ("#1a231b", "#c5d1b8"), ("#a7b399", "#161f18")
    els: list[dict] = [{"kind": "rect", "x": 0, "y": 0, "w": 296, "h": 240, "fill": True, "colour": page}]
    styled(els, {"kind": "rect", "x": 8, "y": 8, "w": 280, "h": 194, "radius": 12, "fill": True}, glass)
    # the unlit segments behind the time: an 8 in every digit cell and the colon, where the number element (right
    # aligned, so a one-digit hour leaves the first cell unlit) puts them
    digit_w = Image.open(FACE / "digits-lcd/8.png").width
    colon_w = Image.open(FACE / "digits-lcd/colon.png").width
    spacing, y = 4, 66
    x = left = (296 - (4 * digit_w + colon_w + 4 * spacing)) // 2
    for i in range(5):
        styled(els, {"kind": "image", "x": x, "y": y, "image": "colon" if i == 2 else "eight"}, ghost)
        x += (colon_w if i == 2 else digit_w) + spacing
    styled(els, {"kind": "text", "x": 24, "y": 18, "w": 130, "h": 26, "font": "small_bold", "source": "weekday", "format": "long"}, ink)
    styled(els, {"kind": "text", "x": 160, "y": 18, "w": 40, "h": 26, "font": "small_bold", "source": "ampm"}, ink)
    styled(els, {"kind": "number", "x": 200, "y": 17, "w": 72, "align": "right", "set": "small", "source": "day", "spacing": 3}, ink)
    styled(els, {"kind": "rect", "x": 20, "y": 54, "w": 256, "h": 2, "fill": True}, ink)
    styled(els, {"kind": "number", "x": left, "y": y, "w": x - spacing - left, "align": "right", "set": "seg", "source": "time",
                 "spacing": spacing}, ink)
    styled(els, {"kind": "rect", "x": 20, "y": 164, "w": 256, "h": 2, "fill": True}, ink)
    styled(els, {"kind": "text", "x": 24, "y": 171, "w": 80, "h": 28, "font": "large_bold", "source": "weather_temp"}, ink, INFO)
    styled(els, {"kind": "text", "x": 108, "y": 174, "w": 164, "h": 22, "font": "small", "align": "right", "source": "weather_place"},
           ink, INFO)
    els += [
        {"kind": "text", "x": 44, "y": 207, "w": 50, "h": 26, "font": "small_bold", "literal": "UPD", "colour": "#f59e0b",
         "visible_if": [{"above_zero": "update_available"}]},
        {"kind": "icon", "x": 180, "y": 212, "w": 16, "h": 16, "icon": "wifi", "colour": grey},
        {"kind": "icon", "x": 202, "y": 212, "w": 16, "h": 16, "icon": "ble", "colour": grey},
        {"kind": "icon", "x": 224, "y": 212, "w": 14, "h": 16, "icon": "charging", "colour": "#f59e0b"},
        {"kind": "text", "x": 240, "y": 208, "w": 48, "h": 24, "font": "small", "align": "right", "source": "battery_pct", "colour": grey},
    ]
    return {"images": [{"digit_set": "seg", "format": "mono1", "dir": "digits-lcd"},
                       {"digit_set": "small", "format": "mono1", "dir": "small-lcd"},
                       {"name": "eight", "format": "mono1", "file": "digits-lcd/8.png"},
                       {"name": "colon", "format": "mono1", "file": "digits-lcd/colon.png"}], "elements": els}


def render() -> str:
    face = {
        "meta": {"id": "epager.digital", "name": "Digital", "name_vi": "Điện tử", "author": "ePager", "version": "1.1.0",
                 "min_contract": "0.15.0", "update_anchor": "top_right"},
        "options": [
            {"id": 1, "type": "toggle", "label": "Info row", "label_vi": "Dòng thông tin", "default": True},
            {"id": 2, "type": "choice", "label": "Display", "label_vi": "Màn hình", "default": 0,
             "choices": [{"label": "Light", "label_vi": "Sáng"}, {"label": "Dark", "label_vi": "Tối"}]},
        ],
        "boards": {"paper": paper(), "lcd": lcd()},
    }
    text = json.dumps(face, ensure_ascii=False, indent=2)
    for board in ("paper", "lcd"):  # one element per line
        els = ",\n        ".join(json.dumps(e, ensure_ascii=False) for e in face["boards"][board]["elements"])
        start = text.index('"elements": [', text.index(f'"{board}": {{'))
        end = text.index("\n      ]", start)
        text = text[:start] + '"elements": [\n        ' + els + text[end:]
    return text + "\n"


if __name__ == "__main__":
    (FACE / "face.json").write_text(render())
    print("wrote", FACE / "face.json")
