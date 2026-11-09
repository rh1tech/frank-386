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

/** Leave QPI mode and reset the PSRAM before a software reboot. */
void psram_reset_to_spi(void);

/** MF ID and KGD read at boot by psram_init() (0x0D, 0x5D on APS6404). */
void psram_get_id(uint8_t *mfid, uint8_t *kgd);

/** Cap every later psram_init_with_freq() at mhz (0 = no cap). */
void psram_set_freq_cap(int mhz);
int psram_get_freq_cap(void);

/** What the first detection write read back (0x13579BDF when it worked). */
uint32_t psram_probe_readback(void);

/** Detect physical capacity: 1, 2, 4, 8 or 16 MiB; 0 means unusable. */
size_t psram_detect_size(void);

/** Physical capacity found by the most recent detection. */
size_t psram_detected_size(void);

/** Physical capacity available to the emulator. */
size_t psram_usable_size(void);

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
