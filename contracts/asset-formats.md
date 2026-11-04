# Asset formats

Contract 0.15.0 (`app_icon` and `asset_support` `width`/`height` added in 0.4.0; backgrounds selected
with the `ui.background_id` config key since 0.7.0; `rgb565a8` and the LCD formats since 0.8.0; clock faces
since 0.15.0). Assets are uploaded with the transfer protocol (protocol.md §7, `kind` = asset) and
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


## Clock faces (all boards)

### `clock_face` + `clock_face_v1`: stable (since 0.15.0)

A clock face describes what the clock screen draws: shapes, images, numbers written with image digit sets,
text in the board's built-in fonts and built-in icons, placed on a pixel grid, and the settings the person may
change without editing it. A face is **data, never code**: the board reads it with a bounded parser, draws it
with its own drawing code and cannot be made to run anything, reach the network, the flash or notification
content. Selected with `ui.clock_face_id`, customised with `ui.clock_face_opts` (protocol.md §5). The
built-in face **Classic** (the board's own clock screen) is always there: it shows whenever the selected face
is missing, the wrong size or fails any check below. Vectors: [`test-vectors/clock_faces.json`](test-vectors/clock_faces.json).
The tools that build faces and the gallery they are shared through are described in
[`clock-face-gallery.md`](clock-face-gallery.md).

#### File

The container (above) with `asset_type` 6 and `format` 7. `width` x `height` in the header are the **logical
screen size the face is made for**: the board draws the face only when its own logical screen size (after
`display.orientation`) is exactly this; for another size Classic shows. One file carries one board's screen:
the gallery publishes one file per board. The asset id is the face's `meta.id` (the transfer's `id` and the
file's `meta.id` must be equal, else `IMAGE_INVALID`).

The payload is:

| Offset in payload | Size | Contents |
|---|---|---|
| 0 | 4 | `tlv_len`, little-endian |
| 4 | `tlv_len` | the description: a TLV body (protocol.md §2.3) of the registry struct `face_root` |
| 4 + `tlv_len` | 0-3 | zero padding so the data area starts at a multiple of 4 from the start of the **file** (the header is 16 bytes, so the padding makes `20 + tlv_len` a multiple of 4) |
| after the padding | rest | the data area: the pixels of every image, in image-table order, with no gaps |

The data area's length must equal the sum of the image sizes exactly. Unknown TLV tags are skipped at every
level (forward compatibility); duplicates of a non-repeated field, wrong widths and the other TLV rules of
§2.4 make the file invalid. The registry struct tables (`face_root`, `face_meta`, `face_image`, `face_option`,
`face_choice`, `face_element`, `face_cond` in [`reference.md`](reference.md)) list every tag.

#### Limits

| | Paper | LCD |
|---|---|---|
| `max_size` of the file (`asset_support`) | 65,536 | 1,048,576 |
| image formats | `mono1` | `mono1`, `rgb565`, `rgb565a8` |
| `arc` elements | no | yes |
| images | 32 | 32 |
| options / choices per option | 8 / 8 | 8 / 8 |
| elements | 48 | 48 |
| digit sets | 8 | 8 |

Image width and height are 1-400. A board may refuse more than 16 installed faces or a total above its
budget (`NO_SPACE`). A file above any limit is `IMAGE_INVALID` at `XFER_FINISH`.

#### Meta

`meta` is required: `id` (an `id`), `name` (English, 1-32 bytes), `name_vi` (optional), `author` (optional),
`version` (semver of the face itself), `min_contract` (optional; a board whose contract is older refuses the
face with `IMAGE_INVALID`) and `update_anchor` (below).

#### Images

One `image` per picture: `format`, `width`, `height`, and for a digit-set member `set` (1-8) and `slot`.
Pixels, in the data area in table order:

- `mono1`: as above (rows top to bottom, `ceil(width / 8)` bytes per row, most significant bit leftmost, padding
  bits 0). In a face a `mono1` image is a **mask**: the pixels with bit 0 (black) are ink and are drawn in the
  element's colour, the pixels with bit 1 (white) are transparent. On the LCD an element's colour recolours it.
- `rgb565`: `width * height` little-endian RGB565 pixels, opaque, drawn as they are (an element's colour does
  not apply).
- `rgb565a8`: as the icon format above (RGB565 plane, then an alpha plane), drawn as it is.

A **digit set** is the images that share a `set` number. Slots: 0-9 the digits, 10 colon, 11 `AM`, 12 `PM`,
13 minus, 14 degree sign, 15 percent sign. A set a `number` element uses must hold all ten digits, each slot
once, all digits of the same height. The other slots are optional; a character with no image in the set is
left out. Images may have different widths (a narrow `1`, a wide colon).

#### Options

At most 8. Each has a stable `id` (a byte, unique in the face and kept across versions of the face), a `type`,
a `label` (English, and `label_vi`) and a `default`. The value of an option is one byte:

- `toggle`: 0 or 1, no `choice` entries.
- `choice`: the index of one of 1-8 `choice` entries (each with a `label` and `label_vi`).
- `colour`: the index of one of 1-8 `choice` entries that each carry an RGB565 `colour`.

The `default` is a valid value. A person's values are stored by the board in `ui.clock_face_opts` as
`(id, value)` byte pairs; an unknown id is ignored and a value that is out of range counts as the default.

#### Elements

At most 48, drawn in table order (later ones on top). Coordinates are pixels from the top left of the screen;
every element's box (`x`, `y`, `w`, `h`) must lie inside the screen. An element with an unknown `kind`, an
unknown `source`, `font`, `icon` or `format` value is **skipped, not an error**: a face made for a newer
contract still draws, without the parts the board does not know. Every element may carry up to two
`visible_if` conditions (all must hold) and a `colour` (RGB565, default black) or a `colour_option` (the id of
a colour option; its chosen colour wins; ignored if the id is not a colour option). A 1-bit display draws a
colour white when its luma (0.299 R + 0.587 G + 0.114 B, each channel of the RGB565 value expanded to 8 bits) is at least half and black otherwise.

| Kind | Draws | Fields |
|---|---|---|
| `rect` | a rectangle | `x y w h` (w, h at least 1), `fill`, `thickness` (border), `radius` |
| `line` | a straight line from (`x`, `y`) to (`x + w`, `y + h`) | `x y w h` (not both 0), `thickness` (default 1) |
| `arc` (LCD) | a ring that fills with a value, 0-100 | `x y w h` (square, at least 8), `source`, `arc_start`, `arc_end`, `thickness`; the arc runs from `arc_start` degrees (0 = three o'clock, clockwise) to `arc_end` at 100 % |
| `image` | one image of the table | `x y`, `image` (index); the size is the image's |
| `number` | a value written with a digit set | `x y`, `w` (0 = the value starts at `x`; else the box in which `align` places it), `set`, `source`, `format`, `spacing` (extra pixels between digits, -64 to 64) |
| `text` | a value or fixed text in a built-in font | `x y w h`, `font`, `align`, `source` + `format`, or `literal` (up to 24 bytes) when there is no source; text that does not fit its box is cut |
| `icon` | a built-in icon, centred in its box | `x y w h`, `icon` |

Fonts (`font`; the first column is the size the face is designed on):

| `font` | Paper | LCD | Glyphs |
|---|---|---|---|
| `small` | regular 16 px | regular 16 px | the board's full text range (Latin and Vietnamese) |
| `small_bold` | bold 16 px | bold 20 px | the same |
| `large_bold` | bold 24 px | bold 20 px | Paper: digits, space, `% - . C F` and the degree sign only; LCD: full range |
| `clock` | bold 56 px | bold 72 px | digits, space, colon, minus |

A text drawn with a font that lacks a character leaves it out. Use `large_bold` and `clock` for numbers only.

Icons: `weather` (the board's icon of the weather condition and of day or night; shown when there is a reading),
`ble` (the Bluetooth rune, connected or struck through), `wifi` (the Wi-Fi fan; shown only while connected),
`charging` (the bolt; shown only while charging).

#### Conditions

`visible_if` holds one of: `option_eq` (the option `option` has the value `value`), `source_has` (the source
`source` has a value now), `source_gt0` (the source's number is above zero; a true yes-or-no source counts as
1). `negate` inverts the result. A condition naming an option the face does not have, or of an unknown kind,
does not hold (before `negate`, so a negated unknown condition does).

#### Data sources and formats

The text of a source is what `text` and `number` elements write. `number` maps the characters 0-9, `:`, `-`,
the degree sign and `%` to digit-set slots and `AM` / `PM` to slots 11 / 12; any other character is left out.
The hour, minute, day, weekday, month, year and date come from the board's UTC clock plus
`time.tz_offset_min`; names come from `ui.language`. The global settings stay global: the board applies
`ui.clock_24h` and `ui.temp_unit` itself and a face has no option for them.

| `source` | Text (default `format`) | Other `format` values | No value when |
|---|---|---|---|
| `hour` | hour as shown: 24-hour `09`, 12-hour `9` | `pad2` `09`; `plain` `9` | time unknown: `--` |
| `minute` | `05` | `plain` `5` | time unknown: `--` |
| `ampm` | `AM` or `PM` (12-hour only) | | 24-hour, or time unknown |
| `time` | `HH:MM` as the Classic clock: 24-hour `09:05`, 12-hour `9:05` | | time unknown: `--:--` |
| `day` | day of the month `7` | `pad2` `07` | time unknown |
| `month` | month number `11` | `pad2`; `short` `Nov`; `long` `November` | time unknown |
| `year` | `2026` | `year2` `26` | time unknown |
| `weekday` | the short name `Mon` | `short` as default; `long` `Monday` | time unknown |
| `date` | the Classic date line, e.g. `Monday, 4 Nov` (Vietnamese `Thứ Hai, 4/11`); `Time not set` when unknown | | |
| `weather_temp` `weather_high` `weather_low` | whole degrees in the board's unit with `°C` / `°F`, e.g. `32°C` | `nounit` `32`; `unit` as default | no reading, or the value is missing |
| `weather_rain` | chance of rain `86%` | `nounit` `86` | no value |
| `weather_condition` | (icons only) | | no reading |
| `weather_place` | the place name | | none |
| `room_temp` | one decimal in the board's unit `23.5°C` | `nounit` `23.5` | the board has no sensor or the reading failed |
| `room_humidity` | `60%` | `nounit` `60` | the same |
| `battery_pct` | `80%` | `nounit` `80` | level unknown |
| `charging` `ble_connected` `wifi_connected` `update_available` | (icons and conditions only) | | |
| `notif_count` | the number of notifications waiting `3` | | |

Names by `ui.language`: weekdays short `Sun Mon Tue Wed Thu Fri Sat` / `CN T2 T3 T4 T5 T6 T7`, long `Sunday` ... `Saturday` /
`Chủ nhật Thứ Hai Thứ Ba Thứ Tư Thứ Năm Thứ Sáu Thứ Bảy`; months short `Jan` ... `Dec` / `T1` ... `T12`, long `January` ... `December` /
`Tháng 1` ... `Tháng 12`. Temperatures are converted from the board's tenths of a degree Celsius to `ui.temp_unit`
(Fahrenheit = C x 9/5 + 32) and rounded half away from zero, whole degrees for the weather, one decimal for the
room; the room humidity is rounded to a whole percent. An element whose source has no value is not drawn.

#### The firmware's own parts

- **The notification badge** (the count of waiting notifications) is drawn by the firmware on the built-in
  faces only, in the bottom left corner. An installed face never gets it: it shows the count where it places
  the `notif_count` source, or not at all.
- **The update mark** (a firmware update is waiting): a face that draws the `update_available` source (an icon
  or a condition) says where it goes; any other face gets the firmware's small mark at the corner named by
  `meta.update_anchor` (default top right).

#### Validation (`XFER_FINISH`)

`IMAGE_INVALID` when: the header is wrong or its size differs from the board's logical screen size; the
payload or data area lengths do not add up; the TLV body is invalid (§2.4); `meta` is missing or its `id`
differs from the asset id; a limit above is exceeded or an image has a format the board does not accept; an
image's `set`/`slot` combination repeats, or a digit set an element uses lacks a digit or mixes heights; an
option's ids repeat, its choices or `default` do not fit its type; an element of a known kind lacks a
required field, its box leaves the screen, it names an image, digit set or colour option that does not exist,
or it is an `arc` on a board without arcs; a `visible_if` names an option value above 255. Everything that
only newer firmware understands is skipped instead.
