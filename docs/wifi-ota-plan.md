# Plan: firmware updates over Wi-Fi from GitHub Releases (M6c)

**Status: approved 2026-11-03; steps 0-5 done on the Paper the same day (contract 0.14.0, portable logic, Wi-Fi task, screens, hardware acceptance). Open: a look at the e-paper, the LCD board, BLE `BUSY`, the battery refusal.** As built, the prompt is the Software update page opened by the firmware (four rows) and the mark is a text on the clock screen: see [M6c](milestone-notes/M6c.md).

**User decisions, 2026-11-03:** the board checks for an update **once a day** by itself; when one is found the user chooses **Install**, **Remind later** or **Skip this update**. Nothing installs without the user's choice.

## Why

There is no iOS app, and iOS notifications work without one (ANCS), so an iPhone user has no way to send a firmware image over BLE. The board must update itself. The repository is public (2026-11-03) and already publishes signed releases, so the board can fetch them directly. BLE OTA (M6b) stays for the Android app and for development; both paths share one sink, one verification and one rollback.

## Facts this plan rests on

| Fact | How it was established |
| --- | --- |
| Releases `v0.1.0` and `v0.1.1` are published, each with `manifest.json`, `SHA256SUMS`, one `.bin` per board and the ELF files. The `Release` workflow passed for both. | `gh release view`, `gh run list`, 2026-11-03 |
| `manifest.json` is the one in [contracts/releases.md](../contracts/releases.md): `version`, `contract_version`, and per board `file`, `size`, `sha256` (666 bytes for two boards). | downloaded and read |
| `https://github.com/tdpi95/epager-firmware/releases/latest/download/manifest.json` answers 302 to the tagged URL, which answers 302 to `release-assets.githubusercontent.com/...` (Azure Blob, a signed URL of **965 characters**). `latest` never points at a pre-release. | `curl -I` |
| The asset host answers `Range` requests with 206 and `Accept-Ranges: bytes`. | `curl -r 0-99` |
| Downloads from `github.com/.../releases/download` are not part of the REST API, so there is no 60-requests-an-hour limit to design around. | GitHub behaviour, not measured on a board |
| The firmware's HTTP client for the weather uses a 1024-byte header buffer, the common CA bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN`: DigiCert, Sectigo, ISRG, Amazon, ...), a TLS stack in PSRAM, and an 8 KiB task. | `components/net/src/net.c`, `variants/sdkconfig.common` |
| **Not established:** whether the common bundle validates the chain of `github.com` and of `release-assets.githubusercontent.com`. This host's network intercepts TLS (issuer `ZaloGroup Proxy EC CA`), so the real chains cannot be read from here. | step 0 below |
| Boards flashed with 0.1.1 have no firmware sink: **the first OTA-capable release must be flashed over USB** (or the flashing page). | M6b |

## Design

```
 net task (owns the radio, components/net)                       main loop (app.c)                    UI
   daily: join + SNTP, GET manifest -> upd_decide  --net_poll-->  upd_policy (portable, NVS record) --> prompt, status mark,
   on Install: GET image -> upd_download -> ota_sink -> esp_ota_*  <-- user action ---                  Software update page
                                                                   core: xfer_ota gates, progress, reboot
```

### The daily check

- **When:** 24 hours after the last successful check (`last_check_utc`, kept in NVS), plus a small per-device jitter (up to 30 minutes, from the BLE name) so a batch of boards does not call GitHub together. If the clock has never been set, the check waits for the first SNTP or phone time (TLS needs the time anyway); a clock that is earlier than `last_check_utc` counts as due. After a failure (no network, GitHub unreachable) it retries after 1 hour, at most 6 tries, then waits for the next day. The first check happens one hour after boot at the earliest, so a boot loop cannot hammer the network.
- **How:** inside the weather fetch cycle when one is running (the radio is already on: one more request), otherwise as its own cycle (join, SNTP, manifest, off). Same battery rule as a fetch: nothing is checked while the radio is not allowed.
- **Off switch:** a new config key `update.auto_check` (bool, default on; Settings row `Check for updates: Daily / Off`; also settable from the app). Off stops the daily check only: `Check now` still works.
- **Cost to measure:** one TLS connection and a ~700-byte answer a day. The battery log already counts joins (`joins` column), so the cost is visible.
- **What GitHub sees:** the board's public IP and the user agent `ePager/<version> (<board_id>)`. Nothing else leaves the board. README's privacy paragraph gets one sentence.

