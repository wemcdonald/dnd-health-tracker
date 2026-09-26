"""Firmware version -- host/dev fallback.

This file is only used by host tests and stock-MicroPython dev copies
(`just flash esp32`). OTA builds freeze build/gen/version.py instead, written
by `just build esp32 N`, so the published image carries the build's N.

A monotonically increasing integer. OTA compares it to the server manifest's
version field (line 1 of /firmware/esp32/latest) and only updates when the
server's is higher.
"""

FIRMWARE_VERSION = 1
