#include <stdio.h>
#include "../cpu.h"
#include "../bios.h"

#define BIOS_FONT_SEG       0xF000
#define BIOS_FONT8X16_OFF   0xA000
#define BIOS_FONT8X14_OFF   0xB000
#define BIOS_FONT8X8_OFF    0xBE00

static bool no_handler(CPU* cpu) {
    cpu_err_msg(cpu, "VIDEO BIOS - ERROR: no handler defined");
while(1); // remove it
    return true;
}

/*
 * Update hardware text-mode cursor through the VGA CRT Controller.
 *
 * IBM PC compatible VGA/MDA/CGA adapters store the cursor position
 * inside CRTC registers:
 *
 *   index 0Eh = cursor location high byte
 *   index 0Fh = cursor location low byte
 *
 * The cursor position is NOT stored as X/Y coordinates.
 * Instead, hardware uses a linear character-cell index relative
 * to the start of display memory.
 *
 * Example for standard 80x25 text mode:
 *
 *   row 0 col 0  -> position 0
 *   row 0 col 1  -> position 1
 *   row 1 col 0  -> position 80
 *
 * VGA text memory is organized as:
 *
 *   one character cell = 2 bytes
 *     byte 0 = ASCII character
 *     byte 1 = attribute/color
 *
 * BDA fields used:
 *
 *   40:4A = screen columns
 *   40:4C = video page size in bytes
 *   40:63 = CRTC base port
 *
 * Typical CRTC ports:
 *
 *   3D4h = color adapters (CGA/EGA/VGA)
 *   3B4h = monochrome adapters (MDA/Hercules)
 *
 * page_size is measured in BYTES, while cursor position is measured
 * in CHARACTER CELLS, therefore:
 *
 *   page_size / 2
 *
 * is used when converting page offset into character coordinates.
 *
 * Access protocol:
 *
 *   out(crtc, index)
 *   out(crtc+1, value)
 *
 * This matches real VGA hardware programming.
 */
static void bios_10h_set_crtc_cursor(CPU* cpu,
                                     uint8_t page,
                                     uint8_t row,
                                     uint8_t col)
{
    if (page != read86(0x462)) return;
    /*
     * Number of text columns.
     * Usually 80 in mode 03h.
     */
    uint16_t cols = readw86(0x44A);

    /*
     * Video page size in bytes.
     * Standard VGA text page:
     *   80 * 25 * 2 = 4000 bytes
     * BIOS often rounds to 4096 (1000h).
     */
    uint16_t page_size = readw86(0x44C);

    /*
     * CRT controller I/O base port.
     *
     * 3D4h = color text modes
     * 3B4h = monochrome
     */
    uint16_t crtc = readw86(0x463);

    if (cols == 0)
        cols = 80;

    if (page_size == 0)
        page_size = 0x1000;

    if (crtc == 0)
        crtc = 0x3D4;

    /*
     * Convert page-relative X/Y coordinates into
     * absolute character-cell index.
     *
     * page_size is bytes -> divide by 2 because:
     *   one text cell = 2 bytes
     */
    uint16_t pos =
        (uint16_t)(page * (page_size / 2)) +
        (uint16_t)row * cols +
        col;

    /*
     * VGA_CRTC_CURSOR_HI (0Eh)
     */
    cpu_portout8(crtc, 0x0E);
    cpu_portout8(crtc + 1, pos >> 8);

    /*
     * VGA_CRTC_CURSOR_LO (0Fh)
     */
    cpu_portout8(crtc, 0x0F);
    cpu_portout8(crtc + 1, pos & 0xFF);
}

