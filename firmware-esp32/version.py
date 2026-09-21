"""Firmware version.

A monotonically increasing integer. OTA compares this to the server manifest's
version field (line 1 of /firmware/latest) and only updates when the server's is
higher. Bump this on every published image and keep it in step with the
`version` you write into the OTA manifest.txt.
"""

FIRMWARE_VERSION = 1
