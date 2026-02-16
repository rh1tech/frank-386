/**
 * murm386 - i386 PC Emulator for RP2350
 *
 * VGA Driver - based on pico-286's vga-nextgen by xrip.
 * Reads directly from emulator VRAM and renders text/graphics on-the-fly.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#pragma GCC optimize("Ofast")

#include "vga_hw.h"
#include "vga_osd.h"
#include "font8x16.h"
#include "debug.h"
#include "board_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

// ============================================================================
// PIO Program
// ============================================================================

static const uint16_t pio_vga_instructions[] = {
    0x6008,  // out pins, 8
};

static const struct pio_program pio_vga_program = {
    .instructions = pio_vga_instructions,
    .length = 1,
    .origin = -1,
};

// ============================================================================
// VGA Timing (640x400 @ 70Hz - standard VGA text mode timing)
// ============================================================================

#define VGA_CLK 25175000.0f


#define LINE_SIZE           800
/*
#define N_LINES_TOTAL       449
#define N_LINES_VISIBLE     400
#define LINE_VS_BEGIN       412
#define LINE_VS_END         414
*/

#define N_LINES_TOTAL   525
#define N_LINES_VISIBLE 400
//#define LINE_VS_BEGIN   490
//#define LINE_VS_END     492

#define LINE_VS_BEGIN   452
#define LINE_VS_END     454

#define HS_SIZE             96
#define SHIFT_PICTURE       VGA_SHIFT_PICTURE  // Where active video starts (from board_config.h)

// Sync encoding in bits 6-7
#define TMPL_LINE           0xC0
#define TMPL_HS             0x80
#define TMPL_VS             0x40
#define TMPL_VHS            0x00

// ============================================================================
// Module State
// ============================================================================

// Line pattern buffers - now 6 buffers:
// 0 = hsync template, 1 = vsync template, 2-5 = active video (4 line rolling buffer)
static uint32_t *lines_pattern[6];
static uint32_t *lines_pattern_data = NULL;

// DMA channels
static int dma_data_chan = -1;
static int dma_ctrl_chan = -1;

// PIO state
static uint vga_sm = 0;

// Spin lock for frame state synchronization
static spin_lock_t *vga_state_lock = NULL;

// Frame counter
static volatile uint32_t frame_count = 0;
static volatile uint32_t current_line = 0;

// Text buffer in SRAM (non-static to allow OSD reuse when paused)
uint8_t text_buffer_sram[80 * 25 * 2] __attribute__((aligned(4)));
static volatile int update_requested = 0;  // Set by update call
static volatile int in_vblank = 0;         // Set by IRQ during vblank

#define GFX_BUFFER_SIZE (256 * 1024)
uint8_t gfx_buffer[GFX_BUFFER_SIZE] __attribute__((aligned(4)));
static volatile int gfx_write_done = 0;  // Set when write buffer has new frame
static volatile int gfx_copy_allowed = 0;  // Set during vblank to allow copy

// Text mode palette (16 colors -> 6-bit VGA)
static uint8_t txt_palette[16];

// Fast text palette for 2-bit pixel pairs
static uint16_t txt_palette_fast[256 * 4];

// Graphics palette (256 entries) - 16-bit dithered format
// Each entry: low byte = c_hi (conv0), high byte = c_lo (conv1)
// even - A, odd - B
static uint16_t palette_a[256];
static uint16_t palette_b[256];

// 16-color EGA palette (for 16-color modes) - 8-bit VGA format with sync bits
static uint8_t ega_palette[16];

// CGA 4-color palette - 8-bit VGA format with sync bits
// Default to palette 1 high intensity: black, cyan, magenta, white
static uint8_t cga_palette[4];

// Current video mode (0=blank, 1=text, 2=graphics)
static int current_mode = 1;  // Default text mode
static volatile int pending_mode = -1;  // Pending mode change (-1 = none)

// Graphics sub-mode: 1=CGA 4-color, 2=EGA planar, 3=VGA 256-color, 4=CGA 2-color
static int gfx_submode = 3;
static int gfx_width = 320;
static int gfx_height = 200;
static int gfx_line_offset = 40;  // Words per line (40 for 320px EGA, 80 for 640px)
static int gfx_sram_stride = 41;  // Words per line in SRAM buffer (width/8 + 1)

// Cursor state
static int cursor_x = 0, cursor_y = 0;
static int cursor_start = 0, cursor_end = 15;
static int cursor_blink_state = 1;

// VRAM offset for scrolling (set by emulator, copied to frame_vram_offset during vblank)
static uint16_t vram_offset = 0;
static uint16_t frame_vram_offset = 0;  // Snapshotted at start of frame for consistent rendering
static uint8_t pixel_panning = 0;       // Horizontal pixel panning (0-7)
static uint8_t frame_pixel_panning = 0; // Snapshotted at start of frame
static int line_compare = -1;           // Line Compare register (-1 = disabled/off-screen)
static int frame_line_compare = -1;     // Snapshotted at start of frame

// Debug counters
static volatile uint32_t gfx_fallback_count = 0;

// ============================================================================
// Color Conversion
// ============================================================================

