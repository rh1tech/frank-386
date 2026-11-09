/**
 * frank-386 - i386 PC Emulator for RP2350
 *
 * HDMI output by PIO, with audio. The output engine is frank-quake2's PIO
 * HDMI driver (drivers/hdmi_pio there; from the frank-msx / pico-nes driver,
 * TMDS by table, and the HDMI framing of shuichitakano's PicoDVI): every
 * line is sent as HDMI, not DVI: a data island in hsync after its preamble
 * (audio, the clock regeneration, the info frames or a null packet), the
 * active video after its preamble and guard band. Capture cards want that.
 *
 * The output state machine sends 10 instructions a pixel (clk_sys / 2 at
 * 504 MHz: 25.2 MHz). It is fed by DMA channel "conv", 16 bytes (two
 * pixels) at a time, from the address the address state machine makes of
 * each 16-bit index of the line: conv_color | index << 4 (an 8 KB window).
 * Indices 0..255 are the palette (the renderers' 8-bit pixels, each a pair
 * of output pixels), 256..335 the HDMI entries: control symbols, the null
 * islands and two slots the IRQ encodes packets into.
 *
 * DMA channel "line" sends the indices of a line in 4 blocks, loaded by
 * channel "ctrl" from the line's block list: the front porch with the data
 * preamble, the island, the rest of the blanking, the pixels (a row of 16-bit
 * indices, or one blank index repeated). The IRQ at the end of the third
 * block points "ctrl" at the next line's list and makes the one after: its
 * list, its island, its row (rendered by the scanline renderers below).
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <assert.h>
#include <hardware/dma.h>
#include <hardware/pio.h>
#include <hardware/irq.h>
#include <hardware/sync.h>
#include <hardware/timer.h>
#include <pico/time.h>
#include <hardware/clocks.h>
#include <arm_acle.h>

#include "vga.h"
#include "vga_osd.h"
#include "hdmi.h"
#include "hstx_packet.h"
#include "font8x16.h"

//активный видеорежим
extern int current_mode;  // Default text mode

#define SCREEN_WIDTH (320)
#define SCREEN_HEIGHT (240)

#define GFX_BUFFER_SIZE (256 * 1024)
extern uint8_t gfx_buffer[GFX_BUFFER_SIZE];
extern uint8_t text_buffer_sram[80 * 25 * 2];
extern int text_cols;
// Stride in *character cells* (uint32_t per cell in gfx_buffer text layout).
// For VGA CRTC Offset (0x13): cells_per_row = cr13 * 2 (80-col -> 40*2, 40-col -> 20*2).
extern int text_stride_cells;
// Direct pointer to VGA register state (set once by core0 after vga_init).
// ISR reads cr[], ar[] directly at the right moment — no volatile intermediates.
extern VGAState *vga_state;
// Per-frame values latched by ISR from vga_state->cr[] late in vblank
extern uint16_t frame_vram_offset;
extern uint8_t  frame_pixel_panning;
extern int      frame_line_compare;
// Cursor state
extern int cursor_x, cursor_y;
extern int cursor_start, cursor_end;
extern int cursor_blink_state;

extern int active_start;
extern int active_end;

extern int gfx_submode;
extern int gfx_width;
extern int gfx_height;
extern int gfx_line_offset;  // Words per line (40 for 320px EGA, 80 for 640px)
extern int gfx_sram_stride;  // Words per line in SRAM buffer (width/8 + 1)

extern volatile uint32_t frame_update_request;

#define ACTIVE_LINES 480
#define TOTAL_LINES 525
#define VSYNC_FIRST 490 /* 490, 491 */
#define ROW_PIXELS 320  /* indices: each two output pixels */

/* the HDMI entries (index 256 + E_...) */
enum {
    E_BLANK = 0,     /* [vsync] */
    E_HSYNC = 2,     /* [vsync] */
    E_PRE_DATA = 4,  /* [vsync] the preamble to a data island */
    E_PRE_VIDEO = 6, /* the preamble to the active video */
    E_GUARD_VIDEO = 7,
    E_NULL = 8,      /* [vsync] 18 entries each */
    E_SLOT = 44,     /* [2] 18 entries each */
    E_COUNT = 80,
};
#define ISLAND_ENTRIES 18 /* 36 pixels: 2 guard, 32 packet, 2 guard */
#define IDX(e) (256 + (e))

/* The TMDS table: 16 bytes an index (two pixels' serialized symbols), the
 * palette (0..255) then the HDMI entries. The address machine ORs the index
 * into its base, so the table is aligned to its 8 KB window: the top 8 KB of
 * the heap, aligned by itself (anywhere else the alignment costs up to 8 KB
 * of padding, and the heap that pc_new() lives on is tight); the rows go in
 * the rest of it with the audio ring. VGA builds take none of it. Entries 0..255 are written
 * live by graphics_set_palette_hdmi*(). */
#define CONV_COLOR_WORDS ((256 + E_COUNT) * 4)
#define HDMI_RAM_BLOCK 8192
uint32_t *conv_color;
bool required_to_repair_text_pal = false; /* text mode again: hdmi_text_palette() */
volatile bool hdmi_palette_reload;        /* the OSD went: the guest's palette again */

typedef struct {
    const void *read;
    volatile void *write;
    uint32_t count;
    uint32_t ctrl;
} block_t;

/* the index lists (SRAM: read by DMA while the CPU works) */
static uint16_t head[2][8];
static uint16_t isl_null[2][ISLAND_ENTRIES];
static uint16_t isl_slot[2][ISLAND_ENTRIES];
static uint16_t rest_active[54];
static uint16_t rest_blank[2][54];
static uint16_t blank_px[2];
/* a row a line: line N sent, N + 1 ready, N + 2 being made (by line % 3:
 * 525 lines are a whole number of threes, the wrap keeps the order); after
 * the table, in its 8 KB block */
static uint16_t (*rows)[ROW_PIXELS];
static uint8_t *row8;
/* the two lines' lists, then a stop: should the IRQ come too late to point
 * "ctrl" at the next list, it runs on into blocks[2], all zeros, whose ctrl
 * word (EN clear) starts nothing; the chain stops (hdmi_watchdog restarts
 * it) instead of loading whatever follows as DMA descriptors, one of which
 * once sent 32 KB over the RAM after gfx_buffer */
static block_t blocks[3][4] __attribute__((aligned(16)));

static uint32_t ctrl_quiet, ctrl_irq; /* "line" ctrl words: incrementing reads */
static uint32_t ctrl_quiet_fixed;     /* the same, the read address fixed */

static PIO const pio_video = PIO_VIDEO;
static uint sm_video, sm_conv;
static uint off_video, off_conv;
static pio_sm_config cfg_video, cfg_conv;
static volatile bool running;
static uint dma_ctrl, dma_line, dma_conv_ctrl, dma_conv;

volatile uint32_t hdmi_frame_count;
volatile uint32_t hdmi_irq_max_us;
volatile uint32_t hdmi_underruns; /* frames in which the output machine starved */
volatile uint32_t hdmi_resyncs;   /* restarts after the output stopped (hdmi_watchdog) */

/* serialized lane symbols (see ser_lane): TERC4 by nibble, a lane each */
static uint64_t terc_lane[3][16];
static uint64_t guard12; /* the data island guard band on lanes 1 and 2 */

/* packets sent from the IRQ, made at init */
static hstx_packet_t pkt_acr, pkt_avi, pkt_aif;
static uint32_t audio_accum, audio_step; /* samples, 16.16: 735 a frame */
static uint32_t audio_packets;           /* mod 48: the B flag */

extern uint8_t hstx_bch_table[256]; /* hstx_packet.c, in SRAM */

/* TERC4 (data islands) and the control symbols by CTL bits (c1 << 1 | c0),
 * bit 0 sent first, as in hstx_packet.c and PicoDVI */
static const uint16_t terc4[16] = {0x29c, 0x263, 0x2e4, 0x2e2, 0x171, 0x11e, 0x18e, 0x13c,
                                   0x2cc, 0x139, 0x19c, 0x2c6, 0x28e, 0x271, 0x163, 0x2c3};
static const uint16_t ctl_sym[4] = {0x354, 0x0ab, 0x154, 0x2ab};
#define GUARD_DATA 0x133 /* lanes 1, 2 of a data island guard band */
#define GUARD_VIDEO_0 0x2cc
#define GUARD_VIDEO_1 0x133

