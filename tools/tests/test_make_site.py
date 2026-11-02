"""tools/web/make_site.py: the flashing page's manifests, checks and index, on synthetic build directories."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from web import make_site


def make_build(root: Path, *, chip: str = "esp32s3", signed: bool = False, bad_app: bool = False) -> Path:
    build = root / "build"
    (build / "bootloader").mkdir(parents=True)
    (build / "partition_table").mkdir()
    (build / "bootloader/bootloader.bin").write_bytes(b"\xe9" + b"b" * 99)
    (build / "partition_table/partition-table.bin").write_bytes(b"\xaa\x50" + b"p" * 30)
    (build / "ota_data_initial.bin").write_bytes(b"\xff" * 8192)
    (build / "epager_x.bin").write_bytes((b"\x00" if bad_app else b"\xe9") + b"a" * 1000)
    (build / "flasher_args.json").write_text(json.dumps({
        "flash_settings": {"flash_size": "8MB"},
        "flash_files": {"0x0": "bootloader/bootloader.bin", "0x20000": "epager_x.bin",
                        "0x8000": "partition_table/partition-table.bin", "0x19000": "ota_data_initial.bin"},
        "app": {"offset": "0x20000", "file": "epager_x.bin"},
        "extra_esptool_args": {"chip": chip},
    }))
    (build / "project_description.json").write_text(json.dumps({"project_version": "1.2.3"}))
    if signed:
        (build / "sdkconfig").write_text("CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y\n")
    return build


@pytest.fixture
def web(tmp_path: Path) -> Path:
    d = tmp_path / "web"
    d.mkdir()
    (d / "index.html").write_text("<html></html>")
    return d


def run(tmp_path: Path, web: Path, build: Path, extra: list[str] | None = None) -> int:
    return make_site.main(["--out", str(tmp_path / "site"), "--board", f"x={build}", "--web", str(web),
                           "--commit", "abc"] + (extra or []))


def test_manifest_parts_sorted_with_offsets(tmp_path, web):
    assert run(tmp_path, web, make_build(tmp_path / "b")) == 0
    site = tmp_path / "site"
    manifest = json.loads((site / "firmware/x/manifest.json").read_text())
    build = manifest["builds"][0]
    assert build["chipFamily"] == "ESP32-S3"
    assert [p["offset"] for p in build["parts"]] == [0, 0x8000, 0x19000, 0x20000]
    assert manifest["version"] == "1.2.3"
    for part in build["parts"]:
        assert (site / "firmware/x" / part["path"]).is_file()
    assert (site / "index.html").is_file() and (site / ".nojekyll").is_file()


def test_part_names_carry_a_content_hash(tmp_path, web):
    build = make_build(tmp_path / "b")
    assert run(tmp_path, web, build) == 0
    app = json.loads((tmp_path / "site/firmware/x/manifest.json").read_text())["builds"][0]["parts"][-1]["path"]
    digest = hashlib.sha256((build / "epager_x.bin").read_bytes()).hexdigest()
    assert app == f"epager_x-{digest[:8]}.bin"


def test_index_describes_the_board(tmp_path, web):
    assert run(tmp_path, web, make_build(tmp_path / "b", signed=True)) == 0
    index = json.loads((tmp_path / "site/firmware/index.json").read_text())
    info = index["boards"]["x"]
    assert index["commit"] == "abc"
    assert info["signed"] is True and info["version"] == "1.2.3" and info["flash_size"] == "8MB"
    assert info["manifest"] == "firmware/x/manifest.json"
    assert info["app_size"] == 1001 and info["total_size"] == 100 + 32 + 8192 + 1001


def test_unsigned_without_sdkconfig_flag(tmp_path, web):
    assert run(tmp_path, web, make_build(tmp_path / "b")) == 0
    assert json.loads((tmp_path / "site/firmware/index.json").read_text())["boards"]["x"]["signed"] is False


def test_version_override(tmp_path, web):
    assert run(tmp_path, web, make_build(tmp_path / "b"), ["--version", "9.9.9"]) == 0
    assert json.loads((tmp_path / "site/firmware/x/manifest.json").read_text())["version"] == "9.9.9"


def test_bad_app_magic_is_refused(tmp_path, web, capsys):
    assert run(tmp_path, web, make_build(tmp_path / "b", bad_app=True)) == 1
    assert "not an ESP image" in capsys.readouterr().err


def test_unknown_chip_is_refused(tmp_path, web, capsys):
    assert run(tmp_path, web, make_build(tmp_path / "b", chip="esp8266")) == 1
    assert "unsupported chip" in capsys.readouterr().err


def test_overlapping_parts_are_refused(tmp_path, web, capsys):
    build = make_build(tmp_path / "b")
    (build / "bootloader/bootloader.bin").write_bytes(b"\xe9" * 0x9000)  # runs into the table at 0x8000
    assert run(tmp_path, web, build) == 1
    assert "overlaps" in capsys.readouterr().err


def test_missing_build_is_refused(tmp_path, web, capsys):
    assert run(tmp_path, web, tmp_path / "nope") == 1
    assert "flasher_args.json" in capsys.readouterr().err


def test_duplicate_board_is_refused(tmp_path, web):
    build = make_build(tmp_path / "b")
    assert make_site.main(["--out", str(tmp_path / "s"), "--web", str(web),
                           "--board", f"x={build}", "--board", f"x={build}"]) == 2
