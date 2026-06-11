#ifndef BIOS_H
#define BIOS_H

#include <stdbool.h>
#include "i386.h"

bool bios_00h(CPU*); // Division by zero, etc...
bool bios_05h(CPU*); // PRINT SCREEN / BOUND RANGE EXCEEDED
bool bios_08h(CPU*); // IRQ0 timer tick
bool bios_09h(CPU*); // IRQ1 keyboard
bool bios_09h_phase2(CPU*); // IRQ1 keyboard phase2 (called via INT 77h after INT 15h/4Fh)
bool bios_10h(CPU*); // VIDEO
bool bios_11h(CPU*); // EQUIPMENT LIST
bool bios_12h(CPU*); // LOW MEM SIZE
bool bios_13h(CPU*); // DISK 
bool bios_14h(CPU*); // SERIAL
bool bios_15h(CPU*); // TSR
bool bios_16h(CPU*); // KEYBOARD
bool bios_18h(CPU*); // Call internal Basic
bool bios_19h(CPU*); // Bootstrap
bool bios_1Ah(CPU*); // Time/Date services

bool bios_16h_store_key(uint16_t ax); // shared with INT 9
void bios_10h_install_rom_fonts(CPU*); // INT 10h support
void vga_bios_baner(CPU* cpu);
void install_floppy_dpt(void); // INT 13h support

// Адреса в ROM для DPTE и структур
#define DPTE_ADDR_0   0xFFF50u
#define DPTE_ADDR_1   0xFFF60u

#define cpu_portout8(p, v) cpu->cb.io_write8(cpu->cb.io, p, v)
#define cpu_portout16(p, v) cpu->cb.io_write16(cpu->cb.io, p, v)
#define cpu_portin8(p) (cpu->cb.io_read8(cpu->cb.io, p))
#define cpu_portin16(p) (cpu->cb.io_read16(cpu->cb.io, p))

#endif // BIOS_H
