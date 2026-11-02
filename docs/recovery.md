# Recovery: USB flashing, erasing, rollback, signed images

Applies to the Paper (Waveshare ESP32-S3-ePaper-1.54 V2). **Nothing here has been run on the board yet** (M4 has no hardware); statements about what the board does are marked [HW] and tracked in [hw-checklist.md](hw-checklist.md). Facts about ESP-IDF come from the 5.5.5 source tree.

Setup in every shell: `. ~/esp/esp-idf/export.sh`.

## 1. Flash over USB

The Type-C connector goes straight to the S3's native USB (GPIO19/20, schematic), there is no UART bridge: the board shows up as a CDC serial port (`/dev/ttyACM*` on Linux) and the console is the USB Serial/JTAG port (`sdkconfig.defaults`).

```bash
idf.py -C variants/paper -B build/paper -p /dev/ttyACM0 flash monitor
```

This writes the bootloader, partition table, `otadata` and the app. It does not touch NVS (bonds, config) or the assets partition. Dev builds that must accept OTA later need the signed overlay (§4): `-DEPAGER_SIGN=ON` (use a separate build directory, e.g. `-B build/paper-signed`).

### If the port does not appear (crashed firmware, or firmware sleeping)

Enter the ROM download mode: with USB unplugged, hold **BOOT** (GPIO0, the key marked BOOT0), plug in the cable, release BOOT, and flash as above. [HW] The schematic shows only BOOT and PWR keys and no reset key (the EN net has just an RC), so the power cycle is how you reset; confirm the procedure and whether the PWR latch (§5) interferes.

## 2. Erase

| Goal | Command | Effect |
| --- | --- | --- |
| Everything back to factory | `idf.py -C variants/paper -B build/paper erase-flash`, then flash | bonds, config, assets and firmware all gone |
| Forget bonds and config only | `esptool.py --chip esp32s3 erase_region 0x9000 0x10000` | NVS only; the next boot formats it |
| Re-format assets | `esptool.py --chip esp32s3 erase_region 0x620000 0x1E0000` | the firmware formats the blank LittleFS partition at the next boot |
| Reset the OTA slot choice | `idf.py -C variants/paper -B build/paper erase-otadata` | boots the first app slot (`ota_0`) |

Offsets are the ones in `boards/waveshare_s3_epaper_154/partitions.csv`.

## 3. Rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on: a new OTA image starts "pending verify" and the running image must mark itself valid after its self-check (PLAN D11, implemented in M6). If it resets or hangs first, the bootloader returns to the previous slot at the next boot. In M4 nothing installs OTA images yet, so rollback is configured but unexercised. If a build in a slot is bad and still boots, flash the last good build over USB.

## 4. Signed images (PLAN D12, option A)

`variants/sdkconfig.signed` (applied by `-DEPAGER_SIGN=ON`) turns on `SECURE_SIGNED_APPS_NO_SECURE_BOOT` and `SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` with RSA-3072 (Secure Boot V2 signature block). **No eFuse is burned** and USB recovery always works.

What this does and does not do (from `components/bootloader_support` in ESP-IDF 5.5.5):

- The **bootloader does not check the signature at boot** in this mode (the boot-time check is only compiled for hardware Secure Boot). An unsigned image flashed over USB boots.
- The **running app** verifies an OTA image's signature, and takes the trusted public-key digest from its own signature block ("Take trusted digest key(s) from running app"). So the image you flash over USB must itself be signed with the key you will use for updates, or no OTA update can ever be accepted ("No signatures were found for the running app"). Always flash `-DEPAGER_SIGN=ON` builds to a board that should accept OTA.
- Protects against spoofed OTA images over BLE, not against someone with USB access to the board.

### Keys

```bash
espsecure.py generate_signing_key --version 2 keys/dev_signing_key.pem   # RSA-3072
```

- `keys/` is git-ignored and `*.pem` is ignored everywhere; **never commit a private key**.
- The M4 build signs with `keys/dev_signing_key.pem` (path in `variants/sdkconfig.signed`, relative to the variant folder, e.g. `variants/paper`). This is a development key. A release key and `tools/sign` (generation, signing, verification) come with M6.
- **Back the key up outside the repository.** Losing the key does not brick the board, but then no OTA image can be accepted by boards running images signed with it: they must be reflashed over USB with an image signed with a new key. Rotating a key without USB needs a transition image signed with both keys (the signature block holds several signatures).
- Check a signature: `espsecure.py signature_info_v2 build/paper-signed/epager_paper.bin` and `espsecure.py verify_signature --version 2 --keyfile keys/dev_signing_key.pem <image>`.

### Not used, and never without asking

Hardware Secure Boot, flash encryption and the secure-version (anti-rollback) eFuse are **irreversible**. They are not enabled and must not be enabled without the project owner's explicit approval (AGENTS.md).

## 5. Power-on from the battery [HW]

On battery the board is powered only while the PWR key is held or GPIO17 (`BAT_Control`) is high (schematic: GPIO17 drives T2, which pulls the gate of the battery PMOS Q4 low). The firmware drives GPIO17 high as the first thing `app_main` does. The bootloader runs before that (about 0.3 s [HW]): if the board dies when the PWR key is released before the app is up, hold the key longer and record the time in the checklist; the fix would be a bootloader hook (`bootloader_before_init`) that sets the pin. On USB power the board stays up regardless.

## 6. Probe a new board and back up its stock firmware

