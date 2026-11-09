/**
 * frank-386 - i386 PC Emulator for RP2350
 *
 * Platform Integration Layer - provides POSIX-like compatibility functions
 * and platform-specific implementations for RP2350 (Pico 2).
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"

#include "board_config.h"
#include "platform_rp2350.h"

//=============================================================================
// Time and Clock Functions
//=============================================================================

// Base time offset (seconds since Unix epoch when device booted)
// Default to a reasonable date if RTC not set: 2024-01-01 00:00:00 UTC
static uint32_t boot_time_offset = 1704067200;

/**
 * Set the base time offset (call this if you have a valid time source).
 */
void platform_set_time_offset(uint32_t unix_timestamp) {
    boot_time_offset = unix_timestamp - (time_us_64() / 1000000);
}

/**
 * clock_gettime implementation using Pico SDK.
 * Newlib doesn't provide this, so we implement it.
 */
int clock_gettime(int clk_id, struct timespec *tp) {
    if (!tp) return -1;

    uint64_t us = time_us_64();

    if (clk_id == CLOCK_MONOTONIC) {
        tp->tv_sec = us / 1000000;
        tp->tv_nsec = (us % 1000000) * 1000;
    } else {
        // CLOCK_REALTIME
        tp->tv_sec = boot_time_offset + (us / 1000000);
        tp->tv_nsec = (us % 1000000) * 1000;
    }

    return 0;
}

//=============================================================================
// Sleep Functions
//=============================================================================

/**
 * Sleep for specified number of microseconds.
 */
int usleep(unsigned int usec) {
    sleep_us(usec);
    return 0;
}

//=============================================================================
// Serial/Terminal Functions (stubs for embedded)
//=============================================================================

/**
 * Stub for CaptureKeyboardInput - not needed on RP2350.
 * On RP2350, keyboard input comes from PS/2, not from terminal.
 */
void CaptureKeyboardInput(void) {
    // No-op on RP2350
}

//=============================================================================
// Assertion/Debug Support
//=============================================================================

/**
 * Assertion failure handler.
 */
void __assert_func(const char *file, int line, const char *func, const char *expr) {
    printf("\n*** ASSERTION FAILED ***\n");
    printf("File: %s\n", file);
    printf("Line: %d\n", line);
    printf("Function: %s\n", func);
    printf("Expression: %s\n", expr);
    printf("\nSystem halted.\n");

    // Flash LED in pattern to indicate failure
    while (1) {
        sleep_ms(100);
    }
}

//=============================================================================
// Heap
//=============================================================================

/*
 * The heap, with a top that can be taken off it (heap_reserve_top). The
 * SDK's _sbrk (weak) with a movable limit in place of __StackLimit.
 */
extern char end, __StackLimit;
static char *heap_end;
static char *heap_limit = &__StackLimit;

void *_sbrk(int incr) {
    if (heap_end == 0)
        heap_end = &end;
    char *prev_heap_end = heap_end;
    if (heap_end + incr > heap_limit)
        return (char *)-1;
    heap_end += incr;
    return prev_heap_end;
}

/*
 * Take the top `size` bytes of the heap (size a power of 2: the block is
 * aligned to it, as the top of main RAM is), or NULL when the heap has
 * grown into them. For the HDMI driver's TMDS table, whose address window
 * must be aligned to its size: anywhere else that alignment costs up to
 * as much again in padding. Call while one core alone allocates.
 */
void *video_ram_block;

void *heap_reserve_top(size_t size) {
    char *top = (char *)(((uintptr_t)heap_limit - size) & ~(uintptr_t)(size - 1));
    if (top < (heap_end ? heap_end : &end))
        return NULL;
    heap_limit = top;
    return top;
}

/*
 * newlib grows the heap in 4 KB steps sized from the request alone, not
 * from what the heap's free top already holds: with 8 KB left below the
 * limit a 4 KB block can fail. One block as large as fits, freed again,
 * leaves the whole heap as that free top, which later blocks are cut from
 * with no more growing. Bypasses pico_malloc's panic on failure; call while
 * one core alone allocates.
 */
void heap_grow_all(void) {
    extern void *__real_malloc(size_t size);
    extern void __real_free(void *p);
    char *cur = heap_end ? heap_end : &end;
    for (long n = heap_limit - cur; n >= 1024; n -= 256) {
        void *p = __real_malloc((size_t)n);
        if (p) {
            __real_free(p);
            return;
        }
    }
}

