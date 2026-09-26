// This configuration is for the D&D Health Bar HEALTHBAR_C3 board (Seeed XIAO
// ESP32-C3, 4 MiB flash), derived from the upstream ESP32_GENERIC_C3 board.

#ifndef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME               "D&D Health Bar (XIAO ESP32-C3)"
#endif
#define MICROPY_HW_MCU_NAME                 "ESP32C3"

// Enable UART REPL for modules that have an external USB-UART and don't use native USB.
#define MICROPY_HW_ENABLE_UART_REPL         (1)

// No Bluetooth on this board: frees flash/RAM so the app fits one OTA slot.
#define MICROPY_PY_BLUETOOTH                (0)
