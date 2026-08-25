#ifndef CPU_MEM_H
#define CPU_MEM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "ems.h"

#ifdef __cplusplus
extern "C" {
#endif
void *dos_api_memcpy(void *dst, const void *src, size_t len);
const void *dos_api_memchr(const void *src, int value, size_t len);
void *nf_memset(void *ptr, int value, size_t len);
#ifdef __cplusplus
}
#endif
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
#include "ega128_paging.h"
#endif

#if EMULATE_LTEMS
#define IF_EMS(x) x
#else
#define IF_EMS(x)
#endif

#if defined(EGA128) || defined(VGA128) || defined(MCGA)
extern uint8_t *guest_ram_base;
#ifdef MCGA
#define RAM_PAGES_SIZE (192u << 10)
#else
#define RAM_PAGES_SIZE (128u << 10)
#endif
extern uint8_t ram_pages[RAM_PAGES_SIZE];
#define PC_RAM (guest_ram_base)
#define PC_RAM32 ((uint32_t*)guest_ram_base)
#define EGA128_QSPI_RAM ((uint8_t *)0x11000000u)
#else
#define PC_RAM ((uint8_t *)0x11000000u)
#define PC_RAM32 ((uint32_t *)0x11000000u)
#endif
#define CHECK_RAM_BOARDER_ENABLED 0
extern unsigned long phys_mem_size;
extern void* g_pc;

#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)

#define VGA_WINDOW(addr) (((addr - 0xa0000u) < 0x20000u) | (addr >= 0xe0000000u))

uint8_t iomem_read8(void* iomem, uint32_t addr);
uint16_t iomem_read16(void* iomem, uint32_t addr);
uint32_t iomem_read32(void* iomem, uint32_t addr);
void iomem_write8(void *iomem, uint32_t addr, uint8_t val);
void iomem_write16(void *iomem, uint32_t addr, uint16_t val);
void iomem_write32(void *iomem, uint32_t addr, uint32_t val);
bool iomem_write_string(void *iomem, uint32_t addr, uint32_t buf, int len);
bool iomem_write_string_ptr(void *iomem, uint32_t addr, const uint8_t *buf, int len);
void reset_umb();

/*
 * Разрешение линейного гостевого адреса в host-указатель с учётом окна EMS.
 * В *span возвращается длина непрерывного участка от addr: окно EMS
 * банкуется 16К-страницами (у каждого подокна page frame свой селектор
 * ems_pages[]), поэтому гранула - до ближайшей 16К-границы; вне окна
 * дробление по той же грануле безвредно и упрощает вызывающий цикл.
 *
 * Обязателен для любых НАТИВНЫХ bulk-копий в гостевую память по адресу,
 * который задаёт гость (файловый ввод-вывод DOS, far-примитивы ядра):
 * CPU-путь pload/pstore банкуется сам, а голый PC_RAM+addr кладёт данные
 * в сырую линейную память - после переключения страницы гость видит
 * содержимое EMS-страницы, и данные "исчезают" (симптом: Wolf3D грузит
 * VSWAP-чанки INT 21h-чтением прямо в замапленный page frame и теряет
 * текстуры). Окно VGA (A0000..BFFFF) здесь сознательно НЕ обрабатывается:
 * ядро в него не целится, а запись в видеопамять обязана идти через
 * write86/iomem.
 */
static inline uint8_t *guest_span_ptr_ex(uint32_t addr, uint32_t *span,
                                         bool write_access)
{
    *span = 0x4000u - (addr & 0x3FFFu);
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr)))
        return ems_host_ptr(addr);
#endif
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (unlikely(ega128_paging_active()))
        return ega128_page_ptr(addr, span, write_access);
#else
    (void)write_access;
#endif
    return PC_RAM + addr;
}

static inline uint8_t *guest_span_ptr(uint32_t addr, uint32_t *span)
{
    return guest_span_ptr_ex(addr, span, true);
}