Do this once, **before the first flash**, on any ESP32-S3 board that arrives with vendor firmware. Everything here only reads the board (esptool resets it into download mode and back). Run on 2026-10-13 against the LCD board (ostb-xiaozhi-3st).

`esptool.py`, `python` (with `pyserial`) and `gen_esp32part.py` come from ESP-IDF's Python environment (`~/.espressif/python_env/idf5.5_py3.12_env/bin/`): run `. ~/esp/esp-idf/export.sh` in the shell first.

**Access.** Serial ports belong to the `dialout` group; add yourself once, then log out and back in:

```bash
sudo usermod -aG dialout $USER
```

Plug in a USB **data** cable. `lsusb` shows `303a:1001 Espressif USB JTAG/serial debug unit` and the port is `/dev/ttyACM0` when the cable reaches the S3's native USB; a CH340/CP210x bridge shows as `/dev/ttyUSB0`.

**Chip, PSRAM, flash, security.**

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 flash_id
esptool.py --chip esp32s3 -p /dev/ttyACM0 get_security_info
```

`flash_id` prints the chip revision, crystal, embedded PSRAM, flash vendor/device id and detected size. `get_security_info` must show **Secure Boot: Disabled** and **Flash Encryption: Disabled**; if either is on, stop and ask before flashing anything (the board may only accept the vendor's signed images).

**Full backup** (16 MB took about 4 min over native USB; pass the detected size):

```bash
mkdir -p build/<board>-stock
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 read_flash 0 0x1000000 build/<board>-stock/flash_16mb.bin
sha256sum build/<board>-stock/flash_16mb.bin
```

`build/` is git-ignored and is wiped by clean builds: copy the image somewhere outside the repository. **It contains the vendor NVS, including any Wi-Fi password the board stored**: never commit or share it.

**Partition table and app versions** (read from the backup, the board is not needed):

```bash
python ~/esp/esp-idf/components/partition_table/gen_esp32part.py \
  <(dd if=build/<board>-stock/flash_16mb.bin bs=1 skip=$((0x8000)) count=3072 2>/dev/null)
```

Cut each app slot out with `dd` (offsets and sizes from the table), then `esptool.py --chip esp32s3 image_info --version 2 <slot>.bin` checks the image. The app description (`esp_app_desc_t`: version, project name, build date, ESP-IDF version) starts 0x20 bytes into the slot. The active slot is the `otadata` entry (0xd000 and 0xe000) with the higher sequence number: `(seq - 1) % 2` gives `ota_0` or `ota_1`. `strings -n 6 <slot>.bin | grep -iE 'lcd|touch|codec|adc'` names the drivers the vendor linked in.

**Boot log.** Vendor firmware often logs the board name, the display and touch chips and the audio codecs at boot. Toggling RTS resets the chip through the USB Serial/JTAG port; this captures 12 s of output:

```bash
python -I -c "
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.2)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
t = time.time(); out = b''
while time.time() - t < 12: out += s.read(4096)
open('build/<board>-stock/boot_log.txt', 'wb').write(out)"
```

The log can contain the Wi-Fi network name and IP address; keep it out of the repository.

**Restore the stock firmware** [16 MB in 108 s at 921600 baud, hash verified, the stock firmware boots]:

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash 0 build/<board>-stock/flash_16mb.bin
```

Facts from a vendor image (driver names, log lines) are clues, not sources: record them as such and confirm pins from the schematic or on the board ([adding-a-board.md](adding-a-board.md) step 1).

## 7. LCD (ostb-xiaozhi-3st)

**Nothing is flashed to this board without the user's go-ahead** (AGENTS.md).

- **Flash** [VERIFIED P1a, 7 times with no key pressed, also while our image ran]: `idf.py -C variants/lcd -B build/lcd -p /dev/ttyACM0 flash monitor`. This writes the bootloader, our partition table, `otadata` and the app. Our NVS partition (0x9000, 64 KiB) overlaps the stock NVS, `otadata` and `phy_init`: if NVS finds those pages unusable the image erases it and starts clean, and LittleFS formats the old SPIFFS assets area at the first boot.
- **No BOOT key and a closed case:** the power key is a hardware power switch, not GPIO0 (test 2026-10-13), so the ROM download mode can only be reached through USB Serial/JTAG, which esptool does automatically. That works only while the running image leaves USB alone: never reuse GPIO19/20, never power down the USB PHY, no light sleep while USB is connected (PLAN D50). If a bad image ever breaks USB, the board can only be recovered by opening the case to reach GPIO0.
- **Back to stock:** `esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash 0 build/xiaozhi-3st-stock/flash_16mb.bin` restores the board exactly, including its Wi-Fi settings (check the backup's SHA-256 first). To go back to ePager afterwards, flash the LCD image again.
- **Erase** (offsets from `boards/ostb_xiaozhi_3st/partitions.csv`): NVS `erase_region 0x9000 0x10000`, assets `erase_region 0x820000 0x7E0000`.

## 8. Quick reference

| Symptom | Likely cause | Action |
| --- | --- | --- |
| No serial port | firmware asleep or crashed | download mode (§1) |
| Boots but loops | bad image | flash the last good build; `erase-otadata` if the slot is wrong |
| OTA refused with "no signatures" | running image was flashed unsigned | flash a signed build over USB |
| Assets look empty after an update | assets partition is separate and survives updates | check it was not erased; re-upload |
| Phone no longer connects after reflashing | bonds live in NVS; erase-flash dropped them (or the phone still has the old bond) | forget the device on the phone, pair again |
