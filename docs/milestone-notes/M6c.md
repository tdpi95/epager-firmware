# M6c: firmware update over Wi-Fi from GitHub Releases (2026-11-03)

Approved the same day as [wifi-ota-plan.md](../wifi-ota-plan.md) (daily check; the person chooses install, remind later or skip). Steps 0-5 are done on the Paper (below); the LCD board has not run it, and what the e-paper showed has not been looked at yet.

## Done

- **Contract 0.14.0** (additive): config key `update.auto_check` (83, bool, default on, both variants); `protocol.md` §5 text and §7.2 step 8 (`XFER_BEGIN` for firmware answers `BUSY` while the board installs an image it fetched over Wi-Fi; before, `INTERNAL`); `releases.md` "Board behaviour"; vectors `config_values.json` (3 cases); generated files; changelog. The Paper's About golden shows protocol 0.14.0.
- **`components/ota`, portable and host-tested (`test_upd.c`, 35 cases, plus the sink cases in `test_ota.c`; ASan/UBSan; 95-99 % line coverage of the new files):**
  - `upd_decide`: reads `manifest.json` with the bounded JSON reader of `components/weather`, checks every rule of `release-manifest.schema.json` (also the shared vectors in `releases.json`), the file-name rule, duplicate `board_id`s, a size that is a real integer from 1 to 2^32-1, and a version that does not fit 32 characters is invalid, not cut. Then the board's rules in order: no image, variant, slot size, contract major, pre-release, not newer.
  - `upd_url_allowed` (https on `github.com` or `*.githubusercontent.com`, no user name, no other port) and `upd_image_url` (the tagged URL for the `latest` manifest).
  - `upd_policy`: the daily schedule (24 h + per-device jitter up to 30 min, first check an hour after boot, nothing without a clock, retry hourly up to 6 tries then a day), remind later (24 h; leaving the prompt unanswered counts as remind later), skip per version (a newer release is offered again), the NVS record `upd`, and a version that is no longer newer than the running firmware is forgotten.
  - `upd_download`: streams the image into a firmware sink in 4 KiB blocks, hashes as it goes, follows a dropped connection with `Range` (up to 3 resumes; accepts only a 206 with the right first byte and total, else starts over), refuses extra or missing bytes, checks SHA-256 before it asks the sink to install, cancel.
  - **One-writer rule on `ota_sink`**: two views (`iface` for BLE, `wifi`); whoever starts an image owns the sink; the other gets `BUSY`; a cleanup `abort` from the other path does nothing; `ota_sink_ble_gone` frees a BLE claim when the link drops.
- **`components/net/src/net_update.c`** (Wi-Fi task, `CONFIG_EPAGER_FEATURE_WIFI_OTA`): `esp_http_client` with the redirects followed by hand (every hop through `upd_url_allowed`; the signed release URL is about 1000 characters, so 2048-byte rx and tx buffers), manifest read, install from a **fresh** manifest (the version must still be the one the person agreed to), progress and errors in `net_status_t`. `net.c`: a check rides on a weather fetch when one is under way, else its own join/SNTP/step/off cycle; no TLS without a clock (`CLOCK`).
- **`app.c`**: `upd_policy` in the main loop, daily check due once a second, `update.auto_check` follows the config key, the battery and version gates of `xfer_ota` before an install, progress through the existing `Updating N%` line (not tied to the BLE link when the image comes over Wi-Fi) with `ui_activity` so the LCD stays on, `xfer_ota_installed` when the image is in (record, restart, `pending_verify`, `updated`/`rolled_back` as for BLE), the BLE sink claim released on disconnect.
- **Debug console** `update status|check|install|cancel|later|skip|due|url URL|interval MIN`.

## Verified, and how

- Host: 17 test programs pass (new: `test_upd`; `test_ota` unchanged in count; `test_transfer` +1 for the `BUSY` answer; `test_config` updated for key 83); `tools/xtensa_check.sh` cross-compiles the portable files with `-Werror`; `contract_check.py`; `pytest tools` (311 passed; one golden regenerated); coverage 97.8 % of the portable core.
- Builds with zero warnings: Paper (0x178140 bytes of a 3 MiB slot), Paper with the dev console, signed Paper (`-DEPAGER_SIGN=ON`), LCD (0x2b4780 of 4 MiB).

## Step 0 on the Paper (2026-11-03)

The dev-console image (0.1.1, unsigned, NVS and assets kept) was flashed to the connected Paper; the network "VNG Internet" was already saved. `update check`: join, three TLS handshakes (github.com twice, then `release-assets.githubusercontent.com`), all validated with the **common** CA bundle, manifest `200`, verdict up to date (release 0.1.1 = the board), about 10 s. No change to the CA bundle is needed. The check time was stored and the daily policy is running on that board.

