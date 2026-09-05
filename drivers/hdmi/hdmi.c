#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdalign.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/pio.h>
#include <hardware/sync.h>
#include <pico/time.h>
#include <pico/multicore.h>
#include <hardware/clocks.h>
#include "hardware/structs/bus_ctrl.h"
#include <arm_acle.h>

#include "vga.h"
#include "vga_osd.h"
#include "hdmi.h"
#include "font8x16.h"

// Portable 8-bit bit reversal. GCC 13 does not expose __rbit() through
// <arm_acle.h> for this build configuration, so avoid relying on that intrinsic.
static inline uint8_t reverse_bits8(uint8_t v) {
    v = (uint8_t)((v >> 4) | (v << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}

//PIO параметры
static uint offs_prg0 = 0;
static uint offs_prg1 = 0;

//SM
static int SM_video = -1;
static int SM_conv = -1;

//активный видеорежим
extern int current_mode;  // Default text mode

//буфер  палитры 256 цветов в формате R8G8B8
extern uint32_t palette_a[256];

#define SCREEN_WIDTH (320)
#define SCREEN_HEIGHT (240)

/* The guest's VGA RAM.  Was hardcoded at 256 KB in two files while
 * EMU_VGA_MEM_SIZE_KB configured a third place; deriving it means the buffer
 * and the size the emulator reports can no longer disagree. */
#ifndef EMU_VGA_MEM_SIZE_KB
#define EMU_VGA_MEM_SIZE_KB 256
#endif
#define GFX_BUFFER_SIZE (EMU_VGA_MEM_SIZE_KB * 1024)
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
extern uint8_t  frame_panning_split_off;
extern uint8_t  frame_crtc_mode;
extern uint8_t  frame_max_scan;
extern uint8_t  frame_border_pix;
extern volatile uint8_t frame_blank_active;

/* Frames the guest has held the display blanked, and the bound past which we
 * stop believing it (see dma_handler_HDMI).  ~2 s at 60 Hz. */
#define HDMI_BLANK_MAX_FRAMES 120u
static uint32_t hdmi_blank_frames = 0;
extern uint8_t  frame_preset_row;
extern uint8_t  frame_text_char_h;
extern uint8_t  frame_overscan;
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
extern const uint32_t * volatile hdmi_ega320_cache_active;
extern const uint32_t * volatile hdmi_ega320_cache_pending;

// #define HDMI_WIDTH 480 //480 Default
// #define HDMI_HEIGHT 644 //524 Default
// #define HDMI_HZ 52 //60 Default

//DMA каналы
//каналы работы с первичным графическим буфером
static int dma_chan_ctrl;
static int dma_chan;
//каналы работы с конвертацией палитры
static int dma_chan_pal_conv_ctrl;
static int dma_chan_pal_conv;

//DMA буферы
//основные строчные данные
// Four line buffers for lower-priority rendering.
// Keep these DMA control structures in the original low-contention scratch
// bank.  Moving them into the busy main-SRAM bank makes HDMI startup unstable
// on Z2.  The large conversion LUT, not these bottom-of-bank words, was the
// object observed being overwritten at runtime.
static uint32_t* __scratch_y("hdmi_ptr_3") dma_lines[4] = { NULL,NULL,NULL,NULL };
static uint32_t* __scratch_y("hdmi_ptr_4") DMA_BUF_ADDR[4] __aligned(16);
// Buffers 0-1 live in conv_color's tail; buffers 2-3 are separate.
static uint32_t hdmi_extra_line_buf[2][100];

//ДМА палитра для конвертации
//в хвосте этой памяти выделяется dma_data
alignas(4096) uint32_t conv_color[1224];
uint32_t conv_color2[1024]; // backup to fast restore pallete
bool required_to_repair_text_pal = false;

//индекс, проверяющий зависание
static volatile uint32_t irq_inx = 0;

// SWD-readable timing diagnostics. EGA renderers are the heaviest users of
// the scanline IRQ and must leave enough headroom before the next 31.8 us line.
volatile uint32_t hdmi_isr_last_us = 0;
volatile uint32_t hdmi_isr_max_us = 0;
volatile uint32_t hdmi_isr_over_30_count = 0;
volatile uint32_t hdmi_sync_isr_max_us = 0;
volatile uint32_t hdmi_render_queue_overflow = 0;
volatile uint32_t hdmi_render_late_drop = 0;

#define HDMI_RENDER_QUEUE_SIZE 8u
typedef struct {
    uint16_t line;
    uint16_t deadline_irq;
    uint8_t buffer;
} hdmi_render_job_t;

static hdmi_render_job_t hdmi_render_jobs[HDMI_RENDER_QUEUE_SIZE];
static volatile uint32_t hdmi_render_head = 0;
static volatile uint32_t hdmi_render_tail = 0;
static int hdmi_render_irq = -1;

static inline bool hdmi_render_deadline_passed(uint16_t deadline_irq) {
    return (int16_t)((uint16_t)irq_inx - deadline_irq) >= 0;
}


//функции и константы HDMI

/*
 * The four HDMI control symbols travel in the same byte stream as pixels and
 * index the same 256-entry conv_color[] table, so four byte values can never
 * be pixels - both palette setters refuse those indices for that reason.
 * Which four is a free choice, and 252..255 was the worst one available.
 *
 * A 640-pixel 16-colour line packs two 4-bit colours per byte, high nibble
 * on the left, so 0xfc..0xff mean "white next to light red / light magenta /
 * yellow / white".  A run of white is 0xff in every single byte.  That is why
 * every white area - the boot menu, M602's white text, the white background
 * of the Windows 95 installer - had every second pixel forced down to 0xfb,
 * which is white next to light cyan.  The coloured fringing that has been
 * visible since the beginning was never a rendering bug; the encoding simply
 * ran out of room in the one place it shows most.
 *
 * 0xd4..0xd7 mean "light magenta next to red / magenta / brown / light grey".
 * Nothing in ordinary text or in a UI puts those side by side, and none of
 * them is a same-colour run, so they cannot appear in a flat fill at all.
 * The cost in 256-colour modes moves with them, from palette entries 252-255
 * at the top of the VGA grey ramp to 212-215 in the middle of the colour
 * cube, which is no worse and is used less.
 */
#define HDMI_CTRL_0 (0xd4)
#define HDMI_CTRL_1 (0xd5)
#define HDMI_CTRL_2 (0xd6)
#define HDMI_CTRL_3 (0xd7)

//программа конвертации адреса
uint16_t pio_program_instructions_conv_HDMI[] = {
    0x80a0, //  0: pull   block
    0x40e8, //  1: in     osr, 8
    0x4034, //  2: in     x, 20
    0x8020, //  3: push   block
};


const struct pio_program pio_program_conv_addr_HDMI = {
    .instructions = pio_program_instructions_conv_HDMI,
    .length = 4,
    .origin = -1,
};

//программа видеовывода
static const uint16_t instructions_PIO_HDMI[] = {
    0x7006, //  0: out    pins, 6         side 2
    0x7006, //  1: out    pins, 6         side 2
    0x7006, //  2: out    pins, 6         side 2
    0x7006, //  3: out    pins, 6         side 2
    0x7006, //  4: out    pins, 6         side 2
    0x6806, //  5: out    pins, 6         side 1
    0x6806, //  6: out    pins, 6         side 1
    0x6806, //  7: out    pins, 6         side 1
    0x6806, //  8: out    pins, 6         side 1
    0x6806, //  9: out    pins, 6         side 1
};

static const struct pio_program program_PIO_HDMI = {
    .instructions = instructions_PIO_HDMI,
    .length = 10,
    .origin = -1,
};

static uint64_t __time_critical_func(get_ser_diff_data)(const uint16_t dataR, const uint16_t dataG, const uint16_t dataB) {
    uint64_t out64 = 0;
    for (int i = 0; i < 10; i++) {
        out64 <<= 6;
        if (i == 5) out64 <<= 2;
#ifdef BOARD_PC
        uint8_t bG = (dataR >> (9 - i)) & 1;
        uint8_t bR = (dataG >> (9 - i)) & 1;
#else
        uint8_t bR = (dataR >> (9 - i)) & 1;
        uint8_t bG = (dataG >> (9 - i)) & 1;
#endif
        uint8_t bB = (dataB >> (9 - i)) & 1;

        bR |= (bR ^ 1) << 1;
        bG |= (bG ^ 1) << 1;
        bB |= (bB ^ 1) << 1;

        if (HDMI_PIN_invert_diffpairs) {
            bR ^= 0b11;
            bG ^= 0b11;
            bB ^= 0b11;
        }
        uint8_t d6;
        if (HDMI_PIN_RGB_notBGR) {
            d6 = (bR << 4) | (bG << 2) | (bB << 0);
        }
        else {
            d6 = (bB << 4) | (bG << 2) | (bR << 0);
        }


        out64 |= d6;
    }
    return out64;
}

//конвертор TMDS
static uint __time_critical_func(tmds_encoder)(const uint8_t d8) {
    int s1 = 0;
    for (int i = 0; i < 8; i++) s1 += (d8 & (1 << i)) ? 1 : 0;
    bool is_xnor = false;
    if ((s1 > 4) || ((s1 == 4) && ((d8 & 1) == 0))) is_xnor = true;
    uint16_t d_out = d8 & 1;
    uint16_t qi = d_out;
    for (int i = 1; i < 8; i++) {
        d_out |= ((qi << 1) ^ (d8 & (1 << i))) ^ (is_xnor << i);
        qi = d_out & (1 << i);
    }

    if (is_xnor) d_out |= 1 << 9;
    else d_out |= 1 << 8;

    return d_out;
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

#define is_hdmi_sync(c) ((((c) & 0xfcu) == HDMI_CTRL_0))

/*
 * Replacement for a pixel byte that collides with a control symbol.
 *
 * Flipping bit 3 - which is what this used to do unconditionally - reasons
 * about the byte as a *pair* of 4-bit colours, where it only brightens the
 * right-hand pixel by one intensity step.  In a 256-colour mode the byte is
 * not a pair: it is the palette index itself, and 0xd4..0xd7 then land on
 * 0xdc..0xdf, four entirely unrelated entries of the guest's palette.
 *
 * Dune II draws the Westwood logo entirely in indices 212..215 - a blue ramp
 * whose last step, #0f38bf, is 3964 of the logo's 10414 non-black pixels -
 * while 220..223 held white and dark navy.  The logo came out white, and the
 * sparkle, which is an animation of exactly those four palette entries, could
 * not be seen at all.  The same substitution turned dark purple shadows in
 * the intro's throne room into light grey speckles.
 *
 * So the substitute is chosen by colour instead: whoever programs the palette
 * calls hdmi_set_pixel_substitutes() with the index whose colour is nearest
 * to each reserved one.  Palettes very often hold the same colour twice -
 * Dune II's holds this ramp again at 227, 229 and 231 - and then the
 * substitution is exact.  The bit-3 flip stays as the initial value, so a
 * mode that never programs a palette behaves exactly as it did.
 */
static uint8_t hdmi_ctrl_sub[4] = {
    (uint8_t)(HDMI_CTRL_0 ^ 8u), (uint8_t)(HDMI_CTRL_1 ^ 8u),
    (uint8_t)(HDMI_CTRL_2 ^ 8u), (uint8_t)(HDMI_CTRL_3 ^ 8u),
};

#define hdmi_pixel_fixup(c) (hdmi_ctrl_sub[(c) & 3u])

/* sub[k] is the byte to emit in place of HDMI_CTRL_0 + k. */
void hdmi_set_pixel_substitutes(const uint8_t *sub) {
    for (int k = 0; k < 4; k++) {
        uint8_t s = sub[k];
        /* A substitute that is itself a control symbol would defeat the point. */
        hdmi_ctrl_sub[k] = is_hdmi_sync(s) ? (uint8_t)(s ^ 8u) : s;
    }
}

#define ob(x) { register uint8_t c = x; *output_buffer++ = is_hdmi_sync(c) ? hdmi_pixel_fixup(c) : c; }

/*
 * Pixel bytes index conv_color[], but four of them are the HDMI control
 * symbols (see HDMI_CTRL_0).  A 640-pixel EGA line packs two 4-bit colours
 * into each byte, so a perfectly valid colour pair can otherwise select a
 * sync symbol in the middle of active video.  The original byte-at-a-time
 * ob() path protected against this; keep the same protection in the newer
 * 32-bit EGA renderer.
 */
static __attribute__((always_inline)) inline uint8_t hdmi_safe_pixel_index(uint8_t c) {
    return is_hdmi_sync(c) ? hdmi_pixel_fixup(c) : c;
}

static void __time_critical_func(render_text_line)(uint32_t line, uint8_t *output_buffer) {
    /*
     * Split screen, row-scan preset and CRTC stride.
     *
     * Above the line-compare match the CRTC walks from the frame's start
     * address with the row-scan counter preset from CR08; on the line after
     * the match it restarts at address 0 with row scan 0.  Text mode had none
     * of this: it always read from the start address with a hard-wired
     * 16-line character, so everything below the split came from past the end
     * of the top region's data.
     *
     * Prehistorik 2's intro is built entirely on that mechanism - a smooth
     * scrolling message in the top region, and below the split a logo drawn
     * with a redefined font.  On the board it showed the message alone on a
     * black screen, because the rest of the screen was reading blanks.
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
    /* gfx_buffer holds 65536 cells; a row that would run past the end is not
     * something any real mode asks for, and clamping beats reading out. */
    if (row_cell + (uint32_t)cols <= 65536u) {
        const uint32_t *text_row = (const uint32_t *)gfx_buffer + row_cell;

        for (int col = 0; col < cols; col++) {
            uint16_t cell = text_row[col];
            uint8_t ch   = (uint8_t)(cell & 0xFF);
            uint8_t attr = (uint8_t)(cell >> 8);
            bool cursor_here = cursor_blink_state && col == cursor_x &&
                char_row == (uint32_t)cursor_y &&
                glyph_line >= (uint32_t)cursor_start &&
                glyph_line <= (uint32_t)cursor_end;
            register uint8_t glyph;
            if (cursor_here) {
                glyph = 0xFF;
            } else {
                const uint8_t *fp = vga_get_font_ptr(vga_state, ch, (attr >> 3) & 1);
                if (fp) {
                    glyph = reverse_bits8(fp[glyph_line * 4]);
                } else {
                    glyph = font_8x16[ch * 16 + glyph_line];
                }
            }
           // uint8_t blink_or_highlite_bg = attr & 0b10000000; // TODO: use it?
            register uint8_t fg_color0 = attr & 0b00001111;
            /* Attribute bit 7 is blink when AC mode-control bit 3 is set;
             * otherwise it is the background-intensity bit. */
            register uint8_t bg_index = (attr >> 4) & 0x07;
            if (vga_state && !(vga_state->ar[0x10] & 0x08)) {
                bg_index |= (attr >> 4) & 0x08;
            }
            // In blink mode, attribute bit 7 hides the glyph for one phase.
            if (!cursor_here && vga_state && (vga_state->ar[0x10] & 0x08) &&
                (attr & 0x80) && !cursor_blink_state) {
                glyph = 0;
            }
            register uint8_t bg_color1 = bg_index << 4;
            register uint8_t bg_color0 = bg_index;
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

static inline uint32_t line_compare_src(int height);

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
        /*
         * frame_line_compare counts *display* lines, but src_line is in
         * source lines and a 320x200 mode shows each source line twice.
         * Comparing the two directly - which is what this renderer alone
         * still did - put the split twice as far down the screen as the
         * guest asked for.  The Legend of Kyrandia scrolls its title screen
         * in with a shrinking split, so half the picture came from the
         * scrolled offset and half from address 0, where the previous game's
         * framebuffer was still sitting.
         */
        uint32_t offset;
        if (frame_line_compare >= 0 && (int)line > frame_line_compare) {
            offset = (src_line - line_compare_src(gfx_height)) * stride;
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
// Spread the bits of one plane byte into bit positions 0,4,8,...28.
// Computing this directly avoids a 1 KiB LUT: scratch Y is vulnerable to the
// core-0 stack, while moving that LUT to main SRAM exhausts the emulator heap.
static __attribute__((always_inline)) inline uint32_t
ega_spread8(uint32_t plane) {
    plane = (plane | (plane << 12)) & 0x000F000Fu;
    plane = (plane | (plane <<  6)) & 0x03030303u;
    plane = (plane | (plane <<  3)) & 0x11111111u;
    return plane;
}

// Merge 4 plane bytes [P3|P2|P1|P0] into 8 nibbles (pixel color indices).
static __attribute__((always_inline)) inline uint32_t
ega_pack8_from_planes(const uint32_t ega_planes) {
    return
     ega_spread8((uint8_t)ega_planes) |
     ega_spread8((uint8_t)(ega_planes >> 8)) << 1 |
     ega_spread8((uint8_t)(ega_planes >> 16)) << 2 |
     ega_spread8((uint8_t)(ega_planes >> 24)) << 3;
}

static __attribute__((always_inline)) inline uint32_t ega_pair(uint8_t ab) {
    return ((uint32_t)(ab & 15) << 8) | (uint32_t)(ab >> 4);
}

// Render EGA planar 16-color graphics line
// Supports both 320x200 (doubled) and 640x350 (native) modes
// Reads from SRAM buffer (copied from PSRAM during main loop)
/*
 * Convert the CRTC's split-screen line into source-line units.
 *
 * frame_line_compare counts *display* lines, while the renderers address VRAM
 * in source lines - in a 320x200 mode the display has 400 lines and each
 * source line is shown twice.  Comparing the two directly means the split
 * never happens: Supaplex asks for line compare 351 and src_line never gets
 * past 199, so its status panel came from the scrolling offset instead of
 * from address 0, ending up too low and cut off.  The mapping here is the
 * same one each renderer uses to derive src_line from line.
 */
/*
 * The CRTC's compatibility address wrap (CR17 bits 0 and 1), reduced to a
 * mask.
 *
 * Clear bit 0 and address bit 13 comes from row-scan bit 0 instead of the
 * counter; clear bit 1 and bit 14 comes from row-scan bit 1.  With a maximum
 * scan line of zero the row counter never leaves zero, so this is a wrap at
 * 8 KB (and 16 KB) - which is what Prehistorik 2's scrolling menus are built
 * on: they keep an 8 KB ring and let the start address run past its end.
 *
 * The substitution applies to *every* address the line reads, not just its
 * start: a line is 39 words long and one starting just below the boundary
 * crosses it partway across.  Doing that with a call per word made the
 * scanline renderer miss its deadline and the picture jitter, so the whole
 * thing collapses to an AND and an OR computed once per line.
 */
/*
 * Every address this renderer computes, one slot per display line, in the
 * unused port-write histogram at guest 0xb5000.
 *
 * The model I reconstructed by hand says the whole window has content, but
 * the screen shows a blank band along the bottom, so the renderer is reading
 * somewhere I am not predicting.  Rather than guess again, let it say what it
 * actually reads: dump this and compare against the same arithmetic done on
 * the host.
 */
#ifndef HDMI_ADDR_LOG_ENABLED
#define HDMI_ADDR_LOG_ENABLED 0        /* debugging aid; off in normal builds */
#endif
/* Guest 0xa2000: the instruction ring's space, which is free whenever the
 * per-instruction hook is compiled out (AUDIO_DIAG_HOT=OFF).  It must not
 * go at 0xb5000 - that is the port write histogram, which frank_diag_port()
 * keeps updating and which trampled the first version of this log. */
#define HDMI_ADDR_LOG ((volatile uint32_t *)(0x11000000u + 0x000a2000u))

static inline void crtc_compat_mask(uint32_t src_line,
                                    uint32_t *and_mask, uint32_t *or_bits)
{
    uint32_t a = 0xFFFFu, o = 0u;

    if ((frame_crtc_mode & 0x03u) != 0x03u) {
        uint32_t row = frame_max_scan ? (src_line % (frame_max_scan + 1u)) : 0u;
        if (!(frame_crtc_mode & 0x01u)) { a &= ~0x2000u; o |= (row & 1u) << 13; }
        if (!(frame_crtc_mode & 0x02u)) { a &= ~0x4000u; o |= ((row >> 1) & 1u) << 14; }
    }
    *and_mask = a;
    *or_bits  = o;
}

static inline uint32_t line_compare_src(int height)
{
    /* The address counter is reset *after* the matching scanline, so the
     * first line of the split region is line_compare + 1.  Starting it one
     * line early made the region a source line too tall, and that extra line
     * read past the end of Supaplex's panel into the playfield tiles behind
     * it - a strip of red and green blocks along the bottom edge. */
    uint32_t lc = (uint32_t)frame_line_compare + 1u;

    if (height <= 100) return lc >> 2;
    if (height <= 200) return lc >> 1;
    if (height <= 350) {
        int act = active_end - active_start;
        return act > 0 ? (lc * (uint32_t)height) / (uint32_t)act : lc;
    }
    return lc;
}

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

    const uint32_t *cached_frame = hdmi_ega320_cache_active;
    if (cached_frame && gfx_width == 320 && height == 200) {
        const uint32_t *cached_row = cached_frame + src_line * 40u;
        uint32_t *out32 = (uint32_t *)output_buffer;
        for (uint32_t i = 0; i < 40u; ++i) {
            uint32_t eight_pixels = cached_row[i];
            *out32++ = ega_pair(eight_pixels >> 24) |
                       (ega_pair(eight_pixels >> 16) << 16);
            *out32++ = ega_pair(eight_pixels >> 8) |
                       (ega_pair(eight_pixels) << 16);
        }
        return;
    }

    uint32_t gfx_width8 = gfx_width >> 3;
    uint32_t stride = gfx_line_offset > 0 ? (gfx_line_offset << 1) : gfx_width8;

    uint32_t offset;
    bool in_split = (frame_line_compare >= 0 && (int)line > frame_line_compare);
    if (in_split) {
        /* The split region always restarts at address 0; compare in display
         * lines and subtract the split point in source lines. */
        offset = (src_line - line_compare_src(height)) * stride;
    } else {
        offset = frame_vram_offset + src_line * stride;
    }

    uint32_t wrap_and, wrap_or;
    crtc_compat_mask(src_line, &wrap_and, &wrap_or);
    offset = ((offset & 0xFFFFu) & wrap_and) | wrap_or;

    /*
     * The address has to be wrapped for *every* word of the line, not just
     * for its start.  A line is 39 words long, so one that begins just below
     * the compatibility wrap boundary crosses it partway across and the
     * hardware folds the rest back; reading on linearly puts the tail of such
     * a line outside the ring the game maintains, which is the seam along the
     * right edge and around the fold.  The look-ahead word that pixel panning
     * needs has to be wrapped for the same reason.
     */
#if HDMI_ADDR_LOG_ENABLED
    if (line < 400u) {
        HDMI_ADDR_LOG[line] = offset | (src_line << 16);
        if (line == 0u) {
            HDMI_ADDR_LOG[400] = frame_vram_offset;
            HDMI_ADDR_LOG[401] = wrap_and;
            HDMI_ADDR_LOG[402] = wrap_or;
            HDMI_ADDR_LOG[403] = (uint32_t)stride | ((uint32_t)gfx_width8 << 16);
        }
    }
#endif

    register const uint32_t *vram = (const uint32_t *)gfx_buffer;
    /* Panning is suppressed below the split when the guest asks for it, which
     * is what keeps the strip anchored while the rest of the screen scrolls. */
    register int panning = (in_split && frame_panning_split_off)
                         ? 0 : frame_pixel_panning;
    register uint8_t shift1 = panning << 2;
    register uint8_t shift2 = 32 - shift1;

    // Loop over display width
    int words_to_render = gfx_width8;
    if (words_to_render > 80) words_to_render = 80; // Cap at 640px

    register uint32_t* out32 = (uint32_t*)output_buffer;
    // 320-wide mode: double each pixel horizontally
    for (int i = 0; i < words_to_render; ++i) {
        uint32_t a0 = (((offset + (uint32_t)i) & wrap_and) | wrap_or);
        register uint32_t eight_pixels = ega_pack8_from_planes(vram[a0]);
        if (panning > 0) {
            uint32_t a1 = (((offset + (uint32_t)i + 1u) & wrap_and) | wrap_or);
            eight_pixels = (eight_pixels << shift1) | (ega_pack8_from_planes(vram[a1]) >> shift2);
        }
        *out32++ = ega_pair(eight_pixels >> 24) | (ega_pair(eight_pixels >> 16) << 16);
        *out32++ = ega_pair(eight_pixels >> 8) | (ega_pair(eight_pixels) << 16);
    }

    /* Everything past the active display is overscan, not last line's
     * leftovers.  With gfx_width 312 that is the final eight pixels. */
    uint32_t written = (uint32_t)words_to_render * 8u;
    if (written < SCREEN_WIDTH)
        nf_memset(output_buffer + written, frame_border_pix,
                  SCREEN_WIDTH - written);
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
    bool in_split = (frame_line_compare >= 0 && (int)line > frame_line_compare);
    if (in_split) {
        /* The split region always restarts at address 0; compare in display
         * lines and subtract the split point in source lines. */
        offset = (src_line - line_compare_src(height)) * stride;
    } else {
        offset = frame_vram_offset + src_line * stride;
    }

    uint32_t wrap_and, wrap_or;
    crtc_compat_mask(src_line, &wrap_and, &wrap_or);
    offset = ((offset & 0xFFFFu) & wrap_and) | wrap_or;

    register const uint32_t *vram = (const uint32_t *)gfx_buffer;
    /* Panning is suppressed below the split when the guest asks for it, which
     * is what keeps the strip anchored while the rest of the screen scrolls. */
    register int panning = (in_split && frame_panning_split_off)
                         ? 0 : frame_pixel_panning;
    register uint8_t shift1 = panning << 2;
    register uint8_t shift2 = 32 - shift1;

    // Loop over display width
    int words_to_render = gfx_width8;
    if (words_to_render > 80) words_to_render = 80; // Cap at 640px

    register uint32_t *out32 = (uint32_t *)output_buffer;
    // 640-wide mode: no horizontal doubling. Each packed word already holds
    // four two-pixel palette indices; only byte order needs reversing.
    for (register int i = 0; i < words_to_render; i++) {
        uint32_t a0 = (((offset + (uint32_t)i) & wrap_and) | wrap_or);
        register uint32_t eight_pixels = ega_pack8_from_planes(vram[a0]);

        if (panning > 0) {
            uint32_t a1 = (((offset + (uint32_t)i + 1u) & wrap_and) | wrap_or);
            eight_pixels = (eight_pixels << shift1) | (ega_pack8_from_planes(vram[a1]) >> shift2);
        }
        uint32_t packed = __builtin_bswap32(eight_pixels);
        *out32++ =
            (uint32_t)hdmi_safe_pixel_index((uint8_t)packed) |
            ((uint32_t)hdmi_safe_pixel_index((uint8_t)(packed >> 8)) << 8) |
            ((uint32_t)hdmi_safe_pixel_index((uint8_t)(packed >> 16)) << 16) |
            ((uint32_t)hdmi_safe_pixel_index((uint8_t)(packed >> 24)) << 24);
    }
    /* Same as the 320-wide path: the tail of the line is overscan.  Here each
     * output byte carries two pixels, so the fill value is doubled up. */
    uint32_t written640 = (uint32_t)words_to_render * 4u;
    if (written640 < SCREEN_WIDTH)
        nf_memset(output_buffer + written640, frame_border_pix,
                  SCREEN_WIDTH - written640);
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

/* Render pixels at a lower interrupt priority. The DMA IRQ is priority 0 and
 * can preempt this worker every scanline to keep the control channel fed.
 * With four buffers the queued line has roughly three scanlines of lead time.
 */
static void __isr __time_critical_func(hdmi_render_worker)(void) {
    uint32_t tail = hdmi_render_tail;
    if (tail == hdmi_render_head) return;

    hdmi_render_job_t job = hdmi_render_jobs[tail & (HDMI_RENDER_QUEUE_SIZE - 1u)];
    __dmb();
    hdmi_render_tail = tail + 1u;

    uint32_t started_us = timer_hw->timerawl;
    bool transactional_ega = vga_state && current_mode == 2 &&
        gfx_submode == 6 && gfx_width == 320 && gfx_height == 200 &&
        !osd_is_visible();
    uint8_t *output = (uint8_t *)dma_lines[job.buffer] + 72;

    if (transactional_ega) {
        // 320x200 is vertically doubled. The preceding even job atomically
        // filled both DMA buffers, so the odd job deliberately does no work.
        if (job.line & 1u)
            goto render_done;

        uint16_t second_deadline = job.deadline_irq + 1u;
        if (hdmi_render_deadline_passed(second_deadline)) {
            hdmi_render_late_drop += 2u;
            goto render_done;
        }

        uint32_t staging[SCREEN_WIDTH / sizeof(uint32_t)] __aligned(16);
        render_line(job.line, (uint8_t *)staging);

        // The complete line is committed in one short SRAM burst. Holding off
        // the priority-0 sync IRQ for this sub-microsecond copy guarantees it
        // cannot select the target buffer halfway through the commit.
        uint32_t irq_state = save_and_disable_interrupts();
        bool first_ok = !hdmi_render_deadline_passed(job.deadline_irq);
        bool second_ok = !hdmi_render_deadline_passed(second_deadline);
        if (second_ok) {
            uint32_t *dst = (uint32_t *)output;
            uint32_t *dst_second = (uint32_t *)dma_lines[(job.buffer + 1u) & 3u] + 18;
            for (uint32_t i = 0; i < SCREEN_WIDTH / sizeof(uint32_t); ++i) {
                uint32_t pixels = staging[i];
                if (first_ok)
                    dst[i] = pixels;
                dst_second[i] = pixels;
            }
            if (!first_ok)
                ++hdmi_render_late_drop;
        } else {
            hdmi_render_late_drop += 2u;
        }
        restore_interrupts(irq_state);
    } else {
        render_line(job.line, output);
    }

render_done:
    uint32_t elapsed_us = timer_hw->timerawl - started_us;
    hdmi_isr_last_us = elapsed_us;
    if (elapsed_us > hdmi_isr_max_us) hdmi_isr_max_us = elapsed_us;
    if (elapsed_us > 30) ++hdmi_isr_over_30_count;

    if (hdmi_render_tail != hdmi_render_head)
        irq_set_pending((uint)hdmi_render_irq);
}

static inline void hdmi_queue_render(uint32_t line, uint32_t buffer) {
    uint32_t head = hdmi_render_head;
    if (head - hdmi_render_tail >= HDMI_RENDER_QUEUE_SIZE) {
        ++hdmi_render_queue_overflow;
        return;
    }
    hdmi_render_job_t *job = &hdmi_render_jobs[head & (HDMI_RENDER_QUEUE_SIZE - 1u)];
    job->line = (uint16_t)line;
    job->deadline_irq = (uint16_t)(irq_inx + 2u);
    job->buffer = (uint8_t)buffer;
    __dmb();
    hdmi_render_head = head + 1u;
    irq_set_pending((uint)hdmi_render_irq);
}

static void __time_critical_func(dma_handler_HDMI)() {
    uint32_t sync_started_us = timer_hw->timerawl;
    static uint line = 0;
    irq_inx++;

    dma_hw->ints0 = 1u << dma_chan_ctrl;

    if (line >= 524) {
        line = 0;
        frame_update_request = 1;

        /*
         * Honour the guest's display blank (attribute controller PAS bit
         * clear).  A real VGA shows nothing while a game reprograms itself
         * behind the blank; we used to keep scanning out, so the outgoing
         * screen stayed visible under the incoming palette.
         *
         * This has to live here rather than in vga_hw.c: dma_handler_vga()
         * is the analogue path and never runs on an HDMI build.
         *
         * The frame bound is the point of the counter.  A guest that leaves
         * PAS clear must not be able to black the board out until the next
         * reboot - that is why the blank used to be ignored outright.
         */
        if (vga_state && !(vga_state->ar_index & 0x20)) {
            if (hdmi_blank_frames < HDMI_BLANK_MAX_FRAMES)
                hdmi_blank_frames++;
        } else {
            hdmi_blank_frames = 0;
        }
        frame_blank_active = (hdmi_blank_frames != 0 &&
                              hdmi_blank_frames < HDMI_BLANK_MAX_FRAMES);
    } else {
        ++line;
    }

    // Never carry late active-video work into the next frame. A stale job's
    // target buffer has already wrapped and may be in use by DMA again.
    if (line == 480) {
        hdmi_render_tail = hdmi_render_head;

        /*
         * Latch the CRTC start address here, at the START of vertical
         * blanking, because that is where a real VGA loads it into its
         * address counter.  It used to be read at line 521, at the end of
         * blanking, and that one difference is visible:
         *
         * Prehistorik 2 scrolls a pixel per frame and writes, in the same
         * blanking interval, "start += 1" together with "panning = 7", then
         * "panning = 0" in the next one.  On hardware the start address
         * write comes too late for this frame's latch, so it lands one frame
         * after the panning and the two stay in step.  Latching at 521 caught
         * it in the same frame, putting the picture 8 px ahead for one frame
         * and 8 px back the next - a jump every eight pixels of scroll.
         *
         * Written as two separate OUTs, so do not combine bytes from two
         * different page-flip addresses.
         */
        if (vga_state) {
            const uint8_t *cr = vga_state->cr;
            uint8_t hi, lo, hi_check, lo_check;
            do {
                hi = cr[0x0c];
                lo = cr[0x0d];
                __dmb();
                hi_check = cr[0x0c];
                lo_check = cr[0x0d];
            } while (hi != hi_check || lo != lo_check);
            frame_vram_offset = (uint16_t)((hi << 8) | lo);
        }
    }

    // Update VGA status register 1 (port 0x3DA) from ISR
    if (vga_state) {
        if (line >= 480) {
            vga_state->st01 |=  ST01_V_RETRACE;
            vga_state->st01 &= ~ST01_DISP_ENABLE;
        } else {
            vga_state->st01 &= ~ST01_V_RETRACE;
            vga_state->st01 |=  ST01_DISP_ENABLE;
        }
    }

    // Preserve the original four-buffer phase exactly.
    uint32_t read_buf = line & 3u;
    uint32_t render_buf = (line + 2u) & 3u;
    dma_channel_set_read_addr(dma_chan_ctrl, &DMA_BUF_ADDR[read_buf], false);

    uint8_t* activ_buf = (uint8_t *)dma_lines[render_buf];

    if (line < 480) { //область изображения
        uint8_t* output_buffer = activ_buf + 72;
        /* The guest has the display blanked (attribute controller PAS bit
         * clear) while it reprograms itself.  A real VGA shows nothing, so
         * neither do we - otherwise the outgoing screen stays visible under
         * the incoming palette.  vga_hw.c bounds how long this can last. */
        if (frame_blank_active) {
            nf_memset(output_buffer, frame_border_pix, SCREEN_WIDTH);
            goto active_sync;
        }
        /* Above and below the picture is letterbox, not part of the guest's
         * screen - paint it black rather than palette index 0. */
        if (line < (uint32_t)active_start) {
            nf_memset(output_buffer, frame_border_pix, SCREEN_WIDTH);
            goto active_sync;
        }
        if (line >= (uint32_t)active_end) {
            nf_memset(output_buffer, frame_border_pix, SCREEN_WIDTH);
            goto active_sync;
        }

        /*
         * The 320x200x16 planar converter is now fully inlined into SRAM and
         * completes in about 7 us.  Render it here, while the target buffer is
         * exactly two scanlines ahead of DMA, instead of sending it through
         * the deferred queue.  This removes the paired-buffer ownership race
         * which could display complete but stale scanlines even though no
         * deadline/overflow counter fired.
         */
        if (vga_state && current_mode == 2 && gfx_submode == 6 &&
            gfx_width == 320 && gfx_height == 200 && !osd_is_visible()) {
            uint32_t render_started_us = timer_hw->timerawl;
            render_gfx_line_ega320(line - active_start, output_buffer);
            uint32_t render_us = timer_hw->timerawl - render_started_us;
            hdmi_isr_last_us = render_us;
            if (render_us > hdmi_isr_max_us) hdmi_isr_max_us = render_us;
            if (render_us > 30) ++hdmi_isr_over_30_count;
            goto active_sync;
        }

        hdmi_queue_render(line - active_start, render_buf);
active_sync:
        nf_memset(activ_buf + 48, HDMI_CTRL_0, 24);
        nf_memset(activ_buf, HDMI_CTRL_1, 48);
        nf_memset(activ_buf + 392, HDMI_CTRL_0, 8);
    }
    else {
        if ((line >= 490) && (line < 492)) {
            //кадровый синхроимпульс
            //для выравнивания синхры
            // --|_|---|_|---|_|----
            //---|___________|-----
            nf_memset(activ_buf + 48, HDMI_CTRL_2, 352);
            nf_memset(activ_buf, HDMI_CTRL_3, 48);
        }
        else {
            //ССИ без изображения
            //для выравнивания синхры
            nf_memset(activ_buf + 48, HDMI_CTRL_0, 352);
            nf_memset(activ_buf, HDMI_CTRL_1, 48);
        };

        // Line N_LINES_TOTAL-4 (521): late in vblank, just before DMA needs line 0.
        // Wolf3D has already written the new page address to CRTC by now.
        // Read cr[] and ar[] directly — no intermediate volatile copies.
        if (line == 521) {
            if (vga_state) {
                const uint8_t *cr = vga_state->cr;
                /* The CRTC start address is latched at line 480 instead -
                 * see the comment there.  Pixel panning stays here, at the
                 * end of blanking, because real hardware applies it to the
                 * very next scanlines rather than holding it for a frame. */
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
                frame_line_compare = (lc > 0 && lc < 480) ? lc : -1;
            }

            const uint32_t *pending = hdmi_ega320_cache_pending;
            if (pending) {
                __dmb();
                hdmi_ega320_cache_active = pending;
                hdmi_ega320_cache_pending = NULL;
            }
        }
    }

    uint32_t sync_us = timer_hw->timerawl - sync_started_us;
    if (sync_us > hdmi_sync_isr_max_us) hdmi_sync_isr_max_us = sync_us;
}

static inline void irq_remove_handler_DMA_core1() {
    irq_set_enabled(VIDEO_DMA_IRQ, false);
    irq_remove_handler(VIDEO_DMA_IRQ, irq_get_exclusive_handler(VIDEO_DMA_IRQ));
}

static inline void irq_set_exclusive_handler_DMA_core1() {
    irq_set_exclusive_handler(VIDEO_DMA_IRQ, dma_handler_HDMI);
    irq_set_priority(VIDEO_DMA_IRQ, 0);
    irq_set_enabled(VIDEO_DMA_IRQ, true);
}

void graphics_set_palette_hdmi2(
    const uint8_t R1, const uint8_t G1, const uint8_t B1,
    const uint8_t R2, const uint8_t G2, const uint8_t B2,
    uint8_t i
);

//деинициализация - инициализация ресурсов
static inline bool hdmi_init() {
    //выключение прерывания DMA
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_set_irq0_enabled(dma_chan_ctrl, false);
    }
    else {
        dma_channel_set_irq1_enabled(dma_chan_ctrl, false);
    }

    irq_remove_handler_DMA_core1();

    if (hdmi_render_irq < 0) {
        hdmi_render_irq = user_irq_claim_unused(true);
        irq_set_exclusive_handler((uint)hdmi_render_irq, hdmi_render_worker);
        // Above ordinary audio/USB IRQs, below the priority-0 HDMI sync IRQ.
        irq_set_priority((uint)hdmi_render_irq, 0x40);
    } else {
        irq_set_enabled((uint)hdmi_render_irq, false);
    }
    hdmi_render_head = hdmi_render_tail = 0;


    //остановка всех каналов DMA
    dma_hw->abort = (1 << dma_chan_ctrl) | (1 << dma_chan) | (1 << dma_chan_pal_conv) | (
                        1 << dma_chan_pal_conv_ctrl);
    while (dma_hw->abort) tight_loop_contents();

    //выключение SM основной и конвертора

#if BOARD_Z2
    pio_set_gpio_base(PIO_VIDEO, 16);
    pio_set_gpio_base(PIO_VIDEO_ADDR, 16);
#endif

    // pio_sm_restart(PIO_VIDEO, SM_video);
    pio_sm_set_enabled(PIO_VIDEO, SM_video, false);

    //pio_sm_restart(PIO_VIDEO_ADDR, SM_conv);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, false);


    //удаление программ из соответствующих PIO
    pio_remove_program(PIO_VIDEO_ADDR, &pio_program_conv_addr_HDMI, offs_prg1);
    pio_remove_program(PIO_VIDEO, &program_PIO_HDMI, offs_prg0);


    offs_prg1 = pio_add_program(PIO_VIDEO_ADDR, &pio_program_conv_addr_HDMI);
    offs_prg0 = pio_add_program(PIO_VIDEO, &program_PIO_HDMI);
    pio_set_x(PIO_VIDEO_ADDR, SM_conv, ((uint32_t)conv_color >> 12));

    // Заполнение палитры — CGA 16 цветов (индексы 0-15)
    // Формат cga_colors: 6-бит RRGGBB (как в VGA DAC)
    static uint8_t cga_colors[16][3] = {
        { 0,  0,  0},  //  0: Black
        { 0,  0, 42},  //  1: Blue        (0x02 -> b=2/3*63)
        { 0, 42,  0},  //  2: Green
        { 0, 42, 42},  //  3: Cyan
        {42,  0,  0},  //  4: Red
        {42,  0, 42},  //  5: Magenta
        {42, 21,  0},  //  6: Brown
        {42, 42, 42},  //  7: Light Gray
        {21, 21, 21},  //  8: Dark Gray
        {21, 21, 63},  //  9: Light Blue
        {21, 63, 21},  // 10: Light Green
        {21, 63, 63},  // 11: Light Cyan
        {63, 21, 21},  // 12: Light Red
        {63, 21, 63},  // 13: Light Magenta
        {63, 63, 21},  // 14: Yellow
        {63, 63, 63},  // 15: White
    };
    
    // заполнение палитры (text) 4 старших bit первый пиксел, 4 младших - второй
    for (int c1 = 0; c1 < 16; ++c1) {
        const uint8_t* c13 = cga_colors[c1];
        for (int c2 = 0; c2 < 16; ++c2) {
            const uint8_t* c23 = cga_colors[c2];
            int ci = c1 << 4 | c2; // compund index
            graphics_set_palette_hdmi2(
                c13[0] << 2, c13[1] << 2, c13[2] << 2,
                c23[0] << 2, c23[1] << 2, c23[2] << 2,
                ci
            );
        }
    }

    //BASE_HDMI_CTRL_INX +3 служебные данные(синхра) напрямую вносим в массив -конвертер
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint16_t b0 = 0b1101010100;
    const uint16_t b1 = 0b0010101011;
    const uint16_t b2 = 0b0101010100;
    const uint16_t b3 = 0b1010101011;

    conv_color64[2 * HDMI_CTRL_0 + 0] = get_ser_diff_data(b0, b0, b3);
    conv_color64[2 * HDMI_CTRL_0 + 1] = get_ser_diff_data(b0, b0, b3);

    conv_color64[2 * HDMI_CTRL_1 + 0] = get_ser_diff_data(b0, b0, b2);
    conv_color64[2 * HDMI_CTRL_1 + 1] = get_ser_diff_data(b0, b0, b2);

    conv_color64[2 * HDMI_CTRL_2 + 0] = get_ser_diff_data(b0, b0, b1);
    conv_color64[2 * HDMI_CTRL_2 + 1] = get_ser_diff_data(b0, b0, b1);

    conv_color64[2 * HDMI_CTRL_3 + 0] = get_ser_diff_data(b0, b0, b0);
    conv_color64[2 * HDMI_CTRL_3 + 1] = get_ser_diff_data(b0, b0, b0);

    memcpy(conv_color2, conv_color, 1024 * 4);

    //настройка PIO SM для конвертации
    pio_sm_config c_c = pio_get_default_sm_config();
    sm_config_set_wrap(&c_c, offs_prg1, offs_prg1 + (pio_program_conv_addr_HDMI.length - 1));
    sm_config_set_in_shift(&c_c, true, false, 32);

    pio_sm_init(PIO_VIDEO_ADDR, SM_conv, offs_prg1, &c_c);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, true);

    //настройка PIO SM для вывода данных
    c_c = pio_get_default_sm_config();
    sm_config_set_wrap(&c_c, offs_prg0, offs_prg0 + (program_PIO_HDMI.length - 1));

    //настройка side set
    sm_config_set_sideset_pins(&c_c,beginHDMI_PIN_clk);
    sm_config_set_sideset(&c_c, 2,false,false);
    for (int i = 0; i < 2; i++) {
        pio_gpio_init(PIO_VIDEO, beginHDMI_PIN_clk + i);
        gpio_set_drive_strength(beginHDMI_PIN_clk + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_clk + i, GPIO_SLEW_RATE_FAST);
    }

#if BOARD_Z2
    // Настройка направлений пинов для state machines
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_video, HDMI_BASE_PIN, 8, true);
    pio_sm_set_consecutive_pindirs(PIO_VIDEO_ADDR, SM_conv, HDMI_BASE_PIN, 8, true);

    uint64_t mask64 = (uint64_t)(3ull << beginHDMI_PIN_clk);
    pio_sm_set_pins_with_mask64(PIO_VIDEO, SM_video, mask64, mask64);
    pio_sm_set_pindirs_with_mask64(PIO_VIDEO, SM_video, mask64, mask64);
    // пины
#else
    pio_sm_set_pins_with_mask(PIO_VIDEO, SM_video, 3u << beginHDMI_PIN_clk, 3u << beginHDMI_PIN_clk);
    pio_sm_set_pindirs_with_mask(PIO_VIDEO, SM_video, 3u << beginHDMI_PIN_clk, 3u << beginHDMI_PIN_clk);
    // пины
#endif

    for (int i = 0; i < 6; i++) {
        gpio_set_slew_rate(beginHDMI_PIN_data + i, GPIO_SLEW_RATE_FAST);
        pio_gpio_init(PIO_VIDEO, beginHDMI_PIN_data + i);
        gpio_set_drive_strength(beginHDMI_PIN_data + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_data + i, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_video, beginHDMI_PIN_data, 6, true);
    //конфигурация пинов на выход
    sm_config_set_out_pins(&c_c, beginHDMI_PIN_data, 6);

    //
    sm_config_set_out_shift(&c_c, true, true, 30);
    sm_config_set_fifo_join(&c_c, PIO_FIFO_JOIN_TX);

    sm_config_set_clkdiv(&c_c, clock_get_hz(clk_sys) / 252000000.0f);
    pio_sm_init(PIO_VIDEO, SM_video, offs_prg0, &c_c);
    pio_sm_set_enabled(PIO_VIDEO, SM_video, true);

    //настройки DMA — 4 line buffers (2 in conv_color, 2 separate)
    dma_lines[0] = &conv_color[1024];
    dma_lines[1] = &conv_color[1124];
    dma_lines[2] = hdmi_extra_line_buf[0];
    dma_lines[3] = hdmi_extra_line_buf[1];

    // Start with four complete, valid blanking lines. This avoids sending
    // zero/garbage TMDS indices while the lower-priority renderer warms up.
    for (uint32_t b = 0; b < 4u; ++b) {
        uint8_t *line = (uint8_t *)dma_lines[b];
        nf_memset(line, HDMI_CTRL_1, 48);
        nf_memset(line + 48, HDMI_CTRL_0, 352);
    }

    //основной рабочий канал
    dma_channel_config cfg_dma = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&cfg_dma, dma_chan_ctrl);
    channel_config_set_high_priority(&cfg_dma, true);

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);


    uint dreq = DREQ_PIO1_TX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_TX0 + SM_conv;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan,
        &cfg_dma,
        &PIO_VIDEO_ADDR->txf[SM_conv], // Write address
        &dma_lines[0][0], // read address
        400, //
        false // Don't start yet
    );

    //контрольный канал для основного
    cfg_dma = dma_channel_get_default_config(dma_chan_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan);
    channel_config_set_high_priority(&cfg_dma, true);

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    DMA_BUF_ADDR[0] = &dma_lines[0][0];
    DMA_BUF_ADDR[1] = &dma_lines[1][0];
    DMA_BUF_ADDR[2] = &dma_lines[2][0];
    DMA_BUF_ADDR[3] = &dma_lines[3][0];

    dma_channel_configure(
        dma_chan_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan].read_addr, // Write address
        &DMA_BUF_ADDR[0], // read address
        1, //
        false // Don't start yet
    );

    //канал - конвертер палитры

    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv_ctrl);
    channel_config_set_high_priority(&cfg_dma, true);

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_TX0 + SM_video;
    if (PIO_VIDEO == pio0) dreq = DREQ_PIO0_TX0 + SM_video;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv,
        &cfg_dma,
        &PIO_VIDEO->txf[SM_video], // Write address
        &conv_color[0], // read address
        4, //
        false // Don't start yet
    );

    //канал управления конвертером палитры

    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_RX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_RX0 + SM_conv;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan_pal_conv].read_addr, // Write address
        &PIO_VIDEO_ADDR->rxf[SM_conv], // read address
        1, //
        true // start yet
    );

    //стартуем прерывание и канал
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_acknowledge_irq0(dma_chan_ctrl);
        dma_channel_set_irq0_enabled(dma_chan_ctrl, true);
    }
    else {
        dma_channel_acknowledge_irq1(dma_chan_ctrl);
        dma_channel_set_irq1_enabled(dma_chan_ctrl, true);
    }

    irq_set_exclusive_handler_DMA_core1();
    irq_set_enabled((uint)hdmi_render_irq, true);

    dma_start_channel_mask((1u << dma_chan_ctrl));

    return true;
};

