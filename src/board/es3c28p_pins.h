// ES3C28P 3.5" ESP32-S3 display module: pin map, from ~/Code/MeetMicRec/include/board_config.h
// (confirmed on hardware there for display, touch and audio).
#pragma once

#define ES3C28P_LCD_H_RES   320
#define ES3C28P_LCD_V_RES   480

// ST77922 panel over QSPI
#define ES3C28P_LCD_CS      10
#define ES3C28P_LCD_SCK     12
#define ES3C28P_LCD_D0      11
#define ES3C28P_LCD_D1      13
#define ES3C28P_LCD_D2      14
#define ES3C28P_LCD_D3      9
#define ES3C28P_LCD_BL      41  // high = on

// Touch controller (custom protocol, not FT5x06), I2C
#define ES3C28P_TOUCH_SDA   38
#define ES3C28P_TOUCH_SCL   39
#define ES3C28P_TOUCH_RST   48  // low = reset
#define ES3C28P_TOUCH_INT   47  // low while touched
#define ES3C28P_TOUCH_ADDR  0x55

// SD card, SDIO 4-bit
#define ES3C28P_SD_CLK      5
#define ES3C28P_SD_CMD      4
#define ES3C28P_SD_D0       6
#define ES3C28P_SD_D1       7
#define ES3C28P_SD_D2       2
#define ES3C28P_SD_D3       3

// Battery ADC (divider ratio unverified on hardware)
#define ES3C28P_BATTERY_PIN      8
#define ES3C28P_BATTERY_DIVIDER  2.0f

// Not used yet: RGB LED (WS2812) on 40, ES8311 audio (I2S MCK 17, SCK 18, DO 16, LRC 21, DI 15, EN 1)
