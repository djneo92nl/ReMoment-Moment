#pragma once

#include <stddef.h>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

// QSPI bus + ST77922 panel with the vendor init sequence. `on_done` runs (in ISR
// context) when a draw_bitmap transfer finished; `ctx` is passed through.
esp_lcd_panel_handle_t es3c28p_panel_init(size_t max_transfer_bytes, esp_lcd_panel_io_color_trans_done_cb_t on_done,
                                          void *ctx);

#ifdef __cplusplus
}
#endif
