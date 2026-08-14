#ifndef __NATIVE_DOS_PROCESS_H__
#define __NATIVE_DOS_PROCESS_H__

#ifndef DOS_OS_API_SYS_TABLE_BASE
#define DOS_OS_API_SYS_TABLE_BASE ((void *)(0x10100000ul))
#endif

static const unsigned long * const _dos_process_sys_table_ptrs =
    (const unsigned long * const)DOS_OS_API_SYS_TABLE_BASE;

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/*
 * Terminate the current native DOS process.
 *
 * The kernel unwinds to its own main() trampoline and resumes exactly as if
 * main() had returned status.  The application does not own or manipulate the
 * native stack and therefore needs no assembler termination stub.
 */
static inline __attribute__((noreturn)) void dos_process_exit(int status)
{
    typedef void (*fn_ptr_t)(int) __attribute__((noreturn));
    ((fn_ptr_t)_dos_process_sys_table_ptrs[11])(status);
    __builtin_unreachable();
}

/* Return non-zero after DOS has requested termination of this native child. */
static inline bool dos_termination_requested(void)
{
    typedef bool (*fn_ptr_t)(void);
    return ((fn_ptr_t)_dos_process_sys_table_ptrs[106])();
}

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_PROCESS_H__ */
