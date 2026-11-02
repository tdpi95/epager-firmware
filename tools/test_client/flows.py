"""Scripted end-to-end flows against a board. Each flow takes a Session and raises on failure.

Flows marked needs_board drive failures through the in-process fake board (battery, storage,
corruption, OTA image checks, security, the screen); the others also run against real hardware.
"""
from __future__ import annotations

import asyncio
import struct
import zlib
from dataclasses import dataclass, field
from typing import Awaitable, Callable

from epager_proto import assets as A  # noqa: E402  (client.py puts tools/ on sys.path)

from .client import REG, EpagerClient, status_name

ASSET, FIRMWARE = 1, 2
FONT, BACKGROUND, THEME, APP_ICON = 1, 2, 3, 5
MONO1, FONT_V1, THEME_V1 = 1, 3, 4


@dataclass
class Session:
    connect: Callable[[], Awaitable[EpagerClient]]
    board: object | None = None          # tools/fake_board FakeBoard, in-process
    client: EpagerClient | None = None
    log: Callable[[str], None] = field(default=lambda s: None)

    async def open(self) -> EpagerClient:
        self.client = await self.connect()
        return self.client

    async def close(self) -> None:
        if self.client:
            await self.client.link.close()
            await asyncio.wait_for(self.client.link.closed.wait(), 5)
            if self.board:  # let the board see the disconnect before the next connection
                for _ in range(100):
                    if self.board.link is None:
                        break
                    await asyncio.sleep(0.01)
            self.client = None

    async def reopen(self) -> EpagerClient:
        await self.close()
        return await self.open()

    async def wait(self, ms: int) -> None:
        """Let board time pass: advances a manual clock, else sleeps."""
        if self.board is not None and self.board.clock == "manual":
            self.board.advance(ms)
            await asyncio.sleep(0.01)
        else:
            await asyncio.sleep(ms / 1000)


def asset(asset_type: int, fmt: int, payload: bytes, width: int = 0, height: int = 0) -> bytes:
    """asset-formats.md container."""
    return b"ESPA" + struct.pack("<BBBBHHI", 1, asset_type, fmt, 0, width, height, len(payload)) + payload


def pattern(n: int, seed: int = 0) -> bytes:
    return bytes((i * 131 + seed * 7 + (i >> 8)) & 0xFF for i in range(n))


def truncate_utf8(s: str, max_bytes: int) -> str:
    """What the app must do before sending a text field (protocol.md 6.1)."""
    b = s.encode()
    if len(b) <= max_bytes:
        return s
    return b[:max_bytes].decode("utf-8", "ignore")


def entries(rsp: dict) -> dict[int, str]:
    return {e["key"]: e["value"] for e in rsp["fields"].get("entry", [])}


async def check(cond: bool, what: str) -> None:
    if not cond:
        raise AssertionError(what)


# --------------------------------------------------------------------------- basic

async def flow_capabilities(s: Session) -> None:
    caps = (await s.client.capabilities())["fields"]
    await check(caps["contract_version"].split(".")[0] == str(REG.proto_major), f"contract major {caps}")
    await check(caps["variant"] == 1, "variant paper")
    await check(caps["display"]["width"] == 200 and caps["display"]["color_depth"] == 1, "display 200x200 1-bit")
    st = await s.client.status()
    await check(st is not None and st["type"] == "DEVICE_STATUS", "Status read value")
    s.log(f"capabilities: {caps['board_id']} {caps['firmware_version']}, contract {caps['contract_version']}")


async def flow_ping(s: Session) -> None:
    c = s.client
    r = await c.request("PING", {"payload": "00ff10"})
    await check(r["fields"]["payload"] == "00ff10", "echo")
    big = pattern(512).hex()
    await check((await c.request("PING", {"payload": big}))["fields"]["payload"] == big, "segmented echo")
    for char in (2, 3, 4):
        await c.request("PING", {}, char=char)


