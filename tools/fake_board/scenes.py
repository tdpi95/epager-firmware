"""Deterministic screen scenarios on the simulated Paper (manual clock, fixed inputs). Used by the
golden-image tests (tools/tests/test_golden.py, images in tests/golden/paper/200x200/) and `fake_board snapshot`.
Each scene returns (name, panel pixels) pairs: 200x200, one byte per pixel, 0 black .. 255 white."""
from __future__ import annotations

from typing import Callable

from . import loopback, sample_icons
from .board import BoardConfig, FakeBoard
from .native import BTN_BOOT, BTN_PWR, HEIGHT, WIDTH

TS = 1791700215  # 2026-10-11 06:30:15 UTC
Frames = list[tuple[str, bytes]]

VI_TITLE = "Tin nhắn mới từ Minh"
VI_BODY = "Chào bạn! Tối nay đi ăn phở không? Mình đặt bàn lúc 7 giờ ở quán Phở Thìn nhé. Nhớ mang theo ô."


def board() -> FakeBoard:
    b = FakeBoard(BoardConfig(screen=True), clock="manual")
    settle(b)
    return b


def settle(b: FakeBoard, limit_ms: int = 10000) -> None:
    """Advance until the panel is idle and nothing is pending."""
    b.advance(50)
    t = 0
    while b.display_state().refreshing and t < limit_ms:
        b.advance(50)
        t += 50
    b.advance(50)


def set_config(b: FakeBoard, values: dict[str, object]) -> None:
    """CONFIG_SET, then let the "Setting updated" message (D79) go before the frame that is looked at."""
    loopback.config_set(b, values)
    b.advance(3200)
    settle(b)


def post(b: FakeBoard, n: int, title: str, body: str = "", app: str = "Zalo", app_id: str = "com.zing.zalo",
         category: int = 4, dwell: bool = True) -> None:
    st, _ = b.android(key=f"k{n}", app_id=app_id, app_name=app, title=title, body=body, category=category)
    assert st == 0, st
    if dwell:
        b.advance(2200)  # a card stays on the panel for two seconds before a newer one replaces it (D81)


def scene_idle() -> Frames:
    return [("idle", board().panel())]


def scene_clock() -> Frames:
    """The idle screen once the app has sent the time (UTC+7): 24-hour clock in Celsius with a phone
    connected, then 12-hour in Fahrenheit with the Vietnamese date and no phone."""
    b = board()
    loopback.time_sync(b, TS + 2 * 3600 + 27 * 60, 420)  # 15:57 local
    b.set_climate(23.73, 62.97)
    phone = loopback.idle_link()
    b.connect(phone)  # the status bar shows the Bluetooth rune while a phone is connected
    settle(b)
    out: Frames = [("clock_24h", b.panel())]
    b.disconnect(phone)
    set_config(b, {"ui.clock_24h": False, "ui.temp_unit": 1, "ui.language": 1})
    out.append(("clock_12h_vi", b.panel()))
    return out


def scene_vietnamese() -> Frames:
    b = board()
    b.set_utc(TS)  # the card shows the notification time (UTC: no TIME_SYNC offset)
    post(b, 1, VI_TITLE, VI_BODY)
    settle(b)
    return [("vietnamese", b.panel())]


def scene_emoji() -> Frames:
    """Emoji are drawn as 1-bit glyphs (the ZWJ family shows its first emoji, the flag the flag glyph); text
    the font does not cover (here CJK) shows one placeholder box per run."""
    b = board()
    b.set_utc(TS)
    post(b, 1, "Party 🎉🎉 tonight", "👨‍👩‍👧 Gia đình 🇻🇳 đã sẵn sàng ✅ — hẹn 8h 😀❤️🔥👍", app="Messenger",
         app_id="com.facebook.orca")
    settle(b)
    out = [("emoji", b.panel())]
    post(b, 2, "你好 Zalo", "Xin chào 你好 bạn", app="Zalo", app_id="com.zing.zalo")
    settle(b)
    out.append(("placeholder", b.panel()))
    return out


def scene_truncation() -> Frames:
    b = board()
    title = "Your order #4471 has shipped and is on its way to the delivery address you gave us"
    body = ("Hello! Package 1 of 3 left our warehouse this morning. Expected delivery: Tuesday between 9 and 12. "
            "Track it in the app or reply STOP to unsubscribe from these messages. ") * 3
    post(b, 1, title, body, app="Shopee Express Delivery Notifications", app_id="vn.shopee.express", category=9)
    settle(b)
    return [("truncation", b.panel())]


