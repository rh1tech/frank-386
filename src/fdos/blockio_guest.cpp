#include <cstddef>
#include <cstdint>

#define new fdos_new
#ifndef _Static_assert
#define _Static_assert static_assert
#define FDOS_UNDEF_STATIC_ASSERT 1
#endif
extern "C" {
#include "hdrs.h"
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
#include "blockio_guest.h"

namespace {
constexpr fdos_guest::linear_t lol_linear =
    (static_cast<fdos_guest::linear_t>(DOS_PSP) << 4) + 0x08F0u;
const fdos_guest::lol_ref lol_state(lol_linear);
}

extern "C" dos_far_ptr fdos_buffer_first(void) { return lol_state.firstbuf(); }
extern "C" void fdos_buffer_first_set(dos_far_ptr p) { lol_state.firstbuf(p); }
extern "C" UWORD fdos_buffer_count(void) { return lol_state.nbuffers(); }
extern "C" dos_far_ptr fdos_buffer_search(ULONG blkno, COUNT dsk)
{
    using fdos_guest::buffer_ref;
    dos_far_ptr first = lol_state.firstbuf();
    const UWORD firstbp = FP_OFF(first);
    const UWORD nbuffers = lol_state.nbuffers();
    dos_far_ptr p = first;
    UWORD uncache_buf = 0;
    UWORD last_non_fat = 0;
    int fat_count = 0;
    unsigned guard = 0;

    auto move_to_front = [firstbp](dos_far_ptr bp) {
        const UWORD bp_off = FP_OFF(bp);
        const UWORD prev = buffer_ref(bp).prev();
        const UWORD next = buffer_ref(bp).next();
        const dos_far_ptr prev_bp = MK_FP(FP_SEG(bp), prev);
        const dos_far_ptr next_bp = MK_FP(FP_SEG(bp), next);
        const dos_far_ptr first_bp = MK_FP(FP_SEG(bp), firstbp);
        const UWORD first_prev = buffer_ref(first_bp).prev();
        buffer_ref(next_bp).prev(prev);
        buffer_ref(prev_bp).next(next);
        buffer_ref(bp).prev(first_prev);
        buffer_ref(bp).next(firstbp);
        buffer_ref(first_bp).prev(bp_off);
        buffer_ref(MK_FP(FP_SEG(bp), first_prev)).next(bp_off);
    };

    do {
        const buffer_ref b(p);
        const ULONG b_blkno = b.blkno();
        BYTE b_flag = b.flag();
        const BYTE b_unit = b.unit();
        if (b_blkno == blkno && (b_flag & BFR_VALID) && b_unit == dsk) {
            b.flag(static_cast<BYTE>(b_flag & ~BFR_UNCACHE));
            if (FP_OFF(p) != firstbp) {
                const UWORD off = FP_OFF(p);
                lol_state.firstbuf(MK_FP(FP_SEG(first), off));
                move_to_front(p);
            }
            return p;
        }
        if (b_flag & BFR_UNCACHE) uncache_buf = FP_OFF(p);
        if (b_flag & BFR_FAT) ++fat_count; else last_non_fat = FP_OFF(p);
        const UWORD next = b.next();
        if (next == 0xffff || guard >= nbuffers) return MK_FP(0xffff, 0xffff);
        p = MK_FP(FP_SEG(p), next);
        ++guard;
    } while (FP_OFF(p) != firstbp);

    if (uncache_buf) p = MK_FP(FP_SEG(p), uncache_buf);
    else if ((buffer_ref(p).flag() & BFR_FAT) && fat_count < 3 && last_non_fat)
        p = MK_FP(FP_SEG(p), last_non_fat);
    else p = MK_FP(FP_SEG(p), buffer_ref(first).prev());

    buffer_ref(p).flag(static_cast<BYTE>(buffer_ref(p).flag() | BFR_UNCACHE));
    if (FP_OFF(p) != firstbp) {
        const UWORD off = FP_OFF(p);
        move_to_front(p);
        lol_state.firstbuf(MK_FP(FP_SEG(p), off));
    }
    return p;
}
namespace {

static inline dos_far_ptr buffer_data_far(dos_far_ptr p)
{
    return MK_FP(FP_SEG(p),
                 static_cast<UWORD>(FP_OFF(p) + offsetof(struct buffer, b_buffer)));
}

static BOOL buffer_flush_impl(dos_far_ptr p)
{
    using fdos_guest::buffer_ref;
    using fdos_guest::dpb_ref;

    const buffer_ref b(p);
    BOOL ok = TRUE;
    BYTE flag = b.flag();
    if ((flag & (BFR_VALID | BFR_DIRTY)) == (BFR_VALID | BFR_DIRTY)) {
        ULONG b_offset = 0;
        UBYTE b_copies = 1;
        ULONG blkno = b.blkno();
        const BYTE unit = b.unit();

        if (flag & BFR_FAT) {
            b_copies = b.copies();
            b_offset = b.offset();
#ifdef WITHFAT32
            if (b_offset == 0) {
                const dos_far_ptr dpbp = b.dpbp();
                if (far_is_null(dpbp) || far_is_end(dpbp)) {
                    b_copies = 1;
                } else {
                    b_offset = dpb_ref(dpbp).dpb_xfatsize();
                }
            }
#endif
        }

        while (b_copies--) {
            if (dskxfer(unit, blkno, buffer_data_far(p), 1, DSKWRITE))
                ok = FALSE;
            blkno += b_offset;
        }
    }

    flag = b.flag();
    flag = static_cast<BYTE>(flag & ~BFR_DIRTY);
    if (!ok)
        flag = static_cast<BYTE>(flag & ~BFR_VALID);
    b.flag(flag);
    return ok;
}

} // namespace

extern "C" BOOL fdos_buffer_flush(dos_far_ptr p)
{
    return buffer_flush_impl(p);
}

extern "C" dos_far_ptr fdos_buffer_getblk(ULONG blkno, COUNT dsk, BOOL overwrite)
{
    using fdos_guest::buffer_ref;

    const dos_far_ptr p = fdos_buffer_search(blkno, dsk);
    if (far_is_end(p))
        return p;

    const buffer_ref b(p);
    const BYTE flag = b.flag();
    if (!(flag & BFR_UNCACHE)) {
#ifdef FDOS_BUFFER_NOCACHE
        if (((flag & BFR_DIRTY) || overwrite)
#ifdef FDOS_BUFFER_NOCACHE_UNIT
            || dsk != (FDOS_BUFFER_NOCACHE_UNIT)
#endif
           )
            return p;
#else
        return p;
#endif
    }

    if (!buffer_flush_impl(p))
        return MK_FP(0, 0);

    if (!overwrite && dskxfer(dsk, blkno, buffer_data_far(p), 1, DSKREAD))
        return MK_FP(0, 0);

    b.flag(BFR_VALID | BFR_DATA);
    b.unit(static_cast<BYTE>(dsk));
    b.blkno(blkno);
    return p;
}

extern "C" UWORD fdos_buffer_next(dos_far_ptr p) { return fdos_guest::buffer_ref(p).next(); }
extern "C" UWORD fdos_buffer_prev(dos_far_ptr p) { return fdos_guest::buffer_ref(p).prev(); }
extern "C" void fdos_buffer_next_set(dos_far_ptr p, UWORD v) { fdos_guest::buffer_ref(p).next(v); }
extern "C" void fdos_buffer_prev_set(dos_far_ptr p, UWORD v) { fdos_guest::buffer_ref(p).prev(v); }
extern "C" BYTE fdos_buffer_unit(dos_far_ptr p) { return fdos_guest::buffer_ref(p).unit(); }
extern "C" void fdos_buffer_unit_set(dos_far_ptr p, BYTE v) { fdos_guest::buffer_ref(p).unit(v); }
extern "C" BYTE fdos_buffer_flag(dos_far_ptr p) { return fdos_guest::buffer_ref(p).flag(); }
extern "C" void fdos_buffer_flag_set(dos_far_ptr p, BYTE v) { fdos_guest::buffer_ref(p).flag(v); }
extern "C" ULONG fdos_buffer_blkno(dos_far_ptr p) { return fdos_guest::buffer_ref(p).blkno(); }
extern "C" void fdos_buffer_blkno_set(dos_far_ptr p, ULONG v) { fdos_guest::buffer_ref(p).blkno(v); }
extern "C" UBYTE fdos_buffer_copies(dos_far_ptr p) { return fdos_guest::buffer_ref(p).copies(); }
extern "C" UWORD fdos_buffer_offset(dos_far_ptr p) { return fdos_guest::buffer_ref(p).offset(); }
extern "C" dos_far_ptr fdos_buffer_dpbp(dos_far_ptr p) { return fdos_guest::buffer_ref(p).dpbp(); }
