#include <cstddef>

#define new fdos_new
#define _Static_assert static_assert
extern "C" {
#include "hdrs.h"
}
#undef _Static_assert
#undef new
#ifdef load
#undef load
#endif

#include "guest_ref.hpp"
#include "kernel_guest_proxy.h"

namespace {
using fdos_guest::cds_ref;
using fdos_guest::dos_data_ref;
using fdos_guest::lol_ref;

static const dos_data_ref idata(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF);
static const lol_ref kernel_lol(((uint32_t)DOS_PSP << 4) + 0x08F0u);
}

extern "C" UWORD fdos_cds_flags(dos_far_ptr p) { return cds_ref(p).flags(); }
extern "C" dos_far_ptr fdos_cds_dpb(dos_far_ptr p) { return cds_ref(p).dpb(); }
extern "C" WORD fdos_cds_backslash_offset(dos_far_ptr p) { return cds_ref(p).backslash_offset(); }
extern "C" WORD fdos_cds_join_offset(dos_far_ptr p) { return cds_ref(p).join_offset(); }
extern "C" void fdos_cds_copy_current_path(dos_far_ptr p, char *dst, size_t n) { cds_ref(p).copy_current_path(dst, n); }
extern "C" void fdos_cds_current_path_byte(dos_far_ptr p, unsigned i, UBYTE v) { cds_ref(p).current_path_byte(i, v); }
extern "C" UBYTE fdos_dos_default_drive(void) { return idata.default_drive(); }
extern "C" UBYTE fdos_dos_lastdrive(void) { return kernel_lol.lastdrive(); }
extern "C" void fdos_dos_set_current_ldt(dos_far_ptr v) { idata.current_ldt(v); }
