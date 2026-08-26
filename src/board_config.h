/**
 * frank-386 - i386 PC Emulator for RP2350
 *
 * Board Configuration - supports M1 and M2 board variants with
 * different GPIO layouts for VGA, SD card, PS/2, and I2S audio.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

//#define DIAG 1

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
#if !defined(BOARD_M1) && !defined(BOARD_M2) && !defined(BOARD_PC) && \
    !defined(BOARD_Z2) && !defined(BOARD_C2)
#define BOARD_M1
#endif

/*
 * Input capability flags.
 *
 * Every board except C2 has a PS/2 keyboard and mouse header. C2 has no
 * PS/2 at all — GPIO0/1 are the debug UART (J2) and GPIO2/3 are unrouted
 * — so USB HID is the only input path there. Code that touches the PS/2
 * driver is guarded on BOARD_HAS_PS2 rather than on the board name, so
 * adding a future PS/2-less board does not mean revisiting every call
 * site again.
 */
#ifndef BOARD_C2
#define BOARD_HAS_PS2 1
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

#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

//=============================================================================
// PSRAM Pin Auto-Detection
//=============================================================================

// PSRAM pin for RP2350A variants
#ifdef BOARD_M1
    #define PSRAM_PIN_RP2350A 19
#else
    #ifdef BOARD_Z2 // no RP2350A option, GP47 only
        #define PSRAM_PIN_RP2350A 47
    #else // M2 / C2
        #define PSRAM_PIN_RP2350A 8
    #endif
#endif

// PSRAM pin for RP2350B (always GPIO47)
#define PSRAM_PIN_RP2350B 47

// PSRAM memory size (8MB)
#if EMULATE_LTEMS
    #define PSRAM_SIZE (6 * 1024 * 1024)
#else
    #define PSRAM_SIZE (8 * 1024 * 1024)
#endif
#define PSRAM_BASE 0x11000000

// Aliases for compatibility with psram_init.h
#define PSRAM_SIZE_BYTES PSRAM_SIZE
#ifndef PSRAM_BASE_ADDR
#define PSRAM_BASE_ADDR  PSRAM_BASE
#endif

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

/* Current core voltage, in millivolts, as last programmed by main.c. */
extern volatile uint16_t current_vreg_mv;

