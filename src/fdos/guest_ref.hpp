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
    __attribute__((always_inline)) dos_far_ptr stack() const { dos_far_ptr v; uint32_t x=scalar_proxy<uint32_t>(addr_+offsetof(psp,ps_stack)); __builtin_memcpy(&v,&x,sizeof(v)); return v; }
    __attribute__((always_inline)) void stack(dos_far_ptr v) const { uint32_t x; __builtin_memcpy(&x,&v,sizeof(x)); scalar_proxy<uint32_t>(addr_+offsetof(psp,ps_stack))=x; }
    __attribute__((always_inline)) UWORD max_files() const {
        return scalar_proxy<UWORD>(addr_ + offsetof(psp, ps_maxfiles));
    }
    __attribute__((always_inline)) dos_far_ptr file_table() const {
        dos_far_ptr v;
        uint32_t x = scalar_proxy<uint32_t>(addr_ + offsetof(psp, ps_filetab));
        __builtin_memcpy(&v, &x, sizeof(v));
        return v;
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
    __attribute__((always_inline)) dos_far_ptr dpb() const {
        const UWORD offset = scalar_load<UWORD>(offsetof(cds, cdsDpb));
        const UWORD segment = scalar_load<UWORD>(offsetof(cds, cdsDpb) + sizeof(UWORD));
        return MK_FP(segment, offset);
    }
    __attribute__((always_inline)) WORD backslash_offset() const {
        return scalar_load<WORD>(offsetof(cds, cdsBackslashOffset));
    }
    __attribute__((always_inline)) WORD join_offset() const {
        return backslash_offset();
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

    void read_struct(sft &out) const {
#ifdef EGA128
        auto *d = reinterpret_cast<uint8_t *>(&out);
        for (std::size_t i = 0; i < sizeof(out); ++i)
            d[i] = pload8(addr_ + static_cast<uint32_t>(i));
#else
        __builtin_memcpy(&out, reinterpret_cast<const void *>(X86_RAM_BASE + addr_), sizeof(out));
#endif
    }

    void store(const sft &in) const {
#ifdef EGA128
        const auto *d = reinterpret_cast<const uint8_t *>(&in);
        for (std::size_t i = 0; i < sizeof(in); ++i)
            pstore8(addr_ + static_cast<uint32_t>(i), d[i]);
#else
        __builtin_memcpy(reinterpret_cast<void *>(X86_RAM_BASE + addr_), &in, sizeof(in));
#endif
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
        uint32_t x = scalar_load<uint32_t>(offsetof(sfttbl, sftt_next));
        dos_far_ptr v;
        __builtin_memcpy(&v, &x, sizeof(v));
        return v;
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
    FDOS_GUEST_RW8(dpb_subunit)
    FDOS_GUEST_RW16(dpb_secsize)
    FDOS_GUEST_RW8(dpb_clsmask)
    FDOS_GUEST_RW8(dpb_shftcnt)
    FDOS_GUEST_RW16(dpb_fatstrt)
    FDOS_GUEST_RW8(dpb_fats)
    FDOS_GUEST_RW16(dpb_dirents)
    FDOS_GUEST_RW16(dpb_data)
    FDOS_GUEST_RW16(dpb_size)
    FDOS_GUEST_RW16(dpb_fatsize)
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
    __attribute__((always_inline)) dos_far_ptr device() const { return far_load(offsetof(dpb, dpb_device)); }

#ifdef WITHFAT32
    FDOS_GUEST_RW16(dpb_xflags)
    FDOS_GUEST_RW16(dpb_xfsinfosec)
    FDOS_GUEST_RW16(dpb_xbackupsec)
    FDOS_GUEST_RW32(dpb_xdata)
    FDOS_GUEST_RW32(dpb_xsize)
    FDOS_GUEST_RW32(dpb_xfatsize)
    FDOS_GUEST_RW32(dpb_xrootclst)
    FDOS_GUEST_RW32(dpb_xcluster)
    __attribute__((always_inline)) void xnfree(ULONG v) const { scalar_store<ULONG>(offsetof(dpb, dpb_nfreeclst_un), v); }
#endif

#undef FDOS_GUEST_RW8
#undef FDOS_GUEST_RW16
#undef FDOS_GUEST_RW32

private:
    __attribute__((always_inline)) dos_far_ptr far_load(std::size_t off) const {
        uint32_t x = scalar_load<uint32_t>(off);
        dos_far_ptr p;
        __builtin_memcpy(&p, &x, sizeof(p));
        return p;
    }
};

class bpb_ref final : private ref_base<bpb> {
public:
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
    FDOS_GUEST_R32(bpb_huge)
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
    FDOS_REQ_SCALAR(BYTE, mcretcode, r_mcretcode)
    FDOS_REQ_SCALAR(UBYTE, nunits, r_nunits)
    FDOS_REQ_SCALAR(UBYTE, firstunit, r_firstunit)
    FDOS_REQ_SCALAR(UBYTE, ndbyte, r_ndbyte)
    FDOS_REQ_SCALAR(UWORD, count, r_count)
    FDOS_REQ_SCALAR(UWORD, start, r_start)
    FDOS_REQ_SCALAR(LONG, huge, r_huge)
    FDOS_REQ_SCALAR(UBYTE, cat, r_cat)
    FDOS_REQ_SCALAR(UBYTE, fun, r_fun)
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

    __attribute__((always_inline)) UWORD attr() const {
        return scalar_load<UWORD>(offsetof(dhdr, dh_attr));
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
    __attribute__((always_inline)) scalar_proxy<UWORD> current_sft_idx() const { return {addr_+offsetof(dos_data,current_sft_idx)}; }
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
    __attribute__((always_inline)) UWORD data16(std::size_t off) const { return scalar_load<UWORD>(offsetof(buffer, b_buffer) + off); }
    __attribute__((always_inline)) ULONG data32(std::size_t off) const { return scalar_load<ULONG>(offsetof(buffer, b_buffer) + off); }

private:
    __attribute__((always_inline)) dos_far_ptr far_load(std::size_t off) const {
        uint32_t x = scalar_load<uint32_t>(off);
        dos_far_ptr p{};
        __builtin_memcpy(&p, &x, sizeof(p));
        return p;
    }
    __attribute__((always_inline)) void far_store(std::size_t off, dos_far_ptr p) const {
        uint32_t x;
        __builtin_memcpy(&x, &p, sizeof(x));
        scalar_store<uint32_t>(off, x);
    }
};

} // namespace fdos_guest
