# AGENTS.md — ePager firmware

Read this first in every session. Details live in `docs/PLAN.md` (architecture, decisions) and `docs/MILESTONES.md` (status, acceptance criteria). Per-milestone notes live in `docs/milestone-notes/M<n>.md`; read only the ones the task needs.

## What this is

ePager: small ESP32-S3 desk displays that show phone notifications over BLE only for the phone link (no protocol over Wi-Fi in v1; both boards join Wi-Fi only to fetch the weather and the time, PLAN D60, D78; firmware updates from GitHub Releases over Wi-Fi are required because there is no iOS app, and planned: docs/wifi-ota-plan.md). iOS via ANCS (board is the ANCS client, no iOS app needed); Android via the companion app writing to our custom GATT service. Two firmware products from shared components:

- `variants/paper`: Waveshare ESP32-S3-ePaper-1.54 **V2** (200x200 B/W, 8 MB flash, 8 MB PSRAM); flashed and running since 2026-10-15 (hw-checklist 16, 61), Wi-Fi and weather added in D78 (not yet run on the board, checklist 60-62).
- `variants/lcd`: colour LCD on the **ostb-xiaozhi-3st** (ESP32-S3, 16 MB flash, 8 MB octal PSRAM, 240x296 LCD (NV3023 or JD9853 class), CST816S touch; arrived 2026-10-13, facts in `docs/PLAN.md` §2b and `docs/ostb-xiaozhi-3st.md`).

This repo is the **source of truth** for the app/firmware contract in `/contracts`.

## Repo layout

```
contracts/     protocol.md, asset-formats.md, gatt.json, registry.json, schemas/, test-vectors/,
               CONTRACT_VERSION, CHANGELOG.md, compatibility.md, releases.md + release-manifest.schema.json
               (firmware GitHub Releases; hand-written schema)
components/    protocol, notification_pipeline, config_store, transfer, core   (portable core, no ESP calls;
               core = dispatcher: characteristic writes -> handlers, events, read values)
               board_api (headers: frame path for e-paper, area path + backlight + polled touch for LCDs;
               board_util.h = host-tested battery curve and level table, key detector, calendar,
               PCF85063 registers, SHTC3 read-out, CST816S touch point),
               ui_common (LVGL 9.5.0 glue + placeholder card UI with app/category icons + idle clock/climate
               screen; builds on the host and in firmware)
               weather (portable: bounded JSON reader, Open-Meteo/ipinfo parsers, URLs, fetch scheduler, D64)
               clock_face (portable, M8a: `cf_parse` bounded parser of a clock face file, `cf_opts` option values, `cf_eval`
               what every element draws; contracts/asset-formats.md "Clock faces"; the renderer is ui_common/ui_face.c)
               net (Wi-Fi task: fetch cycles, HTTPS, SNTP, setup hotspot; net_util.c is portable and host-tested)
               app (firmware glue shared by both variants, debug console, D54/D57, battery log)
               power (esp_pm skeleton, M4), ota   (ESP-IDF glue, M6)
               ble_core (M5: NimBLE glue; `ble_util.c` is portable and host-tested), ancs_client (M5: `ancs_sm.c` is the
               portable state machine, `ancs_glue.c` the NimBLE side)
variants/      common.cmake + sdkconfig.common + sdkconfig.signed (shared by every variant); paper/, lcd/ (P1a bring-up image)
               (one ESP-IDF project each: CMakeLists.txt, main/, sdkconfig.defaults, version.txt, dependencies.lock);
               a board is picked with -DEPAGER_BOARD=<boards/ folder>, docs/adding-a-board.md
boards/        waveshare_s3_epaper_154/ (firmware: paper variant), sim_epaper_154/ (host-only simulator),
               ostb_xiaozhi_3st/ (lcd variant: LCD on the area path, CST816S touch)
               (BoardProfile + DisplayDriver per board; pins in one header per board)
cmake/         epager.cmake (warning flags for our ESP-IDF components)
web/           flashing page (index.html, GitHub Pages, ESP Web Tools); built into a site by tools/web/make_site.py
keys/          git-ignored: signing keys (dev key for -DEPAGER_SIGN=ON), never committed
tests/host/    CMake + Unity host tests (portable core, UI on the sim board) against contract vectors;
               also builds build/sim/libepager_{core,sim}.so for the fake board (lvgl.cmake fetches LVGL)
tests/golden/  golden PNGs per variant and screen size, <variant>/<WxH>/ (tools/tests/test_golden.py)
tools/         epager_proto/ (Python reference codec + transfer board model; clockface.py = the clock face reference), codegen/ (gen.py, vectors.py, clockface_vectors.py),
               clock_face/ (M8a: compiler of face.json + PNGs into a clock_face_v1 file, gen_builtin.py; the full `epager-face` CLI is M8c),
               contract_check.py, coverage.py, xtensa_check.sh, feature_check.sh, lvgl_conf_check.py,
               fake_board/ (ctypes shim in native/, TCP + BLE, Tk viewer), test_client/ (TCP + bleak),
               assets/font_gen.py, assets/icon_gen.py (built-in icons), release/ (release manifest), sign/ (M6); see tools/README.md
.deps/         git-ignored downloads pinned by SHA-256 (LVGL archive, source fonts)
docs/          PLAN.md, MILESTONES.md, milestone-notes/M<n>.md, adding-a-board.md, recovery.md, flashing-page.md, hw-checklist.md,
               ostb-xiaozhi-3st.md (LCD board notes: screen), waveshare-epaper-154-v2.md (vendor docs for the
               Paper board: V1/V2, battery figures, pin picture, doubtful points)
```

