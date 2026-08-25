#include <pico.h>
#include <pico/time.h>
#include <hardware/pio.h>
#include <ctype.h>
#define new fdos_new
#ifndef _Static_assert
#define _Static_assert static_assert
#define FDOS_UNDEF_STATIC_ASSERT 1
#endif
extern "C" {
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
}
#ifdef FDOS_UNDEF_STATIC_ASSERT
#undef _Static_assert
#undef FDOS_UNDEF_STATIC_ASSERT
#endif
#undef new
#ifdef load
#undef load
#endif

#include "guest_ref.hpp"

#define printf(...) dos_printf(__VA_ARGS__)

/*    -----------------------------------------------------------------
    Device Driver Interface: dskxfer()
    -----------------------------------------------------------------

    Transfer one or more blocks to/from disk through the block device
    driver (execrh()/blockio(), see above), translating dpbp into a
    request packet. Migrated from blockio.c.

    Differences from the original:
      - block_error()/CriticalError() are minimal stubs in this
        codebase (always FAIL, see fdos_21h.c) - there is no
        interactive Abort/Retry/Ignore yet, so a disk error here
        always returns immediately instead of looping on RETRY.
*/
_Static_assert(sizeof(request) == 30, "request no longer fits in internal_data->IoReqHdr[30], see lol.h");

UWORD dskxfer(COUNT dsk, ULONG blkno, dos_far_ptr buf, UWORD numblocks, COUNT mode)
{
  dos_far_ptr _dpbp = get_dpb(dsk);
  if (far_is_null(_dpbp)) {
    return 0x0201;              /* illegal command */
  }
  using fdos_guest::dhdr_ref;
using fdos_guest::request_ref;
  using fdos_guest::dpb_ref;
  using fdos_guest::dos_data_ref;
  using fdos_guest::lol_ref;
  const lol_ref dsk_lol((static_cast<fdos_guest::linear_t>(DOS_PSP) << 4) + 0x08F0u);
  const dos_data_ref dsk_idata((static_cast<fdos_guest::linear_t>(DOS_PSP) << 4) + X86_INTERNAL_DATA_OFF);
  const dpb_ref dpbp(_dpbp);
  const dos_far_ptr dpb_device = dpbp.device();
  const UBYTE dpb_unit = dpbp.dpb_unit();
  const UBYTE dpb_subunit = dpbp.dpb_subunit();
  const UWORD dpb_secsize = dpbp.dpb_secsize();
  const UBYTE dpb_mdb = dpbp.dpb_mdb();
  const UWORD device_attr = dhdr_ref(dpb_device).attr();

  /*
   * The current native block backend transfers 512-byte sectors only.
   * Upstream FreeDOS has no panic here: sector size is supplied by the
   * DPB and handled by the installed block driver.  Until this port's
   * backend supports other sector sizes, reject such a request with the
   * same request-status value used above for an unusable drive instead
   * of halting the whole DOS kernel.
   */
  if (dpb_secsize != 512)
    return 0x0201;              /* error + illegal command */

  const dos_far_ptr rq_far = MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(dos_data, IoReqHdr)));
  request_ref rq(rq_far);

  for (;;)
  {
    rq.length(sizeof(request));
    rq.unit(dpb_subunit);

    switch (mode)
    {
      case DSKWRITE:
        if ((UBYTE)dsk_idata.verify_ena())
        {
          rq.command(C_OUTVFY);
          break;
        }
        /* else fall through */
      case DSKWRITEINT26:
        rq.command(C_OUTPUT);
        break;

      case DSKREADINT25:
      case DSKREAD:
        rq.command(C_INPUT);
        break;
      default:
        return 0x0100;          /* illegal command */
    }

    rq.status(0);
    rq.meddesc((BYTE)dpb_mdb);
    rq.count(numblocks);
    if ((device_attr & ATTR_HUGE) || blkno >= MAXSHORT)
    {
      rq.start(HUGECOUNT);
      rq.huge(blkno);
    }
    else
      rq.start((UWORD)blkno);

    /*
     * Some drivers normalise transfer address so HMA transfers are disastrous!
     * Then transfer block through deblock_buf (DiskTransferBuffer doesn't work!)
     * (But this won't work for multi-block HMA transfers... are there any?)
     */
    if (FP_SEG(buf) >= 0xa000 && numblocks == 1 && (UBYTE)dsk_lol.bufloc() != LOC_CONV)
    {
      rq.trans(dsk_lol.deblock_buf());
      if (mode == DSKWRITE || mode == DSKWRITEINT26)
        fmemcpy(dsk_lol.deblock_buf(), buf, dpb_secsize);

      execrh(rq_far, dpb_device);

      if (mode == DSKREAD || mode == DSKREADINT25)
        fmemcpy(buf, dsk_lol.deblock_buf(), dpb_secsize);
    }
    else
    {
      rq.trans(buf);
      execrh(rq_far, dpb_device);
    }
    if ((rq.status() & (S_ERROR | S_DONE)) == S_DONE)
      break;

    /*
     * A present removable drive with no medium is an expected terminal
     * condition in this emulator.  Do not route E_NOTRDY through INT 24h:
     * there is nothing Abort/Retry/Ignore can change until the host mounts
     * an image, and entering the guest critical-error callback from the
     * native block path is both unnecessary and re-entrant.
     */
    if ((rq.status() & (S_ERROR | S_MASK)) ==
        (S_ERROR | E_NOTRDY))
      return rq.status();

    /* INT25/26 (_SEEMS_ TO) return immediately with 0x8002,
       if drive is not online,...

       normal operations (DIR) wait for ABORT/RETRY

       other condition codes not tested
     */
    if (mode >= DSKWRITEINT26)
      return rq.status();

  loop:
    switch (block_error_status(rq.status(), dpb_unit, dpb_device, mode))
    {
      case ABORT:
      case FAIL:
        return rq.status();

      case RETRY:
        continue;

      case CONTINUE:
        break;

      default:
        goto loop;
    }
    break;
  }                             /* retry loop */

  return 0;                     /* Success!  Return 0 for a successful operation. */
}


