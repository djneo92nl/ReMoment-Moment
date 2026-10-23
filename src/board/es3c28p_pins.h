// ES3C28P 3.5" ESP32-S3 display module: pin map, from ~/Code/MeetMicRec/include/board_config.h
// (confirmed on hardware there for display, touch and audio).
#pragma once

#define ES3C28P_LCD_H_RES   320
#define ES3C28P_LCD_V_RES   480

// Case mounts the panel upside down: 1 = rotate display and touch 180 degrees
#ifndef ES3C28P_ROTATE_180
#define ES3C28P_ROTATE_180  1
#endif
#define ES3C28P_MADCTL      (ES3C28P_ROTATE_180 ? 0xC0 : 0x00)  // MY | MX

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

// RGB status LED (WS2812, single wire)
#define ES3C28P_RGB_LED     40

// ES8311 codec (I2C 0x18 on the touch bus) and speaker amplifier
#define ES3C28P_I2S_MCK     17
#define ES3C28P_I2S_SCK     18  // bit clock
#define ES3C28P_I2S_DO      16  // to the speaker
#define ES3C28P_I2S_LRC     21  // word select
#define ES3C28P_I2S_DI      15  // from the mic (unused)
#define ES3C28P_AUDIO_EN    1   // amplifier, low = on
