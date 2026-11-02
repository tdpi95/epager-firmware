# M6a: asset sink and app icons (2026-10-17)

The asset half of M6 (transfer + OTA), done on its own at the user's request ("start app icon milestone with current plan", scope "asset sink only"). Before it, both firmware images mounted the `assets` partition but gave the core no `blob_storage`, so `CAPABILITIES` listed no assets, every upload was refused and cards could only show the built-in category icons. The firmware sink, signing, rollback and the battery gate are still M6 and not started. Decision: PLAN D100.

## Done

- **`blob_fs`** (`components/transfer/src/blob_fs.c`, `epager/blob_fs.h`): `blob_storage` over a directory, POSIX calls only. One file stays open between writes; every other operation, `flush` of that file and a write to another file sync and close it first (LittleFS refuses to rename an open file). A write that would not fit writes nothing and creates no file. `list` shows regular files only (the battery log's `/assets/diag` is not an asset). Names with `/`, a leading `.` or more than 40 characters are refused.
- **`components/app/src/app.c`**: when the mount worked, the core gets the store, feature `assets`, storage total and free (from `esp_littlefs_info`) and one `asset_support` entry for `app_icon`: Paper 24x24 `mono1` (88 B), LCD 32x32 `rgb565a8` (3,088 B), chosen by the display's colour depth. `on_asset` calls `ui_assets_changed`, and the UI reads icons from the same store. If the mount failed the board logs it and keeps the built-in icons.
- Not advertised: backgrounds, themes, fonts. No firmware code reads them, so an app would upload files nobody draws.
- No contract change (0.11.0). The contract already described all of it (protocol.md §6.4, asset-formats.md).

## Verified, and how

- Host tests: `test_blob_fs` (12 cases: write, append, overwrite inside, gaps refused, no-space writes nothing, atomic replace by rename, idempotent remove, flush and the kept-open file, another file closing the first, list order and sizes, bad names, a missing directory) and `test_app_icon_upload_on_a_file_system` in `test_transfer` (an icon upload cut at 40 bytes, resumed from the journal after a simulated reboot, then replaced, all through the real transfer messages on a directory). All 15 host tests pass under ASan and UBSan; `blob_fs.c` has 94 % line coverage (portable core total 98 %).
- Both firmware images build with no warnings (Paper 1.42 MiB of a 3 MiB slot, LCD 2.67 MiB of 4 MiB). `tools/xtensa_check.sh`, `pytest tools` (294 passed) and `contract_check.py` pass.

## Not verified

- Nothing ran on a board or over BLE (checklist 76): LittleFS behaviour with the kept-open file (rename after close, sync on close), time per chunk, the partition's real free space in `CAPABILITIES`, an icon surviving a reset, the UI drawing an icon read from LittleFS, the LCD's colour icon.
- `tools/test_client` cannot upload icons to a real board yet (only to the fake board over TCP).

## Update: icons follow the seen list (D101, contract 0.12.0)

The board now deletes an app's icon when the app leaves the seen list (`APPS_FORGET`, or eviction for a new app): `npipe` `on_app_removed`, `xfer_asset_remove` (shared with `ASSET_DELETE`), core hook. Contract first: protocol.md §5/§6.4, CHANGELOG 0.12.0, vectors `app_icons.json` `removal` (10 scenarios; reference model `tools/epager_proto/apps.py`, C runner `test_icon_removal`; a mutation check without the hook fails 8 of them). One golden image changed: the About screen shows 0.12.0. Host tests, `pytest tools` (295), `contract_check.py` and both firmware builds pass. The LCD board was flashed with the build before this change (it still keeps icons after a forget) and has not been reflashed. The companion app must follow §6.4: no "uploaded" cache across a forget, upload on every `APP_SEEN`; its vector run now sees a new `removal` key it can ignore.

## Update: first upload on the LCD board failed (2026-10-17)

The companion app's icon upload to the real LCD board ended in `XFER_ACK INTERNAL`. Not the partition table (read back from the flash: same as `partitions.csv`), not the mount (`assets: 24576 of 8257536 bytes used`), not a stale image (`verify_flash` against `build/lcd` matched). The cause was in `blob_fs.c`: `fs_write` took the size of the open file from `fstat()`, and esp_littlefs implements `fstat()` as `lfs_stat(path)`, which has no entry for a file created or grown since its last sync (ENOENT on chunk 2; `ack_window` is 4096 B, so a 3088-byte icon showed only that error as "the first chunk"). POSIX on the host reports the in-memory size, so the host tests and the fake board never saw it. Now `blob_fs` tracks `open_size` itself. `test_blob_fs` links with `-Wl,--wrap=fstat` and a wrapper that always fails, so any use of `fstat` there breaks (`test_chunks_to_an_open_file_need_no_fstat`; it failed before the fix). A failed write now logs `asset write failed at <step>: errno N (...)` through `blob_fs_set_trace` (steps usage, stat, open, seek, write, sync; no asset names or app ids). No contract change. Reflashed: the upload from the phone app worked and the LCD drew the icon (checklist 76 has the rest still open).

## Decisions

- D100 (above). Left out on purpose: the per-notification icon (PLAN D34 rejected icon bytes in `NOTIF_POST`), a background picker and the firmware sink.
