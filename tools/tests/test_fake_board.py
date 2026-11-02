"""Fake board details not covered by the flows: TCP transport rules (protocol.md 10), board events,
and the rule that shared code has no simulator special-casing."""
from __future__ import annotations

import asyncio
import re
import struct
from pathlib import Path

from epager_proto import codec
from epager_proto import registry as R
from fake_board import native as N
from fake_board.board import BoardConfig, FakeBoard, describe
from fake_board.tcp import TcpServer

REPO = Path(__file__).resolve().parents[2]
REG = R.load()


async def _open(port: int) -> tuple[asyncio.StreamReader, asyncio.StreamWriter]:
    return await asyncio.open_connection("127.0.0.1", port)


async def _frame(r: asyncio.StreamReader, timeout: float = 2.0) -> tuple[int, int, bytes] | None:
    try:
        char, op, n = struct.unpack("<BBH", await asyncio.wait_for(r.readexactly(4), timeout))
        return char, op, await r.readexactly(n)
    except (asyncio.IncompleteReadError, ConnectionError):
        return None


def _tcp_case(fn) -> None:
    async def run() -> None:
        board = FakeBoard(BoardConfig(screen=False), clock="manual")
        server = TcpServer(board, port=0, log=lambda _s: None)
        await server.start()
        try:
            await fn(board, server.port)
        finally:
            await server.stop()
    asyncio.run(run())


def test_tcp_reads_and_ignored_frames(fb_libs) -> None:
    async def case(board: FakeBoard, port: int) -> None:
        r, w = await _open(port)
        w.write(struct.pack("<BBH", 9, 0, 0))            # unknown characteristic: ignored
        w.write(struct.pack("<BBH", 2, 0, 0))            # Config is not readable: ignored
        w.write(struct.pack("<BBH", 1, 3, 0))            # op notify from the app: ignored
        w.write(struct.pack("<BBH", 5, 1, 2) + b"\xC0\x00")  # Status is not writable: ignored
        w.write(struct.pack("<BBH", 1, 0, 0))            # Capabilities read
        f = await _frame(r)
        assert f and f[0] == 1 and f[1] == 4 and f[2][1] == 0x7E
        caps = codec.decode_message(REG, f[2])["fields"]   # Paper app icons: fixed 24x24 (asset-formats.md)
        assert {"asset_type": 5, "format": 1, "max_size": 88, "width": 24, "height": 24} in caps["asset_support"]
        w.write(struct.pack("<BBH", 6, 0, 0))            # Battery Level read
        assert await _frame(r) == (6, 4, b"\x50")
        board.set_secure(False)
        w.write(struct.pack("<BBH", 5, 0, 0))            # Status read on an insecure link: empty
        assert await _frame(r) == (5, 4, b"")
        w.close()
    _tcp_case(case)


def test_tcp_one_client_at_a_time(fb_libs) -> None:
    async def case(board: FakeBoard, port: int) -> None:
        r1, w1 = await _open(port)
        w1.write(struct.pack("<BBH", 1, 0, 0))
        assert await _frame(r1)
        r2, _w2 = await _open(port)
        assert await _frame(r2) is None                   # closed immediately
        w1.write(struct.pack("<BBH", 1, 0, 0))
        assert await _frame(r1)                           # the first one is unaffected
        w1.close()
    _tcp_case(case)


def test_tcp_oversized_frame_closes(fb_libs) -> None:
    async def case(board: FakeBoard, port: int) -> None:
        r, w = await _open(port)
        w.write(struct.pack("<BBH", 2, 1, 513) + bytes(513))
        assert await _frame(r) is None
        for _ in range(100):
            if board.link is None:
                break
            await asyncio.sleep(0.01)
        assert board.link is None                         # a disconnect for the board
    _tcp_case(case)


def test_small_mtu_segments_notifications(fb_libs) -> None:
    board = FakeBoard(BoardConfig(screen=False, mtu=23), clock="manual")

    class Sink:
        def __init__(self) -> None:
            self.notes: list[tuple[int, bytes]] = []

        def notify(self, char: int, value: bytes) -> None:
            self.notes.append((char, value))

        def drop(self) -> None:
            pass

    sink = Sink()
    assert board.connect(sink)
    from epager_proto import segments
    ping = bytes([0, 0x01, 1, 0]) + b"\x01\x2c\x01" + bytes(300)  # PING with a 300-byte payload
    for seg in segments.split(ping, 20):
        board.write(sink, N.CONFIG, seg)
    assert len(sink.notes) == (4 + 3 + 1 + 3 + 300 + 18) // 19 and all(len(v) <= 20 for _, v in sink.notes)


def test_events_and_describe(fb_libs) -> None:
    board = FakeBoard(BoardConfig(screen=False), clock="manual")
    st, nid = board.android(key="k", app_id="com.zing.zalo", app_name="Zalo", title="Chào 👋", body="x" * 2000)
    assert st == N_TOO_LARGE  # over the field limit, as message validation would refuse it
    st, nid = board.android(key="k", app_id="com.zing.zalo", app_name="Zalo", title="Chào 👋")
    assert st == 0 and nid == 1
    ev = [e for e in board.events if e.type == N.EV_NOTIF][-1]
    assert ev.notif["title"] == "Chào 👋" and ev.notif["kind"] == "added"
    assert "Chào 👋" in describe(ev) and "shown android app=Zalo" in describe(ev)
    assert describe([e for e in board.events if e.type == N.EV_REBOOT][0]) == "[boot] firmware 0.1.0"


N_TOO_LARGE = 5


def test_no_simulator_special_casing_in_shared_code() -> None:
    """M3 acceptance: the simulators plug in through board_api only."""
    pattern = re.compile(r"\bSIM_|\bsim_|sim_epaper|sim_lcd|FB_WITH_SIM")
    hits = []
    for root in ("components", "variants", "boards"):
        for p in (REPO / root).rglob("*"):
            if p.is_file() and p.suffix in (".c", ".h", ".txt", ".cmake", ".yml", ".csv", ".defaults", ".projbuild"):
                if "sim_epaper_154" in p.parts or "sim_lcd_296x240" in p.parts:
                    continue
                for i, line in enumerate(p.read_text(errors="replace").splitlines(), 1):
                    if pattern.search(line):
                        hits.append(f"{p.relative_to(REPO)}:{i}: {line.strip()}")
    assert not hits, "\n".join(hits)
