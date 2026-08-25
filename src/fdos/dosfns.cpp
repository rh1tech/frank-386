#define new fdos_new
extern "C" {
#include "bios/bios.h"
#include "hdrs.h"
}
#undef new
#ifdef load
#undef load
#endif
#include "guest_ref.hpp"
#include "path_guest.h"

using fdos_guest::cds_ref;
using fdos_guest::dhdr_ref;
using fdos_guest::dos_data_ref;
using fdos_guest::dmatch_ref;
using fdos_guest::dpb_ref;
using fdos_guest::lol_ref;
using fdos_guest::psp_ref;
using fdos_guest::sft_ref;
using fdos_guest::sfttbl_ref;
using fdos_guest::guest_bytes_ref;

static const lol_ref dosfns_lol(((uint32_t)DOS_PSP << 4) + 0x08F0u);
static const dos_data_ref dosfns_idata(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF);

static inline dos_far_ptr pri_path_far(void)
{
  return MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(dos_data, PriPathBuffer)));
}

static inline dos_far_ptr sec_path_far(void)
{
  return MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(dos_data, SecPathBuffer)));
}

static inline fdos_path_ref pri_path_ref(void) { return fdos_path_guest(pri_path_far()); }
static inline fdos_path_ref sec_path_ref(void) { return fdos_path_guest(sec_path_far()); }

static inline uint32_t sda_dmatch_linear(void)
{
  return ((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF + offsetof(dos_data, sda_tmp_dm);
}

static inline dos_far_ptr sda_dmatch_far(void)
{
  return MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(dos_data, sda_tmp_dm)));
}

