/****************************************************************/
/*                                                              */
/*                          process.h                           */
/*                                                              */
/*            DOS exec data structures & declarations           */
/*                                                              */
/*                      November 23, 1991                       */
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


/*  Modes available as first argument to the spawnxx functions. */

#define P_WAIT    0             /* child runs separately, parent waits until exit */
#define P_NOWAIT  1             /* both concurrent -- not implemented */
#define P_OVERLAY 2             /* child replaces parent, parent no longer exists */

/*  Modes available as the first argument to DosExec() (task.c) - same
    values as INT 21h AH=4Bh's AL. */
#define EXEC_LOADNGO    0
#define EXEC_LOAD       1
#define EXEC_OVERLAY    3

#pragma pack(push, 1)
typedef struct {
  union {
    struct {
      UWORD load_seg;
      UWORD reloc;
    } _load;
    struct {
      UWORD env_seg;
      dos_far_ptr cmd_line;
      dos_far_ptr fcb_1;
      dos_far_ptr fcb_2;
      dos_far_ptr stack;
      dos_far_ptr start_addr;
    } _exec;
  } ldata;
} exec_blk;

#define exec    ldata._exec
#define load    ldata._load

typedef struct {
  UWORD ps_exit;                /* 00 CP/M-like exit point: int 20 */
  UWORD ps_size;                /* 02 segment of first byte beyond */
                                /*    memory allocated to program  */
  BYTE ps_fill1;                /* 04 single char fill=0           */

  /* CP/M-like entry point                                         */
  /* offsets 5-9 are a far call to absolute address 0:00C0h
     encoded using 1MB wrap form of address (e.g. 0F01D:FEF0h)
     for compatiblity with CP/M apps that do a near call to psp:5
     and expect size (KB) of allocated segment in word at offset 6 */
  UBYTE ps_farcall;             /* 05  far call opcode             */
  intvec ps_reentry;            /* 06  re-entry point              */

  intvec ps_isv22,              /* 0a terminate address            */
         ps_isv23,              /* 0e ctrl-break address           */
         ps_isv24;              /* 12 critical error address       */
  UWORD ps_parent;              /* 16 parent psp segment           */
  UBYTE ps_files[20];           /* 18 file table - 0xff is unused  */
  UWORD ps_environ;             /* 2c environment paragraph        */
  dos_far_ptr ps_stack;         /* 2e user stack pointer - int 21  */
  UWORD ps_maxfiles;            /* 32 maximum open files           */
  dos_far_ptr ps_filetab;       /* 34 open file table pointer      */
  dos_far_ptr ps_prevpsp;       /* 38 previous psp pointer         */
  UBYTE ps_dbcs_inputmode;      /* 3c unused,see int21/6301h/6302h */
  UBYTE ps_truename;            /* 3d unused,append truename flag int2f/B711h */
  UBYTE ps_netx_taskid[2];      /* 3e [Novell only field] task id  */
  UWORD ps_retdosver;           /* 40 version to return on int21/30h, defaults to true version */
  UWORD pdb_next;               /* 42 [Win only field] PSP chain   */
  UBYTE ps_fill2b[4];           /* 44 unused, 4 bytes              */
  UBYTE ps_olddos;              /* 48 [Win only field] DOS/Win program */
  UBYTE ps_fill2c[7];           /* 49 unused, 7 bytes              */
  UBYTE ps_unix[3];             /* 50 unix style call - 0xcd 0x21 0xcb */
  BYTE ps_fill3[9];             /* 53 */
  union {
    struct {
      fcb _ps_fcb1;             /* 5c first command line argument */
    } _u1;
    struct {
      BYTE fill4[16];
      fcb _ps_fcb2;             /* second command line argument */
    } _u2;
    struct {
      BYTE fill5[36];
      CommandTail _ps_cmd;
    } _u3;
  } _u;
} psp;
#pragma pack(pop)

#define ps_fcb1 _u._u1._ps_fcb1
#define ps_fcb2 _u._u2._ps_fcb2
#define ps_cmd  _u._u3._ps_cmd


/*
    The PSP is the single most ABI-visible structure in DOS: guest
    programs, TSRs and debuggers index it by documented offset.

    ps_files at +0x18 is load-bearing beyond mere layout: the default JFT
    lives there, and ps_filetab (+0x34) must point at it as the canonical
    far pair <psp_seg>:0018h. Programs test exactly that pair to decide
    whether the JFT has been relocated away from the PSP (SetJFTSize).
*/
_Static_assert(sizeof(psp) == 256, "psp must stay exactly 256 bytes (one paragraph-aligned PSP)");
_Static_assert(offsetof(psp, ps_files)    == 0x18, "default JFT must stay at PSP+18h");
_Static_assert(offsetof(psp, ps_maxfiles) == 0x32, "ps_maxfiles must stay at PSP+32h");
_Static_assert(offsetof(psp, ps_filetab)  == 0x34, "ps_filetab must stay at PSP+34h");
_Static_assert(offsetof(psp, ps_cmd)      == 0x80, "command tail must stay at PSP+80h");
_Static_assert(sizeof(exec_blk) == 22, "exec_blk must match the INT 21h AH=4Bh parameter block size");

/*
    The guest-visible INT 21h register frame.

    fdos_21h() builds this on the guest stack on every DOS call and publishes
    it at PSP:2Eh (ps_stack), which is also what internal_data->user_r points
    at - the port's equivalent of upstream's "user_r". It is the frame the
    original entry.asm creates with PUSH$ALL, so the field order is ABI, not
    an implementation detail: INT 24h handlers and anything walking ps_stack
    read it directly.

    Declared here rather than inside fdos_21h.c because task.c needs the
    caller's return CS:IP out of it when it installs a child's terminate
    vector (INT 22h).
*/
struct int21_guest_iregs {
  UWORD ax, bx, cx, dx;
  UWORD si, di, bp, ds, es;
  UWORD ip, cs, flags;
} __attribute__((packed));

_Static_assert(sizeof(struct int21_guest_iregs) == 24,
               "INT 21h guest iregs ABI must be 24 bytes");
