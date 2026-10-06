// ST7701 bring-up for the Guition ESP32-4848S040, in C because esp_lcd_rgb_panel_config_t uses
// designated initializers out of declaration order. The init table is the one Arduino_GFX
// ships for this board (st7701_type9_init_operations); timings are its example's values.
#if defined(BOARD_ESP32_4848S040)

#include "guition_4848s040_panel.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "guition_4848s040_pins.h"

static const char *TAG = "g4848";

// Entries: command, data length, data..., terminated by command 0x00 with length 0xFF.
// Length 0xFE = delay of the next byte in ms.
#define DELAY_MS 0xFE
static const uint8_t kInit[] = {
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x10,
    0xC0, 2, 0x3B, 0x00,
    0xC1, 2, 0x0D, 0x02,
    0xC2, 2, 0x31, 0x05,
    0xCD, 1, 0x00,
    0xB0, 16, 0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18,
    0xB1, 16, 0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18,
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x11,
    0xB0, 1, 0x60,
    0xB1, 1, 0x32,
    0xB2, 1, 0x07,
    0xB3, 1, 0x80,
    0xB5, 1, 0x49,
    0xB7, 1, 0x85,
    0xB8, 1, 0x21,
    0xC1, 1, 0x78,
    0xC2, 1, 0x78,
    0xE0, 3, 0x00, 0x1B, 0x02,
    0xE1, 11, 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44,
    0xE2, 12, 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00,
    0xE3, 4, 0x00, 0x00, 0x11, 0x11,
    0xE4, 2, 0x44, 0x44,
    0xE5, 16, 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0,
    0xE6, 4, 0x00, 0x00, 0x11, 0x11,
    0xE7, 2, 0x44, 0x44,
    0xE8, 16, 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0,
    0xEB, 7, 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40,
    0xEC, 2, 0x3C, 0x00,
    0xED, 16, 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA,
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x13,
    0xE5, 1, 0xE4,
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x00,
    0x3A, 1, 0x60,  // as in the Arduino_GFX table (0x70 RGB888, 0x60 RGB666, 0x50 RGB565)
    0x11, 0,
    0x00, DELAY_MS, 120,
    0x29, 0,
};

static void spi_word(bool data, uint8_t value) {
    uint16_t word = (data ? 0x100 : 0) | value;
    for (int bit = 8; bit >= 0; bit--) {
        gpio_set_level(G4848_LCD_SCK, 0);
        gpio_set_level(G4848_LCD_MOSI, (word >> bit) & 1);
        esp_rom_delay_us(2);
        gpio_set_level(G4848_LCD_SCK, 1);
        esp_rom_delay_us(2);
    }
    gpio_set_level(G4848_LCD_SCK, 0);
}

static void send_init(void) {
    uint64_t mask = (1ULL << G4848_LCD_CS) | (1ULL << G4848_LCD_SCK) | (1ULL << G4848_LCD_MOSI);
    gpio_config_t io = {.pin_bit_mask = mask, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(G4848_LCD_CS, 1);

    for (size_t i = 0; i < sizeof(kInit);) {
        uint8_t cmd = kInit[i++], len = kInit[i++];
        if (cmd == 0x00 && len == DELAY_MS) {
            vTaskDelay(pdMS_TO_TICKS(kInit[i++]));
            continue;
        }
        gpio_set_level(G4848_LCD_CS, 0);
        spi_word(false, cmd);
        for (uint8_t n = 0; n < len; n++) spi_word(true, kInit[i++]);
        gpio_set_level(G4848_LCD_CS, 1);
        esp_rom_delay_us(5);
    }
}

esp_lcd_panel_handle_t guition_4848s040_panel_init(int lines_bounce) {
    send_init();

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings =
            {
                .pclk_hz           = 12 * 1000 * 1000,
                .h_res             = G4848_LCD_H_RES,
                .v_res             = G4848_LCD_V_RES,
                .hsync_pulse_width = 8,
                .hsync_back_porch  = 50,
                .hsync_front_porch = 10,
                .vsync_pulse_width = 8,
                .vsync_back_porch  = 20,
                .vsync_front_porch = 10,
                .flags             = {.hsync_idle_low = 0, .vsync_idle_low = 0, .pclk_active_neg = 0},
            },
        .data_width            = 16,
        .bits_per_pixel        = 16,
        .num_fbs               = 1,
        .bounce_buffer_size_px = G4848_LCD_H_RES * lines_bounce,
        .sram_trans_align      = 4,
        .psram_trans_align     = 64,
        .hsync_gpio_num        = G4848_RGB_HSYNC,
        .vsync_gpio_num        = G4848_RGB_VSYNC,
        .de_gpio_num           = G4848_RGB_DE,
        .pclk_gpio_num         = G4848_RGB_PCLK,
        .disp_gpio_num         = -1,
        .data_gpio_nums =
            {
                G4848_RGB_B0, G4848_RGB_B1, G4848_RGB_B2, G4848_RGB_B3, G4848_RGB_B4,
                G4848_RGB_G0, G4848_RGB_G1, G4848_RGB_G2, G4848_RGB_G3, G4848_RGB_G4, G4848_RGB_G5,
                G4848_RGB_R0, G4848_RGB_R1, G4848_RGB_R2, G4848_RGB_R3, G4848_RGB_R4,
            },
        .flags = {.fb_in_psram = 1},
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_LOGI(TAG, "panel up");
    return panel;
}

#endif
