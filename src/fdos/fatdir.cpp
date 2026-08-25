#define new fdos_new
#ifndef _Static_assert
#define _Static_assert static_assert
#define FDOS_LOCAL_STATIC_ASSERT_MACRO 1
#endif
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
#ifdef FDOS_LOCAL_STATIC_ASSERT_MACRO
#undef _Static_assert
#undef FDOS_LOCAL_STATIC_ASSERT_MACRO
#endif
#undef new
#ifdef load
#undef load
#endif

#include "guest_ref.hpp"
#include "path_guest.h"
using fdos_guest::dpb_ref;
using fdos_guest::buffer_ref;

extern "C" {
#define printf(...) dos_printf(__VA_ARGS__)

/* Description.
 *  Initialize a fnode so that it will point to the directory with 
 *  dirstart starting cluster; in case of passing dirstart == 0
 *  fnode will point to the start of a root directory

    Migrated from fatdir.c verbatim. &sda_tmp_dm/&sda_tmp_dm_ren become
    &sda_tmp_dmD/&sda_tmp_dm_renD - see dirmatch.h for why these are
    macros (SDA fields, named with a trailing D so they don't expand
    recursively into themselves) rather than plain variables.
*/
VOID dir_init_fnode(f_node_ptr fnp, CLUSTER dirstart)
{
  /* reset the directory flags    */
  fnp->f_sft_idx = 0xff;
  fnp->f_dmp = dmatch_guest(MK_FP(
      DOS_PSP,
      (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, sda_tmp_dm))));
  if (fnp == fnode_slot(1))
    fnp->f_dmp = dmatch_guest(MK_FP(
        DOS_PSP,
        (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, sda_tmp_dm_ren))));
  fnp->f_offset = 0l;
  fnp->f_cluster_offset = 0;

  /* root directory */
#ifdef WITHFAT32
  if (dirstart == 0)
    { const dpb_ref d(fnp->f_dpb); dirstart = d.dpb_fatsize() == 0 ? d.dpb_xrootclst() : 0; }
#endif
  fnp->f_cluster = dirstart;
  DM_SET32(fnp->f_dmp, dm_dircluster, dirstart);
}

/* swap internal and external delete flags */
/*
    Migrated from fatdir.c verbatim.
*/
STATIC void swap_deleted(char *name)
{
  if (name[0] == DELETED || name[0] == EXT_DELETED)
    name[0] ^= EXT_DELETED - DELETED; /* 0xe0 */
}

