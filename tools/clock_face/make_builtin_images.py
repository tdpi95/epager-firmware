"""Draw the digit images of the built-in faces "Big" and "Minimal" (components/ui_common/faces/<face>/digits-*/).

The PNGs are checked in (the face source is what people copy); this script only says how they were made:
    python tools/clock_face/make_builtin_images.py
Black ink on a transparent background: mono1 masks, drawn in the element's colour.

Big: Noto Sans Bold (the project's font, .deps/fonts, fetched by tools/assets/font_gen.py), hours over minutes.
Minimal: Ubuntu Light (the system font package `fonts-ubuntu`), thin digits and a colon in one row.
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


if __name__ == "__main__":
    make(FACES / "big/digits-paper", NOTO_BOLD, 80, colon=False)
    make(FACES / "big/digits-lcd", NOTO_BOLD, 96, colon=False)
    make(FACES / "minimal/digits-paper", UBUNTU_LIGHT, 50, colon=True)
    print("wrote", FACES)
