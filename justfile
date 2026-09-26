# D&D Health Tracker - device tasks. Run `just` to list.
#
# One interface for both boards; the board is the first argument:
#
#   just build pico            build firmware for a board (pico | esp32)
#   just flash pico            flash a USB-connected board (dev)
#   just publish pico 2        build + publish an OTA image (version 2) to the server feed
#   just set name shen         provision character slug over USB (pico)
#   just set wifi SSID 'pw'    provision wifi (repeat pairs; priority = order)
#   just test                  run host unit tests for both firmwares
#
# Firmware images are served per board at /firmware/<board>/... so a Pico image
# can never reach an ESP32. The board differences (toolchain, flasher, OTA slot)
# live in these recipes; the server + publish tool are board-agnostic.

set positional-arguments

pico_sdk  := env_var_or_default("PICO_SDK_PATH", justfile_directory() / ".." / "pico-sdk")
build_dir := justfile_directory() / "firmware-c" / "build"
uf2       := build_dir / "m1_portal.uf2"
esp32_src := justfile_directory() / "firmware-esp32"
fw_root   := justfile_directory() / "server" / "firmware"
publish   := "node " + justfile_directory() / "server" / "tools" / "publish-fw.mjs"
provision := "node " + justfile_directory() / "tools" / "provision.mjs"

# list targets
default:
    @just --list

# build firmware for a board (dev build):  just build pico | just build esp32 [version]
build board version="1":
    #!/usr/bin/env sh
    set -e
    case "{{board}}" in
      pico)
        (cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake -B build -DPICO_BOARD=pico2_w \
            -DPOLL_HOST=dndhealth.willflix.org -DPOLL_PORT=80 -DENABLE_STATUSD=ON \
            -DHEALTHBAR_NAME= -DDEV_SEED_CONFIG=OFF -DFIRMWARE_VERSION="{{version}}" -DOTA_TBYB=OFF)
        (cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake --build build -j4 --target m1_portal) ;;
      esp32)
        # A dual-OTA MicroPython image with the app frozen in. Needs a MicroPython
        # + ESP-IDF checkout; see firmware-esp32/README.md#ota. FROZEN_MANIFEST
        # freezes firmware-esp32/*.py into the image.
        [ -n "$MICROPYTHON_DIR" ] || { echo "set MICROPYTHON_DIR to your micropython checkout"; exit 1; }
        make -C "$MICROPYTHON_DIR/ports/esp32" BOARD=ESP32_GENERIC_C3 \
            FROZEN_MANIFEST="{{esp32_src}}/frozen_manifest.py" ;;
      *) echo "unknown board: {{board}} (pico|esp32)"; exit 1 ;;
    esac

# flash a USB-connected board (dev; not OTA):  just flash pico | just flash esp32
flash board:
    #!/usr/bin/env sh
    set -e
    case "{{board}}" in
      pico) picotool load "{{uf2}}" -f -x ;;
      esp32)
        # One mpremote session with `resume`: boot.py runs the app and never
        # returns, so the soft reset mpremote does on each connect would hang it.
        # Set ESP32_PORT when another board (e.g. the Pico) is also plugged in.
        set -- connect "${ESP32_PORT:-auto}" resume \
          exec "import os; 'data' in os.listdir('/') or os.mkdir('/data')" \
          + fs cp {{esp32_src}}/data/config.json :/data/config.json
        for f in {{esp32_src}}/*.py; do set -- "$@" + fs cp "$f" ":/$(basename "$f")"; done
        mpremote "$@" + reset ;;
      *) echo "unknown board: {{board}} (pico|esp32)"; exit 1 ;;
    esac

# build an OTA image and publish it to the server feed:  just publish pico [version]
# Pico builds TBYB-flagged so a bad update auto-reverts on the device.
publish board version="1":
    #!/usr/bin/env sh
    set -e
    case "{{board}}" in
      pico)
        (cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake -B build -DPICO_BOARD=pico2_w \
            -DPOLL_HOST=dndhealth.willflix.org -DPOLL_PORT=80 -DENABLE_STATUSD=ON \
            -DHEALTHBAR_NAME= -DDEV_SEED_CONFIG=OFF -DFIRMWARE_VERSION="{{version}}" -DOTA_TBYB=ON)
        (cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake --build build -j4 --target m1_portal)
        IMG="{{build_dir}}/m1_portal.bin" ;;
      esp32)
        just build esp32 "{{version}}"
        IMG="${ESP32_IMAGE:-$MICROPYTHON_DIR/ports/esp32/build-ESP32_GENERIC_C3/micropython.bin}" ;;
      *) echo "unknown board: {{board}} (pico|esp32)"; exit 1 ;;
    esac
    FIRMWARE_DIR="{{fw_root}}" {{publish}} "{{board}}" "$IMG" "{{version}}"

# provision config over USB (pico): name / wifi
set *args:
    {{provision}} "$@"

# run host unit tests for both firmwares (no hardware)
test:
    bash firmware-c/test/run.sh
    cd firmware-esp32 && for t in tests/test_*.py; do python3 "$t"; done

# ONE-TIME (pico): convert a device from the single-image layout to A/B OTA.
# Device USB-connected in BOOTSEL. Config auto-migrates on first boot. Back up the
# device's wifi/slug first (its status page) — runbook in firmware-c/ota/MIGRATION.md.
migrate-ota version="1":
    #!/usr/bin/env sh
    set -e
    just build pico "{{version}}"                          # build the app first (no device needed)
    cd firmware-c
    picotool partition create ota/partition_table.json build/pt.uf2
    picotool load build/pt.uf2 -f                          # write the A/B partition table
    picotool reboot -u && sleep 2                          # reboot so the bootrom registers the PT
    picotool load build/m1_portal.uf2 -p 0 -f -x           # flash slot A (partition index 0) and run
