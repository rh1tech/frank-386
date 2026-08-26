/*
 * diag.c - core0 lockup / fault catcher.
 *
 * Answers, in one run, without a SWD probe and without UART:
 *
 *   1. Did core0 take a fault?  isr_hardfault dumps the stacked frame; the
 *      ARM PC is the faulting instruction.  MemManage/Bus/Usage faults are
 *      enabled so CFSR is meaningful instead of a bare "escalated".
 *
 *   2. Did core0 overflow its stack?  MSPLIM is armed at __StackBottom, so a
 *      push below it raises a UsageFault (CFSR bit 20, STKOF) at the exact
 *      instruction that did it - instead of silently walking down out of
 *      SCRATCH_Y into SCRATCH_X, which is core1's stack.
 *
 *   3. Is core0 spinning in a native loop?  A hardware alarm IRQ keeps firing
 *      on core0 even while pc_step() is never reached again; its ISR grabs the
 *      stacked PC, which is the address inside the stuck loop.
 *
 *   4. Wedged with interrupts masked?  Then neither of the above fires, and
 *      core1 (still drawing) reports that the heartbeat stopped.
 *
 * Everything is written straight into the guest text framebuffer at B8000h
 * through pstore16() - no BIOS, no INT 10h, no UART.  Rows 0..9, white on red.
 *
 * Resolve the printed PC with:
 *      arm-none-eabi-addr2line -f -e build/murm386.elf 0x100xxxxx
 */

#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/structs/scb.h"

#include "i386.h"        /* pulls in mem.h -> pstore16() */
#include "diag.h"
#include "core0_stack.h"

/* RP2040 has TIMER_IRQ_0; RP2350 has two timers, so TIMER0_IRQ_0.. */
#if defined(PICO_RP2040) && PICO_RP2040
#define DIAG_TIMER_IRQ_BASE  TIMER_IRQ_0
#else
#define DIAG_TIMER_IRQ_BASE  TIMER0_IRQ_0
#endif

/* ------------------------------------------------------------------ */
/* heartbeat                                                           */
/* ------------------------------------------------------------------ */

volatile uint32_t diag_hb;          /* bumped by pc_step() on core0 */

extern const char *last_int_call;   /* bios_intcall.c */

/* ------------------------------------------------------------------ */
/* output: straight into the guest text buffer                         */
/* ------------------------------------------------------------------ */

#define DIAG_ATTR   0x4F00u         /* white on red */

static int diag_row;

static void diag_clear_row(int row)
{
    uint32_t base = 0xB8000u + (uint32_t)row * 160u;
    for (int col = 0; col < 160; col += 2)
        pstore16(base + col, DIAG_ATTR | ' ');
}

static void diag_puts(const char *s)
{
    int row = diag_row;
    if (row > 24) return;
    diag_clear_row(row);
    uint32_t base = 0xB8000u + (uint32_t)row * 160u;
    int col = 0;
    while (*s && col < 160) {
        pstore16(base + col, DIAG_ATTR | (uint8_t)*s++);
        col += 2;
    }
    diag_row++;
}

static char *diag_hex(char *p, uint32_t v)
{
    static const char d[] = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4)
        *p++ = d[(v >> i) & 0xF];
    return p;
}

