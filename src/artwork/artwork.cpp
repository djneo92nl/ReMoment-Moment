#include "artwork.h"

#include <Arduino.h>
#include <JPEGDEC.h>
#include <SD_MMC.h>
#include <lvgl.h>

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
static QueueHandle_t queue;
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

static bool decode(const String &path, Slot &slot) {
    size_t size;
    uint8_t *data = read_file(path, size);
    if (!data) return false;

    bool ok = false;
    if (jpeg->openRAM(data, size, on_draw)) {
        int w = jpeg->getWidth(), h = jpeg->getHeight();
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
            ok = jpeg->decode(0, 0, 0) == 1;
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

    String path = cache::fetch(cache_kind, url);
    if (path.isEmpty() || !decode(path, slot)) {
        kind.shown  = -1;
        kind.url[0] = '\0';  // retry on the next request
        return nullptr;
    }
    kind.shown = next;
    return &slot.dsc;
}

// ---- Pre-caching: while idle, fill the SD card from the server's artwork list ----
// (GET /api/clients/{token}/artwork: source logos, then albums most recently played first)

static const TickType_t    kIdleBeforePrecache = pdMS_TO_TICKS(30000);
static const unsigned long kPrecacheIntervalMs = 6UL * 60 * 60 * 1000;

static long          precache_cursor  = 0;   // -1 = sweep finished
static unsigned long precache_done_ms = 0;

static bool busy() { return uxQueueMessagesWaiting(queue) > 0; }

// One page of the list. Returns false when there's nothing more to do for now.
static bool precache_page() {
    if (model.phase != Phase::Running || busy()) return false;
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
        if (busy()) return false;  // a track change wins; resume this page later
        if (!cache::nearly_full(cache::Kind::Cover)) cache::fetch(cache::Kind::Cover, item["proxy_320"] | "", false);
        if (!cache::nearly_full(cache::Kind::Background)) cache::fetch(cache::Kind::Background, item["proxy_bg"] | "", false);
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

static void task(void *) {
    jpeg = new JPEGDEC();
    bool cache_ok = cache::begin();
    if (!cache_ok) Serial.println("[artwork] SD cache unavailable");

    Request req;
    for (;;) {
        if (xQueueReceive(queue, &req, cache_ok ? kIdleBeforePrecache : portMAX_DELAY) != pdTRUE) {
            while (precache_page()) {
            }
            continue;
        }
        if (!cache_ok) req.cover_url[0] = req.background_url[0] = '\0';

        // No cover (or still being processed on the server): show the server's placeholder logo
        if (cache_ok && !req.cover_url[0]) strlcpy(req.cover_url, model.placeholder_url, sizeof(req.cover_url));

        bool cover_changed, background_changed;
        const lv_img_dsc_t *cover      = load(covers, cache::Kind::Cover, req.cover_url, cover_changed);
        const lv_img_dsc_t *background = load(backgrounds, cache::Kind::Background, req.background_url, background_changed);
        ui::artwork_show(cover, cover_changed, background, background_changed, req.has_accent, req.accent);
    }
}

void begin(bool sd_available) {
    if (!sd_available) {
        ui::now_playing_set_notice("Insert an SD card to show album art");
        return;
    }
    queue = xQueueCreate(1, sizeof(Request));
    // Core 0 next to WiFi; LVGL renders on core 1
    xTaskCreatePinnedToCore(task, "artwork", 8192, nullptr, 1, nullptr, 0);
}

void request(const Request &req) {
    if (queue) xQueueOverwrite(queue, &req);
}

void clear() {
    Request req = {};
    request(req);
}

}  // namespace artwork
