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

/*    ConvertPathNameToFCBName/set_fcbname - convert PriPathName's final
    component into FCB (8.3, space-padded) form, stashed in
    internal_data->DirEntBuffer (cast to a struct dirent - see lol.h:
    DirEntBuffer is a plain BYTE[32], same as the original's "extern
    ASM DirEntBuffer", and 32 == sizeof(struct dirent) is now enforced
    by a _Static_assert in fat.h).

    Migrated from dosfns.c verbatim.
*/
STATIC void ConvertPathNameToFCBName(char *FCBName, const char *PathName)
{
  ConvertNameSZToName83(FCBName, get_root(PathName));
  FCBName[FNAME_SIZE + FEXT_SIZE] = '\0';
}

STATIC void set_fcbname(void)
{
  ConvertPathNameToFCBName(((struct dirent *)internal_data->DirEntBuffer)->dir_name, PriPathName);
}

/*
    get_free_sft(sft_idx) - find the first unused SFT entry (one with
    sft_count == 0) across all SFT blocks reachable from
    LoL->sfthead, and return a pointer to it; *sft_idx receives its
    global index (suitable for storing into a process's
    ps_filetab[]).

    Migrated from dosfns.c. The MS-NET hook (extern current_sft_idx)
    is preserved since LoL->current_sft_idx already exists in this
    codebase (see lol.h); nothing else in this iteration reads it yet.
*/
STATIC sft *get_free_sft(COUNT *sft_idx)
{
  COUNT sys_idx = 0;
  dos_far_ptr x86_sp = LoL->sfthead;

  for (; !far_is_end(x86_sp); )
  {
    sfttbl *sp = (sfttbl *)ARM_PTR(x86_sp);
    REG COUNT i = sp->sftt_count;
    sft *sfti = sp->sftt_table;

    for (; --i >= 0; sys_idx++, sfti++)
    {
      if (sfti->sft_count == 0)
      {
        *sft_idx = sys_idx;

        /* MS NET uses this on open/creat TE */
        internal_data->current_sft_idx = sys_idx;

        return sfti;
      }
    }

    x86_sp = sp->sftt_next;
  }
  /* If not found, return an error                */
  return (sft *)-1;
}


/* initialize SFT fields (for open/creat) for character devices

    Migrated from dosfns.c. sftp->sft_dev is a dos_far_ptr here (see
    sft.h), so BinaryCharIO() (which takes a pointer to a dos_far_ptr,
    see fdos_21h.c) is called against a local copy of it rather than
    "&sftp->sft_dev" directly - same reasoning as ExecuteClockDriverRequest()
    passing &LoL->clock. The original passes MK_FP(0,0) as BinaryCharIO's
    data-buffer argument for C_OPEN (no data transferred); that becomes
    a plain NULL native pointer here.
*/
STATIC int DeviceOpenSft(struct dhdr *dhp, sft *sftp)
{
  int i;

  sftp->sft_shroff = -1;      /* /// Added for SHARE - Ron Cemer */
  sftp->sft_count += 1;
  sftp->sft_flags =
    (dhp->dh_attr & ~(SFT_MASK | SFT_FSHARED)) | SFT_FDEVICE | SFT_FEOF;
  memcpy(sftp->sft_name, dhp->dh_name, FNAME_SIZE);

  /* pad with spaces */
  for (i = FNAME_SIZE + FEXT_SIZE - 1; sftp->sft_name[i] == '\0'; i--)
    sftp->sft_name[i] = ' ';
  /* and uppercase */
  DosUpFMem(sftp->sft_name, FNAME_SIZE + FEXT_SIZE);

  sftp->sft_dev = x86_FAR_PTR(DOS_PSP, dhp);
  sftp->sft_date = dos_getdate();
  sftp->sft_time = dos_gettime();
  sftp->sft_attrib = D_DEVICE;

  if (dhp->dh_attr & SFT_FOCRM)
  {
    /* if Open/Close/RM bit in driver's attribute is set
     * then issue an Open request to the driver
     */
    dos_far_ptr dev = sftp->sft_dev;
    if (BinaryCharIO(&dev, 0, NULL, C_OPEN) != SUCCESS)
      return DE_ACCESS;
  }
  return SUCCESS;
}

/*
extended open codes
0000 0000 always fail
0000 0001 open O_OPEN
0000 0010 replace O_TRUNC

0001 0000 create new file O_CREAT
0001 0001 create if not exists, open if exists O_CREAT | O_OPEN
0001 0010 create O_CREAT | O_TRUNC

bits for flags (bits 11-8 are internal FreeDOS bits only)
15 O_FCB  called from FCB open
14 O_SYNC commit for each write (not implemented yet)
13 O_NOCRIT do not invoke int23 (not implemented yet)
12 O_LARGEFILE allow files >= 2gb but < 4gb (not implemented yet)
11 O_LEGACY not called from int21/ah=6c: find right fn for redirector
10 O_CREAT if file does not exist, create it
9 O_TRUNC if file exists, truncate and open it \ not both 
8 O_OPEN  if file exists, open it              /
7 O_NOINHERIT do not inherit handle on exec
6 \ 
5  - sharing modes
4 / 
3 reserved 
2 bits 2,1,0 = 100: RDONLY and do not modify file's last access time
                    (not implemented yet)
1 \ 0=O_RDONLY, 1=O_WRONLY,
0 / 2=O_RDWR, 3=O_EXECCASE (preserve case for redirector EXEC,
                            (not implemented yet))
*/

