/****************************************************************/
/*                                                              */
/*                           device.h                           */
/*                      Device Driver Header File               */
/*                                                              */
/*                       November 20, 1991                      */
/*                                                              */
/*                      Copyright (c) 1995                      */
/*                      Pasquale J. Villani                     */
/*                      All Rights Reserved                     */
/*                                                              */
/* This file is part of DOS-C.                                  */
/*                                                              */
/* DOS-C is free software; you can redistribute it and/or       */
/* modify it under the terms of the GNU General Public License  */
/* as published by the Free Software Foundation; either version */
/* 2, or (at your option) any later version.                    */
/*                                                              */
/* DOS-C is distributed in the hope that it will be useful, but */
/* WITHOUT ANY WARRANTY; without even the implied warranty of   */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See    */
/* the GNU General Public License for more details.             */
/*                                                              */
/* You should have received a copy of the GNU General Public    */
/* License along with DOS-C; see the file COPYING.  If not,     */
/* write to the Free Software Foundation, 675 Mass Ave,         */
/* Cambridge, MA 02139, USA.                                    */
/****************************************************************/

#pragma pack(push, 1)

/*
 *      Status Word Bits
 */

#define S_ERROR         0x8000  /* Error bit                    */
#define S_BUSY          0x0200  /* Device busy bit              */
#define S_DONE          0x0100  /* Device operation completed   */
#define S_MASK          0x00ff  /* Mask to extract error code   */

/*
 *      MEDIA Descriptor Byte Bits
 */

#define MD_2SIDE        1       /* MEDIA is two sided           */
#define MD_8SECTOR      2       /* MEDIA is eight sectored      */
#define MD_REMOVABLE    4       /* MEDIA is removable (floppy)  */

/*
 *      Media Return Codes
 */
#define M_CHANGED       -1      /* MEDIA was changed            */
#define M_DONT_KNOW     0       /* MEDIA state unkown           */
#define M_NOT_CHANGED   1       /* MEDIA was not changed        */

/*
 *      Error Return Codes
 */

#define E_WRPRT         0       /* Write Protect                */
#define E_UNIT          1       /* Unknown Unit                 */
#define E_NOTRDY        2       /* Device Not Ready             */
#define E_CMD           3       /* Unknown Command              */
#define E_CRC           4       /* Crc Error                    */
#define E_LENGTH        5       /* Bad Length                   */
#define E_SEEK          6       /* Seek Error                   */
#define E_MEDIA         7       /* Unknown MEDIA                */
#define E_NOTFND        8       /* Sector Not Found             */
#define E_PAPER         9       /* No Paper                     */
#define E_WRITE         10      /* Write Fault                  */
#define E_READ          11      /* Read Fault                   */
#define E_FAILURE       12      /* General Failure              */

/*
 *      Command codes
 */

#define C_INIT          0x00    /* Initialize                   */
#define C_MEDIACHK      0x01    /* MEDIA Check                  */
#define C_BLDBPB        0x02    /* Build BPB                    */
#define C_IOCTLIN       0x03    /* Ioctl In                     */
#define C_INPUT         0x04    /* Input (Read)                 */
#define C_NDREAD        0x05    /* Non-destructive Read         */
#define C_ISTAT         0x06    /* Input Status                 */
#define C_IFLUSH        0x07    /* Input Flush                  */
#define C_OUTPUT        0x08    /* Output (Write)               */
#define C_OUTVFY        0x09    /* Output with verify           */
#define C_OSTAT         0x0a    /* Output                       */
#define C_OFLUSH        0x0b    /* Output Flush                 */
#define C_IOCTLOUT      0x0c    /* Ioctl Out                    */
#define C_OPEN          0x0d    /* Device Open                  */
#define C_CLOSE         0x0e    /* Device Close                 */
#define C_REMMEDIA      0x0f    /* Removable MEDIA              */
#define C_OUB           0x10    /* Output till busy             */
#define C_GENIOCTL      0x13    /* Generic Ioctl                */
#define C_GETLDEV       0x17    /* Get Logical Device           */
#define C_SETLDEV       0x18    /* Set Logical Device           */
#define C_IOCTLQRY      0x19    /* Ioctl Query                  */

