# M6b: firmware OTA sink, self-check and rollback (2026-11-03)

The firmware half of M6 (transfer + OTA), on the user's request "add ota updater". The transfer state machine, the portable OTA policy (`xfer_ota`: version, battery, progress, pending-verify/rollback states), the contract and the release tooling already existed; both firmware images passed `fw = NULL`, so the boards advertised no `ota` feature and refused firmware transfers. This milestone adds the ESP-IDF side.

## Done

- **`components/ota`** (`epager/ota.h`):
  - `ota_sink.c` (portable, host-tested): the `xfer_fw_sink_t`. Keeps the write position, accepts only sequential writes (`BAD_OFFSET` otherwise), refuses writes past the slot (`NO_SPACE`), reads only what was written, and maps flash results to protocol statuses (`BUSY`, `IMAGE_INVALID`, `SIGNATURE_INVALID`, `INTERNAL`). The flash is an `ota_flash_t` table so tests run on a fake.
  - `ota_esp.c` (ESP-IDF): the `ota_flash_t` on the inactive slot (`esp_ota_get_next_update_partition`). `begin` = `esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES)` (a sector is erased just before it is written, so an interrupted image can continue), `resume` = `esp_ota_resume` at the journaled offset, `end` = project-name check, `esp_ota_end` (image checksum and, with `EPAGER_SIGN`, the signature), `esp_ota_set_boot_partition`. Boot side: `ota_esp_pending_verify`, `ota_esp_mark_valid`, `ota_esp_rollback` (`esp_ota_mark_app_invalid_rollback_and_reboot`), `ota_esp_reboot_in` (one-shot timer).
- **`components/app/src/app.c`** (gated by the new `CONFIG_EPAGER_FEATURE_OTA`, on in `variants/sdkconfig.common`): the core gets the sink, feature `ota`, the self-check bits and the verify timeout. `core_boot` is told whether the bootloader started this image on probation.
- **Self-check (D111):** a new image is kept only after all of `OTA_CHECK_UI` (storage, core and UI came up), `OTA_CHECK_BLE` (the BLE host started, so the next update can still arrive) and `OTA_CHECK_RUNNING` (30 s of main loop without a reset). Wi-Fi and the weather are deliberately not required. Not valid after 120 s: `XFER_OTA_EV_ROLLBACK`. If the UI never comes up the main loop is never reached, so `app_run` rolls back at once. Any reset before the image is marked valid also rolls back (bootloader, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).
- **Finish:** after `esp_ota_set_boot_partition` the core emits `XFER_OTA_EV_REBOOT`; the seen-apps list is flushed (it is otherwise written once a minute) and the chip restarts 1 s later, after the `XFER_FINISH` answer has left. After the reboot `DEVICE_STATUS.ota_state` goes `pending_verify`, then `updated` (or `rolled_back` on the old image).
- **Progress:** the status bar shows `Updating N%` in steps of 5 % (a panel refresh per percent is too many for the e-paper); it disappears when the link drops or no chunk has come for 15 s.
- **Image of another variant:** `ota_esp.c` compares the new image's `project_name` with the running one (`epager_paper` / `epager_lcd`) before `esp_ota_end`, so a Paper image cannot be installed on the LCD board even though it has a valid checksum and signature: `IMAGE_INVALID`.
- No contract change (0.13.0): protocol.md §7 already describes all of it.

## Verified, and how

- `tests/host/test_ota.c` (11 cases, ASan/UBSan, on a fake flash that erases a sector when a sequential write reaches it and can only clear bits): whole image; offset, hole and overlap refusals; slot overflow; every flash/verdict status mapping; finish refused for a short image; read bounds; resume after a simulated reboot at six journal offsets (sector edges and mid-sector) rewriting bytes that are already in flash; resume checks; abort. All 16 host tests pass.
- Both images build with zero warnings: Paper 0x171760 bytes (1.45 MiB of a 3 MiB slot), LCD 0x2addd0 (2.68 MiB of 4 MiB), signed Paper (`-DEPAGER_SIGN=ON`) 0x181000. `tools/xtensa_check.sh` cross-compiles `ota_sink.c` (409 bytes) with `-Werror`.

## Not verified (checklist 83)

- Nothing ran on a board. In particular unverified: `esp_ota_resume` after a real power loss; that the signed config rejects an unsigned or wrong-key image in `esp_ota_end` (the sink reports `SIGNATURE_INVALID` for any `ESP_ERR_OTA_VALIDATE_FAILED` once the SHA-256 of the transfer has matched, which also covers a damaged image structure; the log line says which); the rollback after a failed self-check and after a reset on probation; that an image built without `EPAGER_SIGN` is accepted (development builds) and a signed device accepts only signed ones; the 30 s and 120 s limits; sleep and screen behaviour during a long transfer (the LCD must not sleep mid-update); the throughput and the time of `esp_ota_end` on a 2.7 MiB image; that bonds, NVS and assets survive an update.
- Release images are not built or signed with the release key yet; `releases.md` and the workflows are untouched.
- **Never flash the LCD board without the user's go-ahead in the session.**

## Decisions

- D111 (above): self-check = UI + BLE + 30 s running, 120 s timeout, project-name guard, 5 % progress steps. Left out on purpose: a Wi-Fi/weather check, anti-rollback eFuses, Secure Boot or flash encryption (none is enabled; each needs the user's approval).
