"""Draw the seven-segment digit images of the built-in face "Big" (components/ui_common/faces/big/digits-*/).

The PNGs are checked in (the face source is what people copy); this script only says how they were made:
    python tools/clock_face/make_builtin_images.py
Black ink on a transparent background: mono1 masks, drawn in the element's colour.
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

FACES = Path(__file__).resolve().parents[2] / "components/ui_common/faces"
SEGMENTS = {"0": "abcdef", "1": "bc", "2": "abdeg", "3": "abcdg", "4": "bcfg", "5": "acdfg", "6": "acdefg", "7": "abc",
            "8": "abcdefg", "9": "abcdfg"}


def digit(w: int, h: int, t: int, segs: str) -> Image.Image:
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    g, half = 2, t // 2

    def bar_h(cy: int) -> None:
        x1, x2 = half + g, w - 1 - half - g
        d.polygon([(x1 - half, cy), (x1, cy - half), (x2, cy - half), (x2 + half, cy), (x2, cy + half), (x1, cy + half)],
                  fill=(0, 0, 0, 255))

    def bar_v(cx: int, y1: int, y2: int) -> None:
        d.polygon([(cx, y1 - half), (cx + half, y1), (cx + half, y2), (cx, y2 + half), (cx - half, y2), (cx - half, y1)],
                  fill=(0, 0, 0, 255))

    top, mid, bot = half, h // 2, h - 1 - half
    left, right = half, w - 1 - half
    if "a" in segs:
        bar_h(top)
    if "g" in segs:
        bar_h(mid)
    if "d" in segs:
        bar_h(bot)
    if "f" in segs:
        bar_v(left, top + half + g, mid - half - g)
    if "b" in segs:
        bar_v(right, top + half + g, mid - half - g)
    if "e" in segs:
        bar_v(left, mid + half + g, bot - half - g)
    if "c" in segs:
        bar_v(right, mid + half + g, bot - half - g)
    return im


def colon(w: int, h: int, t: int) -> Image.Image:
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x = (w - t) // 2
    for cy in (h // 3, 2 * h // 3):
        d.rectangle([x, cy - t // 2, x + t - 1, cy + t // 2 - 1], fill=(0, 0, 0, 255))
    return im


def make(folder: Path, w: int, h: int, t: int, colon_w: int) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    for ch, segs in SEGMENTS.items():
        digit(w, h, t, segs).save(folder / f"{ch}.png", optimize=True)
    colon(colon_w, h, t).save(folder / "colon.png", optimize=True)


if __name__ == "__main__":
    make(FACES / "big/digits-paper", 36, 80, 9, 14)
    make(FACES / "big/digits-lcd", 48, 80, 10, 20)
    print("wrote", FACES / "big")
