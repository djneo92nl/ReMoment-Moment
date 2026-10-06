#!/usr/bin/env python3
"""Walk through the screens of the connected device and save a PNG of each.

usage: tools/screenshots.py [out_dir] [screens]   e.g. tools/screenshots.py shots nvqdels
Run with PlatformIO's python (has pyserial): ~/.platformio/penv/bin/python tools/screenshots.py
"""
import os, sys, time
import screenshot as shot

NAMES = {"n": "now_playing", "v": "volume", "q": "queue", "d": "devices", "e": "device_details",
         "l": "library", "s": "settings"}
out_dir = sys.argv[1] if len(sys.argv) > 1 else "screenshots"
screens = sys.argv[2] if len(sys.argv) > 2 else "nvqdels"
os.makedirs(out_dir, exist_ok=True)

s = shot.open_port()
for letter in screens:
    s.reset_input_buffer()
    s.write(b"\xa6" + letter.encode())
    reply = s.readline().decode(errors="ignore").strip()
    if not reply.endswith("ok"):
        print(f"skipped {NAMES.get(letter, letter)}: {reply or 'no reply'}")
        continue
    time.sleep(3)  # slide animation + data loads (library, queue, ...)
    shot.grab(s, os.path.join(out_dir, f"{NAMES[letter]}.png"))
s.write(b"\xa6n")  # back to now playing
