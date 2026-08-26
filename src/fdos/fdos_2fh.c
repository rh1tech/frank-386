#include "hdrs.h"
#include "request_guest.h"
#include "kernel_guest_proxy.h"
#include "bios/bios.h"
#include "fdos.h"
#include "sdcard.h"
#include "psram_layout.h"

#define XMS_VERSION 0x00
#define REQUEST_HMA 0x01
#define RELEASE_HMA 0x02
#define GLOBAL_ENABLE_A20 0x03
#define GLOBAL_DISABLE_A20 0x04
#define LOCAL_ENABLE_A20 0x05
#define LOCAL_DISABLE_A20 0x06
#define QUERY_A20 0x07

#define QUERY_EMB 0x08
#define ALLOCATE_EMB 0x09
#define RELEASE_EMB 0x0A
#define MOVE_EMB 0x0B

#define LOCK_EMB 0x0C
#define UNLOCK_EMB 0x0D
#define EMB_HANDLE_INFO 0x0E
#define REALLOCATE_EMB 0x0F

#define REQUEST_UMB 0x10
#define RELEASE_UMB 0x11

#define XMS_HANDLES 64
/*
 * EMB pool lives ABOVE the HMA. HMA is FFFF:0010..FFFF:FFFF, i.e. physical
 * 0x100000..0x10FFEF, and is owned by the kernel (DOS=HIGH, buffers).
 * Handing EMB storage out starting at 0x100000 lets any XMS client
 * (e.g. FreeCOM swap) overwrite the kernel/buffers in HMA.
 */

#define to_physical_offset(offset) (((uint16_t)(((offset) >> 16) & 0xFFFF) << 4) + (uint16_t)((offset) & 0xFFFF))

static inline uint16_t xms_memory_kb(void) {
    /* CMOS 17h/18h is initialized by BIOS POST as extended memory above 1 MiB. */
    return (uint16_t)cmos_read(cpu, 0x17) | ((uint16_t)cmos_read(cpu, 0x18) << 8);
}

/* Extended memory reported by CMOS starts at 1 MiB and includes the HMA;
 * the EMB pool excludes the first 64 KiB (HMA). */
static inline uint32_t emb_pool_size(void) {
    uint16_t kb = xms_memory_kb();
    return (kb > 64) ? ((uint32_t)(kb - 64) << 10) : 0;
}

static inline uint8_t *xms_ptr(uint32_t offset) {
    return X86_RAM_BASE + FDOS_XMS_EMB_BASE_PHYS + offset;
}

static inline uint8_t xms_load8(uint32_t offset)
{
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (ega128_paging_active())
        return read86(FDOS_XMS_EMB_BASE_PHYS + offset);
#endif
    return *xms_ptr(offset);
}

static inline void xms_store8(uint32_t offset, uint8_t value)
{
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (ega128_paging_active()) {
        write86(FDOS_XMS_EMB_BASE_PHYS + offset, value);
        return;
    }
#endif
    *xms_ptr(offset) = value;
}

typedef struct {
    uint32_t base;   /* byte offset inside the EMB pool */
    uint32_t size;   /* bytes */
    uint8_t  used;
    uint8_t  locks;
} emb_handle_t;

static emb_handle_t emb_handles[XMS_HANDLES + 1]; /* index 0 is never a valid handle */

static void xms_update_ff_qspi_floor(void)
{
    uint32_t high = 0;
    for (int i = 1; i <= XMS_HANDLES; ++i) {
        if (!emb_handles[i].used)
            continue;
        uint32_t end = emb_handles[i].base + emb_handles[i].size;
        if (end > high)
            high = end;
    }
    sdcard_ff_qspi_cache_set_floor(
        SDCARD_FF_QSPI_OWNER_XMS,
        (void *)((uintptr_t)PSRAM_BASE_ADDR + FDOS_XMS_EMB_BASE_PHYS + high));
}

/* HMA ownership. There is exactly one HMA (FFFF:0010..FFFF:FFFF, ~64K-16).
   REQUEST_HMA (01h) grants it to the FIRST caller and must refuse everyone
   after with 91h.

   Crucially, when DOS=HIGH the KERNEL already owns the HMA (DosLoadedInHMA,
   set by MoveKernelToHMA() - see inithma.c) via a path that never goes through
   this handler. Before this, REQUEST_HMA was a stub that always returned
   success: a guest asking for the HMA on a DOS=HIGH system was told it now
   owned the very region the kernel was running in, and any use of it corrupted
   the kernel. Both claims are consulted below.

   DosLoadedInHMA is read live rather than cached: it is settled inside
   Dosmem() during DoConfig(1), long before the first guest program starts at
   P_0(), so no guest can observe it mid-flight - but reading it directly means
   we do not depend on that ordering staying true. */
static bool xms_hma_taken_by_guest = false;

static bool hma_in_use(void) {
    return DosLoadedInHMA || xms_hma_taken_by_guest;
}

static int emb_free_handle_count(void) {
    int n = 0;
    for (int i = 1; i <= XMS_HANDLES; ++i)
        if (!emb_handles[i].used) n++;
    return n;
}

/* First-fit search for a gap of `size` bytes in the pool. Returns true and
 * the base offset on success. size == 0 is legal per XMS spec. */
static bool emb_find_gap(uint32_t size, uint32_t *out_base) {
    const uint32_t pool = emb_pool_size();
    if (size > pool)
        return false;
    uint32_t cand = 0;
    bool moved = true;
    while (moved) {
        moved = false;
        for (int i = 1; i <= XMS_HANDLES; ++i) {
            const emb_handle_t *h = &emb_handles[i];
            if (!h->used || h->size == 0)
                continue;
            if (cand < h->base + h->size && h->base < cand + (size ? size : 1)) {
                cand = h->base + h->size;
                moved = true;
            }
        }
        if (cand + size > pool)
            return false;
    }
    *out_base = cand;
    return true;
}

static void emb_free_stats(uint32_t *largest, uint32_t *total) {
    const uint32_t pool = emb_pool_size();
    uint32_t used = 0;
    uint32_t big = 0;
    /* total free */
    for (int i = 1; i <= XMS_HANDLES; ++i)
        if (emb_handles[i].used)
            used += emb_handles[i].size;
    *total = (pool > used) ? pool - used : 0;
    /* largest gap: walk gaps between sorted extents (O(n^2), n<=64) */
    uint32_t cursor = 0;
    for (;;) {
        /* find the used extent with the smallest base >= cursor */
        uint32_t next_base = pool, next_end = pool;
        for (int i = 1; i <= XMS_HANDLES; ++i) {
            const emb_handle_t *h = &emb_handles[i];
            if (h->used && h->size && h->base + h->size > cursor && h->base < next_base) {
                next_base = h->base;
                next_end = h->base + h->size;
            }
        }
        if (next_base > cursor && next_base - cursor > big)
            big = next_base - cursor;
        if (next_base == pool)
            break;
        cursor = (next_end > cursor) ? next_end : cursor + 1;
    }
    *largest = big;
}

#ifdef NO_HANDLER_DETECTOR
static bool no_handler(CPU* cpu) {
    cpu_err_msg(cpu, "DOS 2FH - ERROR: no handler defined ");
while(1); // remove it
    return true;
}
#endif

typedef struct __attribute__((packed, aligned)) {
    uint32_t length;
    uint16_t source_handle;
    uint32_t source_offset;
    uint16_t destination_handle;
    uint32_t destination_offset;
} move_data_t;

typedef struct umb {
    uint16_t segment;
    uint16_t size; // paragraphs
    int allocated_paragraphs; // -1 for a chain
} umb_t;

