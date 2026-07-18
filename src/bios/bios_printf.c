#include "bios.h"
#include "286/cpu.h"
#include <stdio.h>

void bios_puts(CPU* cpu, const char* str) {
    u8 al = CPU_AL;
    while(*str) {
        if (*str == '\n') {
            bios_teletype(cpu, '\r', 0);
        }
        CPU_AL = *str;
        bios_teletype(cpu, *str, 0);
        str++;
    }
    CPU_AL = al;
}

void bios_printf(CPU* cpu, const char *fmt, ...) {
    /* static, а не кадр: 256 байт стека в трап-контексте core0 - дорого.
       Реентерабельность не нужна: bios_puts() -> bios_teletype() - чистая
       нативная функция, гостевой код она не исполняет, а значит вернуться
       сюда до выхода не может. */
    static char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bios_puts(cpu, buf);
}
