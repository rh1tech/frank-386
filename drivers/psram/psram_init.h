/**
 * PSRAM Driver for RP2350
 *
 * Initializes QSPI PSRAM on CS1 for use as external memory.
 * PSRAM is memory-mapped at XIP_SRAM_BASE (0x11000000).
 */

#ifndef PSRAM_INIT_H
#define PSRAM_INIT_H

#include "pico/stdlib.h"
#include "board_config.h"
#include <stdbool.h>

// PSRAM memory map (may be defined by board_config.h)
#ifndef PSRAM_BASE_ADDR
#define PSRAM_BASE_ADDR   0x11000000
#endif
#ifndef PSRAM_SIZE_BYTES
#define PSRAM_SIZE_BYTES  (4 * 1024 * 1024)  // 4 MB
#endif

/**
 * Initialize PSRAM on the specified CS pin.
 * This function must be called from RAM (not flash) as it reconfigures XIP.
 *
 * @param cs_pin GPIO pin connected to PSRAM CS (auto-detected via get_psram_pin())
 */
void psram_init(uint cs_pin);

/**
 * Initialize PSRAM with a specific frequency.
 * This function must be called from RAM (not flash) as it reconfigures XIP.
 *
 * @param cs_pin GPIO pin connected to PSRAM CS
 * @param freq_mhz Target PSRAM frequency in MHz (e.g., 133, 166)
 */
void psram_init_with_freq(uint cs_pin, int freq_mhz);

/**
 * Re-time the PSRAM for a system clock that has not been applied yet.
 *
 * Call this immediately before set_sys_clock_khz() raises clk_sys, the way
 * set_flash_timings() is called: psram_init_with_freq() reads the live
 * clk_sys, so on its own it always leaves the PSRAM overclocked for the whole
 * window between the PLL moving and the re-init landing.  When lowering the
 * clock the old divisor is only conservative, so re-time afterwards instead.
 *
 * Requires PSRAM to already be initialised - it performs the dummy access the
 * QMI needs to latch the new divisor.
 *
 * @param cpu_mhz   system clock the PSRAM must be timed for, in MHz
 * @param psram_mhz target PSRAM frequency in MHz
 */
void psram_set_timings(int cpu_mhz, int psram_mhz);

/**
 * Test PSRAM functionality.
 * Performs a simple read/write test.
 *
 * @return true if PSRAM is working correctly
 */
bool psram_test(void);

/**
 * Get pointer to PSRAM memory region.
 *
 * @return Pointer to start of PSRAM memory
 */
static inline void *psram_get_ptr(void) {
    return (void *)PSRAM_BASE_ADDR;
}

/**
 * Get PSRAM size in bytes.
 *
 * @return Size of PSRAM in bytes
 */
static inline size_t psram_get_size(void) {
    /// TODO: implment real chip size detection
    return PSRAM_SIZE_BYTES;
}

#endif // PSRAM_INIT_H
