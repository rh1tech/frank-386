/****************************************************************/
/*                                                              */
/*                          memmgr.cpp                          */
/*                                                              */
/*               Memory Manager for Core Allocation             */
/*                                                              */
/*  Ported from upstream FreeDOS kernel/memmgr.c. The algorithms */
/*  are unchanged. Guest MCBs are addressed by guest offsets;   */
/*  no host pointer to an MCB survives a paging operation.       */
/*                                                              */
/****************************************************************/

#include <cstring>

/* Transitional C headers: xstructs.h has a field named `new`, and proto.h
   declares a C strchr signature that conflicts with the C++ overload set. */
#define new fdos_new
#define strchr fdos_strchr_compat
#ifndef _Static_assert
#define _Static_assert static_assert
#define FDOS_LOCAL_STATIC_ASSERT_MACRO 1
#endif
extern "C" {
#include "hdrs.h"
}
#ifdef FDOS_LOCAL_STATIC_ASSERT_MACRO
#undef _Static_assert
#undef FDOS_LOCAL_STATIC_ASSERT_MACRO
#endif
#undef strchr
#undef new
#ifdef load
#undef load
#endif

#include "guest_ref.hpp"

using fdos_guest::mcb_ref;
using fdos_guest::lol_ref;
using fdos_guest::dos_data_ref;

