#include <stdio.h>
#include "../cpu.h"
#include "../bios.h"

#define BIOS_FONT_SEG       0xF000
#define BIOS_FONT8X16_OFF   0xA000
#define BIOS_FONT8X14_OFF   0xB000
#define BIOS_FONT8X8_OFF    0xBE00

/*
 * INT 10h/AX=1A00h Display Combination Code values.
 *
 * 08h = VGA with analog color display.
 *
 * The native BIOS exposes a plain VGA-compatible adapter: standard CGA/EGA/VGA
 * register set, no SVGA/VESA extensions.  Therefore the active and alternate
 * display are both reported as the same VGA color display.  This is enough for
 * DOS software that uses AX=1A00h only to detect VGA/EGA capability.
 */
#define BIOS10_DCC_VGA_COLOR_ANALOG  0x08

 /*
 * INT 10h/AH=12h/BL=10h GET EGA/VGA INFORMATION constants.
 */
#define BIOS10_EGA_INFO_COLOR_IO      0x00  /* active CRTC is 3Dxh */
#define BIOS10_EGA_INFO_MONO_IO       0x01  /* active CRTC is 3Bxh */

/*
 * INT 10h/AH=12h/BL=10h memory-size code returned in BL.
 *
 * 00h =  64 KiB
 * 01h = 128 KiB
 * 02h = 192 KiB
 * 03h = 256 KiB
 *
 * Plain VGA has 256 KiB addressable video RAM.
 */
#define BIOS10_EGA_INFO_MEM_256K      0x03

/*
 * EGA/VGA switch byte fallback for BDA 40:88.
 *
 * 09h is the usual VGA/EGA enhanced color configuration value after mode set:
 * primary EGA/VGA color display, enhanced mode.
 */
#define BIOS10_EGA_SWITCHES_COLOR_ENH 0x09

/*
 * INT 10h/AH=13h write-string mode bits in AL.
 */
#define BIOS10_WRITE_STRING_UPDATE_CURSOR  0x01
#define BIOS10_WRITE_STRING_HAS_ATTRS      0x02

/*
 * VGA Attribute Controller ports/registers.
 *
 * 3DAh read resets the Attribute Controller flip-flop to "index" state.
 * 3C0h is then used first as index port, then as data port.
 * 3C1h reads the selected Attribute Controller register.
 */
#define VGA_INPUT_STATUS_1_COLOR_PORT  0x3DA
#define VGA_ATTR_INDEX_DATA_PORT       0x3C0
#define VGA_ATTR_DATA_READ_PORT        0x3C1

#define VGA_ATTR_ENABLE_DISPLAY        0x20
#define VGA_ATTR_MODE_CONTROL_REG      0x10
#define VGA_ATTR_OVERSCAN_COLOR_REG    0x11

/*
 * VGA DAC / PEL ports.
 *
 * 3C8h = DAC write index.
 * 3C9h = DAC data port: red, green, blue, each 6 bits on standard VGA.
 */
#define VGA_DAC_WRITE_INDEX_PORT       0x3C8
#define VGA_DAC_DATA_PORT              0x3C9
#define VGA_DAC_READ_INDEX_PORT        0x3C7
#define VGA_PEL_MASK_PORT              0x3C6

/*
 * INT 10h/AH=0Bh/BH=01h CGA palette selector bits.
 *
 * BL bit 0 selects the CGA 320x200 4-color palette:
 *   0 = green / red     / brown
 *   1 = cyan  / magenta / white
 *
 * BL bit 1 selects high-intensity variant.
 */
#define BIOS10_CGA_PALETTE_ALT         0x01
#define BIOS10_CGA_PALETTE_INTENSE     0x02

/*
 * VGA Attribute Controller palette indexes corresponding to classic CGA
 * 320x200 4-color palettes.
 */
#define BIOS10_CGA_PAL0_COLOR1         0x02
#define BIOS10_CGA_PAL0_COLOR2         0x04
#define BIOS10_CGA_PAL0_COLOR3         0x06
#define BIOS10_CGA_PAL1_COLOR1         0x03
#define BIOS10_CGA_PAL1_COLOR2         0x05
#define BIOS10_CGA_PAL1_COLOR3         0x07

/*
 * INT 10h/AH=0Ch WRITE GRAPHICS PIXEL constants.
 *
 * AL bit 7 requests XOR drawing for graphics modes except 256-color mode 13h.
 * In mode 13h all 8 bits of AL are the pixel color.
 */
#define BIOS10_PIXEL_XOR               0x80

/*
 * Physical VGA memory bases used by BIOS graphics modes.
 */
#define VGA_MEM_BASE_CGA               0xB8000u
#define VGA_MEM_BASE_MDA               0xB0000u
#define VGA_MEM_BASE_GFX               0xA0000u

/*
 * INT 10h AX=1013h / AX=101Ah state.
 *
 * Plain VGA supports 16 DAC pages when blink is disabled.
 * BIOS keeps the selected page state here.
 */
static uint8_t vga_dac_page_mode;
static uint8_t vga_dac_page;

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
 * Return true for BIOS text modes backed by character/attribute memory.
 *
 * Supported here:
 *   00h/01h  40x25 color text
 *   02h/03h  80x25 color text
 *   07h      80x25 monochrome text
 *
 * Graphics modes need font rendering and pixel addressing, so read/write/
 * scroll helpers below deliberately reject them.
 */
static bool bios_10h_is_text_mode(uint8_t mode)
{
    return mode <= 0x03 || mode == 0x07;
}

/*
 * Return the physical base address of text video RAM for current mode.
 *
 * Color text modes use B800:0000.
 * Monochrome mode 07h uses B000:0000.
 */
static uint32_t bios_10h_text_base(uint8_t mode)
{
    return (mode == 0x07) ? 0xB0000u : 0xB8000u;
}

/*
 * Program VGA CRTC display start address for selected text page.
 *
 * BDA 40:4C stores page size in bytes, while CRTC start address is measured
 * in character cells / words.  Therefore page byte offset is divided by two.
 *
 * CRTC registers:
 *   0Ch = start address high
 *   0Dh = start address low
 */