// Dithering lookup tables from quakegeneric
// These map 3-bit values (0-7) to 2-bit values with different rounding
// conv0 rounds down more, conv1 rounds up more
// Alternating between them spatially creates perceived intermediate colors
static const uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
static const uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };

// Convert 6-bit VGA DAC values (0-63) to 8-bit output with sync bits
// Used for EGA/CGA palettes (no dithering)
static uint8_t vga_color_to_output(uint8_t r6, uint8_t g6, uint8_t b6) {
    // Map 6-bit VGA colors to 2-bit per channel (RRGGBB in bits 0-5)
    // Bits 6-7 are sync: 0xC0 = no sync pulses during active video
    uint8_t r2 = r6 >> 4;  // 6-bit to 2-bit
    uint8_t g2 = g6 >> 4;
    uint8_t b2 = b6 >> 4;
    return TMPL_LINE | (r2 << 4) | (g2 << 2) | b2;
}

// Convert 6-bit VGA DAC values to 16-bit dithered output
// Returns: low byte = c_hi (conv0), high byte = c_lo (conv1)
// When output as 16-bit, adjacent pixels get different colors for spatial dithering
static void vga_color_to_dithered(uint8_t r6, uint8_t g6, uint8_t b6, uint32_t idx) {
    // Convert 6-bit (0-63) to 3-bit (0-7) for dither table lookup
    // 63/7 ≈ 9, so divide by 9
    uint8_t r = r6 / 9;
    uint8_t g = g6 / 9;
    uint8_t b = b6 / 9;
    if (r > 7) r = 7;
    if (g > 7) g = 7;
    if (b > 7) b = 7;

    uint8_t c_hi = TMPL_LINE | (conv0[r] << 4) | (conv0[g] << 2) | conv0[b];
    uint8_t c_lo = TMPL_LINE | (conv1[r] << 4) | (conv1[g] << 2) | conv1[b];

    palette_a[idx] = (uint16_t)c_hi | ((uint16_t)c_lo << 8);
    palette_b[idx] = (uint16_t)c_lo | ((uint16_t)c_hi << 8);
}

static void init_palettes(void) {
    // Standard 16-color text palette (CGA colors)
    // Each entry is 6-bit: RRGGBB
    static const uint8_t cga_colors[16] = {
        0x00,  // 0: Black
        0x02,  // 1: Blue
        0x08,  // 2: Green
        0x0A,  // 3: Cyan
        0x20,  // 4: Red
        0x22,  // 5: Magenta
        0x28,  // 6: Brown (dark yellow)
        0x2A,  // 7: Light Gray
        0x15,  // 8: Dark Gray
        0x17,  // 9: Light Blue
        0x1D,  // 10: Light Green
        0x1F,  // 11: Light Cyan
        0x35,  // 12: Light Red
        0x37,  // 13: Light Magenta
        0x3D,  // 14: Yellow
        0x3F,  // 15: White
    };
    
    for (int i = 0; i < 16; i++) {
        txt_palette[i] = cga_colors[i] | TMPL_LINE;
    }
    
    // Build fast palette for text rendering
    // Each entry handles 2 pixels (foreground/background combinations)
    // For 2-bit value from glyph: high bit is LEFT pixel, low bit is RIGHT pixel
    // When we extract (glyph >> 6) & 3, we get bits 7,6 where bit7=left, bit6=right
    // Index XY (X=bit7, Y=bit6): X is left pixel, Y is right pixel
    // Output 16-bit: low byte outputs first (left), high byte outputs second (right)
    for (int i = 0; i < 256; i++) {
        uint8_t fg = txt_palette[i & 0x0F];
        uint8_t bg = txt_palette[i >> 4];
        
        // Index bits: [left_pixel][right_pixel]
        // For little-endian 16-bit output: low byte = left, high byte = right
        txt_palette_fast[i * 4 + 0] = bg | (bg << 8);  // 00: left=bg, right=bg
        txt_palette_fast[i * 4 + 1] = fg | (bg << 8);  // 01: left=fg, right=bg (bit7=0,bit6=1)
        txt_palette_fast[i * 4 + 2] = bg | (fg << 8);  // 10: left=bg, right=fg (bit7=1,bit6=0)
        txt_palette_fast[i * 4 + 3] = fg | (fg << 8);  // 11: left=fg, right=fg
    }
    
    // Initialize 256-color dithered palette with black (will be overwritten by emulator)
    for (int i = 0; i < 256; i++) {
        vga_color_to_dithered(0, 0, 0, i);
    }
    
    // Initialize CGA 4-color palette (palette 1 high intensity: black, cyan, magenta, white)
    // Use direct RGB values for proper CGA colors
    cga_palette[0] = vga_color_to_output(0, 0, 0);     // Black
    cga_palette[1] = vga_color_to_output(0, 63, 63);   // Cyan (bright)
    cga_palette[2] = vga_color_to_output(63, 0, 63);   // Magenta (bright)
    cga_palette[3] = vga_color_to_output(63, 63, 63);  // White
}

// ============================================================================
// DMA Interrupt Handler - Renders each scanline
// ============================================================================

