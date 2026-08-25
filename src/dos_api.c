#include <pico.h>
#include <stdio.h>
#include <pc.h>
#include <bios/bios.h>
#include "mem.h"
#include "board_config.h"
#include "psram_init.h"
#include "tsr_callback.h"
#include "ps2kbd_wrapper.h"
#include <math.h>

#define __in_systable(group) __attribute__((section(".dos_api" group)))

extern PC *pc;
extern void arm_elf_process_exit(int status);
extern uint32_t arm_elf_yield(void);
extern bool terminate_requested(void);
extern const void *arm_ez_get_process_info(void);
extern uint32_t DosMemBlockSize(uint16_t para);
extern int DosMemLargest(uint16_t *size);
extern void *arm_native_app_malloc(size_t size);
extern void *arm_native_app_calloc(size_t count, size_t size);
extern void *arm_native_app_realloc(void *ptr, size_t size);
extern void arm_native_app_free(void *ptr);
extern size_t arm_native_app_malloc_largest(void);

int dos_keyboard_get_event_raw(uint32_t flags, int *is_down, int *keycode)
{
    if (!is_down || !keycode || (flags & ~3u))
        return -1;
#ifdef BOARD_HAS_PS2
    return ps2kbd_get_event(is_down, keycode,
                            (flags & 1u) != 0, (flags & 2u) != 0);
#else
    (void)flags;
    return 0;
#endif
}

/*
 * Native-ELF diagnostic latch.
 *
 * Applications write this word directly through system-table slot 101.
 * There is deliberately no callback, DOS interrupt or device service on the
 * write path: one volatile 32-bit store is the complete instrumentation cost.
 * The video core only reads it while generating the normally blank border.
 */
volatile uint32_t dos_diag_code = 0;

#if DIAG
/* FDOS/kernel diagnostics use a separate latch from the native app. */
volatile uint32_t dos_diag_kernel_code = 0;
#endif

/*
 * Full native math/compiler-runtime backend.
 *
 * The exported compiler helpers are the firmware toolchain/libgcc symbols.
 * Native relocatable ELF applications deliberately do not carry a private
 * libgcc copy.  Client-side wrappers tail-branch to these entries so unusual
 * ABI results (notably divmod's r0/r1 pair and complex helpers) are preserved
 * exactly.
 */
extern void __aeabi_fadd(void);
extern void __aeabi_fsub(void);
extern void __aeabi_fmul(void);
extern void __aeabi_fdiv(void);
extern void __aeabi_fcmpeq(void);
extern void __aeabi_fcmpge(void);
extern void __aeabi_fcmpgt(void);
extern void __aeabi_fcmple(void);
extern void __aeabi_fcmplt(void);
extern void __aeabi_fcmpun(void);
extern void __aeabi_i2f(void);
extern void __aeabi_ui2f(void);
extern void __aeabi_f2iz(void);
extern void __aeabi_f2uiz(void);
extern void __aeabi_l2f(void);
extern void __aeabi_ul2f(void);
extern void __aeabi_f2lz(void);
extern void __aeabi_f2ulz(void);

extern void __aeabi_dadd(void);
extern void __aeabi_dsub(void);
extern void __aeabi_dmul(void);
extern void __aeabi_ddiv(void);
extern void __aeabi_dcmpeq(void);
extern void __aeabi_dcmpge(void);
extern void __aeabi_dcmplt(void);
extern void __aeabi_dcmpgt(void);
extern void __aeabi_dcmple(void);
extern void __aeabi_dcmpun(void);
extern void __aeabi_f2d(void);
extern void __aeabi_d2f(void);
extern void __aeabi_i2d(void);
extern void __aeabi_ui2d(void);
extern void __aeabi_d2iz(void);
extern void __aeabi_d2uiz(void);
extern void __aeabi_l2d(void);
extern void __aeabi_ul2d(void);
extern void __aeabi_d2lz(void);
extern void __aeabi_d2ulz(void);

