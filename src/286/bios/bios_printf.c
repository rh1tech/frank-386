#include "../bios.h"
#include "../cpu.h"
#include <stdio.h>

/*
 * Вывести один символ через INT 10h AH=0Eh.
 * BH=0 (page 0), BL=07h (атрибут, учитывается только в графических режимах).
 */
inline static void bios_putchar(CPU *cpu, char c)
{
 
    CPU_AH = 0x0E;
    CPU_AL = (uint8_t)c;
    CPU_BH = 0x00;   /* страница 0 */
    CPU_BL = 0x07;   /* атрибут (серый на чёрном) */
    bios_10h(cpu);
}

void bios_printf(CPU* cpu, const char *fmt, ...) {
    char buf[256];
    va_list ap;
     va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
 
    uint16_t ax = CPU_AX;
    uint16_t bx = CPU_BX;
    for (const char *p = buf; *p; ++p) {
        bios_putchar(cpu, *p);
    }
    CPU_AX = ax;
    CPU_BX = bx;
}
