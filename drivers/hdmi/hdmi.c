#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdalign.h>
#include <hardware/dma.h>
#include <hardware/pio.h>
#include <pico/time.h>
#include <pico/multicore.h>
#include <hardware/clocks.h>
#include "vga.h"
#include "hdmi.h"
#include "font8x16.h"

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

#define GFX_BUFFER_SIZE (256 * 1024)
extern uint8_t gfx_buffer[GFX_BUFFER_SIZE];
extern volatile int text_cols;
// Stride in *character cells* (uint32_t per cell in gfx_buffer text layout).
// For VGA CRTC Offset (0x13): cells_per_row = cr13 * 2 (80-col -> 40*2, 40-col -> 20*2).
extern volatile int text_stride_cells;
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

void vga_hw_new_frame(void); // vsync

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
static uint32_t* __scratch_y("hdmi_ptr_3") dma_lines[2] = { NULL,NULL };
static uint32_t* __scratch_y("hdmi_ptr_4") DMA_BUF_ADDR[2];

//ДМА палитра для конвертации
//в хвосте этой памяти выделяется dma_data
alignas(4096) uint32_t conv_color[1224];

//индекс, проверяющий зависание
static uint32_t irq_inx = 0;

//функции и константы HDMI

#define BASE_HDMI_CTRL_INX (251)
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

static uint64_t get_ser_diff_data(const uint16_t dataR, const uint16_t dataG, const uint16_t dataB) {
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
static uint tmds_encoder(const uint8_t d8) {
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
            register uint8_t glyph = font_8x16[ch * 16 + glyph_line];
            if (cursor_blink_state && col == cursor_x &&
                char_row == (uint32_t)cursor_y &&
                glyph_line >= (uint32_t)cursor_start &&
                glyph_line <= (uint32_t)cursor_end) {
                glyph = 0xFF;
            } else {
                glyph = font_8x16[ch * 16 + glyph_line];
            }
           // uint8_t blink_or_highlite_bg = attr & 0b10000000; // TODO: use it?
            register uint8_t fg_color0 = attr & 0b00001111;
            register uint8_t bg_color1 = attr & 0b01110000;
            register uint8_t bg_color0 = bg_color1 >> 4;
            register uint8_t fg_color1 = fg_color0 << 4;
            if (!double_h) {
                *output_buffer++ = ((glyph & 0b00000001) ? fg_color1 : bg_color1) | ((glyph & 0b00000010) ? fg_color0 : bg_color0);
                *output_buffer++ = ((glyph & 0b00000100) ? fg_color1 : bg_color1) | ((glyph & 0b00001000) ? fg_color0 : bg_color0);
                *output_buffer++ = ((glyph & 0b00010000) ? fg_color1 : bg_color1) | ((glyph & 0b00100000) ? fg_color0 : bg_color0);
                *output_buffer++ = ((glyph & 0b01000000) ? fg_color1 : bg_color1) | ((glyph & 0b10000000) ? fg_color0 : bg_color0);
            } else {
                *output_buffer++ = ((glyph & 0b00000001) ? fg_color1 : bg_color1) | ((glyph & 0b00000001) ? fg_color0 : bg_color0);
                *output_buffer++ = ((glyph & 0b00000010) ? fg_color1 : bg_color1) | ((glyph & 0b00000010) ? fg_color0 : bg_color0);
                *output_buffer++ = ((glyph & 0b00000100) ? fg_color1 : bg_color1) | ((glyph & 0b00000100) ? fg_color0 : bg_color0);
                *output_buffer++ = ((glyph & 0b00001000) ? fg_color1 : bg_color1) | ((glyph & 0b00001000) ? fg_color0 : bg_color0);
                *output_buffer++ = ((glyph & 0b00010000) ? fg_color1 : bg_color1) | ((glyph & 0b00010000) ? fg_color0 : bg_color0);
                *output_buffer++ = ((glyph & 0b00100000) ? fg_color1 : bg_color1) | ((glyph & 0b00100000) ? fg_color0 : bg_color0);
                *output_buffer++ = ((glyph & 0b01000000) ? fg_color1 : bg_color1) | ((glyph & 0b01000000) ? fg_color0 : bg_color0);
                *output_buffer++ = ((glyph & 0b10000000) ? fg_color1 : bg_color1) | ((glyph & 0b10000000) ? fg_color0 : bg_color0);
            }
        }
    }
}

