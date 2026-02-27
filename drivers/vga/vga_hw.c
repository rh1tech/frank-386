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

bool SELECT_VGA = false;

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
// VGA Timing (640x480 @ 60Hz - standard? VGA text mode timing)
// ============================================================================
#ifndef VGA_SHIFT_PICTURE
#define VGA_SHIFT_PICTURE 144
#endif

#define VGA_CLK 25175000.0f

#define LINE_SIZE       800
#define N_LINES_TOTAL   525
#define N_LINES_VISIBLE 480
#define LINE_VS_BEGIN   490
#define LINE_VS_END     491

// Default active area for 400-line modes (text, CGA, EGA ≤400, mode 13h)
#define DEFAULT_ACTIVE_START  40
#define DEFAULT_ACTIVE_END    (DEFAULT_ACTIVE_START + 400)  // 440

// Dynamic active area — adjusted by vga_hw_set_gfx_mode() for 480-line modes
int active_start = DEFAULT_ACTIVE_START;
int active_end   = DEFAULT_ACTIVE_END;

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

// Visual debug overlay state (updated from emulator thread, read in ISR)
// Packed to avoid tearing: mode in bits 7-4, submode in bits 3-0
volatile uint8_t vdbg_mode_sub = 0x10; // default: mode=1, sub=0
volatile uint8_t vdbg_vblank = 0;      // last vblank state seen at 0x3DA read
volatile uint8_t vdbg_palette_ok = 0;  // set when palette first loaded
volatile uint8_t vdbg_vram_ok = 0;     // set when first gfx VRAM write seen

// Fast text palette for 2-bit pixel pairs
static uint16_t txt_palette_fast[256 * 4];

// Graphics palette (256 entries) - 16-bit dithered format
// Each entry: low byte = c_hi (conv0), high byte = c_lo (conv1)
// even - A, odd - B
uint16_t palette_a[512] __aligned(4);
static uint16_t* palette_b = &palette_a[256];

// 16-color EGA palette (for 16-color modes) - 8-bit VGA format with sync bits
static uint8_t ega_palette[16];

// CGA 4-color palette - 8-bit VGA format with sync bits
// Default to palette 1 high intensity: black, cyan, magenta, white
static uint8_t cga_palette[4];

// Current video mode (0=blank, 1=text, 2=graphics)
int current_mode = 1;  // Default text mode
static volatile int pending_mode = -1;  // Pending mode change (-1 = none)

// Graphics sub-mode: 1=CGA 4-color, 2=EGA planar, 3=VGA 256-color, 4=CGA 2-color
static int gfx_submode = 3;
static int gfx_width = 320;
static int gfx_height = 200;
static int gfx_line_offset = 40;  // Words per line (40 for 320px EGA, 80 for 640px)
static int gfx_sram_stride = 41;  // Words per line in SRAM buffer (width/8 + 1)

// Cursor state
int cursor_x = 0, cursor_y = 0;
int cursor_start = 0, cursor_end = 15;
int cursor_blink_state = 1;

// Direct pointer to VGA register state (set once by core0 after vga_init).
// ISR reads cr[], ar[] directly at the right moment — no volatile intermediates.
VGAState *vga_state = NULL;

// Per-frame values latched by ISR from vga_state->cr[] late in vblank
uint16_t frame_vram_offset   = 0;
uint8_t  frame_pixel_panning = 0;
int      frame_line_compare  = -1;

// Debug counters
static volatile uint32_t gfx_fallback_count = 0;


volatile int text_cols = 80;
// Stride in *character cells* (uint32_t per cell in gfx_buffer text layout).
// For VGA CRTC Offset (0x13): cells_per_row = cr13 * 2 (80-col -> 40*2, 40-col -> 20*2).
volatile int text_stride_cells = 80;

static volatile int pending_text_cols = 80;
static volatile int pending_text_stride = 80;
static volatile int text_geom_pending = 0;

inline static void vga_hw_submit_text_geom(int cols, int stride_cells) {
    if (cols != 40 && cols != 80)
        return;
    if (stride_cells <= 0 || stride_cells > 256)
        return;
    pending_text_cols = cols;
    pending_text_stride = stride_cells;
    text_geom_pending = 1;
}

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
static inline uint8_t vga_color_to_output(uint8_t r6, uint8_t g6, uint8_t b6) {
    // Map 6-bit VGA colors to 2-bit per channel (RRGGBB in bits 0-5)
    // Bits 6-7 are sync: 0xC0 = no sync pulses during active video
    uint8_t r2 = r6 >> 4;  // 6-bit to 2-bit
    uint8_t g2 = g6 >> 4;
    uint8_t b2 = b6 >> 4;
    return TMPL_LINE | (r2 << 4) | (g2 << 2) | b2;
}

