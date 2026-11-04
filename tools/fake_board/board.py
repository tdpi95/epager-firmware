"""FakeBoard: the C board core (plus the simulated Paper or LCD screen) behind a thread-safe
Python object that transports (TCP, BLE) and tools drive.

A transport implements `Link`: it receives the board's notifications and is told when the board
drops the connection (reboot after an OTA, power off). One link at a time, like the firmware.
"""
from __future__ import annotations

import collections
import threading
import time
from dataclasses import dataclass
from typing import Callable, Protocol

from . import native as N
from .native import Event, Native

CATEGORIES = ("other", "incoming_call", "missed_call", "voicemail", "social", "schedule", "email", "news",
              "health_and_fitness", "business_and_finance", "location", "entertainment")
PRIORITIES = ("low", "normal", "high", "urgent")
OTA_STATES = ("none", "pending_verify", "rolled_back", "updated")


class Link(Protocol):
    def notify(self, char: int, value: bytes) -> None: ...   # must not block
    def drop(self) -> None: ...                               # the board closed the link


@dataclass
class BoardConfig:
    screen: bool = True                 # a simulator library (screen) or libepager_core.so
    board: str = "paper"               # "paper" (200x200 e-paper) or "lcd" (296x240 LCD, touch)
    storage_total: int = 1966080        # asset storage (Paper partition plan: 1920 KiB)
    fw_slot: int = 3 * 1024 * 1024      # OTA slot
    version: str = "0.1.0"
    allow_downgrade: bool = False
    device_name: str = "ePager-FA4E"
    secure: bool = True                 # links are treated as encrypted and bonded
    mtu: int = 517