/* ==========================================================================
 * UMB maps. Really occupied above A0000h:
 *   A0000-BFFFF  VGA RAM (VGA_WINDOW in mem.h) - controller window, never UMB
 *   C0000-C7FFF  Video BIOS ROM - loaded only together with an external BIOS
 *                (see load_bios_and_reset); never present in native mode
 *   C8000-CFFFF  free
 *   D0000-DFFFF  free; EMS window when EMULATE_LTEMS (0xD0000..0xE0000)
 *   E0000-EFFFF  free in native mode; an external 128K BIOS image lives here,
 *                and SeaBIOS also puts its tables / option ROM copies there
 *   F0000-FFFFF  BIOS ROM (fake one in native mode) - never UMB.
 *                Fake BIOS layout: F0000 strings, FA000-FC5FF ROM fonts,
 *                FC600/FE000 strings, FEFC7 DPT, FFE00-FFEFF INT trap markers
 *                (any CS:IP with (lin>>8)==0xFFE is trapped in i286_step),
 *                FFF06 IRET, FFF10 INT15/C0h table, FFF30-FFF6F DPT/DPTE,
 *                FFF70-FFF82 executable stubs, FFFF0/FFFF5-FFFFF reset+date.
 *
 * native BIOS : C0000-EFFFF = 192 KB (128 KB with EMULATE_LTEMS)
 * external    : C0000-DFFFF = 128 KB ( 64 KB with EMULATE_LTEMS), minus the
 *               Video BIOS and minus whatever the BIOS image overlaps
 * ========================================================================== */
static umb_t umb_native[] = {
    {0xC000, 0x0080, 0}, {0xC080, 0x0080, 0}, {0xC100, 0x0080, 0}, {0xC180, 0x0080, 0},
    {0xC200, 0x0080, 0}, {0xC280, 0x0080, 0}, {0xC300, 0x0080, 0}, {0xC380, 0x0080, 0},
    {0xC400, 0x0080, 0}, {0xC480, 0x0080, 0}, {0xC500, 0x0080, 0}, {0xC580, 0x0080, 0},
    {0xC600, 0x0080, 0}, {0xC680, 0x0080, 0}, {0xC700, 0x0080, 0}, {0xC780, 0x0080, 0},
    {0xC800, 0x0080, 0}, {0xC880, 0x0080, 0}, {0xC900, 0x0080, 0}, {0xC980, 0x0080, 0},
    {0xCA00, 0x0080, 0}, {0xCA80, 0x0080, 0}, {0xCB00, 0x0080, 0}, {0xCB80, 0x0080, 0},
    {0xCC00, 0x0080, 0}, {0xCC80, 0x0080, 0}, {0xCD00, 0x0080, 0}, {0xCD80, 0x0080, 0},
    {0xCE00, 0x0080, 0}, {0xCE80, 0x0080, 0}, {0xCF00, 0x0080, 0}, {0xCF80, 0x0080, 0},
#ifndef EMULATE_LTEMS
    {0xD000, 0x0080, 0}, {0xD080, 0x0080, 0}, {0xD100, 0x0080, 0}, {0xD180, 0x0080, 0},
    {0xD200, 0x0080, 0}, {0xD280, 0x0080, 0}, {0xD300, 0x0080, 0}, {0xD380, 0x0080, 0},
    {0xD400, 0x0080, 0}, {0xD480, 0x0080, 0}, {0xD500, 0x0080, 0}, {0xD580, 0x0080, 0},
    {0xD600, 0x0080, 0}, {0xD680, 0x0080, 0}, {0xD700, 0x0080, 0}, {0xD780, 0x0080, 0},
    {0xD800, 0x0080, 0}, {0xD880, 0x0080, 0}, {0xD900, 0x0080, 0}, {0xD980, 0x0080, 0},
    {0xDA00, 0x0080, 0}, {0xDA80, 0x0080, 0}, {0xDB00, 0x0080, 0}, {0xDB80, 0x0080, 0},
    {0xDC00, 0x0080, 0}, {0xDC80, 0x0080, 0}, {0xDD00, 0x0080, 0}, {0xDD80, 0x0080, 0},
    {0xDE00, 0x0080, 0}, {0xDE80, 0x0080, 0}, {0xDF00, 0x0080, 0}, {0xDF80, 0x0080, 0},
#endif
    {0xE000, 0x0080, 0}, {0xE080, 0x0080, 0}, {0xE100, 0x0080, 0}, {0xE180, 0x0080, 0},
    {0xE200, 0x0080, 0}, {0xE280, 0x0080, 0}, {0xE300, 0x0080, 0}, {0xE380, 0x0080, 0},
    {0xE400, 0x0080, 0}, {0xE480, 0x0080, 0}, {0xE500, 0x0080, 0}, {0xE580, 0x0080, 0},
    {0xE600, 0x0080, 0}, {0xE680, 0x0080, 0}, {0xE700, 0x0080, 0}, {0xE780, 0x0080, 0},
    {0xE800, 0x0080, 0}, {0xE880, 0x0080, 0}, {0xE900, 0x0080, 0}, {0xE980, 0x0080, 0},
    {0xEA00, 0x0080, 0}, {0xEA80, 0x0080, 0}, {0xEB00, 0x0080, 0}, {0xEB80, 0x0080, 0},
    {0xEC00, 0x0080, 0}, {0xEC80, 0x0080, 0}, {0xED00, 0x0080, 0}, {0xED80, 0x0080, 0},
    {0xEE00, 0x0080, 0}, {0xEE80, 0x0080, 0}, {0xEF00, 0x0080, 0}, {0xEF80, 0x0080, 0},
/*
 * F-segment UMB, derived from the static native-BIOS map (not from CheckIt):
 * the only writes the fake BIOS makes below F9000 are the two identity strings
 * at F0000, and those have been removed (dupes remain at FC600/FE000). The
 * first functional data is the INT 10h static table at F9000, then the ROM
 * fonts at FA000+. So F0000-F8FFF (36 KB) is genuinely free and handed out as
 * UMB; rom_start is set to 0xF9000 in umb_select_map() to keep F9000+ as BIOS.
 */
    // 0xF0000-0xF7FFF (32 KB)
    {0xF000, 0x0080, 0}, {0xF080, 0x0080, 0}, {0xF100, 0x0080, 0}, {0xF180, 0x0080, 0},
    {0xF200, 0x0080, 0}, {0xF280, 0x0080, 0}, {0xF300, 0x0080, 0}, {0xF380, 0x0080, 0},
    {0xF400, 0x0080, 0}, {0xF480, 0x0080, 0}, {0xF500, 0x0080, 0}, {0xF580, 0x0080, 0},
    {0xF600, 0x0080, 0}, {0xF680, 0x0080, 0}, {0xF700, 0x0080, 0}, {0xF780, 0x0080, 0},

    // 0xF8000-0xF8FFF (8 KB) - F9000+ is the INT 10h static table + fonts
    {0xF800, 0x0080, 0}, {0xF880, 0x0080, 0},
};
static umb_t umb_guest[] = {
    {0xC000, 0x0080, 0}, {0xC080, 0x0080, 0}, {0xC100, 0x0080, 0}, {0xC180, 0x0080, 0},
    {0xC200, 0x0080, 0}, {0xC280, 0x0080, 0}, {0xC300, 0x0080, 0}, {0xC380, 0x0080, 0},
    {0xC400, 0x0080, 0}, {0xC480, 0x0080, 0}, {0xC500, 0x0080, 0}, {0xC580, 0x0080, 0},
    {0xC600, 0x0080, 0}, {0xC680, 0x0080, 0}, {0xC700, 0x0080, 0}, {0xC780, 0x0080, 0},
    {0xC800, 0x0080, 0}, {0xC880, 0x0080, 0}, {0xC900, 0x0080, 0}, {0xC980, 0x0080, 0},
    {0xCA00, 0x0080, 0}, {0xCA80, 0x0080, 0}, {0xCB00, 0x0080, 0}, {0xCB80, 0x0080, 0},
    {0xCC00, 0x0080, 0}, {0xCC80, 0x0080, 0}, {0xCD00, 0x0080, 0}, {0xCD80, 0x0080, 0},
    {0xCE00, 0x0080, 0}, {0xCE80, 0x0080, 0}, {0xCF00, 0x0080, 0}, {0xCF80, 0x0080, 0},
#ifndef EMULATE_LTEMS
    {0xD000, 0x0080, 0}, {0xD080, 0x0080, 0}, {0xD100, 0x0080, 0}, {0xD180, 0x0080, 0},
    {0xD200, 0x0080, 0}, {0xD280, 0x0080, 0}, {0xD300, 0x0080, 0}, {0xD380, 0x0080, 0},
    {0xD400, 0x0080, 0}, {0xD480, 0x0080, 0}, {0xD500, 0x0080, 0}, {0xD580, 0x0080, 0},
    {0xD600, 0x0080, 0}, {0xD680, 0x0080, 0}, {0xD700, 0x0080, 0}, {0xD780, 0x0080, 0},
    {0xD800, 0x0080, 0}, {0xD880, 0x0080, 0}, {0xD900, 0x0080, 0}, {0xD980, 0x0080, 0},
    {0xDA00, 0x0080, 0}, {0xDA80, 0x0080, 0}, {0xDB00, 0x0080, 0}, {0xDB80, 0x0080, 0},
    {0xDC00, 0x0080, 0}, {0xDC80, 0x0080, 0}, {0xDD00, 0x0080, 0}, {0xDD80, 0x0080, 0},
    {0xDE00, 0x0080, 0}, {0xDE80, 0x0080, 0}, {0xDF00, 0x0080, 0}, {0xDF80, 0x0080, 0},
#endif
};
static umb_t *umb_blocks       = umb_native;
static int    umb_blocks_count = (int)(sizeof(umb_native) / sizeof(umb_t));
#define UMB_BLOCKS_COUNT umb_blocks_count

