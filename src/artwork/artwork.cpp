#include "artwork.h"

#include <Arduino.h>
#include <JPEGDEC.h>
#include "sd_card.h"
#include <lvgl.h>

#include "board/board.h"
#include "cache.h"
#include "model.h"
#include "net/api.h"
#include "net/player.h"
#include "ui/layout.h"
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
static QueueHandle_t prefetch_queue;  // PrefetchRequest, length 1 (latest wins)
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
    File f = RM_SD.open(path, FILE_READ);
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

// Box-filter a decoded square cover down to the size it is shown at, so LVGL never has to zoom
// it (its transform draws the cover offset, with the edge pixels smeared over the gap).
static void shrink(Slot &slot, int target) {
    int w = slot.dsc.header.w, h = slot.dsc.header.h;
    if (target <= 0 || target >= w || w != h) return;
    size_t needed = (size_t)target * target * 2;
    uint16_t *out = (uint16_t *)ps_malloc(needed);
    if (!out) return;
    const uint16_t *in = (const uint16_t *)slot.pixels;
    for (int y = 0; y < target; y++) {
        int y0 = y * h / target, y1 = LV_MAX((y + 1) * h / target, y0 + 1);
        for (int x = 0; x < target; x++) {
            int x0 = x * w / target, x1 = LV_MAX((x + 1) * w / target, x0 + 1);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; sy++) {
                for (int sx = x0; sx < x1; sx++) {
                    lv_color_t c;
                    c.full = in[sy * w + sx];
                    uint32_t c32 = lv_color_to32(c);
                    r += (c32 >> 16) & 0xFF;
                    g += (c32 >> 8) & 0xFF;
                    b += c32 & 0xFF;
                    n++;
                }
            }
            lv_color_t c = lv_color_make(r / n, g / n, b / n);
            out[y * target + x] = c.full;
        }
    }
    free(slot.pixels);
    slot.pixels = (uint8_t *)out;
    slot.capacity = needed;
    slot.dsc.header.w = slot.dsc.header.h = target;
    slot.dsc.data_size = needed;
    slot.dsc.data = slot.pixels;
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
    if (ok && cache_kind == cache::Kind::Cover) shrink(slot, ui::make_layout().cover);

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

struct PrefetchRequest {
    char cover_url[192];
    char background_url[192];
};

struct ThumbRequest {
    uint32_t token;
    bool     drop;   // free this token's buffer (the UI scrolled it far away)
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
    if (r.drop) {
        for (int i = 0; i < thumb_count; i++) {
            if (thumbs[i].token != r.token) continue;
            free(thumbs[i].pixels);
            thumbs[i] = thumbs[--thumb_count];
            break;
        }
        return true;
    }
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

// Next track's files: only the downloads, they are decoded when the track starts
static bool busy();
static bool busy_now() { return busy(); }

static bool process_prefetch() {
    PrefetchRequest r;
    if (!prefetch_queue || xQueueReceive(prefetch_queue, &r, 0) != pdTRUE) return false;
    unsigned long start = millis();
    bool cover = r.cover_url[0] && !cache::fetch(cache::Kind::Cover, r.cover_url, false).isEmpty();
    bool background = r.background_url[0] && !busy_now() && !cache::fetch(cache::Kind::Background, r.background_url, false).isEmpty();
    Serial.printf("[artwork] prefetch next: cover %s, background %s (%lu ms)\n", cover ? "ok" : "no", background ? "ok" : "no",
                  millis() - start);
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

    String token = player::client().apiToken();
    if (token.isEmpty()) return false;

    String path = "/clients/" + token + "/artwork";
    if (precache_cursor > 0) path += "?cursor=" + String(precache_cursor);
    Serial.printf("[artwork] pre-cache page %ld\n", precache_cursor);  // TEMPORARY: glitch hunt
    JsonDocument page(api::psram());  // a page stays around for minutes of downloads
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

    if (strcmp(req.cover_url, covers.url) != 0) ui::artwork_pending(req.has_accent, req.accent);

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
        if (cache_ok && process_prefetch()) continue;
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
    prefetch_queue = xQueueCreate(1, sizeof(PrefetchRequest));
    // Core 0 next to WiFi; LVGL renders on core 1
    xTaskCreatePinnedToCore(task, "artwork", 8192, nullptr, 1, &task_handle, 0);
}

void request(const Request &req) {
    if (!queue) return;
    xQueueOverwrite(queue, &req);
    xTaskNotifyGive(task_handle);
}

void prefetch(const char *cover_url, const char *background_url) {
    if (!prefetch_queue || !cover_url || !*cover_url) return;
    PrefetchRequest r = {};
    strlcpy(r.cover_url, cover_url, sizeof(r.cover_url));
    if (background_url) strlcpy(r.background_url, background_url, sizeof(r.background_url));
    xQueueOverwrite(prefetch_queue, &r);
    xTaskNotifyGive(task_handle);
}

void clear() {
    Request req = {};
    request(req);
}

bool thumb_request(uint32_t token, const char *url, bool large, bool half) {
    if (!thumb_queue || !url || !*url) return false;
    ThumbRequest r = {token, false, large, half, ""};
    strlcpy(r.url, url, sizeof(r.url));
    bool queued = xQueueSend(thumb_queue, &r, 0) == pdTRUE;
    xTaskNotifyGive(task_handle);
    return queued;
}

bool thumb_drop(uint32_t token) {
    if (!thumb_queue) return false;
    ThumbRequest r = {token, true, false, false, ""};
    bool queued = xQueueSend(thumb_queue, &r, 0) == pdTRUE;
    xTaskNotifyGive(task_handle);
    return queued;
}

void thumbs_release(uint32_t generation) {
    thumb_generation = generation;
    if (task_handle) xTaskNotifyGive(task_handle);
}

}  // namespace artwork
