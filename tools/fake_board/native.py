"""ctypes bindings for the fake board shim (tools/fake_board/native/fb.h)."""
from __future__ import annotations

import ctypes as C
from dataclasses import dataclass
from pathlib import Path

from . import build

WIDTH = HEIGHT = 200  # the Paper's panel; a Native reports its own size (screen_size)
FRAME_LEN = (WIDTH + 7) // 8 * HEIGHT
BOARDS = {"paper": "libepager_sim.so", "lcd": "libepager_sim_lcd.so"}

# Characteristics (protocol.md 10)
CAPABILITIES, CONFIG, NOTIFICATION, TRANSFER, STATUS, BATTERY = 1, 2, 3, 4, 5, 6
CHAR_NAMES = {1: "capabilities", 2: "config", 3: "notification", 4: "transfer", 5: "status", 6: "battery"}

EV_NOTIF, EV_CONFIG, EV_OTA, EV_DISCONNECT, EV_REBOOT, EV_ACTION, EV_POWER = range(1, 8)
NOTIF_KINDS = ("added", "modified", "removed")
OTA_EVENTS = ("progress", "reboot", "mark_valid", "rollback", "state")
BTN_BOOT, BTN_PWR, BTN_UP, BTN_DOWN, BTN_UP_DOWN, BTN_BOOT_PWR = 0, 1, 2, 3, 4, 5
ACTIONS = {1: "pairing", 2: "power", 3: "dismiss", 4: "brightness_up", 5: "brightness_down", 6: "wifi_setup",
           7: "wifi_setup_close"}  # ui_action_t
SETUP_STATES = ("off", "waiting", "joining", "done", "failed")  # ui_setup_state_t
UPDATE_STATES = ("none", "checking", "current", "available", "downloading", "failed", "needs-app")  # ui_update_state_t
UPDATE_ERRORS = ("none", "no-network", "net", "clock", "download", "rejected", "busy", "cancelled", "battery",
                 "internal")  # ui_update_error_t
WIFI_STATES = ("unconfigured", "idle", "connecting", "connected", "auth_failed", "not_found", "no_internet")
WEATHER_STATES = ("off", "waiting", "ok", "stale", "locate_failed", "fetch_failed")
WX_RESULTS = ("ok", "format", "not_found", "api")  # wx_result_t


class _Event(C.Structure):
    _fields_ = [("type", C.c_uint32), ("a", C.c_uint32), ("b", C.c_uint32), ("c", C.c_uint32),
                ("id", C.c_uint32), ("timestamp", C.c_uint32),
                ("source", C.c_uint8), ("category", C.c_uint8), ("priority", C.c_uint8), ("flags", C.c_uint8),
                ("shown", C.c_uint8), ("pad", C.c_uint8 * 3),
                ("app_id", C.c_char * 65), ("app_name", C.c_char * 65), ("title", C.c_char * 257),
                ("subtitle", C.c_char * 257), ("body", C.c_char * 1025), ("text", C.c_char * 64)]


LINK_ICONS = {1: "connected", 2: "disconnected", 3: "off"}  # ui_link_icon_t
WIFI_ICONS = {1: "ok", 2: "connecting", 3: "failed"}  # ui_wifi_icon_t


class _Display(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ("refreshing", "asleep", "kind", "phase", "elapsed_ms", "partial", "full",
                                          "violations", "ui_partial", "ui_full", "ui_skipped",
                                          "partials_since_full", "powered", "link", "backlight", "wifi",
                                          "quiet")]


@dataclass
class Event:
    type: int
    a: int = 0
    b: int = 0
    c: int = 0
    notif: dict | None = None
    text: str = ""


@dataclass
class DisplayState:
    refreshing: bool
    asleep: bool
    kind: str          # "partial" | "full" (current or last refresh)
    phase: int
    elapsed_ms: int
    partial: int       # completed by the panel
    full: int
    violations: int
    ui_partial: int    # started by the UI
    ui_full: int
    ui_skipped: int
    partials_since_full: int
    powered: bool
    link: str          # status-bar connection icon: "connected" | "disconnected" | "off" (quiet hours) | "none"
    backlight: int     # LCD: percent set last (e-paper: 0)
    wifi: str          # status-bar Wi-Fi icon: "ok" | "connecting" | "failed" | "none"
    quiet: bool = False  # status-bar quiet-hours moon (PLAN D126)