/*
VIDEO - SET VIDEO MODE
AH = 00h
AL = desired video mode (see #00010)

Return:
AL = video mode flag (Phoenix, AMI BIOS)
20h mode > 7
30h modes 0-5 and 7
3Fh mode 6
AL = CRT controller mode byte (Phoenix 386 BIOS v1.10)

Desc: Specify the display mode for the currently active display adapter

InstallCheck:
For Ahead adapters, the signature "AHEAD" at C000h:0025h.
For Paradise adapters, the signature "VGA=" at C000h:007Dh.
For Oak Tech OTI-037/057/067/077 chipsets, the signature "OAK VGA" at
C000h:0008h.
For ATI adapters, the signature "761295520" at C000h:0031h; the byte
at C000h:0043h indicates the chipset revision:
31h for 18800
32h for 18800-1
33h for 18800-2
34h for 18800-4
35h for 18800-5
62h for 68800AX (Mach32) (see also #00732)
the two bytes at C000h:0040h indicate the adapter type
"22" EGA Wonder
"31" VGA Wonder
"32" EGA Wonder800+
the byte at C000h:0042h contains feature flags

bit 1:
Mouse port present

bit 4:
Programmable video clock
the byte at C000h:0044h contains additional feature flags if chipset
byte > 30h (see #00009).
For Genoa video adapters, the signature 77h XXh 99h 66h at C000h:NNNNh,
where NNNNh is stored at C000h:0037h and XXh is
00h for Genoa 6200/6300
11h for Genoa 6400/6600
22h for Genoa 6100
33h for Genoa 5100/5200
55h for Genoa 5300/5400
for SuperEGA BIOS v2.41+, C000h:0057h contains the product level
for Genoa SuperEGA BIOS v3.0+, C000h:0070h contains the signature
"EXTMODE", indicating support for extended modes
*/
static void vga_write_regs_80x25_color(CPU* cpu)
{
    static const uint8_t seq[5] = {
        0x03, 0x00, 0x03, 0x00, 0x02
    };

    static const uint8_t crtc[25] = {
        0x5F, 0x4F, 0x50, 0x82, 0x55,
        0x81, 0xBF, 0x1F, 0x00, 0x4F,
        0x0D, 0x0E, 0x00, 0x00, 0x00,
        0x50, 0x9C, 0x0E, 0x8F, 0x28,
        0x1F, 0x96, 0xB9, 0xA3, 0xFF
    };

    static const uint8_t gc[9] = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x10, 0x0E, 0x00, 0xFF
    };

    static const uint8_t ac[21] = {
        0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3A, 0x3B,
        0x3C, 0x3D, 0x3E, 0x3F,
        0x0C, 0x00, 0x0F, 0x08,
        0x00
    };

    cpu_portout8(0x3C2, 0x67);   /* Misc Output: color, 80-col timing */

    for (uint8_t i = 0; i < 5; i++) {
        cpu_portout8(0x3C4, i);
        cpu_portout8(0x3C5, seq[i]);
    }

    /* unlock CRTC regs 00h..07h */
    cpu_portout8(0x3D4, 0x11);
    cpu_portout8(0x3D5, crtc[0x11] & ~0x80);

    for (uint8_t i = 0; i < 25; i++) {
        cpu_portout8(0x3D4, i);
        cpu_portout8(0x3D5, crtc[i]);
    }

    for (uint8_t i = 0; i < 9; i++) {
        cpu_portout8(0x3CE, i);
        cpu_portout8(0x3CF, gc[i]);
    }

    /*
     * Attribute Controller needs flip-flop reset by reading 3DAh.
     */
    (void)cpu_portin8(0x3DA);

    for (uint8_t i = 0; i < 21; i++) {
        cpu_portout8(0x3C0, i);
        cpu_portout8(0x3C0, ac[i]);
    }

    /* enable video output */
    cpu_portout8(0x3C0, 0x20);
}

typedef struct {
    uint8_t misc;
    uint8_t seq[5];
    uint8_t crtc[25];
    uint8_t gc[9];
    uint8_t ac[21];
} VgaRegs;

typedef struct {
    uint8_t mode;
    uint8_t text;
    uint16_t cols;
    uint8_t rows_minus_1;
    uint8_t char_height;
    uint16_t page_size;
    uint16_t crtc_base;
    uint32_t clear_base;
    uint32_t clear_size;
    const VgaRegs *regs;
} VgaMode;

static const VgaRegs vga_80x25_text = {
    0x67,
    {0x03,0x00,0x03,0x00,0x02},
    {0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,
     0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x00,
     0x9C,0x0E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF},
    {0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF},
    {0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
     0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
     0x0C,0x00,0x0F,0x08,0x00}
};

