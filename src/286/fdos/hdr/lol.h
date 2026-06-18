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
  void (*a20ptr)(void);        /* 3b pointer to fix A20 ctrl           ???   */
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
#ifdef WIN31SUPPORT
  unsigned short winInstanced; /* WinInit called                          */
  unsigned long  winStartupInfo[4];
  unsigned short instanceTable[5];
#endif
  // TODO:
  char os_release_str[8];
};
