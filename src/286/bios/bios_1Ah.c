#include "i286.h"
#include "bios.h"

/* INT 1Ah  –  Time/Date services
 *
 * AH=00h  Get System Time       CX:DX = ticks since midnight, AL = midnight flag
 * AH=01h  Set System Time       CX:DX = ticks to set
 * AH=02h  Get RTC Time          CH=hours, CL=min, DH=sec (BCD), CF=0 ok / CF=1 RTC lost power
 * AH=03h  Set RTC Time          CH=hours, CL=min, DH=sec, DL=DST (BCD)
 * AH=04h  Get RTC Date          CH=century, CL=year, DH=month, DL=day (BCD), CF=0 ok
 * AH=05h  Set RTC Date          CH=century, CL=year, DH=month, DL=day (BCD)
 *
 * BDA layout (used by AH=00h/01h):
 *   0x046C  dword  ticks since midnight (incremented by INT 08h at 18.2 Hz)
 *   0x0470  byte   midnight rollover flag (set by INT 08h, cleared here on read)
 *
 * CMOS access (used by AH=02h..05h):
 *   Registers read/written via I/O ports 0x70 (index) / 0x71 (data).
 *   All values are BCD as stored by the emulated MC146818.
 *   CMOS reg map: 00=sec, 02=min, 04=hour, 06=DOW, 07=day, 08=month, 09=year, 0x32=century
 */
bool bios_1Ah(void)
{
    switch (CPU_AH) {

    /* ── AH=00h: Get System Time ──────────────────────────────────────── */
    case 0x00: {
        uint32_t ticks = pload32(0x046C);
        CPU_CX = (uint16_t)(ticks >> 16);
        CPU_DX = (uint16_t)(ticks & 0xFFFF);
        CPU_AL = pload8(0x0470);   /* midnight rollover flag */
        pstore8(0x0470, 0);        /* clear after read (IBM BIOS behaviour) */
        cf = 0;
        break;
    }

    /* ── AH=01h: Set System Time ──────────────────────────────────────── */
    case 0x01: {
        uint32_t ticks = ((uint32_t)CPU_CX << 16) | CPU_DX;
        pstore32(0x046C, ticks);
        pstore8(0x0470, 0);
        cf = 0;
        break;
    }

    /* ── AH=02h: Get RTC Time ─────────────────────────────────────────── */
    case 0x02: {
        /* CF=1 if RTC lost power (REG_D bit 7 = VRT, 0 means battery dead) */
        if (!(cmos_read(0x0D) & 0x80)) {
            cf = 1;
            break;
        }
        CPU_CH = cmos_read(0x04); /* hours   BCD */
        CPU_CL = cmos_read(0x02); /* minutes BCD */
        CPU_DH = cmos_read(0x00); /* seconds BCD */
        CPU_DL = 0;               /* DST: not supported */
        cf = 0;
        break;
    }

    /* ── AH=03h: Set RTC Time ─────────────────────────────────────────── */
    case 0x03: {
        cmos_write(0x04, CPU_CH); /* hours   BCD */
        cmos_write(0x02, CPU_CL); /* minutes BCD */
        cmos_write(0x00, CPU_DH); /* seconds BCD */
        cf = 0;
        break;
    }

    /* ── AH=04h: Get RTC Date ─────────────────────────────────────────── */
    case 0x04: {
        if (!(cmos_read(0x0D) & 0x80)) {
            cf = 1;
            break;
        }
        CPU_CH = cmos_read(0x32); /* century BCD */
        CPU_CL = cmos_read(0x09); /* year    BCD */
        CPU_DH = cmos_read(0x08); /* month   BCD */
        CPU_DL = cmos_read(0x07); /* day     BCD */
        cf = 0;
        break;
    }

    /* ── AH=05h: Set RTC Date ─────────────────────────────────────────── */
    case 0x05: {
        cmos_write(0x32, CPU_CH); /* century BCD */
        cmos_write(0x09, CPU_CL); /* year    BCD */
        cmos_write(0x08, CPU_DH); /* month   BCD */
        cmos_write(0x07, CPU_DL); /* day     BCD */
        cf = 0;
        break;
    }

    default:
        CPU_AH = 0x86;
        cf = 1;
        break;
    }

    return true;
}
