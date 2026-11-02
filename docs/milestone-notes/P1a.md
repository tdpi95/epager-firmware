# P1a: LCD board bring-up (2026-10-13)

The LCD board arrived on 2026-10-13: an **ostb-xiaozhi-3st**, an unofficial "xiaozhi" AI-toy board (ESP32-S3, 16 MB flash, 8 MB octal PSRAM, 240x296 LCD, CST816S touch, ES8311/ES7210 audio). There is no schematic and the case stays closed (user). Plan approved the same day with these user decisions: community pins as the starting point; nothing flashed without the user's explicit go-ahead; the LCD driven with the stock firmware's init list by our own driver; P1a before M5. PLAN §2b, §5b, §7b, D46-D50. Board notes: [ostb-xiaozhi-3st.md](../ostb-xiaozhi-3st.md).

## Done

- **Stock firmware backed up and studied**: every pin, the SPI/LCD setup (40 MHz, swap + mirror y, 24-pixel gap), the 25-command init list, the touch setup (polled, no reset/interrupt pin), battery on GPIO17 / ADC2 channel 6 with raw-count level tables, the keys (GPIO40/39 only), the backlight (GPIO13, 25 kHz). The power key is on no GPIO (a hardware power switch) and is not GPIO0 (reset-only test). The init list unlocks with `DF 98 53`, a JD9853-family command, so the controller may not be the NV3023 the stock driver is named after. Its assets (20 face GIFs, a CJK font, a wake-word model) were unpacked for study only.
- **`board_api`** (D46, commit `a888699`): optional area path for LCDs (`write_area`: RGB565 in CPU order, copied by the driver), `set_backlight`, polled `read_touch` (`board_touch_t`), `BOARD_BTN_UP`/`DOWN`. `board_util.h`: `board_table_pct`, `board_cst816s_decode`, host-tested. Paper and simulator unchanged.
- **`boards/ostb_xiaozhi_3st`** (`e099dbd` and fixes): `xz3_pins.h`; `xz3_lcd.c` (esp_lcd SPI IO, stock reset timing and command order, black frame before display on, two 24-line DMA bands with the byte swap, LEDC backlight); `xz3_touch.c` (I2C bus shared with the audio chips, CST816S polled, a per-axis calibration measured on the board); `board.c` (amplifier off and backlight off first, volume keys with the `board_util` detector, battery through the stock tables with the stock smoothing, charge status as the external-power hint, `set_rail(AUDIO)` = amplifier).
- **`variants/lcd`** (`7d6a12a` and fixes): 16 MB partitions (2 x 4 MiB slots, 7.9 MiB LittleFS), QIO flash and 80 MHz PSRAM, no power management yet, touch feature on, BLE later (M5). `main.c` is a **bring-up image**, not the product: test patterns through `write_area` (colour bars with border and corner markers, gradients, solid colours, touch canvas, 9 touch targets), keys switch patterns and backlight, touch and battery on the console, NVS and LittleFS mounted. 257 KB (6 % of a slot).
- **Docs**: board notes, PLAN, `hw-checklist.md` items 21-28, `recovery.md` §6 (probe and back up any new board) and §7 (LCD), `adding-a-board.md` (area path), `AGENTS.md`. The planned `tools/hw_probe` was dropped (D48): every pin came from the stock code.

## Verified, and how

On the board (hardware session 2026-10-13, 7 flashes over USB with no key pressed; hw-checklist):

| Item | Result |
| --- | --- |
| 21 LCD picture | pass: colour bars, border, corner markers, gradients, solid colours (user) |
| 22 Reset robustness | pass: 20 resets in a row clean (user); every boot identical in the log |
| 23 LCD speed | pass at **80 MHz**: full frame 28.1 ms at 40 MHz (stock clock), 15.3 ms at 80 MHz, 15.0 ms with 80 MHz PSRAM; 60 MHz is not reachable on the S3 |
| 24 Backlight | pass: 0-100 %, off at 0, no flicker or whine; dark until the first frame on a cold boot (user) |
| 25 Touch | pass with calibration: axes match; 28 taps on 9 targets, error 6.0/6.4 px before and 4.9/4.3 px after the fit (checked with a second round); the left edge reads 15-30 px in when the finger is pressed against the case (the contact patch's centre, not an offset) |
| 26 Keys | pass: volume up/down short and long; power key switches off and on in hardware (user, log) |
| 27 Battery, charging | partly: raw 2483-2485 (98-99 %) on USB, status line high; 2026-10-14 from 62 %: line low while charging (polarity settled), level rises 62 -> 80 % in 22 min |
| 28 First boot | pass: PSRAM, storage, the three I2C chips, heap and stack, speaker silent, QIO + 80 MHz PSRAM (picture 0.95 s after reset) |

On the PC: LCD and Paper builds with zero warnings, host tests (coverage 98.1 %), 238 Python tests (golden images unchanged), contract check, `xtensa_check.sh`, `feature_check.sh`, `lvgl_conf_check.py`.

## Not verified

- Battery (item 27): the level on battery only (falling in use; needs a log that survives without USB) and the status line at charge termination seen live; the battery divider and capacity stay unknown (case closed).
- Whether the touch chip still answers after a long time untouched (its own low-power mode).
- The controller model (NV3023 or JD9853): no datasheet confirmed; not needed while the stock list works.
- Heat and long runs at 80 MHz SPI; light sleep and BLE on this board (P1b/M5).

## After closing

- 2026-10-13, at the user's request: the stock firmware was restored from the backup (`recovery.md` §6: 16 MB in 108 s, hash verified). It boots as before (xiaozhi 2.4.2.1, NV3023 init, CST816S id 184, Wi-Fi scanning with its saved network). The board runs the stock firmware until the next LCD flash.

## Decisions and contract changes

- No contract change (still 0.7.0).
- D46 area path (synchronous copy instead of caller-owned DMA buffers; polled touch), D47 own drivers with the stock init list, D48 pins from the stock code and `hw_probe` dropped, D49 partitions, D50 USB-only recovery (no power management in the first image; `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` when PM comes).
- LCD at 80 MHz (twice the stock clock; 40 MHz is the documented fallback). QIO flash and 80 MHz PSRAM as the stock firmware runs them.

## For P1b

- The UI: LVGL on `write_area` (partial render, the driver already copies and swaps), colour fonts and icons, touch and key navigation, the simulator twin, contract 0.8.0 (`display.brightness`, colour app icons). Keep touch targets away from the very edges (finger against the case).
- Backlight dimming and switch-off when idle: the main lever for the panel's life (user question).
- `CONFIG_SPIRAM_MEMTEST=n` would cut another 0.4 s from the time to picture.
- Pairing mode needs a key combination: there is no BOOT key and the power key is hardware.
- Items 27 and the touch idle check carry over.