### What the user sees, and the three choices

When a check finds a newer version that is **not skipped** and **not snoozed**:

1. A status-bar mark (a small up-arrow, built-in icon) shows on the clock and cards. It stays until the update is installed or skipped.
2. A prompt screen appears **once, when the board is awake and idle on the clock screen** (it never wakes the LCD, never covers a notification card, and on the Paper costs one refresh): `Update available - 0.2.0 (2.7 MB)` with
   - **Install**: the install flow below;
   - **Remind later**: the prompt is hidden for 24 hours, then comes back with the next daily check (the status mark stays);
   - **Skip this update**: this version is never offered again and the mark goes away. A **newer** release (for example 0.2.1 after skipping 0.2.0) is offered again. Skipping is stored, so it survives restarts.
3. The choice is reversible: Settings → `Software update` always shows the newest version found, including a skipped one (`Version 0.2.0 available (skipped)`), with `Install` and `Check now`. `Check now` ignores skip and snooze.

Prompt controls: touch boards tap a button (three buttons in the 296 x 240 layout); the Paper moves a bar with short BOOT / PWR and selects with long BOOT, like the settings list (D96, D98). Unanswered (screen timeout or leaving the screen) counts as **Remind later** for that session and the prompt returns at the next idle moment after a day, not at once.

### Install

Always user-confirmed, always from a fresh manifest: the board re-reads `manifest.json` when you press Install, so it never installs from stale data; if the newest version is no longer the one that was shown it asks again with the new version. Then: version and battery gates (the existing `xfer_ota` rules, 30 % without external power), claim the sink (`BUSY` if a BLE firmware transfer is open), join, stream the image, verify, install, reboot. After the restart the existing path reports `pending_verify`, then `updated` or `rolled_back` (D111 self-check, unchanged), and the update record in NVS is cleared (a successful update of version X also clears a skip of X).

### Persistent state (NVS, one record `upd`, versioned)

`last_check_utc`, `retries_today`, `available_version` (newest found, for the mark right after a boot, without Wi-Fi), `skipped_version`, `remind_until_utc`. No manifest data is kept.

## Components

**Reused, not rewritten:** `xfer_ota` (version/downgrade rule, battery gate, progress events, reboot event, `ota_pending` record, mark-valid and rollback), `ota_sink` and `ota_esp` (sequential writes, resume, project-name guard, `esp_ota_end` image and signature check), the weather task's join/SNTP/HTTP code, the `Updating N%` status line, the setup hotspot for entering a Wi-Fi password without an app.

**New, portable and host-tested (in `components/ota`, prefix `upd_`):**
- `upd_manifest_parse` / `upd_decide`: bounded JSON read (reuse `wx_json_*` from `components/weather`; extract it to a shared header in the same change if that dependency looks wrong), then the board's rules in order: invalid manifest, no image for this `board_id`, `variant` mismatch, `size` above the slot, `contract_version` major differs from the board's (the Android app could not talk to it afterwards: show "needs a newer app" and do not install), pre-release version (only with the developer URL override), not newer than the running version. Result: `UP_TO_DATE`, `AVAILABLE{version, size, sha256, file}` or a reason. The existing manifest vectors (`contracts/test-vectors/releases.json`) check the parser's valid/invalid cases.
- `upd_policy`: the schedule and the user's choices (state machine over the NVS record, a `now_utc` that may be unknown, and the decision of each check). Tests with `kv_mem`: due after 24 h and not before; jitter bounds; clock unset, earlier than the record, jumping; failure, retry and the daily cap; auto-check off; skip persists across a "reboot" and a newer version is offered again; remind later hides for 24 h and the mark stays; unanswered prompt; `Check now` overrides skip and snooze; the installed version clears the record; an `available_version` that is not newer than the running one (after an update by BLE) is dropped.
- `upd_download(transport, sink, expected)`: reads in 4 KiB blocks from an `upd_http_t` function table (open with optional `Range`, read, close), writes each block into the sink, hashes on the fly, reports progress, honours cancel, retries a dropped connection up to three times with `Range: bytes=N-` (accepts only 206 with the right `Content-Range`, else restarts from 0), checks size and SHA-256 against the manifest **before** `finish`. Tested on a fake transport: full download, short body, extra bytes, hash mismatch, drop and resume, 200 instead of 206, cancel, slot overflow, sink errors.
- One-writer rule on the sink: whoever calls `begin`/`resume` owns it until `finish`/`abort`; the other path (BLE `XFER_BEGIN` firmware, or Wi-Fi install) gets `BUSY`. Unit-tested.

