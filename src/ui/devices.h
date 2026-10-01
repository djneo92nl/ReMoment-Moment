// Device list (swipe up from Now Playing) and per-device details (power, sources,
// radio, multiroom). Internal to the UI layer: callers hold the LVGL lock.
#pragma once

#include <lvgl.h>

#include "layout.h"
#include "model.h"
#include "ui.h"

namespace ui::devices {

void build(const Layout &layout);
lv_obj_t *list_screen();
lv_obj_t *details_screen();

void sync(const Device *devices, uint8_t count, int current);
void update(int index, const Device &device);

// Fills the details screen for `device_id` (sections load asynchronously)
void open_details(int device_id);
void set_options(int device_id, Section section, const Option *options, int count);

}  // namespace ui::devices
