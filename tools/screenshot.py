#!/usr/bin/env python3
"""Grab a screenshot from the connected device over USB serial.

usage: tools/screenshot.py [out.png] [port]     (needs pyserial; PlatformIO's python has it:
~/.platformio/penv/bin/python tools/screenshot.py)
"""
import glob, struct, sys, zlib
import serial

out = sys.argv[1] if len(sys.argv) > 1 else "screenshot.png"
ports = glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
port = sys.argv[2] if len(sys.argv) > 2 else ports[0]

s = serial.Serial(timeout=2)
s.port, s.baudrate = port, 115200
s.dtr = s.rts = False  # opening the port must not reset the board
s.open()
s.reset_input_buffer()
for attempt in range(5):
    s.write(b"\xa5")
    line = ""
    while True:
        line = s.readline().decode(errors="ignore").strip()
        if not line or line.startswith(("RMSHOT", "[screenshot]")):
            break
    if line:
        break
if not line:
    sys.exit("no response (firmware without screenshot support?)")
if line.startswith("[screenshot]"):
    sys.exit(line)
_, w, h, swap, baud = line.split()
w, h = int(w), int(h)
s.baudrate = int(baud)
s.timeout = 10
data = s.read(w * h * 2)
s.baudrate = 115200
if len(data) != w * h * 2:
    sys.exit(f"short read: {len(data)} of {w * h * 2} bytes")
raw = bytearray(data)
if swap == "1":
    raw[0::2], raw[1::2] = data[1::2], data[0::2]
px = bytearray(w * h * 3)
for i in range(w * h):
    v = raw[2 * i] | (raw[2 * i + 1] << 8)
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    px[3 * i] = r << 3 | r >> 2
    px[3 * i + 1] = g << 2 | g >> 4
    px[3 * i + 2] = b << 3 | b >> 2
def chunk(tag, body):
    c = tag + body
    return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c))


rows = b"".join(b"\0" + bytes(px[y * w * 3:(y + 1) * w * 3]) for y in range(h))
with open(out, "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))
print(f"saved {out} ({w}x{h})")
