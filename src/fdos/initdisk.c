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

/*
 * Native FreeDOS calls the BIOS handlers directly instead of executing a
 * guest INT instruction.  int13_set_status() nevertheless updates the FLAGS
 * word of a normal INT 13h frame at SS:SP+4.  During these direct calls there
 * is no such frame, so that write corrupts the live FreeDOS stack.
 *
 * Keep the return registers and CPU flags produced by INT 13h, but preserve
 * the caller's stack word and segment registers.  This mirrors the isolation
 * already used by the runtime block-driver path in dsk.c.
 */
static bool init_bios_13h(CPU *cpu)
{
  const uint16_t saved_ds = CPU_DS;
  const uint16_t saved_es = CPU_ES;
  const uint32_t outer_flags_addr =
      ((uint32_t)CPU_SS << 4) + CPU_SP + 4u;
  const uint16_t outer_flags = readw86(outer_flags_addr);

  const bool ret = bios_13h(cpu);

  SET_DS(saved_ds);
  SET_ES(saved_es);
  writew86(outer_flags_addr, outer_flags);
  return ret;
}

void BIOS_drive_reset(CPU* cpu, unsigned drive)
{
    CPU_DL = drive | 0x80;
    CPU_AH = 0;
    init_bios_13h(cpu);
}

/*
    internal global data
*/

BOOL ExtLBAForce = FALSE;
COUNT nUnits BSS_INIT(0);
UWORD LBA_WRITE_VERIFY = 0x4302;

typedef struct {
  UWORD bpb_nbyte;              /* Bytes per Sector             */
  UBYTE bpb_nsector;            /* Sectors per Allocation Unit  */
  UWORD bpb_nreserved;          /* # Reserved Sectors           */
  UBYTE bpb_nfat;               /* # FATs                       */
  UWORD bpb_ndirent;            /* # Root Directory entries     */
  UWORD bpb_nsize;              /* Size in sectors              */
  UBYTE bpb_mdesc;              /* MEDIA Descriptor Byte        */
  UWORD bpb_nfsect;             /* FAT size in sectors          */
  UWORD bpb_nsecs;              /* Sectors per track            */
  UWORD bpb_nheads;             /* Number of heads              */
} floppy_bpb;

#define FLOPPY_SEC_SIZE 512u  /* common sector size */
#define NDEV            26      /* up to Z:                     */

static const floppy_bpb floppy_bpbs[5] = {
/* copied from Brian Reifsnyder's FORMAT, bpb.h */
  {FLOPPY_SEC_SIZE, 2, 1, 2, 112, 720, 0xfd, 2, 9, 2}, /* FD360  5.25 DS   */
  {FLOPPY_SEC_SIZE, 1, 1, 2, 224, 2400, 0xf9, 7, 15, 2},       /* FD1200 5.25 HD   */
  {FLOPPY_SEC_SIZE, 2, 1, 2, 112, 1440, 0xf9, 3, 9, 2},        /* FD720  3.5  LD   */
  {FLOPPY_SEC_SIZE, 1, 1, 2, 224, 2880, 0xf0, 9, 18, 2},       /* FD1440 3.5  HD   */
  {FLOPPY_SEC_SIZE, 2, 1, 2, 240, 5760, 0xf0, 9, 36, 2}        /* FD2880 3.5  ED   */
};

static COUNT init_getdriveparm(CPU* cpu, UBYTE drive, bpb * pbpbarray)
{
  REG UBYTE type;

  if (drive & 0x80)
    return 5;
  CPU_AH = 0x08;
  CPU_DL = drive;
  init_bios_13h(cpu); // GET DRIVE PARAMETERS
  type = CPU_BL - 1;
  if (cf)
    type = 0;                   /* return 320-360 for XTs */
  else if (type > 6)
    type = 8;                   /* any odd ball drives get 8&7=0: the 320-360 table */
  else if (type == 5)
    type = 4;                   /* 5 and 4 are both 2.88 MB */

  memcpy(pbpbarray, &floppy_bpbs[type & 7], sizeof(floppy_bpb));
  ((bpb *)pbpbarray)->bpb_hidden = 0;  /* very important to init to 0, see bug#1789 */
  ((bpb *)pbpbarray)->bpb_huge = 0;

  if (type == 3)
    return 7;                   /* 1.44 MB */

  if (type == 4)
    return 9;                   /* 2.88 almost forgot this one */

  /* 0=320-360kB, 1=1.2MB, 2=720kB, 8=any odd ball drives */
  return type;
}

static COUNT init_readdasd(CPU* cpu, UBYTE drive)
{
  CPU_AH = 0x15;
  CPU_DL = drive;
  init_bios_13h(cpu); // GET DISK TYPE
  if (!cf)
    switch (CPU_AH)
    {
      case 2:
        return DF_CHANGELINE;
      case 3:
        return DF_FIXED;
    }
  return 0;
}

STATIC void push_ddt(ddt *pddt)
{
  dos_far_ptr fddt = DynAlloc("ddt", 1, sizeof(ddt));
  guest_write_block(((uint32_t)FP_SEG(fddt) << 4) + FP_OFF(fddt), pddt, sizeof(ddt));
  if (pddt->ddt_logdriveno != 0) {
    uint32_t prev = EFFECTIVE(fddt) - sizeof(ddt);
    pstore16(prev + offsetof(ddt, ddt_next), FP_OFF(fddt));
    pstore16(prev + offsetof(ddt, ddt_next) + sizeof(UWORD), FP_SEG(fddt));
    if (pddt->ddt_driveno == 0 && pddt->ddt_logdriveno == 1) {
      UWORD flags = pload16(prev + offsetof(ddt, ddt_descflags));
      pstore16(prev + offsetof(ddt, ddt_descflags), flags | DF_CURLOG | DF_MULTLOG);
    }
  }
}