static const VgaRegs vga_40x25_text = {
    0x67,
    {0x03,0x08,0x03,0x00,0x02},
    {0x2D,0x27,0x28,0x90,0x2B,0x80,0xBF,0x1F,
     0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x00,
     0x9C,0x0E,0x8F,0x14,0x1F,0x96,0xB9,0xA3,0xFF},
    {0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF},
    {0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
     0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
     0x0C,0x00,0x0F,0x08,0x00}
};

static const VgaRegs vga_320x200x4 = {
    0x63,
    {0x03,0x09,0x03,0x00,0x02},
    {0x2D,0x27,0x28,0x90,0x2B,0x80,0xBF,0x1F,
     0x00,0xC1,0x00,0x00,0x00,0x00,0x00,0x00,
     0x9C,0x0E,0x8F,0x14,0x00,0x96,0xB9,0xA3,0xFF},
    {0x00,0x00,0x00,0x00,0x00,0x30,0x0F,0x00,0xFF},
    {0x00,0x13,0x15,0x17,0x02,0x04,0x06,0x07,
     0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
     0x01,0x00,0x03,0x00,0x00}
};

static const VgaRegs vga_640x200x2 = {
    0x63,
    {0x03,0x01,0x01,0x00,0x06},
    {0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,
     0x00,0xC1,0x00,0x00,0x00,0x00,0x00,0x00,
     0x9C,0x0E,0x8F,0x28,0x00,0x96,0xB9,0xA3,0xFF},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x0D,0x00,0xFF},
    {0x00,0x17,0x17,0x17,0x17,0x17,0x17,0x17,
     0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,
     0x01,0x00,0x01,0x00,0x00}
};

static const VgaRegs vga_320x200x16 = {
    0x63,
    {0x03,0x01,0x0F,0x00,0x06},
    {0x2D,0x27,0x28,0x90,0x2B,0x80,0xBF,0x1F,
     0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
     0x9C,0x0E,0x8F,0x14,0x00,0x96,0xB9,0xA3,0xFF},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF},
    {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
     0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
     0x01,0x00,0x0F,0x00,0x00}
};

static const VgaRegs vga_640x200x16 = {
    0x63,
    {0x03,0x01,0x0F,0x00,0x06},
    {0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,
     0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
     0x9C,0x0E,0x8F,0x28,0x00,0x96,0xB9,0xA3,0xFF},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF},
    {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
     0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
     0x01,0x00,0x0F,0x00,0x00}
};

static const VgaRegs vga_640x350x16 = {
    0xA3,
    {0x03,0x01,0x0F,0x00,0x06},
    {0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,
     0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
     0x83,0x85,0x5D,0x28,0x0F,0x63,0xBA,0xE3,0xFF},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF},
    {0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
     0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
     0x01,0x00,0x0F,0x00,0x00}
};

static const VgaRegs vga_640x480x16 = {
    0xE3,
    {0x03,0x01,0x0F,0x00,0x06},
    {0x5F,0x4F,0x50,0x82,0x54,0x80,0x0B,0x3E,
     0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
     0xEA,0x8C,0xDF,0x28,0x00,0xE7,0x04,0xE3,0xFF},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF},
    {0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
     0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
     0x01,0x00,0x0F,0x00,0x00}
};

static const VgaRegs vga_320x200x256 = {
    0x63,
    {0x03,0x01,0x0F,0x00,0x0E},
    {0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,
     0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
     0x9C,0x0E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF},
    {0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF},
    {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
     0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
     0x41,0x00,0x0F,0x00,0x00}
};

