#include "hdrs.h"

static dos_far_ptr Dyn = DYN_BUFFER;

dos_far_ptr DynAlloc(char *what, unsigned num, unsigned size)
{
  unsigned total = num * size;
  struct DynS far *Dynp = (struct DynS far *)ARM_PTR(Dyn);

#ifndef DEBUG
  UNREFERENCED_PARAMETER(what);
#endif

  if ((ULONG) total + Dynp->Allocated > 0xffff)
  {
    printf("PANIC: total + Dyn->Allocated %05x > FFFFh\n", (ULONG) total + Dynp->Allocated);
    for (;;) ;
  }

  DebugPrintf(("DYNDATA:allocating %s - %u * %u bytes, total %u, %u..%u\n",
               what, num, size, total, Dynp->Allocated,
               Dynp->Allocated + total));

  dos_far_ptr now = ADD_OFF(Dyn, sizeof(struct DynS) + Dynp->Allocated);
  fmemset(now, 0, total);

  Dynp->Allocated += total;

  return now;
}

dos_far_ptr DynLast(void) {
  struct DynS far *Dynp = (struct DynS far *)ARM_PTR(Dyn);
  return ADD_OFF(Dyn, sizeof(struct DynS) + Dynp->Allocated);
}