**New, ESP-IDF glue:**
- `components/net/src/net_update.c`: `esp_http_client` transport. Header buffer 2048 (the redirect is 965 characters), `max_redirection_count` 4, CA bundle attached. The `Location` of every hop must be `https` on `github.com` or `*.githubusercontent.com`, otherwise abort. The asset URL is built from the configured base plus `v<version>/<file>`, never taken from the manifest as a free URL. The scheduler hook runs in `fetch_cycle`.
- `app.c` mediates between the net task, the policy and the core (the core is single-threaded on the main task): the net task publishes state and percent through `net_poll`; the main loop owns `upd_policy`, feeds `xfer_ota_progress` and, when the net task reports an installed image, calls `xfer_ota_installed` (record, reboot event, existing `on_ota`).
- Kconfig: `CONFIG_EPAGER_FEATURE_WIFI_OTA` (needs `FEATURE_WIFI` and `FEATURE_OTA`), `CONFIG_EPAGER_UPDATE_URL` (default the `latest/download/manifest.json` URL above; forks change it). Debug console: `update check | install | cancel | due | skip | later | url <manifest-url> | interval <minutes>` (the URL override is RAM only and also allows pre-releases, so a test release is tried without publishing it as `latest`; `interval` shortens the day for tests).
- While installing: both PM locks held, the LCD kept on, Paper shows `Updating N%` in 5 % steps (existing), the BLE link stays up.

**Contract (0.14.0, additive minor; first in the work, with vectors and changelog in the same commit):** config key `update.auto_check` (bool, default true) in `registry.json`, `protocol.md` and the config vectors; regenerate with `tools/codegen`. The companion app repo must pass the new vectors (an unknown key is already skipped, so an older app is not affected). `DEVICE_STATUS.ota_state` values for "available" and "downloading" are **not** added now: the Android app can learn them in a later minor bump if it wants to show the state.

**UI (both layouts, English and Vietnamese):**
- Settings rows: `Check for updates: Daily / Off` and `Software update` (opens the page).
- The `Software update` page: running version and one state line (`Checking...`, `Up to date`, `Version 0.2.0 available (2.7 MB)`, `... (skipped)`, `Updating 35%`, or an error: `No Wi-Fi network saved`, `Cannot reach the network`, `Battery below 30%`, `Download failed`, `Update rejected`, `Needs a newer app`, `Clock not set`) with `Check now`, `Install` and `Cancel`.
- The prompt screen above, and the status-bar mark (new built-in icon from `tools/assets/icon_gen.py`, mono and colour).
- Fake-board console commands (`update available 0.2.0 size=2822144`, `update prompt`, `update progress 40`, `update fail net`) show every state on the simulator; golden images cover the prompt, the page and the mark, like the weather and Wi-Fi states.

## Security

- **Transport:** TLS with certificate validation; time set first. **Image:** `esp_ota_end` checks the checksum and, in signed builds, the RSA-3072 signature against the key digest built into the *running* image (PLAN D12 option A); the project name must match; the version must be newer; the SHA-256 and size from the manifest must match the stream. **The manifest is not signed** (releases.md): what stops a hostile manifest is the image signature, not the manifest.
- A bad release cannot be pulled back by the board (it never downgrades): publish a higher version. The self-check protects against an image that does not boot, not against one that boots and misbehaves. Because of the daily check a bad release reaches boards within a day *as a prompt*, never as an install.
- **Key story:** release images are signed with the `EPAGER_SIGNING_KEY` secret. A board running a build signed with the local dev key (`keys/dev_signing_key.pem`) rejects a release image, and the other way round; an unsigned dev build accepts any image of its project. Boards meant for Wi-Fi updates therefore start from a release image (flashing page or `esptool`). Confirm that the secret is the key the user wants to keep forever: it cannot change later without a USB reflash of every board.
- No notification data, Wi-Fi password or token goes to GitHub. No automatic install in any phase.

## Phases and acceptance

