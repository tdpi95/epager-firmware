"""Vectors for clock faces (contracts/test-vectors/clock_faces.json): the test faces, the files that must be
refused, option merging and evaluation. Every expected result is computed with tools/epager_proto/clockface.py and
the C component must agree (tests/host/test_clock_face.c)."""
from __future__ import annotations

import copy
import struct

from epager_proto import clockface as CF
from epager_proto import registry as R
from epager_proto import transfer

ENUM = CF.ENUM
E, S, F, FONT, ICON, COND, OPT = (ENUM[k] for k in ("face_element", "face_source", "face_format", "face_font", "face_icon",
                                                    "face_cond", "face_option_type"))
MONO1, RGB565, RGB565A8 = ENUM["asset_format"]["mono1"], ENUM["asset_format"]["rgb565"], ENUM["asset_format"]["rgb565a8"]
DIGIT_W = {0: 6, 1: 4, 2: 6, 3: 6, 4: 7, 5: 6, 6: 6, 7: 6, 8: 6, 9: 6, 10: 3, 11: 10, 12: 10, 13: 5, 14: 4, 15: 7}
DIGIT_H = 8


def _px(fmt: int, w: int, h: int, seed: int) -> bytes:
    """Deterministic pixels for an image of the given format."""
    n = CF.image_size(fmt, w, h)
    return bytes((i * 37 + seed * 11 + 5) & 0xFF for i in range(n))


def digit_set(set_id: int, fmt: int, scale: int = 1) -> tuple[list[dict], list[bytes]]:
    imgs, pix = [], []
    for slot in range(16):
        w, h = DIGIT_W[slot] * scale, DIGIT_H * scale
        imgs.append({"format": fmt, "width": w, "height": h, "set": set_id, "slot": slot})
        pix.append(_px(fmt, w, h, slot + set_id))
    return imgs, pix


def opt_toggle(i, label, default):
    return {"id": i, "type": OPT["toggle"], "label": label, "label_vi": label + " vi", "default": default}


def opt_choice(i, label, names, default):
    return {"id": i, "type": OPT["choice"], "label": label, "default": default,
            "choice": [{"label": n, "label_vi": n + " vi"} for n in names]}


def opt_colour(i, label, colours, default):
    return {"id": i, "type": OPT["colour"], "label": label, "default": default,
            "choice": [{"label": f"c{k}", "colour": c} for k, c in enumerate(colours)]}


def cond(kind, **kw):
    return {"kind": COND[kind], **kw}