def _s(b: bytes) -> str:
    return b.decode("utf-8", "replace")


_SIGS = {
    "fb_event_size": ([], C.c_size_t), "fb_display_size": ([], C.c_size_t), "fb_has_screen": ([], C.c_int),
    "fb_init": ([C.c_uint32, C.c_uint32, C.c_char_p, C.c_int, C.c_char_p], C.c_int),
    "fb_reboot": ([], None), "fb_set_time": ([C.c_uint32], None), "fb_tick": ([], None),
    "fb_connect": ([C.c_uint16, C.c_int], None), "fb_disconnect": ([], None), "fb_set_secure": ([C.c_int], None),
    "fb_connected": ([], C.c_int),
    "fb_write": ([C.c_uint8, C.c_char_p, C.c_size_t], None),
    "fb_read": ([C.c_uint8, C.c_void_p, C.c_size_t], C.c_size_t),
    "fb_poll": ([C.POINTER(C.c_uint8), C.c_void_p, C.c_size_t], C.c_int),
    "fb_poll_event": ([C.POINTER(_Event)], C.c_int),
    "fb_set_battery": ([C.c_int, C.c_int, C.c_uint8, C.c_uint8], None),
    "fb_set_pairing": ([C.c_int], None), "fb_set_passkey": ([C.c_int32], None), "fb_set_utc": ([C.c_uint32], None), "fb_set_bonded": ([C.c_uint8], None),
    "fb_set_rtc": ([C.c_int, C.c_uint32], None), "fb_rtc_utc": ([], C.c_uint32),
    "fb_set_climate": ([C.c_int, C.c_int, C.c_int16, C.c_uint16], None),
    "fb_set_storage_total": ([C.c_uint32], None), "fb_storage_budget": ([C.c_int32], None),
    "fb_blob_put": ([C.c_char_p, C.c_char_p, C.c_size_t], C.c_int),
    "fb_corrupt_transfer": ([], C.c_int), "fb_set_fw_image_status": ([C.c_uint8], None),
    "fb_set_self_check_fail": ([C.c_int], None),
    "fb_ancs": ([C.c_uint8, C.c_uint8, C.c_uint8, C.c_uint32] + [C.c_char_p] * 6, None),
    "fb_ancs_reset": ([], None),
    "fb_android": ([C.c_char_p] * 6 + [C.c_uint8] * 3 + [C.POINTER(C.c_uint32)], C.c_int),
    "fb_android_remove": ([C.c_char_p], C.c_int),
    "fb_config_int": ([C.c_uint16], C.c_int32), "fb_config_revision": ([], C.c_uint32),
    "fb_notif_count": ([C.c_int], C.c_uint32), "fb_ota_state": ([], C.c_uint8),
    "fb_battery_pct": ([], C.c_uint8), "fb_now_utc": ([], C.c_uint32),
    "fb_version": ([C.c_char_p, C.c_size_t], C.c_size_t),
    "fb_panel": ([C.c_void_p], None), "fb_frame": ([C.c_void_p], None),
    "fb_display_state": ([C.POINTER(_Display)], None), "fb_button": ([C.c_uint8, C.c_int], None),
    "fb_set_ghosting": ([C.c_int], None), "fb_set_refresh_timing": ([C.c_uint32, C.c_uint32], None),
    "fb_screen_text": ([C.c_char_p, C.c_size_t], C.c_size_t),
    "fb_screen_info": ([C.POINTER(C.c_uint16), C.POINTER(C.c_uint16), C.POINTER(C.c_uint8)], None),
    "fb_touch": ([C.c_int, C.c_uint16, C.c_uint16], None),
    "fb_wifi_network": ([C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t], C.c_size_t),
    "fb_set_wifi_state": ([C.c_uint8], None), "fb_set_weather_state": ([C.c_uint8], None),
    "fb_weather": ([C.c_int, C.c_int16, C.c_int32, C.c_int, C.c_int16, C.c_int16, C.c_uint8, C.c_char_p], None),
    "fb_weather_json": ([C.c_char_p, C.c_size_t, C.c_char_p], C.c_int),
    "fb_wifi_setup": ([C.c_uint8] + [C.c_char_p] * 4, None),
    "fb_set_update": ([C.c_uint8, C.c_uint8, C.c_char_p, C.c_uint32, C.c_int], None),
    "fb_update_prompt": ([], C.c_int),
    "fb_clock_face_shown": ([C.c_char_p, C.c_size_t], C.c_size_t), "fb_clock_face_errors": ([], C.c_uint32),
}

