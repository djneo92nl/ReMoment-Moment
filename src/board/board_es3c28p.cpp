// ES3C28P 3.5" module: 320x480 ST77922 over QSPI, custom-protocol touch at I2C 0x55.
// Display and touch handling ported from ~/Code/MeetMicRec (src/display.c, src/touch.c),
// LVGL glue written here (no esp_lvgl_port under Arduino).
#if defined(BOARD_ES3C28P)

#include <Arduino.h>
#include <SD_MMC.h>
#include <lvgl.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "es3c28p_audio.h"
#include "es3c28p_panel.h"
#include "es3c28p_pins.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"

namespace board {

static const Info kInfo = {"ES3C28P 3.5\"", ES3C28P_LCD_H_RES, ES3C28P_LCD_V_RES, true, true, true};

static const int      kBufferLines     = 40;  // partial buffers; full refresh trips the watchdog here
static const uint32_t kTickPeriodMs    = 2;
static const uint32_t kTaskMinDelayMs  = 5;
static const uint32_t kTaskMaxDelayMs  = 30;

static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t      lvgl_mux;
static lv_disp_draw_buf_t     draw_buf;
static lv_disp_drv_t          disp_drv;
static lv_indev_drv_t         indev_drv;
static i2c_master_bus_handle_t i2c_bus;  // touch controller and audio codec
static i2c_master_dev_handle_t touch_dev;
static uint8_t touch_points = 1;  // reported by the controller at init (max simultaneous points)

// ---- Display --------------------------------------------------------------------

static bool IRAM_ATTR on_transfer_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *ctx) {
    lv_disp_flush_ready((lv_disp_drv_t *)ctx);
    return false;
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels) {
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
}

// ST77922 quirk: flush areas must start and end on a multiple of 4 columns
static void rounder(lv_disp_drv_t *, lv_area_t *area) {
    area->x1 = (area->x1 >> 2) << 2;
    area->x2 = ((area->x2 >> 2) << 2) + 3;
}

// ---- Touch: custom protocol with 16-bit registers (not FT5x06) -------------------

static const uint16_t kRegMaxTouches  = 0x0009;
static const uint16_t kRegTouchInfo   = 0x0010;
static const uint16_t kRegTouchPoint0 = 0x0014;
static const uint8_t  kInfoHasData    = 0x08;
static const uint8_t  kPointValid     = 0x80;

static esp_err_t touch_read(uint16_t reg, uint8_t *buf, size_t len) {
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_transmit_receive(touch_dev, addr, sizeof(addr), buf, len, 50);
}

static void read_touch(lv_indev_drv_t *, lv_indev_data_t *data) {
    static lv_coord_t last_x = 0, last_y = 0;
    static bool pressed = false;
    static uint8_t frame[7 * 10];

    // TP_INT is low while a report is pending. The whole report frame (7 bytes for every
    // point the controller supports) must be read, like the vendor driver does: reading only
    // point 0 left the controller stuck on its first report with TP_INT low, which showed up
    // as a button that stayed pressed and repeated taps at the old position.
    bool touched = false;
    if (gpio_get_level((gpio_num_t)ES3C28P_TOUCH_INT) == 0) {
        uint8_t info = 0;
        if (touch_read(kRegTouchInfo, &info, 1) == ESP_OK && (info & kInfoHasData) &&
            touch_read(kRegTouchPoint0, frame, 7 * touch_points) == ESP_OK && (frame[0] & kPointValid)) {
            last_x  = ((frame[0] & 0x3F) << 8) | frame[1];
            last_y  = ((frame[2] & 0x3F) << 8) | frame[3];
            touched = true;
        }
    }
    if (touched != pressed) {
        pressed = touched;
        Serial.printf("[touch] %s %d,%d\n", touched ? "press" : "release", last_x, last_y);
    }
    data->point.x = last_x;
    data->point.y = last_y;
    data->state   = touched ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

static void touch_init() {
    gpio_config_t rst = {};
    rst.pin_bit_mask = 1ULL << ES3C28P_TOUCH_RST;
    rst.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&rst);
    gpio_config_t irq = {};
    irq.pin_bit_mask = 1ULL << ES3C28P_TOUCH_INT;
    irq.mode         = GPIO_MODE_INPUT;
    irq.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&irq);

    gpio_set_level((gpio_num_t)ES3C28P_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)ES3C28P_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = -1;
    bus_cfg.sda_io_num        = (gpio_num_t)ES3C28P_TOUCH_SDA;
    bus_cfg.scl_io_num        = (gpio_num_t)ES3C28P_TOUCH_SCL;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = ES3C28P_TOUCH_ADDR;
    dev_cfg.scl_speed_hz    = 400000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &touch_dev));

    uint8_t max_touches = 0;
    if (touch_read(kRegMaxTouches, &max_touches, 1) == ESP_OK) {
        touch_points = max_touches >= 1 && max_touches <= 10 ? max_touches : 1;
        Serial.printf("[board] touch controller ok, %u points\n", max_touches);
    } else {
        Serial.println("[board] touch controller not answering yet");
    }
}

