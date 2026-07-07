#include "bios/bios.h"
#include "hdrs.h"

/* DOS calls this to see if it's okay to open the file.
    Returns a file_table entry number to use (>= 0) if okay
    to open.  Otherwise returns < 0 and may generate a critical
    error.  If < 0 is returned, it is the negated error return
    code, so DOS simply negates this value and returns it in
    AX. */
int share_open_check(dos_far_ptr filename,  /* pointer to fully qualified filename */
                     unsigned short pspseg, /* psp segment address of owner process */
                     int openmode,          /* 0=read-only, 1=write-only, 2=read-write */
                     int sharemode) {       /* SHARE_COMPAT, etc... */
    CPU_regs saved;
    cpu_save_regs(cpu, &saved);

    SET_DS(FP_SEG(filename));
    CPU_SI = FP_OFF(filename);
    CPU_BX = pspseg;
    CPU_CX = openmode;
    CPU_DX = sharemode;

    CPU_AX = 0x10A0;
    bios_intcall(cpu, 0x2F, "SHARE OPEN 2F");

    int res = (int16_t)CPU_AX;
    cpu_restore_regs(cpu, &saved);
    return res;
}

/* DOS calls this to record the fact that it has successfully
    closed a file, or the fact that the open for this file failed. */
void share_close_file(int fileno) {  /* file_table entry number */
    CPU_regs saved;
    cpu_save_regs(cpu, &saved);
    CPU_BX = fileno;
    CPU_AX = 0x10A1;
    bios_intcall(cpu, 0x2F, "SHARE CLOSE 2F");
    cpu_restore_regs(cpu, &saved);
}


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
STATIC int DeviceOpenSft(dos_far_ptr /*struct dhdr*/ x86_dhp, sft *sftp)
{
  int i;
  struct dhdr* dhp = (struct dhdr*)ARM_PTR(x86_dhp);
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

  sftp->sft_dev = x86_dhp;
  sftp->sft_date = dos_getdate();
  sftp->sft_time = dos_gettime();
  sftp->sft_attrib = D_DEVICE;

  if (dhp->dh_attr & SFT_FOCRM)
  {
    /* if Open/Close/RM bit in driver's attribute is set
     * then issue an Open request to the driver
     */
    dos_far_ptr dev = sftp->sft_dev;
    if (BinaryCharIO(&dev, 0, MK_FP(0,0), C_OPEN) != SUCCESS)
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
  dos_far_ptr dhp;
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
  if ((result & IS_DEVICE) && !(result & IS_NETWORK)) {
      dhp = IsDevice((const char *)ARM_PTR(fname));
      if (EFFECTIVE(dhp) != 0) {
        int rc = DeviceOpenSft(dhp, sftp);
        /* check the status code returned by the
        * driver when we tried to open it
        */
        if (rc < SUCCESS)
          return rc;
        return sft_idx;
      }
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
    /// TODO:
///    status = (int)network_redirector_mx(cmd, sftp, (void *)(intptr_t)attrib);
///    if (status >= SUCCESS)
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
  dos_far_ptr x86_sp;

  internal_data->lpCurSft = MK_FP(0xffff, 0xffff);
  if (SftIndex < 0)
    return -1;

  /* Get the SFT block that contains the SFT      */
  for (x86_sp = LoL->sfthead; !far_is_end(x86_sp); x86_sp = sp->sftt_next)
  {
    sp = (sfttbl *)ARM_PTR(x86_sp);
    if (SftIndex < sp->sftt_count)
    {
      /* finally, point to the right entry            */
      internal_data->lpCurSft = linear_to_far(&sp->sftt_table[SftIndex]);
      return SftIndex;
    }
    SftIndex -= sp->sftt_count;
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

  idx = ((UBYTE *) ARM_PTR(p->ps_filetab))[hndl];
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


/*
    SftSeek(sft_idx, new_pos, mode) / DosSeek(hndl, new_pos, mode, rc) -
    the real implementation behind INT 21h AH=42h (LSEEK).

    Migrated from upstream FreeDOS dosfns.c. Differences from the
    original:
      - idx_to_sft()/lpCurSft follow the same dos_far_ptr-aware pattern
        as the rest of this file (internal_data->lpCurSft instead of a
        bare pointer).
      - the SFT_FSHARED (network redirector) branch of SEEK_END is
        dropped: it can only be reached through a network drive, and -
        per DosOpenSft()'s IS_NETWORK branch above - no network drive
        can ever exist in this codebase (no redirector to create one).
        remote_lseek() is therefore not implemented; if that
        assumption ever stops holding, this needs revisiting alongside
        DosOpenSft()'s IS_NETWORK branch.
*/
STATIC COUNT SftSeek2(int sft_idx, LONG new_pos, unsigned mode, UDWORD *p_result)
{
  sft *s = idx_to_sft(sft_idx);

  if (s == (sft *) - 1)
    return DE_INVLDHNDL;

  /* Test for invalid mode                        */
  if (mode > SEEK_END)
    return DE_INVLDFUNC;

  internal_data->lpCurSft = x86_FAR_PTR(FP_SEG(LoL->sfthead), s);

  /* Do special return for character devices      */
  if (s->sft_flags & SFT_FDEVICE)
  {
    new_pos = 0;
  }
  else if (mode == SEEK_CUR)
  {
    new_pos += s->sft_posit;
  }
  else if (mode == SEEK_END)      /* seek from end of file */
  {
    new_pos += s->sft_size;
  }

  s->sft_posit = new_pos;

  *p_result = (UDWORD)new_pos;
  return SUCCESS;
}

COUNT SftSeek(int sft_idx, LONG new_pos, unsigned mode)
{
  UDWORD result;
  return SftSeek2(sft_idx, new_pos, mode, &result);
}

ULONG DosSeek(unsigned hndl, LONG new_pos, COUNT mode, int *rc)
{
  int sft_idx = get_sft_idx(hndl);
  UDWORD result;

  *rc = SftSeek2(sft_idx, new_pos, mode, &result);
  if (*rc == SUCCESS)
    return result;
  return (ULONG)*rc;
}

/*
    DosRWSft(sft_idx, n, bp, mode) - the real implementation behind
    INT 21h AH=3Fh/40h (read/write): dispatch to the network
    redirector, a character device, or rwblock() (regular files),
    depending on the SFT's flags.

    Migrated from dosfns.c. Differences from the original:
      - bp is a dos_far_ptr (it comes straight from the guest program
        via DS:DX, like rwblock()'s buffer above) - converted to a
        native pointer only where BinaryCharIO()/cooked_read()/
        cooked_write() (which all take native void* / char* - see their
        definitions above) need one; passed straight through
        (untranslated) to rwblock(), which itself expects a
        dos_far_ptr.
      - the SFT_FSHARED (network redirector) branch is migrated as-is
        but unreachable, same reasoning as DosCloseSft()'s SFT_FSHARED
        branch above - dta/lpCurSft/current_filepos below are
        internal_data fields here (see lol.h), not bare "extern ASM"
        variables.
      - the SHARE-installed branch (share_access_check()) is left as
        a deliberate panic, same reasoning as DosOpenSft()'s SHARE
        branch above.
*/
long DosRWSft(int sft_idx, size_t n, dos_far_ptr bp, int mode)
{
  /* Get the SFT block that contains the SFT      */
  sft *s = idx_to_sft(sft_idx);

  if (s == (sft *) - 1)
  {
    return DE_INVLDHNDL;
  }
  /* If for read and write-only or for write and read-only then exit */
  if((mode == XFR_READ && (s->sft_mode & O_WRONLY)) ||
     (mode == XFR_WRITE && (s->sft_mode & O_ACCMODE) == O_RDONLY))
  {
    return DE_ACCESS;
  }
  if (mode == XFR_FORCE_WRITE)
    mode = XFR_WRITE;
    
/*
 *   Do remote first or return error.
 *   must have been opened from remote.
 */
/* /// TODO:
  if (s->sft_flags & SFT_FSHARED)
  {
    /// unreachable: see the function-level comment above.
    long XferCount;
    dos_far_ptr save_dta;

    save_dta = internal_data->dta;
    internal_data->lpCurSft = x86_FAR_PTR(FP_SEG(LoL->sfthead), s);
    internal_data->current_filepos = s->sft_posit;     /* needed for MSCDEX * /
    internal_data->dta = bp;
    XferCount = remote_rw(mode == XFR_READ ? REM_READ : REM_WRITE, s, n);
    internal_data->dta = save_dta;
    return XferCount;
  }
*/
  /* Do a device transfer if device                   */
  if (s->sft_flags & SFT_FDEVICE)
  {
    dos_far_ptr dev = s->sft_dev;

    /* Now handle raw and cooked modes      */
    if (s->sft_flags & SFT_FBINARY)
    {
      long rc = BinaryCharIO(&dev, n, bp,
                             mode == XFR_READ ? C_INPUT : C_OUTPUT);
      if (mode == XFR_WRITE && rc > 0 && (s->sft_flags & SFT_FCONOUT))
      {
        size_t cnt = (size_t)rc;
        const char *p = (const char *)ARM_PTR(bp);
        while (cnt--)
          update_scr_pos(*p++, 1);
      }
      return rc;
    }

    /* cooked mode */
    if (mode==XFR_READ)
    {
      long rc;
      /* Test for eof and exit                */
      /* immediately if it is                 */
      if (!(s->sft_flags & SFT_FEOF))
        return 0;

      if (s->sft_flags & SFT_FCONIN)
        rc = read_line_handle(sft_idx, n, (char *)ARM_PTR(bp));
      else
        rc = cooked_read(&dev, n, (char *)ARM_PTR(bp));
      if (*(char *)ARM_PTR(bp) == CTL_Z)
        s->sft_flags &= ~SFT_FEOF;
      return rc;
    }
    else
    {
      /* reset EOF state (set to no EOF)      */
      s->sft_flags |= SFT_FEOF;

      /* if null just report full transfer    */
      if (s->sft_flags & SFT_FNUL)
        return n;
      return cooked_write(&dev, n, (char *)ARM_PTR(bp));
    }
  }

  /* a block transfer                           */
  /* /// Added for SHARE - Ron Cemer */
  if (IsShareInstalled(FALSE) && (s->sft_shroff >= 0))
  {
    /// unreachable: IsShareInstalled() always returns FALSE in this
    /// codebase. share_access_check() is not implemented, so this is
    /// left as a deliberate panic rather than silently doing nothing,
    /// in case that assumption ever stops holding.
    printf("PANIC: DosRWSft reached share_access_check unexpectedly\n");
    for (;;) ;
  }
  /* /// End of additions for SHARE - Ron Cemer */
  return rwblock(sft_idx, bp, n, mode);
}


/*
    get_root(fname) - return a pointer to the last path component
    (filename) in fname, i.e. whatever follows the last '/', '\\', or
    ':' - or fname itself if it contains none of those.

    Migrated from dosfns.c verbatim. fname/the return value are plain
    native char* here (see dos_open()'s "path" parameter for why), so
    the original's fstrlen()/FAR pointer arithmetic becomes ordinary
    strlen()/pointer arithmetic - no other change.
*/
const char *get_root(const char *fname)
{
  /* find the end                                 */
  register unsigned length = strlen(fname);
  char c;

  /* now back up to first path seperator or start */
  fname += length;
  while (length)
  {
    length--;
    c = *--fname;
    if (c == '/' || c == '\\' || c == ':') {
      fname++;
      break;
    }
  }
  return fname;
}


/* check for a device
   returns device header if match, else returns NULL
   can only match character devices (as only they have names)

    Migrated from dosfns.c. The device chain (dh_next) is walked via
    dos_far_ptr/ARM_PTR()/far_is_end(), the same way the device table
    built earlier in this file (see update_dcb()) already is, instead
    of following a native "struct dhdr FAR *" chain directly - dh_next
    is a dos_far_ptr in this codebase (see device.h), not a directly
    dereferenceable pointer like the original's "struct dhdr FAR *".
*/
dos_far_ptr /*struct dhdr*/ IsDevice(const char *fname)
{
  dos_far_ptr x86_dhp;
  const char *froot = get_root(fname);
  int i;

/* /// BUG!!! This is absolutely wrong.  A filename of "NUL.LST" must be
       treated EXACTLY the same as a filename of "NUL".  The existence or
       content of the extension is irrelevent in determining whether a
       filename refers to a device.
       - Ron Cemer
  // if we have an extension, can't be a device <--- WRONG.
  if (*froot != '.')
  {
*/

/*  BUGFIX: MSCD000<00> should be handled like MSCD000<20> TE 
    ie the 8 character device name may be padded with spaces ' ' or NULs '\0'

    Note: fname is assumed an ASCIIZ string (ie not padded, unknown length)
    but the name in the device header is assumed FNAME_SIZE and padded.  KJD
*/


  /* check for names that will never be devices to avoid checking all device headers.
     only the file name (not path nor extension) need be checked, "" == root or empty name
   */
  if ( (*froot == '\0') ||
       ((*froot=='.') && ((*(froot+1)=='\0') || (*(froot+2)=='\0' && *(froot+1)=='.')))
     )
  {
    return MK_FP(0, 0);
  }

  /* cycle through all device headers checking for match */
  struct dhdr* dhp;
  for (x86_dhp = x86_FAR_PTR(DOS_PSP, &LoL->nul_dev); !far_is_end(x86_dhp); x86_dhp = dhp->dh_next)
  {
    dhp = (struct dhdr*)ARM_PTR(x86_dhp);

    if (!(dhp->dh_attr & ATTR_CHAR))  /* if this is block device, skip */
      continue;

    for (i = 0; i < FNAME_SIZE; i++)
    {
      unsigned char c1 = (unsigned char)froot[i];
      /* ignore extensions and handle filenames shorter than FNAME_SIZE */
      if (c1 == '.' || c1 == '\0')
      {
        /* check if remainder of device name consists of spaces or nulls */
        for (; i < FNAME_SIZE; i++)
        {
          unsigned char c2 = dhp->dh_name[i];
          if (c2 != ' ' && c2 != '\0')
            break;
        }
        break;
      }
      if (DosUpFChar(c1) != DosUpFChar(dhp->dh_name[i]))
        break;
    }

    /* if found a match then return device header */
    if (i == FNAME_SIZE)
      return x86_dhp;
  }

  return MK_FP(0, 0);
}

/*
    get_free_hndl() - find a free slot in the current process's file
    handle table (psp->ps_filetab), i.e. the lowest DOS file handle
    number not currently in use.

    Migrated from dosfns.c. p/q/r are native pointers here (fmemchr()
    becomes plain memchr()) - see get_sft_idx() above for the same
    "psp through internal_data->cu_psp" pattern.
*/
STATIC long get_free_hndl(void)
{
  psp *p = (psp *)ARM_PTR(MK_FP(internal_data->cu_psp, 0));
  UBYTE *q = (UBYTE *) ARM_PTR(p->ps_filetab);
  UBYTE *r = (UBYTE *)memchr(q, 0xff, p->ps_maxfiles);
  if (r == NULL) return DE_TOOMANY;
  return (unsigned)(r - q);
}

/*
    DosOpen(fname, mode, attrib) - allocate a DOS file handle for the
    current process and bind it to a newly DosOpenSft()'d SFT entry.

    Migrated from dosfns.c verbatim, aside from the native-pointer psp
    access noted in get_free_hndl() above.
*/
long DosOpen(dos_far_ptr fname, unsigned mode, unsigned attrib)
{
  long result;
  unsigned hndl;
  psp *p;

  /* test if mode is in range                     */
  if ((mode & ~O_VALIDMASK) != 0)
    return DE_INVLDACC;

  /* get a free handle  */
  if ((result = get_free_hndl()) < 0)
    return result;
  hndl = (unsigned)result;

  result = DosOpenSft(fname, mode, attrib);
  if (result < SUCCESS)
    return result;

  p = (psp *)ARM_PTR(MK_FP(internal_data->cu_psp, 0));
  ((UBYTE *) ARM_PTR(p->ps_filetab))[hndl] = (UBYTE)result;
  return hndl | (result & 0xffff0000l);
}

COUNT DosGetFattr(dos_far_ptr name)
{
  COUNT result;

  result = truename(name, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;
  
/* /// Added check for "d:\", which returns 0x10 (subdirectory) under DOS.
       - Ron Cemer */
           /* Theoretically: If the redirectory's qualify function
               doesn't return nonsense this check can be reduced to
               PriPathname[3] == 0, because local path names always
               have the three-byte string ?:\ and UNC path shouldn't
               validy consist of just two slashes.
               -- 2001/09/03 ska*/

  if (PriPathName[3] == '\0')
    return 0x10;

  set_fcbname();
/// TODO:
///  if (result & IS_NETWORK)
///    return network_redirector(REM_GETATTRZ);

  if (result & IS_DEVICE)
    return DE_FILENOTFND;

  return dos_getfattr(PriPathName);
}

/* This function is almost identical to DosGetFattr().
   Maybe it is nice to join both functions.
       -- 2001/09/03 ska*/
COUNT DosSetFattr(dos_far_ptr name, UWORD attrp)
{
  COUNT result;

  result = truename(name, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;

  set_fcbname();
/// TODO:
///  if (result & IS_NETWORK)
///    return remote_setfattr(attrp);

  if (result & IS_DEVICE)
    return DE_FILENOTFND;

  DebugPrintf(("DosSetFattr(%s)\n", name));
  if (IsShareInstalled(TRUE))
  {
    /* SHARE closes the file if it is opened in
     * compatibility mode, else generate a critical error.
     * Here generate a critical error by opening in "rw compat" mode */
    if ((result = share_open_check(linear_to_far(PriPathName), DOS_PSP, O_RDWR, 0)) < 0)
      return result;
    /* else dos_setfattr will close the file */
    share_close_file(result);
  }
  return dos_setfattr(PriPathName, attrp);
}

COUNT DosMkRmdir(const dos_far_ptr dir, int action)
{
  COUNT result;

  result = truename(dir, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;

  set_fcbname();
/// TODO:
///  if (result & IS_NETWORK)
///    return network_redirector(action == 0x39 ? REM_MKDIR : REM_RMDIR);

  if (result & IS_DEVICE)
    return DE_ACCESS;

  return (action == 0x39 ? dos_mkdir : dos_rmdir)(PriPathName);
}

COUNT DosRenameTrue(BYTE * path1, BYTE * path2, int attrib)
{
  if (path1[0] != path2[0])
  {
    return DE_DEVICE; /* not same device */
  }
  /// TODO:
///  if (FP_OFF(current_ldt) == 0xFFFF || (current_ldt->cdsFlags & CDSNETWDRV))
///    return network_redirector(REM_RENAME);

///  if (IsShareInstalled(TRUE) && share_is_file_open(path1))
///    return DE_ACCESS;

  return dos_rename(path1, path2, attrib);
}

COUNT DosRename(dos_far_ptr path1, dos_far_ptr path2)
{
  COUNT result;

  result = truename(path2, SecPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;

  if ((result & (IS_NETWORK | IS_DEVICE)) == IS_DEVICE)
    return DE_FILENOTFND;

  result = truename(path1, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;

  set_fcbname();

  if ((result & (IS_NETWORK | IS_DEVICE)) == IS_DEVICE)
    return DE_FILENOTFND;

  return DosRenameTrue(PriPathName, SecPathName, D_ALL);
}

COUNT DosTruename(dos_far_ptr src, dos_far_ptr dest)
{
  /*
   * RBIL/upstream FreeDOS semantics: on error, the caller's output
   * buffer must be left unchanged.  Build the canonical name in the
   * kernel scratch buffer first, then copy it to ES:DI only on success.
   *
   * AH=60h passes DS:SI -> source and ES:DI -> destination.
   * This port's truename() already accepts a guest far source pointer
   * and writes to a native kernel buffer.
   */
  COUNT rc = truename(src, PriPathName, CDS_MODE_ALLOW_WILDCARDS);
  if (rc >= SUCCESS) {
    strcpy((char *)ARM_PTR(dest), PriPathName);
    set_fcbname();
  }
  return rc;
}

#ifdef WITHFAT32
/* same convention as get_cds1(): drive is 0 for default, 1=A, 2=B, ... */
struct dpb FAR *GetDriveDPB(UBYTE drive, COUNT *rc)
{
  struct cds FAR *cdsp = get_cds1(drive);

  if (cdsp == NULL || far_is_null(cdsp->cdsDpb) || (cdsp->cdsFlags & CDSNETWDRV))
  {
    *rc = DE_INVLDDRV;
    return NULL;
  }

  *rc = SUCCESS;
  return (struct dpb FAR *)ARM_PTR(cdsp->cdsDpb);
}

#define IS_SLASH(ch) ((ch) == '\\' || (ch) == '/')
COUNT DosGetExtFree(BYTE FAR *DriveString, struct xfreespace FAR *xfsp)
{
  struct dpb FAR *dpbp;
  struct cds FAR *cdsp;
  dos_far_ptr cdsp_x86;

  memset(xfsp, 0, sizeof(struct xfreespace));
  xfsp->xfs_datasize = sizeof(struct xfreespace);

  cdsp = NULL;
  if (!*DriveString || (*DriveString == '.') || (IS_SLASH(DriveString[0]) && !IS_SLASH(DriveString[1])))
  {
    cdsp_x86 = get_cds(internal_data->default_drive);
    if (!far_is_null(cdsp_x86))
      cdsp = (struct cds FAR *)ARM_PTR(cdsp_x86);
  }
  else if (DriveString[1] == ':')
  {
    cdsp_x86 = get_cds(DosUpFChar(*DriveString) - 'A');
    if (!far_is_null(cdsp_x86))
      cdsp = (struct cds FAR *)ARM_PTR(cdsp_x86);
  }

  if (cdsp == NULL)
    return DE_INVLDDRV;

  if (cdsp->cdsFlags & CDSNETWDRV)
  {
    return DE_INVLDDRV;
    #if 0
    if (remote_getfree_11a3(cdsp, rg) != SUCCESS)
    {
      if (remote_getfree(cdsp, rg) != SUCCESS)
        return DE_INVLDDRV;

      xfsp->xfs_clussize = rg[0];
      xfsp->xfs_totalclusters = rg[1];
      xfsp->xfs_secsize = rg[2];
      xfsp->xfs_freeclusters = rg[3];
    }
    else
    {
      UDWORD total, avail;
      UDWORD bps, spc;

      bps = rg[4];
      spc = 1;
      total = (((UDWORD)rg[0] << 16UL) | rg[1]);
      avail = (((UDWORD)rg[2] << 16UL) | rg[3]);

      while (total > 0x00ffffffUL && spc < 128) {
        spc *= 2;
        avail /= 2;
        total /= 2;
      }
      while (total > 0x00ffffffUL && bps < 32768UL) {
        bps *= 2;
        avail /= 2;
        total /= 2;
      }

      xfsp->xfs_secsize = bps;
      xfsp->xfs_clussize = spc;
      xfsp->xfs_totalclusters = total;
      xfsp->xfs_freeclusters = avail;
    }
    #endif
  }
  else
  {
    if (far_is_null(cdsp->cdsDpb))
      return DE_INVLDDRV;
    dpbp = (struct dpb FAR *)ARM_PTR(cdsp->cdsDpb);
    if (media_check(dpbp) < 0)
      return DE_INVLDDRV;

    xfsp->xfs_secsize = dpbp->dpb_secsize;
    xfsp->xfs_totalclusters = (ISFAT32(dpbp) ? dpbp->dpb_xsize : dpbp->dpb_size) - 1;
/// TODO:    xfsp->xfs_freeclusters = dos_free(dpbp); replacemnt:
    xfsp->xfs_freeclusters = ISFAT32(dpbp)
                            ? dpbp->dpb_xnfreeclst
                            : (dpbp->dpb_nfreeclst == 0xFFFF
                               ? 0 : dpbp->dpb_nfreeclst);
    xfsp->xfs_clussize = dpbp->dpb_clsmask + 1;
  }

  xfsp->xfs_totalunits = xfsp->xfs_totalclusters;
  xfsp->xfs_freeunits = xfsp->xfs_freeclusters;
  xfsp->xfs_totalsectors = xfsp->xfs_totalclusters * xfsp->xfs_clussize;
  xfsp->xfs_freesectors = xfsp->xfs_freeclusters * xfsp->xfs_clussize;
  xfsp->xfs_datasize = sizeof(struct xfreespace);

  return SUCCESS;
}
#undef IS_SLASH
#endif

/* FIX (analysis patch): declared in proto.h, called by INT 21h AH=47h
   (GET CURRENT DIRECTORY - see fdos_21h.c), but never implemented in
   this port. drive: 0 = default drive, 1 = A:, 2 = B:, ...
   dst: destination buffer as a guest dos_far_ptr (ES:DI / DS:SI as
   passed by the caller - see MK_FP(CPU_DS, CPU_SI) at the call site). */
COUNT DosGetCuDir(UBYTE drive, dos_far_ptr dst)
{
  struct cds FAR *cdsp = get_cds1(drive);
  const BYTE *src;
  if (cdsp == NULL)
    return DE_INVLDDRV;
  src = cdsp->cdsCurrentPath + cdsp->cdsBackslashOffset + 1;
  strcpy((char *)ARM_PTR(dst), (const char *)src);
  return SUCCESS;
}

/* FIX (analysis patch): declared in proto.h, called by INT 21h AH=3Bh
   (CHDIR - see fdos_21h.c), but never implemented in this port.
   Migrated from upstream dosfns.c; network-redirector branch dropped in
   the same style already used above for DosMkRmdir/DosRenameTrue in
   this file (no network_redirector() in this port). */
COUNT DosChangeDir(dos_far_ptr s)
{
  COUNT result;

  result = truename(s, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return DE_PATHNOTFND;

  set_fcbname();

  if (EFFECTIVE(internal_data->current_ldt) &&
      (strlen(PriPathName) >= MAX_CDSPATH))
    return DE_PATHNOTFND;

  /// TODO:
  ///  if (result & IS_NETWORK)
  ///    return network_redirector(REM_CHDIR);

  result = dos_cd(PriPathName);
  if (result < SUCCESS)
    return result;

  /* Copy the path to the current directory structure. Some redirectors
     do not write back to the CDS - not applicable here (no redirector),
     kept for parity with upstream. */
  if (EFFECTIVE(internal_data->current_ldt))
  {
    struct cds *cdsp = (struct cds *)ARM_PTR(internal_data->current_ldt);
    fstrcpy(cdsp->cdsCurrentPath, PriPathName);
    if (PriPathName[7] == 0)
      cdsp->cdsCurrentPath[8] = 0; /* Need two Zeros at the end */
  }
  return SUCCESS;
}

/* FIX (analysis patch): declared in proto.h, called by INT 21h AH=41h
   (see fdos_21h.c), but never implemented in this port. Migrated from
   upstream dosfns.c; network/share branches dropped in the same style
   already used above for DosMkRmdir/DosRenameTrue in this file, since
   network_redirector()/share_is_file_open() are not ported (single-user,
   no SHARE.EXE / redirector support). */
COUNT DosDelete(dos_far_ptr path, int attrib)
{
  COUNT result;

  result = truename(path, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;

  set_fcbname();

  /// TODO:
  ///  if (result & IS_NETWORK)
  ///    return network_redirector(REM_DELETE);

  if (result & IS_DEVICE)
    return DE_FILENOTFND;

  ///  if (IsShareInstalled(TRUE) && share_is_file_open(PriPathName))
  ///    return DE_ACCESS;

  return dos_delete(PriPathName, attrib);
}
