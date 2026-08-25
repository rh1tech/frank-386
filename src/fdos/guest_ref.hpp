#pragma once

#include <cstddef>
#include <cstdint>

#include "mem.h"

namespace fdos_guest {

template <typename T>
struct __attribute__((packed, may_alias)) unaligned_value {
    T value;
};

template <typename T>
__attribute__((always_inline)) static inline T unaligned_load(const void *p)
{
    return reinterpret_cast<const unaligned_value<T> *>(p)->value;
}

template <typename T>
__attribute__((always_inline)) static inline void unaligned_store(void *p, T value)
{
    reinterpret_cast<unaligned_value<T> *>(p)->value = value;
}

using linear_t = uint32_t;

template <typename T>
class ref_base {
protected:
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    explicit constexpr ref_base(linear_t addr) : addr_(addr) {}
#else
    explicit ref_base(linear_t addr)
        : addr_(addr), native_(X86_RAM_BASE + addr) {}
#endif

    template <typename V>
    __attribute__((always_inline)) V scalar_load(std::size_t off) const {
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
        const uint32_t a = addr_ + static_cast<uint32_t>(off);
        if constexpr (sizeof(V) == 1)
            return static_cast<V>(pload8(a));
        else if constexpr (sizeof(V) == 2)
            return static_cast<V>(pload16(a));
        else {
            static_assert(sizeof(V) == 4, "guest scalar must be 1, 2 or 4 bytes");
            return static_cast<V>(pload32(a));
        }
#else
        return unaligned_load<V>(native_ + off);
#endif
    }

    template <typename V>
    __attribute__((always_inline)) void scalar_store(std::size_t off, V value) const {
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
        const uint32_t a = addr_ + static_cast<uint32_t>(off);
        if constexpr (sizeof(V) == 1)
            pstore8(a, static_cast<uint8_t>(value));
        else if constexpr (sizeof(V) == 2)
            pstore16(a, static_cast<uint16_t>(value));
        else {
            static_assert(sizeof(V) == 4, "guest scalar must be 1, 2 or 4 bytes");
            pstore32(a, static_cast<uint32_t>(value));
        }
#else
        unaligned_store<V>(native_ + off, value);
#endif
    }

    __attribute__((always_inline)) uint8_t load_byte(std::size_t off) const {
        return scalar_load<uint8_t>(off);
    }

    __attribute__((always_inline)) void store_byte(std::size_t off, uint8_t value) const {
        scalar_store<uint8_t>(off, value);
    }

    __attribute__((always_inline)) linear_t base_linear() const { return addr_; }

    linear_t addr_;
#if !defined(EGA128) && !defined(VGA128) && !defined(MCGA)
    uint8_t *native_;
#endif
};

class mcb_ref final : private ref_base<mcb> {
public:
    __attribute__((always_inline)) explicit mcb_ref(seg s)
        : ref_base<mcb>(static_cast<linear_t>(s) << 4), seg_(s) {}

    __attribute__((always_inline)) seg segment() const { return seg_; }
    __attribute__((always_inline)) BYTE type() const { return scalar_load<BYTE>(offsetof(mcb, m_type)); }
    __attribute__((always_inline)) UWORD psp() const { return scalar_load<UWORD>(offsetof(mcb, m_psp)); }
    __attribute__((always_inline)) UWORD size() const { return scalar_load<UWORD>(offsetof(mcb, m_size)); }

    __attribute__((always_inline)) void type(BYTE v) const { scalar_store<BYTE>(offsetof(mcb, m_type), v); }
    __attribute__((always_inline)) void psp(UWORD v) const { scalar_store<UWORD>(offsetof(mcb, m_psp), v); }
    __attribute__((always_inline)) void size(UWORD v) const { scalar_store<UWORD>(offsetof(mcb, m_size), v); }

    __attribute__((always_inline)) BYTE name(unsigned i) const {
        return load_byte(offsetof(mcb, m_name) + i);
    }
    __attribute__((always_inline)) void name(unsigned i, BYTE v) const {
        store_byte(offsetof(mcb, m_name) + i, static_cast<uint8_t>(v));
    }
    __attribute__((always_inline)) void clear_name() const {
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
        pstore32(addr_ + offsetof(mcb, m_name), 0);
        pstore32(addr_ + offsetof(mcb, m_name) + 4, 0);
#else
        unaligned_store<uint32_t>(native_ + offsetof(mcb, m_name), 0u);
        unaligned_store<uint32_t>(native_ + offsetof(mcb, m_name) + 4u, 0u);
#endif
    }

    __attribute__((always_inline)) bool is_free() const { return psp() == FREE_PSP; }
    __attribute__((always_inline)) bool valid() const {
        const UWORD s = size();
        const BYTE t = type();
        return s != 0xffff && (t == MCB_NORMAL || t == MCB_LAST);
    }
    __attribute__((always_inline)) bool freeable() const {
        const BYTE t = type();
        return t == MCB_NORMAL || t == MCB_LAST;
    }
    __attribute__((always_inline)) seg next_segment() const {
        return static_cast<seg>(seg_ + size() + 1u);
    }

private:
    seg seg_;
};


template <typename V>
class scalar_proxy {
public:
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    constexpr scalar_proxy(linear_t addr) : addr_(addr) {}
#else
    __attribute__((always_inline)) scalar_proxy(linear_t addr)
        : ptr_(X86_RAM_BASE + addr) {}
    __attribute__((always_inline)) explicit scalar_proxy(uint8_t *ptr)
        : ptr_(ptr) {}
#endif
    __attribute__((always_inline)) operator V() const {
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
        if constexpr (sizeof(V) == 1) {
            return static_cast<V>(pload8(addr_));
        } else if constexpr (sizeof(V) == 2) {
            return static_cast<V>(pload16(addr_));
        } else {
            static_assert(sizeof(V) == 4, "guest scalar must be 1, 2 or 4 bytes");
            return static_cast<V>(pload32(addr_));
        }
#else
        return unaligned_load<V>(ptr_);
#endif
    }
    __attribute__((always_inline)) scalar_proxy &operator=(const scalar_proxy &other) {
        return *this = static_cast<V>(other);
    }
    __attribute__((always_inline)) scalar_proxy &operator=(V v) {
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
        if constexpr (sizeof(V) == 1) pstore8(addr_, static_cast<uint8_t>(v));
        else if constexpr (sizeof(V) == 2) pstore16(addr_, static_cast<uint16_t>(v));
        else { static_assert(sizeof(V) == 4, "guest scalar must be 1, 2 or 4 bytes"); pstore32(addr_, static_cast<uint32_t>(v)); }
#else
        unaligned_store<V>(ptr_, v);
#endif
        return *this;
    }
    __attribute__((always_inline)) scalar_proxy &operator++() { V v = *this; *this = static_cast<V>(v + 1); return *this; }
    __attribute__((always_inline)) scalar_proxy &operator--() { V v = *this; *this = static_cast<V>(v - 1); return *this; }
    __attribute__((always_inline)) scalar_proxy &operator|=(V x) { V v = *this; *this = static_cast<V>(v | x); return *this; }
    __attribute__((always_inline)) scalar_proxy &operator&=(V x) { V v = *this; *this = static_cast<V>(v & x); return *this; }
private:
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    linear_t addr_;
#else
    uint8_t *ptr_;
#endif
};

class flag_proxy {
public:
    constexpr flag_proxy(linear_t addr, uint32_t mask) : addr_(addr), mask_(mask) {}
    __attribute__((always_inline)) operator unsigned() const {
        return (static_cast<uint32_t>(scalar_proxy<uint32_t>(addr_)) & mask_) != 0;
    }
    __attribute__((always_inline)) flag_proxy &operator=(unsigned v) {
        uint32_t f = scalar_proxy<uint32_t>(addr_);
        f = v ? (f | mask_) : (f & ~mask_);
        scalar_proxy<uint32_t> dst(addr_);
        dst = f;
        return *this;
    }
private:
    linear_t addr_;
    uint32_t mask_;
};

class cpu_regs_ref final {
public:
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    explicit constexpr cpu_regs_ref(linear_t addr) : addr_(addr) {}
#define FDOS_CPU_REG_PROXY(type, off) scalar_proxy<type>(addr_ + (off))
#else
    __attribute__((always_inline)) explicit cpu_regs_ref(linear_t addr)
        : addr_(addr), native_(X86_RAM_BASE + addr) {}
#define FDOS_CPU_REG_PROXY(type, off) scalar_proxy<type>(native_ + (off))
#endif
    __attribute__((always_inline)) scalar_proxy<uint32_t> r32(unsigned i) const { return FDOS_CPU_REG_PROXY(uint32_t, offsetof(CPU_regs, gprx) + i * sizeof(gprx_t)); }
    __attribute__((always_inline)) scalar_proxy<uint16_t> r16(unsigned i) const { return FDOS_CPU_REG_PROXY(uint16_t, offsetof(CPU_regs, gprx) + i * sizeof(gprx_t)); }
    __attribute__((always_inline)) scalar_proxy<uint8_t> r8l(unsigned i) const { return FDOS_CPU_REG_PROXY(uint8_t, offsetof(CPU_regs, gprx) + i * sizeof(gprx_t)); }
    __attribute__((always_inline)) scalar_proxy<uint8_t> r8h(unsigned i) const { return FDOS_CPU_REG_PROXY(uint8_t, offsetof(CPU_regs, gprx) + i * sizeof(gprx_t) + 1u); }
    __attribute__((always_inline)) scalar_proxy<uint16_t> es() const { return FDOS_CPU_REG_PROXY(uint16_t, offsetof(CPU_regs, es)); }
    __attribute__((always_inline)) scalar_proxy<uint16_t> ds() const { return FDOS_CPU_REG_PROXY(uint16_t, offsetof(CPU_regs, ds)); }
    __attribute__((always_inline)) scalar_proxy<uint16_t> fs() const { return FDOS_CPU_REG_PROXY(uint16_t, offsetof(CPU_regs, fs)); }
    __attribute__((always_inline)) scalar_proxy<uint16_t> gs() const { return FDOS_CPU_REG_PROXY(uint16_t, offsetof(CPU_regs, gs)); }
    __attribute__((always_inline)) scalar_proxy<uint32_t> flags() const { return FDOS_CPU_REG_PROXY(uint32_t, offsetof(CPU_regs, flags)); }
#undef FDOS_CPU_REG_PROXY
    __attribute__((always_inline)) flag_proxy carry() const { return {addr_ + offsetof(CPU_regs, flags), 0x0001u}; }
    __attribute__((always_inline)) flag_proxy zero() const { return {addr_ + offsetof(CPU_regs, flags), 0x0040u}; }
    __attribute__((always_inline)) void save_cpu(const CPU *cpu) const {
        for (unsigned i = 0; i < 8; ++i)
            r32(i) = cpu->gprx[i].r32;
        flags() = cpu->flags.value;
        es() = cpu->ext_accessors->get_seg16(cpu, SEG_ES);
        ds() = cpu->ext_accessors->get_seg16(cpu, SEG_DS);
        fs() = cpu->ext_accessors->get_seg16(cpu, SEG_FS);
        gs() = cpu->ext_accessors->get_seg16(cpu, SEG_GS);
    }

