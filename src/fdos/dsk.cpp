extern "C" {
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
}
#include "guest_ref.hpp"

using fdos_guest::bpb_ref;
using fdos_guest::ddt_ref;
using fdos_guest::bios_lba_packet_ref;
using fdos_guest::gblkio_ref;
using fdos_guest::gblkrw_ref;
using fdos_guest::gblkfv_ref;
using fdos_guest::gioc_media_ref;
using fdos_guest::access_info_ref;
using fdos_guest::guest_bytes_ref;

#define printf(...) dos_printf(__VA_ARGS__)

extern const dos_far_ptr x86_blk_dev;

static inline UBYTE blk_dev_units(void)
{
  const uint32_t base = ((uint32_t)FP_SEG(x86_blk_dev) << 4) +
                        FP_OFF(x86_blk_dev);
  return pload8(base + offsetof(struct dhdr, dh_name));
}

static inline ddt_ref getddt_ref(int dev)
{
  return ddt_ref(getddt_far(dev));
}


#define NENTRY          26      /* total size of dispatch table */

#define LBA_READ         0x4200
#define LBA_WRITE        0x4300
#define LBA_VERIFY       0x4400
#define LBA_FORMAT       0xffff /* fake number for FORMAT track
                                   (only for NON-LBA floppies now!) */


/* true if drive descflags indicate fixed (hard disk) media.
   Migrated from dsk.c (#define hd(x) ((x) & DF_FIXED)). */
#define hd(x)   ((x) & DF_FIXED)

/* Private diskchange() result: drive exists, but no medium is inserted. */
#define M_NO_MEDIA      2

/*
 * During early kernel bootstrap no guest driver can have hooked INT 13h yet,
 * so the native BIOS may be called directly.  Once the first CONFIG.SYS
 * driver is loaded, all runtime disk traffic must honor the current IVT[13h].
 */
static BOOL disk_guest_int13 = FALSE;

extern "C" VOID fdos_disk_enable_guest_int13(VOID)
{
  disk_guest_int13 = TRUE;
}

STATIC void fdos_bios_13h(CPU *cpu, const char *owner)
{
  /*
   * Loading any CONFIG.SYS driver only means that INT 13h *may* have been
   * hooked.  Do not force the normal native BIOS path through bios_intcall()
   * merely because that point in boot has been reached: nested guest calls
   * add callback state and are unnecessary while IVT[13h] still points at
   * the native FFE0:0013 trap.  Honor a real guest hook when the vector
   * actually changes.
   */
  const UWORD int13_ip = getmem16(0, 0x13u * 4u);
  const UWORD int13_cs = getmem16(0, 0x13u * 4u + 2u);
  const BOOL guest_hooked = disk_guest_int13 &&
                            (int13_cs != 0xFFE0u || int13_ip != 0x0013u);

  if (guest_hooked)
  {
    bios_intcall(cpu, 0x13, owner);
  }
  else
  {
    /*
     * Native bios_13h() shares the implementation used by a real guest
     * INT 13h and therefore updates the caller's FLAGS at SS:SP+4.
     * Early DOS bootstrap is allowed to call it directly, but there is no
     * interrupt frame in that case.  Build a private six-byte frame below
     * the current guest stack so bios_13h() never overwrites live DOS state
     * (rwblock_workspace starts exactly at the current SS:SP).
     */
    const UWORD saved_sp = CPU_SP;
    const UWORD frame_sp = (UWORD)(saved_sp - 6u);
    const uint32_t frame = ((uint32_t)CPU_SS << 4) + frame_sp;

    CPU_SP = frame_sp;
    writew86(frame + 0u, CPU_IP);
    writew86(frame + 2u, CPU_CS);
    writew86(frame + 4u, cpu_getflags(cpu));
    bios_13h(cpu);
    CPU_SP = saved_sp;
  }
}


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
/* ReadPCClock(): BIOS tick counter at 0040:006C, the original reads the same
   place through the CLOCK$ driver. Used only to throttle boot sector rereads. */
STATIC ULONG ReadPCClock(void)
{
  return pload32(0x46C);
}

STATIC VOID tmark(ddt_ref pddt)
{
  pddt.lasttime(ReadPCClock());
}

STATIC BOOL tdelay(ddt_ref pddt, ULONG ticks)
{
  return ReadPCClock() - pddt.lasttime() >= ticks;
}

/* fl_readkey(): INT 16h AH=00h - wait for a keystroke. */
STATIC VOID fl_readkey(CPU *cpu)
{
  CPU_regs saved;

  cpu_save_regs(cpu, &saved);
  CPU_AH = 0x00;
  bios_16h(cpu);
  cpu_restore_regs(cpu, &saved);
}

/* floppy_change(): INT 2Fh AX=4A00h - let a multitasker (Windows) put up the
   "insert diskette" dialog itself. Returns 0xFFFF when nobody handles it, in
   which case we print the prompt ourselves, exactly as the original does. */
STATIC UWORD floppy_change(CPU *cpu, UWORD dx)
{
  CPU_regs saved;
  UWORD ret;

  cpu_save_regs(cpu, &saved);
  CPU_AX = 0x4A00;
  CPU_DX = dx;
  bios_intcall(cpu, 0x2F, "DJ 2F/4A00");
  ret = CPU_AX;
  cpu_restore_regs(cpu, &saved);
  return ret;
}

STATIC char template_string[] = "Remove diskette in drive X:\n";
#define DRIVE_POS (sizeof(template_string) - 4)

