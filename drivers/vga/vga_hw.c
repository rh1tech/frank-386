/**
 * frank-386 - i386 PC Emulator for RP2350
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
#include "hardware/timer.h"
#include "hardware/vreg.h"
#include "pico/time.h"
#include <arm_acle.h>
#include "../../drivers/psram/psram_init.h"

// Portable 8-bit bit reversal. GCC 13 does not expose __rbit() through
// <arm_acle.h> for this build configuration, so avoid relying on that intrinsic.
static inline uint8_t reverse_bits8(uint8_t v) {
    v = (uint8_t)((v >> 4) | (v << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}

bool SELECT_VGA = false;
extern bool required_to_repair_text_pal;

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

// Text buffer in SRAM (non-static to allow OSD reuse when paused)
uint8_t text_buffer_sram[80 * 25 * 2] __attribute__((aligned(4)));
static volatile int update_requested = 0;  // Set by update call

/* The guest's VGA RAM.  Was hardcoded at 256 KB in two files while
 * EMU_VGA_MEM_SIZE_KB configured a third place; deriving it means the buffer
 * and the size the emulator reports can no longer disagree. */
#ifndef EMU_VGA_MEM_SIZE_KB
#define EMU_VGA_MEM_SIZE_KB 256
#endif
#define GFX_BUFFER_SIZE (EMU_VGA_MEM_SIZE_KB * 1024)
uint8_t gfx_buffer[GFX_BUFFER_SIZE] __attribute__((aligned(4)));

/*
 * HDMI EGA 320x200 frame cache. The guest's physical 0xa0000-0xbffff
 * aperture is redirected to gfx_buffer, so its PSRAM shadow is unused apart
 * from the first 2 KiB reserved by the OPL3 emulation. Two 32 KiB slots here
 * let core 0 build one complete planar->chunky frame while HDMI reads the
 * other; the DMA ISR publishes a completed slot only during vblank.
 */
#define HDMI_EGA320_CACHE_BYTES (320u * 200u / 2u)
#define HDMI_EGA320_CACHE0 ((uint32_t *)(PSRAM_BASE_ADDR + 0x000a8000u))
#define HDMI_EGA320_CACHE1 ((uint32_t *)(PSRAM_BASE_ADDR + 0x000b0000u))
const uint32_t * volatile hdmi_ega320_cache_active = NULL;
const uint32_t * volatile hdmi_ega320_cache_pending = NULL;

// Fast text palette for 2-bit pixel pairs
extern uint32_t conv_color2[1024]; // 4096 in hdmi only
static uint16_t* txt_palette_fast = (uint16_t*)&conv_color2[0]; ///[256 * 4]; 2048, reusing

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

// Graphics sub-mode: 1=CGA 4-color, 2=EGA planar, 3=VGA 256-color, 4=CGA 2-color
int gfx_submode = 3;
int gfx_width = 320;
int gfx_height = 200;
int gfx_line_offset = 40;  // Words per line (40 for 320px EGA, 80 for 640px)
int gfx_sram_stride = 41;  // Words per line in SRAM buffer (width/8 + 1)

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
/*
 * Attribute Mode Control (AR10) bit 5, latched with the rest of the frame.
 *
 * A split screen exists so the bottom strip stays put while the top scrolls,
 * and this bit is how the hardware is told to leave it alone: with it set,
 * a successful line compare forces the pixel panning register to zero for
 * the split region.  Applying panning there too makes the strip slide with
 * the scroll - Supaplex's status panel wobbles left and right instead of
 * being anchored.
 */
uint8_t  frame_panning_split_off = 0;
/*
 * CRTC Mode Control (CR17) and the maximum scan line (CR09 bits 4-0).
 *
 * CR17 bits 0 and 1 are the CGA/EGA compatibility bits: with bit 0 clear the
 * hardware substitutes bit 0 of the row-scan counter for address bit 13, and
 * with bit 1 clear it does the same for bit 14 with row-scan bit 1.  That is
 * how the old interleaved banks worked, and software still uses it as a cheap
 * address wrap.
 *
 * Prehistorik 2's menu screens do exactly that: they keep an 8 KB ring and
 * scroll the start address through it, relying on bit 13 being forced to zero
 * so the window folds back to the top of the pattern.  Without it the bottom
 * of the screen walks off into memory the game never draws, and the pattern
 * only reappears when the scroll offset happens to be small enough.
 */
uint8_t  frame_crtc_mode = 0xe3;   /* CR17; default = no compatibility wrap */
uint8_t  frame_max_scan   = 0;     /* CR09 bits 4-0 */
/*
 * Overscan colour (AR11), latched with the rest of the frame.
 *
 * A smooth-scrolling mode shows fewer pixels than the line buffer holds -
 * Prehistorik 2 displays 312 of 320 so it has eight spare for pixel panning -
 * and the renderers only ever wrote the visible part, leaving the tail of
 * each line holding whatever the previous line left there.  On a scrolling
 * screen that is a column of moving rubbish down the right edge.  Real
 * hardware shows the overscan colour outside the active display.
 */
uint8_t  frame_overscan   = 0;     /* AR11 */

/*
 * Row-scan preset (CR08 bits 4-0) and character height (CR09), latched with
 * the rest of the frame.
 *
 * The preset is the row-scan counter's starting value, which is how text mode
 * scrolls vertically by less than a whole character: the first displayed row
 * begins part way down its glyph.  The character height has to come from the
 * CRTC as well - the renderer used to assume 16 unconditionally, which is only
 * true of the modes the BIOS happens to set.
 */
/*
 * The pixel value to paint where the guest's picture is not.
 *
 * The board sends 640x480 while a 200-line mode is 312x400, so the picture is
 * letterboxed - forty lines above it, forty below, and sixteen output pixels
 * down the right where the guest displays only 39 of 40 character clocks.
 * Those margins were filled with 0, which is a *palette index*, not a colour:
 * Prehistorik 2's map screen defines colour 0 as blue, so its picture came
 * framed in three blue bands instead of black.
 *
 * None of that region exists on real hardware - a real monitor shows those
 * 400 lines full height - so the honest thing to paint there is black.  This
 * holds whichever palette entry is actually darkest, packed the way the
 * current mode packs pixels into a byte (twice per byte in 16-colour and text
 * modes, once in 256-colour), and is updated whenever a palette is set.
 */
