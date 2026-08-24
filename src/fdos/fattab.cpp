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

using fdos_guest::dpb_ref;

#define printf(...) dos_printf(__VA_ARGS__)

/*
    -----------------------------------------------------------------
    FAT table layer (migrated from fattab.c)
    -----------------------------------------------------------------

    FAT reads and writes keep guest addresses only.  DPB metadata is
    accessed through dpb_ref and FAT cache buffers through buffer_ref,
    so no native pointer returned by the paging/cache layer survives a
    call which may remap guest RAM.
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
STATIC dos_far_ptr getFATblock(dos_far_ptr /* -> struct dpb */ x86_dpbp,
                               CLUSTER clussec)
{
  using fdos_guest::buffer_ref;
  using fdos_guest::dpb_ref;
  const dpb_ref d(x86_dpbp);
  const dos_far_ptr x86_bp = getblock(clussec, d.dpb_unit());

  if (!far_is_null(x86_bp))
  {
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
    return x86_bp;
  }

  clusterMessage("I/O: 0x", clussec);
  return MK_FP(0, 0);
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
    out = getFATblock(x86_dpbp, sector);
    return !far_is_null(out);
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
  using fdos_guest::buffer_ref;
  using fdos_guest::dpb_ref;

  if ((unsigned)Cluster2 == READ_CLUSTER)
    return read_fat_guest(x86_dpbp, Cluster1);

  const dpb_ref d(x86_dpbp);
  const UWORD dpb_size = d.dpb_size();
  const UWORD secsize = d.dpb_secsize();
  const UWORD fatstrt = d.dpb_fatstrt();
  const UWORD fatsize = d.dpb_fatsize();
  const bool fat12 = (dpb_size - 1u) < FAT_MAGIC;
  const bool fat16 = !fat12 && dpb_size <= FAT_MAGIC16;
#ifdef WITHFAT32
  const bool fat32 = fatsize == 0;
#else
  const bool fat32 = false;
#endif
  unsigned idx;
  unsigned secdiv; /* FAT entries per sector; nibbles for FAT12! */
  unsigned char wasfree;
  CLUSTER clussec = Cluster1;
  CLUSTER max_cluster = dpb_size;

#ifdef WITHFAT32
  if (fat32)
    max_cluster = d.dpb_xsize();
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

  secdiv = secsize;
  if (fat12)
  {
    clussec = (unsigned)clussec * 3;
    secdiv *= 2;
  }
  else /* FAT16 or FAT32 */
  {
    secdiv /= 2;
#ifdef WITHFAT32
    if (fat32)
      secdiv /= 2;
#endif
  }

  /* idx is a pointer to an index which is the nibble offset of the FAT
     entry within the sector for FAT12, or word offset for FAT16, or
     dword offset for FAT32 */
  idx = (unsigned)(clussec % secdiv);
  clussec /= secdiv;
  clussec += fatstrt;
#ifdef WITHFAT32
  if (fat32)
  {
    const UWORD xflags = d.dpb_xflags();
    if (xflags & FAT_NO_MIRRORING)
    {
      /* we must modify the active fat,
         its number is in the 0-3 bits of dpb_xflags */
      clussec += (xflags & 0xf) * d.dpb_xfatsize();
    }
  }
#endif

  /* Get the block that this cluster is in. Keep only its guest address:
     getblock()/getFATblock() can remap pageable guest RAM. */
  const dos_far_ptr x86_bp = getFATblock(x86_dpbp, clussec);
  if (far_is_null(x86_bp))
    return 1; /* the only error code possible here */
  const buffer_ref bp(x86_bp);

  if (fat12)
  {
    unsigned cluster, cluster2;

    /* form an index so that we can read the block as a byte array */
    idx /= 2;

    UBYTE lo = bp.data8(idx);
    UBYTE hi;
    dos_far_ptr x86_bp1 = MK_FP(0, 0);

    if (idx >= (unsigned)secsize - 1)
    {
      /* blockio.c LRU logic ensures that bp != bp1 */
      x86_bp1 = getFATblock(x86_dpbp, (unsigned)clussec + 1);
      if (far_is_null(x86_bp1))
        return 1; /* the only error code possible here */

      const buffer_ref bp1(x86_bp1);
      bp1.flag(static_cast<BYTE>(bp1.flag() | BFR_DIRTY | BFR_VALID));
      hi = bp1.data8(0);
    }
    else
    {
      hi = bp.data8(idx + 1u);
    }

    cluster = (unsigned)lo | ((unsigned)hi << 8);
    {
      unsigned res = cluster;

      /* Now to unpack the contents of the FAT entry. Odd and
         even bytes are packed differently. */
      if (Cluster1 & 0x01)
        cluster >>= 4;
      cluster &= 0x0fff;

      wasfree = (cluster == FREE);
      cluster = res;
    }

    /* Cluster2 may be set to LONG_LAST_CLUSTER == 0x0FFFFFFFUL or 0xFFFF
       -- please don't remove this mask! */
    cluster2 = (unsigned)Cluster2 & 0x0fff;

    /* Now pack the value in. */
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
    bp.data8(idx, (UBYTE)cluster);
    if (far_is_null(x86_bp1))
      bp.data8(idx + 1u, (UBYTE)(cluster >> 8));
    else
      buffer_ref(x86_bp1).data8(0, (UBYTE)(cluster >> 8));
  }
  else if (fat16)
  {
    const UWORD res = bp.data16(idx * 2u);
    bp.data16(idx * 2u, (UWORD)Cluster2);
    wasfree = (res == FREE);
  }
#ifdef WITHFAT32
  else if (fat32)
  {
    const ULONG res = bp.data32(idx * 4u) & LONG_LAST_CLUSTER;
    bp.data32(idx * 4u, Cluster2 & LONG_LAST_CLUSTER);
    wasfree = (res == FREE);
  }
#endif
  else
  {
    printf("Bad DPB!\n"); /* FAT1x size field > 65525U (see fat.h) */
    return 1;
  }

  /* update the free space count */
  bp.flag(static_cast<BYTE>(bp.flag() | BFR_DIRTY | BFR_VALID));
  if (Cluster2 == FREE || wasfree)
  {
    int adjust = 0;
    if (!wasfree)
      adjust = 1;
    else if (Cluster2 != FREE)
      adjust = -1;
#ifdef WITHFAT32
    if (fat32)
    {
      const ULONG nfree = d.xnfree();
      if (nfree != XUNKNCLSTFREE)
      {
        d.xnfree((ULONG)(nfree + adjust));
        write_fsinfo(x86_dpbp);
      }
    }
    else
#endif
    {
      const UWORD nfree = d.nfree();
      if (nfree != UNKNCLSTFREE)
        d.nfree((UWORD)(nfree + adjust));
    }
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
void read_fsinfo(dos_far_ptr x86_dpbp)
{
  const fdos_guest::dpb_ref d(x86_dpbp);
  const UWORD sec = d.dpb_xfsinfosec();
  if (sec == 0xffff)
    return;

  const dos_far_ptr x86_bp = getblock(sec, d.dpb_unit());
  if (far_is_null(x86_bp))
    return;

  const fdos_guest::buffer_ref b(x86_bp);
  BYTE flag = b.flag();
  flag &= (BYTE)~(BFR_DATA | BFR_DIR | BFR_FAT | BFR_DIRTY);
  flag |= BFR_VALID;
  b.flag(flag);

  CLUSTER cluster = b.data32(0x1e4u + offsetof(struct fsinfo, fi_nfreeclst));
  const ULONG max_cluster = d.dpb_xsize();
  if (cluster >= max_cluster)
    cluster = XUNKNCLSTFREE;
  d.xnfree(cluster);

  cluster = b.data32(0x1e4u + offsetof(struct fsinfo, fi_cluster));
  if (cluster < 2 || cluster > max_cluster)
    cluster = UNKNCLUSTER;
  d.dpb_xcluster(cluster);
}

void write_fsinfo(dos_far_ptr x86_dpbp)
{
  const dpb_ref d(x86_dpbp);
  const UWORD sec = d.dpb_xfsinfosec();
  if (sec == 0xffff)
    return;

  const dos_far_ptr x86_bp = getblock(sec, d.dpb_unit());
  /* FSInfo is a hint - skip the update when the sector cannot be cached. */
  if (far_is_null(x86_bp))
    return;

  const fdos_guest::buffer_ref b(x86_bp);
  BYTE flag = b.flag();
  flag &= (BYTE)~(BFR_DATA | BFR_DIR | BFR_FAT);
  flag |= BFR_VALID;

  const ULONG nfree = d.xnfree();
  const ULONG cluster = d.dpb_xcluster();
  const std::size_t off = 0x1e4u;
  if (b.data32(off + offsetof(struct fsinfo, fi_nfreeclst)) != nfree ||
      b.data32(off + offsetof(struct fsinfo, fi_cluster)) != cluster)
    flag |= BFR_DIRTY;
  b.flag(flag);
  b.data32(off + offsetof(struct fsinfo, fi_nfreeclst), nfree);
  b.data32(off + offsetof(struct fsinfo, fi_cluster), cluster);
}
#endif
