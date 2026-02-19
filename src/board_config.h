/**
 * murm386 - i386 PC Emulator for RP2350
 *
 * Board Configuration - supports M1 and M2 board variants with
 * different GPIO layouts for VGA, SD card, PS/2, and I2S audio.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"

/*
 * Board Configuration Variants:
 *
 * BOARD_M1 - M1 GPIO layout
 * BOARD_M2 - M2 GPIO layout
 *
 * PSRAM pin is auto-detected based on chip package:
 *   RP2350B: GPIO47 (for both M1 and M2)
 *   RP2350A: GPIO19 (M1) or GPIO8 (M2)
 *
 * M1 GPIO Layout:
 *   HDMI: CLKN=6, CLKP=7, D0N=8, D0P=9, D1N=10, D1P=11, D2N=12, D2P=13
 *   SD:   CLK=2, CMD=3, DAT0=4, DAT3=5
 *   PS/2: CLK=0, DATA=1
 *   I2S:  DATA=26, CLK=27, LRCK=28
 *
 * M2 GPIO Layout:
 *   HDMI: CLKN=12, CLKP=13, D0N=14, D0P=15, D1N=16, D1P=17, D2N=18, D2P=19
 *   SD:   CLK=6, CMD=7, DAT0=4, DAT3=5
 *   PS/2: CLK=2, DATA=3
 *   I2S:  DATA=9, CLK=10, LRCK=11
 *
 * CPU/PSRAM Speed (set via CMake -DCPU_SPEED=xxx -DPSRAM_SPEED=xxx):
 *   252 MHz - no overclock (default for stable operation)
 *   378 MHz - medium overclock
 *   504 MHz - high overclock
 */

// Default to M1 if no config specified
#if !defined(BOARD_M1) && !defined(BOARD_M2)
#define BOARD_M1
#endif

//=============================================================================
// CPU/PSRAM Speed Defaults (can be overridden via CMake)
//=============================================================================
#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 252
#endif

#ifndef CPU_VOLTAGE
#define CPU_VOLTAGE VREG_VOLTAGE_1_50
#endif

#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 133
#endif

//=============================================================================
// PSRAM Pin Auto-Detection
//=============================================================================

// PSRAM pin for RP2350A variants
#ifdef BOARD_M1
#define PSRAM_PIN_RP2350A 19
#else
#define PSRAM_PIN_RP2350A 8
#endif

// PSRAM pin for RP2350B (always GPIO47)
#define PSRAM_PIN_RP2350B 47

// PSRAM memory size (8MB)
#define PSRAM_SIZE (8 * 1024 * 1024)
#define PSRAM_BASE 0x11000000

// Aliases for compatibility with psram_init.h
#define PSRAM_SIZE_BYTES PSRAM_SIZE
#define PSRAM_BASE_ADDR  PSRAM_BASE