// Render graphics line directly from SRAM framebuffer (for IRQ use)
// Uses dithered 16-bit palette for ~2197 perceived colors
static void __time_critical_func(render_gfx_line_from_sram)(uint32_t line, uint32_t *output_buffer) {
    uint32_t *out32 = output_buffer + SHIFT_PICTURE / 4;

    // Determine source line based on graphics height
    // If height > 200 (e.g. 400 in Mode X), map 1:1
    // If height <= 200 (e.g. 320x200), double lines
    uint32_t src_line;
    if (gfx_height > 200) {
        src_line = line;
    } else {
        src_line = line / 2;
    }

    if (src_line >= gfx_height && gfx_height > 0) {
        // Blank line below visible area
        for (int i = 0; i < 160; i++) {
            *out32++ = (TMPL_LINE) | (TMPL_LINE << 8) | (TMPL_LINE << 16) | (TMPL_LINE << 24);
        }
    } else if (src_line >= 200 && gfx_height <= 0) {
         // Fallback if gfx_height not set
        for (int i = 0; i < 160; i++) {
            *out32++ = (TMPL_LINE) | (TMPL_LINE << 8) | (TMPL_LINE << 16) | (TMPL_LINE << 24);
        }
    } else {
        // Read from display buffer (stable during active video)
        // 320 pixels = 320 bytes. Stride is 320.
        uint32_t stride = 80; // 320px / 4 pixels per word
        uint32_t offset;
        if (frame_line_compare >= 0 && src_line >= (uint32_t)frame_line_compare) {
            offset = (src_line - frame_line_compare) * stride;
        } else {
            offset = frame_vram_offset + src_line * stride;
        }

        offset &= 0xFFFF;

        const uint32_t *src32 = (const uint32_t *)(gfx_buffer + (offset * 4));

        // Each pixel outputs 16 bits (2 bytes) of dithered color
        // Low byte = c_hi (conv0), high byte = c_lo (conv1)
        // This creates spatial dithering for ~2197 perceived colors
        uint16_t * active_palette = src_line & 1 ? palette_a : palette_b;
        for (int i = 0; i < 80; i++) {
            uint32_t pixels = src32[i];
            uint16_t p0 = active_palette[pixels & 0xFF];
            uint16_t p1 = active_palette[(pixels >> 8) & 0xFF];
            uint16_t p2 = active_palette[(pixels >> 16) & 0xFF];
            uint16_t p3 = active_palette[pixels >> 24];
            // Each pixel outputs 16 bits (dithered pair): 4 pixels = 8 bytes = 2 x uint32_t
            *out32++ = (uint32_t)p0 | ((uint32_t)p1 << 16);
            *out32++ = (uint32_t)p2 | ((uint32_t)p3 << 16);
        }
    }
}

// Render CGA 4-color graphics line (320x200, 2 bits per pixel, interleaved)
// VGA stores CGA data in odd/even mode with interleaved planes
static void __time_critical_func(render_gfx_line_cga)(uint32_t line, uint32_t *output_buffer) {
    uint32_t *out32 = output_buffer + SHIFT_PICTURE / 4;

    // CGA 320x200 mode (doubled to 640x400)
    uint32_t src_line = line / 2;
    if (src_line >= 200) {
        // Blank line
        for (int i = 0; i < 160; i++) {
            *out32++ = (TMPL_LINE) | (TMPL_LINE << 8) | (TMPL_LINE << 16) | (TMPL_LINE << 24);
        }
    } else {
        // CGA interleaved scanlines:
        // Even lines (0,2,4,...) at offset 0x0000
        // Odd lines (1,3,5,...) at offset 0x2000
        uint32_t cga_bank = (src_line & 1) ? 0x2000 : 0x0000;
        uint32_t cga_row = src_line >> 1;  // Which row within bank (0-99)
        uint32_t cga_line_offset;
        if (frame_line_compare >= 0 && src_line >= (uint32_t)frame_line_compare) {
            cga_line_offset = cga_bank + (src_line - frame_line_compare) * 80;
        } else {
            cga_line_offset = frame_vram_offset + cga_bank + cga_row * 80;
        }
        cga_line_offset &= 0xFFFF;

        const uint8_t *src = gfx_buffer;

        // In CGA/odd-even mode, data is stored linearly in planes 0 and 1
        // Even bytes go to plane 0, odd bytes go to plane 1
        // VGA address = ((cga_addr & ~1) << 1) | (cga_addr & 1)
        // This spreads byte pairs across 4-byte boundaries

        // 80 bytes per CGA scanline = 320 pixels, doubled to 640
        for (int i = 0; i < 80; i++) {
            uint32_t cga_addr = cga_line_offset + i;
            uint32_t vga_addr = ((cga_addr & ~1) << 1) | (cga_addr & 1);
            uint8_t byte = src[vga_addr];

            // Extract 4 pixels (2 bits each), MSB first
            uint8_t p0 = cga_palette[(byte >> 6) & 3];
            uint8_t p1 = cga_palette[(byte >> 4) & 3];
            uint8_t p2 = cga_palette[(byte >> 2) & 3];
            uint8_t p3 = cga_palette[byte & 3];

            // Double each pixel horizontally (4 pixels -> 8 output pixels)
            *out32++ = (p0) | (p0 << 8) | (p1 << 16) | (p1 << 24);
            *out32++ = (p2) | (p2 << 8) | (p3 << 16) | (p3 << 24);
        }
    }
}

