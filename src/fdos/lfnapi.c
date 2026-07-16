/****************************************************************/
/*                                                              */
/*                           lfnapi.c                            */
/*                                                              */
/*       Directory access functions for LFN helper API          */
/*                                                              */
/****************************************************************/

#include "hdrs.h"
#include "fdos.h"

#ifndef WITHLFNAPI
#error lfnapi.c must be compiled only when WITHLFNAPI is enabled
#endif

#ifndef LFN_FNODE_COUNT
#define LFN_FNODE_COUNT 8
#endif

#define LHE_INVLDHNDL  -1
#define LHE_NOFREEHNDL -2
#define LHE_IOERROR    -3
#define LHE_INVLDDRV   -4
#define LHE_DAMAGEDFS  -5
#define LHE_NOSPACE    -6
#define LHE_SEEK       -7

#define CHARS_IN_LFN_ENTRY 13
#define UNICODE_FILLER 0xffff
#define lfn(fnp) ((struct lfn_entry *)&(fnp)->f_dir)

/*
 * The original kernel keeps LFN helper handles in its resident DOS data.
 * Keep the same ownership model here: the slots are allocated from Dyn and
 * therefore live in guest conventional memory below the first MCB.  Only the
 * far pointer to the pool occupies native SRAM.
 *
 * Current f_node stores its directory cursor through f_dmp, so each persistent
 * helper fnode needs a private persistent dmatch as well.
 */
typedef struct lfn_fnode_slot {
  UBYTE used;
  UBYTE reserved[3];
  struct f_node fnode;
  dmatch dm;
} lfn_fnode_slot;

static dos_far_ptr lfn_slots_x86;

static lfn_fnode_slot *lfn_slot(UWORD handle)
{
  if (far_is_null(lfn_slots_x86) || handle >= LFN_FNODE_COUNT)
    return NULL;

  return (lfn_fnode_slot *)ARM_PTR(
      ADD_OFF(lfn_slots_x86, (ULONG)handle * sizeof(lfn_fnode_slot)));
}

static f_node_ptr lfn_get_fnode(UWORD handle)
{
  lfn_fnode_slot *slot = lfn_slot(handle);
  return slot != NULL && slot->used ? &slot->fnode : NULL;
}

static void lfn_init_dir_fnode(lfn_fnode_slot *slot, CLUSTER dirstart)
{
  f_node_ptr fnp = &slot->fnode;
  struct dpb *dpb = (struct dpb *)ARM_PTR(fnp->f_dpb);

  memset(&slot->dm, 0, sizeof(slot->dm));
  fnp->f_sft_idx = 0xff;
  fnp->f_dmp = &slot->dm;
  fnp->f_offset = 0;
  fnp->f_cluster_offset = 0;

#ifdef WITHFAT32
  if (dirstart == 0 && ISFAT32(dpb))
    dirstart = dpb->dpb_xrootclst;
#endif

  fnp->f_cluster = dirstart;
  fnp->f_dmp->dm_dircluster = dirstart;
}

void lfnapi_init(void)
{
  if (!far_is_null(lfn_slots_x86))
    return;

  lfn_slots_x86 = DynAlloc("LFN fnodes", LFN_FNODE_COUNT,
                           sizeof(lfn_fnode_slot));
}

COUNT lfn_allocate_inode(void)
{
  dos_far_ptr dpbp = get_dpb(internal_data->default_drive);

  if (far_is_null(dpbp) || media_check(dpbp) < 0)
    return LHE_INVLDDRV;

  for (UWORD handle = 0; handle < LFN_FNODE_COUNT; handle++)
  {
    lfn_fnode_slot *slot = lfn_slot(handle);
    if (!slot->used)
    {
      memset(slot, 0, sizeof(*slot));
      slot->used = TRUE;
      slot->fnode.f_dpb = dpbp;
      slot->fnode.f_dmp = &slot->dm;
      slot->fnode.f_sft_idx = 0xff;
      return handle;
    }
  }

  return LHE_NOFREEHNDL;
}

