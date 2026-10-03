#include "battery.h"

#include <Arduino.h>

#include "board/board.h"

namespace battery {

static const float kNoBatteryVolts = 2.5f;   // below this nothing is connected
static const float kFullVolts      = 4.15f;  // with external power: charge is complete
static const float kRiseVolts      = 0.03f;  // rise over the history window that means "charging"
static const float kAlpha          = 0.3f;
static const int   kHistory        = 12;     // samples; one per call (5 s) = 1 minute

// Resting LiPo discharge curve (volts -> percent)
static const struct { float v; uint8_t pct; } kCurve[] = {
    {3.27f, 0},  {3.61f, 5},  {3.69f, 10}, {3.71f, 15}, {3.73f, 20}, {3.75f, 25}, {3.77f, 30},
    {3.79f, 35}, {3.80f, 40}, {3.82f, 45}, {3.84f, 50}, {3.85f, 55}, {3.87f, 60}, {3.91f, 65},
    {3.95f, 70}, {3.98f, 75}, {4.02f, 80}, {4.08f, 85}, {4.11f, 90}, {4.15f, 95}, {4.20f, 100},
};

static int percent_for(float v) {
    const int n = sizeof(kCurve) / sizeof(kCurve[0]);
    if (v <= kCurve[0].v) return 0;
    if (v >= kCurve[n - 1].v) return 100;
    for (int i = 1; i < n; i++) {
        if (v <= kCurve[i].v) {
            float t = (v - kCurve[i - 1].v) / (kCurve[i].v - kCurve[i - 1].v);
            return kCurve[i - 1].pct + (int)(t * (kCurve[i].pct - kCurve[i - 1].pct) + 0.5f);
        }
    }
    return 100;
}

static State  st;
static float  history[kHistory];
static int    history_count = 0;
static int    history_pos   = 0;
static bool   seeded        = false;

const State &update() {
    float raw = board::battery_voltage();
    if (raw < kNoBatteryVolts) {
        st = State();
        seeded = false;
        history_count = 0;
        return st;
    }

    st.volts = seeded ? st.volts + kAlpha * (raw - st.volts) : raw;
    seeded   = true;

    // Charging shows as a voltage climbing over the last minute (a charger-only cable is
    // invisible to the USB host check)
    bool rising = false;
    if (history_count == kHistory) rising = st.volts - history[history_pos] >= kRiseVolts;
    history[history_pos] = st.volts;
    history_pos = (history_pos + 1) % kHistory;
    if (history_count < kHistory) history_count++;

    st.usb     = board::usb_power_present() || rising || (st.usb && st.volts >= kFullVolts);
    st.full    = st.usb && st.volts >= kFullVolts;
    st.percent = percent_for(st.volts);

    // Hysteresis so the icon does not flicker around a threshold
    switch (st.level) {
        case Level::Ok:       if (st.percent <= 20) st.level = Level::Low; break;
        case Level::Low:      if (st.percent > 25) st.level = Level::Ok;
                              else if (st.percent <= 10) st.level = Level::Critical;
                              break;
        case Level::Critical: if (st.percent > 13) st.level = st.percent > 25 ? Level::Ok : Level::Low; break;
    }
    return st;
}

}  // namespace battery
