# Plan: installable clock faces (M8)

**Status: approved 2026-11-04 (D113), with the open defaults below as written. Nothing is implemented; M8a is next.**

**User decisions, 2026-11-04:**
- The feature is called **clock face** (`clock_face` in the contract and code): the board is a desk display, not a watch, and the UI already calls its idle screen the clock screen.
- Faces can draw numbers from **images** (digit sets), so 3D or hand-drawn styles are possible, not only fonts.
- A **face gallery is hosted on GitHub**; other developers build faces with our tools and submit them by pull request.
- Users can **change parts of a face** (colours, show or hide weather, digit style) without editing it.
- Users can **install their own faces without the gallery**.
- **No hotspot upload page**: an iOS app will come later, so both phone apps install faces over BLE.
- **The number of waiting notifications is a field** that a face places and styles like any other value. **The firmware's notification badge shows on built-in faces only**; an installed face shows the count only where it places the field.

## Why

Pebble and Garmin users expect to choose and install faces. The board already has everything below the screen: a bounded asset transfer over BLE, a LittleFS asset store with replace and delete (M6a), per-board `asset_support`, and placeholders in the contract (`theme`, `font`, `background`, `ui.theme_id`, `ui.font_id`, `ui.background_id`). What is missing is a face format, a renderer and the tools around them.

## Facts this plan rests on

| Fact | Where |
| --- | --- |
| The idle screen is hand-written per layout: `build_idle` / `apply_idle` in `ui_layout_mono.c` (Paper) and `apply_idle` in `ui_layout_colour.c` (LCD). | source |
| Neither board draws `ui.background_id` yet; the key is stored only. `app.c` advertises one `asset_support` entry (`app_icon`), because no other type has a reader. | `components/app/src/app.c`, `contracts/asset-formats.md` |
| Asset partition: Paper 1.875 MiB, LCD 7.875 MiB (LittleFS). App slots: Paper 3 MiB (image 1.5 MB), LCD 4 MiB (image 2.8 MB). | `boards/*/partitions.csv`, `build/*/*.bin` 2026-10-24 |
| The UI already has every value a face needs: UTC and time zone, 12/24 h, language, temperature unit, the last weather reading, room climate, battery and charging, BLE and Wi-Fi state, the number of cards, the update mark. | `ui.h`, `ui_priv.h` |
| `asset_type` values in use: 1 font, 2 background, 3 theme, 5 app_icon (4 is a gap: do not reuse it). `asset_format` values: 1-4, 6. | `contracts/registry.json` |
| Config already supports `bytes` values. UI keys live at 64-66. | `contracts/registry.json` |
| Asset writes need an encrypted, bonded BLE link (protocol.md). | contract |
| **Not established:** BLE throughput on either board, so the upload time of a large LCD face is unknown. Whether an iOS app can connect next to the system's ANCS link is a hardware test (AGENTS.md). | hw-checklist 39-53 |

## Design

```
 face.json + PNGs  --epager-face build-->  .epface (clock_face_v1)  --BLE asset transfer-->  LittleFS
 (developer)          (tools/clock_face)    gallery release / file                         |
                                                                                            v
 ui state (time, weather, battery, ...)  -->  cf_eval (portable)  -->  ui_face.c (LVGL)  -->  screen
 ui.clock_face_id, ui.clock_face_opts    -->  cf_parse (portable, bounded)
```

### Data only

A face is a description, never code. The board parses it with a bounded parser, like every other asset, and draws it with LVGL. No script engine, no virtual machine. A face cannot reach the network, the flash or notification content.

### The face file: `clock_face` + `clock_face_v1`

The usual 16-byte asset container (`ESPA`), then a TLV payload (protocol.md §2.3 encoding). `width`/`height` in the header are the screen size the face is made for. One file carries one board's screen; the gallery and the tools publish one file per board.