COUNT lfn_free_inode(UWORD handle)
{
  lfn_fnode_slot *slot = lfn_slot(handle);
  if (slot == NULL || !slot->used)
    return LHE_INVLDHNDL;

  memset(slot, 0, sizeof(*slot));
  return SUCCESS;
}

COUNT lfn_setup_inode(UWORD handle, ULONG dirstart, ULONG diroff)
{
  lfn_fnode_slot *slot = lfn_slot(handle);
  if (slot == NULL || !slot->used)
    return LHE_INVLDHNDL;

  lfn_init_dir_fnode(slot, (CLUSTER)dirstart);
  slot->dm.dm_entry = (UWORD)diroff;
  return SUCCESS;
}

static COUNT ufstrlen(const UNICODE *s)
{
  COUNT cnt = 0;
  while (*s++ != 0)
    cnt++;
  return cnt;
}

static UBYTE lfn_checksum(const UBYTE *sfn_name)
{
  UBYTE sum;
  COUNT i;

  for (sum = 0, i = 11; --i >= 0; sum += *sfn_name++)
    sum = (sum << 7) | (sum >> 1);

  return sum;
}

static BOOL transfer_unicode(UNICODE **dptr, const UNICODE **sptr,
                             COUNT count)
{
  BOOL found_zero = FALSE;

  for (COUNT i = 0; i < count; i++, (*dptr)++, (*sptr)++)
  {
    if (found_zero)
      **dptr = UNICODE_FILLER;
    else
      **dptr = **sptr;

    if (**sptr == 0)
      found_zero = TRUE;
  }

  return found_zero;
}

static BOOL lfn_to_unicode(UNICODE **name, struct lfn_entry *lep)
{
  const UNICODE *ptr;

  ptr = lep->lfn_name0_4;
  if (transfer_unicode(name, &ptr, 5)) return TRUE;
  ptr = lep->lfn_name5_10;
  if (transfer_unicode(name, &ptr, 6)) return TRUE;
  ptr = lep->lfn_name11_12;
  if (transfer_unicode(name, &ptr, 2)) return TRUE;

  return FALSE;
}

static void unicode_to_lfn(const UNICODE **name, struct lfn_entry *lep)
{
  UNICODE *ptr;

  ptr = lep->lfn_name0_4;
  transfer_unicode(&ptr, name, 5);
  ptr = lep->lfn_name5_10;
  transfer_unicode(&ptr, name, 6);
  ptr = lep->lfn_name11_12;
  transfer_unicode(&ptr, name, 2);
}