static int umb_blocks_allocated = 0;

uint8_t xms_handles = 0;

void reset_umb() {
    for (int i = 0; i < UMB_BLOCKS_COUNT; ++i) {
        umb_blocks[i].allocated_paragraphs = 0;
    }
    umb_blocks_allocated = 0;
    for (int i = 0; i <= XMS_HANDLES; ++i) {
        emb_handles[i].used = 0;
        emb_handles[i].locks = 0;
        emb_handles[i].base = emb_handles[i].size = 0;
    }
    xms_handles = 0;
    xms_update_ff_qspi_floor();
}

/*
 * Select the UMB map. Called from load_bios_and_reset() before bios_post().
 *   native_bios     - 1 if the BIOS is generated by bios_post()
 *   rom_start       - physical start of the external BIOS image
 *                     (0x100000 - bios_size); ignored when native_bios
 *   vga_bios_loaded - 1 if vgabios.bin was really loaded at 0xC0000
 *
 * Both tables are sorted ascending and the excluded parts always sit at the
 * edges (Video BIOS at the bottom, BIOS image at the top), so the window is
 * only narrowed; the tables themselves are never mutated (idempotent).
 */
void umb_select_map(int native_bios, uint32_t rom_start, int vga_bios_loaded)
{
    umb_t *src;
    int n, first, last;

    if (native_bios) {
        src = umb_native;
        n = (int)(sizeof(umb_native) / sizeof(umb_t));
        rom_start = 0xF9000u;   /* native BIOS occupies F9000-FFFFF only
                                   (INT 10h static table + fonts + top data);
                                   F0000-F8FFF is UMB. See umb_native[]. */
        vga_bios_loaded = 0;
    } else {
        src = umb_guest;
        n = (int)(sizeof(umb_guest) / sizeof(umb_t));
    }

    first = 0;
    while (first < n && vga_bios_loaded &&
           ((uint32_t)src[first].segment << 4) < 0xC8000u)
        ++first;

    last = n;
    while (last > first) {
        uint32_t end = ((uint32_t)src[last - 1].segment << 4) +
                       ((uint32_t)src[last - 1].size << 4);
        if (end <= rom_start)
            break;
        --last;
    }

    umb_blocks = src + first;
    umb_blocks_count = last - first;
    reset_umb();
}

int UMB_get_largest(dos_far_ptr driverAddress, UCOUNT *seg, UCOUNT *size)
{
    int res;
    u16 save_ax = CPU_AX;
    u16 save_bx = CPU_BX;
    u16 save_dx = CPU_DX;
    CPU_DX = 0xffff;
    CPU_AX = 0x1000;
    dpb_watch_check_chain("UMB_get_largest 1");
    cpu_far_call(cpu, FP_SEG(driverAddress), FP_OFF(driverAddress));
    if (CPU_BL != 0xb0 || CPU_DX == 0) {
        res = 0;
        goto ret;
    }
    CPU_AX = 0x1000;
    dpb_watch_check_chain("UMB_get_largest 2");
    /* DX оставляем равным largest size */
    cpu_far_call(cpu, FP_SEG(driverAddress), FP_OFF(driverAddress));
    if (CPU_AX != 1) {
        res = 0;
        goto ret;
    }
    *seg = CPU_BX;
    *size = CPU_DX;
    res = CPU_AX;
ret:
    dpb_watch_check_chain("UMB_get_largest 3");
    CPU_AX = save_ax;
    CPU_BX = save_bx;
    CPU_DX = save_dx;
    return res;
}

static inline void xms_move_to(uint32_t destination, uint32_t source, uint32_t length) {
    dpb_watch_check_chain("xms_move_to 1");
    while (length--)
        xms_store8(destination++, read86(source++));
    dpb_watch_check_chain("xms_move_to 2");
}
 
static inline void xms_move_from(uint32_t source, uint32_t destination, uint32_t length) {
    dpb_watch_check_chain("xms_move_from 1");
    while (length--)
        write86(destination++, xms_load8(source++));
    dpb_watch_check_chain("xms_move_from 2");
}

static inline void xms_move_mem_to_mem(uint32_t destination, uint32_t source, uint32_t length) {
    dpb_watch_check_chain("xms_move_mem_to_mem 1");

    /* Handle-0 operands are guest real-mode addresses.  They must go through
       the normal memory accessors: a raw PC_RAM/X86_RAM_BASE memmove bypasses
       EMS page-frame banking (and any other mapped low-memory window).

       Preserve memmove semantics for overlapping conventional-memory ranges. */
    if (destination > source && destination - source < length) {
        source += length;
        destination += length;
        while (length--) {
            --source;
            --destination;
            write86(destination, read86(source));
        }
    } else {
        while (length--)
            write86(destination++, read86(source++));
    }

    dpb_watch_check_chain("xms_move_mem_to_mem 2");
}

static inline void xms_move_xms_to_xms(uint32_t destination, uint32_t source, uint32_t length) {
    dpb_watch_check_chain("xms_move_xms_to_xms 1");
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (ega128_paging_active()) {
        if (destination > source && destination - source < length) {
            source += length;
            destination += length;
            while (length--)
                xms_store8(--destination, xms_load8(--source));
        } else {
            while (length--)
                xms_store8(destination++, xms_load8(source++));
        }
    } else
#endif
    {
        memmove(xms_ptr(destination), xms_ptr(source), length);
    }
    dpb_watch_check_chain("xms_move_xms_to_xms 2");
}

const umb_t *get_largest_free_umb_block(uint16_t *psz) {
    const umb_t *best = NULL;
    int best_length = 0;
    int i = 0;
    while (i < UMB_BLOCKS_COUNT) {
        if (0 == umb_blocks[i].allocated_paragraphs) {
            int j = i;
            int length = 0;
            while (j < UMB_BLOCKS_COUNT && umb_blocks[j].allocated_paragraphs == 0) {
                if (j > i) {
                    const uint16_t expected_segment = umb_blocks[j - 1].segment + umb_blocks[j - 1].size;
                    if (umb_blocks[j].segment != expected_segment)
                        break;
                }
                length += umb_blocks[j].size;
                j++;
            }
            if (length > best_length) {
                best = &umb_blocks[i];
                best_length = length;
            }
            i = j;
        } else {
            i++;
        }
    }
    *psz = best_length;
    return best;
}

umb_t *get_free_umb_block(const uint16_t size) {
    umb_t *best = NULL;
    int best_size = 0;
    int i = 0;
    while (i < UMB_BLOCKS_COUNT) {
        if (umb_blocks[i].allocated_paragraphs != 0) {
            i++;
            continue;
        }
        uint16_t total_size = 0;
        int j = i;
        while (j < UMB_BLOCKS_COUNT && umb_blocks[j].allocated_paragraphs == 0) {
            if (j > i) {
                const uint16_t expected_segment = umb_blocks[j - 1].segment + umb_blocks[j - 1].size;
                if (umb_blocks[j].segment != expected_segment)
                    break;
            }
            total_size += umb_blocks[j].size;
            j++;
        }
        if (total_size >= size) {
            if (best == NULL || total_size < best_size) {
                best = &umb_blocks[i];
                best_size = total_size;
            }
        }
        i = j; // skip tested block
    }
    return best;
}

