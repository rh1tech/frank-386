#ifndef __NATIVE_DOS_VIDEO_H__
#define __NATIVE_DOS_VIDEO_H__

#include <stdint.h>

#ifndef DOS_OS_API_SYS_TABLE_BASE
#define DOS_OS_API_SYS_TABLE_BASE ((void *)(0x10100000ul))
#endif

static const unsigned long * const _dos_video_sys_table_ptrs =
    (const unsigned long * const)DOS_OS_API_SYS_TABLE_BASE;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return the raw native backing store used by the VGA renderer and, when
 * size is non-NULL, its size in bytes.
 *
 * IMPORTANT: this is NOT equivalent to writes through dos_phys_write*() to
 * the x86 VGA aperture.  Direct stores bypass VGA address mapping, latches,
 * write modes, set/reset logic, bit masks and plane-write masks.  Use this
 * interface only when the application intentionally understands the current
 * gfx_buffer layout.
 */
inline static uint8_t *dos_video_get_buffer(uint32_t *size)
{
    typedef uint8_t *(*fn_ptr_t)(uint32_t *);
    return ((fn_ptr_t)_dos_video_sys_table_ptrs[116])(size);
}

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_VIDEO_H__ */