async def flow_errors(s: Session) -> None:
    c = s.client
    rid = c.req_id()
    msg = codec_msg("CONFIG_GET", rid, {})
    r = await c.raw_request(3, msg, "ERROR", rid)  # Config request on the Notification characteristic
    await check(r["type"] == "ERROR" and status_name(r) == "UNKNOWN_MESSAGE", f"wrong channel: {r}")
    rid = c.req_id()
    r = await c.raw_request(2, codec_msg("PING_RSP", rid, {"status": 0}), "ERROR", rid)
    await check(status_name(r) == "UNKNOWN_MESSAGE", "response type written")
    rid = c.req_id()
    bad = codec_msg("CONFIG_GET", rid, {}) + b"\x01\x05"
    r = await c.raw_request(2, bad, "CONFIG_GET_RSP", rid)
    await check(status_name(r) == "MALFORMED" and set(r["fields"]) == {"status"}, f"malformed: {r}")


def codec_msg(type_name: str, req_id: int, fields: dict) -> bytes:
    from .client import codec
    return codec.encode_message(REG, {"type": type_name, "req_id": req_id, "fields": fields})


# --------------------------------------------------------------------------- config

async def flow_config(s: Session) -> None:
    c = s.client
    await c.request("CONFIG_RESET")
    c.drain_events()
    g = await c.request("CONFIG_GET")
    rev, keys = g["fields"]["revision"], entries(g)
    await check(3 in keys and keys[4] == "01", f"Paper keys and defaults: {keys}")
    r = await c.request("CONFIG_SET", {"entry": [{"key": 4, "value": "00"}, {"key": 3, "value": "05"}]})
    await check(r["fields"]["revision"] == rev + 1, "revision bump")
    ev = await c.next_event("CONFIG_CHANGED")
    await check(ev["fields"] == {"revision": rev + 1, "key": [3, 4]}, f"CONFIG_CHANGED {ev}")
    await c.request("CONFIG_SET", {"expected_revision": rev, "entry": [{"key": 4, "value": "01"}]},
                    want="REVISION_CONFLICT")
    await c.request("CONFIG_SET", {"entry": [{"key": 3, "value": "00"}]}, want="OUT_OF_RANGE")
    await c.request("CONFIG_SET", {"entry": [{"key": 33, "value": "01"}]}, want="TYPE_MISMATCH")
    r = await c.request("CONFIG_SET", {"entry": [{"key": 999, "value": "01"}, {"key": 19, "value": "00"}]})
    await check([k["status"] for k in r["fields"]["key_status"]] == [9, 0], "unknown key skipped")
    g = await c.request("CONFIG_GET", {"key": [19, 4, 3]})
    await check([e["key"] for e in g["fields"]["entry"]] == [19, 4, 3], "requested order")
    await check(entries(g) == {19: "00", 4: "00", 3: "05"}, "values")
    await c.request("CONFIG_RESET")
    await check(entries(await c.request("CONFIG_GET", {"key": [3]})) == {3: "1e"}, "reset to defaults")
    c.drain_events()


async def flow_time_sync(s: Session) -> None:
    c = s.client
    await c.request("TIME_SYNC", {"utc_s": 1791700215, "tz_offset_min": 420})
    g = await c.request("CONFIG_GET", {"key": [48]})
    await check(entries(g) == {48: struct.pack("<h", 420).hex()}, "tz offset stored")
    if s.board:
        await check(abs(s.board.state()["utc"] - 1791700215) <= 2, "board clock set")
    await c.request("TIME_SYNC", {"utc_s": 1791700215, "tz_offset_min": 0})
    c.drain_events()


# --------------------------------------------------------------------------- notifications

LONG_VI = ("Thông báo rất dài: " + "Nguyễn Văn Ánh gửi lời chúc mừng sinh nhật, chúc bạn tuổi mới thật nhiều niềm vui. " * 20)