uint8_t  frame_border_pix  = 0;
uint8_t  frame_border_rgb[3] = { 0, 0, 0 };

/*
 * Honour the attribute controller's blank (PAS bit clear).  A game blanks the
 * display while it reloads the DAC and redraws, and a real VGA shows nothing
 * for that time.  We used to ignore it and keep scanning out, so the previous
 * screen's pixels became visible under the incoming palette - Prehistorik 2's
 * MODE BEGINNER screen reappearing in the level's salmon palette for about a
 * second on the way into the level.
 *
 * vga_hw_set_mode() still ignores mode 0, so the mode itself survives the
 * blank; only the scanout is blanked, and only for a bounded time.  The bound
 * is the point: a guest that leaves PAS clear must not be able to black the
 * board out until the next reboot, which is what made the blank be ignored in
 * the first place.
 */
#define BLANK_MAX_FRAMES 120u          /* ~2 s at 60 Hz */
volatile uint8_t  frame_blank_active = 0;
static   uint32_t frame_blank_frames = 0;
uint8_t  frame_preset_row  = 0;
uint8_t  frame_text_char_h = 16;

int text_cols = 80;
// Stride in *character cells* (uint32_t per cell in gfx_buffer text layout).
// For VGA CRTC Offset (0x13): cells_per_row = cr13 * 2 (80-col -> 40*2, 40-col -> 20*2).
int text_stride_cells = 80;

