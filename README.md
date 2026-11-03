# ePager firmware

**Your phone's notifications, on a small desk display.**

**ePager** is ESP32-S3 firmware for e-paper and colour LCD boards. The phone connects over Bluetooth LE; Wi-Fi fetches weather and time and provides a local setup hotspot.

- **iPhone:** notifications through Apple Notification Center Service (ANCS), with no iOS app required.
- **Android:** notifications and configuration through a companion app using ePager's custom GATT service. **The app will be available later.**
- **On the desk:** notification cards, an idle clock and weather, with a shared UI adapted to each display.
- **On your computer:** the same portable C core and UI run in a simulator for development without hardware.

**Work in progress.** Both boards have been flashed and run on hardware; verification is tracked per feature in the [hardware checklist](docs/hw-checklist.md). Firmware OTA is still planned. See the [milestones](docs/MILESTONES.md) for implementation status.

[Try the simulator](#try-it-without-hardware) · [Build and flash](#build-and-flash) · [Probe a new board with an AI agent](#probe-a-new-board-with-an-ai-agent) · [Documentation](#documentation)

## Supported hardware

| Variant | Board | Display | Memory |
| --- | --- | --- | --- |
| `paper` | Waveshare ESP32-S3-ePaper-1.54 **V2** (`waveshare_s3_epaper_154`) | 200 × 200 black and white e-paper | 8 MB flash, 8 MB PSRAM |
| `lcd` | ostb-xiaozhi-3st (`ostb_xiaozhi_3st`) | 296 × 240 colour LCD in landscape, CST816S touch | 16 MB flash, 8 MB PSRAM |

A **variant** defines the product's UI and features; a **board** supplies its pins, drivers and hardware configuration. Additional boards can use the shared components through `board_api`. See [Adding a board](docs/adding-a-board.md) for the porting guide and the [LCD board notes](docs/ostb-xiaozhi-3st.md) for an example of investigating undocumented hardware.

## Try it without hardware

Run these commands from the repository root. The development environment uses **Python 3.12**, a host C compiler, **CMake** and **Ninja**. The screen viewer also needs **Tk** (the simulator uses Tk, not SDL). ESP-IDF can provide CMake and Ninja; activate it as shown in the build section if needed.

```bash
python3 -m venv tools/.venv
tools/.venv/bin/pip install -r tools/requirements.txt

# Paper screen; the native libraries build automatically on first use.
tools/.venv/bin/python tools/fake_board --view

# Or start the LCD simulator instead.
tools/.venv/bin/python tools/fake_board --view --board lcd
```

The first build downloads pinned LVGL sources. In the simulator console, type `help` for commands or `post vi` to show a sample notification. The viewer also has a form for posting notifications. See the [tools guide](tools/README.md) for TCP/BLE clients, screenshots and simulation controls.

## Build and flash

Use **ESP-IDF 5.5.5**. With ESP-IDF installed at `~/esp/esp-idf`:

```bash
. ~/esp/esp-idf/export.sh

idf.py -C variants/paper -B build/paper build
idf.py -C variants/lcd -B build/lcd build
```

The first build fetches managed dependencies. To select another supported board, pass `-DEPAGER_BOARD=<folder-under-boards>` and use a fresh build directory; an existing `sdkconfig` retains its previous settings.

Read the [recovery guide](docs/recovery.md) and back up vendor firmware **before the first flash**. For the Paper board, replace the example port with your board's serial port:

```bash
idf.py -C variants/paper -B build/paper -p /dev/ttyACM0 flash monitor
```

**LCD recovery requires extra care:** the ostb-xiaozhi-3st has no BOOT key. Keep native USB Serial/JTAG working, leave GPIO19/20 alone, and preserve `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`. An AI agent must have your explicit go-ahead in the current session before flashing this board, including probe images. See [LCD flashing and recovery](docs/recovery.md#7-lcd-ostb-xiaozhi-3st).

The repository also includes a [browser flashing page](docs/flashing-page.md) using Web Serial in desktop Chrome or Edge. Its setup and publishing instructions are documented there; flashing through the page still needs hardware verification. For signed builds, use `-DEPAGER_SIGN=ON` with a local development key as described in the [signing guide](docs/recovery.md#4-signed-images-plan-d12-option-a).

## Pairing and Wi-Fi setup

With no saved bonds, the board opens pairing mode automatically. To reopen it:

| Board | Enter pairing mode |
| --- | --- |
| Paper | Hold **BOOT + PWR** together for 1.5 seconds. |
| LCD | Wake the screen with a volume key if needed, then hold **both volume keys** for 3 seconds. |

For iPhone testing, open **Settings → Bluetooth**, select `ePager-XXXX` and enter the six-digit code shown on the board. Allow **Share System Notifications** if offered. Full ANCS notification delivery and reconnect behaviour still need [hardware verification](docs/hw-checklist.md); pairing alone does not establish that they work.

From the pairing screen, open Wi-Fi setup with a short **BOOT** press on Paper or the **Wi-Fi setup** button on LCD. Use the displayed QR code or hotspot details to connect and configure your network. Pairing mode closes after three minutes of inactivity; passkey entry and Wi-Fi setup pause the timeout.

## Probe a new board with an AI agent

A coding agent with access to this checkout and your local terminal can help identify a board, inspect its stock firmware and prepare a port. Start by providing the **exact model and revision**, USB serial port, product links or schematics, and any limits such as a case that must stay closed. Tell the agent which board is connected if you have more than one.

Use the [AI-assisted discovery workflow and copyable prompt](docs/adding-a-board.md#ai-assisted-discovery) to start with read-only probes, a private stock-firmware backup and a sourced bring-up plan. The same guide covers implementing the port after plan approval.

## Development and tests

The portable C11 components run on the host without ESP-IDF or FreeRTOS. Host tests use Unity with AddressSanitizer and UndefinedBehaviorSanitizer enabled by default:

```bash
cmake -S tests/host -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure

# End-to-end flows, golden screen images and Python tooling.
tools/.venv/bin/pytest -q tools

# Contract consistency and generated-file drift.
tools/.venv/bin/python tools/contract_check.py
```

Read [AGENTS.md](AGENTS.md) before contributing, whether working manually or with a coding agent. It defines coding conventions, verified commands, hardware constraints and the milestone review process.

## App/firmware contract

This repository is the source of truth for the [app/firmware contract](contracts/protocol.md). The current version is in [`contracts/CONTRACT_VERSION`](contracts/CONTRACT_VERSION); [`contracts/registry.json`](contracts/registry.json) defines every protocol number.

Change the specification and vector cases before implementation, then regenerate:

```bash
tools/.venv/bin/python tools/codegen/gen.py
tools/.venv/bin/python tools/codegen/vectors.py
tools/.venv/bin/python tools/contract_check.py
```

Generated files are never edited by hand. Every contract change updates `CONTRACT_VERSION`, `contracts/CHANGELOG.md` and test vectors in the same commit. Both firmware and the companion app must pass the shared vectors.

## Privacy and security

Notifications stay in RAM and notification content is excluded from firmware logs by default. Writes require an encrypted, bonded BLE link using LE Secure Connections. Once a day (switch: config key `update.auto_check`) a board with Wi-Fi asks GitHub's release servers for the newest firmware manifest; GitHub sees the board's public IP address and a user agent with the firmware version and board ID, nothing else, and nothing is installed without the person's choice on the board. Private signing keys belong in the git-ignored `keys/` directory. See [AGENTS.md](AGENTS.md) for the full rules.

## Repository layout

```text
contracts/    Protocol specification, registry, schemas and shared test vectors
components/   Portable C core, shared UI and ESP-IDF integration
boards/       Board drivers, pin maps, hardware configuration and host simulators
variants/     Paper and LCD projects with shared build configuration
tests/        Host unit tests and golden screen images
tools/        Simulator, test client, reference codec, generators and release tools
web/          Browser flashing page
docs/         Architecture, milestones, board notes and hardware verification
```

## Documentation

- [Architecture](docs/PLAN.md)
- [Adding a board](docs/adding-a-board.md) · [USB flashing and recovery](docs/recovery.md)
- [Python tools and simulator](tools/README.md) · [Browser flashing page](docs/flashing-page.md)

## Licence

[MIT](LICENSE). Bundled third-party fonts and test libraries retain their own licences; see `components/ui_common/fonts/LICENSE-*` and `tests/host/third_party/`.
