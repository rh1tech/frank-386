#ifndef EGA128_PAGING_H
#define EGA128_PAGING_H

#include <stdbool.h>
#include <stdint.h>

#if defined(EGA128) || defined(VGA128) || defined(MCGA)
typedef uint8_t  (*ega128_read8_fn)(uint32_t addr);
typedef uint16_t (*ega128_read16_fn)(uint32_t addr);
typedef uint32_t (*ega128_read32_fn)(uint32_t addr);
typedef void (*ega128_write8_fn)(uint32_t addr, uint8_t value);
typedef void (*ega128_write16_fn)(uint32_t addr, uint16_t value);
typedef void (*ega128_write32_fn)(uint32_t addr, uint32_t value);
typedef uint8_t *(*ega128_guest_ptr_fn)(uint32_t addr, bool write_access);

extern ega128_read8_fn ega128_mem_read8;
extern ega128_read16_fn ega128_mem_read16;
extern ega128_read32_fn ega128_mem_read32;
extern ega128_write8_fn ega128_mem_write8;
extern ega128_write16_fn ega128_mem_write16;
extern ega128_write32_fn ega128_mem_write32;
extern ega128_guest_ptr_fn ega128_guest_ptr;

void ega128_select_direct_backend(void);

#define EGA128_VIRTUAL_RAM_SIZE (8u << 20)
#define EGA128_PAGE_SIZE        2048u

bool ega128_paging_init(void);
bool ega128_paging_active(void);
const char *ega128_paging_post_label(void);
uint8_t  ega128_pload8(uint32_t addr);
uint16_t ega128_pload16(uint32_t addr);
uint32_t ega128_pload32(uint32_t addr);
void ega128_pstore8(uint32_t addr, uint8_t value);
void ega128_pstore16(uint32_t addr, uint16_t value);
void ega128_pstore32(uint32_t addr, uint32_t value);
uint8_t *ega128_page_ptr(uint32_t addr, uint32_t *span, bool write_access);
bool ega128_cache_ptr_to_linear(const void *ptr, uint32_t *linear);
#endif

#endif