void graphics_set_palette_hdmi(const uint8_t R, const uint8_t G, const uint8_t B,  uint8_t i);
// Convert 6-bit VGA DAC values to 16-bit dithered output
// Returns: low byte = c_hi (conv0), high byte = c_lo (conv1)
// When output as 16-bit, adjacent pixels get different colors for spatial dithering
static void vga_color_to_dithered(uint8_t r6, uint8_t g6, uint8_t b6, uint32_t idx) {
    if (!SELECT_VGA) {
        graphics_set_palette_hdmi(r6, g6, b6, idx);
        return;
    }
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
        0x00 | TMPL_LINE,  // 0: Black
        0x02 | TMPL_LINE,  // 1: Blue
        0x08 | TMPL_LINE,  // 2: Green
        0x0A | TMPL_LINE,  // 3: Cyan
        0x20 | TMPL_LINE,  // 4: Red
        0x22 | TMPL_LINE,  // 5: Magenta
        0x28 | TMPL_LINE,  // 6: Brown (dark yellow)
        0x2A | TMPL_LINE,  // 7: Light Gray
        0x15 | TMPL_LINE,  // 8: Dark Gray
        0x17 | TMPL_LINE,  // 9: Light Blue
        0x1D | TMPL_LINE,  // 10: Light Green
        0x1F | TMPL_LINE,  // 11: Light Cyan
        0x35 | TMPL_LINE,  // 12: Light Red
        0x37 | TMPL_LINE,  // 13: Light Magenta
        0x3D | TMPL_LINE,  // 14: Yellow
        0x3F | TMPL_LINE,  // 15: White
    };
    
    // Build fast palette for text rendering
    // Each entry handles 2 pixels (foreground/background combinations)
    // For 2-bit value from glyph: high bit is LEFT pixel, low bit is RIGHT pixel
    // When we extract (glyph >> 6) & 3, we get bits 7,6 where bit7=left, bit6=right
    // Index XY (X=bit7, Y=bit6): X is left pixel, Y is right pixel
    // Output 16-bit: low byte outputs first (left), high byte outputs second (right)
    for (int i = 0; i < 256; i++) {
        uint8_t fg = cga_colors[i & 0x0F];
        uint8_t bg = cga_colors[i >> 4];
        
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

// Render VGA 256-color planar (Mode X: 320x200x256, unchained)
// VRAM layout in our emulator: packed planes in dwords.
// Each dword holds 4 bytes: plane0..plane3, and those bytes are pixels x%4.
static void __time_critical_func(render_gfx_line_vga_planar256)(uint32_t line, uint32_t *output_buffer) {
    uint32_t *out32 = (uint32_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);

    int active_lines = active_end - active_start;
    uint32_t src_line = (gfx_height * 2 <= active_lines) ? (line >> 1) : line;

    if (src_line >= (uint32_t)gfx_height) {
        uint32_t blank = TMPL_LINE | (TMPL_LINE << 8) | (TMPL_LINE << 16) | (TMPL_LINE << 24);
        for (int i = 0; i < 160; i++) out32[i] = blank;
        return;
    }

    // CRTC Offset (CR13) in words -> dword stride
    uint32_t stride = (gfx_line_offset > 0) ? ((uint32_t)gfx_line_offset * 2u) : 80u;

    uint32_t base;
    // frame_line_compare is in display-line units (same as `line` parameter).
    // Compare against `line` (not src_line) to get the right split point.
    if (frame_line_compare >= 0 && (int)line >= frame_line_compare) {
        uint32_t lc_src = (gfx_height * 2 <= active_lines) ? (uint32_t)frame_line_compare >> 1
                                                           : (uint32_t)frame_line_compare;
        base = (src_line - lc_src) * stride;
    } else {
        base = frame_vram_offset + src_line * stride;
    }
    base &= 0xFFFF;

    // base is in dwords; gfx_buffer is bytes, so byte offset = base * 4
    const uint32_t *src32 = (const uint32_t *)(gfx_buffer + base * 4);
    // Select dither phase per scanline
    uint16_t *active_palette = (src_line & 1) ? palette_a : palette_b;
    // 320 pixels -> 80 dwords -> 160 output dwords (640px doubled)
    for (int i = 0; i < 80; i++) {
        uint32_t pixels = src32[i];
        uint16_t p0 = active_palette[pixels & 0xFF];
        uint16_t p1 = active_palette[(pixels >> 8) & 0xFF];
        uint16_t p2 = active_palette[(pixels >> 16) & 0xFF];
        uint16_t p3 = active_palette[(pixels >> 24) & 0xFF];
        *out32++ = (uint32_t)p0 | ((uint32_t)p1 << 16);
        *out32++ = (uint32_t)p2 | ((uint32_t)p3 << 16);
    }
}

// Render graphics line directly from SRAM framebuffer (for IRQ use)
// Uses dithered 16-bit palette for ~2197 perceived colors
static void __time_critical_func(render_gfx_line_from_sram)(uint32_t line, uint32_t *output_buffer) {
    uint32_t *out32 = (uint32_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);

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
        // Read from VRAM (stable during active video)
        // Stride comes from CRTC Offset (CR13) which is in words for VGA.
        // We use 32-bit words for fetch, so convert words->dwords.
        uint32_t stride = (gfx_line_offset > 0) ? ((uint32_t)gfx_line_offset * 2u) : 80u;
        uint32_t offset;
        if (frame_line_compare >= 0 && src_line >= (uint32_t)frame_line_compare) {
            offset = (src_line - frame_line_compare) * stride;
        } else {
            offset = frame_vram_offset + src_line * stride;
        }

        offset &= 0xFFFF;

        // offset is in dwords; gfx_buffer is bytes, so byte offset = offset * 4
        const uint32_t *src32 = (const uint32_t *)(gfx_buffer + offset * 4);
        // Select dither phase per scanline
        uint16_t *active_palette = (src_line & 1) ? palette_a : palette_b;
        // 320 pixels -> 80 dwords
        for (int i = 0; i < 80; i++) {
            uint32_t pixels = src32[i];
            uint16_t p0 = active_palette[pixels & 0xFF];
            uint16_t p1 = active_palette[(pixels >> 8) & 0xFF];
            uint16_t p2 = active_palette[(pixels >> 16) & 0xFF];
            uint16_t p3 = active_palette[(pixels >> 24) & 0xFF];
            *out32++ = (uint32_t)p0 | ((uint32_t)p1 << 16);
            *out32++ = (uint32_t)p2 | ((uint32_t)p3 << 16);
        }
    }
}