// Render CGA 2-color graphics line (640x200, 1 bit per pixel, interleaved)
// Mode 6: 640x200 monochrome CGA mode
// Memory layout: planar (4 bytes per screen byte), plane 0 only contains data
// Row interleaving: even rows at bank 0, odd rows at bank 1 (0x2000 offset)
static void __time_critical_func(render_gfx_line_cga2)(uint32_t line, uint32_t *output_buffer) {
    uint32_t *out32 = output_buffer + SHIFT_PICTURE / 4;

    // CGA 640x200 mode (doubled to 640x400)
    uint32_t src_line = line / 2;
    if (src_line >= 200) {
        // Blank line
        for (int i = 0; i < 160; i++) {
            *out32++ = (TMPL_LINE) | (TMPL_LINE << 8) | (TMPL_LINE << 16) | (TMPL_LINE << 24);
        }
    } else {
        // CGA interleaved scanlines:
        // Even lines (0,2,4,...) at offset 0x0000
        // Odd lines (1,3,5,...) at offset 0x2000 (which is 0x800 words)
        // Each "byte" of screen data is stored at 4-byte boundaries (planar layout)
        uint32_t bank_offset = (src_line & 1) ? 0x2000 : 0x0000;
        uint32_t row_in_bank = src_line >> 1;  // Which row within bank (0-99)
        // Base address for this scanline (in bytes): bank + row * 80 bytes/row
        // In planar layout: multiply by 4 to get actual byte offset
        uint32_t offset;
        if (frame_line_compare >= 0 && src_line >= (uint32_t)frame_line_compare) {
            offset = bank_offset + (src_line - frame_line_compare) * 80;
        } else {
            offset = frame_vram_offset + bank_offset + row_in_bank * 80;
        }
        offset &= 0xFFFF;
        uint32_t base_addr = offset * 4;
        const uint8_t *src = gfx_buffer;

        // CGA 2-color palette: 0 = black, 1 = white (or foreground color)
        uint8_t bg = cga_palette[0];  // Background (black)
        uint8_t fg = cga_palette[3];  // Foreground (white)

        // 80 bytes per CGA scanline = 640 pixels (1 bit per pixel)
        // Data is in plane 0 (every 4th byte in planar layout)
        for (int i = 0; i < 80; i++) {
            // In planar layout, plane 0 is at offset 0, 4, 8, 12, ...
            uint8_t byte = src[base_addr + i * 4];

            // Extract 8 pixels (1 bit each), MSB first
            // Output directly (no horizontal doubling since 640 is native width)
            uint8_t p0 = (byte & 0x80) ? fg : bg;
            uint8_t p1 = (byte & 0x40) ? fg : bg;
            uint8_t p2 = (byte & 0x20) ? fg : bg;
            uint8_t p3 = (byte & 0x10) ? fg : bg;
            uint8_t p4 = (byte & 0x08) ? fg : bg;
            uint8_t p5 = (byte & 0x04) ? fg : bg;
            uint8_t p6 = (byte & 0x02) ? fg : bg;
            uint8_t p7 = (byte & 0x01) ? fg : bg;

            // 8 pixels = 8 bytes = 2 x uint32_t (no doubling)
            *out32++ = (p0) | (p1 << 8) | (p2 << 16) | (p3 << 24);
            *out32++ = (p4) | (p5 << 8) | (p6 << 16) | (p7 << 24);
        }
    }
}

// Spread 8 bits of a byte into positions 0,4,8,...28
static inline uint32_t spread8(uint32_t plane) {
    plane = (plane | (plane << 12)) & 0x000F000Fu;
    plane = (plane | (plane <<  6)) & 0x03030303u;
    plane = (plane | (plane <<  3)) & 0x11111111u;
    return plane;
}

// Merge 4 plane bytes [P3|P2|P1|P0] into 8 nibbles (pixel color indices).
static inline uint32_t ega_pack8_from_planes(const uint32_t ega_planes) {
    const uint32_t pixel1 = spread8(ega_planes        & 0xFFu);
    const uint32_t pixel2 = spread8((ega_planes >> 8) & 0xFFu);
    const uint32_t pixel3 = spread8((ega_planes >>16) & 0xFFu);
    const uint32_t pixel4 = spread8(ega_planes >>24);

    return pixel1 | pixel2 << 1 | pixel3 << 2 | pixel4 << 3;
}

