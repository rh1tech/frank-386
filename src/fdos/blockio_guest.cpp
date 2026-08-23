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
