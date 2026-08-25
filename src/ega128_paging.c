#include "ega128_paging.h"

#if defined(EGA128) || defined(VGA128) || defined(MCGA)
#include <string.h>
#include <stdio.h>
#include "ff.h"
#include "board_config.h"
#include "mem.h"
#include "pico/platform.h"

#ifdef BOARD_M1
#include "psram_spi.h"
#endif

#define PAGE_COUNT       (EGA128_VIRTUAL_RAM_SIZE / EGA128_PAGE_SIZE)
#define CACHE_PAGE_COUNT (RAM_PAGES_SIZE / EGA128_PAGE_SIZE)
#define INVALID_PAGE     0xffffu

static uint16_t cache_page[CACHE_PAGE_COUNT];
static uint8_t cache_dirty[CACHE_PAGE_COUNT];
static uint8_t backing_valid[PAGE_COUNT / 8u];

/* Tiny page->slot lookup for the CPU hot path.  Four entries are enough to
 * cover the usual code/data/stack working set without spending kilobytes of
 * scarce SRAM on a full PAGE_COUNT reverse map.  Entries are always validated
 * against cache_page[] because the round-robin victim may reuse a slot. */
#define HOT_PAGE_COUNT 4u
static uint16_t hot_page[HOT_PAGE_COUNT];
static uint8_t hot_slot[HOT_PAGE_COUNT];

static uint16_t victim;
static bool active;
static bool use_spi_psram;
static FIL pagefile;
static bool pagefile_open;

static inline bool backing_page_valid(uint32_t page)
{
    return (backing_valid[page >> 3] >> (page & 7)) & 1u;
}

static inline void backing_page_set_valid(uint32_t page)
{
    backing_valid[page >> 3] |= (uint8_t)(1u << (page & 7));
}

static bool pagefile_seek(uint32_t offset)
{
    return pagefile_open && f_lseek(&pagefile, offset) == FR_OK;
}

static bool backing_read(uint32_t page, uint8_t *dst)
{
    if (!backing_page_valid(page)) {
        nf_memset(dst, 0, EGA128_PAGE_SIZE);
        return true;
    }
#ifdef BOARD_M1
    if (use_spi_psram) {
        /*
         * psram_read() encodes the requested read length in one byte as a
         * bit count (PIO: out y, 8), so a single transaction is limited to
         * 255 bits.  Keep each transfer at <= 31 bytes.
         */
        uint32_t addr = page * EGA128_PAGE_SIZE;
        size_t left = EGA128_PAGE_SIZE;
        while (left) {
            size_t chunk = left > 31u ? 31u : left;
            psram_read(&psram_spi, addr, dst, chunk);
            addr += (uint32_t)chunk;
            dst += chunk;
            left -= chunk;
        }
        return true;
    }
#endif
    UINT got = 0;
    return pagefile_seek(page * EGA128_PAGE_SIZE) &&
           f_read(&pagefile, dst, EGA128_PAGE_SIZE, &got) == FR_OK &&
           got == EGA128_PAGE_SIZE;
}

static bool backing_write(uint32_t page, const uint8_t *src)
{
#ifdef BOARD_M1
    if (use_spi_psram) {
        /*
         * psram_write() encodes (4 + payload_bytes) * 8 in one byte
         * (PIO: out x, 8), so the payload is limited to 27 bytes.
         */
        uint32_t addr = page * EGA128_PAGE_SIZE;
        size_t left = EGA128_PAGE_SIZE;
        while (left) {
            size_t chunk = left > 27u ? 27u : left;
            psram_write(&psram_spi, addr, src, chunk);
            addr += (uint32_t)chunk;
            src += chunk;
            left -= chunk;
        }
        backing_page_set_valid(page);
        return true;
    }
#endif
    UINT put = 0;
    if (!pagefile_seek(page * EGA128_PAGE_SIZE) ||
        f_write(&pagefile, src, EGA128_PAGE_SIZE, &put) != FR_OK ||
        put != EGA128_PAGE_SIZE)
        return false;
    backing_page_set_valid(page);
    return true;
}

static inline void __not_in_flash_func(hot_page_remember)(uint32_t page, uint32_t slot)
{
    for (uint32_t i = HOT_PAGE_COUNT - 1u; i != 0; --i) {
        hot_page[i] = hot_page[i - 1u];
        hot_slot[i] = hot_slot[i - 1u];
    }
    hot_page[0] = (uint16_t)page;
    hot_slot[0] = (uint8_t)slot;
}

