/****************************************************************/
/*                                                              */
/*                          memmgr.c                            */
/*                                                              */
/*               Memory Manager for Core Allocation             */
/*                                                              */
/*  Ported from upstream FreeDOS kernel/memmgr.c. The algorithms */
/*  (search rule, split direction, join-adjacent-free-blocks)    */
/*  are unchanged; only the addressing model differs - see the   */
/*  file-level note in the description above.                    */
/*                                                              */
/****************************************************************/

#include "hdrs.h"

/* MCBs live in guest memory.  In a pageable EGA128 build a host pointer to
   an MCB is not stable across another page mapping, so the memory manager works
   on 16-byte snapshots and writes them back explicitly.  In the normal linear
   PSRAM build these helpers collapse to one native struct load/store. */
static inline void mcb_load(seg s, mcb *out)
{
#ifdef EGA128
  const uint8_t *p = ega128_guest_ptr((uint32_t)s << 4, false);
  memcpy(out, p, sizeof(*out));
#else
  *out = *para2far(s);
#endif
}

static inline void mcb_store(seg s, const mcb *in)
{
#ifdef EGA128
  uint8_t *p = ega128_guest_ptr((uint32_t)s << 4, true);
  memcpy(p, in, sizeof(*in));
#else
  *para2far(s) = *in;
#endif
}

#define nxtMCBseg(s, p)  ((seg)((s) + (p).m_size + 1))
#define mcbFree(p)        ((p).m_psp == FREE_PSP)
#define mcbValid(p)       (((p).m_size != 0xffff) && \
                           ((p).m_type == MCB_NORMAL || (p).m_type == MCB_LAST))
#define mcbFreeable(p)    ((p).m_type == MCB_NORMAL || (p).m_type == MCB_LAST)

STATIC COUNT joinMCBs(seg para)
{
  mcb p, q;
  seg qseg;

  mcb_load(para, &p);
  while (p.m_type == MCB_NORMAL)
  {
    qseg = nxtMCBseg(para, p);
    mcb_load(qseg, &q);
    if (!mcbFree(q))
      break;
    if (!mcbValid(q))
      return DE_MCBDESTRY;

    p.m_type = q.m_type;
    p.m_size += q.m_size + 1;
    q.m_size = 0xffff;
    mcb_store(qseg, &q);
    mcb_store(para, &p);
  }
  return SUCCESS;
}

#ifdef INT21_DIAG
void mcb_dump_chain(void)
{
  int pass;
  for (pass = 0; pass < 2; pass++)
  {
    seg pseg = pass ? LoL->uppermem_root : LoL->first_mcb;
    int n;
    if (pass && (pseg == 0 || pseg == 0xffff))
      break;
    printf("MCB chain (%s):\n", pass ? "UMB" : "low");
    for (n = 0; n < 64; n++)
    {
      mcb p;
      mcb_load(pseg, &p);
      printf("  %04x: '%c' psp=%04x size=%04x\n",
             pseg, (p.m_type >= ' ' && p.m_type < 127) ? p.m_type : '?',
             p.m_psp, p.m_size);
      if (p.m_type == MCB_LAST || !mcbValid(p))
        break;
      pseg = nxtMCBseg(pseg, p);
    }
  }
}
#endif

