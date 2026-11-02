"""TCP development transport (contracts/protocol.md section 10).

Frame: char_id u8, op u8, len u16 LE (<= 512), value. One client at a time; a TCP connection is a
connected and bonded BLE link. Notifications are always enabled.
"""
from __future__ import annotations

import asyncio
import json
import struct
from pathlib import Path

from . import native as N
from .board import FakeBoard

_TCP = json.loads((Path(__file__).resolve().parents[2] / "contracts" / "gatt.json").read_text())["tcp"]
DEFAULT_PORT = _TCP["default_port"]
MAX_VALUE = _TCP["max_value"]
OP_READ, OP_WRITE, OP_WRITE_NR, OP_NOTIFY, OP_READ_RSP = (
    _TCP["ops"][k] for k in ("read", "write", "write_without_response", "notify", "read_response"))
READABLE = (N.CAPABILITIES, N.STATUS, N.BATTERY)
WRITABLE = (N.CONFIG, N.NOTIFICATION, N.TRANSFER)


def frame(char: int, op: int, value: bytes) -> bytes:
    return struct.pack("<BBH", char, op, len(value)) + value


class _TcpLink:
    def __init__(self, loop: asyncio.AbstractEventLoop, writer: asyncio.StreamWriter):
        self.loop = loop
        self.writer = writer
        self.dropped = False

    def notify(self, char: int, value: bytes) -> None:
        self.loop.call_soon_threadsafe(self._send, frame(char, OP_NOTIFY, value))

    def _send(self, data: bytes) -> None:
        if not self.writer.is_closing():
            self.writer.write(data)

    def drop(self) -> None:
        self.dropped = True
        self.loop.call_soon_threadsafe(self.writer.close)


class TcpServer:
    def __init__(self, board: FakeBoard, host: str = "127.0.0.1", port: int = DEFAULT_PORT, log=print):
        self.board = board
        self.host = host
        self.port = port
        self.log = log
        self._server: asyncio.base_events.Server | None = None

    async def start(self) -> None:
        self._server = await asyncio.start_server(self._client, self.host, self.port)
        self.port = self._server.sockets[0].getsockname()[1]

    async def stop(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    async def _client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        link = _TcpLink(asyncio.get_running_loop(), writer)
        if not self.board.connect(link):
            self.log(f"[tcp] refused {peer}: {'board is off' if not self.board.powered else 'already connected'}")
            writer.close()
            return
        self.log(f"[tcp] connected {peer}")
        try:
            while not link.dropped:
                hdr = await reader.readexactly(4)
                char, op, n = struct.unpack("<BBH", hdr)
                if n > MAX_VALUE:
                    self.log(f"[tcp] frame of {n} bytes: closing")
                    break
                value = await reader.readexactly(n)
                if op == OP_READ and char in READABLE:
                    writer.write(frame(char, OP_READ_RSP, self.board.read(link, char)))
                elif op in (OP_WRITE, OP_WRITE_NR) and char in WRITABLE:
                    self.board.write(link, char, value)
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            self.board.disconnect(link)
            writer.close()
            self.log(f"[tcp] disconnected {peer}")
