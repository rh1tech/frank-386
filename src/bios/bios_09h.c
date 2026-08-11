#include <stdbool.h>
#include <stdint.h>
#include "286/cpu.h"
#include "bios.h"

#define printf(...) bios_printf(cpu, __VA_ARGS__)

#define BDA_KBD_FLAGS1  0x417u
#define BDA_KBD_FLAGS2  0x418u   /* upper byte of SeaBIOS kbd_flag0 */
#define BDA_KBD_FLAG1   0x496u   /* kbd_flag1: KF1_LAST_E0, KF1_LAST_E1, KF1_RCTRL, KF1_RALT */

#define KBD_FLAG_RSHIFT  0x01u
#define KBD_FLAG_LSHIFT  0x02u
#define KBD_FLAG_CTRL    0x04u
#define KBD_FLAG_ALT     0x08u
#define KBD_FLAG_SCROLL  0x10u
#define KBD_FLAG_NUM     0x20u
#define KBD_FLAG_CAPS    0x40u
#define KBD_FLAG_INS     0x80u

/* BDA 40:18, upper byte of SeaBIOS kbd_flag0 (KF0_* >> 8). */
#define KBD_FLAG2_LCTRL   0x01u
#define KBD_FLAG2_LALT    0x02u
#define KBD_FLAG2_PAUSE   0x08u
#define KBD_FLAG2_SCROLL  0x10u
#define KBD_FLAG2_NUM     0x20u
#define KBD_FLAG2_CAPS    0x40u

/* kbd_flag1 bits (BDA 0x496, SeaBIOS KF1_*) */
#define KF1_LAST_E1  0x01u
#define KF1_LAST_E0  0x02u
#define KF1_RCTRL    0x04u
#define KF1_RALT     0x08u

/* ROM scratch and stub addresses (pc.c load_bios_and_reset) */
#define IRQ1_SCRATCH  0xFFF70u   /* 1 byte: scan code for INT 15h/4Fh */
#define IRQ1_STUB_CS  0xFFF0u
#define IRQ1_STUB_IP  0x0071u    /* CS:IP = 0xFFF0:0071 → phys 0xFFF71 */

static uint16_t translate_bios_key(uint8_t scan, uint8_t ascii, uint8_t flags) {
    bool shift = (flags & (KBD_FLAG_LSHIFT | KBD_FLAG_RSHIFT)) != 0;
    bool ctrl  = (flags & KBD_FLAG_CTRL) != 0;
    bool alt   = (flags & KBD_FLAG_ALT) != 0;
    /* BIOS-compatible scan codes for modified function keys.
     * F1..F10 are 3B00..4400 without modifiers. Since these keys already
     * have ASCII=00h, Alt/Ctrl/Shift are encoded by changing the scan byte.
     */
    if (scan >= 0x3B && scan <= 0x44) {
        if (alt)
            return (uint16_t)(0x68u + (scan - 0x3Bu)) << 8;
        if (ctrl)
            return (uint16_t)(0x5Eu + (scan - 0x3Bu)) << 8;
        if (shift)
            return (uint16_t)(0x54u + (scan - 0x3Bu)) << 8;
    }
    /* F11/F12 enhanced keyboard scan codes. */
    if (scan == 0x57 || scan == 0x58) {
        uint8_t n = scan - 0x57u;
        if (alt)
            return (uint16_t)(0x8Bu + n) << 8;
        if (ctrl)
            return (uint16_t)(0x89u + n) << 8;
        if (shift)
            return (uint16_t)(0x87u + n) << 8;
        return (uint16_t)(0x85u + n) << 8;
    }
    return ((uint16_t)scan << 8) | ascii;
}