// Runtime function to get PSRAM pin based on chip package
static inline uint get_psram_pin(void) {
    // Check if RP2350A (bit 0 set) or RP2350B (bit 0 clear)
    uint32_t package_sel = *((io_ro_32*)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    if (package_sel & 1) {
        // RP2350A - use board-specific pin
        return PSRAM_PIN_RP2350A;
    } else {
        // RP2350B - always GPIO47
        return PSRAM_PIN_RP2350B;
    }
}

//=============================================================================
// M1 Layout Configuration
//=============================================================================
#ifdef BOARD_M1

// HDMI Pins
#define HDMI_PIN_CLKN 6
#define HDMI_PIN_CLKP 7
#define HDMI_PIN_D0N  8
#define HDMI_PIN_D0P  9
#define HDMI_PIN_D1N  10
#define HDMI_PIN_D1P  11
#define HDMI_PIN_D2N  12
#define HDMI_PIN_D2P  13

#define HDMI_BASE_PIN HDMI_PIN_CLKN

// SD Card Pins (directly define for both naming conventions)
#define SDCARD_PIN_CLK    2
#define SDCARD_PIN_CMD    3
#define SDCARD_PIN_D0     4
#define SDCARD_PIN_D3     5

// SD Card pin aliases for sdcard.c
#define SDCARD_PIN_SPI0_SCK   SDCARD_PIN_CLK
#define SDCARD_PIN_SPI0_MOSI  SDCARD_PIN_CMD
#define SDCARD_PIN_SPI0_MISO  SDCARD_PIN_D0
#define SDCARD_PIN_SPI0_CS    SDCARD_PIN_D3

// PS/2 Keyboard Pins
#define PS2_PIN_CLK  0
#define PS2_PIN_DATA 1

// PS/2 Mouse Pins (if available)
#define PS2_MOUSE_CLK  14
#define PS2_MOUSE_DATA 15

// I2S Audio Pins
#define I2S_DATA_PIN       26
#define I2S_CLOCK_PIN_BASE 27

// VGA Pins (directly map to HDMI data pins for VGA resistor DAC mode)
// VGA uses 8 consecutive GPIOs: BBGGRRHS (B=blue, G=green, R=red, H=hsync, S=vsync)
// For VGA mode on M1, we use GPIO 6-13 with different encoding
#define VGA_BASE_PIN HDMI_BASE_PIN

#endif // BOARD_M1

//=============================================================================
// M2 Layout Configuration
//=============================================================================
#ifdef BOARD_M2

// HDMI Pins
#define HDMI_PIN_CLKN 12
#define HDMI_PIN_CLKP 13
#define HDMI_PIN_D0N  14
#define HDMI_PIN_D0P  15
#define HDMI_PIN_D1N  16
#define HDMI_PIN_D1P  17
#define HDMI_PIN_D2N  18
#define HDMI_PIN_D2P  19

#define HDMI_BASE_PIN HDMI_PIN_CLKN

// SD Card Pins
#define SDCARD_PIN_CLK    6
#define SDCARD_PIN_CMD    7
#define SDCARD_PIN_D0     4
#define SDCARD_PIN_D3     5

// SD Card pin aliases for sdcard.c
#define SDCARD_PIN_SPI0_SCK   SDCARD_PIN_CLK
#define SDCARD_PIN_SPI0_MOSI  SDCARD_PIN_CMD
#define SDCARD_PIN_SPI0_MISO  SDCARD_PIN_D0
#define SDCARD_PIN_SPI0_CS    SDCARD_PIN_D3

// PS/2 Keyboard Pins
#define PS2_PIN_CLK  2
#define PS2_PIN_DATA 3

// PS/2 Mouse Pins
#define PS2_MOUSE_CLK  0
#define PS2_MOUSE_DATA 1

// I2S Audio Pins
#define I2S_DATA_PIN       9
#define I2S_CLOCK_PIN_BASE 10

// VGA Base Pin
#define VGA_BASE_PIN HDMI_BASE_PIN

#endif // BOARD_M2

//=============================================================================
// Common PIO Assignments
//=============================================================================

// Video output uses PIO1
#define PIO_VIDEO       pio1
#define PIO_VIDEO_ADDR  pio1

// PS/2 Keyboard uses PIO0
#define PIO_PS2KBD      pio0

// SD Card PIO (if using PIO SPI)
#define PIO_SDCARD      pio1

// DMA IRQ assignments
#define VIDEO_DMA_IRQ   DMA_IRQ_0

//=============================================================================
// HDMI Configuration
//=============================================================================

// HDMI differential pair encoding options
#define HDMI_PIN_RGB_notBGR       1
#define HDMI_PIN_invert_diffpairs 1

// HDMI clock pins (relative to base)
#define beginHDMI_PIN_clk   HDMI_BASE_PIN
#define beginHDMI_PIN_data  (HDMI_BASE_PIN + 2)

//=============================================================================
// VGA Display Configuration
//=============================================================================

// VGA horizontal shift (where active video starts)
// Adjust this value to center the display on your monitor
// Default: 106 (tuned for typical monitor centering)
#ifndef VGA_SHIFT_PICTURE
#define VGA_SHIFT_PICTURE 144
#endif

//=============================================================================
// Emulator Memory Configuration
//=============================================================================

// Main memory size (configurable, limited by 8MB PSRAM minus VGA memory)
#ifndef EMU_MEM_SIZE_MB
#define EMU_MEM_SIZE_MB 7
#endif

// VGA memory size (up to 2MB)
#ifndef EMU_VGA_MEM_SIZE_KB
#define EMU_VGA_MEM_SIZE_KB 256
#endif

// CPU generation (3=386, 4=486, 5=586)
#ifndef EMU_CPU_GEN
#define EMU_CPU_GEN 4
#endif

//=============================================================================
// SD Card Configuration
//=============================================================================

// SD Card SPI bus (use hardware SPI0 or PIO)
#define SDCARD_SPI_BUS spi0

// Enable PIO-based SD card for better performance
#define SDCARD_PIO 1

//=============================================================================
// Debug Configuration
//=============================================================================

// USB Serial console delay at startup (milliseconds)
#define USB_CONSOLE_DELAY_MS 5000

// Enable debug output
#ifdef DEBUG
#define DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINTF(...)
#endif

#endif // BOARD_CONFIG_H
