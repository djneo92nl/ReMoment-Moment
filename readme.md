# ReMomentClient

Touch remote for ReMoment, a Laravel controller that puts networked media devices (Sonos, Bang & Olufsen, Spotify, …) behind one API. This firmware runs on an ESP32-S3 with an LVGL UI, black background and simple shapes in the spirit of the BeoSound Moment.

## Features

- **Now Playing**: artist, track, album, cover art, progress and play state, pushed over MQTT
- **Playback control**: transport, seek, volume, mute
- **Volume screen** (swipe right): volume slider and the source picker (active source outlined, input and Borrowed badges)
- **Queue** (swipe left): up next, tap to jump
- **Devices**: list with live state, details, sources
- **Library**: recent, playlists, radio, A–Z, search; tap to play on the current device
- **Artwork**: cached on SD, downloaded in a background task
- **Setup**: captive-portal WiFi setup, settings, OTA and SD file manager via [garnet_web](https://github.com/djneo92nl/garnet_web); the server is found via mDNS, and the client registers itself and waits for approval with a pairing code

See `ROADMAP.md` for the plan and `CLAUDE.md` for architecture and threading notes.

## Boards

| Environment | Board |
|---|---|
| `ESP32-S3-Touch-LCD-7B` | Waveshare 7" 1024×600 (primary) |
| `ES3C28P` | 3.5" 320×480 |
| `ESP32-4848S040` | Guition 4" 480×480 (not yet verified on hardware) |

Hardware access lives in `src/board/`; layouts derive from the screen size.

## Build and flash

`pio` lives in `~/.platformio/penv/bin/`.

```bash
pio run -e ESP32-S3-Touch-LCD-7B                  # build
pio run --target upload -e ESP32-S3-Touch-LCD-7B  # flash
pio device monitor                                # serial, 115200 baud
```

There are no automated tests; verify by flashing and watching serial output and the display.

## Configuration

No credentials in the source. On first boot the device opens a setup AP; set WiFi, server URL (empty = mDNS) and the web password at `http://<device>/`. The default web password is in `src/app_config.h`.

## Screenshots

`tools/screenshot.py` grabs the screen over serial; `tools/screenshots.py` captures all main screens.
