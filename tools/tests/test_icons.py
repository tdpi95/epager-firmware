"""App icons on the fake board: the console commands and the loopback installer (both go through the
real transfer protocol), the sample icons, and the generated built-in icons. The protocol flows
(`app_icons`, `app_icons_screen`) run in test_e2e.py."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

from epager_proto import assets
from fake_board import console, loopback, sample_icons, scenes
from fake_board.board import BoardConfig, FakeBoard
from test_client.flows import icon_region

REPO = Path(__file__).resolve().parents[2]


def _card(board: FakeBoard, app_id: str = "com.zing.zalo") -> None:
    scenes.post(board, 1, "Hello", app_id=app_id)
    scenes.settle(board)


@pytest.mark.parametrize("name", list(sample_icons.SAMPLES))
def test_sample_icons_are_valid_app_icons(name: str) -> None:
    rows = sample_icons.SAMPLES[name]()
    assert len(rows) == 24 and all(len(r) == 24 and set(r) <= {"#", "."} for r in rows)
    assert any("#" in r for r in rows) and any("." in r for r in rows)
    f = assets.app_icon(rows)
    assert len(f) == 88 and f[:4] == b"ESPA"


def test_console_icon_commands(fb_libs) -> None:
    b = scenes.board()
    lines: list[str] = []
    _card(b)
    before = icon_region(b.panel())
    for name in sample_icons.SAMPLES:
        console.run_command(b, f"icon com.zing.zalo {name}", lines.append)
        scenes.settle(b)
        assert icon_region(b.panel()) == sample_icons.SAMPLES[name](), name
    console.run_command(b, "icon-remove com.zing.zalo", lines.append)
    scenes.settle(b)
    assert icon_region(b.panel()) == before, "back to the category icon"
    assert lines == ["icon installed for com.zing.zalo"] * 3 + ["icon of com.zing.zalo deleted"]
    console.run_command(b, "icon-remove com.zing.zalo", lines.append)  # nothing installed: reported, not raised
    assert lines[-1].startswith("? ")


def test_loopback_install_runs_the_board_checks(fb_libs) -> None:
    b = scenes.board()
    loopback.install_app_icon(b, "com.zing.zalo", sample_icons.zalo())
    narrow = [r[:16] for r in sample_icons.zalo()]  # well-formed, but not the advertised 24x24
    loopback.install_app_icon(b, "com.example.narrow", narrow, want="IMAGE_INVALID")
    with pytest.raises(loopback.LoopbackError, match="IMAGE_INVALID"):  # the status is checked, not ignored
        loopback.install_app_icon(b, "com.example.narrow", narrow)


def test_icon_follows_the_app_id_case_sensitively(fb_libs) -> None:
    b = scenes.board()
    loopback.install_app_icon(b, "com.zing.zalo", sample_icons.zalo())
    _card(b, "com.zing.Zalo")
    assert icon_region(b.panel()) != sample_icons.zalo()


def test_loopback_needs_the_boards_free_link(fb_libs) -> None:
    """The loopback needs the board's one link: with an app connected it says so instead of failing oddly."""
    b = FakeBoard(BoardConfig(screen=True), clock="manual")

    class L:
        def notify(self, char: int, value: bytes) -> None: ...
        def drop(self) -> None: ...

    link = L()
    assert b.connect(link)
    with pytest.raises(loopback.LoopbackError, match="link already"):
        loopback.install_app_icon(b, "a", sample_icons.zalo())
    b.disconnect(link)


def test_builtin_icons_up_to_date() -> None:
    """The committed components/ui_common/src/ui_icons.c matches a fresh run of tools/assets/icon_gen.py."""
    r = subprocess.run([sys.executable, str(REPO / "tools" / "assets" / "icon_gen.py"), "--check"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_builtin_icons_are_distinct_and_cover_every_category() -> None:
    sys.path.insert(0, str(REPO / "tools" / "assets"))
    import icon_gen

    names = icon_gen.category_order()
    assert len(names) == 12 and names[0] == "other"
    rendered = [tuple(icon_gen.render(n)) for n in names]
    assert len(set(rendered)) == 12, "every category has its own icon"
    for rows in rendered:
        black = sum(r.count("#") for r in rows)
        assert 60 <= black <= 400, "neither blank nor a filled square"
