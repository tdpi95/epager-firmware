"""Synchronous in-process client of a FakeBoard: runs a request through the real protocol path
(segments, validation, transfer state machine, storage), without TCP. For scenes, tests and console
commands that need an asset, the time or a config value on the board; the async client in tools/test_client does the same over a link.
"""
from __future__ import annotations

import hashlib
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from epager_proto import assets, codec, config, segments  # noqa: E402
from epager_proto import registry as R  # noqa: E402

from .board import FakeBoard  # noqa: E402

REG = R.load()
CHAR_CONFIG = 2
CHAR_TRANSFER = 4
KIND_ASSET = 1


class LoopbackError(AssertionError):
    pass


class _Link:
    def __init__(self) -> None:
        self.rx = {c: segments.Reassembler(1 << 16) for c in (2, 3, 4, 5)}
        self.messages: list[tuple[int, dict]] = []

    def notify(self, char: int, value: bytes) -> None:
        if char in self.rx:
            for kind, val in self.rx[char].feed(value):
                if kind == "message":
                    self.messages.append((char, codec.decode_message(REG, val)))

    def drop(self) -> None:
        pass


def idle_link() -> _Link:
    """A link that only listens (a connected phone doing nothing), for scenes and tests."""
    return _Link()


class _Session:
    def __init__(self, board: FakeBoard):
        self.board, self.link, self.req = board, _Link(), 0
        if not board.connect(self.link):
            raise LoopbackError("the board has a link already (or is off)")

    def close(self) -> None:
        self.board.disconnect(self.link)

    def request(self, type_name: str, fields: dict, want: str = "OK", char: int = CHAR_TRANSFER) -> dict:
        self.req += 1
        raw = codec.encode_message(REG, {"type": type_name, "req_id": self.req, "fields": fields})
        for seg in segments.split(raw, min(self.board.cfg.mtu - 3, 512)):
            self.board.write(self.link, char, seg)
        want_type = REG.msg_by_id[REG.msg_by_name[type_name].id | 0x80].name
        for _char, msg in self.link.messages:
            if msg["type"] == want_type and msg["req_id"] == self.req:
                got = REG.status_by_id.get(msg["fields"].get("status"), "?")
                if got != want:
                    raise LoopbackError(f"{type_name}: status {got}, wanted {want}")
                return msg["fields"]
        raise LoopbackError(f"{type_name}: no response")


def install_asset(board: FakeBoard, asset_type: int, ident: str, data: bytes, want: str = "OK",
                  origin: str | None = None) -> None:
    """Upload `data` (an asset file) as asset `ident` of `asset_type` with the transfer protocol, in
    chunks of the board's `max_chunk`. `want` is the expected status of XFER_FINISH; anything else
    raises LoopbackError."""
    s = _Session(board)
    try:
        begin = {"kind": KIND_ASSET, "asset_type": asset_type, "id": ident, "size": len(data),
                 "crc32": zlib.crc32(data), "sha256": hashlib.sha256(data).hexdigest()}
        if origin is not None:
            begin["origin"] = {"unknown": 0, "gallery": 1, "local": 2}[origin]
        b = s.request("XFER_BEGIN", begin)
        for off in range(0, len(data), b["max_chunk"]):  # chunks are single-segment writes
            piece = data[off:off + b["max_chunk"]]
            chunk = codec.encode_message(REG, {"type": "XFER_CHUNK", "req_id": 0,
                                               "fields": {"handle": b["handle"], "offset": off, "data": piece.hex()}})
            board.write(s.link, CHAR_TRANSFER, b"\xC0" + chunk)
        s.request("XFER_FINISH", {"handle": b["handle"]}, want=want)
    finally:
        s.close()


def install_app_icon(board: FakeBoard, app_id: str, rows: list[str], want: str = "OK") -> None:
    """Install the icon of `app_id` (rows of '#' black / '.' white, 24x24 on the Paper), named as
    the board looks it up (asset-formats.md)."""
    install_asset(board, assets.ASSET_TYPE_APP_ICON, assets.app_icon_id(app_id), assets.app_icon(rows), want)


