#include <pico.h>
#include <pico/time.h>
#include "286/cpu.h"
#include "bios.h"

/*
 * INT 15h/AH=83h: отложенное взведение флага события. Армируется из
 * обработчика, завершается bios_15h_event_wait_tick() (девайс-блок
 * pc_step()). Один активный интервал, как у оригинала (40:A0).
 */
uint32_t get_uticks(void);

static uint32_t event_wait_flag_lin;
static uint32_t event_wait_deadline;
static bool     event_wait_armed;

void bios_15h_event_wait_arm(uint32_t flag_lin, uint32_t usec)
{
    event_wait_flag_lin = flag_lin;
    event_wait_deadline = get_uticks() + usec;
    event_wait_armed = true;
}

void bios_15h_event_wait_cancel(void)
{
    event_wait_armed = false;
}

void bios_15h_event_wait_tick(void)
{
    if (!event_wait_armed)
        return;
    if ((int32_t)(get_uticks() - event_wait_deadline) < 0)
        return;
    event_wait_armed = false;
    pstore8(event_wait_flag_lin, pload8(event_wait_flag_lin) | 0x80);
    pstore8(0x4A0, 0x00);
}

#define printf(...) bios_printf(cpu, __VA_ARGS__)

#define EBDA_MOUSE_HANDLER_OFF  0x22
#define EBDA_MOUSE_HANDLER_SEG  0x24
#define EBDA_MOUSE_FLAG1        0x26
#define EBDA_MOUSE_FLAG2        0x27
#define EBDA_MOUSE_DATA         0x28

#define INT15C2_SUCCESS         0x00
#define INT15C2_INV_FUNCTION    0x01
#define INT15C2_INV_INPUT       0x02
#define INT15C2_INTERFACE       0x03
#define INT15C2_RESEND          0x04
#define INT15C2_NO_HANDLER      0x05

#define KBD_STATUS_PORT         0x64
#define KBD_DATA_PORT           0x60
#define KBD_CMD_WRITE_MOUSE     0xD4
#define KBD_STAT_OBF            0x01
#define KBD_STAT_IBF            0x02
#define PS2_ACK                 0xFA

static uint32_t bios_15h_ebda(void)
{
    uint16_t seg = pload16(0x40E);
    return (uint32_t)seg << 4;
}

static void bios_15h_c2_ok(CPU* cpu)
{
    CPU_AH = INT15C2_SUCCESS;
    cf = 0;
}

static void bios_15h_c2_err(CPU* cpu, uint8_t code)
{
    CPU_AH = code;
    cf = 1;
}

static bool ps2_wait_in(CPU* cpu)
{
    for (uint32_t i = 0; i < 100000; i++)
        if (!(cpu_portin8(KBD_STATUS_PORT) & KBD_STAT_IBF))
            return true;
    return false;
}

static bool ps2_wait_out(CPU* cpu)
{
    for (uint32_t i = 0; i < 100000; i++)
        if (cpu_portin8(KBD_STATUS_PORT) & KBD_STAT_OBF)
            return true;
    return false;
}

static bool ps2_mouse_write(CPU* cpu, uint8_t v)
{
    if (!ps2_wait_in(cpu)) return false;
    cpu_portout8(KBD_STATUS_PORT, KBD_CMD_WRITE_MOUSE);
    if (!ps2_wait_in(cpu)) return false;
    cpu_portout8(KBD_DATA_PORT, v);
    if (!ps2_wait_out(cpu)) return false;
    return cpu_portin8(KBD_DATA_PORT) == PS2_ACK;
}

static bool ps2_mouse_cmd(CPU* cpu, uint8_t cmd, uint8_t *param,
                          uint8_t out_count, uint8_t in_count)
{
    if (!ps2_mouse_write(cpu, cmd))
        return false;
    if (out_count) {
        if (!ps2_mouse_write(cpu, *param))
            return false;
    }
    for (uint8_t i = 0; i < in_count; i++) {
        if (!ps2_wait_out(cpu))
            return false;
        param[i] = cpu_portin8(KBD_DATA_PORT);
    }
    return true;
}