static char *diag_dec(char *p, uint32_t v)
{
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static char *diag_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

/* ------------------------------------------------------------------ */
/* stack                                                               */
/* ------------------------------------------------------------------ */

#define DIAG_PAINT 0xC0DEC0DEu

/* core0 stack: top of SCRATCH_Y, grows down.  Below __StackBottom lie the
   rest of SCRATCH_Y and then SCRATCH_X - which is core1's stack. */
extern uint32_t __StackBottom;
extern uint32_t __StackTop;
/* Bottom of the TEXT_BUFFER region (0x2003D000) and the hard floor for the
   non-relocated core0 stack. It may legitimately extend down from CORE0_STACK
   through CORE0_STACK_EXT and into TEXT_BUFFER, but must never drop below this.
   Direct-QSPI reduced-VRAM builds instead move SP into GFX_BUFFER and reclaim
   CORE0_STACK_EXT+CORE0_STACK as the FatFs cache. */
extern uint32_t __text_buffer_area__;

static uint32_t *diag_lo, *diag_hi;

static void diag_paint_stack(void)
{
    uint32_t sp;
    __asm volatile ("mov %0, sp" : "=r" (sp));

    if (core0_stack_uses_gfx_buffer) {
        diag_lo = (uint32_t *)core0_stack_floor_runtime;
        diag_hi = (uint32_t *)core0_stack_top_runtime;
    } else {
        diag_lo = &__StackBottom;
        diag_hi = &__StackTop;
    }

    uint32_t *end = (uint32_t *)((sp - 256) & ~3u);
    if (end > diag_hi) end = diag_hi;
    for (uint32_t *p = diag_lo; p < end; p++)
        *p = DIAG_PAINT;
}

static uint32_t diag_stack_unused(void)
{
    uint32_t *p = diag_lo;
    if (!p) return 0;
    while (p < diag_hi && *p == DIAG_PAINT) p++;
    return (uint32_t)((uintptr_t)p - (uintptr_t)diag_lo);
}

uint32_t diag_stack_total(void)
{
    return (uint32_t)((uintptr_t)diag_hi - (uintptr_t)diag_lo);
}

/* ------------------------------------------------------------------ */
/* frame dump                                                          */
/* ------------------------------------------------------------------ */

/* exception frame: r0 r1 r2 r3 r12 lr pc xpsr */
static void diag_dump(const char *what, uint32_t *f)
{
    char b[160], *p;

    diag_row = 0;

    p = diag_str(b, "*** ");
    p = diag_str(p, what);
    p = diag_str(p, "  core ");
    p = diag_dec(p, get_core_num());
    *p = 0; diag_puts(b);

    p = diag_str(b, "  PC   = ");
    p = diag_hex(p, f[6]);
    p = diag_str(p, "   <-- addr2line this");
    *p = 0; diag_puts(b);

    p = diag_str(b, "  LR   = ");
    p = diag_hex(p, f[5]);
    p = diag_str(p, "   xPSR = ");
    p = diag_hex(p, f[7]);
    *p = 0; diag_puts(b);

    p = diag_str(b, "  R0-3 = ");
    p = diag_hex(p, f[0]); *p++ = ' ';
    p = diag_hex(p, f[1]); *p++ = ' ';
    p = diag_hex(p, f[2]); *p++ = ' ';
    p = diag_hex(p, f[3]);
    p = diag_str(p, "  R12 = ");
    p = diag_hex(p, f[4]);
    *p = 0; diag_puts(b);

    p = diag_str(b, "  CFSR = ");
    p = diag_hex(p, scb_hw->cfsr);
    p = diag_str(p, "   HFSR = ");
    p = diag_hex(p, scb_hw->hfsr);
    *p = 0; diag_puts(b);

    p = diag_str(b, "  MMFAR= ");
    p = diag_hex(p, scb_hw->mmfar);
    p = diag_str(p, "   BFAR = ");
    p = diag_hex(p, scb_hw->bfar);
    *p = 0; diag_puts(b);

    if (scb_hw->cfsr & (1u << 20)) {      /* STKOF */
        p = diag_str(b, "  *** STACK OVERFLOW - MSPLIM hit ***");
        *p = 0; diag_puts(b);
    }

    p = diag_str(b, "  stack unused = ");
    p = diag_dec(p, diag_stack_unused());
    p = diag_str(p, " of ");
    p = diag_dec(p, diag_stack_total());
    *p = 0; diag_puts(b);

    p = diag_str(b, "  last_int_call = ");
    p = diag_str(p, last_int_call ? last_int_call : "?");
    *p = 0; diag_puts(b);

    p = diag_str(b, "  heartbeat = ");
    p = diag_dec(p, diag_hb);
    *p = 0; diag_puts(b);
}

/* ------------------------------------------------------------------ */
/* fault handler                                                       */
/* ------------------------------------------------------------------ */

void diag_fault_c(uint32_t *frame)
{
    static volatile int once;
    if (!once) {
        once = 1;
        diag_dump("CORE0 FAULT", frame);
    }
    for (;;)
        __asm volatile ("wfi");
}

void __attribute__((naked)) isr_hardfault(void)
{
    __asm volatile (
        "movs r2, #0            \n"   /* drop MSPLIM first: a stack-overflow */
        "msr  msplim, r2        \n"   /* fault could not push its own frame  */
        "movs r0, #4            \n"
        "mov  r1, lr            \n"
        "tst  r1, r0            \n"
        "beq  1f                \n"
        "mrs  r0, psp           \n"
        "b    2f                \n"
        "1:                     \n"
        "mrs  r0, msp           \n"
        "2:                     \n"
        "ldr  r1, =diag_fault_c \n"
        "bx   r1                \n"
    );
}

/* ------------------------------------------------------------------ */
/* stall detector                                                      */
/* ------------------------------------------------------------------ */

#define DIAG_TICK_US      500000u
#define DIAG_STALL_TICKS  4          /* ~2 s without a pc_step() */

static int      diag_alarm_num = -1;
static uint32_t diag_last_hb;
static int      diag_ticks;
static int      diag_reported;
static volatile uint32_t diag_native_depth;

void diag_native_code_enter(void)
{
    diag_native_depth++;
}

void diag_native_code_leave(void)
{
    if (diag_native_depth != 0)
        diag_native_depth--;
}

void diag_alarm_c(uint32_t *frame)
{
    timer_hw->intr = 1u << diag_alarm_num;
    timer_hw->alarm[diag_alarm_num] = timer_hw->timerawl + DIAG_TICK_US;

    /* Native ARM ELF code legitimately runs outside pc_step().  While its
       IRQs are alive, this alarm itself is the heartbeat; if it wedges with
       IRQs masked, core1 still observes a stuck diag_hb and reports it. */
    if (diag_native_depth != 0) {
        diag_hb++;
        diag_last_hb = diag_hb;
        diag_ticks = 0;
        diag_reported = 0;
        return;
    }

    if (diag_hb != diag_last_hb) {
        diag_last_hb = diag_hb;
        diag_ticks = 0;
        diag_reported = 0;
        return;
    }

    if (++diag_ticks < DIAG_STALL_TICKS)
        return;
    if (diag_reported >= 3)
        return;
    diag_reported++;

    /* pc_step() has not run for ~2 s, yet this IRQ still fires: core0 is
       looping in native code with interrupts on.  f[6] is inside that loop. */
    diag_dump("CORE0 STALL - native loop, IRQs alive", frame);
}

static void __attribute__((naked)) diag_alarm_isr(void)
{
    __asm volatile (
        "movs r0, #4            \n"
        "mov  r1, lr            \n"
        "tst  r1, r0            \n"
        "beq  1f                \n"
        "mrs  r0, psp           \n"
        "b    2f                \n"
        "1:                     \n"
        "mrs  r0, msp           \n"
        "2:                     \n"
        "ldr  r1, =diag_alarm_c \n"
        "bx   r1                \n"
    );
}

/* ------------------------------------------------------------------ */
/* core1 side                                                          */
/* ------------------------------------------------------------------ */

void diag_core1_poll(void)
{
    static uint32_t last_hb;
    static absolute_time_t next;
    static int said;
    char b[160], *p;

    if (absolute_time_diff_us(get_absolute_time(), next) > 0)
        return;
    next = make_timeout_time_ms(1000);

    if (diag_hb != last_hb) {
        last_hb = diag_hb;
        said = 0;
        return;
    }

    if (said++ == 3) {
        diag_row = 0;
        diag_puts("*** CORE0 STALL seen from core1 ***");

        p = diag_str(b, "  heartbeat stuck at ");
        p = diag_dec(p, diag_hb);
        *p = 0; diag_puts(b);

        p = diag_str(b, "  last_int_call = ");
        p = diag_str(p, last_int_call ? last_int_call : "?");
        *p = 0; diag_puts(b);

        p = diag_str(b, "  stack unused = ");
        p = diag_dec(p, diag_stack_unused());
        p = diag_str(p, " of ");
        p = diag_dec(p, diag_stack_total());
        *p = 0; diag_puts(b);

        diag_puts("  no CORE0 dump above => core0 wedged with IRQs masked");
    }
}

/* ------------------------------------------------------------------ */

void diag_init(void)
{
    diag_paint_stack();

    /* report the real fault class instead of a bare escalation */
    scb_hw->shcsr |= (1u << 16) | (1u << 17) | (1u << 18);

    /* ARMv8-M stack limit: turn a silent overflow into a precise UsageFault.
       The runtime floor is TEXT_BUFFER for the normal stack, or the end of
       active video RAM when the unused GFX_BUFFER tail is the core0 stack. */
    __asm volatile ("msr msplim, %0"
                    :: "r" ((uint32_t)core0_stack_floor_runtime));

    diag_alarm_num = (int)hardware_alarm_claim_unused(true);
    irq_set_exclusive_handler((uint)(DIAG_TIMER_IRQ_BASE + diag_alarm_num),
                              diag_alarm_isr);
    irq_set_enabled((uint)(DIAG_TIMER_IRQ_BASE + diag_alarm_num), true);
    timer_hw->inte |= 1u << diag_alarm_num;
    timer_hw->alarm[diag_alarm_num] = timer_hw->timerawl + DIAG_TICK_US;
}
