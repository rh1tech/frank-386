#include <stdio.h>
#include "286/cpu.h"
#include "bios.h"
#include "vga.h"
#include "board_config.h"
#include "hardware/clocks.h"

#define BIOS_FONT_SEG       0xF000
#define BIOS_FONT8X16_OFF   0xA000
#define BIOS_FONT8X14_OFF   0xB000
#define BIOS_FONT8X8_OFF    0xBE00

/*
 * INT 10h/AX=1A00h Display Combination Code values.
 *
 * 08h = VGA with analog color display.
 *
 * The native BIOS exposes a VGA-compatible adapter plus a deliberately small
 * VBE 1.2 extension (currently mode 100h, 640x400x8 banked).  The legacy display
 * combination code still reports a normal VGA color display, which is what DOS
 * software expects from AX=1A00h.
 */
#define BIOS10_DCC_VGA_COLOR_ANALOG  0x08
#define BIOS10_DCC_EGA_COLOR          0x04
#define BIOS10_DCC_MCGA_COLOR_ANALOG  0x0C

/*
 * INT 10h/AX=1B00h GET FUNCTIONALITY/STATE INFORMATION.
 *
 * ES:DI points to a 64-byte caller buffer.  The first field is a far pointer
 * to the static functionality table.  Keep that table in guest-visible BIOS
 * ROM area, below the built-in font tables.
 */
#define BIOS10_FUNC_STATIC_SEG        BIOS_FONT_SEG
#define BIOS10_FUNC_STATIC_OFF        0x9000
#define BIOS10_FUNC_INFO_SIZE         0x40

/*
 * Minimal VBE 1.2 ROM data. Keep it below the functionality table/font area.
 */
#define BIOS10_VBE_ROM_SEG             BIOS_FONT_SEG
#define BIOS10_VBE_MODELIST_OFF        0x9040
#define BIOS10_VBE_OEM_OFF             0x9050
#define BIOS10_VBE_WINFUNC_OFF         0x9060
#define BIOS10_VBE_MODE_640x400x8      0x0100
#define BIOS10_VBE_MODE_320x200x15     0x010D
#define BIOS10_VBE_MODE_320x200x16     0x010E
#define BIOS10_VBE_MODE_320x200x24     0x010F
#define BIOS10_VBE_WINDOW_KB           64
#define BIOS10_VBE_TOTAL_64K_BLOCKS    4

/*
 * Supported video modes bitmap for INT 10h/AX=1B00h static table.
 *
 * Bits set here match vga_modes[]:
 *   00h..07h, 0Dh..13h
 */
#if defined(EGA128)
#define BIOS10_FUNC_MODES_BITMAP      0x0001E0FFu /* 00h..07h, 0Dh..10h */
#elif defined(VGA128)
#define BIOS10_FUNC_MODES_BITMAP      0x000BE0FFu /* 00h..11h, 13h; no 12h */
#elif defined(MCGA)
#define BIOS10_FUNC_MODES_BITMAP      0x000A00FFu /* 00h..07h, 11h, 13h */
#else
#define BIOS10_FUNC_MODES_BITMAP      0x000FE0FFu
#endif

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
#define BIOS10_EGA_INFO_MEM_64K       0x00
#define BIOS10_EGA_INFO_MEM_128K      0x01
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
#define VGA_ATTR_COLOR_SELECT_REG      0x14
#define VGA_ATTR_MODE_P54S             0x80

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
#define VGA_MISC_OUTPUT_READ_PORT      0x3CC
#define VGA_MISC_OUTPUT_WRITE_PORT     0x3C2
#define VGA_SEQ_INDEX_PORT             0x3C4
#define VGA_SEQ_DATA_PORT              0x3C5
#define VGA_GFX_INDEX_PORT             0x3CE
#define VGA_GFX_DATA_PORT              0x3CF
#define BIOS10_STATE_BLOCK             64

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
 * VGA text character generator layout used by this emulator.
 *
 * Low-level renderer reads font bytes from VGA RAM plane 2:
 *
 *   block_base + ch * 32 * 4 + row * 4 + 2
 *
 * where:
 *   32 = maximum VGA character-cell height in bytes;
 *   *4 = emulator stores VGA planes as four consecutive bytes;
 *   +2 = plane 2, the standard VGA font plane.
 *
 * BL in INT 10h/AX=11xx selects one of eight 8 KiB font blocks.
 */
#define BIOS10_FONT_BLOCK_COUNT        8
#define BIOS10_FONT_CHARS_PER_BLOCK    256
#define BIOS10_FONT_MAX_HEIGHT         32
#define BIOS10_FONT_BLOCK_BYTES        (BIOS10_FONT_CHARS_PER_BLOCK * BIOS10_FONT_MAX_HEIGHT)

/*
 * EGA/VGA BIOS Data Area fields used by alternate-select services.
 */
/* Имена и адреса строго по seabios/src/std/bda.h:
 *   40:65 video_msr    - тень Mode-Select Register порта 3x8h
 *   40:87 video_ctl    - бит0 cursor emulation, бит1 grayscale?, бит7 no_clear
 *   40:89 modeset_ctl  - биты 7/4 scan lines, бит1 grayscale summing
 * Прежние имена VIDEO_CTL/VIDEO_MODE_OPTIONS/VIDEO_DISPLAY_DATA были
 * смещены на одно поле и приводили к чтению/записи не тех байт. */
#define BIOS10_BDA_VIDEO_MSR           0x465
#define BIOS10_BDA_VIDEO_CTL           0x487
#define BIOS10_BDA_MODESET_CTL         0x489

/*
 * BIOS-private flag: AH=12h/BL=30h explicitly selected a text scan-line
 * mode, so the next mode set should apply it.
 *
 * Живёт в modeset_ctl (40:89) бит 6 - он не используется ни SeaBIOS
 * (vgainit.c: modeset_ctl = 0x51), ни этим BIOS.  Раньше флаг занимал
 * video_ctl бит 7, который SeaBIOS отводит под no_clear (vgabios.c:303),
 * из-за чего set-mode с mode|0x80 ложно взводил флаг, а обычный set-mode
 * (video_ctl = 0x60) стирал выбор BL=30h до того, как он применялся.
 */
#define BIOS10_MC_SCANLINE_SELECTED    0x40

/*
 * BDA 40:89 scan-line selection bits.
 *
 * VGA BIOS convention:
 *   bits 7,4 = 00b -> 350 scan lines
 *   bits 7,4 = 01b -> 400 scan lines
 *   bits 7,4 = 10b -> 200 scan lines
 */
#define BIOS10_MC_VGA_ACTIVE           0x01
#define BIOS10_MC_SCANLINE_MASK        0x90
#define BIOS10_MC_SCANLINE_350         0x00
#define BIOS10_MC_SCANLINE_400         0x10
#define BIOS10_MC_SCANLINE_200         0x80
#define BIOS10_MC_GRAYSCALE_SUMMING    0x02

/*
 * Convert the logical cursor shape stored in BDA to the physical shape
 * required by the current character-cell height, then program the CRTC.
 *
 * The BDA value itself remains unchanged, so INT 10h/AH=03h returns the
 * same shape that was set by software.
 */
static void bios_10h_program_cursor_shape(CPU* cpu, uint16_t shape)
{
    uint8_t start = shape >> 8;
    uint8_t end = shape;

    /*
     * Match SeaBIOS get_cursor_shape().
     * BDA video_ctl bit 0:
     *   0 = emulate old 8-scan-line cursor values
     *   1 = use supplied scan lines directly
     */
    if ((read86(BIOS10_BDA_VIDEO_CTL) & 0x01) == 0) {
        uint8_t emu_start = start & 0x3F;
        uint8_t emu_end = end & 0x1F;
        uint16_t height = readw86(0x485);

        if (height > 8 && emu_end < 8 && emu_start < 0x20) {
            if (emu_end != (uint8_t)(emu_start + 1))
                emu_start =
                    ((uint16_t)(emu_start + 1) * height / 8) - 1;
            else
                emu_start =
                    ((uint16_t)(emu_end + 1) * height / 8) - 2;

            emu_end = ((uint16_t)(emu_end + 1) * height / 8) - 1;
            start = emu_start;
            end = emu_end;
        }
    }

    uint16_t crtc = readw86(0x463);
    if (crtc == 0)
        crtc = 0x3D4;

    cpu_portout8(crtc, 0x0A);
    cpu_portout8(crtc + 1, start);
    cpu_portout8(crtc, 0x0B);
    cpu_portout8(crtc + 1, end);
}

/*
 * Forward declaration: mode-set scan-line selection needs to load the
 * corresponding ROM font into VGA plane-2 font RAM.  The full implementation
 * is below the AX=11xx font services.
 */
static void bios_10h_load_font_block(CPU* cpu,
                                     uint8_t block,
                                     uint32_t src,
                                     uint16_t first_char,
                                     uint16_t count,
                                     uint8_t bytes_per_char);