def paper_full() -> tuple[dict, list[bytes]]:
    """Every element kind, a digit set, three options: the face the evaluation vectors use (Paper, mono1)."""
    imgs, pix = digit_set(1, MONO1)
    imgs.append({"format": MONO1, "width": 8, "height": 8})
    pix.append(_px(MONO1, 8, 8, 99))
    plain = len(imgs) - 1
    el = [
        {"kind": E["rect"], "x": 0, "y": 0, "w": 200, "h": 200, "fill": True, "colour": 0xFFFF},
        {"kind": E["number"], "x": 10, "y": 20, "set": 1, "source": S["time"], "spacing": 1},
        {"kind": E["number"], "x": 10, "y": 40, "w": 100, "align": 2, "set": 1, "source": S["hour"], "format": F["pad2"]},
        {"kind": E["text"], "x": 10, "y": 60, "w": 80, "h": 20, "font": FONT["small"], "source": S["weekday"], "format": F["long"]},
        {"kind": E["text"], "x": 100, "y": 60, "w": 80, "h": 20, "font": FONT["small_bold"], "literal": "ePager",
         "colour_option": 3, "colour": 0x1234},
        {"kind": E["text"], "x": 10, "y": 80, "w": 180, "h": 20, "font": FONT["small"], "source": S["date"], "align": 1},
        {"kind": E["number"], "x": 10, "y": 100, "set": 1, "source": S["weather_high"], "format": F["unit"],
         "visible_if": [cond("option_eq", option=1, value=1), cond("source_has", source=S["weather_high"])]},
        {"kind": E["icon"], "x": 150, "y": 100, "w": 32, "h": 32, "icon": ICON["weather"],
         "visible_if": [cond("option_eq", option=1, value=1)]},
        {"kind": E["icon"], "x": 150, "y": 140, "w": 16, "h": 16, "icon": ICON["ble"]},
        {"kind": E["icon"], "x": 170, "y": 140, "w": 16, "h": 16, "icon": ICON["wifi"]},
        {"kind": E["icon"], "x": 130, "y": 140, "w": 16, "h": 16, "icon": ICON["charging"]},
        {"kind": E["image"], "x": 50, "y": 150, "image": plain, "colour": 0},
        {"kind": E["text"], "x": 10, "y": 160, "w": 60, "h": 20, "font": FONT["small"], "literal": "Low",
         "visible_if": [cond("source_gt0", source=S["charging"], negate=True)]},
        {"kind": E["number"], "x": 10, "y": 120, "set": 1, "source": S["notif_count"],
         "visible_if": [cond("source_gt0", source=S["notif_count"])]},
        {"kind": E["line"], "x": 10, "y": 190, "w": 50, "h": 0, "thickness": 2},
        {"kind": 99, "x": 0, "y": 0, "w": 10, "h": 10},
        {"kind": E["text"], "x": 0, "y": 0, "w": 10, "h": 10, "font": FONT["small"], "source": 200},
        {"kind": E["text"], "x": 10, "y": 170, "w": 60, "h": 20, "font": FONT["large_bold"], "source": S["room_temp"],
         "format": F["nounit"]},
        {"kind": E["text"], "x": 70, "y": 170, "w": 60, "h": 20, "font": FONT["small"], "source": S["room_humidity"]},
        {"kind": E["text"], "x": 130, "y": 170, "w": 60, "h": 20, "font": FONT["small"], "source": S["month"],
         "format": F["long"]},
        {"kind": E["number"], "x": 120, "y": 20, "set": 1, "source": S["year"], "format": F["year2"]},
        {"kind": E["text"], "x": 120, "y": 40, "w": 40, "h": 20, "font": FONT["small"], "source": S["ampm"]},
        {"kind": E["number"], "x": 160, "y": 40, "set": 1, "source": S["ampm"]},
        {"kind": E["number"], "x": 10, "y": 135, "set": 1, "source": S["weather_rain"]},
        {"kind": E["number"], "x": 60, "y": 135, "set": 1, "source": S["day"], "format": F["pad2"]},
        {"kind": E["text"], "x": 110, "y": 135, "w": 80, "h": 20, "font": FONT["small"], "source": S["weather_place"]},
        {"kind": E["number"], "x": 100, "y": 120, "set": 1, "source": S["room_temp"]},
        {"kind": E["text"], "x": 120, "y": 190, "w": 60, "h": 10, "font": FONT["small"], "source": S["battery_pct"]},
        {"kind": E["text"], "x": 60, "y": 190, "w": 50, "h": 10, "font": FONT["small"], "source": S["weather_temp"]},
        {"kind": E["text"], "x": 60, "y": 180, "w": 50, "h": 10, "font": FONT["small"], "source": S["weekday"]},
        {"kind": E["text"], "x": 0, "y": 180, "w": 40, "h": 10, "font": FONT["small"], "source": S["hour"],
         "format": F["plain"]},
        {"kind": E["text"], "x": 40, "y": 180, "w": 20, "h": 10, "font": FONT["small"], "source": S["minute"],
         "format": F["plain"]},
        {"kind": E["text"], "x": 0, "y": 190, "w": 30, "h": 10, "font": FONT["small"], "source": S["day"]},
        {"kind": E["text"], "x": 30, "y": 190, "w": 30, "h": 10, "font": FONT["small"], "source": S["month"],
         "format": F["short"]},
        {"kind": E["text"], "x": 0, "y": 170, "w": 10, "h": 10, "font": FONT["small"],
         "source": S["weather_low"], "format": F["nounit"]},
    ]
    desc = {"meta": {"id": "test.full", "name": "Full", "name_vi": "Day du", "author": "tests", "version": "1.2.3",
                     "min_contract": "0.15.0", "update_anchor": 3},
            "image": imgs,
            "option": [opt_toggle(1, "Weather", 1), opt_choice(2, "Digits", ["Thin", "Thick"], 0),
                       opt_colour(3, "Accent", [0x0000, 0xF800, 0x07E0], 1)],
            "element": el}
    return desc, pix


