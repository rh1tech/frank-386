#include "286/cpu.h"
#include "bios.h"

#define BDA_LPT_BASE(n)      (0x408 + ((n) * 2))
#define BDA_LPT_TIMEOUT(n)   (0x478 + (n))

static uint16_t lpt_base(unsigned no) {
    if (no >= 3) return 0;
    return pload16(BDA_LPT_BASE(no));
}

static uint8_t lpt_status_bios(CPU* cpu, uint16_t base) {
    uint8_t s = cpu_portin8(base + 1);

    /*
       LPT status port:
       bit7 = not busy
       bit6 = ack
       bit5 = paper out
       bit4 = selected
       bit3 = /error, inverted for BIOS bit3
       
       BIOS INT 17h AH:
       bit7 = not busy
       bit6 = ack
       bit5 = out of paper
       bit4 = selected
       bit3 = I/O error
       bit0 = timeout
    */
    return (s & 0xF0) | ((s & 0x08) ? 0x00 : 0x08);
}

static bool lpt_ready(uint8_t bios_status) {
    return (bios_status & 0x80) &&   /* not busy */
           (bios_status & 0x10) &&   /* selected */
          !(bios_status & 0x20) &&   /* no paper-out */
          !(bios_status & 0x08);     /* no error */
}

static uint8_t lpt_wait_ready(CPU* cpu, uint16_t base, unsigned no) {
    uint8_t timeout = pload8(BDA_LPT_TIMEOUT(no));
    uint32_t limit = timeout ? ((uint32_t)timeout << 12) : 0x1000;

    uint8_t st;
    do {
        st = lpt_status_bios(cpu, base);
        if (lpt_ready(st))
            return st;
    } while (--limit);

    return st | 0x01; /* timeout */
}

// PRINTERS - INT 17h
bool bios_17h(CPU* cpu) {
    uint8_t fn = CPU_AH;
    unsigned no = CPU_DX & 0xFF;
    uint16_t base = lpt_base(no);

    if (!base) {
        CPU_AH = 0x80 | 0x01; /* not busy + timeout условно */
        return true;
    }

    switch (fn) {
    case 0x00: { /* print character, AL = char */
        uint8_t st = lpt_wait_ready(cpu, base, no);
        if (st & 0x01) {
            CPU_AH = st;
            return true;
        }

        cpu_portout8(base + 0, CPU_AL);

        uint8_t ctrl = cpu_portin8(base + 2);
        cpu_portout8(base + 2, ctrl | 0x01);   /* strobe on */
        cpu_portout8(base + 2, ctrl & ~0x01);  /* strobe off */

        CPU_AH = lpt_status_bios(cpu, base);
        break;
    }

    case 0x01: { /* initialize printer */
        uint8_t ctrl = cpu_portin8(base + 2);

        cpu_portout8(base + 2, (ctrl & ~0x04)); /* INIT low */
        for (volatile int i = 0; i < 256; ++i) { }
        cpu_portout8(base + 2, (ctrl | 0x0C));  /* INIT high + select */

        CPU_AH = lpt_status_bios(cpu, base);
        break;
    }

    case 0x02: /* get status */
        CPU_AH = lpt_status_bios(cpu, base);
        break;

    default:
        CPU_AH = lpt_status_bios(cpu, base) | 0x01;
        break;
    }

    return true;
}
