// Album art and backgrounds: download (SD cache), decode JPEG into PSRAM, hand to the UI.
// Runs in its own task so downloads and decoding never block MQTT or the LVGL task.
#pragma once

#include <stdint.h>

namespace artwork {

struct Request {
    char     cover_url[192];   // artwork.proxy_320, "" = placeholder
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

// Library thumbnails: load `url` (proxy_120, or proxy_320 when `large`) and hand the decoded
// image to ui::library_thumb(token, image). Upper 16 bits of `token` = UI generation.
// Safe from any task. Returns false when the request couldn't be queued.
bool thumb_request(uint32_t token, const char *url, bool large = false);

// The UI dropped every thumbnail older than `generation`: their buffers can be freed.
void thumbs_release(uint32_t generation);

}  // namespace artwork
