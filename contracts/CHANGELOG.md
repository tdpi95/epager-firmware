# Contract changelog

Rules (protocol.md §9): while 0.x, breaking changes bump the minor version and are marked **BREAKING**; from 1.0.0, additive = minor and breaking = major. Every entry lists the vector files that changed. A contract change, its version bump, this changelog and the vectors land in one commit.

## 0.14.0 (2026-11-03)

Additive, both variants: the board can check for a new firmware release by itself once a day (docs/wifi-ota-plan.md; there is no iOS app to send an image over BLE). An app that does not know the key needs no change (an unknown key is skipped, §9).

- New config key **`update.auto_check`** (83, bool, default true, `lcd` and `paper`): the daily check over Wi-Fi. Off = only when the person asks on the board. The board always asks the person before installing; nothing in the BLE transfer (§7) changes.
- `protocol.md` §5 describes it; `releases.md` gains "Board behaviour" (what a board does with the manifest). §7.2 gains step 8: `XFER_BEGIN` for firmware answers `BUSY` while the board installs an image it fetched over Wi-Fi (before, a board that could not start the image answered `INTERNAL`); no new status code.
- Vectors: `config_values.json` (new `update_auto_check_*` cases); every other vector file only carries the new version.

## 0.13.0 (2026-10-19)

Naming only: the public product, advertising and setup-hotspot name is now **`ePager` / `ePager-XXXX`** (was lower-case `epager-XXXX`). No new message, tag or key; UUIDs and identifiers are unchanged.

- `gatt.json` `advertising.name_prefix` is `ePager-`. An app that falls back to the name prefix compares it case-insensitively, so boards from before 0.13.0 are still found (protocol.md §1).
- Vectors: every vector file only carries the new version.

## 0.12.1 (2026-10-18)

Behaviour only, to save battery (PLAN D103). No new message, tag or key; an app needs no change.

- Config key **`weather.interval_min`** (82): the default is now **60** (was 30). Every fetch joins Wi-Fi and runs a TLS handshake, the costliest thing the board does while idle. The range 15-240 is unchanged.
- Config key **`display.idle_off_s`** (9, LCD): the default is now **60** (was 300). With the screen off the chip sleeps, and the backlight is the main draw while it is on. `display.idle_dim_s` (30) is unchanged.
- A stored value of either key stays as it is.
- Vectors: `config_values.json` (the two defaults); every other vector file only carries the new version.

## 0.12.0 (2026-10-17)

Behaviour of the board, no new messages or fields: **an app's icon is deleted when the app leaves the seen list** (user, 2026-10-17). Until 0.11.0 the board kept every icon until the app deleted it, so icons of forgotten or evicted apps filled the assets partition. Now `APPS_FORGET` (named apps, or all) and the least-recently-seen eviction that makes room for a new app also delete the icon `app_icon_id(app_id)` and its version tag (protocol.md §5, §6.4). An app written for 0.11.0 stays correct if it rechecks `ASSET_LIST` against `APPS_LIST` after connecting, which §6.4 already told it to do; one that keeps its own "uploaded" cache must drop the entry when it forgets an app, and must upload again on `APP_SEEN`.

- Vectors: `app_icons.json` gains `removal` (board scenarios: forgetting one or all apps, eviction, an app with an explicit policy, an app that was never seen).

## 0.10.1 (2026-10-16)

Additive, both variants: an option to hide long numbers on the cards, so a one-time code or a balance cannot be read over a shoulder. An app that does not know the key needs no change.

- Config key **`display.mask_numbers`** (10, bool, default false): the board shows every number of 4 or more digits as `****`; shorter numbers stay (`123` stays, `1234`, `1.234` and `12345` become `****`). Digits joined by a single `.` or `,` belong to one number (`1,234,567` is one number). It applies to the title, subtitle and body of a card; the notification itself is stored unchanged.
- Vectors: `config_values.json` (key 10); every other vector file only carries the new version.

## 0.10.0 (2026-10-15)

**BREAKING** while the contract is pre-1.0 (PLAN D90): `WIFI_SET` now manages up to three saved networks instead of one. The message tags and types are unchanged, but a new SSID no longer erases the old one and `forget: true` plus `ssid` now removes that named network.