static char scan_to_ascii(uint8_t scan, bool shift, bool ctrl, bool caps)
{
    /* Keypad operator keys have BIOS ASCII even though their set-1
     * scan codes are outside the main alphanumeric table.  Some DOS
     * programs ignore scan-only 4A00/4E00 and expect 4A2D/4E2B. */
    switch (scan) {
    case 0x37: return '*';
    case 0x4A: return '-';
    case 0x4E: return '+';
    default: break;
    }    
    if (ctrl) {
        char base;
        /* Ctrl+A..Ctrl+Z */
        static const char normal_for_ctrl[] = {
            0, 0, '1','2','3','4','5','6','7','8','9','0','-','=', 0, 0,
            'q','w','e','r','t','y','u','i','o','p','[',']', 0, 0,
            'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
            'z','x','c','v','b','n','m',',','.','/'\
        };
        if (scan < sizeof(normal_for_ctrl)) {
            base = normal_for_ctrl[scan];
            if (base >= 'a' && base <= 'z')
                return (char)(base - 'a' + 1);
        }
        switch (scan) {
        case 0x1C: return 0x0A; /* Ctrl+Enter */
        case 0x0E: return 0x7F; /* Ctrl+Backspace */
        default: break;
        }
    }

    switch (scan) {
    case 0x01: return 0x1B;
    case 0x0E: return 0x08;
    case 0x0F: return 0x09;
    case 0x1C: return 0x0D;
    case 0x39: return ' ';
    }

    static const char normal[] = {
        0, 0, '1','2','3','4','5','6','7','8','9','0','-','=', 0, 0,
        'q','w','e','r','t','y','u','i','o','p','[',']', 0, 0,
        'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
        'z','x','c','v','b','n','m',',','.','/'\
    };
    static const char shifted[] = {
        0, 0, '!','@','#','$','%','^','&','*','(',')','_','+', 0, 0,
        'Q','W','E','R','T','Y','U','I','O','P','{','}', 0, 0,
        'A','S','D','F','G','H','J','K','L',':','"','~', 0,'|',
        'Z','X','C','V','B','N','M','<','>','?'\
    };

    if (scan >= sizeof(normal))
        return 0;

    char c = shift ? shifted[scan] : normal[scan];
    if (c >= 'a' && c <= 'z' && caps)
        c = (char)(c - 'a' + 'A');
    else if (c >= 'A' && c <= 'Z' && caps)
        c = (char)(c - 'A' + 'a');
    return c;
}

/* Phase 2: called via INT 77h after INT 15h/4Fh returns from guest.
 * CF and CPU_AL are the result of the INT 15h/4Fh call.
 * Continues scan code processing. */
