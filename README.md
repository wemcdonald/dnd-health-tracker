# D&D Beyond LED Health Bar

An LED health bar that mirrors a **public** D&D Beyond character's hit points on a
WS2812B strip — green→amber→red gradient, damage/heal flashes, and a low-HP
heartbeat — so the table can see how close a character is to going down.

Runs on a **Raspberry Pi Pico 2 W** (RP2350 microcontroller) in MicroPython.

## Layout

- **[`firmware/`](firmware/)** — the Pico 2 W firmware (MicroPython). Start here:
  see [`firmware/README.md`](firmware/README.md) for flashing, deploying, and
  running the tests/sim.
- **[`docs/plans/`](docs/plans/)** — design docs. Current:
  [`2026-06-24-pico2w-healthbar-design.md`](docs/plans/2026-06-24-pico2w-healthbar-design.md).
- **[`legacy-go/`](legacy-go/)** — the original Go implementation for the
  Raspberry Pi Zero 2 W (full Linux). Superseded by the Pico 2 W rewrite; kept
  for reference and history.

## Quick start

```sh
# Flash the Pico 2 W MicroPython UF2 (once), then:
cd firmware
./push.sh            # deploy to the board over USB (needs `pip install mpremote`)
```

First boot with no saved WiFi brings up a `healthbar-setup` access point with a
captive portal — connect to it to enter your WiFi and a public character ID.

Develop and test without hardware:

```sh
cd firmware
for t in tests/test_*.py; do python3 "$t"; done   # unit tests (CPython)
python3 demo.py                                    # animated terminal preview
```
