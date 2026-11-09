# ePager firmware: architecture and implementation plan

Original plan approved 2026-10-11. **Status refreshed 2026-10-31** against the source, contract and recorded hardware results; this documentation update does not imply a new build, flash or hardware test. Current contract: **0.13.0**, in [CONTRACT_VERSION](../contracts/CONTRACT_VERSION).

Legend: **[V]** verified in source, a primary document or host tests (the source is named) · **[HW✓]** observed on hardware, with a recorded result · **[S]** vendor/community statement or stock-image clue awaiting confirmation · **[HW]** hardware verification still needed · **[?]** unresolved question.

[Milestones](MILESTONES.md) track delivery and approval; [milestone notes](milestone-notes/) record dated snapshots. For individual hardware checks, use the results in [hw-checklist.md](hw-checklist.md), including later results that supersede an earlier milestone's “not verified” summary.

## 1. Scope

The v1 **phone link is BLE only**. Both products use Wi-Fi for weather, time and a local setup hotspot; the app/firmware protocol is not carried over Wi-Fi. A Wi-Fi transfer path remains an unscheduled v2 option, only when requested.

| Product | Hardware | Current implementation and evidence |
| --- | --- | --- |
| **Paper** (`variants/paper`) | Waveshare ESP32-S3-ePaper-1.54 **V2**, 200 × 200 B/W, no touch; 8 MB flash / 8 MB PSRAM | Shared notification UI, clock, climate, settings, Wi-Fi/weather, BLE/ANCS, asset storage and diagnostics implemented. Running on the board since 2026-10-15; individual display, sensor, network and power checks remain open. |
| **LCD** (`variants/lcd`) | **ostb-xiaozhi-3st**, 296 × 240 landscape colour LCD and CST816S touch; 16 MB flash / 8 MB PSRAM | P1a bring-up hardware-verified 2026-10-13. Product UI, simulator twin, settings, Wi-Fi/weather, BLE/ANCS and app icons implemented; P1b remains in progress. |

The iPhone/iPad route uses the board as an ANCS client without an iOS app. Android uses the Flutter companion app in `epager-app` through the custom GATT service. This repository owns the [app/firmware contract](../contracts/protocol.md).

App-icon transfer is implemented. **The firmware OTA sink, self-check and rollback are implemented (M6b, builds, host-tested) but not yet run on a board**; signed-image rejection, rollback and power-loss behaviour are open (hw-checklist 83).

## 2. Platform and Paper board facts

### Development baseline

- ESP-IDF **5.5.5**, installed at `~/esp/esp-idf`; activate with `. ~/esp/esp-idf/export.sh`. Recorded toolchain: Xtensa GCC 14.2, CMake 3.30.2 and Ninja 1.12.1. Host baseline: GCC 13.3, Python 3.12 with `tools/.venv`, Tk 8.6 and BlueZ 5.72. These are the recorded environment versions, not a fresh machine inventory.
- NimBLE supplies the firmware's BLE host and GATT client/server APIs. ANCS fields and parsers were implemented from Apple's specification in M1 and exercised by shared vectors; reading the ANCS appendix is no longer future work. Phone interoperability remains a separate hardware check.
- Signed application verification without hardware Secure Boot is configured through `variants/sdkconfig.signed`: RSA-3072, with the running application's signature supplying the update trust anchor. This source-reviewed configuration does not establish working firmware OTA. Hardware Secure Boot, flash encryption and anti-rollback eFuses are not enabled by this project and require explicit approval before any irreversible change.

### Waveshare ESP32-S3-ePaper-1.54 V2

Sources: the schematic and PICO-1 datasheet recorded during M4, [vendor documentation notes](waveshare-epaper-154-v2.md), and hardware checklist results. Pins live in `boards/waveshare_s3_epaper_154/src/ws154_pins.h`.