// Render EGA planar 16-color graphics line
// Supports both 320x200 (doubled) and 640x350 (native) modes
// Reads from SRAM buffer (copied from PSRAM during main loop)
static void __time_critical_func(render_gfx_line_ega)(uint32_t line, uint32_t *output_buffer) {
    uint32_t *out32 = output_buffer + SHIFT_PICTURE / 4;

    // Determine if we need pixel doubling (for 320-wide modes)
    int double_pixels = (gfx_width <= 320);

    // Determine source line with appropriate scaling
    // 400 display lines -> gfx_height source lines
    // gfx_height is the actual number of unique scanlines in VRAM
    uint32_t src_line;
    int height = gfx_height > 0 ? gfx_height : 200;

    // Calculate vertical scale factor: how many display lines per source line
    // For 400 display lines and 200 source lines: scale = 2 (double each line)
    // For 400 display lines and 100 source lines: scale = 4 (quadruple each line)
    // For 400 display lines and 350 source lines: scale ≈ 1.14
    if (height <= 100) {
        // Very low res (e.g., 640x100 doubled twice): each source line shows 4x
        src_line = line >> 2;
    } else if (height <= 200) {
        // 200-line mode: double vertically (400/2 = 200)
        src_line = line >> 1;
    } else if (height <= 350) {
        // 350-line mode: map 400 display lines to 350 source lines
        // Scale: src = line * 350 / 400 = line * 7 / 8
        src_line = (line * height) / N_LINES_VISIBLE;
    } else {
        // 400-line mode: 1:1 mapping
        src_line = line;
    }

    // Check if source line is beyond the actual height
    if (src_line >= (uint32_t)height) {
        // Blank line - fast fill
        uint32_t blank = TMPL_LINE | (TMPL_LINE << 8) | (TMPL_LINE << 16) | (TMPL_LINE << 24);
        for (int i = 0; i < 160; i++) {
            out32[i] = blank;
        }
        return;
    }
    uint32_t stride = gfx_line_offset > 0 ? (gfx_line_offset * 2) : (gfx_width / 8);

    uint32_t offset;
    if (frame_line_compare >= 0 && src_line >= (uint32_t)frame_line_compare) {
        offset = (src_line - frame_line_compare) * stride;
    } else {
        offset = frame_vram_offset + src_line * stride;
    }

    offset &= 0xFFFF;

    const uint32_t *src32 = (const uint32_t *)(gfx_buffer + (offset * 4));
    int panning = frame_pixel_panning;
    int shift = panning * 4;

    // Loop over display width
    int words_to_render = gfx_width / 8;
    if (words_to_render > 80) words_to_render = 80; // Cap at 640px

    if (double_pixels) {
        // 320-wide mode: double each pixel horizontally
        for (int i = 0; i < words_to_render; i++) {
            uint32_t ega_planes = src32[i];
            uint32_t eight_pixels = ega_pack8_from_planes(ega_planes);

            if (panning > 0) {
                uint32_t next_planes = src32[i+1];
                uint32_t next_eight = ega_pack8_from_planes(next_planes);
                eight_pixels = (eight_pixels << shift) | (next_eight >> (32 - shift));
            }

            // Lookup and double each pixel
            uint8_t c0 = ega_palette[eight_pixels >> 28];
            uint8_t c1 = ega_palette[(eight_pixels >> 24) & 0xF];
            uint8_t c2 = ega_palette[(eight_pixels >> 20) & 0xF];
            uint8_t c3 = ega_palette[(eight_pixels >> 16) & 0xF];
            uint8_t c4 = ega_palette[(eight_pixels >> 12) & 0xF];
            uint8_t c5 = ega_palette[(eight_pixels >> 8) & 0xF];
            uint8_t c6 = ega_palette[(eight_pixels >> 4) & 0xF];
            uint8_t c7 = ega_palette[eight_pixels & 0xF];

            // 4 x 32-bit writes = 16 bytes (8 doubled pixels)
            *out32++ = c0 | (c0 << 8) | (c1 << 16) | (c1 << 24);
            *out32++ = c2 | (c2 << 8) | (c3 << 16) | (c3 << 24);
            *out32++ = c4 | (c4 << 8) | (c5 << 16) | (c5 << 24);
            *out32++ = c6 | (c6 << 8) | (c7 << 16) | (c7 << 24);
        }
    } else {
        // 640-wide mode: no horizontal doubling
        for (int i = 0; i < words_to_render; i++) {
            uint32_t ega_planes = src32[i];
            uint32_t eight_pixels = ega_pack8_from_planes(ega_planes);

            if (panning > 0) {
                uint32_t next_planes = src32[i+1];
                uint32_t next_eight = ega_pack8_from_planes(next_planes);
                eight_pixels = (eight_pixels << shift) | (next_eight >> (32 - shift));
            }

            // Lookup each pixel (no doubling)
            uint8_t c0 = ega_palette[eight_pixels >> 28];
            uint8_t c1 = ega_palette[(eight_pixels >> 24) & 0xF];
            uint8_t c2 = ega_palette[(eight_pixels >> 20) & 0xF];
            uint8_t c3 = ega_palette[(eight_pixels >> 16) & 0xF];
            uint8_t c4 = ega_palette[(eight_pixels >> 12) & 0xF];
            uint8_t c5 = ega_palette[(eight_pixels >> 8) & 0xF];
            uint8_t c6 = ega_palette[(eight_pixels >> 4) & 0xF];
            uint8_t c7 = ega_palette[eight_pixels & 0xF];

            // 2 x 32-bit writes = 8 bytes (8 pixels, no doubling)
            *out32++ = c0 | (c1 << 8) | (c2 << 16) | (c3 << 24);
            *out32++ = c4 | (c5 << 8) | (c6 << 16) | (c7 << 24);
        }
    }
}

static volatile int text_cols = 80;
// Stride in *character cells* (uint32_t per cell in gfx_buffer text layout).
// For VGA CRTC Offset (0x13): cells_per_row = cr13 * 2 (80-col -> 40*2, 40-col -> 20*2).
static volatile int text_stride_cells = 80;

