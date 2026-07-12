/****************************************************************/
/*                                                              */
/*                           lol.h                              */
/*                                                              */
/*              DOS List of Lists structure                     */
/*                                                              */
/*                      Copyright (c) 2003                      */
/*                         Bart Oldeman                         */
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
/* License along with DOS-C; if not, write to the Free Software */
/* Foundation, Inc., 59 Temple Place, Suite 330,                */
/* Boston, MA  02111-1307  USA.                                 */
/****************************************************************/

enum {LOC_CONV=0, LOC_HMA=1};

/* note: we start at DOSDS:0, but the "official" list of lists starts a
   little later at DOSDS:26 (this is what is returned by int21/ah=52) */
#pragma pack(push, 1)
struct lol {
/* it was char filler[0x22];
segment _FIXED_DATA

; Because of the following bytes of data, THIS MODULE MUST BE THE FIRST
; IN THE LINK SEQUENCE.  THE BYTE AT DS:0004 determines the SDA format in
; use.  A 0 indicates MS-DOS 3.X style, a 1 indicates MS-DOS 4.0-6.X style.
                global  DATASTART
DATASTART:
                global  _DATASTART
_DATASTART:
dos_data        db      0
                dw      kernel_start
                db      0               ; padding
                dw      1               ; Hardcoded MS-DOS 4.0+ style

                times (0eh - ($ - DATASTART)) db 0
                global  _NetBios
_NetBios        dw      0               ; NetBios Number

                times (26h - 0ch - ($ - DATASTART)) db 0
; Globally referenced variables - WARNING: DO NOT CHANGE ORDER
; BECAUSE THEY ARE DOCUMENTED AS UNDOCUMENTED (?) AND HAVE
; MANY MULTIPLEX PROGRAMS AND TSRs ACCESSING THEM
                global  _NetRetry
_NetRetry       dw      3               ;-000c network retry count
                global  _NetDelay
_NetDelay       dw      1               ;-000a network delay count
                global  _DskBuffer
_DskBuffer      dd      -1              ;-0008 current dos disk buffer
                global  _inputptr
_inputptr       dw      0               ;-0004 Unread con input
                global  _first_mcb
_first_mcb      dw      0               ;-0002 Start of user memory
                global  _DPBp
                global  MARK0026H
; A reference seems to indicate that this should start at offset 26h.
*/
  uint8_t      dos_data;        /* 0x00  abs / -0x26 rel: SDA format byte (0=DOS3.x, 1=DOS4+) */
  uint16_t     kernel_start_off;/* 0x01  abs / -0x25 rel: offset of kernel_start */
  uint8_t      _pad0;           /* 0x03  abs / -0x23 rel: padding */
  uint16_t     version_style;   /* 0x04  abs / -0x22 rel: 1 = MS-DOS 4.0+ style */
  uint8_t      _pad1[8];        /* 0x06  abs / -0x20 rel: padding to NetBios */
  uint16_t     NetBios;         /* 0x0E  abs / -0x18 rel: NetBios Number */
  uint8_t      _pad2[10];       /* 0x10  abs / -0x16 rel: padding to NetRetry */
  uint16_t     NetRetry;        /* 0x1A  abs / -0x0C rel: network retry count */
  uint16_t     NetDelay;        /* 0x1C  abs / -0x0A rel: network delay count */
  dos_far_ptr  DskBuffer;       /* 0x1E  abs / -0x08 rel: current dos disk buffer */
  dos_short_ptr inputptr;       /* 0x22  abs / -0x04 rel: unread CON input */
  uint16_t     first_mcb;       /* 0x24  abs / -0x02 rel: start of user memory */
/* === MARK0026H, offset 0x26 === */
  /* 0x26 abs /  0x00 rel: First Drive Parameter Block */
  /*struct dpb*/ dos_far_ptr DPBp;      /*  0 First drive Parameter Block          */
  /*struct sfttbl*/ dos_far_ptr sfthead;/*  4 System File Table head               */
  /*struct dhdr*/ dos_far_ptr clock;    /*  8 CLOCK$ device                        */
  /*struct dhdr*/ dos_far_ptr syscon;   /*  c console device                       */
  unsigned short maxsecsize;            /* 10 max bytes per sector for any blkdev  */
  dos_far_ptr inforecptr;               /* 12 pointer to disk buffer info record   */
  /*struct cds*/ dos_far_ptr CDSp;      /* 16 Current Directory Structure          */
  /*struct sfttbl*/ dos_far_ptr FCBp;   /* 1a FCB table pointer                    */
  unsigned short nprotfcb;     /* 1e number of protected fcbs             */
  unsigned char nblkdev;       /* 20 number of block devices              */
  unsigned char lastdrive;     /* 21 value of last drive                  */
  struct dhdr nul_dev;         /* 22 NUL device driver header(no pointer!)*/
  unsigned char njoined;       /* 34 number of joined devices             */
  unsigned short specialptr;   /* 35 pointer to list of spec. prog(unused)*/
  dos_far_ptr setverPtr;       /* 37 pointer to SETVER list               */
  u16 a20pt;                   /* 3b near pointer to fix A20 ctrl         */ /// TODO: may be some impl. is required
  unsigned short recentpsp;    /* 3d PSP of most recently exec'ed prog    */
  unsigned short nbuffers;     /* 3f Number of buffers                    */
  unsigned short nlookahead;   /* 41 Number of lookahead buffers          */
  unsigned char BootDrive;     /* 43 bootdrive (1=A:)                     */
  unsigned char cpu;           /* 44 CPU family [was unused dword moves]  */
  unsigned short xmssize;      /* 45 extended memory size in KB           */ 
  /*struct buffer*/ dos_far_ptr firstbuf; /* 47 head of buffers linked list          */
  unsigned short dirtybuf;     /* 4b number of dirty buffers              */
  /*struct buffer*/ dos_far_ptr lookahead;/* 4d pointer to lookahead buffer          */
  unsigned short slookahead;   /* 51 number of lookahead sectors          */
  unsigned char bufloc;        /* 53 BUFFERS loc (1=HMA)                  */
  dos_far_ptr deblock_buf;     /* 54 pointer to workspace buffer          */
  char filler2[5];             /* 58 ???/unused                           */
  unsigned char int24fail;     /* 5d int24 fail while making i/o stat call*/
  unsigned char memstrat;      /* 5e memory allocation strat during exec  */
  unsigned char a20count;      /* 5f nr. of int21 calls for which a20 off */
  unsigned char VgaSet;        /* 60 bitflags switches=/w, int21/4b05     */
  unsigned short unpack;       /* 61 offset of unpack code start          */
  unsigned char uppermem_link; /* 63 UMB Link flag                        */
  unsigned short min_pars;     /* 64 minimum para req by program execed   */
  unsigned short uppermem_root;/* 66 Start of umb chain (usually 9fff)    */
  unsigned short last_para;    /* 68 para: start scanning during memalloc */
  /* ANY ITEM BELOW THIS POINT MAY CHANGE */
  /* FreeDOS specific entries */
  unsigned char os_setver_minor;/*6a settable minor DOS version           */
  unsigned char os_setver_major;/*6b settable major DOS version           */
  unsigned char os_minor;      /* 6c minor DOS version                    */
  unsigned char os_major;      /* 6d major DOS version                    */
  unsigned char rev_number;    /* 6e DOS revision#, only 3 bits           */
  unsigned char version_flags; /* 6f DOS version flags                    */
  dos_short_ptr os_release;    /* 70 near pointer to os_release string    */
  unsigned short winInstanced; /* WinInit called                          */
  unsigned long  winStartupInfo[4];
  unsigned short instanceTable[5];
  char os_release_str[12];
  char aux_str[4];
  char con_str[4];
  char prn_str[4];
  /*
      The original kernel ships a built-in, 5-entry SFT block
      ("_firstsftt" in kernel.asm) baked into the kernel's static data
      at a fixed offset (0xCC from the start of this structure), with
      sftt_next already set to the end-of-list terminator (-1) and
     sftt_count already set to 5 at link time - i.e. it never needs
      runtime initialization in the original, because the assembler
      wrote the terminator/count bytes directly into the kernel image.

      This codebase has no equivalent assembled-in data segment, so we
      give it an explicit field here instead of pointing LoL->sfthead
      at a magic offset computed by hand. The 0xCC offset itself is
      kept (rather than just appending the field wherever it lands),
      since some original code/tooling may still expect to find the
      built-in SFT block there; _pad_to_firstsftt below pads out to
      that exact offset, and the _Static_assert further down makes
      sure no future edit silently shifts it. It still needs to be
      filled in explicitly at runtime (see PreConfig()) - a plain
      memset(0) would leave sftt_next == 0 instead of the -1
      terminator, which idx_to_sft_()/get_free_sft() would
      misinterpret as "more SFT blocks follow, starting at guest
      address 0000:0000".
  */
  sftheader firstsftt;
  sft sft_table[5];
  /// adjust to sizeof(struct lol) to 1FBh, just to show reusable bytes
  BYTE _free_pad[2];
};
/*
 * INT 21h/AH=52h exposes this binary layout to TSRs and command
 * interpreters.  FreeCOM LOADHIGH reads first_mcb through ES:[BX-2]
 * and accesses several later fields directly.
 */
