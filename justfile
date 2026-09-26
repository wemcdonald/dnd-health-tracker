# D&D Health Tracker - device tasks. Run `just` to list.
#
# One interface for both boards; the board is the first argument:
#
#   just build pico            build firmware for a board (pico | esp32)
#   just flash pico            flash a USB-connected board (dev)
#   just publish pico 2        build + publish an OTA image (version REQUIRED, e.g. 2) to the server feed
#   just set name shen         provision character slug over USB (pico)
#   just set wifi SSID 'pw'    provision wifi (repeat pairs; priority = order)
#   just test                  run host unit tests for both firmwares
#   just flash-full esp32      first-time USB flash of a full image (ERASES the board)
#
# Firmware images are served per board at /firmware/<board>/... so a Pico image
# can never reach an ESP32. The board differences (toolchain, flasher, OTA slot)
# live in these recipes; the server + publish tool are board-agnostic.

set positional-arguments

pico_sdk  := env_var_or_default("PICO_SDK_PATH", justfile_directory() / ".." / "pico-sdk")
build_dir := justfile_directory() / "firmware-c" / "build"
uf2       := build_dir / "m1_portal.uf2"
esp32_src := justfile_directory() / "firmware-esp32"
mpy_dir     := env_var_or_default("MICROPYTHON_DIR", justfile_directory() / ".." / "micropython")
esp32_build := esp32_src / "build"
esp32_board := esp32_src / "board" / "HEALTHBAR_C3"
# OTA slot size; MUST match board/HEALTHBAR_C3/partitions.csv (0x1D0000).
esp32_slot  := "1900544"
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
        # Dual-OTA MicroPython image with the app frozen in (board/HEALTHBAR_C3).
        # Needs ESP-IDF sourced (source ~/esp/esp-idf/export.sh) and a MicroPython
        # v1.29.0 checkout at ../micropython (or MICROPYTHON_DIR). If Python errors
        # appear, strip ~/.asdf/shims from PATH before sourcing ESP-IDF's export.sh.
        #
        # Built via idf.py directly, NOT `make BUILD=...`: a BUILD=... assignment
        # on the make command line propagates (via MAKEFLAGS) into every nested
        # `make` invocation in the tree, including the mpy-cross auto-build rule
        # in py/mkrules.cmake (which only clears FROZEN_MANIFEST/USER_C_MODULES
        # there, not BUILD). That makes mpy-cross write its own genhdr into our
        # BUILD dir and corrupts moduledefs.h (empty-mod_defs assert). Driving
        # idf.py -B directly avoids ever setting a make-level BUILD var.
        # ESP-IDF only fills in SDKCONFIG_DEFAULTS keys that are MISSING from an
        # existing build/idf/sdkconfig -- it never overwrites keys already set
        # there. After editing sdkconfig.board or partitions.csv you must
        # `rm -rf firmware-esp32/build/idf` or the change silently won't apply.
        [ -f "{{mpy_dir}}/ports/esp32/Makefile" ] || { echo "no MicroPython at {{mpy_dir}} (set MICROPYTHON_DIR)"; exit 1; }
        [ -n "$IDF_PATH" ] || { echo "ESP-IDF not sourced: source ~/esp/esp-idf/export.sh"; exit 1; }
        case "{{version}}" in
          ''|*[!0-9]*) echo "version must be a non-negative integer"; exit 1 ;;
        esac
        mkdir -p "{{esp32_build}}/gen"
        printf 'FIRMWARE_VERSION = %s\n' "{{version}}" > "{{esp32_build}}/gen/version.py"
        (cd "{{mpy_dir}}/ports/esp32" && idf.py -D MICROPY_BOARD=HEALTHBAR_C3 \
            -D MICROPY_BOARD_DIR="{{esp32_board}}" -B "{{esp32_build}}/idf" build)
        python3 "{{mpy_dir}}/ports/esp32/makeimg.py" \
            "{{esp32_build}}/idf/sdkconfig" \
            "{{esp32_build}}/idf/bootloader/bootloader.bin" \
            "{{esp32_build}}/idf/partition_table/partition-table.bin" \
            "{{esp32_build}}/idf/micropython.bin" \
            "{{esp32_build}}/idf/firmware.bin" \
            "{{esp32_build}}/idf/micropython.uf2"
        APP="{{esp32_build}}/idf/micropython.bin"
        SIZE=$(wc -c < "$APP" | tr -d ' ')
        FREE=$(( {{esp32_slot}} - SIZE ))
        echo "esp32 app: $SIZE bytes, $FREE bytes free in OTA slot"
        [ "$FREE" -ge 131072 ] || { echo "app too big: need >= 128 KiB free in the {{esp32_slot}}-byte OTA slot"; exit 1; } ;;
      *) echo "unknown board: {{board}} (pico|esp32)"; exit 1 ;;
    esac

# flash a USB-connected board (dev; not OTA):  just flash pico | just flash esp32
flash board:
    #!/usr/bin/env sh
    set -e
    case "{{board}}" in
      pico) picotool load "{{uf2}}" -f -x ;;
      esp32)
        # Stock-MicroPython dev flow only: on an OTA (frozen) build, boot.py puts
        # the frozen app ahead of filesystem copies, so copied .py files are ignored.
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

# Pico builds TBYB-flagged so a bad update auto-reverts on the device. version
# is required (no default) so `just publish esp32` can't silently republish or
# downgrade to v1: build an OTA image and publish it:  just publish pico <version>
publish board version:
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
        IMG="{{esp32_build}}/idf/micropython.bin" ;;
      *) echo "unknown board: {{board}} (pico|esp32)"; exit 1 ;;
    esac
    FIRMWARE_DIR="{{fw_root}}" {{publish}} "{{board}}" "$IMG" "{{version}}"

# first-time USB flash of a full image (ERASES the board, including /data):  just flash-full esp32
flash-full board:
    #!/usr/bin/env sh
    set -e
    case "{{board}}" in
      esp32)
        [ -f "{{esp32_build}}/idf/firmware.bin" ] || { echo "no image: run 'just build esp32' first"; exit 1; }
        P="${ESP32_PORT:?set ESP32_PORT, e.g. /dev/cu.usbmodem2133301}"
        esptool --chip esp32c3 --port "$P" erase-flash
        esptool --chip esp32c3 --port "$P" write-flash -z 0x0 "{{esp32_build}}/idf/firmware.bin" ;;
      *) echo "flash-full: only esp32 (use 'just flash pico')"; exit 1 ;;
    esac

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