/* The bit position of a TMDS lane (0 blue + syncs, 1 green, 2 red) in each
 * 6-bit step of the output program: the board's pair order. */
static inline int lane_shift(int lane) {
#ifdef BOARD_PC
    if (lane) lane ^= 3; /* red and green pairs swapped */
#endif
    if (lane == 1) return 2;
    return (lane == 0) == (HDMI_PIN_RGB_notBGR != 0) ? 0 : 4;
}

/* One lane's 10-bit symbol as the output program shifts it: 10 steps of 6
 * bits (a 2-bit pair a lane, the second half of the word 2 bits on). */
static uint64_t __time_critical_func(ser_lane)(uint16_t sym, int lane) {
    int shift = lane_shift(lane);
    uint64_t out = 0;
    for (int i = 0; i < 10; i++) {
        out <<= 6;
        if (i == 5) out <<= 2;
        uint32_t b = (sym >> (9 - i)) & 1;
        b |= (b ^ 1) << 1;
        if (HDMI_PIN_invert_diffpairs) b ^= 3;
        out |= (uint64_t)b << shift;
    }
    return out;
}

static uint64_t __time_critical_func(get_ser_diff_data)(const uint16_t dataR, const uint16_t dataG, const uint16_t dataB) {
    return ser_lane(dataB, 0) | ser_lane(dataG, 1) | ser_lane(dataR, 2);
}

/* TMDS symbols as frank-quake2's table (and the PicoDVI one) has them:
 * bit 8 set for XOR, bit 9 for XNOR. Sent with its data bits (0-7) flipped
 * the symbol is the DC complement; receivers take both. Never 0x3xx: the
 * capture card would not lock on the standard inverted symbols. */
static uint16_t __time_critical_func(tmds_encoder)(const uint8_t d8) {
    int s1 = __builtin_popcount(d8);
    uint32_t xnor = s1 > 4 || (s1 == 4 && !(d8 & 1));
    uint16_t q = d8 & 1;
    for (int i = 1; i < 8; i++)
        q |= (((q >> (i - 1)) ^ (d8 >> i) ^ xnor) & 1) << i;
    return q | (xnor ? 1u << 9 : 1u << 8);
}

static void pio_set_x(PIO pio, const int sm, uint32_t v) {
    uint instr_shift = pio_encode_in(pio_x, 4);
    uint instr_mov = pio_encode_mov(pio_x, pio_isr);
    for (int i = 0; i < 8; i++) {
        const uint32_t nibble = (v >> (i * 4)) & 0xf;
        pio_sm_exec(pio, sm, pio_encode_set(pio_x, nibble));
        pio_sm_exec(pio, sm, instr_shift);
    }
    pio_sm_exec(pio, sm, instr_mov);
}

static inline void* __not_in_flash_func(nf_memset)(void* ptr, int value, size_t len)
{
    uint8_t* p = (uint8_t*)ptr;
    uint8_t v8 = (uint8_t)value;

    // --- выравниваем до 4 байт ---
    while (len && ((uintptr_t)p & 3)) {
        *p++ = v8;
        len--;
    }

    // --- основной 32-битный цикл ---
    if (len >= 4) {
        uint32_t v32 = v8;
        v32 |= v32 << 8;
        v32 |= v32 << 16;

        uint32_t* p32 = (uint32_t*)p;
        size_t n32 = len >> 2;

        while (n32--) {
            *p32++ = v32;
        }

        p = (uint8_t*)p32;
        len &= 3;
    }

    // --- хвост ---
    while (len--) {
        *p++ = v8;
    }

    return ptr;
}

/* all 256 palette entries are pixels: no sync indices to keep out */
#define ob(x) { *output_buffer++ = (uint8_t)(x); }


static void __time_critical_func(render_text_line)(uint32_t line, uint8_t *output_buffer) {
    uint32_t char_row = line >> 4; // div 16
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
            register uint8_t glyph;
            if (cursor_blink_state && col == cursor_x &&
                char_row == (uint32_t)cursor_y &&
                glyph_line >= (uint32_t)cursor_start &&
                glyph_line <= (uint32_t)cursor_end) {
                glyph = 0xFF;
            } else {
                const uint8_t *fp = vga_get_font_ptr(vga_state, ch, (attr >> 3) & 1);
                if (fp) {
                    glyph = __rbit(fp[glyph_line * 4]) >> 24;
                } else {
                    glyph = font_8x16[ch * 16 + glyph_line];
                }
            }
           // uint8_t blink_or_highlite_bg = attr & 0b10000000; // TODO: use it?
            register uint8_t fg_color0 = attr & 0b00001111;
            register uint8_t bg_color1 = attr & 0b01110000;
            register uint8_t bg_color0 = bg_color1 >> 4;
            register uint8_t fg_color1 = fg_color0 << 4;
            if (!double_h) {
                ob( ((glyph & 0b00000001) ? fg_color1 : bg_color1) | ((glyph & 0b00000010) ? fg_color0 : bg_color0) );
                ob( ((glyph & 0b00000100) ? fg_color1 : bg_color1) | ((glyph & 0b00001000) ? fg_color0 : bg_color0) );
                ob( ((glyph & 0b00010000) ? fg_color1 : bg_color1) | ((glyph & 0b00100000) ? fg_color0 : bg_color0) );
                ob( ((glyph & 0b01000000) ? fg_color1 : bg_color1) | ((glyph & 0b10000000) ? fg_color0 : bg_color0) );
            } else {
                // TODO: optimize it
                ob( ((glyph & 0b00000001) ? fg_color1 : bg_color1) | ((glyph & 0b00000001) ? fg_color0 : bg_color0) );
                ob( ((glyph & 0b00000010) ? fg_color1 : bg_color1) | ((glyph & 0b00000010) ? fg_color0 : bg_color0) );
                ob( ((glyph & 0b00000100) ? fg_color1 : bg_color1) | ((glyph & 0b00000100) ? fg_color0 : bg_color0) );
                ob( ((glyph & 0b00001000) ? fg_color1 : bg_color1) | ((glyph & 0b00001000) ? fg_color0 : bg_color0) );
                ob( ((glyph & 0b00010000) ? fg_color1 : bg_color1) | ((glyph & 0b00010000) ? fg_color0 : bg_color0) );
                ob( ((glyph & 0b00100000) ? fg_color1 : bg_color1) | ((glyph & 0b00100000) ? fg_color0 : bg_color0) );
                ob( ((glyph & 0b01000000) ? fg_color1 : bg_color1) | ((glyph & 0b01000000) ? fg_color0 : bg_color0) );
                ob( ((glyph & 0b10000000) ? fg_color1 : bg_color1) | ((glyph & 0b10000000) ? fg_color0 : bg_color0) );
            }
        }
    }
}

static void __time_critical_func(render_gfx_line_from_sram)(uint32_t line, uint8_t *output_buffer) {
    // Determine source line based on graphics height
    // If height > 200 (e.g. 400 in Mode X), map 1:1
    // If height <= 200 (e.g. 320x200), double lines
    uint32_t src_line = (gfx_height > 200) ? line : (line >> 1);

    if (src_line >= gfx_height && gfx_height > 0) {
        // Blank line below visible area
        nf_memset(output_buffer, 0, SCREEN_WIDTH);
    } else if (src_line >= 200 && gfx_height <= 0) {
        // Fallback if gfx_height not set
        nf_memset(output_buffer, 0, SCREEN_WIDTH);
    } else {
        // Read from VRAM (stable during active video)
        // Stride comes from CRTC Offset (CR13) which is in words for VGA.
        // We use 32-bit words for fetch, so convert words->dwords.
        uint32_t off = gfx_line_offset;
        uint32_t stride = (off > 0) ? (off << 1) : 80u;
        uint32_t offset;
        if (frame_line_compare >= 0 && src_line >= (uint32_t)frame_line_compare) {
            offset = (src_line - frame_line_compare) * stride;
        } else {
            offset = frame_vram_offset + src_line * stride;
        }
        offset &= 0xFFFF;
        // chain4: pixels are stored linearly, addr IS the byte offset
        const uint8_t *src = gfx_buffer + (offset << 2);
        for (int i = 0; i < SCREEN_WIDTH; ++i) {
            ob( *src++ );
        }
    }
}

