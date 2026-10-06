// SD card cache for server artwork, stored as the server's JPEG files:
//   /remoment/covers/{hash}.jpg       (artwork.proxy_320)
//   /remoment/backgrounds/{w}x{h}/{hash}.jpg  (artwork.proxy_bg_{w}x{h}, one folder per screen size)
//   /remoment/thumbs/{hash}.jpg       (proxy_120, library grids and lists)
// {hash} is the md5 directory the server uses (/storage/artwork/{hash}/320.jpg).
// Last-use counters for LRU eviction are kept in memory and saved to /remoment/.lru/{kind}.idx.
// Used only from the artwork task.
#pragma once

#include <Arduino.h>

namespace artwork::cache {

enum class Kind : uint8_t { Cover, Background, Thumb };

// Creates the cache folders and counts entries. Returns false without an SD card.
bool begin();

// SD path (for RM_SD.open) of the cached file for `url`, downloading it when missing.
// Returns "" when the download failed. `touch` marks it as just used (LRU); pre-cached
// files aren't touched, so they're evicted before anything that was actually shown.
String fetch(Kind kind, const char *url, bool touch = true);

// Drops a cached file that turned out to be unusable (e.g. a truncated download)
void forget(Kind kind, const String &path);

// Saves the last-use index when it changed and nothing happened for a while.
// Call from the artwork task when it's idle.
void save_if_idle();

// True when the folder is close to its limit (pre-caching stops there instead of evicting)
bool nearly_full(Kind kind);

}  // namespace artwork::cache
