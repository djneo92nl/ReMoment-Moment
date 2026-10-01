// SD card cache for server artwork. Layout: /remoment/art/{hash}/{file}.jpg, where {hash}
// is the md5 directory the server already uses (/storage/artwork/{hash}/320.jpg).
// Used only from the artwork task.
#pragma once

#include <Arduino.h>

namespace artwork::cache {

// Creates the cache directory and counts entries. Returns false without an SD card.
bool begin();

// SD path (for SD_MMC.open) of the cached file for `url`, downloading it when missing.
// Returns "" when the download failed.
String fetch(const char *url);

}  // namespace artwork::cache
