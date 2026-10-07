// Now Playing screen. Internal to the UI layer: callers hold the LVGL lock.
#pragma once

#include <lvgl.h>

#include "layout.h"
#include "model.h"

namespace ui::now_playing {

void build(const Layout &layout);
lv_obj_t *screen();

void update(const Device &device, uint8_t what);  // what = ui::kUpdate* flags
void set_notice(const char *text);

void set_cover(const lv_img_dsc_t *image);       // nullptr = placeholder
void set_background(const lv_img_dsc_t *image);  // nullptr = black
void set_accent(lv_color_t color);
void artwork_pending(lv_color_t accent);  // new artwork requested, not there yet
void artwork_arrived();
void release_image(const lv_img_dsc_t *image);

}  // namespace ui::now_playing
