// Debug/marketing screenshots over the serial port: the host sends kTrigger, the device answers with
// "RMSHOT <w> <h> <rgb565 byte-swapped 0|1> <baud>\n" followed by w*h*2 bytes of raw RGB565, at a higher baud rate.
// See tools/screenshot.py.
#pragma once

namespace screenshot {
void poll();  // call from loop(); cheap when nothing was requested
}
