#!/usr/bin/env python3
"""Assemble the files of a firmware GitHub Release (contracts/releases.md) from ESP-IDF build directories.

    tools/.venv/bin/python tools/release/make_release.py --version 0.2.0 --out build/release \
        --image paper=build/paper --image lcd=build/lcd

For every variant it takes the application image `<build>/epager_<variant>.bin` (signed by the build when
-DEPAGER_SIGN=ON), copies it to `<out>/epager-<board_id>-<version>.bin` and writes `<out>/manifest.json`
(validated against contracts/release-manifest.schema.json when `jsonschema` is installed).
`board_id` is the `BOARD_ID` of the board folder the variant was built for (the variant's default board,
or `EPAGER_BOARD` from the build's CMakeCache.txt).

Refuses an unsigned image (an OTA image must carry a signature block) unless --allow-unsigned, and a
build whose version differs from --version or from the variant's version.txt.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SIGNED_OPTION = "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y"
IMAGE_MAGIC = 0xE9
VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$")


class ReleaseError(Exception):
    pass


def default_board(variant: str) -> str:
    cmake = ROOT / "variants" / variant / "CMakeLists.txt"
    if not cmake.is_file():
        raise ReleaseError(f"no variant '{variant}' ({cmake} not found)")
    m = re.search(r"set\(EPAGER_DEFAULT_BOARD\s+(\S+?)\)", cmake.read_text(encoding="utf-8"))
    if not m:
        raise ReleaseError(f"{cmake}: no EPAGER_DEFAULT_BOARD")
    return m.group(1)


def board_folder(variant: str, build: Path) -> str:
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        m = re.search(r"^EPAGER_BOARD:[A-Z]+=(\S+)$", cache.read_text(encoding="utf-8", errors="replace"), re.M)
        if m:
            return m.group(1)
    return default_board(variant)


def board_id(folder: str) -> str:
    src = ROOT / "boards" / folder / "src" / "board.c"
    m = re.search(r'^#define\s+BOARD_ID\s+"([^"]+)"', src.read_text(encoding="utf-8"), re.M)
    if not m:
        raise ReleaseError(f"{src}: no BOARD_ID")
    return m.group(1)


def build_image(variant: str, build: Path, out: Path, version: str, allow_unsigned: bool) -> dict:
    app = build / f"epager_{variant}.bin"
    if not app.is_file():
        raise ReleaseError(f"{app} not found (build the {variant} variant first)")
    data = app.read_bytes()
    if not data or data[0] != IMAGE_MAGIC:
        raise ReleaseError(f"{app.name}: not an ESP application image (bad magic byte)")

    sdkconfig = build / "sdkconfig"
    signed = sdkconfig.is_file() and SIGNED_OPTION in sdkconfig.read_text(errors="replace").splitlines()
    if not signed and not allow_unsigned:
        raise ReleaseError(f"{variant}: the build is not signed (-DEPAGER_SIGN=ON); the board would refuse the OTA")

    version_txt = (ROOT / "variants" / variant / "version.txt").read_text(encoding="utf-8").strip()
    if version_txt != version:
        raise ReleaseError(f"{variant}: version.txt is {version_txt}, the release is {version}")
    desc = build / "project_description.json"
    built = json.loads(desc.read_text(encoding="utf-8")).get("project_version") if desc.is_file() else None
    if built != version:
        raise ReleaseError(f"{variant}: the build is version {built}, the release is {version} (stale build?)")

    bid = board_id(board_folder(variant, build))
    name = f"epager-{bid}-{version}.bin"
    shutil.copyfile(app, out / name)
    return {"board_id": bid, "variant": variant, "file": name, "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def validate(manifest: dict) -> None:
    ids = [i["board_id"] for i in manifest["images"]]
    if len(ids) != len(set(ids)):
        raise ReleaseError("duplicate board_id in the manifest")
    try:
        import jsonschema
    except ImportError:
        print("warning: jsonschema not installed, manifest not validated against the schema", file=sys.stderr)
        return
    schema = json.loads((ROOT / "contracts" / "release-manifest.schema.json").read_text(encoding="utf-8"))
    try:
        jsonschema.validate(manifest, schema)
    except jsonschema.ValidationError as e:
        raise ReleaseError(f"manifest.json fails the schema: {e.message}") from e


def parse_image(spec: str) -> tuple[str, Path]:
    variant, sep, path = spec.partition("=")
    if not sep or not variant or not path:
        raise argparse.ArgumentTypeError(f"expected VARIANT=BUILD_DIR, got {spec!r}")
    return variant, Path(path)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True, help="release version, the tag without the leading v")
    ap.add_argument("--out", type=Path, required=True, help="directory for the release assets (recreated)")
    ap.add_argument("--image", type=parse_image, action="append", required=True, metavar="VARIANT=BUILD_DIR")
    ap.add_argument("--notes-url", help="https:// link to the release notes")
    ap.add_argument("--allow-unsigned", action="store_true", help="accept unsigned builds (development only)")
    ns = ap.parse_args(argv)
    try:
        if not VERSION_RE.match(ns.version):
            raise ReleaseError(f"'{ns.version}' is not MAJOR.MINOR.PATCH[-pre]")
        if ns.out.exists():
            shutil.rmtree(ns.out)
        ns.out.mkdir(parents=True)
        manifest = {
            "manifest_version": 1,
            "version": ns.version,
            "contract_version": (ROOT / "contracts" / "CONTRACT_VERSION").read_text(encoding="utf-8").strip(),
            "images": [build_image(v, b, ns.out, ns.version, ns.allow_unsigned) for v, b in ns.image],
        }
        if ns.notes_url:
            manifest["notes_url"] = ns.notes_url
        validate(manifest)
    except ReleaseError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    (ns.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    for img in manifest["images"]:
        print(f"{img['file']}  {img['size']} bytes  sha256 {img['sha256']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