static inline const uint8_t *guest_span_ptr_read(uint32_t addr, uint32_t *span)
{
    return guest_span_ptr_ex(addr, span, false);
}

/* Native <-> guest block copies.  Paging/EMS window boundaries are resolved
 * here once per contiguous span; callers must not open-code pload8/pstore8
 * loops for bulk transfers.  VGA remains device I/O and is handled through
 * iomem rather than exposing a raw host pointer. */
static inline void guest_read_block(uint32_t src, void *dst, size_t len)
{
    uint8_t *out = (uint8_t *)dst;
    while (len) {
        if (unlikely(VGA_WINDOW(src))) {
            *out++ = iomem_read8(g_pc, src++);
            --len;
            continue;
        }
        uint32_t span;
        const uint8_t *p = guest_span_ptr_read(src, &span);
        size_t n = len < span ? len : span;
        dos_api_memcpy(out, p, n);
        out += n;
        src += (uint32_t)n;
        len -= n;
    }
}

static inline void guest_write_block(uint32_t dst, const void *src, size_t len)
{
    const uint8_t *in = (const uint8_t *)src;
    while (len) {
        if (unlikely(VGA_WINDOW(dst))) {
            (void)iomem_write_string_ptr(g_pc, dst, in, (int)len);
            return;
        }
        uint32_t span;
        uint8_t *p = guest_span_ptr(dst, &span);
        size_t n = len < span ? len : span;
        dos_api_memcpy(p, in, n);
        in += n;
        dst += (uint32_t)n;
        len -= n;
    }
}
/* fdos_2fh.c: pick the UMB map matching the selected BIOS */
void umb_select_map(int native_bios, uint32_t rom_start, int vga_bios_loaded);

/* Общий bounce-буфер нативных bulk-обменов вынесен в bulk_bounce.h:
   mem.h включается почти всем деревом, и extern здесь провоцировал бы
   переиспользование буфера из контекстов, где его предположения о
   невложенности не выполняются. */

static inline uint8_t __attribute__((always_inline)) pload8(uint32_t addr)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_read8(g_pc, addr);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        return *ems_host_ptr(addr);
    }
#endif
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (unlikely(guest_ram_base == ram_pages))
        return ega128_mem_read8(addr);
    return EGA128_QSPI_RAM[addr];
#endif
#if CHECK_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return 0xFF;
	}
#endif
	return PC_RAM[addr];
}

static inline uint16_t __attribute__((always_inline)) pload16(uint32_t addr)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_read16(g_pc, addr);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        return *(uint16_t*)ems_host_ptr(addr);
    }
#endif
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (unlikely(guest_ram_base == ram_pages))
        return ega128_mem_read16(addr);
    return *(uint16_t *)(EGA128_QSPI_RAM + addr);
#endif
#if CHECK_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return 0xFFFF;
	}
#endif
	return *(uint16_t*)(PC_RAM + addr);
}

static inline uint32_t __attribute__((always_inline)) pload32(uint32_t addr)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_read32(g_pc, addr);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        return *(uint32_t*)ems_host_ptr(addr);
    }
#endif
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (unlikely(guest_ram_base == ram_pages))
        return ega128_mem_read32(addr);
    return *(uint32_t *)(EGA128_QSPI_RAM + addr);
#endif
#if CHECK_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return 0xFFFFFFFF;
	}
#endif
	return *(uint32_t*)(PC_RAM + addr);
}

static inline void __attribute__((always_inline)) pstore8(uint32_t addr, uint8_t val)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_write8(g_pc, addr, val);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        *ems_host_ptr(addr) = val;
        return;
    }
#endif
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (unlikely(guest_ram_base == ram_pages)) {
        ega128_mem_write8(addr, val);
        return;
    }
    EGA128_QSPI_RAM[addr] = val;
    return;
#endif
#if CHECK_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return;
	}
