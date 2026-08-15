.syntax unified
.thumb

/*
 * int __ez_crt_call_main(main_fn, argc, argv)
 *
 * AAPCS on entry:
 *   r0 = main_fn
 *   r1 = argc
 *   r2 = argv
 *
 * Save a fixed recovery frame before BLX main().  __ez_crt_exit() restores the
 * exact saved SP and executes the same epilogue, so exit(status) from arbitrary
 * depth below main() behaves exactly like a normal return from main().
 *
 * The sequence is Thumb-1 compatible: r8-r11 are moved through low registers
 * because Cortex-M0+/ARMv6-M cannot push/pop high registers directly.
 */
.global __ez_crt_call_main
.type __ez_crt_call_main, %function
.thumb_func
__ez_crt_call_main:
    push {r4-r7, lr}
    mov  r4, r8
    mov  r5, r9
    mov  r6, r10
    mov  r7, r11
    push {r4-r7}
    sub  sp, #4

    ldr  r4, =__ez_crt_main_sp
    mov  r5, sp
    str  r5, [r4]

    mov  r3, r0
    mov  r0, r1
    mov  r1, r2
    blx  r3

    add  sp, #4
    pop  {r4-r7}
    mov  r8, r4
    mov  r9, r5
    mov  r10, r6
    mov  r11, r7
    pop  {r4-r7, pc}
.size __ez_crt_call_main, .-__ez_crt_call_main

/*
 * void __ez_crt_exit(int status) -- noreturn
 *
 * r0 already contains the status which must become __ez_crt_call_main()'s
 * return value.  Do not modify it while restoring the saved main trampoline
 * frame.  No kernel data is touched; the recovery word lives in this EZ image.
 */
.global __ez_crt_exit
.type __ez_crt_exit, %function
.thumb_func
__ez_crt_exit:
    ldr  r1, =__ez_crt_main_sp
    ldr  r1, [r1]
    mov  sp, r1

    add  sp, #4
    pop  {r4-r7}
    mov  r8, r4
    mov  r9, r5
    mov  r10, r6
    mov  r11, r7
    pop  {r4-r7, pc}
.size __ez_crt_exit, .-__ez_crt_exit