static constexpr uint32_t guest_fixed_data_linear = ((uint32_t)DOS_PSP << 4) + 0x08F0u;
static constexpr uint32_t guest_internal_data_linear = ((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF;
static const lol_ref guest_lol(guest_fixed_data_linear);
static const dos_data_ref guest_idata(guest_internal_data_linear);

static inline bool mcb_valid(const mcb_ref &m) { return m.valid(); }
static inline seg next_mcb(seg s, const mcb_ref &m)
{
  return static_cast<seg>(s + m.size() + 1u);
}

STATIC COUNT joinMCBs(seg para)
{
  mcb_ref p(para);

  while (p.type() == MCB_NORMAL)
  {
    seg qseg = next_mcb(para, p);
    mcb_ref q(qseg);
    if (!q.is_free())
      break;
    if (!mcb_valid(q))
      return DE_MCBDESTRY;

    p.type(q.type());
    p.size(static_cast<UWORD>(p.size() + q.size() + 1u));
    q.size(0xffff);
  }
  return SUCCESS;
}

#ifdef INT21_DIAG
extern "C" void mcb_dump_chain(void)
{
  for (int pass = 0; pass < 2; pass++)
  {
    seg pseg = pass ? (UWORD)guest_lol.uppermem_root() : (UWORD)guest_lol.first_mcb();
    if (pass && (pseg == 0 || pseg == 0xffff))
      break;
    printf("MCB chain (%s):\n", pass ? "UMB" : "low");
    for (int n = 0; n < 64; n++)
    {
      mcb_ref p(pseg);
      BYTE type = p.type();
      printf("  %04x: '%c' psp=%04x size=%04x\n",
             pseg, (type >= ' ' && type < 127) ? type : '?',
             p.psp(), p.size());
      if (type == MCB_LAST || !mcb_valid(p))
        break;
      pseg = next_mcb(pseg, p);
    }
  }
}
#endif

extern "C" COUNT DosMemAlloc(UWORD size, COUNT mode, seg *para, UWORD *asize)
{
  seg pseg;
  seg foundSegSeg = 0;
  UWORD foundSize = 0;
  BOOL found = FALSE;
  seg biggestSegSeg = 0;
  UWORD biggestSize = 0;
  BOOL biggest = FALSE;

searchAgain:
  pseg = (UWORD)guest_lol.first_mcb();
  biggest = found = FALSE;
  biggestSize = foundSize = 0;

  if (((UBYTE)guest_lol.uppermem_link() & 1) && (UWORD)guest_lol.uppermem_root() != 0xffff)
  {
    COUNT tmpmode = (mode == LARGEST ? (COUNT)(UBYTE)guest_idata.mem_access_mode() : mode);
    if ((mode != LARGEST || size == 0xffff) &&
        (tmpmode & (FIRST_FIT_UO | FIRST_FIT_U)))
      pseg = (UWORD)guest_lol.uppermem_root();
  }

#ifdef INT21_DIAG
  int diag_hops = 0;
#endif
  for (;;)
  {
    mcb_ref p(pseg);
    if (!mcb_valid(p))
    {
#ifdef INT21_DIAG
      printf("MCB DESTROYED at %04x: type=%02x psp=%04x size=%04x\n",
             pseg, p.type(), p.psp(), p.size());
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

    if (p.is_free())
    {
      if (joinMCBs(pseg) != SUCCESS)
        return DE_MCBDESTRY;

      mcb_ref joined(pseg);
      const UWORD joined_size = joined.size();
      if (!biggest || biggestSize < joined_size)
      {
        biggest = TRUE;
        biggestSize = joined_size;
        biggestSegSeg = pseg;
      }

      if (joined_size >= size)
      {
        switch (mode)
        {
          case LAST_FIT:
          case LAST_FIT_U:
          case LAST_FIT_UO:
          default:
            found = TRUE;
            foundSegSeg = pseg;
            foundSize = joined_size;
            break;

          case LARGEST:
            break;

          case BEST_FIT:
          case BEST_FIT_U:
          case BEST_FIT_UO:
            if (!found || foundSize > joined_size)
            {
              found = TRUE;
              foundSegSeg = pseg;
              foundSize = joined_size;
            }
            break;

          case FIRST_FIT:
          case FIRST_FIT_U:
          case FIRST_FIT_UO:
            found = TRUE;
            foundSegSeg = pseg;
            foundSize = joined_size;
            goto stopIt;
        }
      }
    }

    mcb_ref current(pseg);
    if (current.type() == MCB_LAST)
      break;
    seg next = next_mcb(pseg, current);
    if (next <= pseg)
      return DE_MCBDESTRY;
    pseg = next;
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
        ((UBYTE)guest_lol.uppermem_link() & 1) && (UWORD)guest_lol.uppermem_root() != 0xffff)
    {
      mode &= ~FIRST_FIT_U;
      goto searchAgain;
    }
    if (asize)
      *asize = biggest ? biggestSize : 0;
    return DE_NOMEM;
  }

stopIt:
  {
    mcb_ref p(foundSegSeg);
    const UWORD old_size = p.size();
    const BYTE old_type = p.type();

    if (mode != LARGEST && size != old_size)
    {
      if ((mode == LAST_FIT) || (mode == LAST_FIT_UO) || (mode == LAST_FIT_U))
      {
        const seg remainderSeg = foundSegSeg;
        const UWORD remainderSize = static_cast<UWORD>(old_size - size - 1u);
        foundSegSeg = static_cast<seg>(remainderSeg + remainderSize + 1u);

        mcb_ref remainder(remainderSeg);
        mcb_ref allocated(foundSegSeg);
        allocated.type(old_type);
        allocated.size(size);
        allocated.psp((UWORD)guest_idata.cu_psp());
        allocated.name(0, 0);

        remainder.type(MCB_NORMAL);
        remainder.size(remainderSize);
        remainder.psp(FREE_PSP);
      }
      else
      {
        const seg remainderSeg = static_cast<seg>(foundSegSeg + size + 1u);
        mcb_ref remainder(remainderSeg);
        remainder.type(old_type);
        remainder.size(static_cast<UWORD>(old_size - size - 1u));
        remainder.psp(FREE_PSP);

        p.type(MCB_NORMAL);
        p.size(size);
        p.psp((UWORD)guest_idata.cu_psp());
        p.name(0, 0);
      }
    }
    else
    {
      p.psp((UWORD)guest_idata.cu_psp());
      p.name(0, 0);
    }
  }

  *para = foundSegSeg;
  return SUCCESS;
}

extern "C" COUNT DosMemLargest(UWORD *size)
{
  seg dummy;
  *size = 0;
  DosMemAlloc(0xffff, LARGEST, &dummy, size);
  if (guest_idata.mem_access_mode() & 0x80)
  {
    UWORD lowsize = 0;
    guest_idata.mem_access_mode() &= (UBYTE)~0x80u;
    DosMemAlloc(0xffff, LARGEST, &dummy, &lowsize);
    guest_idata.mem_access_mode() |= 0x80;
    if (lowsize > *size)
      *size = lowsize;
  }
  return *size ? SUCCESS : DE_NOMEM;
}

extern "C" COUNT DosMemFree(UWORD para)
{
  if (!para)
    return DE_INVLDMCB;
  mcb_ref p(static_cast<seg>(para));
  if (!p.freeable())
    return DE_INVLDMCB;
  p.psp(FREE_PSP);
  p.clear_name();
  return SUCCESS;
}

extern "C" ULONG DosMemBlockSize(UWORD para)
{
  if (!para)
    return 0;
  mcb_ref p(static_cast<seg>(para - 1));
  if (!p.valid() || p.psp() != (UWORD)guest_idata.cu_psp())
    return 0;
  return static_cast<ULONG>(p.size()) << 4;
}

extern "C" COUNT DosMemChange(UWORD para, UWORD size, UWORD *maxSize)
{
  const seg pseg = static_cast<seg>(para - 1);
  mcb_ref p(pseg);

  if (!p.valid())
    return DE_MCBDESTRY;

  if (size > p.size())
  {
    if (joinMCBs(pseg) != SUCCESS)
      return DE_MCBDESTRY;
    if (size > p.size())
    {
      if (maxSize)
        *maxSize = p.size();
      return DE_NOMEM;
    }
  }

  if (size < p.size())
  {
    const UWORD old_size = p.size();
    const BYTE old_type = p.type();
    const seg qseg = static_cast<seg>(pseg + size + 1u);
    mcb_ref q(qseg);
    q.size(static_cast<UWORD>(old_size - size - 1u));
    q.type(old_type);
    q.psp(FREE_PSP);
    q.clear_name();

    p.size(size);
    p.type(MCB_NORMAL);

    if (joinMCBs(qseg) != SUCCESS)
      return DE_MCBDESTRY;
  }

  p.psp((UWORD)guest_idata.cu_psp());
  return SUCCESS;
}

extern "C" COUNT DosMemCheck(void)
{
  seg pseg = (UWORD)guest_lol.first_mcb();
  for (;;)
  {
    mcb_ref p(pseg);
    if (!p.valid())
      return DE_MCBDESTRY;
    if (p.type() == MCB_LAST)
      return SUCCESS;
    if (p.type() != MCB_NORMAL)
      return DE_MCBDESTRY;
    seg next = next_mcb(pseg, p);
    if (next <= pseg)
      return DE_MCBDESTRY;
    pseg = next;
  }
}

extern "C" COUNT FreeProcessMem(UWORD ps)
{
  COUNT rc = SUCCESS;
  BYTE oldumbstate = (UBYTE)guest_lol.uppermem_link() & 1;

  DosUmbLink(1);
  for (seg pseg = (UWORD)guest_lol.first_mcb();;)
  {
    mcb_ref p(pseg);
    if (!p.valid())
    {
      rc = DE_MCBDESTRY;
      break;
    }
    const BYTE type = p.type();
    const seg next = type == MCB_LAST ? pseg : next_mcb(pseg, p);
    if (p.psp() == ps)
      DosMemFree(pseg);
    if (type == MCB_LAST)
      break;
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

extern "C" void DosUmbLink(unsigned n)
{
  if ((UWORD)guest_lol.uppermem_root() == 0xffff)
    return;
  if (n > 1 || ((UBYTE)guest_lol.uppermem_link() & 1) == n)
    return;

  seg pseg = (UWORD)guest_lol.first_mcb();
  seg qseg = 0;
  BOOL have_q = FALSE;

  for (;;)
  {
    mcb_ref p(pseg);
    if (pseg == (UWORD)guest_lol.uppermem_root() || p.type() == MCB_LAST)
      break;
    if (!p.valid())
      return;
    qseg = pseg;
    have_q = TRUE;
    seg next = next_mcb(pseg, p);
    if (next <= pseg)
      return;
    pseg = next;
  }

  mcb_ref p(pseg);
  if (n == 0)
  {
    if (have_q && qseg != 0 && pseg == (UWORD)guest_lol.uppermem_root())
    {
      mcb_ref q(qseg);
      if (q.type() == MCB_NORMAL)
        q.type(MCB_LAST);
    }
  }
  else if (p.type() == MCB_LAST)
  {
    const seg link_next = next_mcb(pseg, p);
    if (link_next != (UWORD)guest_lol.uppermem_root())
      return;
    mcb_ref bridge(link_next);
    if (!bridge.valid())
    {
#ifdef INT21_DIAG
      printf("UMB SEAM BROKEN: Z at %04x size=%04x -> next %04x, root=%04x\n",
             pseg, p.size(), link_next, (UWORD)guest_lol.uppermem_root());
      mcb_dump_chain();
#endif
      return;
    }
    p.type(MCB_NORMAL);
  }
  else
    return;

  guest_lol.uppermem_link() = (UBYTE)n;
}