static bool bios_09h_phase2(CPU* cpu, bios_callback_params_t* params)
{
    /* SeaBIOS INT 15h/AH=4Fh contract:
     *   CF clear -> the intercept handler consumed the byte; BIOS stops here.
     *   CF set   -> continue with AL, which the handler may have modified.
     *
     * SeaBIOS performs this call on a separate bregs object.  Capture the only
     * two defined results first, then restore the interrupted register image so
     * a guest INT 15h hook cannot leak BX/CX/DX/SI/DI/BP/segments/FLAGS into
     * the interrupted application.
     */
    const bool intercepted = !cf;
    const uint8_t raw = CPU_AL;
    CPU_regs *saved = (CPU_regs *)params->data;
    if (saved)
        cpu_restore_regs(cpu, saved);

    if (intercepted) {
        /* SeaBIOS handle_09(): some old programs expect the ISR to
         * turn the keyboard interface back on after process_key(). */
        cpu_portout8(0x64, 0xAE);
        cpu_portout8(0x20, 0x20);
        return true;
    }

    /* SeaBIOS passes E0/E1 through INT 15h/4Fh as well, and only records
     * the prefix after the intercept handler allows it through. */
    if (raw == 0xE0 || raw == 0xE1) {
        uint8_t f1 = read86(BDA_KBD_FLAG1);
        f1 &= (uint8_t)~(KF1_LAST_E0 | KF1_LAST_E1);
        f1 |= (raw == 0xE0) ? KF1_LAST_E0 : KF1_LAST_E1;
        write86(BDA_KBD_FLAG1, f1);
        cpu_portout8(0x64, 0xAE);
        cpu_portout8(0x20, 0x20);
        return true;
    }

    uint8_t scan = raw & 0x7Fu;
    //printf("[09] scan: %02xh raw: %02xh\n", scan, raw);

    if (scan == 0) {
        cpu_portout8(0x64, 0xAE);
        cpu_portout8(0x20, 0x20);
        return true;
    }

    uint8_t flags  = read86(BDA_KBD_FLAGS1);
    uint8_t flags1 = read86(BDA_KBD_FLAG1);
    bool is_up = (raw & 0x80u) != 0;

    ///printf("[09] raw=%02X scan=%02X flags=%02X flags1=%02X cf=%d al=%02X\n",
    ///   read86(IRQ1_SCRATCH), scan, flags, flags1, cf, CPU_AL);
    switch (scan) {
    case 0x2A: /* L Shift */
        if (flags1 & KF1_LAST_E0) break;  /* ignore fake shift (SeaBIOS) */
        if (!is_up) flags |= KBD_FLAG_LSHIFT; else flags &= (uint8_t)~KBD_FLAG_LSHIFT;
        write86(BDA_KBD_FLAGS1, flags);
        goto eoi_return;
    case 0x36: /* R Shift */
        if (flags1 & KF1_LAST_E0) break;  /* ignore fake shift */
        if (!is_up) flags |= KBD_FLAG_RSHIFT; else flags &= (uint8_t)~KBD_FLAG_RSHIFT;
        write86(BDA_KBD_FLAGS1, flags);
        goto eoi_return;
    case 0x1D: /* Ctrl */
        if (flags1 & KF1_LAST_E0) {
            if (!is_up) { flags |= KBD_FLAG_CTRL;  flags1 |= KF1_RCTRL; }
            else         { flags &= (uint8_t)~KBD_FLAG_CTRL; flags1 &= (uint8_t)~KF1_RCTRL; }
        } else {
            uint8_t flags2 = read86(BDA_KBD_FLAGS2);
            if (!is_up) {
                flags |= KBD_FLAG_CTRL;
                flags2 |= KBD_FLAG2_LCTRL;
            } else {
                flags &= (uint8_t)~KBD_FLAG_CTRL;
                flags2 &= (uint8_t)~KBD_FLAG2_LCTRL;
            }
            write86(BDA_KBD_FLAGS2, flags2);
        }
        write86(BDA_KBD_FLAGS1, flags);
        write86(BDA_KBD_FLAG1, flags1);
        goto eoi_return;
    case 0x38: /* Alt */
        if (flags1 & KF1_LAST_E0) {
            if (!is_up) { flags |= KBD_FLAG_ALT;  flags1 |= KF1_RALT; }
            else         { flags &= (uint8_t)~KBD_FLAG_ALT; flags1 &= (uint8_t)~KF1_RALT; }
        } else {
            uint8_t flags2 = read86(BDA_KBD_FLAGS2);
            if (!is_up) {
                flags |= KBD_FLAG_ALT;
                flags2 |= KBD_FLAG2_LALT;
            } else {
                flags &= (uint8_t)~KBD_FLAG_ALT;
                flags2 &= (uint8_t)~KBD_FLAG2_LALT;
            }
            write86(BDA_KBD_FLAGS2, flags2);
        }
        write86(BDA_KBD_FLAGS1, flags);
        write86(BDA_KBD_FLAG1, flags1);
        goto eoi_return;
    case 0x3A: { /* Caps Lock */
        uint8_t flags2 = read86(BDA_KBD_FLAGS2);
        if (!is_up) {
            flags ^= KBD_FLAG_CAPS;
            flags2 |= KBD_FLAG2_CAPS;
        } else {
            flags2 &= (uint8_t)~KBD_FLAG2_CAPS;
        }
        write86(BDA_KBD_FLAGS1, flags);
        write86(BDA_KBD_FLAGS2, flags2);
        goto eoi_return;
    }
    case 0x45: { /* Num Lock */
        if (flags1 & KF1_LAST_E1) goto eoi_return;  /* Pause key — ignore */
        uint8_t flags2 = read86(BDA_KBD_FLAGS2);
        if (!is_up) {
            flags ^= KBD_FLAG_NUM;
            flags2 |= KBD_FLAG2_NUM;
        } else {
            flags2 &= (uint8_t)~KBD_FLAG2_NUM;
        }
        write86(BDA_KBD_FLAGS1, flags);
        write86(BDA_KBD_FLAGS2, flags2);
        goto eoi_return;
    }
    case 0x46: /* Scroll Lock */
        if (flags1 & KF1_LAST_E0) {
            /* E0+46 = Ctrl+Break */
            if (is_up) {
                uint16_t buf_start = readw86(0x480);
                writew86(0x41A, buf_start);
                writew86(0x41C, buf_start + 2);
                writew86(0x400 + buf_start, 0x0000);
                write86(0x471, 0x80);  /* break_flag */
            }
            goto eoi_return;
        } else {
            uint8_t flags2 = read86(BDA_KBD_FLAGS2);
            if (!is_up) {
                flags ^= KBD_FLAG_SCROLL;
                flags2 |= KBD_FLAG2_SCROLL;
            } else {
                flags2 &= (uint8_t)~KBD_FLAG2_SCROLL;
            }
            write86(BDA_KBD_FLAGS1, flags);
            write86(BDA_KBD_FLAGS2, flags2);
            goto eoi_return;
        }
    case 0x52: /* Insert */
        if (!is_up && !(flags1 & KF1_LAST_E0)) {
            flags ^= KBD_FLAG_INS;
            write86(BDA_KBD_FLAGS1, flags);
        }
        break;
    case 0x53: /* Delete — Ctrl+Alt+Del = reboot */
        if (!is_up && (flags & KBD_FLAG_CTRL) && (flags & KBD_FLAG_ALT)) {
            writew86(0x472, 0x1234);  /* warm boot flag */
            cpu_portout8(0x20, 0x20); /* EOI */
            cpu_portout8(0x64, 0xFE); /* reset via keyboard controller */
            return true;
        }
        break;
    default:
        break;
    }

    if (!is_up) {
        bool shift = (flags & (KBD_FLAG_LSHIFT | KBD_FLAG_RSHIFT)) != 0;
        bool ctrl  = (flags & KBD_FLAG_CTRL) != 0;
        bool caps  = (flags & KBD_FLAG_CAPS) != 0;
        bool num   = (flags & KBD_FLAG_NUM) != 0;

        /* SeaBIOS: NumLock inverts shift for numpad keys 0x47..0x53 */
        if (num && scan >= 0x47 && scan <= 0x53)
            shift ^= 1;
        
        uint8_t ascii = (uint8_t)scan_to_ascii(scan, shift, ctrl, caps);
        uint16_t ax = translate_bios_key(scan, ascii, flags);

        if (flags1 & KF1_LAST_E0) {
            /* E0+1Ch = extended Enter → 0xE00D (SeaBIOS key_ext_enter) */
            if (scan == 0x1C)
                ax = 0xE00D;
            /* E0+35h = numpad slash → 0xE02F (SeaBIOS key_ext_slash) */
            else if (scan == 0x35)
                ax = 0xE02F;
            /* other extended keys: replace ASCII with 0xE0 (SeaBIOS) */
            else
                ax = (ax & 0xFF00u) | 0x00E0u;
        }

        if (ax != 0)
            bios_16h_store_key(ax);
    }

eoi_return:
    /* SeaBIOS handle_09(): some old programs expect ISR to turn the
     * keyboard interface back on after process_key(). */
    cpu_portout8(0x64, 0xAE);

    /* Clear E0/E1 prefix flags */
    write86(BDA_KBD_FLAG1, read86(BDA_KBD_FLAG1) & (uint8_t)~(KF1_LAST_E0 | KF1_LAST_E1));
    cpu_portout8(0x20, 0x20);
    return true;
}

