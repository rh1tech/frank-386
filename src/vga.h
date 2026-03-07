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
int vga_get_text_cols(VGAState *s);      // Visible text columns (40/80)
void vga_get_cursor(VGAState *s, int *x, int *y, int *start, int *end);
void vga_get_cursor_info(VGAState *s, int *x, int *y, int *start, int *end, int *visible);
const uint8_t *vga_get_palette(VGAState *s);  // 768-byte RGB palette (256 x 3)
int vga_is_palette_dirty(VGAState *s);        // Check and clear palette dirty flag
void vga_get_palette16(VGAState *s, uint8_t *palette16);  // 48-byte EGA palette (16 x 3)
int vga_get_graphics_mode(VGAState *s, int *width, int *height);  // Returns: 0=text, 1=CGA4, 2=EGA, 3=VGA256, 4=CGA2
int vga_get_line_offset(VGAState *s);    // Line offset in words
int vga_get_line_compare(VGAState *s);   // Scanline where address resets to 0
bool vga_in_retrace(VGAState *s);        // Check if in vertical retrace
int vga_get_cursor_blink_phase(VGAState *s);  // Cursor blink phase (1=visible, 0=hidden)
int vga_get_char_height(VGAState *s);         // Character cell height (typically 8 or 16)
const uint8_t* vga_get_font_ptr(VGAState *s, uint8_t ch, int attr_bit3);

#ifndef BPP
#define BPP 32
#endif

//#define DEBUG_VBE
//#define DEBUG_VGA_REG

#define MSR_COLOR_EMULATION 0x01
#define MSR_PAGE_SELECT     0x20

#define ST01_V_RETRACE      0x08
#define ST01_DISP_ENABLE    0x01

#define VBE_DISPI_INDEX_ID              0x0
#define VBE_DISPI_INDEX_XRES            0x1
#define VBE_DISPI_INDEX_YRES            0x2
#define VBE_DISPI_INDEX_BPP             0x3
#define VBE_DISPI_INDEX_ENABLE          0x4
#define VBE_DISPI_INDEX_BANK            0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH      0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT     0x7
#define VBE_DISPI_INDEX_X_OFFSET        0x8
#define VBE_DISPI_INDEX_Y_OFFSET        0x9
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0xa
#define VBE_DISPI_INDEX_NB              0xb

#define VBE_DISPI_ID0                   0xB0C0
#define VBE_DISPI_ID1                   0xB0C1
#define VBE_DISPI_ID2                   0xB0C2
#define VBE_DISPI_ID3                   0xB0C3
#define VBE_DISPI_ID4                   0xB0C4
#define VBE_DISPI_ID5                   0xB0C5

#define VBE_DISPI_DISABLED              0x00
#define VBE_DISPI_ENABLED               0x01
#define VBE_DISPI_GETCAPS               0x02
#define VBE_DISPI_8BIT_DAC              0x20
#define VBE_DISPI_LFB_ENABLED           0x40
#define VBE_DISPI_NOCLEARMEM            0x80

#define FB_ALLOC_ALIGN (1 << 20)

#define MAX_TEXT_WIDTH 132
#define MAX_TEXT_HEIGHT 60

struct FBDevice {
    /* the following is set by the device */
    int width;
    int height;
    int stride; /* current stride in bytes */
    uint8_t *fb_data; /* current pointer to the pixel data */
};

struct VGAState {
    FBDevice *fb_dev;
    int graphic_mode;
    uint32_t cursor_blink_time;
    int cursor_visible_phase;
    uint32_t retrace_time;
    int retrace_phase;
    int force_8dm;

    uint8_t *vga_ram;
    int vga_ram_size;
    
    uint8_t sr_index;
    uint8_t sr[8];
    uint8_t gr_index;
    uint8_t gr[16];
    uint8_t ar_index;
    uint8_t ar[21];
    int ar_flip_flop;
    uint8_t cr_index;
    uint8_t cr[256]; /* CRT registers */
    uint8_t msr; /* Misc Output Register */
    uint8_t fcr; /* Feature Control Register */
    uint8_t st00; /* status 0 */
    uint8_t st01; /* status 1 */
    uint8_t dac_state;
    uint8_t dac_sub_index;
    uint8_t dac_read_index;
    uint8_t dac_write_index;
    uint8_t dac_8bit;
    uint8_t dac_cache[3]; /* used when writing */
    uint8_t palette[768];
    int palette_dirty;    /* set when palette is modified */
    int32_t bank_offset;

    uint32_t latch;
    
    /* text mode state */
    uint32_t last_palette[16];
#ifndef FULL_UPDATE
    uint16_t last_ch_attr[MAX_TEXT_WIDTH * MAX_TEXT_HEIGHT];
#endif
    uint32_t last_width;
    uint32_t last_height;
    uint16_t last_line_offset;
    uint16_t last_start_addr;
    uint16_t last_cursor_offset;
    uint8_t last_cursor_start;
    uint8_t last_cursor_end;

    /* VBE extension */
    uint16_t vbe_index;
    uint16_t vbe_regs[VBE_DISPI_INDEX_NB];
    uint32_t vbe_start_addr;
    uint32_t vbe_line_offset;

#if defined(SCALE_3_2) || defined(SWAPXY)
#ifndef LCD_WIDTH
#define LCD_WIDTH 2048
#endif
    uint8_t tmpbuf[(LCD_WIDTH > 720 ? LCD_WIDTH : 720) * 3 * 2];
#endif
};

uint32_t get_uticks();


#endif /* VGA_H */