- `WIFI_SET {ssid, password}` adds or updates a profile. Updating a profile makes it most recently used; adding a fourth distinct SSID evicts the least recently used profile.
- `WIFI_SET {forget: true}` erases all profiles. `WIFI_SET {ssid, forget: true}` erases only that profile; its password, if sent, is ignored.
- A board scans before a fetch, chooses the strongest visible saved SSID, then falls back through the other visible saved SSIDs after a failed association. `DEVICE_STATUS.wifi_ssid` is the selected or last connected SSID, not a complete saved-network list.
- Vectors: `wifi.json` now covers three profiles, LRU eviction, updating an existing profile and forgetting one or all profiles; every other vector file carries the new version.

## 0.9.2 (2026-10-15)

Behaviour only, for the Paper (PLAN D83): fewer full refreshes (they flash the panel). No new message, tag or key; an app needs no change.

- Config key **`display.full_refresh_every`** (3): the default is now **30** (was 10). A clock redraw counts as one partial refresh toward it, any other redraw (a card, a message, the pairing screen) as a quarter, and a full refresh that falls due while the board is in use waits for a quiet moment (at most until twice the setting). The range 1-100 is unchanged; a stored value stays as it is.
- Vectors: `config_values.json` (the default of key 3); every other vector file only carries the new version.

## 0.9.1 (2026-10-15)

Additive, for the Paper (PLAN D78): the Paper can join Wi-Fi and show the weather like LCD. A 0.9.0 app needs no change: it already skips what a board does not report, and Wi-Fi and weather are still announced by the `wifi` and `weather` feature bits and the `DEVICE_STATUS` tags of 0.9.0.

- Config keys **`weather.enabled`** (80), **`weather.city`** (81) and **`weather.interval_min`** (82) are now valid on the Paper as well (`variants` is `lcd, paper`); on a Paper build without the `weather` feature they are stored and ignored.
- Vectors: `config_values.json` (`weather_enabled_on_paper` now expects OK, new `weather_city_on_paper`, `weather_interval_on_paper`); `messages.json` (new `capabilities_paper_weather`); every other vector file only carries the new version.

## 0.9.0 (2026-10-14)

Additive, for LCD (P1b, PLAN D60-D65): the board fetches the weather itself over Wi-Fi. A 0.8.0 app needs no change: it skips the new config keys (absent on the Paper), the new `DEVICE_STATUS` tags, feature bits and enum types, and never sends `WIFI_SET`.

- New feature bits **`weather`** (12) and **`wifi`** (13) (protocol.md §4). The `transports` bit `wifi` keeps its meaning (the protocol over Wi-Fi, not yet used).
- New request **`WIFI_SET`** (20) / **`WIFI_SET_RSP`** (148) on the Config characteristic: `ssid` (str 1-32), `password` (str 0-63; 1-7 bytes answer `OUT_OF_RANGE`), `forget` (bool); write-only password (protocol.md §5).
- New config keys, LCD: **`weather.enabled`** (80, bool, default true), **`weather.city`** (81, str max 64, default `""` = locate by IP), **`weather.interval_min`** (82, u16 15-240, default 30).
- `DEVICE_STATUS` gains **`wifi_state`** (8, enum `wifi_state`), **`wifi_ssid`** (9, str max 32) and **`weather_state`** (10, enum `weather_state`), notified on change.
- Vectors: new `wifi.json` (board rules for `WIFI_SET`); `messages.json` (`wifi_set*`, `event_device_status_wifi*`, four `wifi_*` invalid cases; `capabilities_lcd` has the two new feature bits); `config_values.json` (keys 80-82); every other vector file only carries the new version.

## 0.8.0 (2026-10-14)

Additive, for LCD (P1b, PLAN D55). A 0.7.0 app needs no change: it skips the new config keys (absent on the Paper anyway), the new `asset_format` value and the `NOTIF_DISMISSED` event (unknown message types are ignored).