static const VgaMode vga_modes[] = {
    {0x00,1,40,24,16,0x0800,0x3D4,0xB8000,0x8000,&vga_40x25_text},
    {0x01,1,40,24,16,0x0800,0x3D4,0xB8000,0x8000,&vga_40x25_text},
    {0x02,1,80,24,16,0x1000,0x3D4,0xB8000,0x8000,&vga_80x25_text},
    {0x03,1,80,24,16,0x1000,0x3D4,0xB8000,0x8000,&vga_80x25_text},

    {0x04,0,40,24,8, 0x4000,0x3D4,0xB8000,0x4000,&vga_320x200x4},
    {0x05,0,40,24,8, 0x4000,0x3D4,0xB8000,0x4000,&vga_320x200x4},
    {0x06,0,80,24,8, 0x4000,0x3D4,0xB8000,0x4000,&vga_640x200x2},
    {0x07,1,80,24,16,0x1000,0x3B4,0xB0000,0x8000,&vga_80x25_text},

    {0x0D,0,40,24,8, 0x2000,0x3D4,0xA0000,0x20000,&vga_320x200x16},
    {0x0E,0,80,24,8, 0x4000,0x3D4,0xA0000,0x20000,&vga_640x200x16},
    {0x0F,0,80,24,14,0x8000,0x3B4,0xA0000,0x20000,&vga_640x350x16},
    {0x10,0,80,24,14,0x8000,0x3D4,0xA0000,0x20000,&vga_640x350x16},
    {0x11,0,80,29,16,0x0000,0x3D4,0xA0000,0x10000,&vga_640x480x16},
    {0x12,0,80,29,16,0x0000,0x3D4,0xA0000,0x20000,&vga_640x480x16},
    {0x13,0,40,24,8, 0x1000,0x3D4,0xA0000,0x10000,&vga_320x200x256},
};

static const VgaMode *vga_find_mode(uint8_t mode)
{
    for (uint8_t i = 0; i < sizeof(vga_modes) / sizeof(vga_modes[0]); i++) {
        if (vga_modes[i].mode == mode)
            return &vga_modes[i];
    }
    return NULL;
}

static void vga_program_regs(CPU* cpu, const VgaRegs *r, uint16_t crtc_base)
{
    cpu_portout8(0x3C2, r->misc);

    for (uint8_t i = 0; i < 5; i++) {
        cpu_portout8(0x3C4, i);
        cpu_portout8(0x3C5, r->seq[i]);
    }

    cpu_portout8(crtc_base, 0x11);
    cpu_portout8(crtc_base + 1, r->crtc[0x11] & 0x7F);

    for (uint8_t i = 0; i < 25; i++) {
        cpu_portout8(crtc_base, i);
        cpu_portout8(crtc_base + 1, r->crtc[i]);
    }

    for (uint8_t i = 0; i < 9; i++) {
        cpu_portout8(0x3CE, i);
        cpu_portout8(0x3CF, r->gc[i]);
    }

    (void)cpu_portin8(crtc_base + 6);

    for (uint8_t i = 0; i < 21; i++) {
        cpu_portout8(0x3C0, i);
        cpu_portout8(0x3C0, r->ac[i]);
    }

    cpu_portout8(0x3C0, 0x20);
}

/*
VIDEO - SET VIDEO MODE
AH = 00h
AL = desired video mode (see #00010)

Return:
AL = video mode flag (Phoenix, AMI BIOS)
20h mode > 7
30h modes 0-5 and 7
3Fh mode 6
AL = CRT controller mode byte (Phoenix 386 BIOS v1.10)

Desc: Specify the display mode for the currently active display adapter
*/
static bool bios_10h_00h(CPU* cpu)
{
    uint8_t raw_mode = CPU_AL;
    uint8_t mode = raw_mode & 0x7F;
    bool no_clear = (raw_mode & 0x80) != 0;

    const VgaMode *m = vga_find_mode(mode);
    if (!m) {
        cf = 1;
        return true;
    }

    vga_program_regs(cpu, m->regs, m->crtc_base);

    write86 (0x449, mode);
    writew86(0x44A, m->cols);
    writew86(0x44C, m->page_size);
    writew86(0x44E, 0x0000);
    write86 (0x462, 0x00);
    writew86(0x463, m->crtc_base);
    writew86(0x466, 0x0000);
    write86 (0x484, m->rows_minus_1);
    writew86(0x485, m->char_height);

    for (uint8_t page = 0; page < 8; page++)
        writew86(0x450 + page * 2, 0x0000);

    if (m->char_height <= 8)
        writew86(0x460, 0x0607);
    else if (m->char_height <= 14)
        writew86(0x460, 0x0B0C);
     else
        writew86(0x460, 0x0E0F);

    /* SeaBIOS vga_set_mode: update video_ctl, video_switches, modeset_ctl */
    write86(0x465, no_clear ? 0xE0 : 0x60);             /* video_ctl: bit7=no_clear (SeaBIOS) */
    write86(0x488, 0xF9);                               /* video_switches */
    write86(0x489, read86(0x489) & ~0x80);              /* modeset_ctl: clear bit 7 */

    if (!no_clear) {
        if (m->text) {
            for (uint32_t off = 0; off < m->clear_size; off += 2) {
                write86(m->clear_base + off + 0, ' ');
                write86(m->clear_base + off + 1, 0x07);
            }
        } else {
            for (uint32_t off = 0; off < m->clear_size; off++)
                write86(m->clear_base + off, 0x00);
        }
    }

    bios_10h_set_crtc_cursor(cpu, 0, 0, 0);

    CPU_AL = (mode == 0x06) ? 0x3F :
             (mode <= 0x07) ? 0x30 :
                               0x20;

    cf = 0;
    return true;
}

