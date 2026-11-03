"""The LCD fake board as the app sees it (P1b): Capabilities, dismissing a card on the board
(swipe down -> NOTIF_DISMISSED with the Android key, swipe up hides the card), brightness from a long key press (CONFIG_CHANGED),
the backlight keys, and the touch canvas of the simulator. Contract 0.8.0. Wi-Fi and weather (contract
0.9.0): WIFI_SET and DEVICE_STATUS, the weather row from the console and through the board's parser, the
pairing screen's "Wi-Fi setup" button."""
from __future__ import annotations

from fake_board import loopback, scenes_lcd
from fake_board.console import run_command
from fake_board.native import BTN_DOWN, BTN_UP, BTN_UP_DOWN, EV_ACTION
from fake_board.scenes import post

CAPABILITIES = 1


def _caps(b) -> dict:
    link = loopback.idle_link()
    assert b.connect(link)
    raw = b.read(link, CAPABILITIES)
    b.disconnect(link)
    return loopback.codec.decode_message(loopback.REG, raw)["fields"]


def test_capabilities(fb_libs) -> None:
    caps = _caps(scenes_lcd.board())
    assert caps["variant"] == 2  # lcd
    assert caps["display"] == {"type": 2, "width": 296, "height": 240, "color_depth": 16, "refresh_class": 2}
    assert caps["features"] & (1 << 9)  # touch
    assert caps["features"] & (1 << 12) and caps["features"] & (1 << 13)  # weather, wifi
    assert {"asset_type": 5, "format": 6, "max_size": 3088, "width": 32, "height": 32} in caps["asset_support"]
    assert {"asset_type": 2, "format": 2, "max_size": 142096} in caps["asset_support"]


def test_swipe_down_dismisses_and_tells_the_app(fb_libs) -> None:
    b = scenes_lcd.board()
    post(b, 7, "Hello", "Swipe me away")
    link = loopback.idle_link()
    assert b.connect(link)
    for i in range(7):  # finger from the top to the bottom
        b.touch(True, 148, 60 + i * 25)
        b.advance(20)
    b.touch(False)
    b.advance(100)
    dismissed = [m for _c, m in link.messages if m["type"] == "NOTIF_DISMISSED"]
    assert dismissed == [{"ver": 0, "type": "NOTIF_DISMISSED", "req_id": 0,
                          "fields": {"notif_id": 1, "source": 2, "key": "k7"}}]
    assert [c for c, m in link.messages if m["type"] == "NOTIF_DISMISSED"] == [3]  # Notification characteristic
    assert "Hello" not in b.screen_text()
    assert b.state()["notifications"] == 0


def test_swipe_up_hides_the_card_without_dismissing_it(fb_libs) -> None:
    b = scenes_lcd.board()
    post(b, 7, "Hello", "Keep me")
    link = loopback.idle_link()
    assert b.connect(link)
    scenes_lcd.swipe(b, 148, 200, 148, 60)  # up: the clock screen with a badge
    assert "Hello" not in b.screen_text()
    assert b.state()["notifications"] == 1
    assert not [m for _c, m in link.messages if m["type"] == "NOTIF_DISMISSED"]  # the phone is not told
    scenes_lcd.swipe(b, 148, 60, 148, 200)  # down: the card again
    assert "Hello" in b.screen_text()
    assert b.state()["notifications"] == 1


def test_long_key_press_changes_brightness_through_config(fb_libs) -> None:
    b = scenes_lcd.board()
    link = loopback.idle_link()
    assert b.connect(link)
    b.advance(50)
    assert b.display_state().backlight == 70
    b.button(BTN_UP, long_press=True)
    b.advance(50)
    changed = [m for _c, m in link.messages if m["type"] == "CONFIG_CHANGED"]
    assert changed and changed[-1]["fields"]["key"] == [7]  # display.brightness
    assert b.display_state().backlight == 80


def test_idle_dims_then_switches_off(fb_libs) -> None:
    b = scenes_lcd.board()
    loopback.config_set(b, {"display.idle_dim_s": 5, "display.idle_off_s": 10})
    b.advance(6000)
    assert b.display_state().backlight == 20
    b.advance(5000)
    d = b.display_state()
    assert d.backlight == 0 and d.asleep
    b.touch(True, 100, 100)  # a touch wakes it (and does nothing else)
    b.advance(40)
    b.touch(False)
    b.advance(40)
    d = b.display_state()
    assert d.backlight == 70 and not d.asleep and d.violations == 0


def _status(b, link) -> dict:
    return loopback.codec.decode_message(loopback.REG, b.read(link, 5))["fields"]