// Render CGA 4-color graphics line (320x200, 2 bits per pixel, interleaved)
// VGA stores CGA data in odd/even mode with interleaved planes
static void __time_critical_func(render_gfx_line_cga)(uint32_t line, uint32_t *output_buffer) {
    uint32_t *out32 = (uint32_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);

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
    uint32_t *out32 = (uint32_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);

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
    uint32_t *out32 = (uint32_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);

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
        // 350-line mode: map active display lines to 350 source lines
        // Scale: src = line * 350 / 400 = line * 7 / 8
        int ega_active_lines = active_end - active_start;
        src_line = (line * height) / ega_active_lines;
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

    const uint32_t *src32 = (const uint32_t *)(gfx_buffer + offset * 4);
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

// 80 cols: one uint16 = 2 pixels (left in low byte, right in high byte)
// 40 cols: need true 2x horizontal scaling per pixel: A B -> A A B B
static inline void __time_critical_func(out16_2x_per_pixel)(uint16_t **pp, uint16_t v) {
    uint16_t *p = *pp;
    uint8_t a = (uint8_t)(v & 0xFF);
    uint8_t b = (uint8_t)(v >> 8);
    *p++ = (uint16_t)a | ((uint16_t)a << 8);  // A A
    *p++ = (uint16_t)b | ((uint16_t)b << 8);  // B B
    *pp = p;
}

// Helper function to render a single text line
static void __time_critical_func(render_text_line)(uint32_t line, uint32_t *output_buffer) {
    uint16_t *out16 = (uint16_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);

    uint32_t char_row = line / 16;
    uint32_t glyph_line = line & 15;

    int cols = text_cols;
    int double_h = (cols == 40);  // 40 columns => 2x horizontal scaling

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
            if (!double_h) {
                v = pal[glyph & 3];           *out16++ = v;
                v = pal[(glyph >> 2) & 3];    *out16++ = v;
                v = pal[(glyph >> 4) & 3];    *out16++ = v;
                v = pal[(glyph >> 6) & 3];    *out16++ = v;
            } else {
                // true per-pixel doubling: (A,B) -> (A,A,B,B)
                v = pal[glyph & 3];           out16_2x_per_pixel(&out16, v);
                v = pal[(glyph >> 2) & 3];    out16_2x_per_pixel(&out16, v);
                v = pal[(glyph >> 4) & 3];    out16_2x_per_pixel(&out16, v);
                v = pal[(glyph >> 6) & 3];    out16_2x_per_pixel(&out16, v);
            }
        }
    }
}
#if DBG_OVERLAY
// ============================================================================
// Visual Debug Overlay
// Draws a 16-pixel status bar at the top of the screen showing:
//   - Colored block for mode (blue=text, green=gfx, red=blank)
//   - Colored block for submode (width encodes value 0-9)
//   - Colored block for palette_ok (dark/bright)
//   - Colored block for vram_ok (dark/bright)
//   - Frame counter as a scrolling brightness band
// All output is in native format: TMPL_LINE | R2G2B2
// ============================================================================

