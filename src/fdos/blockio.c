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
#include "blockio_guest.h"
#include "fatfs_guest.h"
#include "config.h"
#include "hdr/network.h"
#include "init-mod.h"
#include "dyndata.h"

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
  struct dpb* dpbp = (struct dpb*)ARM_PTR(_dpbp); 
  struct dhdr* dpb_device = (struct dhdr *)ARM_PTR(dpbp->dpb_device);

  /*
   * The current native block backend transfers 512-byte sectors only.
   * Upstream FreeDOS has no panic here: sector size is supplied by the
   * DPB and handled by the installed block driver.  Until this port's
   * backend supports other sector sizes, reject such a request with the
   * same request-status value used above for an unusable drive instead
   * of halting the whole DOS kernel.
   */
  if (dpbp->dpb_secsize != 512)
    return 0x0201;              /* error + illegal command */

  for (;;)
  {
    IoReqHdrD.r_length = sizeof(request);
    IoReqHdrD.r_unit = dpbp->dpb_subunit;

    switch (mode)
    {
      case DSKWRITE:
        if (internal_data->verify_ena)
        {
          IoReqHdrD.r_command = C_OUTVFY;
          break;
        }
        /* else fall through */
      case DSKWRITEINT26:
        IoReqHdrD.r_command = C_OUTPUT;
        break;

      case DSKREADINT25:
      case DSKREAD:
        IoReqHdrD.r_command = C_INPUT;
        break;
      default:
        return 0x0100;          /* illegal command */
    }

    IoReqHdrD.r_status = 0;
    IoReqHdrD.r_meddesc = dpbp->dpb_mdb;
    IoReqHdrD.r_count = numblocks;
    if ((dpb_device->dh_attr & ATTR_HUGE) || blkno >= MAXSHORT)
    {
      IoReqHdrD.r_start = HUGECOUNT;
      IoReqHdrD.r_huge = blkno;
    }
    else
      IoReqHdrD.r_start = (UWORD)blkno;

    /*
     * Some drivers normalise transfer address so HMA transfers are disastrous!
     * Then transfer block through deblock_buf (DiskTransferBuffer doesn't work!)
     * (But this won't work for multi-block HMA transfers... are there any?)
     */
    if (FP_SEG(buf) >= 0xa000 && numblocks == 1 && LoL->bufloc != LOC_CONV)
    {
      IoReqHdrD.r_trans = LoL->deblock_buf;
      if (mode == DSKWRITE || mode == DSKWRITEINT26)
        fmemcpy(LoL->deblock_buf, buf, dpbp->dpb_secsize);

      execrh(x86_FAR_PTR(DOS_PSP, &IoReqHdrD) /* -> request */, dpbp->dpb_device);

      if (mode == DSKREAD || mode == DSKREADINT25)
        fmemcpy(buf, LoL->deblock_buf, dpbp->dpb_secsize);
    }
    else
    {
      IoReqHdrD.r_trans = buf;
      execrh(x86_FAR_PTR(DOS_PSP, &IoReqHdrD) /* -> request */, dpbp->dpb_device);
    }
    if ((IoReqHdrD.r_status & (S_ERROR | S_DONE)) == S_DONE)
      break;

    /*
     * A present removable drive with no medium is an expected terminal
     * condition in this emulator.  Do not route E_NOTRDY through INT 24h:
     * there is nothing Abort/Retry/Ignore can change until the host mounts
     * an image, and entering the guest critical-error callback from the
     * native block path is both unnecessary and re-entrant.
     */
    if ((IoReqHdrD.r_status & (S_ERROR | S_MASK)) ==
        (S_ERROR | E_NOTRDY))
      return IoReqHdrD.r_status;

    /* INT25/26 (_SEEMS_ TO) return immediately with 0x8002,
       if drive is not online,...

       normal operations (DIR) wait for ABORT/RETRY

       other condition codes not tested
     */
    if (mode >= DSKWRITEINT26)
      return (IoReqHdrD.r_status);

  loop:
    switch (block_error(&IoReqHdrD, dpbp->dpb_unit, dpbp->dpb_device, mode))
    {
      case ABORT:
      case FAIL:
        return (IoReqHdrD.r_status);

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

    bp here is a native ARM pointer (FAR expands to nothing on this
    "linear architecture"), so FP_SEG()/FP_OFF() - which only operate
    on dos_far_ptr - cannot be applied to it directly. buf_seg_off(bp)
    below recovers the offset bp would have *as a guest far pointer in
    LoL->firstbuf's segment* (every buffer lives in that one segment),
    and bufptr(off) is the inverse: turn such an offset back into a
    native pointer. b_next()/b_prev() are then just bufptr() applied to
    the stored b_next/b_prev field.
*/
#define bufptr(_bp, off) ((struct buffer *)ARM_PTR(MK_FP(FP_SEG(_bp), off)))
#define b_next_fp(_bp, bp) MK_FP(FP_SEG(_bp), (bp)->b_next)
#define b_prev_fp(_bp, bp) MK_FP(FP_SEG(_bp), (bp)->b_prev)
#define b_next(_bp, bp) bufptr(_bp, (bp)->b_next)
#define b_prev(_bp, bp) bufptr(_bp, (bp)->b_prev)
#define b_buffer_fp(_bp) MK_FP(FP_SEG(_bp), FP_OFF(_bp) + offsetof(struct buffer, b_buffer))

/*
    Search and LRU movement are one C++ wrapper operation.  This keeps the
    no-pinning guest-pointer model without paying a C/C++ call per buffer
    field in this hot path.
*/
STATIC dos_far_ptr/*struct buffer*/ searchblock(ULONG blkno, COUNT dsk)
{
  dos_far_ptr bp = fdos_buffer_search(blkno, dsk);
  if (far_is_end(bp))
  {
    printf("PANIC: bad buffer link blk=%lu dsk=%d\n",
           (unsigned long)blkno, dsk);
    while (1);
  }
  return bp;
}

/*      Write one disk buffer                                           */
STATIC BOOL flush1(dos_far_ptr/*struct buffer*/ _bp)
{
  return fdos_buffer_flush(_bp);
}

/*
    getblk(blkno, dsk, overwrite) - return a pointer to a buffer
    holding the requested disk block, reading it first unless
    "overwrite" says the caller will fill the whole block itself.

    Search, flush and cache metadata updates are one C++ wrapper operation;
    resolve the legacy host pointer only after all page-remapping work ends.
*/
struct buffer *getblk(ULONG blkno, COUNT dsk, BOOL overwrite)
{
  dos_far_ptr _bp = fdos_buffer_getblk(blkno, dsk, overwrite);
  if (far_is_end(_bp))
  {
    printf("PANIC: bad buffer link blk=%lu dsk=%d\n",
           (unsigned long)blkno, dsk);
    while (1);
  }
  if (far_is_null(_bp))
    return NULL;
  return (struct buffer *)ARM_PTR(_bp);
}

/*      Mark all buffers for a disk as not valid                        */
VOID setinvld(REG COUNT dsk) {
  dos_far_ptr _bp = LoL->firstbuf;
  struct buffer* bp = (struct buffer*)ARM_PTR(_bp);
  UWORD firstbp = FP_OFF(LoL->firstbuf);
  do
  {
    if (bp->b_unit == dsk)
      bp->b_flag = 0;
    _bp = b_next_fp(_bp, bp);
    bp = (struct buffer*)ARM_PTR(_bp);
  }
  while (FP_OFF(_bp) != firstbp);
}

/*      Check if there is at least one dirty buffer                     */
BOOL dirty_buffers(REG COUNT dsk) {
  dos_far_ptr _bp = LoL->firstbuf;
  struct buffer* bp = (struct buffer*)ARM_PTR(_bp);
  UWORD firstbp = FP_OFF(LoL->firstbuf);
  do
  {
    if (bp->b_unit == dsk &&
        (bp->b_flag & (BFR_VALID | BFR_DIRTY)) == (BFR_VALID | BFR_DIRTY))
      return TRUE;
    _bp = b_next_fp(_bp, bp);
    bp = (struct buffer*)ARM_PTR(_bp);
  }
  while (FP_OFF(_bp) != firstbp);
  return FALSE;
}

/*      Write all disk buffers for one drive                            */
BOOL flush_buffers(REG COUNT dsk) {
  dos_far_ptr _bp = LoL->firstbuf;
  struct buffer* bp = (struct buffer*)ARM_PTR(_bp);
  UWORD firstbp = FP_OFF(LoL->firstbuf);
  REG BOOL ok = TRUE;
  do
  {
    if (bp->b_unit == dsk)
      if (!flush1(_bp))
        ok = FALSE;
    _bp = b_next_fp(_bp, bp);
    bp = (struct buffer*)ARM_PTR(_bp);
  }
  while (FP_OFF(_bp) != firstbp);
  return ok;
}

/*      Write all disk buffers                                          */
BOOL flush(void) {
  dos_far_ptr _bp = LoL->firstbuf;
  struct buffer* bp = (struct buffer*)ARM_PTR(_bp);
  UWORD firstbp = FP_OFF(LoL->firstbuf);
  REG BOOL ok = TRUE;
  do
  {
    if (!flush1(_bp))
      ok = FALSE;
    bp->b_flag &= ~BFR_VALID;
    _bp = b_next_fp(_bp, bp);
    bp = (struct buffer*)ARM_PTR(_bp);
  }
  while (FP_OFF(_bp) != firstbp);

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
  dos_far_ptr _bp = LoL->firstbuf;
  struct buffer* bp = (struct buffer*)ARM_PTR(_bp);
  UWORD firstbp = FP_OFF(LoL->firstbuf);
  /* Search through buffers to see if the required block  */
  /* is already in a buffer                               */
  do {
    if (blknolow <= bp->b_blkno && bp->b_blkno <= blknohigh && (bp->b_flag & BFR_VALID) && (bp->b_unit == dsk)) {
      if (mode == XFR_READ)
        flush1(_bp);
      else
        bp->b_flag = 0;
    }
    _bp = b_next_fp(_bp, bp);
    bp = (struct buffer*)ARM_PTR(_bp);
  }
  while (FP_OFF(_bp) != firstbp);
  return FALSE;
}