/*
VIDEO - SET TEXT-MODE CURSOR SHAPE
AH = 01h
CH = cursor start scan line (bits 0-4) + options (bits 5-6)
     bit 5: 0 = normal, 1 = cursor invisible (CGA/EGA/VGA)
     bit 6: reserved
CL = cursor end scan line (bits 0-4)

BDA: 0x460 = cursor shape word (CH<<8 | CL), as stored and returned by AH=03h
CRTC: reg 0Ah = start scan / cursor disable, reg 0Bh = end scan
*/
static bool bios_10h_01h(CPU* cpu)
{
    uint8_t ch_raw = CPU_CH;
    uint8_t cl_raw = CPU_CL & 0x1F;
    bool    hidden = (ch_raw & 0x20) != 0; /* bit 5 = cursor disable */

    /* Save raw CH:CL in BDA 0x460 (SeaBIOS: SET_BDA(cursor_type, CX) — raw) */
    writew86(0x460, ((uint16_t)ch_raw << 8) | CPU_CL);

    /* Program CRTC */
    uint16_t crtc = readw86(0x463);
    if (crtc == 0) crtc = 0x3D4;

    /* CRTC reg 0Ah: bit5=CD (cursor disable), bits4:0=start scan line */
    uint8_t reg0a = (ch_raw & 0x1F);
    if (hidden) reg0a |= 0x20;
    cpu_portout8(crtc,     0x0A);
    cpu_portout8(crtc + 1, reg0a);

    /* CRTC reg 0Bh: bits4:0=end scan line */
    cpu_portout8(crtc,     0x0B);
    cpu_portout8(crtc + 1, cl_raw);

    return true;
}

/*
VIDEO - SET CURSOR POSITION
AH = 02h
BH = page number
0-3 in modes 2&3
0-7 in modes 0&1
0 in graphics modes
DH = row (00h is top)
DL = column (00h is left)

Return:
Nothing
*/
static bool bios_10h_02h(CPU* cpu) {
    uint8_t page = CPU_BH;
    if (page > 7) return true;
    uint8_t row = CPU_DH;
    uint8_t col = CPU_DL;
    uint16_t cur;
    /*
     * BDA:
     * 40:50..5F = cursor positions for pages
     * high byte = row
     * low byte  = column
     */
    cur = ((uint16_t)row << 8) | col;
    writew86(0x450 + ((uint16_t)page * 2), cur);
    bios_10h_set_crtc_cursor(cpu, page, row, col);
    return true;
}

/*
VIDEO - GET CURSOR POSITION AND SIZE
AH = 03h
BH = page number
0-3 in modes 2&3
0-7 in modes 0&1
0 in graphics modes

Return:
AX = 0000h (Phoenix BIOS)
CH = start scan line
CL = end scan line
DH = row (00h is top)
DL = column (00h is left)

Notes: A separate cursor is maintained for each of up to 8 display pages. Many ROM BIOSes incorrectly return the default size for a
color display (start 06h, end 07h) when a monochrome display is attached. With PhysTechSoft's PTS ROM-DOS the BH value is ignored on entry.
*/
static bool bios_10h_03h(CPU* cpu) {
    uint8_t page = CPU_BH;
    if (page > 7) { CPU_AX = 0; CPU_CX = 0; CPU_DX = 0; return true; }
    uint16_t shape = readw86(0x460);
    uint16_t cur   = readw86(0x450 + ((uint16_t)page * 2));
    CPU_AX = 0;
    CPU_CH = (uint8_t)(shape >> 8);   /* cursor start scan line */
    CPU_CL = (uint8_t)(shape & 0xFF); /* cursor end scan line   */
    CPU_DH = (uint8_t)(cur >> 8);     /* row */
    CPU_DL = (uint8_t)(cur & 0xFF);   /* column */
    return true;
}

