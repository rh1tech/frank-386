#ifndef __NATIVE_DOS_PROCESS_H__
#define __NATIVE_DOS_PROCESS_H__

#include <stdint.h>

/*
 * Optional native ELF startup requirements.
 *
 * The loader calls __required_dos_api_verion() first.  After the requested
 * API version is accepted, but before _init(), it looks for the optional
 * __native_dos_process_requirements() symbol.  When absent, the historical
 * defaults remain in force: 4096 bytes native ARM stack and 256 bytes guest
 * DOS stack.
 *
 * A zero stack field means "use loader default".  struct_size makes this
 * record forward-extensible: applications should always initialize it to
 * sizeof(native_dos_process_requirements).
 */
typedef struct native_dos_process_requirements {
    uint32_t struct_size;
    uint32_t native_stack_size;
    uint32_t dos_stack_size;
} native_dos_process_requirements;

const native_dos_process_requirements *__native_dos_process_requirements(void);

#endif /* __NATIVE_DOS_PROCESS_H__ */