def test_wifi_set_and_device_status(fb_libs) -> None:
    b = scenes_lcd.board()
    link = loopback.idle_link()
    assert b.connect(link)
    st = _status(b, link)
    assert (st["wifi_state"], st["weather_state"]) == (0, 1)  # unconfigured, waiting
    assert "wifi_ssid" not in st
    b.disconnect(link)

    loopback.wifi_set(b, "Quán Cafe 5G", "matkhau123")
    assert b.wifi_network() == ("Quán Cafe 5G", "matkhau123")
    loopback.wifi_set(b, "Home", "short", want="OUT_OF_RANGE")
    loopback.wifi_set(b, password="12345678", want="MISSING_FIELD")
    assert b.wifi_network()[0] == "Quán Cafe 5G"
    link = loopback.idle_link()
    assert b.connect(link)
    st = _status(b, link)
    assert (st["wifi_state"], st["wifi_ssid"]) == (1, "Quán Cafe 5G")  # idle: stored, radio off
    assert "matkhau123".encode() not in b.read(link, 5)
    b.set_wifi_state("connected")
    events = [m["fields"] for _c, m in link.messages if m["type"] == "DEVICE_STATUS"]
    assert events and events[-1]["wifi_state"] == 3
    b.disconnect(link)
    loopback.wifi_set(b, forget=True)
    assert b.wifi_network() == ("", "")
    assert b.state()["wifi_network"] is None


def test_weather_row_from_console_and_parser(fb_libs) -> None:
    b = scenes_lcd.board()
    out: list[str] = []
    run_command(b, "time 1791700215 tz=420", out.append)
    run_command(b, "weather 31.2 1 hi=32.1 lo=24.6 rain=86", out.append)
    b.advance(50)
    assert "31\u00b0\n32\u00b0 / 25\u00b0   \u261486%" in b.screen_text()
    doc = (b'{"utc_offset_seconds":25200,"current":{"temperature_2m":-3.4,"weather_code":65,"is_day":0},'
           b'"daily":{"temperature_2m_max":[1.0],"temperature_2m_min":[-5.5],"precipitation_probability_max":[100]}}')
    assert b.weather_json(doc) == "ok"
    b.advance(50)
    assert "-3\u00b0\n1\u00b0 / -6\u00b0" in b.screen_text()
    assert b.weather_json(b"<html>busy</html>") == "format"
    assert b.weather_json(b'{"error":true,"reason":"x"}') == "api"
    run_command(b, "weather off", out.append)
    b.advance(50)
    assert "\u00b0" not in b.screen_text()
    assert not [line for line in out if line.startswith("?")]


def test_wifi_setup_from_the_pairing_screen(fb_libs) -> None:
    b = scenes_lcd.board()
    b.set_pairing(True)
    b.advance(100)
    assert "Wi-Fi setup" in b.screen_text()
    scenes_lcd.tap(b, 148, 194)
    assert [e.a for e in b.events if e.type == EV_ACTION][-1] == 6  # UI_ACTION_WIFI_SETUP
    assert "Join Wi-Fi\nePager-FA4E\nPassword\n" in b.screen_text()
    b.button(BTN_DOWN)  # any key closes it
    b.advance(100)
    assert [e.a for e in b.events if e.type == EV_ACTION][-1] == 7
    assert "Join Wi-Fi" not in b.screen_text()
    assert "Wi-Fi setup" in b.screen_text()  # back on the pairing screen


def test_settings_page_and_sleep_now(fb_libs) -> None:
    b = scenes_lcd.board()
    b.advance(100)
    scenes_lcd.swipe(b, 148, 200, 148, 60)  # swipe up on the clock screen: the settings (PLAN D99)
    assert "Settings\n1/4\nClock\n24h\nTemperature\n" in b.screen_text()
    scenes_lcd.tap(b, 148, 116)  # Temperature: Celsius -> Fahrenheit, stored like a CONFIG_SET
    assert "Temperature\n\u00b0F\n" in b.screen_text()
    scenes_lcd.swipe(b, 148, 200, 148, 60)  # the second page
    scenes_lcd.swipe(b, 148, 200, 148, 60)  # the third page (the second ends with the firmware update's check switch)
    scenes_lcd.tap(b, 148, 76)  # Pairing mode: the first row of the third page
    assert [e.a for e in b.events if e.type == EV_ACTION][-1] == 1  # UI_ACTION_PAIRING_TOGGLE
    assert "Settings" not in b.screen_text()
    assert "Open the ePager app" in b.screen_text()  # the firmware turned pairing mode on
    scenes_lcd.swipe(b, 148, 60, 148, 200)  # swipe down: leave pairing mode
    assert "Open the ePager app" not in b.screen_text()
    # both keys together: the screen goes off at once; any key wakes it
    b.button(BTN_UP_DOWN)
    b.advance(100)
    assert b.display_state().asleep and b.display_state().backlight == 0
    b.button(BTN_DOWN)
    b.advance(100)
    assert not b.display_state().asleep and b.display_state().backlight > 0
