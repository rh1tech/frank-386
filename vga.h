#ifndef VGA_H
#define VGA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct FBDevice FBDevice;

typedef void SimpleFBDrawFunc(void *opaque,
                              int x, int y, int w, int h);

typedef struct VGAState VGAState;
VGAState *vga_init(char *vga_ram, int vga_ram_size,
                   uint8_t *fb, int width, int height);
void vga_set_force_8dm(VGAState *s, int v);

int vga_step(VGAState *vga);
void vga_refresh(VGAState *s,
                 SimpleFBDrawFunc *redraw_func, void *opaque, int full_update);

void vga_ioport_write(VGAState *s, uint32_t addr, uint32_t val);
uint32_t vga_ioport_read(VGAState *s, uint32_t addr);

void vbe_write(VGAState *s, uint32_t offset, uint32_t val);
uint32_t vbe_read(VGAState *s, uint32_t offset);

void vga_mem_write(VGAState *s, uint32_t addr, uint8_t val);
uint8_t vga_mem_read(VGAState *s, uint32_t addr);
void vga_mem_write16(VGAState *s, uint32_t addr, uint16_t val);
void vga_mem_write32(VGAState *s, uint32_t addr, uint32_t val);
bool vga_mem_write_string(VGAState *s, uint32_t addr, uint8_t *buf, int len);

typedef struct PCIDevice PCIDevice;
typedef struct PCIBus PCIBus;
PCIDevice *vga_pci_init(VGAState *s, PCIBus *bus,
                        void *o, void (*set_bar)(void *, int, uint32_t, bool));

// Accessor functions for hardware VGA driver integration
int vga_get_mode(VGAState *s);           // 0=blank, 1=text, 2=graphics
uint16_t vga_get_start_addr(VGAState *s); // Start address in VGA memory
uint8_t vga_get_panning(VGAState *s);    // Horizontal pixel panning (0-7)
void vga_get_cursor(VGAState *s, int *x, int *y, int *start, int *end);
void vga_get_cursor_info(VGAState *s, int *x, int *y, int *start, int *end, int *visible);
const uint8_t *vga_get_palette(VGAState *s);  // 768-byte RGB palette (256 x 3)
void vga_get_palette16(VGAState *s, uint8_t *palette16);  // 48-byte EGA palette (16 x 3)
int vga_get_graphics_mode(VGAState *s, int *width, int *height);  // Returns: 0=text, 1=CGA4, 2=EGA, 3=VGA256, 4=CGA2
int vga_get_line_offset(VGAState *s);    // Line offset in words
int vga_get_line_compare(VGAState *s);   // Scanline where address resets to 0
bool vga_in_retrace(VGAState *s);        // Check if in vertical retrace
int vga_get_cursor_blink_phase(VGAState *s);  // Cursor blink phase (1=visible, 0=hidden)
int vga_get_char_height(VGAState *s);         // Character cell height (typically 8 or 16)

#ifndef BPP
#define BPP 32
#endif

#endif /* VGA_H */
