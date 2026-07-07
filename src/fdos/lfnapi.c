#include "hdrs.h"
#include "fdos.h"

#ifndef WITHLFNAPI
#error lfnapi.c must be compiled only when WITHLFNAPI is enabled
#endif

#ifndef LFN_INODE_COUNT
#define LFN_INODE_COUNT 16
#endif

typedef struct lfn_inode_stub {
  BOOL used;
  ULONG dir_cluster;
  ULONG dir_offset;
  struct lfn_inode data;
} lfn_inode_stub;

static lfn_inode_stub lfn_inodes[LFN_INODE_COUNT];

COUNT lfn_allocate_inode(void)
{
  for (COUNT i = 0; i < LFN_INODE_COUNT; i++)
  {
    if (!lfn_inodes[i].used)
    {
      memset(&lfn_inodes[i], 0, sizeof(lfn_inodes[i]));
      lfn_inodes[i].used = TRUE;
      return i;
    }
  }

  return DE_TOOMANY;
}

COUNT lfn_free_inode(UWORD inode)
{
  if (inode >= LFN_INODE_COUNT || !lfn_inodes[inode].used)
    return DE_INVLDPARM;

  memset(&lfn_inodes[inode], 0, sizeof(lfn_inodes[inode]));
  return SUCCESS;
}

COUNT lfn_setup_inode(UWORD inode, ULONG dir_cluster, ULONG dir_offset)
{
  if (inode >= LFN_INODE_COUNT || !lfn_inodes[inode].used)
    return DE_INVLDPARM;

  lfn_inodes[inode].dir_cluster = dir_cluster;
  lfn_inodes[inode].dir_offset = dir_offset;
  return SUCCESS;
}

COUNT lfn_create_entries(UWORD inode, lfn_inode_ptr out)
{
  if (inode >= LFN_INODE_COUNT || !lfn_inodes[inode].used || out == NULL)
    return DE_INVLDPARM;

  memcpy(out, &lfn_inodes[inode].data, sizeof(lfn_inodes[inode].data));
  return SUCCESS;
}

COUNT lfn_dir_read(UWORD inode, lfn_inode_ptr out)
{
  if (inode >= LFN_INODE_COUNT || !lfn_inodes[inode].used || out == NULL)
    return DE_INVLDPARM;

  memcpy(out, &lfn_inodes[inode].data, sizeof(lfn_inodes[inode].data));
  return SUCCESS;
}

COUNT lfn_dir_write(UWORD inode)
{
  if (inode >= LFN_INODE_COUNT || !lfn_inodes[inode].used)
    return DE_INVLDPARM;

  return SUCCESS;
}