async def flow_notifications(s: Session) -> None:
    c = s.client
    await c.request("NOTIF_CLEAR", char=3)
    c.drain_events()
    vi = {"key": "0|com.zing.zalo|1", "app_id": "com.zing.zalo", "app_name": "Zalo", "title": "Tin nhắn mới",
          "body": "Chào bạn, tối nay đi ăn phở không?", "category": 4, "priority": 1}
    r = await c.request("NOTIF_POST", vi)
    nid = r["fields"]["notif_id"]
    await check(r["fields"]["shown"] is True, "shown")
    seen = await c.next_event("APP_SEEN", pred=lambda m: m["fields"]["app"]["app_id"] == "com.zing.zalo")
    await check(seen["fields"]["app"]["name"] == "Zalo", "APP_SEEN name")
    r = await c.request("NOTIF_POST", {**vi, "body": "Cập nhật"})
    await check(r["fields"]["notif_id"] == nid, "same key = same notification")
    emoji = {"key": "k-emoji", "app_id": "com.facebook.orca", "title": "Party 🎉🎉", "body": "👨‍👩‍👧 family 🇻🇳"}
    await c.request("NOTIF_POST", emoji)
    long = {"key": "k-long", "app_id": "com.google.android.gm", "app_name": "Gmail",
            "title": truncate_utf8(LONG_VI, 256), "body": truncate_utf8(LONG_VI, 1024)}
    await c.request("NOTIF_POST", long)
    await c.request("NOTIF_POST", {"key": "k-over", "app_id": "a.b", "body": "x" * 1025}, want="TOO_LARGE")
    if s.board:
        notes = [e.notif for e in s.board.events if e.notif]
        by_title = {n["title"]: n for n in notes}
        await check("Party 🎉🎉" in by_title, f"emoji are kept: {list(by_title)}")
        await check(by_title["Party 🎉🎉"]["body"] == "👨 family 🏳", "ZWJ sequence and flag collapse to one emoji")
    await c.request("NOTIF_REMOVE", {"key": "k-emoji"}, char=3)
    await c.request("NOTIF_REMOVE", {"key": "k-emoji"}, char=3, want="NOT_FOUND")
    await c.request("NOTIF_CLEAR", char=3)
    if s.board:
        await check(s.board.state()["notifications"] == 0, "cleared")
    c.drain_events()


async def flow_filters_and_apps(s: Session) -> None:
    c = s.client
    await c.request("APPS_FORGET")
    await c.request("APPS_SET_POLICY", {"entry": [{"app_id": "com.spam.app", "policy": 2}]})
    r = await c.request("NOTIF_POST", {"key": "k1", "app_id": "com.spam.app", "title": "Buy now"})
    await check(r["fields"]["shown"] is False, "blocked app hidden")
    await c.request("CONFIG_SET", {"entry": [{"key": 18, "value": "02"}]})  # min priority high
    r = await c.request("NOTIF_POST", {"key": "k2", "app_id": "com.zing.zalo", "title": "Hi", "priority": 1})
    await check(r["fields"]["shown"] is False, "below min priority")
    r = await c.request("NOTIF_POST", {"key": "k3", "app_id": "com.zing.zalo", "title": "Call", "priority": 3})
    await check(r["fields"]["shown"] is True, "urgent passes")
    lst = await c.request("APPS_LIST", {"limit": 1})
    await check(lst["fields"]["total"] == 2 and len(lst["fields"]["app"]) == 1, f"paging {lst}")
    lst2 = await c.request("APPS_LIST", {"offset": 1, "limit": 16})
    ids = {lst["fields"]["app"][0]["app_id"], lst2["fields"]["app"][0]["app_id"]}
    await check(ids == {"com.spam.app", "com.zing.zalo"}, f"apps {ids}")
    await c.request("APPS_FORGET", {"app_id": ["com.spam.app"]})
    await check((await c.request("APPS_LIST"))["fields"]["total"] == 1, "forgotten")
    await c.request("CONFIG_RESET")
    await c.request("NOTIF_CLEAR", char=3)
    await c.request("APPS_FORGET")
    c.drain_events()


# --------------------------------------------------------------------------- transfer

BG = asset(BACKGROUND, MONO1, pattern(5000, 1), 200, 200)


