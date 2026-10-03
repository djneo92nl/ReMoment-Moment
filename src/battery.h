// Battery state from the board's voltage reading: percentage, charging/full, low levels.
#pragma once

#include <stdint.h>

namespace battery {

enum class Level : uint8_t { Ok, Low, Critical };  // Low <= 20 %, Critical <= 10 %

struct State {
    int   percent = -1;  // -1 = no battery detected
    float volts   = 0;   // filtered
    bool  usb     = false;  // external power: USB host seen, or voltage rising
    bool  full    = false;
    Level level   = Level::Ok;
};

// Samples the board (call from the loop task, at most every few seconds) and returns the new state.
const State &update();

}  // namespace battery
