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
bool bios_10h(CPU*); // VIDEO
bool bios_11h(CPU*); // EQUIPMENT LIST
bool bios_12h(CPU*); // LOW MEM SIZE
bool bios_13h(CPU*); // DISK 
void bios_13h_init(void);
void bios_13h_fdc_mediachange(int drive);
bool bios_14h(CPU*); // SERIAL
bool bios_15h(CPU*); // TSR
bool bios_15h_89h(CPU*); // SWITCH TO PROTECTED MODE
bool bios_15h_E820h(CPU*); // GET SYSTEM MEMORY MAP
bool bios_16h(CPU*); // KEYBOARD
bool bios_17h(CPU*); // PRINTERS
bool bios_18h(CPU*); // Call internal Basic
bool bios_19h(CPU*); // Bootstrap
bool bios_1Ah(CPU*); // Time/Date services
bool bios_33h(CPU*); // MS MOUSE
bool bios_74h(CPU*); // IRQ12: PS/2 aux device (mouse)
bool bios_FFh(CPU*); // W/A BIOS callback
bool set_bios_callback(CPU*, bios_callback_params_t*, bool reenter);
bool drop_bios_callback(CPU*, bios_callback_params_t*);
void bios_intcall(CPU*, uint8_t, const char*); // sync call

bool bios_teletype(CPU* cpu, uint8_t ch, uint8_t page);
void bios_15h_event_wait_arm(uint32_t flag_lin, uint32_t usec);
void bios_15h_event_wait_cancel(void);
void bios_15h_event_wait_tick(void);
bool bios_16h_store_key(uint16_t ax); // shared with INT 9
void bios_10h_install_rom_fonts(CPU*); // INT 10h support

/* INT 33h / IRQ12 (bios_33h.c) */
void bios_33h_install(CPU* cpu, int enabled);   // из bios_post()
void bios_33h_reset(void);

void vga_bios_baner(CPU* cpu);
void bios_puts(CPU* cpu, const char* str);
void bios_printf(CPU* cpu, const char *fmt, ...);
void cpu_err_msg(CPU* cpu, const char* msg);
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
