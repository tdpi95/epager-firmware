/*
 * Waveshare ESP32-S3-ePaper-1.54 V2: every pin and electrical polarity the firmware uses, in ONE
 * place. Source: Waveshare schematic ESP32-S3-Touch-ePaper-1.54-Schematic.pdf (one sheet for both
 * versions; read 2026-10-11) and the ESP32-S3-PICO-1 datasheet. The Waveshare ESP-IDF V2 demo
 * (07_BATT_PWR_Test/main/user_config.h) agrees with the schematic on every pin below.
 *
 * Module ESP32-S3-PICO-1-N8R8: 8 MB quad flash and 8 MB OCTAL PSRAM in the package, so GPIO33..37
 * (and SPICS1) are taken by the PSRAM and must never be used. GPIO19/20 are the USB D-/D+ pair of
 * the Type-C connector (console = USB Serial/JTAG).
 */
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* ---- e-paper panel, 24-pin FPC J10 (sheet "e-Paper & Touch"); BS is strapped to GND = 4-wire SPI */
#define WS154_EPD_SPI_HOST SPI2_HOST
#define WS154_EPD_BUSY GPIO_NUM_8 /* high = busy */
#define WS154_EPD_RST GPIO_NUM_9
#define WS154_EPD_DC GPIO_NUM_10  /* 0 = command, 1 = data */
#define WS154_EPD_CS GPIO_NUM_11
#define WS154_EPD_SCLK GPIO_NUM_12
#define WS154_EPD_SDI GPIO_NUM_13 /* MOSI */

/* ---- power (sheets "VBAT", "e-Paper & Touch", "Codec") */
/* EPD3V3_EN: gate of PMOS Q2 (AO3401) with a 10 K pull-up to 3V3 -> LOW switches the panel rail ON. */
#define WS154_EPD_PWR GPIO_NUM_6
/* PA_EN: gate of PMOS Q6 (AO3401), pull-up R73 10 K -> LOW switches the audio amplifier rail ON.
 * The firmware keeps it OFF (no audio in v1). PA_CTRL (GPIO46) is the amplifier enable, unused. */
#define WS154_AUDIO_PWR GPIO_NUM_42
/* ---- audio (sheet "Codec", read 2026-10-15 in the Touch variant's schematic; ES8311 U10, amplifier NS4150B):
 * the ESP32 is the I2S master, the codec a slave. I2S_ASDOUT (GPIO16, codec output = microphone) is unused. */
#define WS154_I2S_MCLK GPIO_NUM_14
#define WS154_I2S_BCLK GPIO_NUM_15 /* "I2S_SCLK" */
#define WS154_I2S_LRCK GPIO_NUM_38
#define WS154_I2S_DOUT GPIO_NUM_45 /* "I2S_DSDIN": ESP32 to the codec's DAC */
#define WS154_PA_CTRL GPIO_NUM_46  /* NS4150B enable, high = on (pulled down: off by default). TODO(hw-verify) */
#define WS154_CODEC_ADDR 0x18
/* BAT_Control: drives NPN T2 (R64 1 K) that pulls the gate of PMOS Q4 low, connecting VBAT to the
 * system. HIGH = battery connected = the power latch. Pressing the PWR key also pulls that gate
 * low (through D9), which is what powers the board on from battery: the firmware must set this pin
 * HIGH early, and LOW to power off (it does not switch USB power: Q5 / VBUS path is separate). */
#define WS154_BAT_LATCH GPIO_NUM_17

/* ---- keys (sheet "OUTPUT & KEY"). Both active low. */
#define WS154_BTN_BOOT GPIO_NUM_0 /* BOOT0 key, external pull-up R1 10 K */
#define WS154_BTN_PWR GPIO_NUM_18 /* BAT_KEY: PWR key through diode D8, pull-up R58 10 K */

/* ---- battery (sheet "Power & ADC"): VBAT -- R21 200 K -- BAT_ADC -- R38 200 K -- GND, so the pin
 * sees VBAT / 2. BAT_ADC is GPIO4 = ADC1 channel 3 on the S3. */
#define WS154_BAT_ADC_GPIO 4
#define WS154_BAT_DIVIDER 2
/* What the ADC reads (mV) with the cell at rest after the charger ended the charge: 4082-4114 over eight charges in
 * the battery log of 2026-10-17/19 (the highest reading under charge was 4166). Shown as 100 % (PLAN D104).
 * TODO(hw-verify): a multimeter on the cell would say whether the cell is full or the charger stops early. */
#define WS154_BAT_FULL_MV 4080
/* The charger (ETA6098 U4) STAT output only drives the orange LED; no GPIO reads VBUS or the charge
 * state, so "external power" cannot be read directly (see board.c). LED_G is GPIO3 (unused). */

/* ---- I2C bus shared by the RTC PCF85063A (U8 "PCF85063ATL", 0x51), SHTC3 (U1, 0x70), codec ES8311
 * (0x18) and the touch connector; external 4.7 K pull-ups R20/R70 (ws154_i2c.c). RTC_INT is GPIO5
 * (unused: no RTC alarms or timers). */
#define WS154_I2C_SDA GPIO_NUM_47
#define WS154_I2C_SCL GPIO_NUM_48
#define WS154_RTC_INT GPIO_NUM_5

/* Not available: the 32 kHz crystal pins of the ESP32 (GPIO15/16) are used as I2S_SCLK/I2S_ASDOUT,
 * so there is NO 32 kHz crystal for the ESP32 (Y2 belongs to the PCF85063). PLAN D17: BLE modem
 * sleep must use the main XTAL as low-power clock. */
