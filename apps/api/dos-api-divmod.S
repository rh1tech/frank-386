/*
 * ARM EABI divmod tail shims.
 *
 * These four helpers return quotient/remainder in register pairs, so an
 * ordinary C forwarding function is not safe.  Everything else in the math
 * runtime remains ordinary C.
 */

#include "dos_math_api.h"

.syntax unified
.thumb

#define DOS_API_TABLE_BASE 0x10100000

.macro DIVMOD_SHIM name, slot
    .global \name
    .type \name, %function
    .thumb_func
\name:
    push    {r4}
    ldr     r4, =(DOS_API_TABLE_BASE + (\slot * 4))
    ldr     r4, [r4]
    mov     r12, r4
    pop     {r4}
    bx      r12
    .size \name, .-\name
.endm

DIVMOD_SHIM __aeabi_idivmod,  DOS_API_MATH_AEABI_IDIVMOD
DIVMOD_SHIM __aeabi_uidivmod, DOS_API_MATH_AEABI_UIDIVMOD
DIVMOD_SHIM __aeabi_ldivmod,  DOS_API_MATH_AEABI_LDIVMOD
DIVMOD_SHIM __aeabi_uldivmod, DOS_API_MATH_AEABI_ULDIVMOD
