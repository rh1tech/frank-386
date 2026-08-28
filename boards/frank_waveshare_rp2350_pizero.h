// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------
//
// Waveshare RP2350-PiZero board definition for frank-386.
//
// The board uses RP2350B (48 GPIOs), 16 MiB W25Q128 flash and places
// HDMI, microSD and optional PSRAM on GPIOs above 29.  It therefore
// MUST NOT be built with the stock "pico2" (RP2350A) board definition.

#ifndef _BOARDS_FRANK_WAVESHARE_RP2350_PIZERO_H
#define _BOARDS_FRANK_WAVESHARE_RP2350_PIZERO_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define FRANK_WAVESHARE_RP2350_PIZERO

// --- RP2350 VARIANT ---
#define PICO_RP2350A 0

// RP2350B has 48 GPIOs. frank-386 Z2 HDMI lives on GPIO32..39, so PIO
// needs the movable 32-pin GPIO window (base 16).
pico_board_cmake_set_default(PICO_PIO_USE_GPIO_BASE, 1)
#ifndef PICO_PIO_USE_GPIO_BASE
#define PICO_PIO_USE_GPIO_BASE 1
#endif

// --- UART ---
// Same defaults as the current upstream Pico SDK Waveshare board header.
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 1
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 4
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 5
#endif

// --- FLASH ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
