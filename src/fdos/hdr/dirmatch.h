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

/* The guest SDA reserves 21 bytes for each temporary directory-match
   prefix.  That is exactly dm_drive..reserved2; result fields beginning
   at dm_attr_fnd live elsewhere.  Keep this ABI checked explicitly
   because dmatch_handle accesses the SDA prefix by guest address rather
   than by a native dmatch pointer. */
_Static_assert(offsetof(dmatch, dm_attr_fnd) == 21,
                "dmatch's dm_drive..reserved2 prefix no longer fits the 21-byte SDA temporary match fields, see lol.h");
_Static_assert(sizeof(dmatch) == 43, "sizeof(dmatch) changed - re-check directory-match ABI and copy sites");

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

/* SDA search state is guest-resident.  Native aliases are intentionally
   omitted; use dmatch_handle and guest field/block accessors. */