// Render VGA 256-color planar (Mode X: 320x200x256, unchained)
// VRAM layout in our emulator: packed planes in dwords.
// Each dword holds 4 bytes: plane0..plane3, and those bytes are pixels x%4.
static void __time_critical_func(render_gfx_line_vga_planar256)(uint32_t line, uint8_t *output_buffer) {
    int active_lines = active_end - active_start;
    uint32_t src_line = (gfx_height * 2 <= active_lines) ? (line >> 1) : line;
    if (src_line >= (uint32_t)gfx_height) {
        nf_memset(output_buffer, 0, SCREEN_WIDTH);
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
    const uint8_t *src = gfx_buffer + (base << 2);
    for (int i = 0; i < SCREEN_WIDTH; ++i) {
        ob( *src++ );
    }
}

// Render OSD overlay onto a scanline
// This is called from the ISR, so it must be fast
void __time_critical_func(osd_render_line_hdmi)(uint32_t line, uint8_t *output_buffer) {
    // VGA output is 640x400, text mode is 80x25 with 8x16 font
    // So each character row is 16 scanlines
    uint32_t char_row = line >> 4;
    uint32_t glyph_line = line & 15;

    if (char_row >= OSD_ROWS) return;

    // Get pointer to this row in OSD buffer (reuses text_buffer_sram)
    uint8_t *row_data = &text_buffer_sram[char_row * OSD_COLS * 2];

    // Render each character
    // Bit order matches render_text_line: bits 1,0 are leftmost pair, etc.
    for (int col = 0; col < (OSD_COLS << 1);) {
        uint32_t ch = row_data[col++];
        uint8_t attr = row_data[col++];
        // Get foreground and background colors
        uint8_t fg = attr & 0x0F;
        uint8_t bg = attr >> 4;
        // Get glyph data for this scanline
        register uint8_t glyph = font_8x16[(ch << 4) + glyph_line];
        register uint8_t fg_color0 = attr & 0b00001111;
        register uint8_t bg_color1 = attr & 0b01110000;
        register uint8_t bg_color0 = bg_color1 >> 4;
        register uint8_t fg_color1 = fg_color0 << 4;
        ob( ((glyph & 0b00000001) ? fg_color1 : bg_color1) | ((glyph & 0b00000010) ? fg_color0 : bg_color0) );
        ob( ((glyph & 0b00000100) ? fg_color1 : bg_color1) | ((glyph & 0b00001000) ? fg_color0 : bg_color0) );
        ob( ((glyph & 0b00010000) ? fg_color1 : bg_color1) | ((glyph & 0b00100000) ? fg_color0 : bg_color0) );
        ob( ((glyph & 0b01000000) ? fg_color1 : bg_color1) | ((glyph & 0b10000000) ? fg_color0 : bg_color0) );
    }
}

// Render CGA 4-color graphics line (320x200, 2 bits per pixel, interleaved)
// VGA stores CGA data in odd/even mode with interleaved planes
static void __time_critical_func(render_gfx_line_cga)(uint32_t line, uint8_t *output_buffer) {
    // CGA 320x200 mode (doubled to 640x400)
    uint32_t src_line = line >> 1;
    if (src_line >= 200) {
        // Blank line
        nf_memset(output_buffer, 0, SCREEN_WIDTH);
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
        uint32_t cga_addr = cga_line_offset;
        for (int i = 0; i < 80; i++, cga_addr++) {
            uint32_t vga_addr = ((cga_addr & ~1) << 1) | (cga_addr & 1);
            uint8_t byte = src[vga_addr];
            // Extract 4 pixels (2 bits each), MSB first
            *output_buffer++ = (byte >> 6) & 3;
            *output_buffer++ = (byte >> 4) & 3;
            *output_buffer++ = (byte >> 2) & 3;
            *output_buffer++ = byte & 3;
        }
    }
}

// Render CGA 2-color graphics line (640x200, 1 bit per pixel, interleaved)
// Mode 6: 640x200 monochrome CGA mode
// Memory layout: planar (4 bytes per screen byte), plane 0 only contains data
// Row interleaving: even rows at bank 0, odd rows at bank 1 (0x2000 offset)
static void __time_critical_func(render_gfx_line_cga2)(uint32_t line, uint8_t *output_buffer) {
    // CGA 640x200 mode (doubled to 640x400)
    uint32_t src_line = line >> 1;
    if (src_line >= 200) {
        // Blank line
        nf_memset(output_buffer, 0, SCREEN_WIDTH);
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
        // 80 bytes per CGA scanline = 640 pixels (1 bit per pixel)
        // Data is in plane 0 (every 4th byte in planar layout)
        for (int i = 0; i < 80; i++) {
            // In planar layout, plane 0 is at offset 0, 4, 8, 12, ...
            uint8_t byte = src[base_addr + i * 4];

            // Extract 8 pixels (1 bit each), MSB first
            // Output directly (no horizontal doubling since 640 is native width)
            *output_buffer++ = ((byte >> 7) << 1) | ((byte >> 6) & 1);
            *output_buffer++ = (((byte >> 5) & 1) << 1) | ((byte >> 4) & 1);
            *output_buffer++ = (((byte >> 3) & 1) << 1) | ((byte >> 2) & 1);
            *output_buffer++ = (((byte >> 1) & 1) << 1) | (byte & 1);
        }
    }
}

// Spread 8 bits of a byte into positions 0,4,8,...28
extern uint32_t spread8_lut[256];

// Merge 4 plane bytes [P3|P2|P1|P0] into 8 nibbles (pixel color indices).
static __force_inline uint32_t ega_pack8_from_planes(const uint32_t ega_planes) {
    return
     spread8_lut[(uint8_t)ega_planes] |
     spread8_lut[(uint8_t)(ega_planes >> 8)] << 1 |
     spread8_lut[(uint8_t)(ega_planes >> 16)] << 2 |
     spread8_lut[(uint8_t)(ega_planes >> 24)] << 3;
}

static __force_inline uint32_t ega_pair(uint8_t ab) {
    return ((uint32_t)(ab & 15) << 8) | (uint32_t)(ab >> 4);
}

// Render EGA planar 16-color graphics line
// Supports both 320x200 (doubled) and 640x350 (native) modes
// Reads from SRAM buffer (copied from PSRAM during main loop)
static void __time_critical_func(render_gfx_line_ega320)(uint32_t line, uint8_t *output_buffer) {
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
        nf_memset(output_buffer, 0, SCREEN_WIDTH);
        return;
    }
    uint32_t gfx_width8 = gfx_width >> 3;
    uint32_t stride = gfx_line_offset > 0 ? (gfx_line_offset << 1) : gfx_width8;

    uint32_t offset;
    if (frame_line_compare >= 0 && src_line >= (uint32_t)frame_line_compare) {
        offset = (src_line - frame_line_compare) * stride;
    } else {
        offset = frame_vram_offset + src_line * stride;
    }

    offset &= 0xFFFF;

    register const uint32_t *src32 = (const uint32_t *)(gfx_buffer + (offset << 2));
    register int panning = frame_pixel_panning;
    register uint8_t shift1 = panning << 2;
    register uint8_t shift2 = 32 - shift1;

    // Loop over display width
    int words_to_render = gfx_width8;
    if (words_to_render > 80) words_to_render = 80; // Cap at 640px

    register uint32_t* out32 = (uint32_t*)output_buffer;
    // 320-wide mode: double each pixel horizontally
    for (int i = 0; i < words_to_render; ++i) {
        register uint32_t eight_pixels = ega_pack8_from_planes(src32[i]);
        if (panning > 0) {
            eight_pixels = (eight_pixels << shift1) | (ega_pack8_from_planes(src32[i+1]) >> shift2);
        }
        *out32++ = ega_pair(eight_pixels >> 24) | (ega_pair(eight_pixels >> 16) << 16);
        *out32++ = ega_pair(eight_pixels >> 8) | (ega_pair(eight_pixels) << 16);
    }
}

