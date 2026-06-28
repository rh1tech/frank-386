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
#include "dyndata.h"

#define printf(...) dos_printf(__VA_ARGS__)

UWORD HMAFree BSS_INIT(0);            /* first byte in HMA not yet used      */
BYTE DosLoadedInHMA BSS_INIT(FALSE);  /* set to TRUE if loaded HIGH          */

/*
    this allocates some bytes from the HMA area
    only available if DOS=HIGH was successful
*/
dos_far_ptr HMAalloc(COUNT bytesToAllocate)
{
  if (!DosLoadedInHMA)
    return MK_FP(0, 0);

  if (HMAFree > 0xfff0 - bytesToAllocate)
    return MK_FP(0, 0);

  dos_far_ptr HMAptr = MK_FP(0xffff, HMAFree);

  /* align on 16 byte boundary */
  HMAFree = (HMAFree + bytesToAllocate + 0xf) & 0xfff0;

  /*printf("HMA allocated %d byte at %x\n", bytesToAllocate, HMAptr); */

  fmemset(HMAptr, 0, bytesToAllocate);

  return HMAptr;
}
