set(IDF_TARGET esp32c3)

# Partition table lives in this board dir; sdkconfig can't expand CMake vars,
# so write a one-line fragment with the absolute path at configure time.
set(HB_PART_FRAGMENT ${CMAKE_BINARY_DIR}/sdkconfig.healthbar_partitions)
file(WRITE ${HB_PART_FRAGMENT}
     "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${MICROPY_BOARD_DIR}/partitions.csv\"\n")

# Same as boards/mpconfigboard_esp32c3_common.cmake, minus boards/sdkconfig.ble
# (no Bluetooth on this board -- see sdkconfig.board), plus our fragments.
set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.riscv
    boards/sdkconfig.c3
    ${MICROPY_BOARD_DIR}/sdkconfig.board
    ${HB_PART_FRAGMENT}
)

set(MICROPY_FROZEN_MANIFEST ${MICROPY_BOARD_DIR}/../../frozen_manifest.py)