def lcd_full() -> tuple[dict, list[bytes]]:
    """LCD: rgb565a8 digits, an rgb565 picture, a mono1 mask, an arc."""
    imgs, pix = digit_set(1, RGB565A8, 2)
    imgs.append({"format": RGB565, "width": 40, "height": 30})
    pix.append(_px(RGB565, 40, 30, 7))
    imgs.append({"format": MONO1, "width": 16, "height": 16})
    pix.append(_px(MONO1, 16, 16, 8))
    pic, mask = len(imgs) - 2, len(imgs) - 1
    el = [
        {"kind": E["rect"], "x": 0, "y": 0, "w": 296, "h": 240, "fill": True, "colour": 0x0841},
        {"kind": E["image"], "x": 10, "y": 10, "image": pic},
        {"kind": E["image"], "x": 60, "y": 10, "image": mask, "colour_option": 1, "colour": 0xFFFF},
        {"kind": E["number"], "x": 20, "y": 60, "w": 250, "align": 1, "set": 1, "source": S["time"], "spacing": 2},
        {"kind": E["arc"], "x": 200, "y": 150, "w": 80, "h": 80, "source": S["battery_pct"], "arc_start": 135,
         "arc_end": 360, "thickness": 6, "colour_option": 1},
        {"kind": E["text"], "x": 20, "y": 200, "w": 200, "h": 28, "font": FONT["small_bold"], "source": S["date"],
         "colour": 0xFFFF},
        {"kind": E["text"], "x": 20, "y": 170, "w": 100, "h": 28, "font": FONT["clock"], "source": S["minute"]},
    ]
    desc = {"meta": {"id": "test.lcd", "name": "LCD full", "version": "0.1.0"},
            "image": imgs, "option": [opt_colour(1, "Accent", [0xFFFF, 0x07FF], 0)], "element": el}
    return desc, pix


def heavy() -> tuple[dict, list[bytes]]:
    """48 number elements (5 glyph objects each): valid, but more LVGL objects than a board draws (the renderer
    falls back to Classic: tests/host/test_ui*.c)."""
    imgs, pix = digit_set(1, MONO1)
    el = [{"kind": E["number"], "x": 0, "y": 4 * i, "set": 1, "source": S["time"]} for i in range(48)]
    return {"meta": {"id": "test.heavy", "name": "Heavy", "version": "1.0.0"}, "image": imgs, "element": el}, pix


def with_update() -> tuple[dict, list[bytes]]:
    """A face that draws the update_available source itself (a "U" while an update waits): the firmware adds no mark."""
    return ({"meta": {"id": "test.update", "name": "Update", "version": "1.0.0"},
             "element": [{"kind": E["text"], "x": 0, "y": 0, "w": 20, "h": 20, "font": FONT["small"], "literal": "U",
                          "visible_if": [cond("source_gt0", source=S["update_available"])]}]}, [])


def minimal() -> tuple[dict, list[bytes]]:
    return ({"meta": {"id": "test.min", "name": "Min", "version": "1.0.0"},
             "element": [{"kind": E["rect"], "x": 0, "y": 0, "w": 10, "h": 10, "fill": True}]}, [])


# --------------------------------------------------------------------------- invalid files

def _mutated(base: tuple[dict, list[bytes]], width: int, height: int, fn) -> bytes:
    desc, pix = copy.deepcopy(base[0]), list(base[1])
    fn(desc, pix)
    return CF.build_face(desc, pix, width, height)


