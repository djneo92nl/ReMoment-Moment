// SD card cache for server artwork, stored as the server's JPEG files:
//   /remoment/covers/{hash}.jpg       (artwork.proxy_320)
//   /remoment/backgrounds/{hash}.jpg  (artwork.proxy_bg, this screen's size)
// {hash} is the md5 directory the server uses (/storage/artwork/{hash}/320.jpg).
// Last-use counters for LRU eviction live in /remoment/.lru/{kind}/{hash}.
// Used only from the artwork task.
#pragma once

#include <Arduino.h>

namespace artwork::cache {

enum class Kind : uint8_t { Cover, Background };

// Creates the cache folders and counts entries. Returns false without an SD card.
bool begin();

// SD path (for SD_MMC.open) of the cached file for `url`, downloading it when missing.
// Returns "" when the download failed.
String fetch(Kind kind, const char *url);

}  // namespace artwork::cache
