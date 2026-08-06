// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------
//
// FRANK Core 2 — master half (U3, RP2350B in QFN-80).
//
// Used by BOARD_VARIANT=C2. The stock pico2 definition declares
// PICO_RP2350A 1, which caps NUM_BANK0_GPIOS at 30 and switches off
// PIO's movable GPIO window. The master drives the inter-processor
// link on GPIO20..43, the status LED on GPIO46 and PSRAM CS on
// GPIO47, so it needs the B-package definition or those pins silently
// alias down into the 0..29 range.

#ifndef _BOARDS_FRANK_CORE2_MASTER_H
#define _BOARDS_FRANK_CORE2_MASTER_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define FRANK_CORE2_MASTER

// --- RP2350 VARIANT ---
// RP2350B: 48 GPIOs, so PIO needs the movable 32-pin window.
#define PICO_RP2350A 0

pico_board_cmake_set_default(PICO_PIO_USE_GPIO_BASE, 1)
#ifndef PICO_PIO_USE_GPIO_BASE
#define PICO_PIO_USE_GPIO_BASE 1
#endif

// --- UART (J2 header: pin 1 = TX, pin 2 = GND, pin 3 = RX) ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED ---
// LD1 is a WS2812B, not a plain GPIO LED; declaring it as
// PICO_DEFAULT_LED_PIN would make SDK helpers drive it as a level.
#ifndef PICO_DEFAULT_WS2812_PIN
#define PICO_DEFAULT_WS2812_PIN 46
#endif

// --- SPI (microSD, J7) ---
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 0
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 6
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 7
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 4
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 5
#endif

// --- FLASH (U1, W25Q128JVPIQ, 16 MB) ---
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