// 4x5 mini-digits: each digit is 5 rows of 4 bits (MSB=left)
static const uint8_t mini_digits[10][5] = {
    {0b1110, 0b1010, 0b1010, 0b1010, 0b1110}, // 0
    {0b0100, 0b1100, 0b0100, 0b0100, 0b1110}, // 1
    {0b1110, 0b0010, 0b1110, 0b1000, 0b1110}, // 2
    {0b1110, 0b0010, 0b1110, 0b0010, 0b1110}, // 3
    {0b1010, 0b1010, 0b1110, 0b0010, 0b0010}, // 4
    {0b1110, 0b1000, 0b1110, 0b0010, 0b1110}, // 5
    {0b1110, 0b1000, 0b1110, 0b1010, 0b1110}, // 6
    {0b1110, 0b0010, 0b0100, 0b0100, 0b0100}, // 7
    {0b1110, 0b1010, 0b1110, 0b1010, 0b1110}, // 8
    {0b1110, 0b1010, 0b1110, 0b0010, 0b1110}, // 9
};

static void __time_critical_func(render_vdbg_line)(uint32_t line, uint32_t *output_buffer) {
    if (line >= 16) return;

    uint8_t *out = (uint8_t *)output_buffer + SHIFT_PICTURE;

    // Background: black with sync bits
    uint8_t bg = TMPL_LINE;
    for (int i = 0; i < 640; i++) out[i] = bg;

    if (line == 0 || line == 15) {
        // Top/bottom border: white line
        uint8_t w = TMPL_LINE | 0x3F;
        for (int i = 0; i < 640; i++) out[i] = w;
        return;
    }

    // Row 1-14 (line 1..14): draw info blocks
    uint32_t row = line - 1; // 0..13

    // --- Block 0 (x=2..17): Mode indicator ---
    // Shows current_mode as seen by the ISR renderer (not the requested mode)
    // Blue = text (1), Green = graphics (2), Red = blank/other, Magenta = pending≠current
    uint8_t mode_now = current_mode & 0xF;
    uint8_t pend = (pending_mode >= 0) ? 1 : 0;
    uint8_t mode_col;
    if      (mode_now == 1 && !pend) mode_col = TMPL_LINE | 0x03;       // blue = text, settled
    else if (mode_now == 1 &&  pend) mode_col = TMPL_LINE | 0x23;       // cyan-blue = text, pending change
    else if (mode_now == 2) mode_col = TMPL_LINE | 0x0C;       // green = graphics
    else                    mode_col = TMPL_LINE | 0x30;       // red = blank/other
    for (int x = 2; x < 18; x++) out[x] = mode_col;

    // --- Block 1 (x=20..35): Sub-mode + pending_mode indicator ---
    // Left 8px: pending_mode value (0=none=dark, 1=bright, 2=green, etc)
    // Right 8px: gfx_submode bar
    uint8_t pend_col;
    if      (pending_mode < 0)  pend_col = TMPL_LINE | 0x00; // black = no pending
    else if (pending_mode == 1) pend_col = TMPL_LINE | 0x03; // blue = pending text
    else if (pending_mode == 2) pend_col = TMPL_LINE | 0x0C; // green = pending gfx
    else                        pend_col = TMPL_LINE | 0x30; // red = pending blank
    for (int x = 20; x < 28; x++) out[x] = pend_col;

    uint8_t sub = gfx_submode & 0xF;
    uint8_t sub_dim  = TMPL_LINE | 0x08; // dark yellow
    uint8_t sub_brt  = TMPL_LINE | 0x3C; // bright yellow
    for (int x = 28; x < 38; x++) {
        int seg = x - 28;
        out[x] = (seg < (int)(sub * 2)) ? sub_brt : sub_dim;
    }

    // --- Block 2 (x=38..45): Palette OK ---
    uint8_t pal_col = vdbg_palette_ok ? (TMPL_LINE | 0x3F) : (TMPL_LINE | 0x04);
    for (int x = 38; x < 46; x++) out[x] = pal_col;

    // --- Block 3 (x=48..55): VRAM write seen ---
    uint8_t vram_col = vdbg_vram_ok ? (TMPL_LINE | 0x3F) : (TMPL_LINE | 0x04);
    for (int x = 48; x < 56; x++) out[x] = vram_col;

    // --- Block 4 (x=58..65): vblank state ---
    uint8_t vbl_col = vdbg_vblank ? (TMPL_LINE | 0x30) : (TMPL_LINE | 0x03);
    for (int x = 58; x < 66; x++) out[x] = vbl_col;

    // --- Digits: show frame_vram_offset (6 hex digits) so we know where renderer reads ---
    if (row >= 4 && row <= 8) {
        uint32_t drow = row - 4;
        // Show frame_vram_offset as 4 hex digits, then gfx_submode and gfx_line_offset
        uint32_t val = ((uint32_t)frame_vram_offset << 16) |
                       ((uint32_t)(gfx_submode & 0xF) << 8) |
                       (uint32_t)(gfx_line_offset & 0xFF);
        uint8_t hexdigits[6];
        for (int d = 5; d >= 0; d--) {
            hexdigits[d] = val & 0xF;
            val >>= 4;
        }
        // Use mini_digits for 0-9, treat A-F as 0-9 too (close enough)
        for (int d = 0; d < 6; d++) {
            uint8_t bits = mini_digits[hexdigits[d] & 9][drow];
            int x0 = 70 + d * 6;
            for (int b = 0; b < 4; b++) {
                uint8_t px = ((bits >> (3 - b)) & 1) ?
                             (TMPL_LINE | 0x3F) :
                             (TMPL_LINE | 0x00);
                if (x0 + b < 640) out[x0 + b] = px;
            }
        }
    }
}
#endif

