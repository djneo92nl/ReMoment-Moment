// Now Playing screen. Internal to the UI layer: callers must hold the LVGL lock.
#pragma once

#include <lvgl.h>

#include "layout.h"

namespace ui::now_playing {

void build(const Layout &layout);
lv_obj_t *screen();

void set_device(const char *name);
void set_track(const char *artist, const char *track);
void set_progress(int percent);
void set_playing(bool playing);
void set_notice(const char *text);

void set_cover(const lv_img_dsc_t *image);       // nullptr = placeholder
void set_background(const lv_img_dsc_t *image);  // nullptr = black
void set_accent(lv_color_t color);
void release_image(const lv_img_dsc_t *image);

}  // namespace ui::now_playing
