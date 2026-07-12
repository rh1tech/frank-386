/*
 * INT 33h - Microsoft Mouse driver (native implementation)
 *
 * В native-режиме в гостя не загружается никакой DOS-драйвер мыши, поэтому
 * INT 33h реализован прямо в эмуляторе поверх хостовой мыши (PS/2 / USB / NES).
 *
 * Что важно для детекторов (Norton SysInfo, CheckIt, MSD):
 *   - вектор INT 33h НЕ должен указывать на IRET и не должен быть нулевым,
 *     если драйвер есть (проверяется в pc.c / bios_post());
 *   - AX=0000h должен вернуть AX=FFFFh (драйвер установлен) и BX=число кнопок;
 *   - AX=0024h возвращает версию, тип (CH=04h => PS/2) и IRQ (CL=00h для PS/2).
 *
 * Курсор: программный, только текстовые режимы (BDA 40:49 = 0..3, 7).
 * В графических режимах курсор не рисуется (позиция всё равно отслеживается,
 * функции 3/5/6/0Bh работают).
 *
 * Обработчики событий (AX=000Ch/0014h) сохраняются, но НЕ вызываются:
 * far-call в гостя из нативного хендлера здесь не делается. Подавляющее
 * большинство DOS-программ (включая SysInfo, NC, Norton) опрашивают функцию 3.
 */

#include <string.h>
#include "286/cpu.h"
#include "bios.h"

#define BDA_VIDEO_MODE   0x449u
#define BDA_VIDEO_COLS   0x44Au
#define BDA_VIDEO_ROWS   0x484u   /* rows - 1 */
#define BDA_VIDEO_PAGE   0x462u
#define BDA_VIDEO_PGSIZE 0x44Cu

#define MOUSE_BUTTONS    2        /* сообщаем 2 кнопки (PS/2 стандарт) */

/* Виртуальное разрешение MS Mouse: всегда 640x200 в текстовых режимах */
#define VIRT_W  640
#define VIRT_H  200

typedef struct {
    int      installed;

    int      x, y;              /* виртуальные координаты (0..639 / 0..199) */
    int      minx, maxx;
    int      miny, maxy;

    uint8_t  buttons;           /* bit0=left, bit1=right, bit2=middle */

    /* счётчики нажатий/отпусканий (функции 5/6) */
    uint16_t press_cnt[3];
    uint16_t press_x[3], press_y[3];
    uint16_t rel_cnt[3];
    uint16_t rel_x[3], rel_y[3];

    int      mickey_x, mickey_y;  /* аккумулятор для функции 0Bh */
    int      frac_x, frac_y;      /* остаток mickeys при пересчёте в пиксели */

    int      hide_count;          /* MS-семантика: курсор виден только при ==0 */

    uint16_t scr_mask;            /* текстовый курсор: AND-маска */
    uint16_t cur_mask;            /* текстовый курсор: XOR-маска */

    int      mpp_x, mpp_y;        /* mickeys per 8 pixels (функция 0Fh) */
    uint8_t  sens_x, sens_y, sens_d;

    /* сохранённая ячейка под курсором */
    int      drawn;
    uint32_t drawn_addr;
    uint16_t drawn_cell;

    /* пользовательский обработчик (не вызывается, только хранится) */
    uint16_t cb_mask;
    uint16_t cb_seg, cb_off;
} MouseState;

static MouseState m;

/* ------------------------------------------------------------------ */
/* Текстовый курсор                                                    */
/* ------------------------------------------------------------------ */

static int text_mode_base(uint32_t *base, int *cols, int *rows)
{
    uint8_t mode = pload8(BDA_VIDEO_MODE);

    if (mode == 0x07) {
        *base = 0xB0000u;
    } else if (mode <= 0x03) {
        *base = 0xB8000u;
    } else {
        return 0;   /* графика — курсор не рисуем */
    }

    *cols = pload16(BDA_VIDEO_COLS);
    if (*cols <= 0 || *cols > 132) *cols = 80;
    *rows = pload8(BDA_VIDEO_ROWS) + 1;
    if (*rows <= 0 || *rows > 60) *rows = 25;

    /* активная страница */
    uint16_t pgsize = pload16(BDA_VIDEO_PGSIZE);
    uint8_t  page   = pload8(BDA_VIDEO_PAGE);
    if (pgsize == 0) pgsize = 0x1000;
    *base += (uint32_t)page * pgsize;
    return 1;
}

