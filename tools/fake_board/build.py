"""Build (or locate) the fake board libraries: build/sim/libepager_{core,sim,sim_lcd}.so from tests/host.

The libraries are built without sanitizers (a sanitized library cannot be loaded into Python).
cmake and ninja are taken from PATH, else from the ESP-IDF tools directory (~/.espressif), so
`. ~/esp/esp-idf/export.sh` is not required. EPAGER_LIB_DIR points at prebuilt libraries instead.
"""
from __future__ import annotations

import glob
import os
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build" / "sim"


class BuildError(RuntimeError):
    pass


def _tool(name: str) -> str | None:
    found = shutil.which(name)
    if found:
        return found
    pattern = str(Path.home() / ".espressif" / "tools" / name / "*" / "**" / name)
    hits = sorted(p for p in glob.glob(pattern, recursive=True) if os.access(p, os.X_OK) and Path(p).is_file())
    return hits[-1] if hits else None


def lib_dir(build: bool = True) -> Path:
    """Directory with the libraries; builds them first (incrementally) unless EPAGER_LIB_DIR is set."""
    env = os.environ.get("EPAGER_LIB_DIR")
    if env:
        return Path(env)
    if build:
        ensure_built()
    return BUILD


def ensure_built(quiet: bool = True) -> None:
    cmake, ninja = _tool("cmake"), _tool("ninja")
    if not cmake or not ninja:
        raise BuildError("cmake/ninja not found: install them (or run . ~/esp/esp-idf/export.sh), "
                         "or set EPAGER_LIB_DIR to prebuilt libraries")
    out = subprocess.DEVNULL if quiet else None
    if not (BUILD / "build.ninja").exists():
        r = subprocess.run([cmake, "-S", str(REPO / "tests" / "host"), "-B", str(BUILD), "-G", "Ninja",
                            f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DEPAGER_SANITIZE=OFF", "-DEPAGER_SHARED_LIBS=ON"],
                           stdout=out, stderr=subprocess.PIPE, text=True)
        if r.returncode:
            raise BuildError(f"cmake configure failed:\n{r.stderr}")
    targets = ["epager_core_so"]
    if "epager_sim_so" in (BUILD / "build.ninja").read_text():  # absent with -DEPAGER_WITH_UI=OFF
        targets += ["epager_sim_so", "epager_sim_lcd_so"]
    r = subprocess.run([cmake, "--build", str(BUILD), "--target", *targets],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise BuildError(f"build failed:\n{r.stdout[-4000:]}")
