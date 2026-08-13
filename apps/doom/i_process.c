/* Native DOS process requirements for DOOM. */

#include <stdio.h>
#include <stdint.h>
#include <native_process.h>

static native_dos_process_requirements doom_process_requirements = {
    sizeof(native_dos_process_requirements),
    64u * 1024u, /* native ARM stack: WAD directory alloca() is tens of KiB */
    4u * 1024u,  /* guest DOS stack used while native code enters DOS/BIOS */
    0,
    0,
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

    printf("Native process stacks: requested ARM=%lu DOS=%lu, "
           "assigned ARM=%lu DOS=%lu, PSRAM=%08lx..%08lx, SP=%08lx\n",
           (unsigned long)doom_process_requirements.native_stack_size,
           (unsigned long)doom_process_requirements.dos_stack_size,
           (unsigned long)doom_process_requirements.assigned_native_stack_size,
           (unsigned long)doom_process_requirements.assigned_dos_stack_size,
           (unsigned long)doom_process_requirements.app_psram_begin,
           (unsigned long)doom_process_requirements.app_psram_end,
           (unsigned long)sp);
}