void vga_hw_set_text_cols(int cols) {
    if (cols == 40 || cols == 80) text_cols = cols;
}

void vga_hw_set_text_stride(int stride_cells) {
    if (stride_cells > 0 && stride_cells <= 256)
        text_stride_cells = stride_cells;
}

// Helper function to render a single text line
static void __time_critical_func(render_text_line)(uint32_t line, uint32_t *output_buffer) {
    uint16_t *out16 = (uint16_t *)output_buffer + SHIFT_PICTURE / 2;

    uint32_t char_row = line / 16;
    uint32_t glyph_line = line & 15;

    int cols = text_cols;
    int double_h = (cols == 40);

    if (char_row < 25) {
        // Use snapped start address for the frame (prevents mid-frame tearing).
        const uint32_t *base = (const uint32_t *)(gfx_buffer + ((uint32_t)frame_vram_offset << 2));
        const uint32_t *text_row = base + (char_row * (uint32_t)text_stride_cells);

        for (int col = 0; col < cols; col++) {
            uint16_t cell = text_row[col];
            uint8_t ch   = (uint8_t)(cell & 0xFF);
            uint8_t attr = (uint8_t)(cell >> 8);
            uint8_t glyph = font_8x16[ch * 16 + glyph_line];
            uint16_t *pal = &txt_palette_fast[(attr & 0x7F) * 4];

            if (cursor_blink_state && col == cursor_x &&
                char_row == (uint32_t)cursor_y &&
                glyph_line >= (uint32_t)cursor_start &&
                glyph_line <= (uint32_t)cursor_end) {
                glyph = 0xFF;
            }

            // 8px glyph -> 4x uint16 (каждый uint16 = 2 пикселя)
            uint16_t v;

            v = pal[glyph & 3];           *out16++ = v; if (double_h) *out16++ = v;
            v = pal[(glyph >> 2) & 3];    *out16++ = v; if (double_h) *out16++ = v;
            v = pal[(glyph >> 4) & 3];    *out16++ = v; if (double_h) *out16++ = v;
            v = pal[(glyph >> 6) & 3];    *out16++ = v; if (double_h) *out16++ = v;
        }
    }
}

// Dispatch to appropriate renderer based on current mode
static void __time_critical_func(render_line)(uint32_t line, uint32_t *output_buffer) {
    // If OSD is visible, it takes over the display completely
    // (it reuses text_buffer_sram so we can't render normal text)
    if (osd_is_visible()) {
        osd_render_line(line, output_buffer);
        return;
    }
    if (current_mode == 1) {
        // Text mode now rendered from linear framebuffer
        render_text_line(line, output_buffer);
        return;
    }
    if (current_mode == 2) {
        // Graphics mode - choose renderer based on submode
        if (gfx_submode == 1) {
            // CGA 4-color
            render_gfx_line_cga(line, output_buffer);
        } else if (gfx_submode == 2) {
            // EGA planar 16-color
            render_gfx_line_ega(line, output_buffer);
        } else if (gfx_submode == 4) {
            // CGA 2-color (640x200 monochrome)
            render_gfx_line_cga2(line, output_buffer);
        } else {
            // VGA 256-color (mode 13h) - default
            render_gfx_line_from_sram(line, output_buffer);
        }
    }
}

static void __isr __time_critical_func(dma_handler_vga)(void) {
    dma_hw->ints0 = 1u << dma_ctrl_chan;
    
    current_line++;
    if (current_line >= N_LINES_TOTAL) {
        current_line = 0;
        frame_count++;
        // Note: cursor_blink_state is now set externally via vga_hw_set_cursor_blink()
    }
    
    in_vblank = (current_line >= N_LINES_VISIBLE);
    
    // Allow copy during entire vblank period
    gfx_copy_allowed = in_vblank;
    
    // At start of vblank (end of active video), swap buffers if write is done
    if (current_line == N_LINES_VISIBLE) {
        frame_vram_offset = vram_offset;
        frame_line_compare = line_compare;
        frame_pixel_panning = pixel_panning;        
        // Apply pending mode change during vblank (safe time to switch)
        if (pending_mode >= 0) {
            current_mode = pending_mode;
            pending_mode = -1;
        }
        
        if (gfx_write_done) {
            gfx_write_done = 0;
        }
    }
    
    // Vertical blanking region
    if (current_line >= N_LINES_VISIBLE) {
        if (current_line >= LINE_VS_BEGIN && current_line <= LINE_VS_END) {
            dma_channel_set_read_addr(dma_ctrl_chan, &lines_pattern[1], false);
        } else {
            dma_channel_set_read_addr(dma_ctrl_chan, &lines_pattern[0], false);
        }
        
        // Pre-render first few lines late in vblank (buffers already swapped)
        if (current_line == N_LINES_TOTAL - 4) {
            render_line(0, lines_pattern[2]);
            render_line(1, lines_pattern[3]);
            render_line(2, lines_pattern[4]);
            render_line(3, lines_pattern[5]);
        }
        return;
    }
    
    // Active video: DMA reads from buffer (line % 4), we render (line + 2) % 4
    uint32_t line = current_line;
    uint32_t read_buf = 2 + (line & 3);
    uint32_t render_buf = 2 + ((line + 2) & 3);
    uint32_t render_line_num = line + 2;
    
    // Set DMA to read from the buffer we already rendered
    dma_channel_set_read_addr(dma_ctrl_chan, &lines_pattern[read_buf], false);
    
    // Pre-render 2 lines ahead (if still in active region)
    if (render_line_num < N_LINES_VISIBLE) {
        render_line(render_line_num, lines_pattern[render_buf]);
    }
}

