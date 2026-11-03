# ePager tools

Python 3.12 tools. Setup once:

```bash
python3 -m venv tools/.venv && tools/.venv/bin/pip install -r tools/requirements.txt
```

| Tool | What it is |
|---|---|
| `epager_proto/` | Python reference codec, segments, text rules, transfer and dispatch models (vector generation) |
| `codegen/` | `gen.py` (registry → C tables, schemas, reference.md), `vectors.py` (contract vectors) |
| `contract_check.py` | contract drift and portability checks |
| `fake_board/` | the board core in a process: TCP and BLE transports, simulated Paper screen, viewer |
| `test_client/` | scripted end-to-end flows and a benchmark, over TCP or BLE |
| `assets/font_gen.py` | LVGL fonts for the UI from Noto Sans, covering the registry's renderable set: 1 bpp for the Paper, 4 bpp plus colour emoji (Noto Color Emoji, RGB565A8) for LCD |
| `assets/icon_gen.py` | the 12 built-in category icons (24x24 mono1) as `components/ui_common/src/ui_icons.c` |
| `web/make_site.py` | the flashing page's site: copies a build's parts under hashed names, writes the ESP Web Tools manifests and `firmware/index.json` (docs/flashing-page.md) |
| `coverage.py`, `xtensa_check.sh` | coverage report and ESP32-S3 cross-compile of the portable core |
| `feature_check.sh` | M4: the gated variant features leave no symbols in the Paper image (`--prove` builds with them on and expects the symbols) |
| `lvgl_conf_check.py` | M4: diffs the effective `LV_*` configuration of the host build and the firmware build; unexplained differences fail |

Both M4 checks need the ESP-IDF environment and a firmware build (`idf.py -C variants/paper -B build/paper build`, see AGENTS.md).

## Fake board

The fake board runs the **same C code** as the firmware (`components/core` and the portable
components) through ctypes, so the app talks to the real protocol implementation. Two libraries
are built from `tests/host` without sanitizers into `build/sim/`:

- `libepager_core.so`: the core on in-memory storage (no screen).
- `libepager_sim.so`: the core plus `ui_common` (LVGL 9.5.0) on the simulated board
  `boards/sim_epaper_154`.

They are built automatically on first use (cmake and ninja from PATH or `~/.espressif`), or by
hand: `cmake -S tests/host -B build/sim -G Ninja -DEPAGER_SANITIZE=OFF && cmake --build build/sim`.
The first configure downloads LVGL once into `.deps/` (about 100 MB, SHA-256 checked).

```bash
tools/.venv/bin/python tools/fake_board                    # TCP on 127.0.0.1:47800, console on stdin
tools/.venv/bin/python tools/fake_board --view             # plus the Tk window with the panel
tools/.venv/bin/python tools/fake_board --view --zoom 4   # start scale (window: 1x-6x buttons, keys 1-6, + and -)
# the window also has a "Post notification" form (Android or iOS/ANCS, category, presets, Remove last)
tools/.venv/bin/python tools/fake_board --host 0.0.0.0     # reachable from a phone on the LAN
tools/.venv/bin/python tools/fake_board --ble --view       # BLE peripheral on this laptop as well
tools/.venv/bin/python tools/fake_board --no-screen        # core only
tools/.venv/bin/python tools/fake_board snapshot out.png --post "Xin chào" --body "Thử nghiệm"
tools/.venv/bin/python tools/fake_board snapshot out.png --scene refresh_sequence
```

Options: `--mtu N` (lower the TCP link MTU to exercise segmentation), `--insecure` (requests are
answered `INSUFFICIENT_SECURITY`), `--version`, `--allow-downgrade`, `--storage BYTES`, `--ghosting`,
`--name`. Type `help` in the console for the simulation commands: inject Android or iOS (ANCS)
notifications (`post vi`, `post emoji`, `post long`, `post "Title" "Body"`, `ancs ...`), battery level
and external power, low storage, storage write failures, data corruption, firmware image and
self-check results (OTA rollback), secure on/off, pairing mode, disconnect, reboot, buttons,
ghosting, `screen` (the text on the panel), `snapshot`, `state`. The console prints every
normalised NotificationEvent and the board's OTA and config events. (Development tool output: the
firmware itself never logs notification content.)

### For the Flutter app (TCP)

Implement the TCP transport of `contracts/protocol.md` §10 next to BLE: frames of
`char_id u8, op u8, len u16 LE, value`; a TCP connection is a connected and bonded link; MTU 517
(segments up to 512 bytes). The Android emulator reaches the host's `127.0.0.1` as `10.0.2.2`; a
phone needs `--host 0.0.0.0` and the laptop's address.

### BLE

`--ble` registers the ePager service and the Battery Service with BlueZ (via `bless`) and
advertises `ePager-XXXX` with the service UUID. Limits of a laptop peripheral:

- No ANCS solicitation in the advertisement and no ANCS client: iOS notifications are simulated
  with the `ancs` console command only.
- Device Information is BlueZ's own service.
- The encrypted characteristics need pairing through a BlueZ agent, e.g. in another terminal:
  `bluetoothctl`, then `agent on`, `default-agent`, `pairable on`, `discoverable on`. Capabilities is
  readable without pairing.
