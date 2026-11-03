# Compatibility

| Firmware (variant) | Firmware version | Contract | App versions known to work | Verified on |
|---|---|---|---|---|
| paper | 0.2.0 | 0.14.0 | (no companion-app release tested yet) | host tests; on a Paper board: Wi-Fi updates (download, resume, cancel, signature refusal), checklist 84; BLE updates not run (checklist 83) |
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
