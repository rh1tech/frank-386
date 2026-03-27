/**
 * frank-386 - i386 PC Emulator for RP2350
 *
 * Platform Integration Layer - platform-specific declarations for RP2350.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#ifndef _PLATFORM_RP2350_H_
#define _PLATFORM_RP2350_H_

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// POSIX Time Compatibility - only define what's missing from newlib
//=============================================================================

// Clock IDs (may not be defined in bare newlib)
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

// clock_gettime - not provided by newlib
int clock_gettime(int clk_id, struct timespec *tp);

// Sleep functions
int usleep(unsigned int usec);

//=============================================================================
// Platform Time Functions
//=============================================================================

/**
 * Set the base time offset for time() calculations.
 * @param unix_timestamp Current Unix timestamp
 */
void platform_set_time_offset(uint32_t unix_timestamp);

//=============================================================================
// Platform Information
//=============================================================================

/**
 * Print platform information to console.
 */
void platform_print_info(void);

/**
 * Get CPU cycle count for performance measurement.
 */
uint32_t platform_get_cycles(void);

/**
 * Get chip temperature in degrees Celsius * 100.
 */
int32_t platform_get_temperature(void);

//=============================================================================
// Critical Sections
//=============================================================================

/**
 * Enter critical section (disable interrupts).
 */
void platform_enter_critical(void);

/**
 * Exit critical section (restore interrupts).
 */
void platform_exit_critical(void);

//=============================================================================
// Watchdog
//=============================================================================

/**
 * Feed the watchdog timer.
 */
void platform_feed_watchdog(void);

//=============================================================================
// Performance Monitoring
//=============================================================================

/**
 * Record frame timing for performance monitoring.
 */
void platform_record_frame_time(uint32_t time_us);

/**
 * Get average frame time over last 16 frames.
 */
uint32_t platform_get_avg_frame_time(void);

//=============================================================================
// Initialization
//=============================================================================

/**
 * Platform-specific initialization.
 */
void platform_init(void);

/**
 * Configure DMA priorities for optimal performance.
 */
void platform_configure_dma_priorities(void);

//=============================================================================
// Debug Memory (optional)
//=============================================================================

#ifdef DEBUG_MEMORY
void *debug_malloc(size_t size);
void debug_free(void *ptr);
void debug_memory_stats(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _PLATFORM_RP2350_H_ */
