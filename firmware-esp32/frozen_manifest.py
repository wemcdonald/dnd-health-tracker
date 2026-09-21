# MicroPython frozen manifest — bakes the health-bar app into the ESP32 image so
# an OTA update ships the whole app atomically in one signed/hashed image.
#
# Used by the build:
#   make -C $MICROPYTHON_DIR/ports/esp32 BOARD=ESP32_GENERIC_C3 \
#        FROZEN_MANIFEST=<repo>/firmware-esp32/frozen_manifest.py
#
# Per-device config (data/config.json, wifi.json) is NOT frozen — it lives on the
# littlefs filesystem and persists across OTA updates.

include("$(PORT_DIR)/boards/manifest.py")  # base ESP32 board manifest (bootloader, drivers)

# App modules, relative to this file. boot.py auto-runs main.run() at startup.
freeze(".", (
    "boot.py",
    "main.py",
    "anim.py",
    "colors.py",
    "config.py",
    "leds.py",
    "poll.py",
    "ota.py",
    "wifi.py",
    "portal.py",
    "version.py",
))
