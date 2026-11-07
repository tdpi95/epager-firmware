# M8b: install, choose and customise clock faces (firmware, host-verified)

Date: 2026-11-04. Plan: [clock-faces-plan.md](../clock-faces-plan.md) (D113), step 4; M8a notes: [M8a.md](M8a.md). **Both firmware images build and the host tests pass; nothing has run on a board** (checklist 85). Stops for review; M8c (the `epager-face` tool, the developer guide, the gallery) is next.

## Done

**Install (transfer layer, portable)**
- `xfer_asset_support_t` has two local limits that are not advertised: `budget` (bytes of all installed assets of the type together) and `max_count`. `XFER_BEGIN` answers `NO_SPACE` when the new file would break either; a replacement does not count the file it replaces. The boards set them for faces: Paper 64 KiB per file / 1 MiB / 16, LCD 1 MiB / 4 MiB / 16.
- `xfer_params_t.check_asset` (and `core_params_t.check_asset`): the content check of an asset at `XFER_FINISH`, after the generic header check. For faces it is `ui_face_check` (`ui_face.c`): the file is read once into PSRAM and parsed by `cf_parse` with the board's limits and the asset id. A bad face is `IMAGE_INVALID` and discarded; nothing is installed. `transfer` knows nothing of faces.
- **Origin** (contract 0.15.0 §7.2): `XFER_BEGIN.origin` is kept for the request that completes the transfer (not journaled) and stored in the asset's version file as `version [NUL origin]` (old files, which hold only the version, read as before); `ASSET_LIST` returns it, with the version or alone.
- Vectors: `transfer.json` gains `asset_origin` and `asset_budget` (and the scenario boards may set `budget` / `max_count`); the Python board model and the C harness run them (`CHANGELOG.md` 0.15.0 says so; no spec change, no version bump).

**Choose and customise (core, portable)**
- Each face keeps its option values in the file `opt_<id>` next to the assets (the name cannot be an asset: assets start with a number). `core.c` does the bookkeeping in `cb_config_changed`: a write of `ui.clock_face_opts` stores the active face's values (temp file and atomic rename, an empty value removes the file); a change of `ui.clock_face_id` alone loads the new face's values into `ui.clock_face_opts` (none stored = defaults); both keys in one `CONFIG_SET`: the given values win.
- When a face is installed or deleted, `cb_asset` removes the option files of faces that are gone and, if the face in use is gone, sets `ui.clock_face_id` back to `""` (Classic; the phone sees the change). `core_face_select`, `core_face_option` and `core_face_delete` are the calls the settings pages use (shared by the firmware, the fake board and the console).

**Settings pages (both layouts, English and Vietnamese)**
- Settings → **Clock face** (the row before About, so the rows above keep their places): the list is Classic, the built-in faces of the layout, then the installed faces (their names read from the first 512 bytes of the file by `cf_peek_names`; the face in use says "In use", a face installed from a file says "From file"). Selecting a row opens the **face's page**: its status, **Use this face** (not in use), the face's **options** (only for the face in use: a select moves the option to its next value), and **Delete** for installed faces (asks "Delete?" first, like Forget). Built-in faces and Classic cannot be deleted.
- Keys (Paper): short BOOT / PWR move, long BOOT selects, long PWR goes back; touch (LCD): tap selects, swipe up / down turns the page, swipe right goes back. UI actions `UI_ACTION_FACE_SELECT`, `_OPTION`, `_DELETE` (`ui_face_list_id(index)` gives the id for an index of the list).
- `CONFIG_EPAGER_FEATURE_CLOCK_FACES` (components/app/Kconfig, on in `variants/sdkconfig.common`): advertises the `clock_face` asset type, sets the check and the limits, and shows the Clock face row (`ui_params_t.no_faces`). Off: Classic and the data-only renderer stay in the image.
- Debug console (`CONFIG_EPAGER_DEBUG_CONSOLE` image): `face [list|select ID|opt N V|delete ID]`.
- Fake board: the console's `face` commands now go through the board's own bookkeeping (the Python option memory of M8a is gone); `face install FILE origin=gallery|local`; `loopback.put_clock_face_unchecked` stores a file past `XFER_FINISH` for the "damaged after install" scenes (the fallback scenes need it now that a bad face cannot be uploaded).

**The LVGL pool (D117, decided)**: `LV_MEM_SIZE` is **128 KB on every build and lives in PSRAM on the boards** (`LV_ATTRIBUTE_LARGE_RAM_ARRAY` = `EXT_RAM_BSS_ATTR`); it was 48 KB of internal RAM. The Paper's idle screen leaves about 5 KB of 48, a face needs up to 220 objects (about 70 KB), so 128 KB leaves a face's room; internal RAM gains the 48 KB. Not measured on a board (the object structs are read from PSRAM while LVGL draws; the pixel buffers stay internal): checklist 85.

