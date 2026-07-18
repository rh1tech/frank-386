#include "286/cpu.h"
#include "bios.h"
#include "ff.h"
#include <stdlib.h>

#define PRINTER_MAX_COLS 160

static bool text_mode(uint8_t mode) {
    return mode == 0x00 || mode == 0x01 ||
           mode == 0x02 || mode == 0x03 ||
           mode == 0x07;
}

/*
PRINT SCREEN
Desc: Dump the current text screen to the first printer

Notes: Normally invoked by the INT 09 handler when PrtSc key is pressed, but may be invoked directly by applications.
Byte at 0050h:0000h contains status used by default handler 00h not active 01h PrtSc in progress FFh last PrtSc encountered error.
Default handler is at F000h:FF54h in IBM PC and 100%-compatible BIOSes.
Since the BOUND instruction also calls INT 05h, but returns control to the BOUND instruction, a failed BOUND check will cause an
infinite loop of PrtScreens unless the INT 05 handler is aware of the problem and checks whether the interrupt was invoked by a BOUND instruction
*/
bool bios_05h(CPU* cpu) {
    uint16_t ret_ip = getmem16(CPU_SS, CPU_SP + 2);
    uint16_t ret_cs = getmem16(CPU_SS, CPU_SP + 4);

    if (getmem8(ret_cs, ret_ip) == 0x62) {
        print_line("BOUND EXCEPTION ! ", 0);
        #if 0
        CPU_IP += 1;
        /* Set IF=1 in the flags word already pushed on stack by intcall86,
        * so that after any IRQ's IRET we still have interrupts enabled. */
        uint16_t flags_on_stack = readw86((CPU_SS << 4) + CPU_SP + 4);
        writew86((CPU_SS << 4) + CPU_SP + 4, flags_on_stack | 0x0200); /* IF bit */
        ifl = 1; /* allow IRQs while waiting for keypress */
        return false; // bound exception, TODO: ???
        #endif
        return true;
    }
    pstore8(0x500, 0x01); /* PrtSc in progress */

    uint8_t mode = pload8(0x449);       /* BDA video mode */
    uint16_t cols = pload16(0x44A);     /* columns */
    uint16_t page_size = pload16(0x44C);
    uint8_t page = pload8(0x462);       /* active page */
    uint8_t rows_minus_1 = pload8(0x484);

    if (!text_mode(mode) || cols == 0) {
        pstore8(0x500, 0xFF);
        return true;
    }

    uint16_t rows = rows_minus_1 ? (rows_minus_1 + 1) : 25;
    if (page_size == 0)
        page_size = cols * rows * 2;
        
    uint32_t base = (mode == 0x07) ? 0xB0000UL : 0xB8000UL;
    uint32_t screen = base + (uint32_t)page * page_size;

    /* FIL (~592 байта) не держим ни на стеке трапа, ни в статике: на
       стеке core0 это слишком дорого, а постоянный SRAM ради редкого
       PrtSc жалко. Куча - лучший компромисс: есть память - печать
       работает, нет - PrtSc честно вернёт ошибку и не помешает работе
       прошивки. Тот же приём в netredirect.c:547 (тоже трап на core0). */
    FIL *fp = (FIL *)malloc(sizeof(FIL));
    if (!fp) {
        pstore8(0x500, 0xFF);
        return true;
    }
    UINT bw;
    FRESULT fr = f_open(fp, "/" SD_DATA_DIR_SLASH "prn.txt", FA_WRITE | FA_OPEN_APPEND | FA_OPEN_ALWAYS);
    if (fr != FR_OK) {
        free(fp);
        pstore8(0x500, 0xFF);
        return true;
    }
    static char line[PRINTER_MAX_COLS + 1]; /* text plus newline */
    uint16_t line_cols = cols;
    if (line_cols > PRINTER_MAX_COLS)
        line_cols = PRINTER_MAX_COLS;

    for (uint16_t y = 0; y < rows; y++) {
        uint16_t n = line_cols;

        for (uint16_t x = 0; x < n; x++) {
            uint8_t ch = pload8(screen + ((uint32_t)y * cols + x) * 2);
            if (ch == 0 || ch < 0x20)
                ch = ' ';
            line[x] = (char)ch;
        }

        line[n++] = '\n';

        fr = f_write(fp, line, n, &bw);
        if (fr != FR_OK || bw != n) {
            f_close(fp);
            free(fp);
            pstore8(0x500, 0xFF);
            return true;
        }
    }

    fr = f_write(fp, "\f", 1, &bw);
    f_close(fp);
    free(fp);

    if (fr != FR_OK)
        pstore8(0x500, 0xFF);
    else
        pstore8(0x500, 0x00);

    return true;
}
