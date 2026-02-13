## Media Interface for Remoment Player


| Category | Task | Description | Dependencies | Notes |
|----------|------|-------------|--------------|-------|
| Core Stability | LVGL + WiFi + MQTT stabilization | Ensure glitch-free updates and stable backlight |  | Pixel clock ~18–20MHz, PWM at high frequency |
| Core Stability | Backlight PWM tuning | Stable brightness without flicker | LVGL + WiFi | Use hardware PWM 20–40kHz |
| Core Stability | LVGL task timing tuning | Optimize MIN/MAX delays | LVGL core | Set MIN_DELAY_MS ~2, MAX_DELAY_MS ~20 |
| Player Infrastructure | Dynamic player objects | Support multiple players updating their labels/sliders | Core stability | PlayerID mapping required |
| Player Infrastructure | Progress slider update | uic_playing_progress slider reflects MQTT progress | Dynamic player objects | Use lv_async_call() for thread safety |
| Player Infrastructure | Artist/track labels | Display artist and track info | Dynamic player objects | Fixed string buffers recommended |
| Player Infrastructure | Album art placeholders | Display album art | Dynamic player objects | LVGL image object |
| Asset Management | SD card initialization and testing | Prepare filesystem for asset storage | Core stability | Supports FAT32 or exFAT |
| Asset Management | Download images | Download album art/wallpapers to SD card | SD card setup | HTTP/MQTT handling, safe memory loading |
| Asset Management | Album art rendering | Render images in LVGL | Downloaded assets | Memory-aware loading (avoid SRAM overrun) |
| Asset Management | Dynamic wallpaper | Change background dynamically | Downloaded assets | Partial redraw preferred |
| UI Screens | Now Playing | Display current player info | Dynamic player objects | Album art, labels, slider |
| UI Screens | Player Selection Screen | Change active player dynamically | Dynamic player objects | Scroll list, highlight active player |
| UI Screens | Sources Screen | Select input sources | Core stability | Dynamic UI |
| UI Screens | Favorites Screen | Quick presets | Dynamic player objects |  |
| UI Screens | Search Screen | Search tracks/playlists | Core stability |  |
| UI Screens | Volume/Media Control Overlay | Slide-in panel for volume and media controls | Now Playing | LVGL overlay |
| UI Screens | Video Device Support | Display + control video devices | Now Playing | Extend controls for video playback |
| Setup/Configuration | WiFi setup | Connect to network | Core stability |  |
| Setup/Configuration | Player ID setup | Set playerID for this device | Dynamic player objects |  |
| Setup/Configuration | SD card format | Add format option in UI | SD card initialization | Destructive operation, confirm prompt |
| Setup/Configuration | Server configuration | Configure API server address/port | WiFi setup |  |
| Setup/Configuration | Advanced controls | Speaker groups, WISA speaker management | Core stability | Some features remain in API interface |
| Networking/Messaging | MQTT improvements | Efficient JSON for metadata + separate topics for progress | Dynamic player objects | High-frequency progress updates |
| Networking/Messaging | Multiple player support | Support multiple players updating independently | Dynamic player objects | Subscribe to topics dynamically |
| Polish/Performance | Partial redraw | Only update changed objects | LVGL core | Reduces CPU/PSRAM load |
| Polish/Performance | Smooth animations | Sliders, labels, transitions | Now Playing | LVGL timer tuning |
| Polish/Performance | Memory optimization | Ensure PSRAM usage is efficient | Asset management | Draw buffer allocation in PSRAM |
| Optional/Advanced | OTA updates | Network firmware update | Core stability |  |
| Optional/Advanced | Themes/skins | Change UI appearance | LVGL core |  |
| Optional/Advanced | Multi-player sync | Show progress/labels for all players | Dynamic player objects |  |
| Optional/Advanced | Playlist/queue management | Remote queue control | Core stability |  |
| Optional/Advanced | Lyrics/metadata display | Extra info per track | Album art/metadata |  |
