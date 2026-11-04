"""Interactive console commands for the fake board (typed on stdin while it serves)."""
from __future__ import annotations

import shlex
import sys
import time
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from epager_proto import clockface as CF  # noqa: E402

from . import live_weather, loopback, png, sample_icons, scenes  # noqa: E402
from .board import FakeBoard  # noqa: E402
from .native import BTN_BOOT, BTN_BOOT_PWR, BTN_DOWN, BTN_PWR, BTN_UP, BTN_UP_DOWN

PRESETS = {
    "vi": ("Zalo", "com.zing.zalo", scenes.VI_TITLE, scenes.VI_BODY, 4),
    "emoji": ("Messenger", "com.facebook.orca", "Party 🎉🎉 tonight", "👨‍👩‍👧 Gia đình 🇻🇳 đã sẵn sàng ✅", 4),
    "long": ("Gmail", "com.google.android.gm",
             "Your order #4471 has shipped and is on its way to the delivery address you gave us",
             "Hello! Package 1 of 3 left our warehouse this morning. " * 30, 6),
    "call": ("Phone", "com.android.dialer", "Mẹ đang gọi", "", 1),
}
STATUS = {"ok": 0, "image_invalid": 21, "signature_invalid": 20}

HELP = """commands:
  post [vi|emoji|long|call]            inject an Android notification preset
  post "TITLE" ["BODY"] [app=NAME] [id=APP_ID] [key=KEY]
  remove KEY                           remove an Android notification
  ancs "TITLE" ["BODY"] [app=NAME] [id=APP_ID] [uid=N]   inject an iOS (ANCS) notification
  ancs-reset                           new ANCS session (drops ANCS notifications)
  icon APP_ID [zalo|ring|cross]        upload an app icon (through the transfer protocol), shown for that app
  icon-remove APP_ID                   delete it again (the category icon is used)
  face install FILE [id=ID] [origin=gallery|local]  install a clock face file (id from the file unless given)
  face select [ID]                     show an installed or built-in face (no ID: Classic); its options come back
  face opt N V                         set option N of the shown face to V
  face list | delete ID                the installed faces; remove one
  battery PCT [ext] [unreadable] [none]  battery level / external power / failing or no reader
  time [now|UTC_S] [tz=MIN]            TIME_SYNC as the app sends it (clock, RTC, time zone; default now, tz=0)
  clock 12|24                          clock format (config ui.clock_24h)
  unit c|f                             temperature unit (config ui.temp_unit)
  mask on|off                          show numbers of 4+ digits as **** (config display.mask_numbers)
  climate TEMP_C RH_PCT | fail | none  temperature/humidity sensor reading (none: no sensor after reboot)
  weather TEMP_C WMO_CODE [night] [hi=C] [lo=C] [rain=PCT] [place=NAME]   a weather reading on the idle screen
  weather off                          remove it
  weather live [CITY] [lang=vi]        fetch the real weather (CITY, else by IP) through the board's parser
  wifi                                 the network the app stored with WIFI_SET
  wifi-state STATE                     DEVICE_STATUS wifi_state (idle, connecting, connected, auth_failed, ...)
  setup waiting|joining|done|failed|off [NETWORK]   the Wi-Fi setup screen (Paper: short BOOT on the pairing screen opens it too)
  update STATE [VERSION] [size=BYTES] [skipped] [error=NAME]   the firmware update the UI shows (none, checking, current,
                                       available, downloading, failed, needs-app); update prompt opens the prompt
  storage BYTES                        asset storage size (simulate low storage)
  fail-storage N|off                   storage fails after N more writes (transfer failures)
  corrupt                              flip a byte of the incomplete transfer's data
  fw-image ok|image_invalid|signature_invalid   result of the next firmware image check
  self-check pass|fail                 self-check result of the next new image (fail = rollback)
  secure on|off                        treat the link as encrypted/bonded or not
  pairing on|off                       pairing mode
  bonded N                             the number of bonded phones (the settings list lists them)
  passkey NNNNNN|off                   show the code a pairing phone asks for
  disconnect                           drop the link (out of range)
  reboot                               reset / power cut (RAM lost, storage kept)
  button boot|pwr|up|down|both|bootpwr [long|hold]  press a button (up/down/both on the LCD; both = sleep now, both long = pairing mode; paper: pwr = settings from the clock, boot = the cards, in settings boot/pwr = up/down, boot long = select, pwr long = back, pwr hold = power off, bootpwr long = pairing mode)
  touch X Y [X2 Y2]                    LCD: tap at X,Y, or swipe to X2,Y2
  ghosting on|off                      ghosting effect in the simulator
  screen                               print the text on the screen
  snapshot FILE.png [ZOOM]             save the panel as PNG
  state                                board state
  help | quit"""


def _face_limits(board: FakeBoard) -> CF.Limits:
    return CF.PAPER if board.size[2] == 1 else CF.LCD