void __time_critical_func(pre_render_line)(void) {
    // Check retrace and submit frame (fast path)
    // We must check this frequently to catch the VBLANK edge
    static bool was_in_retrace = false;
    bool in_retrace = vga_in_retrace(vga_state);
    // Latch values at the END of retrace (falling edge of VBLANK)
    if (was_in_retrace && !in_retrace) {
        // Text geometry:
        // - visible cols from CRTC 0x01 (40/80)
        // - stride in cells from CRTC 0x13 (offset) * 2
        int cols = vga_get_text_cols(vga_state);
        int cr13 = vga_get_line_offset(vga_state);   // CRTC offset register
        int stride_cells = cr13 * 2;
        vga_hw_submit_text_geom(cols, stride_cells);
    }
    was_in_retrace = in_retrace;
}

// Dispatch to appropriate renderer based on current mode
static void __time_critical_func(render_line)(uint32_t line, uint32_t *output_buffer) {
    pre_render_line();
#if DBG_OVERLAY
    // Visual debug overlay: always show status in top 16 scanlines
    if (line < 16) {
        render_vdbg_line(line, output_buffer);
        return;
    }
#endif
    // --- Верхнее поле ---
    if (line < (uint32_t)active_start) {
        uint32_t blank = TMPL_LINE | (TMPL_LINE<<8) | (TMPL_LINE<<16) | (TMPL_LINE<<24);
        uint32_t *out32 = (uint32_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);
        for (int i = 0; i < 160; i++)
            out32[i] = blank;
        return;
    }

    // --- Нижнее поле ---
    if (line >= (uint32_t)active_end) {
        uint32_t blank = TMPL_LINE | (TMPL_LINE<<8) | (TMPL_LINE<<16) | (TMPL_LINE<<24);
        uint32_t *out32 = (uint32_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);
        for (int i = 0; i < 160; i++)
            out32[i] = blank;
        return;
    }

    // --- Активная зона 640×400 ---
    line -= active_start;
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
        } else if (gfx_submode == 5) {
            // VGA 256-color planar (Mode X)
            render_gfx_line_vga_planar256(line, output_buffer);
        } else {
            // VGA 256-color (mode 13h) - default
            render_gfx_line_from_sram(line, output_buffer);
        }
        return;
    }
    // mode 0 = blanked (AR bit5 cleared during BIOS mode transitions).
    // Emit black pixels with sync bits so the monitor sees a valid signal.
    uint32_t *out32 = (uint32_t *)((uint8_t *)output_buffer + SHIFT_PICTURE);
    uint32_t blank = TMPL_LINE | (TMPL_LINE << 8) | (TMPL_LINE << 16) | (TMPL_LINE << 24);
    for (int i = 0; i < 160; i++) out32[i] = blank;
}