_Static_assert(offsetof(struct lol, first_mcb)     == 0x24, "LoL ABI: first_mcb must be at MARK0026H-2");
_Static_assert(offsetof(struct lol, DPBp)          == 0x26, "LoL ABI: DPBp/MARK0026H must be at 0x26");
_Static_assert(offsetof(struct lol, a20pt)         == 0x61, "LoL ABI: a20ptr must be at MARK0026H+0x3B");
_Static_assert(offsetof(struct lol, uppermem_link) == 0x89, "LoL ABI: uppermem_link must be at MARK0026H+0x63");
_Static_assert(offsetof(struct lol, uppermem_root) == 0x8C, "LoL ABI: uppermem_root must be at MARK0026H+0x66");
_Static_assert(offsetof(struct lol, firstsftt)     == 0xCC, "firstsftt start offset looks incorrect, firstsftt should be on +0xCC");
_Static_assert(sizeof(sft) == 59, "sft size must match original 59-byte SFT entry");
_Static_assert(sizeof(sftheader) == 6, "sftheader must be 6 bytes");
_Static_assert(offsetof(struct lol, sft_table) == 0xCC + sizeof(sftheader), "built-in SFT table must follow firstsftt header");
_Static_assert(sizeof(struct lol) <= 0x01FB, "LoL overlaps internal_data");