| Fact | Source and verification |
| --- | --- |
| ESP32-S3-PICO-1-N8R8: 8 MB **quad** flash, 8 MB **octal** PSRAM; GPIO33–37 and SPICS1 reserved by the package | [V] PICO-1 datasheet. V1's 4 MB flash / 2 MB PSRAM configuration must not be used for V2. |
| E-paper: DC 10, CS 11, SCK 12, MOSI 13, RST 9, BUSY 8 (high = busy), display rail 6 (active low) | [V] schematic; board has displayed the firmware UI, but controller identity, command values, waveform quality, refresh timings and long-run ghosting remain [HW]. |
| Battery latch 17 (high = connected), BOOT 0, PWR key 18; audio rail 42 (active low) | [V] schematic. [HW✓] Paper stayed powered and BLE-connected for five minutes after USB unplug once the sleep GPIO isolation was fixed (2026-10-15, checklist 69). Startup latch timing and longer runs remain [HW]. |
| I2C SDA 47 / SCL 48; PCF85063A RTC at 0x51, SHTC3 at 0x70, ES8311 at 0x18; RTC interrupt 5 | [V] schematic. [HW✓] All three answered with the audio rail on; the unpowered codec clamps the bus (2026-10-15, checklist 16). Driver powers the rail around accesses. |
| RTC access retries | [HW✓] 2026-10-17: first retry succeeded after a timeout in two boots and time was correct at the next boot (checklist 74). The initial timeout's cause, power-off retention and drift remain open. |
| Battery ADC GPIO4 (ADC1_CH3), 200 K / 200 K divider, VBAT/2 | [V] schematic. ADC accuracy, cell capacity/chemistry and the charge curve remain [HW]. Vendor documentation identifies an MX1.25 2-pin lithium-battery header; its existence is no longer an open question. |
| No VBUS or charger-status GPIO | [V] schematic: ETA6098 STAT drives the orange LED. Firmware estimates charging from battery behaviour; this is not a direct external-power measurement and needs validation. |
| No 32 kHz crystal for the **SoC** | [V] schematic: GPIO15/16 carry I2S; Y2 belongs to the RTC. BLE uses the main XTAL in light sleep. Current consumption remains [HW]. |
| Native USB Serial/JTAG on GPIO19/20, no UART bridge; BOOT and PWR keys, no reset key | [V] schematic; [recovery procedures](recovery.md). |
| Speaker and power-off support | [HW✓] User heard the boot chime and sound tests, and reported power-off working on 2026-10-15. Loudness, current draw and the exact battery/USB power-off cases were not fully recorded. |

The display is SSD1681-class according to available clues; the exact controller and its datasheet remain `TODO(verify)`. Vendor demos have no licence: use hardware facts, never copy their code or waveform tables.

## 2b. LCD board: ostb-xiaozhi-3st

No schematic is available in the recorded board research. The stock flash was backed up and analysed before porting; community pins were cross-checked against it. Detailed facts and the stock display setup are in [ostb-xiaozhi-3st.md](ostb-xiaozhi-3st.md); P1a measurements are in [P1a.md](milestone-notes/P1a.md).

