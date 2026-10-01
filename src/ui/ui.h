// UI layer. Every function here takes the LVGL lock itself, so call them from any task
// except the LVGL task (event callbacks there already hold the lock).
#pragma once

#include <stdint.h>

namespace ui {

void init();

// Startup / setup / pairing screen: logo, a status line and an optional big code.
void show_status(const char *message, const char *detail = nullptr, const char *code = nullptr);

void show_now_playing();
void now_playing_set_device(const char *name);
void now_playing_set_track(const char *artist, const char *track);
void now_playing_set_progress(int percent);
void now_playing_set_playing(bool playing);

// Small icon in the top-left corner while something is down; hidden when all is well.
void set_connection(bool wifi, bool server, bool mqtt);

}  // namespace ui
