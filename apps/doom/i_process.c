/* Native DOS process requirements for DOOM. */

#include <stdio.h>
#include <stdint.h>
#include <native_process.h>
#include <dos_api_version.h>

const native_ez_process_requirements __native_ez_process_requirements = {
    64u * 1024u, /* native ARM stack */
     4u * 1024u, /* guest DOS stack */
    DOS_API_VERSION,
};

static native_dos_process_requirements doom_process_requirements = {
    sizeof(native_dos_process_requirements),
    64u * 1024u, /* native ARM stack: WAD directory alloca() is tens of KiB */
    4u * 1024u,  /* guest DOS stack used while native code enters DOS/BIOS */
    0,
    0
};

native_dos_process_requirements *__native_dos_process_requirements(void)
{
    return &doom_process_requirements;
}

void I_PrintProcessRequirements(void)
{
    uintptr_t sp;

    /*
     * Diagnostic only: read the live ARM stack pointer after the loader has
     * switched to the application's native stack.
     */
    __asm volatile ("mov %0, sp" : "=r" (sp));

    printf("Native process stacks: ARM=%lu DOS=%lu, SP=%08lx\n",
           (unsigned long)doom_process_requirements.native_stack_size,
           (unsigned long)doom_process_requirements.dos_stack_size,
           (unsigned long)sp);
}
