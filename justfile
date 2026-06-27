# D&D Health Tracker - device tasks. Run `just` to list.
#
#   just build                build the generic firmware
#   just deploy               flash over USB (no BOOTSEL)
#   just set name shen        set character slug
#   just set wifi SSID 'pw'   set wifi networks (repeat pairs; priority = order)
#   just show                 print on-device config (no passwords)
#
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
build:
    cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake -B build -DPICO_BOARD=pico2_w \
        -DPOLL_HOST=dndhealth.willflix.org -DPOLL_PORT=80 -DENABLE_STATUSD=ON \
        -DHEALTHBAR_NAME= -DDEV_SEED_CONFIG=OFF
    cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake --build build -j4 --target m1_portal

# flash over USB (no BOOTSEL)
deploy:
    picotool load "{{uf2}}" -f -x

# provision config over USB (name/wifi)
set *args:
    {{provision}} "$@"

# print on-device config
show:
    {{provision}} show
