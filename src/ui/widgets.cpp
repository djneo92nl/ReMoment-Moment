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

static lv_obj_t *badge(lv_obj_t *parent, const Layout &l, const char *text) {
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, color::text(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
    lv_obj_set_style_pad_hor(b, l.gap, 0);
    lv_obj_set_style_pad_ver(b, 2, 0);
    lv_obj_t *t = label(b, text, l.font_small, color::muted());
    lv_obj_center(t);
    return b;
}

lv_obj_t *source_row(lv_obj_t *parent, const Layout &l, const char *name, const char *provider, const char *input,
                     bool borrowed, bool active, lv_event_cb_t on_click, void *user_data) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r, lv_pct(100), l.row);
    lv_obj_set_style_radius(r, l.gap, 0);
    lv_obj_set_style_border_width(r, active ? 3 : 1, 0);
    lv_obj_set_style_border_color(r, active ? color::text() : lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_color(r, color::text(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(r, l.margin, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, l.gap, 0);
    if (on_click) lv_obj_add_event_cb(r, on_click, LV_EVENT_CLICKED, user_data);

    lv_obj_t *text = box(r);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *n = label(text, name, l.font_body, color::text());
    lv_obj_set_width(n, lv_pct(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    if (provider && *provider) {
        lv_obj_t *p = label(text, provider, l.font_small, color::muted());
        lv_obj_set_width(p, lv_pct(100));
        lv_label_set_long_mode(p, LV_LABEL_LONG_DOT);
    }
    if (input && *input) badge(r, l, input);
    if (borrowed) badge(r, l, "Borrowed");
    return r;
}

static void gesture_cb(lv_event_t *e) {
    GestureFn fn = (GestureFn)lv_event_get_user_data(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    lv_indev_wait_release(lv_indev_get_act());
    fn(dir);
}

void on_gesture(lv_obj_t *obj, GestureFn fn) { lv_obj_add_event_cb(obj, gesture_cb, LV_EVENT_GESTURE, (void *)fn); }

}  // namespace ui::widgets