/*
 *      Convienence macros
 */
#define failure(x)      (S_ERROR+S_DONE+x)
#ifndef TRUE
#define TRUE            1
#endif
#ifndef FALSE
#define FALSE           0
#endif

/*
 *      structures
 */
struct request;
/* Device header.
 *
 * The x86 variant's dh_strategy/dh_interrupt are plain WORD *offsets*,
 * not far pointers: real MS-DOS/FreeDOS device driver headers (the
 * literal on-disk bytes of a .SYS file) store them as two 16-bit
 * offsets, always implicitly within the same segment as the header
 * itself (see x86_execrh() in kernel.c, which combines them with
 * FP_SEG() of the header's own address). Keeping this variant at 4
 * bytes total - the same size as the "arm" variant's single native
 * function pointer - is what keeps this union's layout byte-for-byte
 * compatible with the real, on-disk 18-byte header format
 * (dh_next[4] + dh_attr[2] + strategy[2] + interrupt[2] + dh_name[8]).
 * That compatibility is what makes it possible to load real,
 * unmodified x86 .SYS driver files straight into guest RAM (see
 * DosExec() in task.c) and run them as-is: if this union were instead
 * sized for two 4-byte dos_far_ptr (8 bytes), dh_name would be read 4
 * bytes past where every real driver file actually puts it.
 */
struct dhdr {
    /* dh_next chains device headers. For an x86 driver it is a guest
       seg:off; the built-in native drivers are still linked with guest
       pointers today (their headers live in guest RAM - only the
       dh_interrupt arm below is native), so this is a genuine guest
       pointer in every current path. Typed mixed_ptr to flag that a
       future genuinely-native external driver (see the ATTR_NATIVE
       TODO) could make it carry a packed native pointer instead, at
       which point every walk of the chain must gate on ATTR_NATIVE
       before choosing ARM_PTR vs NATIVE_ARM_PTR. */
    /*struct dhdr*/ mixed_ptr dh_next;
    UWORD dh_attr;
    union {
      struct {
        // no dh_strategy support for native drivers
        VOID(*dh_interrupt)(dos_far_ptr rq);
      } arm;
      struct {
        UWORD dh_strategy;    /* offset, within this header's own segment */
        UWORD dh_interrupt;   /* offset, within this header's own segment */
      } x86;
    };
    UBYTE dh_name[8];
};

#define ATTR_SUBST      0x8000
#define ATTR_CHAR       0x8000
#define ATTR_IOCTL      0x4000
#define ATTR_BLDFAT     0x2000
#define ATTR_REMOTE     0x1000
#define ATTR_EXCALLS    0x0800
#define ATTR_RAW        0x0400
#define ATTR_NATIVE     0x0200 // for RP2350 only for now
#define ATTR_RESERVED   0x0100
#define ATTR_QRYIOCTL   0x0080
#define ATTR_GENIOCTL   0x0040
#define ATTR_FASTCON    0x0010
#define ATTR_CLOCK      0x0008
#define ATTR_NULL       0x0004
#define ATTR_CONOUT     0x0002
#define ATTR_HUGE       0x0002
#define ATTR_CONIN      0x0001

/*                                                                      */
/* Bios Parameter Block structure                                       */
/*                                                                      */

#define FAT_NO_MIRRORING 0x80

#define BPB_SIZEOF 31           /* size of the standard BPB */