#define STACK_SIZE (384/2) // stack allocated in words

/*
; Some references seem to indicate that this data should start at 01fbh in
; order to maintain 100% MS-DOS compatibility.
*/
// MARK01FBH
struct dos_data {
    BYTE local_buffer[LINEBUFSIZECON]; // local_buffer is 256 bytes long, so it overflows into kb_buf!!
    // only when kb_buf is used, local_buffer is limited to 128 bytes.
    keyboard kb_buf;
/*
;
; Variables that follow are documented as part of the DOS 4.0-6.X swappable
; data area in Ralf Browns Interrupt List #56
;
; this byte is used for ^P support
*/
    BYTE PrinterEcho;          // -34 0 = no printer echo, ~0 echo
    BYTE verify_ena;           // -33 ~0, write with verify
    BYTE scr_pos;              // -32 Current Cursor Column
    char switchar;             // -31 switch char
    BYTE mem_access_mode;      /* -30 memory allocation strategy */
    BYTE sharing_flag;         /* -29 00 = sharing module not loaded... */
    BYTE net_set_count;        /* -28 count the name below was set */
    char net_name[16];         /* -27..-12 */
    UWORD CritPatch[5];        /* -11..-2 */
    BYTE CritPatchPad;         /* -1, 90h */
// _internal_data:              ; <-- Address returned by INT21/5D06
    BYTE ErrorMode;          /* 00 Critical Error Flag*/
    BYTE InDOS;              /* 01 Indos Flag */
    BYTE CritErrDrive;       /* 02 Drive on write protect error */
    BYTE CritErrLocus;       /* 03 Error Locus */
    UWORD CritErrCode;       /* 04 DOS format error Code */
    BYTE CritErrAction;      /* 06 Error Action Code */
    BYTE CritErrClass;       /* 07 Error Class */
    dos_far_ptr CritErrDev;  /* 08 Failing Device Address */
    dos_far_ptr dta;         /* 0C current DTA */
    UWORD cu_psp;            /* 10 Current PSP */
    UWORD break_sp;          /* 12 used in int 23 */
    UWORD return_code;       /* 14 return code from process */
    BYTE default_drive;      /* 16 Current Drive */
    BYTE break_ena;          /* 17 Break Flag (default TRUE) */
    BYTE flag18;             // 18 flag, code page switching
    BYTE flag19;             // 19 flag, copy of 18 on int 24h abort
// _swap_always:
    UWORD Int21AX;           // 1A - AX from last Int 21
    UWORD owning_psp;        // 1C - owning PSP
    UWORD MachineId;         // 1E - remote machine ID
    UWORD first_mcb;         // 20 - First usable MCB
    UWORD best_mcb;          // 22 - Best usable MCB
    UWORD last_mcb;          // 24 - Last usable MCB
    UWORD mem_size_para;     // 26 - memory size in paragraphs
    UWORD unk28;             // 28 - unknown
    BYTE  unk2A;             // 2A - unknown
    BYTE  unk2B;             // 2B - unknown
    BYTE  unk2C;             // 2C - unknown
    BYTE  break_flg;         // 2D - Program aborted by ^C
    BYTE  unk2E;             // 2E - unknown
    BYTE  unk2F;             // 2F - not referenced

