// Guition ESP32-4848S040 4" module: 480x480 ST7701 RGB panel, GT911 touch (no reset/INT wired),
// backlight on a PWM pin, no battery/LED/speaker. LVGL glue follows board_es3c28p.cpp.
#if defined(BOARD_ESP32_4848S040)

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <lvgl.h>

#include "board.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "guition_4848s040_panel.h"
#include "guition_4848s040_pins.h"

namespace board {

static const Info kInfo = {"Guition 4848S040 4\"", G4848_LCD_H_RES, G4848_LCD_V_RES, false, false, false};

static const int      kBounceLines    = 10;  // RGB bounce buffer (internal RAM)
static const int      kBufferLines    = 20;  // LVGL draw buffer, internal RAM like the 7" board
static const uint32_t kTickPeriodMs   = 2;
static const uint32_t kTaskMinDelayMs = 5;
static const uint32_t kTaskMaxDelayMs = 30;

static esp_lcd_panel_handle_t  panel;
static SemaphoreHandle_t       lvgl_mux;
static lv_disp_draw_buf_t      draw_buf;
static lv_disp_drv_t           disp_drv;
static lv_indev_drv_t          indev_drv;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t touch_dev;

// ---- Display --------------------------------------------------------------------

static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels) {
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
    lv_disp_flush_ready(drv);
}

// ---- Touch: GT911, 16-bit register addresses ---------------------------------------

static const uint16_t kRegStatus = 0x814E;  // bit 7 = data ready, low nibble = point count
static const uint16_t kRegPoint0 = 0x814F;  // track id, x lo, x hi, y lo, y hi, ...

static esp_err_t touch_read(uint16_t reg, uint8_t *buf, size_t len) {
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_transmit_receive(touch_dev, addr, sizeof(addr), buf, len, 20);
}

static void touch_clear_status() {
    uint8_t cmd[3] = {(uint8_t)(kRegStatus >> 8), (uint8_t)(kRegStatus & 0xFF), 0};
    i2c_master_transmit(touch_dev, cmd, sizeof(cmd), 20);
}

static void read_touch(lv_indev_drv_t *, lv_indev_data_t *data) {
    static lv_coord_t last_x = 0, last_y = 0;
    static bool pressed = false;

    uint8_t status = 0;
    if (touch_read(kRegStatus, &status, 1) == ESP_OK && (status & 0x80)) {
        uint8_t points = status & 0x0F;
        if (points > 0) {
            uint8_t p[5];
            if (touch_read(kRegPoint0, p, sizeof(p)) == ESP_OK) {
                last_x  = p[1] | (p[2] << 8);
                last_y  = p[3] | (p[4] << 8);
                pressed = true;
            }
        } else {
            pressed = false;
        }
        touch_clear_status();  // the controller won't report again until this is cleared
    }
    data->point.x = last_x;
    data->point.y = last_y;
    data->state   = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

static void touch_init() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = -1;
    bus_cfg.sda_io_num        = (gpio_num_t)G4848_TOUCH_SDA;
    bus_cfg.scl_io_num        = (gpio_num_t)G4848_TOUCH_SCL;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    // Without reset/INT wiring the controller's address is whatever it powered up with
    uint16_t addr = 0x5D;
    if (i2c_master_probe(i2c_bus, 0x5D, 50) != ESP_OK && i2c_master_probe(i2c_bus, 0x14, 50) == ESP_OK) addr = 0x14;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = addr;
    dev_cfg.scl_speed_hz    = 400000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &touch_dev));

    uint8_t id[4] = {0};
    if (touch_read(0x8140, id, sizeof(id)) == ESP_OK) {
        Serial.printf("[board] touch GT%c%c%c%c at 0x%02X\n", id[0], id[1], id[2], id[3], addr);
    } else {
        Serial.printf("[board] touch controller not answering at 0x%02X\n", addr);
    }
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

    panel = guition_4848s040_panel_init(kBounceLines);
    // The panel's chip-select was found low after bring-up, which makes it listen to the SD bus
    // it shares (SPI 47/48): hold it high
    pinMode(G4848_LCD_CS, OUTPUT);
    digitalWrite(G4848_LCD_CS, HIGH);

    size_t pixels = G4848_LCD_H_RES * kBufferLines;
    void  *buf    = heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(buf);
    lv_disp_draw_buf_init(&draw_buf, buf, nullptr, pixels);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = G4848_LCD_H_RES;
    disp_drv.ver_res  = G4848_LCD_V_RES;
    disp_drv.flush_cb = flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    lv_timer_set_period(disp->refr_timer, 16);  // ~60 Hz target (was 30 ms): smoother scrolling

    touch_init();
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = read_touch;
    indev_drv.scroll_limit = 6;   // start a drag after 6 px (default 10)
    indev_drv.scroll_throw = 5;   // keep flick momentum longer (default 10: loses 10 % per step)
    lv_indev_t *indev = lv_indev_drv_register(&indev_drv);
    lv_timer_set_period(indev->driver->read_timer, 10);  // poll touch at 100 Hz (was 30 ms)

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = tick;
    tick_args.name     = "lv_tick";
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, kTickPeriodMs * 1000));

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 6 * 1024, nullptr, 2, nullptr, 1);

    set_backlight(100);
}

const Info &info() { return kInfo; }

bool lvgl_lock(int timeout_ms) {
    TickType_t ticks = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_mux, ticks) == pdTRUE;
}

void lvgl_unlock() { xSemaphoreGiveRecursive(lvgl_mux); }

void set_backlight(uint8_t percent) { analogWrite(G4848_LCD_BL, min<uint8_t>(percent, 100) * 255 / 100); }

void set_led(uint32_t) {}
void set_led_brightness(uint8_t) {}
void set_click_volume(uint8_t) {}

float battery_voltage() { return 0; }
bool  usb_power_present() { return false; }

// Not working yet: the slot is SPI-wired (CS 42, MOSI 47, CLK 48, MISO 41, shared with the panel's
// init SPI) but neither SDMMC 1-bit nor SPI mode (also at 400 kHz, MISO/MOSI swapped) got a card
// to mount; and the app expects SD_MMC. See ROADMAP.
bool sd_mount() {
    static SPIClass spi(FSPI);
    spi.begin(G4848_SD_CLK, G4848_SD_MISO, G4848_SD_MOSI, G4848_SD_CS);
    // The first attempt right after power-up sometimes fails; the card needs a moment
    for (int attempt = 0; attempt < 3; attempt++) {
        if (SD.begin(G4848_SD_CS, spi, 20000000, "/sdcard") && SD.cardType() != CARD_NONE) return true;
        SD.end();
        delay(100);
    }
    return false;
}

}  // namespace board

#endif
