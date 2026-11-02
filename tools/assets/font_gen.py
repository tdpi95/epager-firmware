#!/usr/bin/env python3
"""Generate LVGL fonts (lv_font_fmt_txt, LVGL 9) as C source from TrueType fonts.

Coverage is exactly the registry's `text.renderable_ranges` and `text.emoji_ranges` (protocol.md 8),
so the board's text normalisation and the font agree. Emoji come from Noto Emoji's monochrome
glyphs (a variable font: the weight axis picks the stroke thickness per UI font), drawn at the
UI font's pixel size, with the advance set to the glyph's own width plus 2 px. Glyphs are rasterised anti-aliased and thresholded at 50 % (the
Paper panel is 1-bit), or kept anti-aliased at 4 bpp for the LCD (PLAN D59). FreeType's monochrome hinting was tried first: it moves the stems of
some precomposed letters (the stem of `ì` sat one pixel left of `i`'s), which shows as uneven
spacing once LVGL places each glyph on whole pixels. Code points the font lacks, including the
placeholder U+25A1 in Noto Sans, are drawn as the placeholder box.

The source fonts are downloaded once into .deps/fonts/ and checked against pinned SHA-256 hashes;
the generated C files are committed, so builds never need the network or Pillow.

Display fonts (the large clock and climate digits) set `chars`: exactly those characters, no emoji.

LCD fonts (`emoji="colour"`) leave the emoji out and name `ui_font_emoji_colour` as their LVGL fallback:
one RGB565A8 image per emoji from Noto Color Emoji (its 109 px bitmaps, scaled down with premultiplied
alpha), LZ4-compressed (PLAN D60: 1.64 MB raw, 1.20 MB compressed, so the LCD image keeps the 1.4x
headroom with Wi-Fi), drawn by LVGL as image glyphs through its image decoder, which decompresses each one
into a small buffer (components/ui_common/src/ui_emoji.c), written to EMOJI_COLOUR_OUT.

Usage: font_gen.py [--check]      (regenerates every font in FONTS; --check compares instead)
"""
from __future__ import annotations

import argparse
import hashlib
import sys
import urllib.request
from dataclasses import dataclass
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import lz4_block  # noqa: E402
from epager_proto import registry as R  # noqa: E402

THRESHOLD = 128
DEPS = REPO / ".deps" / "fonts"
OUT = REPO / "components" / "ui_common" / "fonts"
NOTO = "https://github.com/notofonts/notofonts.github.io/raw/main/fonts/NotoSans/hinted/ttf/"
SOURCES = {
    "NotoSans-Regular.ttf": (NOTO + "NotoSans-Regular.ttf",
                             "478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823"),
    "NotoSans-Bold.ttf": (NOTO + "NotoSans-Bold.ttf",
                          "1df075a380fc7cb898acf64c1f7b3b4dd780de3caa860178bf929de35817a913"),
    "NotoEmoji[wght].ttf": ("https://github.com/google/fonts/raw/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf",
                            "de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551"),
    "NotoColorEmoji.ttf": ("https://github.com/googlefonts/noto-emoji/raw/v2.051/fonts/NotoColorEmoji.ttf",
                           "72a635cb3d2f3524c51620cdde406b217204e8a6a06c6a096ff8ed4b5fd6e27b"),
}
EMOJI_SOURCE = "NotoEmoji[wght].ttf"
COLOUR_SOURCE = "NotoColorEmoji.ttf"
COLOUR_PPEM = 109        # the only size of Noto Color Emoji's CBDT bitmaps
EMOJI_CELL = 20          # colour emoji: the font's 136 x 128 cell scaled to this height (about 18 px of ink)
EMOJI_COLOUR_OUT = OUT / "ui_font_emoji_colour.c"


@dataclass
class FontSpec:
    name: str      # C symbol
    source: str    # key of SOURCES
    size: int      # pixels (FreeType size)
    emoji_wght: int  # weight axis of the emoji font (300-700)
    chars: str = ""  # non-empty: exactly these characters (display fonts), no emoji
    bpp: int = 1     # 1: thresholded (e-paper); 4: anti-aliased (LCD)
    emoji: str = "mono"  # "mono": Noto Emoji glyphs in the font; "colour": none, ui_font_emoji_colour is the fallback