#endif
	PC_RAM[addr] = val;
}

static inline void __attribute__((always_inline)) pstore16(uint32_t addr, uint16_t val)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_write16(g_pc, addr, val);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        *(uint16_t*)ems_host_ptr(addr) = val;
        return;
    }
#endif
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (unlikely(guest_ram_base == ram_pages)) {
        ega128_mem_write16(addr, val);
        return;
    }
    *(uint16_t *)(EGA128_QSPI_RAM + addr) = val;
    return;
#endif
#if CHECK_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return;
	}
#endif
	*(uint16_t*)(PC_RAM + addr) = val;
}

static inline void __attribute__((always_inline)) pstore32(uint32_t addr, uint32_t val)
{
    if (unlikely(VGA_WINDOW(addr))) {
        return iomem_write32(g_pc, addr, val);
    }
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(addr))) {
        *(uint32_t*)ems_host_ptr(addr) = val;
        return;
    }
#endif
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (unlikely(guest_ram_base == ram_pages)) {
        ega128_mem_write32(addr, val);
        return;
    }
    *(uint32_t *)(EGA128_QSPI_RAM + addr) = val;
    return;
#endif
#if CHECK_RAM_BOARDER_ENABLED
	if (unlikely(addr >= phys_mem_size)) {
		return;
	}
#endif
	*(uint32_t*)(PC_RAM + addr) = val;
}

/* Bulk fill belongs to the paging API for the same reason as block copies:
 * resolve the current guest backing once per contiguous span instead of
 * open-coding byte-at-a-time pstore8 loops at each caller. */
static inline void guest_fill_block(uint32_t dst, uint8_t value, size_t len)
{
    while (len) {
        if (unlikely(VGA_WINDOW(dst))) {
            pstore8(dst++, value);
            --len;
            continue;
        }
        uint32_t span;
        uint8_t *p = guest_span_ptr(dst, &span);
        size_t n = len < span ? len : span;
        nf_memset(p, value, n);
        dst += (uint32_t)n;
        len -= n;
    }
}

/* Search guest memory without paying pload8() dispatch on every byte.
 * Linear RAM has one direct native scan; pageable RAM resolves one cache span
 * at a time.  EMS/VGA still use the generic span/device path because their
 * backing is not one stable linear array. */
static inline size_t guest_find_byte_spans(uint32_t src, uint8_t value, size_t len)
{
    size_t done = 0;
    while (len) {
        if (unlikely(VGA_WINDOW(src))) {
            if (pload8(src) == value)
                return done;
            ++src;
            ++done;
            --len;
            continue;
        }

        uint32_t span;
        const uint8_t *p = guest_span_ptr_read(src, &span);
        size_t n = len < span ? len : span;
        const uint8_t *q = (const uint8_t *)dos_api_memchr(p, value, n);
        if (q)
            return done + (size_t)(q - p);
        src += (uint32_t)n;
        done += n;
        len -= n;
    }
    return SIZE_MAX;
}

static inline size_t guest_find_byte(uint32_t src, uint8_t value, size_t len)
{
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (unlikely(ega128_paging_active()))
        return guest_find_byte_spans(src, value, len);
#endif
#if EMULATE_LTEMS
    if (unlikely(EMS_WINDOW(src) || (len && EMS_WINDOW(src + (uint32_t)len - 1u))))
        return guest_find_byte_spans(src, value, len);
#endif
    if (unlikely(VGA_WINDOW(src) || (len && VGA_WINDOW(src + (uint32_t)len - 1u))))
        return guest_find_byte_spans(src, value, len);

    const uint8_t *base = PC_RAM + src;
    const uint8_t *q = (const uint8_t *)dos_api_memchr(base, value, len);
    return q ? (size_t)(q - base) : SIZE_MAX;
}

static inline size_t guest_strnlen_block(uint32_t src, size_t maxlen)
{
    size_t off = guest_find_byte(src, 0, maxlen);
    return off == SIZE_MAX ? maxlen : off;
}