    BYTE  DayOfMonth;        // 30 - day of month
    BYTE  Month;             // 31 - month
    UWORD YearsSince1980;    // 32 - year since 1980
    UWORD daysSince1980;     // 34 - number of days since epoch
    BYTE  DayOfWeek;         // 36 - day of week
    BYTE  console_swap;      // 37 - console swapped during read from dev
    BYTE  dosidle_flag;      // 38 - safe to call int28 if nonzero
    BYTE  abort_progress;    // 39 - abort in progress

    request ClkReqHdr;       // 3A - Device driver request header
    dos_far_ptr clk_driver;  // 58 - pointer to driver entry

    BYTE  MediaReqHdr[22];   // 5C - Device driver request header
    BYTE  IoReqHdr[30];      // 72 - Device driver request header
    BYTE  unk90[6];          // 90 - unknown
    struct ClockRecord
          ClkRecord;         // 96 - CLOCK$ transfer record
    UWORD unk9C;             // 9C - unknown

    BYTE  PriPathBuffer[0x80]; // 9E - buffer for file name
    BYTE  SecPathBuffer[0x80]; // 11E - buffer for file name

    BYTE  sda_tmp_dm[21];    // 19E - 21 byte search state
    BYTE  SearchDir[32];     // 1B3 - 32 byte dir entry
    BYTE  TempCDS[88];       // 1D3 - Temporary CDS buffer
    BYTE  DirEntBuffer[32];  // 22B - space enough for 1 dir entry

    UWORD wAttr;             // 24B - extended FCB file attribute
    BYTE  SAttr;             // 24D - Attribute Mask for Dir Search
    BYTE  OpenMode;          // 24E - File Open Attribute

    BYTE  pad24F[3];         // 24F
    BYTE  Server_Call;       // 252 - Server call Func 5D sub 0
    BYTE  pad253;            // 253
    BYTE  pad254[0x25C - 0x254];

