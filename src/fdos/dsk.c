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

#pragma pack(push, 1)
struct FS_info {
  ULONG serialno;
  BYTE volume[11];
  BYTE fstype[8];
};
#pragma pack(pop)

/*
 * Check removable-media change state.
 *
 * Fixed disks never change.  For floppy drives with a change line,
 * use the already implemented BIOS INT 13h/AH=16h service.  Drives
 * without a usable change line return M_DONT_KNOW so mediachk() can
 * fall back to rereading the BPB/serial number, as upstream FreeDOS
 * does.
 */
STATIC WORD diskchange(CPU *cpu, ddt *pddt)
{
  CPU_regs saved;
  WORD result;

  if (hd(pddt->ddt_descflags))
    return M_NOT_CHANGED;

  if (!(pddt->ddt_descflags & DF_CHANGELINE))
    return M_DONT_KNOW;

  cpu_save_regs(cpu, &saved);
  CPU_AH = 0x16;
  CPU_DL = pddt->ddt_driveno;
  bios_13h(cpu);

  if (!cf && CPU_AH == 0x00)
    result = M_NOT_CHANGED;
  else if (cf && CPU_AH == 0x06)
    result = M_CHANGED;
  else
    result = M_DONT_KNOW;

  cpu_restore_regs(cpu, &saved);
  return result;
}

STATIC int LBA_Transfer(CPU* cpu,
    ddt *pddt, UWORD mode, dos_far_ptr buffer,
    ULONG LBA_address, unsigned totaltodo,
    UWORD *transferred);

STATIC WORD RWzero(CPU *cpu, ddt *pddt, UWORD mode)
{
  UWORD done = 0;
  return LBA_Transfer(cpu, pddt, mode, DiskTransferBuffer, pddt->ddt_offset, 1, &done);
}

STATIC WORD getbpb(CPU *cpu, ddt *pddt)
{
  BYTE *buf = (BYTE *)ARM_PTR(DiskTransferBuffer);
  bpb *pbpbarray = &pddt->ddt_bpb;
  ULONG count;
  unsigned secs_per_cyl;
  WORD ret;

  if (diskchange(cpu, pddt) != M_NOT_CHANGED)
    pddt->ddt_descflags |= DF_DISKCHANGE;

  ret = RWzero(cpu, pddt, LBA_READ);
  if (ret != 0)
    return ret;

  pbpbarray->bpb_nbyte = fgetword(&buf[BT_BPB]);

  if (buf[0x1fe] != 0x55 || buf[0x1ff] != 0xaa ||
      pbpbarray->bpb_nbyte == 0 ||
      pbpbarray->bpb_nbyte % 512)
  {
    memcpy(pbpbarray, &pddt->ddt_defbpb, sizeof(bpb));
    return 0;
  }

  pddt->ddt_descflags &= ~DF_NOACCESS;
  memcpy(pbpbarray, &buf[BT_BPB], sizeof(bpb));

  {
    struct FS_info *fs = (struct FS_info *)&buf[0x27];
    BYTE sig = buf[0x26];

    if (sig == 0x29 || sig == 0x28)
      pddt->ddt_serialno = fgetlong(&fs->serialno);
    else
      pddt->ddt_serialno = 0;

    if (sig == 0x29) {
      memcpy(pddt->ddt_volume, fs->volume, sizeof fs->volume);
      memcpy(pddt->ddt_fstype, fs->fstype, sizeof fs->fstype);
    } else {
      memcpy(pddt->ddt_volume, "NO NAME    ", 11);
      memcpy(pddt->ddt_fstype, "FAT??   ", 8);
    }
  }

  count = pbpbarray->bpb_nsize == 0 ? pbpbarray->bpb_huge : pbpbarray->bpb_nsize;
  secs_per_cyl = pbpbarray->bpb_nheads * pbpbarray->bpb_nsecs;

  if (secs_per_cyl == 0)
    return failure(E_FAILURE);

  pddt->ddt_ncyl = (UWORD)((count + (secs_per_cyl - 1)) / secs_per_cyl);
  return 0;
}

STATIC WORD blk_mediachk(CPU *cpu, request FAR *rq, ddt *pddt)
{
  if (pddt->ddt_descflags & DF_REFORMAT) {
    pddt->ddt_descflags &= ~DF_REFORMAT;
    rq->r_mcretcode = M_CHANGED;
  } else if (pddt->ddt_descflags & DF_DISKCHANGE) {
    pddt->ddt_descflags &= ~DF_DISKCHANGE;
    rq->r_mcretcode = M_DONT_KNOW;
  } else {
    rq->r_mcretcode = diskchange(cpu, pddt);

    if (rq->r_mcretcode == M_DONT_KNOW)
    {
      ULONG serialno = pddt->ddt_serialno;
      WORD result = getbpb(cpu, pddt);

      if (result != 0)
        return result;

      if (serialno != pddt->ddt_serialno)
        rq->r_mcretcode = M_CHANGED;
    }
  }

  return S_DONE;
}