static void __time_critical_func(render_gfx_line_ega640)(uint32_t line, uint8_t *output_buffer) {
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
        nf_memset(output_buffer, 0, SCREEN_WIDTH);
        return;
    }
    uint32_t gfx_width8 = gfx_width >> 3;
    uint32_t stride = gfx_line_offset > 0 ? (gfx_line_offset << 1) : gfx_width8;

    uint32_t offset;
    if (frame_line_compare >= 0 && src_line >= (uint32_t)frame_line_compare) {
        offset = (src_line - frame_line_compare) * stride;
    } else {
        offset = frame_vram_offset + src_line * stride;
    }

    offset &= 0xFFFF;

    register const uint32_t *src32 = (const uint32_t *)(gfx_buffer + (offset << 2));
    register int panning = frame_pixel_panning;
    register uint8_t shift1 = panning << 2;
    register uint8_t shift2 = 32 - shift1;

    // Loop over display width
    int words_to_render = gfx_width8;
    if (words_to_render > 80) words_to_render = 80; // Cap at 640px

    // 640-wide mode: no horizontal doubling
    for (register int i = 0; i < words_to_render; i++) {
        register uint32_t eight_pixels = ega_pack8_from_planes(src32[i]);

        if (panning > 0) {
            eight_pixels = (eight_pixels << shift1) | (ega_pack8_from_planes(src32[i+1]) >> shift2);
        }
/// TODO: compose palleter for this case
        // Lookup each pixel (no doubling)
        ob ( (eight_pixels >> 24) );
        ob ( (eight_pixels >> 16) );
        ob ( (eight_pixels >> 8) );
        ob ( eight_pixels );
    }
}

void pre_render_line(void);
static void __time_critical_func(render_line)(uint32_t line, uint8_t *output_buffer) {
    // Before emulator init: output black, avoid calling any flash-resident
    // functions that could cause XIP contention with Core 0's BIOS loading.
    if (!vga_state) {
        nf_memset(output_buffer, 0, SCREEN_WIDTH);
        return;
    }
    pre_render_line();
    if (osd_is_visible()) {
        return osd_render_line_hdmi(line, output_buffer);
    }
    int mode = current_mode;
    if (mode == 1) {
        // Text mode now rendered from linear framebuffer
        return render_text_line(line, output_buffer);
    }
    if (mode == 2) {
        uint8_t submode = gfx_submode;
        // Graphics mode - choose renderer based on submode
        if (submode == 2) {
            // EGA planar 16-color 640*
            render_gfx_line_ega640(line, output_buffer);
            return;
        }
        if (submode == 6) {
            // EGA planar 16-color 320*
            render_gfx_line_ega320(line, output_buffer);
            return;
        }
        if (submode == 1) {
            // CGA 4-color
            render_gfx_line_cga(line, output_buffer);
            return;
        }
        if (submode == 4) {
            // CGA 2-color (640x200 monochrome)
            render_gfx_line_cga2(line, output_buffer);
            return;
        }
        if (submode == 5) {
            // VGA 256-color planar (Mode X)
            render_gfx_line_vga_planar256(line, output_buffer);
            return;
        }
        // VGA 256-color (mode 13h) - default
        render_gfx_line_from_sram(line, output_buffer);
        return;
    }
    // mode 0 - blank screen (gray)
    nf_memset(output_buffer, 0x77, SCREEN_WIDTH);
}

//=============================================================================
// HDMI output engine
//=============================================================================

static inline uint64_t *entry(int e) {
    return (uint64_t *)&conv_color[(256 + e) * 4];
}

static void set_hdmi_entry(int e, uint64_t v) {
    entry(e)[0] = v;
    entry(e)[1] = v;
}

/* A data island into entries e..e+17, from a packet: lane 0 carries the
 * header and the syncs (hv: VSYNC << 1 | HSYNC levels), lanes 1 and 2 the
 * subpackets (laid out as hstx_packet.c does for the HSTX). */
static void __time_critical_func(encode_island)(int e, const hstx_packet_t *p, uint32_t hv) {
    uint64_t *d = entry(e);
    uint64_t g = terc_lane[0][0xc | hv] | guard12;
    d[0] = g;
    d[1] = g;
    d[34] = g;
    d[35] = g;
    for (int j = 0; j < 8; j++) {
        uint32_t v = p->subpacket[0][j] | p->subpacket[1][j] << 8 | p->subpacket[2][j] << 16 |
                     (uint32_t)p->subpacket[3][j] << 24;
        uint32_t x = (v ^ (v >> 7)) & 0x00aa00aa;
        v = v ^ x ^ (x << 7);
        x = (v ^ (v >> 14)) & 0x0000cccc;
        v = v ^ x ^ (x << 14);
        for (int k = 0; k < 4; k++) {
            int i = j * 4 + k;                   /* the packet's pixel */
            int sh = (k & 1) * 16 + (k >> 1) * 4; /* lane 1: 0, 16, 4, 20; lane 2: 8 more */
            uint32_t bit = (p->header[i >> 3] >> (i & 7)) & 1;
            uint32_t n0 = bit << 2 | hv | (i ? 8 : 0);
            d[2 + i] = terc_lane[0][n0] | terc_lane[1][(v >> sh) & 0xf] | terc_lane[2][(v >> (sh + 8)) & 0xf];
        }
    }
}

//-----------------------------------------------------------------------------
// HDMI audio: the mixer (core 0, 44.1 kHz) pushes frames into a ring, the
// line IRQ (core 1) takes four for each audio packet; HDMI is the clock.
//-----------------------------------------------------------------------------
#define AUDIO_RING 128      /* frames, a power of 2: 2.9 ms */
#define AUDIO_HIGH 64       /* above it: one frame dropped a packet */
static uint32_t *audio_ring; /* in the HDMI block, after the rows */
static volatile uint32_t audio_wr, audio_rd;
static uint32_t audio_last;
static volatile bool audio_on;
volatile uint32_t hdmi_audio_underruns, hdmi_audio_drops;

void __not_in_flash_func(hdmi_audio_push)(int16_t left, int16_t right) {
    if (!audio_on) return;
    uint32_t w = audio_wr;
    if (w - audio_rd >= AUDIO_RING) { /* full: dropped */
        hdmi_audio_drops++;
        return;
    }
    audio_ring[w & (AUDIO_RING - 1)] = (uint16_t)left | (uint32_t)(uint16_t)right << 16;
    __dmb();
    audio_wr = w + 1;
}

static inline uint32_t parity8(uint32_t v) {
    v ^= v >> 4;
    return (0x6996u >> (v & 0xf)) & 1;
}

/* the next audio packet: 4 frames from the ring (the last one repeated if
 * it ran dry) */
static void __time_critical_func(make_audio_packet)(hstx_packet_t *p) {
    uint32_t rd = audio_rd, fill = audio_wr - rd;
    if (fill > AUDIO_HIGH) {
        rd++; /* the mixer ran ahead: catch up a frame at a time */
        fill--;
        hdmi_audio_drops++;
    }
    p->header[0] = 0x02;
    p->header[1] = 0x0f;
    p->header[2] = audio_packets == 0 ? 0x10 : 0; /* B: a new 192-sample block */
    if (++audio_packets == 48) audio_packets = 0;
    uint8_t v = hstx_bch_table[p->header[0]];
    v = hstx_bch_table[p->header[1] ^ v];
    p->header[3] = hstx_bch_table[p->header[2] ^ v];
    for (int k = 0; k < 4; k++) {
        if (fill) {
            audio_last = audio_ring[rd & (AUDIO_RING - 1)];
            rd++;
            fill--;
        } else {
            hdmi_audio_underruns++;
        }
        int l = (int16_t)audio_last, r = (int16_t)(audio_last >> 16);
        uint8_t *d = p->subpacket[k];
        d[0] = 0;
        d[1] = (uint8_t)l;
        d[2] = (uint8_t)(l >> 8);
        d[3] = 0;
        d[4] = (uint8_t)r;
        d[5] = (uint8_t)(r >> 8);
        d[6] = (uint8_t)(parity8(d[1] ^ d[2]) << 3 | parity8(d[4] ^ d[5]) << 7);
        uint8_t b = hstx_bch_table[0];
        for (int i = 1; i < 7; i++) b = hstx_bch_table[d[i] ^ b];
        d[7] = b;
    }
    audio_rd = rd;
}

/* 8-bit pixels to 16-bit indices */
static void __time_critical_func(convert_row)(uint16_t *dst, const uint8_t *src) {
    const uint32_t *s = (const uint32_t *)src;
    uint32_t *d = (uint32_t *)dst;
    for (int i = 0; i < ROW_PIXELS / 4; i++) {
        uint32_t w = s[i];
        d[2 * i] = (w & 0xff) | (w & 0xff00) << 8;
        d[2 * i + 1] = (w >> 16 & 0xff) | (w >> 24) << 16;
    }
}

