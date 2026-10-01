// Small building blocks shared by the screens, in one visual language: black,
// thin white outlines, round buttons, muted secondary text. Callers hold the LVGL lock.
#pragma once

#include <lvgl.h>

#include "layout.h"

namespace ui::widgets {

// Round outline button with a symbol (LV_SYMBOL_*), `size` px across
lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, lv_coord_t size, const lv_font_t *font,
                       lv_event_cb_t on_click, void *user_data = nullptr);
void      set_symbol(lv_obj_t *button, const char *symbol);
// Active = filled white with a black symbol; inactive = outline only
void      set_active(lv_obj_t *button, bool active);

// Plain container without styling or scrolling
lv_obj_t *box(lv_obj_t *parent);

// Label with font and color in one call
lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);

// Swipe handling: calls `on_gesture(dir)` for LV_EVENT_GESTURE on `obj`
using GestureFn = void (*)(lv_dir_t dir);
void on_gesture(lv_obj_t *obj, GestureFn fn);

}  // namespace ui::widgets