FONTS = [
    FontSpec("ui_font_regular_16", "NotoSans-Regular.ttf", 16, 600),
    FontSpec("ui_font_bold_16", "NotoSans-Bold.ttf", 16, 700),
    FontSpec("ui_font_clock_56", "NotoSans-Bold.ttf", 56, 700, " -0123456789:"),
    FontSpec("ui_font_bold_24", "NotoSans-Bold.ttf", 24, 700, " %-.0123456789CF\u00b0"),
    # LCD (colour layout)
    FontSpec("ui_font_lcd_16", "NotoSans-Regular.ttf", 16, 0, bpp=4, emoji="colour"),
    FontSpec("ui_font_lcd_20", "NotoSans-Regular.ttf", 20, 0, bpp=4, emoji="colour"),
    FontSpec("ui_font_lcd_bold_20", "NotoSans-Bold.ttf", 20, 0, bpp=4, emoji="colour"),
    FontSpec("ui_font_lcd_clock_72", "NotoSans-Bold.ttf", 72, 0, " -0123456789:", bpp=4),
]


def fetch(source: str) -> Path:
    url, sha = SOURCES[source]
    path = DEPS / source
    if not path.exists():
        DEPS.mkdir(parents=True, exist_ok=True)
        print(f"downloading {url}")
        with urllib.request.urlopen(url, timeout=60) as r:
            data = r.read()
        path.write_bytes(data)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != sha:
        raise SystemExit(f"{path}: SHA-256 {digest} does not match the pinned {sha}")
    return path


def emoji_code_points() -> list[int]:
    emoji: set[int] = set()
    for lo, hi in R.load().text["emoji_ranges"]:
        emoji.update(range(int(lo, 16), int(hi, 16) + 1))
    return sorted(emoji)


def code_points(spec: FontSpec) -> tuple[list[int], set[int]]:
    """(every code point of the font in ascending order, the subset that are emoji)"""
    if spec.chars:
        return sorted({ord(c) for c in spec.chars}), set()
    cps: set[int] = set()
    for lo, hi in R.load().text["renderable_ranges"]:
        cps.update(cp for cp in range(int(lo, 16), int(hi, 16) + 1) if cp >= 0x20)
    emoji = set(emoji_code_points())
    if spec.emoji == "colour":
        return sorted(cps - emoji), set()  # the fallback font draws them
    return sorted(cps | emoji), emoji


@dataclass
class Glyph:
    cp: int
    adv_w: int     # 1/16 px
    box_w: int
    box_h: int
    ofs_x: int
    ofs_y: int     # bottom of the box above the baseline
    bits: bytes    # rows packed continuously, MSB first


def pack(rows: list[list[int]], bpp: int) -> bytes:
    """Rows packed continuously, MSB first, `bpp` bits per pixel (values 0 .. 2**bpp - 1)."""
    out, acc, n = bytearray(), 0, 0
    for row in rows:
        for v in row:
            acc = (acc << bpp) | v
            n += bpp
            if n == 8:
                out.append(acc)
                acc, n = 0, 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def level(v: int, bpp: int) -> int:
    """8-bit coverage to a `bpp`-bit glyph value (1 bpp: thresholded at 50 %)."""
    return (1 if v >= THRESHOLD else 0) if bpp == 1 else (v * 15 + 127) // 255


