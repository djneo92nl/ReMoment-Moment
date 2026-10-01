#include "artwork.h"

#include <Arduino.h>
#include <JPEGDEC.h>
#include <SD_MMC.h>
#include <lvgl.h>

#include "board/board.h"
#include "cache.h"
#include "model.h"
#include "net/api.h"
#include "net/registration.h"
#include "ui/ui.h"

namespace artwork {

static const size_t kMaxFileBytes = 512 * 1024;

// Two decode targets per kind: one on screen, one being filled (and cross-faded in)
struct Slot {
    lv_img_dsc_t dsc;
    uint8_t     *pixels;
    size_t       capacity;
};

struct Kind {
    Slot  slots[2];
    int   shown = -1;    // slot index on screen, -1 = placeholder
    char  url[192] = "";  // URL of the shown slot
};

static Kind covers, backgrounds;
static QueueHandle_t queue;        // Request, length 1 (latest wins)
static QueueHandle_t thumb_queue;  // ThumbRequest
static TaskHandle_t  task_handle;
static JPEGDEC *jpeg;

static int on_draw(JPEGDRAW *draw) {
    lv_img_dsc_t *dsc = (lv_img_dsc_t *)draw->pUser;
    int w = dsc->header.w, h = dsc->header.h;
    int cols = LV_MIN(draw->iWidthUsed, w - draw->x);
    uint16_t *dst = (uint16_t *)dsc->data;
    for (int row = 0; row < draw->iHeight && draw->y + row < h; row++) {
        memcpy(dst + (draw->y + row) * w + draw->x, draw->pPixels + row * draw->iWidth, cols * 2);
    }
    return 1;
}

static uint8_t *read_file(const String &path, size_t &size) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return nullptr;
    size = f.size();
    uint8_t *data = size > 0 && size <= kMaxFileBytes ? (uint8_t *)ps_malloc(size) : nullptr;
    if (data && f.read(data, size) != size) {
        free(data);
        data = nullptr;
    }
    f.close();
    return data;
}

// `half`: decode at half resolution (JPEGDEC scales while decoding), e.g. a 320 cover into a
// 160 px tile, so the UI never has to scale images at draw time
static bool decode(const String &path, Slot &slot, bool half = false) {
    size_t size;
    uint8_t *data = read_file(path, size);
    if (!data) return false;

    bool ok = false;
    if (jpeg->openRAM(data, size, on_draw)) {
        int w = jpeg->getWidth(), h = jpeg->getHeight();
        if (half) {
            w /= 2;
            h /= 2;
        }
        size_t needed = (size_t)w * h * 2;
        if (slot.capacity < needed) {
            free(slot.pixels);
            slot.pixels   = (uint8_t *)ps_malloc(needed);
            slot.capacity = slot.pixels ? needed : 0;
        }
        if (slot.pixels) {
            slot.dsc.header.always_zero = 0;
            slot.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            slot.dsc.header.w  = w;
            slot.dsc.header.h  = h;
            slot.dsc.data_size = needed;
            slot.dsc.data      = slot.pixels;
            jpeg->setPixelType(LV_COLOR_16_SWAP ? RGB565_BIG_ENDIAN : RGB565_LITTLE_ENDIAN);
            jpeg->setUserPointer(&slot.dsc);
            ok = jpeg->decode(0, 0, half ? JPEG_SCALE_HALF : 0) == 1;
        }
        jpeg->close();
    }
    if (!ok) Serial.printf("[artwork] decode %s failed (%d)\n", path.c_str(), jpeg->getLastError());
    free(data);
    return ok;
}