STATIC WORD blk_bldbpb(CPU *cpu, request FAR *rq, ddt *pddt)
{
  WORD ret = getbpb(cpu, pddt);

  if (ret != 0)
    return ret;

  rq->r_bpptr = linear_to_far(&pddt->ddt_bpb);
  return S_DONE;
}

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
*/
STATIC unsigned DMA_max_transfer(dos_far_ptr buffer, unsigned count)
{
  unsigned dma_off = FP_OFF(buffer);
  unsigned sectors_to_dma_boundary = (dma_off == 0 ? 0xffff / LoL->maxsecsize : (UWORD)(-dma_off) / LoL->maxsecsize);
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
      - LBA_FORMAT uses the already implemented INT 13h/AH=05h backend
        directly instead of upstream's fl_format() assembly wrapper.
*/
STATIC int LBA_Transfer(CPU* cpu,
    ddt *pddt, UWORD mode, dos_far_ptr buffer,
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

  /* Upstream treats low-level formatting of fixed disks as a no-op. */
  if (mode == LBA_FORMAT && hd(pddt->ddt_descflags))
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

    if (EFFECTIVE(buffer) >= 0xa0000 || count == 0)
    {
      transfer_address = (BYTE *)ARM_PTR(DiskTransferBuffer);
      transfer_far = DiskTransferBuffer;
      count = 1;

      if ((mode & 0xff00) == (LBA_WRITE & 0xff00))
      {
        fmemcpy(DiskTransferBuffer, buffer, bytes_sector);
      }
    }
    else
    {
      transfer_address = (BYTE *)ARM_PTR(buffer);
      transfer_far = buffer;
    }

    for (num_retries = 0; num_retries < N_RETRY; num_retries++)
    {
      if ((pddt->ddt_descflags & DF_LBA) && mode != LBA_FORMAT)
      {
        pdap->number_of_blocks = count; // spec says 0 < number_of_blocks < 128;
                                        // original dsk.c does not clamp this either, and
                                        // our bios_13h's int13_transfer_lba() has no such
                                        // limit, but a real BIOS might reject large counts.

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
        CPU_AH = (mode == LBA_READ)   ? 0x02 :
                 (mode == LBA_VERIFY) ? 0x04 :
                 (mode == LBA_FORMAT) ? 0x05 :
                                        0x03; /* write or write+verify */
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
      fmemcpy(buffer, DiskTransferBuffer, bytes_sector);
    }

    *transferred += count;
    LBA_address += count;
    totaltodo -= count;

    buffer = ADD_OFF(buffer, count * bytes_sector);
  }

  return 0;
}

/*
    C_INPUT / C_OUTPUT / C_OUTVFY - migrated from blockio() in dsk.c.
*/
STATIC WORD blk_rw(CPU* cpu, request FAR *rq, ddt *pddt)
{
  UWORD mode;
  UWORD transferred;
  ULONG start;
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
      return failure(E_CMD);
  }

  if (rq->r_count == 0)
    return S_DONE;

  start = (rq->r_start != HUGECOUNT) ? rq->r_start : rq->r_huge;
  err = LBA_Transfer(cpu, pddt, mode, rq->r_trans,
                     pddt->ddt_offset + start,
                     rq->r_count, &transferred);

  rq->r_count = transferred;

  return err ? (WORD)err : S_DONE;
}

/* ------------------------------------------------------------------------ */
/* The remaining dispatch entries, ported from dsk.c                         */
/* ------------------------------------------------------------------------ */

STATIC WORD blk_error(CPU* cpu, request FAR *rq, ddt *pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(pddt);

  rq->r_count = 0;
  return failure(E_FAILURE);    /* general failure */
}

STATIC WORD blk_noerr(CPU* cpu, request FAR *rq, ddt *pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(rq);
  UNREFERENCED_PARAMETER(pddt);

  return S_DONE;
}

STATIC WORD blk_nondr(CPU* cpu, request FAR *rq, ddt *pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(rq);
  UNREFERENCED_PARAMETER(pddt);

  return S_BUSY | S_DONE;
}

STATIC WORD blk_Open(CPU* cpu, request FAR *rq, ddt *pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(rq);

  pddt->ddt_FileOC++;
  return S_DONE;
}

STATIC WORD blk_Close(CPU* cpu, request FAR *rq, ddt *pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(rq);

  pddt->ddt_FileOC--;
  return S_DONE;
}

STATIC WORD blk_Media(CPU* cpu, request FAR *rq, ddt *pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(rq);

  if (hd(pddt->ddt_descflags))
    return S_BUSY | S_DONE;     /* Hard Drive: not removable */
  else
    return S_DONE;              /* Floppy: removable         */
}

/*
   0 if not set, 1 = a, 2 = b, etc, assume set.
   page 424 MS Programmer's Ref.
 */
STATIC WORD Getlogdev(CPU* cpu, request FAR *rq, ddt *pddt)
{
  int i;
  ddt *pddt2;

  UNREFERENCED_PARAMETER(cpu);

  if (!(pddt->ddt_descflags & DF_MULTLOG)) {
    rq->r_unit = 0;
    return S_DONE;
  }

  for (i = 0; i < blk_dev->dh_name[0]; i++)
  {
    pddt2 = getddt(i);
    if (pddt->ddt_driveno == pddt2->ddt_driveno &&
        (pddt2->ddt_descflags & (DF_MULTLOG | DF_CURLOG)) ==
        (DF_MULTLOG | DF_CURLOG))
        break;
  }

  rq->r_unit = i + 1;
  return S_DONE;
}