| Fact | Source and verification |
| --- | --- |
| ESP32-S3 QFN56 rev v0.2, 40 MHz crystal, 8 MB octal PSRAM in package, 16 MB Boya quad flash (JEDEC 0x68 / 0x4018) | [V] esptool and stock boot log. [HW✓] QIO flash and 80 MHz PSRAM booted cleanly; GPIO33–37 are reserved for PSRAM. |
| Secure Boot and flash encryption disabled, no eFuse key blocks used | [V] read-only security probe on 2026-10-13; not a fresh security-state reading. |
| Native USB Serial/JTAG (`303a:1001`); no BOOT key; case stays closed | [HW✓] USB reflashing worked without a key, including with our image running. The power key is not GPIO0. USB is the accessible recovery path. |
| LCD pins: SCLK 9, MOSI 10, CS 14, DC 8, RST 18 active low; backlight 13 active high, 25 kHz PWM | [S] stock firmware; [HW✓] patterns, orientation, colour order and backlight tested on 2026-10-13. Pins live in `xz3_pins.h`. |
| 296 × 240 landscape RGB565; stock setup uses SPI3 mode 0, 40 MHz, swap XY / mirror Y and a 24-pixel offset | [S] stock image. [HW✓] Our own driver with the stock init list passed image tests and 20 clean resets. Current SPI clock is **80 MHz**: about 15 ms per full frame; extended heat/stability tests remain [HW]. |
| LCD controller identity | [?] Stock driver is named NV3023, but the init list begins `DF 98 53`, a JD9853-family clue. Do not relabel the controller as confirmed or change working commands from an unverified datasheet. |
| CST816S at I2C 0x15; SDA 12 / SCL 11, polled, no reset or interrupt pin used | [HW✓] Touch coordinates and calibration tested on the panel. Long-idle behaviour remains [HW]. |
| Volume up 40 / down 39, active low; power key on no GPIO | [HW✓] Volume-key events and power-key off/on behaviour on battery and USB tested. The power key is a hardware switch; firmware cannot use it as a wake GPIO. |
| Battery ADC GPIO17 (ADC2 channel 6); charge status GPIO47, low = charging | [S] stock pin map; [HW✓] charging polarity and rising/falling level observed (checklist 27). Vendor level tables cause a jump when changing between charging and battery tables. Divider, capacity, calibration and full discharge remain open. |
| ES8311 0x18 and ES7210 0x40; I2S MCLK 5, BCLK 15, WS 16, DOUT 6, DIN 7; amplifier enable 4 | [S] stock image; [HW✓] both I2C addresses answered. Audio operation is not established by that test. |
| No RTC or climate sensor discovered; SoC 32 kHz crystal presence unknown | [HW✓] Recorded I2C scan found only 0x15, 0x18 and 0x40. Firmware uses the main XTAL for BLE low-power timing; do not infer a crystal or additional sensors. |

Stock firmware was “Xiaozhi ESP32 Mod” 2.4.2.1, with two 5376 KiB app slots and SPIFFS assets. Those are **stock** partitions; ePager uses §5b. A similar retail kit's 300 mAh listing is not evidence for this unit's battery capacity.

## 3. Architecture

```text
 iPhone/iPad: ANCS GATT server             Android app: custom GATT client
                 |                                      |
      ancs_client: GATT client                ble_core: GATT server
                 |                                      |
                 +---------- NimBLE event queues --------+
                                      |
                         shared app task (components/app)
                                      |
                    core dispatcher + portable components
                   /                  |                  \
     notification_pipeline      config_store          transfer
       RAM notifications         NVS adapter       blob_fs -> LittleFS
                   \                  |                  /
                            callbacks in the app task
                                      |
                          ui_common (LVGL 9.5.0)
                                      |
                  board_api -> selected board's private drivers

 net task: Wi-Fi / HTTPS / SNTP / setup hotspot
     + portable weather parsers and scheduler
     -> queued status/readings applied by the app task

 + components/ota: esp_ota_* sink (ota_sink.c portable), self-check, mark-valid / rollback (M6b, D111)

 clock faces (M8a-b, D113-D122): asset file -> clock_face (portable: cf_parse, cf_opts, cf_eval)
     -> ui_common/ui_face.c (LVGL objects; Classic = the layouts' own idle screen is the fallback)
```

- **Portable C11:** protocol, notification pipeline, config store, transfer, core, weather and clock_face (the clock face parser and evaluator, M8a), plus portable helpers in net, BLE and ANCS. No ESP-IDF/FreeRTOS includes in these parts; platform needs use storage, firmware-sink and clock/battery interfaces. Caller-owned buffers and explicit lengths keep parser and notification hot paths bounded.
- **Threading:** the NimBLE host queues events; one app task owns the core, ANCS state machine and LVGL UI, services keys/radio/network events, and performs core storage operations. There is no separate UI task consuming a notification-event queue. The Wi-Fi task fetches data independently and reports results for the app task to apply. See [app.c](../components/app/src/app.c).
- **Notification model:** both sources become `npipe_notif_t` callbacks with a board-local id, source, added/modified/removed kind, category, priority, flags, app id/name, title/subtitle/body and UTC timestamp. The UI does not branch on phone source. Notifications stay in RAM; seen apps and policies are persisted separately.
- **Board selection:** `variants/common.cmake` selects one board with `-DEPAGER_BOARD=<folder>`. `board.cmake` names its product class and target chip. Settings layer common → chip → variant → board → optional signing / extra fragments. Use a fresh build directory for a new configuration; an existing `sdkconfig` retains old values.
- **Feature gating:** Kconfig controls touch, animation, Wi-Fi/weather and diagnostics. Shared code does not name individual boards. Host simulator boards plug into the same interface; no simulator special-casing in firmware components.

