"""Links to a board: characteristic reads, writes and notifications over TCP (the fake board,
protocol.md 10) or BLE (bleak: the fake board's BLE peripheral or real hardware)."""
from __future__ import annotations

import asyncio
import json
import struct
from pathlib import Path
from typing import Callable

GATT = json.loads((Path(__file__).resolve().parents[2] / "contracts" / "gatt.json").read_text())
CHARS = {c["name"]: c["char_id"] for c in GATT["service"]["characteristics"]} | {"battery": 6}
UUIDS = {c["char_id"]: c["uuid"] for c in GATT["service"]["characteristics"]}
UUIDS[6] = "00002a19-0000-1000-8000-00805f9b34fb"
SERVICE_UUID = GATT["service"]["uuid"]
NOTIFYING = (2, 3, 4, 5, 6)
TCP = GATT["tcp"]
OPS = TCP["ops"]
OP_READ, OP_WRITE, OP_WRITE_NR, OP_NOTIFY, OP_READ_RSP = (
    OPS[k] for k in ("read", "write", "write_without_response", "notify", "read_response"))

Notify = Callable[[int, bytes], None]


class LinkClosed(ConnectionError):
    pass


class Link:
    mtu: int = 23
    on_notify: Notify | None = None

    async def read(self, char: int) -> bytes: ...
    async def write(self, char: int, value: bytes, response: bool = False) -> None: ...
    async def close(self) -> None: ...

    @property
    def closed(self) -> asyncio.Event: ...


class TcpLink(Link):
    """protocol.md 10: char_id u8, op u8, len u16 LE, value."""

    def __init__(self, host: str = "127.0.0.1", port: int = TCP["default_port"]):
        self.host, self.port = host, port
        self.mtu = 517
        self.on_notify = None
        self._reads: dict[int, list[asyncio.Future]] = {}
        self._closed = asyncio.Event()
        self._task: asyncio.Task | None = None

    @property
    def closed(self) -> asyncio.Event:
        return self._closed

    async def connect(self) -> "TcpLink":
        self._reader, self._writer = await asyncio.open_connection(self.host, self.port)
        self._task = asyncio.create_task(self._rx())
        return self

    async def _rx(self) -> None:
        try:
            while True:
                char, op, n = struct.unpack("<BBH", await self._reader.readexactly(4))
                value = await self._reader.readexactly(n)
                if op == OP_NOTIFY and self.on_notify:
                    self.on_notify(char, value)
                elif op == OP_READ_RSP and self._reads.get(char):
                    fut = self._reads[char].pop(0)
                    if not fut.done():
                        fut.set_result(value)
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            self._closed.set()
            for futs in self._reads.values():
                for f in futs:
                    if not f.done():
                        f.set_exception(LinkClosed("link closed"))

    async def read(self, char: int) -> bytes:
        if self._closed.is_set():
            raise LinkClosed("link closed")
        fut = asyncio.get_running_loop().create_future()
        self._reads.setdefault(char, []).append(fut)
        self._writer.write(struct.pack("<BBH", char, OP_READ, 0))
        return await asyncio.wait_for(fut, 5)

    async def write(self, char: int, value: bytes, response: bool = False) -> None:
        if self._closed.is_set():
            raise LinkClosed("link closed")
        self._writer.write(struct.pack("<BBH", char, OP_WRITE if response else OP_WRITE_NR, len(value)) + value)
        await self._writer.drain()

    async def close(self) -> None:
        if not self._writer.is_closing():
            self._writer.close()
        if self._task:
            await asyncio.wait([self._task], timeout=2)


class BleLink(Link):
    """bleak client. The board only bonds in pairing mode and asks for a passkey (PLAN D6): by default the link
    pairs through a BlueZ agent (bluez_agent.py) that takes the code from `passkey` or from the terminal;
    pair=False leaves pairing to the OS (an already bonded laptop)."""

    def __init__(self, address: str, passkey: int | None = None, pair: bool = True):
        self.address = address
        self.passkey = passkey
        self.pair = pair
        self._agent = None
        self.on_notify = None
        self._closed = asyncio.Event()

    @property
    def closed(self) -> asyncio.Event:
        return self._closed

    @staticmethod
    async def scan(timeout: float = 5.0) -> list[tuple[str, str]]:
        from bleak import BleakScanner
        found = await BleakScanner.discover(timeout=timeout, return_adv=True)
        out = []
        for dev, adv in found.values():
            name = adv.local_name or dev.name or ""
            if SERVICE_UUID in [u.lower() for u in adv.service_uuids] or name.lower().startswith("epager-"):
                out.append((dev.address, name))
        return out

    async def connect(self) -> "BleLink":
        from bleak import BleakClient
        self._client = BleakClient(self.address, disconnected_callback=lambda _c: self._closed.set())
        await self._client.connect()
        if self.pair:
            await self._pair()
        self.mtu = self._client.mtu_size
        for char in NOTIFYING:
            uuid = UUIDS[char]
            if self._client.services.get_characteristic(uuid) is not None:
                await self._client.start_notify(uuid, self._handler(char))
        return self

    async def _pair(self) -> None:
        from .bluez_agent import PasskeyAgent
        self._agent = PasskeyAgent(self.passkey)
        await self._agent.register()
        try:
            await self._client.pair()
        except Exception as e:  # noqa: BLE001 (BlueZ: AlreadyExists when this laptop is bonded already)
            if "AlreadyExists" not in str(e) and "Already Exists" not in str(e):
                raise

    def _handler(self, char: int):
        def cb(_sender, data: bytearray) -> None:
            if self.on_notify:
                self.on_notify(char, bytes(data))
        return cb

    async def read(self, char: int) -> bytes:
        return bytes(await self._client.read_gatt_char(UUIDS[char]))

    async def write(self, char: int, value: bytes, response: bool = False) -> None:
        await self._client.write_gatt_char(UUIDS[char], value, response=response)

    async def close(self) -> None:
        await self._client.disconnect()
        if self._agent is not None:
            await self._agent.unregister()
