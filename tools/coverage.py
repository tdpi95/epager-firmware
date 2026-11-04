#!/usr/bin/env python3
"""Line coverage of the portable core from a gcov-instrumented host build (no gcovr needed).

    cmake -S tests/host -B build/cov -G Ninja -DEPAGER_COVERAGE=ON -DEPAGER_SANITIZE=OFF
    cmake --build build/cov && ctest --test-dir build/cov
    tools/.venv/bin/python tools/coverage.py build/cov [--min 85]

Runs `gcov --json-format --stdout` on every .gcda file and reports executable/covered lines per
source file of the portable components. Exit 1 if the total is below --min.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PORTABLE = ("protocol", "config_store", "notification_pipeline", "transfer", "core", "weather", "net", "ble_core", "ancs_client", "ota", "clock_face")


def collect(build: Path) -> dict[Path, dict[int, int]]:
    lines: dict[Path, dict[int, int]] = {}
    gcdas = sorted(build.rglob("*.gcda"))
    if not gcdas:
        sys.exit(f"no .gcda files under {build}: build with -DEPAGER_COVERAGE=ON and run ctest first")
    for gcda in gcdas:
        out = subprocess.run(["gcov", "--json-format", "--stdout", str(gcda)], capture_output=True, text=True,
                             cwd=gcda.parent, check=True).stdout
        for doc in (json.loads(x) for x in out.splitlines() if x.strip()):
            for f in doc["files"]:
                path = Path(f["file"])
                path = (path if path.is_absolute() else Path(doc["current_working_directory"]) / path).resolve()
                per = lines.setdefault(path, {})
                for ln in f["lines"]:
                    per[ln["line_number"]] = per.get(ln["line_number"], 0) + ln["count"]
    return lines


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=Path)
    ap.add_argument("--min", type=float, default=0.0, help="fail below this total line coverage (percent)")
    args = ap.parse_args()
    data = collect(args.build.resolve())
    rows = []
    for comp in PORTABLE:
        src = REPO / "components" / comp / "src"
        for path in sorted(p for p in data if p.is_relative_to(src)):
            per = data[path]
            rows.append((str(path.relative_to(REPO)), len(per), sum(1 for c in per.values() if c > 0)))
    width = max(len(r[0]) for r in rows)
    for name, total, hit in rows:
        print(f"{name:<{width}}  {hit:5}/{total:<5} {100.0 * hit / total:6.1f} %")
    total = sum(r[1] for r in rows)
    hit = sum(r[2] for r in rows)
    pct = 100.0 * hit / total
    print(f"{'TOTAL (portable core)':<{width}}  {hit:5}/{total:<5} {pct:6.1f} %")
    if pct < args.min:
        print(f"coverage {pct:.1f} % is below {args.min:.1f} %")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
