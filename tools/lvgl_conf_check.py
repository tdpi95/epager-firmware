#!/usr/bin/env python3
"""Compare the effective LVGL configuration of the host build and the Paper firmware build.

Under ESP-IDF, every LV_* option that lv_conf.h does not set falls back to the Kconfig default
(lv_conf_internal.h, LV_KCONFIG_PRESENT) instead of LVGL's own default, so the same lv_conf.h can
configure the two builds differently. This script preprocesses lv_conf_internal.h in both
environments (-E -dM) and diffs every LV_* macro. Differences must be listed in ALLOWED with a
reason; anything else fails.

Needs the firmware build directory (build/paper, compile_commands.json) and the host LVGL sources
(.deps/lvgl-9.5.0, fetched by tests/host). Run inside the ESP-IDF environment.
Usage: tools/lvgl_conf_check.py   (EPAGER_FW_BUILD=<dir> for a firmware build directory other than build/paper)
"""
from __future__ import annotations

import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LVGL_HOST = REPO / ".deps" / "lvgl-9.5.0"
CONF_DIR = REPO / "components" / "ui_common" / "lv_conf"
FW_BUILD = Path(os.environ.get("EPAGER_FW_BUILD", REPO / "build" / "paper"))

# Differences that are expected: macro -> reason. Keep this list short and explained.
ALLOWED: dict[str, str] = {
    "LV_CONF_PATH": "firmware selects lv_conf.h by path, the host by include directory",
    "LV_LVGL_H_INCLUDE_SIMPLE": "include style only (host -D)",
    "LV_ASSERT_HANDLER_INCLUDE": "header that declares the assert handler; the handler itself is the same",
    "LV_BUILD_EXAMPLES": "the host build compiles only src/, the firmware turns examples off in sdkconfig",
    "LV_BUILD_DEMOS": "same as LV_BUILD_EXAMPLES",
    "LV_FS_DEFAULT_DRIVER_LETTER": "'\\0' vs 0: the same value (no default drive letter)",
    "LV_ATTRIBUTE_LARGE_RAM_ARRAY": "the firmware puts the 128 KB object pool in PSRAM (EXT_RAM_BSS_ATTR, D117); the host has no such section",
}

PROBE = '#include "lvgl.h"\n'


def norm(value: str) -> str:
    """Spelling-insensitive value: whitespace removed, integer expressions evaluated."""
    v = re.sub(r"\s+", "", value)
    plain = re.sub(r"(?<=[0-9a-fA-F])[uUlL]+\b", "", v)
    if re.fullmatch(r"[0-9a-fA-Fx*+()\-]+", plain):
        try:
            return str(eval(plain, {"__builtins__": {}}, {}))  # noqa: S307 - digits and operators only
        except (SyntaxError, NameError, TypeError):
            pass
    return v


def expand(cmd: list[str], cwd: str | None, tmp: Path) -> dict[str, str]:
    """Value of every LV_* object-like macro after full preprocessing (-E), not the macro text."""
    src = tmp / "probe.c"
    src.write_text(PROBE)
    defs = subprocess.run(cmd + ["-E", "-dM", str(src)], capture_output=True, text=True, cwd=cwd)
    if defs.returncode != 0:
        sys.exit(defs.stderr)
    names = sorted({m.group(1) for line in defs.stdout.splitlines()
                    if (m := re.match(r"#define (LV_[A-Za-z0-9_]+) ?", line))})
    src.write_text(PROBE + "".join(f'"{n}" {n}\n' for n in names))
    out = subprocess.run(cmd + ["-E", str(src)], capture_output=True, text=True, cwd=cwd)
    if out.returncode != 0:
        sys.exit(out.stderr)
    return {m.group(1): norm(m.group(2)) for line in out.stdout.splitlines()
            if (m := re.match(r'"(LV_[A-Za-z0-9_]+)" (.*)$', line))}


def host_macros(tmp: Path) -> dict[str, str]:
    cmd = ["gcc", "-std=gnu11", f"-I{LVGL_HOST}", f"-I{LVGL_HOST}/src", f"-I{CONF_DIR}",
           "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_LVGL_H_INCLUDE_SIMPLE"]
    return expand(cmd, None, tmp)


def firmware_macros(tmp: Path) -> dict[str, str]:
    db = json.loads((FW_BUILD / "compile_commands.json").read_text())
    entry = next(e for e in db if e["file"].endswith("components/ui_common/src/ui.c"))
    args = shlex.split(entry["command"])
    # drop output/dependency/source arguments, keep the compiler and every -D/-I/-include flag
    keep: list[str] = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "-MF", "-MT"):
            skip = True
            continue
        if a in ("-c", entry["file"]) or a.startswith(("-MD", "-MMD")):
            continue
        keep.append(a)
    return expand(keep, entry["directory"], tmp)


def main() -> int:
    for p in (LVGL_HOST, FW_BUILD / "compile_commands.json"):
        if not p.exists():
            print(f"missing {p}: build the host tests and the firmware first", file=sys.stderr)
            return 2
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        host, fw = host_macros(tmp), firmware_macros(tmp)
    diffs = {k: (host.get(k), fw.get(k)) for k in sorted(set(host) | set(fw)) if host.get(k) != fw.get(k)}
    bad = {k: v for k, v in diffs.items() if k not in ALLOWED}
    for k, (h, f) in diffs.items():
        tag = "allowed" if k in ALLOWED else "DIFF   "
        print(f"{tag} {k}: host={h!r} firmware={f!r}")
    print(f"{len(host)} host and {len(fw)} firmware LV_* macros, {len(diffs)} differ, {len(bad)} not explained")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