## Environment

- ESP-IDF **v5.5.5** at `~/esp/esp-idf`; activate with `. ~/esp/esp-idf/export.sh`. Xtensa GCC 14.2.
- cmake 3.30.2 + ninja 1.12.1 installed via `idf_tools.py` into `~/.espressif`: on PATH **only after** `export.sh`.
- Host: GCC 13.3, Python 3.12.3 with `tools/.venv` (pytest, jsonschema, bless 0.3.0, bleak 3.0.2, Pillow, fonttools), Tk 8.6, BlueZ 5.72 with `hci0` (peripheral mode works: GATT app + advertising register).
- Network goes through the HTTPS proxy in the environment; the component registry and GitHub are reachable. SDL2 runtime only (no `libsdl2-dev`, no `pkg-config`): the screen simulator uses Tk, not SDL.

## Commands (status in brackets; update when verified)

- Env for cmake/ninja/idf.py: `. ~/esp/esp-idf/export.sh`
- Host tests [VERIFIED M2]: `cmake -S tests/host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host --output-on-failure` (ASan+UBSan on by default; longer fuzz: `EPAGER_FUZZ_ITERS=1000000 build/host/test_fuzz`)
- Coverage [VERIFIED M2] (no gcovr needed): `cmake -S tests/host -B build/cov -G Ninja -DEPAGER_COVERAGE=ON -DEPAGER_SANITIZE=OFF && cmake --build build/cov && ctest --test-dir build/cov && tools/.venv/bin/python tools/coverage.py build/cov --min 85`
- Cross-compile the portable core for ESP32-S3 (warnings as errors, sizes, stack frames) [VERIFIED M2]: `tools/xtensa_check.sh`
- Regenerate after editing `contracts/registry.json` or vector cases [VERIFIED M2]: `tools/.venv/bin/python tools/codegen/gen.py && tools/.venv/bin/python tools/codegen/vectors.py`
- Contract check [VERIFIED M2]: `tools/.venv/bin/python tools/contract_check.py`
- Python tools setup [VERIFIED M1]: `python3 -m venv tools/.venv && tools/.venv/bin/pip install -r tools/requirements.txt`
- Python tests [VERIFIED M3]: `tools/.venv/bin/pytest -q tools` (builds build/sim on first use; includes the end-to-end flows over TCP and the golden images)
- Host tests include `test_core` and `test_ui` [VERIFIED M3], `test_ble` and `test_ancs` [VERIFIED M5]; `-DEPAGER_WITH_UI=OFF` skips LVGL.
- Fake board libraries [VERIFIED M3] (built on demand by the tools, or by hand): `cmake -S tests/host -B build/sim -G Ninja -DEPAGER_SANITIZE=OFF && cmake --build build/sim`
- Fake board [VERIFIED M3]: `tools/.venv/bin/python tools/fake_board [--view] [--ble] [--no-screen]` (console `help`; clock and sensor [VERIFIED M4b]: `time now tz=420`, `clock 12|24`, `unit c|f`, `mask on|off` [VERIFIED D91], `climate 23.5 60`; LCD keys and touch [VERIFIED D66]: `button both` = sleep now, `button both long` = pairing mode, `touch 148 200 148 60` = swipe up = settings (a tap on a row selects, swipe up/down = page, `touch 60 120 220 120` = swipe right = back: PLAN D99; on a card swipe up hides it), `touch 148 60 148 200` = swipe down = dismiss a card / open the cards: PLAN D70; paper keys [VERIFIED D98, host]: `button pwr` = settings from the clock, `button boot` = the cards, in the settings `button boot|pwr` = up|down, `button boot long` = select, `button pwr long` = back, `button pwr hold` = power off (5 s), `button bootpwr long` = pairing mode); headless PNG: `tools/.venv/bin/python tools/fake_board snapshot out.png --scene vietnamese`; weather and Wi-Fi [VERIFIED P1b, both boards since D78]: `weather 31.2 1 hi=32.1 lo=24.6 rain=86 place=Hanoi`, `weather off`, `weather live [CITY] [lang=vi]` (real Open-Meteo/ipinfo answers through the board's parser; needs the network), `wifi`, `wifi-state connected`, `setup waiting|joining|done|failed|off [NETWORK]`
- Test client [VERIFIED M3]: `tools/.venv/bin/python tools/test_client --self-test` or `--tcp HOST:PORT run|bench`; over BLE against a real board [UNVERIFIED, needs the board]: `--scan`, then `--ble ADDRESS [--passkey NNNNNN] run|bench` with the board in pairing mode (a BlueZ agent, `tools/test_client/bluez_agent.py`, types the code the board shows)
- Golden images [VERIFIED M3]: part of `pytest tools`; regenerate with `tools/.venv/bin/pytest tools --update-golden`
- Fonts [VERIFIED M3]: `tools/.venv/bin/python tools/assets/font_gen.py` (`--check` to compare)
- Built-in icons [VERIFIED M3]: `tools/.venv/bin/python tools/assets/icon_gen.py` (`--check` to compare, `--preview out.png` to look)
- Firmware build [VERIFIED M4] (first run fetches `lvgl/lvgl` and `joltwallet/littlefs` from the registry; a clean build takes about 1-4 min and has zero warnings): `idf.py -C variants/paper -B build/paper build`
- Signed firmware (option A) [VERIFIED M4]: generate the dev key once with `espsecure.py generate_signing_key --version 2 keys/dev_signing_key.pem`, then `idf.py -C variants/paper -B build/paper-signed -DEPAGER_SIGN=ON build`
- Size report [VERIFIED M4]: `idf.py -C variants/paper -B build/paper size` and `... size-components`
- Feature gating [VERIFIED M4] (needs build/paper; `--prove` also builds with the features on): `tools/feature_check.sh [--prove]`
- LVGL host-vs-firmware config diff [VERIFIED M4] (needs build/paper and the host build): `tools/.venv/bin/python tools/lvgl_conf_check.py`
- Probe a new board and back up its stock flash [VERIFIED 2026-10-13, ostb-xiaozhi-3st] (needs the `dialout` group): `esptool.py --chip esp32s3 -p /dev/ttyACM0 flash_id`, `... get_security_info`, `... -b 921600 read_flash 0 0x1000000 build/<board>-stock/flash_16mb.bin`; partition table, boot log and restore in docs/recovery.md §6. The backup holds vendor NVS (Wi-Fi passwords): never commit it.
- LCD firmware build [VERIFIED P1a, zero warnings, 257 KB bring-up image]: `idf.py -C variants/lcd -B build/lcd build` (an existing `build/lcd/sdkconfig` keeps its old values when `sdkconfig.defaults` changes: delete it to regenerate; PM, tickless idle and the USB light-sleep guard came in with D66)
- LCD flash [VERIFIED P1a, 7 flashes; only after the user's go-ahead] and monitor: `idf.py -C variants/lcd -B build/lcd -p /dev/ttyACM0 flash monitor` (docs/recovery.md §7)
- LCD P1a bring-up image [VERIFIED 2026-10-14] (a fragment with `CONFIG_EPAGER_LCD_BRINGUP=y`): `idf.py -C variants/lcd -B build/lcd-bringup -DEPAGER_EXTRA_SDKCONFIG=<fragment> build`
- Battery log [LCD: UNVERIFIED on the board; Paper since D95: builds, UNVERIFIED] (`CONFIG_EPAGER_BATTERY_LOG=y` in the LCD defaults and the Paper defaults, every 5 minutes on the Paper): run on battery, plug USB in, open the monitor (it resets the board); the boot prints the CSV between `batlog: begin` and `batlog: end`; debug console `batlog` / `batlog clear`. Columns `up_s,utc,raw,smoothed,pct,charging,ble,wifi,joins,wakes,drops,awake_ms,awake_max_ms` (a `# columns:` line follows each `# boot` line; `awake_ms` = time with the sleep locks held since boot, `awake_max_ms` the longest stretch of the period); A/B drain tests (PLAN D106; needs the console image: `idf.py -C variants/paper -B build/paper-ab -DEPAGER_EXTRA_SDKCONFIG=$PWD/variants/sdkconfig.devconsole build`; opening the serial port resets the board, so set a switch, close the port, unplug): debug console `ab` (`ab rail on|off`, `ab ble default|MIN MAX LATENCY [TIMEOUT]`, `ab ancs default|off|SECONDS`, `ab tick default|MS` (the BLE host tick, PLAN D110), `ab reset`); every change is a `# ab ...` line in the log, RAM only; the same image has `CONFIG_PM_PROFILING` (PLAN D107): the console `pm` prints the power manager's lock and mode table (over USB the chip never sleeps, so run on battery and read the `# pm sleep_us=... sleep_pct=... ls=... rejected=...` lines of the log, and every 30 minutes the whole table as `# ` lines; delete `build/paper-ab/sdkconfig` after changing the fragment); per-task CPU time (PLAN D108): the console `tasks`, `# tasks:` lines every 30 minutes and a `# loop passes=... busy_ms=... io= ui= policy= core= log= status= climate=` line every 5 minutes in the log and the console `loop` (where the main loop's time goes; run time counters wrap at 71 minutes, so read `# tasks:` tables as differences); the RTC event ring (`CONFIG_EPAGER_EVENT_LOG`) keeps only the last 25 minutes; Pebble-style idle (PLAN D123, builds and host-tested, NOT measured on the board: checklist 86): the idle BLE link is 165-200 ms, latency 4, 6 s (`ab ble 90 120 4 4000` = the link before D123) and the Paper light-sleeps while the panel refreshes (BUSY wakes it); D124: `CONFIG_ESP_PHY_MAC_BB_PD=y` in `variants/sdkconfig.common`; quiet hours save power (PLAN D125, contract 0.17.0, builds and host-tested, NOT run on a board: checklist 87): Bluetooth off in the window with `quiet.radio_off` (default true; `blec_stop`/`blec_resume`), scheduled Wi-Fi waits until 10 minutes before the end; console `quiet on START_MIN END_MIN | off | radio on|off`
- Clock faces [M8a + M8b: host-tested (`test_clock_face`: the vectors, a fuzz run, edge cases; the UI cases of `test_ui`/`test_ui_lcd`; `test_core`, the transfer scenarios), golden scenes `face_*` and `settings_face*`, Python tests `tools/tests/test_clock_face.py`; firmware images build (both variants, signed Paper, console image); NOT run on a board; checklist 85; notes/M8a.md, notes/M8b.md]: the board installs faces over BLE (`asset_support` entry, per-type budget and count in `xfer_asset_support_t`, `check_asset` = `cf_parse` at `XFER_FINISH`, `origin` stored with the version), keeps each face's option values in `opt_<id>` (core), and Settings → Clock face lists, selects, customises and deletes them; `CONFIG_EPAGER_FEATURE_CLOCK_FACES` (on in `variants/sdkconfig.common`); debug console image: `face [list|select ID|opt N V|delete ID]`; the LVGL pool is 128 KB in PSRAM (D122); contract 0.15.0, 0.16.0 adds `CAPABILITIES.builtin_face` (the built-in face ids) (asset type `clock_face`, format `clock_face_v1`, config keys `ui.clock_face_id` / `ui.clock_face_opts` (a new `bytes` key type), the `origin` tag, `contracts/clock-face-gallery.md`); build a face: `cd tools && ../tools/.venv/bin/python -m clock_face build FACE_DIR --board paper|lcd [-o FILE]` (reads `face.json` + PNGs, runs the board's own checks), check a file: `... -m clock_face check FILE --board paper|lcd [--id ID]`; the built-in faces (`components/ui_common/faces/<name>/face.json`, Classic stays in the layouts) are compiled into `components/ui_common/src/ui_face_builtin.c` by `tools/.venv/bin/python tools/clock_face/gen_builtin.py` (`--check` for drift; the seven-segment digits come from `make_builtin_images.py`); fake-board console: `face install FILE [id=ID] [origin=gallery|local]`, `face select [ID]`, `face opt N V`, `face list`, `face delete ID`; vectors: `contracts/test-vectors/clock_faces.json` from `tools/codegen/clockface_vectors.py`; longer fuzz: `EPAGER_FUZZ_ITERS=1000000 build/host/test_clock_face`; the pool size is in `components/ui_common/lv_conf/lv_conf.h` (checklist 85)
- App icons [M6a: builds, host-tested (`test_blob_fs`, `test_app_icon_upload_on_a_file_system`), UNVERIFIED on a board]: both firmware images store `app_icon` assets on the LittleFS partition through `blob_fs` and the UI shows them; an icon is deleted with its app when the app leaves the seen list (D101, contract 0.12.0); no firmware sink or OTA yet; checklist 76; D100
- Firmware OTA [M6b: builds (both variants, signed Paper), host-tested (`test_ota`), UNVERIFIED on a board; checklist 83; D111]: `CONFIG_EPAGER_FEATURE_OTA` (on in `variants/sdkconfig.common`); `components/ota` (`ota_sink.c` portable, `ota_esp.c` esp_ota_*); a new image must pass the self-check in `app.c` (`OTA_CHECK_*`) within 120 s or the board rolls back; push an image with `tools/test_client` flows `ota_dry_run` / `ota_install` (the fake board; over BLE only the dry run is safe on a board, after the go-ahead); an unsigned build accepts any image of its own project, a `-DEPAGER_SIGN=ON` build only signed ones
- Firmware updates over Wi-Fi [M6c: builds (Paper, Paper console image, signed Paper, LCD), host-tested (`test_upd`, `test_ota`, the update cases of `test_ui`/`test_ui_lcd`, golden scenes `update`), run on the Paper (download, `Range` resume, cancel, signature refusal, daily timing: checklist 84), NOT run on the LCD board; docs/wifi-ota-plan.md]: the UI is the Software update page (Settings) and its prompt (`ui_show_update_prompt`); `CONFIG_EPAGER_FEATURE_WIFI_OTA` (on in `variants/sdkconfig.common`), `CONFIG_EPAGER_UPDATE_URL`; config key `update.auto_check` (contract 0.14.0); portable `components/ota/src/upd_*.c`, Wi-Fi side `components/net/src/net_update.c`, glue in `app.c`; drive it from the debug console image (`idf.py -C variants/paper -B build/paper-console -DEPAGER_EXTRA_SDKCONFIG=$PWD/variants/sdkconfig.devconsole build`): `update status|check|install|cancel|later|skip|due|url URL|interval MIN|drop KIB|auto 0|1` (the board needs a saved Wi-Fi network and a clock; `url`, `interval`, `drop` are for testing only; a test build with another version: `-DPROJECT_VER=0.0.9` on the idf.py command line, and another signing key through a fragment with `CONFIG_SECURE_BOOT_SIGNING_KEY`)
- BLE and ANCS [M5: builds, host-tested, UNVERIFIED on a board]: the firmware advertises as `ePager-XXXX` after `app_run`; `CONFIG_EPAGER_BLE_STATS` / `CONFIG_EPAGER_BLE_2M_PHY` (ble_core Kconfig) for the throughput checks; debug console `unbond`; checklist 39-53; decisions D67-D69
- Flashing page [site builder: host-tested; page: opened on the host only; flashing UNVERIFIED on a board, checklist 81]: `python3 tools/web/make_site.py --out build/site --board paper=build/paper --board lcd=build/lcd`, then `python3 -m http.server 8000 --directory build/site`; CI `.github/workflows/pages.yml` (tag `v*` or manual); docs/flashing-page.md
- CI and release [run on GitHub: `v0.1.0` and `v0.1.1` are published in the public repository `tdpi95/epager-firmware`; their images predate the OTA sink, so the first OTA-capable release is flashed over USB; script host-tested]: `.github/workflows/ci.yml` (host tests, contract check, `pytest tools`, unsigned build of both variants, on push to main and PRs); `.github/workflows/release.yml` (tag `v<version>`: signed builds with the secret `EPAGER_SIGNING_KEY`, then a GitHub Release with `manifest.json` + `epager-<board_id>-<version>.bin`, contracts/releases.md; by hand = dry run); assemble the files locally: `tools/.venv/bin/python tools/release/make_release.py --version 0.1.0 --out build/release --image paper=build/paper --image lcd=build/lcd [--allow-unsigned]`
- Flash and monitor [UNVERIFIED, no board yet]: `idf.py -C variants/paper -B build/paper -p /dev/ttyACM0 flash monitor` (docs/recovery.md)