static uint32_t __not_in_flash_func(map_page)(uint32_t page, bool write_access)
{
    /* Instruction fetches and DOS data accesses normally stay on a handful
     * of pages.  Avoid the 64/96-entry cache scan on those repeated hits. */
    for (uint32_t i = 0; i < HOT_PAGE_COUNT; ++i) {
        uint32_t slot = hot_slot[i];
        if (hot_page[i] == page && slot < CACHE_PAGE_COUNT &&
            cache_page[slot] == page) {
            if (write_access) cache_dirty[slot] = 1;
            return slot;
        }
    }

    for (uint32_t slot = 0; slot < CACHE_PAGE_COUNT; ++slot) {
        if (cache_page[slot] == page) {
            if (write_access) cache_dirty[slot] = 1;
            hot_page_remember(page, slot);
            return slot;
        }
    }

    uint32_t slot = victim++;
    if (victim == CACHE_PAGE_COUNT) victim = 0;

    if (cache_page[slot] != INVALID_PAGE && cache_dirty[slot]) {
        if (!backing_write(cache_page[slot], ram_pages + slot * EGA128_PAGE_SIZE)) {
            printf("guest-RAM paging: backing write failed for page %u\n", cache_page[slot]);
        }
    }
    if (!backing_read(page, ram_pages + slot * EGA128_PAGE_SIZE)) {
        nf_memset(ram_pages + slot * EGA128_PAGE_SIZE, 0, EGA128_PAGE_SIZE);
        printf("guest-RAM paging: backing read failed for page %u\n", (unsigned)page);
    }
    cache_page[slot] = (uint16_t)page;
    cache_dirty[slot] = write_access ? 1 : 0;
    hot_page_remember(page, slot);
    return slot;
}

uint8_t *__not_in_flash_func(ega128_page_ptr)(uint32_t addr, uint32_t *span, bool write_access)
{
    uint32_t page = addr / EGA128_PAGE_SIZE;
    uint32_t off = addr & (EGA128_PAGE_SIZE - 1u);
    uint32_t slot = map_page(page, write_access);
    if (span) *span = EGA128_PAGE_SIZE - off;
    return ram_pages + slot * EGA128_PAGE_SIZE + off;
}

bool ega128_cache_ptr_to_linear(const void *ptr, uint32_t *linear)
{
    uintptr_t p = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)ram_pages;
    if (p < base || p >= base + RAM_PAGES_SIZE)
        return false;

    uint32_t cache_off = (uint32_t)(p - base);
    uint32_t slot = cache_off / EGA128_PAGE_SIZE;
    uint32_t off = cache_off & (EGA128_PAGE_SIZE - 1u);
    if (cache_page[slot] == INVALID_PAGE)
        return false;

    if (linear)
        *linear = (uint32_t)cache_page[slot] * EGA128_PAGE_SIZE + off;
    return true;
}

uint8_t __not_in_flash_func(ega128_pload8)(uint32_t addr)
{
    return *ega128_page_ptr(addr, NULL, false);
}

uint16_t __not_in_flash_func(ega128_pload16)(uint32_t addr)
{
    if ((addr & (EGA128_PAGE_SIZE - 1u)) <= EGA128_PAGE_SIZE - 2u) {
        uint8_t *p = ega128_page_ptr(addr, NULL, false);
        return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    }
    return (uint16_t)ega128_pload8(addr) | ((uint16_t)ega128_pload8(addr + 1) << 8);
}

