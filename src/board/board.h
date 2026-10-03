// Hardware abstraction: everything board-specific lives behind this interface,
// so supporting another screen means adding one board_*.cpp, not #ifdefs in the app.
#pragma once

#include <stdint.h>

namespace board {

struct Info {
    const char *name;
    uint16_t    width;
    uint16_t    height;
    bool        has_led;      // RGB status LED
    bool        has_speaker;  // tap sounds
    bool        has_battery;  // battery voltage can be measured
};

// Brings up I2C, touch, display, LVGL (in its own task) and the backlight.
void init();

const Info &info();

// LVGL is not thread-safe: hold this lock for every LVGL call made outside the LVGL task.
bool lvgl_lock(int timeout_ms = -1);
void lvgl_unlock();

// 0 = off, 100 = full brightness
void set_backlight(uint8_t percent);

// Status LED colour as 0xRRGGBB, 0 = off, scaled by set_led_brightness(). No-op without an LED.
void set_led(uint32_t rgb);
void set_led_brightness(uint8_t percent);

// Short UI feedback sound (taps). `volume` 0-100, 0 = off. No-op without a speaker.
void set_click_volume(uint8_t volume);

// Battery voltage in volts, or 0 when the board can't measure it.
float battery_voltage();

// True while USB power is present (host or charger that enumerates). False when the board can't tell.
bool usb_power_present();

// Mounts the SD card at /sdcard. Returns false when no card is present.
bool sd_mount();

}  // namespace board

// RAII guard for board::lvgl_lock()
class LvglLock {
  public:
    LvglLock() : locked_(board::lvgl_lock()) {}
    ~LvglLock() {
        if (locked_) board::lvgl_unlock();
    }
    explicit operator bool() const { return locked_; }
    LvglLock(const LvglLock &) = delete;
    LvglLock &operator=(const LvglLock &) = delete;

  private:
    bool locked_;
};