extern void __aeabi_idiv(void);
extern void __aeabi_idivmod(void);
extern void __aeabi_uidiv(void);
extern void __aeabi_uidivmod(void);
extern void __aeabi_lmul(void);
extern void __aeabi_uldivmod(void);
extern void __aeabi_ldivmod(void);
extern void __aeabi_llsr(void);
extern void __aeabi_llsl(void);
extern void __aeabi_lasr(void);
extern void __aeabi_lcmp(void);

extern void __clzsi2(void);
extern void __ctzsi2(void);
extern void __popcountsi2(void);
extern double _Complex __muldc3(double, double, double, double);
extern double _Complex __divdc3(double, double, double, double);
extern float _Complex __mulsc3(float, float, float, float);
extern float _Complex __divsc3(float, float, float, float);
extern void __powisf2(void);
extern void __powidf2(void);

/*
 * GCC normally emits negation inline and the uploaded MOS2 runtime only
 * declared __aeabi_fneg/__aeabi_dneg without providing them.  Export explicit
 * firmware adapters so native applications have a complete EABI surface.
 */
static float dos_math_fneg(float x) { return -x; }
static double dos_math_dneg(double x) { return -x; }

/* MOS2 math-wrapper.c helper surface. */
static uint32_t dos_math_u32_div(uint32_t x, uint32_t y) { return x / y; }
static uint32_t dos_math_u32_rem(uint32_t x, uint32_t y) { return x % y; }
static float dos_math_fff_div(float x, float y) { return x / y; }
static float dos_math_fff_mul(float x, float y) { return x * y; }
static float dos_math_ffu32_mul(float x, uint32_t y) { return x * y; }
static double dos_math_ddd_div(double x, double y) { return x / y; }
static double dos_math_ddd_mul(double x, double y) { return x * y; }
static double dos_math_ddu32_mul(double x, uint32_t y) { return x * y; }
static double dos_math_ddf_mul(double x, float y) { return x * y; }
static float dos_math_ffu32_div(float x, uint32_t y) { return x / y; }
static double dos_math_ddu32_div(double x, uint32_t y) { return x / y; }


PC* __not_in_flash_func(get_PC)() {
    return pc;
}

static uint8_t __not_in_flash_func(dos_phys_read8)(uint32_t addr) {
    return pload8(addr);
}

static uint16_t __not_in_flash_func(dos_phys_read16)(uint32_t addr) {
    return pload16(addr);
}

static uint32_t __not_in_flash_func(dos_phys_read32)(uint32_t addr) {
    return pload32(addr);
}

static void __not_in_flash_func(dos_phys_write8)(uint32_t addr, uint8_t val) {
    pstore8(addr, val);
}

static void __not_in_flash_func(dos_phys_write16)(uint32_t addr, uint16_t val) {
    pstore16(addr, val);
}

static void __not_in_flash_func(dos_phys_write32)(uint32_t addr, uint32_t val) {
    pstore32(addr, val);
}

/*
 * Direct native access to the renderer's VGA backing store.
 *
 * This deliberately exposes the raw backing buffer, not the emulated VGA
 * aperture semantics implemented by vga_mem_write().  It is intended for
 * native applications which know the active gfx_buffer layout and need the
 * shortest possible video-memory write path.
 */
static uint8_t *__not_in_flash_func(dos_video_get_buffer)(uint32_t *size) {
    if (size)
        *size = pc ? (uint32_t)pc->vga_mem_size : 0u;
    return pc ? pc->vga_mem : NULL;
}


static uint32_t __not_in_flash_func(psram_size)(void) {
    return (uint32_t)psram_usable_size();
}

/*
 * Native-app memory primitives.
 *
 * Do not export the firmware libc memcpy/memset entry points directly: their
 * placement is a toolchain/ROM/XIP detail.  Keep this ABI on explicit SRAM
 * functions instead.  The memset core mirrors the proven nf_memset() used by
 * the HDMI driver; memcpy uses word transfers only when source/destination
 * have compatible alignment.
 */