    __attribute__((always_inline)) void restore_cpu(CPU *cpu) const {
        cpu->flags.value = flags();
        cpu->ext_accessors->set_seg16(cpu, SEG_ES, es());
        cpu->ext_accessors->set_seg16(cpu, SEG_DS, ds());
        cpu->ext_accessors->set_seg16(cpu, SEG_FS, fs());
        cpu->ext_accessors->set_seg16(cpu, SEG_GS, gs());
        for (unsigned i = 0; i < 8; ++i)
            cpu->gprx[i].r32 = r32(i);
    }
private:
    linear_t addr_;
#if !defined(EGA128) && !defined(VGA128) && !defined(MCGA)
    uint8_t *native_;
#endif
};

class psp_ref final {
public:
    explicit constexpr psp_ref(seg s) : addr_(static_cast<linear_t>(s) << 4) {}
    __attribute__((always_inline)) dos_far_ptr stack() const {
        const UWORD offset = scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_stack));
        const UWORD segment = scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_stack) + sizeof(UWORD));
        return MK_FP(segment, offset);
    }
    __attribute__((always_inline)) void stack(dos_far_ptr v) const {
        scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_stack)) = FP_OFF(v);
        scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_stack) + sizeof(UWORD)) = FP_SEG(v);
    }
    __attribute__((always_inline)) UWORD max_files() const {
        return scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_maxfiles));
    }
    __attribute__((always_inline)) void max_files(UWORD v) const {
        scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_maxfiles)) = v;
    }
    __attribute__((always_inline)) dos_far_ptr file_table() const {
        const UWORD offset = scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_filetab));
        const UWORD segment = scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_filetab) + sizeof(UWORD));
        return MK_FP(segment, offset);
    }
    __attribute__((always_inline)) void file_table(dos_far_ptr v) const {
        scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_filetab)) = FP_OFF(v);
        scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_filetab) + sizeof(UWORD)) = FP_SEG(v);
    }
    __attribute__((always_inline)) UBYTE file_handle(UWORD index) const {
        const dos_far_ptr table = file_table();
        const linear_t base = (static_cast<linear_t>(FP_SEG(table)) << 4) + FP_OFF(table);
        return pload8(base + index);
    }
    __attribute__((always_inline)) void file_handle(UWORD index, UBYTE value) const {
        const dos_far_ptr table = file_table();
        const linear_t base = (static_cast<linear_t>(FP_SEG(table)) << 4) + FP_OFF(table);
        pstore8(base + index, value);
    }
    __attribute__((always_inline)) UWORD return_dos_version() const {
        return scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_retdosver));
    }
    __attribute__((always_inline)) void return_dos_version(UWORD v) const {
        scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_retdosver)) = v;
    }
private: linear_t addr_;
};


