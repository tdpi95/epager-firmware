# Waveshare ESP32-S3-ePaper-1.54 V2 (with battery): vendor documentation notes

What Waveshare's official documentation says about the board the Paper variant runs on: **V2, no touch,
with lithium battery** (the product the wiki calls "ESP32-S3-ePaper-1.54 (Without touch control, with
lithium battery)", SKU 32298; the `-EN` SKU 32299 is the same board without battery). The summary of board
facts we rely on is [PLAN.md](PLAN.md) §2; pins are in `boards/waveshare_s3_epaper_154/src/ws154_pins.h`;
this file records the vendor's own statements, so the sources of those facts stay checkable.

Sources (read 2026-10-18, all in English, public):

- Overview: <https://docs.waveshare.com/ESP32-S3-ePaper-1.54>
- Resources: <https://docs.waveshare.com/ESP32-S3-ePaper-1.54/Resources-And-Documents>
- ESP-IDF page: <https://docs.waveshare.com/ESP32-S3-ePaper-1.54/ESP-IDF>
- FAQ: <https://docs.waveshare.com/ESP32-S3-ePaper-1.54/FAQ>

Status tags as in PLAN.md: **[V]** verified by us on the board, **[S]** stated by the vendor, not verified by us.
The vendor's pages are prose and pictures of varying care (see "Doubtful points"): schematic and datasheets win.

## Telling V1 from V2

- V2 has "V2" printed on the back or silkscreened in the top-left corner of the PCB; otherwise it is V1. V2 was
  phased in from 2025-11-01. **Example programs are not interchangeable between the versions** (the FAQ's first
  answer to "flashed but won't run" is to check that the example matches the product version). [S]
