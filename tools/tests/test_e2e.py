"""End to end over TCP: tools/test_client against tools/fake_board (the C core through ctypes)."""
from __future__ import annotations

import asyncio

import pytest

from fake_board.board import BoardConfig, FakeBoard
from fake_board.tcp import TcpServer
from test_client.client import EpagerClient
from test_client.flows import FLOWS, Session, run_flows
from test_client.links import TcpLink


async def _session(board: FakeBoard) -> tuple[Session, TcpServer]:
    server = TcpServer(board, port=0, log=lambda _s: None)
    await server.start()

    async def connect() -> EpagerClient:
        return EpagerClient(await TcpLink("127.0.0.1", server.port).connect())

    return Session(connect, board), server


async def _run(names: list[str], screen: bool = True) -> dict[str, str]:
    board = FakeBoard(BoardConfig(screen=screen), clock="manual")
    s, server = await _session(board)
    try:
        await s.open()
        return await run_flows(s, names)
    finally:
        await s.close()
        await server.stop()


@pytest.mark.parametrize("name", list(FLOWS))
def test_flow(fb_libs, name: str) -> None:
    """Every flow on a fresh board, including the failure paths."""
    assert asyncio.run(_run([name]))[name] == "ok"


def test_all_flows_one_session(fb_libs) -> None:
    """The flows back to back on one board, as an app session would run them."""
    results = asyncio.run(_run(list(FLOWS)))
    assert all(r == "ok" for r in results.values()), results


def test_flows_without_screen(fb_libs) -> None:
    """libepager_core.so (no LVGL): everything except the screen flows."""
    names = [n for n in FLOWS if n not in ("screen", "app_icons_screen")]
    results = asyncio.run(_run(names, screen=False))
    assert all(r == "ok" for r in results.values()), results


def test_flows_skip_without_board(fb_libs) -> None:
    """Against hardware there is no in-process board: board-driven flows are skipped, not failed."""
    async def run() -> dict[str, str]:
        board = FakeBoard(BoardConfig(), clock="manual")
        s, server = await _session(board)
        s.board = None
        try:
            await s.open()
            return await run_flows(s, ["capabilities", "security"])
        finally:
            await s.close()
            await server.stop()

    r = asyncio.run(run())
    assert r["capabilities"] == "ok" and r["security"].startswith("skipped")
