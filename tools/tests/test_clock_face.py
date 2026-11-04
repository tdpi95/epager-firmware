"""Clock faces: the Python reference against the vector file, the compiler and the built-in faces, and the fake board's
`face` console commands. The C parser and evaluator run the same vectors in tests/host/test_clock_face.c; the renderer's
pixels are the golden images (face_*.png)."""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

from clock_face import build as B
from epager_proto import clockface as CF
from epager_proto import registry as R
from fake_board import console, loopback, sample_faces, scenes, scenes_lcd
from fake_board.board import BoardConfig, FakeBoard

REPO = Path(__file__).resolve().parents[2]
FACES = REPO / "components" / "ui_common" / "faces"
VECTORS = json.loads((R.CONTRACTS / "test-vectors" / "clock_faces.json").read_text())
FILES = {name: bytes.fromhex(f["file_hex"]) for name, f in VECTORS["faces"].items()}


def _file(case: dict) -> bytes:
    return FILES[case["face"]] if "face" in case else bytes.fromhex(case["file_hex"])


def _limits(case: dict) -> CF.Limits:
    lim = CF.LIMITS[case["board"]]
    if "max_file" in case:
        lim = CF.Limits(lim.width, lim.height, case["max_file"], lim.formats, lim.arcs)
    return lim


# --------------------------------------------------------------------------- the reference against the vectors

@pytest.mark.parametrize("case", VECTORS["parse"], ids=[c["name"] for c in VECTORS["parse"]])
def test_parse_vectors(case: dict) -> None:
    try:
        f = CF.parse_face(_file(case), _limits(case), case["id"])
    except CF.FaceError as e:
        assert e.status == case["expect"], e.reason
        return
    assert case["expect"] == "OK"
    s = case["summary"]
    assert (f.meta["id"], f.meta["name"], f.meta["version"], len(f.images), len(f.options), len(f.elements)) == \
        (s["id"], s["name"], s["version"], s["images"], s["options"], s["elements"])
    assert CF.draws_source(f, "update_available") == s["draws_update_available"]


@pytest.mark.parametrize("case", VECTORS["options"], ids=[c["name"] for c in VECTORS["options"]])
def test_option_vectors(case: dict) -> None:
    f = CF.parse_face(FILES[case["face"]], CF.PAPER, "test.full")
    values = CF.resolve_opts(f, bytes.fromhex(case["pairs_hex"]))
    assert values == case["values"]
    assert CF.pack_opts(f, values).hex() == case["packed_hex"]


@pytest.mark.parametrize("case", VECTORS["eval"], ids=[c["name"] for c in VECTORS["eval"]])
def test_eval_vectors(case: dict) -> None:
    lim = CF.LCD if case["face"].startswith("lcd") else CF.PAPER
    f = CF.parse_face(FILES[case["face"]], lim, "test.lcd" if lim is CF.LCD else "test.full")
    values = CF.resolve_opts(f, bytes.fromhex(case["pairs_hex"]))
    assert CF.evaluate(f, values, case["data"]) == case["elements"]


def test_vectors_cover_every_element_kind_and_source() -> None:
    """The eval vectors draw something of every kind, and read every data source the contract defines."""
    f = CF.parse_face(FILES["paper_full"], CF.PAPER, "test.full")
    kinds = {e["kind"] for e in f.elements}
    for name in ("rect", "line", "image", "number", "text", "icon"):
        assert CF.ENUM["face_element"][name] in kinds, name
    lcd = CF.parse_face(FILES["lcd_full"], CF.LCD, "test.lcd")
    assert CF.ENUM["face_element"]["arc"] in {e["kind"] for e in lcd.elements}
    used = {e.get("source") for e in f.elements} | {e.get("source") for e in lcd.elements}
    used |= {c.get("source") for e in f.elements for c in e.get("visible_if", [])}
    implied_by_icons = ("weather_condition", "ble_connected", "wifi_connected")  # icons read these themselves
    missing = [n for n, v in CF.ENUM["face_source"].items()
               if v and v not in used and n not in implied_by_icons + ("update_available",)]
    assert missing == [], f"sources without an element in the vector faces: {missing}"