/*
    -----------------------------------------------------------------
    Block cache layer (migrated from blockio.c)
    -----------------------------------------------------------------

    All buffers are allocated as one contiguous array in a single
    guest segment (LoL->firstbuf, see config_init_buffers() above),
    exactly like the original. The original's b_next/b_prev fields are
    "near" offsets within that one segment, and b_next(bp)/b_prev(bp)
    there are plain far-pointer reconstructions: MK_FP(FP_SEG(bp), ...).

    The cache walk and public getblk() API keep only guest far pointers;
    buffer metadata/data are accessed through buffer_ref.
*/

namespace {
constexpr fdos_guest::linear_t blockio_lol_linear =
    (static_cast<fdos_guest::linear_t>(DOS_PSP) << 4) + 0x08F0u;
const fdos_guest::lol_ref blockio_lol(blockio_lol_linear);

static inline __attribute__((always_inline)) dos_far_ptr buffer_data_far(dos_far_ptr p)
{
  return MK_FP(FP_SEG(p),
               static_cast<UWORD>(FP_OFF(p) + offsetof(struct buffer, b_buffer)));
}

static inline __attribute__((always_inline)) void move_buffer(dos_far_ptr bp, UWORD firstbp)
{
  using fdos_guest::buffer_ref;
  const UWORD bp_off = FP_OFF(bp);
  const UWORD prev = buffer_ref(bp).prev();
  const UWORD next = buffer_ref(bp).next();
  const dos_far_ptr prev_bp = MK_FP(FP_SEG(bp), prev);
  const dos_far_ptr next_bp = MK_FP(FP_SEG(bp), next);
  const dos_far_ptr first = MK_FP(FP_SEG(bp), firstbp);
  const UWORD first_prev = buffer_ref(first).prev();

  buffer_ref(next_bp).prev(prev);
  buffer_ref(prev_bp).next(next);
  buffer_ref(bp).prev(first_prev);
  buffer_ref(bp).next(firstbp);
  buffer_ref(first).prev(bp_off);
  buffer_ref(MK_FP(FP_SEG(bp), first_prev)).next(bp_off);
}

static dos_far_ptr searchblock(ULONG blkno, COUNT dsk)
{
  using fdos_guest::buffer_ref;
  unsigned guard = 0;
  int fat_count = 0;
  UWORD last_non_fat = 0;
  UWORD uncache_buf = 0;
  dos_far_ptr first = blockio_lol.firstbuf();
  const UWORD firstbp = FP_OFF(first);
  const UWORD nbuffers = blockio_lol.nbuffers();
  dos_far_ptr p = first;

  do {
    const buffer_ref b(p);
    const ULONG b_blkno = b.blkno();
    BYTE b_flag = b.flag();
    const BYTE b_unit = b.unit();

    if (b_blkno == blkno && (b_flag & BFR_VALID) && b_unit == dsk) {
      b.flag(static_cast<BYTE>(b_flag & ~BFR_UNCACHE));
      if (FP_OFF(p) != firstbp) {
        const UWORD off = FP_OFF(p);
        blockio_lol.firstbuf(MK_FP(FP_SEG(first), off));
        move_buffer(p, firstbp);
      }
      return p;
    }

    if (b_flag & BFR_UNCACHE) uncache_buf = FP_OFF(p);
    if (b_flag & BFR_FAT) ++fat_count;
    else last_non_fat = FP_OFF(p);

    const UWORD next = b.next();
    if (next == 0xffff || guard >= nbuffers)
      return MK_FP(0xffff, 0xffff);
    p = MK_FP(FP_SEG(p), next);
    ++guard;
  } while (FP_OFF(p) != firstbp);

  const dos_far_ptr lru = MK_FP(FP_SEG(first), buffer_ref(first).prev());

  if (uncache_buf)
    p = MK_FP(FP_SEG(first), uncache_buf);
  else if ((buffer_ref(lru).flag() & BFR_FAT) && fat_count < 3 && last_non_fat)
    p = MK_FP(FP_SEG(first), last_non_fat);
  else
    p = lru;

  buffer_ref(p).flag(static_cast<BYTE>(buffer_ref(p).flag() | BFR_UNCACHE));
  if (FP_OFF(p) != firstbp) {
    const UWORD off = FP_OFF(p);
    move_buffer(p, firstbp);
    blockio_lol.firstbuf(MK_FP(FP_SEG(p), off));
  }
  return p;
}

static BOOL flush1(dos_far_ptr p)
{
  using fdos_guest::buffer_ref;
  using fdos_guest::dpb_ref;
  const buffer_ref b(p);
  BOOL ok = TRUE;
  BYTE flag = b.flag();

  if ((flag & (BFR_VALID | BFR_DIRTY)) == (BFR_VALID | BFR_DIRTY)) {
    ULONG b_offset = 0;
    UBYTE b_copies = 1;
    ULONG blkno = b.blkno();
    const BYTE unit = b.unit();

    if (flag & BFR_FAT) {
      b_copies = b.copies();
      b_offset = b.offset();
#ifdef WITHFAT32
      if (b_offset == 0) {
        const dos_far_ptr dpbp = b.dpbp();
        if (far_is_null(dpbp) || far_is_end(dpbp))
          b_copies = 1;
        else
          b_offset = dpb_ref(dpbp).dpb_xfatsize();
      }
#endif
    }

    while (b_copies--) {
      if (dskxfer(unit, blkno, buffer_data_far(p), 1, DSKWRITE))
        ok = FALSE;
      blkno += b_offset;
    }
  }

  flag = b.flag();
  flag = static_cast<BYTE>(flag & ~BFR_DIRTY);
  if (!ok)
    flag = static_cast<BYTE>(flag & ~BFR_VALID);
  b.flag(flag);
  return ok;
}
} // namespace