The current [board API](../components/board_api/include/epager/board_api.h) provides:

| Interface | Responsibility |
| --- | --- |
| `board_profile_t` | Board id, flash size, features, low-battery OTA threshold, early init; optional battery/diagnostic, button, rail, sleep/wake, RTC, climate, touch, power-off and audio callbacks. Pins stay private to the board. |
| E-paper display path | Whole mono frames, asynchronous `refresh` / `busy`, refresh policy, sleep/wake. UI owns the frame conversion and LVGL flush. |
| LCD display path | `write_area` copies RGB565 pixels in CPU byte order; driver handles byte swaps/DMA. Backlight and rotation callbacks are optional. Touch coordinates follow the display orientation. |

## 4. Implementation status and remaining delivery

| Area | Status |
| --- | --- |
| Contracts, portable core and simulator (M1–M3) | Implemented, with recorded host tests, shared vectors, sanitizer checks and golden images. Current contract is 0.15.0 (clock faces, M8a). |
| Firmware and idle UI (M4/M4b), LCD bring-up (P1a) | Implemented. Both boards have run firmware; hardware coverage is per checklist item, not a blanket pass. |
| BLE and ANCS (M5) | Firmware glue and portable state machine implemented and host-tested. Android pairing/reconnect/encrypted communication and iPad pairing/service discovery have partial hardware evidence; complete interoperability remains open (§9). |
| LCD product UI (P1b) | Implemented UI, simulator, settings and weather; milestone remains in progress pending its remaining acceptance checks and review. |
| Assets (M6 asset half) | LittleFS app-icon storage and UI integration implemented. LCD upload/drawing verified after the open-file-size fix on 2026-10-17. Other device cases remain open. |
| Firmware OTA (M6b) | `components/ota` sink on the inactive slot, self-check (UI, BLE, 30 s running; 120 s timeout), mark-valid and rollback wired in `app.c`; builds, host-tested on a fake flash ([notes](milestone-notes/M6b.md)). Signature rejection, rollback, resume after power loss and bonds/assets surviving an update still need a board (checklist 83). |
| Clock faces (M8) | **M8a done on the host (2026-11-04, [notes](milestone-notes/M8a.md)):** contract 0.15.0, `components/clock_face` (parser, options, evaluator; vectors, fuzz, 96 % coverage), the renderer `ui_face.c`, built-in faces (Big, Minimal on the Paper; Big, Night on the LCD), fake-board commands and golden images. Nothing is installed on a board yet: `asset_support`, budgets, the Settings pages and the hardware checks are M8b; the LVGL pool is too small for a face on the Paper (found in M8a, open for M8b). |
| Hardware completion (M7) | Remaining checklist work includes throughput, power/current measurements, full phone interoperability and device update tests. |

Continue one approved milestone at a time. A documentation refresh does not close an acceptance criterion, approve a flash or start a new milestone.

## 5. Partition table (Paper, 8 MB)

Source: [boards/waveshare_s3_epaper_154/partitions.csv](../boards/waveshare_s3_epaper_154/partitions.csv), established in M4. Partition/slot changes require a USB reflash.

| Name | Type/Subtype | Offset | Size |
| --- | --- | --- | --- |
| bootloader region | — | 0x0 | 32 KiB reserved; actual binary size is build-dependent |
| partition table | — | 0x8000 | 4 KiB |
| nvs | data/nvs | 0x9000 | 64 KiB (bonds, config, seen apps, transfer journal) |
| otadata | data/ota | 0x19000 | 8 KiB |
| phy_init | data/phy | 0x1B000 | 4 KiB |
| ota_0 | app/ota_0 | 0x20000 | **3 MiB** |
| ota_1 | app/ota_1 | 0x320000 | **3 MiB** |
| assets | data/littlefs | 0x620000 | **1920 KiB**, ends at 8 MiB |

