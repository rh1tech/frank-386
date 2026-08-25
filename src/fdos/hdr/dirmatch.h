/****************************************************************/
/*                                                              */
/*                          dirmatch.h                          */
/*                                                              */
/*               FAT File System Match Data Structure           */
/*                                                              */
/*                       January 4, 1992                        */
/*                                                              */
/*                      Copyright (c) 1995                      */
/*                      Pasquale J. Villani                     */
/*                      All Rights Reserved                     */
/*                                                              */
/* This file is part of DOS-C.                                  */
/*                                                              */
/* DOS-C is free software; you can redistribute it and/or       */
/* modify it under the terms of the GNU General Public License  */
/* as published by the Free Software Foundation; either version */
/* 2, or (at your option) any later version.                    */
/*                                                              */
/* DOS-C is distributed in the hope that it will be useful, but */
/* WITHOUT ANY WARRANTY; without even the implied warranty of   */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See    */
/* the GNU General Public License for more details.             */
/*                                                              */
/* You should have received a copy of the GNU General Public    */
/* License along with DOS-C; see the file COPYING.  If not,     */
/* write to the Free Software Foundation, 675 Mass Ave,         */
/* Cambridge, MA 02139, USA.                                    */
/****************************************************************/

#ifdef MAIN
#ifdef VERSION_STRINGS
static BYTE *dirmatch_hRcsId =
    "$Id: dirmatch.h 1415 2009-06-02 13:18:24Z bartoldeman $";
#endif
#endif

#pragma pack(push, 1)
typedef struct {
  UBYTE dm_drive;
  BYTE dm_name_pat[FNAME_SIZE + FEXT_SIZE];
  UBYTE dm_attr_srch;
  UWORD dm_entry;
  CLUSTER dm_dircluster;
#ifndef WITHFAT32
  UWORD reserved;
#endif
  UWORD reserved2;

  UBYTE dm_attr_fnd;            /* found file attribute         */
  dtime dm_time;                /* file time                    */
  ddate dm_date;                /* file date                    */
  ULONG dm_size;                /* file size                    */
  BYTE dm_name[FNAME_SIZE + FEXT_SIZE + 2];     /* file name    */
} dmatch;
#pragma pack(pop)

/* see the comment on sda_tmp_dmD/sda_tmp_dm_renD below: the built-in
   21-byte SDA field only ever needs to hold dm_drive..reserved2 -
   this assert exists so a future change that grows that prefix is
   caught at compile time instead of silently corrupting whatever SDA
   field follows sda_tmp_dm/sda_tmp_dm_ren in struct dos_data. */
_Static_assert(offsetof(dmatch, dm_attr_fnd) == 21,
                "dmatch's dm_drive..reserved2 prefix no longer fits in the 21-byte sda_tmp_dm/sda_tmp_dm_ren SDA fields, see lol.h");
_Static_assert(sizeof(dmatch) == 43, "sizeof(dmatch) changed - re-check every fmemcpy()/sizeof(dmatch) use site");

/*
    sda_tmp_dm/sda_tmp_dm_ren are SDA fields in the original (extern
    ASM, see kernel.asm: _sda_tmp_dm/_sda_tmp_dm_ren), reserved there
    as a fixed 21-byte block each (see lol.h for the matching
    BYTE[21] fields here) - NOT sizeof(dmatch) (43 bytes with
    WITHFAT32 active, same as without it - CLUSTER growing from 2 to
    4 bytes exactly offsets the "reserved" field that only exists
    when WITHFAT32 is off).

    This is not a bug to fix: 21 bytes is exactly dm_drive + dm_name_pat
    + dm_attr_srch + dm_entry + dm_dircluster + reserved2, i.e. every
    field dir_open()/dir_init_fnode()/map_cluster() (the code that
    actually walks fnp->f_dmp) touches. The remaining fields
    (dm_attr_fnd/dm_time/dm_date/dm_size/dm_name) are only filled in
    later by dos_findfirst()/dos_findnext() - and in the original,
    *not* through sda_tmp_dm at all: see dosfns.c, which fills a
    separate "SearchDir" variable and copies its fields into the
    caller's dmatch one at a time, never writing dm_attr_fnd et al.
    through &sda_tmp_dm directly. So as long as a dmatch* aliasing
    this 21-byte field is only ever used the same way (struct fields
    up to and including reserved2), nothing past the reserved region
    is ever touched, exactly as in the original.

    Named with a trailing D, like IoReqHdrD/MediaReqHdrD in device.h,
    so the macro doesn't expand recursively into itself wherever
    internal_data->sda_tmp_dm/sda_tmp_dm_ren is written.
*/


