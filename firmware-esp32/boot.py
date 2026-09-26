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
    # Stock ESP32 MicroPython always has ".frozen" on sys.path, so this branch
    # only runs under CPython / host testing, which has no such entry.
    pass

try:
    import main  # noqa: E402
    main.run()
except Exception as e:
    # main.run() already resets on any error inside _run(), so getting here
    # means something failed at import time (e.g. a broken frozen image) or in
    # main.run() itself. Don't drop to the REPL -- reset so a pending-verify
    # OTA image rolls back.
    try:
        sys.print_exception(e)
    except Exception:
        print(e)
    import time
    time.sleep(5)  # window to interrupt (Ctrl-C) during development before resetting
    try:
        import machine
        machine.reset()
    except ImportError:
        raise SystemExit("reset requested")
