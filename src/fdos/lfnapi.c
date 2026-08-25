/****************************************************************/
/*                                                              */
/*                           lfnapi.c                            */
/*                                                              */
/*       Directory access functions for LFN helper API          */
/*                                                              */
/****************************************************************/

#include "hdrs.h"
#include "fdos.h"
#include "kernel_guest_proxy.h"
#include "lfn_guest_proxy.h"

void *malloc(size_t n);

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
 * LFN helper f_nodes are persistent kernel-only state.  They must not live in
 * pageable guest RAM because FAT/cache operations can remap that backing while
 * a handle remains open.  Keep the slots in native BSS; their guest-facing
 * pointers (f_dpb) remain dos_far_ptr values.  Each slot owns a native dmatch
 * because f_dmp is likewise persistent across directory operations.
 */
typedef struct lfn_fnode_slot {
  UBYTE used;
  UBYTE reserved[3];
  struct f_node fnode;
  dmatch dm;
} lfn_fnode_slot;

static lfn_fnode_slot *lfn_slots;

static lfn_fnode_slot *lfn_slot(UWORD handle)
{
  return lfn_slots != NULL && handle < LFN_FNODE_COUNT
             ? &lfn_slots[handle]
             : NULL;
}

static f_node_ptr lfn_get_fnode(UWORD handle)
{
  lfn_fnode_slot *slot = lfn_slot(handle);
  return slot != NULL && slot->used ? &slot->fnode : NULL;
}

static void lfn_init_dir_fnode(lfn_fnode_slot *slot, CLUSTER dirstart)
{
  f_node_ptr fnp = &slot->fnode;
  memset(&slot->dm, 0, sizeof(slot->dm));
  fnp->f_sft_idx = 0xff;
  fnp->f_dmp = dmatch_native(&slot->dm);
  fnp->f_offset = 0;
  fnp->f_cluster_offset = 0;

#ifdef WITHFAT32
  if (dirstart == 0 && fdos_dpb_is_fat32(fnp->f_dpb))
    dirstart = (CLUSTER)fdos_dpb_root_cluster(fnp->f_dpb);
#endif

  fnp->f_cluster = dirstart;
  DM_SET32(fnp->f_dmp, dm_dircluster, dirstart);
}

void lfnapi_init(void)
{
  if (lfn_slots != NULL)
    return;

  /* Persistent LFN state must be native, but it does not need to move the
   * native heap before PC/CPU allocation.  Allocate it lazily during DOS
   * initialization, after the emulator core objects already have stable SRAM
   * addresses. */
  lfn_slots = (lfn_fnode_slot *)malloc(sizeof(*lfn_slots) * LFN_FNODE_COUNT);
  if (lfn_slots != NULL)
    memset(lfn_slots, 0, sizeof(*lfn_slots) * LFN_FNODE_COUNT);
}

