"""Draw the digit images of the built-in faces "Big", "Minimal" and "Digital"
(components/ui_common/faces/<face>/digits-*/).

The PNGs are checked in (the face source is what people copy); this script only says how they were made:
    python tools/clock_face/make_builtin_images.py
Black ink on a transparent background: mono1 masks, drawn in the element's colour.

Big: Noto Sans Bold (the project's font, .deps/fonts, fetched by tools/assets/font_gen.py), hours over minutes.
Minimal: Ubuntu Light (the system font package `fonts-ubuntu`), thin digits and a colon in one row.
Digital: upright seven-segment digits drawn here (no font); a large set for the time and a small one for the day;
a thermometer and a drop for the room's temperature and humidity (the Paper).
Every digit of a set has the same cell, so a time never jumps sideways when a digit changes.
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[2]
FACES = REPO / "components/ui_common/faces"
NOTO_BOLD = REPO / ".deps/fonts/NotoSans-Bold.ttf"
UBUNTU_LIGHT = Path("/usr/share/fonts/truetype/ubuntu/Ubuntu-L.ttf")
SS = 8  # supersampling: drawn 8 times larger, then reduced and cut at half ink


def make(folder: Path, font_file: Path, height: int, colon: bool) -> None:
    """Digits `height` pixels tall (the union of their ink), each in a cell as wide as the widest digit's ink."""
    if not font_file.exists():
        raise SystemExit(f"{font_file} is missing")
    probe = ImageFont.truetype(str(font_file), 1000)
    boxes = [probe.getbbox(str(d)) for d in range(10)]
    size = round(1000 * height * SS / (max(b[3] for b in boxes) - min(b[1] for b in boxes)))
    font = ImageFont.truetype(str(font_file), size)
    boxes = {ch: font.getbbox(ch) for ch in "0123456789:"}
    top = min(boxes[str(d)][1] for d in range(10))
    cell = -(-max(boxes[str(d)][2] - boxes[str(d)][0] for d in range(10)) // SS)
    folder.mkdir(parents=True, exist_ok=True)
    for old in folder.glob("*.png"):
        old.unlink()
    for ch in "0123456789:" if colon else "0123456789":
        left, _, right, _ = boxes[ch]
        w = cell if ch != ":" else -(-(right - left) // SS) + 6
        big = Image.new("L", (w * SS, height * SS), 0)
        ImageDraw.Draw(big).text(((w * SS - (right - left)) // 2 - left, -top), ch, font=font, fill=255)
        ink = big.resize((w, height), Image.Resampling.BOX).point(lambda v: 255 if v >= 128 else 0)
        im = Image.new("RGBA", (w, height), (0, 0, 0, 0))
        im.putalpha(ink)
        im.save(folder / ("colon.png" if ch == ":" else f"{ch}.png"), optimize=True)


SEGMENTS = {"0": "abcdef", "1": "bc", "2": "abdeg", "3": "abcdg", "4": "bcfg", "5": "acdfg", "6": "acdefg", "7": "abc",
            "8": "abcdefg", "9": "abcdfg"}


def _mask(big: Image.Image, w: int, h: int) -> Image.Image:
    """Reduce the supersampled drawing and cut it at half ink: a mask image."""
    ink = big.resize((w, h), Image.Resampling.BOX).point(lambda v: 255 if v >= 128 else 0)
    im = Image.new("RGBA", ink.size, (0, 0, 0, 0))
    im.putalpha(ink)
    return im


def segment_digit(w: int, h: int, t: int, segs: str) -> Image.Image:
    """One seven-segment digit as on a real display: six-sided bars `t` thick, each pointed at both ends, and between
    two neighbours a clear channel (`gap` pixels wide, measured across) where the pointed ends meet at 45 degrees.

    Every bar's tips lie on its centre line; the centre lines run through the middle of the outer bars, so the
    corner of the digit is where two centre lines cross, and each tip stops `d` short of that crossing."""
    W, H, T = w * SS, h * SS, t * SS
    big = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(big)
    half = T / 2
    gap = max(1.5, t / 4) * SS
    tip = gap / 2 ** 0.5  # how far a tip stops from the crossing of the two centre lines
    left, right, top, bot = half, W - half, half, H - half
    mid = H / 2

    def bar_h(cy: float) -> None:
        x1, x2 = left + tip, right - tip
        d.polygon([(x1, cy), (x1 + half, cy - half), (x2 - half, cy - half), (x2, cy), (x2 - half, cy + half), (x1 + half, cy + half)], fill=255)

    def bar_v(cx: float, y1: float, y2: float) -> None:
        d.polygon([(cx, y1), (cx + half, y1 + half), (cx + half, y2 - half), (cx, y2), (cx - half, y2 - half), (cx - half, y1 + half)], fill=255)

    for name, cy in (("a", top), ("g", mid), ("d", bot)):
        if name in segs:
            bar_h(cy)
    for name, cx, y1, y2 in (("f", left, top, mid), ("b", right, top, mid), ("e", left, mid, bot), ("c", right, mid, bot)):
        if name in segs:
            bar_v(cx, y1 + tip, y2 - tip)
    return _mask(big, w, h)


def segment_colon(w: int, h: int, t: int) -> Image.Image:
    big = Image.new("L", (w * SS, h * SS), 0)
    d = ImageDraw.Draw(big)
    x = (w - t) * SS // 2
    for cy in (h * SS * 3 // 10, h * SS * 7 // 10):
        d.rectangle([x, cy - t * SS // 2, x + t * SS - 1, cy + t * SS // 2 - 1], fill=255)
    return _mask(big, w, h)


def make_segments(folder: Path, w: int, h: int, t: int, colon_w: int = 0) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    for old in folder.glob("*.png"):
        old.unlink()
    for ch, segs in SEGMENTS.items():
        segment_digit(w, h, t, segs).save(folder / f"{ch}.png", optimize=True)
    if colon_w:
        segment_colon(colon_w, h, t).save(folder / "colon.png", optimize=True)


def ghost(folder: Path, spacing: int) -> Image.Image:
    """The unlit segments of a four-digit time: an 8 in every digit cell and the colon, one image, laid out as the `number`
    element lays out the time (`spacing` pixels between cells)."""
    eight, colon = Image.open(folder / "8.png"), Image.open(folder / "colon.png")
    cells = [eight, eight, colon, eight, eight]
    im = Image.new("RGBA", (sum(c.width for c in cells) + spacing * 4, eight.height), (0, 0, 0, 0))
    x = 0
    for c in cells:
        im.alpha_composite(c, (x, 0))
        x += c.width + spacing
    return im


def thermometer(w: int, h: int) -> Image.Image:
    """A tube with a round bulb, outlined, the bulb and the lower half of the tube filled."""
    W, H = w * SS, h * SS
    big = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(big)
    line, cx = 2 * SS, W // 2
    bulb, tube = W // 2 - SS // 2, W // 4 + SS // 2  # radii
    cy = H - bulb - SS // 2
    d.rounded_rectangle([cx - tube, SS // 2, cx + tube, cy], radius=tube, fill=255)
    d.ellipse([cx - bulb, cy - bulb, cx + bulb, cy + bulb], fill=255)
    d.rounded_rectangle([cx - tube + line, SS // 2 + line, cx + tube - line, cy], radius=tube - line, fill=0)
    d.ellipse([cx - bulb + line, cy - bulb + line, cx + bulb - line, cy + bulb - line], fill=0)
    core = max(SS, tube - line - SS)
    d.rectangle([cx - core, H * 2 // 5, cx + core, cy], fill=255)
    d.ellipse([cx - bulb + line + SS, cy - bulb + line + SS, cx + bulb - line - SS, cy + bulb - line - SS], fill=255)
    return _mask(big, w, h)


def drop(w: int, h: int) -> Image.Image:
    """A water drop: a point at the top, a round bottom, outlined."""
    W, H = w * SS, h * SS
    big = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(big)

    def shape(inset: int, fill: int) -> None:
        r = W // 2 - SS // 2 - inset
        cx, cy = W // 2, H - SS // 2 - inset - r
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill)
        d.polygon([(cx, SS // 2 + inset * 2), (cx - r * 0.96, cy - r * 0.3), (cx + r * 0.96, cy - r * 0.3)], fill=fill)

    shape(0, 255)
    shape(2 * SS, 0)
    return _mask(big, w, h)


if __name__ == "__main__":
    make_segments(FACES / "digital/digits-paper", 36, 68, 8, 12)
    make_segments(FACES / "digital/small-paper", 14, 26, 4)
    make_segments(FACES / "digital/digits-lcd", 48, 88, 10, 16)
    make_segments(FACES / "digital/small-lcd", 16, 30, 4)
    (FACES / "digital/icons").mkdir(exist_ok=True)
    ghost(FACES / "digital/digits-lcd", 4).save(FACES / "digital/icons/ghost-lcd.png", optimize=True)
    thermometer(12, 24).save(FACES / "digital/icons/thermometer.png", optimize=True)
    drop(16, 22).save(FACES / "digital/icons/drop.png", optimize=True)
    make(FACES / "big/digits-paper", NOTO_BOLD, 80, colon=False)
    make(FACES / "big/digits-lcd", NOTO_BOLD, 96, colon=False)
    make(FACES / "minimal/digits-paper", UBUNTU_LIGHT, 50, colon=True)
    print("wrote", FACES)