void *__not_in_flash_func(nf_memset)(void *ptr, int value, size_t len)
{
    uint8_t *p = (uint8_t *)ptr;
    uint8_t v8 = (uint8_t)value;

    while (len && ((uintptr_t)p & 3u)) {
        *p++ = v8;
        --len;
    }

    if (len >= 4u) {
        uint32_t v32 = v8;
        v32 |= v32 << 8;
        v32 |= v32 << 16;

        uint32_t *p32 = (uint32_t *)p;
        size_t n32 = len >> 2;
        while (n32--)
            *p32++ = v32;

        p = (uint8_t *)p32;
        len &= 3u;
    }

    while (len--)
        *p++ = v8;

    return ptr;
}

const void *__not_in_flash_func(dos_api_memchr)(const void *src,
                                                    int value,
                                                    size_t len)
{
    const uint8_t *p = (const uint8_t *)src;
    const uint8_t value8 = (uint8_t)value;

    while (len--) {
        if (*p == value8)
            return p;
        ++p;
    }
    return NULL;
}

void *__not_in_flash_func(dos_api_memcpy)(void *dst,
                                          const void *src,
                                          size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0u) {
        while (len && ((uintptr_t)d & 3u)) {
            *d++ = *s++;
            --len;
        }

        if (len >= 4u) {
            uint32_t *d32 = (uint32_t *)d;
            const uint32_t *s32 = (const uint32_t *)s;
            size_t n32 = len >> 2;

            while (n32--)
                *d32++ = *s32++;

            d = (uint8_t *)d32;
            s = (const uint8_t *)s32;
            len &= 3u;
        }
    }

    while (len--)
        *d++ = *s++;

    return dst;
}

static uint32_t dos_api_largest_free_block(void)
{
    uint16_t paragraphs = 0;

    if (DosMemLargest(&paragraphs) != 0)
        return 0;
    return (uint32_t)paragraphs << 4;
}

static void *__not_in_flash_func(dos_api_memmove)(void *dst,
                                                   const void *src,
                                                   size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uintptr_t da = (uintptr_t)d;
    uintptr_t sa = (uintptr_t)s;

    if (len == 0u || da == sa)
        return dst;

    /* Forward copy is safe when the ranges do not overlap, or when the
       destination starts below the source. Reuse the SRAM memcpy path. */
    if (da < sa || da - sa >= len)
        return dos_api_memcpy(dst, src, len);

    /* Overlap with dst inside src: copy backwards. Word transfers are safe
       only when source and destination have the same alignment. */
    d += len;
    s += len;

    if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0u) {
        while (len && ((uintptr_t)d & 3u)) {
            *--d = *--s;
            --len;
        }

        if (len >= 4u) {
            uint32_t *d32 = (uint32_t *)d;
            const uint32_t *s32 = (const uint32_t *)s;
            size_t n32 = len >> 2;

            while (n32--)
                *--d32 = *--s32;

            d = (uint8_t *)d32;
            s = (const uint8_t *)s32;
            len &= 3u;
        }
    }

    while (len--)
        *--d = *--s;

    return dst;
}

static int __not_in_flash_func(dos_api_memcmp)(const void *a,
                                                const void *b,
                                                size_t len)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;

    /* Word-compare equal runs; byte-compare the first unequal word so the
       return sign remains exactly the standard unsigned-char ordering. */
    if ((((uintptr_t)p ^ (uintptr_t)q) & 3u) == 0u) {
        while (len && ((uintptr_t)p & 3u)) {
            if (*p != *q)
                return (int)*p - (int)*q;
            ++p;
            ++q;
            --len;
        }

        while (len >= 4u) {
            uint32_t x = *(const uint32_t *)p;
            uint32_t y = *(const uint32_t *)q;
            if (x != y)
                break;
            p += 4;
            q += 4;
            len -= 4;
        }
    }

    while (len--) {
        if (*p != *q)
            return (int)*p - (int)*q;
        ++p;
        ++q;
    }

    return 0;
}

