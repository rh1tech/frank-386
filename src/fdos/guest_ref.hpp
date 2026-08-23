#pragma once

#include <cstddef>
#include <cstdint>

#include "mem.h"

namespace fdos_guest {

using linear_t = uint32_t;

template <typename T>
class ref_base {
protected:
    explicit constexpr ref_base(linear_t addr) : addr_(addr) {}

    template <typename V>
    __attribute__((always_inline)) V scalar_load(std::size_t off) const {
#ifdef EGA128
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
        const auto *p = reinterpret_cast<const T *>(X86_RAM_BASE + addr_);
        const auto *b = reinterpret_cast<const uint8_t *>(p);
        V v;
        __builtin_memcpy(&v, b + off, sizeof(v));
        return v;
#endif
    }

    template <typename V>
    __attribute__((always_inline)) void scalar_store(std::size_t off, V value) const {
#ifdef EGA128
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
        auto *p = reinterpret_cast<T *>(X86_RAM_BASE + addr_);
        auto *b = reinterpret_cast<uint8_t *>(p);
        __builtin_memcpy(b + off, &value, sizeof(value));
#endif
    }

    __attribute__((always_inline)) uint8_t load_byte(std::size_t off) const {
        return scalar_load<uint8_t>(off);
    }

    __attribute__((always_inline)) void store_byte(std::size_t off, uint8_t value) const {
        scalar_store<uint8_t>(off, value);
    }

    linear_t addr_;
};

class mcb_ref final : private ref_base<mcb> {
public:
    explicit constexpr mcb_ref(seg s)
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
#ifdef EGA128
        pstore32(addr_ + offsetof(mcb, m_name), 0);
        pstore32(addr_ + offsetof(mcb, m_name) + 4, 0);
#else
        auto *p = reinterpret_cast<mcb *>(X86_RAM_BASE + addr_);
        __builtin_memset(p->m_name, 0, sizeof(p->m_name));
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
    constexpr scalar_proxy(linear_t addr) : addr_(addr) {}
    __attribute__((always_inline)) operator V() const {
#ifdef EGA128
        if constexpr (sizeof(V) == 1) {
            return static_cast<V>(pload8(addr_));
        } else if constexpr (sizeof(V) == 2) {
            return static_cast<V>(pload16(addr_));
        } else {
            static_assert(sizeof(V) == 4, "guest scalar must be 1, 2 or 4 bytes");
            return static_cast<V>(pload32(addr_));
        }
#else
        V v; __builtin_memcpy(&v, reinterpret_cast<const void *>(X86_RAM_BASE + addr_), sizeof(v)); return v;
#endif
    }
    __attribute__((always_inline)) scalar_proxy &operator=(V v) {
#ifdef EGA128
        if constexpr (sizeof(V) == 1) pstore8(addr_, static_cast<uint8_t>(v));
        else if constexpr (sizeof(V) == 2) pstore16(addr_, static_cast<uint16_t>(v));
        else { static_assert(sizeof(V) == 4, "guest scalar must be 1, 2 or 4 bytes"); pstore32(addr_, static_cast<uint32_t>(v)); }
#else
        __builtin_memcpy(reinterpret_cast<void *>(X86_RAM_BASE + addr_), &v, sizeof(v));
#endif
        return *this;
    }
    __attribute__((always_inline)) scalar_proxy &operator++() { V v = *this; *this = static_cast<V>(v + 1); return *this; }
    __attribute__((always_inline)) scalar_proxy &operator--() { V v = *this; *this = static_cast<V>(v - 1); return *this; }
    __attribute__((always_inline)) scalar_proxy &operator|=(V x) { V v = *this; *this = static_cast<V>(v | x); return *this; }
    __attribute__((always_inline)) scalar_proxy &operator&=(V x) { V v = *this; *this = static_cast<V>(v & x); return *this; }
private:
    linear_t addr_;
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
    explicit constexpr cpu_regs_ref(linear_t addr) : addr_(addr) {}
    __attribute__((always_inline)) scalar_proxy<uint16_t> r16(unsigned i) const { return {addr_ + offsetof(CPU_regs, gprx) + i * sizeof(gprx_t)}; }
    __attribute__((always_inline)) scalar_proxy<uint8_t> r8l(unsigned i) const { return {addr_ + offsetof(CPU_regs, gprx) + i * sizeof(gprx_t)}; }
    __attribute__((always_inline)) scalar_proxy<uint8_t> r8h(unsigned i) const { return {addr_ + offsetof(CPU_regs, gprx) + i * sizeof(gprx_t) + 1u}; }
    __attribute__((always_inline)) scalar_proxy<uint16_t> es() const { return {addr_ + offsetof(CPU_regs, es)}; }
    __attribute__((always_inline)) scalar_proxy<uint16_t> ds() const { return {addr_ + offsetof(CPU_regs, ds)}; }
    __attribute__((always_inline)) scalar_proxy<uint16_t> fs() const { return {addr_ + offsetof(CPU_regs, fs)}; }
    __attribute__((always_inline)) scalar_proxy<uint16_t> gs() const { return {addr_ + offsetof(CPU_regs, gs)}; }
    __attribute__((always_inline)) scalar_proxy<uint32_t> flags() const { return {addr_ + offsetof(CPU_regs, flags)}; }
    __attribute__((always_inline)) flag_proxy carry() const { return {addr_ + offsetof(CPU_regs, flags), 0x0001u}; }
    __attribute__((always_inline)) flag_proxy zero() const { return {addr_ + offsetof(CPU_regs, flags), 0x0040u}; }
    void read_into(CPU_regs &out) const {
#ifdef EGA128
        auto *d = reinterpret_cast<uint8_t *>(&out); for (size_t i=0;i<sizeof(out);++i) d[i]=pload8(addr_+(uint32_t)i);
#else
        __builtin_memcpy(&out, reinterpret_cast<const void *>(X86_RAM_BASE + addr_), sizeof(out));
#endif
    }
    void write_from(const CPU_regs &in) const {
#ifdef EGA128
        const auto *d = reinterpret_cast<const uint8_t *>(&in); for (size_t i=0;i<sizeof(in);++i) pstore8(addr_+(uint32_t)i,d[i]);
#else
        __builtin_memcpy(reinterpret_cast<void *>(X86_RAM_BASE + addr_), &in, sizeof(in));
#endif
    }
private: linear_t addr_;
};

class psp_ref final {
public:
    explicit constexpr psp_ref(seg s) : addr_(static_cast<linear_t>(s) << 4) {}
    __attribute__((always_inline)) dos_far_ptr stack() const { dos_far_ptr v; uint32_t x=scalar_proxy<uint32_t>(addr_+offsetof(psp,ps_stack)); __builtin_memcpy(&v,&x,sizeof(v)); return v; }
    __attribute__((always_inline)) void stack(dos_far_ptr v) const { uint32_t x; __builtin_memcpy(&x,&v,sizeof(x)); scalar_proxy<uint32_t>(addr_+offsetof(psp,ps_stack))=x; }
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

