/*
 * INT 33h - Microsoft Mouse driver (native BIOS implementation)
 * INT 74h - IRQ12 (PS/2 aux device) handler
 *
 * Никаких хуков в main.c: драйвер живёт целиком внутри BIOS и общается с
 * мышью так же, как настоящий DOS-драйвер — через контроллер 8042 (порты
 * 0x60/0x64) и прерывание IRQ12. Хостовые мыши (PS/2, USB, NES-эмуляция)
 * уже кладут пакеты в i8042 через ps2_mouse_event(), поэтому все три
 * источника работают автоматически.
 *
 * Активируется только при native BIOS: bios_33h_install() зовётся из
 * bios_post(). При внешнем BIOS (SeaBIOS) handlers[] не задействованы,
 * вектора не подменяются — поведение не меняется.
 *
 * Если гость загрузит свой драйвер мыши (CTMOUSE и т.п.), он перепишет
 * вектора INT 33h/INT 74h в IVT, и наш код просто перестанет вызываться.
 *
 * Курсор: программный, только текстовые режимы (BDA 40:49 = 0..3, 7).
 * Обработчики событий (AX=000Ch/0014h) сохраняются, но не вызываются.
 */

#include <string.h>
#include "286/cpu.h"
#include "bios.h"

#define BDA_VIDEO_MODE   0x449u
#define BDA_VIDEO_COLS   0x44Au
#define BDA_VIDEO_ROWS   0x484u   /* rows - 1 */
#define BDA_VIDEO_PAGE   0x462u
#define BDA_VIDEO_PGSIZE 0x44Cu

#define MOUSE_BUTTONS    2

#define VIRT_W  640
#define VIRT_H  200

/* 8042 */
#define KBC_DATA         0x60
#define KBC_STATUS       0x64
#define KBC_CMD          0x64
#define KBC_ST_OBF       0x01
#define KBC_ST_IBF       0x02
#define KBC_ST_AUX_OBF   0x20

typedef struct {
    int      installed;

    int      x, y;
    int      minx, maxx;
    int      miny, maxy;

    uint8_t  buttons;

    uint16_t press_cnt[3], press_x[3], press_y[3];
    uint16_t rel_cnt[3],   rel_x[3],   rel_y[3];

    int      mickey_x, mickey_y;
    int      frac_x, frac_y;

    int      hide_count;          /* курсор виден только при == 0 */

    uint16_t scr_mask, cur_mask;

    int      mpp_x, mpp_y;        /* mickeys per 8 pixels */
    uint8_t  sens_x, sens_y, sens_d;

    int      drawn;
    uint32_t drawn_addr;
    uint16_t drawn_cell;

    uint16_t cb_mask, cb_seg, cb_off;

    /* сборка PS/2-пакета в INT 74h */
    uint8_t  pkt[4];
    uint8_t  pkt_idx;
} MouseState;

static MouseState m;

/* ------------------------------------------------------------------ */
/* Текстовый курсор                                                    */
/* ------------------------------------------------------------------ */

