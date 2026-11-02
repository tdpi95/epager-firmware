"""Shared pytest setup: tools/ on sys.path, the fake board libraries, --update-golden."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--update-golden", action="store_true",
                     help="rewrite tests/golden/*/*/*.png from the current rendering")


@pytest.fixture(scope="session")
def fb_libs() -> Path:
    """Build build/sim/libepager_{core,sim}.so (incremental). Skips if cmake/ninja are missing."""
    from fake_board import build
    try:
        return build.lib_dir()
    except build.BuildError as e:
        if "not found" in str(e):
            pytest.skip(str(e))
        raise