// DC balance XOR mask — inverts differential pairs for alternating pixels.
// Flips TMDS bits 0-7 and bit 9 (DC balance flag) but NOT bit 8 (encoding method).
#define HDMI_DC_BAL_XOR 0x0003ffffffffffffllu

void __time_critical_func(graphics_set_palette_hdmi)(const uint8_t R, const uint8_t G, const uint8_t B,  uint8_t i) {
    if is_hdmi_sync(i) return;
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    conv_color64[i * 2] = get_ser_diff_data(tmds_encoder(R), tmds_encoder(G), tmds_encoder(B));
    conv_color64[i * 2 + 1] = conv_color64[i * 2] ^ HDMI_DC_BAL_XOR;
};

void graphics_set_palette_hdmi2(
    const uint8_t R1, const uint8_t G1, const uint8_t B1,
    const uint8_t R2, const uint8_t G2, const uint8_t B2,
    uint8_t i
) {
    if is_hdmi_sync(i) return;
    uint64_t* conv_color64 = (uint64_t*)conv_color;
    uint64_t c1 = get_ser_diff_data(tmds_encoder(R1), tmds_encoder(G1), tmds_encoder(B1));
    uint64_t c2 = get_ser_diff_data(tmds_encoder(R2), tmds_encoder(G2), tmds_encoder(B2));
    conv_color64[i * 2]     = c1;
    conv_color64[i * 2 + 1] = (c1 == c2) ? (c2 ^ HDMI_DC_BAL_XOR) : c2;
}

