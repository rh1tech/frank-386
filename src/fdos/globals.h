/****************************************************************/
/*                                                              */
/*                          globals.h                           */
/*                            DOS-C                             */
/*                                                              */
/*             Global data structures and declarations          */
/*                                                              */
/*                   Copyright (c) 1995, 1996                   */
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
extern struct lol* LoL;
extern struct dos_data* internal_data;
extern struct dhdr* blk_dev;    /* built-in block device driver header */
COUNT ASMCFUNC
    CriticalError(COUNT nFlag, COUNT nDrive, COUNT nError,
                           dos_far_ptr /* -> struct dhdr */ x86_lpDevice);

/*                                                                      */
/* Convience definitions of TRUE and FALSE                              */
/*                                                                      */
#ifndef TRUE
#define TRUE (1)
#endif
#ifndef FALSE
#define FALSE (0)
#endif

/*                                                                      */
/* Constants and macros                                                 */
/*                                                                      */
/* Defaults and limits - System wide                                    */
#define NAMEMAX         MAX_CDSPATH     /* Maximum path for CDS         */

/* internal error from failure or aborted operation                     */
#define ERROR           -1
#define OK              0

/* internal transfer direction flags                                    */
#define XFR_READ        1
#define XFR_WRITE       2
#define XFR_FORCE_WRITE 3
/* flag to update fcb_rndm field */
#define XFR_FCB_RANDOM  4

#define RDONLY          0
#define WRONLY          1
#define RDWR            2

/* special ascii code equates                                           */
#define SPCL            0x00
#define CTL_C           0x03
#define CTL_F           0x06
#define BELL            0x07
#define BS              0x08
#define HT              0x09
#define LF              0x0a
#define CR              0x0d
#define CTL_P           0x10
#define CTL_Q           0x11
#define CTL_S           0x13
#define CTL_Z           0x1a
#define ESC             0x1b
#define CTL_BS          0x7f

#define INS             0x5200
#define DEL             0x5300

#define F1              0x3b00
#define F2              0x3c00
#define F3              0x3d00
#define F4              0x3e00
#define F5              0x3f00
#define F6              0x4000
#define LEFT            0x4b00
#define RIGHT           0x4d00

/* Blockio constants                                                    */
#define DSKWRITE        1       /* dskxfr function parameters   */
#define DSKREAD         2
#define DSKWRITEINT26   3
#define DSKREADINT25    4

/* PriPathName/SecPathBuffer are SDA fields in the original (extern
   ASM _PriPathBuffer._PriPathName, see globals.h), reserved here as
   internal_data->PriPathBuffer/SecPathBuffer (see lol.h). Named with
   the original's name (not a trailing-D macro like IoReqHdrD) since,
   unlike IoReqHdr/sda_tmp_dm, there is no internal_data field of the
   same name to collide with. */
#define PriPathName ((char *)internal_data->PriPathBuffer)
#define SecPathName ((char *)internal_data->SecPathBuffer)

extern /*struct buffer*/dos_far_ptr x86_firstAvailableBuf;
extern /*UBYTE DiskTransferBuffer[MAX_SEC_SIZE]*/ const dos_far_ptr DiskTransferBuffer; // BSS

/* near fnodes:
 * fnode[0] is used internally for almost all cases.
 * fnode[1] is only used for:
 * 1) rename (target)
 * 2) rmdir (checks if the directory to remove is empty)
 * 3) commit (copies, than closes fnode[0])
 * 3) merge_file_changes (for SHARE)
 */
GLOBAL struct f_node fnode[2];
GLOBAL BYTE ASM ReturnAnyDosVersionExpected;

UWORD fgetword(const void *vp);
void fputword(void *vp, UWORD w);
ULONG fgetlong(const void *vp);
void fputlong(void *vp, ULONG l);