static inline bool __attribute__((always_inline))
pstore_block(uint32_t dst, uint32_t src, int len)
{
#if EMULATE_LTEMS
    bool src_ems = unlikely(EMS_WINDOW(src) || EMS_WINDOW(src + len - 1));
    bool dst_ems = unlikely(EMS_WINDOW(dst) || EMS_WINDOW(dst + len - 1));

    if (src_ems || dst_ems) {
        if (src_ems && VGA_WINDOW(dst)) {
            /* EMS → VGA: собираем блок из EMS и отправляем через ptr-вариант */
            /* Блок гарантированно выровнен и помещается в одну EMS-страницу
             * (гарантируется вызывающим через count из MOVS_helper2),
             * поэтому ems_host_ptr(src) даёт непрерывный буфер */
            return iomem_write_string_ptr(g_pc, dst, ems_host_ptr(src), len);
        }
        /* EMS ↔ RAM или EMS ↔ EMS: word-wide через pload32/pstore32 */
        while (len > 0 && (dst & 3)) {
            pstore8(dst++, pload8(src++));
            len--;
        }
        int n32 = len >> 2;
        while (n32--) {
            pstore32(dst, pload32(src));
            dst += 4; src += 4;
        }
        len &= 3;
        while (len--) pstore8(dst++, pload8(src++));
        return true;
    }
#endif

#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (unlikely(ega128_paging_active())) {
        while (len >= 4) { pstore32(dst, pload32(src)); dst += 4; src += 4; len -= 4; }
        while (len--) pstore8(dst++, pload8(src++));
        return true;
    }
#endif

    if (likely(!VGA_WINDOW(dst) && !VGA_WINDOW(dst + len - 1))) {
#if CHECK_RAM_BOARDER_ENABLED
        if (unlikely((uint32_t)(dst + len) > phys_mem_size)) return false;
#endif
        const uint8_t *s = PC_RAM + src;
        uint8_t       *d = PC_RAM + dst;
        int n32 = len >> 2;
        int rem = len & 3;
        const uint32_t *s32 = (const uint32_t *)s;
        uint32_t       *d32 = (uint32_t *)d;
        while (n32--) *d32++ = *s32++;
        s = (const uint8_t *)s32;
        d = (uint8_t *)d32;
        while (rem--) *d++ = *s++;
        return true;
    }

    /* RAM → VGA */
    return iomem_write_string(g_pc, dst, src, len);
}

/* Guest memmove.  Forward non-overlapping copies use the optimized paging
 * block primitive above.  Only the genuinely overlapping case walks
 * backwards byte-wise; keeping that fallback here preserves memmove
 * semantics without exposing a host pointer that a page remap could stale. */
static inline void guest_move_block(uint32_t dst, uint32_t src, size_t len)
{
    if (dst == src || len == 0)
        return;

    if (dst > src && dst - src < len) {
        dst += (uint32_t)len;
        src += (uint32_t)len;
        while (len--)
            pstore8(--dst, pload8(--src));
        return;
    }

    if (src > dst && src - dst < len) {
        while (len--)
            pstore8(dst++, pload8(src++));
        return;
    }

    while (len) {
        if (unlikely(VGA_WINDOW(src) || VGA_WINDOW(dst))) {
            pstore8(dst++, pload8(src++));
            --len;
            continue;
        }

        uint32_t src_span, dst_span;
        const uint8_t *s = guest_span_ptr_read(src, &src_span);
        uint8_t *d = guest_span_ptr(dst, &dst_span);
        size_t n = len;
        if (n > src_span) n = src_span;
        if (n > dst_span) n = dst_span;
        dos_api_memcpy(d, s, n);
        src += (uint32_t)n;
        dst += (uint32_t)n;
        len -= n;
    }
}

#endif // CPU_MEM_H
