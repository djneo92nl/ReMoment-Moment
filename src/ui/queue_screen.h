// Queue screen (swipe left on Now Playing): the upcoming tracks of the current device as a
// list, in the look of the Library lists. Internal to the UI layer: callers hold the LVGL lock.
#pragma once

#include <lvgl.h>

#include "layout.h"
#include "model.h"
#include "ui.h"

namespace ui::queue_screen {

void build(const Layout &layout);
lv_obj_t *screen();

// The current device has a queue (Now Playing only opens the screen then)
bool available();
// Shows "Loading..." and asks for the queue
void open();

// Same arguments as now_playing::update(): keeps the capabilities and reloads the queue
// when the track changes while the screen is showing
void update(const Device &device, uint8_t what);

void set_items(int device_id, const QueueItem *items, int count);
void load_failed(int device_id, bool unsupported);

}  // namespace ui::queue_screen
