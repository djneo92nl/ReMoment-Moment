// Album art and backgrounds: download (SD cache), decode JPEG into PSRAM, hand to the UI.
// Runs in its own task so downloads and decoding never block MQTT or the LVGL task.
#pragma once

#include <stdint.h>

namespace artwork {

struct Request {
    char     cover_url[192];   // artwork.proxy_320, "" = none
    char     background_url[192];  // artwork.proxy_bg, "" = none
    bool     has_accent;
    uint32_t accent;           // 0xRRGGBB from artwork.safe_colors[0]
};

// Starts the task. Without an SD card artwork stays off and the UI shows a notice.
void begin(bool sd_available);

// Show this artwork; a newer request replaces one that hasn't started yet.
void request(const Request &req);

// Back to the placeholder (device changed, track without artwork).
void clear();

}  // namespace artwork
