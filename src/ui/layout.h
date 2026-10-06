// Screen metrics derived from the panel size, so layouts don't hard-code one resolution.
#pragma once

#include <lvgl.h>

#include "board/board.h"

namespace ui {

struct Layout {
    lv_coord_t width, height;
    lv_coord_t margin;     // outer padding
    lv_coord_t gap;        // spacing between elements
    lv_coord_t cover;      // album art size (square)
    lv_coord_t btn_large;  // play/pause
    lv_coord_t btn;        // previous/next, header buttons
    lv_coord_t btn_small;  // shuffle/repeat/like
    lv_coord_t row;        // list row height
    lv_coord_t tile;       // album / artist tile in grids and shelves
    bool       landscape;
    const lv_font_t *font_title;  // track name
    const lv_font_t *font_body;   // artist, status text
    const lv_font_t *font_small;  // device name, hints
    const lv_font_t *font_code;   // pairing code
};

inline Layout make_layout() {
    const board::Info &b = board::info();
    Layout l;
    l.width     = b.width;
    l.height    = b.height;
    l.landscape = b.width > b.height;
    bool large  = LV_MIN(b.width, b.height) >= 480;
    // Roughly square (4" 480x480): the portrait stack of cover, text, transport and modes doesn't fit
    // with phone-sized buttons, so shrink margins and controls
    bool square = LV_ABS(b.width - b.height) < LV_MIN(b.width, b.height) / 8;
    l.margin    = LV_MIN(b.width, b.height) / (square ? 24 : 15);
    l.gap       = l.margin / 2;
    // Covers are rendered by the server at 320 px; don't upscale beyond that
    // Portrait: leave ~55% of the height for text, controls and the progress bar
    l.cover      = LV_MIN(320, l.landscape ? b.height - 2 * l.margin - 120
                                           : LV_MIN(b.width - 2 * l.margin, b.height * (square ? 41 : 45) / 100));
    l.btn_large  = square ? 72 : large ? 96 : 64;
    l.btn        = square ? 56 : large ? 72 : 48;
    l.btn_small  = square ? 44 : large ? 52 : 40;
    l.row        = large ? 88 : 64;
    l.tile       = large ? 160 : 120;  // 160 = a 320 cover decoded at half size, no runtime scaling
    l.font_title = large ? &lv_font_montserrat_32 : &lv_font_montserrat_20;
    l.font_body  = large ? &lv_font_montserrat_20 : &lv_font_montserrat_16;
    l.font_small = large ? &lv_font_montserrat_16 : &lv_font_montserrat_12;
    l.font_code  = &lv_font_montserrat_48;
    return l;
}

namespace color {
static inline lv_color_t bg() { return lv_color_hex(0x000000); }
static inline lv_color_t text() { return lv_color_hex(0xFFFFFF); }
static inline lv_color_t muted() { return lv_color_hex(0x808080); }
static inline lv_color_t accent() { return lv_color_hex(0xE6E6E6); }  // progress without album colour
static inline lv_color_t warning() { return lv_color_hex(0xE0A030); }
static inline lv_color_t error() { return lv_color_hex(0xD04040); }
}  // namespace color

}  // namespace ui