static inline char get_rp2350_package_letter(void) {
    uint32_t package_sel = *((io_ro_32*)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    return (package_sel & 1) ? 'A' : 'B';
}

//=============================================================================
// M1 Layout Configuration
//=============================================================================
#ifdef BOARD_M1

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

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

// SPI PSRAM support (old M1 style)
#define PSRAM
//#define PSRAM_MUTEX 1
#define PSRAM_SPINLOCK 1
#define PSRAM_ASYNC 1

#define PSRAM_PIN_CS 18
#define PSRAM_PIN_SCK 19
#define PSRAM_PIN_MOSI 20
#define PSRAM_PIN_MISO 21


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

// NES/SNES Gamepad Pins (directly after HDMI pins)
#define NESPAD_GPIO_CLK   14
#define NESPAD_GPIO_DATA  16
#define NESPAD_GPIO_LATCH 15

// I2S Audio Pins
#define I2S_DATA_PIN       26
#define I2S_CLOCK_PIN_BASE 27

// PWM Audio
#define PWM_RIGHT_PIN 26
#define PWM_LEFT_PIN 27
#define BEEPER_PIN 28

// VGA Pins (directly map to HDMI data pins for VGA resistor DAC mode)
// VGA uses 8 consecutive GPIOs: BBGGRRHS (B=blue, G=green, R=red, H=hsync, S=vsync)
// For VGA mode on M1, we use GPIO 6-13 with different encoding
#define VGA_BASE_PIN HDMI_BASE_PIN

#endif // BOARD_M1

//=============================================================================
// M2 Layout Configuration
//=============================================================================
#ifdef BOARD_M2

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

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

// NES/SNES Gamepad Pins
#define NESPAD_GPIO_CLK   20
#define NESPAD_GPIO_DATA  26
#define NESPAD_GPIO_LATCH 21

// I2S Audio Pins
#define I2S_DATA_PIN       9
#define I2S_CLOCK_PIN_BASE 10

// PWM Audio
#define BEEPER_PIN 9
#define PWM_RIGHT_PIN 10
#define PWM_LEFT_PIN 11

// VGA Base Pin
#define VGA_BASE_PIN HDMI_BASE_PIN

#endif // BOARD_M2

//=============================================================================
// Olimex PICO-PC Layout Configuration
//=============================================================================
#ifdef BOARD_PC

// HDMI Pins
#define HDMI_PIN_CLKN 12

#define HDMI_BASE_PIN HDMI_PIN_CLKN

// SD Card Pins
#define SDCARD_PIN_CLK    6
#define SDCARD_PIN_CMD    7
#define SDCARD_PIN_D0     4
#define SDCARD_PIN_D3     22

#define SDCARD_PIN_SPI0_SCK   SDCARD_PIN_CLK
#define SDCARD_PIN_SPI0_MOSI  SDCARD_PIN_CMD
#define SDCARD_PIN_SPI0_MISO  SDCARD_PIN_D0
#define SDCARD_PIN_SPI0_CS    SDCARD_PIN_D3

/*
        # GP-0  DBG1-1
        # GP-1  DBG1-2

        # GP-5  UXT1-10
        # GP-8  UXT1-6
        # GP-9  UXT1-5
        # GP-21 UXT1-4
        # GP-20 UXT1-3

        # GP-2 QWST1-3
        # GP-3 QWST1-4
*/
// PS/2 Keyboard Pins
#define PS2_PIN_CLK  0
#define PS2_PIN_DATA 1

// NES/SNES Gamepad Pins
#define NESPAD_GPIO_CLK   5  // UXT1-10
#define NESPAD_GPIO_DATA  20 // UXT1-3
#define NESPAD_GPIO_LATCH 9  // UXT1-5

// PS/2 Mouse Pins (if available)
#define PS2_MOUSE_CLK  2
#define PS2_MOUSE_DATA 3

#define PWM_RIGHT_PIN 27
#define PWM_LEFT_PIN 28

#endif // BOARD_PC

//=============================================================================
// Waveshare RP2350-PiZero Layout Configuration
//=============================================================================
#ifdef BOARD_Z2

// HDMI Pins
#define HDMI_PIN_CLKN 32

#define HDMI_BASE_PIN HDMI_PIN_CLKN

// SD Card Pins
#define SDCARD_PIN_CLK    30
#define SDCARD_PIN_CMD    31
#define SDCARD_PIN_D0     40
#define SDCARD_PIN_D3     43

#define SDCARD_PIN_SPI0_SCK   SDCARD_PIN_CLK
#define SDCARD_PIN_SPI0_MOSI  SDCARD_PIN_CMD
#define SDCARD_PIN_SPI0_MISO  SDCARD_PIN_D0
#define SDCARD_PIN_SPI0_CS    SDCARD_PIN_D3

// PS/2 Keyboard Pins
#define PS2_PIN_CLK  2
#define PS2_PIN_DATA 3

// NES/SNES Gamepad Pins
#define NESPAD_GPIO_CLK   4
#define NESPAD_GPIO_DATA  6
#define NESPAD_GPIO_LATCH 7

// PS/2 Mouse Pins (if available)
#define PS2_MOUSE_CLK  0
#define PS2_MOUSE_DATA 1

// I2S Audio Pins
#define I2S_DATA_PIN       10
#define I2S_CLOCK_PIN_BASE 11

#define PWM_RIGHT_PIN 10
#define PWM_LEFT_PIN 11
#define BEEPER_PIN 12

#endif // BOARD_PC

//=============================================================================
// FRANK Core 2 Layout Configuration (dual RP2350)
//=============================================================================
/*
 * C2 is the master half (U3, RP2350B) of the FRANK Core 2 / Core 2U
 * board. Every assignment below comes from the KiCad netlist via
 * frank_core2/firmware/common/frank_core2_board.h.
 *
 * The HDMI, microSD and I2S pins happen to be identical to M2, so those
 * paths need no new code. What differs:
 *
 *   - No PS/2 and no NES pad. USB HID is the only input.
 *   - No analog VGA DAC; HDMI is forced (see FORCE_HDMI in CMakeLists).
 *   - LD1 is a WS2812B on GPIO46, not a plain LED.
 *   - GPIO20..42 belong to the inter-processor link and must not be
 *     claimed by anything else. That is why the M2 NES pad pins (20, 21,
 *     26) and PWM audio pins are absent here.
 */
#ifdef BOARD_C2

// HDMI Pins (J5) — same layout as M2
#define HDMI_PIN_CLKN 12
#define HDMI_PIN_CLKP 13
#define HDMI_PIN_D0N  14
#define HDMI_PIN_D0P  15
#define HDMI_PIN_D1N  16
#define HDMI_PIN_D1P  17
#define HDMI_PIN_D2N  18
#define HDMI_PIN_D2P  19

#define HDMI_BASE_PIN HDMI_PIN_CLKN

// microSD (J7) on SPI0 — SDIO pin names from the schematic in comments
#define SDCARD_PIN_CLK    6   // SD CLK
#define SDCARD_PIN_CMD    7   // SD CMD
#define SDCARD_PIN_D0     4   // SD DAT0
#define SDCARD_PIN_D3     5   // SD DAT3/CD

#define SDCARD_PIN_SPI0_SCK   SDCARD_PIN_CLK
#define SDCARD_PIN_SPI0_MOSI  SDCARD_PIN_CMD
#define SDCARD_PIN_SPI0_MISO  SDCARD_PIN_D0
#define SDCARD_PIN_SPI0_CS    SDCARD_PIN_D3

// TDA1387T I2S DAC (U8): DATA = GPIO9, SCLK = GPIO10, LRCK = GPIO11
#define I2S_DATA_PIN       9
#define I2S_CLOCK_PIN_BASE 10

/* VGA is not populated on this board. vga_hw.c still references
 * VGA_BASE_PIN at compile time, but every use is behind SELECT_VGA,
 * which FORCE_HDMI pins to false. Alias it to the HDMI base so the
 * driver compiles without a C2-specific #ifdef in it. */
#define VGA_BASE_PIN HDMI_BASE_PIN

/* Status LED LD1 is a WS2812B via 330R (R11). Deliberately NOT
 * PICO_DEFAULT_LED_PIN — SDK helpers would drive it as a level. */
#define M_LED_WS2812_PIN 46

/*
 * Inter-processor link pins (see boards/frank_core2_master.h and
 * frank_core2/firmware/common/frank_core2_board.h).
 *
 * Two 8-bit source-synchronous buses plus three SIO control wires.
 * Nothing else may claim GPIO20..42.
 */
#define M_LINK_A_DATA_BASE   20   /* GPIO20..27, master -> slave (TX) */
#define M_LINK_A_CLK         28   /* == DATA_BASE + 8 */
#define M_LINK_A_VALID       29   /* == DATA_BASE + 9 */

#define M_LINK_B_DATA_BASE   30   /* GPIO30..37, slave -> master (RX) */
#define M_LINK_B_CLK         38
#define M_LINK_B_VALID       39

#define M_LINK_FS            40   /* frame sync / reset request, out */
#define M_LINK_DB_OUT        41   /* DB_MS, out */
#define M_LINK_DB_IN         42   /* DB_SM, in  */

/*
 * PIO instance for the link.
 *
 * Link bus B lands on GPIO30..39, so the instance needs
 * pio_set_gpio_base(16). That is a per-instance setting, and it makes
 * every pin below 16 unreachable on that instance — so the link cannot
 * share with anything down there.
 *
 * PIO2 is always the I2S DAC (audio.c hardcodes pio2, GPIO9..11). The
 * video path takes a different instance depending on the output:
 *
 *   HDMI build — hdmi.c uses PIO_VIDEO = pio1 (GPIO12..19) => link on PIO0
 *   VGA  build — vga_hw.h defines VGA_PIO = pio0 (GPIO12..19) => link on PIO1
 *
 * Getting this wrong is quiet: pio_sm_init() rejects the configuration
 * with PICO_ERROR_BAD_ALIGNMENT and the link simply never answers, which
 * reads as a dead slave rather than a PIO clash.
 */
#ifdef FORCE_VGA
#define LINK_PIO_MASTER      pio1
#else
#define LINK_PIO_MASTER      pio0
#endif
#define LINK_PIO_GPIO_BASE   16

#endif // BOARD_C2

//=============================================================================
// Common PIO Assignments
//=============================================================================

/* Z2 uses the same PIO assignment as the known-good ZERO2 HDMI path:
 * PIO0 drives GPIO32..39 with gpio_base=16, while PS/2 stays on a separate
 * PIO instance whose gpio_base remains 0.  Other boards keep the existing
 * allocation. */
#ifdef BOARD_Z2
#define PIO_VIDEO       pio0
#define PIO_VIDEO_ADDR  pio0
#define PIO_PS2KBD      pio1
#else
#define PIO_VIDEO       pio1
#define PIO_VIDEO_ADDR  pio1
#define PIO_PS2KBD      pio0
#endif

// SD Card PIO (if using PIO SPI)
#define PIO_SDCARD      pio1

// DMA IRQ assignments
#define VIDEO_DMA_IRQ   DMA_IRQ_0

//=============================================================================
// HDMI Configuration
//=============================================================================

/* HDMI electrical lane order is board-specific and is defined in
 * drivers/hdmi/hdmi.h.  In particular, Z0/Z2 route TMDS data on the first
 * three pairs and TMDS clock on the last pair, unlike M1/M2. */

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

// SD Card SPI bus. Waveshare RP2350-PiZero routes GPIO30/31/40 to SPI1.
#ifdef BOARD_Z2
#define SDCARD_SPI_BUS spi1
#else
#define SDCARD_SPI_BUS spi0
#endif

/*
 * Enable PIO-based SD card for better performance.
 *
 * Not on C2: sdcard.c hardcodes pio1 for its SPI program, and on that
 * board every PIO instance is spoken for. With VGA selected the video
 * path takes pio0 and the I2S DAC takes pio2, so a PIO-based SD card
 * leaves nothing for the inter-processor link — pio_set_gpio_base()
 * then fails with PICO_ERROR_INVALID_STATE and the link silently never
 * answers, which reads as a dead slave.
 *
 * Nothing is lost by dropping it here. The C2 microSD sits on GPIO4..7,
 * which are exactly the hardware SPI0 pins (RX/CSn/SCK/TX), so the
 * non-PIO path in sdcard.c drives it directly.
 */
#if !defined(BOARD_C2) && !defined(BOARD_Z2)
#define SDCARD_PIO 1
#endif

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


/* SD-card data directory (BIOS/config/disk images). Normally supplied by the
   build as -DSD_DATA_DIR / -DSD_DATA_DIR_SLASH, tied to CPU_TARGET (286 vs 386
   need different files). These fallbacks only apply to a non-CMake compile. */
#ifndef SD_DATA_DIR
#define SD_DATA_DIR "386"
#endif
#ifndef SD_DATA_DIR_SLASH
#define SD_DATA_DIR_SLASH SD_DATA_DIR "/"
#endif

#endif // BOARD_CONFIG_H