def _wait(board: FakeBoard, ms: int = 100) -> None:
    """Let the UI draw what a command changed (manual clock: step it; real clock: the ticker thread does it)."""
    if board.clock == "manual":
        board.advance(ms)
    else:
        time.sleep(ms / 1000)


def _face(board: FakeBoard, pos: list[str], kv: dict[str, str]) -> str:
    """face install|select|opt|list|delete. The board keeps each face's option values itself (M8b)."""
    sub = pos[0] if pos else "list"
    shown = board.clock_face_shown()
    if sub == "install":
        data = Path(pos[1]).read_bytes()
        ident = kv.get("id")
        if ident is None:
            ident = CF.parse_face(data, _face_limits(board), None).meta["id"]
        loopback.install_clock_face(board, ident, data, origin=kv.get("origin"))
        return f"face {ident} installed ({len(data)} bytes)"
    if sub == "select":
        loopback.config_set(board, {"ui.clock_face_id": pos[1] if len(pos) > 1 else ""})
        _wait(board)
        return f"face {board.clock_face_shown() or 'classic'} (errors: {board.clock_face_errors()})"
    if sub == "opt":
        if not shown:
            return "no face is shown"
        old = loopback.config_get_raw(board, "ui.clock_face_opts")
        pairs = {old[i]: old[i + 1] for i in range(0, len(old) - 1, 2)}
        pairs[int(pos[1])] = int(pos[2])
        loopback.config_set(board, {"ui.clock_face_opts": b"".join(bytes(p) for p in pairs.items()).hex()})
        _wait(board)
        return f"option {pos[1]} = {pos[2]}"
    if sub == "delete":
        loopback.delete_clock_face(board, pos[1])
        _wait(board)
        return f"face {pos[1]} deleted"
    items = loopback.asset_list(board, loopback.ASSET_TYPE_CLOCK_FACE)
    return "\n".join([f"{'*' if a['id'] == shown else ' '} {a['id']} {a['size']} bytes"
                      f"{' (' + ('gallery', 'local')[a['origin'] - 1] + ')' if a.get('origin') in (1, 2) else ''}"
                      for a in items] or ["(no faces installed)"])