// A20 GATE
static bool bios_15h_24h(CPU* cpu) {
    switch (CPU_AL) {
    case 0x00:              /* disable A20 */
        /* A20 is permanently enabled on this emulated machine. */
        CPU_AH = 0x86;
        cf = 1;
        return true;
    case 0x01:              /* enable A20 */
        cpu_set_a20(cpu, 1);
        CPU_AH = 0x00;
        cf = 0;
        return true;
    case 0x02:              /* get A20 status */
        CPU_AL = (uint8_t)cpu_get_a20(cpu);
        CPU_AH = 0x00;
        cf = 0;
        return true;
    case 0x03:              /* get A20 support — keyboard ctrl + port 0x92 */
        CPU_BX = 0x0003;
        CPU_AH = 0x00;
        cf = 0;
        return true;
    default:
        cf = 1;
        CPU_AH = 0x86;      /* unsupported subfunction */
        return true;
    }
}

/*
SYSTEM - WAIT ON EXTERNAL EVENT (CONVERTIBLE and some others)
AH = 41h
AL = condition type (see #00463)
BH = condition compare or mask value
BL = timeout value times 55 milliseconds
00h means no timeout
DX = I/O port address if AL bit 4 set
ES:DI -> user byte if AL bit 4 clear

Return:
After event or timeout occurs

Note: Call AH=C0h and examine bit 3 of feature byte 1 to determine whether this function is supported

See Also: AH=83h - AH=86h - AH=C0h


Bitfields for external event wait condition type:

Bit(s)  Description     (Table 00463)
0-2    condition to wait for.
0 any external event.
1 compare and return if equal.
2 compare and return if not equal.
3 test and return if not zero.
4 test and return if zero

3      reserved
4      1=port address, 0=user byte
5-7    reserved
*/
static bool bios_15h_41h(CPU* cpu) {
    static int old = -1; // -1 means, we have no prev. state
    static u32 start_us = 0;
    uint16_t flags_on_stack = readw86((CPU_SS << 4) + CPU_SP + 4);
    u8 cond_type = CPU_AL;
    u8 wait4 = cond_type & 0b00000111;
    u8 comp_with = CPU_BH;
    u32 timeout_us = 55000ul * CPU_BL;
    u8 v;
    if (cond_type & 0b00010000) {  // port
        v = cpu_portin8(CPU_DX);
    } else { // address
        v = read86(((u32)CPU_ES << 4) + CPU_DI);
    }
    bool res = true;
    if (!wait4) {
        if (old == -1) {
            old = v;
            start_us = time_us_32();
        }
        if (old != -1 && old != v) {
            old = -1; // cleanup saved value
            goto ok;
        }
        if (timeout_us != 0 && (u32)(time_us_32() - start_us) >= timeout_us) {
             cf = 1; // timed out
            CPU_AH = 0x80;
            old = -1;
            start_us = 0;
            goto rt;
        }
        ifl = 1; // allow interrupt me by IRQ on next step
        res = false;
        goto rt;
    }
    switch (wait4) {
    case 1: /* equal */
        res = (v == CPU_BH);
        break;
    case 2: /* not equal */
        res = (v != CPU_BH);
        break;
    case 3: /* test not zero */
        res = ((v & CPU_BH) != 0);
        break;
    case 4: /* test zero */
        res = ((v & CPU_BH) == 0);
        break;
    }
    if (!res) {
        if (old == -1) {
            old = 0;              /* mark wait started */
            start_us = time_us_32();
        }
        if (timeout_us != 0 && (u32)(time_us_32() - start_us) >= timeout_us) {
            cf = 1;
            CPU_AH = 0x80;
            old = -1;
            start_us = 0;
            goto rt;
        }
        ifl = 1; // allow interrupt me by IRQ on next step
        goto rt;
    }
ok:
    old = -1;
    start_us = 0;
    cf = 0;
    CPU_AH = 0;
rt:
    flags_on_stack = (flags_on_stack & ~0x0041) // reset ZF, CF
                   | (cpu_getflags(cpu) & 0x0041); // set them back from CPU
    writew86((CPU_SS << 4) + CPU_SP + 4, flags_on_stack);
    return res;
}

