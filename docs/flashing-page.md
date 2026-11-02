# Flashing page (GitHub Pages)

A static page that flashes a board from the browser over USB (Web Serial, via
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) 10.4.0 loaded from unpkg). Users need Chrome, Edge or
Opera on a desktop computer and a USB data cable; nothing to install. **Not yet run on a board**
(hw-checklist 81); the page, the site builder and its tests are verified on the host only.

## Parts

| File | What |
|---|---|
| `web/index.html` | the page: board cards (filled from `firmware/index.json`), the install button, troubleshooting |
| `tools/web/make_site.py` | builds the site from ESP-IDF build directories (below); tests: `tools/tests/test_make_site.py` |
| `.github/workflows/pages.yml` | builds both variants in `espressif/idf:v5.5.5`, runs the builder, deploys to Pages |

## What a board gets

The ESP Web Tools manifest lists the four parts of `flasher_args.json` at their offsets: bootloader (0x0),
partition table (0x8000), `otadata` (0x19000) and the app (0x20000). Flash mode, size and frequency are inside
the bootloader header. NVS (bonds, config) and the assets partition are not written, so an update keeps them
while the partition layout is the same; the dialog's *Erase device* box wipes everything (needed when coming
from another firmware). The manifest sets `new_install_improv_wait_time: 0` because the firmware has no Improv.

`make_site.py` refuses a build whose app or bootloader lacks the `0xE9` magic byte, whose table lacks the
partition-table magic, whose parts overlap, or whose chip is unknown. Part files are renamed with an 8-digit
content hash, so a browser never mixes old and new parts after a redeploy.

## Local preview

```bash
. ~/esp/esp-idf/export.sh
idf.py -C variants/paper -B build/paper build && idf.py -C variants/lcd -B build/lcd build
python3 tools/web/make_site.py --out build/site --board paper=build/paper --board lcd=build/lcd
python3 -m http.server 8000 --directory build/site      # then open http://localhost:8000 (a secure context)
```

## Publishing

1. Repository **Settings → Pages → Source: GitHub Actions** (a private repository needs a plan with Pages
   for private repositories, and the published page and binaries are public whatever the repository is).
2. Optional but wanted for real releases: add the RSA-3072 signing key (the PEM text) as the secret
   `EPAGER_SIGNING_KEY`. With it the images are signed, accept OTA updates later (docs/recovery.md §4) and the page
   shows "signed". Without it the workflow warns, builds unsigned images and the page says
   "unsigned dev build". The key is written to `keys/` inside the job container and deleted at the end.
3. Push a tag `v<version>` (it must equal `variants/<variant>/version.txt`) or run the workflow by hand.
   A push to `main` does not publish.

## Open points

- The contract's release layout (contracts/releases.md: `manifest.json`, one `.bin` per board on GitHub Releases) is
  a separate thing for the app's OTA, built by `.github/workflows/release.yml`; the page does not use it. Both workflows run for a `v*` tag and build the images separately.
- ESP Web Tools comes from a CDN at a pinned version; vendor it into `web/` if the page must work without unpkg.
- The workflow itself has not run yet (the image tag `espressif/idf:v5.5.5` exists on Docker Hub).
- The LCD board has no BOOT key: a failed install there recovers only over USB (docs/recovery.md §6-7).