static void cursor_erase(void)
{
    if (!m.drawn)
        return;
    pstore16(m.drawn_addr, m.drawn_cell);
    m.drawn = 0;
}

static void cursor_draw(void)
{
    uint32_t base;
    int cols, rows;

    if (m.drawn || m.hide_count != 0)
        return;
    if (!text_mode_base(&base, &cols, &rows))
        return;

    int cx = m.x >> 3;
    int cy = m.y >> 3;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= cols) cx = cols - 1;
    if (cy >= rows) cy = rows - 1;

    uint32_t addr = base + (uint32_t)(cy * cols + cx) * 2u;
    uint16_t cell = pload16(addr);

    m.drawn_addr = addr;
    m.drawn_cell = cell;
    m.drawn      = 1;

    pstore16(addr, (uint16_t)((cell & m.scr_mask) ^ m.cur_mask));
}

static void cursor_refresh(void)
{
    cursor_erase();
    cursor_draw();
}

/* ------------------------------------------------------------------ */
/* Хук от хостовой мыши (вызывается из main.c рядом с ps2_mouse_event) */
/* dx/dy — в экранных координатах: +x вправо, +y вниз                  */
/* buttons — bit0=left, bit1=right, bit2=middle                        */
/* ------------------------------------------------------------------ */
void bios_33h_mouse_event(int dx, int dy, int buttons)
{
    if (!m.installed)
        return;

    m.mickey_x += dx;
    m.mickey_y += dy;

    /* mickeys -> пиксели: mpp_x mickeys на 8 пикселей */
    m.frac_x += dx * 8;
    m.frac_y += dy * 8;

    int px = m.frac_x / (m.mpp_x ? m.mpp_x : 8);
    int py = m.frac_y / (m.mpp_y ? m.mpp_y : 16);
    m.frac_x -= px * (m.mpp_x ? m.mpp_x : 8);
    m.frac_y -= py * (m.mpp_y ? m.mpp_y : 16);

    if (px || py) {
        m.x += px;
        m.y += py;
        if (m.x < m.minx) m.x = m.minx;
        if (m.x > m.maxx) m.x = m.maxx;
        if (m.y < m.miny) m.y = m.miny;
        if (m.y > m.maxy) m.y = m.maxy;
        cursor_refresh();
    }

    uint8_t nb  = (uint8_t)(buttons & 7);
    uint8_t old = m.buttons;
    for (int b = 0; b < 3; b++) {
        uint8_t bit = (uint8_t)(1u << b);
        if ((nb & bit) && !(old & bit)) {
            m.press_cnt[b]++;
            m.press_x[b] = (uint16_t)m.x;
            m.press_y[b] = (uint16_t)m.y;
        } else if (!(nb & bit) && (old & bit)) {
            m.rel_cnt[b]++;
            m.rel_x[b] = (uint16_t)m.x;
            m.rel_y[b] = (uint16_t)m.y;
        }
    }
    m.buttons = nb;
}

/* ------------------------------------------------------------------ */
/* Сброс                                                               */
/* ------------------------------------------------------------------ */
void bios_33h_reset(void)
{
    int was_installed = m.installed;
    memset(&m, 0, sizeof(m));
    m.installed = was_installed;

    m.minx = 0; m.maxx = VIRT_W - 1;
    m.miny = 0; m.maxy = VIRT_H - 1;
    m.x = VIRT_W / 2;
    m.y = VIRT_H / 2;

    m.scr_mask = 0x77FF;    /* дефолтный текстовый курсор MS Mouse */
    m.cur_mask = 0x7700;

    m.mpp_x = 8;            /* mickeys per 8 pixels */
    m.mpp_y = 16;
    m.sens_x = 50; m.sens_y = 50; m.sens_d = 50;

    m.hide_count = -1;      /* -1 => курсор скрыт (стандарт MS Mouse) */
    m.drawn = 0;
}

