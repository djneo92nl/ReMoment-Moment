#include "artwork.h"

#include <Arduino.h>
#include <JPEGDEC.h>
#include <SD_MMC.h>
#include <lvgl.h>

#include "cache.h"
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
static const lv_img_dsc_t *load(Kind &kind, const char *url, bool &changed) {
    changed = false;
    if (strcmp(url, kind.url) == 0) return kind.shown >= 0 ? &kind.slots[kind.shown].dsc : nullptr;

    changed = true;
    strlcpy(kind.url, url, sizeof(kind.url));
    int next = kind.shown == 0 ? 1 : 0;
    Slot &slot = kind.slots[next];

    // The free slot may still be fading out on screen: take it off first
    ui::artwork_release(&slot.dsc);

    String path = artwork::cache::fetch(url);
    if (path.isEmpty() || !decode(path, slot)) {
        kind.shown  = -1;
        kind.url[0] = '\0';  // retry on the next request
        return nullptr;
    }
    kind.shown = next;
    return &slot.dsc;
}

static void task(void *) {
    jpeg = new JPEGDEC();
    bool cache_ok = artwork::cache::begin();
    if (!cache_ok) Serial.println("[artwork] SD cache unavailable");

    Request req;
    for (;;) {
        if (xQueueReceive(queue, &req, portMAX_DELAY) != pdTRUE) continue;
        if (!cache_ok) req.cover_url[0] = req.background_url[0] = '\0';

        bool cover_changed, background_changed;
        const lv_img_dsc_t *cover      = load(covers, req.cover_url, cover_changed);
        const lv_img_dsc_t *background = load(backgrounds, req.background_url, background_changed);
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