STATIC void make_ddt (CPU* cpu, ddt *pddt, int Unit, int driveno, int flags)
{
  memset(pddt, 0, sizeof(*pddt));
  pddt->ddt_next = MK_FP(0, 0xffff);
  pddt->ddt_logdriveno = Unit;
  pddt->ddt_driveno = driveno;
  pddt->ddt_type = init_getdriveparm(cpu, driveno, &pddt->ddt_defbpb);
  pddt->ddt_ncyl = (pddt->ddt_type & 7) ? 80 : 40;
  pddt->ddt_descflags = init_readdasd(cpu, driveno) | flags;

  pddt->ddt_offset = 0;
  pddt->ddt_serialno = 0x12345678l;
  memcpy(&pddt->ddt_bpb, &pddt->ddt_defbpb, sizeof(bpb));
  push_ddt(pddt);
}

static int BIOS_nrdrives(CPU* cpu)
{
  CPU_AH = 0x08;
  CPU_DL = 0x80;
  init_bios_13h(cpu); // GET DRIVE PARAMETERS
  if (cf)
  {
    printf("no hard disks detected\n");
    return 0;
  }
  return CPU_DL;
}

struct PartTableEntry           /* INTERNAL representation of partition table entry */
{
  UBYTE Bootable;
  UBYTE FileSystem;
  struct CHS Begin;
  struct CHS End;
  ULONG RelSect;
  ULONG NumSect;
};

struct DriveParamS {
  UBYTE driveno;                /* = 0x8x                           */
  UWORD descflags;
  ULONG total_sectors;

  struct CHS chs;               /* for normal   INT 13 */
};

#define MSDOS_EXT_SIGN 0x29     /* extended boot sector signature */
#define MSDOS_FAT12_SIGN "FAT12   "     /* FAT12 filesystem signature */
#define MSDOS_FAT16_SIGN "FAT16   "     /* FAT16 filesystem signature */
#define MSDOS_FAT32_SIGN "FAT32   "     /* FAT32 filesystem signature */

/* local - returned and used for BIOS interface INT 13, AH=48*/
struct _bios_LBA_disk_parameterS {
  UWORD size;
  UWORD information;
  ULONG cylinders;
  ULONG heads;
  ULONG sectors;

  ULONG totalSect;
  ULONG totalSectHigh;
  UWORD BytesPerSector;

  ULONG eddparameters;
};

/* Get the parameters of the hard disk */
STATIC int LBA_Get_Drive_Parameters(CPU* cpu, int drive, struct DriveParamS *driveParam, int firstPass)
{
  if (firstPass && (InitKernelConfig.Verbose >= 1))
    printf("Checking for LBA support in BIOS for drive %02x\n", drive);

  memset(driveParam, 0, sizeof *driveParam);
  drive |= 0x80;

  /* use CHS if LBA support is not enabled by kernel configuration */
  if (!InitKernelConfig.GlobalEnableLBAsupport)
  {
    if (firstPass && (InitKernelConfig.Verbose >= 1)) printf("LBA support disabled.\n");
    goto StandardBios;
  }
  /* check for LBA support */
  CPU_BX = 0x55aa;
  CPU_AH = 0x41;
  CPU_DL = drive;
  SET_DS ( 0x40 );
  /* ds = 40h is to work around a Xi8088 ROM-BIOS bug,
      refer to https://github.com/FDOS/kernel/issues/156
      and https://www.bttr-software.de/forum/forum_entry.php?id=21275 */
  cf = 1;  /* ensure carry is set to force error if unsupported */

  init_bios_13h(cpu);

  if ((cf) || CPU_BX != 0xaa55 || !(CPU_CX & 0x01))
  {
    /* error conditions:
        carry set or BX != 0xaa55 => no EDD spec compatible BIOS (LBA extensions not supported)
        CX bit 1 is set if BIOS supports fixed disk subset (Disk Address Packet [DAP] subset),
        or clear if fixed disk access subset not supported by LBA extensions
    */
    goto StandardBios;
  }

  /* version 1.0, 2.0 have different verify */
  if (CPU_AH < 0x21)
    LBA_WRITE_VERIFY = 0x4301;  /* may be problematic if INT13 is hooked by
                                   different controllers / drivers */

  // put it on x86 stack RAM, do not move SP, since it is temporary
  dos_far_ptr lba_bios_parameters = MK_FP(CPU_SS, CPU_SP - sizeof(struct _bios_LBA_disk_parameterS));
  uint32_t lba_params_linear = EFFECTIVE(lba_bios_parameters);
  /* query disk size and DMA handling, geometry is queried later by INT13,08 */
  guest_fill_block(lba_params_linear, 0, sizeof(struct _bios_LBA_disk_parameterS));
  pstore16(lba_params_linear + offsetof(struct _bios_LBA_disk_parameterS, size),
           sizeof(struct _bios_LBA_disk_parameterS));

  CPU_SI = FP_OFF(lba_bios_parameters);
  SET_DS (FP_SEG(lba_bios_parameters));
  CPU_AH = 0x48;
  CPU_DL = drive;
  init_bios_13h(cpu);

  if (cf)
  {
    /* carry flag set indicates failed LBA disk parameter query */
    goto StandardBios;
  }

  ULONG lba_heads = pload32(lba_params_linear + offsetof(struct _bios_LBA_disk_parameterS, heads));
  ULONG lba_sectors = pload32(lba_params_linear + offsetof(struct _bios_LBA_disk_parameterS, sectors));
  ULONG lba_total = pload32(lba_params_linear + offsetof(struct _bios_LBA_disk_parameterS, totalSect));
  ULONG lba_total_high = pload32(lba_params_linear + offsetof(struct _bios_LBA_disk_parameterS, totalSectHigh));
  UWORD lba_information = pload16(lba_params_linear + offsetof(struct _bios_LBA_disk_parameterS, information));
  if (lba_heads > 0xffff ||
      lba_sectors > 0xffff ||
      (lba_total == 0 && lba_total_high == 0))
  {
    if (firstPass) 
    {
      printf("Suspicious LBA disk parameters, reverting to CHS access:\n");
      printf("  drive %02x, heads=%lu, sectors=%lu, total=0x%lx-%08lx\n",
           drive,
           (ULONG) lba_heads,
           (ULONG) lba_sectors,
           (ULONG) lba_total,
           (ULONG) lba_total_high);
    }

    goto StandardBios;
  }

  /* restrict disk size to 2TB, because we can not handle more */
  if (lba_total_high == 0)
  {
    driveParam->total_sectors = lba_total;
  }
  else
  {
    if (firstPass) printf("Drive %02x is too large to handle, restricted to 2TB\n", drive);
    driveParam->total_sectors = 0xffffffffUL;
  }

  /* if we arrive here, mark drive as LBA capable */
  driveParam->descflags = DF_LBA;
  if (lba_information & 8)
    driveParam->descflags |= DF_WRTVERIFY;

  if (lba_information & 1)
  {
    /* DMA boundary errors are handled transparently */
    driveParam->descflags |= DF_DMA_TRANSPARENT;
  }
  
StandardBios:   /* get disk geometry, and if LBA is not enabled, also size */
  if (firstPass && (InitKernelConfig.Verbose >= 1))
    printf("Retrieving CHS values for drive\n");

  CPU_AH = 0x08;
  CPU_DL = drive;

  init_bios_13h(cpu);

  if (cf) 
  {
    goto ErrorReturn;
  }

  /* int13h call returns max value, store as count (#) i.e. +1 for 0 based heads & cylinders */
  driveParam->chs.Head = (CPU_DX >> 8) + 1; /* DH = max head value = # of heads - 1 (0-255) */
  driveParam->chs.Sector = (CPU_CX & 0x3f); /* CL bits 0-5 = max sector value = # (sectors/track) - 1 (1-63) */
  /* max cylinder value = # cylinders - 1 (0-1023) = [high two bits]CL7:6=cyls9:8, [low byte]CH=cyls7:0 */
  driveParam->chs.Cylinder = ((CPU_CX >> 8) | ((CPU_CX & 0xc0) << 2)) + 1;
  
  if (driveParam->chs.Sector == 0) {
    /* happens e.g. with Bochs 1.x if no harddisk defined */
    driveParam->chs.Sector = 63; /* avoid division by zero...! */
    if (firstPass && (InitKernelConfig.Verbose >= 0)) 
      printf("BIOS reported 0 sectors/track, assuming 63!\n");
  }

  if (!(driveParam->descflags & DF_LBA))
  {
    driveParam->total_sectors =
        (ULONG)driveParam->chs.Cylinder
        * driveParam->chs.Head * driveParam->chs.Sector;
  }

  driveParam->driveno = drive;

  DebugPrintf(("drive %02Xh total: C = %u, H = %u, S = %u,",
               drive,
               driveParam->chs.Cylinder,
               driveParam->chs.Head, driveParam->chs.Sector));
  DebugPrintf((" total size %luMB\n\n", driveParam->total_sectors / 2048));

  return driveParam->driveno;


ErrorReturn:
  /* to avoid division by zero later, use some sane defaults */
  driveParam->total_sectors = 0;
  driveParam->chs.Head = 16;
  driveParam->chs.Sector = 63;
  return 0;
}