void bios_33h_install(int enabled)
{
    bios_33h_reset();
    m.installed = enabled ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* INT 33h                                                             */
/* ------------------------------------------------------------------ */
bool bios_33h(CPU* cpu)
{
    switch (CPU_AX) {

    case 0x0000:    /* Reset driver and read status */
    case 0x0021:    /* Software reset */
        cursor_erase();
        bios_33h_reset();
        m.installed = 1;
        CPU_AX = 0xFFFF;            /* драйвер установлен */
        CPU_BX = MOUSE_BUTTONS;
        break;

    case 0x0001:    /* Show cursor */
        m.hide_count++;
        if (m.hide_count > 0)
            m.hide_count = 0;
        if (m.hide_count == 0)
            cursor_draw();
        break;

    case 0x0002:    /* Hide cursor */
        if (m.hide_count == 0)
            cursor_erase();
        m.hide_count--;
        break;

    case 0x0003:    /* Get position and button status */
        CPU_BX = m.buttons;
        CPU_CX = (uint16_t)m.x;
        CPU_DX = (uint16_t)m.y;
        break;

    case 0x0004:    /* Set position */
        m.x = CPU_CX;
        m.y = CPU_DX;
        if (m.x < m.minx) m.x = m.minx;
        if (m.x > m.maxx) m.x = m.maxx;
        if (m.y < m.miny) m.y = m.miny;
        if (m.y > m.maxy) m.y = m.maxy;
        cursor_refresh();
        break;

    case 0x0005: {  /* Get button press data */
        int b = CPU_BX & 3;
        if (b > 2) b = 2;
        CPU_AX = m.buttons;
        CPU_BX = m.press_cnt[b];
        CPU_CX = m.press_x[b];
        CPU_DX = m.press_y[b];
        m.press_cnt[b] = 0;
        break;
    }

    case 0x0006: {  /* Get button release data */
        int b = CPU_BX & 3;
        if (b > 2) b = 2;
        CPU_AX = m.buttons;
        CPU_BX = m.rel_cnt[b];
        CPU_CX = m.rel_x[b];
        CPU_DX = m.rel_y[b];
        m.rel_cnt[b] = 0;
        break;
    }

    case 0x0007: {  /* Set horizontal min/max */
        int a = (int16_t)CPU_CX, b = (int16_t)CPU_DX;
        if (a > b) { int t = a; a = b; b = t; }
        m.minx = a; m.maxx = b;
        if (m.x < m.minx) m.x = m.minx;
        if (m.x > m.maxx) m.x = m.maxx;
        cursor_refresh();
        break;
    }

    case 0x0008: {  /* Set vertical min/max */
        int a = (int16_t)CPU_CX, b = (int16_t)CPU_DX;
        if (a > b) { int t = a; a = b; b = t; }
        m.miny = a; m.maxy = b;
        if (m.y < m.miny) m.y = m.miny;
        if (m.y > m.maxy) m.y = m.maxy;
        cursor_refresh();
        break;
    }

    case 0x0009:    /* Define graphics cursor — не поддерживается */
        break;

    case 0x000A:    /* Define text cursor */
        cursor_erase();
        if (CPU_BX == 0) {          /* software cursor */
            m.scr_mask = CPU_CX;
            m.cur_mask = CPU_DX;
        } else {                    /* hardware cursor — эмулируем софтовым */
            m.scr_mask = 0x77FF;
            m.cur_mask = 0x7700;
        }
        cursor_draw();
        break;

    case 0x000B:    /* Read motion counters */
        CPU_CX = (uint16_t)(int16_t)m.mickey_x;
        CPU_DX = (uint16_t)(int16_t)m.mickey_y;
        m.mickey_x = 0;
        m.mickey_y = 0;
        break;

    case 0x000C:    /* Define event handler (ES:DX, mask=CX) */
        m.cb_mask = CPU_CX;
        m.cb_seg  = CPU_ES;
        m.cb_off  = CPU_DX;
        break;

    case 0x000D:    /* Light pen emulation on */
    case 0x000E:    /* Light pen emulation off */
        break;

    case 0x000F:    /* Set mickeys per 8 pixels */
        m.mpp_x = CPU_CX ? (int16_t)CPU_CX : 8;
        m.mpp_y = CPU_DX ? (int16_t)CPU_DX : 16;
        break;

    case 0x0010:    /* Define exclusion area — игнорируем */
        break;

    case 0x0013:    /* Set double-speed threshold */
        break;

    case 0x0014:    /* Exchange event handler */
        {
            uint16_t om = m.cb_mask, os = m.cb_seg, oo = m.cb_off;
            m.cb_mask = CPU_CX;
            m.cb_seg  = CPU_ES;
            m.cb_off  = CPU_DX;
            CPU_CX = om;
            SET_ES(os);
            CPU_DX = oo;
        }
        break;

    case 0x0015:    /* Get driver state storage size */
        CPU_BX = sizeof(MouseState);
        break;

    case 0x0016:    /* Save driver state -> ES:DX */
        {
            uint32_t p = ((uint32_t)CPU_ES << 4) + CPU_DX;
            const uint8_t *s = (const uint8_t *)&m;
            for (unsigned i = 0; i < sizeof(MouseState); i++)
                pstore8(p + i, s[i]);
        }
        break;

    case 0x0017:    /* Restore driver state <- ES:DX */
        {
            uint32_t p = ((uint32_t)CPU_ES << 4) + CPU_DX;
            uint8_t *s = (uint8_t *)&m;
            cursor_erase();
            for (unsigned i = 0; i < sizeof(MouseState); i++)
                s[i] = pload8(p + i);
            m.drawn = 0;
            cursor_draw();
        }
        break;

    case 0x001A:    /* Set mouse sensitivity */
        m.sens_x = (uint8_t)CPU_BX;
        m.sens_y = (uint8_t)CPU_CX;
        m.sens_d = (uint8_t)CPU_DX;
        break;

    case 0x001B:    /* Get mouse sensitivity */
        CPU_BX = m.sens_x;
        CPU_CX = m.sens_y;
        CPU_DX = m.sens_d;
        break;

    case 0x001C:    /* Set interrupt rate */
    case 0x001D:    /* Set CRT page */
        break;

    case 0x001E:    /* Get CRT page */
        CPU_BX = pload8(BDA_VIDEO_PAGE);
        break;

    case 0x001F:    /* Disable driver */
        cursor_erase();
        CPU_AX = 0x001F;
        SET_ES(0);
        CPU_BX = 0;
        break;

    case 0x0020:    /* Enable driver */
        break;

    case 0x0024:    /* ---- Get software version, mouse type and IRQ ---- */
        CPU_BX = 0x0800;    /* версия 8.00 (BCD: BH=major, BL=minor)      */
        CPU_CH = 0x04;      /* 04h = PS/2 mouse   <<< это читает SysInfo  */
        CPU_CL = 0x00;      /* PS/2: IRQ не сообщается (0)                */
        break;

    case 0x0026:    /* Get maximum virtual coordinates */
        CPU_BX = m.installed ? 0x0000 : 0xFFFF;   /* BX=0 => драйвер активен */
        CPU_CX = (uint16_t)m.maxx;
        CPU_DX = (uint16_t)m.maxy;
        break;

    case 0x0027:    /* Get screen/cursor masks and mickey counts */
        CPU_AX = m.scr_mask;
        CPU_BX = m.cur_mask;
        CPU_CX = (uint16_t)(int16_t)m.mickey_x;
        CPU_DX = (uint16_t)(int16_t)m.mickey_y;
        break;

    default:
        /* неизвестная функция — молча игнорируем, как реальный драйвер */
        break;
    }

    return true;
}
