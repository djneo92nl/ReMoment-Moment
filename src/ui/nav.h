// Screen navigation inside the UI layer (LVGL task; the lock is already held).
#pragma once

namespace ui::nav {

void to_now_playing();
void to_devices();
void to_details(int device_id);
void to_library();
void to_settings();
// Side screens of Now Playing; stay put (with a toast) when the device can't do it
void to_volume();
void to_queue();

// Toast on the top layer, shared by all screens
void toast(const char *text);

}  // namespace ui::nav