/*
    play_dj() - the "DJ mechanism": A: and B: are the same physical drive
    (DF_MULTLOG). Whenever the logical drive changes, the user has to swap the
    diskette. Ported from dsk.c.

*/
STATIC WORD play_dj(CPU *cpu, ddt_ref pddt)
{
  if ((pddt.descflags() & (DF_MULTLOG | DF_CURLOG)) == DF_MULTLOG)
  {
    int i;
    ddt_ref pddt2(0);

    for (i = 0; i < blk_dev_units(); i++)
    {
      pddt2 = getddt_ref(i);
      if (pddt.driveno() == pddt2.driveno() &&
          (pddt2.descflags() & (DF_MULTLOG | DF_CURLOG)) ==
          (DF_MULTLOG | DF_CURLOG))
        break;
    }

    if (i == blk_dev_units())
    {
      put_string("Error in the DJ mechanism!\n");   /* should not happen! */
    }
    else
    {
      xreg dx;
      dx.b.l = pddt.logdriveno();
      dx.b.h = pddt2.logdriveno();

      /* call int2f/ax=4a00 */
      if (floppy_change(cpu, dx.x) != 0xffff)
      {
        /* if someone else does not make a nice dialog... */
        template_string[DRIVE_POS] = 'A' + pddt2.logdriveno();
        put_string(template_string);
        put_string("Insert");
        template_string[DRIVE_POS] = 'A' + pddt.logdriveno();
        put_string(template_string + 6);
        put_string("Press any key to continue ... \n");
        fl_readkey(cpu);
      }

      pddt2.descflags(pddt2.descflags() & ~DF_CURLOG);
      pddt.descflags(pddt.descflags() | DF_CURLOG);
      pstore8(0x504, pddt.logdriveno());   /* pokeb(0, 0x504, ...) */
    }
    return M_CHANGED;
  }
  return M_NOT_CHANGED;
}

STATIC WORD diskchange(CPU *cpu, ddt_ref pddt)
{
  CPU_regs saved;
  WORD result;

  /* if it's a hard drive, media never changes */
  if (hd(pddt.descflags()))
    return M_NOT_CHANGED;

  if (play_dj(cpu, pddt) == M_CHANGED)
    return M_CHANGED;

  if (pddt.descflags() & DF_CHANGELINE)   /* if we can detect a change ... */
  {
    cpu_save_regs(cpu, &saved);
    CPU_AH = 0x16;
    CPU_DL = pddt.driveno();
    fdos_bios_13h(cpu, "DOS diskchange INT13");

    if (!cf && CPU_AH == 0x00)
      result = M_NOT_CHANGED;
    else if (cf && CPU_AH == 0x06)
      result = M_CHANGED;
    else if (cf && CPU_AH == 0x80)
      result = M_NO_MEDIA;
    else
      result = M_DONT_KNOW;

    cpu_restore_regs(cpu, &saved);

    if (result != M_DONT_KNOW)
      return result;
  }

  /* can not detect or error... - do not reread the boot sector more often
     than every ~2 seconds (37 ticks), as the original does */
  return tdelay(pddt, 37ul) ? M_DONT_KNOW : M_NOT_CHANGED;
}

STATIC int LBA_Transfer_raw(CPU* cpu,
    ddt_ref pddt, UWORD mode, dos_far_ptr buffer,
    ULONG LBA_address, unsigned totaltodo,
    UWORD *transferred);

STATIC int LBA_Transfer(CPU* cpu,
    ddt_ref pddt, UWORD mode, dos_far_ptr buffer,
    ULONG LBA_address, unsigned totaltodo,
    UWORD *transferred);

STATIC WORD RWzero(CPU *cpu, ddt_ref pddt, UWORD mode)
{
  UWORD done = 0;
  return LBA_Transfer(cpu, pddt, mode, DiskTransferBuffer, pddt.offset(), 1, &done);
}

STATIC WORD getbpb(CPU *cpu, ddt_ref pddt)
{
  const guest_bytes_ref buf(DiskTransferBuffer);
  bpb newbpb;
  ULONG count;
  unsigned secs_per_cyl;
  WORD media_state;
  WORD ret;

  media_state = diskchange(cpu, pddt);
  if (media_state == M_NO_MEDIA)
  {
    /* The medium was removed.  Keep the DDT itself alive, but make all
       subsequent block reads fail until a valid BPB is built after mount. */
    pddt.descflags(pddt.descflags() | DF_NOACCESS);
    pddt.copy_current_bpb_from_default();
    return failure(E_NOTRDY);
  }
  if (media_state != M_NOT_CHANGED)
    pddt.descflags(pddt.descflags() | DF_DISKCHANGE);

  ret = RWzero(cpu, pddt, LBA_READ);
  if (ret != 0)
    return ret;

  /*
   * Parse a replacement BPB transactionally.  Media-change errors must not
   * leave ddt_bpb half-updated: LBA_Transfer() uses this geometry on the very
   * next access.
   */
  buf.read(BT_BPB, &newbpb, sizeof(newbpb));

  if (buf.byte(0x1fe) != 0x55 || buf.byte(0x1ff) != 0xaa ||
      newbpb.bpb_nbyte == 0 ||
      newbpb.bpb_nbyte % 512)
  {
    pddt.copy_current_bpb_from_default();
    return 0;
  }

  secs_per_cyl = newbpb.bpb_nheads * newbpb.bpb_nsecs;
  if (secs_per_cyl == 0)
  {
    tmark(pddt);
    return failure(E_FAILURE);
  }

  count = newbpb.bpb_nsize == 0 ? newbpb.bpb_huge : newbpb.bpb_nsize;

  /* Validation is complete; only now publish the new media geometry. */
  pddt.write_current_bpb(&newbpb);
  pddt.descflags(pddt.descflags() & ~DF_NOACCESS);

  {
    BYTE sig;
    size_t fs_off;

    /* The extended BPB sits at a different offset on FAT32: bpb_nfsect
       (sectors per FAT) is always zero there, and that is the discriminator
       the original getbpb() uses. Without it the serial number and the volume
       label of a FAT32 volume are read out of the middle of the FAT32 BPB. */
#ifdef WITHFAT32
    if (newbpb.bpb_nfsect == 0)
    {
      fs_off = 0x43;
      sig = buf.byte(0x42);
    }
    else
#endif
    {
      fs_off = 0x27;
      sig = buf.byte(0x26);
    }

    /* 0x29: serial# + volume label + fstype are valid;
       0x28: older EBPB signature, only the serial# is valid */
    if (sig == 0x29 || sig == 0x28)
      pddt.serialno(buf.dword(fs_off + offsetof(FS_info, serialno)));
    else
      pddt.serialno(0);

    if (sig == 0x29) {
      guest_move_block(pddt.volume_linear(),
                       buf.linear(fs_off + offsetof(FS_info, volume)), 11);
      guest_move_block(pddt.fstype_linear(),
                       buf.linear(fs_off + offsetof(FS_info, fstype)), 8);
    } else {
      pddt.write_volume("NO NAME    ", 11);
      pddt.write_fstype("FAT??   ", 8);
    }
  }

  /* this field is problematic for partitions > 65535 cylinders,
     in general > 512 GiB. However: we are not using it ourselves. */
  pddt.ncyl((UWORD)((count + (secs_per_cyl - 1)) / secs_per_cyl));

  tmark(pddt);
  return 0;
}

