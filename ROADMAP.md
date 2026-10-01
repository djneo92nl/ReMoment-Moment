# ReMomentClient Roadmap

The plan for turning the "Now Playing" display into a full touch remote for ReMoment. It covers work in both repos: **[C]** = this client, **[S]** = `ReMomentServer`.

## Guiding decisions

| Topic | Decision |
|---|---|
| Usage | Both docked (mains, always on) and handheld (battery) |
| Hardware | 7" Waveshare (1024×600) first; 3.5" ES3C28P (320×480) as a handheld remote later — one codebase, one PlatformIO env per board |
| Framework | Stay on Arduino (garnet_web is Arduino-only); port the 3.5" ESP-IDF drivers from `MeetMicRec` into `lib/` |
| UI tooling | Build the UI in C++ from now on; SquareLine Studio exports in `src/ui/` are replaced screen by screen |
| Navigation | Now Playing is home. **Swipe up** → device list. **Swipe down** → library. **Swipe in from the side** → volume overlay |
| Button feedback | Update the screen immediately (optimistic), correct when MQTT confirms |
| Play state | Subscribe to the `state` topic; keep the 15 s no-data timeout as a fallback |
| Devices shown | Only devices assigned to this client on the server, in the server's sort order |
| Artwork storage | SD card required for artwork; without a card, artwork is skipped and a warning is shown |
| Backgrounds | Rendered by the server per album, downloaded and cached like covers |
| Library source | ReMoment's own unified library (not per service) |
| Picking an item | Plays immediately on the current device |
| Nothing playing | Backlight off until touch or until playback starts |
| Connection lost | Keep the screen, show a small status icon, reconnect in the background |
| Settings / setup | [garnet_web](https://github.com/djneo92nl/garnet_web) (captive-portal WiFi setup + web settings) |
| Registration | Client self-registers and shows "waiting for approval" with a code; approve in the server UI |

---

## Phase 1 — Foundation

Removes everything hard-coded (WiFi, broker IP, `PLAYER_ID`) and lays the groundwork for multiple screens.

> Client items marked done are **compile-tested only** — verify on hardware: setup portal, mDNS discovery, registration/approval, MQTT, swipe gestures, `board::set_backlight()` (PWM direction is a guess).

- [x] **[C] garnet_web integration** — captive-portal WiFi setup, settings stored in NVS, replaces `src/config.h`
  - [x] Server URL + MQTT broker setting, with automatic discovery as the default
  - [x] Debug/status page (replaces the current `GET /status` JSON endpoint on port 80)
  - [x] OTA firmware upload — the 16 MB partition table already has two 6.25 MB app slots; firmware is ~1.3 MB once images are off flash
  - [x] *Nice to have:* SD file manager (inspect / clear the artwork cache)
- [x] **[S] Server discovery** — `_remoment._tcp` via Avahi on the Docker host (ansible), plus `GET /api/info` for the MQTT broker; playbook not yet run
- [x] **[S] Pairing code** — 6-character code in the register response and the admin client list
- [x] **[C] Client registration** — `POST /clients/register` with hardware ID + firmware version, show code + "waiting for approval", poll `/clients/status/{token}`, store `apiToken`, send heartbeat
- [x] **[C] Assigned devices** — fetch `GET /clients/{apiToken}/devices`, remember the last used device
- [x] **[C] Startup screen** — small logo + connection steps (WiFi → server → MQTT); replaces the 1536×1024 startup image
- [x] **[C] Connection status icon** on all screens
- [x] **[C] UI in code** — screen manager, swipe navigation, shared styles; port Now Playing out of SquareLine
- [x] **[C] Board abstraction** — keep display, touch, backlight, battery, SD and LVGL locking behind one small `board` interface, so a second board is a new implementation rather than `#ifdef`s through the app
- [x] **[C] Resolution-independent UI** — layouts from screen size (no hard-coded 1024×600 coordinates), so a portrait 320×480 layout can be added later
- [x] **[C] HTTP client layer** — small REST helper for API calls (JSON, timeouts, runs outside the LVGL task)

## Phase 2 — Artwork on SD

Moves all images off flash (~4.7 MB) and downloads them as needed.

> Client items marked done are **compile-tested only** — verify on hardware: download + SD cache, decode speed, cross-fade smoothness (full-screen background fade is software-blended), eviction.

- [x] **[C] SD artwork cache** — download `artwork.proxy_320` from the `/data` payload, store it on SD keyed by the server's hash, evict the oldest when full
- [x] **[C] JPEG decoding** — JPEGDEC (bitbank2) into PSRAM, in a dedicated artwork task
- [x] **[S] Background rendering** — extend `ProcessArtwork` with a 1024×600 background JPEG per album and add it to the `artwork` payload (`proxy_bg`, blurred and darkened cover)
- [x] **[S] Decoder-friendly JPEGs** — baseline (not progressive) JPEGs for all client sizes
- [x] **[C] UI tint from album colors** — progress bar, buttons and accents from `safe_colors`
- [x] **[C] Cross-fade** between old and new cover/background on track change
- [x] **[C/S] Radio and source logos** when there's no album art — server falls back to the station image or a generated logo (Spotify, radio, line-in, Bluetooth, TV, cast, CD, music) through the same `artwork` object, with `kind: album|radio|source`; no client changes needed
- [x] **[S] Pre-render recent artwork** — scheduled job keeps the last N played albums (default 500) fully rendered (cover + background)
- [x] **[C/S] Pre-cache library covers** in the background while idle — server lists artwork most-recently-played first (`GET /api/clients/{token}/artwork`), client downloads missing covers and backgrounds
- [x] **[S] SD card export** — admin download / artisan command: zip of the last N albums in the exact SD layout (`remoment/covers/{hash}.jpg`, `remoment/backgrounds/{hash}.jpg`) to unzip onto the card; `/settings/clients` → SD card artwork, or `php artisan artwork:export-sd`
- [x] **[C] No-SD warning** and artwork placeholders
- [x] **[S] Placeholder in `/api/info`** — the generic music logo as a full artwork object; the client shows `placeholder.proxy_320` whenever there's no cover (client side done)
- [x] **[C] Remove the built-in images** from `src/ui/`

## Phase 3 — Playback control and devices

> Client items marked done are **compile-tested only** and written against the agreed server contract (shuffle/repeat/like/power endpoints, `GET /devices/{id}/radio`, `/modes` topic, `muted` in `/volume`, retained `/data`). Icons are LVGL's built-in symbols until the B&O reference designs arrive (like = plus/check).

- [x] **[C] Transport** — play/pause, next, previous via `POST /devices/{id}/{action}`
- [x] **[C] Volume overlay** (swipe from the side) — slider + mute via `PUT /devices/{id}/volume|mute`
- [x] **[C] Subscribe to `state` and `volume` topics** so the play/pause and volume shown on screen come from the server
- [ ] **[S] Shuffle, repeat, like** — new actions/endpoints; drivers report which ones they support
- [x] **[C] Shuffle, repeat, like buttons**, hidden when the device doesn't support them
- [x] **[C] Device list screen** (swipe up) — assigned devices with a mini now-playing preview; tap to switch (resubscribes MQTT)
- [x] **[C] Source switching** — `GET /devices/{id}/sources`, `POST …/sources/activate`
- [x] **[C] Multiroom** — join/leave groups
- [ ] **[S] Radio station list API** — stations currently only exist in the web UI (`RadioStationController`)
- [x] **[C] Radio stations** — pick and play via `POST /devices/{id}/radio/{station}`
- [ ] **[S] Power on / standby actions** in the API
- [x] **[C] Power/standby** per device

## Phase 4 — Library

- [ ] **[S] Browse API** — paginated artists → albums → tracks from the unified library, compact JSON for the client
- [x] **[S] Small thumbnails** — add a ~120 px size to `ProcessArtwork` for grids
- [ ] **[S] Recently played** endpoint from the playback history
- [ ] **[S] Favorites** — new concept on the server (none exists yet)
- [ ] **[C] Library screen** (swipe down) — artists → albums → tracks, recently played, favorites
- [ ] **[C] Play now** on the current device via `library/play`
- [ ] **[C] Tap album cover** on Now Playing → open that album *(optional shortcut)*

## Power and battery (alongside the phases)

- [ ] **[C] Dim after idle**, **backlight off** when nothing is playing
- [ ] **[C] Wake on touch** (and on playback start)
- [ ] **[C] Battery indicator** in the status bar
- [ ] **[C] Deep sleep on battery** when idle — needs fast WiFi/MQTT reconnect on wake

## Phase 5 — 3.5" handheld remote

Board: ES3C28P (ST77922 320×480 QSPI panel, custom-protocol touch at I2C `0x55`, ES8311 audio, RGB LED, SDIO SD, battery ADC). Reference drivers and hard-won bring-up notes live in `~/Code/MeetMicRec` (`src/display.c`, `src/touch.c`, `include/board_config.h`, `CLAUDE.md`).

- [ ] **[C] PlatformIO env + board definition** for the ES3C28P
- [ ] **[C] Port display driver** — vendor 62-command ST77922 init sequence, 40 MHz QSPI, `LV_COLOR_16_SWAP`, small DMA partial buffer (no full refresh)
- [ ] **[C] Port touch driver** — custom 0x55 protocol, `TP_INT` as the press/release signal
- [ ] **[C] Portrait layouts** focused on a handheld remote: now playing, controls, volume, device list; library limited or left out
- [ ] **[S] Artwork sizes per client** — backgrounds at the client's resolution (320×480), reported at registration
- [ ] **[C] RGB LED status** — play state / connection / album color
- [ ] **[C] Touch/click sounds** through the ES8311 speaker
- [ ] **[C] Battery** — confirm the divider ratio on real hardware (unverified in MeetMicRec)

## Later

- [ ] Search (on-screen keyboard)
- [ ] Playlists
- [ ] Queue view (`GET /devices/{id}/queue`)
- [ ] Video device support

## Dropped

Themes/skins, lyrics/extra track info.