#define SCAN_PRIMARYBOOT 0x00
#define SCAN_PRIMARY     0x01
#define SCAN_EXTENDED    0x02
#define SCAN_PRIMARY2    0x03

#define LBA_to_CHS   init_LBA_to_CHS

/*
    translate LBA sectors into CHS addressing
    initially copied and pasted from dsk.c!

    LBA to/from CHS conversion - see http://www.ata-atapi.com/ How It Works section on CHSxlat - CHS Translation
    LBA (logical block address) simple 0 to N-1 used internally and with extended int 13h (BIOS)
    L-CHS (logical CHS) is the CHS view when using int 13h (BIOS)
    P-CHS (physical CHS) is the CHS view when directly accessing disk, should not, but could be used in BS or MBR

    LBA = ( (cylinder * heads_per_cylinder + heads ) * sectors_per_track ) + sector - 1

    cylinder = LBA / (heads_per_cylinder * sectors_per_track)
        temp = LBA % (heads_per_cylinder * sectors_per_track)
        head = temp / sectors_per_track
      sector = temp % sectors_per_track + 1

    where heads_per_cylinder and sectors_per_track are the current translation mode values.
    cyclinder and heads are 0 to N-1 based, sector is 1 to N based
*/

void init_LBA_to_CHS(struct CHS *chs, ULONG LBA_address,
                     struct DriveParamS *driveparam)
{
  unsigned hs = driveparam->chs.Sector * driveparam->chs.Head;
  unsigned hsrem = (unsigned)(LBA_address % hs);
  
  LBA_address /= hs;

  chs->Cylinder = LBA_address >= 0x10000ul ? 0xffffu : (unsigned)LBA_address;
  chs->Head = hsrem / driveparam->chs.Sector;
  chs->Sector = hsrem % driveparam->chs.Sector + 1;
}

