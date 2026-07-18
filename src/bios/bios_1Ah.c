#include "286/cpu.h"
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
bool bios_1Ah(CPU* cpu)
{
    uint16_t flags_on_stack = readw86((CPU_SS << 4) + CPU_SP + 4);
    switch (CPU_AH) {

    /* ── AH=00h: Get System Time ──────────────────────────────────────── */
    case 0x00: {
        uint32_t ticks = pload32(0x046C);
        CPU_CX = (uint16_t)(ticks >> 16);
        CPU_DX = (uint16_t)(ticks & 0xFFFF);
        CPU_AL = pload8(0x0470);   /* midnight rollover flag */
        pstore8(0x0470, 0);        /* clear after read (IBM BIOS behaviour) */
        cf = 0;
        goto ret;
    }

    /* ── AH=01h: Set System Time ──────────────────────────────────────── */
    case 0x01: {
        uint32_t ticks = ((uint32_t)CPU_CX << 16) | CPU_DX;
        pstore32(0x046C, ticks);
        pstore8(0x0470, 0);
        cf = 0;
        goto ret;
    }

    /* ── AH=02h: Get RTC Time ─────────────────────────────────────────── */
    case 0x02: {
        /* SeaBIOS handle_1a02(): негодность определяется по UIP (REG_A
           бит 7, "идёт обновление"), а не по VRT - тот говорит лишь о
           состоянии батареи. В этой эмуляции UIP не выставляется никогда
           (misc.c обновляет регистры атомарно), так что проверка всегда
           проходит; важна семантика, совпадающая с эталоном. */
        if (cmos_read(cpu, 0x0A) & 0x80) {
            cf = 1;
            goto ret;
        }
        CPU_CH = cmos_read(cpu, 0x04); /* hours   BCD */
        CPU_CL = cmos_read(cpu, 0x02); /* minutes BCD */
        CPU_DH = cmos_read(cpu, 0x00); /* seconds BCD */
        CPU_DL = cmos_read(cpu, 0x0B) & 0x01; /* DSE из STATUS_B */
        CPU_AH = 0x00;
        CPU_AL = CPU_CH;               /* SeaBIOS: AL = часы */
        cf = 0;
        goto ret;
    }

    /* ── AH=03h: Set RTC Time ─────────────────────────────────────────── */
    case 0x03: {
        uint8_t dl = CPU_DL;
        cmos_write(cpu, 0x00, CPU_DH); /* seconds BCD */
        cmos_write(cpu, 0x02, CPU_CL); /* minutes BCD */
        cmos_write(cpu, 0x04, CPU_CH); /* hours   BCD */
        /* SeaBIOS handle_1a03(): RegB = (RegB & (PIE|AIE)) | 24HR |
           (DL & DSE) - сбрасывает SET/UIE/BIN, сохраняет периодические и
           будильниковые прерывания, переносит признак летнего времени. */
        uint8_t b = (uint8_t)((cmos_read(cpu, 0x0B) & 0x60) | 0x02 | (dl & 0x01));
        cmos_write(cpu, 0x0B, b);
        CPU_AH = 0x00;
        CPU_AL = b;                    /* последнее записанное в RegB */
        cf = 0;
        goto ret;
    }

    /* ── AH=04h: Get RTC Date ─────────────────────────────────────────── */
    case 0x04: {
        CPU_AH = 0x00;
        if (cmos_read(cpu, 0x0A) & 0x80) {  /* UIP, как в SeaBIOS */
            cf = 1;
            goto ret;
        }
        CPU_CH = cmos_read(cpu, 0x32); /* century BCD */
        CPU_CL = cmos_read(cpu, 0x09); /* year    BCD */
        CPU_DH = cmos_read(cpu, 0x08); /* month   BCD */
        CPU_DL = cmos_read(cpu, 0x07); /* day     BCD */
        CPU_AL = CPU_CH;               /* SeaBIOS: AL = век */
        cf = 0;
        goto ret;
    }

    /* ── AH=05h: Set RTC Date ─────────────────────────────────────────── */
    case 0x05: {
        cmos_write(cpu, 0x32, CPU_CH); /* century BCD */
        cmos_write(cpu, 0x09, CPU_CL); /* year    BCD */
        cmos_write(cpu, 0x08, CPU_DH); /* month   BCD */
        cmos_write(cpu, 0x07, CPU_DL); /* day     BCD */
        /* SeaBIOS handle_1a05(): снять бит SET (остановка часов). */
        uint8_t b5 = (uint8_t)(cmos_read(cpu, 0x0B) & ~0x80);
        cmos_write(cpu, 0x0B, b5);
        CPU_AH = 0x00;
        CPU_AL = b5;
        cf = 0;
        goto ret;
    }

    default:
        CPU_AH = 0x86;
        cf = 1;
        goto ret;
    }
ret:
    flags_on_stack = (flags_on_stack & ~0x0041) // reset ZF, CF
                   | (cpu_getflags(cpu) & 0x0041); // set them back from CPU
    writew86((CPU_SS << 4) + CPU_SP + 4, flags_on_stack);
    return true;
}
