# Milestones

**Status refreshed 2026-10-31** from the source, [current plan](PLAN.md), dated milestone notes and [hardware checklist](hw-checklist.md). This review adds no build, flash, hardware result or milestone approval. Current contract: **0.13.0** ([version](../contracts/CONTRACT_VERSION), [changelog](../contracts/CHANGELOG.md)). Contract numbers in milestone titles below describe when a feature was introduced.

Status values: `planned` · `in progress` · `done (host-verified)` · `done (hw-verified)` · `blocked` · `dropped` · `not scheduled`. Parent milestones can remain in progress while a delivered subpart has its own notes.

Work on one approved milestone at a time. Each completed milestone has notes recording delivery, verification, remaining checks and contract changes, then stops for review. A host-verified implementation does not imply complete hardware acceptance or owner sign-off. Previously recorded pending reviews for M4, M4b and P1a remain pending; this refresh does not infer approval from later work.

| ID | Title | Status and remaining acceptance | Evidence |
| --- | --- | --- | --- |
| M0 | Plan, AGENTS.md, milestones | done; approved 2026-10-11 | [Notes](milestone-notes/M0.md) |
| M1 | Contracts + portable protocol library + PC vectors | done (host-verified); approved 2026-10-11 | [Notes](milestone-notes/M1.md) |
| M2 | Board-independent logic with host tests | done (host-verified); approved 2026-10-11 | [Notes](milestone-notes/M2.md) |
| M3 | Fake board + screen simulator | done (host-verified); phone/laptop BLE checks remain open; M4 proceeded at the user's request | [Notes](milestone-notes/M3.md) |
| M4 | Paper ESP-IDF project, partitions, size report | done (host-verified), awaiting review; Paper subsequently ran on hardware, with individual checks still open | [Notes](milestone-notes/M4.md), checklist 7–14 and later Paper results |
| M4b | Paper idle clock, temperature and humidity (0.6.0) | done (host-verified), awaiting review; I2C/RTC have partial hardware results, sensor accuracy/retention remain open | [Notes](milestone-notes/M4b.md), checklist 16–20, 74 |
| M5 | BLE custom service + ANCS client | done (host-verified); partial radio verification; full phone acceptance remains open | [Notes](milestone-notes/M5.md), checklist 39–53, 71 |
| M6 | Transfer + firmware OTA, signing, rollback, battery checks | in progress: M6a delivered; firmware OTA integration and failure-path acceptance planned | [M6a notes](milestone-notes/M6a.md), §M6 below |
| M6a | LittleFS asset sink + app icons | done (host-verified); LCD upload/drawing observed 2026-10-17; remaining device cases open | [Notes and updates](milestone-notes/M6a.md), checklist 76 |
| M7 | Complete hardware verification | planned for completion; bring-up and later sessions have already recorded partial evidence | [Checklist](hw-checklist.md) |
| P1a | LCD board bring-up | done (hw-verified) 2026-10-13, awaiting review; battery characterization and long-idle touch carried forward | [Notes](milestone-notes/P1a.md), checklist 21–28 |
| P1b | LCD colour UI, simulator, controls, weather and settings (0.8.0/0.9.0) | in progress; approved 2026-10-14; implemented features still need remaining hardware acceptance and closing notes | §P1b below, checklist 14, 27, 29–38, 54–55 |
| v2 | Wi-Fi transfer fast path | not scheduled; only when requested | §v2 below |

Milestone notes are dated snapshots. Later checklist results can supersede their original “not verified” sections; neither a historical pass nor an old binary size establishes the state of every later firmware image. No P1b closing note exists yet.

## M1: Contracts + portable protocol library + PC tests

**Goal:** a shared app/firmware contract and a portable C library that passes its vectors.

**Status:** delivered and host-verified; approved 2026-10-11. [M1 notes](milestone-notes/M1.md) contain the original verification results. The initial contract was 0.1.0; the current contract is 0.13.0.

**Delivered:**

