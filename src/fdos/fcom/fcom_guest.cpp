/*
 * Offset-based guest-memory helpers for native FreeCOM.
 *
 * A value of type uint32_t is always a guest linear address here.  No host
 * pointer returned by the paging cache is retained across another access.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>

#define new fdos_new
#define strchr fdos_strchr_compat
#define _Static_assert static_assert
extern "C" {
#include "fdos/hdrs.h"
}
#undef _Static_assert
#undef strchr
#undef new
#ifdef load
#undef load
#endif

#include "mem.h"
#include "fdos/guest_ref.hpp"
#include "fdos/fcom/fcom_guest.h"

using fdos_guest::mcb_ref;
using fdos_guest::dos_data_ref;
using fdos_guest::lol_ref;

extern "C" {

uint32_t fcom_guest_linear(uint16_t segment, uint16_t offset)
{
    return (static_cast<uint32_t>(segment) << 4) + offset;
}

uint8_t fcom_guest_read8(uint32_t addr)
{
    return pload8(addr);
}

uint16_t fcom_guest_read16(uint32_t addr)
{
    return pload16(addr);
}

void fcom_guest_write16(uint32_t addr, uint16_t value)
{
    pstore16(addr, value);
}

void fcom_guest_read(uint32_t addr, void *dst, size_t len)
{
    auto *out = static_cast<uint8_t *>(dst);
    while (len--) {
        *out++ = pload8(addr++);
    }
}

void fcom_guest_write(uint32_t addr, const void *src, size_t len)
{
    const auto *in = static_cast<const uint8_t *>(src);
    while (len--) {
        pstore8(addr++, *in++);
    }
}

void fcom_guest_fill(uint32_t addr, uint8_t value, size_t len)
{
    while (len--)
        pstore8(addr++, value);
}

void fcom_guest_copy(uint32_t dst, uint32_t src, size_t len)
{
    /*
     * memmove semantics.  Environment replacement normally copies between
     * distinct allocations, but keeping overlap correct makes this a general
     * guest-memory primitive and avoids ever materialising a host pointer.
     */
    if (dst == src || len == 0)
        return;

    if (dst < src || dst >= src + len) {
        while (len--) {
            pstore8(dst++, pload8(src++));
        }
    } else {
        dst += static_cast<uint32_t>(len);
        src += static_cast<uint32_t>(len);
        while (len--) {
            pstore8(--dst, pload8(--src));
        }
    }
}

size_t fcom_guest_strnlen(uint32_t addr, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && pload8(addr + static_cast<uint32_t>(n)) != 0)
        ++n;
    return n;
}

int fcom_guest_env_name_matches(uint32_t entry, size_t entry_len,
                                const char *name, size_t name_len)
{
    if (entry_len <= name_len)
        return 0;
    if (pload8(entry + static_cast<uint32_t>(name_len)) != '=')
        return 0;

    for (size_t i = 0; i < name_len; ++i) {
        const unsigned char a =
            static_cast<unsigned char>(pload8(entry + static_cast<uint32_t>(i)));
        const unsigned char b = static_cast<unsigned char>(name[i]);

        /* Environment variable names are ASCII case-insensitive. */
        const unsigned char au = (a >= 'a' && a <= 'z') ? a - ('a' - 'A') : a;
        const unsigned char bu = (b >= 'a' && b <= 'z') ? b - ('a' - 'A') : b;
        if (au != bu)
            return 0;
    }
    return 1;
}

uint16_t fcom_guest_psp_environment(uint16_t psp_seg)
{
    const uint32_t base = static_cast<uint32_t>(psp_seg) << 4;
    return pload16(base + static_cast<uint32_t>(offsetof(psp, ps_environ)));
}

void fcom_guest_psp_set_environment(uint16_t psp_seg, uint16_t env_seg)
{
    const uint32_t base = static_cast<uint32_t>(psp_seg) << 4;
    pstore16(base + static_cast<uint32_t>(offsetof(psp, ps_environ)), env_seg);
}

uint16_t fcom_guest_mcb_owner(uint16_t mcb_seg)
{
    return mcb_ref(static_cast<seg>(mcb_seg)).psp();
}

uint16_t fcom_guest_mcb_size(uint16_t mcb_seg)
{
    return mcb_ref(static_cast<seg>(mcb_seg)).size();
}

uint16_t fcom_guest_current_psp(void)
{
    constexpr uint32_t idata_linear =
        (static_cast<uint32_t>(DOS_PSP) << 4) + X86_INTERNAL_DATA_OFF;
    return static_cast<uint16_t>(dos_data_ref(idata_linear).cu_psp());
}

uint8_t fcom_guest_lol_uppermem_link(void)
{
    constexpr uint32_t lol_linear =
        (static_cast<uint32_t>(DOS_PSP) << 4) + 0x08F0u;
    return lol_ref(lol_linear).uppermem_link();
}

} /* extern "C" */
