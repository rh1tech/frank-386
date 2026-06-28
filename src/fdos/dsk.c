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


#define NENTRY          26      /* total size of dispatch table */

#define LBA_READ         0x4200
#define LBA_WRITE        0x4300
#define LBA_VERIFY       0x4400
#define LBA_FORMAT       0xffff /* fake number for FORMAT track
                                   (only for NON-LBA floppies now!) */


/* true if drive descflags indicate fixed (hard disk) media.
   Migrated from dsk.c (#define hd(x) ((x) & DF_FIXED)). */
#define hd(x)   ((x) & DF_FIXED)

/*
    translate LBA sectors into CHS addressing, using the BPB stored in
    a ddt entry (as opposed to init_LBA_to_CHS() above, which is only
    used early, before any ddt exists, while probing raw BIOS geometry).

    Migrated from LBA_to_CHS() in dsk.c.
*/
STATIC int ddt_LBA_to_CHS(ULONG LBA_address, struct CHS *chs,
                          const ddt *pddt, const bpb **ppbpb)
{
  /* we need the defbpb values since those are taken from the
     BIOS, not from some random boot sector, except when
     we're dealing with a floppy */
  const bpb *pbpb = hd(pddt->ddt_descflags) ? &pddt->ddt_defbpb : &pddt->ddt_bpb;
  unsigned hs = pbpb->bpb_nsecs * pbpb->bpb_nheads;
  unsigned hsrem = (unsigned)(LBA_address % hs);

  LBA_address /= hs;

  if (LBA_address > 1023ul)
  {
    printf("LBA-Transfer error : cylinder %lu > 1023\n", LBA_address);
    return 1;
  }

  chs->Cylinder = (UWORD)LBA_address;
  chs->Head = hsrem / pbpb->bpb_nsecs;
  chs->Sector = hsrem % pbpb->bpb_nsecs + 1;
  *ppbpb = pbpb;
  return 0;
}

/*
    Test for 64K boundary crossing and return count small enough not
    to exceed the threshold.

    Migrated from DMA_max_transfer() in dsk.c, but adapted to this
    platform: see linear_to_far() above for why r_trans buffers are
    normalized to a small offset (< 0x10) before being loaded into
    CPU_ES/CPU_BX. With that normalization the boundary is effectively
    never hit (bios_13h's int13_transfer_lba() also walks a plain linear
    address, with no real-8086 same-segment wraparound to guard against
    here), but the check is kept so the control flow still matches the
    original algorithm and stays correct if buffer addressing changes
    later.
*/
STATIC unsigned DMA_max_transfer(const BYTE *buffer, unsigned count)
{
  dos_far_ptr fp = linear_to_far(buffer);
  unsigned dma_off = FP_OFF(fp);
  unsigned sectors_to_dma_boundary = (dma_off == 0 ?
    0xffff / LoL->maxsecsize :
    (UWORD)(-dma_off) / LoL->maxsecsize);

  return min(count, sectors_to_dma_boundary);
}

/*
    translate a BIOS INT 13h error status (left in AH after a failed
    call) into a DOS block device error code.

    Migrated from dskerr() in dsk.c. fl_read/fl_write/fl_verify in the
    original kernel are thin asm wrappers around INT 13h that return
    the same AH status code on error, so feeding bios_13h()'s CPU_AH
    here after cf!=0 matches the original semantics.
*/
STATIC WORD dskerr(COUNT code)
{
  switch (code & 0x03)
  {
    case 1:                    /* invalid command - general failure */
      if (code & 0x08)
        return S_ERROR | E_NOTRDY;
      else
        return failure(E_CMD);

    case 2:                    /* address mark not found - general failure */
      return failure(E_FAILURE);

    case 3:                    /* write protect */
      return failure(E_WRPRT);

    default:
      if (code & 0x80)          /* time-out */
        return failure(E_NOTRDY);
      else if (code & 0x40)     /* seek error */
        return failure(E_SEEK);
      else if (code & 0x10)     /* CRC error */
        return failure(E_CRC);
      else if (code & 0x04)
        return failure(E_NOTFND);
      else
        return failure(E_FAILURE);
  }
}

