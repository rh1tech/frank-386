/* Native DOS process requirements for DOOM. */

#include <native_process.h>

static const native_dos_process_requirements doom_process_requirements = {
    sizeof(native_dos_process_requirements),
    64u * 1024u, /* native ARM stack: WAD directory alloca() is tens of KiB */
    4u * 1024u   /* guest DOS stack used while native code enters DOS/BIOS */
};

const native_dos_process_requirements *__native_dos_process_requirements(void)
{
    return &doom_process_requirements;
}