def test_reference_arithmetic_matches_c_division() -> None:
    assert CF._cdiv(-7, 2) == -3 and CF._cdiv(7, -2) == -3 and CF._cdiv(-7, -2) == 3
    assert CF._shown_c10(-45, False) == -5  # -4.5 rounds away from zero
    assert CF._shown_c10(100, True) == 50   # 10 C = 50 F
    assert CF._shown_c10(-125, True) == 10  # -12.5 C = 9.5 F


# --------------------------------------------------------------------------- the compiler and the built-in faces

@pytest.mark.parametrize("face,board", [("big", "paper"), ("big", "lcd"), ("minimal", "paper"), ("night", "lcd")])
def test_builtin_faces_build_and_fit(face: str, board: str) -> None:
    data = B.build_file(FACES / face, board)
    lim = CF.LIMITS[board]
    f = CF.parse_face(data, lim, json.loads((FACES / face / "face.json").read_text())["meta"]["id"])
    assert len(data) < 8192, "built-in faces stay small (they are linked into the firmware)"
    assert len(f.elements) <= CF.MAX_ELEMENTS and f.meta["min_contract"] == "0.15.0"
    assert B.build_file(FACES / face, board) == data, "building is deterministic"


def test_big_uses_an_image_digit_set_and_minimal_does_not() -> None:
    big = CF.parse_face(B.build_file(FACES / "big", "paper"), CF.PAPER, "epager.big")
    assert len(big.sets[1]) == 11 and all(big.images[i]["format"] == 1 for i in big.sets[1].values())
    assert len(CF.parse_face(B.build_file(FACES / "minimal", "paper"), CF.PAPER, "epager.minimal").images) == 0