// Loads `url` into the free slot of `kind`. Returns the image to show (nullptr = placeholder)
// and sets `changed` when the screen needs updating.
static const lv_img_dsc_t *load(Kind &kind, cache::Kind cache_kind, const char *url, bool &changed) {
    changed = false;
    if (strcmp(url, kind.url) == 0) return kind.shown >= 0 ? &kind.slots[kind.shown].dsc : nullptr;

    changed = true;
    strlcpy(kind.url, url, sizeof(kind.url));
    int next = kind.shown == 0 ? 1 : 0;
    Slot &slot = kind.slots[next];

    // The free slot may still be fading out on screen: take it off first
    ui::artwork_release(&slot.dsc);

    const char *what = cache_kind == cache::Kind::Cover ? "cover" : "background";
    if (!*url) {
        kind.shown = -1;
        return nullptr;
    }
    unsigned long start = millis();
    String path = cache::fetch(cache_kind, url);
    unsigned long fetched = millis();
    bool ok = !path.isEmpty() && decode(path, slot);

    // Backgrounds are cached as {hash}.jpg whatever their size: a file cached before the
    // server rendered this screen's size is the 1024x600 one. Replace it once.
    char sized[24];
    snprintf(sized, sizeof(sized), "bg_%ux%u.", board::info().width, board::info().height);
    if (ok && cache_kind == cache::Kind::Background && strstr(url, sized) &&
        (slot.dsc.header.w != board::info().width || slot.dsc.header.h != board::info().height)) {
        cache::forget(cache_kind, path);
        path = cache::fetch(cache_kind, url);
        ok   = !path.isEmpty() && decode(path, slot);
    }

    if (!ok) {
        if (!path.isEmpty()) cache::forget(cache_kind, path);  // corrupt file: download again next time
        Serial.printf("[artwork] %s failed: %s\n", what, path.isEmpty() ? url : path.c_str());
        kind.shown  = -1;
        kind.url[0] = '\0';  // retry on the next request
        return nullptr;
    }
    Serial.printf("[artwork] %s %dx%d: fetch %lu ms, decode %lu ms (%s)\n", what, slot.dsc.header.w,
                  slot.dsc.header.h, fetched - start, millis() - fetched, path.c_str());
    kind.shown = next;
    return &slot.dsc;
}

const char *background_url(JsonObjectConst art) {
    char key[24];
    snprintf(key, sizeof(key), "proxy_bg_%ux%u", board::info().width, board::info().height);
    const char *sized = art[key] | "";
    return *sized ? sized : (art["proxy_bg"] | "");
}

// ---- Thumbnails for the library screens ------------------------------------------
// Each decoded thumbnail gets its own PSRAM buffer, tagged with the UI's token. The UI
// bumps the generation in the token when it rebuilds a screen; buffers of older
// generations are freed here once the UI has dropped its references (thumbs_release).

static const int kMaxThumbs     = 96;
static const int kThumbQueueLen = 48;

struct ThumbRequest {
    uint32_t token;
    bool     large;  // proxy_320 (cover cache) instead of proxy_120
    bool     half;   // decode at half resolution
    char     url[192];
};

struct Thumb {
    uint32_t     token;
    lv_img_dsc_t dsc;
    uint8_t     *pixels;
};

static Thumb thumbs[kMaxThumbs];
static int   thumb_count = 0;
static volatile uint32_t thumb_generation = 0;

static uint32_t generation_of(uint32_t token) { return token >> 16; }

static void free_stale_thumbs() {
    uint32_t gen = thumb_generation;
    int kept = 0;
    for (int i = 0; i < thumb_count; i++) {
        if (generation_of(thumbs[i].token) == gen) {
            thumbs[kept++] = thumbs[i];
        } else {
            free(thumbs[i].pixels);
        }
    }
    thumb_count = kept;
}

// Returns false when there was nothing to do
static bool process_thumb() {
    ThumbRequest r;
    if (!thumb_queue || xQueueReceive(thumb_queue, &r, 0) != pdTRUE) return false;
    if (generation_of(r.token) != thumb_generation || thumb_count >= kMaxThumbs) return true;

    String path = cache::fetch(r.large ? cache::Kind::Cover : cache::Kind::Thumb, r.url);
    if (path.isEmpty()) return true;
    Slot slot = {};
    if (!decode(path, slot, r.half)) {
        free(slot.pixels);
        return true;
    }
    Thumb &t = thumbs[thumb_count];
    t = {r.token, slot.dsc, slot.pixels};
    if (ui::library_thumb(t.token, &t.dsc)) thumb_count++;
    else free(t.pixels);  // the screen moved on meanwhile
    return true;
}

// ---- Pre-caching: while idle, fill the SD card from the server's artwork list ----
// (GET /api/clients/{token}/artwork: source logos, then albums most recently played first)

static const unsigned long kIdleBeforePrecacheMs = 30000;
static const unsigned long kPrecacheIntervalMs   = 6UL * 60 * 60 * 1000;

static long          precache_cursor  = 0;   // -1 = sweep finished
static unsigned long precache_done_ms = 0;
static unsigned long last_request_ms  = 0;

static bool busy() {
    return uxQueueMessagesWaiting(queue) > 0 || (thumb_queue && uxQueueMessagesWaiting(thumb_queue) > 0);
}

