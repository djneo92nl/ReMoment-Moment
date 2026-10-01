// MQTT connection and live state for all assigned devices.
#pragma once

namespace player {

void begin();

// Keeps MQTT connected and detects "not playing". Call every loop().
void loop();

// Call after the assigned device list changed: resubscribes and refreshes the device list screen.
void devices_changed();

// Call after model.current_device changed: re-renders Now Playing for the new device.
void device_changed();

}  // namespace player
