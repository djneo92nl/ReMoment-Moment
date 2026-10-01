// UI layer. Every function here takes the LVGL lock itself, so call them from any task
// except the LVGL task (event callbacks there already hold the lock). User input goes
// out through net/commands, never by touching the model from the LVGL task.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "model.h"

namespace ui {

void init();

// Startup / setup / pairing screen: logo, a status line and an optional big code.
void show_status(const char *message, const char *detail = nullptr, const char *code = nullptr);

void show_now_playing();

// What changed on a device, for now_playing_update()
enum : uint8_t {
    kUpdateTrack    = 1 << 0,
    kUpdateProgress = 1 << 1,
    kUpdateState    = 1 << 2,
    kUpdateVolume   = 1 << 3,
    kUpdateModes    = 1 << 4,
    kUpdateDevice   = 1 << 5,  // name, capabilities
    kUpdateAll      = 0xFF,
};

// Renders the parts of `device` named by `what` on Now Playing (the current device only).
void now_playing_update(const Device &device, uint8_t what);
void now_playing_set_notice(const char *text);  // small hint line, nullptr = hide

// Device list (swipe up): the full list, and single-row refreshes.
void devices_sync(const Device *devices, uint8_t count, int current);
void devices_update(int index, const Device &device);

// Details screen of one device: options loaded by commands::Type::LoadDetails.
enum class Section : uint8_t { Sources, Radio, Group };
struct Option {
    char id[48];
    char label[48];
    char detail[48];
    bool active;
};
void details_set_options(int device_id, Section section, const Option *options, int count);

// Short message in the middle of the screen (errors, confirmations)
void toast(const char *text);

// Artwork (called from the artwork task). Images stay owned by the caller:
// artwork_show() displays them (cross-fade for changed ones, nullptr = placeholder),
// artwork_release() guarantees an image is no longer referenced before it is overwritten.
void artwork_show(const lv_img_dsc_t *cover, bool cover_changed, const lv_img_dsc_t *background,
                  bool background_changed, bool has_accent, uint32_t accent);
void artwork_release(const lv_img_dsc_t *image);

// Small icon in the top-left corner while something is down; hidden when all is well.
void set_connection(bool wifi, bool server, bool mqtt);

}  // namespace ui