/* Description.
 *  Read next consequitive directory entry, pointed by fnp.
 *  If some error occures the other critical
 *  fields aren't changed, except those used for caching.
 *  The fnp->f_dmp->dm_entry always corresponds to the directory entry
 *  which has been read.
 * Return value.
 *  1              - all OK, directory entry having been read is not empty.
 *  0              - Directory entry is empty.
 *  DE_SEEK        - Attempt to read beyound the end of the directory.
 *  DE_BLKINVLD    - Invalid block.
 * Note. Empty directory entries always resides at the end of the directory.

    Migrated from fatdir.c. getdirent()'s fdos_api_memcpy() (dos_far_ptr-based,
    see proto.h) is replaced with a plain dos_api_memcpy() here: both sides
    (bp->b_buffer and fnp->f_dir) are native ARM memory (see fnode.h's
    note on f_node not being guest-visible), so there is no far
    pointer to translate.
*/
COUNT dir_read(REG f_node_ptr fnp)
{
  dos_far_ptr x86_bp;
  const dpb_ref d(fnp->f_dpb);
  const UWORD secsize = d.dpb_secsize();
  unsigned sector;
  unsigned entry = DM_GET16(fnp->f_dmp, dm_entry);

  /* can't have more than 65535 directory entries */
  if (entry >= 65535U)
      return DE_SEEK;

  /* Determine if we hit the end of the directory. If we have,    */
  /* bump the offset back to the end and exit. If not, fill the   */
  /* dirent portion of the fnode, set the SFT_FCLEAN bit and leave,*/
  /* but only for root directories                                */

  if (DM_GET32(fnp->f_dmp, dm_dircluster) == 0)
  {
    const UWORD dirents = d.dpb_dirents();
    if (entry >= dirents)
      return DE_SEEK;

    fnp->f_dirsector = entry / (secsize / DIRENT_SIZE) +
                       d.dpb_dirstrt();
  }
  else
  {
    /* Do a "seek" to the directory position        */
    fnp->f_offset = entry * (ULONG)DIRENT_SIZE;

    /* Search through the FAT to find the block     */
    /* that this entry is in.                       */
    if (map_cluster(fnp, XFR_READ) != SUCCESS)
      return DE_SEEK;

    /* Re-read DPB geometry through the C++ wrapper after map_cluster():
       FAT/cache accesses above may have remapped the pageable DPB. */
    sector = (UBYTE)(fnp->f_offset / secsize) &
             d.dpb_clsmask();

    fnp->f_dirsector = (((ULONG)(fnp->f_cluster - 2u) << d.dpb_shftcnt()) +
#ifdef WITHFAT32
             (d.dpb_fatsize() == 0 ? d.dpb_xdata() : d.dpb_data())
#else
             d.dpb_data()
#endif
            ) +
                       sector;
    /* Get the block we need from cache             */
  }

  x86_bp = getblock(fnp->f_dirsector, d.dpb_unit());

  /* Now that we have the block for our entry, get the    */
  /* directory entry.                                     */
  if (far_is_null(x86_bp))
    return DE_BLKINVLD;

  const buffer_ref b(x86_bp);
  BYTE flag = static_cast<BYTE>(b.flag() & ~(BFR_DATA | BFR_FAT));
  b.flag(static_cast<BYTE>(flag | BFR_DIR | BFR_VALID));

  fnp->f_diridx = entry % (secsize / DIRENT_SIZE);
  b.read_data(fnp->f_diridx * DIRENT_SIZE, &fnp->f_dir,
              sizeof(struct dirent));

  swap_deleted(fnp->f_dir.dir_name);

  /* and for efficiency, stop when we hit the first       */
  /* unused entry.                                        */
  /* either returns 1 or 0                                */
  return (fnp->f_dir.dir_name[0] != '\0');
}

/*
    dir_open(dirname, split, fnp) - walk a fully-qualified path
    (drive letter + ':' + '\\'-separated components) one component at
    a time, starting from the root directory, ending up with fnp
    pointing at either:
      - the directory the path names (split == FALSE), or
      - the directory containing the last component (split == TRUE,
        used by split_path() below to peel the filename off so the
        caller can search for it separately).

    Migrated from fatdir.c verbatim - dirname/fcbname are plain native
    char* strings throughout (see ConvertNameSZToName83() above), so
    no address-translation changes are needed here.
*/
f_node_ptr dir_open_ref(fdos_path_ref dirname, BOOL split, f_node_ptr fnp)
{
  int i;
  size_t pos = 0;
  char *fcbname;

  /* determine what drive and dpb we are using...                 */
  fnp->f_dpb = get_dpb((COUNT)fdos_path_get(dirname, 0)-'A');
  /* Perform all directory common handling after all special      */
  /* handling has been performed.                                 */

  /* truename() already did a media check()                       */

  /* Walk the directory tree to find the starting cluster         */
  /*                                                              */
  /* Start from the root directory (dirstart = 0)                 */

  /* The CDS's cdsStartCls may be used to shorten the search
     beginning at the CWD, see mapPath() and CDS.H in order
     to enable this behaviour there.
           -- 2001/09/04 ska*/

  dir_init_fnode(fnp, 0);
  DM_SET16(fnp->f_dmp, dm_entry, 0);

  pos = 2;                    /* Assume FAT style drive       */
  BYTE fcbname_buf[FNAME_SIZE + FEXT_SIZE];
  dmatch_read_name_pat(fnp->f_dmp, fcbname_buf);
  fcbname = fcbname_buf;
  while (fdos_path_get(dirname, pos) != '\0')
  {
    /* skip the path separator                              */
    ++pos;

    /* don't continue if we're at the end: this check is    */
    /* for root directories, the only fully-qualified path  */
    /* names that end in a \                                */
    if (fdos_path_get(dirname, pos) == '\0')
      break;

    /* Convert the name into an absolute name for           */
    /* comparison.  Read path bytes through the tagged ref  */
    /* so a paging miss can never invalidate a host pointer.*/
    nf_memset(fcbname, ' ', FNAME_SIZE + FEXT_SIZE);
    for (i = 0; i < FNAME_SIZE + FEXT_SIZE; ++i, ++pos)
    {
      char c = (char)fdos_path_get(dirname, pos);
      if (c == '.')
        i = FNAME_SIZE - 1;
      else if (c != '\0' && c != '\\')
        fcbname[i] = c;
      else
        break;
    }
    dmatch_write_name_pat(fnp->f_dmp, fcbname);

    /* do not continue if we split the filename off and are */
    /* at the end                                           */
    if (split && fdos_path_get(dirname, pos) == '\0')
      break;

    /* Now search through the directory to  */
    /* find the entry...                    */
    i = FALSE;

    while (dir_read(fnp) == 1)
    {
      if (!(fnp->f_dir.dir_attrib & D_VOLID) &&
          fcbmatch(fcbname, fnp->f_dir.dir_name))
      {
        i = TRUE;
        break;
      }
      DM_SET16(fnp->f_dmp, dm_entry, (UWORD)(DM_GET16(fnp->f_dmp, dm_entry) + 1));
    }

    if (!i || !(fnp->f_dir.dir_attrib & D_DIR))
    {
      return (f_node_ptr) 0;
    }
    else
    {
      /* make certain we've moved off */
      /* root                         */
      dir_init_fnode(fnp, getdstart(fnp->f_dpb, &fnp->f_dir));
      DM_SET16(fnp->f_dmp, dm_entry, 0);
    }
  }
  return fnp;
}

