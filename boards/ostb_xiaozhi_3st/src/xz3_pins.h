/*
 * ostb-xiaozhi-3st (LCD board): every pin and bus setting in one place. There is no schematic
 * (PLAN D48): [S+] = read from the stock firmware's code, [S] = community ESPHome port; every pin
 * here is [S+] and agrees with [S]. None is verified on the board yet: TODO(hw-verify), hw-checklist
 * items 21-28. Details and how they were read: docs/ostb-xiaozhi-3st.md.
 */
#pragma once

/* ---------------------------------------------------------------- LCD (SPI3, write only) */
#define XZ3_LCD_HOST SPI3_HOST
#define XZ3_LCD_SCLK 9
#define XZ3_LCD_MOSI 10
#define XZ3_LCD_CS 14
#define XZ3_LCD_DC 8
#define XZ3_LCD_RST 18          /* active low */
#define XZ3_LCD_SPI_HZ 80000000 /* checklist 23: clean on the board; the stock firmware uses 40 MHz (fall back to it if
                                 * a panel shows glitches); 60 MHz is not reachable: 80 MHz / whole divider */
#define XZ3_LCD_BL 13           /* backlight, LEDC PWM, active high */
#define XZ3_LCD_BL_HZ 25000     /* stock PWM frequency */

/* Panel geometry as the stock firmware drives it: 240 x 296 native, used landscape (MADCTL MV|MY),
 * the visible 296 columns at 24..319 of a 320-pixel controller axis. */
#define XZ3_LCD_W 296
#define XZ3_LCD_H 240
#define XZ3_LCD_X_GAP 24
#define XZ3_LCD_Y_GAP 0
#define XZ3_LCD_EDGE_INSET 14   /* rounded corners: the status bar's texts sit 14 px further in */

/* ---------------------------------------------------------------- I2C: touch and audio chips */
#define XZ3_I2C_PORT 0
#define XZ3_I2C_SDA 12
#define XZ3_I2C_SCL 11
#define XZ3_TOUCH_ADDR 0x15     /* CST816S; no reset and no interrupt pin (the stock firmware polls) */
#define XZ3_TOUCH_HZ 400000
#define XZ3_ES8311_ADDR 0x18    /* codec (speaker); not used by ePager */
#define XZ3_ES7210_ADDR 0x40    /* microphone ADC; not used by ePager */

/* ---------------------------------------------------------------- audio (unused, kept silent) */
#define XZ3_AMP_EN 4            /* speaker amplifier enable, high = on */
/* I2S, for reference only: MCLK 5, BCLK 15, WS 16, DOUT 6 (speaker), DIN 7 (microphones). */

/* ---------------------------------------------------------------- keys, power, battery */
#define XZ3_KEY_UP 40           /* volume up, active low */
#define XZ3_KEY_DOWN 39         /* volume down, active low */
/* The power key is on no GPIO: a power chip switches the board in hardware (docs/ostb-xiaozhi-3st.md). */
#define XZ3_CHG_STAT 47         /* charge status, input with pull-up; low = charging (seen on the board) */
#define XZ3_BAT_ADC_GPIO 17     /* ADC2 channel 6, 12 dB attenuation, read raw like the stock firmware */
