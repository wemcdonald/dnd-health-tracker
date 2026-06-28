cmake_minimum_required(VERSION 3.17)

# Board must be set BEFORE pico_sdk_init().
# pico2_w selects RP2350 + cyw43 radio support.
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Pico SDK (must be before project()).
include(pico_sdk_import.cmake)

project(healthbar C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialise the Pico SDK.
pico_sdk_init()

# ─── M0: blink + dual stdio ──────────────────────────────────────────────────
add_executable(m0_blink
    src/m0_blink.c
)

target_include_directories(m0_blink PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(m0_blink
    pico_stdlib
    pico_cyw43_arch_none        # cyw43 driver without lwIP (just for LED GPIO)
)

pico_enable_stdio_usb(m0_blink 1)   # USB-CDC printf
pico_enable_stdio_uart(m0_blink 1)  # UART printf (independent of USB; use picoprobe)
pico_add_extra_outputs(m0_blink)    # produces .uf2 and .elf

# ─── M5 bring-up: standalone WS2812 test (no cyw43, no wifi, no core1) ─────────
add_executable(m5_ledtest
    src/m5_ledtest.c
)
pico_generate_pio_header(m5_ledtest ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)
target_include_directories(m5_ledtest PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_link_libraries(m5_ledtest
    pico_stdlib
    hardware_pio
)
pico_enable_stdio_usb(m5_ledtest 1)
pico_enable_stdio_uart(m5_ledtest 1)
pico_add_extra_outputs(m5_ledtest)

# Firmware version for OTA version gating. Pass -DFIRMWARE_VERSION=N on the cmake line.
if(NOT DEFINED FIRMWARE_VERSION)
    set(FIRMWARE_VERSION 1)
endif()

# ─── M1: captive portal ──────────────────────────────────────────────────────
add_executable(m1_portal
    src/portal.c
    src/config.c
    src/config_choose.c
    src/http_poll.c
    src/leds.c
    src/statusd.c
    src/configform.c
    src/provision.c
    src/dhcpserver.c
    src/dnsserver.c
    src/sha256.c
    src/ota_manifest.c
    src/ota_sink.c
    src/ota_flash.c
    src/ota.c
)

pico_generate_pio_header(m1_portal ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

target_include_directories(m1_portal PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}           # lwipopts.h lives here
    ${CMAKE_CURRENT_LIST_DIR}/src       # dhcpserver.h, dnsserver.h
)

# Configure AP IP address via the pico-sdk helper.
pico_configure_ip4_address(m1_portal PRIVATE
    CYW43_DEFAULT_IP_AP_ADDRESS 192.168.4.1
)

target_link_libraries(m1_portal
    pico_cyw43_arch_lwip_threadsafe_background
    pico_stdlib
    pico_multicore
    pico_lwip_mdns
    pico_bootrom
    hardware_flash
    hardware_sync
    hardware_pio
)

# Setup-AP name suffix. Build with -DHEALTHBAR_NAME=shen -> SSID "healthbar-setup-shen".
# Empty -> SSID is just "healthbar-setup".
set(HEALTHBAR_NAME "" CACHE STRING "Setup-AP suffix; SSID becomes healthbar-setup-<name>")
target_compile_definitions(m1_portal PRIVATE HEALTHBAR_NAME="${HEALTHBAR_NAME}")

# Poll target. Defaults to production; override for local testing, e.g.
# -DPOLL_HOST=10.0.10.123 -DPOLL_PORT=8080
set(POLL_HOST "dndhealth.willflix.org" CACHE STRING "Server host the device polls")
set(POLL_PORT "80" CACHE STRING "Server port the device polls")
target_compile_definitions(m1_portal PRIVATE POLL_HOST="${POLL_HOST}" POLL_PORT=${POLL_PORT})
target_compile_definitions(m1_portal PRIVATE FIRMWARE_VERSION=${FIRMWARE_VERSION})
# Embed the same version into the image-def so the bootrom A/B picker agrees with us.
pico_set_program_version(m1_portal "${FIRMWARE_VERSION}.0")

# STA-mode status web server + mDNS (healthbar-<slug>.local). Opt-in: it pulls in
# the lwIP mDNS responder. Build -DENABLE_STATUSD=OFF for the minimal/safe firmware.
option(ENABLE_STATUSD "Status web server + mDNS in STA mode" ON)
if(ENABLE_STATUSD)
    target_compile_definitions(m1_portal PRIVATE ENABLE_STATUSD)
endif()

# DEV-ONLY provisioning seed (for bring-up without a phone). Never commit creds;
# pass on the command line: -DDEV_SEED_CONFIG=ON -DDEV_SEED_SSID=.. -DDEV_SEED_PSK=.. -DDEV_SEED_SLUG=..
option(DEV_SEED_CONFIG "Seed a wifi config on first boot from build-time creds" OFF)
if(DEV_SEED_CONFIG)
    target_compile_definitions(m1_portal PRIVATE
        DEV_SEED_CONFIG
        DEV_SEED_SSID="${DEV_SEED_SSID}"
        DEV_SEED_PSK="${DEV_SEED_PSK}"
        DEV_SEED_SLUG="${DEV_SEED_SLUG}")
endif()

pico_enable_stdio_usb(m1_portal 1)
pico_enable_stdio_uart(m1_portal 1)
pico_add_extra_outputs(m1_portal)

# ─── SPIKE (Task 1, THROWAWAY): RP2350 A/B + TBYB proof ───────────────────────
# Build with -DFIRMWARE_VERSION=1 then =2. Removed at end of Task 1.
# (FIRMWARE_VERSION default is set above, before m1_portal.)
if(NOT DEFINED SPIKE_BUY)
    set(SPIKE_BUY 0)
endif()
add_executable(m6_tbyb src/m6_tbyb.c)
target_compile_definitions(m6_tbyb PRIVATE FIRMWARE_VERSION=${FIRMWARE_VERSION} SPIKE_BUY=${SPIKE_BUY})
target_link_libraries(m6_tbyb pico_stdlib pico_bootrom hardware_watchdog)
# Embed the version into the image-def so the bootrom A/B picker agrees with us.
pico_set_program_version(m6_tbyb "${FIRMWARE_VERSION}.0")
pico_enable_stdio_usb(m6_tbyb 1)
pico_enable_stdio_uart(m6_tbyb 1)
pico_add_extra_outputs(m6_tbyb)