void pre_render_line(void);
static void __time_critical_func(render_line)(uint32_t line, uint8_t *output_buffer) {
    pre_render_line();
    if (current_mode == 1) {
        // Text mode now rendered from linear framebuffer
        render_text_line(line, output_buffer);
        return;
    }
    if (!(line & 1)) return; // повторяем чётные строки на нечётных
    int y = line >> 1;
    uint8_t* input_buffer = gfx_buffer + y * SCREEN_WIDTH;
    uint8_t* activ_buf_end = output_buffer + SCREEN_WIDTH;
    //рисуем видеобуфер
    const uint8_t* input_buffer_end = input_buffer + SCREEN_WIDTH;
    register size_t x = 0;
    while (activ_buf_end > output_buffer) {
        if (input_buffer < input_buffer_end) {
            register uint8_t c = input_buffer[x++];
            *output_buffer++ = c >= BASE_HDMI_CTRL_INX ? 255 : c;
        }
        else {
            *output_buffer++ = 255;
        }
    }
}

static void __scratch_y("hdmi_driver") dma_handler_HDMI() {
    static uint32_t inx_buf_dma;
    static uint line = 0;
    irq_inx++;

    dma_hw->ints0 = 1u << dma_chan_ctrl;
    dma_channel_set_read_addr(dma_chan_ctrl, &DMA_BUF_ADDR[inx_buf_dma & 1], false);

    if (line++ > 524) {
        line = 0;
        vga_hw_new_frame();
    }

    inx_buf_dma++;

    uint8_t* activ_buf = (uint8_t *)dma_lines[inx_buf_dma & 1];

    if (line < 480) { //область изображения
        uint8_t* output_buffer = activ_buf + 72; //для выравнивания синхры;

        // --- Верхнее поле ---
        if (line < (uint32_t)active_start) {
            nf_memset(output_buffer, 255, SCREEN_WIDTH);
            goto f;
        }
        // --- Нижнее поле ---
        if (line >= (uint32_t)active_end) {
            nf_memset(output_buffer, 255, SCREEN_WIDTH);
            goto f;
        }
        render_line(line - active_start, output_buffer);
f:
        //ССИ
        //для выравнивания синхры
        // --|_|---|_|---|_|----
        //---|___________|-----
        nf_memset(activ_buf + 48,BASE_HDMI_CTRL_INX, 24);
        nf_memset(activ_buf,BASE_HDMI_CTRL_INX + 1, 48);
        nf_memset(activ_buf + 392,BASE_HDMI_CTRL_INX, 8);
    }
    else {
        if ((line >= 490) && (line < 492)) {
            //кадровый синхроимпульс
            //для выравнивания синхры
            // --|_|---|_|---|_|----
            //---|___________|-----
            nf_memset(activ_buf + 48,BASE_HDMI_CTRL_INX + 2, 352);
            nf_memset(activ_buf,BASE_HDMI_CTRL_INX + 3, 48);
        }
        else {
            //ССИ без изображения
            //для выравнивания синхры
            nf_memset(activ_buf + 48,BASE_HDMI_CTRL_INX, 352);
            nf_memset(activ_buf,BASE_HDMI_CTRL_INX + 1, 48);
        };

        // Line N_LINES_TOTAL-4 (521): late in vblank, just before DMA needs line 0.
        // Wolf3D has already written the new page address to CRTC by now.
        // Read cr[] and ar[] directly — no intermediate volatile copies.
        if (line == 521) {
            if (vga_state) {
                const uint8_t *cr = vga_state->cr;
                frame_vram_offset = (uint16_t)((cr[0x0c] << 8) | cr[0x0d]);
                frame_pixel_panning = vga_state->ar[0x13] & 0x07;
                int lc = (int)cr[0x18]
                       | (((int)cr[0x07] & 0x10) << 4)
                       | (((int)cr[0x09] & 0x40) << 3);
                frame_line_compare = (lc > 0 && lc < 480) ? lc : -1;
            }
        }
    }
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

void graphics_set_palette_hdmi(const uint8_t R, const uint8_t G, const uint8_t B,  uint8_t i);
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
    static const uint8_t cga_colors[16][3] = {
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
            // 6-бит (0-63) → 8-бит (0-255)
            graphics_set_palette_hdmi2(
                c13[0] << 2, c13[1] << 2, c13[2] << 2,
                c23[0] << 2, c23[1] << 2, c23[2] << 2,
                ci
            );
        }
    }

    //255 - цвет фона
    graphics_set_palette_hdmi(0, 0, 0, 255);


    //BASE_HDMI_CTRL_INX +3 служебные данные(синхра) напрямую вносим в массив -конвертер
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint16_t b0 = 0b1101010100;
    const uint16_t b1 = 0b0010101011;
    const uint16_t b2 = 0b0101010100;
    const uint16_t b3 = 0b1010101011;
    const int base_inx = BASE_HDMI_CTRL_INX;

    conv_color64[2 * base_inx + 0] = get_ser_diff_data(b0, b0, b3);
    conv_color64[2 * base_inx + 1] = get_ser_diff_data(b0, b0, b3);

    conv_color64[2 * (base_inx + 1) + 0] = get_ser_diff_data(b0, b0, b2);
    conv_color64[2 * (base_inx + 1) + 1] = get_ser_diff_data(b0, b0, b2);

    conv_color64[2 * (base_inx + 2) + 0] = get_ser_diff_data(b0, b0, b1);
    conv_color64[2 * (base_inx + 2) + 1] = get_ser_diff_data(b0, b0, b1);

    conv_color64[2 * (base_inx + 3) + 0] = get_ser_diff_data(b0, b0, b0);
    conv_color64[2 * (base_inx + 3) + 1] = get_ser_diff_data(b0, b0, b0);

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

