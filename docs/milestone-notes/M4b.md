# M4b: Clock, temperature and humidity (2026-10-12)

User request (2026-10-11): show the clock, temperature and humidity on the Paper. Decisions taken with
the user before starting: idle screen with a large clock, the date, temperature and humidity, plus the current
time on a card's footer; 12/24 h and C/F as config keys (contract 0.6.0); readings shown on the board only
(not sent to the app); done now as M4b, before M5. PLAN D43-D45.

## Done

- **Contract 0.6.0** (additive, commit `4856e02`): config keys `ui.clock_24h` (5, bool, default true) and
  `ui.temp_unit` (6, enum `temp_unit`: celsius/fahrenheit, default celsius), feature bit 11 `climate`.
  protocol.md §4 (meaning of `rtc` and `climate`) and §5, changelog, compatibility table. Vectors:
  `config_values.json` (defaults, valid and out-of-range cases for keys 5 and 6, lcd),
  `messages.json` (`capabilities_paper` advertises `rtc` and `climate`); the other vector files only
  carry the new version.
- **board_api / board_util.h**: `board_climate_t` and the optional `read_climate` hook; header-only
  helpers for the calendar (days from civil and back, weekday, days in month), PCF85063 time registers
  (BCD, 24-hour, years 2000-2099, the OS flag and any out-of-range field reject the time), Sensirion
  CRC-8 and the SHTC3 read-out (rounded hundredths).
- **core**: `rtc_get`/`rtc_set` in `core_params_t`; `core_init` sets the soft clock from the RTC,
  `TIME_SYNC` writes it (never UTC 0).
- **ui_common**: idle screen when there is no notification, without the header bar (user follow-up
  2026-10-12): a rounded box with a 56 px bold clock (24 h `09:05`, or 12 h `9:05` with AM/PM on the
  digits' baseline) and the date (`Sunday, 11 Oct` / `Chủ nhật, 11/10`); on boards with the sensor two
  rounded boxes below it, "Temp" `23.7°C` and "Humidity" `63%` (`Nhiệt độ`, `Độ ẩm`; 24 px bold values,
  `--.-°C` / `--%` when a reading fails); without a sensor the clock box fills the space. The footer's
  status line stays (no divider). "Time not set"
  replaces "No notifications" until the clock is known. A card's footer shows the current time. Redraws
  only when the shown minute or the rounded reading changes. `ui_set_time()`, `ui_set_climate()`,
  `ui_params_t.climate`, `ui_config_t.clock_24h/temp_unit`.
- **Fonts**: `font_gen.py` gained `FontSpec.chars` for fixed character sets; `ui_font_clock_56`
  (13 glyphs, 1.4 KB bitmaps) and `ui_font_bold_24` (17 glyphs, 0.4 KB). The 16 px fonts are unchanged.
- **Waveshare board**: `ws154_i2c.c` (I2C master bus at 100 kHz, PCF85063A, SHTC3) from the datasheets;
  `board_init` probes both chips and adds `rtc_get/rtc_set`, `read_climate` and the feature bits only
  when they answer. `main.c`: config now reaches the UI (it did not before: `on_config` was missing, so
  language and the display keys were ignored on the device), the RTC is wired into the core, the time is
  pushed every second and the sensor read once a minute.
- **Simulator and tools**: the simulated board has an RTC that counts with virtual time (lost by
  `sim_epaper_reset`, kept by a fake-board reboot) and a settable sensor (default 24.50 °C, 50 %);
  `fb_set_rtc`, `fb_rtc_utc`, `fb_set_climate`; `loopback.time_sync()` and `loopback.config_set()`;
  console commands `time [now|UTC] [tz=MIN]`, `clock 12|24`, `unit c|f`, `climate T RH|fail|none`; golden
  scene `clock` (`clock_24h.png`, `clock_12h_vi.png`).

## Verified (host only) and how

- Host tests (ASan + UBSan), 9 suites green: `test_board_api` +8 tests (calendar against `gmtime_r` every
  7 h 13 min over the whole u32 range, PCF85063 known registers, round trip 2000-2099, rejection of the
  reset state, bad BCD, Feb 30 and Feb 29 of a non-leap year; CRC-8 and SHTC3 against the datasheet
  examples), `test_core` +2 (RTC at boot, `TIME_SYNC` writes it, UTC 0 never written, no RTC = time lost at
  reboot), `test_ui` +6 (minute redraw with no work in between, time zones and date rollover, 12 h edge
  cases 00:05 and 12:05, the widest times stay inside the clock box and the sensor's extreme values
  (-45.0°C, 257.0°F) inside the climate boxes, climate rounding, °F, negatives, failure, idle-only).
- `pytest tools`: 237 passed, including the new `tools/tests/test_clock.py` (RTC through reboot and power
  loss, Capabilities follow the hardware, failing sensor, console commands, core-only library) and the
  golden images (`idle`, `pairing_low_battery` and the two `refresh_partial_*` frames changed because the
  idle screen changed; `clock_24h` and `clock_12h_vi` are new; all reviewed by eye).
- Contract check, `font_gen.py --check`, `icon_gen.py --check`, coverage of the portable core 98.1 %.
- Firmware: `idf.py -C variants/paper -B build/paper build`, zero warnings; `xtensa_check.sh`,
  `feature_check.sh`, `lvgl_conf_check.py` pass.
- Size: image 801,487 B (0xC3B50 incl. padding). This milestone adds about 17 KB (estimated from
  `size-components`: `esp_driver_i2c` 10.7 KB, the two display fonts about 3 KB, board code about 3 KB);
  the rest of the growth since the M4 notes (657,520 B) is the emoji fonts (D42). DIRAM 64 %.

## Not verified

- Anything on the board (hw-checklist 16-19): the I2C bus with the codec unpowered, the chips answering,
  the RTC set procedure, whether the RTC keeps time while the board is off (what feeds it through D1/D2),
  drift and the load capacitance (CAP_SEL / Offset), SHTC3 accuracy and self-heating, the look and the
  ghosting of the minute refresh, its current cost.
- iPhone users get no time before M5: the clock is set only by `TIME_SYNC` (the companion app, the fake
  board) or a previously set RTC; the iOS Current Time Service client (PLAN D16) comes with M5.

## Decisions and open points

- D43-D45 in PLAN. The RTC holds UTC; the offset stays in config.
- The idle screen triggers a full refresh every `display.full_refresh_every` partials like any other
  change, so with the default 10 it flashes about every 10 minutes when idle: to be judged on the panel
  (checklist 19); a separate idle policy is a small change if needed.
- Not done (not asked): temperature/humidity in DEVICE_STATUS, a sensor offset setting, the Tk viewer's
  form for the clock and sensor (the console commands cover it).

## Update 2026-10-12: connection status in the status bar (user request)

- The status bar (footer, on the idle screen and the cards) shows a 16x16 Bluetooth icon left of the
  battery text: the rune while a phone is connected, the rune struck through otherwise; hidden when
  powered off. The old `BLE` text prefix is gone (`USB 80%` remains). The left text (time, pairing,
  update progress) is clipped before the icon. Icons are pixel art in `icon_gen.py` (`ui_status_icons`);
  `ui_link_icon()` and the fake board's `display_state().link` report which one is shown.
- Tests: `test_ui` `test_connection_icon` (state changes, the icon's pixels on the panel, clipping of the
  longest left text, powered off), `test_clock.py` `test_connection_icon_follows_the_link`; every golden
  image with a status bar changed (the `clock_24h` scene now holds a connected link). Checklist item 20.
- Note: until M5 the firmware never reports a connection, so the board always shows the struck-through rune.