- **CR-1** Config key **`display.brightness`** (7, u8 0-100, default 70, LCD): backlight level while active. New keys **`display.idle_dim_s`** (8, u16, default 30) and **`display.idle_off_s`** (9, u16, default 300), LCD: dim, then switch the backlight and the panel off after that long without activity; 0 = never (protocol.md §5).
- `display.orientation` on boards with fewer rotations: the nearest supported one is shown (LCD: 0 and 2) (protocol.md §5; no wire change).
- New `asset_format` **`rgb565a8`** (6): an RGB565 plane then an 8-bit alpha plane. **LCD app icons**: `app_icon` + `rgb565a8`, 32x32, 3,088-byte file, from the app's launcher icon (asset-formats.md). `background` + `rgb565` is now fully specified (296x240 on LCD).
- New event **`NOTIF_DISMISSED`** (99, Notification characteristic): the user dismissed a notification on the board; Android carries its `key` so the app can cancel it on the phone (protocol.md §6.5).
- Vectors: `messages.json` (`capabilities_lcd`, two `event_notif_dismissed_*`), `config_values.json` (defaults and ranges of keys 7-9, orientation 2 on LCD), `app_icons.json` (`rgb565a8_icons`: a packing case and the LCD icon), `transfer.json` (`lcd_app_icon`, `background_rgb565`); every other vector file only carries the new version.

## 0.7.0 (2026-10-12)

- **CR-3** Config key **`ui.background_id`** (66, str max 32, default `""` = none, all variants): selects an installed `background` asset; wins over a theme's background (protocol.md §5, asset-formats.md). Why a key and not only themes: `background` + `mono1` is stable now while `theme_v1` is still provisional.
- **CR-5** Capabilities tag 14 **`pairing_mode`** (bool): readable before bonding, so onboarding shows "enter pairing mode" only when needed (protocol.md §4). Absent from older boards.
- **CR-6** New [`releases.md`](releases.md) and hand-written [`release-manifest.schema.json`](release-manifest.schema.json): GitHub Release layout (tag `v<version>`, one shared version for all boards, `epager-<board_id>-<version>.bin`, `manifest.json`) and the order in which the app refuses or offers an image. Python reference `tools/epager_proto/releases.py`. The public hosting location is still open.
- **CR-7** Two reassembly cases pinned (behaviour unchanged, now written in protocol.md §2.1): a FIRST with SEQ ≠ 0 during a message reports two `SEGMENT_ERROR`s; a zero-length write is `SEGMENT_ERROR` in every state, **including while segments are being ignored** (an app that ignored it there must report it).
- **CR-8** protocol.md §9: the message header layout, `CAPABILITIES` (type `0x7E`, a plain read) with tags 1-5 and their types, the GATT UUIDs, and the `XFER_*` firmware path (already required by §4) are frozen across majors (also in `compatibility.md`).
- **CR-9** `gatt.json`: `advertising.name_prefix` (`epager-`) and `tcp` (`default_port` 47800, `max_value` 512, `ops`). Generated: `PROTO_ADV_NAME_PREFIX`, and both in `reference.md`; the fake board and test client read them from `gatt.json`.
- **CR-4** protocol.md header now says 0.7.0 (it still said 0.5.0).
- Not taken: **CR-1** (`display.brightness`) and **CR-2** (`pet.mascot_id`) wait for the LCD board (backlight range and default, and `mascot_v1`). The app can already hide a setting whose key is absent from `CONFIG_GET` (§5), so no feature bit is planned for brightness.
- Vectors: `segments.json` (`first_nonzero_seq_mid_message`, `zero_length_while_discarding`), `config_values.json` (key 66), `messages.json` (`capabilities_paper` sends `pairing_mode`), new `releases.json` (manifest validity and image selection, scope `app`).

## 0.6.0 (2026-10-11)

Additive: the board shows a clock and, where it has the sensor, temperature and humidity. A 0.5.0 app needs no change: it skips the unknown feature bit, and the new keys are absent from its settings (`CONFIG_GET` with no keys returns them; unknown keys are skipped by the app, §9).

- Registry `features`: bit 11 **`climate`** (temperature and humidity sensor shown on the board, not reported to the app).
- Registry `enums`: **`temp_unit`** (`celsius` = 0, `fahrenheit` = 1, fallback `celsius`).
- Config keys **`ui.clock_24h`** (5, bool, default true) and **`ui.temp_unit`** (6, u8 enum, default `celsius`), both `since` 0.6.0. A board upgraded from an older contract starts them at their defaults.
- protocol.md §4 (meaning of `rtc` and `climate`) and §5 (the display keys).
- Vectors: `config_values.json` (defaults, valid and out-of-range cases for keys 5 and 6), `messages.json` (`capabilities_paper` advertises `rtc` and `climate`).

