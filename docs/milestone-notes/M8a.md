# M8a: clock face format, parser, renderer, built-in faces (host only)

Date: 2026-11-04. Plan: [clock-faces-plan.md](../clock-faces-plan.md) (D113), steps 1-3. **Host only: nothing here has run on a board and the firmware does not yet advertise `clock_face`.** Stops for review; M8b (firmware glue, Settings pages, hardware acceptance) is next.

## Done

**1. Contract 0.15.0** (additive; `contracts/CHANGELOG.md`)
- Asset type `clock_face` (6), format `clock_face_v1` (7); the file format in `asset-formats.md` ("Clock faces": container, payload, limits, images and digit sets, options, elements, fonts, icons, conditions, sources and formats, the firmware's own parts, validation) and the field tables as registry structs (`face_root`, `face_meta`, `face_image`, `face_option`, `face_choice`, `face_element`, `face_cond`, with new enums `face_element`, `face_source`, `face_format`, `face_font`, `face_icon`, `face_cond`, `face_align`, `face_anchor`, `face_option_type`, `asset_origin`). `gen.py` now exports every struct descriptor (`proto_struct_<name>`), so the C parser validates a face's TLV with the same table-driven validator as a message.
- Config keys `ui.clock_face_id` (67, str 32) and `ui.clock_face_opts` (68, **bytes** 32): `bytes` is a new config value type (registry schema, `gen.py`, `proto_config.c`, `cfg_store.c`, the Python reference). `protocol.md` §5 says how a board keeps each face's option values.
- `XFER_BEGIN.origin` (tag 9) and `asset_entry.origin` (tag 5), `protocol.md` §7.2 and §7.5. **Spec and message vectors only: the transfer layer does not store or list it yet (M8b).**
- `contracts/clock-face-gallery.md` + `clock-face-gallery.schema.json` (hand-written; gallery cases are in the vectors and checked by `contract_check.py`).
- Vectors: new `clock_faces.json` (63 parse cases: 16 accepted, 47 refused, one `TOO_LARGE`; 6 option cases; 25 evaluation cases over 35-element and 7-element faces, English and Vietnamese, 12/24 h, °C/°F, missing weather and sensor, negative values; 11 gallery manifests); `messages.json` (origin), `config_values.json` (keys 67, 68, 9 cases). `CONTRACT_VERSION` 0.15.0.

**2. `components/clock_face`** (portable C11, no ESP-IDF or LVGL headers): `cf_parse` (whole-file validation, in-place image pointers), `cf_opts` (resolve and pack option values), `cf_eval` (text, glyph slots, visibility, colour, value of each element for a `cf_data_t` snapshot). Python reference `tools/epager_proto/clockface.py` (build, parse, options, evaluate): the vectors are computed by it and the C component passes every case. Compiler `tools/clock_face` (`face.json` + PNGs to a file, using the board's own checks; `python -m clock_face build|check`).

**3. Renderer and built-in faces:** `components/ui_common/src/ui_face.c` (LVGL objects per element, updates only what changed, Classic fallback with an error counter), hooks in both layouts (the face covers the screen: header, footer, status bar, Classic idle objects and the Classic badge are hidden), the firmware's badge on built-in faces and its update mark on faces that do not draw `update_available`. Built-in faces (sources in `components/ui_common/faces/`, compiled into `ui_face_builtin.c` by `tools/clock_face/gen_builtin.py`): **Paper** `epager.big` (seven-segment image digit set, weather, status icons, three options) and `epager.minimal` (fonts and a line; Light/Dark and date options); **LCD** `epager.big` (mask digits recoloured by a colour option) and `epager.night` (clock font, battery ring arc, weather, room). Fake board: `asset_support` for `clock_face`, the face keys reach the UI, console `face install|select|opt|list|delete`, 4 scenes per board (Paper 13 images, LCD 14) in `tests/golden/<variant>/<size>/face_*.png`.

## Verified (all host, 2026-11-04)

- `cmake -S tests/host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host`: 18 of 18 (new `test_clock_face`: vectors, edge cases, mutation fuzz; 8 face cases in `test_ui`, 4 in `test_ui_lcd`). Longer fuzz: `EPAGER_FUZZ_ITERS=400000 build/host/test_clock_face` clean under ASan/UBSan.
- `tools/.venv/bin/pytest -q tools`: 433 passed (109 in `test_clock_face.py`, 37 golden scenes; the Paper About golden now shows protocol 0.15.0).
- `tools/contract_check.py`: OK (0.15.0); `gen.py --check` and `vectors.py --check` current.
- Coverage (`tools/coverage.py build/cov --min 85`, now including `clock_face`): `cf_eval.c` 96.3 %, `cf_opts.c` 95.8 %, `cf_parse.c` 97.5 %; portable total 97.8 %.
- `tools/xtensa_check.sh` (now includes `clock_face`): zero warnings, 7.3 KB of code (`cf_parse` 3.9, `cf_eval` 3.3, `cf_opts` 0.2).
- Firmware builds, zero warnings: Paper (`-B build/paper-m8a`, image 1,570,016 bytes of a 3 MB slot, 50 % free; the 0.2.0 release was 1,511,424 bytes) and LCD (`-B build/lcd-m8a`, image 2,866,672 bytes of a 4 MB slot, 32 % free). `tools/feature_check.sh` (against these two builds): feature gating OK, Paper mono layout only, LCD colour only. `lvgl_conf_check.py` (with `EPAGER_FW_BUILD=build/paper-m8a`): 7 differences, 0 unexplained (`LV_MEM_SIZE` added, below).
- Images looked at: all 27 new golden frames (Big and Minimal on the Paper, Big and Night on the LCD, the badge, the update mark, installed faces with and without the count field, the damaged / missing / wrong-size fallbacks).

## Not verified

- Anything on a board: the faces' look on the e-paper (ghosting, the 1-bit colour mapping of mid greys), on the LCD (contrast, the arc), draw time, free internal RAM, PSRAM use, battery cost. Checklist item 85 lists the checks.
- BLE upload of a face and its time; `XFER_FINISH` refusing a bad face (the transfer layer's generic header check accepts a face with a damaged description today, the renderer then shows Classic).
- The companion apps against the new vectors (outside this repository).

## Decisions

- **D114, the payload.** TLV for the description, a data area for pixels (a TLV value is at most 65,535 bytes), aligned to 4 from the start of the file; the face's size must equal the board's logical screen size, the face id must equal the asset id; unknown element kinds, sources, fonts, icons and formats are skipped, never refused; limits per board are in `asset-formats.md`. The full list of differences from the plan text is at the end of [clock-faces-plan.md](../clock-faces-plan.md).
- **D115, evaluation is specified, not just the file.** Both implementations produce the same text, glyph slots, visibility and colour for the same snapshot, including rounding (half away from zero) and C integer division; the vectors test it. `mono1` images are masks recoloured by the element's colour; a face never draws a source without a value (time shows dashes).
- **D116, the renderer and memory.** Images are copied into aligned buffers outside LVGL's pool (zero-copy is unsafe: the pixels start at arbitrary offsets and `rgb565a8` sizes can be odd), the file buffer is freed after the copy, faces are built lazily when the idle screen first needs them, and a face needing more than 220 LVGL objects, or more than the pool has free, shows Classic and counts an error.
- **D117, the LVGL pool (finding, decision for M8b).** The Paper's idle screen leaves **about 5 KB** of the 48 KB `LV_MEM_SIZE` pool (internal RAM; measured on the fake board: 5,160 free of 43,944 usable) and a face needs 30-100 objects of about 250 bytes. As the firmware is configured, the Paper would always show Classic. The host builds now use 192 KB so the tests can draw faces (`lv_conf.h`, allowed in `lvgl_conf_check.py` with its reason). M8b chooses from: a larger pool in PSRAM (`LV_ATTRIBUTE_LARGE_RAM_ARRAY`, slower objects), a larger internal pool, fewer layout objects built up front, and measures it.
- **D118, `bytes` config values.** One key needs raw bytes; `bytes` keys have an empty default, are length-checked (`TOO_LARGE`) and never UTF-8 checked; they print as lower-case hex in the logical form.

## Housekeeping

- `build/host`, `build/sim` and `build/cov` were created in an earlier checkout path (`e-pager`) and refused to configure here; they were deleted and rebuilt. The other directories under `build/` (`paper`, `lcd`, ...) still carry that path: use a fresh `-B` directory (`build/paper-m8a`, `build/lcd-m8a`) or delete them.

## For M8b

`asset_support` entries and budgets (Paper 64 KiB / 1 MiB, LCD 1 MiB / 4 MiB, 16 faces); `cf_parse` as the `XFER_FINISH` check (a hook in the transfer layer: it is portable and must not depend on `clock_face`); store and list `origin` with the version; the `.opt` files, selecting a face loading them, delete and replace of the active face; the Settings pages on both layouts (the built-in faces' names are in the face files, `ui_builtin_faces_*` has the ids); `app.c` filling `ui_config_t.clock_face_*`; the LVGL pool (D117); sizes of the LCD image; `CONFIG_EPAGER_FEATURE_CLOCK_FACES` if it pays; hardware checks (checklist 85).