STATIC WORD Setlogdev(CPU* cpu, request FAR *rq, ddt *pddt)
{
  unsigned char unit = rq->r_unit;

  Getlogdev(cpu, rq, pddt);
  if (rq->r_unit == 0)
    return S_DONE;

  getddt(rq->r_unit - 1)->ddt_descflags &= ~DF_CURLOG;
  pddt->ddt_descflags |= DF_CURLOG;
  rq->r_unit = unit + 1;
  return S_DONE;
}

STATIC WORD IoctlQueblk(CPU* cpu, request FAR *rq, ddt *pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(pddt);

#ifdef WITHFAT32
  if (rq->r_cat == 8 || rq->r_cat == 0x48)
#else
  if (rq->r_cat == 8)
#endif
  {
    switch (rq->r_fun)
    {
    case 0x46:
    case 0x47:
    case 0x60:
    case 0x66:
    case 0x67:
      return S_DONE;
    }
  }
  return failure(E_CMD);
}

/* C_GENIOCTL is not ported yet - see Genblkdev() in the original dsk.c.
   Report "unknown command" rather than "general failure", exactly like the
   undefined dispatch slots do, so callers can fall back cleanly. */
STATIC WORD Genblkdev(CPU* cpu, request FAR *rq, ddt *pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(pddt);
  UNREFERENCED_PARAMETER(rq);

  return failure(E_CMD);
}

/*                                                                      */
/* the function dispatch table                                          */
/*                                                                      */
typedef WORD blk_proc(CPU* cpu, request FAR *rq, ddt *pddt);

STATIC blk_proc * const dispatch[NENTRY] =
{
      /* disk init is done in initdisk.c, so this should never be called */
      blk_error,                /* 00 Initialize                */
      blk_mediachk,             /* 01 Media Check               */
      blk_bldbpb,               /* 02 Build BPB                 */
      blk_error,                /* 03 Ioctl In                  */
      blk_rw,                   /* 04 Input (Read)              */
      blk_nondr,                /* 05 Non-destructive Read      */
      blk_noerr,                /* 06 Input Status              */
      blk_noerr,                /* 07 Input Flush               */
      blk_rw,                   /* 08 Output (Write)            */
      blk_rw,                   /* 09 Output with verify        */
      blk_noerr,                /* 0A Output Status             */
      blk_noerr,                /* 0B Output Flush              */
      blk_error,                /* 0C Ioctl Out                 */
      blk_Open,                 /* 0D Device Open               */
      blk_Close,                /* 0E Device Close              */
      blk_Media,                /* 0F Removable Media           */
      blk_noerr,                /* 10 Output till busy          */
      blk_error,                /* 11 undefined                 */
      blk_error,                /* 12 undefined                 */
      Genblkdev,                /* 13 Generic Ioctl Call        */
      blk_error,                /* 14 undefined                 */
      blk_error,                /* 15 undefined                 */
      blk_error,                /* 16 undefined                 */
      Getlogdev,                /* 17 Get Logical Device        */
      Setlogdev,                /* 18 Set Logical Device        */
      IoctlQueblk               /* 19 Ioctl Query               */
};

/*
    Block device driver entry point - blk_driver() in dsk.c.

    Called from BlkEntry() (the ATTR_NATIVE dh_interrupt of the built-in
    block device) for every request the file system layer builds through
    execrh()/dskxfer().
*/
void blk_driver(CPU* cpu, request FAR *rq)
{
  if (rq->r_unit >= blk_dev->dh_name[0] && rq->r_command != C_INIT)
  {
    rq->r_status = failure(E_UNIT);
    return;
  }

  if (rq->r_command >= NENTRY)
  {
    rq->r_status = failure(E_FAILURE);   /* general failure */
    return;
  }

  rq->r_status = (*dispatch[rq->r_command])(cpu, rq, getddt(rq->r_unit));
}

/*
    getddt(dev) - return pointer to the ddt (drive data table) entry
    for logical drive "dev" (0=A:, 1=B:, ...).

    Migrated from dsk.c. In the original kernel, all ddt entries are
    allocated as one contiguous array at the start of the dynamic data
    area (Dyn), see DynAlloc("ddt", nUnits, sizeof(ddt)) call sites and
    the comment near _Dyn in dsk_init()/InitDsk(). Here the array is
    built incrementally (push_ddt()), but DynAlloc() itself allocates
    sequentially from DYN_BUFFER before the first MCB exists, so the
    array is contiguous in exactly the same way, and entry 0 starts
    right after the struct DynS header.
*/
ddt *getddt(int dev)
{
  dos_far_ptr base = ADD_OFF(DYN_BUFFER, sizeof(struct DynS));
  return (ddt *)ARM_PTR(base) + dev;
}
