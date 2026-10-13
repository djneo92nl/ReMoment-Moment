// Volume screen (swipe right on Now Playing): device name as header, a big volume slider
// with mute, standby, and the active source when the device reports one. Internal to the
// UI layer: callers hold the LVGL lock.
#pragma once

#include <lvgl.h>

#include "layout.h"
#include "model.h"
#include "ui.h"

namespace ui::volume_screen {

void build(const Layout &layout);
lv_obj_t *screen();

// The current device can set its volume (Now Playing only opens the screen then)
bool available();
// Refreshes the widgets and asks for the active source
void open();

// Same arguments as now_playing::update(): mirrors the current device
void update(const Device &device, uint8_t what);
// Source picker of `device_id`; count 0 hides the section
void set_sources(int device_id, const Option *sources, int count);

}  // namespace ui::volume_screen