typedef struct {
  UWORD bpb_nbyte;              /* Bytes per Sector             */
  UBYTE bpb_nsector;            /* Sectors per Allocation Unit  */
  UWORD bpb_nreserved;          /* # Reserved Sectors           */
  UBYTE bpb_nfat;               /* # FATs                       */
  UWORD bpb_ndirent;            /* # Root Directory entries     */
  UWORD bpb_nsize;              /* Total volume Size in sectors */
  UBYTE bpb_mdesc;              /* MEDIA Descriptor Byte        */
  UWORD bpb_nfsect;             /* FAT size in sectors          */
  UWORD bpb_nsecs;              /* Sectors per track            */
  UWORD bpb_nheads;             /* Number of heads              */
  ULONG bpb_hidden;             /* Hidden sectors               */
  ULONG bpb_huge;               /* Total volume Size in sectors if*/
  /* bpb_nsize == 0               */
#ifdef WITHFAT32
  ULONG bpb_xnfsect;            /* FAT size in sectors if       */
  /* bpb_nfsect == 0              */
  UWORD bpb_xflags;             /* extended flags               */
  /* bit 7: disable mirroring     */
  /* bits 6-4: reserved (0)       */
  /* bits 3-0: active FAT number  */
  UWORD bpb_xfsversion;         /* filesystem version           */
  ULONG bpb_xrootclst;          /* starting cluster of root dir */
  UWORD bpb_xfsinfosec;         /* FS info sector number,       */
  /* 0xFFFF if unknown            */
  UWORD bpb_xbackupsec;         /* backup boot sector number    */
  /* 0xFFFF if unknown            */
#endif
} bpb;

#define N_RETRY         5       /* number of retries permitted  */

/* dskxfer() function parameters, migrated from globals.h */
#define DSKWRITE        1
#define DSKREAD         2
#define DSKWRITEINT26   3
#define DSKREADINT25    4

#include "dsk.h"

#define LBA_READ         0x4200
#define LBA_WRITE        0x4300

struct _bios_LBA_address_packet
                                           /* Used to access a hard disk via LBA */
 /*       Added by Brian E. Reifsnyder */
{
  unsigned char packet_size;    /* size of this packet...set to 16  */
  unsigned char reserved_1;     /* set to 0...unused                */
  UWORD number_of_blocks;       /* EDD DAP +02h: 16-bit sector count */
  dos_far_ptr buffer_address;    /* addr of transfer buffer          */
  unsigned long block_address;  /* LBA address                      */
  unsigned long block_address_high;     /* high bytes of LBA addr...unused  */
};

struct CHS {
  UWORD Cylinder;
  UWORD Head;
  UWORD Sector;
};

/* DOS 4.0-7.0 drive data table (see RBIL at INT2F,AX=0803) */
typedef struct ddtstruct {
  dos_far_ptr ddt_next;
  /* pointer to next table (offset FFFFh if last table) */
  UBYTE ddt_driveno;            /* physical unit number (for INT 13)     */
  UBYTE ddt_logdriveno;         /* logical drive number (0=A:)        */
  bpb ddt_bpb;                  /* BIOS Parameter Block */
  UBYTE ddt_flags;
  /* bit 6: 16-bit FAT instead of 12-bit
     bit 7: unsupportable disk (all accesses will return Not Ready) */
  UWORD ddt_FileOC;             /* Count of Open files on Drv */
  UBYTE ddt_type;               /* device type       */
  UWORD ddt_descflags;          /* bit flags describing drive */
  UWORD ddt_ncyl;               /* number of cylinders
                                   (for partition only, if hard disk) */
  bpb ddt_defbpb;               /* BPB for default (highest) capacity supported */
  UBYTE ddt_reserved[6];        /* (part of BPB above) */
  UBYTE ddt_ltrack;             /* last track accessed */
  union {
    ULONG ddt_lasttime;         /* removable media: time of last access
                                   in clock ticks (FFFFFFFFh if never) */
    struct {
      UWORD ddt_part;           /* partition (FFFFh = primary, 0001h = extended)
                                   always 0001h for DOS 5+ */
      UWORD ddt_abscyl;         /* absolute cylinder number of partition's
                                   start on physical drive
                                   (FFFFh if primary partition in DOS 4.x) */
    } ddt_hd;
  } ddt_fh;
  UBYTE ddt_volume[12];         /* ASCIIZ volume label or "NO NAME    " if none
                                   (apparently taken from extended boot record
                                   rather than root directory) */
  ULONG ddt_serialno;           /* serial number */
  UBYTE ddt_fstype[9];          /* ASCIIZ filesystem type ("FAT12   " or "FAT16   ") */
  ULONG ddt_offset;             /* relative partition offset */
} ddt;