class FakeBoard:
    def __init__(self, cfg: BoardConfig | None = None, clock: str = "real",
                 on_event: Callable[[Event], None] | None = None):
        self.cfg = cfg or BoardConfig()
        self.n = Native(screen=self.cfg.screen, board=self.cfg.board)
        self.lock = threading.RLock()
        self.clock = clock
        self.on_event = on_event
        self.events: collections.deque[Event] = collections.deque(maxlen=2000)
        self.link: Link | None = None
        self.powered = True
        self._t0 = time.monotonic()
        self._now = 0
        self._ticker: threading.Thread | None = None
        self._stop = threading.Event()
        with self.lock:
            self.n.init(self.cfg.storage_total, self.cfg.fw_slot, self.cfg.version, self.cfg.allow_downgrade,
                        self.cfg.device_name)
            self._pump()

    @property
    def screen(self) -> bool:
        return self.n.screen

    @property
    def size(self) -> tuple[int, int, int]:
        """Panel width, height and bytes per pixel of panel() (1 grey, 3 RGB)."""
        return self.n.width, self.n.height, self.n.channels

    # ------------------------------------------------------------------ time

    def now_ms(self) -> int:
        if self.clock == "real":
            return int((time.monotonic() - self._t0) * 1000) & 0xFFFFFFFF
        return self._now

    def tick(self) -> None:
        with self.lock:
            self.n.set_time(self.now_ms())
            self.n.tick()
            self._pump()

    def advance(self, ms: int, step: int = 10) -> None:
        """Manual clock: move time forward in steps, ticking the board (refresh timing, OTA reboot)."""
        assert self.clock == "manual", "advance() needs clock='manual'"
        end = self._now + ms
        while self._now < end:
            self._now = min(end, self._now + step)
            self.tick()

    def start(self, period_s: float = 0.02) -> None:
        """Real clock: tick in a background thread."""
        if self._ticker:
            return
        self._stop.clear()

        def run() -> None:
            while not self._stop.wait(period_s):
                self.tick()

        self._ticker = threading.Thread(target=run, name="fake-board-tick", daemon=True)
        self._ticker.start()

    def stop(self) -> None:
        self._stop.set()
        if self._ticker:
            self._ticker.join()
            self._ticker = None

    # ------------------------------------------------------------------ link

    def connect(self, link: Link, mtu: int | None = None, secure: bool | None = None) -> bool:
        with self.lock:
            if self.link is not None or not self.powered:
                return False
            self.link = link
            self.n.connect(mtu or self.cfg.mtu, int(self.cfg.secure if secure is None else secure))
            self._pump()
            return True

    def disconnect(self, link: Link) -> None:
        with self.lock:
            if self.link is link:
                self.n.disconnect()
                self.link = None
                self._pump()

    def write(self, link: Link, char: int, value: bytes) -> None:
        with self.lock:
            if self.link is link:
                self.n.write(char, value)
                self._pump()

    def read(self, link: Link, char: int) -> bytes:
        with self.lock:
            return self.n.read(char) if self.link is link else b""

    def set_secure(self, secure: bool) -> None:
        with self.lock:
            self.cfg.secure = secure
            self.n.set_secure(int(secure))

    def drop_link(self) -> None:
        """Simulate a link loss (out of range)."""
        with self.lock:
            link = self.link
            if link:
                self.disconnect(link)
                link.drop()

    # ------------------------------------------------------------------ simulation controls

    def _call(self, fn: Callable, *args):
        with self.lock:
            r = fn(*args)
            self._pump()
            return r

    def reboot(self) -> None:
        self._call(self.n.reboot)

    def set_battery(self, pct: int, external_power: int = 0, readable: bool = True, reader: bool = True) -> None:
        self._call(self.n.set_battery, int(reader), int(readable), pct, external_power)

    def set_utc(self, utc_s: int) -> None:
        """The soft clock only, as if it had been running; TIME_SYNC (loopback.time_sync) also writes the RTC."""
        self._call(self.n.set_utc, utc_s)

    def set_rtc(self, present: bool, utc_s: int = 0) -> None:
        """Simulated RTC: absent, or holding utc_s (0 = no valid time). Presence applies at the next boot."""
        self._call(self.n.set_rtc, int(present), utc_s)

    def set_climate(self, temp_c: float = 24.5, rh_pct: float = 50.0, readable: bool = True,
                    present: bool = True) -> None:
        """Temperature/humidity sensor reading, shown at once. present=False: no sensor from the next boot."""
        self._call(self.n.set_climate, int(present), int(readable), round(temp_c * 100), round(rh_pct * 100))

    def set_pairing(self, on: bool) -> None:
        self._call(self.n.set_pairing, int(on))

    def set_bonded(self, n: int) -> None:
        """The number of bonded phones (the Paper's settings page lists them)."""
        self._call(self.n.set_bonded, n)

    def set_passkey(self, passkey: int | None) -> None:
        """The code a pairing phone asks for, shown on the screen (None: pairing finished)."""
        self._call(self.n.set_passkey, -1 if passkey is None else passkey)

    def set_weather(self, temp_c: float | None, wmo_code: int = 0, is_day: bool = True, high_c: float | None = None,
                    low_c: float | None = None, precip_pct: int | None = None, place: str = "") -> None:
        """A weather reading on the idle screen (as the firmware's fetch gives it); temp_c None removes it.
        Also reports weather_state ok (or waiting) as the firmware does."""
        def c10(v: float | None) -> int:
            return N.TEMP_UNKNOWN if v is None else round(v * 10)
        self._call(self.n.weather, temp_c is not None, c10(temp_c), wmo_code, is_day, c10(high_c), c10(low_c),
                   N.PCT_UNKNOWN if precip_pct is None else precip_pct, place)
        self._call(self.n.set_weather_state, N.WEATHER_STATES.index("ok" if temp_c is not None else "waiting"))

    def weather_json(self, doc: bytes, place: str = "") -> str:
        """An Open-Meteo forecast document through the board's parser (shown with `place` if it parses): the
        wx_result_t name."""
        r = self._call(self.n.weather_json, doc, place)
        if r == 0:
            self._call(self.n.set_weather_state, N.WEATHER_STATES.index("ok"))
        return N.WX_RESULTS[r] if 0 <= r < len(N.WX_RESULTS) else str(r)

    def set_wifi_state(self, state: str) -> None:
        self._call(self.n.set_wifi_state, N.WIFI_STATES.index(state))

    def set_weather_state(self, state: str) -> None:
        self._call(self.n.set_weather_state, N.WEATHER_STATES.index(state))

    def wifi_network(self) -> tuple[str, str]:
        """The network WIFI_SET stored ("" = none) and its password (the board never sends it back)."""
        with self.lock:
            return self.n.wifi_network()

    def wifi_setup(self, state: str, ap_ssid: str = "", ap_password: str = "", url: str = "",
                   network: str = "") -> None:
        """The Wi-Fi setup screen as the firmware drives it (off, waiting, joining, done, failed)."""
        self._call(self.n.wifi_setup, state, ap_ssid, ap_password, url, network)

    def set_update(self, state: str, error: str = "none", version: str = "", size: int = 0,
                   skipped: bool = False) -> None:
        """The firmware update as the UI shows it: the Software update page and the clock screen's mark."""
        self._call(self.n.set_update, state, error, version, size, skipped)

    def update_prompt(self) -> bool:
        """Open the update prompt (only from the awake clock screen); False if it did not open."""
        return bool(self._call(self.n.update_prompt))

    def clock_face_shown(self) -> str:
        """The id of the clock face the idle screen draws; "" when it is Classic."""
        return self._call(self.n.clock_face_shown)

    def clock_face_errors(self) -> int:
        """How many times a selected clock face could not be shown (and Classic was used instead)."""
        return self._call(self.n.clock_face_errors)

    def set_storage_total(self, total: int) -> None:
        self._call(self.n.set_storage_total, total)

    def blob_put(self, name: str, data: bytes) -> None:
        """A file straight into the asset storage, past the transfer layer's checks (a face damaged after its install)."""
        self._call(self.n.blob_put, name.encode(), data, len(data))

    def storage_budget(self, writes: int) -> None:
        """Storage fails after `writes` more writes (power-cut style); -1 restores it."""
        self._call(self.n.storage_budget, writes)

    def corrupt_transfer(self) -> bool:
        return bool(self._call(self.n.corrupt_transfer))

    def set_fw_image_status(self, status: int) -> None:
        self._call(self.n.set_fw_image_status, status)

    def set_self_check_fail(self, fail: bool) -> None:
        self._call(self.n.set_self_check_fail, int(fail))

    def ancs(self, **kw) -> None:
        self._call(lambda: self.n.ancs(**kw))

    def ancs_reset(self) -> None:
        self._call(self.n.ancs_reset)

    def android(self, **kw) -> tuple[int, int]:
        return self._call(lambda: self.n.android(**kw))

    def android_remove(self, key: str) -> int:
        return self._call(self.n.android_remove, key)

    def button(self, btn: int, long_press: bool = False, hold: bool = False) -> None:
        """A key press: short, long (1.5 s on the Paper) or hold (PWR 5 s, power off)."""
        self._call(self.n.button, btn, 2 if hold else int(long_press))

    def touch(self, pressed: bool, x: int = 0, y: int = 0) -> None:
        """LCD: the finger is down at (x, y), or lifted."""
        self._call(self.n.touch, pressed, x, y)

    def set_ghosting(self, on: bool) -> None:
        self._call(self.n.set_ghosting, int(on))

    def set_refresh_timing(self, partial_ms: int, full_ms: int) -> None:
        self._call(self.n.set_refresh_timing, partial_ms, full_ms)

    # ------------------------------------------------------------------ state

    def state(self) -> dict:
        with self.lock:
            n = self.n
            return {"version": n.version(), "connected": bool(n.connected()), "powered": self.powered,
                    "battery_pct": n.battery_pct(), "notifications": n.notif_count(0),
                    "shown": n.notif_count(1), "config_revision": n.config_revision(),
                    "ota_state": OTA_STATES[n.ota_state()] if n.ota_state() < 4 else n.ota_state(),
                    "utc": n.now_utc(), "rtc": n.rtc_utc(), "wifi_network": n.wifi_network()[0] or None}

    def panel(self) -> bytes:
        with self.lock:
            return self.n.panel()

    def frame(self) -> bytes:
        with self.lock:
            return self.n.frame()

    def display_state(self) -> N.DisplayState:
        with self.lock:
            return self.n.display_state()

    def screen_text(self) -> str:
        with self.lock:
            return self.n.screen_text()

    # ------------------------------------------------------------------ internals

    def _pump(self) -> None:
        while (note := self.n.poll()) is not None:
            if self.link:
                self.link.notify(*note)
        while (ev := self.n.poll_event()) is not None:
            self.events.append(ev)
            if ev.type == N.EV_DISCONNECT and self.link:
                link, self.link = self.link, None
                link.drop()
            elif ev.type == N.EV_POWER:
                self.powered = bool(ev.a)
            if self.on_event:
                self.on_event(ev)