async def flow_asset_resume(s: Session) -> None:
    c = s.client
    free0 = (await c.request("STORAGE_INFO", char=4))["fields"]["free"]
    first = await c.upload(ASSET, "test.bg", BG, asset_type=BACKGROUND, stop_at=2048)
    await check(first.begin["offset"] == 0, "fresh start")
    c = await s.reopen()  # disconnect: the board journals the offset
    second = await c.upload(ASSET, "test.bg", BG, asset_type=BACKGROUND)
    await check(second.begin["offset"] >= 2048, f"resumed at {second.begin['offset']}")
    await check(second.begin["prefix_crc32"] == zlib.crc32(BG[:second.begin["offset"]]), "prefix CRC")
    await check(status_name(second.finish) == "OK", f"finish {second.finish}")
    lst = await c.request("ASSET_LIST", char=4)
    await check({"asset_type": BACKGROUND, "id": "test.bg", "size": len(BG)} in lst["fields"]["asset"], f"{lst}")
    info = (await c.request("STORAGE_INFO", char=4))["fields"]
    await check(info["free"] == free0 - len(BG), "free space accounting")
    await c.request("ASSET_DELETE", {"asset_type": BACKGROUND, "id": "test.bg"}, char=4)
    await c.request("ASSET_DELETE", {"asset_type": BACKGROUND, "id": "test.bg"}, char=4, want="NOT_FOUND")


async def flow_transfer_errors(s: Session) -> None:
    c = s.client
    await c.begin(ASSET, "big", pattern(6000), asset_type=BACKGROUND, want="TOO_LARGE")
    await c.begin(ASSET, "x", pattern(100), want="MISSING_FIELD")
    theme = asset(THEME, THEME_V1, pattern(300, 2))
    b = await c.begin(ASSET, "dark", theme, asset_type=THEME)
    await c.chunk(b["handle"], 10, theme[10:20])
    ack = (await c.next_event("XFER_ACK"))["fields"]
    await check(ack["status"] == REG.status_by_name["BAD_OFFSET"] and ack["offset"] == 0, f"bad offset {ack}")
    await c.chunk(99, 0, b"x")
    ack = (await c.next_event("XFER_ACK"))["fields"]
    await check(ack["status"] == REG.status_by_name["NO_SESSION"], f"no session {ack}")
    await c.request("XFER_FINISH", {"handle": b["handle"]}, char=4, want="BAD_OFFSET")
    st = (await c.request("XFER_STATUS", {"handle": b["handle"]}, char=4))["fields"]
    await check(st["state"] == 1 and st["offset"] == 0, f"status {st}")
    await c.request("XFER_ABORT", {"handle": b["handle"]}, char=4)
    await c.request("XFER_ABORT", {"handle": b["handle"]}, char=4, want="NO_SESSION")
    bad = b"ESPB" + BG[4:]
    r = await c.upload(ASSET, "badmagic", bad, asset_type=BACKGROUND)
    await check(status_name(r.finish) == "IMAGE_INVALID", f"container check {r.finish}")
    r = await c.upload(ASSET, "dark", theme, asset_type=THEME)
    await check(status_name(r.finish) == "OK", "a valid theme")
    await c.request("ASSET_DELETE", {"asset_type": THEME, "id": "dark"}, char=4)


# --------------------------------------------------------------------------- app icons (protocol.md 6.4)

ICON_ORIGIN = (4, 26)  # top-left of the icon on the Paper card (bar 24 + gap 2)