// To be placed on 0x10100000
unsigned long __in_systable() __aligned(4096) dos_api_table_ptrs[] = {
    (unsigned long)get_PC,
    (unsigned long)handlers,
    (unsigned long)bios_intcall,
    (unsigned long)dos_phys_read8,
    (unsigned long)dos_phys_read16,
    (unsigned long)dos_phys_read32,
    (unsigned long)dos_phys_write8,
    (unsigned long)dos_phys_write16,
    (unsigned long)dos_phys_write32,
    (unsigned long)psram_size,
    (unsigned long)vsnprintf,
    (unsigned long)arm_elf_process_exit,
    (unsigned long)arm_elf_yield,
    (unsigned long)__aeabi_idiv,       /* 13: compatibility with API v6 */
    (unsigned long)__aeabi_idivmod,    /* 14 */
    (unsigned long)__aeabi_uidiv,      /* 15 */
    (unsigned long)__aeabi_lmul,       /* 16 */
    (unsigned long)__aeabi_ldivmod,    /* 17 */
    (unsigned long)__aeabi_uldivmod,   /* 18 */
    (unsigned long)dos_math_u32_div, /* 19 */
    (unsigned long)dos_math_u32_rem, /* 20 */
    (unsigned long)dos_math_fff_div, /* 21 */
    (unsigned long)dos_math_fff_mul, /* 22 */
    (unsigned long)dos_math_ffu32_mul, /* 23 */
    (unsigned long)dos_math_ddd_div, /* 24 */
    (unsigned long)dos_math_ddd_mul, /* 25 */
    (unsigned long)dos_math_ddu32_mul, /* 26 */
    (unsigned long)dos_math_ddf_mul, /* 27 */
    (unsigned long)dos_math_ffu32_div, /* 28 */
    (unsigned long)dos_math_ddu32_div, /* 29 */
    (unsigned long)trunc, /* 30 */
    (unsigned long)floor, /* 31 */
    (unsigned long)pow, /* 32 */
    (unsigned long)sqrt, /* 33 */
    (unsigned long)sin, /* 34 */
    (unsigned long)cos, /* 35 */
    (unsigned long)tan, /* 36 */
    (unsigned long)atan, /* 37 */
    (unsigned long)log, /* 38 */
    (unsigned long)exp, /* 39 */
    (unsigned long)powf, /* 40 */
    (unsigned long)__aeabi_fadd, /* 41 */
    (unsigned long)__aeabi_fsub, /* 42 */
    (unsigned long)__aeabi_fmul, /* 43 */
    (unsigned long)__aeabi_fdiv, /* 44 */
    (unsigned long)dos_math_fneg, /* 45 */
    (unsigned long)__aeabi_fcmpeq, /* 46 */
    (unsigned long)__aeabi_fcmpge, /* 47 */
    (unsigned long)__aeabi_fcmpgt, /* 48 */
    (unsigned long)__aeabi_fcmple, /* 49 */
    (unsigned long)__aeabi_fcmplt, /* 50 */
    (unsigned long)__aeabi_fcmpun, /* 51 */
    (unsigned long)__aeabi_i2f, /* 52 */
    (unsigned long)__aeabi_ui2f, /* 53 */
    (unsigned long)__aeabi_f2iz, /* 54 */
    (unsigned long)__aeabi_f2uiz, /* 55 */
    (unsigned long)__aeabi_l2f, /* 56 */
    (unsigned long)__aeabi_ul2f, /* 57 */
    (unsigned long)__aeabi_f2lz, /* 58 */
    (unsigned long)__aeabi_f2ulz, /* 59 */
    (unsigned long)__aeabi_dadd, /* 60 */
    (unsigned long)__aeabi_dsub, /* 61 */
    (unsigned long)__aeabi_dmul, /* 62 */
    (unsigned long)__aeabi_ddiv, /* 63 */
    (unsigned long)dos_math_dneg, /* 64 */
    (unsigned long)__aeabi_dcmpeq, /* 65 */
    (unsigned long)__aeabi_dcmpge, /* 66 */
    (unsigned long)__aeabi_dcmplt, /* 67 */
    (unsigned long)__aeabi_dcmpgt, /* 68 */
    (unsigned long)__aeabi_dcmple, /* 69 */
    (unsigned long)__aeabi_dcmpun, /* 70 */
    (unsigned long)__aeabi_f2d, /* 71 */
    (unsigned long)__aeabi_d2f, /* 72 */
    (unsigned long)__aeabi_i2d, /* 73 */
    (unsigned long)__aeabi_ui2d, /* 74 */
    (unsigned long)__aeabi_d2iz, /* 75 */
    (unsigned long)__aeabi_d2uiz, /* 76 */
    (unsigned long)__aeabi_l2d, /* 77 */
    (unsigned long)__aeabi_ul2d, /* 78 */
    (unsigned long)__aeabi_d2lz, /* 79 */
    (unsigned long)__aeabi_d2ulz, /* 80 */
    (unsigned long)__aeabi_idivmod, /* 81 */
    (unsigned long)__aeabi_idiv, /* 82 */
    (unsigned long)__aeabi_uidiv, /* 83 */
    (unsigned long)__aeabi_uidivmod, /* 84 */
    (unsigned long)__aeabi_lmul, /* 85 */
    (unsigned long)__aeabi_uldivmod, /* 86 */
    (unsigned long)__aeabi_ldivmod, /* 87 */
    (unsigned long)__aeabi_llsr, /* 88 */
    (unsigned long)__aeabi_llsl, /* 89 */
    (unsigned long)__aeabi_lasr, /* 90 */
    (unsigned long)__aeabi_lcmp, /* 91 */
    (unsigned long)__clzsi2, /* 92 */
    (unsigned long)__ctzsi2, /* 93 */
    (unsigned long)__popcountsi2, /* 94 */
    (unsigned long)__muldc3, /* 95 */
    (unsigned long)__divdc3, /* 96 */
    (unsigned long)__mulsc3, /* 97 */
    (unsigned long)__divsc3, /* 98 */
    (unsigned long)__powisf2, /* 99 */
    (unsigned long)__powidf2, /* 100 */
    (unsigned long)&dos_diag_code, /* 101: native diagnostic latch */
    (unsigned long)vsscanf, /* 102: libc scanner backend */
    (unsigned long)dos_api_memcpy, /* 103: SRAM native-app memcpy */
    (unsigned long)nf_memset, /* 104: shared SRAM memset */
    (unsigned long)dos_api_memcmp, /* 105: SRAM native-app memcmp */
    (unsigned long)terminate_requested, /* 106: native process termination state */
    (unsigned long)arm_ez_get_process_info, /* 107: current EZ process info */
    (unsigned long)DosMemBlockSize, /* 108: DOS block size by data segment */
    (unsigned long)dos_api_memmove, /* 109: SRAM native-app memmove */
    (unsigned long)dos_api_largest_free_block, /* 110: largest free DOS block, bytes */
    (unsigned long)arm_native_app_malloc, /* 111: application malloc */
    (unsigned long)arm_native_app_calloc, /* 112: application calloc */
    (unsigned long)arm_native_app_realloc, /* 113: application realloc */
    (unsigned long)arm_native_app_free, /* 114: application free */
    (unsigned long)arm_native_app_malloc_largest, /* 115: largest application block */
    (unsigned long)dos_video_get_buffer, /* 116: direct gfx_buffer pointer + size */
    (unsigned long)set_tsr0_callback, /* 117: core0 timer callback chain */
    (unsigned long)set_tsr1_callback, /* 118: core1 VGA scanline callback chain */
    (unsigned long)dos_keyboard_get_event_raw, /* 119: native keyboard event queue */
    0
};
