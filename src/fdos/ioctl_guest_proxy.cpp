#include <cstddef>
#include <cstdint>

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

#include "ioctl_guest_proxy.h"

namespace {
static inline uint32_t far_linear(dos_far_ptr p)
{
    return (static_cast<uint32_t>(FP_SEG(p)) << 4) + FP_OFF(p);
}

static inline uint32_t sda_linear(void)
{
    return (static_cast<uint32_t>(DOS_PSP) << 4) + X86_INTERNAL_DATA_OFF;
}

static inline uint32_t lol_linear(void)
{
    return (static_cast<uint32_t>(DOS_PSP) << 4) + 0x08f0u;
}

static inline dos_far_ptr far_load(uint32_t addr)
{
    return MK_FP(pload16(addr + 2u), pload16(addr));
}
}

extern "C" void fdos_ioctl_set_network_retry(UWORD delay, UWORD retry, BOOL set_retry)
{
    pstore16(lol_linear() + offsetof(lol, NetDelay), delay);
    if (set_retry)
        pstore16(lol_linear() + offsetof(lol, NetRetry), retry);
}

extern "C" UBYTE fdos_ioctl_default_drive(void)
{
    return pload8(sda_linear() + offsetof(dos_data, default_drive));
}

extern "C" UWORD fdos_ioctl_sft_flags(dos_far_ptr p)
{
    return pload16(far_linear(p) + offsetof(sft, sft_flags_union));
}

extern "C" void fdos_ioctl_sft_set_flags_lo(dos_far_ptr p, UBYTE value)
{
    pstore8(far_linear(p) + offsetof(sft, sft_flags_union), value);
}

extern "C" dos_far_ptr fdos_ioctl_sft_dev(dos_far_ptr p)
{
    return far_load(far_linear(p) + offsetof(sft, sft_dcb_or_dev));
}

extern "C" ULONG fdos_ioctl_sft_position(dos_far_ptr p)
{
    return pload32(far_linear(p) + offsetof(sft, sft_posit));
}

extern "C" ULONG fdos_ioctl_sft_size(dos_far_ptr p)
{
    return pload32(far_linear(p) + offsetof(sft, sft_size));
}

extern "C" UBYTE fdos_ioctl_dpb_subunit(dos_far_ptr p)
{
    return pload8(far_linear(p) + offsetof(dpb, dpb_subunit));
}

extern "C" dos_far_ptr fdos_ioctl_dpb_device(dos_far_ptr p)
{
    return far_load(far_linear(p) + offsetof(dpb, dpb_device));
}

extern "C" BYTE fdos_ioctl_dpb_flags(dos_far_ptr p)
{
    return (BYTE)pload8(far_linear(p) + offsetof(dpb, dpb_flags));
}

extern "C" UWORD fdos_ioctl_dhdr_attr(dos_far_ptr p)
{
    return pload16(far_linear(p) + offsetof(dhdr, dh_attr));
}

extern "C" BOOL fdos_ioctl_cds_flags(unsigned drive, UWORD *flags)
{
    dos_far_ptr base;
    UBYTE last;

    if (drive == 0)
        drive = (unsigned)fdos_ioctl_default_drive() + 1u;
    if (drive == 0)
        return FALSE;
    --drive;

    last = pload8(lol_linear() + offsetof(lol, lastdrive));
    base = far_load(lol_linear() + offsetof(lol, CDSp));
    if (drive >= last || far_is_null(base))
        return FALSE;

    *flags = pload16(far_linear(base) + (uint32_t)drive * sizeof(cds) + offsetof(cds, cdsFlags));
    return TRUE;
}

extern "C" void fdos_ioctl_set_crit_err_code(UWORD value)
{
    pstore16(sda_linear() + offsetof(dos_data, CritErrCode), value);
}