**Step 0, on a board, before the glue (needs the Paper, and the LCD only after the user's go-ahead). Result 2026-11-03 on the Paper (checklist 84): the common CA bundle validates `github.com` and the release-assets host, and the manifest is read through both redirects. Open: the image download with `Range`, and item 1 below (BLE OTA, checklist 83).**
1. Paper: Wi-Fi and weather run on the board (checklist 60-62) and the BLE OTA positive and rollback cases pass (checklist 83). Without them the sink and the radio are unproven.
2. A throwaway console command does an HTTPS GET of the real manifest and the first 4 KiB of the real image through both redirects with the current CA bundle. Record the result. If validation fails: add the missing root with `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE` (`cacrt_local.pem`) or switch to the full bundle, and note the flash cost.
3. Read the real chains once from a network that does not intercept TLS (`openssl s_client -showcerts`), so the bundle choice is not a guess.

**Step 1, contract 0.14.0:** `update.auto_check` in `registry.json`, `protocol.md`, vectors, `CONTRACT_VERSION`, `CHANGELOG.md`, generated files; `contract_check.py` and the host tests pass.

**Step 2, portable (host, no hardware):** `upd_manifest`, `upd_policy`, `upd_download`, the sink's one-writer rule, tests, `tools/xtensa_check.sh`. Done when the host tests and sanitizers pass with the cases listed above.

**Step 3, glue:** `net_update.c`, the scheduler hook, `app.c` mediation and NVS record, config key handling, Kconfig, console commands. Done when both variants and the signed Paper build compile without warnings and the console `update check` reads the real manifest on the board.

**Step 4, UI:** rows, page, prompt, status mark, strings, fake-board commands, golden images, `pytest tools`.

**Step 5, hardware acceptance (new checklist item 84):** from a release image (0.2.0) update to a test release (0.2.1) over Wi-Fi on the Paper and on the LCD: positive install, progress, reboot, `updated`; the daily check fires on time (use `update interval`), only one prompt per version and day, the prompt waits for an awake idle screen and never wakes the LCD or covers a card; Remind later returns after 24 h, Skip survives a reboot and a newer release is offered again, `Check now` overrides both, `Check for updates: Off` stops the daily check; a bad network (wrong password, no internet, router off mid-download) ends in a clear message with the old firmware running and the daily check retries as planned; a dropped connection resumes with `Range`; a flipped byte fails on the SHA-256; an unsigned or wrong-key image fails at `esp_ota_end`; the Paper image on the LCD fails on the project name; a release whose `contract_version` major differs is refused; a test release that never starts BLE rolls back; power cut mid-download leaves the old firmware running; bonds, settings, seen apps and icons survive; BLE stays connected during the download; below 30 % battery is refused. Record the download time, the flash write time, the free heap during the TLS session, and the battery cost of one check (batlog `joins`/`awake_ms`) and one install. The estimate to confirm is 30-90 s for 2.8 MB, dominated by flash erase and write.

## Risks and unknowns (each is a `TODO(hw-verify)` until measured)

- CA bundle coverage for `release-assets.githubusercontent.com` (Azure Blob) and `github.com` (step 0).
- Flash erase and write stalls the cache for tens of milliseconds a sector while Wi-Fi and BLE run: a BLE supervision timeout or Wi-Fi throughput loss during the install is possible. Measure with the BLE link up.
- Internal RAM: a TLS session next to BLE, LVGL and the Wi-Fi driver, mainly on the LCD (earlier `ESP_ERR_NO_MEM`, D78). The TLS stack is in PSRAM; read the heap during a check and a download.
- A daily radio session costs battery on a board that is otherwise asleep; the check rides on the weather fetch when it can. The cost is measured in step 5; if it is too high the default interval can be lengthened without a contract change (the 24 h is firmware, the key only switches it on and off).
- The Paper cannot sense USB power (no pin), so below 30 % it refuses even while charging; the message says to charge first. An override is a decision for the user.
- GitHub signed URLs expire (about an hour); a resume after a long stall asks for a fresh redirect from the tagged URL, which the retry does anyway. GitHub availability and a future change of the redirect host are outside our control; the allowed host list is one constant.

## Defaults still open (the user may change any)

1. Remind later = **24 hours**.
2. Skip is **per version** (a newer release is offered again).
3. Paper battery gate: **keep 30 % with no USB detection**; or add a "charging, install anyway" confirmation.
4. Which key signs the releases the boards will trust for good (see Key story).

## Documents to change when this is approved

`AGENTS.md` and `docs/PLAN.md` (the Wi-Fi scope line: weather, time **and firmware updates**; D112), `docs/MILESTONES.md` (M6c), `contracts/releases.md` (a "Board behaviour" section: the same manifest, the rules above), `contracts/protocol.md` and `registry.json` (the key), `README.md` (privacy sentence), `docs/hw-checklist.md` (item 84), `docs/milestone-notes/M6c.md` at the end.