/* description flag bits */
#define DF_FIXED      0x001     /* fixed media, ie hard drive */
#define DF_CHANGELINE 0x002     /* door lock or disk change detection reported as supported */
#define DF_CURBPBLOCK 0x004     /* current BPB locked, use existing BPB instead of building one */
#define DF_SAMESIZE   0x008     /* all sectors in a track are the same size */
#define DF_MULTLOG    0x010     /* physical drive represents multiple logical ones, eg A: & B: */
#define DF_CURLOG     0x020     /* active (current) logical drive for this physical drive */
#define DF_DISKCHANGE 0x040     /* disk change was detected */
#define DF_DPCHANGED  0x080     /* device parameters changed */
#define DF_REFORMAT   0x100     /* disk formatted so BPB has changed */
#define DF_NOACCESS   0x200     /* don't allow access (read or write) to fixed media */
/* freedos specific flag bits */
#define DF_LBA        0x400
#define DF_WRTVERIFY  0x800
#define DF_DMA_TRANSPARENT   0x1000 /* DMA boundary errors are handled transparently */

/* typedef struct ddtstruct ddt;*/

struct gblkio {
  UBYTE gbio_spcfunbit;
  UBYTE gbio_devtype;
  UWORD gbio_devattrib;
  UWORD gbio_ncyl;
  UBYTE gbio_media;
  bpb gbio_bpb;
  UWORD gbio_nsecs;
};

struct gblkfv                   /* for format / verify track */
{
  UBYTE gbfv_spcfunbit;
  UWORD gbfv_head;
  UWORD gbfv_cyl;
  UWORD gbfv_ntracks;
};

struct gblkrw                   /* for read / write track */
{
  UBYTE gbrw_spcfunbit;
  UWORD gbrw_head;
  UWORD gbrw_cyl;
  UWORD gbrw_sector;
  UWORD gbrw_nsecs;
  dos_far_ptr gbrw_buffer;
};

struct Gioc_media {
  WORD ioc_level;
  ULONG ioc_serialno;
  BYTE ioc_volume[11];
  BYTE ioc_fstype[8];
};

struct Access_info {
  BYTE AI_spec;
  BYTE AI_Flag;
};

/*                                                                      */
/* Boot Block (Super Block)                                             */
/*                                                                      */
/* See BPB comments for the offsets below                               */
/*                                                                      */
#define BT_JUMP         0
#define BT_OEM          3
#define BT_BPB          11
#define BT_SIZEOF       36

typedef struct {
  BYTE bt_jump[3];              /* Boot Jump opcodes            */
  BYTE bt_oem[8];               /* OEM Name                     */
  bpb bt_bpb;                   /* BPB for this media/device    */
  WORD bt_nsecs;                /* # Sectors per Track          */
  WORD bt_nheads;               /* # Heads                      */
  WORD bt_hidden;               /* # Hidden sectors             */
  LONG bt_huge;                 /* use if nsecs == 0            */
  BYTE bt_drvno;
  BYTE bt_reserv;
  BYTE bt_btid;
  ULONG bt_serialno;
  BYTE bt_volume[11];
  BYTE bt_fstype[8];
} boot;

/* File system information structure */
struct fsinfo {
  UDWORD fi_signature;          /* must be 0x61417272 */
  DWORD fi_nfreeclst;           /* number of free clusters, -1 if unknown */
  DWORD fi_cluster;             /* most recently allocated cluster, -1 if unknown */
  UBYTE fi_reserved[12];
};

typedef boot super;             /* Alias for boot structure             */

