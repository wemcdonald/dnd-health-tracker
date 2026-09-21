"""Runs at power-on / wake. Hands straight to the supervisor.

Kept tiny on purpose: all logic lives in main.run() so it is testable and so a
crash there triggers the reset-and-recover path rather than dropping to the REPL.
"""

import main

main.run()