static bool get_286_desc(CPU* cpu, uint32_t addr, uint32_t* base, uint32_t* limit)
{
    uint16_t lim = pload16(addr + 0);
    uint32_t bas = (uint32_t)pload8(addr + 2)
                 | ((uint32_t)pload8(addr + 3) << 8)
                 | ((uint32_t)pload8(addr + 4) << 16);
    uint8_t access = pload8(addr + 5);

    /*
     * 286 data segment descriptor, present.
     *
     * P bit      = 0x80
     * S bit      = 0x10, code/data descriptor
     * type bit3  = 0 data, 1 code
     *
     * Разрешаем data read-only/read-write:
     * access примерно 90h/92h/93h/96h/97h.
     */
    if ((access & 0x80) == 0)
        return false;          /* not present */

    if ((access & 0x10) == 0)
        return false;          /* system descriptor, not data/code */

    if (access & 0x08)
        return false;          /* code segment, not data */

    *base = bas;
    *limit = lim;
    return true;
}

/*
SYSTEM - COPY EXTENDED MEMORY
AH = 87h
CX = number of words to copy (max 8000h)
ES:SI -> global descriptor table (see #00499)

Return:
CF set on error
CF clear if successful
AH = status (see #00498)

Notes: Copy is done in protected mode with interrupts disabled by the default BIOS handler; many 386 memory managers perform 
the copy with interrupts enabled. On the PS/2 30-286 & "Tortuga" this function does not use the port 92h for A20 control,
but instead uses the keyboard controller (8042). Reportedly this may cause the system to crash when access to the 8042
is disabled in password server mode (see also PORT 0064h,#P0398). This function is incompatible with the OS/2 compatibility box
*/
static bool bios_15h_87h(CPU* cpu)
{
    /* ES:SI → 48-byte GDT table:
     *   +00h: null descriptor
     *   +08h: GDT descriptor (filled by BIOS)
     *   +10h: source descriptor
     *   +18h: destination descriptor
     *   +20h: CS descriptor (filled by BIOS)
     *   +28h: SS descriptor (filled by BIOS)
     */
    uint32_t tbl = (uint32_t)CPU_ES * 16 + CPU_SI;
    uint32_t count = CPU_CX;  /* number of WORDS to copy */
    if (count > 0x8000u) {
        CPU_AH = 0x01;  /* invalid parameter */
        cf = 1;
        return true;
    }
    if (!count) {
        CPU_AH = 0;
        cf = 0;
        return true;
    }
    uint32_t src, dst, src_limit, dst_limit;
    if (!get_286_desc(cpu, tbl + 0x10, &src, &src_limit) ||
        !get_286_desc(cpu, tbl + 0x18, &dst, &dst_limit)) {
        CPU_AH = 0x01;
        cf = 1;
        return true;
    }
    u32 bytes = count << 1;
    if (src_limit + 1u < bytes || dst_limit + 1u < bytes) {
        CPU_AH = 0x01;
        cf = 1;
        return true;
    }    

    /* Enable A20 for access above 1MB */
    int prev_a20 = cpu_get_a20(cpu);
    cpu_set_a20(cpu, 1);

    /* Copy CX words */
    for (uint16_t i = 0; i < count; i++) {
        uint16_t w = pload16(src + i * 2);
        pstore16(dst + i * 2, w);
    }

    cpu_set_a20(cpu, prev_a20);

    CPU_AH = 0x00;
    cf = 0;
    return true;
}