f_node_ptr dir_open(const char *dirname, BOOL split, f_node_ptr fnp)
{
  return dir_open_ref(fdos_path_native(dirname), split, fnp);
}

/* Description.
 *  Write fnp->f_dir entry to disk if "update" arg is FALSE, or it's TRUE and
 *  the entry has ever been written (modified) according to its flags.
 * Side effects.
 *    1. F_DMOD flag if original directory entry was modified.
 * Return value.
 *  TRUE  - all OK.
 *  FALSE - error occured (fnode is released).

    Migrated from fatdir.c. The directory cache buffer is guest-visible
    pageable memory, so its fields/data are accessed through buffer_ref;
    fnp->f_dir remains native scratch state.
*/
BOOL dir_write_update(REG f_node_ptr fnp, BOOL update)
{
  dos_far_ptr x86_bp;

  /* Update the entry if it was modified by a write or create...  */
  if (!update || (fnp->f_flags & (SFT_FCLEAN|SFT_FDATE)) != SFT_FCLEAN)
  {
    x86_bp = getblock(fnp->f_dirsector, dpb_ref(fnp->f_dpb).dpb_unit());

    /* Now that we have a block, transfer the directory      */
    /* entry into the block.                                */
    if (far_is_null(x86_bp))
      return FALSE;

    swap_deleted(fnp->f_dir.dir_name);

    const buffer_ref b(x86_bp);
    const std::size_t base = fnp->f_diridx * DIRENT_SIZE;
    if (update)
    {
      /* only update fields that are also in the SFT, for dos_close/commit */
      b.write_data(base + DIR_NAME, fnp->f_dir.dir_name, FNAME_SIZE + FEXT_SIZE);
      b.data8(base + DIR_ATTRIB, fnp->f_dir.dir_attrib);
      b.data16(base + DIR_TIME, fnp->f_dir.dir_time);
      b.data16(base + DIR_DATE, fnp->f_dir.dir_date);
      b.data16(base + DIR_START, fnp->f_dir.dir_start);
#ifdef WITHFAT32
      if (dpb_ref(fnp->f_dpb).is_fat32())
        b.data16(base + DIR_START_HIGH, fnp->f_dir.dir_start_high);
#endif
      b.data32(base + DIR_SIZE, fnp->f_dir.dir_size);
    }
    else
    {
      b.write_data(base, &fnp->f_dir, sizeof(struct dirent));
    }

    swap_deleted(fnp->f_dir.dir_name);

    BYTE flag = static_cast<BYTE>(b.flag() & ~(BFR_DATA | BFR_FAT));
    b.flag(static_cast<BYTE>(flag | BFR_DIR | BFR_DIRTY | BFR_VALID));
  }
  /* Clear buffers after directory write or DOS close                     */
  return flush_buffers(dpb_ref(fnp->f_dpb).dpb_unit());
}