uint32_t __not_in_flash_func(ega128_pload32)(uint32_t addr)
{
    if ((addr & (EGA128_PAGE_SIZE - 1u)) <= EGA128_PAGE_SIZE - 4u) {
        uint8_t *p = ega128_page_ptr(addr, NULL, false);
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    return (uint32_t)ega128_pload8(addr) |
           ((uint32_t)ega128_pload8(addr + 1) << 8) |
           ((uint32_t)ega128_pload8(addr + 2) << 16) |
           ((uint32_t)ega128_pload8(addr + 3) << 24);
}

void __not_in_flash_func(ega128_pstore8)(uint32_t addr, uint8_t value)
{
    *ega128_page_ptr(addr, NULL, true) = value;
}

void __not_in_flash_func(ega128_pstore16)(uint32_t addr, uint16_t value)
{
    if ((addr & (EGA128_PAGE_SIZE - 1u)) <= EGA128_PAGE_SIZE - 2u) {
        uint8_t *p = ega128_page_ptr(addr, NULL, true);
        p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
        return;
    }
    ega128_pstore8(addr, (uint8_t)value);
    ega128_pstore8(addr + 1, (uint8_t)(value >> 8));
}

void __not_in_flash_func(ega128_pstore32)(uint32_t addr, uint32_t value)
{
    if ((addr & (EGA128_PAGE_SIZE - 1u)) <= EGA128_PAGE_SIZE - 4u) {
        uint8_t *p = ega128_page_ptr(addr, NULL, true);
        p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
        p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
        return;
    }
    ega128_pstore8(addr, (uint8_t)value);
    ega128_pstore8(addr + 1, (uint8_t)(value >> 8));
    ega128_pstore8(addr + 2, (uint8_t)(value >> 16));
    ega128_pstore8(addr + 3, (uint8_t)(value >> 24));
}


static uint8_t __not_in_flash_func(ega128_direct_read8)(uint32_t addr)
{
    return guest_ram_base[addr];
}

static uint16_t __not_in_flash_func(ega128_direct_read16)(uint32_t addr)
{
    return *(uint16_t *)(guest_ram_base + addr);
}

static uint32_t __not_in_flash_func(ega128_direct_read32)(uint32_t addr)
{
    return *(uint32_t *)(guest_ram_base + addr);
}

static void __not_in_flash_func(ega128_direct_write8)(uint32_t addr, uint8_t value)
{
    guest_ram_base[addr] = value;
}

static void __not_in_flash_func(ega128_direct_write16)(uint32_t addr, uint16_t value)
{
    *(uint16_t *)(guest_ram_base + addr) = value;
}

static void __not_in_flash_func(ega128_direct_write32)(uint32_t addr, uint32_t value)
{
    *(uint32_t *)(guest_ram_base + addr) = value;
}

static uint8_t *__not_in_flash_func(ega128_direct_guest_ptr)(uint32_t addr, bool write_access)
{
    (void)write_access;
    return guest_ram_base + addr;
}

static uint8_t *__not_in_flash_func(ega128_paged_guest_ptr)(uint32_t addr, bool write_access)
{
    return ega128_page_ptr(addr, NULL, write_access);
}

ega128_read8_fn ega128_mem_read8 = ega128_direct_read8;
ega128_read16_fn ega128_mem_read16 = ega128_direct_read16;
ega128_read32_fn ega128_mem_read32 = ega128_direct_read32;
ega128_write8_fn ega128_mem_write8 = ega128_direct_write8;
ega128_write16_fn ega128_mem_write16 = ega128_direct_write16;
ega128_write32_fn ega128_mem_write32 = ega128_direct_write32;
ega128_guest_ptr_fn ega128_guest_ptr = ega128_direct_guest_ptr;

void ega128_select_direct_backend(void)
{
    ega128_mem_read8 = ega128_direct_read8;
    ega128_mem_read16 = ega128_direct_read16;
    ega128_mem_read32 = ega128_direct_read32;
    ega128_mem_write8 = ega128_direct_write8;
    ega128_mem_write16 = ega128_direct_write16;
    ega128_mem_write32 = ega128_direct_write32;
    ega128_guest_ptr = ega128_direct_guest_ptr;
}

static void ega128_select_paged_backend(void)
{
    ega128_mem_read8 = ega128_pload8;
    ega128_mem_read16 = ega128_pload16;
    ega128_mem_read32 = ega128_pload32;
    ega128_mem_write8 = ega128_pstore8;
    ega128_mem_write16 = ega128_pstore16;
    ega128_mem_write32 = ega128_pstore32;
    ega128_guest_ptr = ega128_paged_guest_ptr;
}

bool ega128_paging_flush(void)
{
    if (!active) return true;

    for (uint32_t slot = 0; slot < CACHE_PAGE_COUNT; ++slot) {
        if (cache_page[slot] == INVALID_PAGE || !cache_dirty[slot])
            continue;
        if (!backing_write(cache_page[slot],
                           ram_pages + slot * EGA128_PAGE_SIZE))
            return false;
        cache_dirty[slot] = 0;
    }

    if (pagefile_open && f_sync(&pagefile) != FR_OK)
        return false;
    return true;
}

bool ega128_paging_active(void)
{
    return active;
}

const char *ega128_paging_post_label(void)
{
#ifdef BOARD_M1
    if (active && use_spi_psram) {
#ifdef MCGA
        return "SPI PSRAM: 8 MB [192 KB / 96 pages]";
#else
        return "SPI PSRAM: 8 MB [128 KB / 64 pages]";
#endif
    }
#endif
#ifdef MCGA
    return "SWAP     : 8 MB [192 KB / 96 pages]";
#else
    return "SWAP     : 8 MB [128 KB / 64 pages]";
#endif
}

bool ega128_paging_init(void)
{
    nf_memset(cache_page, 0xff, sizeof(cache_page));
    nf_memset(cache_dirty, 0, sizeof(cache_dirty));
    nf_memset(backing_valid, 0, sizeof(backing_valid));
    nf_memset(hot_page, 0xff, sizeof(hot_page));
    nf_memset(hot_slot, 0xff, sizeof(hot_slot));
    nf_memset(ram_pages, 0, RAM_PAGES_SIZE);
    victim = 0;

#ifdef BOARD_M1
    psram_spi = psram_spi_init_clkdiv(pio1, -1, 2.4f, false);
    psram_write32(&psram_spi, 0x313373u, 0xDEADBEEFu);
    use_spi_psram = psram_read32(&psram_spi, 0x313373u) == 0xDEADBEEFu;
    PSRAM_AVAILABLE = use_spi_psram;
    if (use_spi_psram) {
        ega128_select_paged_backend();
        active = true;
        printf("guest-RAM paging: %u KiB cache -> SPI PSRAM\n", (unsigned)(RAM_PAGES_SIZE >> 10));
        return true;
    }
#endif

    FRESULT fr = f_open(&pagefile, SD_DATA_DIR_SLASH "pagefile.sys",
                        FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK)
        return false;
    fr = f_expand(&pagefile, EGA128_VIRTUAL_RAM_SIZE, 1);
    if (fr != FR_OK) {
        f_close(&pagefile);
        return false;
    }
    pagefile_open = true;
    ega128_select_paged_backend();
    active = true;
    printf("guest-RAM paging: %u KiB cache -> " SD_DATA_DIR_SLASH "pagefile.sys\n", (unsigned)(RAM_PAGES_SIZE >> 10));
    return true;
}
#endif
