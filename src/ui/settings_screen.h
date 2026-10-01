// Settings screen (swipe up from the device list): brightness, LED, tap sound and device info.
// Internal to the UI layer: callers hold the LVGL lock.
#pragma once

#include <lvgl.h>

#include "layout.h"
#include "ui.h"

namespace ui::settings_screen {

void build(const Layout &layout);
lv_obj_t *screen();

// Refreshes slider values and asks the loop task for the info section
void open();
void set_info(const SettingsInfo &info);

}  // namespace ui::settings_screen
