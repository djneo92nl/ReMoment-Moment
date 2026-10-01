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

}  // namespace ui::now_playing
