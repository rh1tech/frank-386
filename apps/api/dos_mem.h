#ifndef __NATIVE_DOS_MEM_H__
#define __NATIVE_DOS_MEM_H__

#include <stddef.h>
#include <stdint.h>

void *dos_alloc_low(size_t size);

/*
 * Convert a paragraph-aligned pointer returned by dos_alloc_low() back to
 * the DOS segment used by real-mode APIs (for example ES:DX for INT 33h).
 * Returns 0 when ptr is outside conventional guest RAM or is not aligned.
 */
uint16_t dos_ptr_segment(const void *ptr);

#endif /* __NATIVE_DOS_MEM_H__ */