int Read1LBASector(CPU* cpu, struct DriveParamS *driveParam, unsigned drive,
                   ULONG LBA_address, dos_far_ptr buffer)
{
  uint32_t dap_linear = EFFECTIVE(x86_dap);
  pstore8(dap_linear + offsetof(struct _bios_LBA_address_packet, packet_size),
          sizeof(struct _bios_LBA_address_packet));

  struct CHS chs;
  int num_retries;

/* disabled because this should not happen and if it happens the BIOS
   should complain; also there are weird disks around with
   CMOS geometry < real geometry */
#if 0
  if (LBA_address >= driveParam->total_sectors)
  {
    printf("LBA-Transfer error : address overflow = %lu, > %lu total sectors\n",
           LBA_address, driveParam->total_sectors);
    return 1;
  }
#endif

  for (num_retries = 0; num_retries < N_RETRY; num_retries++)
  {
    if (InitKernelConfig.Verbose >= 1)
    {
        printf("retry# %i sector %lu\n", num_retries, LBA_address);
    }

    CPU_DL = drive | 0x80;
    LBA_to_CHS(&chs, LBA_address, driveParam);
    /* Some old "security" software (PROT) traps int13 and assumes non
       LBA accesses. This statement causes partition tables to be read
       using CHS methods even if LBA is available unless CHS can't reach
       them. This can be overridden using kernel config parameters and
       the extended LBA partition type indicator.
    */
    if ((driveParam->descflags & DF_LBA) &&
        (InitKernelConfig.ForceLBA || ExtLBAForce || (chs.Cylinder > 1023)))
    {
      if (InitKernelConfig.Verbose >= 1) printf("LBA mode\n");
      pstore16(dap_linear + offsetof(struct _bios_LBA_address_packet, number_of_blocks), 1);
      pstore16(dap_linear + offsetof(struct _bios_LBA_address_packet, buffer_address),
               FP_OFF(buffer));
      pstore16(dap_linear + offsetof(struct _bios_LBA_address_packet, buffer_address) + sizeof(UWORD),
               FP_SEG(buffer));
      pstore32(dap_linear + offsetof(struct _bios_LBA_address_packet, block_address_high), 0);
      pstore32(dap_linear + offsetof(struct _bios_LBA_address_packet, block_address), LBA_address);

      /* Load the registers and call the interrupt. */
      CPU_AX = LBA_READ;
      CPU_SI = FP_OFF(x86_dap);
      SET_DS ( FP_SEG(x86_dap));
    }
    else
    {                           /* transfer data, using old bios functions */
      if (InitKernelConfig.Verbose >= 1) printf("CHS mode\n");
      /* avoid overflow at end of track */

      if (chs.Cylinder > 1023)
      {
        printf("LBA-Transfer error : address = %lu, cylinder %u > 1023\n", LBA_address, chs.Cylinder);
        return 1;
      }

      CPU_AX = 0x0201;
      CPU_BX = FP_OFF(buffer);
      CPU_CX =
          ((chs.Cylinder & 0xff) << 8) + ((chs.Cylinder & 0x300) >> 2) +
          chs.Sector;
      CPU_DH = chs.Head;
      SET_ES ( FP_SEG(buffer));
    }                           /* end of retries */
    init_bios_13h(cpu);
    if (InitKernelConfig.Verbose >= 1)
      printf("BIOS13 returned CF=%u\n", cf ? 1u : 0u);
    if (cf == 0)
      break;

    BIOS_drive_reset(cpu, driveParam->driveno);
  }

  return cf;
}

/*
    converts physical into logical representation of partition entry
*/

STATIC void ConvCHSToIntern(struct CHS *chs, uint32_t pDisk)
{
  UBYTE b1 = pload8(pDisk + 1);
  chs->Head = pload8(pDisk);
  chs->Sector = b1 & 0x3f;
  chs->Cylinder = pload8(pDisk + 2) + ((b1 & 0xc0) << 2);
}

static BOOL ConvPartTableEntryToIntern(struct PartTableEntry *pEntry,
                                       dos_far_ptr disk)
{
  int i;
  uint32_t pDisk = EFFECTIVE(disk);

  if (pload8(pDisk + 0x1fe) != 0x55 || pload8(pDisk + 0x1ff) != 0xaa)
  {
    memset(pEntry, 0, 4 * sizeof(struct PartTableEntry));

    return FALSE;
  }

  pDisk += 0x1be;

  for (i = 0; i < 4; i++, pDisk += 16, pEntry++)
  {
    pEntry->Bootable = pload8(pDisk);
    pEntry->FileSystem = pload8(pDisk + 4);

    ConvCHSToIntern(&pEntry->Begin, pDisk + 1);
    ConvCHSToIntern(&pEntry->End, pDisk + 5);

    pEntry->RelSect = pload32(pDisk + 8);
    pEntry->NumSect = pload32(pDisk + 12);
  }
  return TRUE;
}


#define FAT12           0x01
#define FAT16SMALL      0x04
#define EXTENDED        0x05
#define FAT16LARGE      0x06
#define FAT32           0x0b    /* FAT32 partition that ends before the 8.4  */
                              /* GB boundary                               */
#define FAT32_LBA       0x0c    /* FAT32 partition that ends after the 8.4GB */
                              /* boundary.  LBA is needed to access this.  */
#define FAT16_LBA       0x0e    /* like 0x06, but it is supposed to end past */
                              /* the 8.4GB boundary                        */
#define FAT12_LBA       0xff    /* fake FAT12 LBA entry for internal use     */
#define EXTENDED_LBA    0x0f    /* like 0x05, but it is supposed to end past */

/* Let's play it safe and do not allow partitions with clusters above  *
 * or equal to 0xff0/0xfff0/0xffffff0 to be created                    *
 * the problem with fff0-fff6 is that they might be interpreted as BAD *
 * even though the standard BAD value is ...ff7                        */

#define FAT12MAX        (FAT_MAGIC-6)
#define FAT16MAX        (FAT_MAGIC16-6)
#define FAT32MAX        (FAT_MAGIC32-6)

#define IsExtPartition(parttyp) ((parttyp) == EXTENDED || \
                                 (parttyp) == EXTENDED_LBA )

#define IsLBAPartition(parttyp) ((parttyp) == FAT12_LBA  || \
                                 (parttyp) == FAT16_LBA  || \
                                 (parttyp) == FAT32_LBA)

#ifdef WITHFAT32
#define IsFATPartition(parttyp) ((parttyp) == FAT12      || \
                                 (parttyp) == FAT16SMALL || \
                                 (parttyp) == FAT16LARGE || \
                                 (parttyp) == FAT16_LBA  || \
                                 (parttyp) == FAT32      || \
                                 (parttyp) == FAT32_LBA)
#else
#define IsFATPartition(parttyp) ((parttyp) == FAT12      || \
                                 (parttyp) == FAT16SMALL || \
                                 (parttyp) == FAT16LARGE || \
                                 (parttyp) == FAT16_LBA)
#endif

static BOOL is_suspect(struct CHS *chs, struct CHS *pEntry_chs)
{
  /* Valid entry:
     entry == chs ||           // partition entry equal to computed values
     (chs->Cylinder > 1023 &&  // or LBA partition
      (entry->Cylinder == 1023 ||
       entry->Cylinder == (0x3FF & chs->Cylinder)))
  */
  return !((pEntry_chs->Cylinder == chs->Cylinder &&
            pEntry_chs->Head     == chs->Head     &&
            pEntry_chs->Sector   == chs->Sector)        ||
           (chs->Cylinder > 1023u &&
            (pEntry_chs->Cylinder == 1023 ||
             pEntry_chs->Cylinder == (0x3ff & chs->Cylinder))));
}