def checker_rows(w: int, h: int, seed: int = 0) -> list[str]:
    """A 3x3-cell checkerboard: unmistakable on the panel, different for each seed."""
    return ["".join("#" if (x // 3 + y // 3 + seed) % 2 == 0 else "." for x in range(w)) for y in range(h)]


async def icon_support(c: EpagerClient) -> dict:
    caps = (await c.capabilities())["fields"]
    entry = next((e for e in caps.get("asset_support", []) if e["asset_type"] == APP_ICON and e["format"] == MONO1), None)
    await check(entry is not None and entry.get("width") and entry.get("height"), "app_icon + mono1 with width/height")
    return entry


async def flow_app_icons(s: Session) -> None:
    """Upload, list, replace, reject a wrong size and delete an app icon (also safe on hardware)."""
    c = s.client
    e = await icon_support(c)
    w, h = e["width"], e["height"]
    ident = A.app_icon_id("com.zing.zalo")
    one, two = A.app_icon(checker_rows(w, h, 0)), A.app_icon(checker_rows(w, h, 1))
    await check(len(one) <= e["max_size"], "an icon fits max_size")
    r = await c.upload(ASSET, ident, one, asset_type=APP_ICON, version="0123abcd")
    await check(status_name(r.finish) == "OK", f"install {r.finish}")
    r = await c.upload(ASSET, ident, two, asset_type=APP_ICON, version="4567ef01")  # replaces it atomically
    await check(status_name(r.finish) == "OK", f"replace {r.finish}")
    lst = (await c.request("ASSET_LIST", {"limit": 16}, char=4))["fields"]
    mine = [a for a in lst["asset"] if a["id"] == ident]
    await check(len(mine) == 1 and mine[0]["asset_type"] == APP_ICON and mine[0]["size"] == len(two), f"{lst}")
    # exactly the advertised size, nothing else: fewer rows is a well-formed image of the wrong size
    short = A.app_icon(checker_rows(w, h - 8))
    r = await c.upload(ASSET, A.app_icon_id("com.example.short"), short, asset_type=APP_ICON)
    await check(status_name(r.finish) == "IMAGE_INVALID", f"wrong size is refused: {r.finish}")
    r = await c.upload(ASSET, A.app_icon_id("com.example.bad"), b"ESPA" + one[4:5] + b"\x02" + one[6:],
                       asset_type=APP_ICON)  # header says asset_type 2, begin says app_icon
    await check(status_name(r.finish) == "IMAGE_INVALID", f"asset_type mismatch: {r.finish}")
    await c.request("ASSET_DELETE", {"asset_type": APP_ICON, "id": ident}, char=4)
    await c.request("ASSET_DELETE", {"asset_type": APP_ICON, "id": ident}, char=4, want="NOT_FOUND")


async def settle(s: Session) -> None:
    for _ in range(100):  # until the panel is idle
        await s.wait(100)
        if not s.board.display_state().refreshing:
            break
    await s.wait(100)


def icon_region(panel: bytes, w: int = 24, h: int = 24) -> list[str]:
    x0, y0 = ICON_ORIGIN
    return ["".join("#" if panel[(y0 + y) * 200 + x0 + x] < 128 else "." for x in range(w)) for y in range(h)]


async def flow_app_icons_screen(s: Session) -> None:
    """The card shows the app's icon for Android and ANCS notifications, the category icon otherwise,
    and follows an icon being installed, replaced and deleted."""
    c, board = s.client, s.board
    if not board.screen:
        raise AssertionError("the fake board has no screen (libepager_core.so)")
    await c.request("NOTIF_CLEAR", char=3)
    c.drain_events()
    app, other = "com.zing.zalo", "com.example.other"
    mine, theirs = checker_rows(24, 24, 0), checker_rows(24, 24, 1)
    await c.request("NOTIF_POST", {"key": "i1", "app_id": app, "app_name": "Zalo", "title": "Mẹ", "category": 4})
    await settle(s)
    builtin = icon_region(board.panel())
    await check(any("#" in r for r in builtin) and builtin != mine, "category icon before an upload")

    async def put(rows: list[str], who: str = app) -> None:
        r = await c.upload(ASSET, A.app_icon_id(who), A.app_icon(rows), asset_type=APP_ICON)
        await check(status_name(r.finish) == "OK", f"{r.finish}")
        await settle(s)

    await put(mine)
    await check(icon_region(board.panel()) == mine, "the uploaded icon replaces the category icon")
    await put(theirs)
    await check(icon_region(board.panel()) == theirs, "a replaced icon is redrawn")
    await put(mine)
    await s.wait(2300)  # the first card's minimum time on the panel (D81)
    await c.request("NOTIF_POST", {"key": "i2", "app_id": other, "title": "Hello", "category": 4})
    await settle(s)
    await check(icon_region(board.panel()) == builtin, "an app without an icon keeps the category icon")
    await s.wait(2300)
    board.ancs(event_id=0, uid=7, app_id=app, app_name="Zalo", title="iOS", message="", date="", category=1)
    await settle(s)
    await check(icon_region(board.panel()) == mine, "the same icon serves ANCS notifications")
    board.ancs_reset()
    await settle(s)
    await c.request("ASSET_DELETE", {"asset_type": APP_ICON, "id": A.app_icon_id(app)}, char=4)
    await settle(s)
    await c.request("NOTIF_REMOVE", {"key": "i2"}, char=3)
    await settle(s)
    await check(icon_region(board.panel()) == builtin, "a deleted icon falls back to the category icon")
    await check(board.display_state().violations == 0, "display protocol")
    await c.request("NOTIF_CLEAR", char=3)
    c.drain_events()


async def flow_transfer_board_failures(s: Session) -> None:
    c, board = s.client, s.board
    # CRC mismatch: the stored bytes are corrupted before FINISH
    r = await c.upload(ASSET, "crc.bg", BG, asset_type=BACKGROUND, before_finish=board.corrupt_transfer)
    await check(status_name(r.finish) == "CRC_MISMATCH", f"{r.finish}")
    # no space
    total = (await c.request("STORAGE_INFO", char=4))["fields"]["total"]
    board.set_storage_total(4000)
    await c.begin(ASSET, "full.bg", BG, asset_type=BACKGROUND, want="NO_SPACE")
    board.set_storage_total(total)
    # storage write failure mid-transfer
    b = await c.begin(ASSET, "fail.bg", BG, asset_type=BACKGROUND)
    board.storage_budget(0)
    await c.chunk(b["handle"], 0, BG[:400])
    ack = (await c.next_event("XFER_ACK"))["fields"]
    await check(status_name({"fields": ack}) in ("NO_SPACE", "INTERNAL"), f"storage failure {ack}")
    board.storage_budget(-1)
    await c.request("XFER_ABORT", {"handle": b["handle"]}, char=4)
    # data corrupted at rest between sessions: the resume uses the journaled prefix CRC, FINISH
    # re-reads the stored bytes and fails; the transfer is discarded and a new one starts at 0
    await c.upload(ASSET, "rest.bg", BG, asset_type=BACKGROUND, stop_at=1024)
    c = await s.reopen()
    board.corrupt_transfer()
    r = await c.upload(ASSET, "rest.bg", BG, asset_type=BACKGROUND)
    await check(r.begin["offset"] > 0 and status_name(r.finish) == "CRC_MISMATCH", f"at-rest corruption {r.finish}")
    r = await c.upload(ASSET, "rest.bg", BG, asset_type=BACKGROUND)
    await check(r.begin["offset"] == 0 and status_name(r.finish) == "OK", "fresh transfer after the failure")
    await c.request("ASSET_DELETE", {"asset_type": BACKGROUND, "id": "rest.bg"}, char=4)


async def flow_resume_after_reboot(s: Session) -> None:
    c, board = s.client, s.board
    font = asset(FONT, FONT_V1, pattern(40000, 3))
    await c.upload(ASSET, "big.font", font, asset_type=FONT, stop_at=36000)
    board.reboot()  # RAM lost; the journal holds the last journaled offset
    await asyncio.wait_for(c.link.closed.wait(), 5)
    c = await s.open()
    r = await c.upload(ASSET, "big.font", font, asset_type=FONT)
    await check(r.begin["offset"] == 32768, f"resumed from the journal at {r.begin['offset']}")
    await check(status_name(r.finish) == "OK", "finish")
    await c.request("ASSET_DELETE", {"asset_type": FONT, "id": "big.font"}, char=4)


# --------------------------------------------------------------------------- OTA

def bump(version: str) -> str:
    major, minor, patch = (int(x) for x in version.split("+")[0].split("-")[0].split("."))
    return f"{major}.{minor}.{patch + 1}"


async def flow_ota_dry_run(s: Session) -> None:
    """Full firmware transfer, then XFER_ABORT instead of FINISH: safe on real hardware."""
    c = s.client
    caps = (await c.capabilities())["fields"]
    image = pattern(64 * 1024, 5)
    r = await c.upload(FIRMWARE, "firmware", image, version=bump(caps["firmware_version"]), finish=False)
    st = (await c.request("XFER_STATUS", {"handle": r.begin["handle"]}, char=4))["fields"]
    await check(st["offset"] == len(image) and st["prefix_crc32"] == zlib.crc32(image), f"all bytes stored {st}")
    await c.request("XFER_ABORT", {"handle": r.begin["handle"]}, char=4)


async def ota_install(s: Session, image: bytes, version: str) -> None:
    r = await s.client.upload(FIRMWARE, "firmware", image, version=version)
    await check(status_name(r.finish) == "OK", f"finish {r.finish}")
    await s.wait(1100)  # the board reboots about 1 s later
    await asyncio.wait_for(s.client.link.closed.wait(), 5)


async def flow_ota_install(s: Session) -> None:
    old = (await s.client.capabilities())["fields"]["firmware_version"]
    new = bump(old)
    await ota_install(s, pattern(100 * 1024, 6), new)
    c = await s.open()
    caps = (await c.capabilities())["fields"]
    await check(caps["firmware_version"] == new, f"running {caps['firmware_version']}")
    await check((await c.status())["fields"]["ota_state"] == 3, "ota_state updated")


async def flow_ota_rollback(s: Session) -> None:
    old = (await s.client.capabilities())["fields"]["firmware_version"]
    s.board.set_self_check_fail(True)
    try:
        await ota_install(s, pattern(8192, 7), bump(old))
        await s.wait(1100)  # the new image fails its self-check and asks for a rollback reboot
        c = await s.open()
        caps = (await c.capabilities())["fields"]
        await check(caps["firmware_version"] == old, f"rolled back to {caps['firmware_version']}")
        await check((await c.status())["fields"]["ota_state"] == 2, "ota_state rolled_back")
    finally:
        s.board.set_self_check_fail(False)


async def flow_ota_refusals(s: Session) -> None:
    c, board = s.client, s.board
    run = (await c.capabilities())["fields"]["firmware_version"]
    img = pattern(4096, 8)
    board.set_battery(10)
    await c.begin(FIRMWARE, "firmware", img, version=bump(run), want="LOW_BATTERY")
    board.set_battery(10, external_power=1)
    b = await c.begin(FIRMWARE, "firmware", img, version=bump(run))  # external power: allowed
    await c.request("XFER_ABORT", {"handle": b["handle"]}, char=4)
    board.set_battery(80)
    await c.begin(FIRMWARE, "firmware", img, version=run, want="VERSION_REJECTED")
    await c.begin(FIRMWARE, "firmware", img, want="VERSION_REJECTED")
    await c.begin(FIRMWARE, "fw", img, version=bump(run), want="INVALID_ID")
    board.set_fw_image_status(REG.status_by_name["SIGNATURE_INVALID"])
    try:
        r = await c.upload(FIRMWARE, "firmware", img, version=bump(run))
        await check(status_name(r.finish) == "SIGNATURE_INVALID", f"{r.finish}")
    finally:
        board.set_fw_image_status(0)


# --------------------------------------------------------------------------- board state

async def flow_security(s: Session) -> None:
    c, board = s.client, s.board
    board.set_secure(False)
    try:
        await c.request("CONFIG_GET", want="INSUFFICIENT_SECURITY")
        await c.request("NOTIF_CLEAR", char=3, want="INSUFFICIENT_SECURITY")
        await check(await c.status() is None and await c.battery_level() is None, "no Status/Battery reads")
        await check((await c.capabilities())["type"] == "CAPABILITIES", "Capabilities stays readable")
    finally:
        board.set_secure(True)
    await c.request("CONFIG_GET")


async def flow_device_status(s: Session) -> None:
    c, board = s.client, s.board
    c.drain_events()
    board.set_battery(55)
    ev = await c.next_event("DEVICE_STATUS")
    await check(ev["fields"]["battery_pct"] == 55, f"{ev}")
    await s.wait(50)
    await check(55 in c.battery, f"Battery Level notified {c.battery}")
    await check(await c.battery_level() == 55, "Battery Level read")
    board.set_pairing(True)
    ev = await c.next_event("DEVICE_STATUS")
    await check(ev["fields"]["pairing_mode"] is True, "pairing mode")
    board.set_pairing(False)
    board.set_battery(80)
    c.drain_events()


async def flow_ancs(s: Session) -> None:
    c, board = s.client, s.board
    c.drain_events()
    board.ancs(event_id=0, uid=42, app_id="com.apple.MobileSMS", app_name="Tin nhắn", title="Mẹ",
               message="Con về ăn cơm chưa?", date="20261011T183000", category=4)
    seen = await c.next_event("APP_SEEN")
    await check(seen["fields"]["app"]["app_id"] == "com.apple.MobileSMS" and seen["fields"]["app"]["source"] == 1,
                f"{seen}")
    await check(board.state()["shown"] == 1, "ANCS notification shown")
    board.ancs_reset()
    await check(board.state()["notifications"] == 0, "session reset drops ANCS notifications")
    c.drain_events()


async def flow_screen(s: Session) -> None:
    """NOTIF_POST appears on the simulated panel after the refresh delay; the full-refresh cadence
    follows display.full_refresh_every (a card counts a quarter, and a full refresh waits while cards are shown:
    PLAN D83, so with 1 it comes at the ninth card at the latest)."""
    c, board = s.client, s.board
    if not board.screen:
        raise AssertionError("the fake board has no screen (libepager_core.so)")
    await c.request("CONFIG_SET", {"entry": [{"key": 3, "value": "01"}]})
    await s.wait(3300)  # the "Setting updated" message (D79) goes after three seconds: one more refresh
    for _ in range(100):  # earlier changes or a reboot may still be refreshing the panel
        await s.wait(100)
        if not board.display_state().refreshing:
            break
    d0 = board.display_state()
    await check(not d0.refreshing, "display idle")
    kinds = []
    for i in range(10):
        before = board.frame()
        await c.request("NOTIF_POST", {"key": f"s{i}", "app_id": "com.zing.zalo", "app_name": "Zalo",
                                       "title": f"Tin nhắn {i}"})
        await s.wait(100)
        await check(board.frame() == before and board.display_state().refreshing, "not shown before the refresh ends")
        kind = board.display_state().kind
        await s.wait(2000 if kind == "full" else 300)
        await check(board.frame() != before, "shown after the refresh")
        await check(f"Tin nhắn {i}" in board.screen_text(), "card text")
        kinds.append(kind[0].upper())
        await s.wait(2200)  # the card's minimum time on the panel (D81): the next one waits for it otherwise
    seq = "".join(kinds)
    await check("F" in seq and seq.index("F") <= 8 and "FF" not in seq, f"refresh sequence {seq}: one full by the ninth card")
    d1 = board.display_state()
    await check(d1.violations == 0 and d1.full - d0.full == seq.count("F"), f"{d1}")
    await c.request("CONFIG_RESET")
    await c.request("NOTIF_CLEAR", char=3)
    c.drain_events()


# name -> (flow, needs the in-process fake board)
FLOWS: dict[str, tuple[Callable[[Session], Awaitable[None]], bool]] = {
    "capabilities": (flow_capabilities, False),
    "ping": (flow_ping, False),
    "errors": (flow_errors, False),
    "config": (flow_config, False),
    "time_sync": (flow_time_sync, False),
    "notifications": (flow_notifications, False),
    "filters_and_apps": (flow_filters_and_apps, False),
    "asset_resume": (flow_asset_resume, False),
    "transfer_errors": (flow_transfer_errors, False),
    "app_icons": (flow_app_icons, False),
    "ota_dry_run": (flow_ota_dry_run, False),
    "transfer_board_failures": (flow_transfer_board_failures, True),
    "resume_after_reboot": (flow_resume_after_reboot, True),
    "ota_refusals": (flow_ota_refusals, True),
    "ota_install": (flow_ota_install, True),
    "ota_rollback": (flow_ota_rollback, True),
    "security": (flow_security, True),
    "device_status": (flow_device_status, True),
    "ancs": (flow_ancs, True),
    "screen": (flow_screen, True),
    "app_icons_screen": (flow_app_icons_screen, True),
}


async def run_flows(s: Session, names: list[str] | None = None) -> dict[str, str]:
    """Run flows in order; returns name -> 'ok' | 'skipped' | 'FAILED: ...'."""
    results = {}
    for name in names or list(FLOWS):
        fn, needs_board = FLOWS[name]
        if needs_board and s.board is None:
            results[name] = "skipped (needs the in-process fake board)"
            continue
        if s.client is None or s.client.link.closed.is_set():
            await s.open()
        try:
            await fn(s)
            results[name] = "ok"
        except Exception as e:  # report every flow, then fail
            results[name] = f"FAILED: {type(e).__name__}: {e}"
        s.log(f"{name}: {results[name]}")
    return results