def invalid_cases(paper_file: bytes) -> list[tuple[str, str, str, bytes, str]]:
    """(name, description, board, file, expected status). All Paper unless noted."""
    base = paper_full()
    out: list[tuple[str, str, str, bytes, str]] = []

    def add(name, desc, fn, board="paper", expect="IMAGE_INVALID", w=200, h=200):
        out.append((name, desc, board, _mutated(base, w, h, fn), expect))

    out.append(("bad_magic", "magic is not ESPA", "paper", b"XSPA" + paper_file[4:], "IMAGE_INVALID"))
    out.append(("bad_header_version", "header_version 2", "paper", paper_file[:4] + b"\x02" + paper_file[5:], "IMAGE_INVALID"))
    out.append(("wrong_asset_type", "asset_type 5", "paper", paper_file[:5] + b"\x05" + paper_file[6:], "IMAGE_INVALID"))
    out.append(("wrong_format", "format 6", "paper", paper_file[:6] + b"\x06" + paper_file[7:], "IMAGE_INVALID"))
    out.append(("flags_set", "flags 1", "paper", paper_file[:7] + b"\x01" + paper_file[8:], "IMAGE_INVALID"))
    out.append(("wrong_screen_size", "a 200x200 face on the LCD", "lcd", paper_file, "IMAGE_INVALID"))
    out.append(("payload_len_short", "payload_len one less", "paper",
                paper_file[:12] + struct.pack("<I", len(paper_file) - 17) + paper_file[16:], "IMAGE_INVALID"))
    out.append(("tlv_len_past_end", "tlv_len larger than the file", "paper",
                paper_file[:16] + struct.pack("<I", len(paper_file)) + paper_file[20:], "IMAGE_INVALID"))
    out.append(("truncated_file", "cut in the data area", "paper", paper_file[:-3], "IMAGE_INVALID"))
    out.append(("too_short", "header only", "paper", paper_file[:16], "IMAGE_INVALID"))
    tlv_len = struct.unpack("<I", paper_file[16:20])[0]
    pad = -(20 + tlv_len) % 4
    if pad:
        b = bytearray(paper_file)
        b[20 + tlv_len] = 1
        out.append(("padding_not_zero", "a padding byte is not 0", "paper", bytes(b), "IMAGE_INVALID"))
    out.append(("data_area_long", "an extra data byte", "paper",
                paper_file[:12] + struct.pack("<I", len(paper_file) - 15) + paper_file[16:] + b"\x00", "IMAGE_INVALID"))

    add("meta_missing", "no meta", lambda d, p: d.pop("meta"))
    add("id_differs", "meta.id is not the asset id", lambda d, p: d["meta"].update(id="other.face"))
    add("min_contract_newer", "needs contract 0.99.0", lambda d, p: d["meta"].update(min_contract="0.99.0"))
    add("min_contract_garbage", "not a version", lambda d, p: d["meta"].update(min_contract="newest"))
    add("element_49", "49 elements", lambda d, p: d["element"].extend([d["element"][0]] * (49 - len(d["element"]))))
    add("image_33", "33 images", lambda d, p: (d["image"].extend([{"format": MONO1, "width": 1, "height": 1}] * 16),
                                                p.extend([b"\x00"] * 16)))
    add("option_9", "9 options", lambda d, p: d["option"].extend([opt_toggle(10 + k, "x", 0) for k in range(6)]))
    add("option_id_repeated", "two options with id 1", lambda d, p: d["option"].append(opt_toggle(1, "again", 0)))
    add("toggle_with_choices", "a toggle that has choices", lambda d, p: d["option"][0].update(choice=[{"label": "a"}]))
    add("toggle_default_2", "toggle default 2", lambda d, p: d["option"][0].update(default=2))
    add("choice_default_out_of_range", "default 2 of 2 choices", lambda d, p: d["option"][1].update(default=2))
    add("choice_none", "a choice option without choices", lambda d, p: d["option"][1].update(choice=[]))
    add("choice_9", "9 choices", lambda d, p: d["option"][1].update(choice=[{"label": f"c{k}"} for k in range(9)]))
    add("colour_without_colour", "a colour choice with no colour", lambda d, p: d["option"][2]["choice"][0].pop("colour"))
    add("image_format_rgb565_on_paper", "rgb565 on a mono1 board",
        lambda d, p: (d["image"].append({"format": RGB565, "width": 2, "height": 2}), p.append(bytes(8))))
    add("image_set_without_slot", "set but no slot", lambda d, p: d["image"][3].pop("slot"))
    add("image_slot_repeated", "two images in slot 4 of set 1", lambda d, p: d["image"][5].update(slot=4))
    add("digit_missing", "set 1 without the digit 7",
        lambda d, p: (d["image"][7].pop("set"), d["image"][7].pop("slot")))
    add("digit_heights_differ", "digit 3 is taller", lambda d, p: (d["image"][3].update(height=9), p.__setitem__(3, bytes(9))))
    add("number_set_unknown", "a number that names set 5", lambda d, p: d["element"][1].update(set=5))
    add("number_without_source", "a number with no source", lambda d, p: d["element"][1].pop("source"))
    add("box_right_edge", "a rectangle that ends at x = 201", lambda d, p: d["element"][0].update(w=201))
    add("box_negative_x", "x = -1", lambda d, p: d["element"][0].update(x=-1))
    add("box_bottom_edge", "text that ends at y = 201", lambda d, p: d["element"][3].update(y=190))
    add("rect_zero_size", "w = 0", lambda d, p: d["element"][0].update(w=0))
    add("line_no_length", "a line from a point to itself", lambda d, p: d["element"][14].update(w=0, h=0))
    add("text_without_font", "no font", lambda d, p: d["element"][3].pop("font"))
    add("text_without_value", "no source and no literal", lambda d, p: d["element"][4].pop("literal"))
    add("icon_without_icon", "no icon", lambda d, p: d["element"][7].pop("icon"))
    add("image_index_out_of_range", "image 99", lambda d, p: d["element"][11].update(image=99))
    add("arc_on_paper", "an arc on the Paper",
        lambda d, p: d["element"].append({"kind": E["arc"], "x": 0, "y": 0, "w": 20, "h": 20, "source": S["battery_pct"]}))
    add("conditions_3", "three visible_if",
        lambda d, p: d["element"][0].update(visible_if=[cond("option_eq", option=1, value=1)] * 3))
    # TLV-level damage inside the description
    out.append(("tlv_element_truncated", "the description ends inside a TLV", "paper",
                _truncate_tlv(paper_file, 2), "IMAGE_INVALID"))
    out.append(("tlv_tag_zero", "tag 0 in the description", "paper", _insert_tlv(paper_file, b"\x00\x00\x00"), "IMAGE_INVALID"))
    out.append(("tlv_duplicate_meta", "meta twice", "paper", _insert_tlv(paper_file, _first_tlv(paper_file)), "IMAGE_INVALID"))
    return out