static inline void vga_hw_set_mode(int mode);

void __time_critical_func(vga_hw_new_frame)(void) {
    static int last_vga_mode = -1;
    // Update cursor
    int cx, cy, cs, ce, cv;
    vga_get_cursor_info(vga_state, &cx, &cy, &cs, &ce, &cv);
    int char_height = vga_get_char_height(vga_state);
    if (cv) {
        vga_hw_set_cursor(cx, cy, cs, ce, char_height);
        // Sync cursor blink phase with emulator
        cursor_blink_state = vga_get_cursor_blink_phase(vga_state);
    } else {
        vga_hw_set_cursor(-1, -1, 0, 0, 16);  // Hide cursor
    }

    // Update VGA mode
    int vga_mode = vga_get_mode(vga_state);
    if (vga_mode != last_vga_mode) {
        vga_hw_set_mode(vga_mode);
        last_vga_mode = vga_mode;
    }

    // Update palette and graphics submode for graphics modes
    if (vga_mode == 2) {
        // Only update palette when it actually changed
        if (vga_is_palette_dirty(vga_state)) {
            vga_hw_set_palette(vga_get_palette(vga_state));
        }

        int gfx_w, gfx_h;
        int gfx_submode = vga_get_graphics_mode(vga_state, &gfx_w, &gfx_h);
        int line_offset = vga_get_line_offset(vga_state);
        static int last_submode = -1;
        if (gfx_submode != last_submode) {
            last_submode = gfx_submode;
        }
        vga_hw_set_gfx_mode(gfx_submode, gfx_w, gfx_h, line_offset);

        // For EGA mode, also update the 16-color palette
        if (gfx_submode == 2) {
            uint8_t ega_pal[48];
            vga_get_palette16(vga_state, ega_pal);
            vga_hw_set_palette16(ega_pal);
        }
    }
}