The 1.4× slot/image rule permits an image of about **2.14 MiB**. The assets partition has 1,966,080 bytes total; free space depends on stored icons and diagnostics. Notifications are not stored there.

The M4 image sizes and RAM figures are historical, preserved in [M4.md](milestone-notes/M4.md). The later **2026-10-17 M6a build** recorded Paper at **1.42 MiB** and LCD at **2.67 MiB**, both warning-free ([M6a.md](milestone-notes/M6a.md)). These are dated measurements, not sizes of a fresh build; the earlier estimate of a final 1.0 MB Paper image is superseded. Re-measure unsigned/signed images, slot headroom, internal heap and stack for the release configuration, including the eventual firmware sink.

## 5b. Partition table (LCD, 16 MB)

Source: [boards/ostb_xiaozhi_3st/partitions.csv](../boards/ostb_xiaozhi_3st/partitions.csv), used since P1a; provisional until the first release. Later layout changes require a USB reflash.

| Name     | Type/Subtype  | Offset   | Size                         |
| -------- | ------------- | -------- | ---------------------------- |
| nvs      | data/nvs      | 0x9000   | 64 KiB                       |
| otadata  | data/ota      | 0x19000  | 8 KiB                        |
| phy_init | data/phy      | 0x1B000  | 4 KiB                        |
| ota_0    | app/ota_0     | 0x20000  | **4 MiB**                    |
| ota_1    | app/ota_1     | 0x420000 | **4 MiB**                    |
| assets   | data/littlefs | 0x820000 | **8064 KiB**, ends at 16 MiB |

