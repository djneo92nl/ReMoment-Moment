// The ReMoment library client (discovery, registration, MQTT, live device state) and the
// bridge from its events to the screens, artwork and status LED.
#pragma once

#include <ReMoment.h>

namespace player {

// The library client. Loop task only (its state is not thread-safe).
remoment::Client &client();

void begin();

// Drives discovery, registration, MQTT and heartbeats. Call every loop().
void loop();

// Call after model.current device changed outside the library: re-renders Now Playing.
void device_changed();

// True once the first device list arrived (the Now Playing screen may be shown)
bool has_devices();

// Re-run discovery / forget the registration and pair again
void restart();
void forget_registration();

}  // namespace player