def _split(file: bytes) -> tuple[bytes, bytes, bytes]:
    tlv_len = struct.unpack("<I", file[16:20])[0]
    return file[:16], file[20:20 + tlv_len], file[20 + tlv_len:]


def _first_tlv(file: bytes) -> bytes:
    _, tlv, _ = _split(file)
    n = struct.unpack("<H", tlv[1:3])[0]
    return tlv[:3 + n]


def _reassemble(file: bytes, tlv: bytes) -> bytes:
    head, old_tlv, rest = _split(file)
    old_pad = -(20 + len(old_tlv)) % 4
    data = rest[old_pad:]
    pad = -(20 + len(tlv)) % 4
    payload = struct.pack("<I", len(tlv)) + tlv + bytes(pad) + data
    return head[:12] + struct.pack("<I", len(payload)) + payload


def _insert_tlv(file: bytes, extra: bytes) -> bytes:
    _, tlv, _ = _split(file)
    return _reassemble(file, extra + tlv)


def _truncate_tlv(file: bytes, n: int) -> bytes:
    _, tlv, _ = _split(file)
    return _reassemble(file, tlv[:-n])


def valid_edge_cases() -> list[tuple[str, str, str, bytes, dict]]:
    """Files that must be accepted although they carry things this contract does not know."""
    out = []
    base = paper_full()

    def add(name, desc, fn, board="paper"):
        w, h = (200, 200) if board == "paper" else (296, 240)
        out.append((name, desc, board, _mutated(base if board == "paper" else lcd_full(), w, h, fn), {}))

    add("plain", "", lambda d, p: None)
    f = out.pop()[3]
    out.append(("unknown_tlv_tag_skipped", "an unknown tag at the top level is skipped", "paper",
                _insert_tlv(f, b"\xee\x02\x00\x01\x02"), {}))
    add("unknown_element_kind", "kind 99 is skipped", lambda d, p: d["element"].append({"kind": 99, "x": 0, "y": 0}))
    add("unknown_source", "a source this contract does not know", lambda d, p: d["element"].append(
        {"kind": E["text"], "x": 0, "y": 0, "w": 10, "h": 10, "font": FONT["small"], "source": 77}))
    add("unknown_font", "a font this contract does not know", lambda d, p: d["element"].append(
        {"kind": E["text"], "x": 0, "y": 0, "w": 10, "h": 10, "font": 9, "literal": "x"}))
    add("unknown_icon", "an icon this contract does not know", lambda d, p: d["element"].append(
        {"kind": E["icon"], "x": 0, "y": 0, "w": 10, "h": 10, "icon": 9}))
    add("unknown_option_type", "an option of a type this contract does not know", lambda d, p: d["option"].append(
        {"id": 9, "type": 9, "label": "future"}))
    add("unknown_condition", "a condition kind this contract does not know", lambda d, p: d["element"][0].update(
        visible_if=[{"kind": 9}]))
    add("no_elements", "a face that draws nothing", lambda d, p: d.update(element=[]))
    add("lcd_face", "LCD face", lambda d, p: None, board="lcd")
    return out


