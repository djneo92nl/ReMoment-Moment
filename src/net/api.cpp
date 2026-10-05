#include "api.h"

#include <esp_heap_caps.h>

#include "player.h"

namespace api {

namespace {
struct PsramAllocator : ArduinoJson::Allocator {
    void *allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void *ptr) override { heap_caps_free(ptr); }
    void *reallocate(void *ptr, size_t size) override { return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM); }
};
}  // namespace

ArduinoJson::Allocator *psram() {
    static PsramAllocator allocator;
    return &allocator;
}

static remoment::Http &http() { return player::client().http(); }

int get(const char *path, JsonDocument &response) { return http().get(path, response); }
int post(const char *path, const JsonDocument &body, JsonDocument &response) { return http().post(path, body, response); }
int put(const char *path, const JsonDocument &body, JsonDocument &response) { return http().put(path, body, response); }
int del(const char *path, JsonDocument &response) { return http().del(path, response); }

}  // namespace api