/*
    Read/Write/Write+verify "totaltodo" sectors starting at LBA_address,
    using LBA addressing when the drive supports it and falling back to
    CHS otherwise. Handles retry on error, the 64K DMA boundary, and
    crossing track boundaries in CHS mode.

    Migrated from LBA_Transfer() in dsk.c. Differences from the original:
      - fl_lba_ReadWrite()/fl_read()/fl_write()/fl_verify() (asm helpers
        that issue INT 13h) are replaced by direct bios_13h(cpu) calls,
        the same way Read1LBASector() above already does it.
      - play_dj() (floppy A:/B: drive-swap "door jingle") and the INT 1Eh
        diskette-parameter-table poke are floppy-only concerns; they are
        left as a TODO since this iteration targets a fixed disk image.
      - LBA_FORMAT (low-level track format) is left as a TODO; it is not
        needed for DosOpen().
*/
STATIC int LBA_Transfer(CPU* cpu,
    ddt *pddt, UWORD mode, BYTE *buffer,
    ULONG LBA_address, unsigned totaltodo,
    UWORD *transferred)
{
  struct _bios_LBA_address_packet *pdap = (struct _bios_LBA_address_packet *)ARM_PTR(x86_dap);
  unsigned count;
  unsigned error_code = 0;
  struct CHS chs;
  BYTE *transfer_address;
  dos_far_ptr transfer_far;
  unsigned char driveno = pddt->ddt_driveno;
  int num_retries;
  UWORD bytes_sector = pddt->ddt_bpb.bpb_nbyte;   /* bytes per sector, usually 512 */

  *transferred = 0;

  /* buffer is range-checked lazily: DMA_max_transfer() and
     linear_to_far() below both call linear_to_far() on every loop
     iteration before buffer is ever loaded into a CPU register, so
     an out-of-range buffer is caught (panic-halt, see linear_to_far())
     before any guest memory access happens. */

  /// TODO: low-level track format (LBA_FORMAT) is not implemented yet.
  if (mode == LBA_FORMAT)
    return 0;

  /// TODO: play_dj(pddt) (floppy A:/B: swap) and INT 1Eh diskette
  /// parameter table maintenance - not needed for a fixed disk image.

  pdap->packet_size = sizeof(struct _bios_LBA_address_packet);

  for (; totaltodo != 0;)
  {
    count = totaltodo;
    if ((pddt->ddt_descflags & DF_DMA_TRANSPARENT) == 0)
    {
      /* avoid overflowing 64K DMA boundary
         for drives that don't handle this transparently */
      count = DMA_max_transfer(buffer, totaltodo);
    }

    if (((intptr_t)buffer - (intptr_t)X86_RAM_BASE) >= 0xa0000 || count == 0)
    {
      transfer_address = (BYTE *)ARM_PTR(DiskTransferBuffer);
      transfer_far = DiskTransferBuffer;
      count = 1;

      if ((mode & 0xff00) == (LBA_WRITE & 0xff00))
      {
        fmemcpy(DiskTransferBuffer, linear_to_far(buffer), bytes_sector);
      }
    }
    else
    {
      transfer_address = buffer;
      transfer_far = linear_to_far(buffer);
    }

    for (num_retries = 0; num_retries < N_RETRY; num_retries++)
    {
      if ((pddt->ddt_descflags & DF_LBA) && mode != LBA_FORMAT)
      {
        pdap->number_of_blocks = count;  /// TODO: spec says 0 < number_of_blocks < 128;
                                          /// original dsk.c does not clamp this either, and
                                          /// our bios_13h's int13_transfer_lba() has no such
                                          /// limit, but a real BIOS might reject large counts.
        pdap->buffer_address = transfer_far;
        pdap->block_address_high = 0;     /* clear high part */
        pdap->block_address = LBA_address;

        CPU_AX = mode;
        CPU_DL = driveno;
        CPU_SI = FP_OFF(x86_dap);
        SET_DS(FP_SEG(x86_dap));
        bios_13h(cpu);
        error_code = cf ? CPU_AH : 0;

        if (error_code == 0 && !(pddt->ddt_descflags & DF_WRTVERIFY) &&
            mode == LBA_WRITE_VERIFY)
        {
          /* verify requested, but not supported by this drive as part
             of the write itself: write, then issue a separate verify */
          CPU_AX = LBA_VERIFY;
          CPU_DL = driveno;
          CPU_SI = FP_OFF(x86_dap);
          SET_DS(FP_SEG(x86_dap));
          bios_13h(cpu);
          error_code = cf ? CPU_AH : 0;
        }
      }
      else
      {                         /* transfer data, using old bios functions */
        const bpb *pbpb;
        if (ddt_LBA_to_CHS(LBA_address, &chs, pddt, &pbpb))
          return failure(E_FAILURE);

        /* avoid overflow at end of track */
        if (chs.Sector + count > (unsigned)pbpb->bpb_nsecs + 1)
        {
          count = pbpb->bpb_nsecs + 1 - chs.Sector;
        }

        CPU_AL = (UBYTE)count;
        CPU_AH = (mode == LBA_READ) ? 0x02 :
                 (mode == LBA_VERIFY) ? 0x04 : 0x03; /* write or write+verify */
        CPU_BX = FP_OFF(transfer_far);
        CPU_CX = ((chs.Cylinder & 0xff) << 8) +
                 ((chs.Cylinder & 0x300) >> 2) + chs.Sector;
        CPU_DH = chs.Head;
        CPU_DL = driveno;
        SET_ES(FP_SEG(transfer_far));
        bios_13h(cpu);
        error_code = cf ? CPU_AH : 0;

        if (error_code == 0 && mode == LBA_WRITE_VERIFY)
        {
          CPU_AL = (UBYTE)count;
          CPU_AH = 0x04;        /* verify */
          CPU_BX = FP_OFF(transfer_far);
          CPU_CX = ((chs.Cylinder & 0xff) << 8) +
                   ((chs.Cylinder & 0x300) >> 2) + chs.Sector;
          CPU_DH = chs.Head;
          CPU_DL = driveno;
          SET_ES(FP_SEG(transfer_far));
          bios_13h(cpu);
          error_code = cf ? CPU_AH : 0;
        }
      }

      if (error_code == 0)
        break;

      BIOS_drive_reset(cpu, driveno);
    }                           /* end of retries */

    if (error_code)
    {
      return dskerr(error_code);
    }

    /* copy to user buffer if necessary */
    if (transfer_address == (BYTE *)ARM_PTR(DiskTransferBuffer) &&
        (mode & 0xff00) == (LBA_READ & 0xff00))
    {
      fmemcpy(linear_to_far(buffer), DiskTransferBuffer, bytes_sector);
    }

    *transferred += count;
    LBA_address += count;
    totaltodo -= count;

    buffer += count * bytes_sector;
  }

  return 0;
}