// One page of the list. Returns false when there's nothing more to do for now.
static bool precache_page() {
    if (model.phase != Phase::Running || busy()) return false;
    if (millis() - last_request_ms < kIdleBeforePrecacheMs) return false;
    if (precache_cursor < 0) {
        if (millis() - precache_done_ms < kPrecacheIntervalMs) return false;
        precache_cursor = 0;  // start a new sweep
    }
    if (cache::nearly_full(cache::Kind::Cover)) return false;

    String token = registration::api_token();
    if (token.isEmpty()) return false;

    String path = "/clients/" + token + "/artwork";
    if (precache_cursor > 0) path += "?cursor=" + String(precache_cursor);
    JsonDocument page;
    if (!api::ok(api::get(path.c_str(), page))) return false;

    for (JsonObjectConst item : page["data"].as<JsonArrayConst>()) {
        if (busy()) return false;  // a track change or a library screen wins; resume this page later
        if (!cache::nearly_full(cache::Kind::Cover)) cache::fetch(cache::Kind::Cover, item["proxy_320"] | "", false);
        if (!cache::nearly_full(cache::Kind::Background)) cache::fetch(cache::Kind::Background, background_url(item), false);
        if (!cache::nearly_full(cache::Kind::Thumb)) cache::fetch(cache::Kind::Thumb, item["proxy_120"] | "", false);
    }

    if (page["next_cursor"].isNull()) {
        precache_cursor  = -1;
        precache_done_ms = millis();
        Serial.println("[artwork] pre-cache sweep done");
        return false;
    }
    precache_cursor = page["next_cursor"] | -1L;
    return true;
}

// ---------------------------------------------------------------------------------

static void show(Request &req, bool cache_ok) {
    if (!cache_ok) req.cover_url[0] = req.background_url[0] = '\0';

    // No cover (or still being processed on the server): show the server's placeholder logo
    if (cache_ok && !req.cover_url[0]) strlcpy(req.cover_url, model.placeholder_url, sizeof(req.cover_url));

    bool cover_changed, background_changed;
    const lv_img_dsc_t *cover      = load(covers, cache::Kind::Cover, req.cover_url, cover_changed);
    const lv_img_dsc_t *background = load(backgrounds, cache::Kind::Background, req.background_url, background_changed);
    ui::artwork_show(cover, cover_changed, background, background_changed, req.has_accent, req.accent);
}

static void task(void *) {
    jpeg = new JPEGDEC();
    bool cache_ok = cache::begin();
    if (!cache_ok) Serial.println("[artwork] SD cache unavailable");

    // Priority: now-playing artwork, then library thumbnails, then idle pre-caching
    Request req;
    for (;;) {
        free_stale_thumbs();
        if (xQueueReceive(queue, &req, 0) == pdTRUE) {
            last_request_ms = millis();
            show(req, cache_ok);
            continue;
        }
        if (cache_ok && process_thumb()) continue;
        if (cache_ok && precache_page()) continue;
        if (cache_ok) cache::save_if_idle();
        // Sleep until request()/thumb_request() wakes us, or check the idle timer again
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleBeforePrecacheMs));
    }
}

void begin(bool sd_available) {
    if (!sd_available) {
        ui::now_playing_set_notice("Insert an SD card to show album art");
        return;
    }
    queue       = xQueueCreate(1, sizeof(Request));
    thumb_queue = xQueueCreate(kThumbQueueLen, sizeof(ThumbRequest));
    // Core 0 next to WiFi; LVGL renders on core 1
    xTaskCreatePinnedToCore(task, "artwork", 8192, nullptr, 1, &task_handle, 0);
}

void request(const Request &req) {
    if (!queue) return;
    xQueueOverwrite(queue, &req);
    xTaskNotifyGive(task_handle);
}

void clear() {
    Request req = {};
    request(req);
}

bool thumb_request(uint32_t token, const char *url, bool large, bool half) {
    if (!thumb_queue || !url || !*url) return false;
    ThumbRequest r = {token, large, half, ""};
    strlcpy(r.url, url, sizeof(r.url));
    bool queued = xQueueSend(thumb_queue, &r, 0) == pdTRUE;
    xTaskNotifyGive(task_handle);
    return queued;
}

void thumbs_release(uint32_t generation) {
    thumb_generation = generation;
    if (task_handle) xTaskNotifyGive(task_handle);
}

}  // namespace artwork