COUNT lfn_create_entries(UWORD handle, lfn_inode_ptr lip)
{
  f_node_ptr fnp = lfn_get_fnode(handle);
  lfn_fnode_slot *slot = lfn_slot(handle);
  COUNT entries_needed, free_entries, rc;
  const UNICODE *lfn_name;
  UBYTE id = 1;
  UBYTE sfn_checksum;
  UWORD sfn_offset;

  if (fnp == NULL || lip == NULL)
    return LHE_INVLDHNDL;

  lfn_name = lip->l_name;
  sfn_checksum = lfn_checksum((UBYTE*)lip->l_dir.dir_name);
  entries_needed = (ufstrlen(lfn_name) + CHARS_IN_LFN_ENTRY - 1)
                 / CHARS_IN_LFN_ENTRY + 1;

  lfn_setup_inode(handle, fnp->f_dmp->dm_dircluster, 0);
  fnp = &slot->fnode;

  free_entries = 0;
  for (;;)
  {
    rc = dir_read(fnp);
    if (rc == 0 || fnp->f_dir.dir_name[0] == DELETED)
    {
      if (++free_entries == entries_needed)
        break;
    }
    else if (rc == DE_BLKINVLD)
    {
      lfn_free_inode(handle);
      return LHE_IOERROR;
    }
    else if (rc == DE_SEEK)
    {
      if (extend_dir(fnp) != SUCCESS)
      {
        lfn_free_inode(handle);
        return LHE_NOSPACE;
      }
      if (fnp->f_dmp->dm_entry != 0)
        fnp->f_dmp->dm_entry--;
    }
    else
    {
      free_entries = 0;
    }

    fnp->f_dmp->dm_entry++;
  }

  sfn_offset = fnp->f_dmp->dm_entry;
  memcpy(&fnp->f_dir, &lip->l_dir, sizeof(struct dirent));
  if (!dir_write(fnp))
    return LHE_IOERROR;

  fnp->f_dmp->dm_entry--;
  for (COUNT i = 0; i < entries_needed - 1; i++, id++)
  {
    const UNICODE *chunk = &lip->l_name[i * CHARS_IN_LFN_ENTRY];

    memset(&fnp->f_dir, 0, sizeof(fnp->f_dir));
    if (i == entries_needed - 2)
      id |= 0x40;

    unicode_to_lfn(&chunk, lfn(fnp));
    lfn(fnp)->lfn_checksum = sfn_checksum;
    lfn(fnp)->lfn_id = id;
    fnp->f_dir.dir_attrib = D_LFN;

    if (!dir_write(fnp))
      return LHE_IOERROR;

    if (fnp->f_dmp->dm_entry != 0)
      fnp->f_dmp->dm_entry--;
  }

  fnp->f_dmp->dm_entry = sfn_offset;
  lip->l_diroff = sfn_offset;
  return SUCCESS;
}

COUNT lfn_dir_read(UWORD handle, lfn_inode_ptr lip)
{
  f_node_ptr fnp = lfn_get_fnode(handle);
  COUNT rc;
  UBYTE id = 1, real_id;
  UNICODE *lfn_name;
  UWORD sfn_diroff;
  BOOL name_tail;

  if (fnp == NULL || lip == NULL)
    return LHE_INVLDHNDL;

  lfn_name = lip->l_name;
  for (;;)
  {
    rc = dir_read(fnp);
    if (rc == DE_SEEK) return LHE_SEEK;
    if (rc == DE_BLKINVLD) return LHE_IOERROR;

    if (fnp->f_dir.dir_name[0] != DELETED &&
        fnp->f_dir.dir_attrib != D_LFN)
   {
      memcpy(&lip->l_dir, &fnp->f_dir, sizeof(struct dirent));
      sfn_diroff = fnp->f_dmp->dm_entry;
      break;
    }

    fnp->f_dmp->dm_entry++;
  }
  for (;;)
  {
    if (fnp->f_dmp->dm_entry == 0)
      break;

    fnp->f_dmp->dm_entry--;
    rc = dir_read(fnp);
    if (rc == DE_BLKINVLD) return LHE_IOERROR;
    if (fnp->f_dir.dir_name[0] == DELETED ||
        fnp->f_dir.dir_attrib != D_LFN)
      break;

    real_id = lfn(fnp)->lfn_id;
    if ((real_id & 0x3f) > 20)
      return LHE_DAMAGEDFS;

    name_tail = lfn_to_unicode(&lfn_name, lfn(fnp));
    if (real_id & 0x40)
    {
      if ((id | 0x40) != real_id)
        return LHE_DAMAGEDFS;
      break;
    }

    if (name_tail || real_id != id ||
        lfn(fnp)->lfn_checksum != lfn_checksum((UBYTE*)lip->l_dir.dir_name))
      return LHE_DAMAGEDFS;

    id++;
  }

  *lfn_name = 0;
  fnp->f_dmp->dm_entry = sfn_diroff;
  lip->l_diroff = sfn_diroff;
  return SUCCESS;
}

COUNT lfn_dir_write(UWORD handle)
{
  f_node_ptr fnp = lfn_get_fnode(handle);
  if (fnp == NULL)
    return LHE_INVLDHNDL;

  return dir_write(fnp) ? SUCCESS : LHE_IOERROR;
}
