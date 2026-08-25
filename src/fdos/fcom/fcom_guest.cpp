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
#ifndef _Static_assert
#define _Static_assert static_assert
#define FDOS_LOCAL_STATIC_ASSERT_MACRO 1
#endif
extern "C" {
#include "fdos/hdrs.h"
}
#ifdef FDOS_LOCAL_STATIC_ASSERT_MACRO
#undef _Static_assert
#undef FDOS_LOCAL_STATIC_ASSERT_MACRO
#endif
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

static constexpr uint32_t fcom_idata_linear =
    (static_cast<uint32_t>(DOS_PSP) << 4) + X86_INTERNAL_DATA_OFF;
static constexpr uint32_t fcom_lol_linear =
    (static_cast<uint32_t>(DOS_PSP) << 4) + 0x08F0u;


namespace {
constexpr unsigned FCOM_SHADOW_MAX_DEPTH = 8;
struct shadow_state {
    uint8_t *storage;
    uint32_t base;
    size_t size;
};

static uint8_t *shadow_storage;
static uint32_t shadow_base;
static size_t shadow_size;
static unsigned shadow_depth;
static shadow_state shadow_stack[FCOM_SHADOW_MAX_DEPTH];

static bool shadow_range(uint32_t addr, size_t len, size_t *off)
{
    if (shadow_depth == 0 || shadow_storage == nullptr)
        return false;
    if (addr < shadow_base)
        return false;
    const uint64_t delta = static_cast<uint64_t>(addr) - shadow_base;
    if (delta > shadow_size || len > shadow_size - static_cast<size_t>(delta))
        return false;
    if (off)
        *off = static_cast<size_t>(delta);
    return true;
}

static void raw_read(uint32_t addr, void *dst, size_t len)
{
    guest_read_block(addr, dst, len);
}

static void raw_write(uint32_t addr, const void *src, size_t len)
{
    guest_write_block(addr, src, len);
}
} // namespace

extern "C" {

uint32_t fcom_guest_linear(uint16_t segment, uint16_t offset)
{
    return (static_cast<uint32_t>(segment) << 4) + offset;
}

uint8_t fcom_guest_read8(uint32_t addr)
{
    size_t off;
    if (shadow_range(addr, 1, &off))
        return shadow_storage[off];
    return pload8(addr);
}

void fcom_guest_write8(uint32_t addr, uint8_t value)
{
    size_t off;
    if (shadow_range(addr, 1, &off)) {
        shadow_storage[off] = value;
        return;
    }
    pstore8(addr, value);
}

uint16_t fcom_guest_read16(uint32_t addr)
{
    return static_cast<uint16_t>(fcom_guest_read8(addr)) |
           (static_cast<uint16_t>(fcom_guest_read8(addr + 1u)) << 8);
}

void fcom_guest_write16(uint32_t addr, uint16_t value)
{
    fcom_guest_write8(addr, static_cast<uint8_t>(value));
    fcom_guest_write8(addr + 1u, static_cast<uint8_t>(value >> 8));
}

void fcom_guest_read(uint32_t addr, void *dst, size_t len)
{
    size_t off;
    if (shadow_range(addr, len, &off)) {
        std::memcpy(dst, shadow_storage + off, len);
        return;
    }
    raw_read(addr, dst, len);
}

void fcom_guest_write(uint32_t addr, const void *src, size_t len)
{
    size_t off;
    if (shadow_range(addr, len, &off)) {
        std::memcpy(shadow_storage + off, src, len);
        return;
    }
    raw_write(addr, src, len);
}

void fcom_guest_fill(uint32_t addr, uint8_t value, size_t len)
{
    size_t off;
    if (shadow_range(addr, len, &off)) {
        std::memset(shadow_storage + off, value, len);
        return;
    }
    guest_fill_block(addr, value, len);
}

void fcom_guest_copy(uint32_t dst, uint32_t src, size_t len)
{
    if (dst == src || len == 0)
        return;
    if (dst < src || dst >= src + len) {
        for (size_t i = 0; i < len; ++i)
            fcom_guest_write8(dst + static_cast<uint32_t>(i),
                              fcom_guest_read8(src + static_cast<uint32_t>(i)));
    } else {
        while (len-- != 0)
            fcom_guest_write8(dst + static_cast<uint32_t>(len),
                              fcom_guest_read8(src + static_cast<uint32_t>(len)));
    }
}

int fcom_guest_shadow_enter(uint32_t base, void *storage, size_t size)
{
    if (storage == nullptr || size == 0 || shadow_depth >= FCOM_SHADOW_MAX_DEPTH)
        return 0;
    if (shadow_depth != 0) {
        raw_write(shadow_base, shadow_storage, shadow_size);
        shadow_stack[shadow_depth - 1] = {shadow_storage, shadow_base, shadow_size};
    }
    shadow_storage = static_cast<uint8_t *>(storage);
    shadow_base = base;
    shadow_size = size;
    ++shadow_depth;
    raw_read(shadow_base, shadow_storage, shadow_size);
    return 1;
}

void fcom_guest_shadow_leave(void)
{
    if (shadow_depth == 0)
        return;
    raw_write(shadow_base, shadow_storage, shadow_size);
    --shadow_depth;
    if (shadow_depth != 0) {
        const shadow_state prev = shadow_stack[shadow_depth - 1];
        shadow_storage = prev.storage;
        shadow_base = prev.base;
        shadow_size = prev.size;
        raw_read(shadow_base, shadow_storage, shadow_size);
    } else {
        shadow_base = 0;
        shadow_size = 0;
        shadow_storage = nullptr;
    }
}

void fcom_guest_shadow_sync_to_guest(void)
{
    if (shadow_depth != 0)
        raw_write(shadow_base, shadow_storage, shadow_size);
}

void fcom_guest_shadow_sync_from_guest(void)
{
    if (shadow_depth != 0)
        raw_read(shadow_base, shadow_storage, shadow_size);
}

void *fcom_guest_shadow_ptr(uint32_t addr, size_t len)
{
    size_t off;
    return shadow_range(addr, len, &off) ? shadow_storage + off : nullptr;
}

size_t fcom_guest_strnlen(uint32_t addr, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && fcom_guest_read8(addr + static_cast<uint32_t>(n)) != 0)
        ++n;
    return n;
}

int fcom_guest_env_name_matches(uint32_t entry, size_t entry_len,
                                const char *name, size_t name_len)
{
    if (entry_len <= name_len)
        return 0;
    if (fcom_guest_read8(entry + static_cast<uint32_t>(name_len)) != '=')
        return 0;

    for (size_t i = 0; i < name_len; ++i) {
        const unsigned char a =
            static_cast<unsigned char>(fcom_guest_read8(entry + static_cast<uint32_t>(i)));
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

extern "C" uint16_t fcom_guest_lol_first_mcb(void)
{
    return lol_ref(fcom_lol_linear).first_mcb();
}

extern "C" uint8_t fcom_guest_mem_access_mode(void)
{
    return dos_data_ref(fcom_idata_linear).mem_access_mode();
}

extern "C" void fcom_guest_set_mem_access_mode(uint8_t value)
{
    dos_data_ref(fcom_idata_linear).mem_access_mode() = value;
}
