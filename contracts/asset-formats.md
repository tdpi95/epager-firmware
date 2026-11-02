# Asset formats

Contract 0.8.0 (`app_icon` and `asset_support` `width`/`height` added in 0.4.0; backgrounds selected
with the `ui.background_id` config key since 0.7.0; `rgb565a8` and the LCD formats since 0.8.0). Assets are uploaded with the transfer protocol (protocol.md §7, `kind` = asset) and
are **data only**: the board parses them with bounds-checked loaders and never executes them.
Which types and formats a board accepts, their maximum sizes and, for fixed-size images, their exact
`width` and `height` are in `CAPABILITIES.asset_support`. Asset ids follow the `id` type
(`[a-z0-9._-]`, 1-32 bytes); `app_icon` ids are derived from the app ID ([below](#app-icons-all-boards)).

Status of each format: **stable** = implemented against vectors; **provisional** = layout may
still change (minor bump while 0.x); **deferred** = defined when its feature is built.

## Container (all assets): stable

Every asset file starts with a 16-byte little-endian header:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | ASCII `ESPA` |
| 4 | 1 | `header_version` | 1 |
| 5 | 1 | `asset_type` | registry enum `asset_type`; must equal `XFER_BEGIN.asset_type` |
| 6 | 1 | `format` | registry enum `asset_format` |
| 7 | 1 | `flags` | 0; non-zero → rejected in this version |
| 8 | 2 | `width` | pixels, 0 if not an image |
| 10 | 2 | `height` | pixels, 0 if not an image |
| 12 | 4 | `payload_len` | must equal file size − 16 |
| 16 | … | payload | format-specific |

`XFER_FINISH` returns `IMAGE_INVALID` if the header is wrong, the format is not supported for the
type on this board, the dimensions are not the required size, or the payload size does not match the
format. The required size of an image is the `width` x `height` of the type's `asset_support` entry
when the board sends them (always for `app_icon`), otherwise the display size. Integrity (CRC-32,
SHA-256) is checked by the transfer layer before this.

## App icons (all boards)

### `app_icon` + `mono1`: stable (since 0.4.0)

A per-app notification icon (protocol.md §6.4). Uploaded by the companion app once per app; the
board shows it for every notification from that app (Android or ANCS) and otherwise falls back to
its built-in category icon.

- **Asset id** = `app_icon_id(app_id)` = lower-case hex of the first 16 bytes of
  SHA-256 over the app ID's UTF-8 bytes (32 characters, always a valid `id`). The app ID is hashed
  exactly as the board reports it in `APP_SEEN` / `APPS_LIST` (= `NOTIF_POST.app_id` or the ANCS
  AppIdentifier): case-sensitive, no trimming or normalisation. Example: `com.zing.zalo` →
  `3fb7c0466d433308884e5268cff76036`; `com.apple.MobileSMS` and `com.apple.mobilesms` are different
  ids. Vectors: [`test-vectors/app_icons.json`](test-vectors/app_icons.json).
  Hashing is needed because app IDs are up to 64 bytes and may contain upper case, which the `id`
  type does not allow. The board cannot check the mapping: an icon whose id matches no app is stored
  but never shown.
- **Size**: exactly the `width` x `height` of the `asset_support` entry for `app_icon` (boards MUST
  send both for this type). The header's `width`/`height` must equal them; anything else is
  `IMAGE_INVALID`. A board lists one `app_icon` entry per format it accepts.
  **Paper: 24x24, `mono1`, payload 72 bytes, file 88 bytes (`max_size` 88).**
  **LCD: 32x32, `rgb565a8`, payload 3,072 bytes, file 3,088 bytes (`max_size` 3088).** Other
  boards document theirs here.
- **`mono1` layout** (same as `background` + `mono1`): rows top to bottom; each row is
  `ceil(width / 8)` bytes; the most significant bit is the leftmost pixel; **bit 1 = white, bit 0 =
  black**; the padding bits at the end of a row are 0 and ignored by the board.
  `payload_len` = `ceil(width / 8) * height`.
- **Rendering (app, recommended)**: on Android use the notification's small icon
  (`Notification.getSmallIcon()`), a monochrome silhouette defined by its alpha channel. Scale it to
  fit `width` x `height` keeping the aspect ratio, centre it, then pixels with alpha >= 50 % are black
  and the rest white (no dithering). For iOS bundle IDs (ANCS) the app may ship a curated pack.
  The board MAY invert the icon to suit its layout (e.g. white on a black header).
- **`rgb565a8` layout** (since 0.8.0): an RGB565 plane then an alpha plane, both rows top to bottom
  with no padding: `width * height` pixels of 2 bytes, **little-endian** RGB565 (bits 15-11 red,
  10-5 green, 4-0 blue), then `width * height` bytes of alpha (0 = transparent, 255 = opaque). The
  colour of a fully transparent pixel is ignored. `payload_len` = `3 * width * height`.
  (This is LVGL's `RGB565A8` image layout.)
- **Rendering `rgb565a8` (app, recommended)**: the app's launcher icon (Android: the package's
  icon; iOS bundle IDs: a curated pack), scaled to `width` x `height` with its alpha kept (no
  background fill), area-averaged, colours rounded to RGB565.
- **Replacing**: uploading the same id again replaces the icon atomically (protocol.md §7.4). The app
  MAY put a content tag (e.g. the first 8 hex characters of the file's SHA-256) in
  `XFER_BEGIN.version` and compare it with `ASSET_LIST` to detect changed icons.
- **Privacy**: an icon id identifies its app (hashing known app IDs reverses it), so the board treats
  icon ids like app IDs: never logged unless content logging is enabled.

## Paper (200x200, 1-bit)

### `background` + `mono1`: stable

- Selected with the config key `ui.background_id` (protocol.md §5, since 0.7.0), which wins over a
  background named by the active theme. Shown behind the idle clock screen.
  TODO(firmware): the Paper UI does not draw backgrounds yet; until it does the key is stored only.
- `width` = `height` = 200 on the Paper (other boards: their display size).
- Rows top to bottom; each row is `ceil(width / 8)` bytes; the most significant bit is the leftmost
  pixel. **Bit 1 = white, bit 0 = black** (the driver converts if its controller differs).
- `payload_len` = `ceil(width / 8) * height` = 5000 bytes for 200x200 (5016-byte file).
- `tools/assets` converts PNG to this format (threshold or Floyd-Steinberg dithering).

### `app_icon` + `mono1`: stable

24x24 (`asset_support` `width`/`height`), 88-byte file; see [App icons](#app-icons-all-boards).

### `font` + `font_v1`: provisional

Defined in the Paper UI work. Requirements already fixed: full Vietnamese and Latin coverage (the
ranges in registry `text.renderable_ranges`), including the placeholder U+25A1 and the ellipsis
U+2026; `width`/`height` = 0; maximum size per `asset_support`. Candidate payload: an LVGL binary
font produced by `lv_font_conv --format bin`, parsed by a bounds-checked loader (LVGL's own loader
is reviewed for untrusted input before adoption).

### `theme` + `theme_v1`: provisional

TLV payload (same TLV encoding as protocol.md §2.3) with keys such as background asset id, font
asset id, layout options. Defined with the UI. A non-empty `ui.background_id` overrides the theme's
background.

## LCD (296x240, RGB565)

### `background` + `rgb565`: stable (since 0.8.0)

- Selected with `ui.background_id` (protocol.md §5); shown behind the idle screen.
- `width` = 296, `height` = 240 (the display size; `asset_support` sends no `width`/`height` for
  `background`, so the display size applies).
- `width * height` pixels, rows top to bottom, no padding, 2 bytes each, **little-endian** RGB565
  (bits 15-11 red, 10-5 green, 4-0 blue). `payload_len` = `2 * width * height` = 142,080 bytes
  (142,096-byte file).
- The idle screen draws its text on the image as it is: pick light, low-contrast images (the theme
  is light).

### `app_icon` + `rgb565a8`: stable (since 0.8.0)

32x32, 3,088-byte file; see [App icons](#app-icons-all-boards).