//=============================================================================
// Memory Debugging
//=============================================================================

#ifdef DEBUG_MEMORY
static size_t total_allocated = 0;
static size_t peak_allocated = 0;

void *debug_malloc(size_t size) {
    void *ptr = malloc(size + sizeof(size_t));
    if (ptr) {
        *(size_t *)ptr = size;
        total_allocated += size;
        if (total_allocated > peak_allocated) {
            peak_allocated = total_allocated;
        }
        return (char *)ptr + sizeof(size_t);
    }
    return NULL;
}

void debug_free(void *ptr) {
    if (ptr) {
        size_t *size_ptr = (size_t *)((char *)ptr - sizeof(size_t));
        total_allocated -= *size_ptr;
        free(size_ptr);
    }
}

void debug_memory_stats(void) {
    printf("Memory: current=%zu bytes, peak=%zu bytes\n",
           total_allocated, peak_allocated);
}
#endif

//=============================================================================
// Platform Information
//=============================================================================

/**
 * Print platform information to console.
 */
void platform_print_info(void) {
    printf("Platform: RP2350 (Pico 2)\n");
    printf("  System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    printf("  USB clock: %lu MHz\n", clock_get_hz(clk_usb) / 1000000);

#ifdef BOARD_M1
    printf("  Board variant: M1\n");
#elif defined(BOARD_M2)
    printf("  Board variant: M2\n");
#endif

    printf("  PSRAM base: 0x%08lx\n", (unsigned long)PSRAM_BASE);
    printf("  PSRAM size: %lu KB\n", (unsigned long)(PSRAM_SIZE / 1024));
}

//=============================================================================
// CPU Cycle Counter
//=============================================================================

/**
 * Get CPU cycle count (for performance measurement).
 * RP2350 has a cycle counter in the SysTick timer.
 */
uint32_t platform_get_cycles(void) {
    // Use time_us_32() scaled by approximate MHz as cycle proxy
    return time_us_32() * (clock_get_hz(clk_sys) / 1000000);
}

//=============================================================================
// Watchdog Feed
//=============================================================================

/**
 * Feed the watchdog (if enabled).
 * Call this periodically in the main loop to prevent reset.
 */
void platform_feed_watchdog(void) {
    // Watchdog is not enabled by default in this port
    // If needed, add: watchdog_update();
}

//=============================================================================
// Critical Section Support
//=============================================================================

static uint32_t saved_irq_state;
static int critical_depth = 0;

/**
 * Enter critical section (disable interrupts).
 */
void platform_enter_critical(void) {
    if (critical_depth++ == 0)
        saved_irq_state = save_and_disable_interrupts();
}

/**
 * Exit critical section (restore interrupts).
 */
void platform_exit_critical(void) {
    if (--critical_depth == 0)
        restore_interrupts(saved_irq_state);
}

//=============================================================================
// DMA Priority Configuration
//=============================================================================

/**
 * Configure DMA priorities for optimal performance.
 * VGA DMA should have highest priority for glitch-free display.
 */
void platform_configure_dma_priorities(void) {
    // DMA channel priorities are configured in vga_hw.c
    // This function is a placeholder for any additional configuration
}

//=============================================================================
// Performance Monitoring
//=============================================================================

static uint32_t frame_times[16];
static int frame_time_index = 0;

/**
 * Record frame timing for performance monitoring.
 */
void platform_record_frame_time(uint32_t time_us) {
    frame_times[frame_time_index] = time_us;
    frame_time_index = (frame_time_index + 1) & 15;
}

/**
 * Get average frame time over last 16 frames.
 */
uint32_t platform_get_avg_frame_time(void) {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += frame_times[i];
    }
    return sum / 16;
}

//=============================================================================
// Temperature Monitoring
//=============================================================================

/**
 * Get chip temperature in degrees Celsius * 100.
 * Returns approximate temperature from internal sensor.
 */
int32_t platform_get_temperature(void) {
    // RP2350 temperature sensor - placeholder
    // Actual implementation would read ADC channel 4
    return 2500;  // Default 25.00 C
}

//=============================================================================
// Initialization
//=============================================================================

/**
 * Platform-specific initialization.
 * Called early in main() before other subsystems.
 */
void platform_init(void) {
    // Initialize frame timing array
    memset(frame_times, 0, sizeof(frame_times));

    // Platform is ready
}