def scene_badge() -> Frames:
    """Past the newest card the clock comes back with a badge for the notifications still waiting (D74, D75)."""
    b = board()
    loopback.time_sync(b, TS + 2 * 3600 + 27 * 60, 420)
    b.set_climate(23.73, 62.97)
    post(b, 1, "Mẹ", "…", app_id="app.1")
    post(b, 2, "Lịch họp thứ Hai", "…", app="Gmail", app_id="app.2")
    settle(b)
    b.button(BTN_PWR)  # newer than the newest card: the clock
    settle(b)
    return [("clock_badge", b.panel())]


def scene_settings() -> Frames:
    """The Paper's settings (PLAN D96-D98): short PWR on the clock opens the list, a choice changes in place, the
    list scrolls to About; the paired phones, the saved Wi-Fi networks and About pages."""
    b = board()
    loopback.time_sync(b, TS + 2 * 3600 + 27 * 60, 420)
    b.set_bonded(2)
    for ssid in ("Home", "Office"):
        loopback.wifi_set(b, ssid, "secret-pass")
    b.advance(3500)  # the "Wi-Fi saved" message goes
    settle(b)
    b.button(BTN_PWR)  # the clock: open the settings
    settle(b)
    out: Frames = [("settings_top", b.panel())]
    b.button(BTN_PWR)  # down to Temperature
    b.button(BTN_BOOT, long_press=True)  # select: Celsius -> Fahrenheit
    settle(b)
    out.append(("settings_fahrenheit", b.panel()))
    b.button(BTN_BOOT)  # up
    b.button(BTN_BOOT)  # up from the first row wraps to the last, the list scrolls
    settle(b)
    out.append(("settings_scrolled", b.panel()))
    b.button(BTN_BOOT, long_press=True)  # About
    settle(b)
    out.append(("settings_about", b.panel()))
    b.button(BTN_PWR, long_press=True)  # back, on the About row
    b.button(BTN_BOOT)  # up to Wi-Fi networks
    b.button(BTN_BOOT, long_press=True)
    settle(b)
    out.append(("settings_networks", b.panel()))
    b.button(BTN_BOOT, long_press=True)  # asks before forgetting
    settle(b)
    out.append(("settings_networks_forget", b.panel()))
    b.button(BTN_PWR, long_press=True)  # back
    b.button(BTN_BOOT)  # up to Paired phones
    b.button(BTN_BOOT, long_press=True)
    settle(b)
    out.append(("settings_phones", b.panel()))
    return out


def scene_rotation() -> Frames:
    """display.orientation from the phone: the same card turned by 90, 180 and 270 degrees (PLAN D77)."""
    b = board()
    post(b, 1, "Lịch họp thứ Hai", "Phòng 3, 9 giờ", app="Gmail", app_id="app.1")
    settle(b)
    out: Frames = []
    for turns in (1, 2, 3):
        set_config(b, {"display.orientation": turns})
        out.append((f"rotation_{turns * 90}", b.panel()))
    return out


def scene_weather() -> Frames:
    """The idle screen with the weather (PLAN D78): clear day with the chance of rain, a night, and a reading
    without high / low or rain; in Vietnamese with the 12-hour clock and Fahrenheit for the last one."""
    b = board()
    loopback.time_sync(b, TS + 2 * 3600 + 27 * 60, 420)
    b.set_climate(23.73, 62.97)
    b.set_weather(31.2, 1, True, 32.1, 24.6, 86, "Hà Nội")  # mainly clear
    b.set_wifi_state("connected")  # the fan in the status line
    settle(b)
    out: Frames = [("weather_day", b.panel())]
    b.set_weather(26.0, 95, False, 28.0, 22.0, 70, "Hà Nội")  # thunderstorm at night
    b.set_wifi_state("idle")  # the radio is off again: no fan
    settle(b)
    out.append(("weather_thunder", b.panel()))
    b.set_weather(18.0, 3, False)  # overcast, nothing but the icon
    set_config(b, {"ui.clock_24h": False, "ui.temp_unit": 1, "ui.language": 1})
    out.append(("weather_icon_only_12h_vi", b.panel()))
    return out


def scene_wifi_setup() -> Frames:
    """The pairing screen offering the Wi-Fi setup, then the hotspot's QR code and the outcomes (PLAN D78)."""
    b = board()
    b.set_pairing(True)
    settle(b)
    out: Frames = [("pairing", b.panel())]
    b.button(BTN_BOOT)  # short BOOT on the pairing screen: Wi-Fi setup
    settle(b)
    out.append(("setup_waiting", b.panel()))
    b.wifi_setup("joining", "ePager-1A2B", "fake-pass1", "192.168.4.1", "Quán cà phê")
    settle(b)
    out.append(("setup_joining", b.panel()))
    b.wifi_setup("failed", "ePager-1A2B", "fake-pass1", "192.168.4.1", "Quán cà phê")
    settle(b)
    out.append(("setup_failed", b.panel()))
    return out