static inline void set_block(block_t *b, const void *read, uint32_t count, uint32_t ctrl) {
    b->read = read;
    b->write = &pio_video->txf[sm_conv];
    b->count = count;
    b->ctrl = ctrl;
}


/* the picture of an active line into its row */
static void __time_critical_func(render_row)(uint32_t line, uint16_t *row) {
    if (line < (uint32_t)active_start || line >= (uint32_t)active_end) {
        nf_memset(row, 0, ROW_PIXELS * sizeof(uint16_t));
        return;
    }
    render_line(line - active_start, row8);
    convert_row(row, row8);
}

/* the block list, island and row of a line, into slot (the lines' parity
 * would not do: 525 a frame) */
static void __time_critical_func(prepare_line)(uint32_t line, uint32_t slot) {
    block_t *b = blocks[slot];
    int blank = line >= ACTIVE_LINES;
    int vs = line >= VSYNC_FIRST && line < VSYNC_FIRST + 2;
    uint32_t hv = vs ? 0 : 2; /* in the island: HSYNC asserted (low) */

    /* the packet, if any: the info frames and the clock regeneration on
     * some blanking lines, audio as due */
    audio_accum += audio_step;
    const hstx_packet_t *pkt = NULL;
    hstx_packet_t audio;
    if (blank && !vs && line == ACTIVE_LINES + 1)
        pkt = &pkt_avi;
    else if (blank && !vs && line == ACTIVE_LINES + 3)
        pkt = &pkt_aif;
    else if (blank && !vs && (line & 3) == 0)
        pkt = &pkt_acr;
    else if (audio_on && audio_accum >= 4u << 16) {
        audio_accum -= 4u << 16;
        make_audio_packet(&audio);
        pkt = &audio;
    }
    if (audio_accum > 8u << 16) audio_accum = 8u << 16; /* never a burst */

    set_block(&b[0], head[vs], 8, ctrl_quiet);
    if (pkt) {
        encode_island(E_SLOT + slot * ISLAND_ENTRIES, pkt, hv);
        set_block(&b[1], isl_slot[slot], ISLAND_ENTRIES, ctrl_quiet);
    } else {
        set_block(&b[1], isl_null[vs], ISLAND_ENTRIES, ctrl_quiet);
    }
    if (!blank) {
        uint16_t *row = rows[line % 3];
        render_row(line, row);
        set_block(&b[2], rest_active, 54, ctrl_irq);
        set_block(&b[3], row, ROW_PIXELS, ctrl_quiet);
    } else {
        set_block(&b[2], rest_blank[vs], 54, ctrl_irq);
        set_block(&b[3], &blank_px[vs], ROW_PIXELS, ctrl_quiet_fixed);
    }
}

/* the end of the blanking of a line: its pixels are next */
static uint32_t line, slot; /* being sent (the IRQ's) */

static void __time_critical_func(dma_handler_HDMI)(void) {
    uint32_t t0 = timer_hw->timerawl;
    dma_hw->ints0 = 1u << dma_line;
    uint32_t next = line + 1 == TOTAL_LINES ? 0 : line + 1;
    /* "ctrl" loads this line's last block as the IRQ comes in; the lists are
     * in main SRAM, where core 0's traffic can hold it up. Repointing it (or
     * rewriting this slot) before it is through would cut the pixel block
     * short with the next list's words: a line of the wrong length, and the
     * sink loses sync. */
    const uint32_t loaded = (uint32_t)&blocks[slot][4];
    for (int i = 0; i < 1000 && dma_hw->ch[dma_ctrl].read_addr != loaded; i++)
        tight_loop_contents();
    dma_channel_set_read_addr(dma_ctrl, blocks[slot ^ 1], false);
    uint32_t after = next + 1 == TOTAL_LINES ? 0 : next + 1;

    if (line == 0) {
        frame_update_request = 1;
        hdmi_frame_count++;
        uint32_t stall = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm_video);
        if (pio_video->fdebug & stall) {
            pio_video->fdebug = stall;
            hdmi_underruns++;
        }
    }
    // VGA status register 1 (port 0x3DA)
    if (vga_state) {
        if (line >= ACTIVE_LINES) {
            vga_state->st01 |=  ST01_V_RETRACE;
            vga_state->st01 &= ~ST01_DISP_ENABLE;
        } else {
            vga_state->st01 &= ~ST01_V_RETRACE;
            vga_state->st01 |=  ST01_DISP_ENABLE;
        }
    }
    // Late in vblank, just before line 0 is made: the new page address
    // (Wolf3D has written it to CRTC by now), panning, line compare.
    if (line == TOTAL_LINES - 4 && vga_state) {
        const uint8_t *cr = vga_state->cr;
        frame_vram_offset = (uint16_t)((cr[0x0c] << 8) | cr[0x0d]);
        frame_pixel_panning = vga_state->ar[0x13] & 0x07;
        int lc = (int)cr[0x18]
               | (((int)cr[0x07] & 0x10) << 4)
               | (((int)cr[0x09] & 0x40) << 3);
        frame_line_compare = (lc > 0 && lc < 480) ? lc : -1;
    }

    prepare_line(after, slot); /* this line's slot: all loaded */
    line = next;
    slot ^= 1;
    uint32_t dt = timer_hw->timerawl - t0;
    if (dt > hdmi_irq_max_us) hdmi_irq_max_us = dt;
}


/* ones minus zeros of a 10-bit symbol */
static inline int disparity(uint16_t sym) {
    return 2 * __builtin_popcount(sym & 0x3ff) - 10;
}

/* the value nearest to each one whose symbol (tmds_encoder) has a disparity
 * of 0, 2, 4, 6 or 8 in size; in flash, made by a script from the encoder */
