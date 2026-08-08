#include <stdbool.h>
#include <stdint.h>
#include "286/cpu.h"
#include "bios.h"

#define BDA_BASE        0x400u
#define BDA_KBD_FLAGS1  0x417u
#define BDA_KBD_FLAGS2  0x418u
#define BDA_KBD_FLAGS3  0x496u
#define BDA_KBD_HEAD    0x41Au
#define BDA_KBD_TAIL    0x41Cu
#define BDA_KBD_START   0x480u
#define BDA_KBD_END     0x482u

static uint16_t kbd_start(void) { return readw86(BDA_KBD_START); }
static uint16_t kbd_end(void)   { return readw86(BDA_KBD_END);   }

static uint16_t kbd_next(uint16_t p)
{
    p += 2;
    if (p >= kbd_end()) p = kbd_start();
    return p;
}

static bool kbd_empty(void)
{
    return readw86(BDA_KBD_HEAD) == readw86(BDA_KBD_TAIL);
}

static uint16_t kbd_peek(void)
{
    return readw86(BDA_BASE + readw86(BDA_KBD_HEAD));
}

static uint16_t kbd_pop(void)
{
    uint16_t head = readw86(BDA_KBD_HEAD);
    uint16_t ax = readw86(BDA_BASE + head);
    writew86(BDA_KBD_HEAD, kbd_next(head));
    return ax;
}

bool bios_16h_store_key(uint16_t ax)
{
    uint16_t tail = readw86(BDA_KBD_TAIL);
    uint16_t next = kbd_next(tail);

    if (next == readw86(BDA_KBD_HEAD))
        return false;

    writew86(BDA_BASE + tail, ax);
    writew86(BDA_KBD_TAIL, next);
    return true;
}

/* INT 16h AH=00h/01h are the legacy keyboard functions.  Enhanced
 * keyboard keys are stored with AL=E0h for AH=10h/11h, but many old DOS
 * programs only test AL==00h before using AH as the scan code.  Convert
 * the enhanced marker to the legacy marker for AH=00h/01h only. */
static uint16_t kbd_legacy_ax(uint16_t ax)
{
    return ((ax & 0x00ffu) == 0x00e0u) ? (ax & 0xff00u) : ax;
}

bool bios_16h(CPU* cpu)
{
    uint16_t flags_on_stack = readw86((CPU_SS << 4) + CPU_SP + 4);
    switch (CPU_AH) {
    case 0x00: /* read keystroke */
        if (kbd_empty()) {// rеenter to the same call, so sometimes IRQ 1 will call INT 9h and update BDA area
            /* Set IF=1 in the flags word already pushed on stack by intcall86,
            * so that after any IRQ's IRET we still have interrupts enabled. */
            writew86((CPU_SS << 4) + CPU_SP + 4, flags_on_stack | 0x0200); /* IF bit */
            ifl = 1; /* allow IRQs while waiting for keypress */
            return false;
        }
        CPU_AX = kbd_legacy_ax(kbd_pop());
        return true;

    case 0x10: /* enhanced read keystroke */
        if (kbd_empty()) {// rеenter to the same call, so sometimes IRQ 1 will call INT 9h and update BDA area
            /* Set IF=1 in the flags word already pushed on stack by intcall86,
            * so that after any IRQ's IRET we still have interrupts enabled. */
            writew86((CPU_SS << 4) + CPU_SP + 4, flags_on_stack | 0x0200); /* IF bit */
            ifl = 1; /* allow IRQs while waiting for keypress */
            return false;
        }
        CPU_AX = kbd_pop();
        return true;

    case 0x01: /* check keystroke */
        cf = 0;  // ← явно сбросить CF (W/A)
        if (kbd_empty()) {
            zf = 1;
            goto rt;
        }
        CPU_AX = kbd_legacy_ax(kbd_peek());
        zf = 0;
        goto rt;

    case 0x11: /* enhanced check keystroke */
        cf = 0;  // ← явно сбросить CF (W/A)
        if (kbd_empty()) {
            zf = 1;
            goto rt;
        }
        CPU_AX = kbd_peek();
        zf = 0;
        goto rt;

    case 0x02: /* get shift flags */
        CPU_AL = read86(BDA_KBD_FLAGS1);
        return true;

    case 0x05: /* store keystroke: CH=scancode, CL=ASCII */
        CPU_AL = bios_16h_store_key(((uint16_t)CPU_CH << 8) | CPU_CL) ? 0x00 : 0x01;
        return true;

    case 0xFF: /* KBUF extensions: add key to tail, DX=scancode/key word */
        CPU_AL = bios_16h_store_key(CPU_DX) ? 0x00 : 0x01;
        cf = 0;
        goto rt;
    case 0x09: /* GET KEYBOARD FUNCTIONALITY (SeaBIOS handle_1609) */
        CPU_AL = 0x30;  /* bits 5,4: AH=10h-12h + AH=0Ah supported */
        return true;

    case 0x0A: /* GET KEYBOARD ID (SeaBIOS handle_160a) */
        CPU_BX = 0x83AB;  /* SeaBIOS: PS/2 ID bytes AB 83 are returned as BX=83ABh */
        return true;

    case 0x6F: /* keyboard extension probe (SeaBIOS handle_166f) */
        if (CPU_AL == 0x08)
            CPU_AH = 0x02;  /* unsupported extension: normal keyboard */
        return true;

    case 0x12: /* get extended shift flags */
        /* AL = kbd_flag0 (0x417)
         * AH = 0x418 with RCTRL/RALT bits overridden from kbd_flag1 (0x496)
         * SeaBIOS: ax = (kbd_flag0 & ~(KF1_RCTRL|KF1_RALT)<<8) | (kbd_flag1 & (KF1_RCTRL|KF1_RALT))<<8 */
        CPU_AL = read86(BDA_KBD_FLAGS1);
        CPU_AH = (read86(BDA_KBD_FLAGS2) & ~0x0C)
               | (read86(BDA_KBD_FLAGS3) & 0x0C);  /* KF1_RCTRL=0x04, KF1_RALT=0x08 */
        return true;

    case 0x92: /* keyboard capability check (SeaBIOS handle_1692) */
        CPU_AH = 0x80;  /* AH=10h-12h supported */
        return true;

    case 0xA2: /* 122-key capability check (SeaBIOS handle_16a2) */
        /* do nothing: 122-key NOT supported, AH unchanged */
        return true;        

    default:
        CPU_AH = 0x86;
        cf = 1;
        zf = 0;
        goto rt;
    }
rt:
    flags_on_stack = (flags_on_stack & ~0x0041) // reset ZF, CF
                   | (cpu_getflags(cpu) & 0x0041); // set them back from CPU
    writew86((CPU_SS << 4) + CPU_SP + 4, flags_on_stack);
    return true;
}
