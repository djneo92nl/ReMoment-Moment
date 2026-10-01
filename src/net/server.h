// Finds the ReMoment server (settings override, else mDNS) and its MQTT broker.
#pragma once

namespace server {

// Fills model.server_url / mqtt_host / mqtt_port. Returns true when the server answered.
bool discover();

}  // namespace server
