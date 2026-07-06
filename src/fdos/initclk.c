#include <pico.h>
#include <pico/time.h>
#include <hardware/pio.h>
#include <ctype.h>
#include "286/cpu.h"
#include "bios/bios.h"
#include "fdos.h"
#include "i8254.h"

#include "hdr/kconfig.h"
#include "hdr/portab.h"

#include "hdr/ddate.h"
#include "hdr/dtime.h"
#include "hdr/error.h"
#include "hdr/clock.h"
#include "hdr/device.h"
#include "hdr/sft.h"
#include "hdr/kbd.h"
#include "hdr/fcb.h"
#include "hdr/fat.h"
#include "hdr/pcb.h"
#include "hdr/dirmatch.h"
#include "hdr/fnode.h"
#include "hdr/mcb.h"
#include "hdr/lol.h"
#include "hdr/dcb.h"
#include "hdr/cds.h"
#include "hdr/tail.h"
#include "hdr/process.h"
#include "hdr/version.h"
#include "proto.h"
#include "globals.h"
#include "hdr/debug.h"
#include "hdr/buffer.h"
#include "hdr/file.h"
#include "config.h"
#include "hdr/network.h"
#include "init-mod.h"

#define printf(...) dos_printf(__VA_ARGS__)

STATIC int InitBcdToByte(int x)
{
  return ((x >> 4) & 0xf) * 10 + (x & 0xf);
}

void Init_clk_driver(CPU* cpu) {
  CPU_regs saved;
  cpu_save_regs(cpu, &saved);

  /* get BIOS time */
  CPU_AH = 2;
  bios_1Ah(cpu);

  /* DosSetTime */
  CPU_AH = 0x2d;
  CPU_CL = InitBcdToByte(CPU_CL);   /* minutes */
  CPU_CH = InitBcdToByte(CPU_CH);   /* hours   */
  CPU_DH = InitBcdToByte(CPU_DH);   /* seconds */
  CPU_DL = 0;
  bios_intcall(cpu, 0x21, "INIT CLK");

  /* get BIOS date */
  CPU_AH = 4;
  bios_intcall(cpu, 0x1A, "INIT CLK");

  /* DosSetDate */
  CPU_AH = 0x2b;
  CPU_CX = 100 * InitBcdToByte(CPU_CH) /* century */
               + InitBcdToByte(CPU_CL);/* year */
  /* A BIOS with y2k (year 2000) bug will always report year 19nn */
  if ((CPU_CX >= 1900) && (CPU_CX < 1980)) CPU_CX += 100;
  CPU_DH = InitBcdToByte(CPU_DH);   /* month */
  CPU_DL = InitBcdToByte(CPU_DL);   /* day   */
  bios_intcall(cpu, 0x21, "INIT CLK");

  cpu_restore_regs(cpu, &saved);
}
