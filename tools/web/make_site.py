#!/usr/bin/env python3
"""Assemble the firmware flashing site (GitHub Pages) from ESP-IDF build directories.

    tools/.venv/bin/python tools/web/make_site.py --out build/site \
        --board paper=build/paper --board lcd=build/lcd

For every board it reads `<build>/flasher_args.json`, checks the files and their offsets, copies the
parts (bootloader, partition table, otadata, app) under content-hashed names into
`<out>/firmware/<board>/`, and writes an ESP Web Tools manifest next to them. `<out>/firmware/index.json`
is what the page (web/index.html) reads: version, sizes, signed or not, per board. The static page
files from `web/` are copied to the root of `<out>`.

Standard library only, so the CI job needs nothing besides Python.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# esptool chip name -> ESP Web Tools chipFamily
CHIP_FAMILIES = {
    "esp32": "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c3": "ESP32-C3",
    "esp32c6": "ESP32-C6",
    "esp32h2": "ESP32-H2",
}

BOARD_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]*$")
IMAGE_MAGIC = 0xE9  # first byte of an ESP application or bootloader image
PARTITION_TABLE_MAGIC = b"\xaa\x50"
SIGNED_OPTION = "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y"


class SiteError(Exception):
    pass


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_sdkconfig_flag(build_dir: Path, line: str) -> bool:
    sdkconfig = build_dir / "sdkconfig"
    if not sdkconfig.is_file():
        return False
    return line in sdkconfig.read_text(encoding="utf-8", errors="replace").splitlines()


def project_version(build_dir: Path) -> str:
    desc = build_dir / "project_description.json"
    if not desc.is_file():
        raise SiteError(f"{desc} not found (pass --version, or build the project first)")
    version = json.loads(desc.read_text(encoding="utf-8")).get("project_version", "")
    if not version:
        raise SiteError(f"no project_version in {desc}")
    return version


def check_part(offset: int, path: Path) -> None:
    """Cheap sanity checks so a wrong file never reaches a board through the page."""
    head = path.read_bytes()[:2]
    if offset == 0x8000:
        if head != PARTITION_TABLE_MAGIC:
            raise SiteError(f"{path.name}: not a partition table (offset 0x8000)")
    elif path.name.startswith(("bootloader", "epager_")):
        if not head or head[0] != IMAGE_MAGIC:
            raise SiteError(f"{path.name}: not an ESP image (bad magic byte)")


def load_parts(build_dir: Path) -> tuple[list[tuple[int, Path]], dict]:
    args_file = build_dir / "flasher_args.json"
    if not args_file.is_file():
        raise SiteError(f"{args_file} not found: build the firmware first")
    args = json.loads(args_file.read_text(encoding="utf-8"))
    parts: list[tuple[int, Path]] = []
    for offset_text, rel in args.get("flash_files", {}).items():
        offset = int(offset_text, 0)
        path = build_dir / rel
        if not path.is_file():
            raise SiteError(f"{path} is listed in flasher_args.json but missing")
        check_part(offset, path)
        parts.append((offset, path))
    if not parts:
        raise SiteError(f"{args_file} lists no files")
    parts.sort()
    for (off_a, path_a), (off_b, _) in zip(parts, parts[1:]):
        if off_a + path_a.stat().st_size > off_b:
            raise SiteError(f"{path_a.name} overlaps the next part (0x{off_b:x})")
    return parts, args


def build_board(name: str, build_dir: Path, out_dir: Path, version: str | None) -> dict:
    if not BOARD_NAME_RE.match(name):
        raise SiteError(f"bad board name {name!r}")
    parts, args = load_parts(build_dir)
    chip = args.get("extra_esptool_args", {}).get("chip", "")
    if chip not in CHIP_FAMILIES:
        raise SiteError(f"{name}: unsupported chip {chip!r}")
    app = args.get("app", {})
    app_path = build_dir / app.get("file", "")
    if not app_path.is_file():
        raise SiteError(f"{name}: app image missing in flasher_args.json")
    version = version or project_version(build_dir)

    dest = out_dir / "firmware" / name
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)

    manifest_parts = []
    for offset, path in parts:
        digest = sha256_file(path)[:8]
        hashed = f"{path.stem}-{digest}{path.suffix}"
        shutil.copyfile(path, dest / hashed)
        manifest_parts.append({"path": hashed, "offset": offset})

    manifest = {
        "name": f"ePager {name}",
        "version": version,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,  # our firmware has no Improv serial
        "builds": [{"chipFamily": CHIP_FAMILIES[chip], "parts": manifest_parts}],
    }
    (dest / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    return {
        "version": version,
        "manifest": f"firmware/{name}/manifest.json",
        "chip": CHIP_FAMILIES[chip],
        "flash_size": args.get("flash_settings", {}).get("flash_size", ""),
        "app_size": app_path.stat().st_size,
        "app_sha256": sha256_file(app_path),
        "total_size": sum(p.stat().st_size for _, p in parts),
        "signed": read_sdkconfig_flag(build_dir, SIGNED_OPTION),
    }


def git_commit() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short=8", "HEAD"], cwd=ROOT, check=True,
                              capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def parse_board(spec: str) -> tuple[str, Path]:
    name, sep, path = spec.partition("=")
    if not sep or not name or not path:
        raise argparse.ArgumentTypeError(f"expected NAME=BUILD_DIR, got {spec!r}")
    return name, Path(path)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True, help="site directory to (re)create")
    ap.add_argument("--board", type=parse_board, action="append", required=True, metavar="NAME=BUILD_DIR",
                    help="a board (the variant name) and its ESP-IDF build directory; repeatable")
    ap.add_argument("--web", type=Path, default=ROOT / "web", help="static page files (default: web/)")
    ap.add_argument("--version", help="override the version read from the build")
    ap.add_argument("--commit", default=None, help="commit shown on the page (default: git HEAD)")
    ns = ap.parse_args(argv)

    names = [n for n, _ in ns.board]
    if len(set(names)) != len(names):
        print("error: a board is given twice", file=sys.stderr)
        return 2
    if not ns.web.is_dir():
        print(f"error: {ns.web} is not a directory", file=sys.stderr)
        return 2

    if ns.out.exists():
        shutil.rmtree(ns.out)
    ns.out.mkdir(parents=True)
    shutil.copytree(ns.web, ns.out, dirs_exist_ok=True)
    (ns.out / ".nojekyll").write_text("", encoding="utf-8")

    boards = {}
    try:
        for name, build_dir in ns.board:
            boards[name] = build_board(name, build_dir, ns.out, ns.version)
    except SiteError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    index = {
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "commit": ns.commit if ns.commit is not None else git_commit(),
        "boards": boards,
    }
    (ns.out / "firmware" / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    for name, info in boards.items():
        kind = "signed" if info["signed"] else "unsigned"
        print(f"{name}: {info['version']} ({kind}), {info['total_size']} bytes in the manifest")
    print(f"site written to {ns.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
