#include "hdrs.h"

static dos_far_ptr Dyn = DYN_BUFFER;

dos_far_ptr DynAlloc(char *what, unsigned num, unsigned size)
{
  unsigned total = num * size;
  uint32_t dyn_linear = EFFECTIVE(Dyn);
  UWORD allocated = pload16(dyn_linear + offsetof(struct DynS, Allocated));
  /*
   * Dyn contains both packed DOS ABI objects (DDT/DPB) and native C
   * objects that require normal ARM alignment (notably struct f_node).
   * sizeof(struct dpb) is 61 with
   * FAT32, so without padding an odd number of DPBs leaves the next
   * native object unaligned (e.g. nUnits=3 puts f_node at address mod 4
   * == 3).
   */
  unsigned start = (allocated + 3u) & ~3u;

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

  pstore16(dyn_linear + offsetof(struct DynS, Allocated), (UWORD)(start + total));

  return now;
}

dos_far_ptr DynLast(void) {
  UWORD allocated = pload16(EFFECTIVE(Dyn) + offsetof(struct DynS, Allocated));
  return ADD_OFF(Dyn, sizeof(struct DynS) + allocated);
}