static void printCHS(char *title, struct CHS *chs)
{
  /* has no fixed size for head/sect: is often 1/1 in our context */
  if (InitKernelConfig.Verbose >= 0) printf("%s%4u-%u-%u", title, chs->Cylinder, chs->Head, chs->Sector);
}

static void print_warning_suspect(char *partitionName, UBYTE fs, struct CHS *chs,
                           struct CHS *pEntry_chs)
{
  if (!InitKernelConfig.ForceLBA)
  {
    if (InitKernelConfig.Verbose >= 0) 
    {
      printf("WARNING: using suspect partition %s FS %02x:", partitionName, fs);
      printCHS(" with calculated values ", chs);
      printCHS(" instead of ", pEntry_chs);
      printf("\n");
    }
  }
  memcpy(pEntry_chs, chs, sizeof(struct CHS));
}

/* Compute ceil(a/b) */
#define cdiv(a, b) (((a) + (b) - 1) / (b))

/* calculates FAT data:
   code adapted by Bart Oldeman from mkdosfs from the Linux dosfstools:
      Author:       Dave Hudson
      Updated by:   Roman Hodek
      Portions copyright 1992, 1993 Remy Card
      and 1991 Linus Torvalds
*/
/* defaults: */
#define MAXCLUSTSIZE 128
#define NSECTORFAT12 8
#define NFAT 2

static VOID CalculateFATData(ddt * pddt, ULONG NumSectors, UBYTE FileSystem)
{
  ULONG fatdata;

  bpb *defbpb = &pddt->ddt_defbpb;

  /* FAT related items */
  defbpb->bpb_nfat = NFAT;
  /* normal value of number of entries in root dir */
  defbpb->bpb_ndirent = 512;
  defbpb->bpb_nreserved = 1;
  /* SEC_SIZE * DIRENT_SIZE / defbpb->bpb_ndirent + defbpb->bpb_nreserved */
  fatdata = NumSectors - (DIRENT_SIZE + 1);
  if (FileSystem == FAT12 || FileSystem == FAT12_LBA)
  {
    unsigned fatdat;
    /* in DOS, FAT12 defaults to 4096kb (8 sector) - clusters. */
    defbpb->bpb_nsector = NSECTORFAT12;
    /* Force maximal fatdata=32696 sectors since with our only possible sector
       size (512 bytes) this is the maximum for 4k clusters.
       #clus*secperclus+#fats*fatlength= 4077 * 8 + 2 * 12 = 32640.
       max FAT12 size for FreeDOS = 16,728,064 bytes */
    fatdat = (unsigned)fatdata;
    if (fatdata > 32640)
      fatdat = 32640;
    /* The "+2*NSECTORFAT12" is for the reserved first two FAT entries */
    defbpb->bpb_nfsect = (UWORD)cdiv((fatdat + 2 * NSECTORFAT12) * 3UL,
                                     FLOPPY_SEC_SIZE * 2 * NSECTORFAT12 + NFAT*3);
#ifdef DEBUG
    /* Need to calculate number of clusters, since the unused parts of the
     * FATS and data area together could make up space for an additional,
     * not really present cluster.
     * (This is really done in fatfs.c, bpbtodpb) */
    {
      unsigned clust = (fatdat - 2 * defbpb->bpb_nfsect) / NSECTORFAT12;
      unsigned maxclust = (defbpb->bpb_nfsect * 2 * FLOPPY_SEC_SIZE) / 3;
      if (maxclust > FAT12MAX)
        maxclust = FAT12MAX;
      printf("FAT12: #clu=%u, fatlength=%u, maxclu=%u, limit=%u\n",
             clust, defbpb->bpb_nfsect, maxclust, FAT12MAX);
      if (clust > maxclust - 2)
      {
        clust = maxclust - 2;
        printf("FAT12: too many clusters: setting to maxclu-2\n");
      }
    }
#endif
    memcpy(pddt->ddt_fstype, MSDOS_FAT12_SIGN, 8);
  }
  else
  { /* FAT16/FAT32 */
    CLUSTER fatlength, maxcl;
    unsigned long clust, maxclust, rest;
    unsigned fatentpersec;
    unsigned divisor;

#ifdef WITHFAT32
    if (FileSystem == FAT32 || FileSystem == FAT32_LBA)
    {
      /* For FAT32, use the cluster size table described in the FAT spec:
       * http://www.microsoft.com/hwdev/download/hardware/fatgen103.pdf
       */
      unsigned sz_gb = (unsigned)(NumSectors / 2097152UL);
      unsigned char nsector = 64; /* disks greater than 32 GB, 32K cluster */
      if (sz_gb <= 32)            /* disks up to 32 GB, 16K cluster */
        nsector = 32;
      if (sz_gb <= 16)            /* disks up to 16 GB, 8K cluster */
        nsector = 16;
      if (sz_gb <= 8)             /* disks up to 8 GB, 4K cluster */
        nsector = 8;
      if (NumSectors <= 532480UL)   /* disks up to 260 MB, 0.5K cluster */
        nsector = 1;
      defbpb->bpb_nsector = nsector;
      defbpb->bpb_ndirent = 0;
      defbpb->bpb_nreserved = 0x20;
      fatdata = NumSectors - 0x20;
      fatentpersec = FLOPPY_SEC_SIZE/4;  /* how many 32bit FAT values fit in a default 512 byte sector */
      maxcl = FAT32MAX;
    }
    else
#endif
    {
      /* FAT16: start at 4 sectors per cluster */
      defbpb->bpb_nsector = 4;
      /* Force maximal fatdata=8387584 sectors (NumSectors=8387617)
         since with our only possible sectorsize (512 bytes) this is the
         maximum we can address with 64k clusters
         #clus*secperclus+#fats*fatlength=65517 * 128 + 2 * 256=8386688.
         max FAT16 size for FreeDOS = 4,293,984,256 bytes = 4GiB-983,040 */
      if (fatdata > 8386688ul)
        fatdata = 8386688ul;
      fatentpersec = FLOPPY_SEC_SIZE/2; /* how many 16bit FAT values fit in a default 512 byte sector */
      maxcl = FAT16MAX;
    }

    DebugPrintf(("%lu sectors for FAT+data, starting with %u sectors/cluster\n", fatdata, defbpb->bpb_nsector));
    do
    {
      DebugPrintf(("Trying with %u sectors/cluster:\n", defbpb->bpb_nsector));
      divisor = fatentpersec * defbpb->bpb_nsector + NFAT; /* # of fat entries per cluster + 2 */
      rest = (unsigned)(fatdata % divisor);
      fatlength  = (CLUSTER)(fatdata / divisor);
      fatlength += (CLUSTER)((2 * defbpb->bpb_nsector + divisor + rest - 1) / divisor);

      /* Need to calculate number of clusters, since the unused parts of the
       * FATS and data area together could make up space for an additional,
       * not really present cluster. */
      clust = (fatdata - NFAT * fatlength) / defbpb->bpb_nsector;
      maxclust = fatlength * fatentpersec;
      if (maxclust > maxcl)
        maxclust = maxcl;
      DebugPrintf(("FAT: #clu=%lu, fatlen=%lu, maxclu=%lu, limit=%lu\n",
                   clust, (ULONG)fatlength, maxclust, (ULONG)maxcl));
      if (clust > maxclust - 2)
      {
        clust = 0;
        DebugPrintf(("FAT: too many clusters\n"));
      }
      else if (clust <= FAT_MAGIC)
      {
        /* The <= 4086 avoids that the filesystem will be misdetected as having a
         * 12 bit FAT. */
        DebugPrintf(("FAT: would be misdetected as FAT12\n"));
        clust = 0;
      }
      if (clust)
        break;
      defbpb->bpb_nsector <<= 1;
    }
    while (defbpb->bpb_nsector && defbpb->bpb_nsector <= MAXCLUSTSIZE);
#ifdef WITHFAT32
    if (FileSystem == FAT32 || FileSystem == FAT32_LBA)
    {
      defbpb->bpb_nfsect = 0;
      defbpb->bpb_xnfsect = fatlength;
      /* set up additional FAT32 fields */
      defbpb->bpb_xflags = 0;
      defbpb->bpb_xfsversion = 0;
      defbpb->bpb_xrootclst = 2;
      defbpb->bpb_xfsinfosec = 1;
      defbpb->bpb_xbackupsec = 6;
      memcpy(pddt->ddt_fstype, MSDOS_FAT32_SIGN, 8);
    }
    else
#endif
    {
      defbpb->bpb_nfsect = (UWORD)fatlength;
      memcpy(pddt->ddt_fstype, MSDOS_FAT16_SIGN, 8);
    }
  }
  pddt->ddt_fstype[8] = '\0';
}