    dos_far_ptr far_value(std::size_t off) const {
        dos_far_ptr v{};
#ifdef EGA128
        auto *d = reinterpret_cast<uint8_t *>(&v);
        for (std::size_t i = 0; i < sizeof(v); ++i)
            d[i] = pload8(addr_ + static_cast<uint32_t>(off + i));
#else
        __builtin_memcpy(&v, reinterpret_cast<const void *>(X86_RAM_BASE + addr_ + off), sizeof(v));
#endif
        return v;
    }
    void far_value(std::size_t off, dos_far_ptr v) const {
#ifdef EGA128
        const auto *d = reinterpret_cast<const uint8_t *>(&v);
        for (std::size_t i = 0; i < sizeof(v); ++i)
            pstore8(addr_ + static_cast<uint32_t>(off + i), d[i]);
#else
        __builtin_memcpy(reinterpret_cast<void *>(X86_RAM_BASE + addr_ + off), &v, sizeof(v));
#endif
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
    explicit constexpr cds_ref(dos_far_ptr p)
        : ref_base<cds>((static_cast<linear_t>(FP_SEG(p)) << 4) + FP_OFF(p)), far_(p) {}

    __attribute__((always_inline)) dos_far_ptr far_ptr() const { return far_; }
    __attribute__((always_inline)) UWORD flags() const { return scalar_load<UWORD>(offsetof(cds, cdsFlags)); }
    __attribute__((always_inline)) dos_far_ptr dpb() const {
        uint32_t x = scalar_load<uint32_t>(offsetof(cds, cdsDpb));
        dos_far_ptr v;
        __builtin_memcpy(&v, &x, sizeof(v));
        return v;
    }

    void load(cds &out) const {
#ifdef EGA128
        auto *d = reinterpret_cast<uint8_t *>(&out);
        for (std::size_t i = 0; i < sizeof(out); ++i)
            d[i] = pload8(addr_ + static_cast<uint32_t>(i));
#else
        __builtin_memcpy(&out, reinterpret_cast<const void *>(X86_RAM_BASE + addr_), sizeof(out));
#endif
    }

    __attribute__((always_inline)) void current_path_byte(std::size_t index, UBYTE value) const {
        if (index < sizeof(((cds *)0)->cdsCurrentPath))
            store_byte(offsetof(cds, cdsCurrentPath) + index, value);
    }

private:
    dos_far_ptr far_;
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
    __attribute__((always_inline)) dos_far_ptr current_ldt() const { return far_load(offsetof(dos_data,current_ldt)); }
    __attribute__((always_inline)) void current_ldt(dos_far_ptr v) const { far_store(offsetof(dos_data,current_ldt),v); }
    __attribute__((always_inline)) dos_far_ptr user_r() const { return far_load(offsetof(dos_data,user_r)); }
    __attribute__((always_inline)) dos_far_ptr prev_user_r() const { return far_load(offsetof(dos_data,prev_user_r)); }
    __attribute__((always_inline)) void user_r(dos_far_ptr v) const { far_store(offsetof(dos_data,user_r),v); }
    __attribute__((always_inline)) void prev_user_r(dos_far_ptr v) const { far_store(offsetof(dos_data,prev_user_r),v); }
private:
    dos_far_ptr far_load(size_t off) const { uint32_t x=scalar_proxy<uint32_t>(addr_+(uint32_t)off); dos_far_ptr v; __builtin_memcpy(&v,&x,sizeof(v)); return v; }
    void far_store(size_t off,dos_far_ptr v) const { uint32_t x; __builtin_memcpy(&x,&v,sizeof(x)); scalar_proxy<uint32_t>(addr_+(uint32_t)off)=x; }
    linear_t addr_;
};

} // namespace fdos_guest