/*
    getblk(blkno, dsk, overwrite) - return the guest far address of a
    buffer holding the requested disk block, reading it first unless
    "overwrite" says the caller will fill the whole block itself.
*/
dos_far_ptr getblk(ULONG blkno, COUNT dsk, BOOL overwrite)
{
  using fdos_guest::buffer_ref;
  const dos_far_ptr p = searchblock(blkno, dsk);
  if (far_is_end(p)) {
    printf("PANIC: bad buffer link blk=%lu dsk=%d\n",
           (unsigned long)blkno, dsk);
    while (1);
  }

  const buffer_ref b(p);
  const BYTE flag = b.flag();
  if (!(flag & BFR_UNCACHE)) {
#ifdef FDOS_BUFFER_NOCACHE
    if (((flag & BFR_DIRTY) || overwrite)
#ifdef FDOS_BUFFER_NOCACHE_UNIT
        || dsk != FDOS_BUFFER_NOCACHE_UNIT
#endif
       )
      return p;
#else
    return p;
#endif
  }

  if (!flush1(p))
    return MK_FP(0, 0);
  if (!overwrite && dskxfer(dsk, blkno, buffer_data_far(p), 1, DSKREAD))
    return MK_FP(0, 0);

  b.flag(BFR_VALID | BFR_DATA);
  b.unit(static_cast<BYTE>(dsk));
  b.blkno(blkno);
  return p;
}

