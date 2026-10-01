#include "widgets.h"

namespace ui::widgets {

lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, lv_coord_t size, const lv_font_t *font,
                       lv_event_cb_t on_click, void *user_data) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, size / 6);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, color::text(), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(btn, color::text(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, color::text(), 0);
    lv_obj_set_style_text_font(btn, font, 0);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_center(icon);

    if (on_click) lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, user_data);
    return btn;
}

void set_symbol(lv_obj_t *button, const char *symbol) { lv_label_set_text(lv_obj_get_child(button, 0), symbol); }

void set_active(lv_obj_t *button, bool active) {
    lv_obj_set_style_bg_opa(button, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(button, active ? color::bg() : color::text(), 0);
}

lv_obj_t *box(lv_obj_t *parent) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return b;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t c) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static void gesture_cb(lv_event_t *e) {
    GestureFn fn = (GestureFn)lv_event_get_user_data(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    lv_indev_wait_release(lv_indev_get_act());
    fn(dir);
}

void on_gesture(lv_obj_t *obj, GestureFn fn) { lv_obj_add_event_cb(obj, gesture_cb, LV_EVENT_GESTURE, (void *)fn); }

}  // namespace ui::widgets
