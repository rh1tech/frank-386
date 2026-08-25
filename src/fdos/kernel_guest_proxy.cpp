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
using fdos_guest::psp_ref;
using fdos_guest::sft_ref;
using fdos_guest::sfttbl_ref;
using fdos_guest::dpb_ref;
using fdos_guest::dhdr_ref;

static const dos_data_ref idata(((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF);
static const lol_ref kernel_lol(((uint32_t)DOS_PSP << 4) + 0x08F0u);
}

extern "C" UWORD fdos_cds_flags(dos_far_ptr p) { return cds_ref(p).flags(); }
extern "C" dos_far_ptr fdos_cds_dpb(dos_far_ptr p) { return cds_ref(p).dpb(); }
extern "C" WORD fdos_cds_backslash_offset(dos_far_ptr p) { return cds_ref(p).backslash_offset(); }
extern "C" void fdos_cds_copy_current_path(dos_far_ptr p, char *dst, size_t n) { cds_ref(p).copy_current_path(dst, n); }
extern "C" void fdos_cds_current_path_byte(dos_far_ptr p, unsigned i, UBYTE v) { cds_ref(p).current_path_byte(i, v); }
extern "C" UBYTE fdos_dos_default_drive(void) { return idata.default_drive(); }
extern "C" UBYTE fdos_dos_lastdrive(void) { return kernel_lol.lastdrive(); }
extern "C" void fdos_lol_or_version_flags(UBYTE bits) { kernel_lol.version_flags() = (UBYTE)((UBYTE)kernel_lol.version_flags() | bits); }
extern "C" void fdos_dos_set_current_ldt(dos_far_ptr v) { idata.current_ldt(v); }

extern "C" UWORD fdos_dos_cu_psp(void) { return idata.cu_psp(); }
extern "C" UBYTE fdos_dos_mem_access_mode(void) { return idata.mem_access_mode(); }
extern "C" void fdos_dos_set_mem_access_mode(UBYTE v) { idata.mem_access_mode() = v; }
extern "C" UBYTE fdos_lol_uppermem_link(void) { return kernel_lol.uppermem_link(); }
extern "C" UWORD fdos_lol_uppermem_root(void) { return kernel_lol.uppermem_root(); }
extern "C" UWORD fdos_lol_first_mcb(void) { return kernel_lol.first_mcb(); }
extern "C" dos_far_ptr fdos_lol_sfthead(void) { return kernel_lol.sfthead(); }
extern "C" ULONG fdos_sft_size(dos_far_ptr p) { return sft_ref(p).size(); }
extern "C" UWORD fdos_sft_count(dos_far_ptr p) { return sft_ref(p).count(); }
extern "C" dos_far_ptr fdos_dos_lp_cur_sft(void) { return idata.lp_cur_sft(); }
extern "C" dos_far_ptr fdos_lol_dpb(void) { return kernel_lol.dpb(); }
extern "C" UBYTE fdos_lol_nblkdev(void) { return kernel_lol.nblkdev(); }
extern "C" dos_far_ptr fdos_dpb_next(dos_far_ptr p) { return dpb_ref(p).next(); }
extern "C" dos_far_ptr fdos_dpb_device(dos_far_ptr p) { return dpb_ref(p).device(); }
extern "C" UBYTE fdos_dpb_unit(dos_far_ptr p) { return dpb_ref(p).dpb_unit(); }
extern "C" UBYTE fdos_dpb_subunit(dos_far_ptr p) { return dpb_ref(p).dpb_subunit(); }
extern "C" BYTE fdos_dpb_flags(dos_far_ptr p) { return dpb_ref(p).flags(); }
extern "C" UBYTE fdos_dpb_mdb(dos_far_ptr p) { return dpb_ref(p).dpb_mdb(); }
extern "C" dos_far_ptr fdos_dhdr_next(dos_far_ptr p) { return dhdr_ref(p).next(); }
extern "C" UWORD fdos_dhdr_attr(dos_far_ptr p) { return dhdr_ref(p).attr(); }
extern "C" UWORD fdos_dhdr_strategy(dos_far_ptr p) { return dhdr_ref(p).strategy(); }
extern "C" UWORD fdos_dhdr_interrupt(dos_far_ptr p) { return dhdr_ref(p).interrupt(); }
extern "C" void fdos_dhdr_read_name(dos_far_ptr p, BYTE *dst) { dhdr_ref(p).read_name(dst); }

static inline uint32_t fdos_fixed_lol_linear(void)
{
    return ((uint32_t)DOS_PSP << 4) + 0x08f0u;
}

static inline uint32_t fdos_sda_linear(void)
{
    return ((uint32_t)DOS_PSP << 4) + X86_INTERNAL_DATA_OFF;
}

static inline dos_far_ptr fdos_proxy_far_load(uint32_t addr)
{
    return MK_FP(pload16(addr + 2u), pload16(addr));
}

static inline void fdos_proxy_far_store(uint32_t addr, dos_far_ptr p)
{
    pstore16(addr, FP_OFF(p));
    pstore16(addr + 2u, FP_SEG(p));
}

extern "C" UWORD fdos_dos_crit_err_code(void)
{
    return pload16(fdos_sda_linear() + offsetof(dos_data, CritErrCode));
}

extern "C" void fdos_dos_set_crit_err_code(UWORD value)
{
    pstore16(fdos_sda_linear() + offsetof(dos_data, CritErrCode), value);
}

extern "C" dos_far_ptr fdos_lol_nul_next(void)
{
    return fdos_proxy_far_load(fdos_fixed_lol_linear() + offsetof(lol, nul_dev) + offsetof(dhdr, dh_next));
}

extern "C" UBYTE fdos_lol_os_major(void)
{
    return pload8(fdos_fixed_lol_linear() + offsetof(lol, os_major));
}

extern "C" UBYTE fdos_lol_os_minor(void)
{
    return pload8(fdos_fixed_lol_linear() + offsetof(lol, os_minor));
}

extern "C" void fdos_lol_set_setver(UBYTE major, UBYTE minor)
{
    pstore8(fdos_fixed_lol_linear() + offsetof(lol, os_setver_major), major);
    pstore8(fdos_fixed_lol_linear() + offsetof(lol, os_setver_minor), minor);
}

extern "C" dos_far_ptr fdos_lol_syscon(void)
{
    return fdos_proxy_far_load(fdos_fixed_lol_linear() + offsetof(lol, syscon));
}

extern "C" dos_far_ptr fdos_cds_slot(unsigned drive)
{
    const UBYTE last = pload8(fdos_fixed_lol_linear() + offsetof(lol, lastdrive));
    const dos_far_ptr base = fdos_proxy_far_load(fdos_fixed_lol_linear() + offsetof(lol, CDSp));
    if (drive >= last || far_is_null(base))
        return MK_FP(0, 0);
    return MK_FP(FP_SEG(base),
                 (UWORD)(FP_OFF(base) + (UWORD)drive * sizeof(struct cds)));
}

extern "C" dos_far_ptr fdos_temp_cds_build(UBYTE drive_letter, unsigned drive)
{
    const dos_far_ptr source = fdos_cds_slot(drive);
    const uint32_t temp = fdos_sda_linear() + offsetof(dos_data, TempCDS);
    const dos_far_ptr temp_far = MK_FP(DOS_PSP,
        (UWORD)(X86_INTERNAL_DATA_OFF + offsetof(dos_data, TempCDS)));
    UWORD flags;
    dos_far_ptr dpb;
    unsigned i;

    if (far_is_null(source))
        return MK_FP(0, 0);

    for (i = 0; i < sizeof(struct cds); ++i)
        pstore8(temp + i, 0);
    pstore8(temp + offsetof(cds, cdsCurrentPath) + 0u, drive_letter);
    pstore8(temp + offsetof(cds, cdsCurrentPath) + 1u, ':');
    pstore8(temp + offsetof(cds, cdsCurrentPath) + 2u, '\\');
    pstore8(temp + offsetof(cds, cdsCurrentPath) + 3u, 0);
    pstore16(temp + offsetof(cds, cdsBackslashOffset), 2);

    flags = pload16(((uint32_t)FP_SEG(source) << 4) + FP_OFF(source) + offsetof(cds, cdsFlags));
    if (flags) {
        dpb = fdos_proxy_far_load(((uint32_t)FP_SEG(source) << 4) + FP_OFF(source) + offsetof(cds, cdsDpb));
        fdos_proxy_far_store(temp + offsetof(cds, cdsDpb), dpb);
        pstore16(temp + offsetof(cds, cdsFlags), CDSPHYSDRV);
    }

    pstore16(temp + offsetof(cds, cdsStrtClst), 0xffffu);
    pstore16(temp + offsetof(cds, cdsParam), 0xffffu);
    pstore16(temp + offsetof(cds, cdsStoreUData), 0xffffu);
    return temp_far;
}

extern "C" UWORD fdos_sft_dec_ref_raw(dos_far_ptr p)
{
    const uint32_t a = ((uint32_t)FP_SEG(p) << 4) + FP_OFF(p) + offsetof(sft, sft_count);
    const UWORD old = pload16(a);
    UWORD next = (UWORD)(old - 1u);
    if (next == 0)
        next = 0xffffu;
    pstore16(a, next);
    return old;
}

extern "C" UWORD fdos_sft_mode_raw(dos_far_ptr p)
{
    return pload16(((uint32_t)FP_SEG(p) << 4) + FP_OFF(p) + offsetof(sft, sft_mode));
}

extern "C" UWORD fdos_sft_flags_raw(dos_far_ptr p)
{
    return pload16(((uint32_t)FP_SEG(p) << 4) + FP_OFF(p) + offsetof(sft, sft_flags_union));
}

extern "C" dos_far_ptr fdos_sft_dev_raw(dos_far_ptr p)
{
    return fdos_proxy_far_load(((uint32_t)FP_SEG(p) << 4) + FP_OFF(p) + offsetof(sft, sft_dcb_or_dev));
}

extern "C" void fdos_sft_set_psp_raw(dos_far_ptr p, UWORD psp)
{
    pstore16(((uint32_t)FP_SEG(p) << 4) + FP_OFF(p) + offsetof(sft, sft_psp), psp);
}

extern "C" UWORD fdos_psp_max_files(UWORD psp_seg)
{
    return pload16(((uint32_t)psp_seg << 4) + offsetof(psp, ps_maxfiles));
}

extern "C" dos_far_ptr fdos_psp_file_table(UWORD psp_seg)
{
    return fdos_proxy_far_load(((uint32_t)psp_seg << 4) + offsetof(psp, ps_filetab));
}

static inline uint32_t fdos_psp_linear(UWORD psp_seg)
{
    return (uint32_t)psp_seg << 4;
}

extern "C" void fdos_psp_set_parent(UWORD psp_seg, UWORD parent)
{
    pstore16(fdos_psp_linear(psp_seg) + offsetof(psp, ps_parent), parent);
}

extern "C" void fdos_psp_set_prev(UWORD psp_seg, dos_far_ptr prev)
{
    fdos_proxy_far_store(fdos_psp_linear(psp_seg) + offsetof(psp, ps_prevpsp), prev);
}

extern "C" void fdos_psp_set_size(UWORD psp_seg, UWORD size)
{
    pstore16(fdos_psp_linear(psp_seg) + offsetof(psp, ps_size), size);
}

extern "C" void fdos_psp_set_max_files(UWORD psp_seg, UWORD count)
{
    psp_ref(psp_seg).max_files(count);
}

extern "C" void fdos_psp_set_file_table(UWORD psp_seg, dos_far_ptr table)
{
    psp_ref(psp_seg).file_table(table);
}

extern "C" void fdos_psp_set_environment(UWORD psp_seg, UWORD env_seg)
{
    pstore16(fdos_psp_linear(psp_seg) + offsetof(psp, ps_environ), env_seg);
}

extern "C" void fdos_psp_set_return_version(UWORD psp_seg, UWORD version)
{
    psp_ref(psp_seg).return_dos_version(version);
}

static inline size_t fdos_psp_vector_offset(unsigned which)
{
    switch (which) {
    case 0x22: return offsetof(psp, ps_isv22);
    case 0x23: return offsetof(psp, ps_isv23);
    default:   return offsetof(psp, ps_isv24);
    }
}

extern "C" void fdos_psp_set_vector(UWORD psp_seg, unsigned which, dos_far_ptr vector)
{
    fdos_proxy_far_store(fdos_psp_linear(psp_seg) + fdos_psp_vector_offset(which), vector);
}

extern "C" dos_far_ptr fdos_psp_vector(UWORD psp_seg, unsigned which)
{
    return fdos_proxy_far_load(fdos_psp_linear(psp_seg) + fdos_psp_vector_offset(which));
}

static inline size_t fdos_psp_fcb_offset(unsigned which)
{
    return which == 1 ? offsetof(psp, ps_fcb1) : offsetof(psp, ps_fcb2);
}

extern "C" void fdos_psp_set_fcb_drive(UWORD psp_seg, unsigned which, UBYTE drive)
{
    pstore8(fdos_psp_linear(psp_seg) + fdos_psp_fcb_offset(which) + offsetof(fcb, fcb_drive), drive);
}

extern "C" UBYTE fdos_psp_fcb_drive(UWORD psp_seg, unsigned which)
{
    return pload8(fdos_psp_linear(psp_seg) + fdos_psp_fcb_offset(which) + offsetof(fcb, fcb_drive));
}

extern "C" void fdos_psp_clear_fcb_name(UWORD psp_seg, unsigned which)
{
    guest_fill_block(fdos_psp_linear(psp_seg) + fdos_psp_fcb_offset(which) + offsetof(fcb, fcb_fname),
                     ' ', FNAME_SIZE + FEXT_SIZE);
}

extern "C" void fdos_psp_set_command_empty(UWORD psp_seg)
{
    const uint32_t base = fdos_psp_linear(psp_seg) + offsetof(psp, ps_cmd);
    pstore8(base + offsetof(CommandTail, ctCount), 0);
    pstore8(base + offsetof(CommandTail, ctBuffer), 0x0d);
}

extern "C" void fdos_psp_set_file_handle(UWORD psp_seg, UWORD index, UBYTE handle)
{
    psp_ref(psp_seg).file_handle(index, handle);
}

extern "C" void fdos_sft_inc_ref_raw(dos_far_ptr sft_ptr)
{
    const sft_ref r(sft_ptr);
    r.count((UWORD)(r.count() + 1u));
}

extern "C" void fdos_lol_set_network_retry(UWORD delay, UWORD retry)
{
    pstore16(fdos_fixed_lol_linear() + offsetof(lol, NetDelay), delay);
    pstore16(fdos_fixed_lol_linear() + offsetof(lol, NetRetry), retry);
}

extern "C" ULONG fdos_sft_position(dos_far_ptr p) { return sft_ref(p).position(); }
extern "C" void fdos_sft_set_size(dos_far_ptr p, ULONG v) { sft_ref(p).size(v); }
extern "C" UWORD fdos_sft_date(dos_far_ptr p) { return sft_ref(p).date(); }
extern "C" UWORD fdos_sft_time(dos_far_ptr p) { return sft_ref(p).time(); }
extern "C" void fdos_sft_or_mode(dos_far_ptr p, UWORD bits) { const sft_ref r(p); r.mode((UWORD)(r.mode() | bits)); }
extern "C" UWORD fdos_sft_psp(dos_far_ptr p) { return pload16(((uint32_t)FP_SEG(p) << 4) + FP_OFF(p) + offsetof(sft, sft_psp)); }
extern "C" UWORD fdos_sfttbl_count(dos_far_ptr p) { return sfttbl_ref(p).count(); }
extern "C" dos_far_ptr fdos_sfttbl_next(dos_far_ptr p) { return sfttbl_ref(p).next(); }
extern "C" dos_far_ptr fdos_sfttbl_entry(dos_far_ptr p, UWORD index) { return sfttbl_ref(p).entry(index); }
extern "C" BOOL fdos_dpb_is_fat32(dos_far_ptr p) { return dpb_ref(p).is_fat32() ? TRUE : FALSE; }
extern "C" ULONG fdos_dpb_root_cluster(dos_far_ptr p) {
#ifdef WITHFAT32
    return dpb_ref(p).dpb_xrootclst();
#else
    (void)p;
    return 0;
#endif
}
extern "C" ULONG fdos_dpb_xfatsize(dos_far_ptr p) {
#ifdef WITHFAT32
    return dpb_ref(p).dpb_xfatsize();
#else
    (void)p;
    return 0;
#endif
}
