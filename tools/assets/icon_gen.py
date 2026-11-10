#!/usr/bin/env python3
"""Generate the built-in notification icons (one per registry `category`, protocol.md 6.4), the
16x16 status-bar icons (Bluetooth connected / not connected, Wi-Fi / Wi-Fi failed) and the colour weather icons
as C source.

Each notification icon is 24x24, `mono1` (asset-formats.md: MSB-first rows, bit 1 = white, bit 0 = black,
padding bits 0), the same layout as an uploaded `app_icon`, so the UI has a single icon path. The artwork
is drawn with Pillow primitives at 4x and thresholded at 50 %; the 16x16 status icons are pixel art
(supersampled 1 px lines blur at that size). All of it is board-defined (the contract only says "one per
category"), so redrawing it is not a contract change.

Weather icons (LCD idle screen, PLAN D64): 32x32 RGB565A8 images of Noto Color Emoji's weather
glyphs (OFL, the font font_gen.py fetches), each cropped to its ink and scaled to fit a 30 px box, centred,
with a soft 1 px slate outline so white clouds stay visible on the light theme.

Paper idle screen (PLAN D78): 32x32 mono1 weather icons (the same eleven conditions), 24x24 thermometer and
humidity icons and a 24x24 umbrella (chance of rain), drawn here with Pillow primitives like the notification
icons (ui_mono_icons.c).

Output: components/ui_common/src/ui_icons.c, ui_weather_icons.c and ui_mono_icons.c, committed (builds never need Pillow).
Usage: icon_gen.py [--check] [--preview FILE.png]   (--preview: contact sheet at 6x, for looking at them)
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path
from typing import Callable

from PIL import Image, ImageDraw, ImageFilter

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))  # font_gen
from epager_proto import assets  # noqa: E402
from epager_proto import registry as R  # noqa: E402

SIZE = 24
S = 4  # supersampling
OUT = REPO / "components" / "ui_common" / "src" / "ui_icons.c"
WEATHER_OUT = REPO / "components" / "ui_common" / "src" / "ui_weather_icons.c"
MONO_OUT = REPO / "components" / "ui_common" / "src" / "ui_mono_icons.c"
WEATHER_SIZE = 32
WEATHER_INK = 30
# order = ui_weather_icon_t (ui_icons.h)
WEATHER_ICONS = {
    "sun": 0x2600, "moon": 0x1F319, "sun_small_cloud": 0x1F324, "sun_cloud": 0x26C5, "cloud": 0x2601,
    "fog": 0x1F32B, "rain": 0x1F327, "heavy_rain": 0x2614, "sun_rain": 0x1F326, "snow": 0x1F328,
    "thunder": 0x26C8,
}

Draw = ImageDraw.ImageDraw


def p(*pts: float) -> list[float]:
    return [v * S for v in pts]


def box(d: Draw, x0: float, y0: float, x1: float, y1: float, r: float = 0, fill: int = 0) -> None:
    if r:
        d.rounded_rectangle(p(x0, y0, x1 - 0.01, y1 - 0.01), r * S, fill=fill)
    else:
        d.rectangle(p(x0, y0, x1 - 0.01, y1 - 0.01), fill=fill)


def line(d: Draw, *pts: float, w: float = 2, fill: int = 0) -> None:
    d.line(p(*pts), fill=fill, width=round(w * S), joint="curve")


def other(d: Draw) -> None:  # bell
    d.pieslice(p(5, 3, 19, 17), 180, 360, fill=0)
    d.polygon(p(5, 10, 19, 10, 21, 18, 3, 18), fill=0)
    box(d, 11, 1.5, 13, 4)
    d.ellipse(p(9.5, 18.5, 14.5, 23), fill=0)


def handset(d: Draw) -> None:
    """A curved grip (polyline along a circle) with a round pad at each end."""
    cx, cy, r = 13.0, 11.0, 8.5
    pts = [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))) for a in range(95, 196, 5)]
    line(d, *[v for pt in pts for v in pt], w=3.6)
    for x, y in (pts[0], pts[-1]):
        d.ellipse(p(x - 3.2, y - 3.2, x + 3.2, y + 3.2), fill=0)


def incoming_call(d: Draw) -> None:
    handset(d)
    line(d, 22, 2, 14, 10, w=1.8)  # arrow towards the handset
    line(d, 14, 5, 14, 10, 19, 10, w=1.8)


def missed_call(d: Draw) -> None:
    handset(d)
    line(d, 14, 2, 22, 10, w=1.8)  # cross
    line(d, 22, 2, 14, 10, w=1.8)


def voicemail(d: Draw) -> None:  # two reels joined by a tape
    d.ellipse(p(1, 6, 10, 15), fill=0)
    d.ellipse(p(14, 6, 23, 15), fill=0)
    d.ellipse(p(3.5, 8.5, 7.5, 12.5), fill=255)
    d.ellipse(p(16.5, 8.5, 20.5, 12.5), fill=255)
    box(d, 1, 16, 23, 19)


def social(d: Draw) -> None:  # speech bubble
    box(d, 2, 3, 22, 17, 4)
    d.polygon(p(6, 16, 6, 22, 12, 16), fill=0)
    for x in (7, 12, 17):
        d.ellipse(p(x - 1.3, 8.7, x + 1.3, 11.3), fill=255)


def schedule(d: Draw) -> None:  # calendar
    box(d, 2, 4, 22, 22, 2)
    box(d, 4, 9, 20, 20, 0, fill=255)
    box(d, 6, 1, 8, 6)
    box(d, 16, 1, 18, 6)
    for x in (6, 11, 16):
        for y in (11, 15):
            box(d, x, y, x + 3, y + 3)


def email(d: Draw) -> None:  # envelope
    box(d, 1, 4, 23, 20, 2)
    line(d, 3, 6, 12, 14, 21, 6, w=1.4, fill=255)


def news(d: Draw) -> None:  # newspaper
    box(d, 2, 3, 22, 21, 2)
    box(d, 4, 5, 20, 19, 0, fill=255)
    box(d, 6, 7, 18, 10)
    box(d, 6, 12, 11, 17)
    for y in (12, 15):
        box(d, 13, y, 18, y + 1.5)


def health(d: Draw) -> None:  # heart
    d.ellipse(p(2, 3, 13, 14), fill=0)
    d.ellipse(p(11, 3, 22, 14), fill=0)
    d.polygon(p(2.6, 11, 21.4, 11, 12, 22.5), fill=0)


def business(d: Draw) -> None:  # bar chart on an axis
    line(d, 3, 2, 3, 21, 22, 21, w=2)
    box(d, 6, 12, 10, 19)
    box(d, 12, 7, 16, 19)
    box(d, 18, 3, 22, 19)


def location(d: Draw) -> None:  # map pin
    d.ellipse(p(4, 1, 20, 17), fill=0)
    d.polygon(p(5.2, 12, 18.8, 12, 12, 23), fill=0)
    d.ellipse(p(9, 5.5, 15, 11.5), fill=255)


def entertainment(d: Draw) -> None:  # play button
    box(d, 2, 3, 22, 21, 5)
    d.polygon(p(9, 7, 9, 17, 17, 12), fill=255)


STATUS_SIZE = 16


def bt_rune() -> list[list[int]]:
    """The Bluetooth rune as pixels (1 px strokes: at 16 px, supersampled lines blur into a blob)."""
    g = [[0] * STATUS_SIZE for _ in range(STATUS_SIZE)]
    pts = [(7, y) for y in range(16)]                                   # stem
    pts += [(7 + i, i) for i in range(5)] + [(11 - i, 4 + i) for i in range(1, 8)]      # upper arrow
    pts += [(7 + i, 15 - i) for i in range(5)] + [(11 - i, 11 - i) for i in range(1, 8)]  # lower arrow
    for x, y in pts:
        g[y][x] = 1
    return g


def bt_on() -> list[list[int]]:
    return bt_rune()


def bt_off() -> list[list[int]]:
    """The rune struck through, with a 1 px white gap on each side of the slash."""
    g = bt_rune()
    for y in range(STATUS_SIZE):
        for x in range(STATUS_SIZE):
            if abs(x - y) == 1:
                g[y][x] = 0
    for i in range(1, STATUS_SIZE - 1):
        g[i][i] = 1
    return g


WIFI_FAN = (  # hand-drawn: at 16 px, computed arcs leave stray pixels; centred on rows 2-12
    "................",
    "................",
    "....########....",
    "..##........##..",
    ".#............#.",
    "#....######....#",
    "...##......##...",
    "..#..........#..",
    "......####......",
    ".....#....#.....",
    "................",
    ".......##.......",
    ".......##.......",
    "................",
    "................",
    "................",
)


def wifi_fan() -> list[list[int]]:
    """The Wi-Fi fan: three arcs and a dot."""
    return [[1 if c == "#" else 0 for c in row] for row in WIFI_FAN]


def wifi_on() -> list[list[int]]:
    return wifi_fan()


def wifi_off() -> list[list[int]]:
    """The fan struck through, as the Bluetooth rune."""
    g = wifi_fan()
    for y in range(STATUS_SIZE):
        for x in range(STATUS_SIZE):
            if abs(x - y) == 1:
                g[y][x] = 0
    for i in range(1, STATUS_SIZE - 1):
        g[i][i] = 1
    return g


CHARGING_BOLT = (  # hand-drawn, like the fan
    "................",
    "..........####..",
    ".........####...",
    "........####....",
    ".......####.....",
    "......####......",
    ".....#########..",
    ".....#########..",
    ".........###....",
    "........###.....",
    ".......###......",
    "......###.......",
    ".....###........",
    "....###.........",
    "....##..........",
    "................",
)


def charging() -> list[list[int]]:
    """A lightning bolt: the board is charging (Paper status line, PLAN D85)."""
    return [[1 if c == "#" else 0 for c in row] for row in CHARGING_BOLT]


QUIET_MOON = (  # a crescent: quiet hours apply (PLAN D126)
    "................",
    "................",
    "....##..........",
    "...###..........",
    "..###...........",
    "..###...........",
    ".####...........",
    ".#####..........",
    ".#####..........",
    ".######.........",
    "..######........",
    "..############..",
    "...##########...",
    "....########....",
    ".....######.....",
    "................",
)


def quiet() -> list[list[int]]:
    """A crescent moon: quiet hours apply (status line, PLAN D126)."""
    return [[1 if c == "#" else 0 for c in row] for row in QUIET_MOON]


# order = ui_status_icon_t
STATUS_ICONS = {"ble_connected": bt_on, "ble_disconnected": bt_off, "wifi": wifi_on, "wifi_failed": wifi_off,
                "charging": charging, "quiet": quiet}

ICONS = {
    "other": other, "incoming_call": incoming_call, "missed_call": missed_call, "voicemail": voicemail,
    "social": social, "schedule": schedule, "email": email, "news": news, "health_and_fitness": health,
    "business_and_finance": business, "location": location, "entertainment": entertainment,
}


def render(name: str) -> list[str]:
    """Rows of '#' / '.' (assets.mono1_pack input)."""
    if name in STATUS_ICONS:
        return ["".join("#" if v else "." for v in row) for row in STATUS_ICONS[name]()]
    img = Image.new("L", (SIZE * S, SIZE * S), 255)
    ICONS[name](ImageDraw.Draw(img))
    small = img.resize((SIZE, SIZE), Image.BOX)
    return ["".join("#" if small.getpixel((x, y)) < 128 else "." for x in range(SIZE)) for y in range(SIZE)]


def category_order() -> list[str]:
    values = sorted(R.load().enums["category"]["values"], key=lambda v: v["value"])
    names = [v["name"] for v in values]
    assert names == list(ICONS), (names, list(ICONS))  # index = enum value
    return names


def generate() -> str:
    lines = [
        "/* Generated by tools/assets/icon_gen.py from the registry `category` enum. Do not edit.",
        " * 24x24 mono1 (rows MSB first, bit 1 = white, bit 0 = black), indexed by category value. */",
        '#include "ui_icons.h"',
        "",
        "const uint8_t ui_builtin_icons[UI_BUILTIN_ICON_COUNT][UI_ICON_BYTES] = {",
    ]
    for name in category_order():
        data = assets.mono1_pack(render(name))
        assert len(data) == 72
        lines.append(f"    /* {name} */ {{")
        for i in range(0, len(data), 12):
            lines.append("        " + ", ".join(f"0x{b:02X}" for b in data[i:i + 12]) + ",")
        lines.append("    },")
    lines += ["};", "", "/* 16x16 mono1 status-bar icons, indexed by ui_status_icon_t. */",
              "const uint8_t ui_status_icons[UI_STATUS_ICON_COUNT][UI_STATUS_ICON_BYTES] = {"]
    for name in STATUS_ICONS:
        data = assets.mono1_pack(render(name))
        assert len(data) == 32
        lines.append(f"    /* {name} */ {{")
        for i in range(0, len(data), 16):
            lines.append("        " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
        lines.append("    },")
    lines += ["};", ""]
    return "\n".join(lines)


# ---------------------------------------------------------------- Paper idle icons (1 bit)
MONO_WX = 32   # weather icons
MONO_ENV = 24  # thermometer, humidity
MONO_RAIN = 24  # umbrella


def circle(d: Draw, cx: float, cy: float, r: float, fill: int = 0) -> None:
    d.ellipse(p(cx - r, cy - r, cx + r, cy + r), fill=fill)


def cloud(d: Draw, ox: float = 0, oy: float = 0, k: float = 1.0, grow: float = 0, fill: int = 0) -> None:
    """A cloud in a 32 grid (about x 3..29, y 6..26), scaled by k about the grid origin, then moved;
    `grow` widens it (a white halo that separates it from what is behind)."""
    def t(v: float, o: float) -> float:
        return o + v * k
    for cx, cy, r in ((10, 19, 6), (17, 14, 8), (24.5, 19.5, 5.5)):
        circle(d, t(cx, ox), t(cy, oy), r * k + grow, fill)
    d.rounded_rectangle(p(t(4, ox) - grow, t(19, oy) - grow, t(29, ox) + grow, t(26, oy) + grow), 3.5 * k * S + grow * S,
                        fill=fill)


def sun(d: Draw, cx: float, cy: float, r: float, ray: float = 4, w: float = 2) -> None:
    circle(d, cx, cy, r)
    for i in range(8):
        a = i * math.pi / 4
        line(d, cx + math.cos(a) * (r + 2), cy + math.sin(a) * (r + 2),
             cx + math.cos(a) * (r + 2 + ray), cy + math.sin(a) * (r + 2 + ray), w=w)


def drops(d: Draw, xs: list[float], y0: float, y1: float, w: float = 2) -> None:
    for x in xs:
        line(d, x + 1.5, y0, x - 1.5, y1, w=w)


def wx_sun(d: Draw) -> None:
    sun(d, 16, 16, 6, 5)


def wx_moon(d: Draw) -> None:
    circle(d, 15, 16, 10)
    circle(d, 21, 12.5, 8.5, 255)


def wx_sun_small_cloud(d: Draw) -> None:
    sun(d, 12, 12, 5, 4)
    cloud(d, 11, 11, 0.7, 1.6, 255)
    cloud(d, 11, 11, 0.7)


def wx_sun_cloud(d: Draw) -> None:
    sun(d, 11, 11, 5, 3.5)
    cloud(d, 3, 3, 0.85, 1.6, 255)
    cloud(d, 3, 3, 0.85)


def wx_cloud(d: Draw) -> None:
    cloud(d, 0, 2)


def wx_fog(d: Draw) -> None:
    cloud(d, 3, -3, 0.85)
    for y, x0, x1 in ((23, 4, 24), (27.5, 8, 28)):
        line(d, x0, y, x1, y, w=2)


def wx_rain(d: Draw) -> None:
    cloud(d, 0, -4)
    drops(d, [10, 17, 24], 23, 29)


def wx_heavy_rain(d: Draw) -> None:
    cloud(d, 0, -5)
    drops(d, [9, 16, 23], 21.5, 26)
    drops(d, [12.5, 19.5, 26.5], 26.5, 31)


def wx_sun_rain(d: Draw) -> None:
    sun(d, 11, 10, 4.5, 3.5)
    cloud(d, 4, 0, 0.8, 1.6, 255)
    cloud(d, 4, 0, 0.8)
    drops(d, [13, 20], 22.5, 28.5)


def wx_snow(d: Draw) -> None:
    cloud(d, 0, -4)
    for x in (9.5, 16, 22.5):
        for a in (0, math.pi / 3, 2 * math.pi / 3):
            line(d, x - math.cos(a) * 2.6, 26.5 - math.sin(a) * 2.6, x + math.cos(a) * 2.6,
                 26.5 + math.sin(a) * 2.6, w=1.4)


def wx_thunder(d: Draw) -> None:
    cloud(d, 0, -5)
    bolt = [(18.5, 18), (10.5, 27.5), (15.5, 27.5), (12.5, 32), (22, 23), (16.5, 23), (20, 18)]
    d.polygon(p(*[v for pt in bolt for v in pt]), fill=0, outline=255, width=round(1.4 * S))
    d.polygon(p(*[v for pt in bolt for v in pt]), fill=0)


def env_thermometer(d: Draw) -> None:
    box(d, 8, 1.5, 16, 16.5, 4)
    box(d, 10, 3.5, 14, 17, 2, 255)
    circle(d, 12, 18, 5.5)
    box(d, 11, 8, 13, 18)
    for y in (5, 9):
        line(d, 18, y, 21.5, y, w=1.5)


def env_humidity(d: Draw) -> None:
    circle(d, 12, 15, 7.5)
    d.polygon(p(12, 1.5, 5.3, 12.5, 18.7, 12.5), fill=0)
    circle(d, 8.8, 16, 1.7, 255)


def rain_umbrella(d: Draw) -> None:
    d.ellipse(p(1.5, 2.2, 22.5, 21), fill=0)
    d.rectangle(p(0, 12.3, 24, 24), fill=255)
    for cx in (5.25, 12, 18.75):  # scallops along the canopy's lower edge
        circle(d, cx, 12.3, 2.7, 255)
    line(d, 12, 12, 12, 19, w=2.1)
    line(d, 12, 19, 9.5, 21.2, w=2.1)


MONO_WX_ICONS = {"sun": wx_sun, "moon": wx_moon, "sun_small_cloud": wx_sun_small_cloud, "sun_cloud": wx_sun_cloud,
                 "cloud": wx_cloud, "fog": wx_fog, "rain": wx_rain, "heavy_rain": wx_heavy_rain,
                 "sun_rain": wx_sun_rain, "snow": wx_snow, "thunder": wx_thunder}  # order = ui_weather_icon_t
MONO_ENV_ICONS = {"thermometer": env_thermometer, "humidity": env_humidity}


def render_mono(fn: Callable[[Draw], None], size: int) -> list[str]:
    img = Image.new("L", (size * S, size * S), 255)
    fn(ImageDraw.Draw(img))
    small = img.resize((size, size), Image.BOX)
    return ["".join("#" if small.getpixel((x, y)) < 128 else "." for x in range(size)) for y in range(size)]


def generate_mono() -> str:
    def block(title: str, names: dict, size: int, ident: str, count: str) -> list[str]:
        out = [f"/* {title} */", f"const uint8_t {ident}[{count}][{size * size // 8}] = {{"]
        for name, fn in names.items():
            data = assets.mono1_pack(render_mono(fn, size))
            out.append(f"    /* {name} */ {{")
            for i in range(0, len(data), size // 8 * 4):
                out.append("        " + ", ".join(f"0x{b:02X}" for b in data[i:i + size // 8 * 4]) + ",")
            out.append("    },")
        return out + ["};", ""]
    lines = ["/* GENERATED by tools/assets/icon_gen.py - DO NOT EDIT. The Paper idle screen's icons (PLAN D78),",
             " * mono1 (rows MSB first, bit 1 = white, bit 0 = black). */",
             '#include "ui_icons.h"', ""]
    lines += block("32x32 weather icons, indexed by ui_weather_icon_t", MONO_WX_ICONS, MONO_WX, "ui_mono_wx_icons",
                   "UI_WEATHER_ICON_COUNT")
    lines += block("24x24: thermometer, humidity (UI_MONO_ENV_*)", MONO_ENV_ICONS, MONO_ENV, "ui_mono_env_icons", "2")
    lines += block("24x24: umbrella, the chance of rain", {"umbrella": rain_umbrella}, MONO_RAIN, "ui_mono_rain_icon",
                   "1")
    return "\n".join(lines)


def weather_images() -> dict[str, Image.Image]:
    from font_gen import ColourEmoji  # noqa: E402  (same directory; fetches the pinned font)
    ce = ColourEmoji(128)  # about the font's own bitmap size: scale once, at the end
    out = {}
    for name, cp in WEATHER_ICONS.items():
        img = ce.image(cp)
        img = img.crop(img.getbbox())
        k = WEATHER_INK / max(img.size)
        img = img.convert("RGBa").resize((max(1, round(img.width * k)), max(1, round(img.height * k))),
                                         Image.LANCZOS).convert("RGBA")
        icon = Image.new("RGBA", (WEATHER_SIZE, WEATHER_SIZE))
        icon.paste(img, ((WEATHER_SIZE - img.width) // 2, (WEATHER_SIZE - img.height) // 2))
        # a soft slate outline: white clouds and fog would vanish on the light theme's white background
        ring = icon.getchannel("A").filter(ImageFilter.MaxFilter(3)).point(lambda a: a * 140 // 255)
        cell = Image.new("RGBA", icon.size, (100, 116, 139, 0))
        cell.putalpha(ring)
        cell.alpha_composite(icon)
        out[name] = cell
    return out


def generate_weather() -> str:
    from font_gen import COLOUR_SOURCE, SOURCES, rgb565a8  # noqa: E402
    data, images = [], []
    for name, img in weather_images().items():
        blob = rgb565a8(img)
        data.append(f'static const alignas(4) uint8_t {name}[] = "' + "".join(f"\\x{v:02x}" for v in blob) + '";')
        images.append(f"    /* U+{WEATHER_ICONS[name]:04X} */ {{.header = {{.magic = LV_IMAGE_HEADER_MAGIC, "
                      f".cf = LV_COLOR_FORMAT_RGB565A8, .w = {WEATHER_SIZE}, .h = {WEATHER_SIZE}, "
                      f".stride = {2 * WEATHER_SIZE}}}, .data_size = {len(blob)}, .data = {name}}},")
    nl = "\n"
    return f"""/* GENERATED by tools/assets/icon_gen.py - DO NOT EDIT.
 * Weather icons, {WEATHER_SIZE}x{WEATHER_SIZE} RGB565A8, indexed by ui_weather_icon_t: {COLOUR_SOURCE}
 * ({SOURCES[COLOUR_SOURCE][0]}), SIL Open Font License 1.1 (fonts/LICENSE-NotoColorEmoji.txt).
 */