COUNT DosMemAlloc(UWORD size, COUNT mode, seg * para, UWORD * asize)
{
  seg pseg;
  mcb p;
  seg foundSegSeg = 0;
  UWORD foundSize = 0;
  BOOL found = FALSE;
  seg biggestSegSeg = 0;
  UWORD biggestSize = 0;
  BOOL biggest = FALSE;

searchAgain:
  pseg = LoL->first_mcb;
  biggest = found = FALSE;
  biggestSize = foundSize = 0;

  if ((LoL->uppermem_link & 1) && LoL->uppermem_root != 0xffff)
  {
    COUNT tmpmode = (mode == LARGEST ? internal_data->mem_access_mode : mode);
    if ((mode != LARGEST || size == 0xffff) &&
        (tmpmode & (FIRST_FIT_UO | FIRST_FIT_U)))
      pseg = LoL->uppermem_root;
  }

#ifdef INT21_DIAG
  int diag_hops = 0;
#endif
  for (;;)
  {
    mcb_load(pseg, &p);
    if (!mcbValid(p))
    {
#ifdef INT21_DIAG
      printf("MCB DESTROYED at %04x: type=%02x psp=%04x size=%04x\n",
             pseg, p.m_type, p.m_psp, p.m_size);
      mcb_dump_chain();
#endif
      return DE_MCBDESTRY;
    }
#ifdef INT21_DIAG
    if (++diag_hops > 4096)
    {
      printf("MCB CHAIN CYCLE suspected (4096 hops) at %04x\n", pseg);
      mcb_dump_chain();
      return DE_MCBDESTRY;
    }
#endif

    if (mcbFree(p))
    {
      if (joinMCBs(pseg) != SUCCESS)
        return DE_MCBDESTRY;
      mcb_load(pseg, &p);       /* join may have enlarged this block */

      if (!biggest || biggestSize < p.m_size)
      {
        biggest = TRUE;
        biggestSize = p.m_size;
        biggestSegSeg = pseg;
      }

      if (p.m_size >= size)
      {
        switch (mode)
        {
          case LAST_FIT:
          case LAST_FIT_U:
          case LAST_FIT_UO:
          default:
            found = TRUE;
            foundSegSeg = pseg;
            foundSize = p.m_size;
            break;

          case LARGEST:
            break;

          case BEST_FIT:
          case BEST_FIT_U:
          case BEST_FIT_UO:
            if (!found || foundSize > p.m_size)
            {
              found = TRUE;
              foundSegSeg = pseg;
              foundSize = p.m_size;
            }
            break;

          case FIRST_FIT:
          case FIRST_FIT_U:
          case FIRST_FIT_UO:
            found = TRUE;
            foundSegSeg = pseg;
            foundSize = p.m_size;
            goto stopIt;
        }
      }
    }

    if (p.m_type == MCB_LAST)
      break;
    {
      seg next = nxtMCBseg(pseg, p);
      if (next <= pseg)
        return DE_MCBDESTRY;
      pseg = next;
    }
  }

  if (mode == LARGEST && biggest && biggestSize >= size)
  {
    found = TRUE;
    foundSegSeg = biggestSegSeg;
    foundSize = biggestSize;
    *asize = foundSize;
  }

  if (!found || !foundSize)
  {
    if ((mode != LARGEST) && (mode & FIRST_FIT_U) &&
        (LoL->uppermem_link & 1) && LoL->uppermem_root != 0xffff)
    {
      mode &= ~FIRST_FIT_U;
      goto searchAgain;
    }
    if (asize)
      *asize = biggest ? biggestSize : 0;
    return DE_NOMEM;
  }

stopIt:
  mcb_load(foundSegSeg, &p);
  if (mode != LARGEST && size != p.m_size)
  {
    mcb remainder;
    seg remainderSeg;

    if ((mode == LAST_FIT) || (mode == LAST_FIT_UO) || (mode == LAST_FIT_U))
    {
      remainderSeg = foundSegSeg;
      remainder = p;
      remainder.m_size -= size + 1;
      foundSegSeg = nxtMCBseg(remainderSeg, remainder);

      mcb allocated;
      mcb_load(foundSegSeg, &allocated);
      allocated.m_type = remainder.m_type;
      allocated.m_size = size;
      allocated.m_psp = internal_data->cu_psp;
      allocated.m_name[0] = '\0';

      remainder.m_type = MCB_NORMAL;
      remainder.m_psp = FREE_PSP;
      mcb_store(remainderSeg, &remainder);
      mcb_store(foundSegSeg, &allocated);
    }
    else
    {
      remainderSeg = foundSegSeg + size + 1;
      mcb_load(remainderSeg, &remainder);
      remainder.m_type = p.m_type;
      remainder.m_size = p.m_size - size - 1;
      remainder.m_psp = FREE_PSP;

      p.m_type = MCB_NORMAL;
      p.m_size = size;
      p.m_psp = internal_data->cu_psp;
      p.m_name[0] = '\0';
      mcb_store(remainderSeg, &remainder);
      mcb_store(foundSegSeg, &p);
    }
  }
  else
  {
    p.m_psp = internal_data->cu_psp;
    p.m_name[0] = '\0';
    mcb_store(foundSegSeg, &p);
  }

  *para = foundSegSeg;
  return SUCCESS;
}

COUNT DosMemLargest(UWORD * size)
{
  seg dummy;
  *size = 0;
  DosMemAlloc(0xffff, LARGEST, &dummy, size);
  if (internal_data->mem_access_mode & 0x80)
  {
    UWORD lowsize = 0;
    internal_data->mem_access_mode &= ~0x80;
    DosMemAlloc(0xffff, LARGEST, &dummy, &lowsize);
    internal_data->mem_access_mode |= 0x80;
    if (lowsize > *size)
      *size = lowsize;
  }
  return *size ? SUCCESS : DE_NOMEM;
}

COUNT DosMemFree(UWORD para)
{
  mcb p;
  if (!para)
    return DE_INVLDMCB;
  mcb_load((seg)para, &p);
  if (!mcbFreeable(p))
    return DE_INVLDMCB;
  p.m_psp = FREE_PSP;
  memset(p.m_name, '\0', 8);
  mcb_store((seg)para, &p);
  return SUCCESS;
}

