#ifndef __NATIVE_DOS_EZ_CRT0_H__
#define __NATIVE_DOS_EZ_CRT0_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * EZ userspace CRT entry ABI.
 *
 * The DOS kernel calls exactly one entry point from an EZ image.  By the time
 * control reaches this function the kernel has already loaded/relocated the
 * image, built argc/argv and switched to the process's native ARM stack.
 * Everything below this boundary is application/CRT policy rather than loader
 * policy.
 */
typedef int (*ez_crt_entry_fn)(int argc, char **argv);

/*
 * Canonical EZ entry point.  elf2ez stores this function's callable Thumb RVA
 * in ez_file_header.entry_rva.
 */
int __ez_start(int argc, char **argv);

/*
 * Return whether main() is currently executing through the EZ CRT trampoline.
 *
 * This is intentionally exposed to the libc layer, not to applications.  A
 * later integration step will let libc exit() choose between the local EZ CRT
 * unwind below and the legacy kernel-owned ELF unwind.
 */
int __ez_crt_main_active(void);

/*
 * Terminate main() locally and resume the EZ CRT as if main() had returned
 * status normally.  The implementation restores the recovery SP recorded by
 * __ez_crt_call_main(); it therefore must only be used while
 * __ez_crt_main_active() is true.
 */
void __ez_crt_exit(int status) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_EZ_CRT0_H__ */
