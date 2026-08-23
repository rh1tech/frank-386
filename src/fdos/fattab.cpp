#include <pico.h>
#include <pico/time.h>
#include <hardware/pio.h>
#include <ctype.h>
#define new fdos_new
#ifndef _Static_assert
#define _Static_assert static_assert
#define FDOS_UNDEF_STATIC_ASSERT 1
#endif
extern "C" {
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
#ifdef FDOS_UNDEF_STATIC_ASSERT
#undef _Static_assert
#undef FDOS_UNDEF_STATIC_ASSERT
#endif
#undef new
#ifdef load
#undef load
#endif

#include "guest_ref.hpp"

#define printf(...) dos_printf(__VA_ARGS__)

/*
    -----------------------------------------------------------------
    FAT table layer (migrated from fattab.c)
    -----------------------------------------------------------------

    Only the read-only path (next_cluster()/is_free_cluster(), via
    link_fat() with Cluster2==READ_CLUSTER) is migrated in this
    iteration - link_fat()'s write path (allocating/freeing FAT
    entries) is kept since next_cluster() calls the same function,
    but nothing in this codebase calls link_fat() to write yet, and
    dpb_xnfreeclst/dpb_nfreeclst free-space-count bookkeeping is
    inactive until something does.
*/

/* special "impossible" "Cluster2" value of 1 denotes reading the
   cluster number rather than overwriting it */
#define READ_CLUSTER 1

STATIC void clusterMessage(const char *msg, CLUSTER clussec)
{
  put_string("Run chkdsk: Bad FAT ");
  put_string(msg);
#ifdef WITHFAT32
  put_unsigned((unsigned)(clussec >> 16), 16, 4);
#endif
  put_unsigned((unsigned)(clussec & 0xffffu), 16, 4);
  put_console('\n');
}

/*
    getFATblock(dpbp, clussec) - fetch the buffer holding FAT sector
    "clussec" (relative to the start of the active FAT), marking it
    as a FAT buffer so flush1() (see above) knows to write it back to
    every FAT copy (dpb_fats of them, dpb_fatsize sectors apart) when
    it's dirty.

    Migrated from fattab.c.
*/
STATIC struct buffer *getFATblock(dos_far_ptr /* -> struct dpb */ x86_dpbp,
                                  CLUSTER clussec)
{
  using fdos_guest::buffer_ref;
  using fdos_guest::dpb_ref;
  const dpb_ref d(x86_dpbp);
  struct buffer *native_bp = getblock(clussec, d.dpb_unit());

  if (native_bp)
  {
    /* Capture the guest address before touching the DPB again. */
    const dos_far_ptr x86_bp = linear_to_far(native_bp);
    const buffer_ref b(x86_bp);
    BYTE flag = b.flag();
    flag = static_cast<BYTE>((flag & ~(BFR_DATA | BFR_DIR)) |
                             BFR_FAT | BFR_VALID);
    b.flag(flag);
    b.dpbp(x86_dpbp);
    b.copies(d.dpb_fats());
    b.offset(d.dpb_fatsize());
#ifdef WITHFAT32
    if (d.dpb_fatsize() == 0 && (d.dpb_xflags() & FAT_NO_MIRRORING))
      b.copies(1);
#endif
    return (struct buffer *)ARM_PTR(x86_bp);
  }

  clusterMessage("I/O: 0x", clussec);
  return NULL;
}

static CLUSTER read_fat_guest(dos_far_ptr x86_dpbp, CLUSTER cluster1)
{
  using namespace fdos_guest;
  const dpb_ref d(x86_dpbp);
#ifdef WITHFAT32
  const bool fat32 = d.dpb_fatsize() == 0;
#else
  const bool fat32 = false;
#endif
  const UWORD dpb_size = d.dpb_size();
  const bool fat12 = (dpb_size - 1u) < FAT_MAGIC;
  const bool fat16 = !fat12 && dpb_size <= FAT_MAGIC16;
  CLUSTER max_cluster = dpb_size;
#ifdef WITHFAT32
  if (fat32)
    max_cluster = d.dpb_xsize();
#endif
  if (cluster1 <= 1 || cluster1 > max_cluster)
    return 1;

  unsigned secdiv = d.dpb_secsize();
  CLUSTER clussec = cluster1;
  if (fat12) {
    clussec = static_cast<CLUSTER>(static_cast<unsigned>(clussec) * 3u);
    secdiv *= 2u;
  } else {
    secdiv /= 2u;
#ifdef WITHFAT32
    if (fat32)
      secdiv /= 2u;
#endif
  }

  const unsigned idx = static_cast<unsigned>(clussec % secdiv);
  clussec /= secdiv;
  clussec += d.dpb_fatstrt();
#ifdef WITHFAT32
  if (fat32) {
    const UWORD xflags = d.dpb_xflags();
    if (xflags & FAT_NO_MIRRORING)
      clussec += static_cast<CLUSTER>(xflags & 0x0fu) * d.dpb_xfatsize();
  }
#endif

  auto get_fat_block = [&](CLUSTER sector, dos_far_ptr &out) -> bool {
    struct buffer *native_bp = getFATblock(x86_dpbp, sector);
    if (native_bp == nullptr)
      return false;
    /* Capture the guest address before any further guest access. */
    out = linear_to_far(native_bp);
    return true;
  };

  dos_far_ptr x86_bp{};
  if (!get_fat_block(clussec, x86_bp))
    return 1;
  const buffer_ref b(x86_bp);

  if (fat12) {
    const unsigned byte_index = idx / 2u;
    const UBYTE lo = b.data8(byte_index);
    UBYTE hi;
    if (byte_index >= static_cast<unsigned>(d.dpb_secsize()) - 1u) {
      dos_far_ptr x86_bp1{};
      if (!get_fat_block(clussec + 1u, x86_bp1))
        return 1;
      hi = buffer_ref(x86_bp1).data8(0);
    } else {
      hi = b.data8(byte_index + 1u);
    }
    unsigned cluster = static_cast<unsigned>(lo) | (static_cast<unsigned>(hi) << 8);
    if (cluster1 & 1u)
      cluster >>= 4;
    cluster &= 0x0fffu;
    if (cluster >= MASK12)
      return LONG_LAST_CLUSTER;
    if (cluster == BAD12)
      return LONG_BAD;
    return cluster;
  }

  if (fat16) {
    const UWORD res = b.data16(idx * 2u);
    if (res >= MASK16)
      return LONG_LAST_CLUSTER;
    if (res == BAD16)
      return LONG_BAD;
    return res;
  }

#ifdef WITHFAT32
  if (fat32) {
    const ULONG res = b.data32(idx * 4u) & LONG_LAST_CLUSTER;
    if (res > LONG_BAD)
      return LONG_LAST_CLUSTER;
    return res;
  }
#endif
  return 1;
}

/* either read the value at Cluster1 (if Cluster2 is READ_CLUSTER) */
/* or write the Cluster2 value to the FAT entry at Cluster1        */
/* Read is always via next_cluster wrapper which has extra checks  */
/* It might make sense to manually check old values before a write */
/* returns: the cluster number (or 1 on error) for read mode       */
/* returns: SUCCESS (or 1 on error) for write mode                 */
/*
    Migrated from fattab.c verbatim (aside from native-pointer
    adjustments noted throughout this file).
*/
CLUSTER link_fat(dos_far_ptr /* -> struct dpb */ x86_dpbp, CLUSTER Cluster1,
                 REG CLUSTER Cluster2)
{
  if ((unsigned)Cluster2 == READ_CLUSTER)
    return read_fat_guest(x86_dpbp, Cluster1);

  struct dpb *dpbp = (struct dpb *)ARM_PTR(x86_dpbp);
  struct buffer *bp;
  unsigned idx;
  unsigned secdiv; /* FAT entries per sector; nibbles for FAT12! */
  unsigned char wasfree;
  CLUSTER clussec = Cluster1;
  CLUSTER max_cluster = dpbp->dpb_size;

#ifdef WITHFAT32
  if (ISFAT32(dpbp))
    max_cluster = dpbp->dpb_xsize;
#endif

  if (clussec <= 1 || clussec > max_cluster) /* try to read out of range? */
  {
    clusterMessage("index: 0x", clussec); /* bad array offset */
    return 1;
  }

  /* Cluster2 can 0 (FREE) or 1 (READ_CLUSTER), a cluster nr. >= 2, */
  /* (range check this case!) LONG_LAST_CLUSTER or LONG_BAD here... */
  if (Cluster2 < LONG_BAD && Cluster2 > max_cluster) /* writing bad value? */
  {
    clusterMessage("write: 0x", Cluster2); /* refuse to write bad value */
    return 1;
  }

  secdiv = dpbp->dpb_secsize;
  if (ISFAT12(dpbp))
  {
    clussec = (unsigned)clussec * 3;
    secdiv *= 2;
  }
  else /* FAT16 or FAT32 */
  {
    secdiv /= 2;
#ifdef WITHFAT32
    if (ISFAT32(dpbp))
      secdiv /= 2;
#endif
  }

  /* idx is a pointer to an index which is the nibble offset of the FAT
     entry within the sector for FAT12, or word offset for FAT16, or
     dword offset for FAT32 */
  idx = (unsigned)(clussec % secdiv);
  clussec /= secdiv;
  clussec += dpbp->dpb_fatstrt;
#ifdef WITHFAT32
  if (ISFAT32(dpbp) && (dpbp->dpb_xflags & FAT_NO_MIRRORING))
  {
    /* we must modify the active fat,
       it's number is in the 0-3 bits of dpb_xflags */
    clussec += (dpbp->dpb_xflags & 0xf) * dpbp->dpb_xfatsize;
  }
#endif

  /* Get the block that this cluster is in                */
  bp = getFATblock(x86_dpbp, clussec);

  if (bp == NULL)
  {
    return 1; /* the only error code possible here */
  }

  if (ISFAT12(dpbp))
  {
    REG UBYTE *fbp0;
    REG UBYTE *fbp1;
    struct buffer *bp1;
    unsigned cluster, cluster2;

    /* form an index so that we can read the block as a     */
    /* byte array                                           */
    idx /= 2;

    /* Test to see if the cluster straddles the block. If   */
    /* it does, get the next block and use both to form the */
    /* the FAT word. Otherwise, just point to the next      */
    /* block.                                               */
    fbp0 = &bp->b_buffer[idx];

    /* pointer to next byte, will be overwritten, if not valid */
    fbp1 = fbp0 + 1;

    if (idx >= (unsigned)dpbp->dpb_secsize - 1)
    {
      /* blockio.c LRU logic ensures that bp != bp1 */
      bp1 = getFATblock(x86_dpbp, (unsigned)clussec + 1);
      if (bp1 == 0)
        return 1; /* the only error code possible here */

      if (Cluster2 != READ_CLUSTER)
        bp1->b_flag |= BFR_DIRTY | BFR_VALID;

      fbp1 = &bp1->b_buffer[0];
    }

    cluster = *fbp0 | (*fbp1 << 8);
    {
      unsigned res = cluster;

      /* Now to unpack the contents of the FAT entry. Odd and */
      /* even bytes are packed differently.                   */

      if (Cluster1 & 0x01)
        cluster >>= 4;
      cluster &= 0x0fff;

      if ((unsigned)Cluster2 == READ_CLUSTER)
      {
        if (cluster >= MASK12)
          return LONG_LAST_CLUSTER;
        if (cluster == BAD12)
          return LONG_BAD;
        return cluster;
      }

      wasfree = 0;
      if (cluster == FREE)
        wasfree = 1;

      cluster = res;
    }

    /* Cluster2 may be set to LONG_LAST_CLUSTER == 0x0FFFFFFFUL or 0xFFFF */
    /* -- please don't remove this mask!                                  */
    cluster2 = (unsigned)Cluster2 & 0x0fff;

    /* Now pack the value in                                */
    if ((unsigned)Cluster1 & 0x01)
    {
      cluster &= 0x000f;
      cluster2 <<= 4;
    }
    else
    {
      cluster &= 0xf000;
    }
    cluster |= cluster2;
    *fbp0 = (UBYTE)cluster;
    *fbp1 = (UBYTE)(cluster >> 8);
  }
  else if (ISFAT16(dpbp))
  {
    /* form an index so that we can read the block as a     */
    /* byte array                                           */
    /* and get the cluster number                           */
    UWORD res = fgetword(&bp->b_buffer[idx * 2]);
    if ((unsigned)Cluster2 == READ_CLUSTER)
    {
      if (res >= MASK16)
        return LONG_LAST_CLUSTER;
      if (res == BAD16)
        return LONG_BAD;

      return res;
    }
    /* Finally, put the word into the buffer and mark the   */
    /* buffer as dirty.                                     */
    fputword(&bp->b_buffer[idx * 2], (UWORD)Cluster2);
    wasfree = 0;
    if (res == FREE)
      wasfree = 1;
  }
#ifdef WITHFAT32
  else if (ISFAT32(dpbp))
  {
    /* form an index so that we can read the block as a     */
    /* byte array                                           */
    ULONG res = fgetlong(&bp->b_buffer[idx * 4]) & LONG_LAST_CLUSTER;
    if (Cluster2 == READ_CLUSTER)
    {
      if (res > LONG_BAD)
        return LONG_LAST_CLUSTER;

      return res;
    }
    /* Finally, put the word into the buffer and mark the   */
    /* buffer as dirty.                                     */
    fputlong(&bp->b_buffer[idx * 4], Cluster2 & LONG_LAST_CLUSTER);
    wasfree = 0;
    if (res == FREE)
      wasfree = 1;
  }
#endif
  else
  {
    printf("Bad DPB!\n"); /* FAT1x size field > 65525U (see fat.h) */
    return 1;
  }

  /* update the free space count                          */
  bp->b_flag |= BFR_DIRTY | BFR_VALID;
  if (Cluster2 == FREE || wasfree)
  {
    int adjust = 0;
    if (!wasfree)
      adjust = 1;
    else if (Cluster2 != FREE)
      adjust = -1;
#ifdef WITHFAT32
    if (ISFAT32(dpbp) && dpbp->dpb_xnfreeclst != XUNKNCLSTFREE)
    {
      /* update the free space count for returned     */
      /* cluster                                      */
      dpbp->dpb_xnfreeclst += adjust;
      write_fsinfo(dpbp);
    }
    else
#endif
    if (dpbp->dpb_nfreeclst != UNKNCLSTFREE)
      dpbp->dpb_nfreeclst += adjust;
  }
  return SUCCESS;
}


/* Given the disk parameters, and a cluster number, this function */
/* looks at the FAT, and returns the next cluster in the clain or */
/* 0 if there is no chain, 1 on error, LONG_LAST_CLUSTER at end.  */
/*
    Migrated from fattab.c verbatim.
*/
CLUSTER next_cluster(dos_far_ptr /* -> struct dpb */ x86_dpbp, CLUSTER ClusterNum)
{
  CLUSTER candidate, following, max_cluster;
  candidate = link_fat(x86_dpbp, ClusterNum, READ_CLUSTER);
  /* empty (0) error (1) bad (LONG_BAD) last (>LONG_BAD) need no checks */
  if (candidate < 2 || candidate >= LONG_BAD)
    return candidate;
  {
    const fdos_guest::dpb_ref d(x86_dpbp);
#ifdef WITHFAT32
    max_cluster = d.dpb_fatsize() == 0 ? d.dpb_xsize() : d.dpb_size();
#else
    max_cluster = d.dpb_size();
#endif
  }
  /* FAT entry points to a possibly invalid next cluster */
  following = link_fat(x86_dpbp, candidate, READ_CLUSTER);
  if (following < 2 || (following < LONG_BAD && following > max_cluster))
  {
    /* chain must not contain free or out of range clusters */
    clusterMessage("value: 0x", following); /* read returned bad value */
    return 1; /* only possible error code here */
  }
  /* without checking "following", a chain can dangle to a free cluster: */
  /* if that cluster is later used by another chain, you get cross links */
  return candidate;
}

/* check if the selected cluster is free (faster than next_cluster) */
BOOL is_free_cluster(dos_far_ptr /* -> struct dpb */ x86_dpbp, CLUSTER ClusterNum)
{
  /* link_fat() now takes the far pointer directly, so no native view needed. */
  return (link_fat(x86_dpbp, ClusterNum, READ_CLUSTER) == FREE);
}

#ifdef WITHFAT32
void read_fsinfo(struct dpb FAR * dpbp)
{
  struct buffer FAR *bp;
  struct fsinfo FAR *fip;
  CLUSTER cluster;

  if (dpbp->dpb_xfsinfosec == 0xffff)
    return;

  bp = getblock(dpbp->dpb_xfsinfosec, dpbp->dpb_unit);
  /* Upstream omits this check; there a NULL far pointer is survivable, here
     bp is a native pointer and dereferencing NULL faults the core. The FSInfo
     sector is only a free-space HINT, so a failed read is not fatal: leave the
     cached counts alone and carry on. */
  if (bp == NULL)
    return;
  bp->b_flag &= ~(BFR_DATA | BFR_DIR | BFR_FAT | BFR_DIRTY);
  bp->b_flag |= BFR_VALID;

  fip = (struct fsinfo FAR *)&bp->b_buffer[0x1e4];
  /* need to range check values because they may not be correct */
  cluster = fip->fi_nfreeclst;
  if (cluster >= dpbp->dpb_xsize)
    cluster = XUNKNCLSTFREE;
  dpbp->dpb_xnfreeclst = cluster;
  cluster = fip->fi_cluster;
  if (cluster < 2 || cluster > dpbp->dpb_xsize)
    cluster = UNKNCLUSTER;
  dpbp->dpb_xcluster = cluster;
}

void write_fsinfo(struct dpb FAR * dpbp)
{
  struct buffer FAR *bp;
  struct fsinfo FAR *fip;

  if (dpbp->dpb_xfsinfosec == 0xffff)
    return;

  bp = getblock(dpbp->dpb_xfsinfosec, dpbp->dpb_unit);
  /* Same as read_fsinfo(): NULL here is a native null-pointer dereference.
     FSInfo is a hint - skip the update rather than fault. */
  if (bp == NULL)
    return;
  bp->b_flag &= ~(BFR_DATA | BFR_DIR | BFR_FAT);
  bp->b_flag |= BFR_VALID;

  fip = (struct fsinfo FAR *)&bp->b_buffer[0x1e4];

  if (fip->fi_nfreeclst != dpbp->dpb_xnfreeclst ||
    fip->fi_cluster != dpbp->dpb_xcluster)
    bp->b_flag |= BFR_DIRTY; /* only flag for update if we had real news */

  fip->fi_nfreeclst = dpbp->dpb_xnfreeclst;
  fip->fi_cluster = dpbp->dpb_xcluster;
}
#endif