#define RGB888(r, g, b) ((r<<16) | (g << 8 ) | b )

void graphics_init_hdmi() {
    // PIO и DMA
    SM_video = pio_claim_unused_sm(PIO_VIDEO, true);
    SM_conv = pio_claim_unused_sm(PIO_VIDEO_ADDR, true);
    dma_chan_ctrl = dma_claim_unused_channel(true);
    dma_chan = dma_claim_unused_channel(true);
    dma_chan_pal_conv_ctrl = dma_claim_unused_channel(true);
    dma_chan_pal_conv = dma_claim_unused_channel(true);

    // DMA high priority is set per-channel below (after hdmi_init).
    // Do NOT set bus_ctrl_hw->priority here — it starves CPU PSRAM writes.

    hdmi_init();

    // DMA starts at normal priority to avoid starving SD/PSRAM during init.
    // Call hdmi_set_dma_high_priority() after BIOS loading is complete.
}

void hdmi_set_dma_high_priority(void) {
    hw_set_bits(&dma_hw->ch[dma_chan].ctrl_trig, DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS);
    hw_set_bits(&dma_hw->ch[dma_chan_ctrl].ctrl_trig, DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS);
    hw_set_bits(&dma_hw->ch[dma_chan_pal_conv].ctrl_trig, DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS);
    hw_set_bits(&dma_hw->ch[dma_chan_pal_conv_ctrl].ctrl_trig, DMA_CH0_CTRL_TRIG_HIGH_PRIORITY_BITS);
}