| Section | Contents |
| --- | --- |
| `meta` | face id (`author.name`, `[a-z0-9._-]`, 1-32 bytes), name (English, optional Vietnamese), author, version (semver), minimum contract |
| `images` | table of images: format (`mono1` on the Paper, `rgb565a8` or `rgb565` on the LCD), width, height, payload. Digit sets are image lists: ten digits, then optional colon, AM, PM, minus, degree, percent |
| `options` | at most 8: id (u8, stable across versions), type (`toggle`, `choice`, `colour`), label (en, vi), default, choices (at most 8; a colour choice is an RGB565 value from the face's palette) |
| `elements` | at most 48, drawn in order |

**Elements:**

| Kind | Draws | Notes |
| --- | --- | --- |
| `rect`, `line`, `arc` | shapes | `arc` (with a value range) on the LCD only, e.g. a battery ring |
| `image` | an image from the table | backgrounds, decorations |
| `number` | a value with a digit set | per-digit widths, spacing, alignment, leading zero |
| `text` | a value with a built-in font | the existing generated fonts (registry enum `face_font`), so Vietnamese works; no uploaded fonts in M8 |
| `icon` | a built-in icon | weather condition, BLE, Wi-Fi, battery, notifications, update |

**Common element fields:** position, size, alignment, colour (fixed or `option N`), `source`, `format` (a few tokens: `HH`, `H`, `mm`, `ddd`, `dddd`, `d`, `MMM`, `MMMM`, `yyyy`, unit on or off), and `visible_if` (option N equals a value, a source has a value, or a source is above zero).

**Data sources** (registry enum `face_source`, additive later): hour, minute, AM/PM, day, month, year, weekday, localised date, weather now / high / low / rain % / condition / place, room temperature and humidity, battery %, charging, BLE connected, Wi-Fi state, number of waiting notifications (`notif_count`), update available (`update_available`). An element with an unknown source or kind is skipped, never fatal, so newer faces still draw on older firmware (with the parts it knows).

**Global settings stay global:** the face reads `ui.clock_24h`, `ui.temp_unit`, `ui.language` and `weather.enabled`; it has no options for them. A face's "show weather" toggle only hides its weather elements.

**Notification count (user, 2026-11-04):** the count is the field `notif_count`. A face draws it with a `number` (its own digit set), a `text` or an `icon` element, hides it with `visible_if: notif_count > 0` or behind one of its options, or leaves it out. **The firmware's standard badge is drawn on built-in faces only** (Classic, as today, and the bundled faces, which the renderer knows by their built-in flag); an installed face never gets it.

**Update mark:** a face that does not draw the `update_available` field gets the firmware's small update mark at an anchor the face names (default top right).

**Screen sizes and rotation:** a face draws only when the logical screen size equals the header's `width`/`height`. When `display.orientation` swaps them (LCD 296x240 to 240x296) and the face has no file for that size, the built-in face shows.

**Limits (per board in `asset_support.max_size`; the others are format rules):** file size Paper 64 KiB, LCD 1 MiB; 32 images; 48 elements; 8 options of 8 choices; total LittleFS budget for faces Paper 1 MiB, LCD 4 MiB; at most 16 installed faces. A face above any limit is `IMAGE_INVALID` at `XFER_FINISH`, `NO_SPACE` when the budget is full.

### Selection and options (contract)

- `ui.clock_face_id` (`str`, 32, default `""` = the built-in Classic face): the face on the clock screen. An id that is not installed shows Classic.
- `ui.clock_face_opts` (`bytes`, 32, default empty): `{option id, value}` pairs for the active face. Unknown ids are ignored; a value out of range uses the option's default.
- The board also keeps each face's values in `<face id>.opt` on LittleFS, so switching faces and back keeps the user's choices. Selecting a face loads its file into `ui.clock_face_opts`; a write of the key also writes the file. Deleting a face deletes its file.
- **Origin:** `XFER_BEGIN` gets an optional tag `origin` (`unknown` / `gallery` / `local`), stored with the asset and reported in `ASSET_LIST`. The apps' gallery updates replace only `gallery` faces and never overwrite a `local` face with the same id without asking the user.
- `ui.background_id`, `ui.theme_id`, `ui.font_id`, `theme_v1` and `font_v1` are left as they are; the background is for Classic only. Faces supersede themes (open default 3).

### Built-in faces

- **Classic** is today's hand-written idle screen, kept as code. It is the fallback for every failure (bad file, wrong size, missing face) and cannot be deleted.
- Two or three more built-in faces are written in the face source format, compiled by our tool and linked into the firmware as const data, then parsed by the same parser. They are the reference examples and prove that the format can express real faces. LCD built-ins stay small (fonts and shapes, at most one image digit set) to keep the firmware image size in check.

### What the user sees

- Settings → `Clock face`: the list of faces (built-in first, then installed, local ones marked `From file`), a check on the active one. Select = use it. On a selected face: `Customize` (its options) and `Delete` (installed faces only, with the existing confirm).
- Paper: short BOOT/PWR move, long BOOT selects, long PWR goes back (D98). LCD: tap and swipe (D99).
- Changing face or an option rebuilds the face's objects; the Paper does one full refresh after it.
- The phone apps show the same list, a live preview, the gallery and `Install from file`.

### Rendering (`components/ui_common/src/ui_face.c`)

- Build: one LVGL object per visible element. Images are read from LittleFS once into a buffer (PSRAM on the LCD; the Paper's images are a few KiB) and converted like the existing `load_mono1`.
- Update: on the minute, a reading or a status change, re-evaluate the sources and change only the objects whose value changed. On the Paper the minute update stays a clock partial in the weighted refresh policy (`display.full_refresh_every`).
- Every failure falls back to Classic and is counted in a diagnostic (`face_errors`); face ids and names are not personal data and may be logged.

## Components

**New, portable and host-tested** (`components/clock_face`, prefix `cf_`, no ESP-IDF or LVGL headers):
- `cf_parse`: validates the whole file once (sections, counts, image sizes and formats against the board's rules, element fields, references to images and options, no overlaps or overruns), then exposes it in place without copying. Status codes from `registry.json`.
- `cf_eval`: from a `cf_data_t` snapshot of the UI state and the option values, the text, digit images, colour and visibility of each element. Format tokens and localised names come from the UI's existing tables.
- `cf_opts`: merging stored values with the face's options (unknown ids, out-of-range values, a face update that removes or adds options).
- Tests: valid, invalid and edge files from the contract vectors; a fuzz target next to the existing ones (ASan/UBSan); `tools/xtensa_check.sh`.

**Changed:**
- `ui_common`: `ui_face.c` (renderer), face list and `Customize` pages in `ui_settings.c`, strings (en, vi), the badge on built-in faces and the update mark. Classic stays in the layouts.
- `app.c`: a second `asset_support` entry, the per-face budget and count, the `.opt` files, the fallback when the active face is deleted, reload when the active face is replaced.
- `core` / `config_store`: the two keys; `transfer`: the `origin` tag and its storage next to the version.
- Fake board: `face install FILE`, `face select ID`, `face opt N V`, `face list`, `face delete ID`.

**New tools** (`tools/clock_face`, Python 3.12, type hints, pytest):
- Source format: `face.json` with a JSON Schema (`tools/clock_face/face.schema.json`), PNGs next to it, one shared description with per-board overrides. This is a tool convention, not contract: it can change without a contract bump.
- `epager-face new | check | build | preview [--matrix] | run | push`: `preview` renders through `libepager_sim.so` (the real parser and UI); `--matrix` covers every option × 12/24 h × °C/°F × en/vi × weather on/off/missing; `run` opens the fake board's viewer with the face; `push` installs on a board over BLE through `tools/test_client`.
- A Python reference parser in `tools/epager_proto` for the vectors.

**Contract (0.15.0, additive minor; first in the work):** `asset_type` 6 `clock_face`, `asset_format` 7 `clock_face_v1`, enums `face_source`, `face_font`, `face_element`, `asset_origin`; config keys `ui.clock_face_id` and `ui.clock_face_opts`; `XFER_BEGIN.origin` and `asset_entry.origin`; the `clock_face_v1` section in `asset-formats.md`; `contracts/clock-face-gallery.md` + `clock-face-gallery.schema.json` (hand-written, like `releases.md`); vectors `contracts/test-vectors/clock_faces.json` (cases in `tools/codegen/vectors.py`); `CONTRACT_VERSION`, `CHANGELOG.md`. The Android and iOS apps must pass the new vectors.

### The gallery (separate public repository, e.g. `tdpi95/epager-faces`)

- One folder per face: `face.json`, PNGs, `LICENSE` (font licences named), `README.md`.
- CI on every pull request: `epager-face check` and `build`, the C parser from the host build under sanitizers on every output, the preview matrix for both boards posted as a comment; it fails on budgets, a missing licence, or a change outside the author's folder. CODEOWNERS: each author owns their folder; a maintainer reviews every first submission (content rules: no brand logos or trademarks, nothing offensive, legible).
- Release: `gallery.json` (id, name, author, version, per board: file, size, SHA-256, minimum contract, thumbnail), the `.epface` files and thumbnails, at `https://github.com/tdpi95/epager-faces/releases/latest/download/gallery.json`. No API calls, so no rate limit.
- **Only the phone apps read the gallery** (open default 1); the board has no gallery client, parser or allow-list entry. Faces are data only, so the manifest is not signed: the SHA-256 in it and HTTPS are enough.

## Phases and acceptance

Three sub-milestones, each ending with notes and a stop for review, like M6a-c.

### M8a: format, parser, renderer, built-in faces (host only)

1. **Contract 0.15.0** as above. Done when `contract_check.py`, the codegen and the host tests pass.
2. **`components/clock_face`** with tests and fuzzing; `tools/clock_face` compiler and the Python reference parser. Done when every vector passes in C and Python, the fuzz run is clean, coverage of the component is at least 85 %, and `xtensa_check.sh` passes.
3. **`ui_face.c`**, the badge on built-in faces, the update mark, two built-in faces per board (one with an image digit set), fake-board commands, golden images (`tests/golden/<variant>/<size>/face_*.png`) for each built-in face, a few option combinations, the badge on a built-in face, an installed face with and without the `notif_count` field (no badge either way), and a broken file that falls back to Classic. Done when `pytest tools` passes.

### M8b: install, choose, customise (firmware)

4. Glue: `asset_support`, budgets, `.opt` files, delete and replace of the active face, the `origin` tag, the two config keys, the Settings pages (both layouts, en and vi), console commands in the debug image. Done when both variants and the signed Paper build compile without warnings, size reports are recorded (expect the renderer and parser in the tens of KiB; measure), and `tools/feature_check.sh` passes if the feature gets a Kconfig switch (`CONFIG_EPAGER_FEATURE_CLOCK_FACES`, on in `sdkconfig.common`).
5. **Hardware acceptance (new checklist item 85; the LCD board only after the user's go-ahead):** install a Paper face and an LCD face with `tools/test_client --ble` and with the Android app; record the upload time of a 64 KiB and a 1 MiB face; select, customise, switch and switch back (options kept); reboot and power cut mid-upload (old face or Classic shows, nothing corrupted); delete the active face (Classic shows); a face for the wrong size or a corrupt file (refused at `XFER_FINISH`); the budget full (`NO_SPACE`); minute updates for a day on the Paper (ghosting with large dark and dithered areas, refresh counts in the battery log); battery cost against Classic on both boards (batlog `awake_ms`); free heap on the LCD with the largest face.

### M8c: developer tools, guide, gallery

6. `epager-face` complete, `docs/clock-faces.md` (developer guide: sources, elements, options, limits per board, design tips for e-paper ghosting and LCD contrast), the built-in faces' sources as examples.
7. The gallery repository with CI and the first release, made by the user or with their go-ahead (it is a new public repository). The phone apps' gallery and install screens are app work, outside this repository; this step is done when the contract, the schema and the first `gallery.json` are ready for them.

## Risks and unknowns (each a `TODO(hw-verify)` until measured)

- BLE upload time for large LCD faces: throughput is not measured. If it is too slow, lower the LCD limit or use `rgb565` backgrounds without alpha; the v2 Wi-Fi fast path stays out of scope.
- LCD memory: a 142 KB background plus digit sets decoded into PSRAM next to LVGL's buffers and a TLS session (D78 saw `ESP_ERR_NO_MEM` in internal RAM). Images go to PSRAM; measure the heap with the largest face.
- E-paper ghosting and refresh cost with dark or dithered faces; the controller and its waveforms are still unverified (checklist 7).
- The parser reads untrusted files: fuzzing and the vectors are the guard, and Classic is always there.
- Draw time on the LCD's area path when many elements change at once (e.g. at midnight).
- iOS installs depend on the iOS app connecting next to ANCS (hardware test).
- Firmware image growth from built-in LCD faces with images: keep built-ins small.

## Defaults (approved with the plan; the user may still change any)

1. The board has **no gallery client**; only the phone apps browse and download.
2. Limits: file **64 KiB Paper / 1 MiB LCD**, budget **1 MiB / 4 MiB**, **16** faces, **8** options of **8** choices, **48** elements, **32** images.
3. Themes and uploaded fonts: leave `theme_v1` / `font_v1` provisional and unused, and drop them in a later contract if faces cover every need.
4. Built-in faces: **Classic plus two** per board.
5. Gallery repository name and licence policy for submitted faces (suggested: any OSI or Creative Commons licence that allows redistribution).

## Documents to change as the work lands

`AGENTS.md` (repo layout: `components/clock_face`, `tools/clock_face`; commands with their status), `docs/PLAN.md` (§3 architecture, §7 requirements: assets; D113), `docs/MILESTONES.md` (M8 status), `contracts/*` as listed, `docs/hw-checklist.md` (item 85), `docs/clock-faces.md` (developer guide, M8c), `docs/milestone-notes/M8a.md`, `M8b.md`, `M8c.md` at the end of each part.