/* Линейный адрес ROM-шрифта по высоте знакоместа (8/14/16). */
static uint32_t bios_10h_rom_font_src(uint8_t height)
{
    switch (height) {
    case 8:  return ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X8_OFF;
    case 14: return ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X14_OFF;
    default: return ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X16_OFF;
    }
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

static const VgaRegs vga_640x480x2 = {
    0xE3,
    {0x03,0x01,0x01,0x00,0x06},
    {0x5F,0x4F,0x50,0x82,0x54,0x80,0x0B,0x3E,
     0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
     0xEA,0x8C,0xDF,0x28,0x00,0xE7,0x04,0xE3,0xFF},
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

#ifndef MCGA
    {0x0D,0,40,24,8, 0x2000,0x3D4,0xA0000,0x20000,&vga_320x200x16},
    {0x0E,0,80,24,8, 0x4000,0x3D4,0xA0000,0x20000,&vga_640x200x16},
    {0x0F,0,80,24,14,0x8000,0x3B4,0xA0000,0x20000,&vga_640x350x16},
    {0x10,0,80,24,14,0x8000,0x3D4,0xA0000,0x20000,&vga_640x350x16},
#endif
#ifndef EGA128
    {0x11,0,80,29,16,0x0000,0x3D4,0xA0000,0x09600,&vga_640x480x2},
#if !defined(VGA128) && !defined(MCGA)
    {0x12,0,80,29,16,0x0000,0x3D4,0xA0000,0x20000,&vga_640x480x16},
#endif
    {0x13,0,40,24,8, 0x1000,0x3D4,0xA0000,0x10000,&vga_320x200x256},
#endif
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
 * Return character height selected by INT 10h/AH=12h/BL=30h.
 *
 * This is used only after that service was explicitly called.  Otherwise
 * normal mode-table defaults remain in effect.
 */
static uint8_t bios_10h_selected_scanline_char_height(void)
{
    switch (read86(BIOS10_BDA_MODESET_CTL) & BIOS10_MC_SCANLINE_MASK) {
    case BIOS10_MC_SCANLINE_200:
        return 8;
    case BIOS10_MC_SCANLINE_350:
        return 14;
    case BIOS10_MC_SCANLINE_400:
    default:
        return 16;
    }
}

/*
 * Apply AH=12h/BL=30h text scan-line selection on the next mode set.
 *
 * VGA BIOS semantics: the request does not immediately change the current
 * mode; it changes how the following text-mode set is initialized.
 *
 * For this native BIOS:
 *   200 scan lines -> 8x8 ROM font, 25 rows
 *   350 scan lines -> 8x14 ROM font, 25 rows
 *   400 scan lines -> 8x16 ROM font, 25 rows
 *
 * This intentionally differs from AX=1102h font loading, where 8x8 can
 * produce 50-row text.  AH=12h/BL=30h is a mode-set policy, not a direct
 * "switch to 50 rows" request.
 */
static void bios_10h_apply_selected_text_scanlines(CPU* cpu)
{
    uint8_t mctl = read86(BIOS10_BDA_MODESET_CTL);
    if (!(mctl & BIOS10_MC_SCANLINE_SELECTED))
        return;
    /* Одноразовая политика: гасим флаг сразу, чтобы следующий set-mode
       вернулся к таблице режимов. */
    write86(BIOS10_BDA_MODESET_CTL, mctl & ~BIOS10_MC_SCANLINE_SELECTED);

    uint8_t height = bios_10h_selected_scanline_char_height();

    bios_10h_load_font_block(cpu, 0, bios_10h_rom_font_src(height),
                             0, 256, height);

    write86(0x484, 24);
    writew86(0x485, height);

    uint16_t crtc = readw86(0x463);
    if (crtc == 0)
        crtc = 0x3D4;

    cpu_portout8(crtc, 0x09);
    uint8_t reg09 = cpu_portin8(crtc + 1);
    reg09 = (reg09 & 0xE0) | ((height - 1) & 0x1F);
    cpu_portout8(crtc, 0x09);
    cpu_portout8(crtc + 1, reg09);
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

#if !defined(EGA128) && !defined(VGA128) && !defined(MCGA)
    /* Legacy VGA mode set must leave DISPI/VBE mode first. */
    cpu_portout16(0x1CE, VBE_DISPI_INDEX_ENABLE);
    cpu_portout16(0x1CF, VBE_DISPI_DISABLED);
#endif

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

    if (m->text)
        writew86(0x460, 0x0607);

    /* SeaBIOS vga_set_mode: video_ctl/video_switches/modeset_ctl.
       video_ctl - это BDA 0x487 (vgabios.c:303: 0x60 = 256K, адаптер
       активен; бит 7 = no_clear). Пишем канонические IBM-значения
       3x8 по номеру режима. */
#ifdef MCGA
    write86(BIOS10_BDA_VIDEO_CTL, no_clear ? 0x80 : 0x00);
#elif defined(EGA128) || defined(VGA128)
    write86(BIOS10_BDA_VIDEO_CTL, no_clear ? 0xA0 : 0x20);
#else
    write86(BIOS10_BDA_VIDEO_CTL, no_clear ? 0xE0 : 0x60);
#endif
    {
        static const uint8_t crt_msr[8] =
            { 0x2C, 0x28, 0x2D, 0x29, 0x2A, 0x2E, 0x1E, 0x29 };
        write86(BIOS10_BDA_VIDEO_MSR, (mode < 8) ? crt_msr[mode] : 0x29);
    }
    write86(0x488, 0xF9);                               /* video_switches */
#ifdef MCGA
    write86(0x48A, BIOS10_DCC_MCGA_COLOR_ANALOG);
#elif defined(EGA128)
    write86(0x48A, BIOS10_DCC_EGA_COLOR);
#else
    write86(0x48A, BIOS10_DCC_VGA_COLOR_ANALOG);
#endif
    if (m->text) {
        /* Плоскость 2 - общая планарная память: легальные записи
           графических режимов (Mode X/Y и т.п.) стирают знакогенератор.
           Как в SeaBIOS vga_set_mode, текстовый mode-set ВСЕГДА
           перезаливает ROM-шрифт режима в plane 2; политика
           AH=12h/BL=30h ниже лишь заменяет его другим по высоте. */
        bios_10h_load_font_block(cpu, 0,
                                 bios_10h_rom_font_src(m->char_height),
                                 0, 256, m->char_height);
        bios_10h_apply_selected_text_scanlines(cpu);
        bios_10h_program_cursor_shape(cpu, readw86(0x460));
    }

    /* SeaBIOS vgabios.c:305 - бит7 modeset_ctl (запрос 200 строк) гасится
       на set-mode.  Делаем это ПОСЛЕ текстового пути: иначе выбор
       AH=12h/BL=30h AL=00h стирался раньше, чем применялся. */
    write86(BIOS10_BDA_MODESET_CTL,
            read86(BIOS10_BDA_MODESET_CTL) & 0x7F);

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

/* -------------------------------------------------------------------------
 * Minimal VBE 1.2 services.
 *
 * Standard banked modes which fit in the 256 KiB VGA aperture and map directly
 * to the existing Bochs-style DISPI backend are exposed here.
 * ---------------------------------------------------------------------- */

typedef struct {
    uint16_t mode;
    uint16_t xres;
    uint16_t yres;
    uint16_t bytes_per_scanline;
    uint8_t bpp;
    uint8_t banks;
    uint8_t memory_model;
    uint8_t red_size, red_pos;
    uint8_t green_size, green_pos;
    uint8_t blue_size, blue_pos;
    uint8_t rsvd_size, rsvd_pos;
} Bios10VbeMode;

static const Bios10VbeMode bios10_vbe_modes[] = {
    { BIOS10_VBE_MODE_640x400x8,  640, 400, 640, 8,  4, 4,
      0, 0, 0, 0, 0, 0, 0, 0 },
    { BIOS10_VBE_MODE_320x200x15, 320, 200, 640, 15, 2, 6,
      5, 10, 5, 5, 5, 0, 1, 15 },
    { BIOS10_VBE_MODE_320x200x16, 320, 200, 640, 16, 2, 6,
      5, 11, 6, 5, 5, 0, 0, 0 },
    { BIOS10_VBE_MODE_320x200x24, 320, 200, 960, 24, 3, 6,
      8, 16, 8, 8, 8, 0, 0, 0 },
};

static const Bios10VbeMode *bios10_vbe_find_mode(uint16_t mode)
{
    for (uint8_t i = 0; i < sizeof(bios10_vbe_modes) / sizeof(bios10_vbe_modes[0]); i++) {
        if (bios10_vbe_modes[i].mode == mode)
            return &bios10_vbe_modes[i];
    }
    return NULL;
}
static inline void bios10_vbe_reg_write(CPU *cpu, uint16_t index, uint16_t value)
{
    cpu_portout16(0x1CE, index);
    cpu_portout16(0x1CF, value);
}

static inline uint16_t bios10_vbe_reg_read(CPU *cpu, uint16_t index)
{
    cpu_portout16(0x1CE, index);
    return cpu_portin16(0x1CF);
}

static inline void bios10_vbe_ok(CPU *cpu)
{
    CPU_AX = 0x004F;
    cf = 0;
}

static inline void bios10_vbe_fail(CPU *cpu)
{
    CPU_AX = 0x014F;
    cf = 0;
}

static void bios10_write_far_ptr(uint32_t dst, uint16_t seg, uint16_t off)
{
    writew86(dst + 0, off);
    writew86(dst + 2, seg);
}

static bool bios_10h_4F00h(CPU *cpu)
{
    uint32_t dst = ((uint32_t)CPU_ES << 4) + CPU_DI;
    uint32_t modelist = ((uint32_t)BIOS10_VBE_ROM_SEG << 4) +
                         BIOS10_VBE_MODELIST_OFF;

    /*
     * Publish exactly the modes handled by 4F01h/4F02h.  VideoModePtr is a
     * persistent far pointer, so the list itself lives in the guest-visible
     * BIOS area rather than in the caller's temporary controller-info block.
     */
    for (uint8_t i = 0;
         i < sizeof(bios10_vbe_modes) / sizeof(bios10_vbe_modes[0]); ++i)
        writew86(modelist + (uint32_t)i * 2u, bios10_vbe_modes[i].mode);
    writew86(modelist +
             (uint32_t)(sizeof(bios10_vbe_modes) / sizeof(bios10_vbe_modes[0])) * 2u,
             0xFFFFu);

    /* VBE 1.x controller information block is 256 bytes. */
    for (uint16_t i = 0; i < 256; ++i)
        write86(dst + i, 0);

    write86(dst + 0, 'V');
    write86(dst + 1, 'E');
    write86(dst + 2, 'S');
    write86(dst + 3, 'A');
    writew86(dst + 4, 0x0102);  /* VBE 1.2 */

    bios10_write_far_ptr(dst + 6, BIOS10_VBE_ROM_SEG, BIOS10_VBE_OEM_OFF);
    writedw86(dst + 10, 0x00000000u); /* capabilities */
    bios10_write_far_ptr(dst + 14, BIOS10_VBE_ROM_SEG, BIOS10_VBE_MODELIST_OFF);
    writew86(dst + 18, BIOS10_VBE_TOTAL_64K_BLOCKS);

    bios10_vbe_ok(cpu);
    return true;
}

static bool bios_10h_4F01h(CPU *cpu)
{
    uint16_t mode = CPU_CX & 0x3FFFu;
    uint32_t dst = ((uint32_t)CPU_ES << 4) + CPU_DI;
    const Bios10VbeMode *m = bios10_vbe_find_mode(mode);

    if (!m) {
        bios10_vbe_fail(cpu);
        return true;
    }

    for (uint16_t i = 0; i < 256; ++i)
        write86(dst + i, 0);

    /* VbeModeInfoBlock, VBE 1.2 fields. */
    writew86(dst + 0x00, 0x0019); /* supported | color | graphics */
    write86 (dst + 0x02, 0x07);   /* WinA relocatable/readable/writable */
    write86 (dst + 0x03, 0x00);   /* no WinB */
    writew86(dst + 0x04, BIOS10_VBE_WINDOW_KB); /* granularity, KiB */
    writew86(dst + 0x06, BIOS10_VBE_WINDOW_KB); /* window size, KiB */
    writew86(dst + 0x08, 0xA000); /* WinA segment */
    writew86(dst + 0x0A, 0x0000); /* WinB segment */
    bios10_write_far_ptr(dst + 0x0C,
                         BIOS10_VBE_ROM_SEG, BIOS10_VBE_WINFUNC_OFF);
    writew86(dst + 0x10, m->bytes_per_scanline);
    writew86(dst + 0x12, m->xres);
    writew86(dst + 0x14, m->yres);
    write86 (dst + 0x16, 8);      /* character cell width */
    write86 (dst + 0x17, 16);     /* character cell height */
    write86 (dst + 0x18, 1);      /* planes */
    write86 (dst + 0x19, m->bpp);
    write86 (dst + 0x1A, m->banks);
    write86 (dst + 0x1B, m->memory_model);
    write86 (dst + 0x1C, 64);     /* bank size, KiB */

    uint32_t image_bytes = (uint32_t)m->bytes_per_scanline * m->yres;
    uint8_t pages = (uint8_t)((256u * 1024u) / image_bytes);
    write86(dst + 0x1D, pages ? (uint8_t)(pages - 1) : 0);
    write86(dst + 0x1E, 0);

    write86(dst + 0x1F, m->red_size);
    write86(dst + 0x20, m->red_pos);
    write86(dst + 0x21, m->green_size);
    write86(dst + 0x22, m->green_pos);
    write86(dst + 0x23, m->blue_size);
    write86(dst + 0x24, m->blue_pos);
    write86(dst + 0x25, m->rsvd_size);
    write86(dst + 0x26, m->rsvd_pos);
    write86(dst + 0x27, 0);

    /* VBE 2.0+ LFB field deliberately stays zero: these modes are banked. */
    writedw86(dst + 0x28, 0x00000000u);

    bios10_vbe_ok(cpu);
    return true;
}

static bool bios_10h_4F02h(CPU *cpu)
{
    uint16_t req = CPU_BX;
    uint16_t mode = req & 0x3FFFu;
    bool no_clear = (req & 0x8000u) != 0;
    const Bios10VbeMode *m = bios10_vbe_find_mode(mode);

    /* Linear-framebuffer requests are not supported by this VBE 1.2 facade. */
    if ((req & 0x4000u) || !m) {
        bios10_vbe_fail(cpu);
        return true;
    }

    /* Establish a known VGA/DAC baseline before enabling DISPI. */
    {
        uint16_t saved_ax = CPU_AX;
        CPU_AH = 0x00;
        CPU_AL = 0x13;
        bios_10h_00h(cpu);
        CPU_AX = saved_ax;
    }

    bios10_vbe_reg_write(cpu, VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bios10_vbe_reg_write(cpu, VBE_DISPI_INDEX_XRES, m->xres);
    bios10_vbe_reg_write(cpu, VBE_DISPI_INDEX_YRES, m->yres);
    bios10_vbe_reg_write(cpu, VBE_DISPI_INDEX_BPP, m->bpp);
    bios10_vbe_reg_write(cpu, VBE_DISPI_INDEX_BANK, 0);
    bios10_vbe_reg_write(cpu, VBE_DISPI_INDEX_ENABLE,
                         VBE_DISPI_ENABLED |
                         (no_clear ? VBE_DISPI_NOCLEARMEM : 0));

    bios10_vbe_ok(cpu);
    return true;
}

static bool bios_10h_4F03h(CPU *cpu)
{
    uint16_t enable = bios10_vbe_reg_read(cpu, VBE_DISPI_INDEX_ENABLE);

    if (enable & VBE_DISPI_ENABLED) {
        uint16_t x = bios10_vbe_reg_read(cpu, VBE_DISPI_INDEX_XRES);
        uint16_t y = bios10_vbe_reg_read(cpu, VBE_DISPI_INDEX_YRES);
        uint16_t bpp = bios10_vbe_reg_read(cpu, VBE_DISPI_INDEX_BPP);
        const Bios10VbeMode *found = NULL;

        for (uint8_t i = 0; i < sizeof(bios10_vbe_modes) / sizeof(bios10_vbe_modes[0]); i++) {
            const Bios10VbeMode *m = &bios10_vbe_modes[i];
            if (m->xres == x && m->yres == y && m->bpp == bpp) {
                found = m;
                break;
            }
        }

        if (!found) {
            bios10_vbe_fail(cpu);
            return true;
        }
        CPU_BX = found->mode;
    } else {
        CPU_BX = read86(0x449); /* standard VGA mode */
    }

    bios10_vbe_ok(cpu);
    return true;
}

static bool bios_10h_4F05h(CPU *cpu)
{
    if (CPU_BL != 0) { /* only window A exists */
        bios10_vbe_fail(cpu);
        return true;
    }

    if (!(bios10_vbe_reg_read(cpu, VBE_DISPI_INDEX_ENABLE) &
          VBE_DISPI_ENABLED)) {
        bios10_vbe_fail(cpu);
        return true;
    }

    switch (CPU_BH) {
    case 0x00: /* set window */
        if (CPU_DX >= BIOS10_VBE_TOTAL_64K_BLOCKS) {
            bios10_vbe_fail(cpu);
            return true;
        }
        bios10_vbe_reg_write(cpu, VBE_DISPI_INDEX_BANK, CPU_DX);
        break;
    case 0x01: /* get window */
        CPU_DX = bios10_vbe_reg_read(cpu, VBE_DISPI_INDEX_BANK);
        break;
    default:
        bios10_vbe_fail(cpu);
        return true;
    }

    bios10_vbe_ok(cpu);
    return true;
}

static bool bios_10h_4Fh(CPU *cpu)
{
    switch (CPU_AL) {
    case 0x00: return bios_10h_4F00h(cpu);
    case 0x01: return bios_10h_4F01h(cpu);
    case 0x02: return bios_10h_4F02h(cpu);
    case 0x03: return bios_10h_4F03h(cpu);
    case 0x05: return bios_10h_4F05h(cpu);
    default:
        bios10_vbe_fail(cpu);
        return true;
    }
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
    /* SeaBIOS stores the caller's unmodified CX in BDA cursor_type. */
    writew86(0x460, CPU_CX);
    bios_10h_program_cursor_shape(cpu, CPU_CX);
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

/* Write one pixel in VGA/MCGA mode 11h (640x480x2).
 * Guest-visible storage is one linear bit plane at A000:0000. */
static void bios_10h_putpixel_mono640(uint16_t x, uint16_t y,
                                      uint8_t color, bool xor_mode)
{
    uint32_t addr = VGA_MEM_BASE_GFX + (uint32_t)y * 80u + (x >> 3);
    uint8_t mask = (uint8_t)(0x80u >> (x & 7u));
    uint8_t old = read86(addr);

    if (xor_mode) {
        if (color & 1u) old ^= mask;
    } else if (color & 1u) {
        old |= mask;
    } else {
        old &= (uint8_t)~mask;
    }
    write86(addr, old);
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

    case 0x11:
        bios_10h_putpixel_mono640(x, y, color, xor_mode);
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

static uint8_t bios_10h_getpixel_mono640(uint16_t x, uint16_t y)
{
    uint32_t addr = VGA_MEM_BASE_GFX + (uint32_t)y * 80u + (x >> 3);
    return (read86(addr) & (uint8_t)(0x80u >> (x & 7u))) ? 1u : 0u;
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

    case 0x11:
        CPU_AL = bios_10h_getpixel_mono640(x, y);
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

bool bios_teletype(CPU* cpu, uint8_t ch, uint8_t page) {
    uint8_t mode = read86(0x449);
    uint16_t cols = readw86(0x44A);
    uint16_t page_size = readw86(0x44C);
    uint8_t active_page = read86(0x462);
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

/* TELETYPE OUTPUT
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
    return bios_teletype(cpu, CPU_AL, CPU_BH);
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
#ifdef MCGA
    CPU_BL = BIOS10_DCC_MCGA_COLOR_ANALOG;
#else
    CPU_BL = BIOS10_DCC_VGA_COLOR_ANALOG;
#endif
    CPU_BH = 0;
    cf = 0;
    return true;
}

/*
VIDEO - GET FUNCTIONALITY/STATE INFORMATION
AX = 1B00h
ES:DI -> 64-byte information buffer

Return if supported:
AL = 1Bh

Desc:
This is the VGA function/state information call.  DOS software mostly uses it
as a richer VGA capability probe after AX=1A00h.
*/
static bool bios_10h_1B00h(CPU* cpu)
{
    uint32_t info = ((uint32_t)CPU_ES << 4) + CPU_DI;
    uint32_t stat = ((uint32_t)BIOS10_FUNC_STATIC_SEG << 4) + BIOS10_FUNC_STATIC_OFF;
    for (uint8_t i = 0; i < BIOS10_FUNC_INFO_SIZE; i++)
        write86(info + i, 0x00);
    /*
     * struct video_func_static, 16 bytes:
     *   +00 dword supported mode bitmap
     *   +07 scanline support flags: 200/350/400
     *   +08 max visible character blocks
     *   +09 total character blocks
     *   +0A misc flags
     *   +0E save/restore flags
     */
    writedw86(stat + 0x00, BIOS10_FUNC_MODES_BITMAP);
    write86  (stat + 0x04, 0x00);
    write86  (stat + 0x05, 0x00);
    write86  (stat + 0x06, 0x00);
    write86  (stat + 0x07, 0x07);
    write86  (stat + 0x08, 0x02);
    write86  (stat + 0x09, BIOS10_FONT_BLOCK_COUNT);
    writew86 (stat + 0x0A, 0x0CE7);
    write86  (stat + 0x0C, 0x00);
    write86  (stat + 0x0D, 0x00);
    write86  (stat + 0x0E, 0x00);
    write86  (stat + 0x0F, 0x00);
    /*
     * struct video_func_info, 64 bytes.
     */
    writew86(info + 0x00, BIOS10_FUNC_STATIC_OFF);
    writew86(info + 0x02, BIOS10_FUNC_STATIC_SEG);
    for (uint8_t i = 0; i < 30; i++)
        write86(info + 0x04 + i, read86(0x449 + i));
    for (uint8_t i = 0; i < 3; i++)
        write86(info + 0x22 + i, read86(0x484 + i));
#ifdef MCGA
    write86 (info + 0x25, BIOS10_DCC_MCGA_COLOR_ANALOG);
#else
    write86 (info + 0x25, BIOS10_DCC_VGA_COLOR_ANALOG);
#endif
    write86 (info + 0x26, 0x00);
    writew86(info + 0x27, 16);
    write86 (info + 0x29, 8);
    write86 (info + 0x2A, 2);
    write86 (info + 0x2B, 0);
    write86 (info + 0x2C, 0);
    write86 (info + 0x2D, 0);
    write86 (info + 0x2E, 0);
#ifdef MCGA
    write86 (info + 0x31, BIOS10_EGA_INFO_MEM_64K);
#elif defined(EGA128) || defined(VGA128)
    write86 (info + 0x31, BIOS10_EGA_INFO_MEM_128K);
#else
    write86 (info + 0x31, BIOS10_EGA_INFO_MEM_256K);
#endif
    write86 (info + 0x32, 0x00);
    write86 (info + 0x33, read86(BIOS10_BDA_MODESET_CTL));
    CPU_AL = 0x1B;
    cf = 0;
    return true;
}

static uint8_t bios_10h_indexed_read(CPU* cpu, uint16_t index_port, uint8_t reg)
{
    cpu_portout8(index_port, reg);
    return cpu_portin8(index_port + 1);
}

static void bios_10h_indexed_write(CPU* cpu, uint16_t index_port, uint8_t reg, uint8_t val)
{
    cpu_portout8(index_port, reg);
    cpu_portout8(index_port + 1, val);
}

static uint16_t bios_10h_save_bda(uint32_t dst)
{
    for (uint8_t i = 0; i < 28; i++)
        write86(dst++, read86(0x449 + i));

    for (uint8_t i = 0; i < 6; i++)
        write86(dst++, read86(0x484 + i));

    writew86(dst + 0, readw86(0x1F * 4 + 0));
    writew86(dst + 2, readw86(0x1F * 4 + 2));
    writew86(dst + 4, readw86(0x43 * 4 + 0));
    writew86(dst + 6, readw86(0x43 * 4 + 2));

    return 28 + 6 + 8;
}

static uint16_t bios_10h_restore_bda(uint32_t src)
{
    for (uint8_t i = 0; i < 28; i++)
        write86(0x449 + i, read86(src++));

    for (uint8_t i = 0; i < 6; i++)
        write86(0x484 + i, read86(src++));

    writew86(0x1F * 4 + 0, readw86(src + 0));
    writew86(0x1F * 4 + 2, readw86(src + 2));
    writew86(0x43 * 4 + 0, readw86(src + 4));
    writew86(0x43 * 4 + 2, readw86(src + 6));

    return 28 + 6 + 8;
}

static uint16_t bios_10h_save_hw(CPU* cpu, uint32_t dst)
{
    uint16_t crtc = readw86(0x463);
    if (crtc == 0)
        crtc = 0x3D4;

    write86(dst++, cpu_portin8(VGA_MISC_OUTPUT_READ_PORT));

    for (uint8_t i = 0; i < 5; i++)
        write86(dst++, bios_10h_indexed_read(cpu, VGA_SEQ_INDEX_PORT, i));

    for (uint8_t i = 0; i < 25; i++)
        write86(dst++, bios_10h_indexed_read(cpu, crtc, i));

    for (uint8_t i = 0; i < 9; i++)
        write86(dst++, bios_10h_indexed_read(cpu, VGA_GFX_INDEX_PORT, i));

    for (uint8_t i = 0; i < 21; i++)
        write86(dst++, bios_10h_attr_read(cpu, i));

    return 1 + 5 + 25 + 9 + 21;
}

static uint16_t bios_10h_restore_hw(CPU* cpu, uint32_t src)
{
    uint16_t crtc = readw86(0x463);
    if (crtc == 0)
        crtc = 0x3D4;

    cpu_portout8(VGA_MISC_OUTPUT_WRITE_PORT, read86(src++));

    for (uint8_t i = 0; i < 5; i++)
        bios_10h_indexed_write(cpu, VGA_SEQ_INDEX_PORT, i, read86(src++));

    for (uint8_t i = 0; i < 25; i++)
        bios_10h_indexed_write(cpu, crtc, i, read86(src++));

    for (uint8_t i = 0; i < 9; i++)
        bios_10h_indexed_write(cpu, VGA_GFX_INDEX_PORT, i, read86(src++));

    for (uint8_t i = 0; i < 21; i++)
        bios_10h_attr_write(cpu, i, read86(src++));

    return 1 + 5 + 25 + 9 + 21;
}

static uint16_t bios_10h_save_dac(CPU* cpu, uint32_t dst)
{
    write86(dst++, cpu_portin8(VGA_PEL_MASK_PORT));

    cpu_portout8(VGA_DAC_READ_INDEX_PORT, 0);
    for (uint16_t i = 0; i < 256 * 3; i++)
        write86(dst++, cpu_portin8(VGA_DAC_DATA_PORT) & 0x3F);

    return 1 + 256 * 3;
}

static uint16_t bios_10h_restore_dac(CPU* cpu, uint32_t src)
{
    cpu_portout8(VGA_PEL_MASK_PORT, read86(src++));

    cpu_portout8(VGA_DAC_WRITE_INDEX_PORT, 0);
    for (uint16_t i = 0; i < 256 * 3; i++)
        cpu_portout8(VGA_DAC_DATA_PORT, read86(src++) & 0x3F);

    return 1 + 256 * 3;
}

static uint16_t bios_10h_state_size(uint16_t states)
{
    uint16_t size = 0;

    if (states & 0x01)
        size += 1 + 5 + 25 + 9 + 21;
    if (states & 0x02)
        size += 28 + 6 + 8;
    if (states & 0x04)
        size += 1 + 256 * 3;

    return size;
}

/*
VIDEO - SAVE/RESTORE VIDEO STATE
AH = 1Ch
AL = 00h get buffer size
AL = 01h save state
AL = 02h restore state
CX = requested state bits:
     bit 0 = hardware registers
     bit 1 = BIOS data area / font vectors
     bit 2 = DAC state
ES:BX -> buffer for AL=01h/02h

Return if supported:
AL = 1Ch
BX = required 64-byte blocks, for AL=00h only
*/
static bool bios_10h_1Ch(CPU* cpu)
{
    uint8_t cmd = CPU_AL;
    uint16_t states = CPU_CX;
    uint32_t pos = ((uint32_t)CPU_ES << 4) + CPU_BX;

    if (cmd > 2 || (states & ~0x0007)) {
        cf = 1;
        return true;
    }

    switch (cmd) {
    case 0x00:
        CPU_BX = (bios_10h_state_size(states) + BIOS10_STATE_BLOCK - 1) / BIOS10_STATE_BLOCK;
        break;
    case 0x01:
        if (states & 0x01) pos += bios_10h_save_hw(cpu, pos);
        if (states & 0x02) pos += bios_10h_save_bda(pos);
        if (states & 0x04) pos += bios_10h_save_dac(cpu, pos);
        break;
    case 0x02:
        if (states & 0x01) pos += bios_10h_restore_hw(cpu, pos);
        if (states & 0x02) pos += bios_10h_restore_bda(pos);
        if (states & 0x04) pos += bios_10h_restore_dac(cpu, pos);
        break;
    default:
unsupported:
        cf = 1;
        return true;
    }

    CPU_AL = 0x1C;
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
This is an old EGA/VGA detection call. Programs use it to distinguish
CGA-only BIOSes from EGA/VGA BIOSes and to determine adapter RAM size.
The MCGA profile deliberately does not dispatch BL=10h.  Software such as
Thexder uses an unchanged EGA-information probe as part of MCGA detection.

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

#ifdef MCGA
    CPU_BL = BIOS10_EGA_INFO_MEM_64K;
#elif defined(EGA128) || defined(VGA128)
    CPU_BL = BIOS10_EGA_INFO_MEM_128K;
#else
    CPU_BL = BIOS10_EGA_INFO_MEM_256K;
#endif
    CPU_CH = switches >> 4;
    CPU_CL = switches;

    cf = 0;
    return true;
}

/*
VIDEO - ALTERNATE FUNCTION SELECT - ALTERNATE PRINT SCREEN
AH = 12h
BL = 20h

Return:
Nothing

Desc:
Installs a video-BIOS Print Screen handler able to handle EGA/VGA text
heights other than 25 rows.

The native BIOS currently has no full INT 05h Print Screen renderer, so this
is accepted as a compatibility no-op.  Keep the TODO because a later INT 05h
implementation should consume this state.
*/
static bool bios_10h_1220h(CPU* cpu)
{
    ///TODO: integrate with native INT 05h Print Screen implementation
    CPU_AL = 0x12;
    return true;
}

/*
VIDEO - ALTERNATE FUNCTION SELECT - SELECT TEXT SCAN LINES
AH = 12h
BL = 30h
AL = vertical resolution
     00h 200 scan lines
     01h 350 scan lines
     02h 400 scan lines

Return:
AL = 12h if function supported

Desc:
Stores the requested text-mode scan-line policy.  VGA applies it on the next
mode set, not immediately.
*/
static bool bios_10h_1230h(CPU* cpu)
{
    uint8_t data = read86(BIOS10_BDA_MODESET_CTL) & ~BIOS10_MC_SCANLINE_MASK;
    data |= BIOS10_MC_SCANLINE_SELECTED;
    switch (CPU_AL) {
    case 0x00:
        data |= BIOS10_MC_SCANLINE_200;
        break;
    case 0x01:
        data |= BIOS10_MC_SCANLINE_350;
        break;
    case 0x02:
        data |= BIOS10_MC_SCANLINE_400;
        break;
    default:
        cf = 1;
        return true;
    }
    write86(BIOS10_BDA_MODESET_CTL, data);
    CPU_AL = 0x12;
    cf = 0;
    return true;
}

/*
VIDEO - ALTERNATE FUNCTION SELECT - DEFAULT PALETTE LOADING
AH = 12h
BL = 31h
AL = 00h enable default palette loading
AL = 01h disable default palette loading

SeaBIOS stores this in BDA 40:65 bit 3.
*/
static bool bios_10h_1231h(CPU* cpu)
{
    uint8_t ctl = read86(BIOS10_BDA_VIDEO_CTL);
    ctl = (ctl & ~0x08) | ((CPU_AL & 0x01) << 3);
    write86(BIOS10_BDA_VIDEO_CTL, ctl);
    CPU_AL = 0x12;
    cf = 0;
    return true;
}

/*
VIDEO - ALTERNATE FUNCTION SELECT - VIDEO ADDRESSING
AH = 12h
BL = 32h
AL bit 0: 0 = enable CPU access to video memory, 1 = disable
*/
static bool bios_10h_1232h(CPU* cpu)
{
    uint8_t misc = cpu_portin8(VGA_MISC_OUTPUT_READ_PORT);
    if (CPU_AL & 0x01)
        misc &= ~0x02;
    else
        misc |= 0x02;
    cpu_portout8(VGA_MISC_OUTPUT_WRITE_PORT, misc);
    CPU_AL = 0x12;
    cf = 0;
    return true;
}

/*
VIDEO - ALTERNATE FUNCTION SELECT - GRAYSCALE SUMMING
AH = 12h
BL = 33h
AL = 00h enable grayscale summing
AL = 01h disable grayscale summing

SeaBIOS stores this in BDA 40:89 (modeset_ctl) bit 1 - vgabios.c:930.
*/
static bool bios_10h_1233h(CPU* cpu)
{
    uint8_t mctl = read86(BIOS10_BDA_MODESET_CTL);
    if (CPU_AL & 0x01)
        mctl &= ~BIOS10_MC_GRAYSCALE_SUMMING;
    else
        mctl |= BIOS10_MC_GRAYSCALE_SUMMING;
    write86(BIOS10_BDA_MODESET_CTL, mctl);
    CPU_AL = 0x12;
    cf = 0;
    return true;
}

/*
VIDEO - ALTERNATE FUNCTION SELECT - CURSOR EMULATION
AH = 12h
BL = 34h
AL = new state
     00h enable alphanumeric cursor emulation
     01h disable alphanumeric cursor emulation

Return:
AL = 12h if function supported

Desc:
Controls whether AH=01h remaps old 8-scan-line cursor shapes to the current
VGA character-cell height.
*/
static bool bios_10h_1234h(CPU* cpu)
{
    if (CPU_AL > 0x01) {
        cf = 1;
        return true;
    }
    uint8_t video_ctl = read86(BIOS10_BDA_VIDEO_CTL);
    write86(BIOS10_BDA_VIDEO_CTL, (video_ctl & ~0x01) | (CPU_AL & 0x01));
    CPU_AL = 0x12;
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

Paging mode:
  BH=00h -> 4 pages of 64 colors
  BH=01h -> 16 pages of 16 colors

Desc:
Uses VGA Attribute Controller state directly:
  AC register 10h bit 7 = page mode selector
  AC register 14h       = Color Select register
*/
static bool bios_10h_1013h(CPU* cpu)
{
    uint8_t mode_ctl = bios_10h_attr_read(cpu, VGA_ATTR_MODE_CONTROL_REG);
    uint8_t color_select = bios_10h_attr_read(cpu, VGA_ATTR_COLOR_SELECT_REG);

    switch (CPU_BL) {
    case 0x00:
        if (CPU_BH & 0x01)
            mode_ctl |= VGA_ATTR_MODE_P54S;
        else
            mode_ctl &= ~VGA_ATTR_MODE_P54S;
        bios_10h_attr_write(cpu, VGA_ATTR_MODE_CONTROL_REG, mode_ctl);
        cf = 0;
        return true;
    case 0x01:
        if (mode_ctl & VGA_ATTR_MODE_P54S) {
            color_select = (color_select & ~0x0F) | (CPU_BH & 0x0F);
        } else {
            color_select = (color_select & ~0x0C) | ((CPU_BH & 0x03) << 2);
        }
        bios_10h_attr_write(cpu, VGA_ATTR_COLOR_SELECT_REG, color_select);
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
     00h = 4 pages of 64 colors
     01h = 16 pages of 16 colors
BH = current active page

Desc:
Reads the state from VGA Attribute Controller registers instead of duplicating
it in BIOS-private variables.
*/
static bool bios_10h_101Ah(CPU* cpu)
{
    uint8_t mode_ctl = bios_10h_attr_read(cpu, VGA_ATTR_MODE_CONTROL_REG);
    uint8_t color_select = bios_10h_attr_read(cpu, VGA_ATTR_COLOR_SELECT_REG);
    if (mode_ctl & VGA_ATTR_MODE_P54S) {
        CPU_BL = 0x01;
        CPU_BH = color_select & 0x0F;
    } else {
        CPU_BL = 0x00;
        CPU_BH = (color_select >> 2) & 0x03;
    }
    cf = 0;
    return true;
}

/*
VIDEO - PERFORM GRAY-SCALE SUMMING
AX = 101Bh
BX = starting DAC register
CX = number of DAC registers

Desc:
Converts selected DAC entries to grayscale in-place.

IBM/VGA grayscale summing uses weighted RGB:
  gray = (77*R + 151*G + 28*B + 128) >> 8

R/G/B are VGA DAC 6-bit components, so gray is also clamped to 00h..3Fh.
*/
static bool bios_10h_101Bh(CPU* cpu)
{
    uint16_t first = CPU_BX;
    uint16_t count = CPU_CX;

    if (first >= 256) {
        cf = 1;
        return true;
    }
    if (count > 256 - first)
        count = 256 - first;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t r, g, b, gray;
        cpu_portout8(VGA_DAC_READ_INDEX_PORT, (uint8_t)(first + i));
        r = cpu_portin8(VGA_DAC_DATA_PORT) & 0x3F;
        g = cpu_portin8(VGA_DAC_DATA_PORT) & 0x3F;
        b = cpu_portin8(VGA_DAC_DATA_PORT) & 0x3F;
        gray = (uint8_t)(((uint16_t)77 * r + (uint16_t)151 * g + (uint16_t)28 * b + 128) >> 8);
        if (gray > 0x3F) {
            gray = 0x3F;
        }
        cpu_portout8(VGA_DAC_WRITE_INDEX_PORT, (uint8_t)(first + i));
        cpu_portout8(VGA_DAC_DATA_PORT, gray);
        cpu_portout8(VGA_DAC_DATA_PORT, gray);
        cpu_portout8(VGA_DAC_DATA_PORT, gray);
    }
    cf = 0;
    return true;
}

/*
 * Return physical address of one byte in VGA character-generator RAM.
 *
 * This follows the layout expected by vga_text_refresh()/vga_get_font_ptr():
 * character glyphs are stored in plane 2, with four bytes per VGA memory
 * address because the emulator keeps planes interleaved.
 */
/*
 * Гостевой адрес байта шрифта в plane 2 - ЛИНЕЙНЫЙ (A0000 + block*2000h +
 * ch*20h + row): в планарном режиме каждая плоскость адресуется гостем
 * плоско, плоскость выбирает map mask секвенсора. Interleave x4 (+2) - это
 * host-раскладка vga_ram, её формирует обработчик записи (стандартная
 * ветка vga_mem_write кладёт ((uint32_t*)vga_ram)[addr] с plane-маской).
 * Запись обязана идти при запрограммированном plane-2 доступе - см.
 * bios_10h_font_access_begin()/_end() ниже.
 */
static uint32_t bios_10h_font_plane_addr(uint8_t block,
                                         uint8_t ch,
                                         uint8_t row)
{
    uint32_t off =
        (uint32_t)(block & 0x07) * BIOS10_FONT_BLOCK_BYTES +
        (uint32_t)ch * BIOS10_FONT_MAX_HEIGHT +
        (uint32_t)row;
    return VGA_MEM_BASE_GFX + off;
}

/*
 * Программирование доступа к знакогенератору - зеркально SeaBIOS stdvga
 * get_font_access()/release_font_access(): plane 2, линейная планарная
 * адресация, окно A0000 64K; восстановление - штатные значения текстового
 * режима (odd/even, окно B8000). Без этого записи в A0000 при текстовом
 * GC6 (карта B8000) отбрасываются обработчиком, и шрифт не грузится -
 * именно так терялся знакогенератор после планарных игр (Wolf3D, Mode Y):
 * их записи легально стирают plane 2, а mode-set обязан перезалить шрифт.
 */
static void bios_10h_font_access_begin(CPU* cpu)
{
    cpu_portout8(0x3C4, 0x00); cpu_portout8(0x3C5, 0x01); /* sync reset */
    cpu_portout8(0x3C4, 0x02); cpu_portout8(0x3C5, 0x04); /* map mask: plane 2 */
    cpu_portout8(0x3C4, 0x04); cpu_portout8(0x3C5, 0x07); /* ext, flat, no chain4 */
    cpu_portout8(0x3C4, 0x00); cpu_portout8(0x3C5, 0x03); /* reset off */
    cpu_portout8(0x3CE, 0x04); cpu_portout8(0x3CF, 0x02); /* read map: plane 2 */
    cpu_portout8(0x3CE, 0x05); cpu_portout8(0x3CF, 0x00); /* write mode 0, flat */
    cpu_portout8(0x3CE, 0x06); cpu_portout8(0x3CF, 0x04); /* map A0000/64K, gfx */
}

static void bios_10h_font_access_end(CPU* cpu)
{
    cpu_portout8(0x3C4, 0x00); cpu_portout8(0x3C5, 0x01);
    cpu_portout8(0x3C4, 0x02); cpu_portout8(0x3C5, 0x03); /* planes 0+1 */
    cpu_portout8(0x3C4, 0x04); cpu_portout8(0x3C5, 0x03); /* ext, odd/even */
    cpu_portout8(0x3C4, 0x00); cpu_portout8(0x3C5, 0x03);
    cpu_portout8(0x3CE, 0x04); cpu_portout8(0x3CF, 0x00);
    cpu_portout8(0x3CE, 0x05); cpu_portout8(0x3CF, 0x10); /* odd/even write */
    cpu_portout8(0x3CE, 0x06); cpu_portout8(0x3CF, 0x0E); /* map B8000/32K, text */
}

/*
 * Update active text character height.
 *
 * INT 10h AX=11xx font-loading services also define the character-cell
 * height used by text rendering.  For standard VGA text modes this is done
 * through CRTC register 09h, low 5 bits = max scan line.
 *
 * BDA fields:
 *   40:84 = rows minus one
 *   40:85 = character height
 */
static void bios_10h_set_text_char_height(CPU* cpu, uint8_t height)
{
    if (height == 0 || height > BIOS10_FONT_MAX_HEIGHT)
        return;

    uint16_t crtc = readw86(0x463);
    if (crtc == 0)
        crtc = 0x3D4;

    cpu_portout8(crtc, 0x09);
    uint8_t reg09 = cpu_portin8(crtc + 1);
    reg09 = (reg09 & 0xE0) | ((height - 1) & 0x1F);
    cpu_portout8(crtc, 0x09);
    cpu_portout8(crtc + 1, reg09);
    writew86(0x485, height);
    /*
     * Current native BIOS standard text modes are 400 scan-line VGA modes.
     * With 8x8 font they become 50 rows; with 8x14/8x16 they remain 25 rows.
     */
    if (height <= 8)
        write86(0x484, 49);
    else
        write86(0x484, 24);
}

/*
 * Load a font table into VGA character-generator RAM.
 *
 * src points to a compact table:
 *   char0 row0..rowN, char1 row0..rowN, ...
 *
 * destination is the selected VGA font block in plane 2.
 * Rows above bytes_per_char are cleared up to 32 bytes so switching from a
 * taller font to a shorter one cannot leave stale glyph scanlines visible.
 */
static void bios_10h_load_font_block(CPU* cpu,
                                     uint8_t block,
                                     uint32_t src,
                                     uint16_t first_char,
                                     uint16_t count,
                                     uint8_t bytes_per_char)
{
    if (bytes_per_char == 0 || bytes_per_char > BIOS10_FONT_MAX_HEIGHT)
        return;

    if (first_char >= BIOS10_FONT_CHARS_PER_BLOCK)
        return;

    if (count > BIOS10_FONT_CHARS_PER_BLOCK - first_char)
        count = BIOS10_FONT_CHARS_PER_BLOCK - first_char;

    block &= 0x07;

    bios_10h_font_access_begin(cpu);

    for (uint16_t i = 0; i < count; i++) {
        uint8_t ch = (uint8_t)(first_char + i);

        for (uint8_t row = 0; row < bytes_per_char; row++) {
            write86(bios_10h_font_plane_addr(block, ch, row),
                    read86(src + (uint32_t)i * bytes_per_char + row));
        }

        for (uint8_t row = bytes_per_char; row < BIOS10_FONT_MAX_HEIGHT; row++) {
            write86(bios_10h_font_plane_addr(block, ch, row), 0x00);
        }
    }

    bios_10h_font_access_end(cpu);
}

/*
VIDEO - LOAD USER-SPECIFIED TEXT-MODE FONT
AX = 1100h
ES:BP -> user font table
CX = number of characters
DX = first character code
BL = font block
BH = bytes per character

Return:
Nothing
*/
static bool bios_10h_1100h(CPU* cpu)
{
    uint32_t src = ((uint32_t)CPU_ES << 4) + CPU_BP;

    bios_10h_load_font_block(cpu, CPU_BL, src, CPU_DX, CPU_CX, CPU_BH);
    bios_10h_set_text_char_height(cpu, CPU_BH);
    cf = 0;
    return true;
}

/*
VIDEO - LOAD 8x14 ROM TEXT-MODE FONT
AX = 1101h
BL = font block

Return:
Nothing
*/
static bool bios_10h_1101h(CPU* cpu)
{
    uint32_t src = ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X14_OFF;
    bios_10h_load_font_block(cpu, CPU_BL, src, 0, 256, 14);
    bios_10h_set_text_char_height(cpu, 14);
    cf = 0;
    return true;
}

/*
VIDEO - LOAD 8x8 ROM TEXT-MODE FONT
AX = 1102h
BL = font block

Return:
Nothing
*/
static bool bios_10h_1102h(CPU* cpu)
{
    uint32_t src = ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X8_OFF;
    bios_10h_load_font_block(cpu, CPU_BL, src, 0, 256, 8);
    bios_10h_set_text_char_height(cpu, 8);
    cf = 0;
    return true;
}

/*
VIDEO - LOAD 8x16 ROM TEXT-MODE FONT
AX = 1104h
BL = font block

Return:
Nothing
*/
static bool bios_10h_1104h(CPU* cpu)
{
    uint32_t src = ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X16_OFF;
    bios_10h_load_font_block(cpu, CPU_BL, src, 0, 256, 16);
    bios_10h_set_text_char_height(cpu, 16);
    cf = 0;
    return true;
}

/*
VIDEO - LOAD USER-SPECIFIED GRAPHICS FONT
AX = 1110h
ES:BP -> user font table
CX = number of characters
DX = first character code
BL = font block
BH = bytes per character

Desc:
VGA stores text/graphics character generator data in the same plane-2 font
RAM.  The 1110h service is therefore implemented through the same loader as
1100h, but kept as a separate BIOS entry point because DOS software may call
the graphics-font variant explicitly.
*/
static bool bios_10h_1110h(CPU* cpu)
{
    uint32_t src = ((uint32_t)CPU_ES << 4) + CPU_BP;
    bios_10h_load_font_block(cpu, CPU_BL, src, CPU_DX, CPU_CX, CPU_BH);
    bios_10h_set_text_char_height(cpu, CPU_BH);
    cf = 0;
    return true;
}

/*
VIDEO - LOAD ROM 8x14 GRAPHICS FONT
AX = 1111h
BL = font block

Desc:
Loads the BIOS ROM 8x14 font into VGA character-generator RAM.
*/
static bool bios_10h_1111h(CPU* cpu)
{
    uint32_t src = ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X14_OFF;
    bios_10h_load_font_block(cpu, CPU_BL, src, 0, 256, 14);
    bios_10h_set_text_char_height(cpu, 14);
    cf = 0;
    return true;
}

/*
VIDEO - LOAD ROM 8x8 GRAPHICS FONT
AX = 1112h
BL = font block

Desc:
Loads the BIOS ROM 8x8 font into VGA character-generator RAM.
*/
static bool bios_10h_1112h(CPU* cpu)
{
    uint32_t src = ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X8_OFF;
    bios_10h_load_font_block(cpu, CPU_BL, src, 0, 256, 8);
    bios_10h_set_text_char_height(cpu, 8);
    cf = 0;
    return true;
}

/*
VIDEO - LOAD ROM 8x16 GRAPHICS FONT
AX = 1114h
BL = font block

Desc:
Loads the BIOS ROM 8x16 font into VGA character-generator RAM.
*/
static bool bios_10h_1114h(CPU* cpu)
{
    uint32_t src = ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X16_OFF;
    bios_10h_load_font_block(cpu, CPU_BL, src, 0, 256, 16);
    bios_10h_set_text_char_height(cpu, 16);
    cf = 0;
    return true;
}

/*
VIDEO - SET ALTERNATE PRINT SCREEN CHARACTERS
AX = 1120h

Desc:
IBM/EGA/VGA BIOS function for print-screen character translation.
*/
static bool bios_10h_1120h(CPU* cpu)
{
    /* INT 10h AX=1120h: Set user 8x8 graphics characters.
     * Store ES:BP into interrupt vector 1Fh.
     */
    writew86(0x0001F * 4 + 0, CPU_BP);
    writew86(0x0001F * 4 + 2, CPU_ES);
    cf = 0;
    return true;
}

/*
VIDEO - SET USER GRAPHICS CHARACTERS
AX = 1121h
ES:BP -> user font table
CX = number of characters
DX = first character code
BL = font block
BH = bytes per character

Desc:
Loads user-supplied graphics character glyphs into VGA plane-2 font RAM.
*/
static bool bios_10h_1121h(CPU* cpu)
{
    uint32_t src = ((uint32_t)CPU_ES << 4) + CPU_BP;
    bios_10h_load_font_block(cpu, CPU_BL, src, CPU_DX, CPU_CX, CPU_BH);
    bios_10h_set_text_char_height(cpu, CPU_BH);
    cf = 0;
    return true;
}

/*
VIDEO - SET GRAPHICS 8x14 CHARACTERS
AX = 1122h
BL = font block

Desc:
Loads BIOS ROM 8x14 glyphs into VGA plane-2 font RAM.
*/
static bool bios_10h_1122h(CPU* cpu)
{
    uint32_t src = ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X14_OFF;
    bios_10h_load_font_block(cpu, CPU_BL, src, 0, 256, 14);
    bios_10h_set_text_char_height(cpu, 14);
    cf = 0;
    return true;
}

/*
VIDEO - SET GRAPHICS 8x8 DOUBLE-DOT CHARACTERS
AX = 1123h
BL = font block

Desc:
Loads BIOS ROM 8x8 glyphs into VGA plane-2 font RAM.

"Double-dot" is adapter terminology for 8x8 graphics character cells.  The
glyph data itself is still a normal 8-byte-per-character font table.
*/
static bool bios_10h_1123h(CPU* cpu)
{
    uint32_t src = ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X8_OFF;
    bios_10h_load_font_block(cpu, CPU_BL, src, 0, 256, 8);
    bios_10h_set_text_char_height(cpu, 8);
    cf = 0;
    return true;
}

/*
VIDEO - SET GRAPHICS 8x16 CHARACTERS
AX = 1124h
BL = font block

Desc:
Loads BIOS ROM 8x16 glyphs into VGA plane-2 font RAM.
*/
static bool bios_10h_1124h(CPU* cpu)
{
    uint32_t src = ((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X16_OFF;
    bios_10h_load_font_block(cpu, CPU_BL, src, 0, 256, 16);
    bios_10h_set_text_char_height(cpu, 16);
    cf = 0;
    return true;
}

/*
VIDEO - ALTERNATE FUNCTION SELECT - DISPLAY SWITCH INTERFACE
AH = 12h
BL = 35h

No real display switch hardware exists here.  SeaBIOS accepts this as a stub.
*/
static bool bios_10h_1235h(CPU* cpu)
{
    CPU_AL = 0x12;
    cf = 0;
    return true;
}

/*
VIDEO - ALTERNATE FUNCTION SELECT - VIDEO REFRESH CONTROL
AH = 12h
BL = 36h

No real refresh-disable path exists here.  SeaBIOS accepts this as a stub.
*/
static bool bios_10h_1236h(CPU* cpu)
{
    CPU_AL = 0x12;
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
    uint16_t flags_on_stack = readw86((CPU_SS << 4) + CPU_SP + 4);
    switch(CPU_AH) {
        case 0x00:
            bios_10h_00h(cpu); // SET VIDEO MODE
            break;
        case 0x01:
            bios_10h_01h(cpu); // SET CURSOR SHAPE
            break;
        case 0x02:
            bios_10h_02h(cpu); // SET CURSOR POSITION
            break;
        case 0x03:
            bios_10h_03h(cpu); // GET CURSOR POSITION AND SIZE
            break;
        case 0x04:
            bios_10h_04h(cpu); // READ LIGHT PEN POSITION
            break;
        case 0x05:
            bios_10h_05h(cpu); // SELECT ACTIVE DISPLAY PAGE
            break;
        case 0x06:
            bios_10h_scroll_window(cpu, false); // SCROLL WINDOW UP
            break;
        case 0x07:
            bios_10h_scroll_window(cpu, true); // SCROLL WINDOW DOWN
            break;
        case 0x08:
            bios_10h_08h(cpu); // READ CHARACTER AND ATTRIBUTE
            break;
        case 0x09:
            bios_10h_09h(cpu); // WRITE CHARACTER AND ATTRIBUTE
            break;
        case 0x0A:
            bios_10h_0Ah(cpu); // WRITE CHARACTER ONLY
            break;
        case 0x0B:
            bios_10h_0Bh(cpu); // SET BORDER/BACKGROUND OR CGA PALETTE
            break;
        case 0x0C:
            bios_10h_0Ch(cpu); // WRITE GRAPHICS PIXEL
            break;
        case 0x0D:
            bios_10h_0Dh(cpu); // READ GRAPHICS PIXEL
            break;
        case 0x0E:
            bios_10h_0Eh(cpu); // TELETYPE OUTPUT
            break;
        case 0x0F:
            bios_10h_0Fh(cpu); // GET CURRENT VIDEO MODE
            break;
        case 0x10:
            switch(CPU_AL) {
            case 0: bios_10h_1000h(cpu); // SET SINGLE PALETTE REGISTER
                break;
            case 1: bios_10h_1001h(cpu); // SET BORDER / OVERSCAN COLOR
                break;
            case 2: bios_10h_1002h(cpu); // SET ALL PALETTE REGISTERS
                break;
            case 3: bios_10h_1003h(cpu); // TOGGLE BLINK / BACKGROUND INTENSITY
                break;
            case 7: bios_10h_1007h(cpu); // READ SINGLE PALETTE REGISTER
                break;
            case 8: bios_10h_1008h(cpu); // READ BORDER / OVERSCAN COLOR
                break;
            case 9: bios_10h_1009h(cpu); // READ ALL PALETTE REGISTERS
                break;
#ifndef EGA128
            case 0x10: bios_10h_1010h(cpu); // SET INDIVIDUAL DAC REGISTER
                break;
            case 0x12: bios_10h_1012h(cpu); // SET BLOCK OF DAC REGISTERS
                break;
#ifndef MCGA
            case 0x13: bios_10h_1013h(cpu); // SELECT VIDEO DAC COLOR PAGE (VGA)
                break;
#endif
            case 0x15: bios_10h_1015h(cpu); // READ INDIVIDUAL DAC REGISTER
                break;
            case 0x17: bios_10h_1017h(cpu); // READ BLOCK OF DAC REGISTERS
                break;
            case 0x18: bios_10h_1018h(cpu); // SET PEL MASK
                break;
            case 0x19: bios_10h_1019h(cpu); // READ PEL MASK
                break;
#ifndef MCGA
            case 0x1A: bios_10h_101Ah(cpu); // GET VIDEO DAC COLOR PAGE STATE (VGA)
                break;
#endif
            case 0x1B: bios_10h_101Bh(cpu); // PERFORM GRAY-SCALE SUMMING
                break;
#endif
            default:
                goto err;
            }
            break;
        case 0x11:
            switch(CPU_AL) {
            case 0: bios_10h_1100h(cpu); // LOAD USER TEXT-MODE FONT
                break;
            case 1: bios_10h_1101h(cpu); // LOAD 8x14 ROM FONT
                break;
            case 2: bios_10h_1102h(cpu); // LOAD 8x8 ROM FONT
                break;
            case 4: bios_10h_1104h(cpu); // LOAD 8x16 ROM FONT
                break;
            case 0x10: bios_10h_1110h(cpu); // LOAD USER GRAPHICS FONT
                break;
            case 0x11: bios_10h_1111h(cpu); // LOAD ROM 8x14 GRAPHICS FONT
                break;
            case 0x12: bios_10h_1112h(cpu); // LOAD ROM 8x8 GRAPHICS FONT
                break;
            case 0x14: bios_10h_1114h(cpu); // LOAD ROM 8x16 GRAPHICS FONT
                break;
            case 0x20: bios_10h_1120h(cpu); // SET ALTERNATE PRINT-SCREEN CHARS
                break;
            case 0x21: bios_10h_1121h(cpu); // SET USER GRAPHICS CHARS
                break;
            case 0x22: bios_10h_1122h(cpu); // SET GRAPHICS 8x14
                break;
            case 0x23: bios_10h_1123h(cpu); // SET GRAPHICS 8x8 DOUBLE-DOT
                break;
            case 0x24: bios_10h_1124h(cpu); // SET GRAPHICS 8x16
                break;
            case 0x30: bios_10h_1130h(cpu); // GET FONT INFORMATION (EGA, MCGA, VGA)
                break;
            default:
                goto err;
            }
            break;
        case 0x12:
            switch(CPU_BL) {
#ifndef MCGA
            case 0x10: bios_10h_1210h(cpu); // GET EGA/VGA INFORMATION
                break;
#endif
            case 0x20: bios_10h_1220h(cpu); // ALTERNATE PRINT SCREEN
                break;
#ifndef EGA128
#ifndef MCGA
            case 0x30: bios_10h_1230h(cpu); // SELECT TEXT SCAN LINES
                break;
#endif
            case 0x31: bios_10h_1231h(cpu); // DEFAULT PALETTE LOADING
                break;
            case 0x32: bios_10h_1232h(cpu); // VIDEO ADDRESSING
                break;
            case 0x33: bios_10h_1233h(cpu); // GRAYSCALE SUMMING
                break;
            case 0x35: bios_10h_1235h(cpu); // DISPLAY SWITCH INTERFACE
                break;
#ifndef MCGA
            case 0x34: bios_10h_1234h(cpu); // CURSOR EMULATION
                break;
            case 0x36: bios_10h_1236h(cpu); // VIDEO REFRESH CONTROL
                break;
#endif
#endif
            default:
                goto err;
            }
            break;
        case 0x13:
            bios_10h_13h(cpu); // WRITE STRING
            break;
#ifndef EGA128
        case 0x1A:
            if (CPU_AL == 0x00)
                bios_10h_1A00h(cpu); // GET DISPLAY COMBINATION CODE
            else
                goto err;
            break;
        case 0x1B:
            if (CPU_AL == 0x00)
                bios_10h_1B00h(cpu); // GET FUNCTIONALITY/STATE INFORMATION
            else
                goto err;
            break;
#ifndef MCGA
        case 0x1C:
            bios_10h_1Ch(cpu); // SAVE/RESTORE VIDEO STATE
            break;
#endif
#endif
#if !defined(EGA128) && !defined(VGA128) && !defined(MCGA)
        case 0x4F:
            if (!bios_10h_4Fh(cpu))
                goto err;
            break;
#endif
        default:
            // unsupported
            goto err;
    }
    goto ok;
err:
    cf = 1; // unsuported unknown function
    flags_on_stack = (flags_on_stack & ~0x0041) // reset ZF, CF
                   | (cpu_getflags(cpu) & 0x0041); // set them back from CPU
    writew86((CPU_SS << 4) + CPU_SP + 4, flags_on_stack);
ok:
    return true;
}

#include "font8x8.h"
#include "font8x14.h"
#include "font8x16.h"

static inline uint8_t bios_10h_bitrev8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

void bios_10h_install_rom_fonts(CPU* cpu) // calling from load_bios_and_reset
{
    /* POST-значения видео-полей BDA (SeaBIOS vgainit.c): modeset_ctl -
       базовые опции 0x51, dcc_index - VGA color 0x08. Читаются
       библиотеками определения адаптера напрямую из BDA. */
    write86(BIOS10_BDA_MODESET_CTL, 0x51);
#ifdef EGA128
    write86(0x48A, BIOS10_DCC_EGA_COLOR);
#elif defined(MCGA)
    write86(0x48A, BIOS10_DCC_MCGA_COLOR_ANALOG);
#else
    write86(0x48A, BIOS10_DCC_VGA_COLOR_ANALOG);
#endif

    /*
     * INT 10h/AX=1130h must return a guest-visible ES:BP pointer.
     * Host pointers to font arrays are useless for DOS code,
     * so copy compact ROM font tables into emulated F000:xxxx area.
     */
    /*
     * The built-in font tables are not all stored in the same bit order.
     * font_8x8 is already in the order expected by the current text renderer,
     * while font_8x16 is the bit-reversed form of the legacy vgafont16 table
     * used by the working 25-line path.  Normalize only the 8x16 ROM image
     * here; do not reverse the 8x8 image used by the working 50-line mode.
     */
    for (uint32_t ch = 0; ch < 256; ch++) {
        for (uint32_t y = 0; y < 16; y++)
            pstore8(((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X16_OFF + ch * 16 + y,
                    bios_10h_bitrev8(font_8x16[ch * 16 + y]));

        for (uint8_t y = 0; y < 14; y++)
            pstore8(((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X14_OFF + ch * 14 + y,
                    font_8x14[ch * 14 + y]);

        for (uint8_t y = 0; y < 8; y++)
            pstore8(((uint32_t)BIOS_FONT_SEG << 4) + BIOS_FONT8X8_OFF + ch * 8 + y,
                    font_8x8[ch * 8 + y]);
    }
}

void vga_bios_baner(CPU* cpu)
{
    char banner[80];
    unsigned mhz = (unsigned)(clock_get_hz(clk_sys) / 1000000u);
    unsigned mv = current_vreg_mv;
#if I386_MODE
    const char *core = "PC em386 BIOS";
#else
    const char *core = "PC em286 BIOS";
#endif
    if (mv)
        snprintf(banner, sizeof(banner), "RP2350%c %u MHz %u.%02uV %s",
                 get_rp2350_package_letter(), mhz, mv / 1000u, (mv % 1000u) / 10u, core);
    else
        snprintf(banner, sizeof(banner), "RP2350%c %u MHz %s",
                 get_rp2350_package_letter(), mhz, core);

    const uint8_t attr = 0x61; // bg=yellow(6), fg=blue(1)
    const uint8_t row  = 0;
    const uint8_t cols = 80;
    uint8_t len = (uint8_t)strlen(banner);
    if (len > cols) len = cols;
    uint8_t col = (uint8_t)((cols - len) / 2);

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
