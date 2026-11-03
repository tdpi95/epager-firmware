# Firmware releases

Since contract 0.7.0. How firmware CI publishes OTA images and how the app finds, checks and offers
them. Schema: [`release-manifest.schema.json`](release-manifest.schema.json) (hand-written).
Vectors: [`test-vectors/releases.json`](test-vectors/releases.json) (scope `app`).

Images are published as **GitHub Releases** of the public repository
[`tdpi95/epager-firmware`](https://github.com/tdpi95/epager-firmware) (public since 2026-11-03; `v0.1.0` and
`v0.1.1` are published). Anyone, the app and the board included, can read them without a token. The stable
manifest URL is `https://github.com/tdpi95/epager-firmware/releases/latest/download/manifest.json`: it
redirects to the newest release that is not a pre-release, and the image URL is
`https://github.com/tdpi95/epager-firmware/releases/download/v<version>/<file>`; both redirect on to
`release-assets.githubusercontent.com` (a long signed URL; `Range` requests are answered). The board itself
is planned to use the same manifest for updates over Wi-Fi: [`docs/wifi-ota-plan.md`](../docs/wifi-ota-plan.md).

## Release layout

- **Tag** `v<version>`, `version` = `MAJOR.MINOR.PATCH[-pre]` (semver, no leading zeros, no build
  metadata). A version with a pre-release part is published as a GitHub **pre-release**.
- One release covers every board: all variants share the release version. The `version.txt` of every
  variant in the release equals `version`, and so does each image's `CAPABILITIES.firmware_version`.
- **Assets**:
  - `manifest.json`: the manifest below (UTF-8 JSON).
  - One signed image per board: `epager-<board_id>-<version>.bin`, the application image exactly as
    `idf.py` builds it with signing on (option A: the signature block is part of the file), i.e. the
    bytes the app sends with `XFER_BEGIN {kind = firmware}`.
  - Anything else (release notes, ELF files, checksums for humans) is ignored by the app.

## `manifest.json`

```json
{
  "manifest_version": 1,
  "version": "0.2.0",
  "contract_version": "0.7.0",
  "images": [
    {"board_id": "waveshare_s3_epaper_154_v2", "variant": "paper",
     "file": "epager-waveshare_s3_epaper_154_v2-0.2.0.bin", "size": 1234567,
     "sha256": "<64 lower-case hex digits>", "min_app_contract": "0.7.0"}
  ],
  "notes_url": "https://…"
}
```

| Field | Rule |
|---|---|
| `manifest_version` | `1`. Any other value: the app does not use the manifest (a layout change that old apps cannot read). |
| `version` | Release version as above; equals the tag without `v`. |
| `contract_version` | `MAJOR.MINOR.PATCH`: the contract the images implement (`CONTRACT_VERSION` at build time). |
| `images[]` | At least one; `board_id` unique within the manifest. |
| `images[].board_id` | `CAPABILITIES.board_id` of the board the image is for; `[a-z0-9][a-z0-9_-]*`, at most 48 bytes. |
| `images[].variant` | Registry enum `variant` name (`paper`, `lcd`). |
| `images[].file` | Exactly `epager-<board_id>-<version>.bin` (the release asset name). |
| `images[].size` | File size in bytes, 1 to 2^32 − 1. |
| `images[].sha256` | SHA-256 of the file, lower-case hex. |
| `images[].min_app_contract` | Optional `MAJOR.MINOR.PATCH`: the oldest app contract that can use this firmware. Release CI sets it when the firmware needs an app change (while the contract is 0.x a minor bump may be breaking). |
| `notes_url` | Optional `https://` link to human-readable release notes. |

Unknown properties are ignored at every level (forward compatibility). A manifest that fails the
schema, repeats a `board_id`, or has an image whose `file` breaks the naming rule is **invalid**: the
app offers nothing from it.

## App behaviour

To decide whether to offer a release to the connected board, the app uses its Capabilities
(`board_id`, `variant`, `firmware_version`) and refuses at the first matching rule, in this order:

1. `invalid_manifest`: the manifest is invalid (above).
2. `prerelease`: `version` has a pre-release part and developer mode is off.
3. `no_image`: no image has the board's `board_id`.
4. `variant_mismatch`: that image's `variant` is not the board's variant.
5. `contract_major`: the major of `contract_version` differs from the app's contract major (after the
   update the app could not talk to the board, protocol.md §4; the user should update the app).
6. `app_too_old`: the app's contract version is lower than `min_app_contract` (semver precedence).
7. `not_newer`: `version` is not newer than the board's `firmware_version` by semver precedence (build
   metadata ignored; a release is newer than its own pre-releases). If the board's version cannot be
   parsed, this rule does not apply and the board decides (`VERSION_REJECTED`, protocol.md §7.2).

Otherwise the app offers the image. After downloading it, the app MUST check that the file's size
and SHA-256 equal the manifest's before starting the transfer, and sends `XFER_BEGIN` with
`kind` = firmware, `id` = `firmware`, `version` = the manifest `version`, and the CRC-32 and SHA-256 it
computed over the file. The board still checks everything itself (protocol.md §7: version, battery,
CRC, SHA-256, image and **signature**); the manifest only helps the app choose. The manifest is not
signed in this version; a detached manifest signature may be added later (additive).

## Board behaviour (updates over Wi-Fi, since 0.14.0)

A board with the `wifi` and `ota` features reads the same `manifest.json` itself, so a phone without an app (an
iPhone) can still update it ([`docs/wifi-ota-plan.md`](../docs/wifi-ota-plan.md)). It uses the URL above, follows
redirects only to `https` on `github.com` and `*.githubusercontent.com`, and applies the app's rules 1, 3, 4, 5 and
7 with these differences: it ignores `min_app_contract`; rule 5 compares its own contract major (a release for
another major is not installed: the person is told it needs a newer app); it adds *too large* (the image does not
fit its OTA slot) and refuses a pre-release unless the manifest URL was overridden on a developer build. The
image is requested from `releases/download/v<version>/<file>`, so it belongs to the version the manifest named;
its size and SHA-256 must equal the manifest's before the board asks the firmware check (image and signature) to
install it, the same check as for an image sent over BLE (protocol.md §7). The check runs once a day unless
`update.auto_check` is off; the board offers an update on its own screen (install, remind later, skip that
version) and installs nothing without the person's choice.

## Release CI (firmware side)

`.github/workflows/release.yml` runs for a tag `v<version>`: it builds every variant with `-DEPAGER_SIGN=ON`
(the repository secret `EPAGER_SIGNING_KEY` is required; a tag push fails without it), then
`tools/release/make_release.py` checks that each variant's `version.txt` and build equal the tag and that the
images are signed, computes sizes and SHA-256, writes `manifest.json` (validated against the schema) and the
`epager-<board_id>-<version>.bin` files, and `gh release create` publishes them with the ELF files and
`SHA256SUMS` (a tag with a pre-release part becomes a GitHub pre-release). The contract version in the manifest
is `contracts/CONTRACT_VERSION` of the tagged commit. Run by hand the workflow is a dry run (unsigned images
allowed, nothing published, files in the `release` workflow artifact). It has run on GitHub for `v0.1.0` and
`v0.1.1` (2026-11-02 and 2026-11-03); the script is host-tested (`tools/tests/test_make_release.py`). The
boards of 0.1.1 and earlier have no firmware sink, so they take no OTA image; the first release with the sink
(M6b) must be flashed over USB or with the flashing page.