# --------------------------------------------------------------------------- evaluation data

WEEK = {"en": "Wednesday, 4 Nov", "vi": "Thứ Tư, 4/11"}


def data(**kw) -> dict:
    d = {"time_known": True, "hour": 9, "minute": 5, "day": 4, "month": 11, "year": 2026, "wday": 3, "clock_24h": True,
         "language": "en", "date": WEEK["en"], "fahrenheit": False,
         "wx": {"temp_c10": 321, "high_c10": 336, "low_c10": 246, "rain": 86, "cond": 7, "day": True, "place": "Hà Nội"},
         "room": {"temp_c100": 2351, "rh_c100": 6049},
         "battery": 80, "charging": False, "ble": True, "wifi": True, "notif": 3, "update": False}
    d.update(kw)
    return d


DATA_CASES = [
    ("morning_24h", "09:05, weather, sensor, 3 notifications", data(), None),
    ("afternoon_12h", "14:27 in 12-hour mode", data(hour=14, minute=27, clock_24h=False), None),
    ("midnight_12h", "00:00 in 12-hour mode is 12 AM", data(hour=0, minute=0, clock_24h=False), None),
    ("noon_12h", "12:30 is 12 PM", data(hour=12, minute=30, clock_24h=False), None),
    ("time_unknown", "no time: dashes, no date parts", data(time_known=False, date="Time not set"), None),
    ("vietnamese", "Vietnamese names", data(language="vi", date=WEEK["vi"]), None),
    ("fahrenheit", "Fahrenheit", data(fahrenheit=True), None),
    ("no_weather", "no reading", data(wx={}), None),
    ("weather_partial", "a reading without high, low and rain", data(wx={"temp_c10": -45, "cond": 0, "day": False}), None),
    ("rain_unknown", "rain above 100 is unknown", data(wx={"temp_c10": 100, "high_c10": 100, "low_c10": 100, "rain": 255, "cond": 1}), None),
    ("no_sensor", "no room climate", data(room=None), None),
    ("cold_room", "negative room temperature, Fahrenheit rounding", data(room={"temp_c100": -5, "rh_c100": 4949}, fahrenheit=True), None),
    ("negative_weather", "-12.5 degrees", data(wx={"temp_c10": -125, "high_c10": -105, "low_c10": -155, "rain": 0, "cond": 10, "day": True}), None),
    ("battery_unknown", "level unknown, charging", data(battery=255, charging=True), None),
    ("battery_full", "100 %", data(battery=100, ble=False, wifi=False), None),
    ("no_notifications", "no notifications", data(notif=0), None),
    ("many_notifications", "40 notifications", data(notif=40, update=True), None),
    ("toggle_weather_off", "option 1 off", data(), [1, 0]),
    ("choice_thick_accent_green", "option 2 = 1, option 3 = 2", data(), [2, 1, 3, 2]),
    ("options_garbage", "unknown ids and values out of range are ignored", data(), [1, 5, 2, 9, 77, 1]),
    ("options_repeated", "the last pair of an id wins", data(), [3, 0, 3, 2]),
]
OPTION_CASES = [
    ("empty", []),
    ("one", [2, 1]),
    ("unknown_id", [9, 1]),
    ("out_of_range", [1, 2, 2, 2, 3, 3]),
    ("repeated", [3, 2, 3, 0]),
    ("odd_trailing_byte", [2, 1, 3]),
]