**Download and install, same day:** a console image built as 0.0.9 (`-DPROJECT_VER=0.0.9`) on the Paper saw the real release 0.1.1 as newer. `update install` re-read the manifest, downloaded the 1,511,424-byte signed image (two handshakes; the unsigned build ignores the signature block), `esp_ota_end` verified it, `esp_ota_set_boot_partition` set it, and the board restarted into it: `download result 0`, 75 s from the command to the restart (about 60 s for the body, about 25 KB/s). Release 0.1.1 has no OTA code, never marks itself valid, and the next reset rolled back: otadata `ota_1` ABORTED, `ota_0` VALID. This exercised the whole path (manual redirects, sink, `esp_ota_*`, progress, restart, rollback of an image that does not validate) except `Range`, cancel, the signature rejection and the screen. The board was reflashed with the normal console image 0.1.1 afterwards. Throughput: Wi-Fi power save off during the install made no difference (reverted). The install now logs its time: 63.1 s for the image, 33.4 s of it waiting for the network (about 45 KB/s) and about 30 s in flash erase and write, hashing and verification, one after the other. Reading and writing in parallel would roughly halve it; not done, 63 s is inside the plan's 30-90 s estimate. About 3 s per TLS handshake.

## Step 4: the screens (as built)

- **The Software update page is an ordinary settings page** (`UI_PAGE_UPDATE` in `ui_settings.c`), so both layouts, keys and touch need no new widgets: a status row (`New version 0.2.0`, `Up to date`, `Checking...`, `Updating 35%`, `No connection`, `Needs newer app`, `Skipped`, ...), a size row, then the actions that fit the state: Install, Remind later, Skip this version, Check now, Cancel. Two new rows in the list on boards with Wi-Fi updates: `Check updates On/Off` (config key 83) and `Software update` (shows the version when one is waiting). English and Vietnamese.
- **The prompt is that page opened by the firmware** (`ui_show_update_prompt`), four rows (status, Install, Remind later, Skip this version) so it is one page on the LCD, the cursor on Install. It opens only from the awake clock screen (never over a notification card, never waking the LCD, not while another screen is up) and counts as remind later at once: backing out, or the settings timing out after a minute, leaves a 24 h snooze. Install keeps the page and shows the progress.
- **The mark** is a text on the clock screen's status line (`Update available`), not an icon (no new bitmap needed on either layout): it stays until the version is installed or skipped.
- App: `update_sync_ui` pushes the policy record and the Wi-Fi task's state to the UI every second and on each action; the actions go through `update_action`.
- Tests: 8 on the Paper (keys) and 3 on the LCD (touch) in `test_ui.c` / `test_ui_lcd.c`; fake-board commands `update STATE ...` and `update prompt`; golden scenes `update` for both boards (11 images) and 13 changed settings images (the list is 14 rows now, so the counter and the last window changed). Python tests: 313 passed.

## Step 5: acceptance on the Paper (checklist 84)

Passed: `Range` resume on the real CDN after a forced drop, cancel, the prompt opening by itself with the 24 h snooze, a wrong-key image refused (`image valid, signature bad`, nothing restarted), the daily check on time with a 2-minute day, `update.auto_check` off and on, skip surviving a reset, a failed check counting a retry. **Finding: the release is signed with the same key as `keys/dev_signing_key.pem`**; the plan asked to confirm which key the boards trust for good: it is that one, so it must be backed up outside the repo and never replaced casually (PLAN section 9). Test hooks added for this: `update drop KIB`, `update auto 0|1`, and the remind time in `update status`.

## First update from a genuine release (2026-11-03)

After v0.2.0 was published, the Paper (console build 0.1.1) found it with `update check` and the prompt opened. `update install` downloaded the signed 1,576,960-byte image in 62.9 s (31.8 s reading the network), installed it in the other slot and restarted into `ePager Paper 0.2.0`; 35 s later `self-check passed: image marked valid`, and the OTA data showed both slots VALID with the new image as the boot image. The board now runs the release image (no debug console).

## Not verified

- Open on a board: flash-write stalls with Wi-Fi and BLE up, heap during a TLS session next to BLE on the LCD, the daily timing, the battery cost. Checklist 84.
- **What the e-paper shows**: the prompt, the mark and the page were driven on the Paper but nobody looked at the panel; only the simulator's golden images were reviewed. The LCD board has not run any of it (flashing needs the user's go-ahead). `BUSY` against a BLE transfer, the 30 % battery refusal, a release of another contract major and a power cut during the download are covered by host tests only.

## Decisions

- D112: Wi-Fi updates as in the plan. Beyond it: `BUSY` for a firmware `XFER_BEGIN` while Wi-Fi installs (a contract wording change in 0.14.0, no new status code); the unanswered prompt counts as remind later; the snooze starts at the next good check when the clock was not set; the install always re-reads the manifest.