STATIC WORD blk_mediachk(CPU *cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  if (pddt.descflags() & DF_REFORMAT) {
    pddt.descflags(pddt.descflags() & ~DF_REFORMAT);
    rq.mcretcode(M_CHANGED);
  } else if (pddt.descflags() & DF_DISKCHANGE) {
    pddt.descflags(pddt.descflags() & ~DF_DISKCHANGE);
    rq.mcretcode(M_DONT_KNOW);
  } else {
    rq.mcretcode(diskchange(cpu, pddt));

    if (rq.mcretcode() == M_NO_MEDIA)
    {
      /* Media check must report a change, not fail immediately.  The DOS
         filesystem layer will invalidate its cached buffers and then issue
         C_BLDBPB, which returns E_NOTRDY while the drive is empty. */
      rq.mcretcode(M_CHANGED);
      return S_DONE;
    }

    if (rq.mcretcode() == M_DONT_KNOW)
    {
      ULONG serialno = pddt.serialno();
      WORD result = getbpb(cpu, pddt);

      if (result != 0)
        return result;

      if (serialno != pddt.serialno())
        rq.mcretcode(M_CHANGED);
    }
  }

  return S_DONE;
}

STATIC WORD blk_bldbpb(CPU *cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  WORD ret = getbpb(cpu, pddt);

  if (ret != 0)
    return ret;

  /* The driver reads r_bpptr as a GUEST far pointer, so it must be the ddt's
     real guest address, not a native address derived from a cache slot
     (which would land in the wrong segment). Recover this ddt's index in the
     contiguous array to get its far base, then add the field offset. The
     dispatch signature stays uniform (no extra parameter). */
  rq.bpptr(ADD_OFF(getddt_far(rq.unit()), offsetof(ddt, ddt_bpb)));
  return S_DONE;
}

/*
    translate LBA sectors into CHS addressing, using the BPB stored in
    a ddt entry (as opposed to init_LBA_to_CHS() above, which is only
    used early, before any ddt exists, while probing raw BIOS geometry).

    Migrated from LBA_to_CHS() in dsk.c.
*/
STATIC int ddt_LBA_to_CHS(ULONG LBA_address, struct CHS *chs,
                          ddt_ref pddt, UWORD *track_sectors)
{
  /* we need the defbpb values since those are taken from the
     BIOS, not from some random boot sector, except when
     we're dealing with a floppy */
  const bpb_ref pbpb = hd(pddt.descflags()) ? pddt.default_bpb()
                                             : pddt.current_bpb();
  const UWORD nsecs = pbpb.bpb_nsecs();
  const UWORD nheads = pbpb.bpb_nheads();
  unsigned hs;
  unsigned hsrem;

  /* Cortex-M does not trap division by zero by default. */
  if (nsecs == 0 || nheads == 0)
  {
    printf("LBA-Transfer error : drive %u has no geometry (nsecs=%u nheads=%u)\n",
           pddt.logdriveno(), nsecs, nheads);
    return 1;
  }

  hs = nsecs * nheads;
  hsrem = (unsigned)(LBA_address % hs);
  LBA_address /= hs;

  if (LBA_address > 1023ul)
  {
    printf("LBA-Transfer error : cylinder %lu > 1023\n", LBA_address);
    return 1;
  }

  chs->Cylinder = (UWORD)LBA_address;
  chs->Head = hsrem / nsecs;
  chs->Sector = hsrem % nsecs + 1;
  *track_sectors = nsecs;
  return 0;
}