/*
    DosOpenSft(fname, flags, attrib) - the real implementation behind
    INT 21h AH=3Dh/3Ch/5Bh/6Ch (open/create/etc): resolve fname via
    truename(), then either hand off to DeviceOpenSft() (character
    devices), the network redirector, or dos_open() (everything else).

    Migrated from dosfns.c. Differences from the original:
      - fname is a dos_far_ptr (see truename()'s signature/comment
        above for why).
      - sftp/cu_psp/sfthead-walking all go through the dos_far_ptr-
        aware helpers already defined above (idx_to_sft()/get_free_sft()/
        internal_data->cu_psp) instead of dereferencing native "sft FAR
        *"/"extern seg cu_psp" directly.
      - the IS_NETWORK branch (REM_CREATE/REM_EXTOC/REM_OPEN via
        network_redirector_mx()) and the SHARE-installed branch
        (share_open_check()) are migrated as-is, but - per
        IsShareInstalled()/network_redirector_mx() always reporting
        "not present" (see their definitions above) - the SHARE branch
        always takes the "not installed" path, and cdsFlags can never
        have CDSNETWDRV set (no network drive can exist without a
        redirector to create one), so the IS_NETWORK branch can never
        be taken either. share_open_check()/share_close_file() are
        therefore not implemented (no call site can reach them); if
        that ever stops being true, the missing symbols will fail to
        link rather than silently misbehaving.
      - ext_open_mode/ext_open_attrib/ext_open_action are
        internal_data fields here (see lol.h), not "extern ASM"
        variables declared inline.
*/
long DosOpenSft(dos_far_ptr fname, unsigned flags, unsigned attrib)
{
  COUNT sft_idx;
  sft *sftp;
  struct dhdr *dhp;
  long result;

  result = truename(fname, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;

  set_fcbname();

  /* now get a free system file table entry       */
  if ((sftp = get_free_sft(&sft_idx)) == (sft *) - 1)
    return DE_TOOMANY;

  memset(sftp, 0, sizeof(sft));

  sftp->sft_psp = internal_data->cu_psp;
  sftp->sft_mode = flags & 0xf0ff;
  internal_data->OpenMode = (BYTE) flags;

  sftp->sft_shroff = -1;        /* /// Added for SHARE - Ron Cemer */
  sftp->sft_attrib = attrib = attrib | D_ARCHIVE;

  /* check for a (local) device */
  if ((result & IS_DEVICE) && !(result & IS_NETWORK) &&
      (dhp = IsDevice((const char *)ARM_PTR(fname))) != NULL)
  {
    int rc = DeviceOpenSft(dhp, sftp);
    /* check the status code returned by the
     * driver when we tried to open it
     */
    if (rc < SUCCESS)
      return rc;
    return sft_idx;
  }

  if (result & IS_NETWORK)
  {
    int status;
    unsigned cmd;
    if ((flags & (O_TRUNC | O_CREAT)) == O_CREAT)
      attrib |= 0x100;

    /// TODO: FP_SEG(LoL->sfthead) assumes sftp lives in the same
    /// segment as the first (built-in) SFT block - true right now
    /// since the second block (PreConfig2(), not implemented/called
    /// yet) doesn't exist, but would need fixing (recovering sftp's
    /// real segment some other way) once it does. Moot for now: this
    /// whole branch is unreachable (see the function-level comment).
    internal_data->lpCurSft = x86_FAR_PTR(FP_SEG(LoL->sfthead), sftp);
    cmd = REM_CREATE;
    if (!(flags & O_LEGACY))
    {
      internal_data->ext_open_mode = flags & 0x70ff;
      internal_data->ext_open_attrib = attrib & 0xff;
      internal_data->ext_open_action = ((flags & 0x0300) >> 8) | ((flags & O_CREAT) >> 6);
      cmd = REM_EXTOC;
    }
    else if (!(flags & O_CREAT))
    {
      cmd = REM_OPEN;
      attrib = (BYTE)flags;
    }
    status = (int)network_redirector_mx(cmd, sftp, (void *)(intptr_t)attrib);
    if (status >= SUCCESS)
    {
      if (sftp->sft_count == 0)
        sftp->sft_count++;
      return sft_idx | ((long)status << 16);
    }
    return status;
  }

  /* First test the flags to see if the user has passed a valid   */
  /* file mode...                                                 */
  if ((flags & O_ACCMODE) > 2)
    return DE_INVLDACC;

  /* NEVER EVER allow directories to be created */
  /* ... though FCBs are weird :) */
  if (!(flags & O_FCB) &&
      (attrib & ~(D_RDONLY | D_HIDDEN | D_SYSTEM | D_ARCHIVE | D_VOLID)))
    return DE_ACCESS;

/* /// Added for SHARE.  - Ron Cemer */
  if (IsShareInstalled(TRUE))
  {
    /// unreachable: IsShareInstalled() always returns FALSE in this
    /// codebase (see its definition above) - share_open_check() is
    /// not implemented, so this branch is left as a deliberate
    /// "not implemented" failure rather than silently doing nothing,
    /// in case that assumption ever stops holding.
    printf("PANIC: DosOpenSft reached the SHARE-installed branch unexpectedly\n");
    for (;;) ;
  }

/* /// End of additions for SHARE.  - Ron Cemer */

  sftp->sft_count++;
  sftp->sft_flags = PriPathName[0] - 'A';
  result = dos_open(PriPathName, flags, attrib, sft_idx);
  if (result < 0)
  {
/* /// Added for SHARE *** CURLY BRACES ADDED ALSO!!! ***.  - Ron Cemer */
    /* if we allocated a share slot above, but open failed, free slot */
    if (sftp->sft_shroff >= 0)  /* SHARE installed status can't change since check above */
    {
      /// unreachable alongside the IsShareInstalled() branch above.
      printf("PANIC: DosOpenSft reached share_close_file unexpectedly\n");
      for (;;) ;
    }
/* /// End of additions for SHARE.  - Ron Cemer */
    sftp->sft_count--;
    return result;
  }
  return sft_idx | ((long)result << 16);
}


/*
    idx_to_sft_(SftIndex) - walk the SFT block list (LoL->sfthead) and
    set DD->lpCurSft to the SFT entry at SftIndex, regardless of
    whether that entry is currently open (sft_count == 0 is valid here).

    Returns SftIndex unchanged on success, -1 if SftIndex is out of
    range. Migrated from dosfns.c (also called from INT 2Fh/AX=1216h
    in the original; that entry point is not implemented here yet).
*/
int idx_to_sft_(int SftIndex)
{
  sfttbl *sp;

  internal_data->lpCurSft = MK_FP(0xffff, 0xffff);
  if (SftIndex < 0)
    return -1;

  /* Get the SFT block that contains the SFT      */
  for (sp = (sfttbl *)ARM_PTR(LoL->sfthead); !far_is_end(LoL->sfthead);
       sp = (sfttbl *)ARM_PTR(LoL->sfthead))
  {
    if (SftIndex < sp->sftt_count)
    {
      /* finally, point to the right entry            */
      internal_data->lpCurSft = MK_FP(FP_SEG(LoL->sfthead),
                             FP_OFF(LoL->sfthead) + offsetof(sfttbl, sftt_table)
                               + SftIndex * sizeof(sft));
      return SftIndex;
    }
    SftIndex -= sp->sftt_count;
    LoL->sfthead = sp->sftt_next;
  }

  /* If not found, return an error                */
  return -1;
}


/*
    get_sft_idx(hndl) - translate a DOS file handle (as seen by the
    guest program, e.g. via AH=3Eh/3Fh/40h) into an SFT index, by
    looking it up in the current process's handle table
    (psp->ps_filetab[hndl]).

    Migrated from dosfns.c.
*/
int get_sft_idx(unsigned hndl)
{
  psp *p = (psp *)ARM_PTR(MK_FP(internal_data->cu_psp, 0));
  int idx;

  if (hndl >= p->ps_maxfiles)
    return DE_INVLDHNDL;

  idx = p->ps_filetab[hndl];
  return idx == 0xff ? DE_INVLDHNDL : idx;
}

/*
    get_sft(hndl) - translate a DOS file handle into a pointer to its
    SFT entry. Returns (sft *)-1 if hndl is not a currently open
    handle for the current process.

    Migrated from dosfns.c.
*/
sft *get_sft(UCOUNT hndl)
{
  /* Get the SFT block that contains the SFT      */
  return idx_to_sft(get_sft_idx(hndl));
}

/*
    IsShareInstalled(recheck) - report whether SHARE.EXE is loaded.

    /// TODO: stub for this iteration. The original calls share_check()
    /// (an INT 2Fh AX=1000h multiplex check, implemented in asm) and
    /// caches the result in share_installed; neither exists in this
    /// codebase, and there is no SHARE.EXE-equivalent driver to load
    /// here yet, so this honestly always reports "not installed" -
    /// which is the truth for this system right now, not a shortcut
    /// around missing functionality.

    Migrated from dosfns.c (signature only; body replaced as above).
*/
BOOL IsShareInstalled(BOOL recheck)
{
  UNREFERENCED_PARAMETER(recheck);
  return FALSE;
}
