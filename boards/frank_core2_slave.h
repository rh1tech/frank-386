// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------
//
// FRANK Core 2 — slave half (U6, RP2350 in QFN-60).
//
// Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
// https://github.com/rh1tech/frank
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The A package, so the stock 30-GPIO layout applies and PIO reaches
// every pin without a base window. Every link pin the slave uses sits
// between GPIO0 and GPIO23.

#ifndef _BOARDS_FRANK_CORE2_SLAVE_H
#define _BOARDS_FRANK_CORE2_SLAVE_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define FRANK_CORE2_SLAVE

// --- RP2350 VARIANT ---
#define PICO_RP2350A 1

// --- UART (J4 header: pin 1 = TX, pin 2 = GND, pin 3 = RX) ---
// GPIO24/25 is UART1. GPIO0 — where the SDK would otherwise put uart0
// TX — is the PSRAM chip select on this half, so the default must move.
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 1
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 24
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 25
#endif

// --- LED (LD2, blue, via 1K R19) ---
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 26
#endif

// --- FLASH (U4, W25Q128JVPIQ, 16 MB) ---
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