/* A directory match can live either in guest SDA memory or in native
 * kernel-only LFN state.  Keep that distinction explicit: a raw dmatch *
 * must never be manufactured for guest memory when paging is active. */
typedef struct dmatch_handle {
  UBYTE is_native;
  union {
    dmatch *native_ptr;
    dos_far_ptr guest_ptr;
  } u;
} dmatch_handle;

static inline dmatch_handle dmatch_native(dmatch *p)
{
  dmatch_handle h;
  h.is_native = TRUE;
  h.u.native_ptr = p;
  return h;
}

static inline dmatch_handle dmatch_guest(dos_far_ptr p)
{
  dmatch_handle h;
  h.is_native = FALSE;
  h.u.guest_ptr = p;
  return h;
}

static inline uint32_t dmatch_guest_addr(dmatch_handle h, size_t off)
{
  return EFFECTIVE(h.u.guest_ptr) + (uint32_t)off;
}

static inline UBYTE dmatch_get8(dmatch_handle h, size_t off)
{
  return h.is_native ? *((UBYTE *)h.u.native_ptr + off)
                     : pload8(dmatch_guest_addr(h, off));
}

static inline UWORD dmatch_get16(dmatch_handle h, size_t off)
{
  UWORD v;
  if (!h.is_native)
    return pload16(dmatch_guest_addr(h, off));
  dos_api_memcpy(&v, (UBYTE *)h.u.native_ptr + off, sizeof(v));
  return v;
}

static inline ULONG dmatch_get32(dmatch_handle h, size_t off)
{
  ULONG v;
  if (!h.is_native)
    return pload32(dmatch_guest_addr(h, off));
  dos_api_memcpy(&v, (UBYTE *)h.u.native_ptr + off, sizeof(v));
  return v;
}

static inline void dmatch_set8(dmatch_handle h, size_t off, UBYTE v)
{
  if (h.is_native)
    *((UBYTE *)h.u.native_ptr + off) = v;
  else
    pstore8(dmatch_guest_addr(h, off), v);
}

static inline void dmatch_set16(dmatch_handle h, size_t off, UWORD v)
{
  if (h.is_native)
    dos_api_memcpy((UBYTE *)h.u.native_ptr + off, &v, sizeof(v));
  else
    pstore16(dmatch_guest_addr(h, off), v);
}

static inline void dmatch_set32(dmatch_handle h, size_t off, ULONG v)
{
  if (h.is_native)
    dos_api_memcpy((UBYTE *)h.u.native_ptr + off, &v, sizeof(v));
  else
    pstore32(dmatch_guest_addr(h, off), v);
}

#define DM_GET8(h, field) dmatch_get8((h), offsetof(dmatch, field))
#define DM_GET16(h, field) dmatch_get16((h), offsetof(dmatch, field))
#define DM_GET32(h, field) dmatch_get32((h), offsetof(dmatch, field))
#define DM_SET8(h, field, v) dmatch_set8((h), offsetof(dmatch, field), (v))
#define DM_SET16(h, field, v) dmatch_set16((h), offsetof(dmatch, field), (v))
#define DM_SET32(h, field, v) dmatch_set32((h), offsetof(dmatch, field), (v))

static inline void dmatch_read_name_pat(dmatch_handle h, BYTE out[FNAME_SIZE + FEXT_SIZE])
{
  const size_t off = offsetof(dmatch, dm_name_pat);
  if (h.is_native)
    dos_api_memcpy(out, (BYTE *)h.u.native_ptr + off, FNAME_SIZE + FEXT_SIZE);
  else
    guest_read_block(EFFECTIVE(h.u.guest_ptr) + (uint32_t)off, out, FNAME_SIZE + FEXT_SIZE);
}

static inline void dmatch_write_name_pat(dmatch_handle h, const BYTE in[FNAME_SIZE + FEXT_SIZE])
{
  const size_t off = offsetof(dmatch, dm_name_pat);
  if (h.is_native)
    dos_api_memcpy((BYTE *)h.u.native_ptr + off, in, FNAME_SIZE + FEXT_SIZE);
  else
    guest_write_block(EFFECTIVE(h.u.guest_ptr) + (uint32_t)off,
                      in, FNAME_SIZE + FEXT_SIZE);
}

#define sda_tmp_dmD (*(dmatch *)&internal_data->sda_tmp_dm)
#define sda_tmp_dm_renD (*(dmatch *)&internal_data->sda_tmp_dm_ren)

#define SearchDirD (*(struct dirent *)&internal_data->SearchDir)
#define SAttrD (internal_data->SAttr)