// ============================================================================
// Public API
// ============================================================================

void vga_hw_init(void) {
    DBG_PRINT("VGA Init (pico-286 style)...\n");

    init_palettes();

    // Calculate clock divider
    float sys_clk = (float)clock_get_hz(clk_sys);
    float clk_div = sys_clk / VGA_CLK;

    DBG_PRINT("  System clock: %.1f MHz\n", sys_clk / 1e6f);
    DBG_PRINT("  Clock divider: %.4f\n", clk_div);
    
    // Allocate line pattern buffers (6 buffers: 2 sync + 4 active)
    lines_pattern_data = (uint32_t *)calloc(LINE_SIZE * 6 / 4, sizeof(uint32_t));
    if (!lines_pattern_data) {
        printf("ERROR: Failed to allocate VGA buffers!\n");
        return;
    }
    
    for (int i = 0; i < 6; i++) {
        lines_pattern[i] = &lines_pattern_data[i * (LINE_SIZE / 4)];
    }
    
    // Initialize line templates
    uint8_t *base = (uint8_t *)lines_pattern[0];
    memset(base, TMPL_LINE, LINE_SIZE);
    memset(base, TMPL_HS, HS_SIZE);
    
    base = (uint8_t *)lines_pattern[1];
    memset(base, TMPL_VS, LINE_SIZE);
    memset(base, TMPL_VHS, HS_SIZE);
    
    // Initialize all 4 active line buffers with the sync template
    for (int i = 2; i < 6; i++) {
        memcpy(lines_pattern[i], lines_pattern[0], LINE_SIZE);
    }

    // Initialize PIO
    uint offset = pio_add_program(VGA_PIO, &pio_vga_program);
    vga_sm = pio_claim_unused_sm(VGA_PIO, true);
    
    // Initialize spin lock for frame state synchronization
    int lock_num = spin_lock_claim_unused(true);
    vga_state_lock = spin_lock_init(lock_num);
    
    // Configure GPIO pins
    for (int i = 0; i < 8; i++) {
        gpio_init(VGA_BASE_PIN + i);
        gpio_set_dir(VGA_BASE_PIN + i, GPIO_OUT);
        pio_gpio_init(VGA_PIO, VGA_BASE_PIN + i);
        gpio_set_slew_rate(VGA_BASE_PIN + i, GPIO_SLEW_RATE_FAST);
        gpio_set_drive_strength(VGA_BASE_PIN + i, GPIO_DRIVE_STRENGTH_8MA);
    }
    
    // Configure PIO state machine
    pio_sm_set_consecutive_pindirs(VGA_PIO, vga_sm, VGA_BASE_PIN, 8, true);
    
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_out_pins(&c, VGA_BASE_PIN, 8);
    
    pio_sm_init(VGA_PIO, vga_sm, offset, &c);
    
    // Set clock divider (16.8 fixed point format: 16 bits integer, 8 bits fraction)
    // clk_div is a float, convert to 24.8 fixed point
    uint32_t div_int = (uint32_t)clk_div;
    uint32_t div_frac = (uint32_t)((clk_div - div_int) * 256.0f);
    uint32_t div_reg = (div_int << 16) | (div_frac << 8);
    VGA_PIO->sm[vga_sm].clkdiv = div_reg;
    
    DBG_PRINT("  Clock divider reg: 0x%08x (int=%d, frac=%d)\n", div_reg, div_int, div_frac);
    
    pio_sm_set_enabled(VGA_PIO, vga_sm, true);
    
    // Initialize DMA
    dma_data_chan = dma_claim_unused_channel(true);
    dma_ctrl_chan = dma_claim_unused_channel(true);
    
    // Data channel
    dma_channel_config c0 = dma_channel_get_default_config(dma_data_chan);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);
    channel_config_set_read_increment(&c0, true);
    channel_config_set_write_increment(&c0, false);
    
    uint dreq = (VGA_PIO == pio0) ? DREQ_PIO0_TX0 + vga_sm : DREQ_PIO1_TX0 + vga_sm;
    channel_config_set_dreq(&c0, dreq);
    channel_config_set_chain_to(&c0, dma_ctrl_chan);
    
    dma_channel_configure(dma_data_chan, &c0, &VGA_PIO->txf[vga_sm],
                          lines_pattern[0], LINE_SIZE / 4, false);
    
    // Control channel
    dma_channel_config c1 = dma_channel_get_default_config(dma_ctrl_chan);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);
    channel_config_set_read_increment(&c1, false);
    channel_config_set_write_increment(&c1, false);
    channel_config_set_chain_to(&c1, dma_data_chan);
    
    dma_channel_configure(dma_ctrl_chan, &c1,
                          &dma_hw->ch[dma_data_chan].read_addr,
                          &lines_pattern[0], 1, false);
    
    // Set up interrupt with highest priority to prevent preemption
    // VGA timing is critical - the ISR must run within ~32us (one scanline)
    // to update the DMA read address before the next transfer starts.
    // Priority 0x00 = highest priority on ARM Cortex-M.
    irq_set_exclusive_handler(VGA_DMA_IRQ, dma_handler_vga);
    irq_set_priority(VGA_DMA_IRQ, 0x00);
    dma_channel_set_irq0_enabled(dma_ctrl_chan, true);
    irq_set_enabled(VGA_DMA_IRQ, true);
    
    // Start DMA
    dma_start_channel_mask(1u << dma_data_chan);
    
    DBG_PRINT("  VGA started (640x400 text mode, IRQ priority=0x00)!\n");
}