class lol_ref final {
public:
    explicit constexpr lol_ref(linear_t addr) : addr_(addr) {}
    __attribute__((always_inline)) scalar_proxy<UWORD> first_mcb() const { return {addr_ + offsetof(lol, first_mcb)}; }
    __attribute__((always_inline)) scalar_proxy<UWORD> nbuffers() const { return {addr_ + offsetof(lol, nbuffers)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> nblkdev() const { return {addr_ + offsetof(lol, nblkdev)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> lastdrive() const { return {addr_ + offsetof(lol, lastdrive)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> boot_drive() const { return {addr_ + offsetof(lol, BootDrive)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> cpu_family() const { return {addr_ + offsetof(lol, cpu)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> bufloc() const { return {addr_ + offsetof(lol, bufloc)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> uppermem_link() const { return {addr_ + offsetof(lol, uppermem_link)}; }
    __attribute__((always_inline)) scalar_proxy<UWORD> uppermem_root() const { return {addr_ + offsetof(lol, uppermem_root)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> os_setver_major() const { return {addr_ + offsetof(lol, os_setver_major)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> os_setver_minor() const { return {addr_ + offsetof(lol, os_setver_minor)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> os_major() const { return {addr_ + offsetof(lol, os_major)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> os_minor() const { return {addr_ + offsetof(lol, os_minor)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> version_flags() const { return {addr_ + offsetof(lol, version_flags)}; }
    __attribute__((always_inline)) scalar_proxy<UWORD> maxsecsize() const { return {addr_ + offsetof(lol, maxsecsize)}; }

    __attribute__((always_inline)) dos_far_ptr far_value(std::size_t off) const {
        const UWORD offset = scalar_proxy<UWORD>(addr_ + static_cast<uint32_t>(off));
        const UWORD segment = scalar_proxy<UWORD>(addr_ + static_cast<uint32_t>(off + sizeof(UWORD)));
        return MK_FP(segment, offset);
    }
    __attribute__((always_inline)) void far_value(std::size_t off, dos_far_ptr v) const {
        scalar_proxy<UWORD>(addr_ + static_cast<uint32_t>(off)) = FP_OFF(v);
        scalar_proxy<UWORD>(addr_ + static_cast<uint32_t>(off + sizeof(UWORD))) = FP_SEG(v);
    }
    __attribute__((always_inline)) dos_far_ptr sfthead() const { return far_value(offsetof(lol, sfthead)); }
    __attribute__((always_inline)) void sfthead(dos_far_ptr v) const { far_value(offsetof(lol, sfthead), v); }
    __attribute__((always_inline)) dos_far_ptr cds() const { return far_value(offsetof(lol, CDSp)); }
    __attribute__((always_inline)) void cds(dos_far_ptr v) const { far_value(offsetof(lol, CDSp), v); }
    __attribute__((always_inline)) dos_far_ptr dpb() const { return far_value(offsetof(lol, DPBp)); }
    __attribute__((always_inline)) dos_far_ptr fcb() const { return far_value(offsetof(lol, FCBp)); }
    __attribute__((always_inline)) dos_far_ptr clock() const { return far_value(offsetof(lol, clock)); }
    __attribute__((always_inline)) dos_far_ptr firstbuf() const { return far_value(offsetof(lol, firstbuf)); }
    __attribute__((always_inline)) void firstbuf(dos_far_ptr v) const { far_value(offsetof(lol, firstbuf), v); }
    __attribute__((always_inline)) dos_far_ptr inforecptr() const { return far_value(offsetof(lol, inforecptr)); }
    __attribute__((always_inline)) void inforecptr(dos_far_ptr v) const { far_value(offsetof(lol, inforecptr), v); }
    __attribute__((always_inline)) dos_far_ptr deblock_buf() const { return far_value(offsetof(lol, deblock_buf)); }
    __attribute__((always_inline)) void deblock_buf(dos_far_ptr v) const { far_value(offsetof(lol, deblock_buf), v); }
    __attribute__((always_inline)) dos_far_ptr nul_dev_next() const { return far_value(offsetof(lol, nul_dev) + offsetof(dhdr, dh_next)); }
    __attribute__((always_inline)) void nul_dev_next(dos_far_ptr v) const { far_value(offsetof(lol, nul_dev) + offsetof(dhdr, dh_next), v); }
private:
    linear_t addr_;
};


class cds_ref final : private ref_base<cds> {
public:
    __attribute__((always_inline)) explicit cds_ref(dos_far_ptr p)
        : ref_base<cds>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)), far_(p) {}

    __attribute__((always_inline)) dos_far_ptr far_ptr() const { return far_; }
    __attribute__((always_inline)) UWORD flags() const { return scalar_load<UWORD>(offsetof(cds, cdsFlags)); }
    __attribute__((always_inline)) void flags(UWORD v) const { scalar_store<UWORD>(offsetof(cds, cdsFlags), v); }
    __attribute__((always_inline)) dos_far_ptr dpb() const {
        const UWORD offset = scalar_load<UWORD>(offsetof(cds, cdsDpb));
        const UWORD segment = scalar_load<UWORD>(offsetof(cds, cdsDpb) + sizeof(UWORD));
        return MK_FP(segment, offset);
    }
    __attribute__((always_inline)) WORD backslash_offset() const {
        return scalar_load<WORD>(offsetof(cds, cdsBackslashOffset));
    }
    __attribute__((always_inline)) void copy_current_path(char *dst, std::size_t dst_size) const {
        if (dst_size == 0)
            return;
        std::size_t n = sizeof(((cds *)0)->cdsCurrentPath);
        if (n > dst_size)
            n = dst_size;
        guest_read_block(addr_ + offsetof(cds, cdsCurrentPath), dst, n);
        dst[n - 1] = '\0';
    }

    __attribute__((always_inline)) void current_path_byte(std::size_t index, UBYTE value) const {
        if (index < sizeof(((cds *)0)->cdsCurrentPath))
            store_byte(offsetof(cds, cdsCurrentPath) + index, value);
    }
    __attribute__((always_inline)) void write_current_path(const char *src) const {
        std::size_t n = 0;
        while (n + 1u < sizeof(((cds *)0)->cdsCurrentPath) && src[n] != '\0')
            ++n;
        guest_write_block(addr_ + offsetof(cds, cdsCurrentPath), src, n);
        store_byte(offsetof(cds, cdsCurrentPath) + n, 0);
    }
    __attribute__((always_inline)) void start_cluster(UWORD value) const {
        scalar_store<UWORD>(offsetof(cds, cdsStrtClst), value);
    }

private:
    dos_far_ptr far_;
};


class sft_ref final : private ref_base<sft> {
public:
    __attribute__((always_inline)) explicit sft_ref(dos_far_ptr p)
        : ref_base<sft>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}

    __attribute__((always_inline)) UWORD count() const {
        return scalar_load<UWORD>(offsetof(sft, sft_count));
    }
    __attribute__((always_inline)) void count(UWORD v) const {
        scalar_store<UWORD>(offsetof(sft, sft_count), v);
    }
    __attribute__((always_inline)) void mode(UWORD v) const {
        scalar_store<UWORD>(offsetof(sft, sft_mode), v);
    }
    __attribute__((always_inline)) UBYTE attrib() const {
        return scalar_load<UBYTE>(offsetof(sft, sft_attrib));
    }
    __attribute__((always_inline)) void attrib(UBYTE v) const {
        scalar_store<UBYTE>(offsetof(sft, sft_attrib), v);
    }
    __attribute__((always_inline)) UWORD mode() const {
        return scalar_load<UWORD>(offsetof(sft, sft_mode));
    }
    __attribute__((always_inline)) UWORD flags() const {
        return scalar_load<UWORD>(offsetof(sft, sft_flags));
    }
    __attribute__((always_inline)) void flags(UWORD v) const {
        scalar_store<UWORD>(offsetof(sft, sft_flags), v);
    }
    __attribute__((always_inline)) dos_far_ptr dev() const {
        return dcb();
    }
    __attribute__((always_inline)) dos_far_ptr dcb() const {
        const UWORD offset = scalar_load<UWORD>(offsetof(sft, sft_dcb));
        const UWORD segment = scalar_load<UWORD>(offsetof(sft, sft_dcb) + sizeof(UWORD));
        return MK_FP(segment, offset);
    }
    __attribute__((always_inline)) void dcb(dos_far_ptr v) const {
        scalar_store<UWORD>(offsetof(sft, sft_dcb), FP_OFF(v));
        scalar_store<UWORD>(offsetof(sft, sft_dcb) + sizeof(UWORD), FP_SEG(v));
    }
    __attribute__((always_inline)) CLUSTER start_cluster() const { return scalar_load<CLUSTER>(offsetof(sft, sft_stclust)); }
    __attribute__((always_inline)) void start_cluster(CLUSTER v) const { scalar_store<CLUSTER>(offsetof(sft, sft_stclust), v); }
    __attribute__((always_inline)) dtime time() const { return scalar_load<dtime>(offsetof(sft, sft_time)); }
    __attribute__((always_inline)) void time(dtime v) const { scalar_store<dtime>(offsetof(sft, sft_time), v); }
    __attribute__((always_inline)) ddate date() const { return scalar_load<ddate>(offsetof(sft, sft_date)); }
    __attribute__((always_inline)) void date(ddate v) const { scalar_store<ddate>(offsetof(sft, sft_date), v); }
    __attribute__((always_inline)) ULONG size() const { return scalar_load<ULONG>(offsetof(sft, sft_size)); }
    __attribute__((always_inline)) void size(ULONG v) const { scalar_store<ULONG>(offsetof(sft, sft_size), v); }
    __attribute__((always_inline)) ULONG position() const { return scalar_load<ULONG>(offsetof(sft, sft_posit)); }
    __attribute__((always_inline)) void position(ULONG v) const { scalar_store<ULONG>(offsetof(sft, sft_posit), v); }
    __attribute__((always_inline)) UWORD rel_cluster() const { return scalar_load<UWORD>(offsetof(sft, sft_relclust)); }
    __attribute__((always_inline)) void rel_cluster(UWORD v) const { scalar_store<UWORD>(offsetof(sft, sft_relclust), v); }
#ifdef WITHFAT32
    __attribute__((always_inline)) UWORD rel_cluster_high() const { return scalar_load<UWORD>(offsetof(sft, sft_relclust_high)); }
    __attribute__((always_inline)) void rel_cluster_high(UWORD v) const { scalar_store<UWORD>(offsetof(sft, sft_relclust_high), v); }
#endif
    __attribute__((always_inline)) ULONG dir_sector() const { return scalar_load<ULONG>(offsetof(sft, sft_dirsector)); }
    __attribute__((always_inline)) void dir_sector(ULONG v) const { scalar_store<ULONG>(offsetof(sft, sft_dirsector), v); }
    __attribute__((always_inline)) UBYTE dir_index() const { return scalar_load<UBYTE>(offsetof(sft, sft_diridx)); }
    __attribute__((always_inline)) void dir_index(UBYTE v) const { scalar_store<UBYTE>(offsetof(sft, sft_diridx), v); }
    __attribute__((always_inline)) CLUSTER current_cluster() const { return scalar_load<CLUSTER>(offsetof(sft, sft_cuclust)); }
    __attribute__((always_inline)) void current_cluster(CLUSTER v) const { scalar_store<CLUSTER>(offsetof(sft, sft_cuclust), v); }
    __attribute__((always_inline)) void read_name(BYTE *dst) const { guest_read_block(addr_ + offsetof(sft, sft_name), dst, sizeof(((sft *)0)->sft_name)); }
    __attribute__((always_inline)) void write_name(const BYTE *src) const { guest_write_block(addr_ + offsetof(sft, sft_name), src, sizeof(((sft *)0)->sft_name)); }
    __attribute__((always_inline)) void psp(UWORD v) const {
        scalar_store<UWORD>(offsetof(sft, sft_psp), v);
    }
    __attribute__((always_inline)) WORD shroff() const {
        return scalar_load<WORD>(offsetof(sft, sft_shroff));
    }
    __attribute__((always_inline)) void shroff(WORD v) const {
        scalar_store<WORD>(offsetof(sft, sft_shroff), v);
    }
    __attribute__((always_inline)) void clear() const {
        guest_fill_block(addr_, 0, sizeof(sft));
    }
};

class sfttbl_ref final : private ref_base<sfttbl> {
public:
    __attribute__((always_inline)) explicit sfttbl_ref(dos_far_ptr p)
        : ref_base<sfttbl>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)), far_(p) {}

    __attribute__((always_inline)) UWORD count() const {
        return scalar_load<UWORD>(offsetof(sfttbl, sftt_count));
    }

    __attribute__((always_inline)) dos_far_ptr next() const {
        const UWORD offset = scalar_load<UWORD>(offsetof(sfttbl, sftt_next));
        const UWORD segment = scalar_load<UWORD>(offsetof(sfttbl, sftt_next) + sizeof(UWORD));
        return MK_FP(segment, offset);
    }

    __attribute__((always_inline)) dos_far_ptr entry(UWORD index) const {
        return MK_FP(FP_SEG(far_),
                     (UWORD)(FP_OFF(far_) + offsetof(sfttbl, sftt_table) +
                             (uint32_t)index * sizeof(sft)));
    }

private:
    dos_far_ptr far_;
};

class dpb_ref final : private ref_base<dpb> {
public:
    __attribute__((always_inline)) explicit dpb_ref(linear_t addr)
        : ref_base<dpb>(addr) {}
    __attribute__((always_inline)) explicit dpb_ref(dos_far_ptr p)
        : ref_base<dpb>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}

#define FDOS_GUEST_RW8(name) \
    __attribute__((always_inline)) UBYTE name() const { return scalar_load<UBYTE>(offsetof(dpb, name)); } \
    __attribute__((always_inline)) void name(UBYTE v) const { scalar_store<UBYTE>(offsetof(dpb, name), v); }
#define FDOS_GUEST_RW16(name) \
    __attribute__((always_inline)) UWORD name() const { return scalar_load<UWORD>(offsetof(dpb, name)); } \
    __attribute__((always_inline)) void name(UWORD v) const { scalar_store<UWORD>(offsetof(dpb, name), v); }
#define FDOS_GUEST_RW32(name) \
    __attribute__((always_inline)) ULONG name() const { return scalar_load<ULONG>(offsetof(dpb, name)); } \
    __attribute__((always_inline)) void name(ULONG v) const { scalar_store<ULONG>(offsetof(dpb, name), v); }

    FDOS_GUEST_RW8(dpb_unit)
    __attribute__((always_inline)) UBYTE unit() const { return dpb_unit(); }
    FDOS_GUEST_RW8(dpb_subunit)
    FDOS_GUEST_RW16(dpb_secsize)
    FDOS_GUEST_RW8(dpb_clsmask)
    __attribute__((always_inline)) UBYTE cluster_mask() const { return dpb_clsmask(); }
    FDOS_GUEST_RW8(dpb_shftcnt)
    FDOS_GUEST_RW16(dpb_fatstrt)
    FDOS_GUEST_RW8(dpb_fats)
    FDOS_GUEST_RW16(dpb_dirents)
    FDOS_GUEST_RW16(dpb_data)
    __attribute__((always_inline)) UWORD data_start() const { return dpb_data(); }
    FDOS_GUEST_RW16(dpb_size)
    FDOS_GUEST_RW16(dpb_fatsize)
    __attribute__((always_inline)) bool is_fat32() const { return dpb_fatsize() == 0; }
    FDOS_GUEST_RW16(dpb_dirstrt)
    FDOS_GUEST_RW8(dpb_mdb)

    __attribute__((always_inline)) BYTE flags() const { return scalar_load<BYTE>(offsetof(dpb, dpb_flags)); }
    __attribute__((always_inline)) void flags(BYTE v) const { scalar_store<BYTE>(offsetof(dpb, dpb_flags), v); }
    __attribute__((always_inline)) UWORD cluster() const { return scalar_load<UWORD>(offsetof(dpb, dpb_cluster)); }
    __attribute__((always_inline)) void cluster(UWORD v) const { scalar_store<UWORD>(offsetof(dpb, dpb_cluster), v); }
    __attribute__((always_inline)) UWORD nfree() const {
#ifdef WITHFAT32
        return scalar_load<UWORD>(offsetof(dpb, dpb_nfreeclst_un));
#else
        return scalar_load<UWORD>(offsetof(dpb, dpb_nfreeclst));
#endif
    }
    __attribute__((always_inline)) void nfree(UWORD v) const {
#ifdef WITHFAT32
        scalar_store<UWORD>(offsetof(dpb, dpb_nfreeclst_un), v);
#else
        scalar_store<UWORD>(offsetof(dpb, dpb_nfreeclst), v);
#endif
    }
#ifdef WITHFAT32
    __attribute__((always_inline)) void nfree_hi(UWORD v) const {
        scalar_store<UWORD>(offsetof(dpb, dpb_nfreeclst_un) + sizeof(UWORD), v);
    }
#endif
    __attribute__((always_inline)) dos_far_ptr next() const { return far_load(offsetof(dpb, dpb_next)); }
    __attribute__((always_inline)) dos_far_ptr device() const { return far_load(offsetof(dpb, dpb_device)); }

#ifdef WITHFAT32
    FDOS_GUEST_RW16(dpb_xflags)
    FDOS_GUEST_RW16(dpb_xfsinfosec)
    FDOS_GUEST_RW16(dpb_xbackupsec)
    FDOS_GUEST_RW32(dpb_xdata)
    __attribute__((always_inline)) ULONG xdata_start() const { return dpb_xdata(); }
    FDOS_GUEST_RW32(dpb_xsize)
    FDOS_GUEST_RW32(dpb_xfatsize)
    FDOS_GUEST_RW32(dpb_xrootclst)
    FDOS_GUEST_RW32(dpb_xcluster)
    __attribute__((always_inline)) ULONG xnfree() const { return scalar_load<ULONG>(offsetof(dpb, dpb_nfreeclst_un)); }
    __attribute__((always_inline)) void xnfree(ULONG v) const { scalar_store<ULONG>(offsetof(dpb, dpb_nfreeclst_un), v); }
#endif

#undef FDOS_GUEST_RW8
#undef FDOS_GUEST_RW16
#undef FDOS_GUEST_RW32

private:
    __attribute__((always_inline)) dos_far_ptr far_load(std::size_t off) const {
        const UWORD offset = scalar_load<UWORD>(off);
        const UWORD segment = scalar_load<UWORD>(off + sizeof(UWORD));
        return MK_FP(segment, offset);
    }
};

class bpb_ref final : private ref_base<bpb> {
public:
    __attribute__((always_inline)) explicit bpb_ref(linear_t addr) : ref_base<bpb>(addr) {}
    __attribute__((always_inline)) explicit bpb_ref(dos_far_ptr p)
        : ref_base<bpb>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}

#define FDOS_GUEST_R8(name) __attribute__((always_inline)) UBYTE name() const { return scalar_load<UBYTE>(offsetof(bpb, name)); }
#define FDOS_GUEST_R16(name) __attribute__((always_inline)) UWORD name() const { return scalar_load<UWORD>(offsetof(bpb, name)); }
#define FDOS_GUEST_R32(name) __attribute__((always_inline)) ULONG name() const { return scalar_load<ULONG>(offsetof(bpb, name)); }
    FDOS_GUEST_R16(bpb_nbyte)
    FDOS_GUEST_R8(bpb_nsector)
    FDOS_GUEST_R16(bpb_nreserved)
    FDOS_GUEST_R8(bpb_nfat)
    FDOS_GUEST_R16(bpb_ndirent)
    FDOS_GUEST_R16(bpb_nsize)
    FDOS_GUEST_R8(bpb_mdesc)
    FDOS_GUEST_R16(bpb_nfsect)
    FDOS_GUEST_R16(bpb_nsecs)
    FDOS_GUEST_R16(bpb_nheads)
    FDOS_GUEST_R32(bpb_huge)
    __attribute__((always_inline)) linear_t linear() const { return base_linear(); }
    __attribute__((always_inline)) void read(void *dst, size_t n = sizeof(bpb)) const {
        guest_read_block(base_linear(), dst, n);
    }
    __attribute__((always_inline)) void write(const void *src, size_t n = sizeof(bpb)) const {
        guest_write_block(base_linear(), src, n);
    }
#ifdef WITHFAT32
    FDOS_GUEST_R32(bpb_xnfsect)
    FDOS_GUEST_R16(bpb_xflags)
    FDOS_GUEST_R32(bpb_xrootclst)
    FDOS_GUEST_R16(bpb_xfsinfosec)
    FDOS_GUEST_R16(bpb_xbackupsec)
#endif
#undef FDOS_GUEST_R8
#undef FDOS_GUEST_R16
#undef FDOS_GUEST_R32
};

class ddt_ref final : private ref_base<ddt> {
public:
    __attribute__((always_inline)) explicit ddt_ref(linear_t addr) : ref_base<ddt>(addr) {}
    __attribute__((always_inline)) explicit ddt_ref(dos_far_ptr p)
        : ref_base<ddt>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}

    __attribute__((always_inline)) linear_t linear() const { return base_linear(); }

#define FDOS_DDT_RW8(name, field) \
    __attribute__((always_inline)) UBYTE name() const { return scalar_load<UBYTE>(offsetof(ddt, field)); } \
    __attribute__((always_inline)) void name(UBYTE v) const { scalar_store<UBYTE>(offsetof(ddt, field), v); }
#define FDOS_DDT_RW16(name, field) \
    __attribute__((always_inline)) UWORD name() const { return scalar_load<UWORD>(offsetof(ddt, field)); } \
    __attribute__((always_inline)) void name(UWORD v) const { scalar_store<UWORD>(offsetof(ddt, field), v); }
#define FDOS_DDT_RW32(name, field) \
    __attribute__((always_inline)) ULONG name() const { return scalar_load<ULONG>(offsetof(ddt, field)); } \
    __attribute__((always_inline)) void name(ULONG v) const { scalar_store<ULONG>(offsetof(ddt, field), v); }
    FDOS_DDT_RW8(driveno, ddt_driveno)
    FDOS_DDT_RW8(logdriveno, ddt_logdriveno)
    FDOS_DDT_RW8(flags, ddt_flags)
    FDOS_DDT_RW16(file_open_count, ddt_FileOC)
    FDOS_DDT_RW8(type, ddt_type)
    FDOS_DDT_RW16(descflags, ddt_descflags)
    FDOS_DDT_RW16(ncyl, ddt_ncyl)
    FDOS_DDT_RW8(ltrack, ddt_ltrack)
    FDOS_DDT_RW32(lasttime, ddt_fh)
    FDOS_DDT_RW32(serialno, ddt_serialno)
    FDOS_DDT_RW32(offset, ddt_offset)
#undef FDOS_DDT_RW8
#undef FDOS_DDT_RW16
#undef FDOS_DDT_RW32

    __attribute__((always_inline)) bpb_ref current_bpb() const {
        return bpb_ref(base_linear() + offsetof(ddt, ddt_bpb));
    }
    __attribute__((always_inline)) bpb_ref default_bpb() const {
        return bpb_ref(base_linear() + offsetof(ddt, ddt_defbpb));
    }
    __attribute__((always_inline)) linear_t current_bpb_linear() const {
        return base_linear() + offsetof(ddt, ddt_bpb);
    }
    __attribute__((always_inline)) linear_t default_bpb_linear() const {
        return base_linear() + offsetof(ddt, ddt_defbpb);
    }

    __attribute__((always_inline)) void copy_current_bpb_from_default() const {
        guest_move_block(current_bpb_linear(), default_bpb_linear(), sizeof(bpb));
    }
    __attribute__((always_inline)) void read_current_bpb(void *dst, size_t n = sizeof(bpb)) const {
        guest_read_block(current_bpb_linear(), dst, n);
    }
    __attribute__((always_inline)) void write_current_bpb(const void *src, size_t n = sizeof(bpb)) const {
        guest_write_block(current_bpb_linear(), src, n);
    }
    __attribute__((always_inline)) void read_default_bpb(void *dst, size_t n = sizeof(bpb)) const {
        guest_read_block(default_bpb_linear(), dst, n);
    }
    __attribute__((always_inline)) void write_default_bpb(const void *src, size_t n = sizeof(bpb)) const {
        guest_write_block(default_bpb_linear(), src, n);
    }
    __attribute__((always_inline)) linear_t volume_linear() const { return base_linear() + offsetof(ddt, ddt_volume); }
    __attribute__((always_inline)) linear_t fstype_linear() const { return base_linear() + offsetof(ddt, ddt_fstype); }
    __attribute__((always_inline)) void read_volume(void *dst, size_t n = sizeof(((ddt *)0)->ddt_volume)) const {
        guest_read_block(volume_linear(), dst, n);
    }
    __attribute__((always_inline)) void write_volume(const void *src, size_t n = sizeof(((ddt *)0)->ddt_volume)) const {
        guest_write_block(volume_linear(), src, n);
    }
    __attribute__((always_inline)) void read_fstype(void *dst, size_t n = sizeof(((ddt *)0)->ddt_fstype)) const {
        guest_read_block(fstype_linear(), dst, n);
    }
    __attribute__((always_inline)) void write_fstype(const void *src, size_t n = sizeof(((ddt *)0)->ddt_fstype)) const {
        guest_write_block(fstype_linear(), src, n);
    }
};


class bios_lba_packet_ref final : private ref_base<_bios_LBA_address_packet> {
public:
    __attribute__((always_inline)) explicit bios_lba_packet_ref(dos_far_ptr p)
        : ref_base<_bios_LBA_address_packet>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}

    __attribute__((always_inline)) void packet_size(UBYTE v) const { scalar_store<UBYTE>(offsetof(_bios_LBA_address_packet, packet_size), v); }
    __attribute__((always_inline)) void reserved_1(UBYTE v) const { scalar_store<UBYTE>(offsetof(_bios_LBA_address_packet, reserved_1), v); }
    __attribute__((always_inline)) void number_of_blocks(UWORD v) const { scalar_store<UWORD>(offsetof(_bios_LBA_address_packet, number_of_blocks), v); }
    __attribute__((always_inline)) void buffer_address(dos_far_ptr p) const {
        const uint32_t x = (static_cast<uint32_t>(FP_SEG(p)) << 16) | FP_OFF(p);
        scalar_store<uint32_t>(offsetof(_bios_LBA_address_packet, buffer_address), x);
    }
    __attribute__((always_inline)) void block_address(ULONG v) const { scalar_store<ULONG>(offsetof(_bios_LBA_address_packet, block_address), v); }
    __attribute__((always_inline)) void block_address_high(ULONG v) const { scalar_store<ULONG>(offsetof(_bios_LBA_address_packet, block_address_high), v); }
};

class gblkio_ref final : private ref_base<gblkio> {
public:
    __attribute__((always_inline)) explicit gblkio_ref(dos_far_ptr p)
        : ref_base<gblkio>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}
    __attribute__((always_inline)) UBYTE spcfunbit() const { return scalar_load<UBYTE>(offsetof(gblkio, gbio_spcfunbit)); }
    __attribute__((always_inline)) UBYTE devtype() const { return scalar_load<UBYTE>(offsetof(gblkio, gbio_devtype)); }
    __attribute__((always_inline)) void devtype(UBYTE v) const { scalar_store<UBYTE>(offsetof(gblkio, gbio_devtype), v); }
    __attribute__((always_inline)) UWORD devattrib() const { return scalar_load<UWORD>(offsetof(gblkio, gbio_devattrib)); }
    __attribute__((always_inline)) void devattrib(UWORD v) const { scalar_store<UWORD>(offsetof(gblkio, gbio_devattrib), v); }
    __attribute__((always_inline)) UWORD ncyl() const { return scalar_load<UWORD>(offsetof(gblkio, gbio_ncyl)); }
    __attribute__((always_inline)) void ncyl(UWORD v) const { scalar_store<UWORD>(offsetof(gblkio, gbio_ncyl), v); }
    __attribute__((always_inline)) void media(UBYTE v) const { scalar_store<UBYTE>(offsetof(gblkio, gbio_media), v); }
    __attribute__((always_inline)) bpb_ref bpb_data() const { return bpb_ref(base_linear() + offsetof(gblkio, gbio_bpb)); }
};

class gblkrw_ref final : private ref_base<gblkrw> {
public:
    __attribute__((always_inline)) explicit gblkrw_ref(dos_far_ptr p)
        : ref_base<gblkrw>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}
    __attribute__((always_inline)) UWORD head() const { return scalar_load<UWORD>(offsetof(gblkrw, gbrw_head)); }
    __attribute__((always_inline)) UWORD cyl() const { return scalar_load<UWORD>(offsetof(gblkrw, gbrw_cyl)); }
    __attribute__((always_inline)) UWORD sector() const { return scalar_load<UWORD>(offsetof(gblkrw, gbrw_sector)); }
    __attribute__((always_inline)) UWORD nsecs() const { return scalar_load<UWORD>(offsetof(gblkrw, gbrw_nsecs)); }
    __attribute__((always_inline)) dos_far_ptr buffer() const {
        const uint32_t x = scalar_load<uint32_t>(offsetof(gblkrw, gbrw_buffer));
        return MK_FP(static_cast<UWORD>(x >> 16), static_cast<UWORD>(x));
    }
};

class gblkfv_ref final : private ref_base<gblkfv> {
public:
    __attribute__((always_inline)) explicit gblkfv_ref(dos_far_ptr p)
        : ref_base<gblkfv>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}
    __attribute__((always_inline)) UBYTE spcfunbit() const { return scalar_load<UBYTE>(offsetof(gblkfv, gbfv_spcfunbit)); }
    __attribute__((always_inline)) void spcfunbit(UBYTE v) const { scalar_store<UBYTE>(offsetof(gblkfv, gbfv_spcfunbit), v); }
    __attribute__((always_inline)) UWORD head() const { return scalar_load<UWORD>(offsetof(gblkfv, gbfv_head)); }
    __attribute__((always_inline)) UWORD cyl() const { return scalar_load<UWORD>(offsetof(gblkfv, gbfv_cyl)); }
    __attribute__((always_inline)) UWORD ntracks() const { return scalar_load<UWORD>(offsetof(gblkfv, gbfv_ntracks)); }
};

class gioc_media_ref final : private ref_base<Gioc_media> {
public:
    __attribute__((always_inline)) explicit gioc_media_ref(dos_far_ptr p)
        : ref_base<Gioc_media>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}
    __attribute__((always_inline)) linear_t volume_linear() const { return base_linear() + offsetof(Gioc_media, ioc_volume); }
    __attribute__((always_inline)) linear_t fstype_linear() const { return base_linear() + offsetof(Gioc_media, ioc_fstype); }
    __attribute__((always_inline)) ULONG serialno() const { return scalar_load<ULONG>(offsetof(Gioc_media, ioc_serialno)); }
    __attribute__((always_inline)) void serialno(ULONG v) const { scalar_store<ULONG>(offsetof(Gioc_media, ioc_serialno), v); }
    __attribute__((always_inline)) void read_volume(void *dst, size_t n = sizeof(((Gioc_media*)0)->ioc_volume)) const { guest_read_block(base_linear() + offsetof(Gioc_media, ioc_volume), dst, n); }
    __attribute__((always_inline)) void write_volume(const void *src, size_t n = sizeof(((Gioc_media*)0)->ioc_volume)) const { guest_write_block(base_linear() + offsetof(Gioc_media, ioc_volume), src, n); }
    __attribute__((always_inline)) void write_fstype(const void *src, size_t n = sizeof(((Gioc_media*)0)->ioc_fstype)) const { guest_write_block(base_linear() + offsetof(Gioc_media, ioc_fstype), src, n); }
};

class access_info_ref final : private ref_base<Access_info> {
public:
    __attribute__((always_inline)) explicit access_info_ref(dos_far_ptr p)
        : ref_base<Access_info>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}
    __attribute__((always_inline)) BYTE flag() const { return scalar_load<BYTE>(offsetof(Access_info, AI_Flag)); }
    __attribute__((always_inline)) void flag(BYTE v) const { scalar_store<BYTE>(offsetof(Access_info, AI_Flag), v); }
};

class guest_bytes_ref final : private ref_base<UBYTE> {
public:
    __attribute__((always_inline)) explicit guest_bytes_ref(dos_far_ptr p)
        : ref_base<UBYTE>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}
    __attribute__((always_inline)) UBYTE byte(size_t off) const { return scalar_load<UBYTE>(off); }
    __attribute__((always_inline)) UWORD word(size_t off) const { return scalar_load<UWORD>(off); }
    __attribute__((always_inline)) ULONG dword(size_t off) const { return scalar_load<ULONG>(off); }
    __attribute__((always_inline)) void byte(size_t off, UBYTE v) const { scalar_store<UBYTE>(off, v); }
    __attribute__((always_inline)) void word(size_t off, UWORD v) const { scalar_store<UWORD>(off, v); }
    __attribute__((always_inline)) void dword(size_t off, ULONG v) const { scalar_store<ULONG>(off, v); }
    __attribute__((always_inline)) linear_t linear(size_t off = 0) const { return base_linear() + static_cast<linear_t>(off); }
    __attribute__((always_inline)) void read(size_t off, void *dst, size_t n) const { guest_read_block(linear(off), dst, n); }
    __attribute__((always_inline)) void write(size_t off, const void *src, size_t n) const { guest_write_block(base_linear() + static_cast<linear_t>(off), src, n); }
};

class request_ref final : private ref_base<request> {
public:
    __attribute__((always_inline)) explicit request_ref(linear_t addr) : ref_base<request>(addr) {}
    __attribute__((always_inline)) explicit request_ref(dos_far_ptr p)
        : ref_base<request>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}

#define FDOS_REQ_SCALAR(type, name, field) \
    __attribute__((always_inline)) type name() const { return scalar_load<type>(offsetof(request, field)); } \
    __attribute__((always_inline)) void name(type v) const { scalar_store<type>(offsetof(request, field), v); }
    FDOS_REQ_SCALAR(UBYTE, length, r_length)
    FDOS_REQ_SCALAR(UBYTE, unit, r_unit)
    FDOS_REQ_SCALAR(UBYTE, command, r_command)
    FDOS_REQ_SCALAR(UWORD, status, r_status)
    FDOS_REQ_SCALAR(BYTE, mcmdesc, r_mcmdesc)
    FDOS_REQ_SCALAR(BYTE, meddesc, r_meddesc)
    FDOS_REQ_SCALAR(BYTE, mcretcode, r_mcretcode)
    FDOS_REQ_SCALAR(UBYTE, nunits, r_nunits)
    FDOS_REQ_SCALAR(UBYTE, firstunit, r_firstunit)
    FDOS_REQ_SCALAR(UBYTE, ndbyte, r_ndbyte)
    FDOS_REQ_SCALAR(UWORD, count, r_count)
    FDOS_REQ_SCALAR(UWORD, start, r_start)
    FDOS_REQ_SCALAR(LONG, huge, r_huge)
    FDOS_REQ_SCALAR(UBYTE, cat, r_cat)
    FDOS_REQ_SCALAR(UBYTE, fun, r_fun)
    FDOS_REQ_SCALAR(UWORD, si, r_si)
    FDOS_REQ_SCALAR(UWORD, di, r_di)
#undef FDOS_REQ_SCALAR

#define FDOS_REQ_FAR(name, field) \
    __attribute__((always_inline)) dos_far_ptr name() const { return far_load(offsetof(request, field)); } \
    __attribute__((always_inline)) void name(dos_far_ptr p) const { far_store(offsetof(request, field), p); }
    FDOS_REQ_FAR(endaddr, r_endaddr)
    FDOS_REQ_FAR(bpptr, r_bpptr)
    FDOS_REQ_FAR(bpfat, r_bpfat)
    FDOS_REQ_FAR(trans, r_trans)
    FDOS_REQ_FAR(io, r_io)
    FDOS_REQ_FAR(rw, r_rw)
    FDOS_REQ_FAR(fv, r_fv)
    FDOS_REQ_FAR(gioc, r_gioc)
    FDOS_REQ_FAR(ai, r_ai)
#undef FDOS_REQ_FAR

private:
    __attribute__((always_inline)) dos_far_ptr far_load(std::size_t off) const {
        const uint32_t x = scalar_load<uint32_t>(off);
        return MK_FP(static_cast<UWORD>(x >> 16), static_cast<UWORD>(x));
    }
    __attribute__((always_inline)) void far_store(std::size_t off, dos_far_ptr p) const {
        const uint32_t x = (static_cast<uint32_t>(FP_SEG(p)) << 16) | FP_OFF(p);
        scalar_store<uint32_t>(off, x);
    }
};

class dhdr_ref final : private ref_base<dhdr> {
public:
    using native_interrupt_t = VOID(*)(dos_far_ptr);

    __attribute__((always_inline)) explicit dhdr_ref(dos_far_ptr p)
        : ref_base<dhdr>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}

    __attribute__((always_inline)) dos_far_ptr next() const {
        const UWORD offset = scalar_load<UWORD>(offsetof(dhdr, dh_next));
        const UWORD segment = scalar_load<UWORD>(offsetof(dhdr, dh_next) + sizeof(UWORD));
        return MK_FP(segment, offset);
    }
    __attribute__((always_inline)) UWORD attr() const {
        return scalar_load<UWORD>(offsetof(dhdr, dh_attr));
    }
    __attribute__((always_inline)) void read_name(BYTE *dst) const {
        guest_read_block(addr_ + offsetof(dhdr, dh_name), dst, sizeof(((dhdr *)0)->dh_name));
    }
    __attribute__((always_inline)) UWORD strategy() const {
        return scalar_load<UWORD>(offsetof(dhdr, x86.dh_strategy));
    }
    __attribute__((always_inline)) UWORD interrupt() const {
        return scalar_load<UWORD>(offsetof(dhdr, x86.dh_interrupt));
    }
    __attribute__((always_inline)) native_interrupt_t native_interrupt() const {
        const uintptr_t p = static_cast<uintptr_t>(scalar_load<uint32_t>(offsetof(dhdr, arm.dh_interrupt)));
        return reinterpret_cast<native_interrupt_t>(p);
    }
};

class dos_data_ref final {
public:
    explicit constexpr dos_data_ref(linear_t addr) : addr_(addr) {}
    __attribute__((always_inline)) scalar_proxy<UWORD> cu_psp() const { return {addr_+offsetof(dos_data,cu_psp)}; }
    __attribute__((always_inline)) scalar_proxy<UWORD> int21ax() const { return {addr_+offsetof(dos_data,Int21AX)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> mem_access_mode() const { return {addr_+offsetof(dos_data,mem_access_mode)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> break_ena() const { return {addr_+offsetof(dos_data,break_ena)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> indos() const { return {addr_+offsetof(dos_data,InDOS)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> error_mode() const { return {addr_+offsetof(dos_data,ErrorMode)}; }
    __attribute__((always_inline)) scalar_proxy<UWORD> crit_err_code() const { return {addr_+offsetof(dos_data,CritErrCode)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> default_drive() const { return {addr_+offsetof(dos_data,default_drive)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> open_mode() const { return {addr_+offsetof(dos_data,OpenMode)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> switchar() const { return {addr_+offsetof(dos_data,switchar)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> verify_ena() const { return {addr_+offsetof(dos_data,verify_ena)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> abort_progress() const { return {addr_+offsetof(dos_data,abort_progress)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> crit_err_drive() const { return {addr_+offsetof(dos_data,CritErrDrive)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> crit_err_locus() const { return {addr_+offsetof(dos_data,CritErrLocus)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> crit_err_class() const { return {addr_+offsetof(dos_data,CritErrClass)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> crit_err_action() const { return {addr_+offsetof(dos_data,CritErrAction)}; }
    __attribute__((always_inline)) scalar_proxy<UWORD> current_sft_idx() const { return {addr_+offsetof(dos_data,current_sft_idx)}; }
    __attribute__((always_inline)) scalar_proxy<UWORD> clock_days() const { return {addr_+offsetof(dos_data,ClkRecord)+offsetof(ClockRecord,clkDays)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> clock_minutes() const { return {addr_+offsetof(dos_data,ClkRecord)+offsetof(ClockRecord,clkMinutes)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> clock_hours() const { return {addr_+offsetof(dos_data,ClkRecord)+offsetof(ClockRecord,clkHours)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> clock_hundredths() const { return {addr_+offsetof(dos_data,ClkRecord)+offsetof(ClockRecord,clkHundredths)}; }
    __attribute__((always_inline)) scalar_proxy<UBYTE> clock_seconds() const { return {addr_+offsetof(dos_data,ClkRecord)+offsetof(ClockRecord,clkSeconds)}; }
    __attribute__((always_inline)) dos_far_ptr dta() const { return far_load(offsetof(dos_data,dta)); }
    __attribute__((always_inline)) void dta(dos_far_ptr v) const { far_store(offsetof(dos_data,dta),v); }
    __attribute__((always_inline)) dos_far_ptr crit_err_dev() const { return far_load(offsetof(dos_data,CritErrDev)); }
    __attribute__((always_inline)) void crit_err_dev(dos_far_ptr v) const { far_store(offsetof(dos_data,CritErrDev),v); }
    __attribute__((always_inline)) dos_far_ptr lp_cur_sft() const { return far_load(offsetof(dos_data,lpCurSft)); }
    __attribute__((always_inline)) void lp_cur_sft(dos_far_ptr v) const { far_store(offsetof(dos_data,lpCurSft),v); }
    __attribute__((always_inline)) dos_far_ptr current_ldt() const { return far_load(offsetof(dos_data,current_ldt)); }
    __attribute__((always_inline)) void current_ldt(dos_far_ptr v) const { far_store(offsetof(dos_data,current_ldt),v); }
    __attribute__((always_inline)) dos_far_ptr user_r() const { return far_load(offsetof(dos_data,user_r)); }
    __attribute__((always_inline)) dos_far_ptr prev_user_r() const { return far_load(offsetof(dos_data,prev_user_r)); }
    __attribute__((always_inline)) void user_r(dos_far_ptr v) const { far_store(offsetof(dos_data,user_r),v); }
    __attribute__((always_inline)) void prev_user_r(dos_far_ptr v) const { far_store(offsetof(dos_data,prev_user_r),v); }
private:
    __attribute__((always_inline)) dos_far_ptr far_load(size_t off) const {
        const UWORD offset = scalar_proxy<UWORD>(addr_ + static_cast<uint32_t>(off));
        const UWORD segment = scalar_proxy<UWORD>(addr_ + static_cast<uint32_t>(off + sizeof(UWORD)));
        return MK_FP(segment, offset);
    }
    __attribute__((always_inline)) void far_store(size_t off, dos_far_ptr v) const {
        scalar_proxy<UWORD>(addr_ + static_cast<uint32_t>(off)) = FP_OFF(v);
        scalar_proxy<UWORD>(addr_ + static_cast<uint32_t>(off + sizeof(UWORD))) = FP_SEG(v);
    }
    linear_t addr_;
};

class dmatch_ref final {
public:
    __attribute__((always_inline)) explicit dmatch_ref(dos_far_ptr p) : addr_(p) {}
    __attribute__((always_inline)) UBYTE drive() const { return read_field<UBYTE>(offsetof(dmatch, dm_drive)); }
    __attribute__((always_inline)) UBYTE attr_search() const { return read_field<UBYTE>(offsetof(dmatch, dm_attr_srch)); }
    __attribute__((always_inline)) UWORD entry() const { return read_field<UWORD>(offsetof(dmatch, dm_entry)); }
    __attribute__((always_inline)) void attr_found(UBYTE v) const { write_field<UBYTE>(offsetof(dmatch, dm_attr_fnd), v); }
    __attribute__((always_inline)) void time_found(dtime v) const { write_field<dtime>(offsetof(dmatch, dm_time), v); }
    __attribute__((always_inline)) void date_found(ddate v) const { write_field<ddate>(offsetof(dmatch, dm_date), v); }
    __attribute__((always_inline)) void size_found(ULONG v) const { write_field<ULONG>(offsetof(dmatch, dm_size), v); }
    __attribute__((always_inline)) void write_prefix(const void *src) const {
        guest_write(at(0), src, offsetof(dmatch, dm_attr_fnd));
    }
    __attribute__((always_inline)) void read_prefix(void *dst) const {
        guest_read(dst, at(0), offsetof(dmatch, dm_attr_fnd));
    }
    __attribute__((always_inline)) void write_name(const void *src, std::size_t len) const {
        guest_write(at(offsetof(dmatch, dm_name)), src, len);
    }
private:
    __attribute__((always_inline)) dos_far_ptr at(std::size_t off) const {
        return MK_FP(FP_SEG(addr_), static_cast<UWORD>(FP_OFF(addr_) + off));
    }
    template <typename T> __attribute__((always_inline)) T read_field(std::size_t off) const {
        T value;
        guest_read(&value, at(off), sizeof(value));
        return value;
    }
    template <typename T> __attribute__((always_inline)) void write_field(std::size_t off, T value) const {
        guest_write(at(off), &value, sizeof(value));
    }
    dos_far_ptr addr_;
};

class buffer_ref final : private ref_base<buffer> {
public:
    __attribute__((always_inline)) explicit buffer_ref(dos_far_ptr p)
        : ref_base<buffer>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)) {}

    __attribute__((always_inline)) UWORD next() const { return scalar_load<UWORD>(offsetof(buffer, b_next)); }
    __attribute__((always_inline)) void next(UWORD v) const { scalar_store<UWORD>(offsetof(buffer, b_next), v); }
    __attribute__((always_inline)) UWORD prev() const { return scalar_load<UWORD>(offsetof(buffer, b_prev)); }
    __attribute__((always_inline)) void prev(UWORD v) const { scalar_store<UWORD>(offsetof(buffer, b_prev), v); }
    __attribute__((always_inline)) BYTE unit() const { return scalar_load<BYTE>(offsetof(buffer, b_unit)); }
    __attribute__((always_inline)) void unit(BYTE v) const { scalar_store<BYTE>(offsetof(buffer, b_unit), v); }
    __attribute__((always_inline)) BYTE flag() const { return scalar_load<BYTE>(offsetof(buffer, b_flag)); }
    __attribute__((always_inline)) void flag(BYTE v) const { scalar_store<BYTE>(offsetof(buffer, b_flag), v); }
    __attribute__((always_inline)) ULONG blkno() const { return scalar_load<ULONG>(offsetof(buffer, b_blkno)); }
    __attribute__((always_inline)) void blkno(ULONG v) const { scalar_store<ULONG>(offsetof(buffer, b_blkno), v); }
    __attribute__((always_inline)) UBYTE copies() const { return scalar_load<UBYTE>(offsetof(buffer, b_copies)); }
    __attribute__((always_inline)) void copies(UBYTE v) const { scalar_store<UBYTE>(offsetof(buffer, b_copies), v); }
    __attribute__((always_inline)) UWORD offset() const { return scalar_load<UWORD>(offsetof(buffer, b_offset)); }
    __attribute__((always_inline)) void offset(UWORD v) const { scalar_store<UWORD>(offsetof(buffer, b_offset), v); }
    __attribute__((always_inline)) dos_far_ptr dpbp() const { return far_load(offsetof(buffer, b_dpbp)); }
    __attribute__((always_inline)) void dpbp(dos_far_ptr v) const { far_store(offsetof(buffer, b_dpbp), v); }
    __attribute__((always_inline)) UBYTE data8(std::size_t off) const { return scalar_load<UBYTE>(offsetof(buffer, b_buffer) + off); }
    __attribute__((always_inline)) void data8(std::size_t off, UBYTE v) const { scalar_store<UBYTE>(offsetof(buffer, b_buffer) + off, v); }
    __attribute__((always_inline)) UWORD data16(std::size_t off) const { return scalar_load<UWORD>(offsetof(buffer, b_buffer) + off); }
    __attribute__((always_inline)) void data16(std::size_t off, UWORD v) const { scalar_store<UWORD>(offsetof(buffer, b_buffer) + off, v); }
    __attribute__((always_inline)) ULONG data32(std::size_t off) const { return scalar_load<ULONG>(offsetof(buffer, b_buffer) + off); }
    __attribute__((always_inline)) void data32(std::size_t off, ULONG v) const { scalar_store<ULONG>(offsetof(buffer, b_buffer) + off, v); }
    __attribute__((always_inline)) void read_data(std::size_t off, void *dst, std::size_t len) const {
        guest_read_block(addr_ + offsetof(buffer, b_buffer) + static_cast<uint32_t>(off), dst, len);
    }
    __attribute__((always_inline)) void write_data(std::size_t off, const void *src, std::size_t len) const {
        guest_write_block(addr_ + offsetof(buffer, b_buffer) + static_cast<uint32_t>(off), src, len);
    }
    __attribute__((always_inline)) void fill_data(std::size_t off, UBYTE value, std::size_t len) const {
        guest_fill_block(addr_ + offsetof(buffer, b_buffer) + static_cast<uint32_t>(off), value, len);
    }

private:
    __attribute__((always_inline)) dos_far_ptr far_load(std::size_t off) const {
        const UWORD offset = scalar_load<UWORD>(off);
        const UWORD segment = scalar_load<UWORD>(off + sizeof(UWORD));
        return MK_FP(segment, offset);
    }
    __attribute__((always_inline)) void far_store(std::size_t off, dos_far_ptr p) const {
        scalar_store<UWORD>(off, FP_OFF(p));
        scalar_store<UWORD>(off + sizeof(UWORD), FP_SEG(p));
    }
};

} // namespace fdos_guest
