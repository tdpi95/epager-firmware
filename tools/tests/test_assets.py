"""The committed fonts match a fresh run of tools/assets/font_gen.py (needs the downloaded source
fonts in .deps/fonts; skipped otherwise, since the generator would download them)."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def test_fonts_up_to_date() -> None:
    sources = ("NotoSans-Regular.ttf", "NotoSans-Bold.ttf", "NotoEmoji[wght].ttf", "NotoColorEmoji.ttf")
    if not all((REPO / ".deps" / "fonts" / f).exists() for f in sources):
        pytest.skip("source fonts not downloaded (run tools/assets/font_gen.py once)")
    r = subprocess.run([sys.executable, str(REPO / "tools" / "assets" / "font_gen.py"), "--check"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
