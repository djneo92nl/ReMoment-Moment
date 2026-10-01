// MQTT connection and now-playing state for the current device.
#pragma once

namespace player {

void begin();

// Keeps MQTT connected, follows model.current_device, detects "not playing". Call every loop().
void loop();

// Call after model.current_device changed: resubscribes and resets the now-playing state.
void device_changed();

}  // namespace player