static inline uint32_t sda_searchdir_linear(void)
{
  return ((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF + offsetof(dos_data, SearchDir);
}

static inline uint32_t dosfns_far_linear(dos_far_ptr p)
{
  return ((uint32_t)FP_SEG(p) << 4) + FP_OFF(p);
}

extern "C" {

#if DIAG
extern volatile unsigned int dos_diag_kernel_code;
#endif

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

/* SHARE_CHECK (int2f.asm): INT 2Fh AX=1000h, AL=FFh means installed. */
STATIC unsigned char share_check(void) {
    CPU_regs saved;
    unsigned char res;
    cpu_save_regs(cpu, &saved);
    CPU_AX = 0x1000;
    bios_intcall(cpu, 0x2F, "SHARE CHECK 2F");
    res = (unsigned char)CPU_AL;
    cpu_restore_regs(cpu, &saved);
    return res;
}

/* SHARE_ACCESS_CHECK / SHARE_LOCK_UNLOCK share one register frame in
   int2f.asm (share_common): BX=pspseg, CX=fileno, SI:DI=ofs (SI high),
   ES:DX=len (ES high), AX = base | allowcriter (resp. | unlock). */
STATIC int share_common(UWORD base, unsigned short pspseg, int fileno,
                        unsigned long ofs, unsigned long len, int flag) {
    CPU_regs saved;
    int res;
    cpu_save_regs(cpu, &saved);
    CPU_BX = pspseg;
    CPU_CX = (UWORD)fileno;
    CPU_SI = (UWORD)(ofs >> 16);
    CPU_DI = (UWORD)(ofs & 0xFFFF);
    SET_ES((UWORD)(len >> 16));
    CPU_DX = (UWORD)(len & 0xFFFF);
    CPU_AX = (UWORD)(base | (UWORD)flag);
    bios_intcall(cpu, 0x2F, "SHARE COMMON 2F");
    res = (int)(int16_t)CPU_AX;
    cpu_restore_regs(cpu, &saved);
    return res;
}

int share_access_check(unsigned short pspseg, int fileno, unsigned long ofs,
                       unsigned long len, int allowcriter) {
    return share_common(0x10A2, pspseg, fileno, ofs, len, allowcriter);
}

int share_lock_unlock(unsigned short pspseg, int fileno, unsigned long ofs,
                      unsigned long len, int unlock) {
    return share_common(0x10A4, pspseg, fileno, ofs, len, unlock);
}


/* Convert a path_ref's final component into FCB 8.3, space-padded
   form in the guest-resident SDA DirEntBuffer.  The destination is
   addressed explicitly in guest memory; no persistent SDA host pointer
   is created. */
STATIC void set_fcbname_ref(fdos_path_ref path)
{
  BYTE name[FNAME_SIZE + FEXT_SIZE + 1];
  size_t len = fdos_path_len(path, FDOS_PATHLEN);
  size_t root = 0;
  size_t i, out = 0;
  const UWORD off = (UWORD)(X86_INTERNAL_DATA_OFF +
                            offsetof(dos_data, DirEntBuffer) +
                            offsetof(struct dirent, dir_name));

  for (i = 0; i < len; ++i)
  {
    UBYTE c = fdos_path_get(path, i);
    if (c == '/' || c == '\\' || c == ':')
      root = i + 1u;
  }

  nf_memset(name, ' ', FNAME_SIZE + FEXT_SIZE);
  for (i = root; i < len && out < FNAME_SIZE + FEXT_SIZE; ++i)
  {
    UBYTE c = fdos_path_get(path, i);
    if (c == '.')
    {
      out = FNAME_SIZE;
      continue;
    }
    if (c == '/' || c == '\\' || c == 0)
      break;
    name[out++] = c;
  }
  name[FNAME_SIZE + FEXT_SIZE] = 0;
  guest_write(MK_FP(DOS_PSP, off), name, sizeof(name));
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
STATIC dos_far_ptr/*sft*/ get_free_sft(COUNT *sft_idx)
{
  COUNT sys_idx = 0;
  for (dos_far_ptr block = dosfns_lol.sfthead(); !far_is_end(block); )
  {
    const sfttbl_ref table(block);
    const UWORD count = table.count();
    for (UWORD i = 0; i < count; ++i, ++sys_idx)
    {
      const dos_far_ptr entry = table.entry(i);
      if (sft_ref(entry).count() == 0)
      {
        *sft_idx = sys_idx;
        dosfns_idata.current_sft_idx() = (UWORD)sys_idx;
        return entry;
      }
    }
    block = table.next();
  }
  return MK_FP((UWORD)-1, (UWORD)-1);
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
STATIC int DeviceOpenSft(dos_far_ptr x86_dhp, dos_far_ptr x86_sftp)
{
  BYTE name[FNAME_SIZE + FEXT_SIZE];
  const dhdr_ref devhdr(x86_dhp);
  const sft_ref sftp(x86_sftp);
  const UWORD dev_attr = devhdr.attr();
  int i;

  sftp.shroff(-1);
  sftp.count((UWORD)(sftp.count() + 1u));
  sftp.flags((UWORD)((dev_attr & ~(SFT_MASK | SFT_FSHARED)) | SFT_FDEVICE | SFT_FEOF));
  nf_memset(name, 0, sizeof(name));
  devhdr.read_name(name);
  for (i = FNAME_SIZE + FEXT_SIZE - 1; name[i] == '\0'; i--)
    name[i] = ' ';
  DosUpFMem(name, FNAME_SIZE + FEXT_SIZE);
  sftp.write_name(name);
  sftp.dcb(x86_dhp);
  sftp.date(dos_getdate());
  sftp.time(dos_gettime());
  sftp.attrib(D_DEVICE);

  if (dev_attr & SFT_FOCRM)
  {
    dos_far_ptr dev = x86_dhp;
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
  /* Keep the canonical path in the resident guest SDA.  A 128-byte native
     pathname snapshot both wastes the constrained core0 stack and breaks the
     paging boundary abstraction. */
  COUNT path_result = truename_guest(fname, pri_path_far(),
                                     CDS_MODE_CHECK_DEV_PATH);
  dpb_watch_check_chain("DosOpenSft 1");
  if (path_result < SUCCESS) {
    dpb_watch_check_chain("DosOpenSft 1err");
    return path_result;
  }
  const unsigned path_kind = (unsigned)path_result;

  set_fcbname_ref(pri_path_ref());
  dpb_watch_check_chain("DosOpenSft 2");

  /* now get a free system file table entry       */
  COUNT sft_idx;
  dos_far_ptr lpCurSft = get_free_sft(&sft_idx);
  dpb_watch_check_chain("DosOpenSft 3");
  if (far_is_end(lpCurSft))
    return DE_TOOMANY;
  {
    const sft_ref ref(lpCurSft);
    ref.clear();
    ref.psp(dosfns_idata.cu_psp());
    ref.mode((UWORD)(flags & 0xf0ffu));
    ref.shroff(-1);
    ref.attrib((UBYTE)(attrib | D_ARCHIVE));
    dosfns_idata.open_mode() = (UBYTE)flags;
  }
  attrib |= D_ARCHIVE;
  dpb_watch_check_chain("DosOpenSft 4");

  dpb_watch_check_chain("DosOpenSft 5");
  /* check for a (local) device */
  if ((path_kind & IS_DEVICE) && !(path_kind & IS_NETWORK)) {
      dos_far_ptr dhp = IsDeviceGuest(pri_path_far());
      dpb_watch_check_chain("DosOpenSft 6");
      if (EFFECTIVE(dhp) != 0) {
        int rc = DeviceOpenSft(dhp, lpCurSft);
        dpb_watch_check_chain("DosOpenSft 7");
        /* check the status code returned by the
        * driver when we tried to open it
        */
        if (rc < SUCCESS) {
          return rc;
        }
        return sft_idx;
      }
  }

  dpb_watch_check_chain("DosOpenSft 8");
  if (path_kind & IS_NETWORK)
  {
    return DE_PATHNOTFND;
    /// TODO:
  #if 0
    int status;
    unsigned cmd;
    if ((flags & (O_TRUNC | O_CREAT)) == O_CREAT)
      attrib |= 0x100;

    internal_data->lpCurSft = lpCurSft;
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
    #endif
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
    const WORD shroff = (WORD)share_open_check(
        pri_path_far(),
        (UWORD)dosfns_idata.cu_psp(), flags & 0x03, (flags >> 4) & 0x07);
    sft_ref(lpCurSft).shroff(shroff);
    if (shroff < 0)
      return shroff;
  }

/* /// End of additions for SHARE.  - Ron Cemer */

  {
    const sft_ref ref(lpCurSft);
    ref.count((UWORD)(ref.count() + 1u));
    ref.flags((UWORD)(UBYTE)(fdos_path_get(pri_path_ref(), 0) - 'A'));
  }
  long open_result = dos_open_ref(pri_path_ref(), flags, attrib, sft_idx);
  dpb_watch_check_chain("DosOpenSft 10");
  if (open_result < 0)
  {
/* /// Added for SHARE *** CURLY BRACES ADDED ALSO!!! ***.  - Ron Cemer */
    /* if we allocated a share slot above, but open failed, free slot */
    if (IsShareInstalled(TRUE))
    {
      const WORD shroff = sft_ref(lpCurSft).shroff();
      if (shroff >= 0)
      {
        share_close_file(shroff);
        sft_ref(lpCurSft).shroff(-1);
      }
    }
/* /// End of additions for SHARE.  - Ron Cemer */
    {
      const sft_ref ref(lpCurSft);
      const UWORD count = ref.count();
      if (count != 0)
        ref.count((UWORD)(count - 1u));
    }
    return open_result;
  }
  dpb_watch_check_chain("DosOpenSft 11");
  return sft_idx | (open_result << 16);
}

/*
    idx_to_sft_(SftIndex) - walk the SFT block list starting at
    LoL->sfthead and set internal_data->lpCurSft to the indexed entry,
    regardless of whether that entry is currently open
    (sft_count == 0 is valid here).

    The list head in LoL is persistent DOS state and must never be
    advanced while searching.  Only a local guest far pointer follows
    sftt_next.

    Returns the index relative to the containing SFT block on success,
    or -1 when the system file number is outside the complete chain.
    INT 2Fh/AX=1216h uses that relative index in BX.
*/
int idx_to_sft_(int SftIndex)
{
  int index = SftIndex;
  if (index < 0)
  {
    dosfns_idata.lp_cur_sft(MK_FP((UWORD)-1, (UWORD)-1));
    return -1;
  }
  for (dos_far_ptr block = dosfns_lol.sfthead(); !far_is_end(block); )
  {
    const sfttbl_ref table(block);
    const UWORD count = table.count();
    if (index < (int)count)
    {
      const dos_far_ptr entry = table.entry((UWORD)index);
      dosfns_idata.lp_cur_sft(entry);
      return index;
    }
    index -= count;
    block = table.next();
  }
  dosfns_idata.lp_cur_sft(MK_FP((UWORD)-1, (UWORD)-1));
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
  const psp_ref process(dosfns_idata.cu_psp());
  const UWORD count = process.max_files();
  const dos_far_ptr table = process.file_table();
  if (hndl >= count || far_is_null(table) || far_is_end(table))
    return DE_INVLDHNDL;
  const UBYTE idx = pload8(dosfns_far_linear(table) + hndl);
  return idx == 0xff ? DE_INVLDHNDL : idx;
}

/*
    get_sft(hndl) - translate a DOS file handle into a pointer to its
    SFT entry. Returns -1 if hndl is not a currently open
    handle for the current process.

    Migrated from dosfns.c.
*/
dos_far_ptr /*sft*/ get_sft(UCOUNT hndl) {
  /* Get the SFT block that contains the SFT      */
  return idx_to_sft(get_sft_idx(hndl));
}

BYTE share_installed = 0;

/*
    IsShareInstalled(recheck) - report whether SHARE.EXE is loaded.

    Migrated from dosfns.c. The kernel's own INT 2Fh handler answers AX=1000h
    with AL=00h ("not installed"), so this stays FALSE on a bare system - but
    a guest SHARE.EXE hooks INT 2Fh ahead of us, and then AL comes back FFh and
    the kernel starts routing opens/locks through it. All the hooks it needs
    (AX=10A0h/10A1h/10A2h/10A4h) are real INT 2Fh calls, see above.
*/
BOOL IsShareInstalled(BOOL recheck)
{
  if (recheck == FALSE)
    return share_installed;
  if (share_check() == 0xff)
    share_installed = TRUE;
  else
    share_installed = FALSE;
  return share_installed;
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
  dos_far_ptr _s = idx_to_sft(sft_idx);

  if (far_is_end(_s))
    return DE_INVLDHNDL;

  /* Test for invalid mode                        */
  if (mode > SEEK_END)
    return DE_INVLDFUNC;

  dosfns_idata.lp_cur_sft(_s);
  const sft_ref sft(_s);
  if (sft.flags() & SFT_FDEVICE)
    new_pos = 0;
  else if (mode == SEEK_CUR)
    new_pos += sft.position();
  else if (mode == SEEK_END)
    new_pos += sft.size();

  sft.position((ULONG)new_pos);

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
        guest far pointer through BinaryCharIO()/cooked_read()/
        cooked_write() (all paging-aware - see their
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
  const dos_far_ptr x86_sft = idx_to_sft(sft_idx);
  if (far_is_end(x86_sft))
  {
#if DIAG
    dos_diag_kernel_code = 0x44000000u | ((unsigned)sft_idx & 0xffffu);
#endif
    return DE_INVLDHNDL;
  }
  const sft_ref sft(x86_sft);
  const UWORD sft_mode = sft.mode();
  const UWORD entry_flags = sft.flags();
#if DIAG
  dos_diag_kernel_code = 0x45000000u | (((unsigned)sft_idx & 0xffu) << 16) | ((unsigned)sft_mode & 0xffffu);
#endif
  if ((mode == XFR_READ && (sft_mode & O_WRONLY)) ||
      (mode == XFR_WRITE && (sft_mode & O_ACCMODE) == O_RDONLY))
    return DE_ACCESS;
  if (mode == XFR_FORCE_WRITE)
    mode = XFR_WRITE;

  if (entry_flags & SFT_FDEVICE)
  {
    dos_far_ptr dev = sft.dev();
    if (entry_flags & SFT_FBINARY)
    {
      const long rc = BinaryCharIO(&dev, n, bp, mode == XFR_READ ? C_INPUT : C_OUTPUT);
      if (mode == XFR_WRITE && rc > 0 && (sft_ref(x86_sft).flags() & SFT_FCONOUT))
      {
        const uint32_t base = dosfns_far_linear(bp);
        for (size_t i = 0; i < (size_t)rc; ++i)
          update_scr_pos((char)pload8(base + (uint32_t)i), 1);
      }
      return rc;
    }
    if (mode == XFR_READ)
    {
      long rc;
      if (!(entry_flags & SFT_FEOF))
        return 0;
      if (entry_flags & SFT_FCONIN)
        rc = read_line_handle(sft_idx, n, bp);
      else
        rc = cooked_read(&dev, n, bp);
      if (n != 0 && pload8(dosfns_far_linear(bp)) == CTL_Z)
      {
        const sft_ref current(x86_sft);
        current.flags((UWORD)(current.flags() & ~SFT_FEOF));
      }
      return rc;
    }
    sft.flags((UWORD)(entry_flags | SFT_FEOF));
    if (entry_flags & SFT_FNUL)
      return n;
    return cooked_write(&dev, n, bp);
  }
  if (IsShareInstalled(FALSE))
  {
    const WORD shroff = sft.shroff();
    if (shroff >= 0)
    {
      const int rc = share_access_check((UWORD)dosfns_idata.cu_psp(), shroff, sft.position(), (unsigned long)n, 1);
      if (rc != SUCCESS)
        return rc;
    }
  }
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
  unsigned length = strlen(fname);
  char c;

  fname += length;
  while (length)
  {
    length--;
    c = *--fname;
    if (c == '/' || c == '\\' || c == ':')
    {
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
static dos_far_ptr IsDeviceRef(fdos_path_ref fname)
{
  dos_far_ptr x86_dhp;
  size_t len = fdos_path_len(fname, FDOS_PATHLEN);
  size_t root = 0;
  size_t k;
  int i;

  for (k = 0; k < len; ++k)
  {
    UBYTE c = fdos_path_get(fname, k);
    if (c == '/' || c == '\\' || c == ':')
      root = k + 1u;
  }

  {
    UBYTE c0 = fdos_path_get(fname, root);
    UBYTE c1 = fdos_path_get(fname, root + 1u);
    UBYTE c2 = fdos_path_get(fname, root + 2u);
    if (c0 == 0 || (c0 == '.' && (c1 == 0 || (c1 == '.' && c2 == 0))))
      return MK_FP(0, 0);
  }

  for (x86_dhp = MK_FP(DOS_PSP, (UWORD)(0x08F0u + offsetof(lol, nul_dev)));
       !far_is_end(x86_dhp); )
  {
    const dhdr_ref dhp(x86_dhp);
    const dos_far_ptr next = dhp.next();
    BYTE name[FNAME_SIZE];
    if (dhp.attr() & ATTR_CHAR)
    {
      dhp.read_name(name);
      for (i = 0; i < FNAME_SIZE; i++)
      {
        unsigned char c1 = fdos_path_get(fname, root + (size_t)i);
        if (c1 == '.' || c1 == 0)
        {
          for (; i < FNAME_SIZE; i++)
          {
            const unsigned char c2 = name[i];
            if (c2 != ' ' && c2 != 0)
              break;
          }
          if (i == FNAME_SIZE)
            return x86_dhp;
          break;
        }
        if (DosUpFChar(c1) != DosUpFChar(name[i]))
          break;
      }
      if (i == FNAME_SIZE)
        return x86_dhp;
    }
    x86_dhp = next;
  }
  return MK_FP(0, 0);
}

dos_far_ptr /*struct dhdr*/ IsDevice(const char *fname)
{
  return IsDeviceRef(fdos_path_native(fname));
}

dos_far_ptr IsDeviceGuest(dos_far_ptr fname)
{
  return IsDeviceRef(fdos_path_guest(fname));
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
  const psp_ref process(dosfns_idata.cu_psp());
  const UWORD count = process.max_files();
  const dos_far_ptr table = process.file_table();
  if (far_is_null(table) || far_is_end(table))
    return DE_TOOMANY;
  const uint32_t base = dosfns_far_linear(table);
  for (UWORD h = 0; h < count; ++h)
    if (pload8(base + h) == 0xff)
      return h;
  return DE_TOOMANY;
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

  /* test if mode is in range                     */
  if ((mode & ~O_VALIDMASK) != 0)
    return DE_INVLDACC;

  /* get a free handle  */
  if ((result = get_free_hndl()) < 0)
    return result;
  hndl = (unsigned)result;

  result = DosOpenSft(fname, mode, attrib);
  dpb_watch_check_chain("DosOpen");
  if (result < SUCCESS)
    return result;

  {
    const psp_ref process(dosfns_idata.cu_psp());
    const UWORD count = process.max_files();
    const dos_far_ptr table = process.file_table();
    if (hndl >= count || far_is_null(table) || far_is_end(table))
      return DE_TOOMANY;
    pstore8(dosfns_far_linear(table) + hndl, (UBYTE)result);
  }

  return hndl | (result & 0xffff0000l);
}

COUNT DosGetFattr(dos_far_ptr name)
{
  COUNT result;

  result = truename_guest(name, pri_path_far(), CDS_MODE_CHECK_DEV_PATH);
  dpb_watch_check_chain("DosGetFattr");
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

  if (fdos_path_get(pri_path_ref(), 3) == '\0')
    return 0x10;

  set_fcbname_ref(pri_path_ref());
/// TODO:
///  if (result & IS_NETWORK)
///    return network_redirector(REM_GETATTRZ);

  if (result & IS_DEVICE)
    return DE_FILENOTFND;

  return dos_getfattr_ref(pri_path_ref());
}

/* This function is almost identical to DosGetFattr().
   Maybe it is nice to join both functions.
       -- 2001/09/03 ska*/
COUNT DosSetFattr(dos_far_ptr name, UWORD attrp)
{
  COUNT result;

  result = truename_guest(name, pri_path_far(), CDS_MODE_CHECK_DEV_PATH);
  dpb_watch_check_chain("DosSetFattr");
  if (result < SUCCESS)
    return result;

  set_fcbname_ref(pri_path_ref());
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
    if ((result = share_open_check(pri_path_far(),
                                   DOS_PSP, O_RDWR, 0)) < 0)
      return result;
    /* else dos_setfattr will close the file */
    share_close_file(result);
  }
  return dos_setfattr_ref(pri_path_ref(), attrp);
}

COUNT DosMkRmdir(const dos_far_ptr dir, int action)
{
  COUNT result;

  result = truename_guest(dir, pri_path_far(), CDS_MODE_CHECK_DEV_PATH);
  dpb_watch_check_chain("DosMkRmdir");
  if (result < SUCCESS)
    return result;

  set_fcbname_ref(pri_path_ref());
/// TODO:
///  if (result & IS_NETWORK)
///    return network_redirector(action == 0x39 ? REM_MKDIR : REM_RMDIR);

  if (result & IS_DEVICE)
    return DE_ACCESS;

  return action == 0x39 ? dos_mkdir_ref(pri_path_ref()) : dos_rmdir_ref(pri_path_ref());
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

COUNT DosRenameTrueGuest(dos_far_ptr path1, dos_far_ptr path2, int attrib)
{
  fdos_path_ref p1 = fdos_path_guest(path1);
  fdos_path_ref p2 = fdos_path_guest(path2);
  if (fdos_path_get(p1, 0) != fdos_path_get(p2, 0))
    return DE_DEVICE;
  return dos_rename_ref(p1, p2, attrib);
}

COUNT DosRename(dos_far_ptr path1, dos_far_ptr path2)
{
  COUNT result;

  result = truename_guest(path2, sec_path_far(), CDS_MODE_CHECK_DEV_PATH);
  dpb_watch_check_chain("DosRename 1");
  if (result < SUCCESS)
    return result;

  if ((result & (IS_NETWORK | IS_DEVICE)) == IS_DEVICE)
    return DE_FILENOTFND;

  result = truename_guest(path1, pri_path_far(), CDS_MODE_CHECK_DEV_PATH);
  dpb_watch_check_chain("DosRename 2");
  if (result < SUCCESS)
    return result;

  set_fcbname_ref(pri_path_ref());

  if ((result & (IS_NETWORK | IS_DEVICE)) == IS_DEVICE)
    return DE_FILENOTFND;

  return dos_rename_ref(pri_path_ref(), sec_path_ref(), D_ALL);
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
  COUNT rc = truename_guest(src, pri_path_far(), CDS_MODE_ALLOW_WILDCARDS);
  dpb_watch_check_chain("DosTruename");
  if (rc >= SUCCESS) {
    /* dest is the guest's ES:DI. A truename can be 128 bytes, so a guest
       that points DI near the end of its segment must have the tail wrap
       back to ES:0000, not run on into the next segment. */
    {
      const size_t n = fdos_path_len(pri_path_ref(), FDOS_PATHLEN - 1u);
      size_t i;
      for (i = 0; i <= n; ++i)
        pstore8(((uint32_t)FP_SEG(dest) << 4) + (UWORD)(FP_OFF(dest) + (UWORD)i),
                fdos_path_get(pri_path_ref(), i));
    }
    set_fcbname_ref(pri_path_ref());
  }
  return rc;
}

/*
    DosGetFree() - INT 21h AH=36h / AH=1Bh/1Ch backend.
    Ported from the original kernel (dosfns.c). Port adaptations:
      - cds/dpb far pointers follow the DosGetExtFree() idiom below
        (dos_far_ptr for the guest pointer, ARM_PTR() to dereference);
      - current_ldt lives in the SDA (internal_data->current_ldt) and
        stores the GUEST pointer, as the rest of the port expects;
      - CDSNETWDRV: network redirector is stubbed out on this platform
        (no NIC on the RP2350 target) - a network drive simply reports
        "invalid drive" (0xffff), which is also what the original does
        when remote_getfree() fails.
    navc==NULL means: called from FatGetDrvData, fcbfns.c */
UWORD DosGetFree(UBYTE drive, UWORD * navc, UWORD * bps, UWORD * nc)
{
  UWORD spc = (UWORD)-1;
  const UBYTE drive_index = drive == 0 ? (UBYTE)dosfns_idata.default_drive() : (UBYTE)(drive - 1);
  const dos_far_ptr cdsp_x86 = get_cds(drive_index);
  if (far_is_null(cdsp_x86))
    return spc;
  const cds_ref cdsp(cdsp_x86);
  dosfns_idata.current_ldt(cdsp_x86);
  if (cdsp.flags() & CDSNETWDRV)
    return spc;
  const dos_far_ptr dpbp_x86 = cdsp.dpb();
  if (far_is_null(dpbp_x86))
    return spc;
  const dpb_ref before(dpbp_x86);
  if (navc == NULL)
  {
    flush_buffers(before.unit());
    dpb_ref(dpbp_x86).flags(M_CHANGED);
  }
  if (media_check(dpbp_x86) < 0)
    return spc;
  const dpb_ref dpbp(dpbp_x86);
  spc = (UWORD)(dpbp.cluster_mask() + 1u);
  *bps = dpbp.dpb_secsize();
#ifdef WITHFAT32
  if (dpbp.is_fat32())
  {
    ULONG cluster_size = (ULONG)dpbp.dpb_secsize() << dpbp.dpb_shftcnt();
    ULONG ntotal = dpbp.dpb_xsize() - 1;
    ULONG nfree = navc != NULL ? dos_free(dpbp_x86) : 0;
    while (ntotal > FAT_MAGIC16 && cluster_size < 0x8000)
    {
      cluster_size <<= 1; spc <<= 1; ntotal >>= 1; nfree >>= 1;
    }
    *nc = ntotal > FAT_MAGIC16 ? FAT_MAGIC16 : (UCOUNT)ntotal;
    if (navc != NULL)
      *navc = nfree > FAT_MAGIC16 ? FAT_MAGIC16 : (UCOUNT)nfree;
    return spc;
  }
#endif
  if (navc != NULL)
    *navc = (UWORD)dos_free(dpbp_x86);
  *nc = dpbp.dpb_size() - 1;
  if (spc > 64)
  {
    spc >>= 1;
    if (navc != NULL)
      *navc = ((unsigned)*navc < FAT_MAGIC16 / 2) ? ((unsigned)*navc << 1) : FAT_MAGIC16;
    *nc = ((unsigned)*nc < FAT_MAGIC16 / 2) ? ((unsigned)*nc << 1) : FAT_MAGIC16;
  }
  return spc;
}

/* ------------------------------------------------------------------
   Block C helpers.
   SetJFTSize()/DosMkTmp() ported from the original kernel/newstuff.c
   (the port keeps them here next to DosTruename(), which also came
   from newstuff.c). DosLockUnlock() ported from kernel/dosfns.c.
   ------------------------------------------------------------------ */

/*
    TE-TODO (inherited from the original): if called repeatedly by same
    process, last allocation must be freed. if handle count < 20, copy
    back to PSP.
*/
int SetJFTSize(UWORD nHandles)
{
  UWORD block, maxBlock, i;
  const psp_ref process((seg)(UWORD)dosfns_idata.cu_psp());
  dos_far_ptr newtab;

  if (nHandles <= process.max_files())
  {
    process.max_files(nHandles);
    return SUCCESS;
  }

  if ((DosMemAlloc((nHandles + 0xf) >> 4, (UBYTE)dosfns_idata.mem_access_mode(),
                   &block, &maxBlock)) < 0)
    return DE_NOMEM;

  ++block;
  newtab = MK_FP(block, 0);

  i = process.max_files();
  /* Copy the existing guest JFT into the new guest JFT, then fill the rest
     with "no open file". If ps_filetab is a sentinel, inherit nothing: a
     guest copy from it would read IVT bytes and turn them into SFT indices. */
  if (far_is_null(process.file_table()) || far_is_end(process.file_table()))
    i = 0;
  else
    guest_move_block(dosfns_far_linear(newtab),
                     dosfns_far_linear(process.file_table()), i);
  fmemset(MK_FP(block, i), 0xff, nHandles - i);

  process.max_files(nHandles);
  process.file_table(newtab);

  return SUCCESS;
}

long DosMkTmp(dos_far_ptr pathname, UWORD attr)
{
  /* create filename from current date and time */
  /* the guest's DS:DX buffer is mutated in place, as documented for
     INT 21h AH=5Ah: the generated name is appended to the path */
  const guest_bytes_ref path(pathname);
  size_t ptmp = guest_strnlen_block(path.linear(), NAMEMAX);
  unsigned long randvar;
  long rc;
  int loop;

  if ((UBYTE)dosfns_lol.os_major() == 5) { /* clone some bad habit of MS DOS 5.0 only */
    if (ptmp == 0 || (path.byte(ptmp - 1) != '\\' && path.byte(ptmp - 1) != '/'))
      path.byte(ptmp++, '\\');
  }
  path.byte(ptmp + 8, '\0');

  randvar = ((unsigned long)dos_getdate() << 16) | dos_gettime();

  loop = 0;
  do {
    unsigned long tmp = randvar++;
    int i;
    for(i = 7; i >= 0; tmp >>= 4, i--)
      path.byte(ptmp + i, (UBYTE)(((char)tmp & 0xf) + 'A'));

    /* DOS versions: > 5: characters A - P
       < 5: hex digits */
    if ((UBYTE)dosfns_lol.os_major() < 5)
      for (i = 0; i < 8; i++) {
        UBYTE ch = path.byte(ptmp + i);
        ch = (UBYTE)(ch - (ch < 'A' + 10 ? '0' - 'A' : 10));
        path.byte(ptmp + i, ch);
      }

    /* only create new file -- 2001/09/22 ska*/
    rc = DosOpen(pathname, O_LEGACY | O_CREAT | O_RDWR, attr);
  } while (rc == DE_FILEEXISTS && loop++ < 0xfff);

  return rc;
}

/* see RBIL D-2152 and D-215D06 before attempting
   to change these two functions!
   (ported from the original kernel/network.c; these are not network
   I/O - they only book-keep the machine name in the SDA and the
   NetBios number in the List of Lists, so they are ported for real
   rather than stubbed)
 */
UWORD get_machine_name(dos_far_ptr /* -> char[16] */ netname)
{
  BYTE name[16];
  guest_read_block(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF +
                       offsetof(dos_data, net_name),
                   name, sizeof(name));
  guest_write(netname, name, sizeof(name));
  return pload16(((uint32_t)DOS_PSP << 4) + 0x08F0u + offsetof(lol, NetBios));
}

VOID set_machine_name(dos_far_ptr netname, UWORD name_num)
{
  BYTE name[15];
  const uint32_t sda = ((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF;
  const uint32_t lol_addr = ((uint32_t)DOS_PSP << 4) + 0x08F0u;
  pstore16(lol_addr + offsetof(lol, NetBios), name_num);
  guest_read(name, netname, sizeof(name));
  guest_write_block(sda + offsetof(dos_data, net_name), name, sizeof(name));
  pstore8(sda + offsetof(dos_data, net_set_count),
          (UBYTE)(pload8(sda + offsetof(dos_data, net_set_count)) + 1u));
}

COUNT DosLockUnlock(COUNT hndl, LONG pos, LONG len, COUNT unlock)
{
  const dos_far_ptr x86_sft = get_sft(hndl);
  if (far_is_end(x86_sft))
    return DE_INVLDHNDL;
  const sft_ref sft(x86_sft);
  if (sft.flags() & SFT_FSHARED)
    return DE_INVLDFUNC;
  if (!IsShareInstalled(FALSE))
    return DE_INVLDFUNC;
  const WORD shroff = sft.shroff();
  if (shroff < 0)
    return DE_LOCK;
  return share_lock_unlock((UWORD)dosfns_idata.cu_psp(), shroff, pos, len, unlock);
}

#ifdef WITHFAT32
/* same convention as get_cds1(): drive is 0 for default, 1=A, 2=B, ... */
dos_far_ptr /*struct dpb*/ GetDriveDPB(UBYTE drive, COUNT *rc)
{
  const dos_far_ptr cdsp_x86 = drive == 0 ? get_cds((UBYTE)dosfns_idata.default_drive()) : get_cds((UBYTE)(drive - 1));
  if (far_is_null(cdsp_x86))
  {
    *rc = DE_INVLDDRV;
    return MK_FP(0, 0);
  }
  const cds_ref cdsp(cdsp_x86);
  const dos_far_ptr dpb = cdsp.dpb();
  if (far_is_null(dpb) || (cdsp.flags() & CDSNETWDRV))
  {
    *rc = DE_INVLDDRV;
    return MK_FP(0, 0);
  }
  *rc = SUCCESS;
  return dpb;
}

#define IS_SLASH(ch) ((ch) == '\\' || (ch) == '/')
COUNT DosGetExtFree(dos_far_ptr DriveString, dos_far_ptr xfsp)
{
  const guest_bytes_ref drive(DriveString);
  const guest_bytes_ref out(xfsp);
  dos_far_ptr cdsp_x86 = MK_FP(0, 0);

  guest_fill_block(out.linear(), 0, sizeof(struct xfreespace));
  out.word(offsetof(struct xfreespace, xfs_datasize), sizeof(struct xfreespace));

  const UBYTE ch0 = drive.byte(0);
  const UBYTE ch1 = drive.byte(1);
  if (!ch0 || ch0 == '.' || (IS_SLASH(ch0) && !IS_SLASH(ch1)))
    cdsp_x86 = get_cds((UBYTE)dosfns_idata.default_drive());
  else if (ch1 == ':')
    cdsp_x86 = get_cds(DosUpFChar(ch0) - 'A');
  if (far_is_null(cdsp_x86))
    return DE_INVLDDRV;
  const cds_ref cdsp(cdsp_x86);
  if (cdsp.flags() & CDSNETWDRV)
    return DE_INVLDDRV;
  const dos_far_ptr dpbp_x86 = cdsp.dpb();
  if (far_is_null(dpbp_x86) || media_check_tagged(dpbp_x86, "DosGetExtFree/cdsDpb") < 0)
    return DE_INVLDDRV;
  const dpb_ref dpbp(dpbp_x86);

  const ULONG secsize = dpbp.dpb_secsize();
#ifdef WITHFAT32
  const ULONG totalclusters =
      (dpbp.is_fat32() ? dpbp.dpb_xsize() : dpbp.dpb_size()) - 1;
#else
  const ULONG totalclusters = dpbp.dpb_size() - 1;
#endif
  const ULONG freeclusters = dos_free(dpbp_x86);
  const ULONG clussize = dpbp.cluster_mask() + 1;

  out.dword(offsetof(struct xfreespace, xfs_secsize), secsize);
  out.dword(offsetof(struct xfreespace, xfs_totalclusters), totalclusters);
  out.dword(offsetof(struct xfreespace, xfs_freeclusters), freeclusters);
  out.dword(offsetof(struct xfreespace, xfs_clussize), clussize);
  out.dword(offsetof(struct xfreespace, xfs_totalunits), totalclusters);
  out.dword(offsetof(struct xfreespace, xfs_freeunits), freeclusters);
  out.dword(offsetof(struct xfreespace, xfs_totalsectors), totalclusters * clussize);
  out.dword(offsetof(struct xfreespace, xfs_freesectors), freeclusters * clussize);
  return SUCCESS;
}

#undef IS_SLASH
#endif

/* declared in proto.h, called by INT 21h AH=47h
   (GET CURRENT DIRECTORY - see fdos_21h.c)
   drive: 0 = default drive, 1 = A:, 2 = B:, ...
   dst: destination buffer as a guest dos_far_ptr (ES:DI / DS:SI as
   passed by the caller - see MK_FP(CPU_DS, CPU_SI) at the call site). */
COUNT DosGetCuDir(UBYTE drive, dos_far_ptr dst)
{
  /*
   * Match upstream FreeDOS: canonicalize "X:" with
   * CDS_MODE_SKIP_PHYSICAL, then return the path without "X:\".
   * Directly copying cdsCurrentPath is not equivalent for SUBST/JOIN.
   */
  if (drive-- == 0)
    drive = (UBYTE)dosfns_idata.default_drive();

  pstore8(sec_path_ref().guest_linear + 0u, (UBYTE)('A' + (drive & 0x1f)));
  pstore8(sec_path_ref().guest_linear + 1u, ':');
  pstore8(sec_path_ref().guest_linear + 2u, 0);

  if (truename_guest(sec_path_far(), pri_path_far(),
                    CDS_MODE_SKIP_PHYSICAL) < SUCCESS)
    return DE_INVLDDRV;

  /* dst is the guest's DS:SI (AH=47h); same wrap reasoning as DosTruename. */
  {
    size_t i;
    fdos_path_ref sub = fdos_path_sub(pri_path_ref(), 3u);
    const size_t n = fdos_path_len(sub, FDOS_PATHLEN - 3u);
    for (i = 0; i <= n; ++i)
      pstore8(((uint32_t)FP_SEG(dst) << 4) + (UWORD)(FP_OFF(dst) + (UWORD)i),
              fdos_path_get(sub, i));
  }
  return SUCCESS;
}

/* declared in proto.h, called by INT 21h AH=3Bh
   (CHDIR - see fdos_21h.c), but never implemented in this port.
   Migrated from upstream dosfns.c; network-redirector branch dropped in
   the same style already used above for DosMkRmdir/DosRenameTrue in
   this file (no network_redirector() in this port). */
COUNT DosChangeDir(dos_far_ptr s)
{
  COUNT result;

  result = truename_guest(s, pri_path_far(), CDS_MODE_CHECK_DEV_PATH);
  dpb_watch_check_chain("DosChangeDir");
  if (result < SUCCESS)
    return DE_PATHNOTFND;

  set_fcbname_ref(pri_path_ref());

  if (CDS_WRITABLE(dosfns_idata.current_ldt()) && (fdos_path_len(pri_path_ref(), FDOS_PATHLEN) >= MAX_CDSPATH))
    return DE_PATHNOTFND;

  /// TODO:
  ///  if (result & IS_NETWORK)
  ///    return network_redirector(REM_CHDIR);

  result = dos_cd_ref(pri_path_ref());
  if (result < SUCCESS)
    return result;

  /* Copy the path to the current directory structure. Some redirectors
     do not write back to the CDS - not applicable here (no redirector),
     kept for parity with upstream. */
  const dos_far_ptr current_ldt = dosfns_idata.current_ldt();
  if (CDS_WRITABLE(current_ldt))
  {
    const cds_ref cdsp(current_ldt);
    {
      const size_t n = fdos_path_len(pri_path_ref(), MAX_CDSPATH - 1u);
      guest_move_block(
          ((uint32_t)FP_SEG(dosfns_idata.current_ldt()) << 4) +
              FP_OFF(dosfns_idata.current_ldt()) + offsetof(cds, cdsCurrentPath),
          pri_path_ref().guest_linear, n + 1u);
    }
    if (fdos_path_get(pri_path_ref(), 7) == 0)
      cdsp.current_path_byte(8, 0);
  }
  return SUCCESS;
}

/* declared in proto.h, called by INT 21h AH=41h
   (see fdos_21h.c), but never implemented in this port. Migrated from
   upstream dosfns.c; network/share branches dropped in the same style
   already used above for DosMkRmdir/DosRenameTrue in this file, since
   network_redirector()/share_is_file_open() are not ported (single-user,
   no SHARE.EXE / redirector support). */
COUNT DosDelete(dos_far_ptr path, int attrib)
{
  COUNT result;

  result = truename_guest(path, pri_path_far(), CDS_MODE_CHECK_DEV_PATH);
  dpb_watch_check_chain("DosDelete");
  if (result < SUCCESS)
    return result;

  set_fcbname_ref(pri_path_ref());

  /// TODO:
  ///  if (result & IS_NETWORK)
  ///    return network_redirector(REM_DELETE);

  if (result & IS_DEVICE)
    return DE_FILENOTFND;

  ///  if (IsShareInstalled(TRUE) && share_is_file_open(PriPathName))
  ///    return DE_ACCESS;

  return dos_delete_ref(pri_path_ref(), attrib);
}

/* DosFindFirst()/DosFindNext() (INT 21h AH=4Eh/4Fh).
   The caller DTA, SDA search prefix, and SearchDir are guest-resident.
   Access them through dmatch_ref/pload/pstore instead of manufacturing
   native aliases; this keeps enumeration valid across paging cache
   refills.  Network redirector handling remains omitted. */
STATIC int pop_dmp(int rc, dos_far_ptr dta_far)
{
  dosfns_idata.dta(dta_far);
  if (rc == SUCCESS)
  {
    BYTE name[FNAME_SIZE + FEXT_SIZE + 2];
    const dmatch_ref dm(dta_far);

    {
      size_t i;
      for (i = 0; i < offsetof(dmatch, dm_attr_fnd); ++i)
        pstore8(((uint32_t)FP_SEG(dta_far) << 4) + (UWORD)(FP_OFF(dta_far) + (UWORD)i),
                pload8(sda_dmatch_linear() + (uint32_t)i));
    }
    dm.attr_found(pload8(sda_searchdir_linear() + offsetof(struct dirent, dir_attrib)));
    dm.time_found(pload16(sda_searchdir_linear() + offsetof(struct dirent, dir_time)));
    dm.date_found(pload16(sda_searchdir_linear() + offsetof(struct dirent, dir_date)));
    dm.size_found(pload32(sda_searchdir_linear() + offsetof(struct dirent, dir_size)));
    {
      BYTE raw[FNAME_SIZE + FEXT_SIZE];
      guest_read_block(sda_searchdir_linear() + offsetof(struct dirent, dir_name), raw, sizeof(raw));
      ConvertName83ToNameSZ(name, raw);
    }
    dm.write_name(name, strlen((const char *)name) + 1u);
  }
  return rc;
}

COUNT DosFindFirst(UCOUNT attr, dos_far_ptr name)
{
  int rc;
  dos_far_ptr dta_far = dosfns_idata.dta();

  rc = truename_guest(name, pri_path_far(), CDS_MODE_CHECK_DEV_PATH | CDS_MODE_ALLOW_WILDCARDS);
  dpb_watch_check_chain("DosFindFirst");
  if (rc < SUCCESS)
    return rc;

  set_fcbname_ref(pri_path_ref());

  pstore8(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF + offsetof(dos_data, SAttr), (UBYTE)attr);

  dosfns_idata.dta(sda_dmatch_far());
  guest_fill_block(sda_dmatch_linear(), 0, offsetof(dmatch, dm_attr_fnd));
  guest_fill_block(sda_searchdir_linear(), 0, sizeof(struct dirent));

  /// TODO:
  ///  if (rc & IS_NETWORK)
  ///    return network_redirector_fp(REM_FINDFIRST, current_ldt);

  if (rc & IS_DEVICE)
  {
    COUNT i;
    size_t root = 0;
    const size_t plen = fdos_path_len(pri_path_ref(), FDOS_PATHLEN);

    /* make sure the next search fails */
    pstore16(sda_dmatch_linear() + offsetof(dmatch, dm_entry), 0xffff);
    /* Found a matching device. Hence there cannot be wildcards. */
    pstore8(sda_searchdir_linear() + offsetof(struct dirent, dir_attrib), D_DEVICE);
    pstore16(sda_searchdir_linear() + offsetof(struct dirent, dir_time), dos_gettime());
    pstore16(sda_searchdir_linear() + offsetof(struct dirent, dir_date), dos_getdate());
    for (size_t k = 0; k < plen; ++k)
    {
      UBYTE c = fdos_path_get(pri_path_ref(), k);
      if (c == '/' || c == '\\' || c == ':')
        root = k + 1u;
    }
    guest_fill_block(sda_searchdir_linear() + offsetof(struct dirent, dir_name), ' ', FNAME_SIZE + FEXT_SIZE);
    for (i = 0; i < FNAME_SIZE; ++i)
    {
      UBYTE c = fdos_path_get(pri_path_ref(), root + (size_t)i);
      if (!c || c == '.')
        break;
      pstore8(sda_searchdir_linear() + offsetof(struct dirent, dir_name) + (uint32_t)i, c);
    }
    rc = SUCCESS;
  }
  else
    rc = dos_findfirst_ref(attr, pri_path_ref());

  return pop_dmp(rc, dta_far);
}

COUNT DosFindNext(void)
{
  COUNT rc;
  dos_far_ptr dta_far = dosfns_idata.dta();

  /* DTA is guest-supplied; only the 21-byte search prefix is consumed by
     the FAT walker. Keep that native working state, but never alias the DTA. */
  {
    size_t i;
    for (i = 0; i < offsetof(dmatch, dm_attr_fnd); ++i)
      pstore8(sda_dmatch_linear() + (uint32_t)i,
              pload8(((uint32_t)FP_SEG(dta_far) << 4) + (UWORD)(FP_OFF(dta_far) + (UWORD)i)));
  }

  /* findnext will always fail on a volume id search or device name */
  /* Upstream guards the dm_entry sentinel with "!(dm_drive & 0x80)": bit 7
     of dm_drive marks a redirector search, where FFFFh is a legitimate
     handle value rather than the "no more files" marker. The port dropped
     the guard. It is unreachable today (no redirector can create such a
     search), but the DTA is guest-writable, so restore upstream's exact
     condition rather than rely on that. */
  if ((pload8(sda_dmatch_linear() + offsetof(dmatch, dm_attr_srch)) & ~(D_RDONLY | D_ARCHIVE | D_DEVICE)) == D_VOLID
      || (!(pload8(sda_dmatch_linear() + offsetof(dmatch, dm_drive)) & 0x80) &&
          pload16(sda_dmatch_linear() + offsetof(dmatch, dm_entry)) == 0xffff))
    return DE_NFILES;

  guest_fill_block(sda_searchdir_linear(), 0, sizeof(struct dirent));
  dosfns_idata.dta(sda_dmatch_far());
  rc = dos_findnext();

  return pop_dmp(rc, dta_far);
}

COUNT DosGetFtime(COUNT hndl, ddate * dp, dtime * tp)
{
  dos_far_ptr _s = get_sft(hndl);
  if ( far_is_end (_s) )
    return DE_INVLDHNDL;
  const sft_ref sft(_s);
  *dp = sft.date();
  *tp = sft.time();
  return SUCCESS;
}

/*
 * Set date/time on an SFT entry.
 *
 * sft_idx is already a system-file-table index.  Do not pass it through
 * get_sft(), which accepts a process handle and performs a JFT lookup.
 */
COUNT DosSetFtimeSft(int sft_idx, ddate dp, dtime tp)
{
  dos_far_ptr _s = idx_to_sft(sft_idx);
  if (far_is_end(_s))
    return DE_INVLDHNDL;
  const sft_ref sft(_s);
  if (sft.flags() & SFT_FDEVICE)
    return SUCCESS;
  sft.flags((UWORD)(sft.flags() | SFT_FDATE));
  sft.date(dp);
  sft.time(tp);

  return SUCCESS;
}

} /* extern "C" */
