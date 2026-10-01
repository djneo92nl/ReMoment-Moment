// Waveshare ESP32-S3-Touch-LCD-7B: 1024x600 RGB panel, GT911 touch, CH422G-style IO expander.
#if defined(BOARD_WAVESHARE_7B)

#include <Arduino.h>

#include "board.h"
#include "io_extension.h"
#include "lvgl_port.h"
#include "user_sd.h"

namespace board {

static const Info kInfo = {"Waveshare ESP32-S3 7B", EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES};

void init() {
    // Must run first: also brings up the shared I2C bus and the IO expander
    esp_lcd_touch_handle_t tp_handle = touch_gt911_init();

    esp_lcd_panel_handle_t panel_handle = waveshare_esp32_s3_rgb_lcd_init();
    waveshare_rgb_lcd_bl_on();

    ESP_ERROR_CHECK(lvgl_port_init(panel_handle, tp_handle));
}

const Info &info() { return kInfo; }

bool lvgl_lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void lvgl_unlock() { lvgl_port_unlock(); }

void set_backlight(uint8_t percent) {
    if (percent == 0) {
        waveshare_rgb_lcd_bl_off();
        return;
    }
    waveshare_rgb_lcd_bl_on();
    // Untested on hardware: the vendor driver caps duty at 97 "to prevent the screen
    // from completely turning off", which suggests higher duty = dimmer.
    IO_EXTENSION_Pwm_Output(100 - min<uint8_t>(percent, 100));
}

void set_led(uint32_t) {}  // no status LED on this board

float battery_voltage() {
    // 10-bit ADC behind a 1:3 divider, 3.3 V reference
    float v = IO_EXTENSION_Adc_Input() * 3 * 3.3f / 1023.0f;
    return min(v, 4.2f);
}

bool sd_mount() { return sd_mmc_init() == ESP_OK; }

}  // namespace board

#endif
