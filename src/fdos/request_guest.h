#pragma once

/* C-compatible thin proxy for DOS request packets stored in guest RAM.
   Include after hdrs.h (or equivalent headers defining request/dos_data). */

typedef struct fdos_request_guest_ref {
    uint32_t linear;
} fdos_request_guest_ref;

static inline fdos_request_guest_ref fdos_request_guest(dos_far_ptr p)
{
    fdos_request_guest_ref r = {
        ((uint32_t)FP_SEG(p) << 4) + FP_OFF(p)
    };
    return r;
}

static inline dos_far_ptr fdos_sda_request_far(size_t field_off)
{
    return MK_FP(DOS_PSP, (UWORD)(X86_INTERNAL_DATA_OFF + field_off));
}

static inline UBYTE fdos_request_get8(fdos_request_guest_ref r, size_t off)
{
    return pload8(r.linear + (uint32_t)off);
}

static inline UWORD fdos_request_get16(fdos_request_guest_ref r, size_t off)
{
    return pload16(r.linear + (uint32_t)off);
}

static inline ULONG fdos_request_get32(fdos_request_guest_ref r, size_t off)
{
    return pload32(r.linear + (uint32_t)off);
}

static inline dos_far_ptr fdos_request_get_far(fdos_request_guest_ref r, size_t off)
{
    const UWORD ofs = pload16(r.linear + (uint32_t)off);
    const UWORD seg = pload16(r.linear + (uint32_t)off + sizeof(UWORD));
    return MK_FP(seg, ofs);
}

static inline void fdos_request_set8(fdos_request_guest_ref r, size_t off, UBYTE v)
{
    pstore8(r.linear + (uint32_t)off, v);
}

static inline void fdos_request_set16(fdos_request_guest_ref r, size_t off, UWORD v)
{
    pstore16(r.linear + (uint32_t)off, v);
}

static inline void fdos_request_set32(fdos_request_guest_ref r, size_t off, ULONG v)
{
    pstore32(r.linear + (uint32_t)off, v);
}

static inline void fdos_request_set_far(fdos_request_guest_ref r, size_t off, dos_far_ptr v)
{
    pstore16(r.linear + (uint32_t)off, FP_OFF(v));
    pstore16(r.linear + (uint32_t)off + sizeof(UWORD), FP_SEG(v));
}

#define FDOS_REQUEST_GET8(r, field)       fdos_request_get8((r), offsetof(request, field))
#define FDOS_REQUEST_GET16(r, field)      fdos_request_get16((r), offsetof(request, field))
#define FDOS_REQUEST_GET32(r, field)      fdos_request_get32((r), offsetof(request, field))
#define FDOS_REQUEST_GET_FAR(r, field)    fdos_request_get_far((r), offsetof(request, field))
#define FDOS_REQUEST_SET8(r, field, v)    fdos_request_set8((r), offsetof(request, field), (UBYTE)(v))
#define FDOS_REQUEST_SET16(r, field, v)   fdos_request_set16((r), offsetof(request, field), (UWORD)(v))
#define FDOS_REQUEST_SET32(r, field, v)   fdos_request_set32((r), offsetof(request, field), (ULONG)(v))
#define FDOS_REQUEST_SET_FAR(r, field, v) fdos_request_set_far((r), offsetof(request, field), (v))