def render(spec: FontSpec, path: Path, emoji_path: Path) -> tuple[list[Glyph], int, int, list[int]]:
    font = ImageFont.truetype(str(path), spec.size)
    cmap = TTFont(str(path))["cmap"].getBestCmap()
    efont = ImageFont.truetype(str(emoji_path), spec.size)
    if spec.emoji_wght:
        efont.set_variation_by_axes([spec.emoji_wght])
    ecmap = TTFont(str(emoji_path))["cmap"].getBestCmap()
    ascent, descent = font.getmetrics()
    glyphs, missing = [], []
    pad = spec.size * 2
    cps, emoji = code_points(spec)
    for cp in cps:
        is_emoji = cp in emoji
        if is_emoji and cp not in ecmap:
            raise SystemExit(f"U+{cp:04X} is in registry text.emoji_ranges but not in {EMOJI_SOURCE}")
        if not is_emoji and (cp not in cmap or cp == 0x25A1):
            if cp != 0x25A1:
                missing.append(cp)
            glyphs.append(placeholder(cp, spec.size, spec.bpp))
            continue
        f = efont if is_emoji else font
        e_ascent, e_descent = f.getmetrics()
        ch = chr(cp)
        img = Image.new("L", (spec.size * 4 + 2 * pad, e_ascent + e_descent + 2 * pad), 0)
        d = ImageDraw.Draw(img)
        d.fontmode = "L"
        d.text((pad, pad), ch, font=f, fill=255)
        adv = round(f.getlength(ch) * 16)
        bbox = img.point(lambda v: 255 if level(v, spec.bpp) else 0).getbbox()
        if bbox is None:  # spaces
            glyphs.append(Glyph(cp, adv, 0, 0, 0, 0, b""))
            continue
        x0, y0, x1, y1 = bbox
        rows = [[level(img.getpixel((x, y)), spec.bpp) for x in range(x0, x1)] for y in range(y0, y1)]
        baseline = pad + e_ascent
        if is_emoji:
            adv = (max(x0 - pad, 0) + (x1 - x0) + 2) * 16
        glyphs.append(Glyph(cp, adv, x1 - x0, y1 - y0, x0 - pad, baseline - y1, pack(rows, spec.bpp)))
    return glyphs, ascent + descent, descent, missing


def placeholder(cp: int, size: int, bpp: int) -> Glyph:
    """U+25A1-style box: an outlined square from the baseline up to about cap height."""
    side = max(5, round(size * 0.6))
    ink = (1 << bpp) - 1
    rows = [[ink] * side] + [[ink] + [0] * (side - 2) + [ink] for _ in range(side - 2)] + [[ink] * side]
    return Glyph(cp, (side + 2) * 16, side, side, 1, 0, pack(rows, bpp))