typedef struct request {
  UBYTE r_length;               /*  Request Header length               */
  UBYTE r_unit;                 /*  Unit Code                           */
  UBYTE r_command;              /*  Command Code                        */
  UWORD r_status;               /*  Status                              */
  BYTE r_reserved[8];           /*  DOS Reserved Area                   */
  union {
    struct {
      UBYTE _r_nunits;          /*  number of units     */
      dos_far_ptr _r_endaddr;   /*  Ending Address      */
      dos_far_ptr _r_bpbptr;    /*  ptr to BPB array (or, during
                                    C_INIT, to the rest of the
                                    DEVICE=/DEVICEHIGH= line, per
                                    real DOS convention - see
                                    init_device() in kernel.c) */
      UBYTE _r_firstunit;
    } _r_init;
    struct {
      BYTE _r_meddesc;          /*  MEDIA Descriptor    */
      BYTE _r_retcode;          /*  Return Code         */
      dos_far_ptr _r_vid;       /* volume id */
    } _r_media;
    struct {
      BYTE _r_meddesc;          /*  MEDIA Descriptor    */
      dos_far_ptr _r_fat;       /*  boot sector pointer */
      dos_far_ptr _r_bpbpt;     /*  ptr to BPB table    */
    } _r_bpb;
    struct {
      BYTE _r_meddesc;          /*  MEDIA Descriptor    */
      dos_far_ptr _r_trans;     /*  Transfer Address    */
      UWORD _r_count;           /*  Byte/Sector Count   */
      UWORD _r_start;           /*  Starting Sector No. */
      dos_far_ptr _r_vid;       /* Pointer to volume id */
      LONG _r_huge;             /* for > 32Mb drives    */
    } _r_rw;
    struct {
      unsigned char _r_ndbyte;  /*  Byte Read From Device       */
    } _r_nd;
    struct {
      UBYTE _r_cat;             /* Category code */
      UBYTE _r_fun;             /* Function code */
      UWORD _r_si;              /* Contents of SI and DI */
      UWORD _r_di;              /* (PC DOS 7 Technical Update, pp 104,105) */
      union
      {
        dos_far_ptr /*struct gblkio*/ _r_io;
        dos_far_ptr /*struct gblkrw*/ _r_rw;
        dos_far_ptr /*struct gblkfv*/ _r_fv;
        dos_far_ptr /*struct Gioc_media*/ _r_gioc;
        dos_far_ptr /*struct Access_info*/ _r_ai;
      } _r_par;                 /* Pointer to param. block from 440C/440D */
    } _r_gen;
  } _r_x;
} request;

#define HUGECOUNT       0xffff
#define MAXSHORT        0xffffl

/*
 * Macros to assist request structure legibility
 */

/* Init packet macros                                                   */
#define r_nunits        _r_x._r_init._r_nunits
#define r_endaddr       _r_x._r_init._r_endaddr
#define r_bpbptr        _r_x._r_init._r_bpbptr
#define r_firstunit     _r_x._r_init._r_firstunit

/* MEDIA Check packet macros                                            */
#define r_mcmdesc       _r_x._r_media._r_meddesc
#define r_mcretcode     _r_x._r_media._r_retcode
#define r_mcvid         _r_x._r_media._r_vid

/* Build BPB packet macros                                              */
#define r_bpmdesc       _r_x._r_bpb._r_meddesc
#define r_bpfat         _r_x._r_bpb._r_fat
#define r_bpptr         _r_x._r_bpb._r_bpbpt

/* rw packet macros                                                     */
#define r_meddesc       _r_x._r_rw._r_meddesc
#define r_trans         _r_x._r_rw._r_trans
#define r_count         _r_x._r_rw._r_count
#define r_start         _r_x._r_rw._r_start
#define r_rwvid         _r_x._r_rw._r_vid
#define r_huge          _r_x._r_rw._r_huge

/* ndread packet macros                                                 */
#define r_ndbyte        _r_x._r_nd._r_ndbyte

/* generic IOCTL and IOCTL query macros */
#define r_cat           _r_x._r_gen._r_cat
#define r_fun           _r_x._r_gen._r_fun
#define r_si            _r_x._r_gen._r_si
#define r_di            _r_x._r_gen._r_di
#define r_rw            _r_x._r_gen._r_par._r_rw
#define r_io            _r_x._r_gen._r_par._r_io
#define r_fv            _r_x._r_gen._r_par._r_fv
#define r_gioc          _r_x._r_gen._r_par._r_gioc
#define r_ai            _r_x._r_gen._r_par._r_ai

/*
 *interrupt support (spl & splx) support - IBM style
 */

#define I_NONE          0       /* Initial value                */

