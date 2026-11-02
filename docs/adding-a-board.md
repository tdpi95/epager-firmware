# Adding a board (or a variant)

A **board** is hardware (pins, display driver, battery, buttons, chip and flash, partition table); a
**variant** is a product class built from shared components (UI layout, features, version, capabilities).
There are two variants, `paper` (e-paper boards) and `lcd` (colour LCD boards); each can have several
boards. Today `variants/paper` runs on `boards/waveshare_s3_epaper_154` and `variants/lcd` on
`boards/ostb_xiaozhi_3st`. A new board is one new folder under `boards/`; no variant and no shared code
changes. Read [PLAN.md](PLAN.md) §3 for the architecture.

Before porting, use [AI-assisted discovery](#ai-assisted-discovery) to identify the hardware, back up
stock firmware and prepare a bring-up plan for review. The implementation steps below follow once
that plan is approved.

```
variants/
  common.cmake        project setup shared by every variant (board selection, sdkconfig layering)
  sdkconfig.common    chip-independent BLE, Wi-Fi, power, LVGL and logging settings
  targets/<chip>.sdkconfig   what depends on the chip (target name, PSRAM use for Wi-Fi/TLS, BLE controller)
  sdkconfig.signed    overlay for -DEPAGER_SIGN=ON
  paper/ lcd/         CMakeLists.txt (variant name + default board), sdkconfig.defaults (variant settings),
                      version.txt, dependencies.lock, main/ (identity: variant id, product name)
boards/<board>/       CMakeLists.txt + src/ (the ESP-IDF component), board.cmake (its variant and chip),
                      sdkconfig.board (chip, flash, PSRAM, console, BLE clock), partitions.csv
```

sdkconfig layers, later wins: `variants/sdkconfig.common`, `variants/targets/<chip>.sdkconfig`,
`variants/<variant>/sdkconfig.defaults`,
`boards/<board>/sdkconfig.board`, the board's partition table (generated into the build directory), then
`sdkconfig.signed` and `-DEPAGER_EXTRA_SDKCONFIG` when asked for.

## AI-assisted discovery

Give a local coding agent the exact board model/revision (or say it is unknown), USB serial port,
product links, schematics/datasheets and physical constraints such as a case that must stay closed.
Identify the intended device if several boards are connected. Start with read-only discovery and
offline analysis of a stock backup; a probe image is still a flash and requires approval.

Use this prompt, replacing the bracketed fields:

```text
Investigate a new board for ePager before implementing a port.
Model/revision: [exact model, or unknown]
USB serial port: [port, or discover it]
Sources: [product page, schematic, datasheets, vendor firmware/source]
Physical constraints: [available buttons, case access, etc.]

Read AGENTS.md, docs/PLAN.md, docs/MILESTONES.md, docs/adding-a-board.md
and docs/recovery.md before starting. Identify the connected device and use
the installed ESP-IDF/esptool tools for read-only chip, flash-size and security
discovery. Explain that serial probes may reset the board. If Secure Boot
or flash encryption is enabled, report the implications before proceeding.

Back up the complete stock flash using the detected size, record its SHA-256,
and have me keep a private copy outside build/. Do not commit or share dumps
or raw boot logs: they may contain Wi-Fi credentials or other private data.
Inspect the backup's partition table, app metadata and driver clues. Capture
the stock boot log where possible without replacing the running firmware.

Write docs/<board>.md with a table of facts, sources, confidence and remaining
hardware checks. Investigate pins/polarities, display and touch controllers,
flash/PSRAM mode and reserved pins, power rails/latch, buttons, battery sensing
and the USB/download-mode recovery path. Prefer schematics and datasheets;
label stock-image and community clues as unverified. Do not guess pin maps
or copy unlicensed vendor code or waveform tables.

Propose a minimal bring-up plan for review before implementing the port.
Keep board details behind board_api, pins in one header, and unknowns marked
TODO(verify) or TODO(hw-verify), with checks in docs/hw-checklist.md.
Build and inspect the image before requesting permission to flash it.
Do not flash any image, erase or restore flash, or change eFuses/security
settings without my explicit approval. Preserve the board's recovery path.
```

The initial result is a private stock backup, a sourced hardware report and a reviewable bring-up
plan. The [probe and backup procedure](recovery.md#6-probe-a-new-board-and-back-up-its-stock-firmware)
contains commands recorded on the existing ESP32-S3 LCD board; adapt chip, port and flash size to the
new device. Reading flash does not verify display wiring, touch or battery measurements. Test those
after plan/flash approval and record results separately from source-derived facts.

## What shared code may know about a board

Only `components/board_api/include/epager/board_api.h`:

| Interface | What the board provides |
|---|---|
| `board_get()` | the `board_profile_t`: id (`CAPABILITIES.board_id`), flash size, hardware `PROTO_FEATURES_*`, OTA low-battery threshold |
| `profile.init` (optional) | first call at boot: take the power latch, put rails and pins in a safe state |
| `profile.display` (`board_display_t`) | `get_info`, `init`, `write_frame`, `refresh(kind)` + `busy`, `choose_refresh`, `sleep`, `wake`; **frame-based** (PLAN D29): the UI renders into its own frame and pushes whole frames |
| `read_battery`, `set_button_handler`, `set_sleep` (stop key polling and arm the keys as light-sleep wake-up sources while the screen is off, PLAN D66), `set_rail`, `rtc_get/set`, `read_climate` | optional (NULL = absent); a board that probes its chips at `init` fills these in (and the matching `features` bits) only when they answer, like the Waveshare RTC and SHTC3 |

Everything else (pins, controller commands, ADC, timers) stays private to the board. Shared code never
includes a board header and never mentions a board by name; `tools/tests/test_fake_board.py` greps for
that. `components/board_api/include/epager/board_util.h` has pure helpers boards can reuse (single-cell
LiPo curve, debounced short/long key detector); they are host-tested in `tests/host/test_board_api.c`.

## Steps

1. **Read the sources first and write down where each fact came from.** Pins and polarities come from
   the schematic, not from vendor demo code (AGENTS.md "Do NOT assume"). Put every pin in **one header**
   (`boards/<board>/src/<board>_pins.h`, see `ws154_pins.h` for the format: net name, sheet, polarity).
   Anything you cannot confirm gets `TODO(verify)` (a document question) or `TODO(hw-verify)` (needs the
   board) and a row in [hw-checklist.md](hw-checklist.md).
2. **Create `boards/<board>/`** as an ESP-IDF component whose directory name is the board name:
   `CMakeLists.txt` (`idf_component_register`, `include(../../cmake/epager.cmake)` and
   `epager_component_warnings()`, so the code builds with `-Wall -Wextra -Werror -Wshadow`), `src/board.c`
   with `board_get()`, `src/<board>_pins.h`, and the display driver. Also in that folder: `board.cmake`
   (`set(EPAGER_BOARD_VARIANT paper)` or `lcd`: building a board under the wrong variant is refused; and
   `set(EPAGER_BOARD_TARGET <chip>)`, which needs `variants/targets/<chip>.sdkconfig`), `sdkconfig.board`
   (flash size and mode, PSRAM, console, BLE low-power clock,
   board-only options such as `CONFIG_EPAGER_BOOT_SOUND`; one line of reason each) and `partitions.csv`
   (slots only change by a USB reflash).
3. **Write the display driver against `board_display_t`.** Contract the UI relies on
   (`components/ui_common/src/ui.c`): `init` leaves the panel awake and ready; `write_frame` only while
   not busy; the panel may be `sleep`ing (rail off) between refreshes, so keep whatever the controller
   needs to resume (the Paper driver keeps the frame that is on the panel); `busy()` must already be
   true when `refresh()` returns; `refresh` never blocks for the whole update. LCD drivers answer FULL
   from `choose_refresh`.
   **LCDs use the area path instead (PLAN D46):** `write_area` takes RGB565 in CPU byte order and
   **copies** it (the driver swaps bytes into its own DMA buffers), so the caller's buffer is free when the
   call returns; `write_frame`, `refresh` and `busy` stay NULL. Keep the backlight off (`set_backlight`)
   until a real frame is on the panel. `boards/ostb_xiaozhi_3st/src/xz3_lcd.c` is the example.
4. **Add the host-testable parts to `board_util.h`** (or test them where they live) rather than leaving
   logic in code that only runs on hardware.
5. **Select the board from a variant.** A variant's `CMakeLists.txt` sets its default board
   (`EPAGER_DEFAULT_BOARD`); `-DEPAGER_BOARD=<dir>` overrides it, e.g.
   `idf.py -C variants/paper -B build/paper-<board> -DEPAGER_BOARD=<board> build`. `variants/common.cmake`
   adds only `boards/${EPAGER_BOARD}` to `EXTRA_COMPONENT_DIRS`, so other boards (including the host-only
   simulator) are never compiled in. A fresh build directory is needed: an existing `sdkconfig` keeps its
   old values.
6. **Describe the product in the variant** (`variants/<variant>/`), only when the product itself changes:
   `sdkconfig.defaults` (UI layout, features; one line of reason per option), `version.txt`, `main/` (what
   it advertises: variant id, features). Variants never copy shared code; features are `CONFIG_EPAGER_FEATURE_*` options (`components/core/Kconfig`), and
   `tools/feature_check.sh --prove` shows that a disabled feature leaves no symbols in the image.
7. **Make the simulator match, if the board has a host twin**: `boards/sim_*` implements the same
   interfaces (host only, never in firmware), so the UI and the app can be developed without the board.
8. **Build and measure**: `idf.py -C variants/<variant> -B build/<variant> build`, then `size` and
   `size-components`; record image size, slot size (rule: slot ≥ 1.4x the final image, slots can only be
   changed by a USB reflash) and leftover asset space in the milestone notes. Run
   `tools/lvgl_conf_check.py` if the UI is built.
9. **Update the docs**: `AGENTS.md` (layout), `docs/PLAN.md` (verified facts, decisions), `hw-checklist.md`.

## Checklist for a new board

- [ ] stock firmware backed up and the chip probed before the first flash ([recovery.md](recovery.md) §6)
- [ ] every pin from the schematic, with polarity, in one header
- [ ] power latch / rails: what must be driven first, and what happens with the battery only
- [ ] display controller identified from a datasheet (not only from a demo)
- [ ] frame format conversion (bit order, inversion) done in the driver
- [ ] battery: pin, divider, how external power is detected (or `PROTO_EXTERNAL_POWER_UNKNOWN`)
- [ ] clock source for BLE light sleep (is there a 32 kHz crystal for the **SoC**?)
- [ ] PSRAM mode (quad or octal) from the module datasheet; GPIOs the module reserves
- [ ] `docs/recovery.md` entry: how to enter the ROM download mode on this board
- [ ] every unverified item listed in `hw-checklist.md`

## Another LCD or paper board, for orientation

Needs from the user: the exact board model. Then: `boards/<board>` as above (display and touch drivers,
pins, `sdkconfig.board`, a partition table sized for the flash), a `board.cmake` naming `lcd` or `paper`.
The variant, the colour asset formats in `contracts/asset-formats.md` and the shared components do not
change. A board on a **new chip** also adds `variants/targets/<chip>.sdkconfig` (and the chip-dependent
lines move out of `sdkconfig.common` if they do not apply to it); a board with a **new screen size** adds
golden images under `tests/golden/<variant>/<WxH>/`. What waits for the second case (shared controller
drivers under `drivers/`, font size classes, a simulator with a size parameter, a toolchain check per
chip) is outlined in PLAN D94. `board_id` in `CAPABILITIES` tells the app which board it is talking to.