/*
    block device request dispatcher: services C_INPUT/C_OUTPUT/C_OUTVFY
    requests coming from the file system layer (via execrh()/dskxfer())
    by driving LBA_Transfer() against the ddt entry for rq->r_unit.

    Migrated from blockio() in dsk.c, adapted to the request-packet
    helpers already used elsewhere in this file (rq_done()/rq_error()).
*/
void blockio(CPU* cpu, request FAR *rq)
{
  ddt *pddt = getddt(rq->r_unit);
  UWORD mode;
  UWORD transferred;
  int err;

  switch (rq->r_command)
  {
    case C_INPUT:
      mode = LBA_READ;
      break;
    case C_OUTPUT:
      mode = LBA_WRITE;
      break;
    case C_OUTVFY:
      mode = LBA_WRITE_VERIFY;
      break;
    default:
      rq_error(rq, E_CMD);
      return;
  }

  if (rq->r_count == 0)
  {
    rq->r_count = 0;
    rq_done(rq);
    return;
  }

  err = LBA_Transfer(cpu, pddt, mode, rq->r_trans,
                      rq->r_start != HUGECOUNT ? rq->r_start : rq->r_huge,
                      rq->r_count, &transferred);

  rq->r_count = transferred;

  if (err)
  {
    rq->r_status = (UWORD)err;
  }
  else
  {
    rq_done(rq);
  }
}

/*
    getddt(dev) - return pointer to the ddt (drive data table) entry
    for logical drive "dev" (0=A:, 1=B:, ...).

    Migrated from dsk.c. In the original kernel, all ddt entries are
    allocated as one contiguous array at the start of the dynamic data
    area (Dyn), see DynAlloc("ddt", nUnits, sizeof(ddt)) call sites and
    the comment near _Dyn in dsk_init()/InitDsk(). Here the array is
    built incrementally (push_ddt()), but DynAlloc() itself allocates
    sequentially from the same fixed segment (0x9000:0000), so the
    array is contiguous in exactly the same way, and entry 0 starts
    right after the struct DynS header.
*/
ddt *getddt(int dev)
{
  dos_far_ptr base = MK_FP(0x9000, sizeof(struct DynS));
  return (ddt *)ARM_PTR(base) + dev;
}