    BYTE  term_type;         // 25C - termination type
    BYTE  pad25D;            // 25D
    UWORD term_psp;          // 25E
    dos_far_ptr int24_esbp;  // 260 - pointer to criticalerr DPB

    dos_far_ptr user_r;      // 264 - pointer to int21h stack frame
    UWORD critical_sp;       // 268 - critical error internal stack
    dos_far_ptr current_ddsc;// 26A - pointer to DPB

    UWORD diskbuf_seg;       // 26E - segment of disk buffer
    DWORD unk270;            // 270 - saving partial cluster number?
    UWORD unk274;            // 274
    UWORD unk276;            // 276 - temp word
    BYTE  media_id;          // 278 - media id returned by AH=1Bh,1Ch
    BYTE  unused279;         // 279

    dos_far_ptr current_device; // 27A - ptr to device header if filename is char device
    dos_far_ptr lpCurSft;       // 27E - Current SFT
    dos_far_ptr current_ldt;    // 282 - Current CDS
    dos_far_ptr sda_lpFcb;      // 286 - pointer to caller's FCB
    UWORD current_sft_idx;      // 28A - SFT index for next open
    UWORD temp_file_handler;    // 28C
    dos_far_ptr jft_entry;      // 28E - pointer to JFT entry

    UWORD sda_WFP_START;        // 292 - offset of first filename argument
    UWORD sda_REN_WFP;          // 294 - offset of second filename argument
    UWORD last_component;       // 296 - 0xffff or offset of last component

    BYTE  pad298[0x2AE - 0x298];

    DWORD current_filepos;      // 2AE - current offset in file

    BYTE  pad2B2[0x2CA - 0x2B2];

    UWORD save_BX;              // 2CA - Win3.x compatibility
    UWORD save_DS;              // 2CC
    UWORD save_unk;             // 2CE

    dos_far_ptr prev_user_r;    // 2D0 - pointer to previous int21 frame

    BYTE  pad2D4[0x2DD - 0x2D4];

    UWORD ext_open_action;      // 2DD - extended open action
    UWORD ext_open_attrib;      // 2DF - extended open attrib
    UWORD ext_open_mode;        // 2E1 - extended open mode
    dos_far_ptr open_filename;  // 2E3 - pointer to filename for AX=6C00h

    BYTE  pad2E7[0x300 - 0x2E7];

/* apistk_bottom: (300h)
 *
 * kernel.asm keeps three API stacks here.  _error_tos / _disk_api_tos /
 * _char_api_tos are the stack *TOP* labels (480h / 600h / 780h) - the storage
 * lies BELOW each of them, and apistk_top == _char_api_tos == 780h:
 *
 *   apistk_bottom:                              ; 300h
 *   _sda_tmp_dm_ren: times 21 db 0x90           ; 300h
 *   _SearchDir_ren:  times 32 db 0x90           ; 315h
 *                    times STACK_SIZE*2-($-apistk_bottom) db 0x90
 *   _error_tos:                                 ; 480h
 *                    times STACK_SIZE dw 0x9090
 *   _disk_api_tos:                              ; 600h
 *                    times STACK_SIZE dw 0x9090
 *   _char_api_tos:
 *   apistk_top:                                 ; 780h
 *
 * i.e. the whole stack area is STACK_SIZE*2*3 == 480h bytes, 300h..780h.
 * Declaring error_tos/disk_api_tos/char_api_tos as ARRAYS at those labels
 * added an extra STACK_SIZE*2 (180h) bytes and pushed device_lookahead and
 * everything behind it from 780h to 900h, so every SDA consumer (INT 21h
 * AX=5D06h -> device_lookahead, fat32_ext, absrdwrflg, high_words) read the
 * wrong fields.
 *
 * The port handles INT 21h natively and never switches stacks, so this is
 * pure layout padding - but the offsets behind it must stay correct.
 * The bottom of the error stack doubles as the rename scratch buffer,
 * exactly as in kernel.asm.
 */
    BYTE  sda_tmp_dm_ren[21];   // 300 - 21 byte search state for rename
    BYTE  SearchDir_ren[32];    // 315 - 32 byte dir entry for rename
    BYTE  error_stack[STACK_SIZE * 2 - 0x35];  // 335 - rest of the error stack
// _error_tos: ^ 480
    UWORD disk_stack[STACK_SIZE];    // 480 - Disk Function Stack
// _disk_api_tos: ^ 600
    UWORD char_stack[STACK_SIZE];    // 600 - Char Function Stack
// _char_api_tos / apistk_top: ^ 780
    BYTE  device_lookahead;    // 780 device driver look-ahead (printer), see ah=64h
    BYTE  VolChange;           // 781 volume change
    BYTE  VirtOpen;            // 782 virtual open flag

