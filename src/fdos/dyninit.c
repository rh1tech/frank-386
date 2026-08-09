#include "hdrs.h"

static dos_far_ptr Dyn = DYN_BUFFER;

dos_far_ptr DynAlloc(char *what, unsigned num, unsigned size)
{
  unsigned total = num * size;
  struct DynS far *Dynp = (struct DynS far *)ARM_PTR(Dyn);
  /*
   * Dyn contains both packed DOS ABI objects (DDT/DPB) and native C
   * objects accessed through ARM_PTR() (notably struct f_node).  The
   * latter require normal ARM alignment.  sizeof(struct dpb) is 61 with
   * FAT32, so without padding an odd number of DPBs leaves the next
   * native object unaligned (e.g. nUnits=3 puts f_node at address mod 4
   * == 3).
   */
  unsigned start = (Dynp->Allocated + 3u) & ~3u;

#ifndef DEBUG
  UNREFERENCED_PARAMETER(what);
#endif

  if ((ULONG) total + start > 0xffff)
  {
    printf("PANIC: total + aligned Dyn->Allocated %05x > FFFFh\n",
           (ULONG) total + start);
    for (;;) ;
  }

  DebugPrintf(("DYNDATA:allocating %s - %u * %u bytes, total %u, %u..%u\n",
               what, num, size, total, start, start + total));

  dos_far_ptr now = ADD_OFF(Dyn, sizeof(struct DynS) + start);
  fmemset(now, 0, total);

  Dynp->Allocated = start + total;

  return now;
}

dos_far_ptr DynLast(void) {
  struct DynS far *Dynp = (struct DynS far *)ARM_PTR(Dyn);
  return ADD_OFF(Dyn, sizeof(struct DynS) + Dynp->Allocated);
}
