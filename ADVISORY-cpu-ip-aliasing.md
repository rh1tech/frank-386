# Advisory: -Wstrict-aliasing on CPU_IP (emulator core, NOT patched)

All 6 remaining -Wstrict-aliasing warnings have ONE root cause:

    286/cpu.h:87   #define CPU_IP  (*(uint16_t*)&(cpu->ip))        // 286 mode
    286/cpu.h:85   #define CPU_IP  (*(uint16_t*)&(cpu->next_ip))  // I386_MODE

They fire from fdos files (fdos_21h.c, kernel.c, task.c) only because those
files expand the macro; the macro itself is core.

## Why it is NOT a simple fix
cpu->ip / cpu->next_ip are `uword` == u32 (i386.h:24). The 286 core uses only
the low 16 bits, so this macro is a 16-bit *view* of a 32-bit field. Crucially
CPU_IP is used as an LVALUE in 52+ places in 286/cpu.c (CPU_IP = ...,
CPU_IP += 1) and bios_05h.c - it must stay assignable.

I tried the obvious rvalue fix `((uint16_t)(cpu->ip))`: it is warning-free and
byte-identical on little-endian (verified), but it BREAKS THE BUILD because a
cast is not an lvalue. Reverted before committing.

## The correct fix (a core change, your call)
Make ip/next_ip a union so the low-16 view is a real, assignable member:

    typedef union { u32 d; struct { uint16_t w, hw; } ; } ipreg_t;  // LE layout
    // ... in struct CPU: ipreg_t ip, next_ip;
    #define CPU_IP  (cpu->ip.w)         // or next_ip.w in I386_MODE

This keeps CPU_IP an lvalue, removes the pun, and is endianness-explicit. But
it touches the CPU struct and every ip/next_ip access in the core (both build
modes), so it belongs to whoever owns the emulator core - not an fdos cleanup.

Alternatively: leave it. The pun is same-underlying-storage and works on this
target; -Wstrict-aliasing is conservative here. If you want the warning gone
without the union, the pragma-scoped suppression around just these two macros
is a lower-risk option than rewriting the register file.

Left as core-owner decision, consistent with the RAM-border advisory.
