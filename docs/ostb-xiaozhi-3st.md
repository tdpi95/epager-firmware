# ostb-xiaozhi-3st (LCD board): hardware notes

What we know about the LCD board, where each fact comes from, and what is still open. The summary table of all board facts is [PLAN.md](PLAN.md) §2b; this file holds the detail. There is no schematic and the case stays closed (user, 2026-10-13), so facts come from three sources:

- **[V]** measured on the board (esptool probes, boot log, tests that only reset the board);
- **[S+]** the stock firmware's own code, read from our flash backup (`docs/recovery.md` §6). It is the vendor's working setup for this exact panel, but still unverified by us;
- **[S]** the community ESPHome port (github.com/MKtuzi/ostb-xiaozhi-3st-esphome, reverse-engineered, no licence: facts only, never its code).

The stock firmware is vendor work under an unknown licence. Its numbers below are facts about the hardware. Its init command list is copied below as a **reference** (user decision 2026-10-13): our driver's sequence still comes from the controller datasheet (PLAN D47) and is checked against it.

## Pins and chips

Every GPIO the stock firmware uses, read from its code (method: last section). With the screen pins this is the whole board; the community port agrees on every pin it lists.

| GPIO | Function | Details | Source |
| --- | --- | --- | --- |
| 9, 10, 14, 8, 18 | LCD SCLK, MOSI, CS, DC, RESET | see Screen | [S+], [S] |
| 13 | LCD backlight | LEDC PWM, **25 kHz**, active high (not inverted) | [S+] `PwmBacklight(13, false, 25000)` |
| 12, 11 | I2C SDA, SCL | one bus (port 0) for touch and both audio chips; internal pull-ups on, glitch filter 7 | [S+] `InitializeI2c()` |
| (I2C 0x15) | CST816S touch | 400 kHz; reports 296 x 240 with no swap or mirror; **no reset and no interrupt pin** used (polled) | [S+] `InitializeCst816sTouchPad()` |
| (I2C 0x18, 0x40) | ES8311 codec, ES7210 4-mic ADC | 24 kHz in and out | [S+] `BoxAudioCodec(...)` |
| 5, 15, 16, 6, 7 | I2S MCLK, BCLK, WS, DOUT (speaker), DIN (mics) | shared clocks for codec and ADC | [S+], [S] |
| 4 | speaker amplifier enable |  | [S+], [S] |
| 40, 39 | volume up, volume down keys | `Button(gpio, active_high = false)`: active low | [S+], [S] |
| 47 | charge status | input with pull-up, polled every 3 s | [S+] `PowerManager(47)` |
| 17 | battery voltage | **ADC2 channel 6**, 12 dB attenuation, 12-bit, raw values (no calibration) | [S+] `PowerManager` |

**Power key:** not on any GPIO (the stock firmware creates GPIO buttons only for 39 and 40, and the power key is not GPIO0: PLAN §2b). A power chip switches the board in hardware: a long press turns it off even on USB. Firmware cannot see the key, and cannot switch the board off.

**Battery level (stock):** the raw ADC reading is smoothed (0.8 old + 0.2 new) and mapped through 12-step tables to 0, 10, ..., 90, 95, 100 %: on battery raw 1890, 1950, 2010, 2070, 2130, 2190, 2250, 2310, 2370, 2430, 2460, 2489; while charging (higher, the charger lifts the voltage) 1890, 2050, 2110, 2160, 2220, 2275, 2330, 2380, 2420, 2465, 2490, 2510. The divider is not known: at 12 dB attenuation raw 2489 is roughly 2.0-2.1 V, which with a 1:2 divider would be a full cell (about 4.1 V) [?].

## Screen

### Facts

| Fact | Value | Source |
| --- | --- | --- |
| Panel size | 240 x 296 native (portrait), used as **296 x 240 landscape** | [S+] stock display object and init window; [S] community port |
| Interface | 4-wire SPI, write only (no MISO), SPI3 host with DMA | [S+] `spi_bus_initialize(SPI3_HOST, ...)` |
| Pins | SCLK **9**, MOSI **10**, CS **14**, DC **8**, RESET **18** (active low) | [S+] stock code, agrees with [S] |
| Backlight | GPIO **13**, LEDC PWM at 25 kHz, active high; the stock firmware sets 50 % then 75 % at boot | [S+]; [V] boot log |
| SPI clock | **40 MHz** in the stock firmware, SPI mode 0, 8-bit commands and parameters; **80 MHz works** (our driver, hw-checklist 23) | [S+]; [V] 80 MHz |
| Largest transfer | 38,400 bytes (240 x 80 x 2, i.e. sized for 80-line bands) | [S+] `max_transfer_sz` |
| Pixel format | RGB565 (`COLMOD` 0x3A = 0x05), RGB element order, colours **not** inverted | [S+] |
| Orientation | `swap_xy` on, mirror **y** only (x not mirrored) | [S+] `esp_lcd_panel_swap_xy/mirror` arguments |
| Offset | **24 pixels** on x (after the swap) and 0 on y: the visible 296 pixels sit at 24..319 of a 320-pixel controller axis | [S+] display object; fits a 240 x 320 controller RAM |
| Tearing effect | TE output enabled by the init (`0x35` = 0x00, V-blank only); whether the TE pin reaches a GPIO is unknown | [S+] command; pin [?] |
| Init timing | hardware reset, init sequence, sleep out (`0x11`) then 120 ms, display on (`0x29`) sent separately; first frame about 0.75 s after the start of init | [S+] sequence; [V] boot log times |
| Driver used by the stock firmware | `78/esp_lcd_nv3023` 1.0.0 (Apache-2.0) as a command sender, with the board's own 25-command sequence; the component's default 128 x 128 table is not used | [S+] |

