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
  fnp->f_dmp = &sda_tmp_dmD;
  if (fnp == &fnode[1])
    fnp->f_dmp = &sda_tmp_dm_renD;
  fnp->f_offset = 0l;
  fnp->f_cluster_offset = 0;

  /* root directory */
#ifdef WITHFAT32
  if (dirstart == 0)
    if (ISFAT32(fnp->f_dpb))
      dirstart = fnp->f_dpb->dpb_xrootclst;
#endif
  fnp->f_cluster = fnp->f_dmp->dm_dircluster = dirstart;
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

    Migrated from fatdir.c. getdirent()'s fmemcpy() (dos_far_ptr-based,
    see proto.h) is replaced with a plain memcpy() here: both sides
    (bp->b_buffer and fnp->f_dir) are native ARM memory (see fnode.h's
    note on f_node not being guest-visible), so there is no far
    pointer to translate.
*/
COUNT dir_read(REG f_node_ptr fnp)
{
  struct buffer *bp;
  REG UWORD secsize = fnp->f_dpb->dpb_secsize;
  unsigned sector;
  unsigned entry = fnp->f_dmp->dm_entry;

  /* can't have more than 65535 directory entries */
  if (entry >= 65535U)
      return DE_SEEK;

  /* Determine if we hit the end of the directory. If we have,    */
  /* bump the offset back to the end and exit. If not, fill the   */
  /* dirent portion of the fnode, set the SFT_FCLEAN bit and leave,*/
  /* but only for root directories                                */

  if (fnp->f_dmp->dm_dircluster == 0)
  {
    if (entry >= fnp->f_dpb->dpb_dirents)
      return DE_SEEK;

    fnp->f_dirsector = entry / (secsize / DIRENT_SIZE) +
      fnp->f_dpb->dpb_dirstrt;
  }
  else
  {
    /* Do a "seek" to the directory position        */
    fnp->f_offset = entry * (ULONG)DIRENT_SIZE;

    /* Search through the FAT to find the block     */
    /* that this entry is in.                       */
    if (map_cluster(fnp, XFR_READ) != SUCCESS)
      return DE_SEEK;

    /* Compute the block within the cluster and the */
    /* offset within the block.                     */
    sector = (UBYTE)(fnp->f_offset / secsize) & fnp->f_dpb->dpb_clsmask;

    fnp->f_dirsector = clus2phys(fnp->f_cluster, fnp->f_dpb) + sector;
    /* Get the block we need from cache             */
  }

  bp = getblock(fnp->f_dirsector, fnp->f_dpb->dpb_unit);

  /* Now that we have the block for our entry, get the    */
  /* directory entry.                                     */
  if (bp == NULL)
    return DE_BLKINVLD;

  bp->b_flag &= ~(BFR_DATA | BFR_FAT);
  bp->b_flag |= BFR_DIR | BFR_VALID;

  fnp->f_diridx = entry % (secsize / DIRENT_SIZE);
  memcpy(&fnp->f_dir, &bp->b_buffer[fnp->f_diridx * DIRENT_SIZE],
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
f_node_ptr dir_open(register const char *dirname, BOOL split, f_node_ptr fnp)
{
  int i;
  char *fcbname;

  /* determine what drive and dpb we are using...                 */
  fnp->f_dpb = get_dpb(dirname[0]-'A');
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
  fnp->f_dmp->dm_entry = 0;

  dirname += 2;               /* Assume FAT style drive       */
  fcbname = fnp->f_dmp->dm_name_pat;
  while(*dirname != '\0')
  {
    /* skip the path seperator                              */
    ++dirname;

    /* don't continue if we're at the end: this check is    */
    /* for root directories, the only fully-qualified path  */
    /* names that end in a \                                */
    if (*dirname == '\0')
      break;

    /* Convert the name into an absolute name for           */
    /* comparison...                                        */

    dirname = ConvertNameSZToName83(fcbname, dirname);

    /* do not continue if we split the filename off and are */
    /* at the end                                           */
    if (split && *dirname == '\0')
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
      fnp->f_dmp->dm_entry++;
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
      fnp->f_dmp->dm_entry = 0;
    }
  }
  return fnp;
}

/* Description.
 *  Write fnp->f_dir entry to disk if "update" arg is FALSE, or it's TRUE and
 *  the entry has ever been written (modified) according to its flags.
 * Side effects.
 *    1. F_DMOD flag if original directory entry was modified.
 * Return value.
 *  TRUE  - all OK.
 *  FALSE - error occured (fnode is released).

    Migrated from fatdir.c. fputbyte()/putdirent() (dos_far_ptr-based
    macros, see proto.h) become plain *(UBYTE*)=.../memcpy(): bp->b_buffer
    and fnp->f_dir are both native ARM memory here (see fnode.h's note
    on f_node not being guest-visible), so there is no far pointer to
    translate, the same reasoning as dir_read()'s memcpy() above.
*/
BOOL dir_write_update(REG f_node_ptr fnp, BOOL update)
{
  struct buffer *bp;
  UBYTE *vp;

  /* Update the entry if it was modified by a write or create...  */
  if (!update || (fnp->f_flags & (SFT_FCLEAN|SFT_FDATE)) != SFT_FCLEAN)
  {
    bp = getblock(fnp->f_dirsector, fnp->f_dpb->dpb_unit);

    /* Now that we have a block, transfer the directory      */
    /* entry into the block.                                */
    if (bp == NULL)
      return FALSE;

    swap_deleted(fnp->f_dir.dir_name);

    vp = &bp->b_buffer[fnp->f_diridx * DIRENT_SIZE];

    if (update)
    {
      /* only update fields that are also in the SFT, for dos_close/commit */
      memcpy(&vp[DIR_NAME], fnp->f_dir.dir_name, FNAME_SIZE + FEXT_SIZE);
      vp[DIR_ATTRIB] = fnp->f_dir.dir_attrib;
      fputword(&vp[DIR_TIME], fnp->f_dir.dir_time);
      fputword(&vp[DIR_DATE], fnp->f_dir.dir_date);
      fputword(&vp[DIR_START], fnp->f_dir.dir_start);
#ifdef WITHFAT32
      if (ISFAT32(fnp->f_dpb))
        fputword(&vp[DIR_START_HIGH], fnp->f_dir.dir_start_high);
#endif
      fputlong(&vp[DIR_SIZE], fnp->f_dir.dir_size);
    }
    else
    {
      memcpy(vp, &fnp->f_dir, sizeof(struct dirent));
    }

    swap_deleted(fnp->f_dir.dir_name);

    bp->b_flag &= ~(BFR_DATA | BFR_FAT);
    bp->b_flag |= BFR_DIR | BFR_DIRTY | BFR_VALID;
  }
  /* Clear buffers after directory write or DOS close                     */
  return flush_buffers(fnp->f_dpb->dpb_unit);
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
  memset(fcbname, ' ', FNAME_SIZE + FEXT_SIZE);

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
