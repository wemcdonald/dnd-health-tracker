# MicroPython runs boot.py then main.py automatically at power-on. We keep the
# real logic in main.run() (wrapped in crash-recovery) and just invoke it here.
import main

main.run()