#if ZERO2
    // Настройка направлений пинов для state machines
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_video, HDMI_BASE_PIN, 8, true);
    pio_sm_set_consecutive_pindirs(PIO_VIDEO_ADDR, SM_conv, HDMI_BASE_PIN, 8, true);

    uint64_t mask64 = (uint64_t)(3u << beginHDMI_PIN_clk);
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

    //настройки DMA
    dma_lines[0] = &conv_color[1024];
    dma_lines[1] = &conv_color[1124];

    //основной рабочий канал
    dma_channel_config cfg_dma = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&cfg_dma, dma_chan_ctrl); // chain to other channel

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
    channel_config_set_chain_to(&cfg_dma, dma_chan); // chain to other channel

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    DMA_BUF_ADDR[0] = &dma_lines[0][0];
    DMA_BUF_ADDR[1] = &dma_lines[1][0];

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
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv_ctrl); // chain to other channel

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

    dma_start_channel_mask((1u << dma_chan_ctrl));

    return true;
};

void graphics_set_palette_hdmi(const uint8_t R, const uint8_t G, const uint8_t B,  uint8_t i) {
    if ((i >= BASE_HDMI_CTRL_INX) && (i != 255)) return; //не записываем "служебные" цвета
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    conv_color64[i * 2] = get_ser_diff_data(tmds_encoder(R), tmds_encoder(G), tmds_encoder(B));
    conv_color64[i * 2 + 1] = conv_color64[i * 2] ^ 0x0003ffffffffffffl;
};

void graphics_set_palette_hdmi2(
    const uint8_t R1, const uint8_t G1, const uint8_t B1,
    const uint8_t R2, const uint8_t G2, const uint8_t B2,
    uint8_t i
) {
    if ((i >= BASE_HDMI_CTRL_INX) && (i != 255)) return;
    uint64_t* conv_color64 = (uint64_t*)conv_color;
    uint64_t c1 = get_ser_diff_data(tmds_encoder(R1), tmds_encoder(G1), tmds_encoder(B1));
    uint64_t c2 = get_ser_diff_data(tmds_encoder(R2), tmds_encoder(G2), tmds_encoder(B2));
    conv_color64[i * 2]     = c1;
    conv_color64[i * 2 + 1] = (c1 == c2) ? (c2 ^ 0x0003ffffffffffffl) : c2;
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

    hdmi_init();
}