static const uint8_t near_val[256][5] = {
    { 16,   8,   4,   2,   0}, { 16,   8,   4,   2,   1}, { 16,   8,   4,   2,   1}, { 16,   8,   4,   3,   1},
    { 16,   8,   4,   3,   1}, { 16,   8,   5,   6,   1}, { 16,   8,   5,   6,   1}, { 16,   8,   5,   7,   1},
    { 16,   8,  10,   7,   1}, { 16,   9,  10,   7,   1}, { 16,   9,  10,  12,   1}, { 16,   9,  11,  12,   1},
    { 16,   9,  11,  12,   1}, { 16,   9,  14,  13,   1}, { 16,  18,  14,  13,   1}, { 16,  18,  15,  13,   1},
    { 16,  18,  15,  13,   1}, { 17,  18,  15,  13,   1}, { 17,  18,  20,  13,   1}, { 17,  19,  20,  24,   1},
    { 17,  19,  20,  24,   1}, { 17,  22,  21,  24,   1}, { 17,  22,  21,  24,   1}, { 17,  23,  21,  24,   1},
    { 17,  23,  26,  24,   1}, { 17,  23,  26,  25,   1}, { 34,  28,  26,  25,   1}, { 34,  28,  27,  25,   1},
    { 34,  28,  27,  25,   1}, { 34,  29,  30,  25,   1}, { 34,  29,  30,  25,   1}, { 34,  32,  31,  25,   1},
    { 34,  32,  31,  25,   1}, { 34,  33,  31,  25,   1}, { 34,  33,  31,  25,   1}, { 35,  36,  31,  25,   1},
    { 35,  36,  40,  25,   1}, { 38,  37,  40,  48,   1}, { 38,  37,  40,  48,   1}, { 39,  37,  40,  48,   1},
    { 39,  42,  40,  48,   1}, { 39,  42,  41,  48,   1}, { 44,  42,  41,  48,   1}, { 44,  43,  41,  48,   1},
    { 44,  43,  41,  48,   1}, { 45,  46,  41,  48,   1}, { 45,  46,  50,  48,   1}, { 45,  47,  50,  48,   1},
    { 45,  47,  50,  48,   1}, { 45,  47,  50,  49,   1}, { 54,  52,  50,  49,   1}, { 54,  52,  51,  49,   1},
    { 54,  52,  51,  49,   1}, { 54,  53,  51,  49,   1}, { 54,  53,  51,  49,   1}, { 55,  53,  51,  49,   1},
    { 56,  58,  60,  62,   1}, { 57,  58,  60,  62,   1}, { 57,  58,  60,  62,   1}, { 57,  59,  60,  62,   1},
    { 57,  59,  60,  62,   1}, { 57,  59,  61,  62,   1}, { 57,  59,  61,  62,   1}, { 68,  66,  64,  63,   1},
    { 68,  66,  64,  63,   1}, { 68,  66,  65,  63,   1}, { 68,  66,  65,  63,   1}, { 68,  67,  65,  63,   1},
    { 68,  67,  65,  63,   1}, { 69,  70,  65,  63,   1}, { 69,  70,  65,  63,   1}, { 69,  71,  65,  63,   1},
    { 74,  72,  78,  63,   1}, { 74,  73,  78,  63,   1}, { 74,  73,  78,  63,   1}, { 75,  76,  78,  63,   1},
    { 75,  76,  78,  63,   1}, { 75,  77,  78,  63,   1}, { 75,  77,  78,  63,   1}, { 75,  77,  79,  63,   1},
    { 84,  82,  80,  96,   1}, { 84,  82,  81,  96,   1}, { 84,  82,  81,  96,   1}, { 84,  83,  81,  96,   1},
    { 84,  83,  81,  96,   1}, { 85,  86,  81,  96,   1}, { 85,  86,  81,  96,   1}, { 85,  87,  81,  96,   1},
    { 90,  88,  94,  96,   1}, { 90,  89,  94,  96,   1}, { 90,  89,  94,  96,   1}, { 91,  92,  94,  96,   1},
    { 91,  92,  94,  96,   1}, { 91,  93,  94,  96,   1}, { 91,  93,  94,  96,   1}, { 91,  93,  95,  96,   1},
    { 91,  93,  95,  96,   1}, { 91, 100,  98,  97,   1}, {104, 100,  98,  97,   1}, {104, 100,  99,  97,   1},
    {104, 100,  99,  97,   1}, {104, 101, 102,  97,   1}, {104, 101, 102,  97,   1}, {104, 101, 103,  97,   1},
    {104, 106, 103,  97,   1}, {105, 106, 103,  97,   1}, {105, 106, 103,  97,   1}, {108, 107, 103,  97,   1},
    {108, 107, 103,  97,   1}, {109, 110, 114,  97,   1}, {109, 110, 114,  97,   1}, {109, 111, 114,  97,   1},
    {109, 112, 114, 126,   1}, {109, 113, 114, 126,   1}, {118, 113, 114, 126,   1}, {118, 116, 115, 126,   1},
    {118, 116, 115, 126,   1}, {118, 117, 115, 126,   1}, {118, 117, 120, 126,   1}, {119, 117, 120, 126,   1},
    {119, 122, 120, 126,   1}, {119, 122, 121, 126,   1}, {119, 122, 121, 126,   1}, {119, 123, 124, 126,   1},
    {119, 123, 124, 126,   1}, {119, 123, 125, 126,   1}, {119, 123, 125, 126,   1}, {119, 123, 125, 127,   1},
    {136, 132, 130, 128, 254}, {136, 132, 130, 129, 254}, {136, 132, 130, 129, 254}, {136, 132, 131, 129, 254},
    {136, 132, 131, 129, 254}, {136, 133, 134, 129, 254}, {136, 133, 134, 129, 254}, {136, 133, 135, 129, 254},
    {136, 138, 135, 129, 254}, {137, 138, 135, 129, 254}, {137, 138, 140, 129, 254}, {137, 139, 140, 129, 254},
    {137, 139, 140, 129, 254}, {137, 142, 141, 129, 254}, {146, 142, 141, 129, 254}, {146, 143, 141, 129, 254},
    {146, 144, 141, 158, 254}, {146, 145, 141, 158, 254}, {146, 145, 141, 158, 254}, {147, 148, 152, 158, 254},
    {147, 148, 152, 158, 254}, {150, 149, 152, 158, 254}, {150, 149, 152, 158, 254}, {151, 149, 152, 158, 254},
    {151, 154, 152, 158, 254}, {151, 154, 153, 158, 254}, {151, 154, 153, 158, 254}, {151, 155, 156, 158, 254},
    {151, 155, 156, 158, 254}, {151, 155, 157, 158, 254}, {164, 155, 157, 158, 254}, {164, 162, 160, 159, 254},
    {164, 162, 160, 159, 254}, {164, 162, 161, 159, 254}, {164, 162, 161, 159, 254}, {164, 163, 161, 159, 254},
    {164, 163, 161, 159, 254}, {165, 166, 161, 159, 254}, {165, 166, 161, 159, 254}, {165, 167, 161, 159, 254},
    {170, 168, 174, 159, 254}, {170, 169, 174, 159, 254}, {170, 169, 174, 159, 254}, {171, 172, 174, 159, 254},
    {171, 172, 174, 159, 254}, {171, 173, 174, 159, 254}, {171, 173, 174, 159, 254}, {171, 173, 175, 159, 254},
    {180, 178, 176, 192, 254}, {180, 178, 177, 192, 254}, {180, 178, 177, 192, 254}, {180, 179, 177, 192, 254},
    {180, 179, 177, 192, 254}, {181, 182, 177, 192, 254}, {181, 182, 177, 192, 254}, {181, 183, 177, 192, 254},
    {186, 184, 190, 192, 254}, {186, 185, 190, 192, 254}, {186, 185, 190, 192, 254}, {187, 188, 190, 192, 254},
    {187, 188, 190, 192, 254}, {187, 189, 190, 192, 254}, {187, 189, 190, 192, 254}, {187, 189, 191, 192, 254},
    {187, 189, 191, 192, 254}, {198, 196, 194, 193, 254}, {198, 196, 194, 193, 254}, {198, 196, 195, 193, 254},
    {198, 196, 195, 193, 254}, {198, 197, 195, 193, 254}, {198, 197, 195, 193, 254}, {199, 197, 195, 193, 254},
    {200, 202, 204, 206, 254}, {201, 202, 204, 206, 254}, {201, 202, 204, 206, 254}, {201, 203, 204, 206, 254},
    {201, 203, 204, 206, 254}, {201, 203, 205, 206, 254}, {210, 208, 205, 206, 254}, {210, 208, 205, 207, 254},
    {210, 208, 205, 207, 254}, {210, 209, 205, 207, 254}, {210, 209, 214, 207, 254}, {211, 212, 214, 207, 254},
    {211, 212, 214, 207, 254}, {211, 213, 214, 207, 254}, {216, 213, 214, 207, 254}, {216, 213, 215, 207, 254},
    {216, 218, 215, 207, 254}, {217, 218, 215, 207, 254}, {217, 218, 215, 207, 254}, {220, 219, 215, 230, 254},
    {220, 219, 224, 230, 254}, {221, 222, 224, 230, 254}, {221, 222, 224, 230, 254}, {221, 223, 224, 230, 254},
    {221, 223, 224, 230, 254}, {221, 226, 225, 230, 254}, {221, 226, 225, 230, 254}, {221, 227, 228, 230, 254},
    {221, 227, 228, 230, 254}, {221, 227, 229, 230, 254}, {238, 232, 229, 230, 254}, {238, 232, 229, 231, 254},
    {238, 232, 234, 231, 254}, {238, 233, 234, 231, 254}, {238, 233, 234, 231, 254}, {238, 236, 235, 231, 254},
    {238, 236, 235, 231, 254}, {238, 237, 235, 242, 254}, {238, 237, 240, 242, 254}, {239, 237, 240, 242, 254},
    {239, 237, 240, 242, 254}, {239, 237, 241, 242, 254}, {239, 246, 241, 242, 254}, {239, 246, 244, 243, 254},
    {239, 246, 244, 243, 254}, {239, 246, 245, 243, 254}, {239, 246, 245, 248, 254}, {239, 247, 245, 248, 254},
    {239, 247, 250, 248, 254}, {239, 247, 250, 249, 254}, {239, 247, 250, 249, 254}, {239, 247, 251, 252, 254},
    {239, 247, 251, 252, 254}, {239, 247, 251, 253, 254}, {239, 247, 251, 253, 254}, {239, 247, 251, 253, 255},
};

static inline int near_dist(int v, int k) {
    int x = near_val[v][k];
    return x > v ? x - v : v - x;
}