/*
    ConvertNameSZToName83(fcbname, dirname) - convert a single path
    component (dirname, a NUL- or '\\'-terminated name) into its
    FCB-style (8.3, space-padded, no dot) form in fcbname, and return
    a pointer to whatever follows it in dirname (the next '\\', or the
    terminating NUL).

    Migrated from fatdir.c verbatim. ". and .. are not allowed [by
    this function], only straightforward 8+3 names" (original comment)
    - dos_open()/split_path() reject "."/".." before this is reached
    (TODO once they're migrated). Operates purely on native char*
    strings (path components are plain C strings throughout this
    file, never dos_far_ptr - see dos_open()'s "path" parameter), so
    no address-translation changes are needed here.
*/
const char *ConvertNameSZToName83(char *fcbname, const char *dirname)
{
  int i;
  nf_memset(fcbname, ' ', FNAME_SIZE + FEXT_SIZE);

  for (i = 0; i < FNAME_SIZE + FEXT_SIZE; i++, dirname++)
  {
    char c = *dirname;
    if (c == '.')
      i = FNAME_SIZE - 1;
    else if (c != '\0' && c != '\\')
      fcbname[i] = c;
    else
      break;
  }
  return dirname;
}

/* dos_findfirst()/dos_findnext() (the FAT-level
   backend for INT 21h AH=4Eh/4Fh - FINDFIRST/FINDNEXT) were declared
   in proto.h but never implemented anywhere in this port, so DIR,
   wildcard COPY, and any program enumerating a directory had no
   working backend at all. Migrated verbatim from upstream fatdir.c,
   using this port's SearchDirD/SAttrD/sda_tmp_dmD macros (see
   dirmatch.h) in place of the original's bare SearchDir/SAttr/
   sda_tmp_dm globals, same rename already applied throughout this
   file for sda_tmp_dm -> sda_tmp_dmD. */
COUNT dos_findfirst_ref(UCOUNT attr, fdos_path_ref name)
{
  REG f_node_ptr fnp;
  dmatch_handle dmp = dmatch_guest(MK_FP(
      DOS_PSP,
      (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, sda_tmp_dm))));

  /* first: findfirst("D:\\") returns DE_NFILES */
  if (fdos_path_get(name, 3) == '\0')
    return DE_NFILES;

  /* Now open this directory so that we can read the      */
  /* fnode entry and do a match on it.                    */
  if ((fnp = split_path_ref(name, fnode_slot(0))) == NULL)
    return DE_PATHNOTFND;

  /* Now search through the directory to find the entry...        */

  /* Special handling - the volume id is only in the root         */
  /* directory and only searched for once.  So we need to open    */
  /* the root and return only the first entry that contains the   */
  /* volume id bit set (while ignoring LFN entries).              */
  /* RBIL: ignore ReaDONLY and ARCHIVE bits but DEVICE ignored too*/
  /* For compatibility with bad search requests, only treat as    */
  /*   volume search if only volume bit set, else ignore it.      */
  if ((attr & ~(D_RDONLY | D_ARCHIVE | D_DEVICE)) == D_VOLID)
    /* if ONLY label wanted redirect search to root dir */
    dir_init_fnode(fnp, 0);

  /* Now further initialize the dirmatch structure.       */
  DM_SET8(dmp, dm_drive, (UBYTE)(fdos_path_get(name, 0) - 'A'));
  DM_SET8(dmp, dm_attr_srch, (UBYTE)attr);

  return dos_findnext();
}

COUNT dos_findfirst(UCOUNT attr, BYTE *name)
{
  return dos_findfirst_ref(attr, fdos_path_native((const char *)name));
}

