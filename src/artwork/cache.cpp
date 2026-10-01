#include "cache.h"

#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>

#include "model.h"

namespace artwork::cache {

static const char *kRoot     = "/remoment/art";
static const char *kSeqFile  = "/remoment/art/.seq";
static const char *kUsedFile = "used";  // per entry: sequence number of the last access

// Evict when there are too many entries or the card is nearly full
static const uint32_t kMaxEntries     = 3000;
static const uint64_t kMinFreeBytes   = 64ULL * 1024 * 1024;
static const uint32_t kEvictBatch     = 100;
static const uint16_t kHttpTimeoutMs  = 10000;

static uint32_t entry_count = 0;
static uint32_t seq         = 0;

static uint32_t read_number(const String &path) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return 0;
    uint32_t n = f.readString().toInt();
    f.close();
    return n;
}

static void write_number(const String &path, uint32_t n) {
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) return;
    f.print(n);
    f.close();
}

static void remove_dir(const String &dir) {
    File d = SD_MMC.open(dir);
    if (!d) return;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        String path = dir + "/" + f.name();
        f.close();
        SD_MMC.remove(path);
    }
    d.close();
    SD_MMC.rmdir(dir);
}

// Deletes the least recently used entries (lowest "used" sequence numbers)
static void evict() {
    struct Candidate { uint32_t used; char name[40]; };
    Candidate *oldest = (Candidate *)ps_calloc(kEvictBatch, sizeof(Candidate));
    if (!oldest) return;
    uint32_t found = 0;

    File root = SD_MMC.open(kRoot);
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (!f.isDirectory()) {
            f.close();
            continue;
        }
        String name = f.name();
        f.close();
        uint32_t used = read_number(String(kRoot) + "/" + name + "/" + kUsedFile);

        // Keep the batch sorted ascending by `used`, newest candidate last
        if (found == kEvictBatch && used >= oldest[found - 1].used) continue;
        uint32_t i = found < kEvictBatch ? found++ : found - 1;
        while (i > 0 && oldest[i - 1].used > used) {
            oldest[i] = oldest[i - 1];
            i--;
        }
        oldest[i].used = used;
        strlcpy(oldest[i].name, name.c_str(), sizeof(oldest[i].name));
    }
    root.close();

    for (uint32_t i = 0; i < found; i++) remove_dir(String(kRoot) + "/" + oldest[i].name);
    entry_count = entry_count > found ? entry_count - found : 0;
    Serial.printf("[artwork] evicted %u entries\n", found);
    free(oldest);
}

static bool needs_eviction() {
    if (entry_count > kMaxEntries) return true;
    return SD_MMC.totalBytes() - SD_MMC.usedBytes() < kMinFreeBytes;
}

// "http://host/storage/artwork/{hash}/320.jpg" -> hash + file. Other URLs get an FNV hash.
static void split(const String &url, String &dir, String &file) {
    int slash = url.lastIndexOf('/');
    file = url.substring(slash + 1);
    int marker = url.indexOf("/artwork/");
    if (marker >= 0 && marker + 9 < slash) {
        dir = url.substring(marker + 9, slash);
        if (dir.indexOf('/') < 0) return;
    }
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < url.length(); i++) h = (h ^ (uint8_t)url[i]) * 16777619u;
    char buf[12];
    snprintf(buf, sizeof(buf), "u%08x", (unsigned)h);
    dir = buf;
    if (file.isEmpty()) file = "image.jpg";
}

// Server URLs are built from its APP_URL, which may not be reachable from here:
// re-anchor /storage/... paths on the artwork base URL from /api/info.
static String resolve(const String &url) {
    int storage = url.indexOf("/storage/");
    if (storage >= 0 && model.artwork_base_url[0]) return String(model.artwork_base_url) + url.substring(storage);
    if (url.startsWith("/")) return String(model.server_url) + url;
    return url;
}

static bool download(const String &url, const String &path) {
    if (WiFi.status() != WL_CONNECTED) return false;
    String tmp = path + ".tmp";

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    if (!http.begin(client, url)) return false;

    bool ok = false;
    int status = http.GET();
    if (status == HTTP_CODE_OK) {
        File f = SD_MMC.open(tmp, FILE_WRITE);
        if (f) {
            ok = http.writeToStream(&f) > 0;
            f.close();
        }
    } else {
        Serial.printf("[artwork] GET %s: %d\n", url.c_str(), status);
    }
    http.end();

    if (ok) ok = SD_MMC.rename(tmp, path);
    if (!ok) SD_MMC.remove(tmp);
    return ok;
}

bool begin() {
    if (!SD_MMC.exists("/remoment")) SD_MMC.mkdir("/remoment");
    if (!SD_MMC.exists(kRoot) && !SD_MMC.mkdir(kRoot)) return false;

    File root = SD_MMC.open(kRoot);
    if (!root) return false;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (f.isDirectory()) entry_count++;
        f.close();
    }
    root.close();
    seq = read_number(kSeqFile);
    Serial.printf("[artwork] cache: %u entries\n", entry_count);
    return true;
}

String fetch(const char *url) {
    if (!url || !*url) return "";

    String dir, file;
    split(url, dir, file);
    String entry = String(kRoot) + "/" + dir;
    String path  = entry + "/" + file;

    if (!SD_MMC.exists(path)) {
        if (!SD_MMC.exists(entry)) {
            if (needs_eviction()) evict();
            if (!SD_MMC.mkdir(entry)) return "";
            entry_count++;
        }
        if (!download(resolve(url), path)) return "";
    }

    write_number(entry + "/" + kUsedFile, ++seq);
    write_number(kSeqFile, seq);
    return path;
}

}  // namespace artwork::cache