- The ATT MTU and the peer are read from BlueZ's request options; until the first access the
  board assumes MTU 23 (`--ble-mtu` forces a value).

### Screen simulator

`boards/sim_epaper_154` implements `board_api` for a 200x200 1-bit panel: controller RAM, partial
refresh (~0.3 s, changed pixels grey while updating), full refresh (~2 s: inverted old image,
black, white, inverted new image), optional ghosting (pixels changed by partial refreshes keep a
grey trace until the next full refresh), BOOT/PWR buttons and a battery. The UI forces a full
refresh after `display.full_refresh_every` partials (a clock redraw counts one, anything else a quarter; a full
refresh that falls due while the board is in use waits for a quiet moment, PLAN D83), skips unchanged frames and sleeps the panel
between refreshes. The UI shown is a placeholder notification card (the real Paper UI comes
later). The timings are emulated, not measured; real panel behaviour is a hardware check.

Viewer keys: `b`/`B` BOOT short/long, `p`/`P` PWR short/long, `H` PWR hold (5 s), `s` snapshot, `q` quit.
Paper keys (PLAN D96, D98): on the clock BOOT = the cards, PWR = the settings; on a card BOOT = older, PWR = newer; in
the settings BOOT / PWR = up / down, BOOT long = select, PWR long = back; PWR hold = power off/on; BOOT + PWR long
= pairing mode (console `button bootpwr long`).

## Test client

```bash
tools/.venv/bin/python tools/test_client --self-test                  # in-process fake board, every flow
tools/.venv/bin/python tools/test_client --tcp 127.0.0.1:47800 run    # flows that work on any board
tools/.venv/bin/python tools/test_client --tcp 127.0.0.1:47800 run config notifications
tools/.venv/bin/python tools/test_client --scan                        # BLE boards nearby
tools/.venv/bin/python tools/test_client --ble AA:BB:CC:DD:EE:FF bench  # latency and throughput
# A real board needs pairing first: put it in pairing mode, then type the six digits it shows (or pass them)
tools/.venv/bin/python tools/test_client --ble AA:BB:CC:DD:EE:FF --passkey 123456 run
tools/.venv/bin/python tools/test_client --ble AA:BB:CC:DD:EE:FF --no-pair run   # this laptop is bonded already
tools/.venv/bin/python tools/test_client list
```

Flows: capabilities, ping, errors, config, time_sync, notifications (Vietnamese, emoji, long text),
filters_and_apps, asset_resume (disconnect and resume), transfer_errors, ota_dry_run (full firmware
transfer, then abort: safe on hardware), app_icons (upload, replace, wrong size refused, list, delete of an
`app_icon`: also safe on hardware). Flows that need the in-process fake board to inject
failures: transfer_board_failures, resume_after_reboot, ota_refusals, ota_install, ota_rollback,
security, device_status, ancs, screen, app_icons_screen (the card shows the uploaded icon for Android and
ANCS notifications, follows replace and delete, and falls back to the category icon). `pytest tools` runs all of them over real TCP sockets.

## Golden images

`tests/golden/<variant>/<WxH>/*.png` are rendered headless by `fake_board/scenes.py` and compared pixel for pixel
by `tools/tests/test_golden.py` (and checked to be byte-identical across two renders). After an
intended UI change: `tools/.venv/bin/pytest tools --update-golden`, then look at the images before
committing. A mismatch writes the actual image to `build/golden-actual/`.

## Fonts

`tools/.venv/bin/python tools/assets/font_gen.py` regenerates `components/ui_common/fonts/*.c`
from Noto Sans, plus the monochrome Noto Emoji glyphs for `text.emoji_ranges` (both downloaded once into
`.deps/fonts/`, SHA-256 pinned; SIL OFL 1.1). The font choice is provisional until the Paper UI work.
LCD's fonts (`ui_font_lcd_*`, 4 bpp) leave the emoji out and fall back to `ui_font_emoji_colour.c`:
Noto Color Emoji v2.051 (OFL) scaled to a 20 px cell, one RGB565A8 image per emoji (about 1.6 MB, only
in the LCD image; PLAN D59).
`--check` compares instead of writing. After editing the registry's `text.renderable_ranges` or
`text.emoji_ranges`, run `gen.py`, `vectors.py` and this script (it fails if a range has no glyph).

## Icons

The card shows a 24x24 icon: the app's own `app_icon` asset when one is installed, else the built-in
icon of the notification's category (contracts/protocol.md 6.4, asset-formats.md). Built-in icons are
drawn by `tools/.venv/bin/python tools/assets/icon_gen.py` (Pillow, committed output
`components/ui_common/src/ui_icons.c`; `--check` compares, `--preview sheet.png` writes a contact
sheet). On the fake board, the console uploads sample icons through the real transfer protocol:

```text
icon com.zing.zalo zalo        # zalo | ring | cross; shown for notifications of that app (Android and ANCS)
icon-remove com.zing.zalo
```

These use an in-process link (`fake_board/loopback.py`), so they need the board's one link to be free:
disconnect the app first, or upload from it. Scene `icons` (`fake_board snapshot out.png --scene icons`)
shows the three states.