static void DosDefinePartition(
    CPU* cpu, struct DriveParamS *driveParam,
    ULONG StartSector, struct PartTableEntry *pEntry,
    int extendedPartNo, int PrimaryNum)
{
  ddt nddt;
  ddt *pddt = &nddt;
  struct CHS chs;

  memset(&nddt, 0, sizeof(nddt));

  if (nUnits >= NDEV)
  {
    printf("more Partitions detected then possible, max = %d\n", NDEV);
    return;                     /* we are done */
  }

  pddt->ddt_next = MK_FP(0, 0xffff);
  pddt->ddt_driveno = driveParam->driveno;
  pddt->ddt_logdriveno = nUnits;
  pddt->ddt_descflags = driveParam->descflags;
  /* Turn off LBA if not forced and the partition is within 1023 cyls and of the right type */
  /* the FileSystem type was internally converted to LBA_xxxx if a non-LBA partition
     above cylinder 1023 was found */
  if (!(InitKernelConfig.ForceLBA || IsLBAPartition(pEntry->FileSystem) || ExtLBAForce))
    pddt->ddt_descflags &= ~DF_LBA;
  pddt->ddt_ncyl = driveParam->chs.Cylinder;

  DebugPrintf(("LBA %senabled for drive %c:\n", (pddt->ddt_descflags & DF_LBA)?"":"not ", 'A' + nUnits));

  pddt->ddt_offset = StartSector;

  pddt->ddt_defbpb.bpb_nbyte = FLOPPY_SEC_SIZE;
  pddt->ddt_defbpb.bpb_mdesc = 0xf8;
  pddt->ddt_defbpb.bpb_nheads = driveParam->chs.Head;
  pddt->ddt_defbpb.bpb_nsecs = driveParam->chs.Sector;
  pddt->ddt_defbpb.bpb_hidden = pEntry->RelSect;

  pddt->ddt_defbpb.bpb_nsize = 0;
  pddt->ddt_defbpb.bpb_huge = pEntry->NumSect;
  if (pEntry->NumSect <= 0xffff)
  {
    pddt->ddt_defbpb.bpb_nsize = (UWORD) (pEntry->NumSect);
    pddt->ddt_defbpb.bpb_huge = 0;  /* may still be set on Win95 */
  }

  /* sectors per cluster, sectors per FAT etc. */
  CalculateFATData(pddt, pEntry->NumSect, pEntry->FileSystem);

  pddt->ddt_serialno = 0x12345678l;
  /* drive inaccessible until bldbpb successful */
  pddt->ddt_descflags |= init_readdasd(cpu, pddt->ddt_driveno) | DF_NOACCESS;
  pddt->ddt_type = 5;
  memcpy(&pddt->ddt_bpb, &pddt->ddt_defbpb, sizeof(bpb));

  push_ddt(pddt);

  /* Alain whishes to keep this in later versions, too 
     Tom likes this too, so he made it configurable by SYS CONFIG ...
   */

  if (InitKernelConfig.InitDiskShowDriveAssignment)
  {
    char *ExtPri;
    int num;

    LBA_to_CHS(&chs, StartSector, driveParam);

    ExtPri = "Pri";
    num = PrimaryNum + 1;
    if (extendedPartNo)
    {
      ExtPri = "Ext";
      num = extendedPartNo;
    }
    printf("%c: HD%d, %s[%2d]", 'A' + nUnits,
           (driveParam->driveno & 0x7f) + 1, ExtPri, num);

    printCHS(", CHS= ", &chs);

    printf(", start=%6lu MB, size=%6lu MB\n",
           StartSector / 2048, pEntry->NumSect / 2048);
  }

  nUnits++;
}

