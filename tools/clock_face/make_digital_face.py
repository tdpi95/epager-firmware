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
INFO = {"option": 1, "equals": 1}  # the right bottom cell: the room on the Paper, the weather on the LCD (it has no sensor)

UPDATE = {"above_zero": "update_available"}


def status_icons(els: list[dict], slots: tuple[int, int, int], y: int, ink: str, bolt: str) -> None:
    """The Wi-Fi, charging and Bluetooth icons, left to right, each in a place of its own to the left of the percentage.
    Wi-Fi and the bolt draw nothing while there is nothing to show, so no layout depends on which of them are there."""
    for x, name, colour in zip(slots, ("wifi", "charging", "ble"), (ink, bolt, ink)):
        els.append({"kind": "icon", "x": x, "y": y, "w": 16, "h": 16, "icon": name, "colour": colour})


def styled(els: list[dict], e: dict, inks: tuple[str, str], *conds: dict) -> None:
    """The element once per style (option 2: 0 = light, 1 = dark), in that style's ink."""
    for style, ink in enumerate(inks):
        els.append({**e, "colour": ink, "visible_if": [{"option": 2, "equals": style}, *conds]})


def date_cell(els: list[dict], x: int, y: int, digits: str, spacing: int, ink: tuple[str, str]) -> None:
    """The plain date as a digital watch writes it, day-month (16-11): two digit pairs and a dash drawn as a bar."""
    digit = Image.open(FACE / digits / "0.png")
    digit_w, h = digit.width, digit.height
    pair = 2 * digit_w + spacing
    dash_w = digit_w // 2 + 2
    styled(els, {"kind": "number", "x": x, "y": y, "w": 0, "set": "small", "source": "day", "format": "pad2", "spacing": spacing}, ink)
    styled(els, {"kind": "rect", "x": x + pair + 5, "y": y + h // 2 - 2, "w": dash_w, "h": 4, "fill": True}, ink)
    styled(els, {"kind": "number", "x": x + pair + 10 + dash_w, "y": y, "w": 0, "set": "small", "source": "month", "format": "pad2",
                 "spacing": spacing}, ink)


def paper() -> dict:
    els: list[dict] = [{"kind": "rect", "x": 0, "y": 0, "w": 200, "h": 200, "fill": True, "colour": "white"}]
    win = {"kind": "rect", "x": 0, "y": 0, "w": 200, "h": 200, "colour": "black"}
    els.append({**win, "thickness": 3, "visible_if": [{"option": 2, "equals": 0}]})
    els.append({**win, "fill": True, "visible_if": [{"option": 2, "equals": 1}]})
    ink = ("black", "white")
    # the status bar on top: a black strip in both styles (white ink, so the icons need no per-style copies); in the dark
    # style a white line separates it from the black window
    els.append({"kind": "rect", "x": 0, "y": 0, "w": 200, "h": 26, "fill": True, "colour": "black"})
    els.append({"kind": "line", "x": 0, "y": 27, "w": 200, "h": 0, "thickness": 2, "colour": "white", "visible_if": [{"option": 2, "equals": 1}]})
    els.append({"kind": "text", "x": 8, "y": 3, "w": 44, "h": 21, "font": "small_bold", "literal": "UPD", "colour": "white", "visible_if": [UPDATE]})
    status_icons(els, (94, 112, 130), 5, "white", "white")
    els.append({"kind": "text", "x": 148, "y": 3, "w": 46, "h": 21, "font": "small", "source": "battery_pct", "colour": "white"})
    styled(els, {"kind": "number", "x": 8, "y": 36, "w": 184, "align": "center", "set": "seg", "source": "time", "spacing": 2}, ink)
    styled(els, {"kind": "line", "x": 10, "y": 112, "w": 180, "h": 0, "thickness": 2}, ink)
    styled(els, {"kind": "line", "x": 100, "y": 112, "w": 0, "h": 52, "thickness": 2}, ink)
    date_cell(els, 16, 125, "small-paper", 2, ink)
    styled(els, {"kind": "image", "x": 108, "y": 117, "image": "thermometer"}, ink, INFO)
    styled(els, {"kind": "text", "x": 124, "y": 119, "w": 70, "h": 22, "font": "small_bold", "source": "room_temp"}, ink, INFO)
    styled(els, {"kind": "image", "x": 107, "y": 141, "image": "drop"}, ink, INFO)
    styled(els, {"kind": "text", "x": 128, "y": 143, "w": 62, "h": 20, "font": "small_bold", "source": "room_humidity"}, ink, INFO)
    # the day and weather bar at the bottom, the same strip
    els.append({"kind": "line", "x": 0, "y": 166, "w": 200, "h": 0, "thickness": 2, "colour": "white", "visible_if": [{"option": 2, "equals": 1}]})
    els.append({"kind": "rect", "x": 0, "y": 168, "w": 200, "h": 32, "fill": True, "colour": "black"})
    els.append({"kind": "text", "x": 8, "y": 173, "w": 64, "h": 21, "font": "small_bold", "source": "weekday", "colour": "white"})
    els.append({"kind": "text", "x": 74, "y": 173, "w": 30, "h": 21, "font": "small_bold", "align": "center", "source": "ampm", "colour": "white"})
    els.append({"kind": "icon", "x": 118, "y": 168, "w": 32, "h": 32, "icon": "weather", "colour": "white"})
    els.append({"kind": "text", "x": 138, "y": 173, "w": 52, "h": 21, "font": "small_bold", "align": "right", "source": "weather_temp", "colour": "white"})
    return {"images": [{"digit_set": "seg", "format": "mono1", "dir": "digits-paper"},
                       {"digit_set": "small", "format": "mono1", "dir": "small-paper"},
                       {"name": "thermometer", "format": "mono1", "file": "icons/thermometer.png"},
                       {"name": "drop", "format": "mono1", "file": "icons/drop.png"}], "elements": els}


def lcd() -> dict:
    glass, ink, ghost = ("#b7c2a8", "#0d130e"), ("#1a231b", "#c5d1b8"), ("#a7b399", "#161f18")
    els: list[dict] = []
    styled(els, {"kind": "rect", "x": 0, "y": 0, "w": 296, "h": 240, "fill": True}, glass)
    # the unlit segments behind the time: one image of an 8 in every digit cell and the colon (made by
    # make_builtin_images.py), where the number element (right aligned, so a one-digit hour leaves the first cell unlit)
    # puts them
    digit_w = Image.open(FACE / "digits-lcd/8.png").width
    colon_w = Image.open(FACE / "digits-lcd/colon.png").width
    spacing, y = 4, 46
    left = (296 - (4 * digit_w + colon_w + 4 * spacing)) // 2
    width = 4 * digit_w + colon_w + 4 * spacing
    styled(els, {"kind": "image", "x": left, "y": y, "image": "ghost"}, ghost)
    # the status bar on top: a dark strip in both styles (light ink, so the icons need no per-style copies)
    bar, light, warn = "#1a231b", "#c5d1b8", "#f59e0b"
    els.append({"kind": "rect", "x": 0, "y": 0, "w": 296, "h": 34, "fill": True, "colour": bar})
    els.append({"kind": "text", "x": 12, "y": 5, "w": 50, "h": 24, "font": "small_bold", "literal": "UPD", "colour": warn, "visible_if": [UPDATE]})
    status_icons(els, (186, 204, 222), 9, light, warn)
    els.append({"kind": "text", "x": 240, "y": 5, "w": 48, "h": 24, "font": "small", "source": "battery_pct", "colour": light})
    styled(els, {"kind": "number", "x": left, "y": y, "w": width, "align": "right", "set": "seg", "source": "time",
                 "spacing": spacing}, ink)
    styled(els, {"kind": "rect", "x": 20, "y": 144, "w": 256, "h": 2, "fill": True}, ink)
    styled(els, {"kind": "rect", "x": 147, "y": 146, "w": 2, "h": 58, "fill": True}, ink)
    date_cell(els, 32, 160, "small-lcd", 3, ink)
    styled(els, {"kind": "text", "x": 154, "y": 162, "w": 54, "h": 28, "font": "small_bold", "align": "right", "source": "weather_high"}, ink, INFO)
    styled(els, {"kind": "text", "x": 208, "y": 162, "w": 12, "h": 28, "font": "small_bold", "align": "center", "literal": "/"}, ink, INFO)
    styled(els, {"kind": "text", "x": 222, "y": 162, "w": 56, "h": 28, "font": "small_bold", "source": "weather_low"}, ink, INFO)
    # the day and weather bar at the bottom, the same strip
    els.append({"kind": "rect", "x": 0, "y": 206, "w": 296, "h": 34, "fill": True, "colour": bar})
    els.append({"kind": "text", "x": 12, "y": 209, "w": 120, "h": 28, "font": "small_bold", "source": "weekday", "format": "long", "colour": light})
    els.append({"kind": "text", "x": 138, "y": 209, "w": 52, "h": 28, "font": "small_bold", "align": "center", "source": "ampm", "colour": light})
    els.append({"kind": "icon", "x": 204, "y": 207, "w": 32, "h": 32, "icon": "weather", "colour": light})
    els.append({"kind": "text", "x": 222, "y": 209, "w": 62, "h": 28, "font": "small_bold", "align": "right", "source": "weather_temp", "colour": light})
    return {"images": [{"digit_set": "seg", "format": "mono1", "dir": "digits-lcd"},
                       {"digit_set": "small", "format": "mono1", "dir": "small-lcd"},
                       {"name": "ghost", "format": "mono1", "file": "icons/ghost-lcd.png"}], "elements": els}


def render() -> str:
    face = {
        "meta": {"id": "epager.digital", "name": "Digital", "name_vi": "Điện tử", "author": "ePager", "version": "1.3.0",
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