TEMP_UNKNOWN, PCT_UNKNOWN = -32768, 255  # WX_TEMP_UNKNOWN, WX_PCT_UNKNOWN


def _enc(s: str | None) -> bytes | None:
    return None if s is None else s.encode("utf-8")


class Native:
    """One loaded shim library (one board per process: the library keeps global state)."""

    _loaded: dict[Path, C.CDLL] = {}

    def __init__(self, screen: bool = True, lib_dir: Path | None = None, board: str = "paper"):
        d = lib_dir or build.lib_dir()
        path = d / (BOARDS[board] if screen else "libepager_core.so")
        if not path.exists():
            raise build.BuildError(f"{path} not found")
        lib = self._loaded.get(path)
        if lib is None:
            lib = self._loaded[path] = C.CDLL(str(path))
            for name, (args, res) in _SIGS.items():
                f = getattr(lib, name)
                f.argtypes, f.restype = args, res
        self.lib = lib
        if lib.fb_event_size() != C.sizeof(_Event) or lib.fb_display_size() != C.sizeof(_Display):
            raise build.BuildError("fb.h and native.py disagree on struct layouts")
        self.screen = bool(lib.fb_has_screen())
        self._buf = C.create_string_buffer(1024)
        w, h, ch = C.c_uint16(), C.c_uint16(), C.c_uint8()
        lib.fb_screen_info(C.byref(w), C.byref(h), C.byref(ch))
        self.width, self.height, self.channels = w.value, h.value, ch.value

    def __getattr__(self, name: str):
        return getattr(self.lib, "fb_" + name)

    def init(self, storage_total: int, fw_slot: int, version: str, allow_downgrade: bool, device_name: str) -> None:
        if self.lib.fb_init(storage_total, fw_slot, _enc(version), int(allow_downgrade), _enc(device_name)):
            raise MemoryError("fb_init")

    def write(self, char: int, data: bytes) -> None:
        self.lib.fb_write(char, data, len(data))

    def read(self, char: int) -> bytes:
        n = self.lib.fb_read(char, self._buf, len(self._buf))
        return self._buf.raw[:n]

    def poll(self) -> tuple[int, bytes] | None:
        chr_ = C.c_uint8()
        n = self.lib.fb_poll(C.byref(chr_), self._buf, len(self._buf))
        return None if n < 0 else (chr_.value, self._buf.raw[:n])

    def poll_event(self) -> Event | None:
        e = _Event()
        if not self.lib.fb_poll_event(C.byref(e)):
            return None
        ev = Event(e.type, e.a, e.b, e.c, text=_s(e.text))
        if e.type == EV_NOTIF:
            ev.notif = {"id": e.id, "kind": NOTIF_KINDS[e.a] if e.a < 3 else e.a, "shown": bool(e.shown),
                        "source": e.source, "category": e.category, "priority": e.priority, "flags": e.flags,
                        "timestamp": e.timestamp, "app_id": _s(e.app_id), "app_name": _s(e.app_name),
                        "title": _s(e.title), "subtitle": _s(e.subtitle), "body": _s(e.body)}
        return ev

    def ancs(self, event_id: int, uid: int, app_id: str = "", app_name: str = "", title: str = "",
             subtitle: str = "", message: str = "", date: str = "", flags: int = 0, category: int = 0) -> None:
        self.lib.fb_ancs(event_id, flags, category, uid, *(_enc(s) for s in (app_id, app_name, title, subtitle,
                                                                               message, date)))

    def android(self, key: str, app_id: str, app_name: str = "", title: str = "", subtitle: str = "",
                body: str = "", category: int = 0, priority: int = 1, flags: int = 0) -> tuple[int, int]:
        nid = C.c_uint32()
        st = self.lib.fb_android(*(_enc(s) for s in (key, app_id, app_name, title, subtitle, body)), category,
                                 priority, flags, C.byref(nid))
        return st, nid.value

    def android_remove(self, key: str) -> int:
        return self.lib.fb_android_remove(_enc(key))

    def wifi_network(self) -> tuple[str, str]:
        ssid, pw = C.create_string_buffer(64), C.create_string_buffer(64)
        self.lib.fb_wifi_network(ssid, len(ssid), pw, len(pw))
        return _s(ssid.value), _s(pw.value)

    def weather(self, present: bool, temp_c10: int = 0, wmo_code: int = 0, is_day: bool = True,
                high_c10: int = TEMP_UNKNOWN, low_c10: int = TEMP_UNKNOWN, precip_pct: int = PCT_UNKNOWN,
                place: str = "") -> None:
        self.lib.fb_weather(int(present), temp_c10, wmo_code, int(is_day), high_c10, low_c10, precip_pct,
                            _enc(place))

    def weather_json(self, doc: bytes, place: str = "") -> int:
        return self.lib.fb_weather_json(doc, len(doc), _enc(place))

    def set_update(self, state: str, error: str = "none", version: str = "", size: int = 0,
                   skipped: bool = False) -> None:
        self.lib.fb_set_update(UPDATE_STATES.index(state), UPDATE_ERRORS.index(error), _enc(version), size,
                               int(skipped))

    def update_prompt(self) -> bool:
        return bool(self.lib.fb_update_prompt())

    def clock_face_shown(self) -> str:
        n = self.lib.fb_clock_face_shown(self._buf, len(self._buf))
        return self._buf.raw[:n].decode()

    def clock_face_errors(self) -> int:
        return self.lib.fb_clock_face_errors()

    def wifi_setup(self, state: str, ap_ssid: str = "", ap_password: str = "", url: str = "",
                   network: str = "") -> None:
        self.lib.fb_wifi_setup(SETUP_STATES.index(state), *(_enc(s) for s in (ap_ssid, ap_password, url, network)))

    def version(self) -> str:
        n = self.lib.fb_version(self._buf, len(self._buf))
        return self._buf.raw[:n].decode()

    def panel(self) -> bytes:
        """What a person sees: width * height * channels bytes (grey for e-paper, RGB for the LCD)."""
        b = C.create_string_buffer(self.width * self.height * self.channels)
        self.lib.fb_panel(b)
        return b.raw

    def frame(self) -> bytes:
        """E-paper: the last completed 1-bit frame; LCD: the panel RAM (RGB565, little-endian)."""
        n = (self.width + 7) // 8 * self.height if self.channels == 1 else self.width * self.height * 2
        b = C.create_string_buffer(n)
        self.lib.fb_frame(b)
        return b.raw

    def touch(self, pressed: bool, x: int = 0, y: int = 0) -> None:
        self.lib.fb_touch(int(pressed), x, y)

    def display_state(self) -> DisplayState:
        d = _Display()
        self.lib.fb_display_state(C.byref(d))
        return DisplayState(bool(d.refreshing), bool(d.asleep), "full" if d.kind else "partial", d.phase,
                            d.elapsed_ms, d.partial, d.full, d.violations, d.ui_partial, d.ui_full, d.ui_skipped,
                            d.partials_since_full, bool(d.powered), LINK_ICONS.get(d.link, "none"), d.backlight,
                            WIFI_ICONS.get(d.wifi, "none"), bool(d.quiet))

    def screen_text(self) -> str:
        b = C.create_string_buffer(4096)
        n = self.lib.fb_screen_text(b, len(b))
        return b.raw[:n].decode("utf-8", "replace")