/*      Mark all buffers for a disk as not valid                        */
VOID setinvld(REG COUNT dsk) {
  using fdos_guest::buffer_ref;
  dos_far_ptr bp = blockio_lol.firstbuf();
  const UWORD firstbp = FP_OFF(bp);
  do
  {
    const buffer_ref b(bp);
    if (b.unit() == dsk)
      b.flag(0);
    bp = MK_FP(FP_SEG(bp), b.next());
  }
  while (FP_OFF(bp) != firstbp);
}

/*      Check if there is at least one dirty buffer                     */
BOOL dirty_buffers(REG COUNT dsk) {
  using fdos_guest::buffer_ref;
  dos_far_ptr bp = blockio_lol.firstbuf();
  const UWORD firstbp = FP_OFF(bp);
  do
  {
    const buffer_ref b(bp);
    const BYTE flag = b.flag();
    if (b.unit() == dsk &&
        (flag & (BFR_VALID | BFR_DIRTY)) == (BFR_VALID | BFR_DIRTY))
      return TRUE;
    bp = MK_FP(FP_SEG(bp), b.next());
  }
  while (FP_OFF(bp) != firstbp);
  return FALSE;
}

/*      Write all disk buffers for one drive                            */
BOOL flush_buffers(REG COUNT dsk) {
  using fdos_guest::buffer_ref;
  dos_far_ptr bp = blockio_lol.firstbuf();
  const UWORD firstbp = FP_OFF(bp);
  REG BOOL ok = TRUE;
  do
  {
    const buffer_ref b(bp);
    const UWORD next = b.next();
    if (b.unit() == dsk && !flush1(bp))
      ok = FALSE;
    bp = MK_FP(FP_SEG(bp), next);
  }
  while (FP_OFF(bp) != firstbp);
  return ok;
}

/*      Write all disk buffers                                          */
BOOL flush(void) {
  using fdos_guest::buffer_ref;
  dos_far_ptr bp = blockio_lol.firstbuf();
  const UWORD firstbp = FP_OFF(bp);
  REG BOOL ok = TRUE;
  do
  {
    const UWORD next = buffer_ref(bp).next();
    if (!flush1(bp))
      ok = FALSE;
    buffer_ref(bp).flag(static_cast<BYTE>(buffer_ref(bp).flag() & ~BFR_VALID));
    bp = MK_FP(FP_SEG(bp), next);
  }
  while (FP_OFF(bp) != firstbp);

  /* No redirector is loaded, so this always reports "not supported"
     (-DE_INVLDFUNC) - the original ignores the return value too. */
  network_redirector(REM_FLUSHALL);

  return ok;
}

/*
    DeleteBlockInBufferCache(blknolow, blknohigh, dsk, mode) - drop
    (XFR_WRITE) or write back (XFR_READ) any cached buffer for drive
    "dsk" whose block number falls in [blknolow, blknohigh], before a
    direct (cache-bypassing) multi-sector dskxfer() touches that same
    range - see rwblock()'s "complete sectors" fast path below.

    Migrated from blockio.c verbatim.
*/
BOOL DeleteBlockInBufferCache(ULONG blknolow, ULONG blknohigh, COUNT dsk, int mode)
{
  using fdos_guest::buffer_ref;
  dos_far_ptr bp = blockio_lol.firstbuf();
  const UWORD firstbp = FP_OFF(bp);
  do {
    const buffer_ref b(bp);
    const UWORD next = b.next();
    const ULONG blkno = b.blkno();
    const BYTE flag = b.flag();
    if (blknolow <= blkno && blkno <= blknohigh &&
        (flag & BFR_VALID) && b.unit() == dsk) {
      if (mode == XFR_READ)
        flush1(bp);
      else
        b.flag(0);
    }
    bp = MK_FP(FP_SEG(bp), next);
  }
  while (FP_OFF(bp) != firstbp);
  return FALSE;
}