def scene_notice() -> Frames:
    """A setting from the phone: the message over the footer for three seconds, then the footer again (D79)."""
    b = board()
    loopback.time_sync(b, TS + 2 * 3600 + 27 * 60, 420)
    b.set_climate(23.73, 62.97)
    settle(b)
    loopback.config_set(b, {"ui.temp_unit": 1})
    settle(b)
    out: Frames = [("notice_setting", b.panel())]
    loopback.wifi_set(b, "Home", "matkhau123")
    settle(b)
    out.append(("notice_wifi", b.panel()))
    b.advance(3200)
    settle(b)
    out.append(("notice_gone", b.panel()))
    return out


def scene_cards() -> Frames:
    b = board()
    for i, (app, title) in enumerate([("Zalo", "Mẹ"), ("Gmail", "Lịch họp thứ Hai"), ("Slack", "Deploy xong")]):
        post(b, i, title, "…", app=app, app_id=f"app.{i}")
    settle(b)
    b.button(BTN_BOOT)  # next (older) card
    settle(b)
    return [("cards_2_of_3", b.panel())]


def scene_icons() -> Frames:
    """The card icon: the built-in one of the category, the app's own once uploaded, then another app
    (a call) back on a built-in icon."""
    b = board()
    b.set_utc(TS)
    post(b, 1, "Mẹ", "Tối nay về ăn cơm nhé con.")
    settle(b)
    out: Frames = [("icon_builtin", b.panel())]
    loopback.install_app_icon(b, "com.zing.zalo", sample_icons.zalo())
    settle(b)
    out.append(("icon_app", b.panel()))
    post(b, 2, "Mẹ đang gọi", app="Phone", app_id="com.android.dialer", category=1)
    settle(b)
    out.append(("icon_other_app_builtin", b.panel()))
    return out


def scene_refresh_sequence() -> Frames:
    """A partial refresh mid-way and done, then a full refresh through its four flash phases."""
    b = board()
    b.set_ghosting(True)
    out: Frames = []
    post(b, 1, "Partial", "Changed pixels are grey while the panel updates.", dwell=False)
    b.advance(150)
    out.append(("refresh_partial_mid", b.panel()))
    settle(b)
    out.append(("refresh_partial_done_ghosting", b.panel()))
    # a card counts a quarter toward display.full_refresh_every and a full refresh waits while cards are shown
    # (D83): with 1, the sixth card after the setting is the full one (the message's two frames count too)
    set_config(b, {"display.full_refresh_every": 1})
    for i in range(5):
        post(b, 1, f"Update {i}", dwell=False)
        settle(b)
        b.advance(2200)  # the shown card's minimum time (D81), so the next one opens at once
    post(b, 2, "After a full refresh", "Ghosting is cleared.", dwell=False)
    b.advance(20)
    assert b.display_state().kind == "full" and b.display_state().refreshing, b.display_state()
    for phase in range(4):
        b.advance(500 if phase else 250)
        out.append((f"refresh_full_phase{phase}", b.panel()))
    settle(b)
    out.append(("refresh_full_done", b.panel()))
    assert b.display_state().violations == 0
    return out


def scene_status() -> Frames:
    b = board()
    b.set_pairing(True)
    b.set_battery(17, external_power=1)
    settle(b)
    out = [("pairing_low_battery", b.panel())]
    b.set_passkey(123456)  # a phone is pairing: its code takes the card's place
    settle(b)
    out.append(("pairing_passkey", b.panel()))
    b.set_passkey(None)
    b.set_pairing(False)
    b.set_battery(86, external_power=1)  # charging: the bolt left of the level (D85)
    settle(b)
    out.append(("idle_charging", b.panel()))
    b.button(1, hold=True)  # PWR held 5 s: power off (D96)
    settle(b)
    out.append(("power_off", b.panel()))
    return out


SCENES: dict[str, Callable[[], Frames]] = {
    "idle": scene_idle,
    "clock": scene_clock,
    "vietnamese": scene_vietnamese,
    "emoji": scene_emoji,
    "truncation": scene_truncation,
    "cards": scene_cards,
    "badge": scene_badge,
    "settings": scene_settings,
    "weather": scene_weather,
    "notice": scene_notice,
    "wifi_setup": scene_wifi_setup,
    "rotation": scene_rotation,
    "icons": scene_icons,
    "refresh_sequence": scene_refresh_sequence,
    "status": scene_status,
}

SIZE = (WIDTH, HEIGHT)