- Specification written before implementation: envelope/segmentation, messages, GATT, security, status/versioning rules, unknown-field handling, limits, text rules and ANCS mapping.
- Registry, GATT layout, generated schemas/reference and shared valid/invalid/edge-case vectors. Transfer scenario vectors were added with M2.
- `components/protocol`: bounded TLV codec, framing/reassembly, CRC32, UTF-8 normalization/truncation and ANCS parsing; generated `proto_gen.h` / `proto_gen.c`.
- `tools/codegen/`, `tools/contract_check.py` and Unity vector/unit/fuzz runners with ASan/UBSan.

**Acceptance and maintenance:** every applicable vector passes, generated files match the registry, malformed input is bounded, fuzzing has no sanitizer findings, and portable code builds warning-free without platform headers. Contract changes update specification, version, changelog and vectors together. Firmware and Dart must pass the shared vectors; this review does not establish the companion app's latest vector result.

## M2: Board-independent logic with host tests

**Goal:** notification/configuration/transfer policy can run and be tested on the host.

**Status:** delivered and host-verified; approved 2026-10-11. [M2 notes](milestone-notes/M2.md).

**Delivered:**

- Notification normalization, filters/quiet hours, RAM history, add/modify/remove events, ANCS session reset, seen-app tracking and policies.
- Typed config validation, batch commit/revision semantics, defaults/migration and `kv_storage` with an in-memory implementation.
- Transfer state machine, atomic asset replacement through `blob_storage`, firmware-sink interface, CRC/SHA checks and resume journal; transfer vectors in `contracts/test-vectors/transfer.json`.
- Portable OTA policy/model: version/downgrade and battery checks, progress and self-check/rollback states, with in-memory firmware/storage fakes.

**Acceptance and maintenance:** cover midnight boundaries, invalid/unknown config, partial-batch rejection, simulated disconnect/reboot/resume, wrong offsets, oversize/corruption, abort, atomic installation and OTA refusals. Portable-core line coverage target remains ≥85%, reported by `tools/coverage.py`. A simulated OTA pass does not establish an ESP-IDF firmware sink or device rollback.

## M3: Fake board + screen simulator

**Goal:** exercise the real portable core and UI without a physical board, including end-to-end client flows.

**Status:** delivered and host-verified. [M3 notes](milestone-notes/M3.md); later LCD support is part of P1b.

**Delivered/current tooling:**

