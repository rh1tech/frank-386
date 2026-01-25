/**
 * murm386 - i386 PC Emulator for RP2350
 *
 * PS/2 Mouse Driver - GPIO interrupt-based PS/2 mouse interface
 * Based on DnCraptor's implementation from murmdoom
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#ifndef PS2MOUSE_H
#define PS2MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mouse state structure
typedef struct {
    int16_t delta_x;      // Accumulated X movement
    int16_t delta_y;      // Accumulated Y movement
    int8_t  wheel;        // Wheel movement (if IntelliMouse)
    uint8_t buttons;      // Button state (bit 0=left, 1=right, 2=middle)
    int     has_wheel;    // True if IntelliMouse detected
    int     initialized;  // True if mouse detected and initialized
} ps2mouse_state_t;

// Initialize the PS/2 mouse driver
void ps2mouse_init(void);

// Poll for mouse data (call regularly from main loop)
void ps2mouse_poll(void);

// Get accumulated mouse movement and clear accumulators
// Returns 1 if there was any movement or button change, 0 otherwise
int ps2mouse_get_state(int16_t *dx, int16_t *dy, int8_t *wheel, uint8_t *buttons);

// Check if mouse is initialized
int ps2mouse_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif // PS2MOUSE_H