// Tick on taps of things that react (not on bare screen background)
static void on_input_feedback(lv_indev_drv_t *, uint8_t event) {
    if (event != LV_EVENT_CLICKED) return;
    lv_obj_t *obj = lv_indev_get_obj_act();
    if (obj && lv_obj_get_parent(obj)) es3c28p_audio::click();
}

// ---- LVGL task --------------------------------------------------------------------

static void tick(void *) { lv_tick_inc(kTickPeriodMs); }

static void lvgl_task(void *) {
    for (;;) {
        uint32_t delay_ms = kTaskMaxDelayMs;
        if (lvgl_lock(-1)) {
            delay_ms = lv_timer_handler();
            lvgl_unlock();
        }
        delay_ms = constrain(delay_ms, kTaskMinDelayMs, kTaskMaxDelayMs);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ---------------------------------------------------------------------------------

void init() {
    lv_init();

    size_t buffer_bytes = ES3C28P_LCD_H_RES * kBufferLines * sizeof(lv_color_t);
    panel = es3c28p_panel_init(buffer_bytes, on_transfer_done, &disp_drv);

    // Two DMA-capable internal buffers: LVGL renders into one while the other is sent
    void *buf1 = heap_caps_malloc(buffer_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(buffer_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(buf1 && buf2);
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, ES3C28P_LCD_H_RES * kBufferLines);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res    = ES3C28P_LCD_H_RES;
    disp_drv.ver_res    = ES3C28P_LCD_V_RES;
    disp_drv.flush_cb   = flush;
    disp_drv.rounder_cb = rounder;
    disp_drv.draw_buf   = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    touch_init();
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = read_touch;
    indev_drv.feedback_cb = on_input_feedback;
    lv_indev_drv_register(&indev_drv);

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = tick;
    tick_args.name     = "lv_tick";
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, kTickPeriodMs * 1000));

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 6 * 1024, nullptr, 2, nullptr, 1);

    set_backlight(100);
    es3c28p_audio::init(i2c_bus);
}

const Info &info() { return kInfo; }

bool lvgl_lock(int timeout_ms) {
    TickType_t ticks = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_mux, ticks) == pdTRUE;
}

void lvgl_unlock() { xSemaphoreGiveRecursive(lvgl_mux); }

void set_backlight(uint8_t percent) { analogWrite(ES3C28P_LCD_BL, min<uint8_t>(percent, 100) * 255 / 100); }

static uint32_t led_color      = 0;
static uint8_t  led_brightness = 25;  // WS2812s are blinding at full power

static void show_led() {
    uint32_t r = (led_color >> 16) & 0xFF, g = (led_color >> 8) & 0xFF, b = led_color & 0xFF;
    rgbLedWrite(ES3C28P_RGB_LED, r * led_brightness / 100, g * led_brightness / 100, b * led_brightness / 100);
}

void set_led(uint32_t rgb) {
    if (rgb == led_color) return;
    led_color = rgb;
    show_led();
}

void set_led_brightness(uint8_t percent) {
    led_brightness = min<uint8_t>(percent, 100);
    show_led();
}

void set_click_volume(uint8_t volume) { es3c28p_audio::set_volume(volume); }

float battery_voltage() {
    // Divider ratio not yet confirmed on hardware; reads ~0 V without a battery.
    // Averaged: the ADC is noisy and the WiFi radio disturbs single reads.
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(ES3C28P_BATTERY_PIN);
    return sum / 8.0f * ES3C28P_BATTERY_DIVIDER / 1000.0f;
}

bool usb_power_present() {
    // Native USB (Serial/JTAG): connected once a host enumerates. A charger-only cable is not
    // seen here; battery.cpp also infers charging from the voltage trend.
    return usb_serial_jtag_is_connected();
}

bool sd_mount() {
    if (!SD_MMC.setPins(ES3C28P_SD_CLK, ES3C28P_SD_CMD, ES3C28P_SD_D0, ES3C28P_SD_D1, ES3C28P_SD_D2, ES3C28P_SD_D3)) {
        return false;
    }
    return SD_MMC.begin("/sdcard", false /* 4-bit */, false) && SD_MMC.cardType() != CARD_NONE;
}

}  // namespace board

#endif
