# Compatibility

| Firmware (variant) | Firmware version | Contract | App versions known to work | Verified on |
|---|---|---|---|---|
| paper | (none released) | 0.10.0 | (none yet) | host tests only |
| lcd | (none released) | 0.10.0 | (none yet) | host tests only (P1b in progress) |

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
