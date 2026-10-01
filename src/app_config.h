// Build-time constants. Runtime settings (WiFi, server URL) live in garnet_web / NVS.
#pragma once

#define FIRMWARE_VERSION "0.2.0"
#define BUILD_NUMBER     2

#define DEVICE_NAME      "ReMoment"   // garnet_web title, setup AP SSID prefix, hostname prefix
#define WEB_DEFAULT_PASSWORD "remoment" // garnet_web login until changed in the System group

// mDNS service advertised by the ReMoment server
#define REMOMENT_MDNS_SERVICE "remoment"
#define REMOMENT_MDNS_PROTO   "tcp"