/*
SYSTEM - GET EXTENDED MEMORY SIZE (286+)
AH = 88h

Return:
CF clear if successful
AX = number of contiguous KB starting at absolute address 100000h
CF set on error
AH = status
80h invalid command (PC,PCjr)
86h unsupported function (XT,PS30)

Notes: TSRs which wish to allocate extended memory to themselves often hook this call, and return a reduced memory size.
They are then free to use the memory between the new and old sizes at will..
The standard BIOS only returns memory between 1MB and 16MB; use AH=C7h for memory beyond 16MB.
Not all BIOSes correctly return the carry flag, making this call unreliable unless one first checks whether it is supported through
a mechanism other than calling the function and testing CF. Due to applications not dealing with more than 24-bit descriptors (286),
Windows 3.0 has problems when this function reports more than 15 MB. Some releases of HIMEM.SYS are therefore limited to use only 15 MB,
even when this function reports more.
*/
static bool bios_15h_88h(CPU* cpu) {
    /* SeaBIOS handle_1588(): по Ralf Brown предел 15 МБ, но реальные
       машины отдают максимум 63 МБ - эталон клампит именно так. CMOS
       0x17/0x18 в POST уже ограничен 0xFFFF, что дало бы 63.99 МБ. */
    uint32_t ext_kb = (uint32_t)cmos_read(cpu, 0x17) |
                      ((uint32_t)cmos_read(cpu, 0x18) << 8);
    if (ext_kb > 63u * 1024u)
        ext_kb = 63u * 1024u;
    cf = 0;
    CPU_AX = (uint16_t)ext_kb;
    return true;
}

/*
SYSTEM - GET CONFIGURATION (XT >1986/1/10,AT mdl 3x9,CONV,XT286,PS)
AH = C0h

Return:
CF set if BIOS doesn't support call
CF clear on success
ES:BX -> ROM table (see #00509)
AH = status
00h successful
The PC XT (since 1986/01/10), PC AT (since 1985/06/10), the
PC XT Model 286, the PC Convertible and most PS/2 machines
will clear the CF flag and return the table in ES:BX.
80h unsupported function
The PC and PCjr return AH=80h/CF set
86h unsupported function
The PC XT (1982/11/08), PC Portable, PC AT (1984/01/10),
or PS/2 prior to Model 30 return AH=86h/CF set

Format of ROM configuration table:

Offset  Size    Description     (Table 00509)
00h    WORD    number of bytes following
02h    BYTE    model (see #00515)
03h    BYTE    submodel (see #00515)

04h    BYTE    BIOS revision:
0 for first release, 1 for 2nd, etc.
05h    BYTE    feature byte 1 (see #00510)
06h    BYTE    feature byte 2 (see #00511)
07h    BYTE    feature byte 3 (see #00512)
08h    BYTE    feature byte 4 (see #00513)
09h    BYTE    feature byte 5 (see #00514)
??? (08h) (Phoenix 386 v1.10)
??? (0Fh) (Phoenix 486 v1.03 PCI)
---AWARD BIOS---
0Ah  N BYTEs   AWARD copyright notice
---Phoenix BIOS---
0Ah    BYTE    ??? (00h)
0Bh    BYTE    major version
0Ch    BYTE    minor version (BCD)
0Dh  4 BYTEs   ASCIZ string "PTL" (Phoenix Technologies Ltd)
also on Phoenix Cascade BIOS
---Quadram Quad386---
0Ah 17 BYTEs   ASCII signature string "Quadram Quad386XT"
---Toshiba (Satellite Pro 435CDS at least)---
0Ah  7 BYTEs   signature "TOSHIBA"
11h    BYTE    ??? (8h)
12h    BYTE    ??? (E7h) product ID??? (guess)
13h  3 BYTEs   "JPN"


Bitfields for feature byte 1:

Bit(s)  Description     (Table 00510)
7      DMA channel 3 used by hard disk BIOS
6      2nd interrupt controller (8259) installed
5      Real-Time Clock installed
4      INT 15/AH=4Fh called upon INT 09h
3      wait for external event (INT 15/AH=41h) supported
2      extended BIOS area allocated (usually at top of RAM)
1      bus is Micro Channel instead of ISA
0      system has dual bus (Micro Channel + ISA)

See Also: #00509 - #00511
*/
static bool bios_15h_C0h(CPU* cpu) {
    /*
     * INT 15h / AH=C0h - GET CONFIGURATION
     *
     * Return:
     *   CF clear
     *   AH = 00h
     *   ES:BX -> ROM configuration table
     *
     * Minimal IBM PC/AT-compatible table:
     *   model        = FCh  IBM PC AT
     *   submodel     = 00h
     *   BIOS rev     = 00h
     *   feature byte 1 is built by bios_post()
     *
     * Feature byte 1:
     *   bit 6 = second interrupt controller installed
     *   bit 5 = RTC installed
     *   bit 4 = INT 15h/AH=4Fh called by INT 09h
     *   bit 3 = INT 15h/AH=41h wait for external event supported
     *
     * We do NOT set bit 2, because EBDA segment at BDA 0040:000E is zero.
     * @See: load_bios_and_reset
     */
    CPU_AH = 0x00;
    SET_ES ( 0xFFF0 );
    CPU_BX = 0x0010;
    cf = 0;
    return true;
}