## Verified (all host and build, 2026-11-04)

- `ctest --test-dir build/host`: 18 of 18. New: core (`test_face_option_values_follow_the_face`, `test_deleting_a_face_takes_its_options_and_the_selection`), UI (`test_settings_clock_face_pages` on the Paper keys, the same by touch on the LCD), `cf_peek_names` (`test_peek_names`), transfer scenarios `asset_origin` and `asset_budget` (C and Python). The settings tests changed only in their row counts (13 rows, 15 with updates, the LCD's pages).
- `tools/.venv/bin/pytest -q tools`: 438 passed, including the fake board's end-to-end checks (a corrupt, a wrong-screen and a wrong-id face are refused with `IMAGE_INVALID` and leave nothing; origin is listed; the 17th face is `NO_SPACE`, a replacement is not; deleting the face in use returns to Classic and removes its option file) and the golden images: five Paper settings frames changed only by their counts, four new frames (`settings_faces`, `settings_face_use`, `settings_faces_big_in_use`, `settings_face_options`) looked at.
- `contract_check.py` OK, `gen.py --check`, `vectors.py --check` current; coverage (`--min 85`): portable total 97.8 %, `xfer.c` 99.2 %, `core.c` 98.1 %, `cf_parse.c` 97.6 %.
- Firmware builds with zero warnings: Paper (image 1,573,735 bytes of a 3 MB slot, 50 % free; M8a 1,570,016), Paper console image, signed Paper (`-DEPAGER_SIGN=ON`), LCD (2,870,499 bytes of 4 MB, 32 % free; M8a 2,866,672), Paper with `CONFIG_EPAGER_FEATURE_CLOCK_FACES=n`. `tools/xtensa_check.sh` clean. `feature_check.sh` (against these builds): OK. `lvgl_conf_check.py`: 7 differences, 0 unexplained (`LV_ATTRIBUTE_LARGE_RAM_ARRAY` is the pool in PSRAM; `LV_MEM_SIZE` no longer differs).

## Follow-up, 2026-11-07: contract 0.16.0 (built-in faces in `CAPABILITIES`)

Asked for so an app can offer and select the built-in faces without a gallery: `CAPABILITIES` tag 15 `builtin_face` (`id`, repeated: the built-in ids other than Classic, in display order, only on boards whose `asset_support` lists `clock_face`). Additive minor bump: `registry.json`, `CONTRACT_VERSION`, `CHANGELOG.md`, `protocol.md` §5-§6 and the `capabilities_*` vectors in the same commit. Core: `core_params_t.builtin_face` callback (`ui_builtin_face_id(colour, i)` in the firmware and the fake board; `test_core` checks the message). An app finds a built-in face's options in the face source of that id (`components/ui_common/faces/`), shipped with the app per firmware release. Paper: `epager.big`, `epager.minimal`; LCD: `epager.big`, `epager.night`. Host tests, `pytest tools` (438; the Paper About golden shows 0.16.0) and the contract check pass; the firmware builds were re-run, see the commit.

## Not verified

- Everything on a board: BLE upload of a face and its time, the check at `XFER_FINISH` with the real PSRAM buffer, the settings pages on the e-paper (refresh behaviour while changing options) and on the LCD touch, the object pool in PSRAM (draw time, free internal heap), the option files on LittleFS, power cut during an upload or during an option write, the Android and iOS apps (outside this repository). Checklist 85 lists them.
- A face name for an installed face is read from the first 512 bytes: the tools of M8c must keep the `meta` section first (`tools/clock_face` does).
- No Dart vectors run here.

## Decisions

- **D119, local limits in `asset_support` entries.** The per-type budget and count are board policy, enforced at `XFER_BEGIN` and not advertised (the contract only says a board may answer `NO_SPACE`); the check hook is the one place the transfer layer calls out for the content of an asset.
- **D120, option files and the selection live in the core.** One implementation for the firmware, the fake board and the console; `opt_<id>` is not an asset name; an explicit write of the options beats the loaded ones; deleting the face in use clears `ui.clock_face_id` instead of leaving Classic to show through a dangling id.
- **D121, the pages.** Options are shown only for the face in use (the key holds one face's values); a face is used, then customised. Names of installed faces come from the file head, falling back to the id. The row sits before About.
- **D122 (supersedes the open part of D117), the pool.** 128 KB in PSRAM for both boards, the same size on the host so the tests see the same limit; to be measured.

## For M8c

`epager-face` (new, check, build, preview, run, push), the developer guide `docs/clock-faces.md` (limits, sources, elements, options, e-paper and LCD design tips, keep `meta` first), the gallery repository and its first release (a new public repository: the user's go-ahead first). Then the hardware acceptance (checklist 85) once a go-ahead for the LCD board and a phone build that installs faces exist.