/*
    Test for 64K boundary crossing and return count small enough not
    to exceed the threshold.
*/
STATIC unsigned DMA_max_transfer(dos_far_ptr buffer, unsigned count)
{
  unsigned dma_off = (UWORD)(((ULONG)FP_SEG(buffer) << 4) + FP_OFF(buffer));
  const fdos_guest::lol_ref dsk_lol((static_cast<fdos_guest::linear_t>(DOS_PSP) << 4) + 0x08F0u);
  unsigned maxsecsize = dsk_lol.maxsecsize();
  unsigned sectors_to_dma_boundary;

  /* same ARM-vs-8086 division trap difference as in ddt_LBA_to_CHS() */
  if (maxsecsize == 0)
    return count;

  sectors_to_dma_boundary = (dma_off == 0 ? 0xffff / maxsecsize
                                          : (UWORD)(-dma_off) / maxsecsize);
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
      - the old asm INT 13h helpers are expressed in C. During early bootstrap
        they may call the native BIOS directly; after the first CONFIG.SYS
        driver is loaded they always enter through the current guest IVT[13h].
      - play_dj() (floppy A:/B: drive-swap "door jingle") and the INT 1Eh
        diskette-parameter-table poke are floppy-only concerns; they are
        left as a TODO since this iteration targets a fixed disk image.
      - LBA_FORMAT uses the already implemented INT 13h/AH=05h backend
        directly instead of upstream's fl_format() assembly wrapper.
*/
STATIC int LBA_Transfer_raw(CPU* cpu,
    ddt_ref pddt, UWORD mode, dos_far_ptr buffer,
    ULONG LBA_address, unsigned totaltodo,
    UWORD *transferred)
{
  const bios_lba_packet_ref pdap(x86_dap);
  unsigned count;
  unsigned error_code = 0;
  struct CHS chs;
  dos_far_ptr transfer_far;
  BOOL using_bounce;
  unsigned char driveno = pddt.driveno();
  int num_retries;
  UWORD bytes_sector = pddt.current_bpb().bpb_nbyte();   /* bytes per sector, usually 512 */

  *transferred = 0;

  /* Upstream treats low-level formatting of fixed disks as a no-op. */
  if (mode == LBA_FORMAT && hd(pddt.descflags()))
    return 0;

  /// TODO: play_dj(pddt) (floppy A:/B: swap) and INT 1Eh diskette
  /// parameter table maintenance - not needed for a fixed disk image.

  pdap.packet_size(sizeof(struct _bios_LBA_address_packet));
  pdap.reserved_1(0);

  for (; totaltodo != 0;)
  {
    count = totaltodo;
    if ((pddt.descflags() & DF_DMA_TRANSPARENT) == 0)
    {
      /* avoid overflowing 64K DMA boundary
         for drives that don't handle this transparently */
      count = DMA_max_transfer(buffer, totaltodo);
    }

    if (EFFECTIVE(buffer) >= 0xa0000 || count == 0)
    {
      using_bounce = TRUE;
      transfer_far = DiskTransferBuffer;
      count = 1;

      if ((mode & 0xff00) == (LBA_WRITE & 0xff00))
      {
        fmemcpy(DiskTransferBuffer, buffer, bytes_sector);
      }
    }
    else
    {
      using_bounce = FALSE;
      transfer_far = buffer;
    }

    for (num_retries = 0; num_retries < N_RETRY; num_retries++)
    {
      if ((pddt.descflags() & DF_LBA) && mode != LBA_FORMAT)
      {
        pdap.number_of_blocks((UWORD)count); // spec says 0 < number_of_blocks < 128;
                                        // original dsk.c does not clamp this either, and
                                        // our bios_13h's int13_transfer_lba() has no such
                                        // limit, but a real BIOS might reject large counts.

        pdap.buffer_address(transfer_far);
        pdap.block_address_high(0);     /* clear high part */
        pdap.block_address(LBA_address);

        CPU_AX = mode;
        CPU_DL = driveno;
        CPU_SI = FP_OFF(x86_dap);
        SET_DS(FP_SEG(x86_dap));
        fdos_bios_13h(cpu, "DOS LBA INT13");
        error_code = cf ? CPU_AH : 0;

        if (error_code == 0 && !(pddt.descflags() & DF_WRTVERIFY) &&
            mode == LBA_WRITE_VERIFY)
        {
          /* verify requested, but not supported by this drive as part
             of the write itself: write, then issue a separate verify */
          CPU_AX = LBA_VERIFY;
          CPU_DL = driveno;
          CPU_SI = FP_OFF(x86_dap);
          SET_DS(FP_SEG(x86_dap));
          fdos_bios_13h(cpu, "DOS LBA verify INT13");
          error_code = cf ? CPU_AH : 0;
        }
      }
      else
      {                         /* transfer data, using old bios functions */
        UWORD track_sectors;
        if (ddt_LBA_to_CHS(LBA_address, &chs, pddt, &track_sectors))
          return failure(E_FAILURE);

        /* avoid overflow at end of track */
        if (chs.Sector + count > (unsigned)track_sectors + 1)
          count = track_sectors + 1 - chs.Sector;

        if (count == 0 || count > (unsigned)track_sectors)
        {
          printf("LBA-Transfer error : bad sector count %u (nsecs=%u, chs=%u/%u/%u)\n",
                 count, track_sectors, chs.Cylinder, chs.Head, chs.Sector);
          return failure(E_FAILURE);
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
        fdos_bios_13h(cpu, "DOS CHS INT13");
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
          fdos_bios_13h(cpu, "DOS CHS verify INT13");
          error_code = cf ? CPU_AH : 0;
        }
      }

      if (error_code == 0)
        break;

      CPU_AH = 0x00;
      CPU_DL = driveno;
      fdos_bios_13h(cpu, "DOS reset INT13");
    }                           /* end of retries */

    if (error_code)
    {
      return dskerr(error_code);
    }

    /* copy to user buffer if necessary */
    if (using_bounce && (mode & 0xff00) == (LBA_READ & 0xff00))
    {
      fmemcpy(buffer, DiskTransferBuffer, bytes_sector);
    }

    *transferred += count;
    LBA_address += count;
    totaltodo -= count;

    /* Normalising advance: a multi-sector transfer can push the offset
       past 0xFFFF, and a plain ADD_OFF() would wrap it back to the start
       of this segment (64K low), corrupting memory there - including a
       DPB that shares the segment. add_far_x86() carries into the
       segment, matching upstream adjust_far(). */
    buffer = add_far_x86(buffer, (uint32_t)count * bytes_sector);
  }

  return 0;
}

/*
 * The early-bootstrap path may call the native BIOS through fdos_bios_13h();
 * that dispatcher supplies a private synthetic INT frame.  After the first
 * driver is loaded it uses bios_intcall() and the real guest IVT[13h] chain.
 * Neither path is therefore allowed to touch the caller's SS:SP workspace.
 */
STATIC int LBA_Transfer(CPU* cpu,
    ddt_ref pddt, UWORD mode, dos_far_ptr buffer,
    ULONG LBA_address, unsigned totaltodo,
    UWORD *transferred)
{
  CPU_regs saved_regs;

  cpu_save_regs(cpu, &saved_regs);
  int ret = LBA_Transfer_raw(cpu, pddt, mode, buffer, LBA_address,
                             totaltodo, transferred);
  cpu_restore_regs(cpu, &saved_regs);

  return ret;
}

/*
    C_INPUT / C_OUTPUT / C_OUTVFY - migrated from blockio() in dsk.c.
*/
STATIC WORD blk_rw(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  ULONG start, size;
  WORD ret;
  UWORD done;
  int action;
  bpb_ref pbpb(0);

  switch (rq.command())
  {
    case C_INPUT:
      action = LBA_READ;
      break;
    case C_OUTPUT:
      action = LBA_WRITE;
      break;
    case C_OUTVFY:
      action = LBA_WRITE_VERIFY;
      break;
    default:
      return failure(E_FAILURE);
  }

  if (pddt.descflags() & DF_NOACCESS)      /* drive has no usable medium */
    return failure(E_NOTRDY);

  tmark(pddt);
  start = (rq.start() != HUGECOUNT ? rq.start() : rq.huge());
  pbpb = hd(pddt.descflags()) ? pddt.default_bpb() : pddt.current_bpb();
  size = pbpb.bpb_nsize() ? pbpb.bpb_nsize() : pbpb.bpb_huge();

  /* The request must stay inside the volume - without this check a bogus
     r_start went straight into LBA_Transfer(). 0408h == S_ERROR|S_DONE|E_NOTFND
     ("sector not found"), the value the original returns here. */
  if (start >= size || start + rq.count() > size)
  {
    return 0x0408;
  }
  start += pddt.offset();

  ret = (WORD)LBA_Transfer(cpu, pddt, action, rq.trans(),
                           start, rq.count(), &done);

  rq.count(done);

  if (ret != 0)
    return ret;

  return S_DONE;
}

/* ------------------------------------------------------------------------ */
/* The remaining dispatch entries, ported from dsk.c                         */
/* ------------------------------------------------------------------------ */

STATIC WORD blk_error(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(pddt);

  rq.count(0);
  return failure(E_FAILURE);    /* general failure */
}

STATIC WORD blk_noerr(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(rq);
  UNREFERENCED_PARAMETER(pddt);

  return S_DONE;
}

STATIC WORD blk_nondr(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(rq);
  UNREFERENCED_PARAMETER(pddt);

  return S_BUSY | S_DONE;
}

STATIC WORD blk_Open(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(rq);

  pddt.file_open_count((UWORD)(pddt.file_open_count() + 1));
  return S_DONE;
}

STATIC WORD blk_Close(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(rq);

  pddt.file_open_count((UWORD)(pddt.file_open_count() - 1));
  return S_DONE;
}

STATIC WORD blk_Media(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(rq);

  if (hd(pddt.descflags()))
    return S_BUSY | S_DONE;     /* Hard Drive: not removable */
  else
    return S_DONE;              /* Floppy: removable         */
}

/*
   0 if not set, 1 = a, 2 = b, etc, assume set.
   page 424 MS Programmer's Ref.
 */
STATIC WORD Getlogdev(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  int i;
  ddt_ref pddt2(0);

  UNREFERENCED_PARAMETER(cpu);

  if (!(pddt.descflags() & DF_MULTLOG)) {
    rq.unit(0);
    return S_DONE;
  }

  for (i = 0; i < blk_dev_units(); i++)
  {
    pddt2 = getddt_ref(i);
    if (pddt.driveno() == pddt2.driveno() &&
        (pddt2.descflags() & (DF_MULTLOG | DF_CURLOG)) ==
        (DF_MULTLOG | DF_CURLOG))
        break;
  }

  rq.unit(i + 1);
  return S_DONE;
}

STATIC WORD Setlogdev(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  unsigned char unit = rq.unit();

  Getlogdev(cpu, rq, pddt);
  if (rq.unit() == 0)
    return S_DONE;

  {
    ddt_ref current = getddt_ref(rq.unit() - 1);
    current.descflags(current.descflags() & ~DF_CURLOG);
  }
  pddt.descflags(pddt.descflags() | DF_CURLOG);
  rq.unit(unit + 1);
  return S_DONE;
}

STATIC WORD IoctlQueblk(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(pddt);

#ifdef WITHFAT32
  if (rq.cat() == 8 || rq.cat() == 0x48)
#else
  if (rq.cat() == 8)
#endif
  {
    switch (rq.fun())
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

/* ------------------------------------------------------------------------ */
/* BIOS floppy helpers, the C equivalents of floppy.asm                       */
/* ------------------------------------------------------------------------ */

/* fl_setmediatype(): INT 13h AH=18h - set media type for format.
   Returns the BIOS status: 0 = ok, 0Ch = geometry not supported by the
   drive, 80h = no media, anything else = older BIOS without AH=18h. */
STATIC int fl_setmediatype(CPU *cpu, UBYTE drive, UWORD tracks, UWORD sectors)
{
  CPU_regs saved;
  int ret;

  cpu_save_regs(cpu, &saved);
  CPU_AH = 0x18;
  CPU_CH = (UBYTE)((tracks - 1) & 0xFF);
  CPU_CL = (UBYTE)((sectors & 0x3F) | (((tracks - 1) >> 2) & 0xC0));
  CPU_DL = drive;
  fdos_bios_13h(cpu, "DOS set media INT13");
  ret = cf ? CPU_AH : 0;
  cpu_restore_regs(cpu, &saved);
  return ret;
}

/* fl_setdisktype(): INT 13h AH=17h - set disk type for format. */
STATIC int fl_setdisktype(CPU *cpu, UBYTE drive, UBYTE type)
{
  CPU_regs saved;
  int ret;

  cpu_save_regs(cpu, &saved);
  CPU_AH = 0x17;
  CPU_AL = type;
  CPU_DL = drive;
  fdos_bios_13h(cpu, "DOS set disk type INT13");
  ret = cf ? CPU_AH : 0;
  cpu_restore_regs(cpu, &saved);
  return ret;
}

/* fl_read(): INT 13h AH=02h - plain CHS read, used to probe for a medium. */
STATIC int fl_read(CPU *cpu, UBYTE drive, UWORD head, UWORD track,
                   UWORD sector, UWORD count, dos_far_ptr buffer)
{
  CPU_regs saved;
  int ret;

  cpu_save_regs(cpu, &saved);
  CPU_AH = 0x02;
  CPU_AL = (UBYTE)count;
  CPU_CH = (UBYTE)(track & 0xFF);
  CPU_CL = (UBYTE)((sector & 0x3F) | ((track >> 2) & 0xC0));
  CPU_DH = (UBYTE)head;
  CPU_DL = drive;
  CPU_BX = FP_OFF(buffer);
  SET_ES(FP_SEG(buffer));
  fdos_bios_13h(cpu, "DOS floppy read INT13");
  ret = cf ? CPU_AH : 0;
  cpu_restore_regs(cpu, &saved);
  return ret;
}

/* read/write block with CHS based off start of drive's partition */
STATIC COUNT Genblockio(CPU *cpu, ddt_ref pddt, UWORD mode, WORD head, WORD track,
                        WORD sector, WORD count, dos_far_ptr buffer)
{
  UWORD transferred;

  /* apparently sector is ZERO, not ONE based !!! */
  return LBA_Transfer(cpu, pddt, mode, buffer,
                      ((ULONG) track * pddt.current_bpb().bpb_nheads() + head) *
                      (ULONG) pddt.current_bpb().bpb_nsecs() +
                      pddt.offset() + sector, count, &transferred);
}

/* read/write block with CHS based off start of disk drive is on */
STATIC COUNT GenblockioAbs(CPU *cpu, ddt_ref pddt, UWORD mode, WORD head, WORD track,
                           WORD sector, WORD count, dos_far_ptr buffer)
{
  UWORD transferred;

  /* apparently sector is ZERO, not ONE based !!! */
  return LBA_Transfer(cpu, pddt, mode, buffer,
                      ((ULONG) track * pddt.current_bpb().bpb_nheads() + head) *
                      (ULONG) pddt.current_bpb().bpb_nsecs() +
                      sector, count, &transferred);
}

/* The 41h/61h track transfers take head/cyl/sector/count straight out of the
   caller's buffer. Genblockio*() feeds them into LBA_Transfer(), which loops
   natively (no pc_step() inside), so a wild count is a core0 lockup, not just
   a bad read. Sanity-check them against the drive's own geometry first. */
STATIC BOOL gen_rw_sane(ddt_ref pddt, gblkrw_ref rw)
{
  const bpb_ref pbpb = hd(pddt.descflags()) ? pddt.default_bpb()
                                             : pddt.current_bpb();
  const UWORD nsecs = pbpb.bpb_nsecs();
  const UWORD nheads = pbpb.bpb_nheads();

  if (nsecs == 0 || nheads == 0)
    return FALSE;
  if (rw.nsecs() == 0 || rw.nsecs() > nsecs)
    return FALSE;
  if (rw.head() >= nheads)
    return FALSE;
  if (rw.sector() >= nsecs)
    return FALSE;
  return TRUE;
}

/*
    C_GENIOCTL - generic IOCTL, ported from Genblkdev() in dsk.c.

    40h/60h set/get device parameters, 41h/61h write/read track,
    42h/62h format/verify track, 46h/66h set/get media id,
    47h/67h set/get access flag.
*/
/*
 * Bisect switch. Before the dispatch table was completed, C_GENIOCTL simply
 * returned E_CMD and callers moved on. Set BLK_GENIOCTL to 0 to go back to
 * that behaviour without touching anything else - if a hang goes away, it is
 * somewhere behind this door (getbpb -> RWzero -> LBA_Transfer, or one of the
 * track functions).
 */
#ifndef BLK_GENIOCTL
#define BLK_GENIOCTL 1
#endif

STATIC WORD Genblkdev(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt)
{
#if !BLK_GENIOCTL
  UNREFERENCED_PARAMETER(cpu);
  UNREFERENCED_PARAMETER(rq);
  UNREFERENCED_PARAMETER(pddt);
  return failure(E_CMD);
#else
  int ret;
  unsigned descflags = pddt.descflags();

#ifdef WITHFAT32
  int extended = 0;

  if (rq.cat() == 0x48)
    extended = 1;
  else
#endif
  if (rq.cat() != 8)
    return failure(E_CMD);

  switch (rq.fun())
  {
    case 0x40:                 /* set device parameters */
      {
        const gblkio_ref gblp(rq.io());
        bpb_ref pbpb(0);

        pddt.type(gblp.devtype());
        pddt.descflags((descflags & ~3) | (gblp.devattrib() & 3)
            | (DF_DPCHANGED | DF_REFORMAT));
        pddt.ncyl(gblp.ncyl());
        /* use default dpb or current bpb? */
        pbpb = (gblp.spcfunbit() & 0x01) == 0
             ? pddt.default_bpb() : pddt.current_bpb();
#ifdef WITHFAT32
        guest_move_block(pbpb.linear(), gblp.bpb_data().linear(),
                         extended ? sizeof(bpb) : BPB_SIZEOF);
#else
        guest_move_block(pbpb.linear(), gblp.bpb_data().linear(), sizeof(bpb));
#endif
        /*pbpb->bpb_nsector = gblp->gbio_nsecs; */
        break;
      }

    case 0x41:                 /* write track - CHS is absolute not relative to partition start */
      {
        const gblkrw_ref rw(rq.rw());
        if (!gen_rw_sane(pddt, rw))
          return failure(E_FAILURE);
        ret = GenblockioAbs(cpu, pddt, LBA_WRITE, rw.head(), rw.cyl(),
                            rw.sector(), rw.nsecs(), rw.buffer());
        if (ret != 0)
          return (WORD)ret;
      }
      break;

    case 0x42:                 /* format/verify track */
      {
        const gblkfv_ref fv(rq.fv());
        const guest_bytes_ref dtb(DiskTransferBuffer);
        COUNT tracks;
        struct thst {
          UBYTE track, head, sector, type;
        } afentry;

        pddt.descflags(pddt.descflags() & ~DF_DPCHANGED);
        if (hd(descflags))
        {
          /* XXX no low-level formatting for hard disks implemented */
          fv.spcfunbit(1);       /* "not supported by bios" */
          return S_DONE;
        }
        if (descflags & DF_DPCHANGED)
        {
          /* first try newer setmediatype function */
          ret = fl_setmediatype(cpu, pddt.driveno(), pddt.ncyl(),
                                pddt.current_bpb().bpb_nsecs());
          if (ret == 0xc)
          {
            /* specified tracks, sectors/track not allowed for drive */
            fv.spcfunbit(2);
            return dskerr(ret);
          }
          else if (ret == 0x80)
          {
            fv.spcfunbit(3);     /* no disk in drive */
            return dskerr(ret);
          }
          else if (ret != 0)
            /* otherwise, setdisktype */
          {
            unsigned char type;
            unsigned ntracks, secs;
            if ((fv.spcfunbit() & 1) &&
                (ret = fl_read(cpu, pddt.driveno(), 0, 0, 1, 1,
                               DiskTransferBuffer)) != 0)
            {
              fv.spcfunbit(3);   /* no disk in drive */
              return dskerr(ret);
            }
            /* type 1: 320/360K disk in 360K drive */
            /* type 2: 320/360K disk in 1.2M drive */
            ntracks = pddt.ncyl();
            secs = pddt.current_bpb().bpb_nsecs();
            type = pddt.type() + 1;
            if (!(ntracks == 40 && (secs == 9 || secs == 8) && type < 3))
            {
              /* type 3: 1.2M disk in 1.2M drive */
              /* type 4: 720kb disk in 1.44M or 720kb drive */
              type++;
              if (type == 9) /* 1.44M drive */
                type = 4;
              if (!(ntracks == 80 && ((secs == 15 && type == 3) ||
                                      (secs == 9 && type == 4))))
              {
                /* specified tracks, sectors/track not allowed for drive */
                fv.spcfunbit(2);
                return dskerr(0xc);
              }
            }
            fl_setdisktype(cpu, pddt.driveno(), type);
          }
        }
        if (fv.spcfunbit() & 1)
          return S_DONE;

        afentry.type = 2;       /* 512 byte sectors */
        afentry.track = fv.cyl();
        afentry.head = fv.head();

        for (tracks = fv.spcfunbit() & 2 ? fv.ntracks() : 1;
             tracks > 0; tracks--)
        {
          if (afentry.track > pddt.ncyl())
            return failure(E_FAILURE);

          for (afentry.sector = 1;
               afentry.sector <= pddt.current_bpb().bpb_nsecs(); afentry.sector++)
            dtb.write((size_t)(afentry.sector - 1u) * sizeof(afentry),
                      &afentry, sizeof(afentry));

          ret = Genblockio(cpu, pddt, LBA_FORMAT, afentry.head, afentry.track, 0,
                           pddt.current_bpb().bpb_nsecs(), DiskTransferBuffer);
          if (ret != 0)
            return (WORD)ret;

          afentry.head++;
          if (afentry.head >= pddt.current_bpb().bpb_nheads())
          {
            afentry.head = 0;
            afentry.track++;
          }
        }
      }

      /* fall through to verify */
      __attribute__((fallthrough));

    case 0x62:                 /* verify track */
      {
        const gblkfv_ref fv(rq.fv());

        {
          /* gbfv_ntracks comes straight from the caller's buffer, and the
             product is passed as a WORD - a wild value turns into a negative
             count and then into a ~4G-sector native loop. Clamp it. */
          ULONG nsec = fv.spcfunbit()
                     ? (ULONG)fv.ntracks() * pddt.default_bpb().bpb_nsecs()
                     : (ULONG)pddt.default_bpb().bpb_nsecs();

          if (nsec == 0 || nsec > 0x7FFFul)
            return failure(E_FAILURE);

          ret = Genblockio(cpu, pddt, LBA_VERIFY, fv.head(), fv.cyl(), 0,
                           (WORD)nsec, DiskTransferBuffer);
        }
        if (ret != 0)
          return (WORD)ret;
        fv.spcfunbit(0); /* success */
      }
      break;

    case 0x61:                 /* read track - CHS is absolute on disk not relative to start of partition */
      {
        const gblkrw_ref rw(rq.rw());
        if (!gen_rw_sane(pddt, rw))
          return failure(E_FAILURE);
        ret = GenblockioAbs(cpu, pddt, LBA_READ, rw.head(), rw.cyl(),
                            rw.sector(), rw.nsecs(), rw.buffer());
        if (ret != 0)
          return (WORD)ret;
      }
      break;

    case 0x46:                 /* set volume serial number */
      {
        const gioc_media_ref gioc(rq.gioc());
        const guest_bytes_ref buf(DiskTransferBuffer);
        BYTE extended_BPB_signature;
        size_t fs_off;

        ret = getbpb(cpu, pddt);
        if (ret != 0)
          return (WORD)ret;

        extended_BPB_signature =
          buf.byte(pddt.current_bpb().bpb_nfsect() != 0 ? 0x26 : 0x42);
        /* return error if media lacks extended BPB with serial # */
        if ((extended_BPB_signature != 0x29) && (extended_BPB_signature != 0x28))
          return failure(E_MEDIA);

        /* otherwise, store serial # in extended BPB */
        fs_off = pddt.current_bpb().bpb_nfsect() != 0 ? 0x27 : 0x43;
        buf.dword(fs_off + offsetof(FS_info, serialno), gioc.serialno());
        pddt.serialno(gioc.serialno());

        /* And volume name if BPB supports it */
        if (extended_BPB_signature == 0x29)
        {
          guest_move_block(buf.linear(fs_off + offsetof(FS_info, volume)),
                           gioc.volume_linear(), 11);
          guest_move_block(pddt.volume_linear(), gioc.volume_linear(), 11);
        }

        ret = RWzero(cpu, pddt, LBA_WRITE);
        if (ret != 0)
          return (WORD)ret;
      }
      break;

    case 0x47:                 /* set access flag */
      {
        const access_info_ref ai(rq.ai());
        pddt.descflags((descflags & ~DF_NOACCESS) |
          (ai.flag() ? 0 : DF_NOACCESS));
      }
      break;

    case 0x60:                 /* get device parameters */
      {
        const gblkio_ref gblp(rq.io());
        bpb_ref pbpb(0);

        gblp.devtype(pddt.type());
        gblp.devattrib((UWORD)(descflags & 3));
        /* 360 kb disk in 1.2 MB drive */
        gblp.media((pddt.type() == 1) && (pddt.ncyl() == 40));
        gblp.ncyl(pddt.ncyl());
        /* use default dpb or current bpb? */
        pbpb = (gblp.spcfunbit() & 0x01) == 0
             ? pddt.default_bpb() : pddt.current_bpb();
#ifdef WITHFAT32
        guest_move_block(gblp.bpb_data().linear(), pbpb.linear(),
                         extended ? sizeof(bpb) : BPB_SIZEOF);
#else
        guest_move_block(gblp.bpb_data().linear(), pbpb.linear(), sizeof(bpb));
#endif
        /*gblp->gbio_nsecs = pbpb->bpb_nsector; */
        break;
      }

    case 0x66:                 /* get volume serial number */
      {
        const gioc_media_ref gioc(rq.gioc());

        ret = getbpb(cpu, pddt);
        if (ret != 0)
          return (WORD)ret;

        /* Note: getbpb() will initialize extended BPB fields with default values */
        gioc.serialno(pddt.serialno());
        guest_move_block(gioc.volume_linear(), pddt.volume_linear(), 11);
        guest_move_block(gioc.fstype_linear(), pddt.fstype_linear(), 8);
      }
      break;

    case 0x67:                 /* get access flag */
      {
        const access_info_ref ai(rq.ai());
        ai.flag(descflags & DF_NOACCESS ? 0 : 1);        /* bit 9 */
      }
      break;

    default:
      return failure(E_CMD);
  }
  return S_DONE;
#endif /* BLK_GENIOCTL */
}

/*                                                                      */
/* the function dispatch table                                          */
/*                                                                      */
typedef WORD blk_proc(CPU* cpu, fdos_guest::request_ref &rq, ddt_ref pddt);

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
void blk_driver(CPU* cpu, fdos_guest::request_ref &rq)
{
  if (rq.unit() >= blk_dev_units() && rq.command() != C_INIT)
  {
    rq.status(failure(E_UNIT));
    return;
  }

  if (rq.command() >= NENTRY)
  {
    rq.status(failure(E_FAILURE));   /* general failure */
    return;
  }

  rq.status((*dispatch[rq.command()])(cpu, rq, getddt_ref(rq.unit())));
}

/*
    getddt_far(dev) - return the guest far pointer to a ddt entry, but as its genuine guest
    far pointer instead of a native one. Needed wherever a ddt (or a field
    inside it) must be handed back to a guest as a dos_far_ptr - e.g. the BPB
    pointer in a Build-BPB request packet, which a real block driver reads as
    a far pointer. Computing it from the far base (rather than linear_to_far()
    on the native pointer) keeps the ddt array's own segment, so the packet
    points where the guest expects.
*/
dos_far_ptr /* -> ddt */ getddt_far(int dev)
{
  dos_far_ptr base = ADD_OFF(DYN_BUFFER, sizeof(struct DynS));
  return ADD_OFF(base, (uint32_t)dev * sizeof(ddt));
}