def run_command(board: FakeBoard, line: str, out: Callable[[str], None] = print) -> bool:
    """Execute one console line. Returns False for quit."""
    try:
        args = shlex.split(line)
    except ValueError as e:
        out(f"? {e}")
        return True
    if not args:
        return True
    cmd, rest = args[0], args[1:]
    kv = {k: v for k, _, v in (a.partition("=") for a in rest if "=" in a)}
    pos = [a for a in rest if "=" not in a]
    try:
        if cmd in ("quit", "exit"):
            return False
        if cmd == "help":
            out(HELP)
        elif cmd == "post":
            if pos and pos[0] in PRESETS and len(pos) == 1:
                app, app_id, title, body, cat = PRESETS[pos[0]]
            else:
                app, app_id, cat = kv.get("app", "Zalo"), kv.get("id", "com.zing.zalo"), int(kv.get("cat", 4))
                title, body = (pos + ["", ""])[:2]
            board._seq = getattr(board, "_seq", 0) + 1
            key = kv.get("key", f"cli-{board._seq}")
            st, nid = board.android(key=key, app_id=app_id, app_name=app, title=title, body=body, category=cat)
            out(f"posted key={key} id={nid}" if st == 0 else f"refused: status {st}")
        elif cmd == "remove":
            out("removed" if board.android_remove(pos[0]) == 0 else "not found")
        elif cmd == "ancs":
            board._uid = getattr(board, "_uid", 100) + 1
            title, body = (pos + ["", ""])[:2]
            board.ancs(event_id=0, uid=int(kv.get("uid", board._uid)), app_id=kv.get("id", "com.apple.MobileSMS"),
                       app_name=kv.get("app", "Messages"), title=title, message=body, category=int(kv.get("cat", 4)))
        elif cmd == "icon":
            loopback.install_app_icon(board, pos[0], sample_icons.SAMPLES[pos[1] if len(pos) > 1 else "zalo"]())
            out(f"icon installed for {pos[0]}")
        elif cmd == "icon-remove":
            loopback.delete_app_icon(board, pos[0])
            out(f"icon of {pos[0]} deleted")
        elif cmd == "face":
            out(_face(board, pos, kv))
        elif cmd == "ancs-reset":
            board.ancs_reset()
        elif cmd == "battery":
            board.set_battery(int(pos[0]), external_power=1 if "ext" in pos else 0,
                              readable="unreadable" not in pos, reader="none" not in pos)
            if "none" in pos:
                out("no battery reader: takes effect for the OTA gate at the next reboot")
        elif cmd == "time":
            utc = int(time.time()) if not pos or pos[0] == "now" else int(pos[0])
            loopback.time_sync(board, utc, int(kv.get("tz", 0)))
            out(f"clock set to {utc} UTC, tz {int(kv.get('tz', 0))} min")
        elif cmd == "clock":
            loopback.config_set(board, {"ui.clock_24h": {"12": False, "24": True}[pos[0]]})
        elif cmd == "mask":
            loopback.config_set(board, {"display.mask_numbers": {"on": True, "off": False}[pos[0]]})
        elif cmd == "unit":
            loopback.config_set(board, {"ui.temp_unit": {"c": 0, "f": 1}[pos[0]]})
        elif cmd == "climate":
            if pos[0] in ("fail", "none"):
                board.set_climate(readable=False, present=pos[0] != "none")
                if pos[0] == "none":
                    out("no sensor: takes effect at the next reboot")
            else:
                board.set_climate(float(pos[0]), float(pos[1]))
        elif cmd == "weather":
            if pos[0] == "off":
                board.set_weather(None)
            elif pos[0] == "live":
                city = " ".join(pos[1:]) or None
                lat, lon, place = live_weather.locate(city, kv.get("lang", "en"))
                result = board.weather_json(live_weather.forecast(lat, lon), place or "")
                out(f"{place or '?'} ({lat:.2f}, {lon:.2f}): {result}")
            else:
                def opt(k: str) -> float | None:
                    return float(kv[k]) if k in kv else None
                rain = kv.get("rain")
                board.set_weather(float(pos[0]), int(pos[1]), "night" not in pos, opt("hi"), opt("lo"),
                                  int(rain) if rain is not None else None, kv.get("place", ""))
        elif cmd == "wifi":
            ssid, _ = board.wifi_network()
            out(f"network: {ssid!r}" if ssid else "no network stored")
        elif cmd == "wifi-state":
            board.set_wifi_state(pos[0])
        elif cmd == "setup":
            board.wifi_setup(pos[0], board.cfg.device_name, "fake-pass1", "192.168.4.1", pos[1] if len(pos) > 1 else "")
        elif cmd == "update":
            if pos[0] == "prompt":
                out("prompt opened" if board.update_prompt() else "no prompt now (needs the awake clock screen and an available update)")
            else:
                board.set_update(pos[0], kv.get("error", "none"), pos[1] if len(pos) > 1 else "", int(kv.get("size", 0)),
                                 "skipped" in pos)
        elif cmd == "storage":
            board.set_storage_total(int(pos[0]))
        elif cmd == "fail-storage":
            board.storage_budget(-1 if pos[0] == "off" else int(pos[0]))
        elif cmd == "corrupt":
            out("corrupted" if board.corrupt_transfer() else "no incomplete transfer")
        elif cmd == "fw-image":
            board.set_fw_image_status(STATUS[pos[0]])
        elif cmd == "self-check":
            board.set_self_check_fail(pos[0] == "fail")
        elif cmd == "secure":
            board.set_secure(pos[0] == "on")
        elif cmd == "bonded":
            board.set_bonded(int(pos[0]))
        elif cmd == "pairing":
            board.set_pairing(pos[0] == "on")
        elif cmd == "passkey":
            board.set_passkey(None if pos[0] == "off" else int(pos[0]))
        elif cmd == "disconnect":
            board.drop_link()
        elif cmd == "reboot":
            board.reboot()
        elif cmd == "button":
            board.button({"boot": BTN_BOOT, "pwr": BTN_PWR, "up": BTN_UP, "down": BTN_DOWN, "both": BTN_UP_DOWN, "bootpwr": BTN_BOOT_PWR}[pos[0]], "long" in pos, "hold" in pos)
        elif cmd == "touch":
            x, y = int(pos[0]), int(pos[1])
            x2, y2 = (int(pos[2]), int(pos[3])) if len(pos) > 3 else (x, y)
            for i in range(7):  # 7 polls of 20 ms: LVGL sees a drag (or a tap when it does not move)
                board.touch(True, x + (x2 - x) * i // 6, y + (y2 - y) * i // 6)
                time.sleep(0.02)
            board.touch(False)
        elif cmd == "ghosting":
            board.set_ghosting(pos[0] == "on")
        elif cmd == "screen":
            out(board.screen_text() if board.screen else "(no screen: started with --no-screen)")
        elif cmd == "snapshot":
            w, h, ch = board.size
            png.write(pos[0], board.panel(), w, h, int(pos[1]) if len(pos) > 1 else 3, ch)
            out(f"wrote {pos[0]}")
        elif cmd == "state":
            out(" ".join(f"{k}={v}" for k, v in board.state().items()))
            if board.screen:
                out(str(board.display_state()))
        else:
            out(f"? unknown command {cmd!r} (help)")
    except live_weather.LiveWeatherError as e:
        out(f"? {e}")
    except (IndexError, KeyError, ValueError, loopback.LoopbackError) as e:
        out(f"? bad arguments ({e}); see help")
    return True
