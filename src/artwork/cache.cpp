#include "cache.h"

#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>

#include "model.h"

namespace artwork::cache {

static const char *kBase    = "/remoment";
static const char *kSeqFile = "/remoment/.lru/seq";

// Evict when a folder holds too many files or the card is nearly full
static const uint64_t kMinFreeBytes  = 64ULL * 1024 * 1024;
static const uint32_t kEvictBatch    = 100;
static const uint16_t kHttpTimeoutMs = 10000;

struct Folder {
    const char *dir;      // the JPEGs
    const char *lru_dir;  // one small file per JPEG with its last-use sequence number
    uint32_t    max_files;
    uint32_t    count;
};

static Folder folders[] = {
    {"/remoment/covers", "/remoment/.lru/covers", 3000, 0},
    {"/remoment/backgrounds", "/remoment/.lru/backgrounds", 1000, 0},
};

static uint32_t seq = 0;

static Folder &folder(Kind kind) { return folders[kind == Kind::Cover ? 0 : 1]; }

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

static bool ensure_dir(const char *dir) { return SD_MMC.exists(dir) || SD_MMC.mkdir(dir); }

static uint32_t count_jpgs(const char *dir) {
    uint32_t n = 0;
    File d = SD_MMC.open(dir);
    if (!d) return 0;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        if (!f.isDirectory() && String(f.name()).endsWith(".jpg")) n++;
        f.close();
    }
    d.close();
    return n;
}

// Deletes the least recently used JPEGs of a folder (lowest last-use numbers)
static void evict(Folder &fo) {
    struct Candidate { uint32_t used; char hash[40]; };
    Candidate *oldest = (Candidate *)ps_calloc(kEvictBatch, sizeof(Candidate));
    if (!oldest) return;
    uint32_t found = 0;

    File d = SD_MMC.open(fo.dir);
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        String name = f.name();
        f.close();
        if (!name.endsWith(".jpg")) continue;
        String hash   = name.substring(0, name.length() - 4);
        uint32_t used = read_number(String(fo.lru_dir) + "/" + hash);

        // Keep the batch sorted ascending by `used`
        if (found == kEvictBatch && used >= oldest[found - 1].used) continue;
        uint32_t i = found < kEvictBatch ? found++ : found - 1;
        while (i > 0 && oldest[i - 1].used > used) {
            oldest[i] = oldest[i - 1];
            i--;
        }
        oldest[i].used = used;
        strlcpy(oldest[i].hash, hash.c_str(), sizeof(oldest[i].hash));
    }
    d.close();

    for (uint32_t i = 0; i < found; i++) {
        SD_MMC.remove(String(fo.dir) + "/" + oldest[i].hash + ".jpg");
        SD_MMC.remove(String(fo.lru_dir) + "/" + oldest[i].hash);
    }
    fo.count = fo.count > found ? fo.count - found : 0;
    Serial.printf("[artwork] evicted %u files from %s\n", found, fo.dir);
    free(oldest);
}

static bool needs_eviction(const Folder &fo) {
    if (fo.count >= fo.max_files) return true;
    return SD_MMC.totalBytes() - SD_MMC.usedBytes() < kMinFreeBytes;
}

// "http://host/storage/artwork/{hash}/320.jpg" -> hash. Other URLs get an FNV hash.
static String hash_of(const String &url) {
    int marker = url.indexOf("/artwork/");
    int slash  = url.lastIndexOf('/');
    if (marker >= 0 && marker + 9 < slash) {
        String hash = url.substring(marker + 9, slash);
        if (hash.indexOf('/') < 0) return hash;
    }
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < url.length(); i++) h = (h ^ (uint8_t)url[i]) * 16777619u;
    char buf[12];
    snprintf(buf, sizeof(buf), "u%08x", (unsigned)h);
    return buf;
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
    if (!ensure_dir(kBase) || !ensure_dir("/remoment/.lru")) return false;
    for (Folder &fo : folders) {
        if (!ensure_dir(fo.dir) || !ensure_dir(fo.lru_dir)) return false;
        fo.count = count_jpgs(fo.dir);
        Serial.printf("[artwork] %s: %u files\n", fo.dir, fo.count);
    }
    seq = read_number(kSeqFile);
    return true;
}

String fetch(Kind kind, const char *url) {
    if (!url || !*url) return "";

    Folder &fo  = folder(kind);
    String hash = hash_of(url);
    String path = String(fo.dir) + "/" + hash + ".jpg";

    if (!SD_MMC.exists(path)) {
        if (needs_eviction(fo)) evict(fo);
        if (!download(resolve(url), path)) return "";
        fo.count++;
    }

    write_number(String(fo.lru_dir) + "/" + hash, ++seq);
    write_number(kSeqFile, seq);
    return path;
}

}  // namespace artwork::cache
