"""ePager protocol client over a Link, using the Python reference codec (tools/epager_proto).

Requests are segmented at the link MTU and matched to their response (same characteristic,
response type, req_id). Everything else that arrives (events, unattributed ERRORs) is queued.
"""
from __future__ import annotations

import asyncio
import hashlib
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from epager_proto import codec, segments  # noqa: E402
from epager_proto import registry as R  # noqa: E402

from .links import Link  # noqa: E402

REG = R.load()
CHAR_OF_CHANNEL = {"config": 2, "notification": 3, "transfer": 4, "status": 5, "any": 2}
WRITE_WITH_RESPONSE = {2}  # Config has no write-without-response property (gatt.json)


class RequestFailed(AssertionError):
    def __init__(self, rsp: dict, want: str):
        super().__init__(f"{rsp['type']} status {status_name(rsp)}, wanted {want}")
        self.rsp = rsp


def status_name(msg: dict) -> str:
    return REG.status_by_id.get(msg["fields"].get("status"), "UNKNOWN")


@dataclass
class Received:
    char: int
    msg: dict


@dataclass
class TransferResult:
    begin: dict
    finish: dict | None
    acks: list[dict] = field(default_factory=list)
    sent: int = 0


class EpagerClient:
    def __init__(self, link: Link):
        self.link = link
        self.rx = {c: segments.Reassembler(1 << 16) for c in (2, 3, 4, 5)}
        self.pending: dict[tuple[int, int, int], asyncio.Future] = {}
        self.events: asyncio.Queue[Received] = asyncio.Queue()
        self.history: list[Received] = []
        self.battery: list[int] = []
        self.next_req = 0
        link.on_notify = self._on_notify

    # ------------------------------------------------------------------ receive

    def _on_notify(self, char: int, value: bytes) -> None:
        if char == 6:
            self.battery.append(value[0] if value else -1)
            return
        if char not in self.rx:
            return
        for kind, val in self.rx[char].feed(value):
            if kind != "message":
                continue
            msg = codec.decode_message(REG, val)
            key = (char, msg["type"], msg["req_id"])
            r = Received(char, msg)
            self.history.append(r)
            if msg["req_id"] and msg["type"] == "ERROR":
                fut = next((f for (c, _t, rid), f in self.pending.items() if c == char and rid == msg["req_id"]), None)
                if fut and not fut.done():
                    fut.set_result(msg)
                    continue
            fut = self.pending.get(key)
            if fut and not fut.done():
                fut.set_result(msg)
            else:
                self.events.put_nowait(r)

    # ------------------------------------------------------------------ send

    @property
    def segment_max(self) -> int:
        return min(self.link.mtu - 3, 512)  # protocol.md 2.1

    def req_id(self) -> int:
        self.next_req = self.next_req % 0xFFFF + 1
        return self.next_req

    async def send_raw(self, char: int, message: bytes, response: bool | None = None) -> None:
        for seg in segments.split(message, self.segment_max):
            await self.link.write(char, seg, char in WRITE_WITH_RESPONSE if response is None else response)

    async def request(self, type_name: str, fields: dict | None = None, char: int | None = None,
                      want: str | None = "OK", timeout: float = 5.0) -> dict:
        """Send a request and return its response (logical form). `want` checks the status
        (None = any)."""
        msg_desc = REG.msg_by_name[type_name]
        char = char or CHAR_OF_CHANNEL[msg_desc.channel]
        rid = self.req_id()
        raw = codec.encode_message(REG, {"type": type_name, "req_id": rid, "fields": fields or {}})
        rsp_type = REG.msg_by_id[msg_desc.id | 0x80].name
        fut = asyncio.get_running_loop().create_future()
        self.pending[(char, rsp_type, rid)] = fut
        try:
            await self.send_raw(char, raw)
            rsp = await asyncio.wait_for(fut, timeout)
        finally:
            self.pending.pop((char, rsp_type, rid), None)
        if want is not None and status_name(rsp) != want:
            raise RequestFailed(rsp, want)
        return rsp

    async def raw_request(self, char: int, message: bytes, rsp_type: str, req_id: int, timeout: float = 5.0) -> dict:
        """Send pre-encoded bytes (e.g. invalid ones) and wait for rsp_type (or ERROR) with req_id."""
        fut = asyncio.get_running_loop().create_future()
        self.pending[(char, rsp_type, req_id)] = fut
        try:
            await self.send_raw(char, message)
            return await asyncio.wait_for(fut, timeout)
        finally:
            self.pending.pop((char, rsp_type, req_id), None)

    async def next_event(self, type_name: str | None = None, timeout: float = 5.0,
                         pred: Callable[[dict], bool] | None = None) -> dict:
        """Next queued message of this type (others before it are dropped)."""
        loop = asyncio.get_running_loop()
        end = loop.time() + timeout
        while True:
            r = await asyncio.wait_for(self.events.get(), max(0.0, end - loop.time()))
            if (type_name is None or r.msg["type"] == type_name) and (pred is None or pred(r.msg)):
                return r.msg

    def drain_events(self) -> list[dict]:
        out = []
        while not self.events.empty():
            out.append(self.events.get_nowait().msg)
        return out

    # ------------------------------------------------------------------ reads

    async def capabilities(self) -> dict:
        return codec.decode_message(REG, await self.link.read(1))

    async def status(self) -> dict | None:
        raw = await self.link.read(5)
        return codec.decode_message(REG, raw) if raw else None

    async def battery_level(self) -> int | None:
        raw = await self.link.read(6)
        return raw[0] if raw else None

    # ------------------------------------------------------------------ transfer

    async def chunk(self, handle: int, offset: int, data: bytes) -> None:
        raw = codec.encode_message(REG, {"type": "XFER_CHUNK", "req_id": 0,
                                         "fields": {"handle": handle, "offset": offset, "data": data.hex()}})
        if len(raw) + 1 > self.segment_max:
            raise ValueError("an XFER_CHUNK must fit one write")
        await self.link.write(4, b"\xC0" + raw, False)

    async def begin(self, kind: int, ident: str, data: bytes, want: str | None = "OK", **extra) -> dict:
        f = {"kind": kind, "id": ident, "size": len(data), "crc32": zlib.crc32(data),
             "sha256": hashlib.sha256(data).hexdigest(), **{k: v for k, v in extra.items() if v is not None}}
        return (await self.request("XFER_BEGIN", f, want=want))["fields"]

    async def upload(self, kind: int, ident: str, data: bytes, asset_type: int | None = None,
                     version: str | None = None, finish: bool = True, stop_at: int | None = None,
                     before_finish: Callable[[], object] | None = None) -> TransferResult:
        """Transfer data with windowed acks, resuming from whatever offset the board returns.
        stop_at: stop sending once this many bytes are acknowledged (to simulate a disconnect)."""
        b = await self.begin(kind, ident, data, asset_type=asset_type, version=version)
        res = TransferResult(b, None)
        offset = b["offset"]
        if offset and b["prefix_crc32"] != zlib.crc32(data[:offset]):
            b = await self.begin(kind, ident, data, asset_type=asset_type, version=version, restart=True)
            res.begin, offset = b, 0
        handle, window = b["handle"], b["ack_window"]
        step = min(b["max_chunk"], self.segment_max - 19)  # 19 = segment, message and TLV headers
        acked = offset
        while acked < len(data):
            if stop_at is not None and acked >= stop_at:
                return res
            end = min(len(data), acked + window)
            for off in range(acked, end, step):
                piece = data[off:min(off + step, end)]
                await self.chunk(handle, off, piece)
                res.sent += len(piece)
            ack = (await self.next_event("XFER_ACK", timeout=10))["fields"]
            res.acks.append(ack)
            if ack["status"] not in (0, REG.status_by_name["BAD_OFFSET"]):
                raise RequestFailed({"type": "XFER_ACK", "fields": ack}, "OK")
            acked = ack["offset"]
        if before_finish:
            r = before_finish()
            if asyncio.iscoroutine(r):
                await r
        if finish:
            res.finish = (await self.request("XFER_FINISH", {"handle": handle}, want=None, timeout=20))
        return res