ULONG DosMemBlockSize(UWORD para)
{
  mcb p;
  if (!para)
    return 0;
  mcb_load((seg)(para - 1), &p);
  if (!mcbValid(p) || p.m_psp != internal_data->cu_psp)
    return 0;
  return (ULONG)p.m_size << 4;
}

COUNT DosMemChange(UWORD para, UWORD size, UWORD * maxSize)
{
  seg pseg = para - 1;
  mcb p;
  mcb_load(pseg, &p);

  if (!mcbValid(p))
    return DE_MCBDESTRY;

  if (size > p.m_size)
  {
    if (joinMCBs(pseg) != SUCCESS)
      return DE_MCBDESTRY;
    mcb_load(pseg, &p);
    if (size > p.m_size)
    {
      if (maxSize)
        *maxSize = p.m_size;
      return DE_NOMEM;
    }
  }

  if (size < p.m_size)
  {
    seg qseg = pseg + size + 1;
    mcb q;
    mcb_load(qseg, &q);
    q.m_size = p.m_size - size - 1;
    q.m_type = p.m_type;
    q.m_psp = FREE_PSP;
    memset(q.m_name, '\0', 8);

    p.m_size = size;
    p.m_type = MCB_NORMAL;
    mcb_store(qseg, &q);
    mcb_store(pseg, &p);

    if (joinMCBs(qseg) != SUCCESS)
      return DE_MCBDESTRY;
    mcb_load(pseg, &p);
  }

  p.m_psp = internal_data->cu_psp;
  mcb_store(pseg, &p);
  return SUCCESS;
}

COUNT DosMemCheck(void)
{
  seg pseg = LoL->first_mcb;
  for (;;)
  {
    mcb p;
    mcb_load(pseg, &p);
    if (!mcbValid(p))
      return DE_MCBDESTRY;
    if (p.m_type == MCB_LAST)
      return SUCCESS;
    if (p.m_type != MCB_NORMAL)
      return DE_MCBDESTRY;
    {
      seg next = nxtMCBseg(pseg, p);
      if (next <= pseg)
        return DE_MCBDESTRY;
      pseg = next;
    }
  }
}

COUNT FreeProcessMem(UWORD ps)
{
  seg pseg;
  COUNT rc = SUCCESS;
  BYTE oldumbstate = LoL->uppermem_link & 1;

  DosUmbLink(1);
  for (pseg = LoL->first_mcb;;)
  {
    mcb p;
    seg next;
    mcb_load(pseg, &p);
    if (!mcbValid(p))
    {
      rc = DE_MCBDESTRY;
      break;
    }
    if (p.m_psp == ps)
      DosMemFree(pseg);
    if (p.m_type == MCB_LAST)
      break;
    next = nxtMCBseg(pseg, p);
    if (next <= pseg)
    {
      rc = DE_MCBDESTRY;
      break;
    }
    pseg = next;
  }
  DosUmbLink(oldumbstate);
  return rc;
}

void DosUmbLink(unsigned n)
{
  seg pseg, qseg = 0;
  mcb p, q;
  BOOL have_q = FALSE;

  if (LoL->uppermem_root == 0xffff)
    return;
  if (n > 1 || (LoL->uppermem_link & 1) == n)
    return;

  pseg = LoL->first_mcb;
  mcb_load(pseg, &p);

  while (pseg != LoL->uppermem_root && p.m_type != MCB_LAST)
  {
    seg next;
    if (!mcbValid(p))
      return;
    qseg = pseg;
    q = p;
    have_q = TRUE;
    next = nxtMCBseg(pseg, p);
    if (next <= pseg)
      return;
    pseg = next;
    mcb_load(pseg, &p);
  }

  if (n == 0)
  {
    if (have_q && q.m_type == MCB_NORMAL && qseg != 0 &&
        pseg == LoL->uppermem_root)
    {
      q.m_type = MCB_LAST;
      mcb_store(qseg, &q);
    }
  }
  else if (p.m_type == MCB_LAST)
  {
    seg link_next = nxtMCBseg(pseg, p);
    mcb bridge;
    if (link_next != LoL->uppermem_root)
      return;
    mcb_load(link_next, &bridge);
    if (!mcbValid(bridge))
    {
#ifdef INT21_DIAG
      printf("UMB SEAM BROKEN: Z at %04x size=%04x -> next %04x, root=%04x\n",
             pseg, p.m_size, link_next, LoL->uppermem_root);
      mcb_dump_chain();
#endif
      return;
    }
    p.m_type = MCB_NORMAL;
    mcb_store(pseg, &p);
  }
  else
    return;

  LoL->uppermem_link = n;
}
