# D&D Health Tracker - device tasks. Run `just` to list.
#
#   just build                build the generic firmware
#   just deploy               flash over USB (no BOOTSEL)
#   just set name shen        set character slug
#   just set wifi SSID 'pw'   set wifi networks (repeat pairs; priority = order)
#
# View current config at the device's web page (healthbar-<slug>.local).
# The firmware is generic; identity (name/wifi) is provisioned at runtime, so you
# build once and provision per device.

set positional-arguments

pico_sdk  := env_var_or_default("PICO_SDK_PATH", justfile_directory() / ".." / "pico-sdk")
build_dir := justfile_directory() / "firmware-c" / "build"
uf2       := build_dir / "m1_portal.uf2"
provision := "node " + justfile_directory() / "tools" / "provision.mjs"

# list targets
default:
    @just --list

# build the generic firmware
build version="1":
    #!/usr/bin/env sh
    # Strip 'version=' prefix if just passes the arg as a positional (set positional-arguments quirk).
    arg="${1:-1}"; FW_VERSION="${arg#version=}"
    (cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake -B build -DPICO_BOARD=pico2_w \
        -DPOLL_HOST=dndhealth.willflix.org -DPOLL_PORT=80 -DENABLE_STATUSD=ON \
        -DHEALTHBAR_NAME= -DDEV_SEED_CONFIG=OFF -DFIRMWARE_VERSION="$FW_VERSION" -DOTA_TBYB=OFF)
    (cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake --build build -j4 --target m1_portal)

# flash over USB (no BOOTSEL)
deploy:
    picotool load "{{uf2}}" -f -x

# provision config over USB (name/wifi)
set *args:
    {{provision}} "$@"

# build a TBYB-flagged OTA image and publish it as the new "latest" (server reads server/firmware).
# OTA_TBYB=ON makes a bad update auto-revert on the device (spike-proven requirement).
publish-fw version="1":
    #!/usr/bin/env sh
    set -e
    arg="${1:-1}"; FW_VERSION="${arg#version=}"
    (cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake -B build -DPICO_BOARD=pico2_w \
        -DPOLL_HOST=dndhealth.willflix.org -DPOLL_PORT=80 -DENABLE_STATUSD=ON \
        -DHEALTHBAR_NAME= -DDEV_SEED_CONFIG=OFF -DFIRMWARE_VERSION="$FW_VERSION" -DOTA_TBYB=ON)
    (cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake --build build -j4 --target m1_portal)
    FIRMWARE_DIR="{{justfile_directory()}}/server/firmware" \
        node "{{justfile_directory()}}/server/tools/publish-fw.mjs" "{{build_dir}}/m1_portal.bin"

# run host-compiled C unit tests (no hardware)
test-host:
    bash firmware-c/test/run.sh

# ONE-TIME: convert a device from the single-image layout to the A/B OTA layout.
# Device must be USB-connected in BOOTSEL. Config auto-migrates from the legacy
# last-sector on first boot. Back up the device's wifi/slug first (open its status
# page at healthbar-<slug>.local) — full runbook + recovery in firmware-c/ota/MIGRATION.md.
migrate-ota version="1":
    #!/usr/bin/env sh
    set -e
    # Strip 'version=' prefix if just passes the arg positionally (set positional-arguments quirk).
    arg="${1:-1}"; FW_VERSION="${arg#version=}"
    just build version="$FW_VERSION"                       # build the app first (no device needed)
    cd firmware-c
    picotool partition create ota/partition_table.json build/pt.uf2
    picotool load build/pt.uf2 -f                          # write the A/B partition table
    picotool reboot -u && sleep 2                          # reboot so the bootrom registers the PT
    picotool load build/m1_portal.uf2 -p 0 -f -x           # flash slot A (partition index 0) and run