/*
VIDEO - WRITE CHARACTER AND ATTRIBUTE AT CURSOR POSITION
AH = 09h
AL = character
BH = page
BL = attribute / color
CX = repeat count

Text modes:
  writes AL+BL at cursor position CX times.
  Cursor is NOT advanced.

Graphics modes:
  writes character using font bitmap, BL = color.
  Minimal BIOS emulation here supports text modes only.
*/
static bool bios_10h_09h(CPU* cpu)
{
    uint8_t ch   = CPU_AL;
    uint8_t page = CPU_BH;
    uint8_t attr = CPU_BL;
    uint16_t cnt = CPU_CX;

    if (cnt == 0)
        return true;

    uint8_t mode = read86(0x449);
    uint16_t cols = readw86(0x44A);
    uint16_t page_size = readw86(0x44C);
    uint8_t rows_minus_1 = read86(0x484);

    if (cols == 0) cols = 80;
    if (page_size == 0) page_size = 0x1000;
    if (rows_minus_1 == 0) rows_minus_1 = 24;

    /*
     * AH=09h in text modes writes at cursor but does not move cursor.
     * For now support text modes 00h,01h,02h,03h,07h.
     */
    if (!((mode <= 0x03) || mode == 0x07)) {
        cf = 1;
        return true;
    }

    uint32_t vram_base = (mode == 0x07) ? 0xB0000u : 0xB8000u;
    uint16_t page_off = (uint16_t)page * page_size;

    uint16_t cur = readw86(0x450 + ((uint16_t)page * 2));
    uint8_t row = (uint8_t)(cur >> 8);
    uint8_t col = (uint8_t)(cur & 0xFF);

    uint8_t rows = rows_minus_1 + 1;
    uint32_t pos = (uint32_t)row * cols + col;
    uint32_t max_cells = (uint32_t)rows * cols;

    while (cnt-- && pos < max_cells) {
        uint32_t cell = vram_base + page_off + pos * 2u;
        write86(cell + 0, ch);
        write86(cell + 1, attr);
        pos++;
    }

    cf = 0;
    return true;
}

/*
AH = 0Eh
AL = character to write
BH = page number
BL = foreground color (graphics modes only)

Return:
Nothing

Desc: Display a character on the screen, advancing the cursor and scrolling the screen as necessary

Notes: Characters 07h (BEL), 08h (BS), 0Ah (LF), and 0Dh (CR) are interpreted and do the expected things.
 IBM PC ROMs dated 1981/4/24 and 1981/10/19 require that BH be the same as the current active page
*/
static bool bios_10h_0Eh(CPU* cpu) {
    uint8_t ch = CPU_AL;
    uint8_t mode = read86(0x449);
    uint16_t cols = readw86(0x44A);
    uint16_t page_size = readw86(0x44C);
    uint8_t active_page = read86(0x462);
    uint8_t page = CPU_BH;
    uint8_t rows_minus_1 = read86(0x484);
    if (cols == 0)
        cols = 80;
    if (rows_minus_1 == 0)
        rows_minus_1 = 24;
    if (page_size == 0)
        page_size = 0x1000;

    /*
     * IBM PC ROM 1981 требовал BH == active page.
     * Для DOS boot/debug output безопаснее игнорировать вывод
     * в неактивную страницу, но не падать.
     */
    if (page != active_page)
        return true;
    /*
     * Только текстовые режимы.
     * mode 7 = MDA, остальное типично B800.
     */
    uint32_t vram_base = (mode == 7) ? 0xB0000u : 0xB8000u;
    uint16_t page_off = (uint16_t)page * page_size;

    uint16_t cur = readw86(0x450 + ((uint16_t)page * 2));
    uint8_t row = (uint8_t)(cur >> 8);
    uint8_t col = (uint8_t)(cur & 0xFF);

    uint8_t rows = rows_minus_1 + 1;
    /*
     * AH=0Eh — TTY output. IBM BIOS всегда использует атрибут 0x07
     * (светло-серый на чёрном) для скролла и вывода символов.
     * Чтение атрибута из vram ненадёжно: при LF/CR курсор уже
     * сдвинут и ячейка может содержать мусор от предыдущего контента.
     */
    uint8_t attr = 0x07;

    switch (ch) {
        case 0x07: /* BEL */
        // TODO: Pc speaker beep should be there
            return true;

        case 0x08: /* BS */
            if (col > 0) {
                --col;
            }
            break;

        case 0x0D: /* CR */
            col = 0;
            break;

        case 0x0A: /* LF */
            row++;
            break;

        default: {
            uint32_t cell = vram_base + page_off + ((uint32_t)row * cols + col) * 2u;
            write86(cell + 0, ch);
            write86(cell + 1, attr);

            col++;
            if (col >= cols) {
                col = 0;
                row++;
            }
            break;
        }
    }

    if (row >= rows) {
        uint32_t page_base = vram_base + page_off;
        uint16_t line_bytes = cols * 2;

        for (uint16_t r = 1; r < rows; r++) {
            for (uint16_t x = 0; x < line_bytes; x++)
                write86(page_base + (uint32_t)(r - 1) * line_bytes + x,
                        read86(page_base + (uint32_t)r * line_bytes + x));
        }

        for (uint16_t c = 0; c < cols; c++) {
            uint32_t cell = page_base + (uint32_t)(rows - 1) * line_bytes + c * 2u;
            write86(cell + 0, ' ');
            write86(cell + 1, attr);
        }

        row = rows - 1;
    }

    cur = ((uint16_t)row << 8) | col;
    writew86(0x450 + ((uint16_t)page * 2), cur);
    bios_10h_set_crtc_cursor(cpu, page, row, col);
    return true;
}

