"""Reference ANCS helpers (Apple ANCS spec; byte layouts cross-checked with ESP-IDF's ble_ancs example)."""
from __future__ import annotations

import struct

CMD_GET_NOTIF_ATTRS = 0
CMD_GET_APP_ATTRS = 1
CMD_PERFORM_ACTION = 2

ATTR_APP_ID, ATTR_TITLE, ATTR_SUBTITLE, ATTR_MESSAGE, ATTR_MESSAGE_SIZE, ATTR_DATE = range(6)
ATTRS_WITH_MAX_LEN = {ATTR_TITLE, ATTR_SUBTITLE, ATTR_MESSAGE}
APP_ATTR_DISPLAY_NAME = 0


class AncsError(Exception):
    def __init__(self, status: str, detail: str = ""):
        super().__init__(f"{status}: {detail}")
        self.status = status


def parse_notification_source(pkt: bytes) -> dict:
    """8 bytes: EventID, EventFlags, CategoryID, CategoryCount, NotificationUID (u32 LE).
    Longer packets are accepted (extra bytes ignored)."""
    if len(pkt) < 8:
        raise AncsError("MALFORMED", "short notification source")
    ev, flags, cat, count, uid = struct.unpack_from("<BBBBI", pkt)
    return {"event_id": ev, "event_flags": flags, "category_id": cat, "category_count": count, "uid": uid}


def build_get_notif_attrs(uid: int, attrs: list[tuple[int, int | None]]) -> bytes:
    out = bytearray([CMD_GET_NOTIF_ATTRS]) + struct.pack("<I", uid)
    for aid, max_len in attrs:
        out.append(aid)
        if aid in ATTRS_WITH_MAX_LEN:
            out += struct.pack("<H", max_len)
    return bytes(out)


def build_get_app_attrs(app_id: bytes, attr_ids: list[int]) -> bytes:
    return bytes([CMD_GET_APP_ATTRS]) + app_id + b"\x00" + bytes(attr_ids)


def build_perform_action(uid: int, action_id: int) -> bytes:
    return bytes([CMD_PERFORM_ACTION]) + struct.pack("<I", uid) + bytes([action_id])


class DataSourceReassembler:
    """Reassemble one Data Source response for a known outstanding request.

    feed() returns None while incomplete, or a dict {attr_id: bytes} when complete.
    Raises AncsError(MALFORMED) on header/order mismatch, AncsError(TOO_LARGE) past capacity.
    Trailing bytes after a complete response are ignored."""

    def __init__(self, command: int, attr_ids: list[int], uid: int | None = None,
                 app_id: bytes | None = None, capacity: int = 2048):
        self.command, self.attr_ids, self.uid, self.app_id = command, attr_ids, uid, app_id
        self.capacity = capacity
        self.buf = bytearray()
        self.done = False

    def feed(self, frag: bytes) -> dict | None:
        if self.done:
            return None
        if len(self.buf) + len(frag) > self.capacity:
            raise AncsError("TOO_LARGE")
        self.buf += frag
        return self._try_parse()

    def _try_parse(self) -> dict | None:
        b = self.buf
        if not b:
            return None
        if b[0] != self.command:
            raise AncsError("MALFORMED", "command id")
        i = 1
        if self.command == CMD_GET_NOTIF_ATTRS:
            if len(b) < 5:
                return None
            if struct.unpack_from("<I", b, 1)[0] != self.uid:
                raise AncsError("MALFORMED", "uid")
            i = 5
        else:
            nul = b.find(0, 1)
            if nul < 0:
                return None
            if bytes(b[1:nul]) != self.app_id:
                raise AncsError("MALFORMED", "app id")
            i = nul + 1
        attrs: dict[int, bytes] = {}
        for aid in self.attr_ids:
            if len(b) - i < 3:
                return None
            if b[i] != aid:
                raise AncsError("MALFORMED", "attribute order")
            ln = struct.unpack_from("<H", b, i + 1)[0]
            if len(b) - i - 3 < ln:
                return None
            attrs[aid] = bytes(b[i + 3:i + 3 + ln])
            i += 3 + ln
        self.done = True
        return attrs


def _days_from_civil(y: int, m: int, d: int) -> int:
    y -= m <= 2
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def parse_date(s: bytes) -> int:
    """'yyyyMMdd'T'HHmmSS' (local time) -> seconds since 1970-01-01T00:00 local.
    SS is treated as seconds (that is what iOS sends). Years 2000-2099 only."""
    if len(s) != 15 or s[8:9] != b"T" or not (s[:8] + s[9:]).isdigit():
        raise AncsError("MALFORMED", "date format")
    y, mo, d = int(s[0:4]), int(s[4:6]), int(s[6:8])
    h, mi, se = int(s[9:11]), int(s[11:13]), int(s[13:15])
    leap = y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)
    mdays = [31, 29 if leap else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    if not (2000 <= y <= 2099 and 1 <= mo <= 12 and 1 <= d <= mdays[mo - 1] and h < 24 and mi < 60 and se < 60):
        raise AncsError("OUT_OF_RANGE", "date value")
    return _days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se