- `components/core` dispatcher, `board_api`, LVGL **9.5.0** UI integration and host-only `boards/sim_epaper_154`. Host LVGL comes from the pinned archive in `tests/host/lvgl.cmake`; the firmware pins the same version.
- `libepager_core.so` and `libepager_sim.so` loaded through ctypes; TCP transport and optional BlueZ/bless BLE peripheral; console injection for Android/ANCS notifications, battery, time/climate, storage/transfer failures and simulated OTA states.
- Shared notification cards with category/uploaded app icons, idle and pairing screens. The initial placeholder UI has since been extended; current controls/defaults are in [PLAN §7c](PLAN.md#7c-current-ui-and-controls).
- Tk viewer (requires Tk), keyboard/mouse controls and headless screenshots through `tools/fake_board snapshot OUT.png`. `tools/test_client` supplies TCP/BLE flows and benchmarks; tools and examples are in [tools/README.md](../tools/README.md).
- Deterministic golden PNGs under `tests/golden/<variant>/<WxH>/`; explicit `--update-golden` regeneration.

**Acceptance and maintenance:**

- Client flows over TCP cover success/failure paths; notifications and icon replacement/deletion render through the real core/UI, including Vietnamese, emoji and truncation.
- Simulated refresh delay/ghosting are test controls, not real panel measurements. Current Paper full-refresh cadence uses the weighted policy in PLAN §7c.
- Golden images remain deterministic. Shared components use `board_api` without simulator special-casing; the tooling checks this boundary.

**Remaining hardware checks:** fake-board advertisement/Capabilities access from a phone, BlueZ MTU/device options and encrypted pairing/agent behaviour (checklist 2–4), laptop client against a real board, and simulator timing/ghosting fidelity. Do not infer a Flutter-app TCP pass from the test-client flows.

## M4: ESP-IDF project for Paper

**Goal:** build the Paper firmware warning-free with its real board configuration, partitions and measured image headroom.

**Status:** delivered and host-verified, awaiting review. [M4 notes](milestone-notes/M4.md) record unsigned/signed builds and the original size report. Paper has since run on hardware; that earlier “no board” snapshot is no longer the overall status.

**Delivered/current layout:**

- `variants/paper/` identity/project, shared build and sdkconfig layers under `variants/`, signing overlay `variants/sdkconfig.signed`, and board-specific configuration/partitions under `boards/waveshare_s3_epaper_154/`.
- Board driver/pin map, early power latch/rails, display refresh path, battery reader, shared UI/LVGL, power-management wrapper and USB console/recovery configuration.
- Signed-build support without hardware Secure Boot, feature-gating/LVGL-config checks, [adding-a-board guide](adding-a-board.md) and [recovery guide](recovery.md).

**Acceptance and maintenance:** warning-free firmware and portable builds; disabled features/layouts absent from the image; image fits its slot with ≥1.4× headroom; record unsigned/signed image size and runtime heap/stack on the release configuration. Paper has two 3 MiB app slots and 1920 KiB of assets, sourced from its board partition CSV.

**Evidence/remaining work:** Paper operation, a five-minute unplug/BLE test and reported power-off/audio are recorded in checklist 69–70 and the boot-chime entry 71. Controller/command confirmation, refresh quality, startup latch timing, calibration and release-config resource measurements remain open. M4 sizes are historical; the later 2026-10-17 M6a builds recorded Paper **1.42 MiB** and LCD **2.67 MiB**, not fresh measurements of current source.

## M4b: Clock, temperature and humidity

**Goal:** Paper shows clock/date and local climate when idle, with RTC-backed time and configurable 12/24-hour and °C/°F display.

**Status:** delivered and host-verified, awaiting review. [M4b notes](milestone-notes/M4b.md). Introduced contract 0.6.0; weather/time over Wi-Fi and later UI changes are now also implemented.

**Delivered:** config keys, optional climate/RTC board hooks, host-tested calendar/PCF85063/SHTC3 helpers, RTC boot/time-sync integration, fonts, simulator controls and golden scenes. Current firmware wiring is shared in `components/app`, not a Paper-only `main.c`.

**Acceptance and maintenance:** only redraw when displayed minute/rounded readings change; simulated RTC retains time over reboot and loses it when reset; host/Python/contract/coverage and firmware checks remain green. Real sensor and retention tests remain independent of simulation.

**Recorded hardware progress:**

- 2026-10-15: RTC, sensor and codec answered with the audio rail on; with it off, the codec clamped the shared I2C bus. Driver now enables the rail for accesses (checklist 16).
- 2026-10-17: an RTC write timeout recovered on the first retry in two boots; time was correct at the next boot (74).

**Remaining:** timeout root cause, RTC power-off retention/drift/load-capacitance choice, SHTC3 accuracy/self-heating, rail energy cost, and clock/sensor legibility, ghosting and current on current firmware (16–20, 61, 66, 74). Time can come from phone sync, RTC or SNTP; an iOS Current Time Service client remains deferred, not a completed M5 feature.

## M5: Real BLE (custom service, then ANCS)

**Goal:** phones pair securely, read capabilities, change config and deliver notifications; iOS notifications arrive through ANCS without an app.

**Status:** implementation delivered and host-verified; **hardware acceptance remains partial**. Approved 2026-10-14. [M5 notes](milestone-notes/M5.md) record the initial implementation; later [checklist results](hw-checklist.md) are the current radio evidence.

**Delivered:**

- NVS-backed config/seen-app/journal storage; BLE custom service, Battery and Device Information services; advertising from the contract GATT layout; Secure Connections + MITM passkey bonding, pairing mode, bond management, MTU request and connection policy.
- NimBLE radio glue and portable BLE helpers with queues to the shared app task. Core, ANCS state machine and UI share that task.
- ANCS discovery, Service Changed/subscription handling, fragmented attribute fetch, per-session app-name cache, removals/reset and negative-action dismiss; portable state-machine tests/fuzzing.
- Passkey screens on both products and simulator, BlueZ passkey agent/client, throughput tooling and optional firmware stats. Pairing now has a three-minute timeout; current settings are described in PLAN §7.

**Recorded hardware progress (2026-10-14):**

- Android/nRF Connect read the GATT table and Capabilities, paired/rejected a wrong passkey, reconnected with a persisted bond after a board reset, and exchanged encrypted traffic with the LCD screen off.
- Advertising intervals and the then-current Android connection parameters were recorded. Later parameter-policy changes require fresh acceptance.
- iPad listed and paired with the board and exposed ANCS. Descriptor discovery and a competing connection-update crash required fixes; a complete post-fix ANCS rerun is not recorded.

**Outstanding acceptance:**

- Complete pre-bond read/write/subscription rejection and Secure-Connections-only checks, stranger/repeat pairing rules, timeout and fourth-bond eviction, on both products/phone families.
- Android companion-app config/notification/dismiss/reconnect flows; LCD icon-upload success alone does not close that full matrix.
- iOS notification delivery, consent/subscription/Service Changed, replay after reconnect, app names, removal/negative action and private-address reconnect; iOS app + system ANCS concurrency remains a hardware question.
- Throughput/MTU/connection parameters, notification wake latency, long-run link stability and sleep/current measurements; content-free full-session logs.
- Real-board laptop/BlueZ-agent flows and the remaining device-specific checks in 39–53 and pairing-at-limit entry 71.

**Verification:** portable `test_ble`/`test_ancs`, vectors, sanitizer/fuzz checks, warning-free builds and source review have recorded passes. Those passes do not close the phone criteria above; historical binary sizes remain in the notes.

## M6: Transfer + OTA with signing, rollback, battery checks

**Goal:** assets and signed firmware install safely over BLE, with recoverable failures.

**Status:** partially delivered through M6a. The user approved doing the asset sink separately on 2026-10-17; the real firmware OTA integration remains planned. M5's complete hardware matrix is still open and must be considered before OTA device acceptance.

### M6a: LittleFS asset sink and app icons

**Status:** implementation done and host-verified; partial LCD hardware verification. [M6a notes and subsequent updates](milestone-notes/M6a.md).

**Delivered:**

- `blob_fs` adapter, atomic replace and resume-journal integration; icons supplied to the shared UI from LittleFS.
- Firmware advertises only supported `app_icon` assets: Paper 24 × 24 `mono1` (88 B max), LCD 32 × 32 `rgb565a8` (3088 B max). Backgrounds/themes/fonts are not advertised uploads.
- Icon deletion when an app is forgotten/evicted (contract 0.12.0), with shared vectors and host tests.
- Fix for LittleFS open-file `fstat()` behaviour: track open size and test with a failing `fstat` wrapper.

**Recorded evidence:** 2026-10-17 firmware builds, sanitizer/coverage/Python checks and transfer-on-filesystem tests passed as documented in the notes. After the fix, the companion app uploaded an icon to the LCD and it was drawn (checklist 76).

**Remaining hardware acceptance:** persist across reboot/power loss, interrupted upload/resume, replacement/deletion/forget/eviction on current firmware, `NO_SPACE`, real storage reporting and Paper upload/rendering. `tools/test_client` real-board icon upload is not established by the TCP/simulator flows.

### Remaining M6 firmware OTA work

**Deliverables still needed:**

- Real `esp_ota_*` firmware sink with resume, signed-image verification and version/battery policy connected to the running firmware.
- Boot self-check/mark-valid and rollback integration, progress display wired to real installs, recovery and key-handling procedures verified end to end.
- Key generation/signing/verification currently use ESP-IDF/`espsecure.py` and `variants/sdkconfig.signed`; no separate `tools/sign/` wrapper has been delivered.

**Release tooling already present:** release-manifest schema, `tools/release/make_release.py`, CI/release/Pages workflows and browser-site builder. Local script tests and workflow files do not establish a successful GitHub release/deployment or an on-device OTA install. Confirm release location, signing secret and deployment behaviour before release.

**Acceptance on hardware:** signed image installs and is marked valid; unsigned/wrong-key image is rejected at FINISH; failing self-check rolls back; downgrade/low-battery refusal works; disconnect resumes; power loss leaves the old firmware usable and config/assets intact; bonds/NVS/assets survive an update. Record actual release-config unsigned/signed sizes and slot headroom. Existing signed USB builds and the portable OTA model do not satisfy these checks.

**Tests:** host state-machine/failure simulations plus scripted on-device negative paths after implementation. No irreversible Secure Boot, flash-encryption or anti-rollback eFuse change is authorized by this milestone description.

## M7: Hardware-verification checklist

**Goal:** close remaining hardware questions with reproducible evidence, including any follow-ups that prevent release.

**Status:** planned for formal completion. Boards are in hand and substantial evidence has already been recorded during P1a and subsequent sessions; this is not an untouched checklist.

**Deliverable:** [hw-checklist.md](hw-checklist.md) with procedure, date, firmware/config, board/phone identity and result. Preserve partial results and distinguish later code changes from tested binaries.

**Remaining work:** BLE throughput/latency and phone interoperability; Paper panel/controller/refresh checks; power latch, battery calibration and overnight current/drain; RTC/climate; LCD long-run/touch/settings/wake behaviour; Wi-Fi/setup/profile/coexistence; asset persistence/resume/full-storage; firmware OTA failure paths once implemented; browser flashing. See [PLAN §9](PLAN.md#9-hardware-evidence-and-remaining-verification) for the grouped evidence and open work.

**Acceptance:** every item has a measured/observed result with its limits, or an explicit tracked follow-up and release impact. A partial result, empty cell or “not run” is not a pass. Throughput remains item 1; its results inform whether a future Wi-Fi transfer path is justified. Simulator passes cannot close panel/radio/current checks.

## P1: LCD (board: ostb-xiaozhi-3st)

Current board facts are in [PLAN §2b](PLAN.md#2b-lcd-board-ostb-xiaozhi-3st), partitions in [§5b](PLAN.md#5b-partition-table-lcd-16-mb), and controls/defaults in [§7c](PLAN.md#7c-current-ui-and-controls). Pins were cross-checked between community information and the stock image, then exercised in P1a; a schematic remains unavailable in the recorded research.

The case stays closed. **Any LCD flash, including a probe or stock restore, requires explicit user go-ahead in the current session.** Keep native USB pins/PHY and `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y` intact. There is no BOOT key; the power key is a hardware switch. See [recovery.md](recovery.md).

### P1a: Board bring-up

**Goal:** board drivers build and boot, and the hardware used by ePager works through `board_api`.

**Status:** done (hw-verified) 2026-10-13, awaiting review. [P1a notes](milestone-notes/P1a.md); battery characterization and long-idle touch were carried forward.

**Delivered:**

- Private stock-flash backup/analysis and recovery procedure. A separate `tools/hw_probe/` image was dropped because the stock image supplied the pin map.
- `boards/ostb_xiaozhi_3st/`: one sourced pin header, early safe-state init, own LCD driver with the approved stock init sequence, calibrated polled CST816S touch, backlight, volume keys, battery tables and charging-status input. No firmware-controlled power latch.
- Optional LCD area/backlight/touch/button board-API additions and host-tested conversion/level/touch helpers.
- LCD ESP-IDF project, partitions, dependency lock and bring-up patterns. The small initial bring-up image is historical; the current default runs the shared product app. `CONFIG_EPAGER_LCD_BRINGUP=y` selects the test image.
- Current memory/console/partition settings live in the board directory; signing is shared under `variants/`. First images used DIO/40 MHz PSRAM with PM off; verified bring-up moved to QIO/80 MHz PSRAM and 80 MHz LCD SPI. PM/light sleep was added with P1b.

**Recorded acceptance:** display colours/borders/orientation/gradients, 20 clean resets, about 15 ms full frames at 80 MHz, backlight, touch calibration, volume keys, hardware power key, PSRAM/storage/I2C boot and USB reflashing passed (checklist 21–28). Recorded PC checks passed, including both firmware builds and 238 Python tests at that time; these are dated results, not a new test run.

**Carried forward:** full battery calibration/discharge/charge-termination behaviour, touch after a long idle, display-controller identity and extended 80 MHz heat/stability. Charging polarity was subsequently established as low = charging, with a short battery log on 2026-10-14; that does not complete battery characterization. Audio operation was outside P1a.

### P1b: Colour UI, simulator twin, contract 0.8.0

**Goal:** the shared core drives a usable colour notification/clock/weather UI with touch/key controls, settings and power policy, with a host simulator twin.

**Status:** in progress; approved 2026-10-14. UI, simulator, weather/network glue and settings are implemented and have recorded host/build verification. Remaining hardware acceptance and `docs/milestone-notes/P1b.md` are still required before closing. The board has since run product/BLE builds; it is not still awaiting its first post-stock flash.

**Implemented deliverables:**

1. Shared `ui_common` model/formatting/icon/input logic, frame and area backends, mono and colour layouts, Kconfig exclusion of the unused layout, and shared `components/app` firmware glue. Core/UI/BLE event handling share the app task; the network fetch task reports results to it.
2. LVGL 9.5.0 RGB565 area rendering through copied `write_area` bands; light 296 × 240 layout with status bar, category-coloured cards, clock/date/weather, pairing/passkey and setup/settings screens. Simulated low-battery/update states exercise UI rendering; real firmware OTA remains M6.
3. Anti-aliased Noto Sans (16/20 px and 72 px clock), compressed colour emoji and tinted built-in icons. Uploaded LCD icons are 32 × 32 RGB565A8; file storage/real upload is tracked in M6a. Background format exists in the contract but a background picker/renderer and advertised background uploads are not delivered features.
4. Current touch controls: on cards left/right changes card, down dismisses, up hides cards/shows clock, tap expands/collapses text and long text scrolls. On clock up opens settings, down opens cards. In settings tap selects, up/down pages and right goes back. Rotation 0°/180° also rotates touch coordinates; targets avoid the case edges.
5. Volume keys navigate/adjust brightness; both released together sleep now, both held three seconds open pairing. Idle defaults: brightness 70%, dim at 30 s to at most 20%, screen off at **60 s**. Existing config survives default changes.
6. LCD sleep is screen off plus light sleep, never deep sleep. Volume keys wake the chip; notifications can wake the screen. **Polled touch has no interrupt and cannot wake the sleeping chip.** USB-host sleep guard, PM locks, board sleep/wake callbacks and shared touch settings list are implemented; current-device wake/current acceptance remains open.
7. `boards/sim_lcd_296x240`, `libepager_sim_lcd.so`, fake board `--board lcd`, colour Tk/mouse controls, headless screenshots and LCD golden images/client flows.
8. Debug console (off in release) and battery/event diagnostics. LCD battery logging is enabled for characterization; Paper also logs every five minutes. Periodic logging and PM/task/A-B diagnostics are implementation support, not measured battery-life results.
9. Contract additions introduced in 0.8.0/0.9.0: brightness/idle timeouts, RGB565A8/colour asset formats, notification dismiss and Wi-Fi/weather. Current contract is 0.13.0; CR-1 brightness is resolved.
10. Portable weather parsing/URLs/scheduler plus `components/net` for on-demand Wi-Fi/HTTPS/SNTP/setup hotspot. Both boards now support weather/time over Wi-Fi; no app/firmware protocol runs over it. Default fetch interval is **60 minutes**. Fake-board console uses `weather live [CITY]`, not a `--live-weather` flag.

**Acceptance still required for closure:**

- Host tests, vectors, coverage ≥85%, contract check, deterministic current Paper/LCD golden images, and area/layout/input/backlight/settings tests pass. Pure refactors must preserve rendering; intentional later UI/default changes require reviewed golden updates rather than an obsolete pre-P1b baseline.
- Both variants build warning-free; feature/layout, Xtensa and LVGL-config checks pass. Record release-config size (≥1.4× slot headroom), LVGL/internal heap and stack with Wi-Fi/BLE active. Earlier build sizes do not establish current headroom.
- Fake-board LCD client flows cover notifications, config, brightness/timeouts and touch; record companion-app coverage separately from test-client coverage.
- Hardware: colour screens/text/emoji/icons legible and matching intended layout; scrolling/gestures/rotation/settings usable; dim/off and key/notification wake work; USB still reflashes. Do not require touch to wake a sleeping chip. Complete remaining checklist 14, 27, 29–38 and 54–55, including long-idle touch and battery work carried from P1a.
- Network/time: Wi-Fi saved by app/console and hotspot, captive page/QR flow on both phone families, weather/SNTP/time precedence, radio off between fetches, saved-profile selection/fallback and BLE coexistence. Hardware Paper extensions are also tracked in 60–62 and 72/78.
- Write the P1b closing note with what passed, what remains and explicit follow-ups, update this status and stop for review. No final sign-off is inferred from the successful LCD icon upload.

## v2: Wi-Fi fast path (only when you ask)

**Status:** not scheduled. Weather/time/setup Wi-Fi already exists in v1; Wi-Fi notification/asset/firmware transport does not.

The deferred proposal is for the app to request AP/join over the encrypted BLE link and use the transfer protocol over HTTP. It needs its own approved plan, protocol/security design and acceptance tests, informed by measured BLE throughput.

## Milestone notes

One file per completed milestone/subpart under `docs/milestone-notes/`: delivered work, verification and method, unverified parts, decisions/contract changes. Read the relevant dated notes and subsequent checklist updates together.

- [M0: Plan (2026-10-11)](milestone-notes/M0.md)
- [M1: Contracts + portable protocol library + PC tests (2026-10-11)](milestone-notes/M1.md)
- [M2: Board-independent logic with host tests (2026-10-11)](milestone-notes/M2.md)
- [M3: Fake board + screen simulator (2026-10-11)](milestone-notes/M3.md)
- [M4: ESP-IDF project for Paper (2026-10-11)](milestone-notes/M4.md)
- [M4b: Clock, temperature and humidity (2026-10-12)](milestone-notes/M4b.md)
- [P1a: LCD board bring-up (2026-10-13)](milestone-notes/P1a.md)
- [M5: BLE and ANCS (2026-10-14, subsequent radio evidence in checklist)](milestone-notes/M5.md)
- [M6a: Asset sink and app icons (2026-10-17, with later fixes/results)](milestone-notes/M6a.md)

P1b, M6 as a whole and M7 have no closing note yet; their remaining work is listed above.

### Notes between milestones

The [contract changelog](../contracts/CHANGELOG.md) is the authoritative version history. These are delivered increments, not newly approved milestones or proof of full hardware acceptance:

| Date / contract | Increment |
| --- | --- |
| 2026-10-11 / 0.4.0 | Per-app icons, SHA-256-derived asset ids, dimensions and fallback rules; simulator/UI support subsequently added to M3. |
| 2026-10-11 / 0.5.0 | Emoji/font/text-normalization additions and golden-image updates; no wire-layout change. |
| 2026-10-12 / 0.7.0 | Contract-request outcomes: stored background id, pairing-mode capabilities, releases/manifest schema, identity/transport constants and extra vectors. Stored background configuration did not implement a renderer. Brightness later resolved with 0.8.0. |
| 2026-10-14 / 0.8.0–0.9.0 | LCD controls/colour asset formats and Wi-Fi/weather, implemented under P1b; hardware coverage remains itemized. |
| 2026-10-15 / 0.9.1–0.9.2 | Paper Wi-Fi/weather and weighted full-refresh policy, default 30; later Paper settings/power/diagnostics work is tracked in PLAN and the hardware checklist. |
| 2026-10-15 / 0.10.0 | Multiple saved Wi-Fi profiles with named/all-profile forget and LRU eviction. |
| 2026-10-16 / 0.10.1–0.11.0 | Number masking, product/variant naming changes. |
| 2026-10-17 / 0.12.0 | Icons removed when their app is forgotten/evicted; vectors, core hooks and UI fallback. Real LCD upload subsequently worked after the LittleFS open-size fix. |
| 2026-10-18 / 0.12.1 | LCD screen-off default reduced to 60 s; weather-fetch default to 60 minutes. Stored settings retained. |
| 2026-10-19 / 0.13.0 | Public product/advertising/hotspot name becomes `ePager` / `ePager-XXXX`; UUIDs and identifiers remain stable. |
