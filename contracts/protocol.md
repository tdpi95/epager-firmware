# ePager app/firmware protocol

Contract version: see [`CONTRACT_VERSION`](CONTRACT_VERSION). This repository (`epager-firmware/contracts`) is the source of truth. The companion app (`epager-app`) implements this document and must pass every vector in [`test-vectors/`](test-vectors).

Machine-readable sources, in order of authority:

1. [`registry.json`](registry.json): every number in this protocol (message types, field tags and types, limits, enums, status codes, config keys). Validated by [`registry.schema.json`](registry.schema.json).
2. [`gatt.json`](gatt.json): GATT service, characteristic UUIDs, properties and security, the advertised name prefix, and the constants of the TCP development transport (§10).
3. Generated from the registry by `tools/codegen` (never edit by hand): [`schemas/`](schemas) (JSON Schema 2020-12 of the decoded _logical_ form of each message), [`reference.md`](reference.md) (tables), and the C header/tables in `components/protocol`.

Firmware releases (where the app finds OTA images): [`releases.md`](releases.md) with [`release-manifest.schema.json`](release-manifest.schema.json).

The key words MUST, SHOULD and MAY are used as in RFC 2119.

## 1. Transport overview

BLE only in v1. The board is a GATT **peripheral/server** for the app and, on iPhones, also a GATT **client** of Apple's ANCS. One connection at a time; up to 3 bonded phones.

| Characteristic | Properties | Security | Carries |
| --- | --- | --- | --- |
| Capabilities | read | none (readable before pairing) | `CAPABILITIES` message (§6) |
| Config | write, notify | encrypted + bonded | config, apps, time requests/responses; `CONFIG_CHANGED`, `APP_SEEN` events |
| Notification | write, write without response, notify | encrypted + bonded | Android notification requests/responses; `NOTIF_DISMISSED` events |
| Transfer | write, write without response, notify | encrypted + bonded | asset/firmware transfer requests/responses; `XFER_ACK` events |
| Status | read, notify | encrypted + bonded | `DEVICE_STATUS` (read value and event); `ERROR` |
| Battery Level (0x180F/0x2A19) | read, notify | encrypted + bonded | standard, 0-100 % |
| Device Information (0x180A) | read | none | standard: manufacturer, model, firmware revision |

UUIDs are in `gatt.json`. Rules:

- A **request** is written to Config, Notification or Transfer. Its **response** is notified on the **same characteristic**. Events are notified on the characteristic listed in the registry (`channel`).
- Writing to an encrypted characteristic before bonding fails at the ATT layer (Insufficient Authentication/Encryption); the phone OS then starts pairing. The app MUST NOT rely on the board reporting `INSUFFICIENT_SECURITY`; it exists for non-BLE transports.
- The app SHOULD request the largest ATT MTU the phone allows. The board requests 517.
- **Pairing:** LE Secure Connections with MITM. The board is DisplayOnly: it shows a 6-digit passkey that the user types on the phone. New bonds are accepted only while the board is in pairing mode.

Advertising (legacy, 31 bytes each). Connectable, undirected:

- ADV: Flags + 128-bit Service Solicitation for ANCS (`7905F431-B5CE-4E99-A40F-4B1E122D00D0`) + Shortened Local Name `ePager`.
- Scan response: Complete Local Name `ePager-XXXX` (XXXX = last two bytes of the BT MAC, upper-case hex)
  - 128-bit Complete Service UUID list with the ePager service.
- Apps SHOULD filter by the ePager service UUID and MAY fall back to the `ePager-` name prefix (compared case-insensitively, so boards from before 0.13.0 are still found) (`gatt.json` `advertising.name_prefix`). The variant is **not** in the advertisement; read Capabilities.

## 2. Framing

### 2.1 Segments

Every value written to or notified from Config, Notification, Transfer and Status is one **segment**:

```
byte 0     segment header
           bit 7  FIRST  (1 = first segment of a message)
           bit 6  LAST   (1 = last segment of a message)
           bit 0-5 SEQ   (0 for the FIRST segment, then +1 mod 64 per segment)
byte 1..   segment payload (may be empty, but a reassembled message must not be)
```

- A message that fits in one write uses header `0xC0` (FIRST|LAST, SEQ 0).
- The sender splits a message into segments of at most min(`ATT_MTU` - 3, 512) bytes (header included; 512 bytes is the ATT limit for an attribute value).
- Receivers reassemble per characteristic and per direction:
  - FIRST while a message is in progress: the partial message is discarded (`SEGMENT_ERROR` is reported for it) and reassembly restarts with the new segment.
  - FIRST with SEQ ≠ 0: `SEGMENT_ERROR`; segments are then ignored until the next FIRST. If a message was in progress, its own `SEGMENT_ERROR` is reported first (two errors).
  - A non-FIRST segment with no message in progress, or a SEQ that is not the expected one: the message is discarded, `SEGMENT_ERROR`; segments are then ignored until the next FIRST. A non-FIRST segment that arrives while segments are ignored is dropped silently.
  - A zero-length write (no segment header) is `SEGMENT_ERROR` in **every** state, including while segments are ignored; a message in progress is discarded and segments are then ignored until the next FIRST.
  - Reassembled size greater than the characteristic's `max_message` (registry `limits`): the message is discarded, `TOO_LARGE`; segments are then ignored until the next FIRST.
  - A message whose total payload is empty is `SEGMENT_ERROR`.