## Coding conventions

- C11 for firmware and portable core; C++ only where a dependency needs it. Prefix public symbols with the component name (`proto_`, `cfg_`, `xfer_`, `npipe_`, `core_`, `ui_`, `board_`, `wx_`, `cf_` (clock faces), `net_`, `bleu_` (BLE helpers), `blec_` (BLE glue), `ancs_` (ANCS state machine), `ancsg_` (ANCS radio side); the simulator `sim_`).
- Portable components (`protocol`, `notification_pipeline`, `config_store`, `transfer`, `core`, `weather`, `clock_face`, and `net/src/net_util.c`, `ble_core/src/ble_util.c`, `ancs_client/src/ancs_sm.c`) must not include any ESP-IDF/FreeRTOS header. Platform needs go through small interfaces: `kv_storage` (config_store), `blob_storage` and `xfer_fw_sink_t` (transfer), clock/battery callbacks. Their in-memory fakes (`kv_mem`, `blob_mem`, `fw_mem`) live next to them, for host tests and the fake board.
- No dynamic allocation in hot paths of the portable core; caller-provided buffers, explicit sizes.
- Shared code has no simulator special-casing: `boards/sim_epaper_154` plugs in through `board_api` only (`tools/tests/test_fake_board.py` greps `components/`, `variants/`, `boards/` for it).
- Every parser bounds-checks and returns a status code from `registry.json`; never trust lengths.
- Features compile out by Kconfig (`CONFIG_EPAGER_*`); variants never duplicate shared code.
- Simple, readable code; a short comment only where the design choice is not obvious.
- Python: 3.12, type hints, pytest. Tools live under `tools/` with `tools/requirements.txt`.

