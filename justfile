# D&D Health Tracker — device tasks
#
#   just build                         build the generic firmware
#   just deploy                        flash over USB (no BOOTSEL)
#   just set name shen                 set character slug (-> healthbar-shen.local, /dnd/shen.txt)
#   just set wifi SSID 'pass' [SSID2 'pass2' ...]   set wifi networks (priority = order)
#   just show                          print on-device config (no passwords)
#
# Identity (name/wifi) is provisioned over USB-serial at runtime, so the firmware
# is generic — build once, provision per device.

set positional-arguments

pico_sdk  := env_var_or_default("PICO_SDK_PATH", justfile_directory() / ".." / "pico-sdk")
build_dir := justfile_directory() / "firmware-c" / "build"
uf2       := build_dir / "m1_portal.uf2"
provision := "node " + justfile_directory() / "tools" / "provision.mjs"

# list available targets
default:
    @just --list

# build the generic firmware (statusd + mDNS + watchdog; no baked identity)
build:
    cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake -B build -DPICO_BOARD=pico2_w \
        -DPOLL_HOST=public.willflix.com -DPOLL_PORT=80 -DENABLE_STATUSD=ON \
        -DHEALTHBAR_NAME= -DDEV_SEED_CONFIG=OFF
    cd firmware-c && PICO_SDK_PATH={{pico_sdk}} cmake --build build -j4 --target m1_portal

# flash the firmware over USB — no BOOTSEL (picotool reboots the running app into the loader)
deploy:
    picotool load "{{uf2}}" -f -x

# provision over USB-serial: `just set name shen` or `just set wifi SSID 'pass' [...]`
set *args:
    {{provision}} "$@"

# print the device's current config (no passwords)
show:
    {{provision}} show