### Stock init sequence (reference)

What `OSTB_XIAOZHI_3ST::InitializeNv3023Display()` passes as the panel's init commands (`init_cmds_size` = 25, table at DROM 0x3c406fd0 of `ota_1.bin`, regenerated from the image, not retyped). It runs after the hardware reset; the driver adds `MADCTL` (`36`) for the swap/mirror settings, and display on (`29`) is sent afterwards by `esp_lcd_panel_disp_on_off`.

| # | Command | Parameters | Delay |
| --- | --- | --- | --- |
| 1 | `DF` | `98 53` |  |
| 2 | `DE` | `00` |  |
| 3 | `B2` | `25` |  |
| 4 | `B7` | `00 29 00 51` |  |
| 5 | `BB` | `4F 1A 55 73 63 F0` |  |
| 6 | `C0` | `44 A4` |  |
| 7 | `C1` | `12` |  |
| 8 | `C3` | `7D 07 14 06 C8 71 6C 77` |  |
| 9 | `C4` | `00 00 94 79 25 0A 16 79 25 0A 16 82` |  |
| 10 | `C8` | `3F 34 2D 26 2B 2B 25 24 23 22 20 17 14 0E 06 00 3F 34 2D 26 2B 2B 25 24 23 22 20 17 14 0E 06 00` |  |
| 11 | `D0` | `04 06 6B 0F 00` |  |
| 12 | `D7` | `00 30` |  |
| 13 | `E6` | `10` |  |
| 14 | `DE` | `01` |  |
| 15 | `B7` | `03 13 EF 35 35` |  |
| 16 | `C1` | `14 15 C0` |  |
| 17 | `C2` | `06 3A` |  |
| 18 | `C4` | `72 12` |  |
| 19 | `BE` | `00` |  |
| 20 | `DE` | `00` |  |
| 21 | `35` | `00` |  |
| 22 | `3A` | `05` |  |
| 23 | `2A` | `00 00 00 EF` |  |
| 24 | `2B` | `00 00 01 27` |  |
| 25 | `11` | (none) | 120 ms |

Reading notes: parameters that point into `.bss` are zero bytes (the component's own default table, whose source is public, decodes the same way, which confirms it). `DE` looks like a register-page select (page 0 for entries 2-13, page 1 for 15-19, back to 0). `C8` is probably the gamma table (two identical 16-byte halves). `2A`/`2B` set the window to columns 0-239 and rows 0-295. The meaning of the vendor registers (`B2`-`E6`) is to be read in the controller datasheet, not guessed.

### Our driver

`boards/ostb_xiaozhi_3st/src/xz3_lcd.c` (PLAN D46, D47) sends exactly the stock sequence above with the stock timing and adds a black frame before display on; `write_area` copies the caller's RGB565 pixels into two 24-line DMA bands (byte-swapped to the panel's order). Touch: the stock firmware gives the CST816S a 296 x 240 range with no swap or mirror, but its touch use may not need positions at all (touch to talk), so the mapping is checked on the board before the UI relies on it (hw-checklist item 25).

### Which controller?

The stock firmware creates the panel with an **NV3023** driver, and the community port calls it an NV3023 too. But the init sequence the board actually sends starts with `DF 98 53`, the password command of the **Jadard JD9853** family, and uses JD9853-style page selection (`DE 00` / `DE 01`); an NV3023 sequence starts with `FF A5` (the component's own default table). A 240 x 296 window plus a 24-pixel offset also fits a 240 x 320 controller such as the JD9853. **Working assumption: JD9853-class controller [?].** Before writing the driver, read both datasheets, then confirm on the panel (colours, orientation, offset). The controller ID cannot be read back: there is no MISO line.

### Open questions

- Controller model (above) and therefore the right datasheet for the init sequence.
- Whether the TE pin is wired to a GPIO (tear-free animation needs it; otherwise time the writes).
- The community port saw a different garbage pattern after some resets (it suspects the RESET pulse timing): measure our reset timing and run 20 resets in a row (P1a acceptance).

### Numbers for the UI

A full 296 x 240 RGB565 frame is 142,080 bytes: measured 28.1 ms at 40 MHz and **15.3 ms at 80 MHz** (our driver), so whole-frame updates top out near 65 frames a second. Changed areas only (PLAN D46) keep animation cheap.

## How these facts were read

1. `esptool.py image_info --version 2` gives the segment map. Its "File offs" is the 8-byte segment header: the data starts 8 bytes later (DROM 0x3c250020 at file offset 0x20).
2. ESP-IDF's `esp_lcd` init tables are arrays of `{int cmd; const void *data; size_t len; unsigned delay_ms}`; scanning DROM for runs of such records found two tables. Data pointers into DRAM past the loaded `.data` point at `.bss`, i.e. zero bytes.
3. The code that uses each table was found through its literal-pool entry (the table's address stored in IROM) and the `l32r` instructions that load it (`xtensa-esp32s3-elf-objdump -b binary -m xtensa --adjust-vma=0x42000020`). One table is referenced from `OSTB_XIAOZHI_3ST::InitializeNv3023Display()` (the board's), the other from `esp_lcd_nv3023.c` (the component default).
4. The configuration structs were read from the stack stores before each call, with the field layouts of ESP-IDF 5.5.5's `esp_lcd_io_spi.h`, `esp_lcd_panel_dev.h` and `spi_common.h`.