/* predefined interrupt levels - 8259 support                           */
#define IRQ0            0x01    /* Level 0 - highest            */
#define IRQ1            0x02
#define IRQ2            0x04
#define IRQ3            0x08
#define IRQ4            0x10
#define IRQ5            0x20
#define IRQ6            0x40
#define IRQ7            0x80    /* Level 7 - lowest             */

/* standard hardware configuration                                      */
#define I_RTC           IRQ0    /* Timer                        */
#define I_KBD           IRQ1    /* Keyboard                     */
#define I_COM2          IRQ3    /* COM1:                        */
#define I_COM1          IRQ4    /* COM2:                        */
#define I_HDC           IRQ5    /* Fixed disk                   */
#define I_FDC           IRQ6    /* Diskette                     */
#define I_PRT           IRQ7    /* Printer                      */

/* standard hardware vectors - 8259 defined                             */
#define V_RTC           0x08    /* Timer                        */
#define V_KBD           0x09    /* Keyboard                     */
#define V_LEV2          0x0a    /* Level 2 - uncomitted         */
#define V_COM2          0x0b    /* COM1:                        */
#define V_COM1          0x0c    /* COM2:                        */
#define V_HDC           0x0d    /* Fixed disk                   */
#define V_FDC           0x0e    /* Diskette                     */
#define V_PRT           0x0f    /* Printer                      */

#define V_LEV0          0x08    /* Level 0 - highest            */
#define V_LEV1          0x09
#define V_LEV2          0x0a    /* Level 2 - uncomitted         */
#define V_LEV3          0x0b
#define V_LEV4          0x0c
#define V_LEV5          0x0d
#define V_LEV6          0x0e
#define V_LEV7          0x0f    /* Level 7 - lowest             */

/*
 */
typedef request FAR *rqptr;
typedef bpb FAR *bpbptr;
typedef BYTE FAR *byteptr;
typedef struct dhdr FAR *dhdrptr;

/* Request headers live in guest SDA.  Persistent native aliases are
   intentionally unavailable; use request_ref/fdos_request_guest_ref. */

/* dsk.c */
/* blk_driver() is declared in init-mod.h: the native implementation takes
   the CPU* it needs for the BIOS INT 13h calls, so it cannot use rqptr-only
   ASM prototype of the original. */
/* Guest far pointer to a DDT entry. Runtime native code accesses it through ddt_ref. */
dos_far_ptr getddt_far(int dev);

/* error.c */
COUNT char_error_status(UWORD status, dos_far_ptr /* -> struct dhdr */ x86_lpDevice);
COUNT block_error_status(UWORD status, COUNT nDrive, dos_far_ptr /* -> struct dhdr */ x86_lpDevice, int mode);
/* sysclk.c */
WORD ASMCFUNC FAR clk_driver(rqptr rp);

WORD execrh(/*request*/ dos_far_ptr rq, /*struct dhdr*/ dos_far_ptr dhp);

/*
 *      end of device.h
 */

#pragma pack(pop)

/*
    ABI locks for the two structures that are read/written as raw bytes
    by code outside this kernel.

    struct dhdr IS the literal on-disk layout of a .SYS device driver
    header - DosExec() loads real, unmodified driver files straight into
    guest RAM and config.c walks them in place (see the long comment on
    struct dhdr above). 18 bytes, dh_name at +0x0A: if the arm/x86 union
    ever grew past 4 bytes, every real driver's name would be read from
    the wrong offset.

    request is copied verbatim into the SDA's fixed-size ClkReqHdr slot
    (dos_data +0x3A, 30 bytes wide - see lol.h, where clk_driver follows
    at +0x58). Growing it silently overruns the next SDA field.
*/
_Static_assert(sizeof(struct dhdr) == 18,
               "struct dhdr must stay 18 bytes: it is the on-disk .SYS device header layout");
_Static_assert(offsetof(struct dhdr, dh_name) == 0x0A,
               "dh_name must stay at +0x0A: real .SYS files put the 8-char name there");
_Static_assert(sizeof(request) == 30,
               "request must stay 30 bytes: it is copied into the SDA ClkReqHdr slot (dos_data +3Ah..+58h)");