## Do NOT assume (verify or leave a clearly marked `TODO(verify): ...`)

- Waveshare I2C chips: the RTC is a PCF85063A (U8 "PCF85063ATL") and the sensor an SHTC3; drivers in `ws154_i2c.c` follow their datasheets but are unverified on the board (hw-checklist 16-19), including whether the RTC keeps time while the board is off and how much the board warms the sensor.
- Waveshare pinout and e-paper controller: the schematic (read in M4, `boards/waveshare_s3_epaper_154/src/ws154_pins.h`) is the source of truth for pins and polarities; the demo repo (`waveshareteam/ESP32-S3-ePaper-1.54`, `02_Example/ESP-IDF/V2`) has **no licence**: do not copy its code or waveform tables. The controller model (SSD1681-class) and every command value in `ws154_epd.c` are unverified until the controller datasheet is read and the panel is seen working. Pins live in ONE header.
- Battery: divider (x2) and ADC pin come from the schematic; accuracy, the cell's chemistry/capacity and the charge curve are unknown until the battery is known. There is no pin for external power.
- LCD board (ostb-xiaozhi-3st): there is **no schematic**. Pins were read from the stock firmware's code (the community ESPHome port agrees; no licence: facts only, never its code) and the ones P1a uses are verified on the board (hw-checklist 21-28). The LCD driver sends the stock init list (user decision 2026-10-13, D47); the controller model is still in doubt (NV3023 or JD9853): do not "fix" commands from a datasheet without checking on the panel. **Never flash the LCD board (any image, `hw_probe` included) without the user's explicit go-ahead in the current session** (user, 2026-10-13); read-only esptool commands are fine. **No BOOT key** (the power key is a hardware switch, not GPIO0; the case stays closed): USB Serial/JTAG is the only way to reflash. Every image must leave GPIO19/20 alone, keep the USB PHY on and never sleep with USB down; with power management, `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y` (PLAN D50, docs/recovery.md §7; on since D66: LCD has PM and light sleep, the first image with it is **unverified on the board**, hw-checklist 14, 36-38). Sleep = screen off + light sleep, never deep sleep; the power key cannot wake the chip (it is on no GPIO) and the touch chip has no interrupt pin: only the volume keys wake it (D66).
- ANCS details: Apple's ANCS spec and Accessory Design Guidelines, not memory. The connection-parameter and advertising rules in `ble_util.h` come from Apple's QA1931 (read 2026-10-14); NimBLE behaviours the BLE glue relies on (ENC_CHANGE and `sec_state.bonded`, `sm_sc_only`, thread safety of `ble_gatts_notify_custom`, resolving a bonded iPhone's private address) are read from the source, not seen running: checklist 39-53.
- Whether an iOS app can connect alongside the system ANCS link: hardware test.
- Whether the board survives releasing the PWR key before `app_main` takes the power latch (hw-checklist item 8).
- BLE throughput and final firmware size: unknown until measured.
- Library APIs: check headers/docs of the installed version (ESP-IDF 5.5.5, NimBLE in-tree). Never invent APIs. Managed component names are `lvgl__lvgl` and `joltwallet__littlefs` in CMake.
- Never enable irreversible eFuse / secure boot / flash encryption settings without asking the user.

## Contract rules

- `/contracts` changes first (spec, schema, vectors), then code. `protocol.md` is written before implementation.
- Any contract change updates `CONTRACT_VERSION`, `CHANGELOG.md` and test vectors **in the same commit**.
- Additive = minor bump; breaking = major bump. Unknown message types, TLV tags and config keys are skipped, never fatal; newer minor versions are accepted, both directions.
- The C core and the app's Dart implementation must both pass every vector in `contracts/test-vectors/`.
- `contracts/registry.json` is the single source of every protocol number. Generated files (`proto_gen.[ch]`, `contracts/schemas/`, `contracts/reference.md`, `contracts/test-vectors/*.json`) are never hand-edited; `contract_check.py` fails on drift. Vector cases live in `tools/codegen/vectors.py`.

## Privacy and security rules

- Notification content (title, body, app names, app IDs) never appears in logs, crash dumps or diagnostics by default. Use the redacting log macros; content logging is a debug-only Kconfig, off in release.
- Notifications live in RAM only; they are not persisted to flash.
- Bonding with LE Secure Connections encryption is mandatory. Config, notification, asset and OTA writes require an encrypted, bonded link. Assets are data only, never executed.
- Never commit private signing keys. Dev keys are generated locally into a git-ignored path.

## Definition of done (per change / milestone)

- Builds warning-free (`-Wall -Wextra -Werror` for portable code); host tests and contract check pass.
- New behaviour has tests; contract vectors cover valid, invalid and edge cases.
- Hardware-dependent parts are marked `TODO(hw-verify)` and listed in `docs/hw-checklist.md`.
- Docs (`AGENTS.md` commands, `docs/PLAN.md`, `docs/MILESTONES.md`, `docs/milestone-notes/`) updated; nothing stale.

## Working agreement

- One milestone at a time, in order, only after the user approves the plan.
- Commit as you go (small, focused commits). Contract changes follow the contract rules above.
- At the end of each milestone: write `docs/milestone-notes/M<n>.md` (done, verified and how, not verified, decisions/contract changes), link it from `docs/MILESTONES.md`, update the status there, report to the user, then **stop for review**.
- Open questions go in `docs/PLAN.md` § Open questions; ask rather than guess on anything in the do-not-assume list.