- Errors that cannot be attributed to a request are reported with an `ERROR` message (§2.4) on the same characteristic, `req_id` 0.
- Capabilities and Status **reads** are plain message bytes without a segment header (ATT long reads handle up to 512 bytes; the Capabilities message is at most `limits.capabilities_max` = 512 bytes).

### 2.2 Message header

```
offset size field
0      1    ver      protocol major version (= CONTRACT_VERSION major; 0 during 0.x)
1      1    type     message type (registry `messages[].id`)
2      2    req_id   little-endian; chosen by the requester, echoed in the response; 0 in events
4      ..   body     TLV fields (§2.3)
```

Message type ranges: requests `0x01-0x5F`; response type = request type `| 0x80`; events `0x60-0x7E` (never answered); `0x7E` CAPABILITIES (read value); `0xFF` ERROR. `0x7F` is reserved. This header layout never changes, not even in a new major (§9).

### 2.3 TLV body

```
tag  u8      0 is invalid
len  u16 LE  length of value in bytes
value
```

Field types (registry `type`):

| Type | Wire | Logical (JSON) |
| --- | --- | --- |
| `u8` `u16` `u32` | exactly 1/2/4 bytes, little-endian | number |
| `i16` `i32` | exactly 2/4 bytes, two's complement LE | number |
| `bool` | exactly 1 byte, 0x00 or 0x01 | true/false |
| `str` | UTF-8, not NUL-terminated, must be valid UTF-8 | string |
| `id` | `str` restricted to `[a-z0-9._-]`, 1-32 bytes, not starting with `.` | string |
| `text` | UTF-8 user-visible text; invalid sequences are **not** an error (sanitised on display, §8) | string |
| `bytes` | raw bytes | lower-case hex string |
| `struct` | nested TLV body (fields from registry `structs`) | object |

- Fields MAY appear in any order; senders SHOULD use ascending tag order (vectors do).
- `repeated` fields appear once per element (logical form: array, even with one element). A non-repeated field that appears twice is `MALFORMED`.
- **Unknown tags are skipped** (forward compatibility), at every nesting level.
- **Enum values** not known to the receiver MUST be accepted and mapped to the enum's documented `fallback` (e.g. an unknown category becomes `other`). Only config values are range-checked against known enum values (§5).

### 2.4 Validation order and status

Receivers MUST report the first error in this order (vectors depend on it):

1. Reassembly: `SEGMENT_ERROR` / `TOO_LARGE` (§2.1).
2. Header shorter than 4 bytes → `MALFORMED`; `ver` ≠ supported major → `UNSUPPORTED_VERSION`; unknown `type` → `UNKNOWN_MESSAGE`. These three are answered with `ERROR {status, msg_type}`; the header's `req_id` is echoed when at least 4 bytes were present. Then (since 0.3.0):
   - **Routing:** a message that is not a request (a response, event, `CAPABILITIES` or `ERROR`), or a request written to a characteristic other than its registry `channel` (`any` = Config, Notification or Transfer), → `ERROR {UNKNOWN_MESSAGE, msg_type}` on the characteristic it was written to, with the header's `req_id`.
   - **Security** (non-BLE transports only, §10): on a link that is not encrypted and bonded, a request is answered with its response type carrying only `status` = `INSUFFICIENT_SECURITY` (`XFER_CHUNK`: `XFER_ACK {handle 0, offset 0, INSUFFICIENT_SECURITY}`).
3. Body, walking TLVs in wire order: fewer than 3 bytes left for a TLV header, a value running past the end, or tag 0 → `MALFORMED`. Unknown tags are skipped. For each **known** tag, in this order: duplicate of a non-repeated field → `MALFORMED`; wrong fixed width or a bool other than 0/1 → `MALFORMED`; length above `max_len` → `TOO_LARGE`; below `min_len` → `MALFORMED`; invalid UTF-8 in `str`/`id` → `INVALID_UTF8`; bad `id` charset → `INVALID_ID`; integer outside `min`/`max` → `OUT_OF_RANGE`; a `struct` value is validated recursively at this point (steps 3-4).
4. At the end of each body (and of each nested struct): a missing `required` field, lowest tag first → `MISSING_FIELD`.
5. Semantic errors from the handler (`NO_SPACE`, `BAD_OFFSET`, …).

A request that fails validation is answered with its normal response type carrying only `status` (and nothing else), except `XFER_CHUNK`, whose errors are reported with `XFER_ACK`.

## 3. Messages

Full field tables: [`reference.md`](reference.md). Summary:

| Request | Channel | Purpose |
| --- | --- | --- |
| `PING` | any | Echo `payload`; latency and throughput tests |
| `CONFIG_GET` | Config | Read listed keys (none = all) and `revision` |
| `CONFIG_SET` | Config | Batch write; optional `expected_revision` |
| `CONFIG_RESET` | Config | All keys to defaults |
| `TIME_SYNC` | Config | UTC seconds + timezone offset (also updates `time.tz_offset_min`) |
| `WIFI_SET` | Config | Add/update or forget Wi-Fi networks the board joins (since 0.10.0, §5) |
| `APPS_LIST` | Config | Page through seen apps |
| `APPS_SET_POLICY` | Config | Set allow/block/default per app |
| `APPS_FORGET` | Config | Remove listed apps (none = all) from the seen list |
| `NOTIF_POST` | Notification | Android: add or update a notification (by `key`) |
| `NOTIF_REMOVE` | Notification | Android: remove by `key` |
| `NOTIF_CLEAR` | Notification | Remove all Android notifications |
| `XFER_BEGIN` / `XFER_CHUNK` / `XFER_STATUS` / `XFER_FINISH` / `XFER_ABORT` | Transfer | §7 |
| `STORAGE_INFO` / `ASSET_LIST` / `ASSET_DELETE` | Transfer | Asset storage management |

