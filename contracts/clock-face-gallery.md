# Clock face gallery

Contract 0.15.0. How clock faces ([`asset-formats.md`](asset-formats.md#clock-faces-all-boards)) are published and found. Like
[`releases.md`](releases.md) this document and [`clock-face-gallery.schema.json`](clock-face-gallery.schema.json) are written by
hand. **Only the phone apps read the gallery**: the board has no gallery client, no gallery parser and no allow-list entry for it;
it receives faces over BLE like any asset (protocol.md §7) with `origin` = `gallery`.

## Where

The gallery is a public GitHub repository (planned: `tdpi95/epager-faces`) with one folder per face (the face's source, its
images, a `LICENSE` that names the licences of any font or picture used, and a `README.md`). Every release of the repository
publishes:

- `gallery.json`, at a stable address that needs no API call and so has no rate limit:
  `https://github.com/<owner>/<repo>/releases/latest/download/gallery.json`;
- one `.epface` file per face and board, and one thumbnail per face and board (a PNG of the face as the board draws it).

A `.epface` file is exactly the asset file of `asset-formats.md` (container header and payload), so installing it is
`XFER_BEGIN` (`kind` asset, `asset_type` `clock_face`, `id` = the face's id, `version` = the entry's `version`, `origin` `gallery`),
the chunks and `XFER_FINISH`. A person's own file is installed the same way with `origin` `local`.

## `gallery.json`

```json
{
  "schema": 1,
  "generated": "2026-11-20T09:30:00Z",
  "faces": [
    {
      "id": "epager.big",
      "name": {"en": "Big", "vi": "Số lớn"},
      "author": "ePager",
      "version": "1.0.0",
      "license": "CC-BY-4.0",
      "boards": {
        "paper": {"file": "epager.big-paper.epface", "size": 5432, "sha256": "<64 hex>", "min_contract": "0.15.0",
                  "thumbnail": "epager.big-paper.png"},
        "lcd": {"file": "epager.big-lcd.epface", "size": 20480, "sha256": "<64 hex>", "min_contract": "0.15.0",
                "thumbnail": "epager.big-lcd.png"}
      }
    }
  ]
}
```

- `schema` is 1; an app that sees a larger number ignores the gallery and says so. Unknown fields are ignored.
- `id` follows the `id` type (`[a-z0-9._-]`, 1-32 bytes, not starting with `.`); by convention `author.name`. Ids starting with
  `epager.` belong to the maintainers.
- `version` is the face's own semver and equals `meta.version` inside the file; an app offers an update when the entry's
  version is newer than the installed asset's `version` (`ASSET_LIST`) and the installed face has `origin` `gallery`.
- `boards` is keyed by the `board` the app finds in `CAPABILITIES.variant` (`paper`, `lcd`); a face with no entry for a
  board is not offered for it. The app downloads `file` (relative to `gallery.json`'s address) over HTTPS, checks `size` and
  `sha256`, and refuses a file that differs. Faces are data only, so the manifest is not signed: HTTPS and the digest are the
  protection, and the board checks every file again (`IMAGE_INVALID`).
- `min_contract`: the lowest board contract version that draws the whole face; the app does not offer it to older boards.

## Rules for submissions (the repository's CI enforces them)

- Built with the contract's tools; every file passes the C parser of the host build under the sanitizers and fits the
  limits for its board.
- Any open or Creative Commons licence that allows redistribution; fonts and pictures are named in `LICENSE`.
- No logos or trademarks of brands, nothing offensive, legible on the board it is made for (the CI posts the preview matrix of
  every option, 12/24 h, °C/°F, English/Vietnamese and weather on/off/missing for review).
- A pull request changes only its author's folder; a maintainer reviews a first submission.