#include <stdalign.h>

#include "ui_icons.h"

{nl.join(data)}

const lv_image_dsc_t ui_weather_icons[UI_WEATHER_ICON_COUNT] = {{
{nl.join(images)}
}};
"""


def preview(path: Path) -> None:
    z, cell = 6, SIZE + 2
    sheet = Image.new("L", (6 * cell * z, 3 * cell * z), 200)
    for i, name in enumerate(category_order() + list(STATUS_ICONS)):
        for y, row in enumerate(render(name)):
            for x, c in enumerate(row):
                cx, cy = (i % 6) * cell + 1 + x, (i // 6) * cell + 1 + y
                sheet.paste(0 if c == "#" else 255, (cx * z, cy * z, cx * z + z, cy * z + z))
    weather = weather_images()
    wz = 4
    row = Image.new("RGBA", (len(weather) * (WEATHER_SIZE + 4) * wz, (WEATHER_SIZE + 4) * wz), (255, 255, 255, 255))
    for i, img in enumerate(weather.values()):
        big = img.resize((WEATHER_SIZE * wz, WEATHER_SIZE * wz), Image.NEAREST)
        row.alpha_composite(big, (i * (WEATHER_SIZE + 4) * wz + 2 * wz, 2 * wz))
    rows = [[(f, MONO_WX) for f in MONO_WX_ICONS.values()],
            [*((f, MONO_ENV) for f in MONO_ENV_ICONS.values()), (rain_umbrella, MONO_RAIN)]]
    mono = Image.new("L", (len(MONO_WX_ICONS) * (MONO_WX + 4) * wz, 2 * (MONO_WX + 4) * wz), 200)
    for ri, items in enumerate(rows):
        x = 2 * wz
        for fn, size in items:
            for yy, r in enumerate(render_mono(fn, size)):
                for xx, c in enumerate(r):
                    mono.paste(0 if c == "#" else 255, (x + xx * wz, (ri * (MONO_WX + 4) + 2 + yy) * wz,
                                                        x + (xx + 1) * wz, (ri * (MONO_WX + 4) + 3 + yy) * wz))
            x += (size + 4) * wz
    full = Image.new("RGB", (max(sheet.width, row.width, mono.width), sheet.height + row.height + mono.height),
                     (200, 200, 200))
    full.paste(sheet.convert("RGB"), (0, 0))
    full.paste(row.convert("RGB"), (0, sheet.height))
    full.paste(mono.convert("RGB"), (0, sheet.height + row.height))
    full.save(path)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="compare the committed file instead of writing it")
    ap.add_argument("--preview", type=Path, help="write a contact sheet PNG and exit")
    args = ap.parse_args()
    if args.preview:
        preview(args.preview)
        return 0
    outputs = {OUT: generate(), WEATHER_OUT: generate_weather(), MONO_OUT: generate_mono()}
    if args.check:
        stale = [p for p, text in outputs.items() if not p.exists() or p.read_text() != text]
        for p in stale:
            print(f"{p} is out of date: run tools/assets/icon_gen.py", file=sys.stderr)
        return 1 if stale else 0
    for p, text in outputs.items():
        p.write_text(text)
        print(f"wrote {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
