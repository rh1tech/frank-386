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

/* segment of the MCB immediately following the one at "s" (whose data
   portion is p->m_size paragraphs) - the +1 accounts for the MCB
   header itself occupying one paragraph. */
#define nxtMCBseg(s, p)  ((seg)((s) + (p)->m_size + 1))

#define mcbFree(p)      ((p)->m_psp == FREE_PSP)
#define mcbValid(p)     (((p)->m_size != 0xffff) && \
                          ((p)->m_type == MCB_NORMAL || (p)->m_type == MCB_LAST))
#define mcbFreeable(p)  ((p)->m_type == MCB_NORMAL || (p)->m_type == MCB_LAST)

/*
 * Join any following unused MCBs to the MCB at segment "para".
 */
STATIC COUNT joinMCBs(seg para)
{
  mcb *p = para2far(para);
  mcb *q;
  seg qseg;

  /* loop as long as the current MCB is not the last one in the chain
     and the next MCB is unused */
  while (p->m_type == MCB_NORMAL)
  {
    qseg = nxtMCBseg(para, p);
    q = para2far(qseg);
    if (!mcbFree(q))
      break;
    if (!mcbValid(q))
      return DE_MCBDESTRY;
    /* join both MCBs */
    p->m_type = q->m_type;      /* possibly the next MCB is the last one */
    p->m_size += q->m_size + 1; /* one for q's MCB itself */
    q->m_size = 0xffff;         /* mark the now unlinked MCB as "fake" */
  }

  return SUCCESS;
}

/*
 * Allocate a new memory area. *para is assigned to the segment of the
 * MCB itself, not the segment of the data portion (i.e. the usable
 * block starts at *para + 1).
 *
 * If mode == LARGEST, asize MUST be != NULL and always receives the
 * largest available block's size, which is what gets allocated.
 * If mode != LARGEST, asize may be NULL, but if not, it is assigned
 * the size of the largest available block only on failure.
 * "size" is the minimum size of the block to search for, even when
 * mode == LARGEST.
 */
