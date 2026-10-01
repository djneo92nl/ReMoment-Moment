// Nothing-playing screen: the time, the date and the current device's state.
// Internal to the UI layer: callers hold the LVGL lock.
#pragma once

#include <lvgl.h>

#include "layout.h"
#include "model.h"

namespace ui::idle {

void build(const Layout &layout);
lv_obj_t *screen();

void set_device(const Device *device);

}  // namespace ui::idle
