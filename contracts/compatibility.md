# Compatibility

| Firmware (variant) | Firmware version | Contract | App versions known to work | Verified on |
|---|---|---|---|---|
| lcd | 0.3.1 | 0.18.0 | (no companion-app release tested yet) | host tests; the Screen off settings row not run on the board yet |
| paper | 0.3.1 | 0.18.0 | (no companion-app release tested yet) | host tests; the Screen off row is LCD only |
| paper | 0.3.0 | 0.18.0 | (no companion-app release tested yet) | host tests; on a Paper board: the Digital face and the settings list (flashed over USB); the clock faces over BLE, quiet hours and the Pebble-style idle drain are in the hw-checklist (85, 86, 87) |
| lcd | 0.3.0 | 0.18.0 | (no companion-app release tested yet) | host tests; the Digital face and the settings list on the LCD board (flashed over USB); clock faces over BLE not run |
| paper | 0.2.0 | 0.14.0 | (no companion-app release tested yet) | host tests; on a Paper board: updated 0.1.1 -> 0.2.0 over Wi-Fi from the published release, resume, cancel, signature refusal (checklist 84); BLE updates not run (checklist 83) |
| lcd | 0.2.0 | 0.14.0 | (no companion-app release tested yet) | host tests; Wi-Fi and BLE updates not run on the LCD board |
| paper, lcd | 0.1.0, 0.1.1 | 0.13.0 | (no companion-app release tested yet) | no firmware update path: update over USB (the flashing page) |

Policy:
- The app reads `CAPABILITIES.contract_version`. Same major → talk normally and ignore unknown fields,
  messages and enum values. Different major → only offer a firmware update (protocol.md §4).
- Frozen across majors (protocol.md §9): the message header, `CAPABILITIES` type `0x7E` and its
  tags 1-5 (`contract_version`, `device_name`, `board_id`, `variant`, `firmware_version`) with their
  types, the GATT UUIDs, and the `XFER_*` firmware path. Any app can identify any board and update it.
- Firmware images and their manifest: [`releases.md`](releases.md).
- While the contract is 0.x, app and firmware are expected to move in lock-step; this table records
  what was actually tested together.
- A row is added for every firmware release and every app release that changes protocol handling.