static BOOL ScanForPrimaryPartitions(
    CPU* cpu,
    struct DriveParamS * driveParam, int scan_type,
    struct PartTableEntry * pEntry, ULONG startSector,
    int partitionsToIgnore, int extendedPartNo)
{
  int i;
  struct CHS chs, end;
  ULONG partitionStart;
  char partitionName[12];

  for (i = 0; i < 4; i++, pEntry++)
  {
    if (pEntry->FileSystem == 0)
      continue;

    if (partitionsToIgnore & (1 << i))
      continue;

    if (IsExtPartition(pEntry->FileSystem))
      continue;

    if (scan_type == SCAN_PRIMARYBOOT && !pEntry->Bootable)
      continue;

    partitionStart = startSector + pEntry->RelSect;

    if (!IsFATPartition(pEntry->FileSystem))
    {
      continue;
    }

    if (extendedPartNo)
      sprintf(partitionName, "Ext:%d", extendedPartNo);
    else
      sprintf(partitionName, "Pri:%d", i + 1);

    /*
       some sanity checks, that partition
       structure is OK
     */
    LBA_to_CHS(&chs, partitionStart, driveParam);
    LBA_to_CHS(&end, partitionStart + pEntry->NumSect - 1, driveParam);

    /* some FDISKs enter for partitions 
       > 8 GB cyl = 1023, other (cyl&1023)
     */

    if (is_suspect(&chs, &pEntry->Begin))
    {
      print_warning_suspect(partitionName, pEntry->FileSystem, &chs,
                            &pEntry->Begin);
    }

    if (is_suspect(&end, &pEntry->End))
    {
      if (pEntry->NumSect == 0)
      {
        printf("Not using partition %s with 0 sectors\n", partitionName);
        continue;
      }
      print_warning_suspect(partitionName, pEntry->FileSystem, &end,
                            &pEntry->End);
    }

    if (chs.Cylinder > 1023 || end.Cylinder > 1023)
    {

      /* if partition exceeds bounds of CHS addressing but LBA is not supported then skip partition */
      if (!(driveParam->descflags & DF_LBA))
      {
        printf
            ("can't use LBA partition without LBA support - part %s FS %02x",
             partitionName, pEntry->FileSystem);

        printCHS(" start ", &chs);
        printCHS(", end ", &end);
        printf("\n");

        continue;
      }

      /* if partition exceeds bounds of CHS addressing and we can use LBA 
	     but partition type indicates to use CHS then print warning 
         and force internal filesystem indicator to enable LBA
      */
      if (!(InitKernelConfig.ForceLBA || IsLBAPartition(pEntry->FileSystem) || ExtLBAForce))
      {
        printf
            ("WARNING: Partition ID does not suggest LBA - part %s FS %02x.\n"
             "Please run FDISK to correct this - using LBA to access partition.\n",
             partitionName, pEntry->FileSystem);

        printCHS(" start ", &chs);
        printCHS(", end ", &end);
        printf("\n");
        pEntry->FileSystem = (pEntry->FileSystem == FAT12 ? FAT12_LBA :
                              pEntry->FileSystem == FAT32 ? FAT32_LBA :
                              /*  pEntry->FileSystem == FAT16 ? */
                              FAT16_LBA);
      }

      /* else its a diagnostic message only */
#ifdef DEBUG
      printf("found and using LBA partition %s FS %02x",
             partitionName, pEntry->FileSystem);
      printCHS(" start ", &chs);
      printCHS(", end ", &end);
      printf("\n");
#endif
    }

    /*
       here we have a partition table in our hand !!
     */

    partitionsToIgnore |= 1 << i;

    DosDefinePartition(cpu, driveParam, partitionStart, pEntry, extendedPartNo, i);

    if (scan_type == SCAN_PRIMARYBOOT || scan_type == SCAN_PRIMARY)
    {
      return partitionsToIgnore;
    }
  }

  return partitionsToIgnore;
}

/* Load the Partition Tables and get information on all drives */
static int ProcessDisk(CPU* cpu, int scanType, unsigned drive, int PartitionsToIgnore)
{
  /* note: error messages are only printed on first call, where (scanType==SCAN_PRIMARYBOOT) */

  struct PartTableEntry PTable[4];
  ULONG RelSectorOffset;
  ULONG ExtendedPartitionOffset;
  int iPart;
  int strangeHardwareLoop;

  int num_extended_found = 0;

  struct DriveParamS driveParam;

  /* Get the hard drive parameters and ensure that the drive exists. */
  /* If there was an error accessing the drive, skip that drive. */

  if (!LBA_Get_Drive_Parameters(cpu, drive, &driveParam, (scanType == SCAN_PRIMARYBOOT)))
  {
    printf("can't get drive parameters for drive %02x\n", drive);
    return PartitionsToIgnore;
  }

  RelSectorOffset = 0;          /* boot sector */
  ExtendedPartitionOffset = 0;  /* not found yet */
  ExtLBAForce = 0;      /* initially we are not dealing with partitions
                           within a type 0x0E LBA extended partition,
                           so we do not enforce LBA access by now  */

    /* Read the Primary Partition Table. */
ReadNextPartitionTable:
    strangeHardwareLoop = 0;
    strange_restart:
  // used only on the init-time
  dos_far_ptr InitDiskTransferBuffer = MK_FP(0x8000, 0000);

  if (Read1LBASector(cpu, &driveParam, drive, RelSectorOffset, InitDiskTransferBuffer))
  {
    printf("Error reading partition table drive %02Xh sector %lu", drive,
           RelSectorOffset);
    return PartitionsToIgnore;
  }

  if (!ConvPartTableEntryToIntern(PTable, InitDiskTransferBuffer))
  {
    /* there is some strange hardware out in the world,
       which returns OK on first read, but the data are
       rubbish. simply retrying works fine.
       there is no logic behind this, but it works TE */

    if (++strangeHardwareLoop < 3)
      goto strange_restart;

    if (scanType==SCAN_PRIMARYBOOT) printf("illegal partition table - drive %02x sector %lu\n", drive,
           RelSectorOffset);
    return PartitionsToIgnore;
  }

  if (scanType == SCAN_PRIMARYBOOT ||
      scanType == SCAN_PRIMARY ||
      scanType == SCAN_PRIMARY2 || num_extended_found != 0)
  {

    PartitionsToIgnore = ScanForPrimaryPartitions(cpu, &driveParam, scanType,
                                                  PTable, RelSectorOffset,
                                                  PartitionsToIgnore,
                                                  num_extended_found);
  }

  if (scanType != SCAN_EXTENDED)
  {
    return PartitionsToIgnore;
  }

  /* scan for extended partitions now */
  PartitionsToIgnore = 0;

  for (iPart = 0; iPart < 4; iPart++)
  {
    if (IsExtPartition(PTable[iPart].FileSystem))
    {
      RelSectorOffset = ExtendedPartitionOffset + PTable[iPart].RelSect;

      if (ExtendedPartitionOffset == 0) /* first extended in chain? */
      {
        ExtendedPartitionOffset = PTable[iPart].RelSect;
        /* grand parent LBA -> all children and grandchildren LBA */
        ExtLBAForce = (PTable[iPart].FileSystem == EXTENDED_LBA);
      }

      num_extended_found++;

      if (num_extended_found > 30)
      {
        printf("found more then 30 extended partitions, terminated\n");
        return 0;
      }

      goto ReadNextPartitionTable;
    }
  }

  return PartitionsToIgnore;
}