static bool xms_handler(CPU* cpu, bios_callback_params_t* params) {
//    printf("xms_handler(%02xh)\n", CPU_AH);
    switch (CPU_AH) {
        case XMS_VERSION:
            // Get XMS Version
            CPU_AX = 0x0200; // XMS spec version 2.00
            CPU_BX = 0x0206; // internal driver revision (we report HIMEM 2.06)
            CPU_DX = 0x0001; // HMA exists
            /* BX is the version, so BL must NOT be zeroed here - it is part of
               the return value, unlike every other function below. */
            break;
        case REQUEST_HMA:
            /* DX = bytes needed (an application passes FFFFh; a TSR passes its
               own size, which HIMEM compares against /HMAMIN=). We do not
               enforce a minimum, but we DO enforce single ownership, including
               the kernel's own DOS=HIGH claim. */
            if (hma_in_use()) {
                CPU_AX = 0;
                CPU_BL = 0x91;   /* HMA already in use */
            } else {
                xms_hma_taken_by_guest = true;
                CPU_AX = 1;
                CPU_BL = 0;
            }
            break;
        case RELEASE_HMA:
            /* The kernel's HMA (DOS=HIGH) is never released through here: it is
               resident for the life of the system. Only a guest that itself
               obtained the HMA may give it back. */
            if (!xms_hma_taken_by_guest) {
                /* Either nobody holds it, or the KERNEL does (DOS=HIGH) - and
                   the kernel's HMA is resident for the life of the system, so
                   a guest can never release it. */
                CPU_AX = 0;
                CPU_BL = 0x93;   /* HMA not allocated by the caller */
            } else {
                xms_hma_taken_by_guest = false;
                CPU_AX = 1;
                CPU_BL = 0;
            }
            break;
        case GLOBAL_ENABLE_A20:
        case LOCAL_ENABLE_A20:
            // Local Enable A20
            CPU_AX = 1; // Success
            CPU_BL = 0;
            cpu_set_a20(cpu, 1);
            break;
        case GLOBAL_DISABLE_A20:
        case LOCAL_DISABLE_A20:
            /* A20 is permanently enabled on this emulated machine. */
            CPU_AX = 0;
            CPU_BL = 0x82;   /* A20 error */
            break;
        case QUERY_A20:
            /* 07h: AX = 1 if A20 is enabled, 0 if disabled (cpu_get_a20()
               returns exactly that). BL must be 00h on success - it was left
               holding whatever the caller passed in. */
            CPU_AX = cpu_get_a20(cpu);
            CPU_BL = 0;
            break;
        case QUERY_EMB: { // 08h
            uint32_t largest, total;
            emb_free_stats(&largest, &total);
            CPU_AX = (uint16_t)(largest >> 10); /* largest free block, KB */
            CPU_DX = (uint16_t)(total >> 10);   /* total free EMB, KB */
            CPU_BL = (largest == 0) ? 0xA0 : 0;
            break;
        }
        case ALLOCATE_EMB: { // Allocate Extended Memory Block (Function 09h), DX = size in KB
            int hnd = 0;
            for (int i = 1; i <= XMS_HANDLES; ++i)
                if (!emb_handles[i].used) { hnd = i; break; }
            if (hnd == 0) {
                CPU_AX = 0;
                CPU_BL = 0xA1; /* out of handles */
                break;
            }
            const uint32_t size = (uint32_t)CPU_DX << 10;
            uint32_t base = 0;
            if (!emb_find_gap(size, &base)) {
                CPU_AX = 0;
                CPU_BL = 0xA0; /* out of memory */
                break;
            }
            emb_handles[hnd].used = 1;
            emb_handles[hnd].locks = 0;
            emb_handles[hnd].base = base;
            emb_handles[hnd].size = size;
            xms_handles++;
            xms_update_ff_qspi_floor();
            CPU_DX = (uint16_t)hnd;
            CPU_AX = 1;
            CPU_BL = 0;
            break;
        }
        case RELEASE_EMB: { // 0Ah, DX = handle
            const uint16_t hnd = CPU_DX;
            if (hnd == 0 || hnd > XMS_HANDLES || !emb_handles[hnd].used) {
                CPU_AX = 0;
                CPU_BL = 0xA2; /* invalid handle */
                break;
            }
            if (emb_handles[hnd].locks) {
                CPU_AX = 0;
                CPU_BL = 0xAB; /* block is locked */
                break;
            }
            emb_handles[hnd].used = 0;
            emb_handles[hnd].base = emb_handles[hnd].size = 0;
            if (xms_handles) xms_handles--;
            xms_update_ff_qspi_floor();
            CPU_AX = 1;
            CPU_BL = 0;
            break;
        }
        case MOVE_EMB: {
            // Move Extended Memory Block (Function 0Bh)
            move_data_t move_data;
            const uint32_t struct_offset = ((uint32_t) CPU_DS << 4) + CPU_SI;

            /* Load the packed XMS move descriptor field-by-field.  Apart from
             * avoiding type-punning through uint16_t *, this keeps every guest
             * access on readw86() -> pload16(), which is required when EGA128
             * paging is active. */
            move_data.length =
                (uint32_t)readw86(struct_offset + 0) |
                ((uint32_t)readw86(struct_offset + 2) << 16);
            move_data.source_handle = readw86(struct_offset + 4);
            move_data.source_offset =
                (uint32_t)readw86(struct_offset + 6) |
                ((uint32_t)readw86(struct_offset + 8) << 16);
            move_data.destination_handle = readw86(struct_offset + 10);
            move_data.destination_offset =
                (uint32_t)readw86(struct_offset + 12) |
                ((uint32_t)readw86(struct_offset + 14) << 16);
            
            /* Validate handles / offsets and rebase EMB offsets onto the
             * per-handle base inside the pool. Real-mode (handle==0) sides
             * are seg:off far pointers. */
            uint8_t err = 0;
            if (move_data.length & 1u)
                err = 0xA7; /* XMS 2.0: move length must be even */
            if (move_data.source_handle) {
                const uint16_t h = move_data.source_handle;
                if (h > XMS_HANDLES || !emb_handles[h].used)
                    err = 0xA3; /* invalid source handle */
                else if (move_data.source_offset + move_data.length > emb_handles[h].size ||
                         move_data.source_offset + move_data.length < move_data.source_offset)
                    err = 0xA4; /* invalid source offset */
                else
                    move_data.source_offset += emb_handles[h].base;
            } else {
                /* Real-mode side (handle 0) is a seg:off far pointer into
                   conventional memory. Nothing rebases or bounds it, so a
                   client can point it just under 1 MB with a large length and
                   have the move run straight through the HMA and into the EMB
                   pool (or, for a write, over another handle's data). Real
                   HIMEM confines handle-0 transfers to the low 1 MB; do the
                   same. phys + length must not cross the 1 MB line. */
                move_data.source_offset = to_physical_offset(move_data.source_offset);
                if (move_data.source_offset > 0x100000ul ||
                    move_data.source_offset + move_data.length > 0x100000ul ||
                    move_data.source_offset + move_data.length < move_data.source_offset)
                    err = 0xA4; /* invalid source offset */
            }
            if (!err && move_data.destination_handle) {
                const uint16_t h = move_data.destination_handle;
                if (h > XMS_HANDLES || !emb_handles[h].used)
                    err = 0xA5; /* invalid destination handle */
                else if (move_data.destination_offset + move_data.length > emb_handles[h].size ||
                         move_data.destination_offset + move_data.length < move_data.destination_offset)
                    err = 0xA6; /* invalid destination offset */
                else
                    move_data.destination_offset += emb_handles[h].base;
            } else if (!err) {
                /* Same bound as the source side above. */
                move_data.destination_offset = to_physical_offset(move_data.destination_offset);
                if (move_data.destination_offset > 0x100000ul ||
                    move_data.destination_offset + move_data.length > 0x100000ul ||
                    move_data.destination_offset + move_data.length < move_data.destination_offset)
                    err = 0xA6; /* invalid destination offset */
            }
            #if XMS_DEBUG
            printf("XMS MOVE: len=%lu sh=%04x so=%08lx dh=%04x do=%08lx phys_dst=%06lx%s\n",
                (unsigned long)move_data.length,
                move_data.source_handle,
                (unsigned long)move_data.source_offset,
                move_data.destination_handle,
                (unsigned long)move_data.destination_offset,
                (unsigned long)(move_data.destination_handle
                    ? FDOS_XMS_EMB_BASE_PHYS + move_data.destination_offset
                    : move_data.destination_offset),
                err ? " REJECTED" : "");
            #endif
            if (err) {
                CPU_AX = 0;
                CPU_BL = err;
                break;
            }

            if (!move_data.source_handle && !move_data.destination_handle) {
                xms_move_mem_to_mem(move_data.destination_offset, move_data.source_offset, move_data.length);
            } else if (!move_data.source_handle) {
                xms_move_to(move_data.destination_offset, move_data.source_offset, move_data.length);
            } else if (!move_data.destination_handle) {
                const uint32_t src = move_data.source_offset;
                const uint32_t dst = move_data.destination_offset;
                const uint32_t len = move_data.length;
                xms_move_from(src, dst, len);
#if XMS_DEBUG
                uint32_t first_bad = len;
                for (uint32_t i = 0; i < len; ++i) {
                    uint8_t expected = xms_load8(src + i);
                    uint8_t actual = read86(dst + i);
                    if (expected != actual) {
                        first_bad = i;
                        printf("XMS RESTORE MISMATCH:"
                               " off=%08lx src=%02x dst=%02x"
                               " phys=%06lx\n",
                               (unsigned long)i,
                               expected, actual,
                               (unsigned long)(dst + i));
                        break;
                    }
                }
                if (first_bad == len)
                    printf("XMS RESTORE VERIFIED: len=%lu dst=%06lx\n",
                           (unsigned long)len, (unsigned long)dst);
#endif
            } else {
                xms_move_xms_to_xms(move_data.destination_offset, move_data.source_offset, move_data.length);
            }
            CPU_AX = 1;
            CPU_BL = 0;
            break;
        }
        case REQUEST_UMB: {
            // Request Upper Memory Block (Function 10h):
            if (CPU_DX == 0xFFFF) {
                /*
                 * Probe largest available block by requesting an
                 * impossibly large UMB. XMS returns B0h and the
                 * largest available size in DX.
                 */
                uint16_t sz = 0;
                const umb_t *umb_block = get_largest_free_umb_block(&sz);
                CPU_AX = 0;
                CPU_DX = sz;
                if (umb_block != NULL && sz != 0) {
                    CPU_BL = 0xB0;
                } else {
                    CPU_BL = 0xB1;
                }
                break;
            } else {
                const uint16_t requested_size = CPU_DX;
                umb_t *umb_block = get_free_umb_block(requested_size);
                if (umb_block != NULL) {
                    int unmarked_size = requested_size;
                    CPU_BX = umb_block->segment;
                    CPU_AX = 0x0001;
                    umb_t *ub = umb_block;
                    int total_allocated = 0;
                    while (unmarked_size > 0) {
                        total_allocated += umb_block->size;
                        umb_block->allocated_paragraphs = -1;
                        umb_blocks_allocated++;
                        unmarked_size -= umb_block->size;
                        umb_block++;
                    }
                    ub->allocated_paragraphs = total_allocated;
                    CPU_DX = total_allocated;
                    break;
                }
            }

            uint16_t sz = 0;
            get_largest_free_umb_block(&sz);
            CPU_AX = 0x0000;
            CPU_DX = sz;
            CPU_BL = umb_blocks_allocated >= UMB_BLOCKS_COUNT ? 0xB1 : 0xB0;
            break;
        }
        case RELEASE_UMB: {
            // Release Upper Memory Block (Function 11h), DX = UMB segment.
            bool released = false;
            for (int i = 0; i < UMB_BLOCKS_COUNT; ++i)
                if (umb_blocks[i].segment == CPU_DX && umb_blocks[i].allocated_paragraphs > 0) {
                    int par = umb_blocks[i].allocated_paragraphs;
                    while (par > 0 && i < UMB_BLOCKS_COUNT) {
                        umb_blocks[i].allocated_paragraphs = 0;
                        par -= umb_blocks[i++].size;
                        umb_blocks_allocated--;
                    }
                    released = true;
                    break;
                }

            if (released) {
                CPU_AX = 0x0001; // Success
                CPU_BL = 0;
            } else {
                CPU_AX = 0x0000; // Failure
                CPU_BL = 0xB2; // Invalid UMB segment
            }
            break;
        }
        case LOCK_EMB: { // 0Ch, DX = handle -> DX:BX = 32-bit physical address
            const uint16_t hnd = CPU_DX;
            if (hnd == 0 || hnd > XMS_HANDLES || !emb_handles[hnd].used) {
                CPU_AX = 0;
                CPU_BL = 0xA2;
                break;
            }
            if (emb_handles[hnd].locks == 0xFF) {
                CPU_AX = 0;
                CPU_BL = 0xAC; /* lock count overflow */
                break;
            }
            emb_handles[hnd].locks++;
            const uint32_t phys = FDOS_XMS_EMB_BASE_PHYS + emb_handles[hnd].base;
            CPU_DX = (uint16_t)(phys >> 16);
            CPU_BX = (uint16_t)(phys & 0xFFFF);
            CPU_AX = 1;
            break;
        }
        case UNLOCK_EMB: { // 0Dh, DX = handle
            const uint16_t hnd = CPU_DX;
            if (hnd == 0 || hnd > XMS_HANDLES || !emb_handles[hnd].used) {
                CPU_AX = 0;
                CPU_BL = 0xA2;
                break;
            }
            if (emb_handles[hnd].locks == 0) {
                CPU_AX = 0;
                CPU_BL = 0xAA; /* block is not locked */
                break;
            }
            emb_handles[hnd].locks--;
            CPU_AX = 1;
            CPU_BL = 0;
            break;
        }
        case EMB_HANDLE_INFO: { // 0Eh, DX = handle -> BH=locks, BL=free handles, DX=size KB
            const uint16_t hnd = CPU_DX;
            if (hnd == 0 || hnd > XMS_HANDLES || !emb_handles[hnd].used) {
                CPU_AX = 0;
                CPU_BL = 0xA2;
                break;
            }
            CPU_BH = emb_handles[hnd].locks;
            CPU_BL = (uint8_t)emb_free_handle_count();
            CPU_DX = (uint16_t)(emb_handles[hnd].size >> 10);
            CPU_AX = 1;
            break;
        }
        case REALLOCATE_EMB: { // 0Fh, BX = new size KB, DX = handle
            const uint16_t hnd = CPU_DX;
            if (hnd == 0 || hnd > XMS_HANDLES || !emb_handles[hnd].used) {
                CPU_AX = 0;
                CPU_BL = 0xA2;
                break;
            }
            if (emb_handles[hnd].locks) {
                CPU_AX = 0;
                CPU_BL = 0xAB; /* block is locked */
                break;
            }
            const uint32_t new_size = (uint32_t)CPU_BX << 10;
            emb_handle_t *h = &emb_handles[hnd];
            if (new_size <= h->size) {
                h->size = new_size;
                xms_update_ff_qspi_floor();
                CPU_AX = 1;
                CPU_BL = 0;
                break;
            }
            /* try to grow in place: temporarily hide the handle and search */
            const uint32_t old_base = h->base, old_size = h->size;
            h->used = 0;
            uint32_t base = 0;
            bool ok = emb_find_gap(new_size, &base);
            h->used = 1;
            if (!ok) {
                CPU_AX = 0;
                CPU_BL = 0xA0; /* out of memory */
                break;
            }
            if (base != old_base && old_size)
                xms_move_xms_to_xms(base, old_base, old_size);
            h->base = base;
            h->size = new_size;
            xms_update_ff_qspi_floor();
            CPU_AX = 1;
            CPU_BL = 0;
            break;
        }
        default:
            // Unhandled function
            CPU_AX = 0x0000; // Function not supported
            CPU_BL = 0x80; // Function not implemented
            break;
    }

    SET_IP ( getmem8(CPU_SS, CPU_SP)
           | ((uint16_t)getmem8(CPU_SS, (CPU_SP + 1) & 0xffff) << 8) );
    SET_CS ( getmem8(CPU_SS, (CPU_SP + 2) & 0xffff)
           | ((uint16_t)getmem8(CPU_SS, (CPU_SP + 3) & 0xffff) << 8) );
    CPU_SP += 4;
//    printf("xms_handler RETF to %04x:%04x\n", CPU_CS, CPU_IP);
    return false;
}

