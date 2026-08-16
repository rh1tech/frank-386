#ifndef __NATIVE_DOS_MEM_H__
#define __NATIVE_DOS_MEM_H__

#include <stddef.h>
#include <stdint.h>

void *dos_alloc_low(size_t size);
void dos_free_low(void *ptr);

/*
 * Convert a paragraph-aligned pointer returned by dos_alloc_low() back to
 * the DOS segment used by real-mode APIs (for example ES:DX for INT 33h).
 * Returns 0 when ptr is outside conventional guest RAM or is not aligned.
 */
uint16_t dos_ptr_segment(const void *ptr);

/*
 * Convert any pointer into conventional guest RAM to its 20-bit DOS linear
 * address. Returns UINT32_MAX when ptr is outside the first 1 MiB guest RAM.
 * Unlike dos_ptr_segment(), paragraph alignment is not required; DMA needs
 * the exact byte address.
 */
uint32_t dos_ptr_linear(const void *ptr);

#endif /* __NATIVE_DOS_MEM_H__ */
