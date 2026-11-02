"""Clock, RTC, climate sensor and the status bar's connection icon on the fake board: TIME_SYNC reaches the simulated RTC, the time survives a
reboot but not a power cut, Capabilities advertise the hardware, the console commands. Screen layouts are
checked by the golden images (scene `clock`) and tests/host/test_ui.c."""
from __future__ import annotations

from epager_proto import codec
from epager_proto import registry as R
from fake_board import console, loopback, scenes
from fake_board.board import BoardConfig, FakeBoard

REG = R.load()
T = scenes.TS + 2 * 3600 + 27 * 60  # 08:57:15 UTC
FEATURE = {b["name"]: 1 << b["bit"] for b in REG.bitmasks["features"]}


class _Link:
    def notify(self, char: int, value: bytes) -> None:
        pass

    def drop(self) -> None:
        pass


def _features(b: FakeBoard) -> int:
    link = _Link()
    assert b.connect(link)
    try:
        return codec.decode_message(REG, b.read(link, 1))["fields"]["features"]
    finally:
        b.disconnect(link)


def test_time_sync_writes_the_rtc_and_survives_a_reboot(fb_libs) -> None:
    b = scenes.board()
    assert b.state()["utc"] == 0 and b.state()["rtc"] == 0
    assert "Time not set" in b.screen_text()
    loopback.time_sync(b, T, 420)
    scenes.settle(b)
    assert b.state()["rtc"] == T
    assert "15:57\nSunday, 11 Oct\n" in b.screen_text()
    b.advance(61_000)
    before = b.state()["utc"]  # settle() spent a few seconds on refreshes too
    b.reboot()  # RAM lost: the clock comes back from the RTC
    assert b.state()["utc"] == before
    scenes.settle(b)
    assert "15:58\n" in b.screen_text()


def test_power_loss_clears_the_rtc(fb_libs) -> None:
    b = scenes.board()
    loopback.time_sync(b, T, 0)
    b.set_rtc(True, 0)  # what the PCF85063 reports after losing power (OS flag set)
    b.reboot()
    scenes.settle(b)
    assert b.state()["utc"] == 0
    assert "--:--\nTime not set\n" in b.screen_text()


def test_capabilities_follow_the_hardware(fb_libs) -> None:
    b = scenes.board()
    f = _features(b)
    assert f & FEATURE["rtc"] and f & FEATURE["climate"]
    assert "24.5°C\n50%" in b.screen_text()
    b.set_rtc(False)
    b.set_climate(present=False)
    b.reboot()
    scenes.settle(b)
    f = _features(b)
    assert not f & FEATURE["rtc"] and not f & FEATURE["climate"]
    assert "°C" not in b.screen_text()  # no sensor: no climate line
    loopback.time_sync(b, T, 0)
    b.reboot()
    assert b.state()["utc"] == 0  # no RTC: the time is lost at every reset


def test_climate_reading_failure_shows_dashes(fb_libs) -> None:
    b = scenes.board()
    b.set_climate(readable=False)
    scenes.settle(b)
    assert "--.-°C\n--%" in b.screen_text()
    b.set_climate(-3.25, 41.6)
    scenes.settle(b)
    assert "-3.3°C\n42%" in b.screen_text()


def test_console_clock_commands(fb_libs) -> None:
    b = scenes.board()
    lines: list[str] = []
    for cmd in (f"time {T} tz=420", "clock 12", "unit f", "climate 30 70.4"):
        console.run_command(b, cmd, lines.append)
    scenes.settle(b)
    assert "3:57\nPM\n" in b.screen_text()
    assert "86.0°F\n70%" in b.screen_text()
    assert b.n.config_int(48) == 420 and b.n.config_int(5) == 0 and b.n.config_int(6) == 1
    console.run_command(b, "climate fail", lines.append)
    console.run_command(b, "time now", lines.append)
    scenes.settle(b)
    assert "--.-°F\n--%" in b.screen_text()
    assert b.state()["utc"] > T
    assert not any(line.startswith("?") for line in lines), lines


def test_connection_icon_follows_the_link(fb_libs) -> None:
    b = scenes.board()
    assert b.display_state().link == "disconnected"
    link = _Link()
    assert b.connect(link)
    scenes.settle(b)
    assert b.display_state().link == "connected"
    b.disconnect(link)
    scenes.settle(b)
    assert b.display_state().link == "disconnected"


def test_clock_needs_no_screen(fb_libs) -> None:
    """The core-only library (no simulated board) accepts the calls and has no RTC."""
    b = FakeBoard(BoardConfig(screen=False), clock="manual")
    b.set_rtc(True, T)
    b.set_climate(20.0, 40.0)
    loopback.time_sync(b, T, 0)
    assert b.state()["rtc"] == 0 and b.state()["utc"] == T


def test_phone_settings_show_a_message(fb_libs) -> None:
    """A CONFIG_SET or WIFI_SET from the phone puts a message over the footer for three seconds (PLAN D79);
    the board's own writes, refused requests and unchanged values do not."""
    b = scenes.board()
    scenes.settle(b)
    assert "updated" not in b.screen_text()
    loopback.config_set(b, {"ui.clock_24h": True})  # the default: nothing changed
    b.advance(500)
    assert "updated" not in b.screen_text()
    loopback.config_set(b, {"ui.clock_24h": False})
    b.advance(500)
    assert "Setting updated" in b.screen_text()
    b.advance(2000)
    assert "Setting updated" in b.screen_text()
    b.advance(1000)
    assert "Setting updated" not in b.screen_text()
    loopback.wifi_set(b, "Home", "short", want="OUT_OF_RANGE")  # refused: nothing stored, nothing said
    b.advance(500)
    assert "Wi-Fi saved" not in b.screen_text()
    loopback.wifi_set(b, "Home", "matkhau123")
    b.advance(500)
    assert "Wi-Fi saved" in b.screen_text()