/*
VIDEO - GET FONT INFORMATION (EGA, MCGA, VGA)
AX = 1130h
BH = pointer specifier
00h INT 1Fh pointer
01h INT 43h pointer
02h ROM 8x14 character font pointer
03h ROM 8x8 double dot font pointer
04h ROM 8x8 double dot font (high 128 characters)
05h ROM alpha alternate (9 by 14) pointer (EGA,VGA)
06h ROM 8x16 font (MCGA, VGA)
07h ROM alternate 9x16 font (VGA only) (see #00021)

Return:
ES:BP = specified pointer
CX    = bytes/character of on-screen font (not the requested font!)
DL    = highest character row on screen
*/
static bool bios_10h_1130h(CPU* cpu) {
    uint8_t spec = CPU_BH;
    uint16_t off;

    /*
     * CX = bytes/character of current on-screen font,
     * not necessarily of requested BH font.
     *
     * In your BDA init 40:85 = 16, so normal mode 03h reports 16.
     */
    CPU_CX = readw86(0x485);
    if (CPU_CX == 0)
        CPU_CX = 16;

    /*
     * DL = highest character row on screen.
     * In normal 80x25 text mode this is 24.
     */
    CPU_DL = read86(0x484);
    if (CPU_DL == 0)
        CPU_DL = 24;

    switch (spec) {
        case 0x02: /* ROM 8x14 */
        case 0x05: /* alternate 9x14: return same 8x14 raster */
            off = BIOS_FONT8X14_OFF;
            break;

        case 0x03: /* ROM 8x8 */
            off = BIOS_FONT8X8_OFF;
            break;

        case 0x04: /* ROM 8x8 high 128 characters */
            off = BIOS_FONT8X8_OFF + 128u * 8u;
            break;

        case 0x00: /* INT 1Fh pointer */
        case 0x01: /* INT 43h/current font pointer */
        case 0x06: /* ROM 8x16 */
        case 0x07: /* alternate 9x16: return same 8x16 raster */
        default:
            off = BIOS_FONT8X16_OFF;
            break;
    }

    SET_ES(BIOS_FONT_SEG);
    CPU_BP = off;
    cf = 0;
    return true;
}