The 1.4× rule permits an image of about **2.86 MiB**. Assets total 8,257,536 bytes; current free space must be read from the board. Stock and ePager data partitions overlap differently: read [recovery.md §7](recovery.md#7-lcd-ostb-xiaozhi-3st) before switching firmware.

## 6. Repository and tooling

- Git is initialized; build outputs, Python environment, downloaded dependencies and private signing keys are ignored. Never commit stock dumps, credentials or notification content.
- `contracts/` contains the specification, registry, GATT layout, generated schemas/reference, flat `test-vectors/*.json` files, contract version/changelog, compatibility and release-manifest specifications. Generators live in `tools/codegen/`; `tools/contract_check.py` checks drift and portability. Firmware and Dart must pass shared vectors; this review does not establish the companion app's latest vector result.
- `tests/host/` builds the portable components and simulated UI with Unity, ASan/UBSan by default; `tools/tests/` covers Python tooling, TCP flows and golden screens. See [AGENTS.md](../AGENTS.md) for verified commands and [tools/README.md](../tools/README.md) for the simulator and BLE client. The laptop client/BlueZ agent still needs its documented real-board checks.
- `tools/assets/` generates fonts and built-in icons. LVGL and source-font downloads are pinned under `.deps/`. ESP-IDF dependency locks are kept with the variants.
- `components/power/` wraps ESP power management and locks; the app/board implement the sleep policy. `components/ota/` holds the firmware OTA sink and boot-side calls (M6b); there is no `tools/sign/` wrapper. Signing uses ESP-IDF/`espsecure.py` and `variants/sdkconfig.signed`; keys stay in git-ignored `keys/`.
- CI, release and Pages workflow files exist under `.github/workflows/`. `tools/release/make_release.py` assembles app release files; `tools/web/make_site.py` builds the [browser flashing page](flashing-page.md). Their presence and local tests do not establish a successful GitHub deployment or browser flash on hardware.

## 7. Confirmed requirements and open questions

| Requirement | Current choice |
| --- | --- |
| Product/board identity | Paper on Waveshare V2; LCD on ostb-xiaozhi-3st. Both are in hand and have run ePager. Advertised name and setup hotspot: `ePager-XXXX` (contract 0.13.0). |
| Pairing and bonds | Displayed six-digit passkey, LE Secure Connections with MITM and bonding; three bonds, one active phone connection; fourth bond replaces oldest. New/repeat pairing is gated by pairing mode. Mode times out after three minutes, paused during passkey entry/setup. Full security/bond-limit tests remain open. |
| Persistence and privacy | Notification history in RAM only; config, bonds and seen apps in NVS; app icons and diagnostics in LittleFS. Notification content is excluded from firmware logs by default. |
| UI languages | English and Vietnamese, with generated fonts and emoji support. |
| Assets | App icons only are advertised by firmware: Paper 24 × 24 `mono1` (max 88 B), LCD 32 × 32 `rgb565a8` (max 3088 B). Forgetting/evicting an app removes its icon. Backgrounds/themes/fonts are not advertised as supported uploads. |
| Clock faces | **M8 approved 2026-11-04 (D113); M8a (contract 0.15.0, parser, renderer, built-in faces) and M8b (install, choose, customise on the board: transfer limits and check, option files, Settings pages, the 128 KB PSRAM pool; D119-D122) done on the host and in the builds, not run on a board; M8c (tools, gallery) not started: [clock-faces-plan.md](clock-faces-plan.md), [M8a](milestone-notes/M8a.md), [M8b](milestone-notes/M8b.md).** Data-only `clock_face` assets (layout, image digit sets, per-face options) installed over BLE by the phone apps, from a GitHub gallery or a file; Classic (today's idle screen) stays as the fallback. An iOS app is planned later (user, 2026-11-04). |
| OTA direction | **Updates over Wi-Fi from the public GitHub Releases are required (user, 2026-11-03: there is no iOS app); implemented and run on the Paper: [wifi-ota-plan.md](wifi-ota-plan.md), [M6c](milestone-notes/M6c.md).** BLE OTA stays for the Android app. Signed application updates without hardware Secure Boot; downgrade refusal and a 30% low-battery threshold are intended. Portable policies and the device-side integration (M6b, D111) exist; both are unverified on a board. |

### 7b. LCD recovery and power constraints

- **Never flash any LCD image, including a probe or stock restore, without explicit user go-ahead in the current session.** Read-only esptool discovery is allowed; it can reset the board. Back up stock flash before the first write and keep a private copy outside `build/`.
- The case stays closed. The power key is a hardware switch and not BOOT/GPIO0. Do not assume access to GPIO0 as a recovery plan.
- Keep GPIO19/20 and the USB PHY intact. Preserve `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`, so native USB remains usable while connected to a host. Detailed procedures are in [recovery.md §7](recovery.md#7-lcd-ostb-xiaozhi-3st).
- LCD sleep means screen off plus **light sleep**, never deep sleep. Only volume keys wake the chip: the power key is not a GPIO and touch has no interrupt pin. The configured policy exists; complete wake latency/current tests remain open.
- Drivers are our own implementation using the approved stock init list. Controller identity is still uncertain; do not change working commands on the assumption that the stock driver's name identifies the chip.

### 7c. Current UI and controls

These describe the implemented code and defaults; hardware acceptance remains per checklist item. Sources: [ui.c](../components/ui_common/src/ui.c), [colour layout](../components/ui_common/src/ui_layout_colour.c), [settings](../components/ui_common/src/ui_settings.c) and [registry](../contracts/registry.json).

| Area | Current behaviour |
| --- | --- |
| LCD orientation/look | Landscape 296 × 240; 0°/180° rotation with matching touch coordinates; light background, dark text and category colour accents. |
| LCD keys | Up/down short = previous/next card (from clock, first opens cards); long = brightness. Both released together = sleep now; both held three seconds = pairing. A key waking the screen does not also navigate. |
| LCD touch | On cards: left/right = next/previous, down = dismiss, up = hide cards/show clock, tap = expand/collapse text; long text scrolls. On clock: down = cards, up = settings. Settings: tap selects, up/down pages, right goes back. |
| LCD idle defaults | Brightness 70%; dim after 30 s to at most 20%; panel/backlight off after **60 s**. Stored settings survive default changes. |
| Paper keys | On clock: short BOOT = cards, short PWR = settings. On cards: BOOT = older, PWR = newer, past the ends = clock. Settings: BOOT/PWR short = up/down, BOOT long = select, PWR long = back. PWR held 5 s powers off; BOOT+PWR held 1.5 s opens pairing. |
| Shared settings/screens | Clock format, units, language, message visibility, number masking, rotation, weather, pairing/Wi-Fi setup, saved phones/networks and About. Cards/settings return to clock after a minute of inactivity. |
| Paper refresh policy | Default `display.full_refresh_every` = **30**. Clock partials count as one, other partials as a quarter. A due full refresh waits for a quiet moment, with a limit of twice the configured count. The earlier “every N partials” description is incomplete. |

### 7d. Weather, Wi-Fi and time

Both boards implement weather through Open-Meteo, IP location through ipinfo when no city is configured, and SNTP time. The idle screen shows conditions, current temperature, today's high/low, rain chance and place; Paper can also show its local climate sensor. Phone time sync takes precedence for 24 hours. Weather/network parsing and scheduling are portable and host-tested; complete per-board network/radio tests remain open.

Wi-Fi credentials can be set by the app or a local setup hotspot opened from pairing (LCD button; short BOOT on Paper). Up to three saved network profiles are supported. Default `weather.interval_min` is **60** (range 15–240); stored settings are retained. Wi-Fi powers down between fetches unless setup is open. Credentials are stored in unencrypted NVS and must never appear in logs or committed backups.

**Before commercial release:** verify current provider terms, commercial eligibility, quotas and attribution requirements for both Open-Meteo and ipinfo. Earlier research is not a current provider-terms review.

## 8. Risks and mitigations

| Risk | Current mitigation / remaining work |
| --- | --- |
| Phone-specific BLE/ANCS behaviour, consent, private-address reconnect and service changes | Portable state machine, source-reviewed NimBLE glue and partial radio tests exist. Complete checklist 39–53, especially iOS discovery fixes, reconnect/replay/dismiss and the laptop client; do not infer full interoperability from successful pairing. |
| BLE transfer throughput and long-run stability | Measure per phone and negotiated parameters; resume is modelled/tested, but device transfer interruption and end-to-end firmware OTA still need validation. |
| Internal RAM and OTA-slot headroom | PSRAM holds Wi-Fi/lwIP buffers; features compile out. Re-measure heap/stack and unsigned/signed image sizes in the release config; dated bring-up memory figures are not product headroom guarantees. |
| Sleep current, latch behaviour and battery estimates | Main-XTAL BLE clock, sleep locks and GPIO isolation fixes are implemented. Five-minute Paper unplug test passed; overnight drain, startup latch timing, calibration and current remain open. Battery/event logs and A/B/PM/task diagnostics support measurement. **D123 (2026-11-09, after the Paper lasted about 1.5 days; modelled on the Pebble Classic's idle rules in Google's PebbleOS source):** the idle BLE parameters are 165-200 ms, latency 4, 6 s (the board listens about once a second instead of twice; a phone's message waits up to 1 s), and the chip light-sleeps while the e-paper runs its waveform: the locks are held for the SPI transfer only, BUSY falling wakes the loop (`board_display_t.idle_wake`, `ui_panel_wait`), the panel's control pins keep their levels in light sleep while its rail is on. Neither is measured on the board yet (checklist 86). The main-XTAL floor stays: a 32 kHz clock for the SoC needs a hardware change (GPIO15 carries I2S). |
| Paper I2C rail interaction and RTC timeout | Power audio rail around bus accesses; retries recover the recorded RTC write timeout. Determine root cause and check retention, sensor accuracy and rail/current cost. |
| E-paper controller uncertainty and ghosting | Datasheet confirmation and long runs on the panel remain necessary; weighted full-refresh policy limits flashing but still needs visual/current acceptance. |
| LCD USB-only recovery | Preserve USB pins/PHY and host-connected sleep guard; no deep sleep, no assumed BOOT key, no flash without the current session's go-ahead. |
| LCD init/80 MHz stability and touch idle | Image, backlight, touch calibration and 20-reset tests passed. Keep stock-command provenance; check extended heat/runs and touch after a long idle. |
| Signing keys, rollback and power loss | **The GitHub release is signed with the same key as `keys/dev_signing_key.pem` (found 2026-11-03: a dev-key build accepted the release image, a build with another key refused it): that one key is what every board trusts, and it cannot change without a USB reflash of each board.** Back up private keys outside the repo; run the negative tests of checklist 83 on a board before claiming OTA support. No irreversible security changes without approval. |
| Unencrypted Wi-Fi credentials or notification leakage | Backups/logs stay private; content logging off in release, notifications RAM-only. Hardware log review remains a checklist item. Flash/NVS encryption is not silently enabled. |
| Contract drift or unavailable build dependencies | Shared vectors/codegen/contract checks; commit dependency locks and use pinned downloads. Confirm companion-app vector results separately. |

## 9. Hardware evidence and remaining verification

[hw-checklist.md](hw-checklist.md) is the detailed record; an empty/not-run result remains open even when the code builds. Do not erase dated evidence or promote partial results to complete passes.

**Recorded successes/partial results:**

- **LCD, 2026-10-13/14:** display patterns, 20 resets, 80 MHz SPI, backlight, calibrated touch, volume keys, hardware power key, PSRAM/storage boot and USB reflashing passed P1a checks (21–28). Charging polarity and a short battery log were observed; full battery characterization is incomplete.
- **BLE, 2026-10-14:** Android/nRF Connect paired, rejected a wrong passkey, reconnected using a persisted bond, read Capabilities and exchanged encrypted traffic with the LCD screen off. Advertising intervals and the then-current Android connection parameters were recorded (39–41, 47, 49–50, 52). Later policy changes need their own verification.
- **iPad, 2026-10-14:** listed the board, paired and exposed ANCS; descriptor discovery and a competing connection-update crash required fixes. The checklist does not record a complete post-fix ANCS rerun (44, 50).
- **Paper, 2026-10-15/17:** I2C devices answered with the audio rail on; after the sleep-isolation fix the phone link survived five minutes unplugged; power-off and audio were reported working; RTC writes succeeded on retry and time survived a subsequent boot (16, 69–70, boot-chime entry 71, 74).
- **LCD icons, 2026-10-17:** the companion app uploaded an icon and the display drew it after `blob_fs` stopped relying on LittleFS `fstat()` for an unsynced open file (76; [M6a update](milestone-notes/M6a.md)).

**Still needed:**

| Area | Remaining checks |
| --- | --- |
| BLE/phones | Throughput and latency per phone; raw advertising/filtering and access-control checks; complete ANCS consent/subscription/replay/removal/action flows, iOS private-address reconnect, fourth-bond replacement and pairing timeout; laptop/BlueZ agent and fake-board BLE checks; content-free logs. |
| Displays and controls | E-paper controller/commands, refresh timings and ghosting; Paper rotation/settings/key behaviour on current firmware; LCD settings/gestures, wake latency, long-idle touch and extended 80 MHz operation. |
| Power and sensors | Startup power latch; battery calibration/capacity/full-discharge and charging inference; overnight BLE/current/drain and PM/A/B measurements; RTC off-state retention/drift and the remaining bus timeout; SHTC3 accuracy/self-heating; LCD SoC low-power crystal question. |
| Network/time | Paper Wi-Fi/weather/setup acceptance, BLE/Wi-Fi coexistence on both products, profile fallback/eviction, reconnect optimizations and time-source precedence on hardware. |
| Assets | Icon persistence across reset/power loss, interrupted-transfer resume, full-storage handling, deletion/eviction on current firmware and Paper uploads/display. |
| Release/update | Actual release-config sizes and runtime headroom; signed boot/update rejection, install/self-check/rollback and power interruption after the firmware sink exists; deployment and browser flashing on real boards. |

The next implementation or hardware session must follow the approved milestone and the explicit flash/security constraints in [AGENTS.md](../AGENTS.md). This status refresh authorizes no board writes.