COUNT DosMemAlloc(UWORD size, COUNT mode, seg * para, UWORD * asize)
{
  seg pseg;
  mcb *p;
  seg foundSegSeg = 0;
  mcb *foundSeg;
  seg biggestSegSeg = 0;
  mcb *biggestSeg;

searchAgain:

  pseg = LoL->first_mcb;
  p = para2far(pseg);

  biggestSeg = foundSeg = NULL;

  /* Hack to hit the UMB region directly for now - saves walking the
     whole conventional chain first when the caller only wants UMBs
     anyway (mirrors upstream's own comment/shortcut). */
  if ((LoL->uppermem_link & 1) && LoL->uppermem_root != 0xffff)
  {
    COUNT tmpmode = (mode == LARGEST ? internal_data->mem_access_mode : mode);
    if ((mode != LARGEST || size == 0xffff) &&
        (tmpmode & (FIRST_FIT_UO | FIRST_FIT_U)))
    {
      pseg = LoL->uppermem_root;
      p = para2far(pseg);
    }
  }

  /* Search through memory blocks */
  for (;;)
  {
    /* check for corruption */
    if (!mcbValid(p))
      return DE_MCBDESTRY;

    if (mcbFree(p))
    {                            /* unused block, check if it applies to the rule */
      if (joinMCBs(pseg) != SUCCESS)    /* join following unused blocks */
        return DE_MCBDESTRY;

      if (!biggestSeg || biggestSeg->m_size < p->m_size)
      {
        biggestSeg = p;
        biggestSegSeg = pseg;
      }

      if (p->m_size >= size)
      {                          /* if the block is too small, ignore */
        /* this block has a "match" size, try the rule set */
        switch (mode)
        {
          case LAST_FIT:        /* search for last possible */
          case LAST_FIT_U:
          case LAST_FIT_UO:
          default:
            foundSeg = p;
            foundSegSeg = pseg;
            break;

          case LARGEST:         /* grab the biggest block - computed
                                    once the whole chain is checked */
            break;

          case BEST_FIT:        /* first, but smallest block */
          case BEST_FIT_U:
          case BEST_FIT_UO:
            if (!foundSeg || foundSeg->m_size > p->m_size)
            {
              foundSeg = p;
              foundSegSeg = pseg;
            }
            break;

          case FIRST_FIT:       /* first possible */
          case FIRST_FIT_U:
          case FIRST_FIT_UO:
            foundSeg = p;
            foundSegSeg = pseg;
            goto stopIt;         /* OK, rest of chain can be ignored */
        }
      }
    }

    if (p->m_type == MCB_LAST)
      break;                     /* end of chain reached */

    pseg = nxtMCBseg(pseg, p);   /* advance to next MCB */
    p = para2far(pseg);
  }

  if (mode == LARGEST && biggestSeg && biggestSeg->m_size >= size)
  {
    foundSeg = biggestSeg;
    foundSegSeg = biggestSegSeg;
    *asize = foundSeg->m_size;
  }

  if (!foundSeg || !foundSeg->m_size)
  {                              /* no block to fulfill the request */
    if ((mode != LARGEST) && (mode & FIRST_FIT_U) &&
        (LoL->uppermem_link & 1) && LoL->uppermem_root != 0xffff)
    {
      mode &= ~FIRST_FIT_U;
      goto searchAgain;
    }
    if (asize)
      *asize = biggestSeg ? biggestSeg->m_size : 0;
    return DE_NOMEM;
  }

stopIt:                         /* reached from FIRST_FIT on match */

  if (mode != LARGEST && size != foundSeg->m_size)
  {
    /* Split the found buffer because it is larger than requested.
       foundSeg/foundSegSeg := the block that will be allocated
       p/pseg                := the block that forms the remainder */
    if ((mode == LAST_FIT) || (mode == LAST_FIT_UO) || (mode == LAST_FIT_U))
    {
      /* allocate the block from the end of the found block */
      pseg = foundSegSeg;
      p = foundSeg;
      p->m_size -= size + 1;     /* size+1 paragraphs go to the new
                                    segment (+1 for its own MCB) */
      foundSegSeg = nxtMCBseg(pseg, p);
      foundSeg = para2far(foundSegSeg);

      /* initialize stuff because foundSeg > p */
      foundSeg->m_type = p->m_type;
      p->m_type = MCB_NORMAL;
    }
    else
    {                            /* all other modes allocate from the beginning */
      pseg = foundSegSeg + size + 1;
      p = para2far(pseg);

      /* initialize stuff because p > foundSeg */
      p->m_type = foundSeg->m_type;
      p->m_size = foundSeg->m_size - size - 1;
      foundSeg->m_type = MCB_NORMAL;
    }

    p->m_psp = FREE_PSP;         /* unused */
    foundSeg->m_size = size;
  }

  foundSeg->m_psp = internal_data->cu_psp;    /* the new block is for
                                                  the current process */
  foundSeg->m_name[0] = '\0';

  *para = foundSegSeg;
  return SUCCESS;
}

/*
 * Unlike the name suggests, this returns the _size_ of the largest
 * available block rather than the block itself.
 */