static void bios_10h_set_display_page(CPU* cpu, uint8_t page)
{
    uint16_t page_size = readw86(0x44C);
    uint16_t crtc = readw86(0x463);

    if (page_size == 0)
        page_size = 0x1000;
    if (crtc == 0)
        crtc = 0x3D4;

    uint16_t start = ((uint16_t)page * page_size) / 2;

    write86(0x462, page);

    cpu_portout8(crtc, 0x0C);
    cpu_portout8(crtc + 1, start >> 8);
    cpu_portout8(crtc, 0x0D);
    cpu_portout8(crtc + 1, start & 0xFF);
}

/*
 * Return physical address of text cell at page,row,col.
 *
 * Each text cell is two bytes:
 *   byte 0 = character
 *   byte 1 = attribute
 */
static uint32_t bios_10h_text_cell(uint8_t mode,
                                   uint8_t page,
                                   uint8_t row,
                                   uint8_t col)
{
    uint16_t cols = readw86(0x44A);
    uint16_t page_size = readw86(0x44C);

    if (cols == 0)
        cols = 80;
    if (page_size == 0)
        page_size = 0x1000;

    return bios_10h_text_base(mode) +
           (uint32_t)page * page_size +
           ((uint32_t)row * cols + col) * 2u;
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
VIDEO - READ LIGHT PEN POSITION

No light pen hardware is present.

Return:
AH = 00h (not triggered)
*/
static bool bios_10h_04h(CPU* cpu)
{
    CPU_AH = 0x00;
    CPU_BX = 0x0000;
    CPU_CX = 0x0000;
    CPU_DX = 0x0000;
    cf = 0;
    return true;
}

/*
VIDEO - SELECT ACTIVE DISPLAY PAGE
AH = 05h
AL = page number

Return:
Nothing

Desc:
Selects which text page is displayed.  Cursor coordinates for every page
are still stored independently in BDA 40:50..5F.
*/
static bool bios_10h_05h(CPU* cpu)
{
    uint8_t page = CPU_AL;
    uint8_t mode = read86(0x449);

    if (page > 7) {
        cf = 1;
        return true;
    }

    /*
     * For graphics modes keep the BIOS call harmless.  Classic BIOSes have
     * mode-specific rules here; for this native BIOS only text pages are
     * useful now.
     */
    if (!bios_10h_is_text_mode(mode) && page != 0) {
        cf = 1;
        return true;
    }

    bios_10h_set_display_page(cpu, page);

    uint16_t cur = readw86(0x450 + (uint16_t)page * 2);
    bios_10h_set_crtc_cursor(cpu, page, cur >> 8, cur & 0xFF);

    cf = 0;
    return true;
}

/*
 * Common implementation for INT 10h/AH=06h and AH=07h.
 *
 * AH=06h scrolls a rectangular text window up.
 * AH=07h scrolls it down.
 *
 * AL = number of lines to scroll.
 *      AL=00h means clear the whole window.
 * BH = attribute used for newly blanked lines.
 * CH,CL = upper-left row/column.
 * DH,DL = lower-right row/column.
 *
 * Only text modes are handled.  Graphics-mode scrolling requires pixel
 * operations and is intentionally not emulated here.
 */
static bool bios_10h_scroll_window(CPU* cpu, bool down)
{
    uint8_t mode = read86(0x449);

    if (!bios_10h_is_text_mode(mode)) {
        cf = 1;
        return true;
    }

    uint16_t cols = readw86(0x44A);
    uint8_t rows_minus_1 = read86(0x484);
    uint8_t page = read86(0x462);
    uint8_t lines = CPU_AL;
    uint8_t attr = CPU_BH;

    if (cols == 0)
        cols = 80;
    if (rows_minus_1 == 0)
        rows_minus_1 = 24;

    uint8_t max_row = rows_minus_1;
    uint8_t max_col = cols - 1;

    uint8_t top = CPU_CH;
    uint8_t left = CPU_CL;
    uint8_t bottom = CPU_DH;
    uint8_t right = CPU_DL;

    if (top > max_row)
        top = max_row;
    if (bottom > max_row)
        bottom = max_row;
    if (left > max_col)
        left = max_col;
    if (right > max_col)
        right = max_col;

    if (top > bottom || left > right) {
        cf = 0;
        return true;
    }

    uint8_t height = bottom - top + 1;

    /*
     * IBM BIOS convention:
     *   AL=0 or AL>=window height clears the entire window.
     */
    if (lines == 0 || lines >= height)
        lines = height;

    if (!down) {
        for (uint8_t r = top; r <= bottom; r++) {
            uint8_t src_r = r + lines;

            for (uint8_t c = left; c <= right; c++) {
                uint32_t dst = bios_10h_text_cell(mode, page, r, c);

                if (src_r <= bottom) {
                    uint32_t src = bios_10h_text_cell(mode, page, src_r, c);
                    write86(dst + 0, read86(src + 0));
                    write86(dst + 1, read86(src + 1));
                } else {
                    write86(dst + 0, ' ');
                    write86(dst + 1, attr);
                }
            }
        }
    } else {
        for (int r = bottom; r >= top; r--) {
            int src_r = r - lines;

            for (uint8_t c = left; c <= right; c++) {
                uint32_t dst = bios_10h_text_cell(mode, page, r, c);

                if (src_r >= top) {
                    uint32_t src = bios_10h_text_cell(mode, page, src_r, c);
                    write86(dst + 0, read86(src + 0));
                    write86(dst + 1, read86(src + 1));
                } else {
                    write86(dst + 0, ' ');
                    write86(dst + 1, attr);
                }
            }
        }
    }

    cf = 0;
    return true;
}

/*
VIDEO - READ CHARACTER AND ATTRIBUTE AT CURSOR POSITION
AH = 08h
BH = page number

Return:
AH = attribute
AL = character

Only text modes are implemented.  Graphics modes would need font/pixel
reverse mapping and are not useful for normal DOS boot text output.
*/
static bool bios_10h_08h(CPU* cpu)
{
    uint8_t mode = read86(0x449);
    uint8_t page = CPU_BH;

    if (page > 7 || !bios_10h_is_text_mode(mode)) {
        cf = 1;
        return true;
    }

    uint16_t cur = readw86(0x450 + (uint16_t)page * 2);
    uint32_t cell = bios_10h_text_cell(mode, page, cur >> 8, cur & 0xFF);

    CPU_AL = read86(cell + 0);
    CPU_AH = read86(cell + 1);
    cf = 0;
    return true;
}

/*
VIDEO - WRITE CHARACTER ONLY AT CURSOR POSITION
AH = 0Ah
AL = character
BH = page
BL = foreground color in graphics modes; ignored in text modes
CX = repeat count

Return:
Nothing

Desc:
Writes only character bytes, preserving existing attributes.
Cursor position is not advanced.

This implementation handles text modes only.  That is enough for normal
DOS text output and matches the rest of the current native BIOS text path.
*/
static bool bios_10h_0Ah(CPU* cpu)
{
    uint8_t mode = read86(0x449);
    uint8_t page = CPU_BH;
    uint16_t cnt = CPU_CX;

    if (cnt == 0) {
        cf = 0;
        return true;
    }

    if (page > 7 || !bios_10h_is_text_mode(mode)) {
        cf = 1;
        return true;
    }

    uint16_t cols = readw86(0x44A);
    uint8_t rows_minus_1 = read86(0x484);

    if (cols == 0)
        cols = 80;
    if (rows_minus_1 == 0)
        rows_minus_1 = 24;

    uint16_t cur = readw86(0x450 + (uint16_t)page * 2);
    uint8_t row = cur >> 8;
    uint8_t col = cur & 0xFF;

    uint32_t pos = (uint32_t)row * cols + col;
    uint32_t max_cells = (uint32_t)(rows_minus_1 + 1) * cols;

    while (cnt-- && pos < max_cells) {
        uint8_t r = pos / cols;
        uint8_t c = pos % cols;
        uint32_t cell = bios_10h_text_cell(mode, page, r, c);

        write86(cell + 0, CPU_AL);
        pos++;
    }

    cf = 0;
    return true;
}

/*
 * Write one pixel in CGA 320x200 4-color modes 04h/05h.
 *
 * Guest-visible CGA layout:
 *   even scanlines: B800:0000
 *   odd  scanlines: B800:2000
 *   80 bytes per scanline
 *   4 pixels per byte, 2 bits per pixel, MSB first
 *
 * Low-level VGA renderer stores CGA bytes through VGA odd/even layout:
 *   vga_addr = ((cga_addr & ~1) << 1) | (cga_addr & 1)
 *
 * That formula is already used by render_gfx_line_cga() in the low-level
 * driver, so BIOS writes the same layout.
 */
static void bios_10h_putpixel_cga4(uint16_t x, uint16_t y,
                                   uint8_t color, bool xor_mode)
{
    uint32_t cga_bank = (y & 1) ? 0x2000u : 0x0000u;
    uint32_t cga_row = y >> 1;
    uint32_t cga_addr = cga_bank + cga_row * 80u + (x >> 2);
    uint32_t vga_addr = ((cga_addr & ~1u) << 1) | (cga_addr & 1u);
    uint32_t addr = VGA_MEM_BASE_CGA + vga_addr;

    uint8_t shift = (uint8_t)((3u - (x & 3u)) * 2u);
    uint8_t mask = (uint8_t)(0x03u << shift);
    uint8_t pix = (uint8_t)((color & 0x03u) << shift);
    uint8_t old = read86(addr);

    if (xor_mode)
        old ^= pix;
    else
        old = (old & ~mask) | pix;

    write86(addr, old);
}

/*
 * Write one pixel in CGA 640x200 2-color mode 06h.
 *
 * Guest-visible CGA layout is the same even/odd 0/2000h split as mode 04h,
 * but each byte contains 8 one-bit pixels.
 *
 * The low-level renderer reads only VGA plane 0 for this mode and places
 * every guest byte at offset*4.  Therefore BIOS writes:
 *
 *   physical = B8000h + cga_addr * 4
 */
static void bios_10h_putpixel_cga2(uint16_t x, uint16_t y,
                                   uint8_t color, bool xor_mode)
{
    uint32_t bank = (y & 1) ? 0x2000u : 0x0000u;
    uint32_t row = y >> 1;
    uint32_t cga_addr = bank + row * 80u + (x >> 3);
    uint32_t addr = VGA_MEM_BASE_CGA + cga_addr * 4u;

    uint8_t mask = (uint8_t)(0x80u >> (x & 7u));
    uint8_t old = read86(addr);

    if (xor_mode) {
        if (color & 0x01)
            old ^= mask;
    } else {
        if (color & 0x01)
            old |= mask;
        else
            old &= ~mask;
    }

    write86(addr, old);
}

/*
 * Write one pixel in EGA/VGA planar 16-color modes.
 *
 * Low-level renderer reads one uint32_t per 8 pixels:
 *
 *   byte 0 = plane 0
 *   byte 1 = plane 1
 *   byte 2 = plane 2
 *   byte 3 = plane 3
 *
 * Each plane byte stores the same pixel bit position.  The final 4-bit color
 * is assembled by render_gfx_line_ega().
 *
 * page_base allows old BIOS page numbers in modes where page_size is nonzero.
 */
static void bios_10h_putpixel_planar16(const VgaMode *m,
                                       uint8_t page,
                                       uint16_t x,
                                       uint16_t y,
                                       uint8_t color,
                                       bool xor_mode)
{
    uint32_t page_base = m->page_size ? (uint32_t)page * m->page_size : 0;
    uint32_t bytes_per_line = m->cols;
    uint32_t byte_index = page_base + (uint32_t)y * bytes_per_line + (x >> 3);
    uint32_t addr = VGA_MEM_BASE_GFX + byte_index * 4u;

    uint8_t mask = (uint8_t)(0x80u >> (x & 7u));

    for (uint8_t plane = 0; plane < 4; plane++) {
        uint8_t old = read86(addr + plane);
        bool bit = ((color >> plane) & 1u) != 0;

        if (xor_mode) {
            if (bit)
                old ^= mask;
        } else {
            if (bit)
                old |= mask;
            else
                old &= ~mask;
        }

        write86(addr + plane, old);
    }
}

/*
 * Write one pixel in VGA 320x200 256-color mode 13h.
 *
 * Mode 13h is linear:
 *
 *   A000:0000 + y * 320 + x
 *
 * AL bit 7 is NOT XOR in 256-color mode; it is part of the 8-bit color.
 */
static void bios_10h_putpixel_linear256(uint16_t x, uint16_t y, uint8_t color)
{
    write86(VGA_MEM_BASE_GFX + (uint32_t)y * 320u + x, color);
}

/*
VIDEO - WRITE GRAPHICS PIXEL
AH = 0Ch
AL = pixel color
     bit 7 = XOR color, except in 256-color mode 13h
BH = display page
CX = column / X
DX = row / Y

Return:
Nothing

Desc:
Writes one pixel using the current BIOS video mode.  Text modes are rejected:
this is a graphics-only BIOS function.
*/
static bool bios_10h_0Ch(CPU* cpu)
{
    uint8_t mode = read86(0x449);
    const VgaMode *m = vga_find_mode(mode);

    if (!m || m->text) {
        cf = 1;
        return true;
    }

    uint16_t x = CPU_CX;
    uint16_t y = CPU_DX;

    if (x >= m->cols * 8u || y >= m->rows_minus_1 + 1u) {
        cf = 0;
        return true;
    }

    if (mode == 0x13) {
        bios_10h_putpixel_linear256(x, y, CPU_AL);
        cf = 0;
        return true;
    }

    bool xor_mode = (CPU_AL & BIOS10_PIXEL_XOR) != 0;
    uint8_t color = CPU_AL & 0x7F;

    switch (mode) {
    case 0x04:
    case 0x05:
        bios_10h_putpixel_cga4(x, y, color, xor_mode);
        break;

    case 0x06:
        bios_10h_putpixel_cga2(x, y, color, xor_mode);
        break;

    default:
        bios_10h_putpixel_planar16(m, CPU_BH, x, y, color, xor_mode);
        break;
    }

    cf = 0;
    return true;
}

/*
 * Read one pixel in CGA 320x200 4-color modes 04h/05h.
 *
 * This is the inverse of bios_10h_putpixel_cga4(): the guest CGA byte
 * address is converted to the odd/even VGA byte address used by the renderer,
 * then the selected 2-bit pixel field is extracted.
 */
static uint8_t bios_10h_getpixel_cga4(uint16_t x, uint16_t y)
{
    uint32_t cga_bank = (y & 1) ? 0x2000u : 0x0000u;
    uint32_t cga_row = y >> 1;
    uint32_t cga_addr = cga_bank + cga_row * 80u + (x >> 2);
    uint32_t vga_addr = ((cga_addr & ~1u) << 1) | (cga_addr & 1u);
    uint32_t addr = VGA_MEM_BASE_CGA + vga_addr;

    uint8_t shift = (uint8_t)((3u - (x & 3u)) * 2u);
    return (read86(addr) >> shift) & 0x03u;
}

/*
 * Read one pixel in CGA 640x200 2-color mode 06h.
 *
 * The renderer reads plane 0 at cga_addr*4, so the BIOS read uses the same
 * physical byte and extracts one MSB-first pixel bit.
 */
static uint8_t bios_10h_getpixel_cga2(uint16_t x, uint16_t y)
{
    uint32_t bank = (y & 1) ? 0x2000u : 0x0000u;
    uint32_t row = y >> 1;
    uint32_t cga_addr = bank + row * 80u + (x >> 3);
    uint32_t addr = VGA_MEM_BASE_CGA + cga_addr * 4u;

    uint8_t mask = (uint8_t)(0x80u >> (x & 7u));
    return (read86(addr) & mask) ? 0x01 : 0x00;
}

/*
 * Read one pixel in EGA/VGA planar 16-color modes.
 *
 * The low-level renderer stores the four VGA planes as four consecutive bytes
 * for each 8-pixel group:
 *
 *   A000:byte_index*4 + 0 = plane 0
 *   A000:byte_index*4 + 1 = plane 1
 *   A000:byte_index*4 + 2 = plane 2
 *   A000:byte_index*4 + 3 = plane 3
 *
 * The result color is reconstructed as a 4-bit value from the same bit
 * position in all four planes.
 */
static uint8_t bios_10h_getpixel_planar16(const VgaMode *m,
                                          uint8_t page,
                                          uint16_t x,
                                          uint16_t y)
{
    uint32_t page_base = m->page_size ? (uint32_t)page * m->page_size : 0;
    uint32_t bytes_per_line = m->cols;
    uint32_t byte_index = page_base + (uint32_t)y * bytes_per_line + (x >> 3);
    uint32_t addr = VGA_MEM_BASE_GFX + byte_index * 4u;

    uint8_t mask = (uint8_t)(0x80u >> (x & 7u));
    uint8_t color = 0;

    for (uint8_t plane = 0; plane < 4; plane++) {
        if (read86(addr + plane) & mask)
            color |= (uint8_t)(1u << plane);
    }

    return color;
}

/*
 * Read one pixel in VGA 320x200 256-color mode 13h.
 *
 * Mode 13h uses a linear byte-per-pixel framebuffer at A000:0000.
 */
static uint8_t bios_10h_getpixel_linear256(uint16_t x, uint16_t y)
{
    return read86(VGA_MEM_BASE_GFX + (uint32_t)y * 320u + x);
}

/*
VIDEO - READ GRAPHICS PIXEL
AH = 0Dh
BH = display page
CX = column / X
DX = row / Y

Return:
AL = pixel color

Desc:
Reads one pixel from the current graphics mode using the same video-memory
layout as INT 10h/AH=0Ch and the low-level VGA renderer.  Text modes are not
graphics modes; for them this native BIOS reports failure instead of returning
a meaningless character-cell byte.
*/
static bool bios_10h_0Dh(CPU* cpu)
{
    uint8_t mode = read86(0x449);
    const VgaMode *m = vga_find_mode(mode);

    if (!m || m->text) {
        cf = 1;
        return true;
    }

    uint16_t x = CPU_CX;
    uint16_t y = CPU_DX;
    uint16_t width = m->cols * 8u;
    uint16_t height = m->rows_minus_1 + 1u;

    if (x >= width || y >= height) {
        CPU_AL = 0;
        cf = 0;
        return true;
    }

    switch (mode) {
    case 0x04:
    case 0x05:
        CPU_AL = bios_10h_getpixel_cga4(x, y);
        break;

    case 0x06:
        CPU_AL = bios_10h_getpixel_cga2(x, y);
        break;

    case 0x13:
        CPU_AL = bios_10h_getpixel_linear256(x, y);
        break;

    default:
        CPU_AL = bios_10h_getpixel_planar16(m, CPU_BH, x, y);
        break;
    }

    cf = 0;
    return true;
}


/*
 * Store one text character cell and optionally update its attribute.
 *
 * Used by INT 10h/AH=13h.  Kept separate because AH=13h has two string
 * formats:
 *
 *   AL bit1 = 0: string contains characters only, BL supplies attribute
 *   AL bit1 = 1: string contains character/attribute pairs
 */
static void bios_10h_store_string_cell(uint8_t mode,
                                       uint8_t page,
                                       uint8_t row,
                                       uint8_t col,
                                       uint8_t ch,
                                       uint8_t attr,
                                       bool write_attr)
{
    uint32_t cell = bios_10h_text_cell(mode, page, row, col);

    write86(cell + 0, ch);

    if (write_attr)
        write86(cell + 1, attr);
}

/*
VIDEO - WRITE STRING
AH = 13h
AL = write mode
     bit 0: update cursor after writing
     bit 1: string contains character/attribute pairs
BH = page
BL = attribute if AL bit 1 is clear
CX = string length in characters
DH = row
DL = column
ES:BP -> string

Return:
Nothing

Desc:
Writes a string directly at DH:DL.  This is a common DOS/application BIOS
call for faster text output than repeated AH=0Eh calls.

Important:
When AL bit 1 is set, CX is still the number of CHARACTERS, not the number
of bytes.  The source then consumes two bytes per character:

    char, attr, char, attr, ...

This implementation does not scroll.  Real BIOS behavior for wrapping and
scrolling varies between adapters/BIOSes; clipping at the visible text page
is safer for this native BIOS layer.
*/
static bool bios_10h_13h(CPU* cpu)
{
    uint8_t mode = read86(0x449);
    uint8_t page = CPU_BH;

    if (page > 7 || !bios_10h_is_text_mode(mode)) {
        cf = 1;
        return true;
    }

    uint16_t cols = readw86(0x44A);
    uint8_t rows_minus_1 = read86(0x484);

    if (cols == 0)
        cols = 80;
    if (rows_minus_1 == 0)
        rows_minus_1 = 24;

    uint8_t row = CPU_DH;
    uint8_t col = CPU_DL;

    if (row > rows_minus_1 || col >= cols) {
        cf = 0;
        return true;
    }

    bool update_cursor = (CPU_AL & BIOS10_WRITE_STRING_UPDATE_CURSOR) != 0;
    bool has_attrs = (CPU_AL & BIOS10_WRITE_STRING_HAS_ATTRS) != 0;
    uint8_t default_attr = CPU_BL;

    uint32_t src = ((uint32_t)CPU_ES << 4) + CPU_BP;
    uint16_t count = CPU_CX;

    while (count--) {
        uint8_t ch = read86(src++);
        uint8_t attr = default_attr;

        if (has_attrs)
            attr = read86(src++);

        bios_10h_store_string_cell(mode, page, row, col, ch, attr, true);

        col++;
        if (col >= cols) {
            col = 0;
            row++;
            if (row > rows_minus_1)
                break;
        }
    }

    if (update_cursor) {
        uint16_t cur = ((uint16_t)row << 8) | col;

        writew86(0x450 + (uint16_t)page * 2, cur);
        bios_10h_set_crtc_cursor(cpu, page, row, col);
    }

    cf = 0;
    return true;
}

/*
VIDEO - GET CURRENT VIDEO MODE
AH = 0Fh

Return:
AH = number of character columns
AL = current video mode
BH = active display page
*/
static bool bios_10h_0Fh(CPU* cpu)
{
    uint16_t cols = readw86(0x44A);

    if (cols == 0)
        cols = 80;

    CPU_AH = cols & 0xFF;
    CPU_AL = read86(0x449);
    CPU_BH = read86(0x462);
    cf = 0;
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
 * Read one VGA Attribute Controller register.
 *
 * The Attribute Controller has an internal index/data flip-flop.  Reading
 * input-status register 3DAh resets it to index state before writing the
 * register number to 3C0h.
 */
static uint8_t bios_10h_attr_read(CPU* cpu, uint8_t reg)
{
    (void)cpu_portin8(VGA_INPUT_STATUS_1_COLOR_PORT);
    cpu_portout8(VGA_ATTR_INDEX_DATA_PORT, reg & 0x1F);
    return cpu_portin8(VGA_ATTR_DATA_READ_PORT);
}

/*
 * Write one VGA Attribute Controller register and re-enable display output.
 *
 * After Attribute Controller access, bit 5 must be written to 3C0h to leave
 * the controller in normal display-enabled state.
 */
static void bios_10h_attr_write(CPU* cpu, uint8_t reg, uint8_t value)
{
    (void)cpu_portin8(VGA_INPUT_STATUS_1_COLOR_PORT);
    cpu_portout8(VGA_ATTR_INDEX_DATA_PORT, reg & 0x1F);
    cpu_portout8(VGA_ATTR_INDEX_DATA_PORT, value);
    cpu_portout8(VGA_ATTR_INDEX_DATA_PORT, VGA_ATTR_ENABLE_DISPLAY);
}

/*
 * Apply classic CGA 320x200 4-color palette selection to VGA Attribute
 * Controller palette registers 1..3.
 *
 * This matters mainly for BIOS modes 04h/05h.  Other modes may legally call
 * AH=0Bh/BH=01h too; for them this write is harmless on a VGA-compatible
 * adapter and keeps the BIOS state consistent with the requested CGA palette.
 */
static void bios_10h_apply_cga_palette(CPU* cpu, uint8_t selector)
{
    bool alt = (selector & BIOS10_CGA_PALETTE_ALT) != 0;
    bool intense = (selector & BIOS10_CGA_PALETTE_INTENSE) != 0;
    uint8_t intensity = intense ? 0x08 : 0x00;

    uint8_t c1 = alt ? BIOS10_CGA_PAL1_COLOR1 : BIOS10_CGA_PAL0_COLOR1;
    uint8_t c2 = alt ? BIOS10_CGA_PAL1_COLOR2 : BIOS10_CGA_PAL0_COLOR2;
    uint8_t c3 = alt ? BIOS10_CGA_PAL1_COLOR3 : BIOS10_CGA_PAL0_COLOR3;

    bios_10h_attr_write(cpu, 0x01, c1 | intensity);
    bios_10h_attr_write(cpu, 0x02, c2 | intensity);
    bios_10h_attr_write(cpu, 0x03, c3 | intensity);
}

/*
VIDEO - SET BACKGROUND/BORDER COLOR OR CGA PALETTE
AH = 0Bh

BH = 00h
BL = border/background color

BH = 01h
BL = CGA 320x200 4-color palette selector

Return:
Nothing

Desc:
BH=00h controls the VGA overscan/border color through Attribute Controller
register 11h.  This is the VGA-compatible equivalent of the old CGA border
color BIOS call.

BH=01h selects the classic CGA 4-color palette.  On VGA this is represented
by Attribute Controller palette registers 1..3.
*/
static bool bios_10h_0Bh(CPU* cpu)
{
    switch (CPU_BH) {
    case 0x00:
        /*
         * VGA overscan color is a 6-bit palette index.  Masking avoids
         * leaking unrelated high bits from non-standard callers.
         */
        bios_10h_attr_write(cpu, VGA_ATTR_OVERSCAN_COLOR_REG, CPU_BL & 0x3F);
        write86(0x466, CPU_BL);
        cf = 0;
        return true;

    case 0x01:
        bios_10h_apply_cga_palette(cpu, CPU_BL);
        write86(0x467, CPU_BL);
        cf = 0;
        return true;

    default:
        cf = 1;
        return true;
    }
}

/*
VIDEO - SET SINGLE PALETTE REGISTER
AX = 1000h
BL = Attribute Controller palette register (00h..0Fh)
BH = palette value (00h..3Fh)

Return:
Nothing

Desc:
Writes one VGA Attribute Controller palette register.

Registers 00h..0Fh map logical EGA/VGA colors to DAC entries.
This is one of the most commonly used VGA palette BIOS services.
*/
static bool bios_10h_1000h(CPU* cpu)
{
    if (CPU_BL > 0x0F) {
        cf = 1;
        return true;
    }
    bios_10h_attr_write(cpu, CPU_BL, CPU_BH & 0x3F);
    cf = 0;
    return true;
}

/*
VIDEO - SET BORDER / OVERSCAN COLOR
AX = 1001h
BH = border / overscan color value, 00h..3Fh

Return:
Nothing

Desc:
Writes VGA Attribute Controller register 11h, the overscan color register.
This is the VGA palette-family equivalent of INT 10h/AH=0Bh/BH=00h.

The value is masked to 6 bits because standard VGA Attribute Controller
palette indexes select one of 64 DAC entries.
*/
static bool bios_10h_1001h(CPU* cpu)
{
    bios_10h_attr_write(cpu, VGA_ATTR_OVERSCAN_COLOR_REG, CPU_BH & 0x3F);
    cf = 0;
    return true;
}

/*
VIDEO - SET ALL PALETTE REGISTERS
AX = 1002h
ES:DX -> 17-byte palette table

Table format:
  bytes 00h..0Fh = Attribute Controller palette registers 00h..0Fh
  byte  10h      = overscan / border color register 11h

Return:
Nothing

Desc:
Loads the complete VGA Attribute Controller logical palette and border color.
This does not program DAC RGB values; it only maps logical attribute colors
to existing DAC indexes.
*/
static bool bios_10h_1002h(CPU* cpu)
{
    uint32_t table = ((uint32_t)CPU_ES << 4) + CPU_DX;
    for (uint8_t i = 0; i < 16; i++) {
        bios_10h_attr_write(cpu, i, read86(table + i) & 0x3F);
    }
    bios_10h_attr_write(cpu, VGA_ATTR_OVERSCAN_COLOR_REG, read86(table + 16) & 0x3F);
    cf = 0;
    return true;
}

/*
VIDEO - TOGGLE INTENSITY / BLINKING BIT
AX = 1003h
BL = 00h enable intensive background colors
BL = 01h enable blinking
BH = 00h, required by many BIOS references

VGA Attribute Controller register 10h:
  bit 3 = blink enable.

This controls interpretation of text attribute bit 7:
  blink enabled  -> bit 7 means blink
  blink disabled -> bit 7 becomes high background intensity
*/
static bool bios_10h_1003h(CPU* cpu)
{
    if (CPU_BH != 0) {
        cf = 1;
        return true;
    }

    uint8_t enable_blink = CPU_BL & 0x01;

    uint8_t mode_ctl = bios_10h_attr_read(cpu, VGA_ATTR_MODE_CONTROL_REG);

    if (enable_blink)
        mode_ctl |= 0x08;
    else
        mode_ctl &= ~0x08;

    bios_10h_attr_write(cpu, VGA_ATTR_MODE_CONTROL_REG, mode_ctl);

    cf = 0;
    return true;
}

/*
VIDEO - READ SINGLE PALETTE REGISTER
AX = 1007h
BL = Attribute Controller palette register (00h..0Fh)

Return:
BH = palette value

Desc:
Reads one VGA Attribute Controller logical palette register.

Registers 00h..0Fh map logical EGA/VGA attribute colors to DAC indexes.
Only the low 6 bits are meaningful on standard VGA.
*/
static bool bios_10h_1007h(CPU* cpu)
{
    if (CPU_BL > 0x0F) {
        cf = 1;
        return true;
    }
    CPU_BH = bios_10h_attr_read(cpu, CPU_BL) & 0x3F;
    cf = 0;
    return true;
}

/*
VIDEO - READ BORDER / OVERSCAN COLOR
AX = 1008h

Return:
BH = border / overscan color value

Desc:
Reads VGA Attribute Controller register 11h, the overscan color register.
This is the read counterpart of AX=1001h and INT 10h/AH=0Bh/BH=00h.

Only the low 6 bits are meaningful on standard VGA.
*/
static bool bios_10h_1008h(CPU* cpu)
{
    CPU_BH = bios_10h_attr_read(cpu, VGA_ATTR_OVERSCAN_COLOR_REG) & 0x3F;
    cf = 0;
    return true;
}

/*
VIDEO - READ ALL PALETTE REGISTERS
AX = 1009h
ES:DX -> 17-byte palette table

Table format:
  bytes 00h..0Fh = Attribute Controller palette registers 00h..0Fh
  byte  10h      = overscan / border color register 11h

Return:
Nothing

Desc:
Reads the complete VGA Attribute Controller logical palette and border color.
This is the read counterpart of AX=1002h.
*/
static bool bios_10h_1009h(CPU* cpu)
{
    uint32_t table = ((uint32_t)CPU_ES << 4) + CPU_DX;
    for (uint8_t i = 0; i < 16; i++) {
        write86(table + i, bios_10h_attr_read(cpu, i) & 0x3F);
    }
    write86(table + 16, bios_10h_attr_read(cpu, VGA_ATTR_OVERSCAN_COLOR_REG) & 0x3F);
    cf = 0;
    return true;
}

/*
VIDEO - SET INDIVIDUAL DAC REGISTER
AX = 1010h
BX = DAC register index
DH = red value,   00h..3Fh
CH = green value, 00h..3Fh
CL = blue value,  00h..3Fh

Return:
Nothing

Desc:
Programs one standard VGA DAC color entry.  Each RGB component is 6 bits.
The DAC auto-increments internally after the third component write.
*/
static bool bios_10h_1010h(CPU* cpu)
{
    cpu_portout8(VGA_DAC_WRITE_INDEX_PORT, CPU_BL);
    cpu_portout8(VGA_DAC_DATA_PORT, CPU_DH & 0x3F);
    cpu_portout8(VGA_DAC_DATA_PORT, CPU_CH & 0x3F);
    cpu_portout8(VGA_DAC_DATA_PORT, CPU_CL & 0x3F);
    cf = 0;
    return true;
}

/*
VIDEO - GET DISPLAY COMBINATION CODE
AX = 1A00h

Return if supported:
AL = 1Ah
BL = active display combination code
BH = alternate display combination code

Desc:
This is the standard VGA/EGA detection call.  Programs usually call it and
check only AL==1Ah to decide that the BIOS supports display-combination
reporting.

For this native BIOS there is only one built-in VGA-compatible adapter and
no second physical display.
*/
static bool bios_10h_1A00h(CPU* cpu)
{
    CPU_AL = 0x1A;
    CPU_BL = BIOS10_DCC_VGA_COLOR_ANALOG;
    CPU_BH = 0;
    cf = 0;
    return true;
}

/*
VIDEO - ALTERNATE FUNCTION SELECT - GET EGA INFORMATION
AH = 12h
BL = 10h

Return:
BH = video state
     00h color mode in effect, CRTC base 3Dxh
     01h mono mode in effect,  CRTC base 3Bxh
BL = installed adapter memory
     03h = 256 KiB
CH = feature connector bits
CL = switch settings

Desc:
This is an old EGA/VGA detection call.  Programs use it to distinguish
CGA-only BIOSes from EGA/VGA BIOSes and to determine adapter RAM size.

The native BIOS exposes a plain VGA-compatible adapter:
  - standard CGA/EGA/VGA register set;
  - 256 KiB VGA memory;
  - no SVGA/VESA claim here.

BDA 40:88 stores the EGA/VGA switch byte. Its high nibble contains feature
connector bits, while the full byte is returned as CL by this BIOS call.
If the BDA field was not initialized yet, use the standard color enhanced
fallback value.
*/
static bool bios_10h_1210h(CPU* cpu)
{
    uint16_t crtc = readw86(0x463);
    uint8_t switches = read86(0x488);

    if (crtc == 0)
        crtc = 0x3D4;

    if (switches == 0)
        switches = BIOS10_EGA_SWITCHES_COLOR_ENH;

    CPU_BH = (crtc == 0x3B4) ?
        BIOS10_EGA_INFO_MONO_IO :
        BIOS10_EGA_INFO_COLOR_IO;

    CPU_BL = BIOS10_EGA_INFO_MEM_256K;
    CPU_CH = switches >> 4;
    CPU_CL = switches;

    cf = 0;
    return true;
}

/*
VIDEO - SET BLOCK OF DAC REGISTERS
AX = 1012h
BX = starting DAC register index
CX = number of DAC registers to set
ES:DX -> RGB table

Table format:
  byte 0 = red
  byte 1 = green
  byte 2 = blue
  repeated CX times

Return:
Nothing

Desc:
Programs a consecutive block of standard VGA DAC entries.  Each component is
6 bits; values are masked to 00h..3Fh.  The VGA DAC auto-increments after
each complete RGB triplet.
*/
static bool bios_10h_1012h(CPU* cpu)
{
    uint32_t table = ((uint32_t)CPU_ES << 4) + CPU_DX;
    uint16_t count = CPU_CX;
    cpu_portout8(VGA_DAC_WRITE_INDEX_PORT, CPU_BL);
    while (count--) {
        cpu_portout8(VGA_DAC_DATA_PORT, read86(table++) & 0x3F);
        cpu_portout8(VGA_DAC_DATA_PORT, read86(table++) & 0x3F);
        cpu_portout8(VGA_DAC_DATA_PORT, read86(table++) & 0x3F);
    }
    cf = 0;
    return true;
}


/*
VIDEO - SELECT VIDEO DAC COLOR PAGE
AX = 1013h
BL = function
     00h set paging mode
     01h select page
BH = value
*/
static bool bios_10h_1013h(CPU* cpu)
{
    switch (CPU_BL) {
    case 0x00:
        vga_dac_page_mode = CPU_BH & 1;
        cf = 0;
        return true;
    case 0x01:
        vga_dac_page = CPU_BH & 0x0F;
        cf = 0;
        return true;
    default:
        cf = 1;
        return true;
    }
}
 
/*
VIDEO - READ INDIVIDUAL DAC REGISTER
AX = 1015h
BX = DAC index

Return:
DH = red
CH = green
CL = blue
*/
static bool bios_10h_1015h(CPU* cpu)
{
    cpu_portout8(VGA_DAC_READ_INDEX_PORT, CPU_BL);
    CPU_DH = cpu_portin8(VGA_DAC_DATA_PORT) & 0x3F;
    CPU_CH = cpu_portin8(VGA_DAC_DATA_PORT) & 0x3F;
    CPU_CL = cpu_portin8(VGA_DAC_DATA_PORT) & 0x3F;
    cf = 0;
    return true;
}

/*
VIDEO - READ BLOCK OF DAC REGISTERS
AX = 1017h
BX = starting DAC index
CX = count
ES:DX -> RGB table
*/
static bool bios_10h_1017h(CPU* cpu)
{
    uint32_t table = ((uint32_t)CPU_ES << 4) + CPU_DX;
    uint16_t count = CPU_CX;
    cpu_portout8(VGA_DAC_READ_INDEX_PORT, CPU_BL);
    while (count--) {
        write86(table++, cpu_portin8(VGA_DAC_DATA_PORT) & 0x3F);
        write86(table++, cpu_portin8(VGA_DAC_DATA_PORT) & 0x3F);
        write86(table++, cpu_portin8(VGA_DAC_DATA_PORT) & 0x3F);
    }
    cf = 0;
    return true;
}

/*
VIDEO - SET PEL MASK
AX = 1018h
BL = mask
*/
static bool bios_10h_1018h(CPU* cpu)
{
    cpu_portout8(VGA_PEL_MASK_PORT, CPU_BL);
    cf = 0;
    return true;
}
 
/*
VIDEO - READ PEL MASK
AX = 1019h

Return:
BL = current mask
*/
static bool bios_10h_1019h(CPU* cpu)
{
    CPU_BL = cpu_portin8(VGA_PEL_MASK_PORT);
    cf = 0;
    return true;
}

/*
VIDEO - GET VIDEO DAC COLOR PAGE STATE
AX = 101Ah

Return:
BL = paging mode
BH = current page
*/
static bool bios_10h_101Ah(CPU* cpu)
{
    CPU_BL = vga_dac_page_mode;
    CPU_BH = vga_dac_page;
    cf = 0;
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
        case 0x04:
            return bios_10h_04h(cpu); // READ LIGHT PEN POSITION
        case 0x05:
            return bios_10h_05h(cpu); // SELECT ACTIVE DISPLAY PAGE
        case 0x06:
            return bios_10h_scroll_window(cpu, false); // SCROLL WINDOW UP
        case 0x07:
            return bios_10h_scroll_window(cpu, true); // SCROLL WINDOW DOWN
        case 0x08:
            return bios_10h_08h(cpu); // READ CHARACTER AND ATTRIBUTE
        case 0x09:
            return bios_10h_09h(cpu); // WRITE CHARACTER AND ATTRIBUTE
        case 0x0A:
            return bios_10h_0Ah(cpu); // WRITE CHARACTER ONLY
        case 0x0B:
            return bios_10h_0Bh(cpu); // SET BORDER/BACKGROUND OR CGA PALETTE
        case 0x0C:
            return bios_10h_0Ch(cpu); // WRITE GRAPHICS PIXEL
        case 0x0D:
            return bios_10h_0Dh(cpu); // READ GRAPHICS PIXEL
        case 0x0E:
            return bios_10h_0Eh(cpu); // TELETYPE OUTPUT
        case 0x0F:
            return bios_10h_0Fh(cpu); // GET CURRENT VIDEO MODE
        case 0x10:
            switch(CPU_AL) {
            case 0: return bios_10h_1000h(cpu); // SET SINGLE PALETTE REGISTER
            case 1: return bios_10h_1001h(cpu); // SET BORDER / OVERSCAN COLOR
            case 2: return bios_10h_1002h(cpu); // SET ALL PALETTE REGISTERS
            case 3: return bios_10h_1003h(cpu); // TOGGLE BLINK / BACKGROUND INTENSITY
            case 7: return bios_10h_1007h(cpu); // READ SINGLE PALETTE REGISTER
            case 8: return bios_10h_1008h(cpu); // READ BORDER / OVERSCAN COLOR
            case 9: return bios_10h_1009h(cpu); // READ ALL PALETTE REGISTERS
            case 0x10: return bios_10h_1010h(cpu); // SET INDIVIDUAL DAC REGISTER
            case 0x12: return bios_10h_1012h(cpu); // SET BLOCK OF DAC REGISTERS
            case 0x13: return bios_10h_1013h(cpu); // SELECT VIDEO DAC COLOR PAGE
            case 0x15: return bios_10h_1015h(cpu); // READ INDIVIDUAL DAC REGISTER
            case 0x17: return bios_10h_1017h(cpu); // READ BLOCK OF DAC REGISTERS
            case 0x18: return bios_10h_1018h(cpu); // SET PEL MASK
            case 0x19: return bios_10h_1019h(cpu); // READ PEL MASK
            case 0x1A: return bios_10h_101Ah(cpu); // GET VIDEO DAC COLOR PAGE STATE
            }
            break;
        case 0x11:
            if (CPU_AL == 0x30)
                return bios_10h_1130h(cpu); // GET FONT INFORMATION (EGA, MCGA, VGA)
            break;
        case 0x12:
            if (CPU_BL == 0x10)
               return bios_10h_1210h(cpu); // GET EGA/VGA INFORMATION
            break;
        case 0x13:
            return bios_10h_13h(cpu); // WRITE STRING
        case 0x1A:
            if (CPU_AL == 0x00)
                return bios_10h_1A00h(cpu); // GET DISPLAY COMBINATION CODE
            break;
        default:
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
