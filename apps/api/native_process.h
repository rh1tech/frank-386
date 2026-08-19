#ifndef __NATIVE_DOS_NATIVE_PROCESS_H__
#define __NATIVE_DOS_NATIVE_PROCESS_H__

#include <stdint.h>
#include "ez.h"

/*
 * Optional native ELF startup requirements.
 *
 * The first three fields are ABI v1 and must never move.  The loader accepts
 * any record with struct_size >= NATIVE_DOS_PROCESS_REQUIREMENTS_V1_SIZE and
 * only touches later fields when struct_size says that they are present.
 *
 * Every ABI field is explicitly 4-byte aligned.  Do not rely on compiler
 * packing options for a structure shared between the firmware and native
 * applications.
 */
#define NATIVE_DOS_ABI_U32(name) \
    uint32_t name __attribute__((aligned(4)))

typedef struct __attribute__((aligned(4))) native_dos_process_requirements {
    NATIVE_DOS_ABI_U32(struct_size);
    NATIVE_DOS_ABI_U32(native_stack_size);
    NATIVE_DOS_ABI_U32(dos_stack_size);

    /*
     * ABI v2 output fields.  The loader writes the aligned sizes it actually
     * selected here before _init()/main().  Applications with the old 12-byte
     * record remain valid because the loader checks struct_size before writes.
     */
    NATIVE_DOS_ABI_U32(assigned_native_stack_size);
    NATIVE_DOS_ABI_U32(assigned_dos_stack_size);

} native_dos_process_requirements;

#define NATIVE_DOS_PROCESS_REQUIREMENTS_V1_SIZE 12u
#define NATIVE_DOS_PROCESS_REQUIREMENTS_V2_SIZE 20u

_Static_assert(sizeof(uint32_t) == 4, "native DOS ABI requires 32-bit uint32_t");
_Static_assert(__alignof__(native_dos_process_requirements) == 4,
               "native DOS process requirements alignment");
_Static_assert(sizeof(native_dos_process_requirements) ==
               NATIVE_DOS_PROCESS_REQUIREMENTS_V2_SIZE,
               "native DOS process requirements layout");

native_dos_process_requirements *__native_dos_process_requirements(void);


#endif /* __NATIVE_DOS_NATIVE_PROCESS_H__ */