def gallery_cases() -> list[dict]:
    """gallery.json manifests (clock-face-gallery.md): 'valid' says whether clock-face-gallery.schema.json accepts it."""
    sha = "ab" * 32
    entry = {"file": "epager.big-paper.epface", "size": 5216, "sha256": sha, "min_contract": "0.15.0",
             "thumbnail": "epager.big-paper.png"}
    good = {"schema": 1, "generated": "2026-11-20T09:30:00Z",
            "faces": [{"id": "epager.big", "name": {"en": "Big", "vi": "Số lớn"}, "author": "ePager", "version": "1.0.0",
                       "license": "CC-BY-4.0", "boards": {"paper": entry, "lcd": dict(entry, file="epager.big-lcd.epface")}}]}

    def with_face(**kw):
        f = copy.deepcopy(good["faces"][0])
        f.update(kw)
        return {**good, "faces": [f]}

    bad_board = copy.deepcopy(good)
    bad_board["faces"][0]["boards"]["esp32"] = entry
    no_sha = copy.deepcopy(good)
    del no_sha["faces"][0]["boards"]["paper"]["sha256"]
    return [
        {"name": "valid", "valid": True, "manifest": good},
        {"name": "valid_unknown_fields", "valid": True, "manifest": {**good, "future": {"x": 1}}},
        {"name": "valid_no_faces", "valid": True, "manifest": {"schema": 1, "faces": []}},
        {"name": "missing_schema", "valid": False, "manifest": {"faces": []}},
        {"name": "missing_sha256", "valid": False, "manifest": no_sha},
        {"name": "unknown_board", "valid": False, "manifest": bad_board},
        {"name": "id_uppercase", "valid": False, "manifest": with_face(id="Epager.Big")},
        {"name": "version_not_semver", "valid": False, "manifest": with_face(version="one")},
        {"name": "file_path_traversal", "valid": False,
         "manifest": with_face(boards={"paper": dict(entry, file="../x.epface")})},
        {"name": "sha256_short", "valid": False, "manifest": with_face(boards={"paper": dict(entry, sha256="ab")})},
        {"name": "no_boards", "valid": False, "manifest": with_face(boards={})},
    ]