def describe(ev: Event) -> str:
    """One readable line (plus the rendered text) for the console. Development tool output: the
    firmware never logs notification content."""
    if ev.type == N.EV_NOTIF:
        n = ev.notif or {}
        cat = CATEGORIES[n["category"]] if n["category"] < len(CATEGORIES) else n["category"]
        pri = PRIORITIES[n["priority"]] if n["priority"] < len(PRIORITIES) else n["priority"]
        src = {1: "ancs", 2: "android"}.get(n["source"], "?")
        head = (f"[notif] {n['kind']} #{n['id']} {'shown' if n['shown'] else 'hidden'} {src} "
                f"app={n['app_name'] or '-'} ({n['app_id']}) {cat}/{pri} flags=0x{n['flags']:02x}")
        lines = [head]
        for k in ("title", "subtitle", "body"):
            if n.get(k):
                lines.append(f"    {k}: {n[k]}")
        return "\n".join(lines)
    if ev.type == N.EV_CONFIG:
        return f"[config] revision {ev.b}, {ev.a} key(s) changed"
    if ev.type == N.EV_OTA:
        kind = N.OTA_EVENTS[ev.a] if ev.a < len(N.OTA_EVENTS) else ev.a
        if kind == "progress":
            return f"[ota] {ev.b} %"
        if kind == "state":
            return f"[ota] state {OTA_STATES[ev.c] if ev.c < 4 else ev.c}"
        return f"[ota] {kind}"
    if ev.type == N.EV_DISCONNECT:
        return "[link] dropped by the board"
    if ev.type == N.EV_REBOOT:
        return f"[boot] firmware {ev.text}{' (new image, self-check)' if ev.a else ''}"
    if ev.type == N.EV_ACTION:
        name = N.ACTIONS.get(ev.a, ev.a)
        return f"[action] {name}" + (f" #{ev.b}" if name == "dismiss" else "")
    if ev.type == N.EV_POWER:
        return f"[power] {'on' if ev.a else 'off'}"
    return f"[event {ev.type}] {ev.a} {ev.b} {ev.c}"
