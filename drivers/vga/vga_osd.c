/**
 * VGA OSD (On-Screen Display) Overlay for murm386
 *
 * Implements a simple text overlay that renders on top of the main VGA output.
 */

#include "vga_osd.h"
#include "font8x16.h"
#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"  // For __time_critical_func

// OSD reuses the VGA text buffer (emulation is paused when OSD is visible)
// This is defined in vga_hw.c - we access it via extern
extern uint8_t text_buffer_sram[80 * 25 * 2];
#define osd_buffer text_buffer_sram

// OSD visibility flag
static bool osd_visible = false;

// Box drawing characters (CP437)
#define BOX_TL  0xDA  // Top-left corner
#define BOX_TR  0xBF  // Top-right corner
#define BOX_BL  0xC0  // Bottom-left corner
#define BOX_BR  0xD9  // Bottom-right corner
#define BOX_H   0xC4  // Horizontal line
#define BOX_V   0xB3  // Vertical line

// Sync encoding from vga_hw.c
#define TMPL_LINE 0xC0

// CGA colors (same as vga_hw.c)
static const uint8_t cga_colors[16] = {
    0x00,  // 0: Black
    0x02,  // 1: Blue
    0x08,  // 2: Green
    0x0A,  // 3: Cyan
    0x20,  // 4: Red
    0x22,  // 5: Magenta
    0x28,  // 6: Brown
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

// Convert CGA color index to 8-bit VGA output (RRGGBB + sync)
static uint8_t color_to_output(uint8_t color_idx) {
    return cga_colors[color_idx & 0x0F] | TMPL_LINE;
}

void osd_init(void) {
    osd_clear();
    osd_visible = false;
}

void osd_show(void) {
    osd_visible = true;
}

void osd_hide(void) {
    osd_visible = false;
}

bool osd_is_visible(void) {
    return osd_visible;
}

void osd_clear(void) {
    // Fill with spaces and default attribute
    for (int i = 0; i < OSD_COLS * OSD_ROWS; i++) {
        osd_buffer[i * 2] = ' ';
        osd_buffer[i * 2 + 1] = OSD_ATTR_NORMAL;
    }
}

void osd_putchar(int x, int y, char ch, uint8_t attr) {
    if (x < 0 || x >= OSD_COLS || y < 0 || y >= OSD_ROWS) return;

    int idx = (y * OSD_COLS + x) * 2;
    osd_buffer[idx] = (uint8_t)ch;
    osd_buffer[idx + 1] = attr;
}

void osd_print(int x, int y, const char *str, uint8_t attr) {
    while (*str && x < OSD_COLS) {
        osd_putchar(x++, y, *str++, attr);
    }
}

void osd_print_center(int y, const char *str, uint8_t attr) {
    int len = strlen(str);
    int x = (OSD_COLS - len) / 2;
    if (x < 0) x = 0;
    osd_print(x, y, str, attr);
}

void osd_draw_box(int x, int y, int w, int h, uint8_t attr) {
    if (w < 2 || h < 2) return;

    // Corners
    osd_putchar(x, y, BOX_TL, attr);
    osd_putchar(x + w - 1, y, BOX_TR, attr);
    osd_putchar(x, y + h - 1, BOX_BL, attr);
    osd_putchar(x + w - 1, y + h - 1, BOX_BR, attr);

    // Top and bottom edges
    for (int i = 1; i < w - 1; i++) {
        osd_putchar(x + i, y, BOX_H, attr);
        osd_putchar(x + i, y + h - 1, BOX_H, attr);
    }

    // Left and right edges
    for (int i = 1; i < h - 1; i++) {
        osd_putchar(x, y + i, BOX_V, attr);
        osd_putchar(x + w - 1, y + i, BOX_V, attr);
    }
}

void osd_draw_box_titled(int x, int y, int w, int h, const char *title, uint8_t attr) {
    osd_draw_box(x, y, w, h, attr);

    // Draw title centered in top border
    int title_len = strlen(title);
    int title_x = x + (w - title_len - 4) / 2;
    if (title_x < x + 1) title_x = x + 1;

    osd_putchar(title_x, y, ' ', attr);
    osd_print(title_x + 1, y, title, attr);
    osd_putchar(title_x + 1 + title_len, y, ' ', attr);
}

void osd_fill(int x, int y, int w, int h, char ch, uint8_t attr) {
    for (int row = y; row < y + h && row < OSD_ROWS; row++) {
        for (int col = x; col < x + w && col < OSD_COLS; col++) {
            osd_putchar(col, row, ch, attr);
        }
    }
}

uint8_t *osd_get_buffer(void) {
    return osd_buffer;
}

// Render OSD overlay onto a scanline
// This is called from the VGA ISR, so it must be fast
void __time_critical_func(osd_render_line)(uint32_t line, uint32_t *output_buffer) {
    if (!osd_visible) return;

    // VGA output is 640x400, text mode is 80x25 with 8x16 font
    // So each character row is 16 scanlines
    uint32_t char_row = line / 16;
    uint32_t glyph_line = line & 15;

    if (char_row >= OSD_ROWS) return;

    // Get pointer to this row in OSD buffer (reuses text_buffer_sram)
    uint8_t *row_data = &osd_buffer[char_row * OSD_COLS * 2];

    // Output starts at SHIFT_PICTURE offset (138 pixels)
    uint8_t *out = (uint8_t *)output_buffer + 138;

    // Render each character
    // Bit order matches render_text_line: bits 1,0 are leftmost pair, etc.
    for (int col = 0; col < OSD_COLS; col++) {
        uint8_t ch = row_data[col * 2];
        uint8_t attr = row_data[col * 2 + 1];

        // Get glyph data for this scanline
        uint8_t glyph = font_8x16[ch * 16 + glyph_line];

        // Get foreground and background colors
        uint8_t fg = color_to_output(attr & 0x0F);
        uint8_t bg = color_to_output((attr >> 4) & 0x0F);

        // Render 8 pixels - bit order: 0,1,2,3,4,5,6,7 (LSB to MSB)
        *out++ = (glyph & 0x01) ? fg : bg;  // bit 0 (leftmost)
        *out++ = (glyph & 0x02) ? fg : bg;  // bit 1
        *out++ = (glyph & 0x04) ? fg : bg;  // bit 2
        *out++ = (glyph & 0x08) ? fg : bg;  // bit 3
        *out++ = (glyph & 0x10) ? fg : bg;  // bit 4
        *out++ = (glyph & 0x20) ? fg : bg;  // bit 5
        *out++ = (glyph & 0x40) ? fg : bg;  // bit 6
        *out++ = (glyph & 0x80) ? fg : bg;  // bit 7 (rightmost)
    }
}
