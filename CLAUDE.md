# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is **ReMomentClient**, the primary hardware client for ReMoment (`~/Code/ReMomentServer`), a Laravel-based controller layer that sits in front of networked media devices (Sonos, Bang & Olufsen, Spotify, …) and exposes them through one unified API. It runs on a Waveshare ESP32-S3 7-inch Touch LCD (originally copied from the Waveshare demo project `13_LVGL_TRANSPLANT`) and renders its UI with LVGL.

**Intended scope** — a full touch remote for ReMoment, not just a display:
- **Now playing** — artist, track, album art, progress, play state (implemented: metadata + progress)
- **Playback control** — play/pause/stop/next/previous, seek, volume, mute
- **Device management** — pick the target device, switch sources, multiroom join/leave
- **Library browsing** — browse the ReMoment media library (artists, albums, playlists) and start playback on a device
- **Queue and radio** — view the play queue, start radio stations

The plan and its order live in `ROADMAP.md`. Currently implemented: WiFi setup and settings via garnet_web, server discovery, client registration, assigned devices, and the now-playing view over MQTT. A second board (3.5" ES3C28P, see `~/Code/MeetMicRec`) is planned, so keep hardware access behind `src/board/` and layouts derived from screen size.

### Server integration

- **MQTT (push, server → client)**: `remoment/player/{deviceId}/{data|progress|state|volume}` — published by listeners in `app/Listeners/Device/` on the server
- **REST API (pull + commands, client → server)**: defined in `routes/api.php` on the server, docs in `docs/api/`. Key endpoints: `GET /devices`, `POST /devices/{id}/{action}` (transport), `PUT /devices/{id}/volume|mute|seek`, `GET /devices/{id}/queue|sources`, `POST /devices/{id}/library/play|play-playlist`, `POST /devices/{id}/radio/{station}`, multiroom endpoints
- **Client registration**: `POST /clients/register` → poll `GET /clients/status/{registrationToken}` → use `apiToken` for `GET /clients/{apiToken}/devices` and `PUT /clients/{apiToken}/heartbeat`
- Always check the server repo (`/Users/remko/Code/ReMomentServer`) for current topic names, payload formats, and endpoint contracts rather than guessing
- **UI design**: the UI is written by hand in C++ (`src/ui/`); SquareLine Studio is no longer used. Look: black background, simple shapes, in the spirit of the Bang & Olufsen BeoSound Moment — ask for the user's reference images before redesigning a screen

## Build Commands

```bash
# Build
pio run -e ESP32-S3-Touch-LCD-7B

# Flash to device
pio run --target upload -e ESP32-S3-Touch-LCD-7B

# Monitor serial output (115200 baud)
pio device monitor

# Clean build
pio run -t clean && pio run -e ESP32-S3-Touch-LCD-7B
```

`pio` lives in `~/.platformio/penv/bin/` (not on PATH by default).

There are no automated tests — verification is done by flashing and observing serial output + display behavior.

## Architecture

### Layered Structure

```
Application:   src/main.cpp (boot flow state machine), src/settings.*, src/model.h
Network:       src/net/  — api (HTTP JSON), server (mDNS discovery + /api/info), registration, player (MQTT)
Artwork:       src/artwork/ — own FreeRTOS task (core 0): SD cache (/remoment/art/{hash}/, LRU eviction) + JPEGDEC decode into double-buffered PSRAM images
UI:            src/ui/   — ui.h is the public API; screens in now_playing.*, layout.h derives metrics from screen size
Board:         src/board/ — board.h interface, one board_*.cpp per supported board (selected by build flag, e.g. BOARD_WAVESHARE_7B)
GUI:           lib/lvgl/  (LVGL v8.4, vendored) + lib/lv_conf.h
HAL:           lib/lvgl_port/  +  lib/rgb_lcd_port/  +  lib/touch/  +  lib/io_extension/  +  lib/i2c/  +  lib/user_sd/
Settings/web:  garnet_web (lib_deps) — captive-portal WiFi setup, settings in NVS, OTA, SD file manager, status page
MCU:           ESP32-S3, Arduino-ESP32 3.x (ESP-IDF 5.5, FreeRTOS)
```

### Threading Model

- **LVGL task** (core 1, `lib/lvgl_port/`): runs `lv_timer_handler()` and LVGL event callbacks. LVGL is not thread-safe, so every LVGL call from another task must hold the lock (`board::lvgl_lock()` or the `LvglLock` RAII guard). All `ui::` functions take the lock themselves; code inside `src/ui/` called from them (e.g. `ui::now_playing::*`) assumes it is already held. This includes `lv_async_call()`, which creates an LVGL timer.
- **Arduino loop task**: `gwLoop()`, the boot flow, HTTP calls (blocking, 5 s timeout), MQTT. Owns `model` (`src/model.h`).
- **Artwork task** (core 0, `src/artwork/artwork.cpp`): downloads, caches and decodes covers/backgrounds; latest request wins (queue of 1). Each kind has two PSRAM slots; it calls `ui::artwork_release()` before overwriting a slot so the UI never references pixels being rewritten, then `ui::artwork_show()` to cross-fade.
- **garnet_web HTTP task**: serves the web UI. Settings callbacks/buttons are deferred to `gwLoop()` (loop task); only `gsInfo` getters run on the HTTP task, and they must stay read-only.

### Boot Flow (main.cpp)

`Phase::Network` (garnet_web connects WiFi or opens the setup AP) → `Discovering` (server URL from settings, else mDNS `_remoment._tcp`, then `GET /api/info` for the MQTT broker) → `Registering` (register, show pairing code, poll until approved, store `api_token` in NVS namespace `rm_reg`) → `Running` (assigned devices, MQTT subscribe to the current device's `{mqtt_topic}/data|progress`, heartbeat, device refresh). Status screens are only shown before the first `Running`; after that, connection problems show the small icon in the top-left (top layer).

### Key Hardware Config

- Display: 1024×600, RGB565 (16-bit), 20 MHz pixel clock — see `lib/rgb_lcd_port/rgb_lcd_port.h`
- Touch: GT911 capacitive controller over I2C, INT on GPIO 4 — see `lib/touch/gt911.h`
- LVGL draw buffer in PSRAM, anti-tearing currently disabled (single LCD framebuffer + bounce buffer) — see `lib/lvgl_port/lvgl_port.h` and `lib/rgb_lcd_port/rgb_lcd_port.h`
- LVGL memory pool: 48 KB; refresh period: 30 ms — see `lib/lv_conf.h`

### Configuration

No credentials in the source. WiFi, server URL (empty = mDNS) and the web password are set in the garnet_web UI at `http://<device>/` (setup AP when no WiFi is known; default web password in `src/app_config.h`). Firmware version and build number also live in `src/app_config.h`.

### Initialization Order (setup())

1. `board::init()`: GT911 touch init (also initializes the shared I2C bus and IO expander, so it must be first) → RGB LCD init → backlight on → `lvgl_port_init()`
2. `ui::init()` (takes the LVGL lock)
3. SD mount → `settings::register_groups()` → `gwBegin()` (must come after all `gsRegister()` calls)
4. `player::begin()`; everything else happens in `loop()`