bool fdos_2fh(CPU* cpu) {
    static bool handler_installed = false;
    static bios_callback_params_t params = {
        .callback = xms_handler,
        .expected_cs = 0xF000,
        .expected_ip = 0xFEFF,
        .done = false,
        .owner = "INT 2fH"
    };

    /*
     * Windows enhanced-mode installation check.
     * No Windows/386 or Windows 3.x enhanced mode is running.
     */
    if (CPU_AX == 0x1000) {
        /*
         * SHARE.EXE installation check.
         *
         * SHARE is intentionally not implemented on this single-user
         * target.  Return the documented "not installed" result rather
         * than falling into the generic unsupported-multiplex path.
         */
        CPU_AL = 0x00;
        cf = 0;
    } else
    if (CPU_AH == 0x11) {
        /*
         * Network redirector (INT 2Fh AH=11h).
         *
         * There is no redirector in the native DOS build, and no network
         * hardware to hang one off.  AL=00h on the AX=1100h installation
         * check is how DOS programs tell this normal condition from a broken
         * INT 2Fh service; every other 11xxh subfunction is answered as an
         * unhandled multiplex call (CF set, registers untouched) - which is
         * what a chain with no redirector in it looks like.
         */
        if (CPU_AL == 0x00) {
            CPU_AL = 0x00;      /* not installed */
            cf = 0;
        } else {
            cf = 1;
        }
    } else
    if (CPU_AX == 0x122A) {
        /* FASTOPEN entry-point registration.  FreeDOS itself implements
         * this as a successful no-op when FASTOPEN support is absent. */
        cf = 0;
    }
    else
    if (CPU_AX == 0x122b) {
        /*
         * Internal Device I/O Control wrapper.
         *
         * Upstream accepts BP=4400h..44FFh, moves BP to AX and invokes
         * DosDevIOctl().  The current port's DosDevIOctl() uses live
         * CPU_* registers, so no synthetic register structure is needed.
         */
        COUNT rc;

        if (CPU_BP < 0x4400 || CPU_BP > 0x44ff) {
            CPU_AX = (UWORD)-DE_INVLDFUNC;
            cf = 1;
        } else {
            CPU_AX = CPU_BP;
            rc = DosDevIOctl();

            if (rc < SUCCESS) {
                CPU_AX = (UWORD)-rc;
                if (rc != DE_DEVICE && rc != DE_ACCESS)
                    fdos_dos_set_crit_err_code(CPU_AX);
                cf = 1;
            } else {
                cf = 0;
            }
        }
    }
    else
    if (CPU_AX == 0x122C) {
        /* Return the second device in the chain, skipping NUL. */
        {
            dos_far_ptr next = fdos_lol_nul_next();
            CPU_BX = FP_SEG(next);
            CPU_AX = FP_OFF(next);
        }
        cf = 0;
    }
    else
    if (CPU_AX == 0x122D) {
        /* DOS internal: get the current extended error code. */
        CPU_AX = fdos_dos_crit_err_code();
        cf = 0;
    }
    else
    if (CPU_AX == 0x122E) {
        /* Error-table address API.  Original FreeDOS currently ignores it
         * and returns success for compatibility. */
        cf = 0;
    }
    else
    if (CPU_AX == 0x122F) {
        /* Set/reset the DOS version returned by INT 21h/AH=30h. */
        if (CPU_DX)
            fdos_lol_set_setver(CPU_DL, CPU_DH);
        else
            fdos_lol_set_setver(fdos_lol_os_major(), fdos_lol_os_minor());
        cf = 0;
    }
    else
    if (CPU_AX == 0x1680) {
        /*
         * Release current virtual-machine time slice / DOS idle call.
         * In a non-multitasking environment this is a successful no-op.
         */
        CPU_AL = 0x00;
        cf = 0;
    } else
    if (CPU_AX == 0x1600) {
        CPU_AL = 0x00;
    } else
    if (CPU_AX == 0x1700) {
        // AX = 1700h if this version of WINOLDAP doesn't support clipboard
        //CPU_AX = 0x1700;
    } else
    if (CPU_AX == 0x4B02 && CPU_BX == 0x0000) {
        SET_ES (0); CPU_DI = 0; // ES:DI = 0000h:0000h if task switcher not loaded
        cf = 1;
    } else
    if (CPU_AX == 0xAE00) {
        /*
        * INT 2F/AX=AE00h - DOS 3.3+ internal
        * INSTALLABLE COMMAND - installation check.
        *
        * Return AL=FFh to report that the installable-command interface is
        * present.  The actual CONFIG.SYS INSTALL/INSTALLHIGH queue/execution
        * is already handled in config.c by CmdInstall()/CmdInstallHigh() and
        * DoInstall(); this function is only the multiplex installation probe.
        */
        CPU_AL = 0xFF;
    }
    else
    if (CPU_AX == 0xAE01) {
        /*
         * INT 2F/AX=AE01h - DOS 3.3+ internal
         * INSTALLABLE COMMAND - execute pending INSTALL=/INSTALLHIGH=
         * commands collected while parsing CONFIG.SYS.
         */
        DoInstall();
        CPU_AL = 0x00;
        cf = 0;
    }
    else
    if (CPU_AX == 0x121F) {
        /* Build SDA TempCDS without materialising source/destination CDS. */
        const uint32_t caller_arg_addr = stk_lin(CPU_SS, CPU_SP, 6);
        const UBYTE drive_letter = (UBYTE)readw86(caller_arg_addr);
        const int drive = (drive_letter & 0x1f) - 1;
        const dos_far_ptr tmp_fp =
            drive < 0 ? MK_FP(0, 0) : fdos_temp_cds_build(drive_letter, (unsigned)drive);

        if (far_is_null(tmp_fp)) {
            cf = 1;
        } else {
            CPU_CX = sizeof(struct cds);
            SET_ES(FP_SEG(tmp_fp));
            CPU_DI = FP_OFF(tmp_fp);
            cf = 0;
        }
    }
    else
    if (CPU_AX == 0x1200) {
        /* DOS internal services installation check. */
        CPU_AL = 0xff;
        cf = 0;
    }
    else
    if (CPU_AX == 0x1202) {
        /*
         * Get interrupt vector without re-entering INT 21h.
         * Input: CL = vector number.  Output: ES:BX = handler.
         */
        dos_far_ptr vec = getvec(CPU_CL);
        SET_ES(FP_SEG(vec));
        CPU_BX = FP_OFF(vec);
        cf = 0;
    }
    else
    if (CPU_AX == 0x1203) {
        /* Return the segment containing DOS fixed data / NUL header. */
        SET_DS(FP_SEG(x86_FIXED_DATA));
        cf = 0;
    }
    else
    if (CPU_AX == 0x1205) {
        /*
         * Kernel-internal character output.  Preserve the caller's
         * register set: character/device paths may execute guest BIOS or
         * driver code, while upstream runs this service against its saved
         * int2f register frame.
         */
        CPU_regs saved;
        UBYTE ch = CPU_AL;

        cpu_save_regs(cpu, &saved);
        {
            dos_far_ptr syscon = fdos_lol_syscon();
            check_handle_break(&syscon);
        }
        write_char_stdout(ch);
        cpu_restore_regs(cpu, &saved);

        CPU_AL = ch;
        cf = 0;
    }
    else
    if (CPU_AX == 0x1206) {
        /*
         * Invoke the DOS critical-error path with explicit parameters.
         * callerARG1 is the word pushed by the caller before INT 2Fh and
         * therefore lives at SS:[SP+6] in this native interrupt frame.
         */
        UWORD arg = readw86(stk_lin(CPU_SS, CPU_SP, 6));
        UWORD flags = arg >> 8;
        UWORD drive = (flags & EFLG_CHAR) ? 0 : (arg & 0xff);
        /* Failing device header, straight from the guest's BP:SI. */
        CPU_AL = CriticalError(flags, drive, CPU_DI, MK_FP(CPU_BP, CPU_SI));
        cf = 0;
    }
    else
    if (CPU_AX == 0x1208) {
        /* Raw SFT reference-count decrement, matching upstream 1208h. */
        CPU_AX = fdos_sft_dec_ref_raw(MK_FP(CPU_ES, CPU_DI));
        cf = 0;
    }
    else
    if (CPU_AX == 0x120a) {
        /*
         * Invoke the critical-error path using the current drive.
         * callerARG1 is the DOS error code pushed before INT 2Fh.
         */
        UWORD error = readw86(stk_lin(CPU_SS, CPU_SP, 6));
        const UBYTE drive = fdos_dos_default_drive();
        const dos_far_ptr cdsp = fdos_cds_slot(drive);
        const dos_far_ptr dpbp = far_is_null(cdsp) ? MK_FP(0, 0) : fdos_cds_dpb(cdsp);

        if (far_is_null(dpbp)) {
            CPU_AL = FAIL;
            cf = 1;
        } else {
            CPU_AL = CriticalError(0x38, drive, error, fdos_dpb_device(dpbp));
            cf = (CPU_AL == RETRY) ? 0 : 1;
        }
    }
    else
    if (CPU_AX == 0x120b) {
        /*
         * Sharing-violation helper.  SHARE/network support is absent,
         * but compatibility/FCB opens still receive the normal critical-
         * error opportunity before DE_SHARE is returned.
         */
        const dos_far_ptr sftp = MK_FP(CPU_ES, CPU_DI);
        const UWORD sft_mode = fdos_sft_mode_raw(sftp);
        UWORD error = readw86(stk_lin(CPU_SS, CPU_SP, 6));
        UBYTE retry = FALSE;

        if ((sft_mode & O_FCB) ||
            !(sft_mode & (O_SHAREMASK | O_NOINHERIT))) {
            const UBYTE drive = fdos_dos_default_drive();
            const dos_far_ptr cdsp = fdos_cds_slot(drive);
            const dos_far_ptr dpbp = far_is_null(cdsp) ? MK_FP(0, 0) : fdos_cds_dpb(cdsp);
            if (!far_is_null(dpbp)) {
                retry = CriticalError(0x38, drive, error, fdos_dpb_device(dpbp)) == RETRY;
            }
        }
        CPU_AX = DE_SHARE;
        cf = retry ? 0 : 1;
    }
    else
    if (CPU_AX == 0x120c) {
        /* Notify a character device about OPEN and set SFT owner PSP. */
        const dos_far_ptr entry = fdos_dos_lp_cur_sft();

        if (!far_is_null(entry) && !far_is_end(entry)) {
            if (fdos_sft_flags_raw(entry) & SFT_FDEVICE) {
                /* The request packet is handed to the driver as a GUEST
                   ES:BX (see x86_execrh), so it must live in guest RAM. A
                   native "request rq;" local does not - linear_to_far() on it
                   yielded a bogus guest pointer. Use the shared IoReqHdr slot
                   in internal_data (guest RAM), like every other execrh() caller. */
                const dos_far_ptr rq_far = fdos_sda_request_far(offsetof(struct dos_data, IoReqHdr));
                const fdos_request_guest_ref rq = fdos_request_guest(rq_far);
                guest_fill_block(rq.linear, 0, sizeof(request));
                FDOS_REQUEST_SET8(rq, r_length, sizeof(request));
                FDOS_REQUEST_SET8(rq, r_command, C_OPEN);
                execrh(rq_far, fdos_sft_dev_raw(entry));
            }
            fdos_sft_set_psp_raw(entry, fdos_dos_cu_psp());
        }
        cf = 0;
    }
    else
    if (CPU_AX == 0x120d) {
        /* Return current date/time in DOS packed directory format. */
        CPU_AX = dos_getdate();
        CPU_DX = dos_gettime();
        cf = 0;
    }
    else
    if (CPU_AX == 0x1220) {
        /*
         * DOS internal: get pointer to the current process' JFT entry.
         *
         * Input:  BX = process file handle.
         * Output: ES:DI -> one-byte JFT entry containing the SFN.
         *         CF clear on success.
         *         AL = DOS error, CF set on an invalid handle.
         */
        const UWORD psp_seg = fdos_dos_cu_psp();
        const UWORD maxfiles = fdos_psp_max_files(psp_seg);
        const dos_far_ptr filetab = fdos_psp_file_table(psp_seg);

        if (CPU_BX >= maxfiles || far_is_null(filetab) || far_is_end(filetab)) {
            CPU_AL = (UBYTE)(-DE_INVLDHNDL);
            cf = 1;
        } else {
            dos_far_ptr jft_entry =
                MK_FP(FP_SEG(filetab),
                      (UWORD)(FP_OFF(filetab) + CPU_BX));

            SET_ES(FP_SEG(jft_entry));
            CPU_DI = FP_OFF(jft_entry);
            cf = 0;
        }
    }
    else
    if (CPU_AX == 0x1216) {
        /*
         * DOS internal: get SFT entry by system file number.
         *
         * Input:  BX = SFN.
         * Output: ES:DI -> SFT entry;
         *         BX = index relative to the containing SFT block;
         *         CF clear on success, set for an invalid SFN.
         */
        int rel_idx = idx_to_sft_(CPU_BX);

        if (rel_idx < 0) {
            cf = 1;
        } else {
            const dos_far_ptr cur = fdos_dos_lp_cur_sft();
            CPU_BX = (UWORD)rel_idx;
            SET_ES(FP_SEG(cur));
            CPU_DI = FP_OFF(cur);
            cf = 0;
        }
    }
    else
    if (CPU_AX == 0x1211) {
        /*
         * DOS internal: normalize an ASCIIZ filename.
         *
         * Input:  DS:SI -> source, ES:DI -> destination.
         * Output: destination contains the NUL-terminated source with
         *         ASCII a..z uppercased and '/' converted to '\\'.
         *
         * This is the same deliberately ASCII-only implementation used by
         * upstream FreeDOS: the internal NLS helpers cannot safely process
         * a source string whose segment is not the kernel data segment.
         */
        uint32_t src = ((uint32_t)CPU_DS << 4) + CPU_SI;
        uint32_t dst = ((uint32_t)CPU_ES << 4) + CPU_DI;
        UBYTE ch;

        do {
            ch = pload8(src++);
            if (ch >= 'a' && ch <= 'z')
                ch -= 'a' - 'A';
            else if (ch == '/')
                ch = '\\';
            pstore8(dst++, ch);
        } while (ch != '\0');

        cf = 0;
    }
    else
    if (CPU_AX == 0x1212) {
        /* DOS internal: length of ES:DI ASCIIZ, including the NUL. */
        const uint32_t s = ((uint32_t)CPU_ES << 4) + CPU_DI;
        CPU_CX = (UWORD)(guest_strnlen_block(s, 0xffffu) + 1u);
        cf = 0;
    }
    else
    if (CPU_AX == 0x1213) {
        /*
         * DOS internal: uppercase one ASCII character.
         *
         * The caller invokes this service as:
         *
         *     push character
         *     mov  ax,1213h
         *     int  2fh
         *
         * At native-handler entry SS:SP points at the INT frame
         * IP,CS,FLAGS, so callerARG1 is the word at SS:[SP+6].
         */
        const uint32_t caller_arg_addr =
            stk_lin(CPU_SS, CPU_SP, 6);
        UBYTE ch = (UBYTE)readw86(caller_arg_addr);

        if (ch >= 'a' && ch <= 'z')
            ch -= 'a' - 'A';

        CPU_AL = ch;
        cf = 0;
    }
    else
    if (CPU_AX == 0x1214) {
        /*
         * DOS internal: compare two far pointers exactly.  Do not
         * canonicalize aliases which map to the same physical address.
         */
        zf = (CPU_DS == CPU_ES && CPU_SI == CPU_DI);
        cf = 0;
    }
    else
    if (CPU_AX == 0x1217) {
        /*
         * DOS internal: return the CDS slot for a zero-based drive number
         * pushed by the caller before INT 2Fh.
         *
         * Stack on native-handler entry:
         *   SS:SP+0  return IP
         *   SS:SP+2  return CS
         *   SS:SP+4  FLAGS
         *   SS:SP+6  callerARG1 (drive: 0=A:, 1=B:, ...)
         */
        const uint32_t caller_arg_addr =
            stk_lin(CPU_SS, CPU_SP, 6);
        const UBYTE drive = (UBYTE)readw86(caller_arg_addr);
        const dos_far_ptr cds_ptr = fdos_cds_slot(drive);

        if (far_is_null(cds_ptr)) {
            cf = 1;
        } else {

            SET_DS(FP_SEG(cds_ptr));
            CPU_SI = FP_OFF(cds_ptr);
            cf = 0;
        }
    }
    else
    if (CPU_AX == 0x1219) {
        /* DOS internal: set default drive, AL is zero-based (0=A:). */
        const UBYTE drv = CPU_AL;

        if (far_is_null(fdos_cds_slot(drv))) {
            CPU_AX = (UWORD)-DE_INVLDDRV;
            cf = 1;
        } else {
            CPU_AL = DosSelectDrv(drv);
            cf = 0;
        }
    }
    else
    if (CPU_AX == 0x121a) {
        /*
         * DOS internal: parse an optional leading drive letter at DS:SI.
         * AL=0 means no explicit drive; AL=1..26 means A:..Z:; AL=FFh
         * means an invalid drive designator.  On a valid X: prefix SI is
         * advanced past the two characters, matching upstream FreeDOS.
         */
        const uint32_t p = ((uint32_t)CPU_DS << 4) + CPU_SI;
        UBYTE ch = pload8(p);

        if (ch == 0 || pload8(p + 1u) != ':') {
            CPU_AL = 0;
        } else {
            ch |= 0x20;
            if (ch >= 'a' && ch <= 'z') {
                CPU_AL = (UBYTE)(ch - 'a' + 1);
                CPU_SI = (UWORD)(CPU_SI + 2);
            } else {
                CPU_AL = 0xff;
            }
        }
        cf = 0;
    }
    else
    if (CPU_AX == 0x121b) {
        /* DOS internal: days in February, using DOS' year-modulo-4 rule. */
        CPU_AL = (CPU_CL & 3) ? 28 : 29;
        cf = 0;
    }
    else
    if (CPU_AX == 0x121e) {
        /* DOS internal: case-insensitive ASCII comparison of two ASCIIZ names. */
        uint32_t s1 = ((uint32_t)CPU_DS << 4) + CPU_SI;
        uint32_t s2 = ((uint32_t)CPU_ES << 4) + CPU_DI;
        UBYTE c1, c2;

        do {
            c1 = pload8(s1++);
            c2 = pload8(s2++);
            if (c1 >= 'a' && c1 <= 'z') c1 -= 'a' - 'A';
            if (c2 >= 'a' && c2 <= 'z') c2 -= 'a' - 'A';
        } while (c1 != 0 && c1 == c2);

        zf = (c1 == c2);
        cf = 0;
    }
    else
    if (CPU_AX == 0x1221) {
        /* DOS internal wrapper for INT 21h/AH=60h TRUENAME. */
        const COUNT rc = DosTruename(MK_FP(CPU_DS, CPU_SI),
                                     MK_FP(CPU_ES, CPU_DI));
        if (rc < SUCCESS) {
            CPU_AX = (UWORD)-rc;
            cf = 1;
        } else {
            CPU_AX = (UWORD)rc;
            cf = 0;
        }
    }
    else
    if (CPU_AX == 0x1224) {
        /* DOS internal SHARE retry parameters.  SHARE itself is absent,
         * but these words are also exposed through IOCTL 440Bh. */
        fdos_lol_set_network_retry(CPU_CX, CPU_DX);
        cf = 0;
    }
    else
    if (CPU_AX == 0x1226) {
        /*
         * Internal OPEN wrapper from upstream INT 2Fh/1226h.
         * Input: DS:DX -> name, CL = open mode.
         * Output: AX = handle or positive DOS error, CF accordingly.
         */
        CPU_regs saved;
        const dos_far_ptr name = MK_FP(CPU_DS, CPU_DX);
        const UBYTE mode = CPU_CL;
        long result;

        cpu_save_regs(cpu, &saved);
        fdos_dos_set_crit_err_code(SUCCESS);
        result = DosOpen(name, O_LEGACY | O_OPEN | mode, 0);
        cpu_restore_regs(cpu, &saved);

        if (result < SUCCESS) {
            CPU_AX = (UWORD)(-result);
            cf = 1;
        } else {
            CPU_AX = (UWORD)result;
            cf = 0;
        }
    }
    else
    if (CPU_AX == 0x1227) {
        /* Internal CLOSE wrapper from upstream INT 2Fh/1227h. */
        CPU_regs saved;
        const UWORD handle = CPU_BX;
        COUNT result;

        cpu_save_regs(cpu, &saved);
        fdos_dos_set_crit_err_code(SUCCESS);
        result = DosClose(handle);
        cpu_restore_regs(cpu, &saved);

        if (result < SUCCESS) {
            CPU_AX = (UWORD)(-result);
            cf = 1;
        } else {
            cf = 0;
        }
    }
    else
    if (CPU_AX == 0x1228) {
        /*
         * Internal LSEEK wrapper from upstream INT 2Fh/1228h.
         * BP must contain 4200h, 4201h or 4202h.
         */
        CPU_regs saved;
        const UWORD handle = CPU_BX;
        const UWORD method = CPU_BP;
        const LONG offset = (LONG)MK_ULONG(CPU_CX, CPU_DX);
        ULONG position;
        COUNT result;

        if (method < 0x4200 || method > 0x4202) {
            CPU_AX = (UWORD)(-DE_INVLDFUNC);
            cf = 1;
        } else {
            cpu_save_regs(cpu, &saved);
            fdos_dos_set_crit_err_code(SUCCESS);
            position = DosSeek(handle, offset, method & 0xff, &result);
            cpu_restore_regs(cpu, &saved);

            if (result < SUCCESS) {
                CPU_AX = (UWORD)(-result);
                cf = 1;
            } else {
                CPU_AX = (UWORD)position;
                CPU_DX = (UWORD)(position >> 16);
                cf = 0;
            }
        }
    }
    else
    if (CPU_AX == 0x1229) {
        /*
         * Internal READ wrapper from upstream INT 2Fh/1229h.
         * Input: BX=handle, CX=count, DS:DX=destination.
         */
        CPU_regs saved;
        const UWORD handle = CPU_BX;
        const UWORD count = CPU_CX;
        const dos_far_ptr buffer = MK_FP(CPU_DS, CPU_DX);
        long result;

        cpu_save_regs(cpu, &saved);
        fdos_dos_set_crit_err_code(SUCCESS);
        result = DosRead(handle, count, buffer);
        cpu_restore_regs(cpu, &saved);

        if (result < SUCCESS) {
            CPU_AX = (UWORD)(-result);
            cf = 1;
        } else {
            CPU_AX = (UWORD)result;
            cf = 0;
        }
    }
    else
    if (CPU_AX == 0x1225) {
        /* DOS internal: length of DS:SI ASCIIZ, including the NUL. */
        const uint32_t s = ((uint32_t)CPU_DS << 4) + CPU_SI;
        CPU_CX = (UWORD)(guest_strnlen_block(s, 0xffffu) + 1u);
        cf = 0;
    }
    else
    if (CPU_AH == 0x14)
        return fdos_nls_2fh(cpu);
    else
    if (CPU_AX == 0x4300)
        CPU_AL = 0x80;
    else 
    if (CPU_AX == 0x4310) {
        if (!handler_installed) {
            set_bios_callback(cpu, &params, true);
            handler_installed = true;
        }
        SET_ES ( params.expected_cs );
        CPU_BX = params.expected_ip ;
    }
    else { 
        /// TODO:
        /// no_handler(cpu);
        cf = 1;
    }
    return true;
}