| Event | Channel | When |
| --- | --- | --- |
| `CONFIG_CHANGED` | Config | After any committed config change (from any source), with new `revision` and changed keys |
| `APP_SEEN` | Config | First time an app ID is seen (from ANCS or Android); the app then uploads its icon (§6.4) |
| `NOTIF_DISMISSED` | Notification | The user dismissed a notification on the board (since 0.8.0, §6.5) |
| `XFER_ACK` | Transfer | Every `ack_window` bytes, on completion of the last chunk, and on chunk errors |
| `DEVICE_STATUS` | Status | On change of battery/power/pairing/OTA/Wi-Fi/weather state; also the Status read value |

- Events caused by a request (`CONFIG_CHANGED`, `APP_SEEN`) are notified **after** that request's response. `XFER_ACK` events answer `XFER_CHUNK`s, which have no response.
- `DEVICE_STATUS` is notified when `battery_pct`, `external_power`, `bonded_count`, `pairing_mode`, `ota_state` or (since 0.9.0) `wifi_state`, `wifi_ssid` or `weather_state` changes (not for `uptime_s` or `notif_count` alone). The Wi-Fi fields are sent only by boards with the `wifi` feature, `weather_state` only by boards with the `weather` feature. `battery_pct` is 255 when the board has no battery reader or cannot read it; `notif_count` counts the notifications currently shown.

## 4. Capabilities

`CAPABILITIES` (type `0x7E`) is the Capabilities read value: contract version, device name, board ID, variant, firmware version, display (type, width, height, colour depth, refresh class), `features` bitmask (ANCS, Android notifications, assets, OTA, battery, RTC, animation, touch, buttons, since 0.6.0 climate, since 0.9.0 weather and wifi), transports bitmask (BLE now, Wi-Fi later), storage total/free, supported asset types/formats with max sizes (and, since 0.4.0, the exact `width`/`height` of fixed-size images such as app icons), preferred MTU and current config revision.

`rtc`: the board keeps the time across resets (a battery-free RTC chip still loses it when the board loses power; the app SHOULD send `TIME_SYNC` on every connection anyway). `climate` (since 0.6.0): the board has a temperature and humidity sensor and shows the readings on its idle screen in `ui.temp_unit`; the readings are not sent to the app. `wifi` (since 0.9.0): the board can join a Wi-Fi network for its own outgoing requests (`WIFI_SET`, §5); the protocol itself stays on BLE (the `transports` bit `wifi` is reserved for carrying the protocol over Wi-Fi later). `weather` (since 0.9.0): the board fetches the weather itself over Wi-Fi and shows it on its idle screen (§5).

`pairing_mode` (since 0.7.0): true while the board accepts new bonds (the same value as `DEVICE_STATUS.pairing_mode`). Capabilities is readable before bonding, so onboarding can tell the user to enter pairing mode only when it is off. The value is current at the time of the read; Capabilities has no notifications, so the app reads it again to see a change. A board older than 0.7.0 does not send the field: the app then cannot tell and SHOULD show the pairing-mode instruction.

The app MUST read Capabilities first and MUST NOT assume features that are not advertised there. If the board's contract **major** differs from the app's, the app MUST NOT write anything except `XFER_*` firmware updates (so an old board can still be updated) and SHOULD tell the user. To do so across majors it relies on the frozen identity fields (§9): `contract_version`, `device_name`, `board_id`, `variant` and `firmware_version`.

## 5. Config

- The board is the single source of truth (stored in NVS). Keys, types, ranges and defaults are in `registry.config_keys`. A key that does not apply to a variant is absent from `CONFIG_GET` and reported `UNKNOWN_KEY` by `CONFIG_SET` on that variant.
- `entry.value` is the key's value encoded with the key's type (§2.3 wire column; `str` = UTF-8 bytes).
- `CONFIG_GET`: entries in the order the keys were first requested (a key listed twice is returned once); no keys = every key of the variant in ascending id order. Requested keys that are unknown or not applicable are omitted. The response also carries `revision`.
- `CONFIG_SET` semantics:
  - A batch of more than 64 entries is rejected with `TOO_LARGE` (its response would not fit the channel). Entries are checked in wire order. Unknown keys are **skipped** and reported per key as `UNKNOWN_KEY` (forward compatible).
  - Known keys are all validated first (`TYPE_MISMATCH` for a wrong value length, `OUT_OF_RANGE`, `INVALID_UTF8`, `TOO_LARGE`, `READ_ONLY`). If any known key fails, **nothing** is committed and the response carries only the first failing key's status.
  - Then, if `expected_revision` is present and differs from the current revision → `REVISION_CONFLICT`, nothing committed.
  - Otherwise all known keys are committed atomically (a key listed twice takes its last value). The response is `OK` with `revision` and one `key_status` per entry, in wire order. If at least one value changed, `revision` increments by 1 and `CONFIG_CHANGED` lists the changed keys (ascending id, each once); if nothing changed, the revision stays and no event is sent.