/*
 * SeaBIOS process_key() invokes INT 15h/4Fh through a separate bregs frame.
 * Keep the interrupted program's register image separate from the intercept
 * call as well; only CF and AL are results of the 4Fh contract.
 */
static CPU_regs irq1_saved_regs;
static bios_callback_params_t params = {
    .callback = bios_09h_phase2,
    .expected_cs = 0xFFE0,
    .expected_ip = 0x00FF,
    .data = &irq1_saved_regs,
    .owner = "INT 09H"
};

/* Phase 1: read scan code, save in scratch, redirect to INT 15h/4Fh stub.
 * Returns false → main loop continues execution at stub (CS:IP set here). */
bool bios_09h(CPU* cpu)
{
    /* SeaBIOS handle_09(): reject AUX data, but do not require OBF.
     * Chained legacy handlers may already have consumed the keyboard byte. */
    uint8_t status = cpu_portin8(0x64);
    if (status & 0x20u) {
        cpu_portout8(0x20, 0x20);
        return true;
    }

    /*
     * A real hardware INT 09h enters through intcall86(), which clears IF.
     * Prince 1.4 is different: its IRQ1 hook reads port 60h, executes STI,
     * then chains to the previous INT 09h with PUSHF/CALL FAR.  Therefore the
     * chained BIOS entry has OBF clear but IF set.
     *
     * Do not replay the i8042 last-byte latch for an OBF-clear entry that
     * still has IF clear.  That is a spurious/duplicate hardware entry;
     * replaying the stale byte can inject a phantom key into the BIOS buffer.
     */
    if (!(status & 0x01u) && !ifl) {
        cpu_portout8(0x20, 0x20);
        return true;
    }

    uint8_t code = cpu_portin8(0x60);
    ///printf("[09p1] code=%02X st=%02X csip=%04X:%04X\n", code, cpu_portin8(0x64), CPU_CS, CPU_IP);

    /* Save raw code for diagnostics/fallback and invoke INT 15h/4Fh for
     * every keyboard byte, including E0/E1 prefixes, as SeaBIOS does. */
    write86(IRQ1_SCRATCH, code);
    cpu_save_regs(cpu, &irq1_saved_regs);
    set_bios_callback(cpu, &params, false);

    /*
     * Approximate SeaBIOS' zero-initialized bregs input without replacing the
     * real-mode stack used by INT itself.  AH/AL and CF are the only defined
     * inputs to INT 15h/4Fh; SP/SS must remain live for the interrupt frame.
     */
    CPU_BX = 0;
    CPU_CX = 0;
    CPU_DX = 0;
    CPU_SI = 0;
    CPU_DI = 0;
    CPU_BP = 0;
    SET_DS(0);
    SET_ES(0);
    CPU_AX = 0x4F00 | code;
    cf = 1;
    SET_CS ( IRQ1_STUB_CS );
    SET_IP ( IRQ1_STUB_IP );
    return false;
}