COUNT DosMemLargest(UWORD * size)
{
  seg dummy;
  *size = 0;
  DosMemAlloc(0xffff, LARGEST, &dummy, size);
  if (internal_data->mem_access_mode & 0x80)  /* the largest block is
                                                  probably low memory */
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

/*
 * Deallocate a memory block. para is the segment of the MCB itself.
 * Can be called with para == 0 (a no-op error return), which eases
 * other parts of the kernel that may not always have a block to free.
 */
COUNT DosMemFree(UWORD para)
{
  mcb *p;

  if (!para)
    return DE_INVLDMCB;

  p = para2far(para);

  if (!mcbFreeable(p))          /* doesn't have to be valid, freeable
                                    is enough */
    return DE_INVLDMCB;

  /* Mark the mcb as free so it can later be merged with other
     surrounding free MCBs by joinMCBs(). */
  p->m_psp = FREE_PSP;
  memset(p->m_name, '\0', 8);

  return SUCCESS;
}

/*
 * Resize an allocated memory block. para is the segment of the *data*
 * portion of the block (para - 1 is the MCB itself) - this matches
 * INT 21h AH=4Ah's BX=segment convention directly.
 *
 * Growing a block resizes it to the largest size <= the requested
 * size if the full amount isn't available (real MS-DOS behavior,
 * per upstream's comment here).
 */
COUNT DosMemChange(UWORD para, UWORD size, UWORD * maxSize)
{
  seg pseg = para - 1;
  mcb *p = para2far(pseg);
  seg qseg;
  mcb *q;

  if (!mcbValid(p))
    return DE_MCBDESTRY;

  /* check if to grow the block */
  if (size > p->m_size)
  {
    /* first try to make the MCB larger by joining with any following
       unused blocks */
    if (joinMCBs(pseg) != SUCCESS)
      return DE_MCBDESTRY;

    if (size > p->m_size)
    {                            /* block is still too small */
      if (maxSize)
        *maxSize = p->m_size;
      return DE_NOMEM;
    }
  }

  /* shrink it down */
  if (size < p->m_size)
  {
    qseg = pseg + size + 1;
    q = para2far(qseg);
    q->m_size = p->m_size - size - 1;
    q->m_type = p->m_type;

    p->m_size = size;
    p->m_type = MCB_NORMAL;      /* the old block can't be last anymore */

    q->m_psp = FREE_PSP;
    memset(q->m_name, '\0', 8);

    if (joinMCBs(qseg) != SUCCESS)
      return DE_MCBDESTRY;
  }

  p->m_psp = internal_data->cu_psp;

  return SUCCESS;
}

/*
 * Free every memory block owned by process "ps" (its PSP segment).
 * Used when a process terminates.
 */
COUNT FreeProcessMem(UWORD ps)
{
  seg pseg;
  mcb *p;
  BYTE oldumbstate = LoL->uppermem_link & 1;

  /* link in upper memory too, so blocks a terminating process owns
     up there get freed as well */
  DosUmbLink(1);

  for (pseg = LoL->first_mcb;; pseg = nxtMCBseg(pseg, p))
  {
    p = para2far(pseg);

    if (!mcbValid(p))
      return DE_MCBDESTRY;

    if (p->m_psp == ps)
      DosMemFree(pseg);

    if (p->m_type == MCB_LAST)
      break;
  }

  DosUmbLink(oldumbstate);

  return SUCCESS;
}

/*
 * Link (n=1) or unlink (n=0) the UMB chain into/from the main MCB
 * chain (INT 21h AX=5803h). No-op if there is no UMB region at all,
 * or if the chain is already in the requested state.
 */
void DosUmbLink(unsigned n)
{
  seg pseg, qseg = 0;
  mcb *p, *q = NULL;

  if (LoL->uppermem_root == 0xffff)
    return;

  pseg = LoL->first_mcb;
  p = para2far(pseg);

  if (n > 1 || (LoL->uppermem_link & 1) == n)
    return;

  while (pseg != LoL->uppermem_root && p->m_type != MCB_LAST)
  {
    if (!mcbValid(p))
      return;
    qseg = pseg;
    q = p;
    pseg = nxtMCBseg(pseg, p);
    p = para2far(pseg);
  }

  if (n == 0)
  {
    if (q && q->m_type == MCB_NORMAL && qseg != 0 && pseg == LoL->uppermem_root)
      q->m_type = MCB_LAST;
  }
  else if (p->m_type == MCB_LAST)
    p->m_type = MCB_NORMAL;
  else
    return;

  LoL->uppermem_link = n;
}