## 0.5.0 (2026-10-11)

Behavioural, wire format unchanged: the board renders emoji. A 0.4.0 app needs no change (text normalisation is board scope); the only visible difference is that text which showed `□` for an emoji now shows the emoji glyph.

- Registry `text`: new **`emoji_ranges`** (1,378 single code-point emoji in 145 ranges, the coverage of Noto Emoji's monochrome glyphs from U+203C to U+1FAFF without the sequence components below), **`zwj`**, **`regional_indicator_range`** and **`flag_glyph`**. `removed_ranges` gains variation selectors U+FE00-FE0F, the keycap mark U+20E3, skin-tone modifiers U+1F3FB-1F3FF and tag characters U+E0020-E007F. All range tables must be sorted and non-overlapping (the C code binary-searches them).
- protocol.md §8 rules 2 and 4: sequences collapse to their first emoji (a ZWJ removes itself and the next emoji; a pair of regional indicators becomes one U+1F3F3), and the renderable set includes the emoji ranges, so an emoji run no longer becomes `□`. Why not draw sequences: ZWJ and flag glyphs need colour or a ligature table that a small 1-bit panel cannot show anyway.
- Vectors: `text.json` (emoji, ZWJ, flag, variation selector, keycap, tag, budget cases; the old `emoji_run_collapses` case is now `emoji_run`).

## 0.4.0 (2026-10-11)

Additive: per-app notification icons. A 0.3.0 receiver decodes everything (unknown enum value → `unknown`, unknown struct tags skipped); a 0.3.0 board simply does not list `app_icon` and the app does not upload icons to it.

- Registry: enum `asset_type` value **5 `app_icon`**; struct `asset_support` gains optional **`width`** (tag 4, u16) and **`height`** (tag 5, u16), the exact size a fixed-size image must have. The registry schema allows a `description` on enum values (shown in `reference.md`).
- Why `width`/`height` in Capabilities rather than a size fixed in `asset-formats.md`: the icon size depends on the board (24x24 on the Paper, larger and colour on an LCD board), and the app must render icons before uploading them. Advertising it lets one app build serve every board without a per-board table, and it is two optional fields that older decoders skip. `asset-formats.md` still documents the Paper value.
- asset-formats.md: new **`app_icon` + `mono1`** (stable): asset id = lower-case hex of the first 16 bytes of SHA-256(app_id UTF-8), because app IDs (up to 64 bytes, upper case allowed) do not fit the `id` type; size = the advertised `width` x `height` (Paper 24x24, 88-byte file), else `IMAGE_INVALID` at `XFER_FINISH`; the `mono1` row padding bits are 0; rendering guidance (Android small icon, alpha threshold); icon ids are treated like app IDs for logging. The general `IMAGE_INVALID` rule now says "the required size" (advertised size, else the display).
- protocol.md §6.4 (new): the board draws an installed app icon, else its built-in icon for the notification's `category`; the app uploads icons after `APP_SEEN` and reconciles `APPS_LIST` with `ASSET_LIST` after connecting; curated packs (e.g. iOS bundle IDs) allowed; the board never deletes icons on its own. §3 and §4 mention icons and the new `asset_support` fields.
- Vectors: new **`app_icons.json`** (7 id cases, 2 icon files; app+board). `messages.json`: the CAPABILITIES case lists `app_icon` with `width`/`height`; new case `xfer_begin_app_icon`. `transfer.json`: the reference board lists `app_icon` (24x24, `max_size` 88); new scenarios `app_icon_install` and `app_icon_wrong_size` (29 scenarios). `segments.json` (`min_mtu_23` splits the longer CAPABILITIES) and `dispatch.json` (CAPABILITIES case) follow from the new CAPABILITIES. `ancs.json`, `config_values.json`, `crc32.json`, `text.json`: `contract_version` only.

## 0.3.0 (2026-10-11)

Additive: no wire format change (message types, tags, status codes and config keys are unchanged). Specified for the M3 fake board, and binding for firmware too:

- §2.4 **routing**: a message that is not a request (response, event, CAPABILITIES, ERROR), or a request on a characteristic other than its `channel`, is answered with `ERROR {UNKNOWN_MESSAGE, msg_type}` on that characteristic. Then a **security gate** for non-BLE transports: on a link that is not secure every request gets a status-only `INSUFFICIENT_SECURITY` answer (`XFER_CHUNK`: an `XFER_ACK` with handle 0 and offset 0), before body validation.
- §3: events caused by a request (`CONFIG_CHANGED`, `APP_SEEN`) are notified after its response; `DEVICE_STATUS` triggers (battery, external power, bonds, pairing mode, OTA state) and the meaning of `battery_pct` 255 and `notif_count` (shown notifications; registry description added).
- §10 TCP transport: one TCP connection = one connected and bonded BLE link, one client at a time, ATT MTU 517 behaviour, notifications always enabled, read requests and responses (Capabilities, Status, Battery Level), ignored frames, 512-byte frame limit, the insecure mode.
- §2.1 **fix**: segments are at most min(ATT_MTU - 3, **512**) bytes. 0.2.0 said ATT_MTU - 3, which allowed 514-byte writes at MTU 517, above the ATT attribute value limit (found by the M3 test client against the TCP frame limit). Senders that already capped at 512 are unaffected.
- Vectors: new `dispatch.json` (18 cases, board scope). Every other vector file changed only its `contract_version` (and the CAPABILITIES case its version string).

## 0.2.0 (2026-10-11)

Additive: no wire format change (message types, tags, status codes and config keys are unchanged), so 0.1.0 encoders/decoders keep working. Behaviour that 0.1.0 left open is now specified:

- §7 Transfer rewritten with exact rules: handles are per connection and die at disconnect, the next `XFER_BEGIN`, or `XFER_ABORT`; journal and resume semantics (a disconnect journals the offset, a reboot resumes from the last journaled offset); the `XFER_BEGIN` check order (BUSY, UNSUPPORTED, MISSING_FIELD/INVALID_ID, TOO_LARGE, NO_SPACE, VERSION_REJECTED, LOW_BATTERY); firmware `id` must be `firmware` and a missing/invalid `version` is VERSION_REJECTED; semver precedence for downgrades; chunk rules (NO_SESSION ack with offset 0, BAD_OFFSET, TOO_LARGE past `size`, storage errors); `XFER_FINISH` order (NO_SESSION, BAD_OFFSET if incomplete, LOW_BATTERY recheck, CRC, SHA-256, image or asset container check) and that a failed FINISH discards the transfer; session states.
- §5 Config: `CONFIG_GET` ordering, `CONFIG_SET` limit of 64 entries and check order (keys, then `expected_revision`), no revision bump or event when nothing changed, `CONFIG_RESET` rules, `APPS_*` rules (`NO_SPACE`, unknown ids), and paging: `APPS_LIST`/`ASSET_LIST` may return fewer entries than `limit`.
- §6 Notifications: filter rules (policy, category mask, minimum priority, silent, quiet hours across midnight, unset clock), storage/eviction, ANCS priority precedence and unknown-UID handling, `NOTIF_REMOVE` NOT_FOUND.
- Vectors: new `transfer.json` (27 scenarios, app+board). Every other vector file changed only its `contract_version` (and the CAPABILITIES case its version string).

## 0.1.0 (2026-10-11)

First draft, for app development against the fake board. Not yet verified on real phones.

- Framing: 1-byte segment header (FIRST/LAST/SEQ), 4-byte message header (ver, type, req_id), TLV body.
- Messages: PING; CONFIG_GET/SET/RESET; TIME_SYNC; APPS_LIST/SET_POLICY/FORGET; NOTIF_POST/REMOVE/CLEAR; XFER_BEGIN/CHUNK/STATUS/FINISH/ABORT; STORAGE_INFO; ASSET_LIST/DELETE; events CONFIG_CHANGED, XFER_ACK, APP_SEEN, DEVICE_STATUS; CAPABILITIES read value; ERROR.
- 14 config keys, 28 status codes, GATT service `a08a0001-0c0a-4214-8b47-6c1925da5fca`.
- Board text rules (NFC for Latin/Vietnamese, placeholder runs, truncation).
- Vectors: messages, segments, config_values, text, ancs, crc32.