- V1: ESP32-S3FH4R2 (4 MB flash, 2 MB PSRAM). V2: **ESP32-S3-PICO-1-N8R8 (8 MB flash, 8 MB PSRAM)**, and the
  whole-board power consumption in sleep is optimised. [S] (flash and PSRAM size agree with the chip's datasheet)
- The wiki's dimension drawing shows a board labelled `ESP32-S3-FH4R2`: it is a **V1** picture, so do not take
  the chip placement on it as V2.

## Features and on-board parts [S]

| Part | Notes |
|---|---|
| SoC | Xtensa LX7 dual core, up to 240 MHz, 2.4 GHz Wi-Fi 802.11 b/g/n and Bluetooth 5 (LE), on-board chip antenna, 512 KB SRAM, 384 KB ROM |
| Display | 1.54 in e-paper, 200 x 200, black/white (2 grey levels), SPI, passively reflective. Full refresh 2 s, partial refresh 0.3 s. Touch IC FT6336 exists on the touch version only |
| RTC | PCF85063 (back side) |
| Sensor | SHTC3 temperature and humidity |
| Audio | ES8311 codec, on-board microphone, speaker header (MX1.25 2-pin) |
| Storage | TF-card slot (FAT32), 1-line SDMMC in the vendor demo |
| Keys | PWR (side; press and hold to power on or off) and BOOT (side; hold it while powering on again to enter download mode) |
| Connectors | USB Type-C (the S3's USB: flashing and serial log), MX1.25 2-pin lithium battery header, 2 x 6 pin 2.54 mm female expansion header |
| Charger | lithium recharge management circuit on the board (ETA6098 on the schematic, PLAN §2) |

The board is 39.8 x 53.0 x 16.9 mm (drawing; case included), screen 27.8 x 27.8 mm. Development: Arduino or
ESP-IDF; **the board needs ESP-IDF 5.5.0 or newer** [S] (we use 5.5.5).

## Battery and power [S]

- Standby on the lithium battery, in the vendor's low-power mode: **about 4 days** waking every 60 s,
  **about 10 days** if it stays in low-power mode. Measured whole-board current in low-power mode: **1 mA**.
  (The battery's capacity is not stated; ours is not known either, PLAN §2.)
- Charging: **30 minutes to 4.1 V, 44 minutes to full**. Our battery log agrees about the end of the charge: the
  ADC reads 4082-4114 mV after the charger ends the charge, and an hour more on USB adds nothing (`WS154_BAT_FULL_MV`, PLAN D104) [V].
- With only the battery connected, the PWR key powers the board on (hold until the screen shows "ON", then
  release) and off (hold until "OFF"). Vendor demo `07_BATT_PWR_Test`. In the vendor code the same pin set holds
  the power latch (`BAT_Control`, GPIO17) [S]; our firmware sets it early in `app_main` (`WS154_BAT_LATCH`).
- Battery voltage is read with ADC1 channel 3 (GPIO4) [S: demo `01_ADC_Test`]; the divider is x2 [V: schematic].

## GPIO overview from the wiki's "Interface Introduction" picture [S]

GPIO to function, as the vendor lists it (V1 and V2 use the same pins, PLAN §2):

| Group | Pins |
|---|---|
| e-paper | IO6 EPD3V3_EN, IO7 EPD_TP_RST (touch reset), IO8 BUSY, IO9 RST, IO10 D/C, IO11 CS, IO12 SCLK, IO13 SDI, IO21 EPD_TP_INT (touch), IO47 / IO48 EPD_TP_SDA / SCL |
| SD card | IO39 SD_CLK, IO40 SD_MISO, IO41 SD_MOSI |
| I2S (ES8311) | IO14 MCLK, IO15 SCLK, IO16 ASDOUT, IO38 LRCK, IO45 DSDIN, IO42 PA_EN, IO46 PA_CTRL |
| USB and UART | IO19 U_N, IO20 U_P, IO43 TXD, IO44 RXD |
| RTC | IO5 RTC_INT, IO47 RTC_SDA, IO48 RTC_SCL |
| Other | IO0 BOOT0, IO4 BAT_ADC, IO17 BAT_Control, IO18 (labelled PA_EN in the picture, see below) |
| "OUTPUT" column (expansion header) | IO1, IO2, IO3, IO19, IO20, IO43, IO44, IO47, IO48 (several are shared with the buses above) |

I2C devices on IO47/IO48: PCF85063 (0x51), SHTC3 (0x70), ES8311 (0x18), and FT6336 on the touch version.

## Vendor ESP-IDF demos (repo `waveshareteam/ESP32-S3-ePaper-1.54`, no licence: read, never copy)

`01_ADC_Test` battery voltage, `02_I2C_PCF85063` RTC, `03_I2C_STHC3` SHTC3, `04_SD_Card`, `05_WIFI_AP`,
`06_WIFI_STA`, `07_BATT_PWR_Test` PWR key on battery, `08_Audio_Test`, `09_LVGL_V8_Test`, `10_LVGL_V9_Test`,
`11_FactoryProgram` (also offered as a ready BIN), `12_RTC_Sleep_Test` (RTC alarm wake-up from low power),
`13_FT6336_Test` (touch version only). The `V2` folder is the one for this board.

Hardware files on the same repo (`04_Hardware`): the schematic PDF (one sheet for V1/V2, with and without touch),
and mechanical drawings as zips.

## Datasheets the wiki links

- ESP32-S3: datasheet and technical reference manual (documentation.espressif.com)
- PCF85063: <https://files.waveshare.com/wiki/common/Pcf85063atl1118-NdPQpTGE-loeW7GbZ7.pdf>
- SHTC3: <https://files.waveshare.com/wiki/common/SHTC3_Datasheet.pdf>
- e-paper panel: <https://files.waveshare.com/wiki/common/1.54inch_e-paper_V2_Datasheet.pdf>
  (the file name says "1.54inch e-paper V2": the panel datasheet, not the board version; we have **not read it**,
  so the controller command values in `ws154_epd.c` still carry their `TODO(verify)`, PLAN D38)

## Doubtful points (do not act on these without a check)

- The picture lists **IO18 as "PA_EN"**, and IO42 as "PA_EN" as well. The schematic we read has IO18 = BAT_KEY (the
  PWR key) and IO42 = PA_EN, and the firmware has run the PWR key on IO18 on the board since 2026-10-15
  (hw-checklist 16, 61). Treat the picture's IO18 label as a vendor typo.
- The wiki's example pages call `05_WIFI_AP` an `.ino` file in an ESP-IDF section: copy-paste from the Arduino page.
  Its sample hotspot and router credentials are placeholders from the vendor.
- The 4-day / 10-day standby figures are the vendor's low-power mode, not our firmware's. Our own figures come
  from the battery log (`docs/hw-checklist.md`, PLAN D95/D103).
