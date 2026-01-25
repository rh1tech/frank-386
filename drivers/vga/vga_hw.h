/**
 * VGA Driver for RP2350 - pico-286 style
 *
 * Reads directly from emulator VRAM and renders text/graphics on-the-fly.
 * No intermediate framebuffer - renders at 640x400 for proper text mode.
 *
 * GPIO Pinout (directly connected to VGA via resistor DAC):
 *   Base+0 = Blue low bit
 *   Base+1 = Blue high bit
 *   Base+2 = Green low bit
 *   Base+3 = Green high bit
 *   Base+4 = Red low bit
 *   Base+5 = Red high bit
 *   Base+6 = H-Sync (directly from bit 6)
 *   Base+7 = V-Sync (directly from bit 7)
 *
 * Pin configuration is set via board_config.h (M1 or M2 layout)
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

// Set pointer to emulator's VRAM (tiny386's vga_mem)
void vga_hw_set_vram(uint8_t *vram);

// Set video mode (3 = 80x25 text, 0x13 = 320x200x256, etc.)
void vga_hw_set_mode(int mode);

// Set cursor position and size for text mode
// char_height is the emulated character cell height (for scaling cursor to 16-line font)
void vga_hw_set_cursor(int x, int y, int start, int end, int char_height);

// Set cursor blink state (1 = visible, 0 = hidden during blink cycle)
void vga_hw_set_cursor_blink(int blink_phase);

// Set VRAM start offset for scrolling
void vga_hw_set_vram_offset(uint16_t offset);

// Set horizontal pixel panning (0-7)
void vga_hw_set_panning(uint8_t panning);

// Set Line Compare (scanline where address resets to 0)
void vga_hw_set_line_compare(int line);

// Submit a new frame state (registers) to be rendered
// This signals Core 1 to copy VRAM and apply these registers at the next opportunity
void vga_hw_submit_frame(uint16_t start_addr, uint8_t panning, int line_compare);

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

// Debug: get graphics pre-render stats (resets counters)
void vga_hw_get_gfx_stats(uint32_t *prerender, uint32_t *fallback);

// Legacy API (compatibility stubs)
uint8_t *vga_hw_get_framebuffer(void);
void vga_hw_clear(uint8_t color);

// Set a single pixel (legacy, no-op)
void vga_hw_set_pixel(int x, int y, uint8_t color);
