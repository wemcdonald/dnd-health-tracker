# MicroPython frozen manifest: bakes the health-bar app into the ESP32 image so
# an OTA update ships the whole app atomically in one hashed image.
#
# Referenced by board/HEALTHBAR_C3/mpconfigboard.cmake (MICROPY_FROZEN_MANIFEST).
# Build with `just build esp32 <version>`, which writes build/gen/version.py first.
#
# Per-device config (/data/*.json) is NOT frozen: it lives on the vfs partition,
# outside both OTA slots, and persists across updates.

include("$(PORT_DIR)/boards/manifest.py")  # base ESP32 manifest (asyncio, neopixel, ...)

freeze(".", (
    "boot.py",
    "main.py",
    "anim.py",
    "colors.py",
    "config.py",
    "device.py",
    "leds.py",
    "poll.py",
    "ota.py",
    "remote_config.py",
    "wifi.py",
    "portal.py",
))

# Generated per build so the baked FIRMWARE_VERSION always equals the published
# manifest version (see justfile `build esp32`).
freeze("build/gen", "version.py")
