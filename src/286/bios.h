#ifndef BIOS_H
#define BIOS_H

#include <stdbool.h>
#include <stdarg.h>
#include "i386.h"

#define BOOT_ADDR 0x07C00u

bool bios_00h(CPU*); // Division by zero, etc...
bool bios_05h(CPU*); // PRINT SCREEN / BOUND RANGE EXCEEDED
bool bios_08h(CPU*); // IRQ0 timer tick
bool bios_09h(CPU*); // IRQ1 keyboard
bool bios_09h_phase2(CPU*, void*); // IRQ1 keyboard phase2 (called after INT 15h/4Fh)
bool bios_10h(CPU*); // VIDEO
bool bios_11h(CPU*); // EQUIPMENT LIST
bool bios_12h(CPU*); // LOW MEM SIZE
bool bios_13h(CPU*); // DISK 
bool bios_14h(CPU*); // SERIAL
bool bios_15h(CPU*); // TSR
bool bios_16h(CPU*); // KEYBOARD
bool bios_17h(CPU*); // PRINTERS
bool bios_18h(CPU*); // Call internal Basic
bool bios_19h(CPU*); // Bootstrap
bool bios_1Ah(CPU*); // Time/Date services
bool bios_FFh(CPU*); // W/A BIOS callback
bool bios_no_callback(CPU*, void*); // default callback for the bios_FFh

void boot_from(CPU* cpu, uint8_t dl); // INT 19h support
bool bios_16h_store_key(uint16_t ax); // shared with INT 9
void bios_10h_install_rom_fonts(CPU*); // INT 10h support
void vga_bios_baner(CPU* cpu);
void bios_puts(CPU* cpu, const char* str);
void bios_printf(CPU* cpu, const char *fmt, ...);
// allow it for BIOS/DOS
int	sprintf (char *__restrict, const char *__restrict, ...)
               __attribute__ ((__format__ (__printf__, 2, 3)));

/*
 * INT 1Eh points to the Diskette Parameter Table.
 * For a PC/AT-compatible BIOS the default table address is F000:EFC7.
 * Keep the vector non-normalized exactly as F000:EFC7, because boot sectors
 * and DOS code may save/restore or compare this BIOS pointer.
 */
#define FLOPPY_DPT_SEG   0xF000u
#define FLOPPY_DPT_OFF   0xEFC7u
#define FLOPPY_DPT_ADDR  (((uint32_t)FLOPPY_DPT_SEG << 4) + FLOPPY_DPT_OFF)
void install_floppy_dpt(void);

// Адреса в ROM для DPTE и структур
#define DPTE_ADDR_0   0xFFF50u
#define DPTE_ADDR_1   0xFFF60u

#define cpu_portout8(p, v) cpu->cb.io_write8(cpu->cb.io, p, v)
#define cpu_portout16(p, v) cpu->cb.io_write16(cpu->cb.io, p, v)
#define cpu_portin8(p) (cpu->cb.io_read8(cpu->cb.io, p))
#define cpu_portin16(p) (cpu->cb.io_read16(cpu->cb.io, p))

#endif // BIOS_H
