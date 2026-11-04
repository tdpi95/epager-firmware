"""Deterministic screen scenarios on the simulated LCD (296x240 RGB LCD, manual clock, fixed inputs).
Used by the golden-image tests (tests/golden/lcd/296x240/) and `fake_board snapshot --board lcd`. Each
scene returns (name, pixels) pairs: 296x240, 3 bytes (RGB) per pixel: what the UI drew (the panel RAM
at full brightness), except `dimmed`, which shows the panel as a person sees it (scaled by the backlight)."""
from __future__ import annotations

from typing import Callable

from . import loopback, sample_faces, sample_icons
from .board import BoardConfig, FakeBoard
from .native import BTN_DOWN
from .scenes import TS, VI_BODY, VI_TITLE, Frames, post

WIDTH, HEIGHT, CHANNELS = 296, 240, 3


def board() -> FakeBoard:
    b = FakeBoard(BoardConfig(screen=True, board="lcd", storage_total=8257536), clock="manual")
    b.advance(50)
    return b


def rgb(b: FakeBoard) -> bytes:
    """The panel RAM (RGB565) as 8-bit RGB, independent of the backlight."""
    f = b.frame()
    out = bytearray()
    for i in range(0, len(f), 2):
        p = f[i] | f[i + 1] << 8
        out += bytes(((p >> 11) * 255 // 31, (p >> 5 & 0x3F) * 255 // 63, (p & 0x1F) * 255 // 31))
    return bytes(out)


def settle(b: FakeBoard) -> None:
    b.advance(100)


def tap(b: FakeBoard, x: int, y: int) -> None:
    b.touch(True, x, y)
    b.advance(60)
    b.touch(False)
    b.advance(100)


def swipe(b: FakeBoard, x0: int, y0: int, x1: int, y1: int) -> None:
    for i in range(7):
        b.touch(True, x0 + (x1 - x0) * i // 6, y0 + (y1 - y0) * i // 6)
        b.advance(20)
    b.touch(False)
    b.advance(100)


def scene_idle() -> Frames:
    """The idle clock (UTC+7, a phone connected), then the same in 12-hour Vietnamese without a phone."""
    b = board()
    loopback.time_sync(b, TS + 2 * 3600 + 27 * 60, 420)  # 15:57 local
    phone = loopback.idle_link()
    b.connect(phone)
    settle(b)
    out: Frames = [("idle", rgb(b))]
    b.disconnect(phone)
    loopback.config_set(b, {"ui.clock_24h": False, "ui.language": 1})
    settle(b)
    out.append(("idle_12h_vi", rgb(b)))
    return out


def scene_card() -> Frames:
    """A Vietnamese message with the built-in category icon, then the same app with its colour icon."""
    b = board()
    b.set_utc(TS)
    post(b, 1, VI_TITLE, VI_BODY)
    settle(b)
    out: Frames = [("card_builtin_icon", rgb(b))]
    loopback.install_app_icon_colour(b, "com.zing.zalo", sample_icons.zalo_colour())
    settle(b)
    out.append(("card_app_icon", rgb(b)))
    tap(b, 148, 150)  # the whole body
    out.append(("card_expanded", rgb(b)))
    return out


def scene_long_card() -> Frames:
    """A message longer than the screen: the cut-off card, a tap shows all of it from the top, a drag scrolls."""
    b = board()
    b.set_utc(TS)
    body = " ".join(f"Dòng {i}: {VI_BODY}" for i in range(1, 6))
    post(b, 1, VI_TITLE + " và một tiêu đề dài hơn một dòng", body)
    settle(b)
    out: Frames = [("long_card_collapsed", rgb(b))]
    tap(b, 148, 150)
    out.append(("long_card_expanded", rgb(b)))
    swipe(b, 148, 200, 148, 60)
    out.append(("long_card_scrolled", rgb(b)))
    for _ in range(4):
        swipe(b, 148, 200, 148, 60)
    out.append(("long_card_end", rgb(b)))
    return out


def scene_cards() -> Frames:
    """Three cards from three categories; the second one shown (accent colours, position 2/3)."""
    b = board()
    b.set_utc(TS)
    for i, (app, title, cat) in enumerate([("Zalo", "Mẹ", 4), ("Gmail", "Lịch họp thứ Hai", 6),
                                           ("Phone", "Cuộc gọi nhỡ", 2)]):
        post(b, i, title, "…", app=app, app_id=f"app.{i}", category=cat)
    settle(b)
    b.button(BTN_DOWN)  # next (older) card
    settle(b)
    return [("cards_2_of_3", rgb(b))]


def scene_badge() -> Frames:
    """Three notifications, swipe up on the newest card: the clock screen with a badge counting them (PLAN D70)."""
    b = board()
    b.set_utc(TS)
    for i, (app, title, cat) in enumerate([("Zalo", "Mẹ", 4), ("Gmail", "Lịch họp thứ Hai", 6),
                                           ("Phone", "Cuộc gọi nhỡ", 2)]):
        post(b, i, title, "…", app=app, app_id=f"app.{i}", category=cat)
    settle(b)
    swipe(b, 148, 200, 148, 60)  # up: back to the clock screen, the notifications stay
    return [("badge", rgb(b))]


def face_board() -> FakeBoard:
    b = board()
    loopback.time_sync(b, TS + 2 * 3600 + 27 * 60, 420)  # 15:57 local
    b.set_weather(31.2, 1, True, 32.1, 24.6, 86, "Ho Chi Minh City")
    b.set_wifi_state("connected")
    b.set_battery(63)
    settle(b)
    return b


def select_face(b: FakeBoard, ident: str, opts: list[int] | None = None) -> None:
    loopback.config_set(b, {"ui.clock_face_id": ident, "ui.clock_face_opts": bytes(opts or []).hex()})
    settle(b)


def scene_face_big() -> Frames:
    """The built-in face Big: seven-segment digits as masks recoloured by an option, then another colour with the
    weather and the date off, then 12-hour in Vietnamese with Fahrenheit."""
    b = face_board()
    select_face(b, "epager.big")
    assert b.clock_face_shown() == "epager.big"
    out: Frames = [("face_big", rgb(b))]
    select_face(b, "epager.big", [3, 1, 1, 0, 2, 0])  # digits amber, weather off, date off
    out.append(("face_big_amber_bare", rgb(b)))
    loopback.config_set(b, {"ui.clock_24h": False, "ui.temp_unit": 1, "ui.language": 1})
    select_face(b, "epager.big", [3, 2])  # digits green
    out.append(("face_big_green_12h_vi_f", rgb(b)))
    return out


def scene_face_night() -> Frames:
    """The built-in face Night: the clock font, a battery ring (an arc), the weather and the room."""
    b = face_board()
    b.set_climate(23.73, 62.97)
    select_face(b, "epager.night")
    out: Frames = [("face_night", rgb(b))]
    b.set_battery(12)
    select_face(b, "epager.night", [2, 1])  # amber
    out.append(("face_night_amber_low_battery", rgb(b)))
    b.set_battery(100)
    b.set_weather(-3.4, 65, False, 1.0, -5.5, 100, "Sa Pa")
    select_face(b, "epager.night", [2, 2, 1, 0, 3, 0])  # green, weather and ring off
    out.append(("face_night_green_bare", rgb(b)))
    return out


def scene_face_firmware_parts() -> Frames:
    """The notification badge on a built-in face, the update mark, and an installed face that gets neither the badge
    nor (when it draws update_available) the mark."""
    b = face_board()
    select_face(b, "epager.big")
    for i, (app, title, cat) in enumerate([("Zalo", "Mẹ", 4), ("Gmail", "Lịch họp thứ Hai", 6)]):
        post(b, i, title, "…", app=app, app_id=f"app.{i}", category=cat)
    settle(b)
    swipe(b, 148, 200, 148, 60)  # up: the clock, the notifications stay
    out: Frames = [("face_builtin_badge", rgb(b))]
    b.set_update("available", version="0.3.0", size=2800000)
    settle(b)
    out.append(("face_builtin_badge_update_mark", rgb(b)))
    loopback.install_clock_face(b, "sample.plain", sample_faces.big_variant("lcd", "sample.plain"))
    select_face(b, "sample.plain")
    out.append(("face_installed_no_badge_update_mark", rgb(b)))
    loopback.install_clock_face(b, "sample.count", sample_faces.big_variant("lcd", "sample.count", count=True))
    select_face(b, "sample.count")
    out.append(("face_installed_with_count", rgb(b)))
    loopback.install_clock_face(b, "sample.update", sample_faces.big_variant("lcd", "sample.update", draws_update=True))
    select_face(b, "sample.update")
    out.append(("face_installed_draws_update", rgb(b)))
    return out


def scene_face_fallback() -> Frames:
    """Classic for a face that is missing, damaged, or made for the other board's screen."""
    b = face_board()
    select_face(b, "no.such.face")
    out: Frames = [("face_missing_classic", rgb(b))]
    assert (b.clock_face_shown(), b.clock_face_errors()) == ("", 1)
    loopback.install_clock_face(b, "bad.file", sample_faces.broken("lcd"))
    select_face(b, "bad.file")
    out.append(("face_damaged_classic", rgb(b)))
    loopback.install_clock_face(b, "paper.big", sample_faces.big_variant("paper", "paper.big"))
    select_face(b, "paper.big")  # made for 200x200
    out.append(("face_wrong_screen_classic", rgb(b)))
    assert (b.clock_face_shown(), b.clock_face_errors()) == ("", 3)
    return out


def scene_emoji() -> Frames:
    b = board()
    b.set_utc(TS)
    post(b, 1, "Party 🎉🎉 tonight", "👨‍👩‍👧 Gia đình 🇻🇳 đã sẵn sàng ✅ — hẹn 8h 😀❤️🔥👍", app="Messenger",
         app_id="com.facebook.orca")
    settle(b)
    return [("emoji", rgb(b))]


def scene_status() -> Frames:
    """Pairing mode with a low battery on USB, then the screen dimmed after 30 s idle."""
    b = board()
    b.set_pairing(True)
    b.set_battery(17, external_power=1)
    settle(b)
    out: Frames = [("pairing_low_battery", rgb(b))]
    b.set_passkey(4821)  # a phone is pairing: the code replaces the hint
    settle(b)
    out.append(("pairing_passkey", rgb(b)))
    b.set_passkey(None)
    settle(b)
    b.advance(31000)
    out.append(("dimmed", b.panel()))  # as seen: 20 % backlight
    return out


def scene_weather() -> Frames:
    """The idle clock with the weather row and the place (recorded Open-Meteo values for Ho Chi Minh City),
    then deg F in 12-hour time at night with rain and a long Vietnamese name, then a reading with only the
    temperature, an unknown condition and no place. The status bar's Wi-Fi icon: the last join worked,
    joining (muted), the last join failed (struck through)."""
    b = board()
    loopback.time_sync(b, TS + 2 * 3600 + 27 * 60, 420)
    b.set_weather(31.2, 1, True, 32.1, 24.6, 86, "Ho Chi Minh City")
    b.set_wifi_state("idle")
    settle(b)
    out: Frames = [("idle_weather", rgb(b))]
    loopback.config_set(b, {"ui.clock_24h": False, "ui.temp_unit": 1})
    b.set_weather(-3.4, 65, False, 1.0, -5.5, 100, "Thị trấn Sa Pa, Lào Cai, Việt Nam, Tây Bắc Bộ")
    b.set_wifi_state("connecting")
    settle(b)
    out.append(("idle_weather_f_night", rgb(b)))
    b.set_weather(18.0, 4)
    b.set_wifi_state("no_internet")
    settle(b)
    out.append(("idle_weather_minimal", rgb(b)))
    return out


def scene_wifi_setup() -> Frames:
    """Pairing screen with "Wi-Fi setup", its hotspot screen (QR code), then joining and a failure."""
    b = board()
    b.set_pairing(True)
    settle(b)
    out: Frames = [("pairing_wifi", rgb(b))]
    tap(b, WIDTH // 2, 194)  # the "Wi-Fi setup" button
    out.append(("wifi_setup", rgb(b)))
    b.wifi_setup("joining", "ePager-FA4E", "fake-pass1", "192.168.4.1", "Quán Cafe 5G")
    settle(b)
    out.append(("wifi_setup_joining", rgb(b)))
    b.wifi_setup("failed", "ePager-FA4E", "fake-pass1", "192.168.4.1", "Quán Cafe 5G")
    settle(b)
    out.append(("wifi_setup_failed", rgb(b)))
    return out


def scene_update() -> Frames:
    """Firmware update (docs/wifi-ota-plan.md): the clock screen's mark, the prompt, the page in a few states."""
    b = board()
    loopback.time_sync(b, TS + 2 * 3600 + 27 * 60, 420)
    loopback.wifi_set(b, "Home", "secret-pass")
    b.advance(3500)
    settle(b)
    b.set_update("available", version="0.2.0", size=2822144)
    settle(b)
    out: Frames = [("update_mark", rgb(b))]
    assert b.update_prompt()
    settle(b)
    out.append(("update_prompt", rgb(b)))
    swipe(b, 60, 120, 220, 120)  # back: remind later, the clock again
    for _ in range(4):  # open the settings, then the pages up to Software update
        swipe(b, 148, 200, 148, 60)
    tap(b, 148, 76)
    out.append(("update_page", rgb(b)))
    b.set_update("failed", error="net", version="0.2.0")
    settle(b)
    out.append(("update_failed", rgb(b)))
    b.set_update("downloading", version="0.2.0", size=2822144)
    settle(b)
    out.append(("update_downloading", rgb(b)))
    return out


def scene_settings() -> Frames:
    """The settings (swipe up on the clock screen, PLAN D99): the first page, in Vietnamese, the second page with a
    value changed by a tap, then the pages of the paired phones and of the saved Wi-Fi networks and About."""
    b = board()
    loopback.time_sync(b, TS + 2 * 3600 + 27 * 60, 420)
    b.set_bonded(2)
    for ssid in ("Home", "Office"):
        loopback.wifi_set(b, ssid, "secret-pass")
    b.advance(3500)
    settle(b)
    swipe(b, 148, 200, 148, 60)
    out: Frames = [("settings", rgb(b))]
    loopback.config_set(b, {"ui.language": 1})
    settle(b)
    out.append(("settings_vi", rgb(b)))
    loopback.config_set(b, {"ui.language": 0})
    settle(b)
    swipe(b, 148, 200, 148, 60)  # the second page
    tap(b, 148, 156)  # Weather: on
    out.append(("settings_page2", rgb(b)))
    swipe(b, 148, 200, 148, 60)  # the third page
    tap(b, 148, 116)  # Paired phones
    tap(b, 148, 76)  # asks before forgetting
    out.append(("settings_phones", rgb(b)))
    swipe(b, 60, 120, 220, 120)  # back to the list
    tap(b, 148, 156)  # Wi-Fi networks
    out.append(("settings_networks", rgb(b)))
    swipe(b, 60, 120, 220, 120)
    tap(b, 148, 196)  # About
    out.append(("settings_about", rgb(b)))
    return out


SCENES: dict[str, Callable[[], Frames]] = {
    "idle": scene_idle,
    "card": scene_card,
    "cards": scene_cards,
    "long_card": scene_long_card,
    "badge": scene_badge,
    "emoji": scene_emoji,
    "status": scene_status,
    "weather": scene_weather,
    "face_big": scene_face_big,
    "face_night": scene_face_night,
    "face_firmware_parts": scene_face_firmware_parts,
    "face_fallback": scene_face_fallback,
    "wifi_setup": scene_wifi_setup,
    "settings": scene_settings,
    "update": scene_update,
}
