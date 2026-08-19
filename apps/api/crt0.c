#include "crt0.h"
#define NATIVE_EZ_PROCESS_REQUIREMENTS_ATTR
#include "ez.h"
#undef NATIVE_EZ_PROCESS_REQUIREMENTS_ATTR
#include "dos_process.h"
#include "dos_api_version.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Linker-owned startup ranges.
 *
 * elf2ez will synthesize each range as one contiguous array in final EZ image
 * order.  The symbols themselves are application-internal and are deliberately
 * absent from ez_file_header: the DOS loader has no reason to know how a C/C++
 * runtime performs constructor/destructor sequencing.
 */
extern void (*__ez_preinit_array_start[])(void);
extern void (*__ez_preinit_array_end[])(void);
extern void (*__ez_init_array_start[])(void);
extern void (*__ez_init_array_end[])(void);
extern void (*__ez_fini_array_start[])(void);
extern void (*__ez_fini_array_end[])(void);

/* Application entry point is mandatory. */
extern int main(int argc, char **argv);

/* Default EZ resource requirements; applications override this weak object. */
const native_ez_process_requirements
__attribute__((weak, used, section(NATIVE_EZ_PROCESS_DEFAULT_SECTION)))
__native_ez_process_requirements = {
    0,
    0,
    DOS_API_VERSION
};

/*
 * Optional hooks retained for compatibility with the current native ELF ABI.
 *
 * crt0 supplies real weak fallbacks rather than leaving these symbols weak-
 * undefined.  A user/application strong definition therefore replaces the
 * fallback during the relocatable link, while an application which does not
 * provide a hook still leaves elf2ez with an ordinary defined symbol and a
 * normal section dependency.
 */
void * __attribute__((weak)) _init(void)
{
    return NULL;
}

void __attribute__((weak)) _fini(void *ctx)
{
    (void)ctx;
}

/*
 * Assembly trampoline implemented in crt0.S.  It records a recovery SP before
 * entering main(), so libc exit() can discard arbitrary nested application
 * frames and return here without any kernel-owned CRT state.
 */
int __ez_crt_call_main(int (*main_fn)(int, char **), int argc, char **argv);

/*
 * These two words belong to each loaded EZ image.  Nested EXEC therefore gets
 * a separate copy automatically; no process-global kernel recovery pointer is
 * needed for EZ applications.
 *
 * __ez_crt_main_sp is written/read by crt0.S and must remain externally named
 * within the image.  It is not an EZ ABI export and need not survive symbol
 * stripping after relocations have been resolved.
 */
volatile uintptr_t __ez_crt_main_sp;
static volatile int ez_crt_main_is_active;

static void run_array_forward(void (**begin)(void), void (**end)(void))
{
    while (begin < end) {
        void (*fn)(void) = *begin++;
        if (fn != NULL && (uintptr_t)fn != ~(uintptr_t)0)
            fn();
    }
}

static void run_array_reverse(void (**begin)(void), void (**end)(void))
{
    while (end > begin) {
        void (*fn)(void) = *--end;
        if (fn != NULL && (uintptr_t)fn != ~(uintptr_t)0)
            fn();
    }
}

int __ez_crt_main_active(void)
{
    return ez_crt_main_is_active != 0;
}

/*
 * Canonical userspace startup sequence.
 *
 * This deliberately mirrors the order currently implemented by
 * arm_elf_run_body() in the kernel:
 *
 *   preinit_array -> init_array -> _init -> main
 *       -> _fini -> fini_array (reverse)
 *
 * A DOS termination request (for example TSR termination) suppresses the fini
 * path exactly as the current ELF runtime does.  Ordinary libc exit(status)
 * will later be routed through __ez_crt_exit(), which makes main appear to have
 * returned status normally; in that case no DOS termination request is set and
 * the normal fini path still executes.
 */
int __ez_start(int argc, char **argv)
{
    void *fini_ctx = NULL;
    int result;

    run_array_forward(__ez_preinit_array_start, __ez_preinit_array_end);
    run_array_forward(__ez_init_array_start, __ez_init_array_end);

    fini_ctx = _init();

    ez_crt_main_is_active = 1;
    result = __ez_crt_call_main(main, argc, argv);
    ez_crt_main_is_active = 0;
    __ez_crt_main_sp = 0;

    if (!dos_termination_requested()) {
        _fini(fini_ctx);
        run_array_reverse(__ez_fini_array_start, __ez_fini_array_end);
    }

    return result;
}
