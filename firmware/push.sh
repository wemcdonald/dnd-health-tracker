#!/usr/bin/env bash
# Push the firmware to a Raspberry Pi Pico 2 W running MicroPython.
#
# Prereqs:
#   - Flash the Pico 2 W (RP2350, cyw43) MicroPython UF2 once (hold BOOTSEL,
#     drag the .uf2 onto the RPI-RP2 drive). See README.md.
#   - pip install mpremote
#
# Usage:
#   ./push.sh            # copy all modules + data, then soft-reset to run
#   ./push.sh --run      # also open the REPL afterwards to watch output
#
# mpremote auto-detects the board; set DEVICE=/dev/ttyACM0 to override.
set -euo pipefail

cd "$(dirname "$0")"
MP=(mpremote)
[ -n "${DEVICE:-}" ] && MP=(mpremote connect "$DEVICE")

MODULES=(boot.py main.py config.py colors.py hp.py anim.py leds.py ddb.py wifi.py portal.py)

echo "Copying modules…"
for f in "${MODULES[@]}"; do
  "${MP[@]}" fs cp "$f" ":$f"
done

echo "Copying default config (only if absent on device)…"
"${MP[@]}" fs mkdir :data 2>/dev/null || true
for f in config.json theme.json wifi.json; do
  if ! "${MP[@]}" fs cat ":data/$f" >/dev/null 2>&1; then
    "${MP[@]}" fs cp "data/$f" ":data/$f"
    echo "  installed data/$f"
  else
    echo "  kept existing data/$f"
  fi
done

echo "Resetting board…"
"${MP[@]}" reset

if [ "${1:-}" = "--run" ]; then
  exec "${MP[@]}" repl
fi
echo "Done. Power-cycle or watch with: mpremote repl"