static void ReadAllPartitionTables(CPU* cpu)
{
    UBYTE foundPartitions[MAX_HARD_DRIVE];
    int HardDrive;
    int nHardDisk;
    ddt nddt;
    /* Setup media info and BPBs arrays for floppies */
    make_ddt(cpu, &nddt, 0, 0, 0);

    /*
     this is a quick patch - see if B: exists
     test for A: also, need not exist
    */
    bios_11h(cpu);  /* get equipment list */
    /*if ((regs.AL & 1)==0)*//* no floppy drives installed  */
    if ((CPU_AL & 1) && (CPU_AL & 0xc0))
    {
        /* floppy drives installed and a B: drive */
        make_ddt(cpu, &nddt, 1, 1, 0);
    }
    else
    {
        /* set up the DJ method : multiple logical drives */
        make_ddt(cpu, &nddt, 1, 0, DF_MULTLOG);
    }

    /* Initial number of disk units                                 */
    nUnits = 2;

    nHardDisk = BIOS_nrdrives(cpu);
    if (nHardDisk > LENGTH(foundPartitions))
        nHardDisk = LENGTH(foundPartitions);

    DebugPrintf(("DSK init: found %d disk drives\n", nHardDisk));

    /* Reset the drives                                             */
    for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
    {
        BIOS_drive_reset(cpu, HardDrive);
        foundPartitions[HardDrive] = 0;
    }

    if (InitKernelConfig.DLASortByDriveNo == 0)
    {
        if (InitKernelConfig.Verbose >= 1) printf("Drive Letter Assignment - DOS order\n");

        /* Process primary partition table   1 partition only      */
        for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
        {
            foundPartitions[HardDrive] = ProcessDisk(cpu, SCAN_PRIMARYBOOT, HardDrive, 0);

            if (foundPartitions[HardDrive] == 0)
                foundPartitions[HardDrive] = ProcessDisk(cpu, SCAN_PRIMARY, HardDrive, 0);
        }

        /* Process extended partition table                      */
        for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
        {
            ProcessDisk(cpu, SCAN_EXTENDED, HardDrive, 0);
        }

    /* Process primary a 2nd time */
    for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
    {
      ProcessDisk(cpu, SCAN_PRIMARY2, HardDrive, foundPartitions[HardDrive]);
    }
  }
  else
  {
    UBYTE bootdrv = pload8(EFFECTIVE(MK_FP(0, 0x5e0)));

    if (InitKernelConfig.Verbose >= 1) printf("Drive Letter Assignment - sorted by drive\n");

    /* Process primary partition table   1 partition only      */
    for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
    {
      struct DriveParamS driveParam;
      if (LBA_Get_Drive_Parameters(cpu, HardDrive, &driveParam, 0) &&
          driveParam.driveno == bootdrv)
      {
        foundPartitions[HardDrive] =
          ProcessDisk(cpu, SCAN_PRIMARYBOOT, HardDrive, 0);
        break;
      }
    }

    for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
    {
      if (foundPartitions[HardDrive] == 0)
      {
        foundPartitions[HardDrive] =
          ProcessDisk(cpu, SCAN_PRIMARYBOOT, HardDrive, 0);

        if (foundPartitions[HardDrive] == 0)
          foundPartitions[HardDrive] =
            ProcessDisk(cpu, SCAN_PRIMARY, HardDrive, 0);
      }

      /* Process extended partition table                      */
      ProcessDisk(cpu, SCAN_EXTENDED, HardDrive, 0);

      /* Process primary a 2nd time */
      ProcessDisk(cpu, SCAN_PRIMARY2, HardDrive, foundPartitions[HardDrive]);
    }
  }
  
  if (InitKernelConfig.Verbose >= 0)
  {
    unsigned foundPartitionsCount = 0;
    /* Tell user if no valid partitions found on any hard drive     */
    for (HardDrive = 0; HardDrive < nHardDisk; HardDrive++)
    {
      foundPartitionsCount += foundPartitions[HardDrive];
    }
    /* printf("Found %i partitions\n", foundPartitionsCount); */
    if (!foundPartitionsCount) printf("No supported partitions found.\n");
  }
}

/* disk initialization: returns number of units */
COUNT dsk_init(CPU* cpu) {
  if (InitKernelConfig.Verbose >= 1) printf("\nInitDisk\n");

  /* Reset the drives                                             */
  BIOS_drive_reset(cpu, 0);

  ReadAllPartitionTables(cpu);

  return nUnits;
}