def emit(spec: FontSpec, glyphs: list[Glyph], line_height: int, base_line: int, missing: list[int]) -> str:
    # cmaps: one FORMAT0_TINY table per run of consecutive code points
    runs: list[tuple[int, int, int]] = []  # (start, length, first glyph id)
    for gid, g in enumerate(glyphs, start=1):
        if runs and runs[-1][0] + runs[-1][1] == g.cp:
            runs[-1] = (runs[-1][0], runs[-1][1] + 1, runs[-1][2])
        else:
            runs.append((g.cp, 1, gid))
    bitmap_lines, dsc_lines, index = [], [], 0
    dsc_lines.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}, /* id 0 reserved */")
    for g in glyphs:
        if g.bits:
            hexs = ", ".join(f"0x{b:02x}" for b in g.bits)
            bitmap_lines.append(f"    /* U+{g.cp:04X} */ {hexs},")
        dsc_lines.append(f"    {{.bitmap_index = {index}, .adv_w = {g.adv_w}, .box_w = {g.box_w}, .box_h = {g.box_h}, "
                         f".ofs_x = {g.ofs_x}, .ofs_y = {g.ofs_y}}},")
        index += len(g.bits)
    cmap_lines = [f"    {{.range_start = 0x{s:04X}, .range_length = {n}, .glyph_id_start = {gid}, .unicode_list = NULL, "
                  f".glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}},"
                  for s, n, gid in runs]
    src_url = SOURCES[spec.source][0]
    miss = ", ".join(f"U+{c:04X}" for c in missing) or "none"
    if not spec.chars:
        miss = "U+25A1 and " + miss
    colour = spec.emoji == "colour" and not spec.chars
    if spec.chars:
        emoji_line, coverage = "", f"display characters {spec.chars!r}"
    elif colour:
        emoji_line = " Emoji: none here; the fallback font ui_font_emoji_colour draws them.\n *"
        coverage = "registry text.renderable_ranges without text.emoji_ranges"
    else:
        emoji_line = f" Emoji: {EMOJI_SOURCE} ({SOURCES[EMOJI_SOURCE][0]}), weight {spec.emoji_wght}.\n *"
        coverage = "registry text.renderable_ranges and text.emoji_ranges"
    depth = "1 bpp (anti-aliased, thresholded at 50 %)" if spec.bpp == 1 else f"{spec.bpp} bpp (anti-aliased)"
    fallback = "\n    .fallback = &ui_font_emoji_colour," if colour else ""
    extern = "\nextern const lv_font_t ui_font_emoji_colour;\n" if colour else ""
    return f"""/* GENERATED by tools/assets/font_gen.py - DO NOT EDIT.
 * {spec.source} ({src_url}), {spec.size} px, {depth}.
 *{emoji_line} Font licences: SIL Open Font License 1.1 (LICENSE-NotoSans.txt, LICENSE-NotoEmoji.txt next to this file).
 * Coverage: {coverage}, {len(glyphs)} glyphs; drawn as the placeholder box: {miss}.
 */
#include "lvgl.h"
{extern}
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{
{chr(10).join(bitmap_lines)}
}};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
{chr(10).join(dsc_lines)}
}};

static const lv_font_fmt_txt_cmap_t cmaps[] = {{
{chr(10).join(cmap_lines)}
}};

static const lv_font_fmt_txt_dsc_t font_dsc = {{
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = {len(runs)},
    .bpp = {spec.bpp},
    .kern_classes = 0,
    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,
    .stride = 0,
}};

const lv_font_t {spec.name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height},
    .base_line = {base_line},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -1,
    .underline_thickness = 1,
    .static_bitmap = 1,
    .dsc = &font_dsc,{fallback}
}};
"""


class ColourEmoji:
    """Noto Color Emoji drawn into a cell of a given height (the font's 136 x 128 cell, scaled with
    premultiplied alpha so edges do not darken)."""

    def __init__(self, cell_h: int) -> None:
        self.font = ImageFont.truetype(str(fetch(COLOUR_SOURCE)), COLOUR_PPEM)
        _, descent = self.font.getmetrics()
        self.src_w, self.src_h = (round(v) for v in self.font.getbbox("\U0001F600")[2:])
        k = cell_h / self.src_h
        self.base_line = round(descent * k)  # cell rows below the baseline
        self.size = (round(self.src_w * k), cell_h)

    def image(self, cp: int) -> Image.Image:
        img = Image.new("RGBA", (self.src_w, self.src_h))
        ImageDraw.Draw(img).text((0, 0), chr(cp), font=self.font, embedded_color=True)
        return img.convert("RGBa").resize(self.size, Image.LANCZOS).convert("RGBA")


def rgb565a8(img: Image.Image) -> bytes:
    """LVGL's RGB565A8 layout: the RGB565 plane (little-endian) then the alpha plane; colour 0 where alpha is 0."""
    rgb, alpha = bytearray(), bytearray()
    for r, g, b, a in img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata():
        c = 0 if a == 0 else ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | (b * 31 + 127) // 255
        rgb += c.to_bytes(2, "little")
        alpha.append(a)
    return bytes(rgb + alpha)


