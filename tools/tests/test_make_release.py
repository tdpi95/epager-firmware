"""tools/release/make_release.py: the release manifest and image names, on synthetic build directories."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

from release import make_release

VERSION = (Path(__file__).resolve().parents[2] / "variants/paper/version.txt").read_text().strip()


def make_build(root: Path, variant: str = "paper", *, signed: bool = True, version: str = VERSION,
               magic: int = 0xE9) -> Path:
    build = root / variant
    build.mkdir(parents=True)
    (build / f"epager_{variant}.bin").write_bytes(bytes([magic]) + b"app" * 100)
    (build / "project_description.json").write_text(json.dumps({"project_version": version}))
    (build / "sdkconfig").write_text("CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y\n" if signed else "")
    return build


def run(tmp_path: Path, build: Path, extra: list[str] | None = None, version: str = VERSION) -> int:
    return make_release.main(["--version", version, "--out", str(tmp_path / "out"),
                              "--image", f"paper={build}"] + (extra or []))


def test_manifest_and_asset(tmp_path):
    assert run(tmp_path, make_build(tmp_path / "b")) == 0
    out = tmp_path / "out"
    manifest = json.loads((out / "manifest.json").read_text())
    (img,) = manifest["images"]
    assert img["board_id"] == "waveshare_s3_epaper_154_v2" and img["variant"] == "paper"
    assert img["file"] == f"epager-waveshare_s3_epaper_154_v2-{VERSION}.bin"
    data = (out / img["file"]).read_bytes()
    assert img["size"] == len(data) and img["sha256"] == hashlib.sha256(data).hexdigest()
    assert manifest["version"] == VERSION
    assert manifest["contract_version"] == (make_release.ROOT / "contracts/CONTRACT_VERSION").read_text().strip()


def test_unsigned_refused_unless_allowed(tmp_path):
    build = make_build(tmp_path / "b", signed=False)
    assert run(tmp_path, build) == 1
    assert run(tmp_path, build, ["--allow-unsigned"]) == 0


def test_version_mismatch_refused(tmp_path):
    assert run(tmp_path, make_build(tmp_path / "b"), version="9.9.9") == 1
    assert run(tmp_path, make_build(tmp_path / "c", version="9.9.9")) == 1


def test_bad_image_refused(tmp_path):
    assert run(tmp_path, make_build(tmp_path / "b", magic=0)) == 1


def test_bad_version_string_refused(tmp_path):
    assert run(tmp_path, make_build(tmp_path / "b"), version="01.2.3") == 1


def test_board_from_cmake_cache(tmp_path):
    build = make_build(tmp_path / "b")
    (build / "CMakeCache.txt").write_text("EPAGER_BOARD:UNINITIALIZED=ostb_xiaozhi_3st\n")
    assert run(tmp_path, build) == 0
    assert json.loads((tmp_path / "out/manifest.json").read_text())["images"][0]["board_id"] == "ostb_xiaozhi_3st"
