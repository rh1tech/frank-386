/**
 * murm386 - i386 PC Emulator for RP2350
 *
 * VGA Driver - based on pico-286's vga-nextgen by xrip.
 * Reads directly from emulator VRAM and renders text/graphics on-the-fly.
 *
 * GPIO Pinout (directly connected to VGA via resistor DAC):
 *   Base+0..5 = RGB (2 bits per color)
 *   Base+6 = H-Sync, Base+7 = V-Sync
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"
#include "hardware/dma.h"

// Include board configuration for pin definitions
#ifdef RP2350_BUILD
#include "board_config.h"
#endif

// VGA Pin Configuration - use HDMI pins for VGA on these boards
// (HDMI pins are directly connected to VGA via resistor DAC)
#ifndef VGA_BASE_PIN
#ifdef HDMI_BASE_PIN
#define VGA_BASE_PIN HDMI_BASE_PIN
#else
#define VGA_BASE_PIN 6  // Default for M1
#endif
#endif

#ifndef VGA_PIO
#define VGA_PIO      pio0
#endif

#ifndef VGA_DMA_IRQ
#define VGA_DMA_IRQ  DMA_IRQ_0
#endif

// Legacy framebuffer dimensions (for compatibility)
#define VGA_FB_WIDTH  320
#define VGA_FB_HEIGHT 200

// Initialize VGA hardware subsystem
void vga_hw_init(void);

// Set cursor position and size for text mode
// char_height is the emulated character cell height (for scaling cursor to 16-line font)
void vga_hw_set_cursor(int x, int y, int start, int end, int char_height);

// Set VRAM start offset for scrolling
void vga_hw_set_vram_offset(uint16_t offset);

// Set horizontal pixel panning (0-7)
void vga_hw_set_panning(uint8_t panning);

// Set Line Compare (scanline where address resets to 0)
void vga_hw_set_line_compare(int line);

// Update 256-color palette from emulator's VGA DAC
// palette_data is 768 bytes (256 entries × 3 bytes RGB, each 0-63)
void vga_hw_set_palette(const uint8_t *palette_data);

// Update EGA 16-color palette from AC palette registers
// palette16_data is 48 bytes (16 entries × 3 bytes RGB, each 0-63)
void vga_hw_set_palette16(const uint8_t *palette16_data);

// Set graphics sub-mode: 1=CGA, 2=EGA planar, 3=VGA 256-color
// line_offset is the number of words per scanline (from VGA cr[0x13])
void vga_hw_set_gfx_mode(int submode, int width, int height, int line_offset);

// Update VGA from PSRAM - call from main loop periodically
// Copies text buffer from PSRAM to SRAM for IRQ-safe rendering
void vga_hw_update(void);

// Get current frame count
uint32_t vga_hw_get_frame_count(void);

// Legacy API (compatibility stubs)
uint8_t *vga_hw_get_framebuffer(void);
void vga_hw_clear(uint8_t color);

// Set a single pixel (legacy, no-op)
void vga_hw_set_pixel(int x, int y, uint8_t color);

// Give the ISR direct read-only access to VGA register state.
// Call once after vga_init(). ISR reads cr[], ar[] at the right moment —
// no volatile intermediates, no synchronisation lag.
#include "../../src/vga.h"
void vga_hw_set_vga_state(VGAState *s);