def gen_clock_faces() -> dict:
    faces: dict[str, dict] = {}

    def face(name: str, board: str, built: tuple[dict, list[bytes]]):
        lim = CF.LIMITS[board]
        file = CF.build_face(built[0], built[1], lim.width, lim.height)
        faces[name] = {"board": board, "id": built[0]["meta"]["id"], "file_hex": file.hex()}
        return file

    paper_file = face("paper_full", "paper", paper_full())
    face("lcd_full", "lcd", lcd_full())
    face("paper_min", "paper", minimal())
    face("paper_heavy", "paper", heavy())
    face("paper_update", "paper", with_update())
    heavy_lcd = heavy()
    faces["lcd_heavy"] = {"board": "lcd", "id": "test.heavy",
                          "file_hex": CF.build_face(heavy_lcd[0], heavy_lcd[1], 296, 240).hex()}
    min_lcd = minimal()
    faces["lcd_min"] = {"board": "lcd", "id": "test.min",
                        "file_hex": CF.build_face(min_lcd[0], min_lcd[1], 296, 240).hex()}

    parse = []

    def expect_ok(name, desc, board, file, face_id, face_name=None):
        f = CF.parse_face(file, CF.LIMITS[board], face_id)
        parse.append({"name": name, "description": desc, "board": board, "id": face_id,
                      **({"face": face_name} if face_name else {"file_hex": file.hex()}),
                      "expect": "OK",
                      "summary": {"id": f.meta["id"], "name": f.meta["name"], "version": f.meta["version"],
                                  "images": len(f.images), "options": len(f.options), "elements": len(f.elements),
                                  "draws_update_available": CF.draws_source(f, "update_available")}})

    def expect_bad(name, desc, board, file, status, face_id="test.full", max_file=None):
        lim = CF.LIMITS[board]
        if max_file is not None:
            lim = CF.Limits(lim.width, lim.height, max_file, lim.formats, lim.arcs)
        try:
            CF.parse_face(file, lim, face_id)
        except CF.FaceError as e:
            assert e.status == status, (name, e.status, e.reason)
        else:
            raise AssertionError(f"{name}: the reference accepted the file")
        case = {"name": name, "description": desc, "board": board, "id": face_id, "file_hex": file.hex(), "expect": status}
        if max_file is not None:
            case["max_file"] = max_file
        parse.append(case)

    for name, board, face_id in (("paper_full", "paper", "test.full"), ("lcd_full", "lcd", "test.lcd"),
                                 ("paper_min", "paper", "test.min"), ("lcd_min", "lcd", "test.min"),
                                 ("paper_heavy", "paper", "test.heavy"), ("lcd_heavy", "lcd", "test.heavy"),
                                 ("paper_update", "paper", "test.update")):
        expect_ok(f"valid_{name}", "", board, bytes.fromhex(faces[name]["file_hex"]), face_id, name)
    for name, desc, board, file, _ in valid_edge_cases():
        fid = "test.lcd" if board == "lcd" else "test.full"
        expect_ok(name, desc, board, file, fid)
    for name, desc, board, file, expect in invalid_cases(paper_file):
        expect_bad(name, desc, board, file, expect)
    expect_bad("above_max_size", "a file above the board's max_size (here 100 bytes)", "paper", paper_file, "TOO_LARGE",
               max_file=100)

    options = []
    pf = CF.parse_face(paper_file, CF.PAPER, "test.full")
    for name, pairs in OPTION_CASES:
        values = CF.resolve_opts(pf, bytes(pairs))
        options.append({"name": name, "face": "paper_full", "pairs_hex": bytes(pairs).hex(), "values": values,
                        "packed_hex": CF.pack_opts(pf, values).hex()})

    evals = []
    for name, desc, d, pairs in DATA_CASES:
        face_name = "paper_full"
        values = CF.resolve_opts(pf, bytes(pairs or []))
        evals.append({"name": name, "description": desc, "face": face_name, "pairs_hex": bytes(pairs or []).hex(),
                      "data": d, "elements": CF.evaluate(pf, values, d)})
    lf = CF.parse_face(bytes.fromhex(faces["lcd_full"]["file_hex"]), CF.LCD, "test.lcd")
    for name, desc, d, pairs in (DATA_CASES[0], DATA_CASES[1], DATA_CASES[4], DATA_CASES[13]):
        p = [1, 1] if name == "afternoon_12h" else []
        values = CF.resolve_opts(lf, bytes(p))
        evals.append({"name": f"lcd_{name}", "description": desc, "face": "lcd_full", "pairs_hex": bytes(p).hex(),
                      "data": d, "elements": CF.evaluate(lf, values, d)})

    return {"description": "Clock faces (asset-formats.md, 'Clock faces'). 'faces' are files by name. 'parse': parsing "
                           "file_hex for the board (limits in asset-formats.md; max_file overrides the file size limit) "
                           "with the asset id 'id' must give 'expect' (OK, or IMAGE_INVALID, or TOO_LARGE); the file is "
                           "file_hex, or the named 'face'; for OK the "
                           "'summary' must match. 'options': resolving the stored pairs against the face gives "
                           "'values' (one per option in table order) and packing them gives 'packed_hex'. 'eval': what "
                           "each element draws for the option pairs and the data snapshot; elements are compared in "
                           "order, fields not listed for an element are absent. Data fields: time_known hour minute "
                           "day month year wday (0 = Sunday) clock_24h language ('en' or 'vi') date fahrenheit; wx "
                           "(temp_c10 high_c10 low_c10 rain cond day place; a missing key = no value); room "
                           "(temp_c100 rh_c100) or null; battery (255 = unknown) charging ble wifi notif update. 'gallery': "
                           "gallery.json manifests; 'valid' says whether clock-face-gallery.schema.json accepts them.",
            "scope": "app+board", "faces": faces, "parse": parse, "options": options, "eval": evals,
            "gallery": gallery_cases()}