def install_app_icon_colour(board: FakeBoard, app_id: str, pixels: list[list[tuple[int, int, int, int]]],
                            want: str = "OK") -> None:
    """Install a colour (`rgb565a8`) icon of `app_id`, as the app does for LCD."""
    install_asset(board, assets.ASSET_TYPE_APP_ICON, assets.app_icon_id(app_id), assets.app_icon_rgb565a8(pixels), want)


def delete_app_icon(board: FakeBoard, app_id: str) -> None:
    s = _Session(board)
    try:
        s.request("ASSET_DELETE", {"asset_type": assets.ASSET_TYPE_APP_ICON, "id": assets.app_icon_id(app_id)})
    finally:
        s.close()


ASSET_TYPE_CLOCK_FACE = 6


def install_clock_face(board: FakeBoard, ident: str, data: bytes, want: str = "OK", origin: str | None = None) -> None:
    """Install a clock face file (a clock_face_v1 asset) under `ident` through the transfer protocol."""
    install_asset(board, ASSET_TYPE_CLOCK_FACE, ident, data, want, origin)


def put_clock_face_unchecked(board: FakeBoard, ident: str, data: bytes) -> None:
    """Store a face file without the checks of XFER_FINISH: what the renderer sees after the flash went bad."""
    board.blob_put(f"{ASSET_TYPE_CLOCK_FACE}_{ident}", data)


def delete_clock_face(board: FakeBoard, ident: str) -> None:
    s = _Session(board)
    try:
        s.request("ASSET_DELETE", {"asset_type": ASSET_TYPE_CLOCK_FACE, "id": ident})
    finally:
        s.close()


def asset_list(board: FakeBoard, asset_type: int | None = None) -> list[dict]:
    """ASSET_LIST, every page: the installed assets (of one type) as {asset_type, id, size, version?, origin?}."""
    out: list[dict] = []
    s = _Session(board)
    try:
        while True:
            r = s.request("ASSET_LIST", {"offset": len(out), "limit": 16})
            page = r.get("asset", [])
            out += page
            if not page or len(out) >= r.get("total", 0):
                break
    finally:
        s.close()
    return [a for a in out if asset_type is None or a["asset_type"] == asset_type]


def time_sync(board: FakeBoard, utc_s: int, tz_offset_min: int = 0) -> None:
    """TIME_SYNC as the app sends it: sets the clock, the RTC and time.tz_offset_min."""
    s = _Session(board)
    try:
        s.request("TIME_SYNC", {"utc_s": utc_s, "tz_offset_min": tz_offset_min}, char=CHAR_CONFIG)
    finally:
        s.close()


def config_set(board: FakeBoard, values: dict[str, object]) -> None:
    """CONFIG_SET of registry key names to logical values, e.g. {"ui.clock_24h": False}."""
    keys = {k["name"]: k for k in REG.config_keys}
    entries = [{"key": keys[name]["id"], "value": config.encode_value(keys[name], v).hex()}
               for name, v in values.items()]
    s = _Session(board)
    try:
        s.request("CONFIG_SET", {"entry": entries}, char=CHAR_CONFIG)
    finally:
        s.close()


def config_get_raw(board: FakeBoard, name: str) -> bytes:
    """CONFIG_GET of one registry key: its value as the wire bytes (a `bytes` key such as ui.clock_face_opts)."""
    key = next(k for k in REG.config_keys if k["name"] == name)
    s = _Session(board)
    try:
        f = s.request("CONFIG_GET", {"key": [key["id"]]}, char=CHAR_CONFIG)
    finally:
        s.close()
    return bytes.fromhex(f["entry"][0]["value"])


def wifi_set(board: FakeBoard, ssid: str | None = None, password: str | None = None, forget: bool = False,
             want: str = "OK") -> None:
    """WIFI_SET as the app sends it (protocol.md 5): store a network, or forget the stored one."""
    fields: dict[str, object] = {}
    if ssid is not None:
        fields["ssid"] = ssid
    if password is not None:
        fields["password"] = password
    if forget:
        fields["forget"] = True
    s = _Session(board)
    try:
        s.request("WIFI_SET", fields, want=want, char=CHAR_CONFIG)
    finally:
        s.close()