/*
    BUGFIX TE 06/28/01

    when using FcbFindXxx, the only information available is
    the cluster number + entrycount. everything else MUST
    be recalculated.
    a good test for this is MSDOS CHKDSK, which now (seems too) work
*/

COUNT dos_findnext(void)
{
  REG f_node_ptr fnp;
  dmatch_handle dmp = dmatch_guest(MK_FP(
      DOS_PSP,
      (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(struct dos_data, sda_tmp_dm))));

  /* Select the default to help non-drive specified path          */
  /* searches...                                                  */
  fnp = fnode_slot(0);
  fnp->f_dpb = get_dpb(DM_GET8(dmp, dm_drive));
  if (media_check_tagged(fnp->f_dpb, "dos_findnext/fnp->f_dpb") < 0)
    return DE_NFILES;

  dir_init_fnode(fnp, DM_GET32(dmp, dm_dircluster));

  /* Search through the directory to find the entry, but do a     */
  /* seek first.                                                  */
  /* Loop through the directory                                   */
  while (dir_read(fnp) == 1)
  {
    DM_SET16(dmp, dm_entry, (UWORD)(DM_GET16(dmp, dm_entry) + 1u));
    if (fnp->f_dir.dir_name[0] != DELETED
        && (fnp->f_dir.dir_attrib & D_LFN) != D_LFN)
    {
      BYTE dm_name_pat[FNAME_SIZE + FEXT_SIZE];
      dmatch_read_name_pat(dmp, dm_name_pat);
      if (fcmp_wild(dm_name_pat, fnp->f_dir.dir_name, FNAME_SIZE + FEXT_SIZE))
      {
        /*
           MSD Command.com uses FCB FN 11 & 12 with attrib set to 0x16.
           Bits 0x21 seem to get set some where in MSD so Rd and Arc
           files are returned.
           RdOnly + Archive bits are ignored
         */

        /* Test the attribute as the final step */
        /* It's either a special volume label search or an                 */
        /* attribute inclusive search. The attribute inclusive search      */
        /* can also find volume labels if you set e.g. D_DIR|D_VOLUME      */
        UBYTE attr_srch;
        attr_srch = DM_GET8(dmp, dm_attr_srch) & ~(D_RDONLY | D_ARCHIVE | D_DEVICE);
        if (attr_srch == D_VOLID)
        {
          if (!(fnp->f_dir.dir_attrib & D_VOLID))
            continue;
        }
        else if (~attr_srch & (D_DIR | D_SYSTEM | D_HIDDEN | D_VOLID) &
                 fnp->f_dir.dir_attrib)
          continue;
        /* If found, transfer it to the dmatch structure                */
        guest_write_block(
            ((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF +
                offsetof(struct dos_data, SearchDir),
            &fnp->f_dir, sizeof(struct dirent));
        /* return the result                                            */
        return SUCCESS;
      }
    }
  }

  /* return the result                                            */
  return DE_NFILES;
}

/*
    this receives a name in 11 char field NAME+EXT and builds
    a zeroterminated string

    unfortunately, blanks are allowed in filenames. like
        "test e", " test .y z",...

    so we have to work from the last blank backward

    Migrated verbatim from upstream fatdir.c.
*/
void ConvertName83ToNameSZ(BYTE FAR * destSZ, BYTE FAR * srcFCBName)
{
  int loop;
  int noExtension = FALSE;

  if (*srcFCBName == '.')
  {
    noExtension = TRUE;
  }

  dos_api_memcpy(destSZ, srcFCBName, FNAME_SIZE);

  srcFCBName += FNAME_SIZE;

  for (loop = FNAME_SIZE; --loop >= 0;)
  {
    if (destSZ[loop] != ' ')
      break;
  }
  destSZ += loop + 1;

  if (!noExtension)             /* not for ".", ".." */
  {
    for (loop = FEXT_SIZE; --loop >= 0;)
    {
      if (srcFCBName[loop] != ' ')
        break;
    }
    if (loop >= 0)
    {
      *destSZ++ = '.';
      dos_api_memcpy(destSZ, srcFCBName, loop + 1);
      destSZ += loop + 1;
    }
  }
  *destSZ = '\0';
}

} /* extern "C" */
