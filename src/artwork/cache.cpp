#include "cache.h"

#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>

#include "model.h"

namespace artwork::cache {

// Small SD writes are expensive on these cards (~0.8 s each, measured on the ES3C28P),
// so the hot path doesn't write: last-use numbers live in PSRAM and are saved as one
// index file per folder when idle, and downloads land in RAM and hit the card once.

static const char *kBase = "/remoment";

static const uint64_t kMinFreeBytes   = 64ULL * 1024 * 1024;
static const uint32_t kEvictBatch     = 100;
static const uint16_t kHttpTimeoutMs  = 10000;
static const size_t   kMaxDownload    = 1024 * 1024;
static const unsigned long kSaveDelayMs = 60000;  // save the index at most once a minute

struct Entry {
    char     hash[34];
    uint32_t used;  // last-use sequence number, 0 = never shown (e.g. pre-cached)
};

struct Folder {
    const char *dir;    // the JPEGs
    const char *index;  // "hash used" per line
    uint32_t    max_files;
    Entry      *entries;  // PSRAM, sorted by hash
    uint32_t    count;
    bool        dirty;
};

static Folder folders[] = {
    {"/remoment/covers", "/remoment/.lru/covers.idx", 3000, nullptr, 0, false},
    {"/remoment/backgrounds", "/remoment/.lru/backgrounds.idx", 1000, nullptr, 0, false},
    {"/remoment/thumbs", "/remoment/.lru/thumbs.idx", 5000, nullptr, 0, false},
};

static uint32_t seq = 0;
static int64_t  free_bytes = 0;  // estimated; f_getfree is slow, so it's only read at boot
static unsigned long last_change_ms = 0;

static Folder &folder(Kind kind) { return folders[(int)kind]; }

// ---- In-memory index (sorted by hash, binary search) -------------------------------

static int find(const Folder &fo, const char *hash, bool *found) {
    int lo = 0, hi = (int)fo.count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int c   = strcmp(fo.entries[mid].hash, hash);
        if (c == 0) {
            *found = true;
            return mid;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    *found = false;
    return lo;
}

static Entry *lookup(Folder &fo, const char *hash) {
    bool found;
    int i = find(fo, hash, &found);
    return found ? &fo.entries[i] : nullptr;
}

static Entry *insert(Folder &fo, const char *hash) {
    bool found;
    int i = find(fo, hash, &found);
    if (found) return &fo.entries[i];
    if (fo.count >= fo.max_files + kEvictBatch) return nullptr;
    memmove(&fo.entries[i + 1], &fo.entries[i], (fo.count - i) * sizeof(Entry));
    strlcpy(fo.entries[i].hash, hash, sizeof(fo.entries[i].hash));
    fo.entries[i].used = 0;
    fo.count++;
    return &fo.entries[i];
}

static void remove_entry(Folder &fo, const char *hash) {
    bool found;
    int i = find(fo, hash, &found);
    if (!found) return;
    memmove(&fo.entries[i], &fo.entries[i + 1], (fo.count - i - 1) * sizeof(Entry));
    fo.count--;
}

static int compare_hash(const void *a, const void *b) {
    return strcmp(((const Entry *)a)->hash, ((const Entry *)b)->hash);
}

// ---- Loading and saving --------------------------------------------------------------

static bool ensure_dir(const char *dir) { return SD_MMC.exists(dir) || SD_MMC.mkdir(dir); }

// Every JPEG on the card gets an entry; the index file then supplies last-use numbers
static void load(Folder &fo) {
    fo.entries = (Entry *)ps_calloc(fo.max_files + kEvictBatch, sizeof(Entry));
    fo.count   = 0;
    if (!fo.entries) return;

    File d = SD_MMC.open(fo.dir);
    for (File f = d.openNextFile(); f && fo.count < fo.max_files + kEvictBatch; f = d.openNextFile()) {
        String name = f.name();
        f.close();
        if (!name.endsWith(".jpg") || name.length() - 4 >= sizeof(Entry::hash)) continue;
        strlcpy(fo.entries[fo.count++].hash, name.substring(0, name.length() - 4).c_str(), sizeof(Entry::hash));
    }
    d.close();
    qsort(fo.entries, fo.count, sizeof(Entry), compare_hash);

    File idx = SD_MMC.open(fo.index, FILE_READ);
    if (!idx) return;
    while (idx.available()) {
        String line = idx.readStringUntil('\n');
        int space   = line.indexOf(' ');
        if (space <= 0) continue;
        Entry *e = lookup(fo, line.substring(0, space).c_str());
        if (!e) continue;  // file is gone
        e->used = line.substring(space + 1).toInt();
        if (e->used > seq) seq = e->used;
    }
    idx.close();
}

static void save(Folder &fo) {
    size_t cap = fo.count * (sizeof(Entry::hash) + 12) + 1;
    char *buf  = (char *)ps_malloc(cap);
    if (!buf) return;
    size_t len = 0;
    for (uint32_t i = 0; i < fo.count; i++) {
        if (fo.entries[i].used == 0) continue;
        len += snprintf(buf + len, cap - len, "%s %u\n", fo.entries[i].hash, (unsigned)fo.entries[i].used);
    }
    File f = SD_MMC.open(fo.index, FILE_WRITE);
    if (f) {
        f.write((const uint8_t *)buf, len);
        f.close();
        fo.dirty = false;
    }
    free(buf);
}

// ---- Eviction ------------------------------------------------------------------------

static int compare_used(const void *a, const void *b) {
    uint32_t x = ((const Entry *)a)->used, y = ((const Entry *)b)->used;
    return x < y ? -1 : x > y;
}

// Deletes the least recently used JPEGs of a folder
static void evict(Folder &fo) {
    Entry *by_use = (Entry *)ps_malloc(fo.count * sizeof(Entry));
    if (!by_use) return;
    memcpy(by_use, fo.entries, fo.count * sizeof(Entry));
    qsort(by_use, fo.count, sizeof(Entry), compare_used);

    uint32_t n = min(kEvictBatch, fo.count);
    for (uint32_t i = 0; i < n; i++) {
        String path = String(fo.dir) + "/" + by_use[i].hash + ".jpg";
        File f = SD_MMC.open(path, FILE_READ);
        if (f) {
            free_bytes += f.size();
            f.close();
        }
        SD_MMC.remove(path);
        remove_entry(fo, by_use[i].hash);
    }
    free(by_use);
    fo.dirty = true;
    Serial.printf("[artwork] evicted %u files from %s\n", n, fo.dir);
}

static bool needs_eviction(const Folder &fo) { return fo.count >= fo.max_files || free_bytes < (int64_t)kMinFreeBytes; }

// ---- Download ------------------------------------------------------------------------

// "http://host/storage/artwork/{hash}/320.jpg" -> hash. Other URLs get an FNV hash.
static String hash_of(const String &url) {
    int marker = url.indexOf("/artwork/");
    int slash  = url.lastIndexOf('/');
    if (marker >= 0 && marker + 9 < slash) {
        String hash = url.substring(marker + 9, slash);
        if (hash.indexOf('/') < 0 && hash.length() < sizeof(Entry::hash)) return hash;
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

// Downloads into PSRAM, then writes the file in one go
static bool download(const String &url, const String &path) {
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    if (!http.begin(client, url)) {
        Serial.printf("[artwork] bad URL %s\n", url.c_str());
        return false;
    }
    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        Serial.printf("[artwork] GET %s: %d\n", url.c_str(), status);
        http.end();
        return false;
    }

    int expected = http.getSize();  // -1 when chunked
    size_t cap   = expected > 0 ? (size_t)expected : 128 * 1024;
    uint8_t *buf = cap <= kMaxDownload ? (uint8_t *)ps_malloc(cap) : nullptr;
    size_t len   = 0;
    WiFiClient *stream = http.getStreamPtr();
    unsigned long deadline = millis() + kHttpTimeoutMs;
    while (buf && http.connected() && (expected < 0 || len < (size_t)expected) && millis() < deadline) {
        size_t avail = stream->available();
        if (!avail) {
            delay(1);
            continue;
        }
        if (len + avail > cap) {
            size_t grown = min(cap * 2, kMaxDownload);
            uint8_t *bigger = grown > cap ? (uint8_t *)ps_realloc(buf, grown) : nullptr;
            if (!bigger) break;
            buf = bigger;
            cap = grown;
        }
        len += stream->readBytes(buf + len, min(avail, cap - len));
    }
    http.end();

    bool ok = buf && len > 0 && (expected < 0 || len == (size_t)expected);
    if (ok) {
        File f = SD_MMC.open(path, FILE_WRITE);
        ok = f && f.write(buf, len) == len;
        if (f) f.close();
        if (!ok) SD_MMC.remove(path);
        else free_bytes -= len;
    }
    if (!ok) Serial.printf("[artwork] download %s failed (%u of %d bytes)\n", url.c_str(), (unsigned)len, expected);
    free(buf);
    return ok;
}

// ---------------------------------------------------------------------------------

bool begin() {
    if (!ensure_dir(kBase) || !ensure_dir("/remoment/.lru")) return false;
    free_bytes = (int64_t)(SD_MMC.totalBytes() - SD_MMC.usedBytes());
    for (Folder &fo : folders) {
        if (!ensure_dir(fo.dir)) return false;
        load(fo);
        Serial.printf("[artwork] %s: %u files\n", fo.dir, fo.count);
    }
    return true;
}

String fetch(Kind kind, const char *url, bool touch) {
    if (!url || !*url) return "";

    Folder &fo  = folder(kind);
    String hash = hash_of(url);
    String path = String(fo.dir) + "/" + hash + ".jpg";

    Entry *e = lookup(fo, hash.c_str());
    if (!e) {
        if (needs_eviction(fo)) evict(fo);
        if (!download(resolve(url), path)) return "";
        e = insert(fo, hash.c_str());
        if (!e) return path;
    }
    if (touch) {
        e->used        = ++seq;
        fo.dirty       = true;
        last_change_ms = millis();
    }
    return path;
}

void forget(Kind kind, const String &path) {
    Folder &fo = folder(kind);
    int slash  = path.lastIndexOf('/');
    String hash = path.substring(slash + 1, path.length() - 4);
    SD_MMC.remove(path);
    remove_entry(fo, hash.c_str());
    fo.dirty = true;
}

void save_if_idle() {
    if (millis() - last_change_ms < kSaveDelayMs) return;
    for (Folder &fo : folders)
        if (fo.dirty) save(fo);
}

bool nearly_full(Kind kind) {
    const Folder &fo = folder(kind);
    return fo.count >= fo.max_files * 9 / 10 || free_bytes < 2 * (int64_t)kMinFreeBytes;
}

}  // namespace artwork::cache