bool bios_10h(CPU* cpu) {
    switch(CPU_AH) {
        case 0x00:
            return bios_10h_00h(cpu); // SET VIDEO MODE
        case 0x01:
            return bios_10h_01h(cpu); // SET CURSOR SHAPE
        case 0x02:
            return bios_10h_02h(cpu); // SET CURSOR POSITION
        case 0x03:
            return bios_10h_03h(cpu); // GET CURSOR POSITION AND SIZE
        case 0x09:
            return bios_10h_09h(cpu); // WRITE CHARACTER AND ATTRIBUTE
        case 0x0E:
            return bios_10h_0Eh(cpu); // TELETYPE OUTPUT
        default:
            if (CPU_AX == 0x1130)
                return bios_10h_1130h(cpu);
            no_handler(cpu);
        case 0x74: // ? HUNTER 16 - SET LCD WINDOWS POSITION
         // unsupported
    }
    cf = 1; // unsuported unknown function
    return true;
}

#include "font8x16.h"
// TODO: other fonts 8x14, 8x8

void bios_10h_install_rom_fonts(CPU* cpu) // calling from load_bios_and_reset
{
    /*
     * INT 10h/AX=1130h must return a guest-visible ES:BP pointer.
     * Host pointers to font_8x16/vgafont16 are useless for DOS code,
     * so copy compact ROM font tables into emulated F000:xxxx area.
     *
     * Source font is 8x16. 8x14 and 8x8 are derived minimally:
     *   8x16: all 16 rows
     *   8x14: rows 1..14
     *   8x8 : rows 4..11
     */
    for (uint32_t ch = 0; ch < 256; ch++) {
        for (uint32_t y = 0; y < 16; y++) {
            write86(((uint32_t)BIOS_FONT_SEG << 4) +
                    BIOS_FONT8X16_OFF + ch * 16 + y,
                    font_8x16[ch * 16 + y]);
        }

        for (uint32_t y = 0; y < 14; y++) {
            write86(((uint32_t)BIOS_FONT_SEG << 4) +
                    BIOS_FONT8X14_OFF + ch * 14 + y,
                    font_8x16[ch * 16 + y + 1]);
        }

        for (uint32_t y = 0; y < 8; y++) {
            write86(((uint32_t)BIOS_FONT_SEG << 4) +
                    BIOS_FONT8X8_OFF + ch * 8 + y,
                    font_8x16[ch * 16 + y + 4]);
        }
    }
}

void vga_bios_baner(CPU* cpu)
{
    const char *banner = "RP2350 PC AT BIOS";
    const uint8_t attr = 0x61; // bg=yellow(6), fg=blue(1)
    const uint8_t row  = 0;
    const uint8_t cols = 80;
    const uint8_t len  = 17;
    const uint8_t col  = (cols - len) / 2; // 31

    // Set 80x25 color text mode
    CPU_AH = 0x00; CPU_AL = 0x03;
    bios_10h(cpu);

    // Hide cursor while drawing the BIOS banner.
    // Otherwise the first cursor position 0:0 can be rendered over
    // the colored banner before we move the cursor to the final line.
    CPU_AH = 0x01; CPU_CH = 0x20; CPU_CL = 0x00;
    bios_10h(cpu);

    // Fill full row 0 with spaces using banner attribute
    CPU_AH = 0x02; CPU_BH = 0x00; CPU_DH = row; CPU_DL = 0x00;
    bios_10h(cpu);

    CPU_AH = 0x09; CPU_AL = ' '; CPU_BH = 0x00; CPU_BL = attr; CPU_CX = cols;
    bios_10h(cpu);

    // Move cursor to centered banner position
    CPU_AH = 0x02; CPU_BH = 0x00; CPU_DH = row; CPU_DL = col; bios_10h(cpu);

    // Print banner via INT 10h-style BIOS service.
    // AH=09h does not advance cursor, so set position explicitly.
    for (uint8_t i = 0; banner[i]; ++i) {
        CPU_AH = 0x02; CPU_BH = 0x00; CPU_DH = row; CPU_DL = col + i;
        bios_10h(cpu);

        CPU_AH = 0x09; CPU_AL = (uint8_t)banner[i]; CPU_BH = 0x00; CPU_BL = attr; CPU_CX = 1;
        bios_10h(cpu);
    }

    // Move cursor to row 1, col 0
    CPU_AH = 0x02; CPU_BH = 0x00; CPU_DH = 0x01; CPU_DL = 0x00;
    bios_10h(cpu);

    // Restore normal 80x25 cursor shape.
    CPU_AH = 0x01; CPU_CH = 0x0E; CPU_CL = 0x0F;
    bios_10h(cpu);
}
