/**
 * PSRAM Driver for RP2350
 *
 * Initializes QSPI PSRAM on CS1 for use as external memory.
 * PSRAM is memory-mapped at XIP_SRAM_BASE (0x11000000).
 */

#ifndef PSRAM_INIT_H
#define PSRAM_INIT_H

#include "pico/stdlib.h"
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
 * Test PSRAM functionality.
 * Performs a simple read/write test.
 *
 * @return true if PSRAM is working correctly
 */
bool psram_test(void);

/** Detect physical capacity: 1, 2, 4, 8 or 16 MiB; 0 means unusable. */
size_t psram_detect_size(void);

/** Physical capacity found by the most recent detection. */
size_t psram_detected_size(void);
void psram_set_detected_size(size_t size);

/** Physical capacity available to the emulator. */
size_t psram_usable_size(void);

/** Commit cached PSRAM writes before starting an uncached non-destructive test. */
void psram_prepare_nondestructive_test(void);

/** Test a PSRAM byte range with two patterns and restore every original word. */
bool psram_test_nondestructive(size_t offset, size_t length);

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
    return psram_usable_size();
}

#endif // PSRAM_INIT_H