- `CONFIG_RESET` sets every key to its default with the same revision/event rules.
- Display keys (since 0.6.0): `ui.clock_24h` picks the board's clock format (24-hour, or 12-hour with AM/PM) and `ui.temp_unit` the temperature unit. Both apply to every variant; a board without the `climate` feature stores `ui.temp_unit` but has nothing to show with it.
- Number masking (since 0.10.1): with `display.mask_numbers` on, the board draws every number of 4 or more digits in a card's title, subtitle and body as `****` (`123` stays; `1234`, `1.234`, `12345` and `1,234,567` become `****`), so a one-time code or a balance is not readable over a shoulder. A number is a run of ASCII digits, where a single `.` or `,` between two digits keeps the run going. It changes only what is drawn: the notification is stored unchanged and the app is not affected.
- Background (since 0.7.0): `ui.background_id` is the id of an installed `background` asset, `""` = none. It applies to every variant and takes precedence over a background named by the active theme. The board does not check that the asset exists when the key is set: an id that is not installed (or later deleted) shows no background. The app offers the setting only when Capabilities lists an `asset_support` entry for `background`, and uploads the image (asset-formats.md) before setting the key. Where the background appears is board-defined (Paper: behind the idle clock screen; LCD: the idle screen).
- Backlight (since 0.8.0, LCD): `display.brightness` is the backlight level in percent while the board is active (0 = off). After `display.idle_dim_s` seconds without activity the board dims the backlight to a board-defined lower level (never above `display.brightness`), and after `display.idle_off_s` seconds it switches the backlight and the panel off; 0 disables either step. Activity is a touch, a key press or a new shown notification; it restores `display.brightness`. Boards without a backlight do not have these keys (the app hides the settings when `CONFIG_GET` does not return them).
- Orientation: `display.orientation` is a rotation in 90-degree steps. A board that supports fewer rotations shows the nearest supported one and keeps the stored value (LCD: 0 and 2, a landscape screen turned by 180 degrees; 1 is shown as 0 and 3 as 2).
- Wi-Fi and weather (since 0.9.0, boards with the `wifi` and `weather` features; LCD, and the Paper since 0.9.1):
  - `WIFI_SET {ssid?, password?, forget?}` saves up to **three** Wi-Fi networks. A request with an existing SSID updates its password and makes it most recently used; a fourth distinct network replaces the least recently used one. `password` empty or absent = open network, otherwise 8-63 bytes (a WPA2/WPA3 passphrase). When Wi-Fi is needed, the board scans once, tries the strongest visible saved SSID first, then tries the other visible saved SSIDs if association fails. After message validation (§2.4) the board answers, in this order: `UNSUPPORTED` without the `wifi` feature; `OK` if `forget` is true (`ssid` absent: erase all saved networks; `ssid` present: erase that network and ignore `password`); `MISSING_FIELD` without `ssid`; `OUT_OF_RANGE` for a 1-7 byte password; else `OK`. The answer comes at once: joining happens later, in the background, and its outcome shows in `DEVICE_STATUS.wifi_state` (vectors: `wifi.json`).
  - The password is write-only: no message returns it, and the board never logs it. The selected or most recently connected network's name is `DEVICE_STATUS.wifi_ssid`; the complete saved-network list is not exposed. Credentials are set only over this encrypted, bonded channel or on the board itself (LCD: a setup hotspot opened from the pairing screen, which is not part of this protocol).
  - `wifi_state`: `unconfigured` (no network stored), `idle` (stored; the radio is off between fetches), `connecting`, `connected`, and the outcome of the last attempt: `auth_failed` (wrong password), `not_found` (network not in range), `no_internet` (joined, but requests fail).
  - `weather.enabled` switches the weather off and on; `weather.city` is the place the board looks up (UTF-8, as a person would type it; `""` = locate by the network's public IP address); `weather.interval_min` is the time between fetches. Which weather and location services the board uses is board-defined; the app SHOULD tell the user that the place name, or the IP address, is sent to them. The readings are shown in `ui.temp_unit` and are not sent to the app.
  - `weather_state`: `off` (`weather.enabled` false), `waiting` (nothing fetched yet, e.g. no network), `ok`, `stale` (the last reading is too old to show), `locate_failed` (the city was not found or the IP lookup failed), `fetch_failed` (the weather service failed).
- Firmware updates over Wi-Fi (since 0.14.0, boards with the `wifi` and `ota` features): `update.auto_check` switches the board's daily check for a new firmware release off and on (default on). The board reads the public release manifest ([`releases.md`](releases.md)) over HTTPS at most once a day, when its Wi-Fi is already in use for the weather or on its own schedule, and tells the person on the board's own screen; it installs nothing without their choice (install, remind later, skip that version). Off stops the daily check only: the person can still check by hand on the board. The check sends the board's public IP address and a user agent with the firmware version and board ID to the release host; the app SHOULD say so when it offers the setting. An update started from the app over BLE (§7) is unaffected by this key.
- Clock faces (since 0.15.0, boards whose `asset_support` lists `clock_face`): `ui.clock_face_id` is the id of the face the clock screen shows: an installed `clock_face` asset ([`asset-formats.md`](asset-formats.md#clock-faces-all-boards)) or one of the board's built-in faces; `""` is the built-in **Classic** face, the board's own clock screen, which also shows whenever the selected face is not installed, was made for another screen size or is damaged. The board does not check that the asset exists when the key is set. `ui.clock_face_opts` holds the active face's option values as pairs of bytes `(option id, value)` (at most 16 pairs); unknown ids are ignored and a value out of range counts as the option's default, so a face update that adds, removes or renumbers options never breaks stored values. The board keeps each face's values on its own, so choosing another face and coming back finds the person's choices again: selecting a face (writing `ui.clock_face_id`) loads that face's stored values into `ui.clock_face_opts` in the same batch unless the batch also writes `ui.clock_face_opts`; writing `ui.clock_face_opts` stores them for the active face; deleting a face deletes its stored values. An app that shows the options reads them from the face's own file (asset-formats.md), which it holds or can download, not from the board. The global settings (`ui.clock_24h`, `ui.temp_unit`, `ui.language`, `weather.enabled`) apply to every face and are not face options.
- Per-app policies (`APPS_*`) are separate from config keys; a policy change also increments `revision` and emits `CONFIG_CHANGED` with no keys. Requests that change nothing keep the revision.
- The seen-apps list holds up to `limits.seen_apps_max` entries (least recently seen evicted first, except apps with an explicit allow/block policy, which are kept). An app is added the first time a notification from it arrives (`APP_SEEN` event) or when `APPS_SET_POLICY` names it.
- `APPS_LIST {offset = 0, limit = 16}` returns `total` and the entries from `offset` in a stable order. `APPS_SET_POLICY` applies its entries in order; an unknown policy value is treated as `default` (enum fallback). If an app must be added while the list is full of apps with explicit policies, the response is `NO_SPACE` and the remaining entries are not applied (earlier ones stay applied). `APPS_FORGET` ignores app IDs that are not in the list. An app that leaves the list, forgotten or evicted, loses its installed `app_icon` (since 0.12.0, §6.4).
- **Paging:** `APPS_LIST` and `ASSET_LIST` MAY return fewer entries than `limit` so the response stays within the channel's `max_message`; the app continues from `offset` + the number of entries received until it has `total`.

## 6. Notifications

Both sources become one board-internal **NotificationEvent**: `id` (board-local u32), `source` (ancs|android), `kind` (added|modified|removed), `app_id`, `app_name`, `title`, `subtitle`, `body`, `timestamp`, `category`, `priority`, `flags`. UI code never sees the source.

### 6.1 Android (`NOTIF_POST`)

- `key` identifies the notification (Android's `StatusBarNotification.getKey()`, or a hash of it if longer than 64 bytes). Posting an existing `key` updates it (`kind = modified`).
- The app MUST truncate `text` fields to their `max_len` on a UTF-8 character boundary before sending; over-long fields are rejected with `TOO_LARGE`.
- `category` uses the shared enum (same numbers as ANCS categories); the app maps Android categories. `priority`: low/normal/high/urgent from Android importance. `flags` use the shared bit meanings. Unknown values fall back to `other` / `normal`. `timestamp` 0 or absent = the board's current time.
- The response carries the board's `notif_id` and `shown` (false when filtered, §6.3).
- `NOTIF_REMOVE` of a `key` the board does not hold → `NOT_FOUND` (the app MAY ignore it). `NOTIF_CLEAR` removes every Android notification.

### 6.2 iOS (ANCS, no app involved)

The board subscribes to Notification Source and Data Source after bonding. For each added/modified notification it requests: AppIdentifier, Title (max 128 bytes), Subtitle (max 128), Message (max 1024), Date; and the app's DisplayName (cached per ANCS session). ANCS mapping:

| ANCS | NotificationEvent |
| --- | --- |
| EventID 0/1/2 | kind added/modified/removed |
| EventFlags (Silent, Important, PreExisting, PositiveAction, NegativeAction) | `flags` bits 0-4 (same order) |
| CategoryID 0-11 | `category` (same numbers; ≥12 → other) |
| IncomingCall category / Important flag / Silent flag | `priority` urgent / high / low, first match in that order; otherwise normal |
| Date `yyyyMMdd'T'HHmmSS` (local time) | `timestamp` (converted with `time.tz_offset_min`; absent or invalid → board's current time) |
| NotificationUID | mapped to a board-local `id`; ANCS UIDs are only valid within one ANCS session |

On a new ANCS session the board drops all ANCS-sourced notifications and rebuilds from the pre-existing notifications iOS re-sends. Data Source responses are reassembled by tuple structure (they carry no fragment markers); a response whose header does not match the outstanding request is discarded. A "modified" event for an unknown UID is handled as "added"; "removed" for an unknown UID is ignored.

### 6.3 Filters and storage (board behaviour)

A notification is **shown** when all of these hold, evaluated when it is added or modified (not re-evaluated later):

1. App policy: `allow` passes, `block` hides, `default` follows `notif.default_app_policy`.
2. Its category bit is set in `notif.category_mask`.
3. `priority` ≥ `notif.min_priority`.
4. It is not flagged silent, or `notif.show_silent` is true.
5. Quiet hours do not apply: `quiet.enabled` is false, the board's clock is unset, or the current local time (UTC + `time.tz_offset_min`) is outside [`quiet.start_min`, `quiet.end_min`). When `end_min` < `start_min` the window spans midnight; `start_min` = `end_min` is an empty window.

Hidden notifications are still stored with their board `id`, so later updates and removals work. The board keeps at most `limits.notif_history_max` notifications in RAM (never in flash); when full, the oldest hidden one is evicted first, otherwise the oldest one. Display text is normalised (§8) and truncated to the field limits on the board.

### 6.4 Icons (since 0.4.0)

Board behaviour: a notification is drawn with

1. the installed `app_icon` asset whose id is `app_icon_id(app_id)` (SHA-256 based, see [`asset-formats.md`](asset-formats.md#app-icons-all-boards)), if there is one; otherwise
2. the board's **built-in icon for its `category`** (one per value of the registry enum `category`; unknown values use `other`'s). Built-in icons are firmware; their artwork is board-defined.

The icon is looked up when the notification is drawn, so an icon installed later applies from the next redraw. The same icon serves notifications from Android (`NOTIF_POST.app_id`) and ANCS (AppIdentifier).

App flow: app icons are uploaded by the companion app with the transfer protocol (§7, `kind` = asset, `asset_type` = `app_icon`), once per app, and stored on the board.

- The app MUST NOT upload icons unless Capabilities lists an `asset_support` entry for `app_icon`, and MUST use that entry's `format`, `width`, `height` and `max_size`.
- On `APP_SEEN` the app uploads the icon of that app if it has not uploaded it to this board (rendered in the entry's format, asset-formats.md: `mono1` from the small icon, `rgb565a8` from the app's launcher icon). Events only reach a connected app, so after connecting the app SHOULD also compare `APPS_LIST` with `ASSET_LIST` (`asset_type` `app_icon`) and upload the icons that are missing.
- The app MAY upload icons for app IDs the board has not seen, e.g. a curated pack keyed by iOS bundle IDs so ANCS notifications get icons too; the id rule is the same.
- `ASSET_LIST` and `ASSET_DELETE` work for icons as for any asset (§7.5).
- **Removal (since 0.12.0):** when an app leaves the seen list, because `APPS_FORGET` named it (or listed none, which forgets all) or because it was evicted to make room for a new app, the board deletes that app's `app_icon` together with its list entry (and its version tag), as `ASSET_DELETE` would. An icon the app uploaded for an app ID that is not in the list (a curated pack, above) is not touched until that app has been in the list and leaves it; the app MAY delete such icons itself. The board sends no event for the deletion, so the app MUST NOT assume an icon is still installed because it uploaded one earlier: `APP_SEEN` always means "upload this app's icon", and after connecting the app compares `ASSET_LIST` with `APPS_LIST` as above.
- Icons are data only: parsed by a bounds-checked loader, never executed (§7).

### 6.5 Dismissing on the board (since 0.8.0)

A board with touch or keys MAY let the user dismiss the notification on screen (LCD: swipe up). The board then removes it from its store, exactly as if the source had removed it, and notifies `NOTIF_DISMISSED {notif_id, source, key?}` on the Notification characteristic: `key` is the `NOTIF_POST` key for Android notifications and absent for ANCS ones.

- Android: the app SHOULD cancel the notification on the phone (when it is allowed to), and MUST NOT post it again because of the dismissal. A `NOTIF_REMOVE` for the key afterwards answers `NOT_FOUND`, which the app may ignore. A later `NOTIF_POST` with the same key adds it again (a new notification).
- ANCS: the board also asks iOS to dismiss it (ANCS Perform Notification Action, negative), when the notification has the NegativeAction flag; the app needs to do nothing.
- Events only reach a connected app: a dismissal while no app is connected is not reported later.

## 7. Transfer (assets and firmware)

Transport-agnostic: the same messages can be carried over HTTP bodies in a later version. Scenario vectors: [`test-vectors/transfer.json`](test-vectors/transfer.json).

### 7.1 Sessions, handles and the journal

- The board runs at most one transfer at a time. `XFER_BEGIN` creates a **session** identified by a `handle` (u8, never 0). The handle is valid on the connection that created it until the next `XFER_BEGIN`, an `XFER_ABORT`, or a disconnect. The app MUST NOT assume handle values.
- The board **journals** the incomplete transfer (kind, asset_type, id, size, crc32, sha256, version, offset, prefix CRC) in non-volatile storage at `XFER_BEGIN`, every few KiB of data (board-defined), and at disconnect. After a reboot it can only resume from the last journaled offset, which may be lower than the last acknowledged one; `prefix_crc32` always matches the offset returned.
- Session states (`xfer_state`): `receiving` until `XFER_FINISH`, then `complete` or `failed` (`verifying` may be reported while a board checks the data asynchronously).

### 7.2 `XFER_BEGIN {kind, id, asset_type?, size, crc32, sha256, version?, restart?, origin?}`

The board checks, in this order, and answers with the first failure (nothing changes on the board):

1. `BUSY`: the session on this connection is `receiving` a different target (kind, asset_type or id).
2. `UNSUPPORTED`: `kind` is not asset/firmware or this board does not support it.
3. Asset: `MISSING_FIELD` without `asset_type`, `UNSUPPORTED` for an asset type the board does not list in Capabilities. Firmware: `INVALID_ID` unless `id` is `firmware`.
4. `TOO_LARGE`: `size` above the asset type's `max_size` (Capabilities), or above the OTA slot.
5. `NO_SPACE` (asset): `size` > free storage + the bytes held by the journaled incomplete transfer.
6. `VERSION_REJECTED` (firmware): `version` absent, not `MAJOR.MINOR.PATCH[-pre][+build]`, or not newer than the running firmware by semver precedence. Development builds MAY allow downgrades.
7. `LOW_BATTERY` (firmware): the board has a battery reader, no external power is detected and the charge is below the board's threshold (Paper: 30 %). Boards without a battery reader skip this.
8. `BUSY` (firmware, since 0.14.0): the board is installing a firmware image it fetched itself over Wi-Fi (§5, `update.auto_check`); try again when `DEVICE_STATUS.ota_state` no longer changes or after the board has restarted.

Then:

- **Resume** if a journaled transfer has the same `kind`, `asset_type`, `id`, `size`, `crc32` and `sha256`, `restart` is not true, and its data is still stored: `offset` is the journaled offset and `prefix_crc32` the CRC-32 of bytes `[0, offset)`. The `version` of this request replaces the journaled one. The app MUST compare `prefix_crc32` with its own file; if they differ it MUST send `XFER_BEGIN` again with `restart` = true.
- Otherwise any journaled transfer is discarded (its stored data deleted) and the new one starts at `offset` 0 with `prefix_crc32` 0.
- Response: `status` OK, `handle`, `offset`, `prefix_crc32`, `max_chunk` (max `data` bytes per chunk) and `ack_window` (bytes between acks).
- **Origin** (since 0.15.0, assets): `origin` says where the file came from: `gallery` (downloaded from the face gallery, [`clock-face-gallery.md`](clock-face-gallery.md)), `local` (the person's own file) or `unknown`. The board stores it with the asset (with the `version`) and returns it in `ASSET_LIST`; the origin of the request that completes a transfer wins, and a resumed transfer takes the new request's. An absent `origin` is stored as absent. The apps' gallery updates replace only `gallery` assets, and never overwrite a `local` asset with the same id without asking the person. Ignored for firmware.

### 7.3 `XFER_CHUNK {handle, offset, data}` and `XFER_ACK {handle, offset, status}`

Chunks are usually written without response; the board answers with `XFER_ACK` events, where `offset` is always the next expected byte. For each chunk, the first matching rule applies:

1. `handle` is not this connection's session, or the session is not `receiving` → `XFER_ACK {handle as sent, offset 0, NO_SESSION}`.
2. `offset` ≠ next expected offset → chunk dropped, `XFER_ACK {BAD_OFFSET}`; the app continues from the acked offset.
3. `offset + len(data)` > `size` → chunk dropped, `XFER_ACK {TOO_LARGE}`.
4. The board cannot store it → chunk dropped, `XFER_ACK {NO_SPACE or INTERNAL}`.
5. Otherwise the data is stored, and `XFER_ACK {OK}` is sent when the bytes received since the last acked offset reach `ack_window`, and when the last byte (`size`) arrives. After `XFER_BEGIN` the last acked offset is the returned `offset`.

An `XFER_CHUNK` that fails validation (§2.4) is answered with `XFER_ACK {status}` (handle 0 if it cannot be read; offset as in rules 1-2). `data` longer than `max_chunk` is accepted if it is valid. The app MUST NOT have more than `ack_window` unacknowledged bytes in flight.

### 7.4 `XFER_FINISH {handle}`

Checked in this order:

1. `NO_SESSION`: not this connection's session, or not `receiving`.
2. `BAD_OFFSET`: not all `size` bytes have been received (the session continues).
3. `LOW_BATTERY` (firmware, rechecked as in §7.2): the session continues; `XFER_FINISH` can be retried.
4. `CRC_MISMATCH`, then `HASH_MISMATCH`: CRC-32 and SHA-256 computed over the **stored** bytes.
5. Firmware: `IMAGE_INVALID` or `SIGNATURE_INVALID` from the ESP-IDF image and signature check. Assets: `IMAGE_INVALID` if the content fails its format check ([`asset-formats.md`](asset-formats.md)).
6. `OK`: the asset is installed atomically (written to a temporary file, verified, then renamed over any previous asset with the same type and id), or the firmware image is set as the boot partition and the board reboots about 1 s later; after reboot `DEVICE_STATUS.ota_state` = `pending_verify`, then `updated` once its self-check passes, or `rolled_back`.

After rules 4-5 the transfer is discarded and the session is `failed`; after 6 it is `complete`. A power loss at any point leaves either the previous asset or the complete new one, never a mix.

### 7.5 Other requests

- `XFER_STATUS {handle}` → `state`, `offset`, `prefix_crc32` of this connection's session, else `NO_SESSION`.
- `XFER_ABORT {handle}` discards the session and any stored data of an incomplete transfer → `OK`, or `NO_SESSION`.
- `STORAGE_INFO` → asset storage `total` and `free` bytes and `asset_count`.
- `ASSET_LIST {offset = 0, limit = 16}` → `total` and installed assets (type, id, size, and the `version` and, since 0.15.0, the `origin` sent at `XFER_BEGIN` if any) in a stable order; paging as in §5.
- `ASSET_DELETE {asset_type, id}` → `OK` or `NOT_FOUND`.

CRC-32 is the IEEE 802.3 / zlib CRC (reflected, polynomial 0xEDB88320, init and xorout 0xFFFFFFFF; `crc32("123456789") = 0xCBF43926`). SHA-256 per FIPS 180-4. Assets are data only and never executed; see [`asset-formats.md`](asset-formats.md).

## 8. Text handling on the board

Applied to `text` fields and ANCS strings before display (vectors: `test-vectors/text.json`, board scope):

1. Invalid UTF-8 sequences become U+FFFD.
2. Controls: `\n` kept; `\t` becomes a space; `\r\n` and a lone `\r` become `\n`; other C0/C1 controls, DEL and the invisible format characters in `text.removed_ranges` (soft hyphen, zero-width spaces, BOM, variation selectors, the keycap mark U+20E3, emoji skin-tone modifiers, tag characters) are removed. Emoji sequences collapse to their first emoji: a ZWJ (`text.zwj`, U+200D) is removed together with the next code point if that is an emoji (`text.emoji_ranges`; applied again after every ZWJ, so `👩‍❤️‍👨` shows `👩`); a ZWJ followed by anything else is just removed. Regional indicators (`text.regional_indicator_range`) are taken in pairs: the first becomes the flag glyph `text.flag_glyph` (U+1F3F3) and the second is dropped, so `🇻🇳` shows `🏳` (a leftover odd indicator also shows `🏳`).
3. At most `text.max_combining_run` (8) combining marks are kept after a base character; extra marks are dropped. Then canonical composition (NFC) using the precomposed letters in `text.compose_ranges` (Latin and Vietnamese), including out-of-order combining marks (e.g. `e` + U+0302 + U+0323 → `ệ`). Combining marks that cannot be composed onto a renderable letter are dropped.
4. Any maximal run of code points the font cannot render (reference set: registry `text.renderable_ranges` plus `text.emoji_ranges`: the Latin and Vietnamese letters and punctuation, and 1,378 single code-point emoji) (other scripts, emoji not in the set, U+FFFD) becomes **one** placeholder (U+25A1 `□`). Emoji are drawn as 1-bit glyphs of the board's font, so a sequence never shows its skin tone, gender or colours.
5. If a budget is given: truncation to the budget in bytes on a code-point boundary, with `…` (U+2026) appended within the budget.

## 9. Versioning and compatibility

- `CONTRACT_VERSION` is semver. While **0.x**, breaking changes bump the minor version and are marked **BREAKING** in [`CHANGELOG.md`](CHANGELOG.md). From 1.0.0: additive = minor, breaking = major.
- Additive (minor): new message types, new optional fields/tags, new enum values, new config keys, new status codes, new capability fields.
- Breaking (major): changing a field's type, tag, meaning or requiredness; removing anything; changing framing.
- Both sides ignore unknown message types (events), unknown tags and unknown config keys, and accept unknown enum values via fallbacks. Unknown **status codes** are treated as a generic failure.
- **Frozen across majors** (since 0.7.0), so that an app can identify a board of any other major, tell the user whether the app or the firmware is older, and offer the right firmware image (§4, [`releases.md`](releases.md)): the GATT service and characteristic UUIDs (`gatt.json`); that Capabilities is a plain read of one message; the message header layout (§2.2: `ver`, `type`, `req_id` LE, then TLVs as in §2.3); the `CAPABILITIES` type `0x7E`; and its tags 1-5 with their types: `contract_version` (str), `device_name` (str), `board_id` (str), `variant` (u8, enum `variant` values never renumbered), `firmware_version` (str). Also the `XFER_*` messages, fields and rules used for firmware (§7), so any board can be updated by any app. A new major may add fields but MUST NOT change these.
- Compatibility of released firmware and app versions: [`compatibility.md`](compatibility.md).

## 10. Development transport (TCP), fake board only

So the app can be developed without hardware, the fake board (`tools/fake_board`) also listens on TCP (default port 47800). The port, the value limit and the `op` values are in `gatt.json` `tcp` (since 0.7.0). Each TCP frame is:

```
u8   char_id    1 Capabilities, 2 Config, 3 Notification, 4 Transfer, 5 Status, 6 Battery Level
u8   op         0 read request, 1 write, 2 write without response, 3 notify, 4 read response
u16  len LE     at most 512
...  value      exactly what the BLE characteristic value would be (segment header included)
```

Rules (0.3.0):

- One TCP connection is one BLE connection: connecting is connecting **and** bonding (the link is treated as encrypted and bonded), closing it is a disconnect (transfer handles end, §7.1). The fake board serves one client at a time and closes any additional connection immediately.
- The link behaves as ATT MTU 517: the app sends segments of at most 512 bytes (§2.1), and the board's notifications use the same limit. (The fake board can be started with a smaller MTU to exercise segmentation; the app must accept segments of any size.)
- Every characteristic's notifications are enabled; there are no CCCD writes.
- The app reads with op 0; the board answers with op 4 carrying the read value (Capabilities, Status = `DEVICE_STATUS`, Battery Level = 1 byte). Ops 1 and 2 behave the same (there is no write response). The board sends op 3 for notifications, including Battery Level when it changes.
- Frames with an unknown `char_id` or `op`, reads of a characteristic that is not readable and writes to one that is not writable are ignored. A frame with `len` above 512 closes the connection.
- There is no pairing over TCP. The fake board can be told to treat the link as **not** secure; the board then answers requests with `INSUFFICIENT_SECURITY` (§2.4) and reads of Status and Battery Level with an empty value.

This transport is never compiled into firmware.
