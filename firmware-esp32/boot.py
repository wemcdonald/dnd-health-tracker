"""Runs at power-on / wake. Hands straight to the supervisor.

Kept tiny on purpose: all logic lives in main.run() so it is testable and so a
crash there triggers the reset-and-recover path rather than dropping to the REPL.

On an OTA build the app is frozen into the image. Put the frozen modules ahead
of the filesystem on sys.path so a stale .py copied onto the board during
development can never shadow the code an OTA update just installed.
"""

import sys

try:
    sys.path.remove(".frozen")
    sys.path.insert(0, ".frozen")
except ValueError:
    pass  # no frozen modules (stock MicroPython dev flow)

import main  # noqa: E402

main.run()
