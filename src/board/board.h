// Hardware abstraction: everything board-specific lives behind this interface,
// so supporting another screen means adding one board_*.cpp, not #ifdefs in the app.
#pragma once

#include <stdint.h>

namespace board {

struct Info {
    const char *name;
    uint16_t    width;
    uint16_t    height;
};

// Brings up I2C, touch, display, LVGL (in its own task) and the backlight.
void init();

const Info &info();

// LVGL is not thread-safe: hold this lock for every LVGL call made outside the LVGL task.
bool lvgl_lock(int timeout_ms = -1);
void lvgl_unlock();

// 0 = off, 100 = full brightness
void set_backlight(uint8_t percent);

// Status LED colour as 0xRRGGBB, 0 = off. No-op on boards without one.
void set_led(uint32_t rgb);

// Battery voltage in volts, or 0 when the board can't measure it.
float battery_voltage();

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