COUNT lfn_allocate_inode(void)
{
  dos_far_ptr dpbp = get_dpb(fdos_dos_default_drive());

  if (far_is_null(dpbp) || media_check(dpbp) < 0)
    return LHE_INVLDDRV;

  if (lfn_slots == NULL)
    return LHE_NOFREEHNDL;

  for (UWORD handle = 0; handle < LFN_FNODE_COUNT; handle++)
  {
    lfn_fnode_slot *slot = lfn_slot(handle);
    if (!slot->used)
    {
      memset(slot, 0, sizeof(*slot));
      slot->used = TRUE;
      slot->fnode.f_dpb = dpbp;
      slot->fnode.f_dmp = dmatch_native(&slot->dm);
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

static BOOL lfn_to_guest_unicode(dos_far_ptr inode, UWORD *name_index,
                                 const struct lfn_entry *lep)
{
  const UNICODE *parts[3] = {
    lep->lfn_name0_4, lep->lfn_name5_10, lep->lfn_name11_12
  };
  const UBYTE counts[3] = {5, 6, 2};
  BOOL found_zero = FALSE;

  for (UBYTE part = 0; part < 3; ++part)
  {
    for (UBYTE i = 0; i < counts[part]; ++i)
    {
      UNICODE ch = parts[part][i];
      fdos_lfn_name_set(inode, (*name_index)++,
                        found_zero ? UNICODE_FILLER : ch);
      if (ch == 0)
        found_zero = TRUE;
    }
    if (found_zero)
      return TRUE;
  }

  return FALSE;
}

static void guest_unicode_to_lfn(dos_far_ptr inode, UWORD start_index,
                                 struct lfn_entry *lep)
{
  UNICODE *parts[3] = {
    lep->lfn_name0_4, lep->lfn_name5_10, lep->lfn_name11_12
  };
  const UBYTE counts[3] = {5, 6, 2};
  BOOL found_zero = FALSE;
  UWORD index = start_index;

  for (UBYTE part = 0; part < 3; ++part)
  {
    for (UBYTE i = 0; i < counts[part]; ++i, ++index)
    {
      if (found_zero)
        parts[part][i] = UNICODE_FILLER;
      else
      {
        UNICODE ch = fdos_lfn_name_get(inode, index);
        parts[part][i] = ch;
        if (ch == 0)
          found_zero = TRUE;
      }
    }
  }
}

COUNT lfn_create_entries(UWORD handle, dos_far_ptr lip)
{
  f_node_ptr fnp = lfn_get_fnode(handle);
  lfn_fnode_slot *slot = lfn_slot(handle);
  COUNT entries_needed, free_entries, rc;
  UBYTE id = 1;
  UBYTE sfn_checksum;
  UWORD sfn_offset;

  if (fnp == NULL || far_is_null(lip))
    return LHE_INVLDHNDL;

  sfn_checksum = fdos_lfn_sfn_checksum(lip);
  entries_needed = (fdos_lfn_name_length(lip) + CHARS_IN_LFN_ENTRY - 1)
                 / CHARS_IN_LFN_ENTRY + 1;

  lfn_setup_inode(handle, DM_GET32(fnp->f_dmp, dm_dircluster), 0);
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
      if (DM_GET16(fnp->f_dmp, dm_entry) != 0)
        DM_SET16(fnp->f_dmp, dm_entry, (UWORD)(DM_GET16(fnp->f_dmp, dm_entry) - 1));
    }
    else
    {
      free_entries = 0;
    }

    DM_SET16(fnp->f_dmp, dm_entry, (UWORD)(DM_GET16(fnp->f_dmp, dm_entry) + 1));
  }

  sfn_offset = DM_GET16(fnp->f_dmp, dm_entry);
  fdos_lfn_dir_to_native(lip, &fnp->f_dir);
  if (!dir_write(fnp))
    return LHE_IOERROR;

  DM_SET16(fnp->f_dmp, dm_entry, (UWORD)(DM_GET16(fnp->f_dmp, dm_entry) - 1));
  for (COUNT i = 0; i < entries_needed - 1; i++, id++)
  {
    memset(&fnp->f_dir, 0, sizeof(fnp->f_dir));
    if (i == entries_needed - 2)
      id |= 0x40;

    guest_unicode_to_lfn(lip, (UWORD)(i * CHARS_IN_LFN_ENTRY), lfn(fnp));
    lfn(fnp)->lfn_checksum = sfn_checksum;
    lfn(fnp)->lfn_id = id;
    fnp->f_dir.dir_attrib = D_LFN;

    if (!dir_write(fnp))
      return LHE_IOERROR;

    if (DM_GET16(fnp->f_dmp, dm_entry) != 0)
      DM_SET16(fnp->f_dmp, dm_entry, (UWORD)(DM_GET16(fnp->f_dmp, dm_entry) - 1));
  }

  DM_SET16(fnp->f_dmp, dm_entry, sfn_offset);
  fdos_lfn_set_diroff(lip, sfn_offset);
  return SUCCESS;
}

COUNT lfn_dir_read(UWORD handle, dos_far_ptr lip)
{
  f_node_ptr fnp = lfn_get_fnode(handle);
  COUNT rc;
  UBYTE id = 1, real_id;
  UWORD lfn_name_index = 0;
  UWORD sfn_diroff;
  BOOL name_tail;

  if (fnp == NULL || far_is_null(lip))
    return LHE_INVLDHNDL;

  for (;;)
  {
    rc = dir_read(fnp);
    if (rc == DE_SEEK) return LHE_SEEK;
    if (rc == DE_BLKINVLD) return LHE_IOERROR;

    if (fnp->f_dir.dir_name[0] != DELETED &&
        fnp->f_dir.dir_attrib != D_LFN)
   {
      fdos_lfn_dir_from_native(lip, &fnp->f_dir);
      sfn_diroff = DM_GET16(fnp->f_dmp, dm_entry);
      break;
    }

    DM_SET16(fnp->f_dmp, dm_entry, (UWORD)(DM_GET16(fnp->f_dmp, dm_entry) + 1));
  }
  for (;;)
  {
    if (DM_GET16(fnp->f_dmp, dm_entry) == 0)
      break;

    DM_SET16(fnp->f_dmp, dm_entry, (UWORD)(DM_GET16(fnp->f_dmp, dm_entry) - 1));
    rc = dir_read(fnp);
    if (rc == DE_BLKINVLD) return LHE_IOERROR;
    if (fnp->f_dir.dir_name[0] == DELETED ||
        fnp->f_dir.dir_attrib != D_LFN)
      break;

    real_id = lfn(fnp)->lfn_id;
    if ((real_id & 0x3f) > 20)
      return LHE_DAMAGEDFS;

    name_tail = lfn_to_guest_unicode(lip, &lfn_name_index, lfn(fnp));
    if (real_id & 0x40)
    {
      if ((id | 0x40) != real_id)
        return LHE_DAMAGEDFS;
      break;
    }

    if (name_tail || real_id != id ||
        lfn(fnp)->lfn_checksum != fdos_lfn_sfn_checksum(lip))
      return LHE_DAMAGEDFS;

    id++;
  }

  fdos_lfn_name_set(lip, lfn_name_index, 0);
  DM_SET16(fnp->f_dmp, dm_entry, sfn_diroff);
  fdos_lfn_set_diroff(lip, sfn_diroff);
  return SUCCESS;
}

COUNT lfn_dir_write(UWORD handle)
{
  f_node_ptr fnp = lfn_get_fnode(handle);
  if (fnp == NULL)
    return LHE_INVLDHNDL;

  return dir_write(fnp) ? SUCCESS : LHE_IOERROR;
}
