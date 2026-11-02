"""Golden images of the simulated screens, rendered headless: the Paper (tests/golden/paper/200x200/*.png,
grey) and the LCD (tests/golden/lcd/296x240/*.png, RGB).

Compared by decoded pixels (zlib output could differ between zlib versions); every scene is also
rendered twice and must give the same PNG bytes. Regenerate with `pytest tools --update-golden`
after an intended UI change, and look at the images before committing them. On a mismatch the
actual image is written to build/golden-actual/.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from fake_board import png, scenes, scenes_lcd

REPO = Path(__file__).resolve().parents[2]
GOLDEN = REPO / "tests" / "golden"
ACTUAL = REPO / "build" / "golden-actual"
# board: (scenes, golden directory, width, height, channels)
# Images live in tests/golden/<variant>/<width>x<height>/: a new size is a new folder.
BOARDS = {"paper": (scenes.SCENES, GOLDEN / "paper" / "200x200", 200, 200, 1),
          "lcd": (scenes_lcd.SCENES, GOLDEN / "lcd" / "296x240", 296, 240, 3)}
CASES = [(board, scene) for board, (sc, *_) in BOARDS.items() for scene in sc]


@pytest.mark.parametrize("board,scene", CASES, ids=[f"{b}-{s}" for b, s in CASES])
def test_golden(fb_libs, board: str, scene: str, request: pytest.FixtureRequest) -> None:
    sc, directory, w, h, ch = BOARDS[board]
    frames = sc[scene]()
    again = sc[scene]()
    assert [png.encode(p, w, h, 1, ch) for _, p in frames] == [png.encode(p, w, h, 1, ch) for _, p in again], \
        "rendering is not deterministic"
    update = request.config.getoption("--update-golden")
    for name, pixels in frames:
        path = directory / f"{name}.png"
        if update:
            directory.mkdir(parents=True, exist_ok=True)
            png.write(path, pixels, w, h, 1, ch)
            continue
        assert path.exists(), f"{path} missing: run pytest tools --update-golden"
        gw, gh, want = png.read(path)
        if (gw, gh, want) != (w, h, pixels):
            out = ACTUAL / board
            out.mkdir(parents=True, exist_ok=True)
            png.write(out / f"{name}.png", pixels, w, h, 1, ch)
            diff = sum(1 for a, b in zip(want, pixels) if a != b) // ch if len(want) == len(pixels) else -1
            pytest.fail(f"{board}/{name}: {diff} pixels differ (actual image in {out})")


@pytest.mark.parametrize("board", list(BOARDS))
def test_golden_files_are_all_used(fb_libs, board: str) -> None:
    sc, directory, *_ = BOARDS[board]
    names = {name for fn in sc.values() for name, _ in fn()}
    assert {p.stem for p in directory.glob("*.png")} == names