static int text_mode_base(uint32_t *base, int *cols, int *rows)
{
    uint8_t mode = pload8(BDA_VIDEO_MODE);

    if (mode == 0x07)      *base = 0xB0000u;
    else if (mode <= 0x03) *base = 0xB8000u;
    else                   return 0;

    *cols = pload16(BDA_VIDEO_COLS);
    if (*cols <= 0 || *cols > 132) *cols = 80;
    *rows = pload8(BDA_VIDEO_ROWS) + 1;
    if (*rows <= 0 || *rows > 60) *rows = 25;

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
/* Применение движения                                                 */
/* dx/dy — экранные (+x вправо, +y вниз)                               */
/* ------------------------------------------------------------------ */
static void mouse_apply(int dx, int dy, uint8_t nb)
{
    m.mickey_x += dx;
    m.mickey_y += dy;

    m.frac_x += dx * 8;
    m.frac_y += dy * 8;

    int mx = m.mpp_x ? m.mpp_x : 8;
    int my = m.mpp_y ? m.mpp_y : 16;
    int px = m.frac_x / mx;
    int py = m.frac_y / my;
    m.frac_x -= px * mx;
    m.frac_y -= py * my;

    if (px || py) {
        m.x += px;
        m.y += py;
        if (m.x < m.minx) m.x = m.minx;
        if (m.x > m.maxx) m.x = m.maxx;
        if (m.y < m.miny) m.y = m.miny;
        if (m.y > m.maxy) m.y = m.maxy;
        cursor_refresh();
    }

    nb &= 7;
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
/* INT 74h — IRQ12                                                     */
/* ------------------------------------------------------------------ */
bool bios_74h(CPU* cpu)
{
    uint8_t st = cpu_portin8(KBC_STATUS);

    if ((st & (KBC_ST_OBF | KBC_ST_AUX_OBF)) == (KBC_ST_OBF | KBC_ST_AUX_OBF)) {
        uint8_t b = cpu_portin8(KBC_DATA);

        /* ACK/RESEND/self-test bytes are not packet data. */
        if (m.pkt_idx == 0 && (b == 0xFA || b == 0xFE || b == 0xAA)) {
            /* ignore asynchronous mouse command response */
        }
        /* ресинхронизация: бит3 первого байта пакета всегда 1 */
        else if (m.pkt_idx == 0 && !(b & 0x08)) {
            /* мусор — игнорируем */
        } else {
            m.pkt[m.pkt_idx++] = b;

            if (m.pkt_idx >= 3) {
                m.pkt_idx = 0;

                uint8_t f = m.pkt[0];
                int dx = m.pkt[1];
                int dy = m.pkt[2];
                if (f & 0x10) dx |= ~0xFF;      /* знак X */
                if (f & 0x20) dy |= ~0xFF;      /* знак Y */
                if (f & 0xC0) { dx = 0; dy = 0; }   /* overflow */

                /* PS/2: +Y = вверх; экран: +Y = вниз */
                if (m.installed)
                    mouse_apply(dx, -dy, (uint8_t)(f & 0x07));
            }
        }
    }

    /* EOI: сначала slave, потом master */
    cpu_portout8(0xA0, 0x20);
    cpu_portout8(0x20, 0x20);
    return true;
}

/* ------------------------------------------------------------------ */
/* Инициализация 8042 + мыши (как это делает настоящий драйвер)         */
/* ------------------------------------------------------------------ */
static void kbc_wait_ibe(CPU* cpu)
{
    for (int i = 0; i < 10000; i++)
        if (!(cpu_portin8(KBC_STATUS) & KBC_ST_IBF))
            return;
}

static int kbc_wait_aux_obf(CPU* cpu)
{
    for (int i = 0; i < 10000; i++) {
        uint8_t st = cpu_portin8(KBC_STATUS);
        if ((st & (KBC_ST_OBF | KBC_ST_AUX_OBF)) ==
            (KBC_ST_OBF | KBC_ST_AUX_OBF))
            return 1;

        /*
         * Do not consume a pending keyboard byte while waiting for a mouse
         * reply.  Leave it queued for IRQ1 and let IRQ12 discard any delayed
         * mouse ACK asynchronously if necessary.
         */
        if ((st & KBC_ST_OBF) && !(st & KBC_ST_AUX_OBF))
            return 0;
    }
    return 0;
}

static void kbc_cmd(CPU* cpu, uint8_t c)
{
    kbc_wait_ibe(cpu);
    cpu_portout8(KBC_CMD, c);
}

static void kbc_data(CPU* cpu, uint8_t d)
{
    kbc_wait_ibe(cpu);
    cpu_portout8(KBC_DATA, d);
}

/* послать байт мыши и съесть ACK, только если это действительно AUX */
static void aux_send(CPU* cpu, uint8_t d)
{
    kbc_cmd(cpu, 0xD4);          /* следующий байт — в aux-порт */
    kbc_data(cpu, d);
    if (kbc_wait_aux_obf(cpu))
        (void)cpu_portin8(KBC_DATA);   /* mouse ACK (normally 0xFA) */
}

static void mouse_hw_init(CPU* cpu)
{
    /* включить aux-порт */
    kbc_cmd(cpu, 0xA8);

    /* command byte: разрешить IRQ12 (bit1) и снять disable-aux (bit5) */
    kbc_cmd(cpu, 0x20);
    /*
     * 0x20 returns the controller command byte through the controller output
     * buffer, not through the AUX device, so wait for ordinary OBF here.
     */
    uint8_t cb = 0x45;
    for (int i = 0; i < 10000; i++) {
        if (cpu_portin8(KBC_STATUS) & KBC_ST_OBF) {
            cb = cpu_portin8(KBC_DATA);
            break;
        }
    }
    cb |=  0x02;    /* enable aux (IRQ12) interrupt */
    cb &= ~0x20;    /* aux clock enable */
    cb |=  0x01;    /* keep keyboard IRQ1 on */
    kbc_cmd(cpu, 0x60);
    kbc_data(cpu, cb);

    aux_send(cpu, 0xF6);   /* set defaults */
    aux_send(cpu, 0xF4);   /* enable data reporting (stream mode) */

    m.pkt_idx = 0;
}

/* ------------------------------------------------------------------ */
void bios_33h_reset(void)
{
    int was = m.installed;
    memset(&m, 0, sizeof(m));
    m.installed = was;

    m.minx = 0; m.maxx = VIRT_W - 1;
    m.miny = 0; m.maxy = VIRT_H - 1;
    m.x = VIRT_W / 2;
    m.y = VIRT_H / 2;

    m.scr_mask = 0x77FF;
    m.cur_mask = 0x7700;

    m.mpp_x = 8;
    m.mpp_y = 16;
    m.sens_x = m.sens_y = m.sens_d = 50;

    m.hide_count = -1;      /* скрыт */
    m.drawn = 0;
    m.pkt_idx = 0;
}

/* Зовётся из bios_post(). enabled == pc->mouse_enabled */
void bios_33h_install(CPU* cpu, int enabled)
{
    bios_33h_reset();
    m.installed = enabled ? 1 : 0;
    if (enabled)
        mouse_hw_init(cpu);
}

/* ------------------------------------------------------------------ */
/* INT 33h                                                             */
/* ------------------------------------------------------------------ */
bool bios_33h(CPU* cpu)
{
    /*
     * A disabled mouse driver still has a callable INT 33h entry point.
     * The standard reset/status probe reports AX=0000h when no driver is
     * installed.  Other functions are harmless no-ops in that state.
     */
    if (!m.installed) {
        if (CPU_AX == 0x0000 || CPU_AX == 0x0021) {
            CPU_AX = 0x0000;
            CPU_BX = 0x0000;
        }
        return true;
    }
    
    switch (CPU_AX) {

    case 0x0000:    /* Reset driver and read status */
    case 0x0021:    /* Software reset */
        cursor_erase();
        bios_33h_reset();
        mouse_hw_init(cpu);
        CPU_AX = 0xFFFF;
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
        if (CPU_BX == 0) {
            m.scr_mask = CPU_CX;
            m.cur_mask = CPU_DX;
        } else {
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

    case 0x000C:    /* Define event handler */
        m.cb_mask = CPU_CX;
        m.cb_seg  = CPU_ES;
        m.cb_off  = CPU_DX;
        break;

    case 0x000D:
    case 0x000E:
        break;

    case 0x000F:    /* Set mickeys per 8 pixels */
        m.mpp_x = CPU_CX ? (int16_t)CPU_CX : 8;
        m.mpp_y = CPU_DX ? (int16_t)CPU_DX : 16;
        break;

    case 0x0010:    /* Define exclusion area — игнорируем */
    case 0x0013:    /* Set double-speed threshold */
        break;

    case 0x0014: {  /* Exchange event handler */
        uint16_t om = m.cb_mask, os = m.cb_seg, oo = m.cb_off;
        m.cb_mask = CPU_CX;
        m.cb_seg  = CPU_ES;
        m.cb_off  = CPU_DX;
        CPU_CX = om;
        SET_ES(os);
        CPU_DX = oo;
        break;
    }

    case 0x0015:    /* Get driver state storage size */
        CPU_BX = sizeof(MouseState);
        break;

    case 0x0016: {  /* Save driver state -> ES:DX */
        uint32_t p = ((uint32_t)CPU_ES << 4) + CPU_DX;
        const uint8_t *s = (const uint8_t *)&m;
        for (unsigned i = 0; i < sizeof(MouseState); i++)
            pstore8(p + i, s[i]);
        break;
    }

    case 0x0017: {  /* Restore driver state <- ES:DX */
        uint32_t p = ((uint32_t)CPU_ES << 4) + CPU_DX;
        uint8_t *s = (uint8_t *)&m;
        cursor_erase();
        for (unsigned i = 0; i < sizeof(MouseState); i++)
            s[i] = pload8(p + i);
        m.drawn = 0;
        cursor_draw();
        break;
    }

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

    case 0x001C:
    case 0x001D:
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

    case 0x0024:    /* Get version / mouse type / IRQ  <<< это читает SysInfo */
        CPU_BX = 0x0800;    /* версия 8.00 (BH=major, BL=minor) */
        CPU_CH = 0x04;      /* 04h = PS/2 mouse */
        CPU_CL = 0x00;      /* PS/2: IRQ не сообщается */
        break;

    case 0x0026:    /* Get maximum virtual coordinates */
        CPU_BX = m.installed ? 0x0000 : 0xFFFF;
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
        break;
    }

    return true;
}
