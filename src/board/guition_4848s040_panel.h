#pragma once

#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sends the ST7701 init sequence over the bit-banged 3-wire SPI, then starts the RGB panel.
// `lines_bounce`: height of the RGB bounce buffer in lines (internal RAM, keeps PSRAM reads off the panel clock).
esp_lcd_panel_handle_t guition_4848s040_panel_init(int lines_bounce);

#ifdef __cplusplus
}
#endif