static void __isr __time_critical_func(dma_handler_vga)(void) {
    dma_hw->ints0 = 1u << dma_ctrl_chan;
    
    current_line++;
    if (current_line >= N_LINES_TOTAL) {
        current_line = 0;
        frame_count++;
        vga_hw_new_frame();
    }
    
    in_vblank = (current_line >= N_LINES_VISIBLE);
    gfx_copy_allowed = in_vblank;

    // Update VGA status register 1 (port 0x3DA) from ISR — this is the
    // authoritative source. Core0 reads it as-is without any logic.
    // Bit 0 (DISP_ENABLE): 1 = active display, 0 = blanking interval
    // Bit 3 (V_RETRACE):   1 = vertical retrace, 0 = active display
    if (vga_state) {
        if (in_vblank) {
            vga_state->st01 |=  ST01_V_RETRACE;
            vga_state->st01 &= ~ST01_DISP_ENABLE;
        } else {
            vga_state->st01 &= ~ST01_V_RETRACE;
            vga_state->st01 |=  ST01_DISP_ENABLE;
        }
    }

    // Line N_LINES_VISIBLE (400): start of vblank.
    // Apply pending mode/geometry changes. Do NOT latch vram_offset here —
    // Wolf3D writes the new CRTC start address during vblank, after this point.
    if (current_line == N_LINES_VISIBLE) {
        if (text_geom_pending) {
            text_cols        = pending_text_cols;
            text_stride_cells = pending_text_stride;
            text_geom_pending = 0;
        }
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

        // Line N_LINES_TOTAL-4 (521): late in vblank, just before DMA needs line 0.
        // Wolf3D has already written the new page address to CRTC by now.
        // Read cr[] and ar[] directly — no intermediate volatile copies.
        if (current_line == N_LINES_TOTAL - 4) {
            if (vga_state) {
                const uint8_t *cr = vga_state->cr;
                frame_vram_offset = (uint16_t)((cr[0x0c] << 8) | cr[0x0d]);
                frame_pixel_panning = vga_state->ar[0x13] & 0x07;
                int lc = (int)cr[0x18]
                       | (((int)cr[0x07] & 0x10) << 4)
                       | (((int)cr[0x09] & 0x40) << 3);
                frame_line_compare = (lc > 0 && lc < N_LINES_VISIBLE) ? lc : -1;
            }
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
int testPins(uint32_t pin0, uint32_t pin1);
void graphics_init_hdmi();
void vga_hw_init(void) {
    uint8_t linkVGA01 = testPins(VGA_BASE_PIN, VGA_BASE_PIN + 1);
    #if defined(BOARD_Z0) || defined(BOARD_Z2) || defined(BOARD_DV)
        SELECT_VGA = linkVGA01 == 0x1F;
    #else
        SELECT_VGA = (linkVGA01 == 0) || (linkVGA01 == 0x1F);
    #endif
    if (!SELECT_VGA) {
        graphics_init_hdmi();
        return;
    }
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

static inline void vga_hw_set_mode(int mode) {
    // Ignore mode 0 (blank): this is a transient state the BIOS sets
    // while reprogramming registers during mode switches.  If we apply
    // it we permanently black out the display until the next reboot.
    // The render_line() fallback already outputs a valid blank scanline
    // for current_mode==0 so the monitor signal stays clean.
    if (mode == 0) return;
    if (mode == 1) {
        active_start = DEFAULT_ACTIVE_START;
        active_end = DEFAULT_ACTIVE_END;
    }
    // Update visual debug: mode in high nibble, keep submode in low nibble
    vdbg_mode_sub = (uint8_t)((mode << 4) | (vdbg_mode_sub & 0xF));
    if (mode != current_mode) {
        // Defer mode change to vblank to prevent signal glitches
        pending_mode = mode;
    }
}

void __time_critical_func(vga_hw_set_cursor)(int x, int y, int start, int end, int char_height) {
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

// These setters are no longer used — ISR reads VGA registers directly.
// Kept as stubs so any remaining callers still compile.
void vga_hw_set_vram_offset(uint16_t offset)  { (void)offset; }
void vga_hw_set_panning(uint8_t panning)       { (void)panning; }
void vga_hw_set_line_compare(int line)          { (void)line; }

void vga_hw_set_vga_state(VGAState *s) {
    vga_state = s;
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
    vdbg_palette_ok = 1;
}

// Update EGA 16-color palette from AC palette registers
// palette16_data is 48 bytes (16 entries × 3 bytes RGB, each 0-63)
void __time_critical_func(vga_hw_set_palette16)(const uint8_t *palette16_data) {
    for (int i = 0; i < 16; i++) {
        uint8_t r6 = palette16_data[i * 3 + 0];
        uint8_t g6 = palette16_data[i * 3 + 1];
        uint8_t b6 = palette16_data[i * 3 + 2];
        ega_palette[i] = vga_color_to_output(r6, g6, b6);
    }
}

// Set graphics sub-mode: 1=CGA 4-color, 2=EGA planar, 3=VGA 256-color, 4=CGA 2-color
void __time_critical_func(vga_hw_set_gfx_mode)(int submode, int width, int height, int line_offset) {
    vdbg_mode_sub = (uint8_t)((2 << 4) | (submode & 0xF));
    gfx_submode = submode;
    gfx_width = width;
    gfx_height = height;
    gfx_line_offset = line_offset > 0 ? line_offset : (width / 8);
    gfx_sram_stride = (width / 8) + 1;

    // Adjust active display area based on mode requirements
    if (height > 400 || (submode == 5 && height > 200)) {
        // 480-line modes: mode 12h (height=480) or Mode X 320×240 (height=240, doubled)
        active_start = 0;
        active_end = N_LINES_VISIBLE;  // 480
    } else {
        active_start = DEFAULT_ACTIVE_START;
        active_end = DEFAULT_ACTIVE_END;
    }
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
