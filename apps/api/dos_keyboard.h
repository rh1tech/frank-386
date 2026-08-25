#ifndef __NATIVE_DOS_KEYBOARD_H__
#define __NATIVE_DOS_KEYBOARD_H__

#include <stdint.h>

#ifndef DOS_OS_API_SYS_TABLE_BASE
#define DOS_OS_API_SYS_TABLE_BASE ((void *)(0x10100000ul))
#endif

static const unsigned long * const _dos_keyboard_sys_table_ptrs =
    (const unsigned long * const)DOS_OS_API_SYS_TABLE_BASE;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dos_keyboard_event {
    int is_down;
    int keycode;
} dos_keyboard_event_t;

#define DOS_KEYBOARD_EVENT_CONSUME 0x01u
#define DOS_KEYBOARD_EVENT_NEWEST  0x02u

inline static int dos_keyboard_get_event(dos_keyboard_event_t *event, uint32_t flags)
{
    typedef int (*fn_ptr_t)(uint32_t, int *, int *);
    if (!event)
        return -1;
    return ((fn_ptr_t)_dos_keyboard_sys_table_ptrs[119])(
        flags, &event->is_down, &event->keycode);
}

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_KEYBOARD_H__ */