static bool bios_15h_C2h(CPU* cpu)
{
    static const uint8_t sample_rates[7] = {10, 20, 40, 60, 80, 100, 200};
    uint32_t ebda = bios_15h_ebda();
    uint8_t p[3];

    if (!ebda) {
        bios_15h_c2_err(cpu, INT15C2_INTERFACE);
        return true;
    }

    switch (CPU_AL) {
    case 0x00:                                      /* enable/disable */
        if (CPU_BH == 0x00) {
            if (!ps2_mouse_cmd(cpu, 0xF5, p, 0, 0))
                bios_15h_c2_err(cpu, INT15C2_RESEND);
            else
                bios_15h_c2_ok(cpu);
            return true;
        }
        if (CPU_BH == 0x01) {
            if (!(pload8(ebda + EBDA_MOUSE_FLAG2) & 0x80)) {
                bios_15h_c2_err(cpu, INT15C2_NO_HANDLER);
                return true;
            }
            if (!ps2_mouse_cmd(cpu, 0xF4, p, 0, 0))
                bios_15h_c2_err(cpu, INT15C2_RESEND);
            else
                bios_15h_c2_ok(cpu);
            return true;
        }
        bios_15h_c2_err(cpu, INT15C2_INV_FUNCTION);
        return true;

    case 0x01:                                      /* reset */
        if (!ps2_mouse_cmd(cpu, 0xFF, p, 0, 2)) {
            bios_15h_c2_err(cpu, INT15C2_RESEND);
            return true;
        }
        CPU_BL = p[0];                              /* usually AAh */
        CPU_BH = p[1];                              /* device id */
        bios_15h_c2_ok(cpu);
       return true;

    case 0x02:                                      /* set sample rate */
        if (CPU_BH >= sizeof(sample_rates)) {
            bios_15h_c2_err(cpu, INT15C2_INV_INPUT);
            return true;
        }
        p[0] = sample_rates[CPU_BH];
        if (!ps2_mouse_cmd(cpu, 0xF3, p, 1, 0))
            bios_15h_c2_err(cpu, INT15C2_RESEND);
        else
            bios_15h_c2_ok(cpu);
        return true;

    case 0x03:                                      /* set resolution */
        if (CPU_BH >= 4) {
            bios_15h_c2_err(cpu, INT15C2_INV_INPUT);
            return true;
        }
        p[0] = CPU_BH;
        if (!ps2_mouse_cmd(cpu, 0xE8, p, 1, 0))
            bios_15h_c2_err(cpu, INT15C2_RESEND);
        else
            bios_15h_c2_ok(cpu);
        return true;

    case 0x04:                                      /* get device id */
        if (!ps2_mouse_cmd(cpu, 0xF2, p, 0, 1)) {
            bios_15h_c2_err(cpu, INT15C2_RESEND);
            return true;
        }
        CPU_BH = p[0];
        bios_15h_c2_ok(cpu);
        return true;

    case 0x05:                                      /* initialize */
        if (CPU_BH != 3) {
            bios_15h_c2_err(cpu, INT15C2_INTERFACE);
            return true;
        }
        pstore8(ebda + EBDA_MOUSE_FLAG1, 0x00);
        pstore8(ebda + EBDA_MOUSE_FLAG2, CPU_BH);
        CPU_AL = 0x01;
        return bios_15h_C2h(cpu);

    case 0x06:                                      /* status / scaling */
        if (CPU_BH == 0x00) {
            if (!ps2_mouse_cmd(cpu, 0xE9, p, 0, 3)) {
                bios_15h_c2_err(cpu, INT15C2_RESEND);
                return true;
            }
            CPU_BL = p[0];
            CPU_CL = p[1];
            CPU_DL = p[2];
            bios_15h_c2_ok(cpu);
            return true;
        }
        if (CPU_BH == 0x01 || CPU_BH == 0x02) {
            if (!ps2_mouse_cmd(cpu, CPU_BH == 0x01 ? 0xE6 : 0xE7, p, 0, 0))
                bios_15h_c2_err(cpu, INT15C2_RESEND);
            else
                bios_15h_c2_ok(cpu);
            return true;
        }
        bios_15h_c2_err(cpu, INT15C2_INV_FUNCTION);
        return true;

    case 0x07:                                      /* set handler ES:BX */
        pstore16(ebda + EBDA_MOUSE_HANDLER_OFF, CPU_BX);
        pstore16(ebda + EBDA_MOUSE_HANDLER_SEG, CPU_ES);
        pstore8(ebda + EBDA_MOUSE_FLAG1, 0x00);
        pstore8(ebda + EBDA_MOUSE_FLAG2, CPU_BX || CPU_ES ? 0x83 : 0x03);
        bios_15h_c2_ok(cpu);
        return true;
    }

    bios_15h_c2_err(cpu, INT15C2_INV_FUNCTION);
    return true;
}