def test_generated_builtin_source_is_up_to_date() -> None:
    r = subprocess.run([sys.executable, str(REPO / "tools" / "clock_face" / "gen_builtin.py"), "--check"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_digit_images_are_the_checked_in_generator_output(tmp_path: Path) -> None:
    from PIL import Image, ImageChops

    sys.path.insert(0, str(REPO / "tools" / "clock_face"))
    import make_builtin_images as M

    M.make(tmp_path, 36, 80, 9, 14)
    for p in sorted(tmp_path.glob("*.png")):
        a = Image.open(p).convert("RGBA")
        b = Image.open(FACES / "big" / "digits-paper" / p.name).convert("RGBA")
        assert ImageChops.difference(a, b).getbbox() is None, p.name


def test_compiler_refuses_bad_sources(tmp_path: Path) -> None:
    src = {"meta": {"id": "t.t", "name": "T", "version": "1.0.0"},
           "boards": {"paper": {"elements": [{"kind": "rect", "x": 0, "y": 0, "w": 10, "h": 10}]}}}
    B.build(src, tmp_path, "paper")
    with pytest.raises(B.SourceError, match="no entry"):
        B.build(src, tmp_path, "lcd")
    for bad, why in (({"kind": "blob"}, "face_element"), ({"kind": "text", "x": 0, "y": 0, "w": 9, "h": 9, "font": "huge",
                                                            "literal": "x"}, "face_font"),
                     ({"kind": "rect", "x": 0, "y": 0, "w": 9, "h": 9, "colour": "pink"}, "colour"),
                     ({"kind": "image", "image": "nope"}, "image list"),
                     ({"kind": "number", "set": "nope", "source": "time"}, "digit set")):
        s = json.loads(json.dumps(src))
        s["boards"]["paper"]["elements"] = [bad]
        with pytest.raises(B.SourceError, match=why):
            B.build(s, tmp_path, "paper")
    s = json.loads(json.dumps(src))
    s["boards"]["paper"]["elements"] = [{"kind": "rect", "x": 0, "y": 0, "w": 300, "h": 10}]
    with pytest.raises(CF.FaceError, match="box leaves the screen"):  # the tool runs the board's own checks
        B.build(s, tmp_path, "paper")


def test_mono1_packing_is_ink_black_on_transparent() -> None:
    from PIL import Image

    im = Image.new("RGBA", (10, 2), (0, 0, 0, 0))
    im.putpixel((0, 0), (0, 0, 0, 255))      # black ink
    im.putpixel((9, 1), (255, 255, 255, 255))  # white is not ink
    im.putpixel((1, 1), (20, 20, 20, 255))   # near black is ink
    data = B.pack_mono1(im)
    assert len(data) == 4
    assert data[0] == 0b01111111 and data[1] == 0b11000000  # row 0: pixel 0 ink = bit 0; padding bits 0
    assert data[2] == 0b10111111 and data[3] == 0b11000000


def test_cli_build_and_check(tmp_path: Path) -> None:
    out = tmp_path / "big.epface"
    env = {"PYTHONPATH": str(REPO / "tools")}
    r = subprocess.run([sys.executable, "-m", "clock_face", "build", str(FACES / "big"), "--board", "paper", "-o", str(out)],
                       capture_output=True, text=True, cwd=REPO / "tools", env=env)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([sys.executable, "-m", "clock_face", "check", str(out), "--board", "paper", "--id", "epager.big"],
                       capture_output=True, text=True, cwd=REPO / "tools", env=env)
    assert r.returncode == 0 and "epager.big 1.0.0" in r.stdout
    r = subprocess.run([sys.executable, "-m", "clock_face", "check", str(out), "--board", "lcd"], capture_output=True,
                       text=True, cwd=REPO / "tools", env=env)
    assert r.returncode == 1 and "screen size" in r.stderr


# --------------------------------------------------------------------------- the fake board

def test_console_face_commands_on_the_paper(fb_libs, tmp_path: Path) -> None:
    b = scenes.face_board()
    lines: list[str] = []
    run = lambda cmd: console.run_command(b, cmd, lines.append)  # noqa: E731
    run("face list")
    assert lines[-1] == "(no faces installed)"
    f = tmp_path / "mine.epface"
    f.write_bytes(sample_faces.big_variant("paper", "me.mine"))
    run(f"face install {f}")
    assert "me.mine installed" in lines[-1]
    run("face list")
    assert lines[-1].endswith("me.mine " + str(f.stat().st_size) + " bytes")
    run("face select me.mine")
    assert b.clock_face_shown() == "me.mine" and "me.mine" in lines[-1]
    run("face opt 2 0")  # the date off
    assert "Sunday" not in b.screen_text()
    run("face select epager.minimal")
    assert b.clock_face_shown() == "epager.minimal"
    run("face select me.mine")  # its option comes back
    assert "Sunday" not in b.screen_text()
    run("face delete me.mine")
    assert b.clock_face_shown() == "" and b.clock_face_errors() == 1  # deleted while shown: Classic
    run("face select")
    assert b.clock_face_shown() == "" and "classic" in lines[-1]


def test_installed_face_listing_has_the_asset_type_and_id(fb_libs) -> None:
    b = FakeBoard(BoardConfig(screen=True), clock="manual")
    loopback.install_clock_face(b, "me.mine", sample_faces.big_variant("paper", "me.mine"))
    items = loopback.asset_list(b, loopback.ASSET_TYPE_CLOCK_FACE)
    assert [(a["asset_type"], a["id"]) for a in items] == [(6, "me.mine")]
    loopback.delete_clock_face(b, "me.mine")
    assert loopback.asset_list(b, loopback.ASSET_TYPE_CLOCK_FACE) == []


def test_lcd_scene_faces_are_installable(fb_libs) -> None:
    b = scenes_lcd.face_board()
    loopback.install_clock_face(b, "me.big", sample_faces.big_variant("lcd", "me.big"))
    scenes_lcd.select_face(b, "me.big")
    assert b.clock_face_shown() == "me.big"