/* Two pixels of a lane as an exactly DC-balanced pair. A symbol with its
 * data bits flipped is the same value with the disparity negated, so a
 * colour against itself is the symbol and its complement (as frank-quake2
 * sends every pixel). Two values balance only when their symbols'
 * disparities are equal in size; where they are not, the values move by the
 * fewest steps (a few in 255) that make them so. Pairs left unbalanced
 * (two colours side by side: text, EGA 640) drift the line's DC level, and
 * the capture card loses sync over it; monitors do not mind. */
static void __time_critical_func(balance_pair)(uint8_t va, uint8_t vb, uint16_t *a, uint16_t *b) {
    uint16_t sa = tmds_encoder(va);
    if (va == vb) {
        *a = sa;
        *b = sa ^ 0xff;
        return;
    }
    int best = 0;
    for (int k = 1; k < 5; k++)
        if (near_dist(va, k) + near_dist(vb, k) < near_dist(va, best) + near_dist(vb, best))
            best = k;
    sa = tmds_encoder(near_val[va][best]);
    uint16_t sb = tmds_encoder(near_val[vb][best]);
    if (disparity(sa) + disparity(sb) != 0)
        sb ^= 0xff; /* -d(s) == d(s ^ 0xff) */
    *a = sa;
    *b = sb;
}

void __time_critical_func(graphics_set_palette_hdmi2)(
    const uint8_t R1, const uint8_t G1, const uint8_t B1,
    const uint8_t R2, const uint8_t G2, const uint8_t B2,
    uint8_t i
) {
    uint16_t r1, r2, g1, g2, b1, b2;
    balance_pair(R1, R2, &r1, &r2);
    balance_pair(G1, G2, &g1, &g2);
    balance_pair(B1, B2, &b1, &b2);
    uint64_t* conv_color64 = (uint64_t*)conv_color;
    conv_color64[i * 2]     = get_ser_diff_data(r1, g1, b1);
    conv_color64[i * 2 + 1] = get_ser_diff_data(r2, g2, b2);
}

void __time_critical_func(graphics_set_palette_hdmi)(const uint8_t R, const uint8_t G, const uint8_t B,  uint8_t i) {
    graphics_set_palette_hdmi2(R, G, B, R, G, B, i);
}

static void fill16(uint16_t *p, uint16_t v, int n) {
    while (n--) *p++ = v;
}

/* The text palette: CGA's 16 colours, an index a pair of them (high nibble
 * the first pixel). Text mode, the OSD and EGA 640 draw with it; made again
 * whenever it was overwritten (in place of keeping a 4 KB copy). */
void __time_critical_func(hdmi_text_palette)(void) {
    static const uint8_t cga_colors[16][3] = {
        { 0,  0,  0}, { 0,  0, 42}, { 0, 42,  0}, { 0, 42, 42},
        {42,  0,  0}, {42,  0, 42}, {42, 21,  0}, {42, 42, 42},
        {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63},
        {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63},
    };
    for (int c1 = 0; c1 < 16; ++c1) {
        const uint8_t* c13 = cga_colors[c1];
        for (int c2 = 0; c2 < 16; ++c2) {
            const uint8_t* c23 = cga_colors[c2];
            graphics_set_palette_hdmi2(
                c13[0] << 2, c13[1] << 2, c13[2] << 2,
                c23[0] << 2, c23[1] << 2, c23[2] << 2,
                c1 << 4 | c2
            );
        }
    }
}

static void make_tables(void) {
    for (int lane = 0; lane < 3; lane++)
        for (int n = 0; n < 16; n++) terc_lane[lane][n] = ser_lane(terc4[n], lane);
    guard12 = ser_lane(GUARD_DATA, 1) | ser_lane(GUARD_DATA, 2);

    hdmi_text_palette();

    for (int vs = 0; vs < 2; vs++) {
        uint32_t v = vs ? 0 : 2; /* VSYNC level (low when asserted) */
        set_hdmi_entry(E_BLANK + vs, get_ser_diff_data(ctl_sym[0], ctl_sym[0], ctl_sym[v | 1]));
        set_hdmi_entry(E_HSYNC + vs, get_ser_diff_data(ctl_sym[0], ctl_sym[0], ctl_sym[v]));
        set_hdmi_entry(E_PRE_DATA + vs, get_ser_diff_data(ctl_sym[1], ctl_sym[1], ctl_sym[v | 1]));
        hstx_packet_t null;
        hstx_packet_set_null(&null);
        encode_island(E_NULL + vs * ISLAND_ENTRIES, &null, v);
    }
    set_hdmi_entry(E_PRE_VIDEO, get_ser_diff_data(ctl_sym[0], ctl_sym[1], ctl_sym[3]));
    set_hdmi_entry(E_GUARD_VIDEO, get_ser_diff_data(GUARD_VIDEO_0, GUARD_VIDEO_1, GUARD_VIDEO_0));

    for (int vs = 0; vs < 2; vs++) {
        fill16(head[vs], IDX(E_BLANK + vs), 4);
        fill16(head[vs] + 4, IDX(E_PRE_DATA + vs), 4);
        for (int k = 0; k < ISLAND_ENTRIES; k++) isl_null[vs][k] = IDX(E_NULL + vs * ISLAND_ENTRIES + k);
        fill16(rest_blank[vs], IDX(E_HSYNC + vs), 30);
        fill16(rest_blank[vs] + 30, IDX(E_BLANK + vs), 24);
        blank_px[vs] = IDX(E_BLANK + vs);
    }
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < ISLAND_ENTRIES; k++) isl_slot[s][k] = IDX(E_SLOT + s * ISLAND_ENTRIES + k);
    /* hsync 30 (60 px, after the island's 36), back porch 19 (38 px), the
     * video preamble 4 (8 px), the guard band 1 (2 px) */
    fill16(rest_active, IDX(E_HSYNC), 30);
    fill16(rest_active + 30, IDX(E_BLANK), 19);
    fill16(rest_active + 49, IDX(E_PRE_VIDEO), 4);
    rest_active[53] = IDX(E_GUARD_VIDEO);

    /* 44.1 kHz: ACR N 6272, CTS 28000 at 25.2 MHz; 640x480 is VIC 1 */
    hstx_packet_set_acr(&pkt_acr, 6272, 28000);
    hstx_packet_set_avi_infoframe(&pkt_avi, 1, 0);
    hstx_packet_set_audio_infoframe(&pkt_aif, 44100, 2, 16);
    audio_step = (735u << 16) / TOTAL_LINES;
}