#ifndef I386_MODE
bool bios_15h_89h(CPU* cpu)
{
    cf = 1;
    CPU_AH = 0x86;
    return true;
}
bool bios_15h_E820h(CPU* cpu) {
    cf = 1;
    CPU_AH = 0x86;
    return true;
}
#endif

bool bios_15h(CPU* cpu) {
    uint16_t flags_on_stack = readw86((CPU_SS << 4) + CPU_SP + 4);
    bool res = true;
    switch(CPU_AH) {
        case 0x24:
            res = bios_15h_24h(cpu); // A20 GATE
            goto ok;
        case 0x41:
            res = bios_15h_41h(cpu); // WAIT ON EXTERNAL EVENT (CONVERTIBLE and some others)
            goto ok;
        case 0x4F:  /* keyboard intercept — not hooked, pass through */
            cf = 1;  /* не перехватывать, AH не трогаем */
            goto ok;
        case 0x52:  /* TODO: REMOVABLE MEDIA EJECT — SeaBIOS: always success */
            cf = 0;
            CPU_AH = 0x00;
            goto ok;
        case 0x86: { /* WAIT — CX:DX microseconds */
            uint32_t usec = ((uint32_t)CPU_CX << 16) | CPU_DX;
            sleep_us(usec);  /* Pico SDK: busy-waits but yields to hardware */
            cf = 0;
            CPU_AH = 0x00;
            goto ok;
        }
        case 0x83: {
            /*
             * SET EVENT WAIT INTERVAL - асинхронный контракт: возврат
             * НЕМЕДЛЕННЫЙ, бит 7 байта по ES:BX взводится ПОЗЖЕ, по
             * истечении CX:DX микросекунд. SeaBIOS делает это периодикой
             * RTC (src/clock.c, handle_1583 -> IRQ8); здесь завершение
             * проверяет bios_15h_event_wait_tick() из девайс-блока
             * pc_step() с миллисекундной каденцией - точность выше
             * 976-мкс гранулы RTC оригинала не требуется. Синхронная
             * реализация (заснуть на весь интервал внутри трапа и
             * взвести флаг до возврата) ломает вызывающих, которые
             * МЕРЯЮТ что-то, опрашивая флаг: цикл подсчёта ретрейсов
             * 3DAh у Norton SysInfo видел уже взведённый флаг, получал
             * нулевой счёт и ретраил замер вечно, вымораживая гостя
             * sleep_us'ом каждого ретрая.
             * Байт 40:A0 - "wait active", как у оригинала.
             */
            if (CPU_AL == 0x01) {
                bios_15h_event_wait_cancel();
                pstore8(0x4A0, 0x00);
                cf = 0; CPU_AH = 0x00;
                goto ok;
            }
            if (CPU_AL != 0x00) { cf = 1; CPU_AH = 0x86; goto ok; }
            if (pload8(0x4A0) & 0x01) { cf = 1; CPU_AH = 0x83; goto ok; }
            uint32_t usec = ((uint32_t)CPU_CX << 16) | CPU_DX;
            pstore8(0x4A0, 0x01);
            bios_15h_event_wait_arm((uint32_t)CPU_ES * 16 + CPU_BX, usec);
            cf = 0; CPU_AH = 0x00;
            goto ok;
        }

        case 0x87:
            res = bios_15h_87h(cpu); // COPY EXTENDED MEMORY
            goto ok;
        case 0x88:
            res = bios_15h_88h(cpu); // GET EXTENDED MEMORY SIZE (286+)
            goto ok;
        case 0x89:
            res = bios_15h_89h(cpu); // SWITCH TO PROTECTED MODE
            goto ok;
        case 0x90:  /* DEVICE BUSY — no-op (SeaBIOS: empty handler) */
        case 0x91:  /* INTERRUPT COMPLETE — no-op (SeaBIOS: empty handler) */
            goto ok;
        case 0xC0:
            res = bios_15h_C0h(cpu); // GET CONFIGURATION
            goto ok;
        case 0xC1: { /* GET EBDA SEGMENT */
            uint16_t ebda = pload16(0x40E);
            if (ebda == 0x0000) {
                cf = 1;  /* no EBDA */
                goto ok;
            }
            SET_ES ( ebda );
            cf = 0;
            goto ok;
        }            
        case 0xC2:
            res = bios_15h_C2h(cpu); // PS/2 MOUSE BIOS
            goto ok;
        case 0xE8: {
            switch (CPU_AL) {
            case 0x01: { /* GET EXTENDED MEMORY (>16MB support) */
                uint32_t ext_kb = (uint16_t)cmos_read(cpu, 0x17) | ((uint16_t)cmos_read(cpu, 0x18) << 8);
                uint32_t rs = (1024 + ext_kb) * 1024u; /* total RAM in bytes */
                if (rs > 16*1024*1024) {
                    CPU_CX = 15 * 1024;
                    CPU_DX = (uint16_t)((rs - 16*1024*1024) / (64*1024));
                } else {
                    CPU_CX = (uint16_t)((rs - 1*1024*1024) / 1024);
                    CPU_DX = 0;
                }
                CPU_AX = CPU_CX;
                CPU_BX = CPU_DX;
                cf = 0; CPU_AH = 0x00;
                goto ok;
            }
            case 0x20: { /* E820 MEMORY MAP — 286 не поддерживает 32-bit регистры */
                res = bios_15h_E820h(cpu);
                goto ok;
            }
            default:
                cf = 1; CPU_AH = 0x86;
                goto ok;
            }
        }
        default:
            // unsupported
    }
    cf = 1;
    CPU_AH = 0x86;   // unsupported function
ok:
    if (res) {
        flags_on_stack = (flags_on_stack & ~0x0041) // reset ZF, CF
                       | (cpu_getflags(cpu) & 0x0041); // set them back from CPU
        writew86((CPU_SS << 4) + CPU_SP + 4, flags_on_stack);
    }
    return res;
}