def emoji_colour() -> str:
    """ui_font_emoji_colour's data: per emoji an RGB565A8 image cropped to its ink, and its place on the pen."""
    ce = ColourEmoji(EMOJI_CELL)
    base_line = ce.base_line
    data, table, images, offset, raw_total = [], [], [], 0, 0
    for cp in emoji_code_points():
        img = ce.image(cp)
        x0, y0, x1, y1 = img.getbbox() or (0, 0, 1, 1)
        img = img.crop((x0, y0, x1, y1))
        w, h = img.size
        raw = rgb565a8(img)
        packed = lz4_block.compress(raw)
        assert lz4_block.decompress(packed, len(raw)) == raw, f"U+{cp:04X}"
        # lv_image_compressed_t: method LZ4 (2) in the low 4 bits, compressed size, decompressed size
        blob = (2).to_bytes(4, "little") + len(packed).to_bytes(4, "little") + len(raw).to_bytes(4, "little") + packed
        raw_total += len(raw)
        data.append(f'static LV_ATTRIBUTE_LARGE_CONST const alignas(4) uint8_t e{cp:X}[] = "' +
                    "".join(f"\\x{v:02x}" for v in blob) + '";')
        # ofs_x: 1 px before the ink, ofs_y: bottom of the box above the baseline, advance: ink + 1 px each side
        table.append(f"    {{0x{cp:04X}, 1, {EMOJI_CELL - base_line - y1}, {w + 2}}},")
        images.append(f"    {{.header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8, "
                      f".flags = LV_IMAGE_FLAGS_COMPRESSED, .w = {w}, .h = {h}, .stride = {2 * w}}}, "
                      f".data_size = {len(blob)}, .data = e{cp:X}}},")
        offset += len(blob)
    url = SOURCES[COLOUR_SOURCE][0]
    return f"""/* GENERATED by tools/assets/font_gen.py - DO NOT EDIT.
 * {COLOUR_SOURCE} ({url}): its {COLOUR_PPEM} px colour bitmaps scaled to a {EMOJI_CELL} px cell, RGB565A8,
 * LZ4-compressed ({raw_total} bytes of pixels before compression; LVGL's image decoder unpacks them).
 * Font licence: SIL Open Font License 1.1 (LICENSE-NotoColorEmoji.txt next to this file).
 * Coverage: registry text.emoji_ranges, {len(table)} emoji, {offset} bytes stored.
 */
#include <stdalign.h>

#include "../src/ui_emoji.h"

/* one array per emoji: the 12-byte lv_image_compressed_t header, then the LZ4 block */
{chr(10).join(data)}

const ui_emoji_t ui_emoji[] = {{
{chr(10).join(table)}
}};

const lv_image_dsc_t ui_emoji_img[] = {{
{chr(10).join(images)}
}};

const uint32_t ui_emoji_count = {len(table)};

const lv_font_t ui_font_emoji_colour = {{
    .get_glyph_dsc = ui_emoji_glyph_dsc,
    .get_glyph_bitmap = ui_emoji_glyph_bitmap,
    .line_height = {EMOJI_CELL},
    .base_line = {base_line},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -1,
    .underline_thickness = 1,
    .static_bitmap = 1,
    .dsc = NULL,
}};
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="fail if a committed font differs from a fresh build")
    args = ap.parse_args()
    stale = []
    for spec in FONTS:
        glyphs, lh, bl, missing = render(spec, fetch(spec.source), fetch(EMOJI_SOURCE))
        text = emit(spec, glyphs, lh, bl, missing)
        out = OUT / f"{spec.name}.c"
        if args.check:
            if not out.exists() or out.read_text() != text:
                stale.append(out)
        else:
            OUT.mkdir(parents=True, exist_ok=True)
            out.write_text(text)
            size = sum(len(g.bits) for g in glyphs)
            print(f"{out.relative_to(REPO)}: {len(glyphs)} glyphs, {size} bitmap bytes, line height {lh}, "
                  f"missing {len(missing)}")
    text = emoji_colour()
    if args.check:
        if not EMOJI_COLOUR_OUT.exists() or EMOJI_COLOUR_OUT.read_text() != text:
            stale.append(EMOJI_COLOUR_OUT)
    else:
        EMOJI_COLOUR_OUT.write_text(text)
        print(f"{EMOJI_COLOUR_OUT.relative_to(REPO)}: {len(text) // 1024} KB of C")
    if stale:
        print("stale fonts (run tools/assets/font_gen.py):", *stale, sep="\n  ")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