void graphics_init_hdmi(void) {
    extern void *video_ram_block; /* main.c: the heap's top 8 KB */
    uint8_t *block = video_ram_block;
    if (!block) block = memalign(HDMI_RAM_BLOCK, HDMI_RAM_BLOCK);
    hard_assert(block);
    memset(block, 0, HDMI_RAM_BLOCK);
    conv_color = (uint32_t *)block;
    rows = (uint16_t (*)[ROW_PIXELS])(block + CONV_COLOR_WORDS * sizeof(uint32_t));
    row8 = (uint8_t *)(rows + 3);
    audio_ring = (uint32_t *)(row8 + SCREEN_WIDTH);
    static_assert(CONV_COLOR_WORDS * sizeof(uint32_t) + 3 * ROW_PIXELS * 2 + SCREEN_WIDTH +
                  AUDIO_RING * sizeof(uint32_t) <= HDMI_RAM_BLOCK,
                  "the HDMI block holds the table, the rows and the audio ring");
    make_tables();

    dma_ctrl = dma_claim_unused_channel(true);
    dma_line = dma_claim_unused_channel(true);
    dma_conv_ctrl = dma_claim_unused_channel(true);
    dma_conv = dma_claim_unused_channel(true);
    sm_video = pio_claim_unused_sm(pio_video, true);
    sm_conv = pio_claim_unused_sm(pio_video, true);
#if HDMI_BASE_PIN >= 16
    pio_set_gpio_base(pio_video, 16); /* 0 or 16 only: GPIO 16..47 */
#endif
    /* the address machine: the window | index << 4 */
    static uint16_t conv_instr[5];
    conv_instr[0] = pio_encode_pull(false, true);
    conv_instr[1] = pio_encode_in(pio_null, 4);
    conv_instr[2] = pio_encode_in(pio_osr, 9);
    conv_instr[3] = pio_encode_in(pio_x, 19);
    conv_instr[4] = pio_encode_push(false, true);
    static const struct pio_program conv_program = {.instructions = conv_instr, .length = 5, .origin = -1};
    /* the output: a pixel in 10 steps on the 3 data pairs, the clock by side-set */
    static const uint16_t video_instr[] = {
        0x7006, 0x7006, 0x7006, 0x7006, 0x7006, /* out pins, 6  side 2 */
        0x6806, 0x6806, 0x6806, 0x6806, 0x6806, /* out pins, 6  side 1 */
    };
    static const struct pio_program video_program = {.instructions = video_instr, .length = 10, .origin = -1};
    off_conv = pio_add_program(pio_video, &conv_program);
    off_video = pio_add_program(pio_video, &video_program);

    pio_sm_config cc = pio_get_default_sm_config();
    sm_config_set_wrap(&cc, off_conv, off_conv + conv_program.length - 1);
    sm_config_set_in_shift(&cc, true, false, 32);
    cfg_conv = cc;
    pio_sm_init(pio_video, sm_conv, off_conv, &cc);
    pio_set_x(pio_video, sm_conv, (uint32_t)conv_color >> 13);

    cc = pio_get_default_sm_config();
    sm_config_set_wrap(&cc, off_video, off_video + video_program.length - 1);
    sm_config_set_sideset_pins(&cc, beginHDMI_PIN_clk);
    sm_config_set_sideset(&cc, 2, false, false);
    for (int i = 0; i < 2; i++) {
        pio_gpio_init(pio_video, beginHDMI_PIN_clk + i);
        gpio_set_drive_strength(beginHDMI_PIN_clk + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_clk + i, GPIO_SLEW_RATE_FAST);
    }
    for (int i = 0; i < 6; i++) {
        pio_gpio_init(pio_video, beginHDMI_PIN_data + i);
        gpio_set_drive_strength(beginHDMI_PIN_data + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_data + i, GPIO_SLEW_RATE_FAST);
    }
    uint64_t clk_mask = (uint64_t)3u << beginHDMI_PIN_clk;
    pio_sm_set_pins_with_mask64(pio_video, sm_video, clk_mask, clk_mask);
    pio_sm_set_pindirs_with_mask64(pio_video, sm_video, clk_mask, clk_mask);
    pio_sm_set_consecutive_pindirs(pio_video, sm_video, beginHDMI_PIN_data, 6, true);
    sm_config_set_out_pins(&cc, beginHDMI_PIN_data, 6);
    sm_config_set_out_shift(&cc, true, true, 30);
    sm_config_set_fifo_join(&cc, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&cc, (float)clock_get_hz(clk_sys) / 252000000.0f);
    cfg_video = cc;
    pio_sm_init(pio_video, sm_video, off_video, &cc);

    /* "line": the ctrl words its blocks load (16-bit indices, chained to
     * "ctrl"; the IRQ after the third) */
    dma_channel_config d = dma_channel_get_default_config(dma_line);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_16);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(pio_video, sm_conv, true));
    channel_config_set_chain_to(&d, dma_ctrl);
    channel_config_set_irq_quiet(&d, true);
    ctrl_quiet = channel_config_get_ctrl_value(&d);
    channel_config_set_irq_quiet(&d, false);
    ctrl_irq = channel_config_get_ctrl_value(&d);
    channel_config_set_irq_quiet(&d, true);
    channel_config_set_read_increment(&d, false);
    ctrl_quiet_fixed = channel_config_get_ctrl_value(&d);

    /* "ctrl": a block (4 words) into the registers of "line", the last
     * write (ctrl_trig) starts it */
    d = dma_channel_get_default_config(dma_ctrl);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, true);
    channel_config_set_ring(&d, true, 4);
    dma_channel_configure(dma_ctrl, &d, &dma_hw->ch[dma_line].read_addr, blocks[0], 4, false);

    /* "conv": an index's 16 bytes to the output machine */
    d = dma_channel_get_default_config(dma_conv);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_chain_to(&d, dma_conv_ctrl);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(pio_video, sm_video, true));
    dma_channel_configure(dma_conv, &d, &pio_video->txf[sm_video], conv_color, 4, false);

    /* "conv ctrl": the address machine's output into "conv", which it starts */
    d = dma_channel_get_default_config(dma_conv_ctrl);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_chain_to(&d, dma_conv);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(pio_video, sm_conv, false));
    dma_channel_configure(dma_conv_ctrl, &d, &dma_hw->ch[dma_conv].read_addr, &pio_video->rxf[sm_conv], 1, false);

    prepare_line(0, 0);
    prepare_line(1, 1);

    /* the IRQ on this core (core 1) */
    irq_set_exclusive_handler(VIDEO_DMA_IRQ, dma_handler_HDMI);
    irq_set_priority(VIDEO_DMA_IRQ, 0);
    dma_channel_acknowledge_irq0(dma_line);
    dma_channel_set_irq0_enabled(dma_line, true);
    irq_set_enabled(VIDEO_DMA_IRQ, true);
    pio_sm_set_enabled(pio_video, sm_conv, true);
    pio_sm_set_enabled(pio_video, sm_video, true);
    dma_channel_start(dma_conv_ctrl);
    dma_channel_start(dma_ctrl);
    audio_on = true;
    running = true;
}

/* The output from line 0 again, after it stopped: the IRQ came more than a
 * line late (core 1's interrupts held off), "ctrl" ran past the lists and
 * the chain died on what it read there. A moment of no picture instead of
 * none until the next reset. */
static void hdmi_restart(void) {
    uint32_t mask = 1u << dma_ctrl | 1u << dma_line | 1u << dma_conv_ctrl | 1u << dma_conv;
    irq_set_enabled(VIDEO_DMA_IRQ, false);
    pio_sm_set_enabled(pio_video, sm_video, false);
    pio_sm_set_enabled(pio_video, sm_conv, false);
    dma_hw->abort = mask;
    while (dma_hw->abort & mask) tight_loop_contents();
    dma_hw->ints0 = 1u << dma_line;
    pio_sm_init(pio_video, sm_conv, off_conv, &cfg_conv);
    pio_set_x(pio_video, sm_conv, (uint32_t)conv_color >> 13);
    pio_sm_init(pio_video, sm_video, off_video, &cfg_video);
    dma_channel_set_read_addr(dma_ctrl, blocks[0], false);
    dma_channel_set_trans_count(dma_ctrl, 4, false);
    dma_channel_set_read_addr(dma_conv, conv_color, false);
    dma_channel_set_trans_count(dma_conv, 4, false);
    dma_channel_set_trans_count(dma_conv_ctrl, 1, false);
    line = 0;
    slot = 0;
    prepare_line(0, 0);
    prepare_line(1, 1);
    hdmi_resyncs++;
    irq_set_enabled(VIDEO_DMA_IRQ, true);
    pio_sm_set_enabled(pio_video, sm_conv, true);
    pio_sm_set_enabled(pio_video, sm_video, true);
    dma_channel_start(dma_conv_ctrl);
    dma_channel_start(dma_ctrl);
}

/* Core 1's loop, often: restarts the output when no frame came for 100 ms */
void hdmi_watchdog(void) {
    static uint32_t last_frames, last_us;
    static bool armed;
    if (!running) return;
    uint32_t now = time_us_32(), f = hdmi_frame_count;
    if (!armed || f != last_frames) {
        armed = true;
        last_frames = f;
        last_us = now;
    } else if (now - last_us > 100000) {
        printf("HDMI: output stopped at frame %lu, restarted\n", (unsigned long)f);
        hdmi_restart();
        last_us = now;
    }
}

/* DMA starts at normal priority to avoid starving SD/PSRAM during init;
 * main.c raises it after BIOS loading. "line" takes its ctrl word from each
 * block, so the words change too. */
void hdmi_set_dma_high_priority(void) {
    ctrl_quiet |= DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS;
    ctrl_irq |= DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS;
    ctrl_quiet_fixed |= DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS;
    hw_set_bits(&dma_hw->ch[dma_ctrl].al1_ctrl, DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS);
    hw_set_bits(&dma_hw->ch[dma_conv].al1_ctrl, DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS);
    hw_set_bits(&dma_hw->ch[dma_conv_ctrl].al1_ctrl, DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS);
}