    BYTE  pad783[0x78C - 0x783];

    BYTE  fat32_ext[0x7BB - 0x78C];// 78C - FAT32 SDA extended
    BYTE  absrdwrflg;              // 7BB
    UWORD high_words[12];          // 7BC..7D3
    UWORD unk7D4[3];

    /* --- port-only appendix (beyond the documented real-DOS SDA) ---
       Guest-resident scratch for fcbfns.c. The original kernel
       redirects the DTA onto kernel-data dmatch instances: a DGROUP
       static for the FindFirst/FindNext state and stack locals inside
       FcbDelete()/FcbRename(). DGROUP is guest-visible memory on real
       DOS, and internal_data->dta is a guest far pointer in this port,
       so the scratch must live in guest memory - here, past the
       documented SDA fields (no guest software depends on offsets
       beyond 7D9h). Layout check: dos_data sits at DOS_PSP:0AEBh, the
       next fixed object is the NLS info block at DOS_PSP:13A4h; these
       three members add 43+43+16 bytes, well inside the slack.       */
    dmatch fcb_dmatch;             /* FcbFindFirstNext persistent state
                                      (original: static dmatch Dmatch) */
    dmatch fcb_dmatch_tmp;         /* FcbDelete/FcbRename transient
                                      (original: stack locals)         */
    char   fcb_ren_name[2 + FNAME_SIZE + 1 + FEXT_SIZE + 2];
                                   /* FcbRename 'A:NAME.EXT' rebuild
                                      buffer (original: stack local;
                                      needs a guest address here since
                                      truename() takes a guest src)    */
};

/* The SDA is what INT 21h AX=5D06h hands out; its base is &ErrorMode. Guest
   software (Win3.x, network redirectors, SHARE, ...) hardcodes these offsets. */
#define SDA_OFF(f) (offsetof(struct dos_data, f) - offsetof(struct dos_data, ErrorMode))
_Static_assert(SDA_OFF(sda_tmp_dm_ren)  == 0x300, "SDA ABI: sda_tmp_dm_ren must be at 300h");
_Static_assert(SDA_OFF(SearchDir_ren)   == 0x315, "SDA ABI: SearchDir_ren must be at 315h");
_Static_assert(SDA_OFF(disk_stack)      == 0x480, "SDA ABI: _error_tos must be at 480h");
_Static_assert(SDA_OFF(char_stack)      == 0x600, "SDA ABI: _disk_api_tos must be at 600h");
_Static_assert(SDA_OFF(device_lookahead)== 0x780, "SDA ABI: apistk_top/device_lookahead must be at 780h");
_Static_assert(SDA_OFF(fat32_ext)       == 0x78C, "SDA ABI: fat32_ext must be at 78Ch");
_Static_assert(SDA_OFF(absrdwrflg)      == 0x7BB, "SDA ABI: absrdwrflg must be at 7BBh");
_Static_assert(SDA_OFF(high_words)      == 0x7BC, "SDA ABI: high_words must be at 7BCh");
#pragma pack(pop)