inline static void vga_hw_submit_text_geom(int cols, int stride_cells) {
    if (cols != 40 && cols != 80)
        return;
    if (stride_cells <= 0 || stride_cells > 256)
        return;
    text_cols = cols;
    text_stride_cells = stride_cells;
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

static inline int c6_to_8(int v)
{
    v &= 0x3f;
    int b = v & 1;
    return (v << 2) | (b << 1) | b;
}

void graphics_set_palette_hdmi(const uint8_t R, const uint8_t G, const uint8_t B,  uint8_t i);
void graphics_set_palette_hdmi2(
    const uint8_t R1, const uint8_t G1, const uint8_t B1,
    const uint8_t R2, const uint8_t G2, const uint8_t B2,
    uint8_t i
);
void hdmi_set_pixel_substitutes(const uint8_t *sub);

/*
 * Four byte values - 0xd4..0xd7, see HDMI_CTRL_0 in hdmi.c - are HDMI control
 * symbols and can never leave the renderer as pixels, so it emits a substitute
 * byte for them.  Which substitute is a free choice, and the only sensible one
 * is the byte that looks most like what the guest asked for, so it has to be
 * recomputed whenever the palette changes.  Dune II draws its Westwood logo
 * entirely in indices 212..215; with a fixed substitute the logo was white.
 */
#define HDMI_RESERVED_LO   0xd4u
#define HDMI_IS_RESERVED(b) (((b) & 0xfcu) == HDMI_RESERVED_LO)

static inline int pal_dist6(const uint8_t *a, const uint8_t *b) {
    int dr = (int)a[0] - (int)b[0];
    int dg = (int)a[1] - (int)b[1];
    int db = (int)a[2] - (int)b[2];
    return dr * dr + dg * dg + db * db;
}

/* 256-colour modes: the byte is the palette index, so compare entries. */
static void hdmi_pick_substitutes_256(const uint8_t *pal) {
    uint8_t sub[4];
    for (int k = 0; k < 4; k++) {
        const uint8_t *want = pal + (HDMI_RESERVED_LO + k) * 3;
        int best = 0, bestd = 1 << 30;
        for (int j = 0; j < 256; j++) {
            if (HDMI_IS_RESERVED(j)) continue;
            int d = pal_dist6(pal + j * 3, want);
            if (d < bestd) {
                bestd = d;
                best = j;
                if (!d) break;     /* an exact duplicate is as good as it gets */
            }
        }
        sub[k] = (uint8_t)best;
    }
    hdmi_set_pixel_substitutes(sub);
}

/*
 * Text mode and EGA 640 pack two 4-bit indices into the byte, so the
 * replacement has to look like both halves at once.  Searching all 256 bytes
 * rather than each nibble separately keeps the reserved range excluded for
 * free.
 */
static void hdmi_pick_substitutes_pair(const uint8_t *pal16) {
    uint8_t sub[4];
    for (int k = 0; k < 4; k++) {
        int want = HDMI_RESERVED_LO + k;
        int best = 0, bestd = 1 << 30;
        for (int j = 0; j < 256; j++) {
            if (HDMI_IS_RESERVED(j)) continue;
            int d = pal_dist6(pal16 + (j >> 4) * 3, pal16 + (want >> 4) * 3)
                  + pal_dist6(pal16 + (j & 15) * 3, pal16 + (want & 15) * 3);
            if (d < bestd) {
                bestd = d;
                best = j;
                if (!d) break;
            }
        }
        sub[k] = (uint8_t)best;
    }
    hdmi_set_pixel_substitutes(sub);
}

// Convert 6-bit VGA DAC values to 16-bit dithered output
// Returns: low byte = c_hi (conv0), high byte = c_lo (conv1)
// When output as 16-bit, adjacent pixels get different colors for spatial dithering
static void vga_color_to_dithered(uint8_t r6, uint8_t g6, uint8_t b6, uint32_t idx) {
    if (!SELECT_VGA) {
        graphics_set_palette_hdmi(c6_to_8(r6), c6_to_8(g6), c6_to_8(b6), idx);
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
static __attribute__((always_inline)) inline uint32_t spread8(uint32_t plane) {
    plane = (plane | (plane << 12)) & 0x000F000Fu;
    plane = (plane | (plane <<  6)) & 0x03030303u;
    plane = (plane | (plane <<  3)) & 0x11111111u;
    return plane;
}

// Merge 4 plane bytes [P3|P2|P1|P0] into 8 nibbles (pixel color indices).
static inline uint32_t ega_pack8_from_planes(const uint32_t ega_planes) {
    const uint32_t pixel1 = spread8(ega_planes        & 0xFFu);
    const uint32_t pixel2 = spread8((ega_planes >> 8) & 0xFFu);
    const uint32_t pixel3 = spread8((ega_planes >> 16) & 0xFFu);
    const uint32_t pixel4 = spread8(ega_planes >> 24);

    return pixel1 | pixel2 << 1 | pixel3 << 2 | pixel4 << 3;
}

static void __attribute__((noinline)) hdmi_build_ega320_cache(void) {
#if !EGA320_PSRAM_CACHE
    // A/B path: scanout converts the authoritative internal-SRAM VGA aperture
    // directly.  Keep both publication pointers clear so no stale PSRAM frame
    // can be selected after a mode change or reboot.
    hdmi_ega320_cache_pending = NULL;
    hdmi_ega320_cache_active = NULL;
    return;
#else
    static uint32_t *build_dst = NULL;
    static uint32_t build_next_y = 0;
    static uint32_t build_stride = 40;
    static uint32_t build_start = 0;
    static uint32_t build_panning = 0;
    static int build_line_compare = -1;

    if (SELECT_VGA || current_mode != 2 || gfx_submode != 6 ||
        gfx_width != 320 || gfx_height != 200) {
        hdmi_ega320_cache_pending = NULL;
        hdmi_ega320_cache_active = NULL;
        build_dst = NULL;
        build_next_y = 0;
        return;
    }

    if (!build_dst) {
        const uint32_t *active = hdmi_ega320_cache_active;
        build_dst = (active == HDMI_EGA320_CACHE0)
                    ? HDMI_EGA320_CACHE1 : HDMI_EGA320_CACHE0;
        build_next_y = 0;
        build_stride = gfx_line_offset > 0
                       ? (uint32_t)gfx_line_offset << 1 : 40u;
        build_start = frame_vram_offset;
        build_panning = frame_pixel_panning;
        build_line_compare = frame_line_compare;
    }

    uint32_t first_y = build_next_y;
    uint32_t end_y = first_y + 100u;
    uint32_t shift1 = build_panning << 2;
    uint32_t shift2 = 32u - shift1;

    for (uint32_t y = first_y; y < end_y; ++y) {
        uint32_t offset;
        if (build_line_compare >= 0 && y >= (uint32_t)build_line_compare)
            offset = (y - (uint32_t)build_line_compare) * build_stride;
        else
            offset = build_start + y * build_stride;
        offset &= 0xffffu;

        const uint32_t *src = (const uint32_t *)(gfx_buffer + (offset << 2));
        uint32_t *row = build_dst + y * 40u;
        for (uint32_t x = 0; x < 40u; ++x) {
            uint32_t packed = ega_pack8_from_planes(src[x]);
            if (build_panning)
                packed = (packed << shift1) |
                         (ega_pack8_from_planes(src[x + 1u]) >> shift2);
            row[x] = packed;
        }
    }

    if (end_y == 200u) {
        __dmb();
        hdmi_ega320_cache_pending = build_dst;
        build_dst = NULL;
        build_next_y = 0;
    } else {
        build_next_y = end_y;
    }
#endif
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

    /*
     * Split screen.  frame_line_compare comes from the CRTC and is counted in
     * *display* lines, while src_line is counted in source lines - in a
     * 320x200 mode the display has 400 lines and each source line is shown
     * twice.  Comparing the two directly, as this used to, means the split
     * never happens: Supaplex asks for line compare 351 and src_line never
     * gets past 199.  Its status panel then came from the scrolling offset
     * instead, which wraps at 16 bits onto the panel image at the wrong
     * place - a panel that is visible but shifted and cut off.
     *
     * So compare in display lines and convert the split point into source
     * lines with the same rule src_line was derived by.  The 256-colour
     * renderer already did this; this path was simply never fixed.
     */
    uint32_t offset;
    bool in_split = (frame_line_compare >= 0 && (int)line > frame_line_compare);
    if (in_split) {
        /* line_compare + 1: the counter resets after the matching line. */
        uint32_t lc = (uint32_t)frame_line_compare + 1u;
        uint32_t lc_src;
        if (height <= 100) {
            lc_src = lc >> 2;
        } else if (height <= 200) {
            lc_src = lc >> 1;
        } else if (height <= 350) {
            int ega_active_lines = active_end - active_start;
            lc_src = ega_active_lines > 0
                   ? (lc * (uint32_t)height) / (uint32_t)ega_active_lines : lc;
        } else {
            lc_src = lc;
        }
        offset = (src_line - lc_src) * stride;
    } else {
        offset = frame_vram_offset + src_line * stride;
    }

    offset &= 0xFFFF;

    const uint32_t *src32 = (const uint32_t *)(gfx_buffer + offset * 4);
    int panning = (in_split && frame_panning_split_off) ? 0 : frame_pixel_panning;
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

    /*
     * Split screen, row-scan preset and CRTC stride - see the same block in
     * drivers/hdmi/hdmi.c, which is the renderer the board actually uses.
     */
    uint32_t ch_h = frame_text_char_h ? (uint32_t)frame_text_char_h : 16u;
    uint32_t base_cell = frame_vram_offset;
    uint32_t v = line + frame_preset_row;
    if (frame_line_compare >= 0 && (int)line > frame_line_compare) {
        base_cell = 0;
        v = line - (uint32_t)(frame_line_compare + 1);
    }
    uint32_t char_row = v / ch_h;
    uint32_t glyph_line = v % ch_h;

    int cols = text_cols;
    int double_h = (cols == 40);  // 40 columns => 2x horizontal scaling

    uint32_t row_cell = base_cell + char_row * (uint32_t)text_stride_cells;
    if (row_cell + (uint32_t)cols <= 65536u) {
        const uint32_t *text_row = (const uint32_t *)gfx_buffer + row_cell;

        for (int col = 0; col < cols; col++) {
            uint16_t cell = text_row[col];
            uint8_t ch   = (uint8_t)(cell & 0xFF);
            uint8_t attr = (uint8_t)(cell >> 8);
            /* Use font from VGA plane 2 (supports loaded fonts via SR3).
             * Stride between rows is 4 bytes (interleaved planes).
             * Fall back to built-in font8x16 if vga_state is unavailable. */
            register uint32_t glyph;
            const uint8_t *fp = vga_get_font_ptr(vga_state, ch, (attr >> 3) & 1);
            if (fp) {
                glyph = reverse_bits8(fp[glyph_line * 4]);
            } else {
                glyph = font_8x16[ch * 16 + glyph_line];
            }
            /*
             * Text attribute bit 7 is BLINK when Attribute Controller
             * Mode Control bit 3 is set.  With blink disabled it is
             * background intensity and must remain part of the palette index.
             */
            uint8_t pal_attr = attr;
            if (vga_state && (vga_state->ar[0x10] & 0x08)) {
                pal_attr &= 0x7F;
            }
            uint16_t *pal = &txt_palette_fast[pal_attr * 4];

            bool cursor_here = cursor_blink_state && col == cursor_x &&
                char_row == (uint32_t)cursor_y &&
                glyph_line >= (uint32_t)cursor_start &&
                glyph_line <= (uint32_t)cursor_end;
            if (cursor_here) {
                glyph = 0xFF;
            } else if (vga_state && (vga_state->ar[0x10] & 0x08) &&
                       (attr & 0x80) && !cursor_blink_state) {
                glyph = 0;
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

void __time_critical_func(pre_render_line)(void) {
    // Check retrace and submit frame (fast path)
    // We must check this frequently to catch the VBLANK edge
    static bool was_in_retrace = false;
    if (!vga_state) return;
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
        osd_render_line_vga(line, output_buffer);
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
        } else if (gfx_submode == 2 || gfx_submode == 6) {
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
void vga_hw_set_text_palette(const uint8_t *palette16_data);

static void vga_hw_new_frame_deferred(void) {
    if (!vga_state) return;
    static int last_vga_mode = -1;
    static int last_gfx_submode = -1;

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

    /*
     * Mode 0 is the transient blank a game sets (attribute controller PAS
     * bit clear) while it reprograms the CRTC and reloads the DAC behind it.
     * vga_hw_set_mode() deliberately ignores it and keeps the previous mode
     * on screen, so the palette of that mode has to keep being maintained
     * through the blank as well.  Returning early here instead - which is
     * what this used to do - froze conv_color[] at the outgoing palette
     * while the guest loaded the incoming one, and everything still being
     * painted stayed in the old colours until the blank ended.
     *
     * Prehistorik 2 blanks for ~320 ms on the way out of its MODE BEGINNER
     * screen; the picture is cleared by then, so what was left visible was
     * the letterbox, still filled with frame_border_pix through a stale
     * table - a coloured frame around a black screen.
     *
     * last_vga_mode and last_gfx_submode are left alone: the blank is not a
     * mode change, and the palette is kept current here, so there is nothing
     * for the end of the blank to have to repair.
     */
    if (vga_mode == 0) {
        if (vga_is_palette_dirty(vga_state)) {
            uint8_t pal16[48];
            vga_get_palette16(vga_state, pal16);
            if (current_mode == 1)
                vga_hw_set_text_palette(pal16);
            else if (gfx_submode == 2 || gfx_submode == 6)
                vga_hw_set_palette16(pal16);
            else
                vga_hw_set_palette(vga_get_palette(vga_state));
        }
        return;
    }

    bool mode_changed = (vga_mode != last_vga_mode);
    if (mode_changed) {
        vga_hw_set_mode(vga_mode);
        last_vga_mode = vga_mode;
    }

    // Update palette and graphics submode for graphics modes
    if (vga_mode == 2) {
        int gfx_w, gfx_h;
        int new_gfx_submode = vga_get_graphics_mode(vga_state, &gfx_w, &gfx_h);
        int line_offset = vga_get_line_offset(vga_state);

        if (new_gfx_submode == 2 && gfx_w <= 320) {
            new_gfx_submode = 6; // EGA 320*
        }

        bool submode_changed = (new_gfx_submode != last_gfx_submode);
        bool palette_dirty = vga_is_palette_dirty(vga_state);

        vga_hw_set_gfx_mode(new_gfx_submode, gfx_w, gfx_h, line_offset);

        /*
         * HDMI EGA uses conv_color[] as a 256-entry table of TWO-pixel
         * combinations. Building it is expensive: 16x16 entries, each
         * requiring TMDS encoding. The old code rebuilt the whole table
         * every frame while HDMI DMA was simultaneously reading it.
         *
         * Besides wasting a large amount of CPU time this makes scanout see
         * a partially rewritten palette, producing visible color flicker
         * (especially in large 640x480x16 areas such as Windows 95 Setup).
         *
         * Rebuild only when the VGA/attribute palette actually changed or
         * when entering/changing the graphics mode.
         */
        if (new_gfx_submode == 2 || new_gfx_submode == 6) {
            if (palette_dirty || mode_changed || submode_changed) {
                uint8_t ega_pal[48];
                vga_get_palette16(vga_state, ega_pal);
                vga_hw_set_palette16(ega_pal);
            }
            last_gfx_submode = new_gfx_submode;
            return;
        }

        // 256-color modes use the normal one-pixel palette.
        if (palette_dirty || mode_changed || submode_changed) {
            vga_hw_set_palette(vga_get_palette(vga_state));
        }

        // HDMI only: fixed CGA palettes only need rebuilding on mode changes.
        if (!SELECT_VGA && (mode_changed || submode_changed)) {
            // CGA 4-color
            if (new_gfx_submode == 1) {
                uint8_t c = c6_to_8(63);
                graphics_set_palette_hdmi(0, 0, 0, 0);   // Black
                graphics_set_palette_hdmi(0, c, c, 1);   // Cyan (bright)
                graphics_set_palette_hdmi(c, 0, c, 2);   // Magenta (bright)
                graphics_set_palette_hdmi(c, c, c, 3);   // White
            }
            // CGA 2-color (640x200 monochrome)
            else if (new_gfx_submode == 4) {
                graphics_set_palette_hdmi2(0,0,0,       0,0,0,       0b00); // Black+Black
                graphics_set_palette_hdmi2(0,0,0,       255,255,255, 0b01); // Black+White
                graphics_set_palette_hdmi2(255,255,255, 0,0,0,       0b10); // White+Black
                graphics_set_palette_hdmi2(255,255,255, 255,255,255, 0b11); // White+White
            }
        }

        last_gfx_submode = new_gfx_submode;
    } else {
        /*
         * Text mode takes its 16 colours from the DAC through the attribute
         * controller, exactly as the EGA graphics modes do.  This path used to
         * restore a fixed CGA table instead (see vga_hw_process_deferred), so
         * a program that loads its own palette was drawn in whatever colours
         * that table happened to hold at those indices.
         *
         * Prehistorik 2's intro is drawn in text mode with a redefined font
         * and fades its palette in: its logo came out magenta instead of
         * yellow and the dotted waves gold instead of white.
         *
         * required_to_repair_text_pal is now a request to rebuild rather than
         * to restore, so a switch out of a graphics mode still repaints the
         * whole 256-entry pair table - just from the right source.
         */
        if (vga_mode == 1) {
            bool palette_dirty = vga_is_palette_dirty(vga_state);
            if (palette_dirty || mode_changed || required_to_repair_text_pal) {
                required_to_repair_text_pal = false;
                uint8_t pal16[48];
                vga_get_palette16(vga_state, pal16);
                vga_hw_set_text_palette(pal16);
            }
        }
        last_gfx_submode = -1;
    }
}

// ============================================================================
// Core 1 ISR load meter
// Displayed as a 5-pixel-tall bar in the inactive region BELOW the active
// picture (lines active_end .. N_LINES_VISIBLE-1, typically 440..479 = 40 px).
//   RED    = ISR busy time fraction of full frame
//   GREEN  = remaining budget
//   YELLOW = 4px marker at left edge when a missed/late ISR was detected
// 640 pixels wide = full frame budget.
// ============================================================================
#define LOAD_BAR_ENABLE  0   // set to 0 to disable
#define LOAD_BAR_HEIGHT  10   // 5px ISR load + 5px new_frame duration

static uint32_t isr_busy_us_acc  = 0;
static uint32_t isr_busy_us_prev = 0;
static uint32_t blank_frame_count = 0;
static uint32_t blank_frame_prev  = 0;
// Missed ISR: detected when current_line jumps by >1 between two calls
static uint32_t missed_isr_count = 0;
static uint32_t missed_isr_prev  = 0;
// Once any missed ISR or blank frame is detected, stay yellow forever
static uint32_t anomaly_ever_seen = 0;
// PIO TX stall detected (FDEBUG.TXSTALL) - confirmed sync loss
static uint32_t pio_stall_ever_seen = 0;
// Max observed vga_hw_new_frame duration in µs (for debugging long new_frame calls)
static uint32_t new_frame_max_us = 0;
// Deferred frame update flag (set in ISR, processed outside)
volatile uint32_t frame_update_request = 0;

// Frame period in µs — computed in vga_hw_init() to avoid overflow.
static uint32_t frame_period_us = 16688u;

// Render the load bar directly into a line buffer.
// Called for absolute scanlines in range [active_end .. active_end+LOAD_BAR_HEIGHT).
static void __time_critical_func(render_load_bar)(uint32_t abs_line,
                                                   uint32_t *output_buffer) {
#if LOAD_BAR_ENABLE
    if (abs_line < (uint32_t)active_end) return;
    if (abs_line >= (uint32_t)active_end + LOAD_BAR_HEIGHT) return;

    uint8_t *out = (uint8_t *)output_buffer + SHIFT_PICTURE;

    // Top 5 rows: ISR total load (red/green/yellow/blue as before)
    // Bottom 5 rows: vga_hw_new_frame() duration vs 32µs budget (orange/green)
    bool is_bottom = (abs_line >= (uint32_t)active_end + 5);

    if (!is_bottom) {
        // ISR load bar
        uint32_t busy  = isr_busy_us_prev;
        uint32_t total = frame_period_us ? frame_period_us : 16688u;
        if (busy > total) busy = total;
        uint32_t red_px = (busy * 640u) / total;

        uint8_t idle_color;
        if (pio_stall_ever_seen)
            idle_color = 0xC3u;  // blue
        else if (anomaly_ever_seen)
            idle_color = 0xFCu;  // yellow
        else
            idle_color = 0xCCu;  // green

        for (uint32_t x = 0; x < 640; x++)
            out[x] = (x < red_px) ? 0xF0u : idle_color;
    } else {
        // new_frame duration bar: budget = 32µs = one scanline
        // ORANGE pixel: R=3,G=1,B=0 → 0xC0|0x34 = 0xF4
        uint32_t nf = new_frame_max_us;
        uint32_t budget = 32u;
        if (nf > 640u) nf = 640u;  // cap visual at 640µs (20× budget)
        // Scale: 640px = 20 × budget; each px = 1µs
        uint32_t orange_px = (nf < 640u) ? nf : 640u;
        // Mark the budget boundary with a bright white tick
        for (uint32_t x = 0; x < 640; x++) {
            if (x == budget)
                out[x] = 0xFFu;  // white tick at 32µs mark
            else if (x < orange_px)
                out[x] = 0xF4u;  // orange: used new_frame time
            else
                out[x] = 0xCCu;  // green: spare
        }
    }
#endif
}

static void __isr __time_critical_func(dma_handler_vga)(void) {
    uint32_t t_enter = timer_hw->timerawl;
    dma_hw->ints0 = 1u << dma_ctrl_chan;
    static uint32_t current_line = 0;
    static uint32_t prev_line    = 0xFFFFFFFF;
    uint32_t line = current_line++;

    // Check PIO FDEBUG.TXSTALL - sticky bit set if PIO ever ran out of data
    // This is the definitive indicator of a DMA underrun causing sync loss.
    uint32_t txstall_bit = 1u << (8 + vga_sm);
    if (VGA_PIO->fdebug & txstall_bit) {
        VGA_PIO->fdebug = txstall_bit;  // clear (write 1 to clear)
        pio_stall_ever_seen = 1;
    }

    // Detect missed ISR: DMA advanced more than 1 line between two ISR calls
    if (prev_line != 0xFFFFFFFF && line > prev_line + 1)
        missed_isr_count += line - prev_line - 1;
    prev_line = line;

    if (line >= N_LINES_TOTAL) {
        line = current_line = prev_line = 0;
        isr_busy_us_prev  = isr_busy_us_acc;
        isr_busy_us_acc   = 0;
        blank_frame_prev  = blank_frame_count;
        blank_frame_count = 0;
        missed_isr_prev   = missed_isr_count;
        missed_isr_count  = 0;
        if (missed_isr_prev || blank_frame_prev)
            anomaly_ever_seen = 1;
       /* {
            uint32_t t0 = timer_hw->timerawl;
            vga_hw_new_frame();
            uint32_t dt = timer_hw->timerawl - t0;
            if (dt > new_frame_max_us) new_frame_max_us = dt;
            // If new_frame took more than one scanline (~32µs), that's a problem
            if (dt > 32) anomaly_ever_seen = 1;
        }*/
        // Defer heavy frame work outside ISR
        frame_update_request = 1;
        if (vga_state && vga_get_mode(vga_state) == 0) {
            blank_frame_count = 1;
            if (frame_blank_frames < BLANK_MAX_FRAMES)
                frame_blank_frames++;
        } else {
            frame_blank_frames = 0;
        }
        frame_blank_active = (frame_blank_frames != 0 &&
                              frame_blank_frames < BLANK_MAX_FRAMES);
    }
    
    // Update VGA status register 1 (port 0x3DA) from ISR — this is the
    // authoritative source. Core0 reads it as-is without any logic.
    // Bit 0 (DISP_ENABLE): 1 = active display, 0 = blanking interval
    // Bit 3 (V_RETRACE):   1 = vertical retrace, 0 = active display
    if (vga_state) {
        if (line >= N_LINES_VISIBLE) {
            vga_state->st01 |=  ST01_V_RETRACE;
            vga_state->st01 &= ~ST01_DISP_ENABLE;
        } else {
            vga_state->st01 &= ~ST01_V_RETRACE;
            vga_state->st01 |=  ST01_DISP_ENABLE;
        }
    }

    // Vertical blanking region
    if (line >= N_LINES_VISIBLE) {
        if (line >= LINE_VS_BEGIN && line <= LINE_VS_END) {
            dma_channel_set_read_addr(dma_ctrl_chan, &lines_pattern[1], false);
        } else {
            dma_channel_set_read_addr(dma_ctrl_chan, &lines_pattern[0], false);
        }

        // Line N_LINES_TOTAL-4 (521): late in vblank, just before DMA needs line 0.
        // Wolf3D has already written the new page address to CRTC by now.
        // Read cr[] and ar[] directly — no intermediate volatile copies.
        if (line == N_LINES_TOTAL - 4) {
            if (vga_state) {
                const uint8_t *cr = vga_state->cr;
                // Wolf3D writes cr[0x0c] and cr[0x0d] as two separate OUT
                // instructions. The ISR may land between them and read a
                // half-updated 16-bit address.  Re-read until both bytes
                // are stable (usually only one extra read is needed).
                uint8_t hi, lo;
                do {
                    hi = cr[0x0c];
                    lo = cr[0x0d];
                } while (hi != cr[0x0c]);
                frame_vram_offset = (uint16_t)((hi << 8) | lo);
                frame_pixel_panning = vga_state->ar[0x13] & 0x07;
                frame_panning_split_off =
                    (vga_state->ar[0x10] & 0x20) ? 1u : 0u;
                frame_crtc_mode = cr[0x17];
                frame_max_scan  = cr[0x09] & 0x1fu;
                frame_overscan  = vga_state->ar[0x11];
                frame_preset_row = cr[0x08] & 0x1fu;
                {
                    /* Character height from CR09, doubled when bit 7 asks for
                     * scan doubling; and the text geometry, which until now was
                     * only refreshed on a vblank edge that this path never
                     * sees - it sat at 80 cells while the CRTC said 82, so
                     * every row after the first was read one cell early. */
                    uint32_t mh = (uint32_t)(cr[0x09] & 0x1fu) + 1u;
                    if (cr[0x09] & 0x80u) mh <<= 1;
                    frame_text_char_h = (uint8_t)mh;
                    int tc = (int)cr[0x01] + 1;
                    int ts = (int)cr[0x13] * 2;
                    if ((tc == 40 || tc == 80) && ts > 0 && ts <= 256) {
                        text_cols = tc;
                        text_stride_cells = ts;
                    }
                }
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
    uint32_t read_buf = 2 + (line & 3);
    uint32_t render_buf = 2 + ((line + 2) & 3);
    uint32_t render_line_num = line + 2;

    // Set DMA to read from the buffer we already rendered
    dma_channel_set_read_addr(dma_ctrl_chan, &lines_pattern[read_buf], false);

    // Pre-render 2 lines ahead
    if (render_line_num < N_LINES_VISIBLE) {
        render_line(render_line_num, lines_pattern[render_buf]);
        // Load bar goes into the inactive region below active_end (e.g. lines 440-479)
        render_load_bar(render_line_num, lines_pattern[render_buf]);
    }

    // Accumulate ISR busy time (µs)
    isr_busy_us_acc += timer_hw->timerawl - t_enter;
}

// ============================================================================
// Public API
// ============================================================================
int testPins(uint32_t pin0, uint32_t pin1);
void graphics_init_hdmi();
// From main.c — needed for safe flash access at high clock speeds
extern void set_flash_timings(int cpu_mhz, int cfg_flash);

// HDMI TMDS requires an exact 252 MHz PIO clock. Normal builds use a
// 504 MHz system clock and integer PIO divider 2; a 252 MHz diagnostic
// build can select integer divider 1 through CMake.
#ifndef HDMI_SYS_CLOCK_MHZ
#define HDMI_SYS_CLOCK_MHZ 504
#endif

static void hdmi_boost_clock(void) {
    int cur_mhz = clock_get_hz(clk_sys) / 1000000;
    if (cur_mhz >= HDMI_SYS_CLOCK_MHZ) return;
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_65);
    sleep_ms(50);
    set_flash_timings(HDMI_SYS_CLOCK_MHZ, FLASH_MAX_FREQ_MHZ);
    /* Same for the PSRAM: its divisor still describes the pre-boost clock,
     * so it has to be widened before the PLL moves, not after. */
    psram_set_timings(HDMI_SYS_CLOCK_MHZ, PSRAM_MAX_FREQ_MHZ);
    set_sys_clock_khz(HDMI_SYS_CLOCK_MHZ * 1000, false);
    /* clk_peri moved with clk_sys; the UART's divisors did not. */
    extern void console_reclock(void);
    console_reclock();
    // Re-run the full init at the new clock. The divisor is already right
    // (psram_set_timings above), but this also re-applies the read/write
    // formats, and it is what reconfigure_clocks() would otherwise have to
    // do much later.
    psram_init_with_freq(get_psram_pin(), PSRAM_MAX_FREQ_MHZ);
}

/* Recalculate and apply VGA PIO clock divider after a sysclk change. */
void vga_hw_reclock(void) {
    if (!SELECT_VGA) return;
    float clk_div = (float)clock_get_hz(clk_sys) / VGA_CLK;
    uint32_t div_int  = (uint32_t)clk_div;
    uint32_t div_frac = (uint32_t)((clk_div - (float)div_int) * 256.0f);
    VGA_PIO->sm[vga_sm].clkdiv = (div_int << 16) | (div_frac << 8);
    frame_period_us = (uint32_t)((float)(LINE_SIZE * N_LINES_TOTAL) * 1000000.0f / VGA_CLK);
}

void vga_hw_init(void) {
    #if defined(FORCE_VGA)
        /* Skip the pin probe and commit to VGA. Wins over FORCE_HDMI so a
         * board that defaults to HDMI can still be built for VGA without
         * unpicking its board block. */
        SELECT_VGA = true;
    #elif defined(FORCE_HDMI)
        SELECT_VGA = false;
    #else
        uint8_t linkVGA01 = testPins(VGA_BASE_PIN, VGA_BASE_PIN + 1);
        #if defined(BOARD_Z0) || defined(BOARD_Z2) || defined(BOARD_DV)
            SELECT_VGA = linkVGA01 == 0x1F;
        #else
            SELECT_VGA = (linkVGA01 == 0) || (linkVGA01 == 0x1F);
        #endif
        // If HDMI detected, reset tested pins to clean hi-Z state.
        // testPins leaves pull-downs enabled via gpio_deinit(), which can
        // disturb the HDMI differential pair during clock boost.
        if (!SELECT_VGA) {
            gpio_init(VGA_BASE_PIN);
            gpio_set_dir(VGA_BASE_PIN, GPIO_IN);
            gpio_disable_pulls(VGA_BASE_PIN);
            gpio_init(VGA_BASE_PIN + 1);
            gpio_set_dir(VGA_BASE_PIN + 1, GPIO_IN);
            gpio_disable_pulls(VGA_BASE_PIN + 1);
        }
    #endif
    DBG_PRINT("  Video output: %s\n", SELECT_VGA ? "VGA" : "HDMI");
    if (!SELECT_VGA) {
        hdmi_boost_clock();
        graphics_init_hdmi();
        return;
    }
    DBG_PRINT("VGA Init (pico-286 style)...\n");

    init_palettes();

    // Calculate clock divider
    float sys_clk = (float)clock_get_hz(clk_sys);
    float clk_div = sys_clk / VGA_CLK;

    // Frame period: LINE_SIZE pixels per line, N_LINES_TOTAL lines, at VGA_CLK px/s
    // Use float to avoid 32-bit overflow (800*525*1e6 ≈ 4.2e11)
    frame_period_us = (uint32_t)((float)(LINE_SIZE * N_LINES_TOTAL) * 1000000.0f / VGA_CLK);

    DBG_PRINT("  System clock: %.1f MHz\n", sys_clk / 1e6f);
    DBG_PRINT("  Clock divider: %.4f\n", clk_div);
    DBG_PRINT("  Frame period: %lu us\n", (unsigned long)frame_period_us);
    
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
    if (!SELECT_VGA && vga_state) { // W/A
        if (mode == 1 && current_mode != 1) { // switch to text
            required_to_repair_text_pal = true;
        }
        if (mode == 2 && current_mode != 2) { // switch to graphics
//            vga_state->palette_dirty = 1;
        }
    }
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
    current_mode = mode;
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
/* Darkest entry of a palette of `n` 6-bit RGB triples. */
static int vga_darkest_index(const uint8_t *pal, int n) {
    int best = 0, best_sum = 1 << 30;
    for (int i = 0; i < n; i++) {
        int sum = pal[i * 3 + 0] + pal[i * 3 + 1] + pal[i * 3 + 2];
        if (sum < best_sum) { best_sum = sum; best = i; }
    }
    return best;
}

/*
 * Which entry to fill the letterbox with.  That margin is our own padding,
 * not part of the guest's picture, so it wants to be black - prefer an entry
 * that actually is black over merely the darkest one, and fall back to the
 * darkest only for a palette that holds no black at all.
 */
static int vga_border_index(const uint8_t *pal, int n) {
    for (int i = 0; i < n; i++)
        if (!pal[i * 3 + 0] && !pal[i * 3 + 1] && !pal[i * 3 + 2])
            return i;
    return vga_darkest_index(pal, n);
}

/* The colour the border is actually painted with, for reading over SWD. */
static void vga_border_note(const uint8_t *pal, int b) {
    frame_border_rgb[0] = pal[b * 3 + 0];
    frame_border_rgb[1] = pal[b * 3 + 1];
    frame_border_rgb[2] = pal[b * 3 + 2];
}


void vga_hw_set_palette(const uint8_t *palette_data) {
    for (int i = 0; i < 256; i++) {
        uint8_t r6 = palette_data[i * 3 + 0];
        uint8_t g6 = palette_data[i * 3 + 1];
        uint8_t b6 = palette_data[i * 3 + 2];
        vga_color_to_dithered(r6, g6, b6, i);
    }

    if (!SELECT_VGA)
        hdmi_pick_substitutes_256(palette_data);

/*
 * The border index and the table entry it selects have to become visible to
 * the scanline ISR together.  Setting frame_border_pix first and then
 * spending ~1 ms rewriting all 256 entries means that, for that millisecond,
 * the letterbox is drawn with the NEW index into the OLD table - and during
 * a palette fade the rebuild runs every frame, so it is not a one-off blink
 * but a steady wrong-coloured margin.  Assign it last.
 */
    {   /* 256-colour modes put one index in a byte. */
        int b = vga_border_index(palette_data, 256);
        vga_border_note(palette_data, b);
        frame_border_pix = (uint8_t)b;
    }
}

// Update EGA 16-color palette from AC palette registers
// palette16_data is 48 bytes (16 entries × 3 bytes RGB, each 0-63)
void __time_critical_func(vga_hw_set_palette16)(const uint8_t *palette16_data) {
    for (int i = 0; i < 16; i++) {
        uint8_t r6 = palette16_data[i * 3 + 0];
        uint8_t g6 = palette16_data[i * 3 + 1];
        uint8_t b6 = palette16_data[i * 3 + 2];
        if (SELECT_VGA) {
            ega_palette[i] = vga_color_to_output(r6, g6, b6);
        } else {
            /*
             * Only submode 2 gets the paired palette, and that is deliberate.
             *
             * On HDMI the renderer is render_gfx_line_ega320() in hdmi.c, not
             * render_gfx_line_ega() in this file, and it emits ONE 4-bit index
             * per byte via ega_pair(). A byte therefore selects a single
             * colour, which is what graphics_set_palette_hdmi() programs.
             * Submode 2 (640-wide) is the one that packs two pixels per byte
             * and needs the 256-entry pair table.
             *
             * Extending this to submode 6 was tried and is wrong: byte j is
             * then looked up as the pair (0, j), so every other column renders
             * as palette entry 0. On Prehistorik that showed as correct colours
             * with black columns interleaved through them.
             */
            if (gfx_submode == 2) {
                for (int j = 0; j < 16; j++) {
                    uint8_t rj = palette16_data[j * 3 + 0];
                    uint8_t gj = palette16_data[j * 3 + 1];
                    uint8_t bj = palette16_data[j * 3 + 2];
                    graphics_set_palette_hdmi2(
                        c6_to_8(r6), c6_to_8(g6), c6_to_8(b6),
                        c6_to_8(rj), c6_to_8(gj), c6_to_8(bj),
                        (i << 4) | j
                    );
                }
            } else {
                graphics_set_palette_hdmi(c6_to_8(r6), c6_to_8(g6), c6_to_8(b6), i);
            }
        }
    }

    /*
     * Only the pair table can put a reserved byte on screen.  The 320-wide
     * modes put one 4-bit index in a byte, so their pixels never exceed 15
     * and the substitutes are never consulted.
     */
    if (!SELECT_VGA && gfx_submode == 2)
        hdmi_pick_substitutes_pair(palette16_data);

/*
 * The border index and the table entry it selects have to become visible to
 * the scanline ISR together.  Setting frame_border_pix first and then
 * spending ~1 ms rewriting all 256 entries means that, for that millisecond,
 * the letterbox is drawn with the NEW index into the OLD table - and during
 * a palette fade the rebuild runs every frame, so it is not a one-off blink
 * but a steady wrong-coloured margin.  Assign it last.
 */
    {
        /*
         * The byte format has to match the table this function just
         * programmed.  Only submode 2 gets the 256-entry pair table; the
         * 320-wide modes get one colour per byte (see the comment in the
         * loop above).  Packing (b << 4) | b unconditionally was the bug:
         * in submode 6 that byte selects conv_color[0xcc], an entry this
         * function never writes, so the letterbox was drawn with whatever
         * a previous 256-colour palette had left there - the level's
         * salmon, in Prehistorik 2, for as long as it took the guest to
         * get back to a 256-colour load.
         */
        int b = vga_border_index(palette16_data, 16);
        vga_border_note(palette16_data, b);
        frame_border_pix = (gfx_submode == 2) ? (uint8_t)((b << 4) | b)
                                              : (uint8_t)b;
    }
}


/*
 * Text mode packs two 4-bit pixels into every output byte, the same as EGA
 * 640, so it needs the full 256-entry pair table - not the one-colour-per-byte
 * table vga_hw_set_palette16() programs for the 320-wide modes.
 */
void __time_critical_func(vga_hw_set_text_palette)(const uint8_t *palette16_data) {
    for (int i = 0; i < 16; i++) {
        uint8_t r6 = palette16_data[i * 3 + 0];
        uint8_t g6 = palette16_data[i * 3 + 1];
        uint8_t b6 = palette16_data[i * 3 + 2];
        if (SELECT_VGA) {
            ega_palette[i] = vga_color_to_output(r6, g6, b6);
            continue;
        }
        for (int j = 0; j < 16; j++) {
            uint8_t rj = palette16_data[j * 3 + 0];
            uint8_t gj = palette16_data[j * 3 + 1];
            uint8_t bj = palette16_data[j * 3 + 2];
            graphics_set_palette_hdmi2(
                c6_to_8(r6), c6_to_8(g6), c6_to_8(b6),
                c6_to_8(rj), c6_to_8(gj), c6_to_8(bj),
                (i << 4) | j
            );
        }
    }

    if (!SELECT_VGA)
        hdmi_pick_substitutes_pair(palette16_data);

/*
 * The border index and the table entry it selects have to become visible to
 * the scanline ISR together.  Setting frame_border_pix first and then
 * spending ~1 ms rewriting all 256 entries means that, for that millisecond,
 * the letterbox is drawn with the NEW index into the OLD table - and during
 * a palette fade the rebuild runs every frame, so it is not a one-off blink
 * but a steady wrong-coloured margin.  Assign it last.
 */
    {   /* Text mode really does pack two pixels per byte. */
        int b = vga_border_index(palette16_data, 16);
        vga_border_note(palette16_data, b);
        frame_border_pix = (uint8_t)((b << 4) | b);
    }
}


// Set graphics sub-mode: 1=CGA 4-color, 2=EGA planar, 3=VGA 256-color, 4=CGA 2-color
void __time_critical_func(vga_hw_set_gfx_mode)(int submode, int width, int height, int line_offset) {
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

extern uint32_t conv_color[1224], conv_color2[1024];

void __not_in_flash_func(vga_hw_process_deferred)(void) {
    if (!frame_update_request)
        return;
    frame_update_request = 0;
    uint32_t t0 = timer_hw->timerawl;
    /* The text palette is rebuilt from the DAC in vga_hw_new_frame_deferred();
     * required_to_repair_text_pal is left set for it to consume.  Copying the
     * fixed CGA table back over conv_color[] here is what made a guest-loaded
     * text palette impossible to see. */
    vga_hw_new_frame_deferred();
    hdmi_build_ega320_cache();
    uint32_t dt = timer_hw->timerawl - t0;
    if (dt > new_frame_max_us)
        new_frame_max_us = dt;
    if (dt > 32)
        anomaly_ever_seen = 1;
}
