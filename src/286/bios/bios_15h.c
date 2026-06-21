#include <pico.h>
#include <pico/time.h>
#include "../cpu.h"
#include "../bios.h"

#define printf(...) bios_printf(cpu, __VA_ARGS__)

// A20 GATE
static bool bios_15h_24h(CPU* cpu) {
    switch (CPU_AL) {
    case 0x00:              /* disable A20 */
        cpu_set_a20(cpu, 0);
        CPU_AH = 0x00;
        cf = 0;
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
            return true;
        }
        ifl = 1; // allow interrupt me by IRQ on next step
        return false;
    }
    bool res = true;
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
            return true;
        }
        ifl = 1; // allow interrupt me by IRQ on next step
        return res;
    }
ok:
    old = -1;
    start_us = 0;
    cf = 0;
    CPU_AH = 0;
    return res;
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
    uint16_t count = CPU_CX;  /* number of WORDS to copy */
    if (count > 0x8000u) {
        CPU_AH = 0x01;  /* invalid parameter */
        cf = 1;
        return true;
    }

    /* Read 24-bit base from descriptor: bytes 2,3,4 */
    uint32_t src = (uint32_t)pload8(tbl + 0x10 + 2)
                 | ((uint32_t)pload8(tbl + 0x10 + 3) << 8)
                 | ((uint32_t)pload8(tbl + 0x10 + 4) << 16);

    uint32_t dst = (uint32_t)pload8(tbl + 0x18 + 2)
                 | ((uint32_t)pload8(tbl + 0x18 + 3) << 8)
                 | ((uint32_t)pload8(tbl + 0x18 + 4) << 16);

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
    cf = 0;
    CPU_AX = (uint16_t)cmos_read(cpu, 0x17) | ((uint16_t)cmos_read(cpu, 0x18) << 8);
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

bool bios_15h(CPU* cpu) {
    switch(CPU_AH) {
        case 0x24:
            return bios_15h_24h(cpu); // A20 GATE
        case 0x41:
            return bios_15h_41h(cpu); // WAIT ON EXTERNAL EVENT (CONVERTIBLE and some others)
        case 0x4F:  /* keyboard intercept — not hooked, pass through */
            cf = 1;  /* не перехватывать, AH не трогаем */
            return true;
        case 0x52:  /* TODO: REMOVABLE MEDIA EJECT — SeaBIOS: always success */
            cf = 0;
            CPU_AH = 0x00;
            return true;
        case 0x86: { /* WAIT — CX:DX microseconds */
            uint32_t usec = ((uint32_t)CPU_CX << 16) | CPU_DX;
            sleep_us(usec);  /* Pico SDK: busy-waits but yields to hardware */
            cf = 0;
            CPU_AH = 0x00;
            return true;
        }
        case 0x83: {
            if (CPU_AL == 0x01) {
                pstore8(0x4A0, 0x00);
                cf = 0; CPU_AH = 0x00;
                return true;
            }
            if (CPU_AL != 0x00) { cf = 1; CPU_AH = 0x86; return true; }
            if (pload8(0x4A0) & 0x01) { cf = 1; CPU_AH = 0x83; return true; }
            uint32_t usec = ((uint32_t)CPU_CX << 16) | CPU_DX;
            pstore8(0x4A0, 0x01);
            sleep_us(usec);
            uint32_t addr = (uint32_t)CPU_ES * 16 + CPU_BX;
            pstore8(addr, pload8(addr) | 0x80);  /* set bit 7 of flag byte */
            pstore8(0x4A0, 0x00);
            cf = 0; CPU_AH = 0x00;
            return true;
        }

        case 0x87:
            return bios_15h_87h(cpu); // COPY EXTENDED MEMORY
        case 0x88:
            return bios_15h_88h(cpu); // GET EXTENDED MEMORY SIZE (286+)
        case 0x90:  /* DEVICE BUSY — no-op (SeaBIOS: empty handler) */
        case 0x91:  /* INTERRUPT COMPLETE — no-op (SeaBIOS: empty handler) */
            return true;
        case 0xC0:
            return bios_15h_C0h(cpu); // GET CONFIGURATION
        case 0xC1: { /* GET EBDA SEGMENT */
            uint16_t ebda = pload16(0x40E);
            if (ebda == 0x0000) {
                cf = 1;  /* no EBDA */
                return true;
            }
            SET_ES ( ebda );
            cf = 0;
            return true;
        }            
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
                return true;
            }
            case 0x20: { /* E820 MEMORY MAP — 286 не поддерживает 32-bit регистры */
                cf = 1; CPU_AH = 0x86;
                return true;
            }
            default:
                cf = 1; CPU_AH = 0x86;
                return true;
            }
        }
        case 0x89:  /* SWITCH TO PROTECTED MODE — not supported, no PM in emulator */
        default:
            // unsupported
    }
    cf = 1;
    CPU_AH = 0x86;   // unsupported function
    return true;
}
