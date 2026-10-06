// Guition ESP32-4848S040 (4" 480x480): pin map from the Arduino_GFX device table
// (examples/PDQgraphicstest, ESP32_4848S040_86BOX_GUITION) and the ESPHome device page.
#pragma once

#define G4848_LCD_H_RES  480
#define G4848_LCD_V_RES  480

// ST7701 panel: init over a bit-banged 3-wire SPI (9-bit words), pixels over 16-bit RGB
#define G4848_LCD_CS     39
#define G4848_LCD_SCK    48
#define G4848_LCD_MOSI   47
#define G4848_LCD_BL     38  // high = on, PWM

#define G4848_RGB_DE     18
#define G4848_RGB_VSYNC  17
#define G4848_RGB_HSYNC  16
#define G4848_RGB_PCLK   21

// RGB565 data lines, in esp_lcd order: D0..D4 = B, D5..D10 = G, D11..D15 = R
#define G4848_RGB_B0     4
#define G4848_RGB_B1     5
#define G4848_RGB_B2     6
#define G4848_RGB_B3     7
#define G4848_RGB_B4     15
#define G4848_RGB_G0     8
#define G4848_RGB_G1     20
#define G4848_RGB_G2     3
#define G4848_RGB_G3     46
#define G4848_RGB_G4     9
#define G4848_RGB_G5     10
#define G4848_RGB_R0     11
#define G4848_RGB_R1     12
#define G4848_RGB_R2     13
#define G4848_RGB_R3     14
#define G4848_RGB_R4     0

// GT911 touch, I2C; no reset or interrupt line is wired. The address depends on the INT level
// at power-up, so both are probed.
#define G4848_TOUCH_SDA  19
#define G4848_TOUCH_SCL  45

// SD slot, SPI-wired only (SDMMC mode finds no card). MOSI/CLK are shared with the panel's init
// SPI, so the panel's CS must stay high.
#define G4848_SD_CS      42
#define G4848_SD_MOSI    47
#define G4848_SD_CLK     48
#define G4848_SD_MISO    41
