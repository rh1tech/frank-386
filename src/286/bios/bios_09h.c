#include <stdbool.h>
#include <stdint.h>
#include "i286.h"
#include "bios.h"

#define BDA_KBD_FLAGS1  0x417u
#define BDA_KBD_FLAG1   0x496u   /* kbd_flag1: KF1_LAST_E0, KF1_LAST_E1, KF1_RCTRL, KF1_RALT */

#define KBD_FLAG_RSHIFT  0x01u
#define KBD_FLAG_LSHIFT  0x02u
#define KBD_FLAG_CTRL    0x04u
#define KBD_FLAG_ALT     0x08u
#define KBD_FLAG_SCROLL  0x10u
#define KBD_FLAG_NUM     0x20u
#define KBD_FLAG_CAPS    0x40u
#define KBD_FLAG_INS     0x80u

/* kbd_flag1 bits (BDA 0x496, SeaBIOS KF1_*) */
#define KF1_LAST_E1  0x01u
#define KF1_LAST_E0  0x02u
#define KF1_RCTRL    0x04u
#define KF1_RALT     0x08u

/* ROM scratch and stub addresses (pc.c load_bios_and_reset) */
#define IRQ1_SCRATCH  0xFFF70u   /* 1 byte: scan code for INT 15h/4Fh */
#define IRQ1_STUB_CS  0xFFF0u
#define IRQ1_STUB_IP  0x0071u    /* CS:IP = 0xFFF0:0071 → phys 0xFFF71 */

static char scan_to_ascii(uint8_t scan, bool shift, bool ctrl, bool caps)
{
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
bool bios_09h_phase2(void)
{
    uint8_t scan;
    if (!cf) {
        /* intercepted: use modified scan from AL */
        scan = CPU_AL;
    } else {
        /* not intercepted: restore original scan from scratch */
        scan = pload8(IRQ1_SCRATCH);
    }

    if (scan == 0) {
        cpu_portout8(0x20, 0x20);
        return true;
    }

    uint8_t flags  = read86(BDA_KBD_FLAGS1);
    uint8_t flags1 = read86(BDA_KBD_FLAG1);
    bool is_up = (pload8(IRQ1_SCRATCH) & 0x80u) != 0;  /* original break bit */

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
            if (!is_up) flags |= KBD_FLAG_CTRL; else flags &= (uint8_t)~KBD_FLAG_CTRL;
        }
        write86(BDA_KBD_FLAGS1, flags);
        write86(BDA_KBD_FLAG1, flags1);
        goto eoi_return;
    case 0x38: /* Alt */
        if (flags1 & KF1_LAST_E0) {
            if (!is_up) { flags |= KBD_FLAG_ALT;  flags1 |= KF1_RALT; }
            else         { flags &= (uint8_t)~KBD_FLAG_ALT; flags1 &= (uint8_t)~KF1_RALT; }
        } else {
            if (!is_up) flags |= KBD_FLAG_ALT; else flags &= (uint8_t)~KBD_FLAG_ALT;
        }
        write86(BDA_KBD_FLAGS1, flags);
        write86(BDA_KBD_FLAG1, flags1);
        goto eoi_return;
    case 0x3A: /* Caps Lock */
        if (!is_up) { flags ^= KBD_FLAG_CAPS; write86(BDA_KBD_FLAGS1, flags); }
        goto eoi_return;
    case 0x45: /* Num Lock */
        if (flags1 & KF1_LAST_E1) goto eoi_return;  /* Pause key — ignore */
        if (!is_up) { flags ^= KBD_FLAG_NUM; write86(BDA_KBD_FLAGS1, flags); }
        goto eoi_return;
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
        }
        if (!is_up) { flags ^= KBD_FLAG_SCROLL; write86(BDA_KBD_FLAGS1, flags); }
        goto eoi_return;
    case 0x52: /* Insert */
        if (!is_up && !(flags1 & KF1_LAST_E0)) {
            flags ^= KBD_FLAG_INS;
            write86(BDA_KBD_FLAGS1, flags);
        }
        break;
    case 0x53: /* Delete — Ctrl+Alt+Del = reboot */
        if (!is_up
            && (flags & KBD_FLAG_CTRL)
            && (flags & KBD_FLAG_ALT)) {
            writew86(0x472, 0x1234);  /* soft_reset_flag = warm boot */
            CPU_CS = 0xFFFF;
            CPU_IP = 0x0000;          /* jump to reset vector */
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
        uint16_t ax = ((uint16_t)scan << 8) | ascii;

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
    /* Clear E0/E1 prefix flags */
    write86(BDA_KBD_FLAG1, read86(BDA_KBD_FLAG1) & (uint8_t)~(KF1_LAST_E0 | KF1_LAST_E1));
    cpu_portout8(0x20, 0x20);
    return true;
}

/* Phase 1: read scan code, save in scratch, redirect to INT 15h/4Fh stub.
 * Returns false → main loop continues execution at stub (CS:IP set here). */
bool bios_09h(void)
{
    /* Check OBF (Output Buffer Full) in i8042 status register.
     * If clear — spurious IRQ1, nothing to read. */
    if (!(cpu_portin8(0x64) & 0x01)) {
        cpu_portout8(0x20, 0x20);
        return true;
    }

    uint8_t code = cpu_portin8(0x60);

    /* E0/E1 prefix: store in kbd_flag1 (BDA 0x496), SeaBIOS-compatible */
    if (code == 0xE0 || code == 0xE1) {
        uint8_t f1 = read86(BDA_KBD_FLAG1);
        f1 &= (uint8_t)~(KF1_LAST_E0 | KF1_LAST_E1);
        f1 |= (code == 0xE0) ? KF1_LAST_E0 : KF1_LAST_E1;
        write86(BDA_KBD_FLAG1, f1);
        cpu_portout8(0x20, 0x20);
        return true;
    }

    /* Save raw code (with break bit) in scratch for phase2 */
    pstore8(IRQ1_SCRATCH, code);

    /* Redirect to INT 15h/4Fh stub in ROM */
    CPU_CS = IRQ1_STUB_CS;
    CPU_IP = IRQ1_STUB_IP;
    return false;
}