void vga_hw_set_mode(int mode) {
    if (mode != current_mode) {
        // Defer mode change to vblank to prevent signal glitches
        pending_mode = mode;
    }
}

void vga_hw_set_cursor(int x, int y, int start, int end, int char_height) {
    cursor_x = x;
    cursor_y = y;
    // Scale cursor scanlines from emulated char_height to our 16-line font
    // For example: if char_height=8 and cursor is at scanlines 6-7,
    // we scale to 12-15 for a 16-line font (preserving bottom position)
    if (char_height > 0 && char_height != 16) {
        cursor_start = start * 16 / char_height;
        cursor_end = (end + 1) * 16 / char_height - 1;
        if (cursor_end < cursor_start) cursor_end = cursor_start;
        if (cursor_end > 15) cursor_end = 15;
    } else {
        cursor_start = start;
        cursor_end = end;
    }
}

void vga_hw_set_cursor_blink(int blink_phase) {
    cursor_blink_state = blink_phase;
}

void vga_hw_set_vram_offset(uint16_t offset) {
    vram_offset = offset;
}

void vga_hw_set_panning(uint8_t panning) {
    pixel_panning = panning & 7;
}

void vga_hw_set_line_compare(int line) {
    line_compare = line;
}

// Pending frame state (submitted by Core 0)
static volatile bool new_frame_pending = false;
static volatile uint16_t pending_start_addr = 0;
static volatile uint8_t pending_panning = 0;
static volatile int pending_line_compare = -1;

void vga_hw_submit_frame(uint16_t start_addr, uint8_t panning, int line_compare) {
    if (vga_state_lock) {
        uint32_t flags = spin_lock_blocking(vga_state_lock);
        pending_start_addr = start_addr;
        pending_panning = panning;
        pending_line_compare = line_compare;
        new_frame_pending = true;
        spin_unlock(vga_state_lock, flags);
    } else {
        pending_start_addr = start_addr;
        pending_panning = panning;
        pending_line_compare = line_compare;
        new_frame_pending = true;
    }
}

// Update palette from emulator's 6-bit VGA DAC values
// palette_data is 768 bytes (256 entries × 3 bytes RGB, each 0-63)
// Uses dithering for ~2197 perceived colors from 64 actual colors
void vga_hw_set_palette(const uint8_t *palette_data) {
    for (int i = 0; i < 256; i++) {
        uint8_t r6 = palette_data[i * 3 + 0];
        uint8_t g6 = palette_data[i * 3 + 1];
        uint8_t b6 = palette_data[i * 3 + 2];
        vga_color_to_dithered(r6, g6, b6, i);
    }
}

// Update EGA 16-color palette from AC palette registers
// palette16_data is 48 bytes (16 entries × 3 bytes RGB, each 0-63)
void vga_hw_set_palette16(const uint8_t *palette16_data) {
    for (int i = 0; i < 16; i++) {
        uint8_t r6 = palette16_data[i * 3 + 0];
        uint8_t g6 = palette16_data[i * 3 + 1];
        uint8_t b6 = palette16_data[i * 3 + 2];
        ega_palette[i] = vga_color_to_output(r6, g6, b6);
    }
}

// Set graphics sub-mode: 1=CGA 4-color, 2=EGA planar, 3=VGA 256-color, 4=CGA 2-color
void vga_hw_set_gfx_mode(int submode, int width, int height, int line_offset) {
    gfx_submode = submode;
    gfx_width = width;
    gfx_height = height;
    gfx_line_offset = line_offset > 0 ? line_offset : (width / 8);
    gfx_sram_stride = (width / 8) + 1;
}

// Call this from main loop to update the SRAM buffers from PSRAM
// For text mode: copies text buffer during vblank
// For graphics mode: pre-renders scanlines ahead of the beam
void vga_hw_update(void) {
    // Don't update text buffer when OSD is visible (it reuses the same buffer)
    if (osd_is_visible()) return;
}

uint32_t vga_hw_get_frame_count(void) {
    return frame_count;
}

// Legacy API for compatibility
uint8_t *vga_hw_get_framebuffer(void) {
    return NULL;  // No framebuffer in this mode
}

void vga_hw_clear(uint8_t color) {
    (void)color;
    // No-op - VRAM is managed by emulator
}

void vga_hw_set_pixel(int x, int y, uint8_t color) {
    (void)x; (void)y; (void)color;
    // No-op - VRAM is managed by emulator
}
