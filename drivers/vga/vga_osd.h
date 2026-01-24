/**
 * VGA OSD (On-Screen Display) Overlay for murm386
 *
 * Simple text-based overlay that renders on top of the main VGA output.
 * Used for the disk manager UI and other system menus.
 */

#ifndef VGA_OSD_H
#define VGA_OSD_H

#include <stdint.h>
#include <stdbool.h>

// OSD dimensions (full screen, reuses VGA text buffer)
#define OSD_COLS 80
#define OSD_ROWS 25

// OSD colors (CGA-style 4-bit: IRGB for foreground, RGB for background)
#define OSD_BLACK       0x00
#define OSD_BLUE        0x01
#define OSD_GREEN       0x02
#define OSD_CYAN        0x03
#define OSD_RED         0x04
#define OSD_MAGENTA     0x05
#define OSD_BROWN       0x06
#define OSD_LIGHTGRAY   0x07
#define OSD_DARKGRAY    0x08
#define OSD_LIGHTBLUE   0x09
#define OSD_LIGHTGREEN  0x0A
#define OSD_LIGHTCYAN   0x0B
#define OSD_LIGHTRED    0x0C
#define OSD_LIGHTMAGENTA 0x0D
#define OSD_YELLOW      0x0E
#define OSD_WHITE       0x0F

// Make attribute byte from foreground and background colors
#define OSD_ATTR(fg, bg) (((bg) << 4) | (fg))

// Common color schemes
#define OSD_ATTR_NORMAL     OSD_ATTR(OSD_WHITE, OSD_BLUE)
#define OSD_ATTR_HIGHLIGHT  OSD_ATTR(OSD_YELLOW, OSD_BLUE)
#define OSD_ATTR_SELECTED   OSD_ATTR(OSD_BLACK, OSD_CYAN)
#define OSD_ATTR_TITLE      OSD_ATTR(OSD_WHITE, OSD_RED)
#define OSD_ATTR_BORDER     OSD_ATTR(OSD_LIGHTCYAN, OSD_BLUE)
#define OSD_ATTR_DISABLED   OSD_ATTR(OSD_DARKGRAY, OSD_BLUE)

// Initialize OSD system
void osd_init(void);

// Show/hide OSD overlay
void osd_show(void);
void osd_hide(void);
bool osd_is_visible(void);

// Clear OSD buffer
void osd_clear(void);

// Set character at position with attribute
void osd_putchar(int x, int y, char ch, uint8_t attr);

// Print string at position with attribute
void osd_print(int x, int y, const char *str, uint8_t attr);

// Print string centered horizontally at row y
void osd_print_center(int y, const char *str, uint8_t attr);

// Draw a box with single-line border
// x, y = top-left corner, w = width, h = height
void osd_draw_box(int x, int y, int w, int h, uint8_t attr);

// Draw a box with title in the top border
void osd_draw_box_titled(int x, int y, int w, int h, const char *title, uint8_t attr);

// Fill a rectangular region
void osd_fill(int x, int y, int w, int h, char ch, uint8_t attr);

// Get pointer to OSD buffer (for direct manipulation)
// Buffer is OSD_COLS * OSD_ROWS * 2 bytes (char + attr pairs)
uint8_t *osd_get_buffer(void);

// Render OSD overlay to VGA output
// Called from VGA driver during scanline rendering
void osd_render_line(uint32_t line, uint32_t *output_buffer);

#endif // VGA_OSD_H
