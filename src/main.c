/**
 * frank-386 - i386 PC Emulator for RP2350
 *
 * Main entry point for the RP2350 platform.
 * Initializes hardware, loads configuration, and starts the emulator.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/adc.h"
#include "hardware/structs/powman.h"

#include "hardware/structs/qmi.h"

#ifdef LIB_PICO_STDIO_UART
#include "hardware/uart.h"
#endif

#include "board_config.h"
#include "audiodiag.h"
#include "psram_init.h"
#include "vga_hw.h"
#include "vga.h"
#include "ps2.h"
#include "ps2kbd_wrapper.h"
#ifdef USB_HID_ENABLED
#include "usbkbd_wrapper.h"
#include "usbmouse_wrapper.h"
#include "usbgamepad.h"
#endif
#ifdef NESPAD_GPIO_CLK
#include "nespad.h"
#endif
#include "sdcard.h"
#include "ff.h"
#include "audio.h"

#include "pc.h"
#include "ini.h"
#include "debug.h"
#include "diskui.h"
#include "settingsui.h"
#include "config_save.h"
#include "vga_osd.h"
#include "profile_subsys.h"
#include "remote_mem.h"
#include "codeprofile.h"
#include "pcsample.h"
#include "autotype.h"
#include "bbprofile.h"
#include "diskcache.h"
#include "gameport.h"

#if FEATURE_AUDIO_PWM
#include <hardware/pwm.h>
#endif

//=============================================================================
// Version Information
//=============================================================================

// Version is defined in CMakeLists.txt from version.txt
#ifndef FRANK_386_VERSION_MAJOR
#define FRANK_386_VERSION_MAJOR 1
#endif
#ifndef FRANK_386_VERSION_MINOR
#define FRANK_386_VERSION_MINOR 0
#endif

//=============================================================================
// Global State
//=============================================================================

PC *pc = NULL;
static PCConfig config;
volatile bool initialized = false;

/*
 * Interpreter throughput, always compiled.
 *
 * SUBSYS_PROFILE cannot answer "is this faster?" — it puts two DWT reads
 * inside the 409-iteration AdLib loop and measurably slows the machine,
 * so any absolute number from it is worthless. This counter touches the
 * hot path once per pc_step(), i.e. once per 4096 guest instructions,
 * which is free.
 *
 * g_mips is thousandths of a MIP (1650 == 1.650 MIPS), sampled over the
 * last window. Read it over SWD; it needs no console.
 */
volatile uint32_t g_mips __attribute__((used));
volatile uint32_t g_mips_clk __attribute__((used));
/*
 * Cumulative average since emulation started.
 *
 * The windowed figure swings ~10% as the Wolf3D demo's scene complexity
 * changes, which would drown any single-digit optimisation. The
 * cumulative average over a deterministic boot path (autotype drives the
 * same keys, the demo is recorded playback) is repeatable to well under
 * a percent, so it is the one to A/B against.
 */
volatile uint32_t g_mips_avg __attribute__((used));

/*
 * FRANK_BOOT_DIAG — a boot trace for a board that has no console to fail into.
 *
 * On Z2 there is nowhere for a panic to go: USB is in Host mode so the guest
 * can have a keyboard, and GPIO0/1 — the only pins a UART could use — carry
 * PS/2. So boot progress is recorded in three words instead, which OpenOCD
 * reads in one transaction. That matters here: reading this board over SWD
 * while it runs has repeatedly knocked it over, so the budget is one touch
 * after it has already stopped, not polling while it works.
 *
 * g_diag_stage advances monotonically, so whatever value it is stuck at names
 * the last milestone reached. g_diag_free_heap is the headroom immediately
 * before pc_new(), which takes ~35 KB in one burst and, at ~92% RAM, is
 * documented as the first allocation to run out. If the board dies with
 * stage == DIAG_PRE_PC_NEW then that number is the whole story.
 */
#define DIAG_CLOCKS       1u
#define DIAG_PSRAM_BEGIN  2u
#define DIAG_PSRAM_OK     3u
#define DIAG_SD_BEGIN     4u
#define DIAG_SD_MOUNTED   5u
#define DIAG_386_DIR      6u
#define DIAG_CONFIG       7u
#define DIAG_INPUT        8u
#define DIAG_PRE_PC_NEW   9u
#define DIAG_PC_NEW_OK   10u
#define DIAG_MAIN_LOOP   11u
volatile uint32_t g_diag_stage __attribute__((used));
volatile uint32_t g_diag_free_heap __attribute__((used));
volatile uint32_t g_diag_pc_new_failed __attribute__((used));

/*
 * Die temperature, in tenths of a degree C, sampled from ADC input 4.
 *
 * The board reboots with POWMAN_CHIP_RESET reporting HAD_BOR while the
 * regulator is provably configured correctly (VREG at 1.65 V, VOUT_OK set,
 * brownout threshold down at 1.10 V). For the core rail to actually collapse
 * that far, the internal regulator has to be running out of headroom - and a
 * hotter die draws more current, which is the one quantity nobody has looked
 * at. It also fits the shape of the complaint: the same clock ran all day
 * yesterday and fails today, after hours of continuous running.
 *
 * g_diag_temp_max survives until the next reset, so after a brownout the peak
 * is still there to read. Sampling once a second costs nothing.
 */
volatile uint32_t g_diag_temp_x10 __attribute__((used));
volatile uint32_t g_diag_temp_max_x10 __attribute__((used));

static void temp_tick(void) {
    static uint64_t next_us;
    static bool inited;
    const uint64_t now = time_us_64();
    if (now < next_us) return;
    next_us = now + 1000000ull;

    if (!inited) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
        inited = true;
        return;                 /* let the sensor settle before the first read */
    }

    adc_select_input(4);
    const uint32_t raw = adc_read();
    /* Datasheet: T = 27 - (V - 0.706) / 0.001721, V = raw * 3.3 / 4096. */
    const int32_t mv = (int32_t)((raw * 3300u) / 4096u);
    const int32_t t_x10 = 270 - (((mv - 706) * 10000) / 17210);
    if (t_x10 < 0 || t_x10 > 2000) return;      /* implausible: ignore */

    g_diag_temp_x10 = (uint32_t)t_x10;
    if ((uint32_t)t_x10 > g_diag_temp_max_x10)
        g_diag_temp_max_x10 = (uint32_t)t_x10;
}

/*
 * FRANK_FAULT_BOX — carry the fault across the reboot it causes.
 *
 * A fault here is unreadable by every channel we have: .bss is wiped by the
 * restart, the USB CDC console dies with the board before it can print, and
 * escalation to lockup clears the core's own state.  The watchdog scratch
 * registers are the one place that survives a watchdog reset, so the handler
 * copies the essentials there and reboots deliberately instead of locking up.
 *
 * Only scratch[0..3] are ours: pico-sdk's watchdog_reboot() writes the boot
 * vector into scratch[4..7], which is why scratch[7] reads "WDOG" on a live
 * board.
 *
 * The naked wrapper exists because a normal C prologue would push registers
 * and move SP off the exception frame before we could find it.  Bit 2 of
 * EXC_RETURN says which stack the frame is on.
 */
#define FAULT_MAGIC 0x46414c54u   /* "FALT" */
#define ABORT_MAGIC 0x41425254u   /* "ABRT" */

/*
 * Name the abort() that fired.
 *
 * The device models call abort() on states they do not implement - vga.c on
 * an unsupported bpp, i8259.c from hw_error(), pci.c, misc.c, i386.c from
 * cpu_abort() - and until Aladdin it was not obvious what that looks like
 * from outside: abort() reaches _exit(), which is an infinite loop, so the
 * board shows a black screen with the guest frozen mid-instruction, no
 * reboot, no fault and nothing in any counter.  Finding the one in i8254.c
 * took halting the core over SWD and walking the stack by hand.
 *
 * The return address makes that a single mdw.  scratch[0..3] survive a
 * reset and are the same three words frank_fault_record() uses, so one
 * read distinguishes a fault ("FALT"), an abort ("ABRT") and a clean guest
 * shutdown ("SHUT").
 */
void __real_abort(void) __attribute__((noreturn));

void __attribute__((noreturn, used)) __wrap_abort(void) {
    watchdog_hw->scratch[0] = ABORT_MAGIC;
    watchdog_hw->scratch[1] = (uint32_t)__builtin_return_address(0);
    watchdog_hw->scratch[2] = g_diag_stage;
    watchdog_hw->scratch[3] = pc ? (uint32_t)cpui386_get_cycle(pc->cpu) : 0u;
    __real_abort();
}


/*
 * Full fault record, in the PSRAM diagnostic block.
 *
 * The four watchdog scratch words are the only storage that survives a
 * reset, which is why the handler used them - but they are also only four
 * words, and the first fault they caught said CFSR = precise bus fault with
 * BFAR valid while the stacked PC pointed at a `movs r2, #24`, an
 * instruction that cannot touch memory.  Three words cannot tell a genuine
 * fault from a record taken off a corrupted stack.
 *
 * This writes the whole exception frame and every fault status register
 * next to the rest of the diagnostics, where there is room, and keeps the
 * scratch markers as the reset-surviving summary.
 */
#define FRANK_FAULT_REC ((volatile uint32_t *)(0x11000000u + 0x000bb000u))

/*
 * Freeze the JIT exit ring the instant anything faults.
 *
 * The ring is in PSRAM and survives the reboot, but the guest runs thousands
 * more blocks on the next boot attempt and overwrites it long before a debug
 * probe can read it.  Setting this stops nj_xr_note() for good, so what stays
 * in the ring is the sixteen exits that led to the fault.
 */
#if NJIT_EXIT_RING
#define NJ_XR_FROZEN ((volatile uint32_t *)(0x11000000u + 0x000a9000u + 68u * 4u))
#endif

void __attribute__((used)) frank_fault_record(uint32_t *frame) {
#if NJIT_EXIT_RING
    *NJ_XR_FROZEN = 0x46524F5Au;   /* "FROZ" - stop the exit ring here */
#endif
    volatile uint32_t *r = FRANK_FAULT_REC;
    r[0]  = FAULT_MAGIC;
    r[1]  = frame[0];   /* r0  */
    r[2]  = frame[1];   /* r1  */
    r[3]  = frame[2];   /* r2  */
    r[4]  = frame[3];   /* r3  */
    r[5]  = frame[4];   /* r12 */
    r[6]  = frame[5];   /* lr  */
    r[7]  = frame[6];   /* pc  */
    r[8]  = frame[7];   /* xpsr */
    r[9]  = (uint32_t)frame;
    r[10] = *(volatile uint32_t *)0xE000ED28u;  /* CFSR  */
    r[11] = *(volatile uint32_t *)0xE000ED2Cu;  /* HFSR  */
    r[12] = *(volatile uint32_t *)0xE000ED34u;  /* MMFAR */
    r[13] = *(volatile uint32_t *)0xE000ED38u;  /* BFAR  */
    r[14] = g_diag_stage;
    r[15] = r[15] + 1u;                          /* how many faults so far */

    watchdog_hw->scratch[0] = FAULT_MAGIC;
    watchdog_hw->scratch[1] = frame[6];                          /* stacked PC */
    watchdog_hw->scratch[2] = *(volatile uint32_t *)0xE000ED28u; /* CFSR */
    watchdog_hw->scratch[3] = *(volatile uint32_t *)0xE000ED38u; /* BFAR */

#if FAULT_PARK
    /*
     * Park instead of rebooting.  Rebooting is right for a board in use - it
     * comes back - but it also destroys the evidence before anyone can look,
     * which is exactly what happened chasing Supaplex: by the time SWD
     * attached, the firmware had restarted and the stack was gone.  Parked,
     * the core can be halted over SWD and the real call stack walked.
     */
    while (true) { tight_loop_contents(); }
#else
    watchdog_reboot(0, 0, 0);
    while (true) { tight_loop_contents(); }
#endif
}

/*
 * Hardware watch on one byte of guest memory.
 *
 * Prehistorik 2 dies because one byte of its code, 10BB:62B8, turns from
 * 0x50 into 0xfb - and every write path the emulator has was instrumented
 * and stayed silent while it happened: not a store by the emulated CPU, not
 * a DMA burst, not a string I/O read.  Whatever writes it is therefore
 * firmware, and only the hardware can say which instruction: the Cortex-M33
 * data watchpoint fires on the access itself and hands over the PC.
 *
 * Armed by writing the physical address into the diagnostics over SWD; the
 * main loop notices and programs the comparator.  One shot - the handler
 * disables it - so a busy address cannot turn into an interrupt storm.
 */
#define FRANK_DWT_COMP0     (*(volatile uint32_t *)0xE0001020u)
#define FRANK_DWT_MASK0     (*(volatile uint32_t *)0xE0001024u)
#define FRANK_DWT_FUNC0     (*(volatile uint32_t *)0xE0001028u)
#define FRANK_DWT_FUNC1     (*(volatile uint32_t *)0xE0001038u)
#define FRANK_SHPR3         (*(volatile uint32_t *)0xE000ED20u)
#define FRANK_DFSR          (*(volatile uint32_t *)0xE000ED30u)
#define FRANK_CPUID_SIO     (*(volatile uint32_t *)0xd0000000u)

/*
 * The hit ring: 32 entries of 16 words, guest 0xbb800..0xbc000, which is the
 * last free space below the CS ring.  It replaces the single record the
 * first version wrote at the same address, so a dump from before this
 * change reads as entry zero and nothing else moves.
 */
#define FRANK_MON_RING      ((volatile uint32_t *)(0x11000000u + 0x000bb800u))
#define FRANK_MON_RING_N    32u

void __attribute__((used)) frank_mon_record(uint32_t *frame) {
    FRANK_DWT_FUNC0 = 0;                 /* quiet while we look at it */
#if FRANK_AUDIO_DIAG
    uint32_t addr = FRANK_DIAG->mon_addr;
    /*
     * The watchpoint is taken after the access retires, so this reads what
     * the write left behind rather than what was there before it - which is
     * the whole question.  Read it uncached: the store went through the XIP
     * cache and a cached read here could answer from the same line, saying
     * only that the cache agrees with itself.
     */
    uint32_t now = addr ? *(volatile uint8_t *)((addr & 0x00ffffffu) | 0x15000000u) : 0u;
    uint32_t n = FRANK_DIAG->mon_head;
    volatile uint32_t *r = FRANK_MON_RING + (n % FRANK_MON_RING_N) * 16u;

    r[0]  = 0x4d4f4e57u;                 /* "MONW" */
    r[1]  = frame[6];                    /* pc, one instruction past the write */
    r[2]  = frame[5];                    /* lr */
    r[3]  = frame[0];                    /* r0 */
    r[4]  = frame[1];                    /* r1 */
    r[5]  = frame[2];                    /* r2 */
    r[6]  = frame[3];                    /* r3 */
    r[7]  = frame[7];                    /* xpsr */
    r[8]  = (uint32_t)frame;
    r[9]  = now;                         /* the byte the write left */
    r[10] = FRANK_CPUID_SIO;             /* which core did it */
    r[11] = time_us_32();
    r[12] = frame[4];                    /* r12 */
    r[13] = FRANK_DFSR;
    r[14] = n;                           /* hit number, so a wrapped ring reads */
    r[15] = addr;

    FRANK_DFSR = FRANK_DFSR;             /* the trap bit is write-one-to-clear */
    FRANK_DIAG->mon_head = n + 1u;
    FRANK_DIAG->mon_hits = n + 1u;

    /*
     * Freeze only on the write that actually breaks the guest.  Everything
     * else - the loader putting the program image down, the game keeping a
     * variable next door - re-arms and costs one exception.
     */
    uint32_t bad = FRANK_DIAG->mon_bad;
    if (bad && now == bad) {
        FRANK_DIAG->ud_reason = 6u;
        FRANK_DIAG->ud_hit = 1u;
        return;                          /* left disarmed: this was the one */
    }
    FRANK_DWT_FUNC0 = (1u << 4) | 0x6u;
#endif
}

void __attribute__((naked)) isr_debugmonitor(void) {
    __asm volatile(
        "tst  lr, #4        \n"
        "ite  eq            \n"
        "mrseq r0, msp      \n"
        "mrsne r0, psp      \n"
        "b    frank_mon_record\n");
}

void frank_mon_arm(uint32_t addr) {
    /*
     * DebugMonitor to the highest configurable priority.  At the SDK default
     * it sits below the audio and video interrupts, and a write made inside
     * one of those would only be reported after that handler returned, with
     * a stacked frame belonging to whatever ran next.  That is exactly the
     * shape of the one hard-fault record this investigation already had to
     * throw away as untrustworthy.
     */
    FRANK_SHPR3 = (FRANK_SHPR3 & 0xffffff00u) | 0x00u;
    *(volatile uint32_t *)0xE000EDFCu |= (1u << 24) | (1u << 16); /* TRCENA, MON_EN */
    FRANK_DWT_COMP0 = addr;
    FRANK_DWT_MASK0 = 0;
    FRANK_DWT_FUNC0 = (1u << 4) | 0x6u;   /* debug event on a data write */
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile(
        "tst  lr, #4        \n"
        "ite  eq            \n"
        "mrseq r0, msp      \n"
        "mrsne r0, psp      \n"
        "b    frank_fault_record\n");
}

static uint32_t tp_steps;
static uint64_t tp_t0;
static uint64_t tp_t_start;
static uint64_t tp_cyc_start, tp_cyc0;

static inline void throughput_tick(void) {
    /*
     * Count instructions actually retired, not pc_step() calls.
     *
     * A pc_step() on a halted guest returns immediately without running
     * its 4096-instruction budget, so estimating from step counts made
     * an idle DOS prompt look like 9 MIPS. cpu->cycle is incremented per
     * decoded instruction in cpu_exec1, which is exact and immune to
     * that. Read once per 2000 steps, so it costs nothing.
     */
    if (++tp_steps < 2000u) return;
    tp_steps = 0;

    const uint64_t now = time_us_64();
    const uint64_t cyc = (uint64_t)(unsigned long)cpui386_get_cycle(pc->cpu);

    if (tp_t_start == 0) { tp_t_start = now; tp_cyc_start = cyc; tp_t0 = now; tp_cyc0 = cyc; return; }

    const uint64_t all_us = now - tp_t_start;
    if (all_us) g_mips_avg = (uint32_t)(((cyc - tp_cyc_start) * 1000ull) / all_us);

    const uint64_t win_us = now - tp_t0;
    if (win_us) g_mips = (uint32_t)(((cyc - tp_cyc0) * 1000ull) / win_us);
    g_mips_clk = clock_get_hz(clk_sys) / 1000000u;

    tp_t0 = now; tp_cyc0 = cyc;
    cp_report((uint32_t)(win_us / 1000ull));
    bb_report();
}

// Framebuffer for VGA output (in PSRAM)
static uint8_t *framebuffer = NULL;

// FatFS state
static FATFS fatfs;

// Flag to track if VGA is initialized (for error display)
static volatile bool vga_initialized = false;

//=============================================================================
// Error Display
//=============================================================================

/**
 * Display a fatal error screen (red box on black background).
 * Halts execution after displaying the message.
 * Can only display errors if VGA is initialized.
 */
static void show_error_screen(const char *title, const char *message, const char *detail) {
    if (!vga_initialized) {
        // VGA not ready, just print to serial and halt
        printf("FATAL ERROR: %s\n", title);
        printf("  %s\n", message);
        if (detail) printf("  %s\n", detail);
        while (1) { sleep_ms(1000); }
    }

    // Initialize OSD for error display
    osd_init();
    osd_clear();

    // Fill screen with black
    uint8_t black_attr = OSD_ATTR(OSD_WHITE, OSD_BLACK);
    osd_fill(0, 0, OSD_COLS, OSD_ROWS, ' ', black_attr);

    // Draw red error box in center
    int box_w = 60;
    int box_h = 10;
    int box_x = (OSD_COLS - box_w) / 2;
    int box_y = (OSD_ROWS - box_h) / 2;

    uint8_t error_attr = OSD_ATTR(OSD_WHITE, OSD_RED);
    uint8_t text_attr = OSD_ATTR(OSD_YELLOW, OSD_RED);

    // Fill box background
    osd_fill(box_x, box_y, box_w, box_h, ' ', error_attr);

    // Draw box border
    osd_draw_box_titled(box_x, box_y, box_w, box_h, title, error_attr);

    // Print message
    int msg_y = box_y + 3;
    osd_print(box_x + 3, msg_y, message, text_attr);

    // Print detail if provided
    if (detail && detail[0]) {
        osd_print(box_x + 3, msg_y + 2, detail, error_attr);
    }

    // Print hint at bottom
    osd_print(box_x + 3, box_y + box_h - 2, "Please check hardware and restart.", error_attr);

    osd_show();

    // Also print to serial
    printf("FATAL ERROR: %s\n", title);
    printf("  %s\n", message);
    if (detail) printf("  %s\n", detail);

    // Halt
    while (1) {
        sleep_ms(1000);
    }
}

/**
 * Display a warning screen (yellow box) but continue execution.
 */
static void show_warning_screen(const char *title, const char *message, int delay_ms) {
    if (!vga_initialized) {
        printf("WARNING: %s - %s\n", title, message);
        return;
    }

    osd_init();
    osd_clear();

    // Fill screen with black
    uint8_t black_attr = OSD_ATTR(OSD_WHITE, OSD_BLACK);
    osd_fill(0, 0, OSD_COLS, OSD_ROWS, ' ', black_attr);

    // Draw yellow warning box
    int box_w = 60;
    int box_h = 8;
    int box_x = (OSD_COLS - box_w) / 2;
    int box_y = (OSD_ROWS - box_h) / 2;

    uint8_t warn_attr = OSD_ATTR(OSD_BLACK, OSD_YELLOW);

    osd_fill(box_x, box_y, box_w, box_h, ' ', warn_attr);
    osd_draw_box_titled(box_x, box_y, box_w, box_h, title, warn_attr);
    osd_print(box_x + 3, box_y + 3, message, warn_attr);

    osd_show();

    printf("WARNING: %s - %s\n", title, message);
    sleep_ms(delay_ms);

    osd_hide();
}

//=============================================================================
// Platform HAL Implementation
//=============================================================================

/**
 * Get microsecond timestamp.
 */
uint32_t __not_in_flash_func(get_uticks)(void) {
    return time_us_32();
}

/**
 * Allocate memory (uses PSRAM for large allocations).
 */
void *pcmalloc(long size) {
    return malloc(size);
}

/**
 * Load ROM file from SD card to memory.
 */
int load_rom(void *phys_mem, const char *file, uword addr, int backward) {
    FIL fp;
    FRESULT res;
    UINT bytes_read;

    char path[256];
    snprintf(path, sizeof(path), "386/%s", file);

    res = f_open(&fp, path, FA_READ);
    if (res != FR_OK) {
        printf("Failed to open ROM: %s (error %d)\n", path, res);
        return -1;
    }

    FSIZE_t size = f_size(&fp);

    uint8_t *dest;
    if (backward) {
        // Load so ROM ends at addr (for BIOS - should end at 1MB boundary)
        dest = (uint8_t *)phys_mem + addr - size;
        DBG_PRINT("Loading ROM: %s (%lu bytes) at 0x%08lx-0x%08lx (dest=%p)\n",
               file, (unsigned long)size,
               (unsigned long)(addr - size), (unsigned long)(addr - 1), dest);
    } else {
        dest = (uint8_t *)phys_mem + addr;
        DBG_PRINT("Loading ROM: %s (%lu bytes) at 0x%08lx (dest=%p)\n",
               file, (unsigned long)size, (unsigned long)addr, dest);
    }

    res = f_read(&fp, dest, size, &bytes_read);
    if (res != FR_OK || bytes_read != size) {
        f_close(&fp);
        printf("ERROR: Failed to read ROM: %s (error %d, read %u of %lu)\n",
               file, res, bytes_read, (unsigned long)size);
        return -1;
    }

    f_close(&fp);

    // Debug: verify data was written to memory
    DBG_PRINT("  First bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           dest[0], dest[1], dest[2], dest[3],
           dest[4], dest[5], dest[6], dest[7]);
    DBG_PRINT("  Last bytes:  %02x %02x %02x %02x %02x %02x %02x %02x\n",
           dest[size-8], dest[size-7], dest[size-6], dest[size-5],
           dest[size-4], dest[size-3], dest[size-2], dest[size-1]);

    return (int)size;  // Return size on success
}

//=============================================================================
// VGA Redraw Callback - Bridge emulator VGA state to hardware driver
//=============================================================================

static void vga_redraw(void *opaque, int x, int y, int w, int h) {
    (void)opaque;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    // No action needed - VGA updates are handled in the main loop
}

#if PC_SAMPLE
/*
 * Lightweight profiler control for real-hardware Z2 testing.
 *
 * Win+F10 resets all sampling state and starts a fresh 10 kHz PC sample.
 * Win+F9 stops sampling and writes 386/profile.txt to the SD card.
 *
 * Nothing is written while the workload is running, so the benchmark is
 * not distorted by FatFS/SD traffic.  The dump contains the hottest host
 * PC buckets (for addr2line against the ELF) and, when BB_PROFILE is also
 * enabled, the hottest guest basic-block start addresses.
 */
#define PROFILE_TOP_N 32

/*
 * Win+F10 is handled in the USB-HID keyboard path, which the SWD key ring
 * does not feed: driving the board from the debug probe there is no way to
 * start a measurement.  This word is polled once per outer loop iteration so
 * a probe can start and stop sampling with a single mww, which is what makes
 * the profiler usable without a human at the keyboard.
 */
volatile uint32_t g_ps_trigger = 0;

static void profile_reset_runtime(void) {
    ps_stop();

    memset(ps_hist, 0, sizeof(ps_hist));
    ps_total = 0;
    ps_outside = 0;

#if BB_PROFILE
    memset(bb_tab, 0, sizeof(bb_tab));
    bb_entries = 0;
    bb_collisions = 0;
    bb_report();
#endif

    /* Align the built-in throughput figures with the new measurement. */
    g_mips = 0;
    g_mips_avg = 0;
    g_mips_clk = 0;
    tp_steps = 0;
    tp_t0 = 0;
    tp_t_start = 0;
    tp_cyc_start = 0;
    tp_cyc0 = 0;

    ps_init(clock_get_hz(clk_sys), 10000u);
    printf("Profiler: reset/start (Win+F9 to save 386/profile.txt)\n");
}

static bool profile_write(FIL *fp, const char *s) {
    UINT bw = 0;
    const UINT len = (UINT)strlen(s);
    return f_write(fp, s, len, &bw) == FR_OK && bw == len;
}

static bool profile_dump_runtime(void) {
    ps_stop();

#if BB_PROFILE
    bb_report();
#endif

    FIL fp;
    if (f_open(&fp, "386/profile.txt", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("Profiler: failed to open 386/profile.txt\n");
        return false;
    }

    char line[192];
    snprintf(line, sizeof(line),
             "FRANK 386 profiler\n"
             "clk_sys_mhz=%lu\n"
             "mips_window_x1000=%lu\n"
             "mips_average_x1000=%lu\n"
             "pc_samples=%lu\n"
             "pc_samples_outside=%lu\n",
             (unsigned long)(clock_get_hz(clk_sys) / 1000000u),
             (unsigned long)g_mips,
             (unsigned long)g_mips_avg,
             (unsigned long)ps_total,
             (unsigned long)ps_outside);
    if (!profile_write(&fp, line)) goto write_error;

    uint32_t top_count[PROFILE_TOP_N] = {0};
    uint32_t top_bucket[PROFILE_TOP_N] = {0};

    for (uint32_t i = 0; i < PS_BUCKETS; ++i) {
        const uint32_t count = ps_hist[i];
        if (!count) continue;

        for (uint32_t p = 0; p < PROFILE_TOP_N; ++p) {
            if (count <= top_count[p]) continue;
            for (uint32_t q = PROFILE_TOP_N - 1; q > p; --q) {
                top_count[q] = top_count[q - 1];
                top_bucket[q] = top_bucket[q - 1];
            }
            top_count[p] = count;
            top_bucket[p] = i;
            break;
        }
    }

    if (!profile_write(&fp,
        "\n[host_pc_samples]\n"
        "; range_start range_end samples percent\n")) goto write_error;

    for (uint32_t p = 0; p < PROFILE_TOP_N && top_count[p]; ++p) {
        const uint32_t start = PS_BASE + (top_bucket[p] << PS_SHIFT);
        const uint32_t end = start + (1u << PS_SHIFT) - 1u;
        const uint32_t pct100 = ps_total
            ? (uint32_t)(((uint64_t)top_count[p] * 10000ull) / ps_total)
            : 0;

        snprintf(line, sizeof(line),
                 "0x%08lx 0x%08lx %lu %lu.%02lu%%\n",
                 (unsigned long)start,
                 (unsigned long)end,
                 (unsigned long)top_count[p],
                 (unsigned long)(pct100 / 100u),
                 (unsigned long)(pct100 % 100u));
        if (!profile_write(&fp, line)) goto write_error;
    }

#if BB_PROFILE
    uint32_t bb_top_hits[PROFILE_TOP_N] = {0};
    uint32_t bb_top_tag[PROFILE_TOP_N] = {0};
    uint64_t bb_total_hits = 0;

    for (uint32_t i = 0; i < BB_SLOTS; ++i) {
        const uint32_t hits = bb_tab[i].hits;
        if (!bb_tab[i].tag || !hits) continue;
        bb_total_hits += hits;

        for (uint32_t p = 0; p < PROFILE_TOP_N; ++p) {
            if (hits <= bb_top_hits[p]) continue;
            for (uint32_t q = PROFILE_TOP_N - 1; q > p; --q) {
                bb_top_hits[q] = bb_top_hits[q - 1];
                bb_top_tag[q] = bb_top_tag[q - 1];
            }
            bb_top_hits[p] = hits;
            bb_top_tag[p] = bb_tab[i].tag;
            break;
        }
    }

    snprintf(line, sizeof(line),
             "\n[basic_blocks]\n"
             "block_entries=%lu\n"
             "collisions=%lu\n"
             "tracked_hits=%llu\n"
             "; guest_linear_ip hits percent_of_tracked_hits\n",
             (unsigned long)bb_entries,
             (unsigned long)bb_collisions,
             (unsigned long long)bb_total_hits);
    if (!profile_write(&fp, line)) goto write_error;

    for (uint32_t p = 0; p < PROFILE_TOP_N && bb_top_hits[p]; ++p) {
        const uint32_t pct100 = bb_total_hits
            ? (uint32_t)(((uint64_t)bb_top_hits[p] * 10000ull) / bb_total_hits)
            : 0;
        snprintf(line, sizeof(line),
                 "0x%08lx %lu %lu.%02lu%%\n",
                 (unsigned long)bb_top_tag[p],
                 (unsigned long)bb_top_hits[p],
                 (unsigned long)(pct100 / 100u),
                 (unsigned long)(pct100 % 100u));
        if (!profile_write(&fp, line)) goto write_error;
    }
#endif

    f_sync(&fp);
    f_close(&fp);
    printf("Profiler: saved 386/profile.txt (%lu PC samples)\n",
           (unsigned long)ps_total);
    return true;

write_error:
    f_close(&fp);
    printf("Profiler: write error while saving 386/profile.txt\n");
    return false;
}
#endif /* PC_SAMPLE */

//=============================================================================
// Keyboard Polling
//=============================================================================

// Track modifier key state for Win+F12 hotkey
static bool win_key_pressed = false;

/* FRANK_NJIT_STATS_HOTKEYS
 *
 * Zero-overhead during emulation: these counters are only touched when the
 * user presses the hotkey. They let us distinguish "JIT is slower" from
 * "the intended hot loop was never compiled/executed".
 *
 * Left Win + F7 = reset counters
 * Left Win + F8 = write 386/jitstats.txt
 */
#if NATIVE_JIT
extern volatile uint32_t g_njit_hits;
extern volatile uint32_t g_njit_misses;
extern volatile uint32_t g_njit_compiles;
extern volatile uint32_t g_njit_insns;
extern volatile uint32_t g_njit_native_iters;
extern volatile uint32_t g_njit_invalidations;
extern volatile uint32_t g_njit_hotwait;
extern volatile uint32_t g_njit_rejects;
extern volatile uint32_t g_njit_flushes;
extern volatile uint32_t g_njit_rej_reason[9];
extern volatile uint32_t g_njit_rej_last_ip;
extern volatile uint32_t g_njit_rej_last_pos;
extern volatile uint32_t g_njit_rej_last_opcode;
extern volatile uint32_t g_njit_rej_last_body_insns;
extern volatile uint32_t g_njit_rej_op[8];
extern volatile uint32_t g_njit_rej_op_count[8];
/* FRANK_NATIVE_JIT_V8_10_DIAG: see the enum comment in i386.c.  The sizes are
 * spelled out here so a mismatch with NJBP_COUNT/NJCH_COUNT is a build error
 * rather than a silently truncated record. */
#define NJBP_SLOTS 16
#define NJCH_SLOTS 13
extern volatile uint32_t g_njit_bp[NJBP_SLOTS];
extern volatile uint32_t g_njit_ch[NJCH_SLOTS];
extern volatile uint32_t g_wl_tlb_refills;
extern volatile uint32_t g_wl_tlb_clears;
extern volatile uint32_t g_wl_exc_pf;
extern volatile uint32_t g_wl_exc_gp;
extern volatile uint32_t g_wl_exc_other;
extern volatile uint32_t g_wl_hw_irq;
extern void cpui386_diag_mode(CPUI386 *cpu, uint32_t out[5]);
extern void njit_diag_reset_hot(void);
extern unsigned njit_diag_reject_snapshot(uint32_t *linear, uint32_t *ip,
                                          uint32_t *hits, uint8_t *bytes,
                                          uint8_t *lens, unsigned cap);
extern unsigned njit_diag_block_snapshot(uint32_t *linear, uint32_t *insns,
                                         uint32_t *entries, uint8_t *ninsns,
                                         uint8_t *flags, unsigned cap);

/*
 * FRANK_WORKLOAD_PROFILE_V88 window origin.
 *
 * g_mips_avg is cumulative since emulation start and is NOT reset by
 * Win+F7, so every jitstats capture in this sequence reported an average
 * diluted by DOS boot and idle time.  native_guest_insns, by contrast, IS
 * window-scoped.  The two were therefore never comparable, which is why no
 * capture so far states what fraction of the measured workload the JIT
 * actually executed.  These two values close that gap.
 */
static uint64_t njs_win_t0;
static uint32_t njs_win_cyc0;

/*
 * One file per measurement window.
 *
 * Win+F7 claims the next unused "386/jitstatsNNN.txt" and Win+F8 writes it,
 * so a session can take run after run without rebooting or renaming
 * anything between them.  The name is chosen by probing the card rather
 * than from a counter, which means a power cycle continues after the
 * highest file already on the card instead of overwriting from 000 again.
 *
 * Pressing Win+F7 twice without a dump in between keeps the same name:
 * nothing was written, so the same slot is still free.  Pressing Win+F8
 * twice rewrites the same file, which is the right behaviour for one
 * window measured once.
 */
#define NJS_CAPTURE_MAX 1000u
static char njs_capture_path[24];
static unsigned njs_capture_seq;

static void njit_stats_pick_path(void) {
    /*
     * Automatic, and deliberately only ever called from Win+F7, whose frame
     * is shallow.  With FF_USE_LFN=1 and FF_MAX_LFN=255 a FILINFO carries a
     * 256-character name buffer; njit_stats_dump() already holds about
     * 1.9 KB of locals against a 2 KB main stack, so it must not reach here,
     * and a static copy would cost 288 bytes of the malloc heap instead.
     */
    FILINFO fno;

    for (unsigned i = njs_capture_seq; i < NJS_CAPTURE_MAX; ++i) {
        snprintf(njs_capture_path, sizeof(njs_capture_path),
                 "386/jitstats%03u.txt", i);
        if (f_stat(njs_capture_path, &fno) != FR_OK) {
            njs_capture_seq = i;
            return;
        }
    }

    /* 1000 captures on one card: stop inventing names and reuse the
     * original fixed one rather than silently writing nothing. */
    snprintf(njs_capture_path, sizeof(njs_capture_path), "386/jitstats.txt");
}

static const char *njit_stats_path(void) {
    /* Never probes: see njit_stats_pick_path().  Without a preceding
     * Win+F7 the window is "since boot" anyway, so the original fixed
     * name is the honest one to use. */
    return njs_capture_path[0] ? njs_capture_path : "386/jitstats.txt";
}

static void njit_stats_reset(void) {
    njit_diag_reset_hot();

    njit_stats_pick_path();

    /*
     * Clear the audio and disk counters as well.
     *
     * Without this the first capture of a session reports everything since
     * boot, and the boot sequence dominates: the 44.1 kHz mixer starts on
     * core 1 as soon as initialized is set, but adlib_core0() only begins
     * filling when the emulation loop starts, several seconds later - SD
     * mount, config parsing, and show_welcome_screen()'s 7 second animation.
     * Every one of those samples was counted as an AdLib underrun, which made
     * a fixed startup cost look like a steady in-game fault: underruns came
     * back at 370-412k regardless of whether the window was 41 s or 66 s.
     */
    {
        uint32_t d0, d1, d2, d3;
        disk_stall_snapshot(&d0, &d1, &d2, &d3);
        sb16_starve_snapshot(&d0, &d1);
        { uint32_t sbd0[8]; sb16_diag_snapshot(sbd0); }
        adlib_gap_snapshot(&d0, &d1, &d2, &d3);
        if (pc && pc->adlib) (void)adlib_underruns(pc->adlib);
    }

    njs_win_t0 = time_us_64();
    njs_win_cyc0 = (pc && pc->cpu)
                 ? (uint32_t)(unsigned long)cpui386_get_cycle(pc->cpu) : 0u;

    g_wl_tlb_refills = 0;
    g_wl_tlb_clears = 0;
    g_wl_exc_pf = 0;
    g_wl_exc_gp = 0;
    g_wl_exc_other = 0;
    g_wl_hw_irq = 0;

    g_njit_hits = 0;
    g_njit_misses = 0;
    g_njit_compiles = 0;
    g_njit_insns = 0;
    g_njit_native_iters = 0;
    g_njit_invalidations = 0;
    g_njit_hotwait = 0;
    g_njit_rejects = 0;
    g_njit_flushes = 0;
    for (int i = 0; i < 9; ++i) g_njit_rej_reason[i] = 0;
    g_njit_rej_last_ip = 0;
    g_njit_rej_last_pos = 0;
    g_njit_rej_last_opcode = 0;
    g_njit_rej_last_body_insns = 0;
    for (int i = 0; i < 8; ++i) {
        g_njit_rej_op[i] = 0;
        g_njit_rej_op_count[i] = 0;
    }
    for (int i = 0; i < NJBP_SLOTS; ++i) g_njit_bp[i] = 0;
    for (int i = 0; i < NJCH_SLOTS; ++i) g_njit_ch[i] = 0;
}

static void njit_stats_dump(void) {
    FIL fp;
    UINT bw;
    char buf[1400];
    uint32_t hot_linear[8], hot_ip[8], hot_hits[8];
    uint8_t hot_bytes[8][32], hot_len[8];
    unsigned hot_n = njit_diag_reject_snapshot(
        hot_linear, hot_ip, hot_hits, &hot_bytes[0][0], hot_len, 8);

    /* Snapshot-and-clear, so every counter below covers the same window. */
    uint32_t dsk_ops, dsk_max_us, dsk_total_us, dsk_stalls;
    disk_stall_snapshot(&dsk_ops, &dsk_max_us, &dsk_total_us, &dsk_stalls);
    uint32_t sb_starves, sb_minfill;
    sb16_starve_snapshot(&sb_starves, &sb_minfill);
    uint32_t sbd[8];
    sb16_diag_snapshot(sbd);
    uint32_t ad_calls, ad_gap_max, ad_gap_over, ad_gap_lost;
    adlib_gap_snapshot(&ad_calls, &ad_gap_max, &ad_gap_over, &ad_gap_lost);

    const char *path = njit_stats_path();

    FRESULT fr = f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        printf("JIT stats: f_open %s failed: %d\n", path, (int)fr);
        return;
    }

    int n = snprintf(
        buf, sizeof(buf),
        "FRANK native JIT stats\n"
        "hits=%lu\n"
        "misses=%lu\n"
        "compiles=%lu\n"
        "native_guest_insns=%lu\n"
        "native_loop_iters=%lu\n"
        "invalidations=%lu\n"
        "hotwait=%lu\n"
        "rejects=%lu\n"
        "flushes=%lu\n"
        "reject_code_window=%lu\n"
        "reject_body_opcode=%lu\n"
        "reject_flag_scratch=%lu\n"
        "reject_incdec_cf=%lu\n"
        "reject_no_branch=%lu\n"
        "reject_empty_body=%lu\n"
        "reject_jcc_no_flags=%lu\n"
        "reject_patch=%lu\n"
        "reject_emit=%lu\n"
        "last_reject_ip=%08lx\n"
        "last_reject_pos=%lu\n"
        "last_reject_opcode=%02lx\n"
        "last_reject_body_insns=%lu\n"
        "rejop0=%02lx,%lu\n"
        "rejop1=%02lx,%lu\n"
        "rejop2=%02lx,%lu\n"
        "rejop3=%02lx,%lu\n"
        "rejop4=%02lx,%lu\n"
        "rejop5=%02lx,%lu\n"
        "rejop6=%02lx,%lu\n"
        "rejop7=%02lx,%lu\n"
        "mips_window_x1000=%lu\n"
        "mips_average_x1000=%lu\n"
        "clk_sys_mhz=%lu\n",
        (unsigned long)g_njit_hits,
        (unsigned long)g_njit_misses,
        (unsigned long)g_njit_compiles,
        (unsigned long)g_njit_insns,
        (unsigned long)g_njit_native_iters,
        (unsigned long)g_njit_invalidations,
        (unsigned long)g_njit_hotwait,
        (unsigned long)g_njit_rejects,
        (unsigned long)g_njit_flushes,
        (unsigned long)g_njit_rej_reason[0],
        (unsigned long)g_njit_rej_reason[1],
        (unsigned long)g_njit_rej_reason[2],
        (unsigned long)g_njit_rej_reason[3],
        (unsigned long)g_njit_rej_reason[4],
        (unsigned long)g_njit_rej_reason[5],
        (unsigned long)g_njit_rej_reason[6],
        (unsigned long)g_njit_rej_reason[7],
        (unsigned long)g_njit_rej_reason[8],
        (unsigned long)g_njit_rej_last_ip,
        (unsigned long)g_njit_rej_last_pos,
        (unsigned long)g_njit_rej_last_opcode,
        (unsigned long)g_njit_rej_last_body_insns,
        (unsigned long)g_njit_rej_op[0], (unsigned long)g_njit_rej_op_count[0],
        (unsigned long)g_njit_rej_op[1], (unsigned long)g_njit_rej_op_count[1],
        (unsigned long)g_njit_rej_op[2], (unsigned long)g_njit_rej_op_count[2],
        (unsigned long)g_njit_rej_op[3], (unsigned long)g_njit_rej_op_count[3],
        (unsigned long)g_njit_rej_op[4], (unsigned long)g_njit_rej_op_count[4],
        (unsigned long)g_njit_rej_op[5], (unsigned long)g_njit_rej_op_count[5],
        (unsigned long)g_njit_rej_op[6], (unsigned long)g_njit_rej_op_count[6],
        (unsigned long)g_njit_rej_op[7], (unsigned long)g_njit_rej_op_count[7],
        (unsigned long)g_mips,
        (unsigned long)g_mips_avg,
        (unsigned long)g_mips_clk
    );

    if (n < 0) n = 0;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    f_write(&fp, buf, (UINT)n, &bw);

    /*
     * FRANK_WORKLOAD_PROFILE_V88 window block.
     *
     * Written as a second record so the block above stays byte-identical
     * to the v8.7.3 format and every archived capture still parses.
     *
     * window_guest_insns is the only figure that turns native_guest_insns
     * into a coverage fraction, and native_coverage_ppm states it
     * directly: it is the hard ceiling on any speedup a JIT coverage
     * change can produce for this workload.
     */
    {
        const uint64_t win_us = time_us_64() - njs_win_t0;
        const uint32_t cyc_now = (pc && pc->cpu)
            ? (uint32_t)(unsigned long)cpui386_get_cycle(pc->cpu) : 0u;
        /* Unsigned wrap-around subtraction: cpu->cycle is 32-bit here and
         * rolls over after roughly 35 minutes at 2 MIPS. */
        const uint32_t win_insns = cyc_now - njs_win_cyc0;
        const uint32_t win_mips  = win_us
            ? (uint32_t)(((uint64_t)win_insns * 1000ull) / win_us) : 0u;
        const uint32_t cov_ppm   = win_insns
            ? (uint32_t)(((uint64_t)g_njit_insns * 1000000ull) / win_insns) : 0u;

        uint32_t mode[5] = {0, 0, 0, 0, 0};
        if (pc && pc->cpu) cpui386_diag_mode(pc->cpu, mode);

        int w = snprintf(
            buf, sizeof(buf),
            "window_us=%lu\n"
            "window_guest_insns=%lu\n"
            "window_mips_x1000=%lu\n"
            "native_coverage_ppm=%lu\n"
            "tlb_refills=%lu\n"
            "tlb_clears=%lu\n"
            "exc_pf=%lu\n"
            "exc_gp=%lu\n"
            "exc_other=%lu\n"
            "hw_irq=%lu\n"
            "mode_cr0=%08lx\n"
            "mode_flags=%08lx\n"
            "mode_cr3=%08lx\n"
            "mode_cpl=%lu\n"
            "mode_code16=%lu\n",
            (unsigned long)win_us,
            (unsigned long)win_insns,
            (unsigned long)win_mips,
            (unsigned long)cov_ppm,
            (unsigned long)g_wl_tlb_refills,
            (unsigned long)g_wl_tlb_clears,
            (unsigned long)g_wl_exc_pf,
            (unsigned long)g_wl_exc_gp,
            (unsigned long)g_wl_exc_other,
            (unsigned long)g_wl_hw_irq,
            (unsigned long)mode[0],
            (unsigned long)mode[1],
            (unsigned long)mode[2],
            (unsigned long)mode[3],
            (unsigned long)mode[4]);

        if (w < 0) w = 0;
        if (w > (int)sizeof(buf)) w = (int)sizeof(buf);
        f_write(&fp, buf, (UINT)w, &bw);
    }

    /*
     * Audio-starvation block.
     *
     * A third record, reusing buf now that it has been flushed, rather than a
     * bigger buffer: njit_stats_dump() already holds about 1.9 KB of locals
     * against a 2 KB main stack (see njit_stats_pick_path()), so this frame
     * has no room to grow.
     *
     * Both audio producers run on core 0 - adlib_core0() and, for SB16,
     * i8257_dma_run() - so a long disk_read() starves both at once. AdLib
     * holds ADLIB_NBUF x ADLIB_BATCH_SIZE = 256 samples, i.e. 5.8 ms; compare
     * disk_read_max_us against that. sb16_minfill shows how close to empty
     * the voice ring runs, and comes back as 0xffffffff if output was never
     * active in the window. FatFS can issue several disk_read() calls per
     * guest f_read(), so disk_read_total_us matters as much as the max.
     */
    {
        int m = snprintf(
            buf, sizeof(buf),
            "adlib_underruns=%lu\n"
            "disk_ops=%lu\n"
            "disk_read_max_us=%lu\n"
            "disk_read_total_us=%lu\n"
            "disk_stalls_2ms=%lu\n"
            "sb16_starves=%lu\n"
            "sb16_minfill=%lu\n"
            "adlib_calls=%lu\n"
            "adlib_gap_max_us=%lu\n"
            "adlib_gap_over=%lu\n"
            "adlib_gap_lost_us=%lu\n"
            "sb16_starve_runs=%lu\n"
            "sb16_starve_max=%lu\n"
            "sb16_refills=%lu\n"
            "sb16_refill_bytes=%lu\n"
            "sb16_dma_gap_max_us=%lu\n"
            "sb16_bytes_per_sec=%lu\n"
            "sb16_freq=%lu\n"
            "sb16_fmtcode=%lu\n",
            (unsigned long)(pc && pc->adlib ? adlib_underruns(pc->adlib) : 0),
            (unsigned long)dsk_ops,
            (unsigned long)dsk_max_us,
            (unsigned long)dsk_total_us,
            (unsigned long)dsk_stalls,
            (unsigned long)sb_starves,
            (unsigned long)sb_minfill,
            (unsigned long)ad_calls,
            (unsigned long)ad_gap_max,
            (unsigned long)ad_gap_over,
            (unsigned long)ad_gap_lost,
            (unsigned long)sbd[0], (unsigned long)sbd[1],
            (unsigned long)sbd[2], (unsigned long)sbd[3],
            (unsigned long)sbd[4], (unsigned long)sbd[5],
            (unsigned long)sbd[6], (unsigned long)sbd[7]);
        if (m < 0) m = 0;
        if (m > (int)sizeof(buf)) m = (int)sizeof(buf);
        f_write(&fp, buf, (UINT)m, &bw);
    }

    /*
     * FRANK_NATIVE_JIT_V8_10_DIAG block.
     *
     * A fourth record, again reusing buf now that it has been flushed, so the
     * three records above stay byte-identical and every archived capture keeps
     * parsing.
     *
     * How to read it:
     *   bp_matched == 0  the exact Symantec loop was never offered to the
     *                    compiler at a hot IP in this window.  No guard is at
     *                    fault and no guard work will help: the loop is not
     *                    reaching the JIT at all.
     *   bp_matched > 0   it was offered and refused.  The largest bp_* counter
     *                    below bp_matched names the guard to attack, and bp_ok
     *                    says how often one got through.
     *   ch_brk_partial dominating ch_calls means blocks abandon the chain on a
     *   guarded side exit; ch_partial_lost is what that costs in guest
     *   instructions.
     */
    {
        int m = snprintf(
            buf, sizeof(buf),
            "bp_attempts=%lu\n"
            "bp_not_code16=%lu\n"
            "bp_sp_mask=%lu\n"
            "bp_code_window=%lu\n"
            "bp_pattern=%lu\n"
            "bp_matched=%lu\n"
            "bp_split_noncontig=%lu\n"
            "bp_static_m2=%lu\n"
            "bp_static_m4=%lu\n"
            "bp_static_m6=%lu\n"
            "bp_static_stk=%lu\n"
            "bp_m6_nonadj=%lu\n"
            "bp_code_overlap=%lu\n"
            "bp_no_room=%lu\n"
            "bp_emit=%lu\n"
            "bp_ok=%lu\n"
            "ch_calls=%lu\n"
            "ch_blocks=%lu\n"
            "ch_brk_zero=%lu\n"
            "ch_brk_not_single=%lu\n"
            "ch_brk_partial=%lu\n"
            "ch_brk_budget=%lu\n"
            "ch_brk_negcache=%lu\n"
            "ch_brk_compile=%lu\n"
            "ch_brk_maxblocks=%lu\n"
            "ch_partial_lost=%lu\n"
            "ch_partial_inside=%lu\n"
            "ch_partial_outside=%lu\n"
            "ch_partial_ready=%lu\n",
            (unsigned long)g_njit_bp[0], (unsigned long)g_njit_bp[1],
            (unsigned long)g_njit_bp[2], (unsigned long)g_njit_bp[3],
            (unsigned long)g_njit_bp[4], (unsigned long)g_njit_bp[5],
            (unsigned long)g_njit_bp[6], (unsigned long)g_njit_bp[7],
            (unsigned long)g_njit_bp[8], (unsigned long)g_njit_bp[9],
            (unsigned long)g_njit_bp[10], (unsigned long)g_njit_bp[11],
            (unsigned long)g_njit_bp[12], (unsigned long)g_njit_bp[13],
            (unsigned long)g_njit_bp[14], (unsigned long)g_njit_bp[15],
            (unsigned long)g_njit_ch[0], (unsigned long)g_njit_ch[1],
            (unsigned long)g_njit_ch[2], (unsigned long)g_njit_ch[3],
            (unsigned long)g_njit_ch[4], (unsigned long)g_njit_ch[5],
            (unsigned long)g_njit_ch[6], (unsigned long)g_njit_ch[7],
            (unsigned long)g_njit_ch[8], (unsigned long)g_njit_ch[9],
            (unsigned long)g_njit_ch[10], (unsigned long)g_njit_ch[11],
            (unsigned long)g_njit_ch[12]);
        if (m < 0) m = 0;
        if (m > (int)sizeof(buf)) m = (int)sizeof(buf);
        f_write(&fp, buf, (UINT)m, &bw);
    }

    for (unsigned i = 0; i < hot_n; ++i) {
        int m = snprintf(buf, sizeof(buf),
                         "hot%u linear=%08lx ip=%08lx rejected_hits=%lu bytes=",
                         i,
                         (unsigned long)hot_linear[i],
                         (unsigned long)hot_ip[i],
                         (unsigned long)hot_hits[i]);
        if (m < 0) m = 0;
        if (m > (int)sizeof(buf)) m = (int)sizeof(buf);
        f_write(&fp, buf, (UINT)m, &bw);

        for (unsigned j = 0; j < hot_len[i]; ++j) {
            m = snprintf(buf, sizeof(buf), "%02x%s",
                         hot_bytes[i][j],
                         (j + 1u == hot_len[i]) ? "" : " ");
            if (m < 0) m = 0;
            if (m > (int)sizeof(buf)) m = (int)sizeof(buf);
            f_write(&fp, buf, (UINT)m, &bw);
        }
        f_write(&fp, "\n", 1, &bw);
    }

    /*
     * FRANK_WORKLOAD_PROFILE_V88: what the JIT compiled and what each
     * block retired.  "compiles=15" alone never said whether one of those
     * 15 was the exact 8-instruction Symantec BP-stack loop that carries
     * the whole real-mode control run, or fifteen short prefix traces.
     * flags: bit0 single_run, bit1 code16, bit2 exact BP-stack block.
     */
    {
        uint32_t blk_linear[8], blk_insns[8], blk_entries[8];
        uint8_t blk_ninsns[8], blk_flags[8];
        unsigned blk_n = njit_diag_block_snapshot(blk_linear, blk_insns,
                                                  blk_entries, blk_ninsns,
                                                  blk_flags, 8);
        for (unsigned i = 0; i < blk_n; ++i) {
            int m = snprintf(buf, sizeof(buf),
                             "blk%u linear=%08lx insns=%lu entries=%lu"
                             " len=%lu flags=%lu\n",
                             i,
                             (unsigned long)blk_linear[i],
                             (unsigned long)blk_insns[i],
                             (unsigned long)blk_entries[i],
                             (unsigned long)blk_ninsns[i],
                             (unsigned long)blk_flags[i]);
            if (m < 0) m = 0;
            if (m > (int)sizeof(buf)) m = (int)sizeof(buf);
            f_write(&fp, buf, (UINT)m, &bw);
        }
    }

    f_sync(&fp);
    f_close(&fp);

    printf("JIT stats written: %s\n", path);
}
#endif

// Process a single keycode, handling disk UI and settings UI hotkeys
// Returns true if key should be passed to emulator, false if consumed
/*
 * FRANK_SWD_KEYBOARD
 *
 * A keyboard the debugger types on.
 *
 * The board has one USB port and it is the guest's keyboard, so driving DOS
 * and flashing used to be mutually exclusive.  SWD solved the flashing half;
 * this solves the other one, without a second USB host or any PIO-USB work:
 * OpenOCD writes keycodes straight into this ring and the main loop feeds
 * them to the emulated 8042 exactly as a real key press would arrive.
 *
 * Each entry is one event, not one character: bit 7 is press/release and
 * bits 0..6 are the Linux input keycode (every key this emulator handles,
 * including F1-F12 at 59-88 and Left Meta at 125, is below 128).  Holding a
 * key down is therefore just a press with no matching release, which is what
 * driving a game needs.
 *
 * The host writes the payload bytes first and bumps g_swdkey_head last; the
 * firmware is the only writer of g_swdkey_tail, so no locking is needed.
 * g_swdkey_magic lets the host script check it is talking to a build that
 * has this, and lets it find the layout it expects.
 */
#define SWDKEY_CAP 64u
#define SWDKEY_MAGIC 0x4b445753u   /* "SWDK" */

volatile uint32_t g_swdkey_magic __attribute__((used)) = SWDKEY_MAGIC;
volatile uint8_t  g_swdkey_buf[SWDKEY_CAP] __attribute__((used));
volatile uint32_t g_swdkey_head __attribute__((used));
volatile uint32_t g_swdkey_tail __attribute__((used));
/* DOS drops keys pressed faster than it polls.  autotype.c settled on ~90 ms
 * per event; 50 ms is responsive enough to drive a menu and still safe.  The
 * host can raise or lower it by writing this word. */
volatile uint32_t g_swdkey_period_us __attribute__((used)) = 50000u;

static void swdkey_tick(void) {
    static uint64_t next_us;
    /* Also the reference that keeps the symbol: --gc-sections drops a
     * variable nothing in the firmware reads, whatever __attribute__((used))
     * tells the compiler.  Checking it here both keeps it and refuses to
     * inject keystrokes if something has scribbled over the ring. */
    if (g_swdkey_magic != SWDKEY_MAGIC) return;
    if (g_swdkey_tail == g_swdkey_head) return;
    const uint64_t now = time_us_64();
    if (now < next_us) return;
    next_us = now + g_swdkey_period_us;

    const uint8_t ev = g_swdkey_buf[g_swdkey_tail % SWDKEY_CAP];
    g_swdkey_tail++;
    if (pc && pc->kbd)
        ps2_put_keycode(pc->kbd, (ev & 0x80u) ? 1 : 0, (int)(ev & 0x7fu));
}

static bool process_keycode(int is_down, int keycode) {
    // Track Win key state
    if (keycode == KEY_LEFTMETA) {
        win_key_pressed = is_down;
    }

#if NATIVE_JIT
    // Linux input keycodes: F7=65, F8=66.
    if (is_down && keycode == 65 && win_key_pressed) {
        njit_stats_reset();
        printf("JIT stats reset; next dump: %s\n", njit_stats_path());
        return false;
    }

    if (is_down && keycode == 66 && win_key_pressed) {
        njit_stats_dump();
        return false;
    }
#endif

    // Check for Win+F12 hotkey to toggle disk UI
    if (is_down && keycode == KEY_F12 && win_key_pressed) {
        if (!diskui_is_open() && !settingsui_is_open()) {
            // Open disk UI and pause emulation
            diskui_open();
            if (pc) {
                pc->paused = 1;
                audio_set_enabled(false);
            }
        } else if (diskui_is_open()) {
            // Close disk UI and resume emulation
            diskui_close();
            if (pc) {
                pc->paused = 0;
                audio_set_enabled(true);
            }
        }
        return false;  // Don't pass to emulator
    }

    // Check for Win+F11 hotkey to toggle settings UI
    if (is_down && keycode == KEY_F11 && win_key_pressed) {
        if (!settingsui_is_open() && !diskui_is_open()) {
            // Open settings UI and pause emulation
            settingsui_open();
            if (pc) {
                pc->paused = 1;
                audio_set_enabled(false);
            }
        } else if (settingsui_is_open()) {
            // Close settings UI and resume emulation
            settingsui_close();
            if (pc) {
                pc->paused = 0;
                audio_set_enabled(true);
            }
        }
        return false;  // Don't pass to emulator
    }

#if PC_SAMPLE
    // Win+F10: clear profiler state and begin a clean measurement window.
    if (is_down && keycode == 68 /* Linux KEY_F10 */ && win_key_pressed) {
        profile_reset_runtime();
        return false;
    }

    // Win+F9: stop sampling and save the current measurement to SD.
    if (is_down && keycode == 67 /* Linux KEY_F9 */ && win_key_pressed) {
        profile_dump_runtime();
        return false;
    }
#endif

    // When disk UI is open, route all keys to it
    if (diskui_is_open()) {
        diskui_handle_key(keycode, is_down);

        // Check if disk UI was closed by Escape
        if (!diskui_is_open() && pc && pc->paused) {
            pc->paused = 0;
            audio_set_enabled(true);
        }
        return false;  // Don't pass to emulator
    }

    // When settings UI is open, route all keys to it
    if (settingsui_is_open()) {
        settingsui_handle_key(keycode, is_down);

        // Check if settings UI was closed by Escape
        if (!settingsui_is_open() && pc && pc->paused) {
            pc->paused = 0;
            audio_set_enabled(true);
        }
        return false;  // Don't pass to emulator
    }

    return true;  // Pass to emulator
}

static void poll_keyboard(void) {
    int is_down, keycode;

#ifdef BOARD_HAS_PS2
    // Poll PS/2 keyboard
    ps2kbd_tick();

    while (ps2kbd_get_key(&is_down, &keycode)) {
        if (process_keycode(is_down, keycode)) {
            if (pc && pc->kbd) {
                ps2_put_keycode(pc->kbd, is_down, keycode);
            }
        }
    }

    // Poll PS/2 mouse (only if PS/2/USB mouse enabled, not NES mouse)
    if (pc && config_get_mouse() && !pc->paused) {
        int16_t dx, dy;
        int8_t dz;
        uint8_t buttons;
        if (ps2_mouse_get_state(&dx, &dy, &dz, &buttons)) {
            if (pc->mouse) {
                int16_t my = config_get_mouse_invert_y() ? -dy : dy;
                ps2_mouse_event(pc->mouse, dx, my, dz, buttons);
            }
        }
    }
#endif // BOARD_HAS_PS2

#ifdef USB_HID_ENABLED
    // Poll USB keyboard
    usbkbd_tick();

    while (usbkbd_get_key(&is_down, &keycode)) {
        if (process_keycode(is_down, keycode)) {
            if (pc && pc->kbd) {
                ps2_put_keycode(pc->kbd, is_down, keycode);
            }
        }
    }

    // Poll USB mouse (only if PS/2/USB mouse enabled, not NES mouse)
    if (pc && config_get_mouse() && !pc->paused) {
        int16_t dx, dy;
        int8_t dz;
        uint8_t buttons;
        if (usbmouse_get_event(&dx, &dy, &dz, &buttons)) {
            if (pc->mouse) {
                ps2_mouse_event(pc->mouse, dx, dy, dz, buttons);
            }
        }
    }
#endif

    /*
     * NES gamepad -> DOS analog joystick (game port at 0x201).
     *
     * The pad is read every poll rather than only when it moves: the game
     * port is level-sensing, not event-driven, so the emulated stick has
     * to hold its position for as long as the button is held.
     */
#ifdef NESPAD_GPIO_CLK
    if (pc && !pc->paused && config_get_nes_joystick()) {
        nespad_read();
        const uint32_t pad = nespad_state;
        int jx = 0, jy = 0;
        if (pad & DPAD_LEFT)  jx = -1;
        if (pad & DPAD_RIGHT) jx =  1;
        if (pad & DPAD_UP)    jy = -1;
        if (pad & DPAD_DOWN)  jy =  1;
        /* A and B are the two buttons every DOS game expects; SNES pads
         * also offer X/Y, mapped alongside so either pair works. */
        uint8_t jb = 0;
        if (pad & (DPAD_A | DPAD_Y)) jb |= 0x01;
        if (pad & (DPAD_B | DPAD_X)) jb |= 0x02;
        gameport_set(jx, jy, jb);
    }
#endif

#ifdef USB_HID_ENABLED
    /* USB gamepad -> the same emulated game port. Checked after the NES
     * pad so that on boards with both, whichever is actually moving
     * wins the last word each poll. */
    if (pc && !pc->paused && config_get_usb_joystick() && usbgamepad_connected()) {
        int jx = 0, jy = 0;
        uint8_t jb = 0;
        usbgamepad_get(&jx, &jy, &jb);
        gameport_set(jx, jy, jb);
    }
#endif

    // NES gamepad -> mouse emulation (if enabled and gamepad pins defined)
#ifdef NESPAD_GPIO_CLK
    if (pc && pc->mouse && !pc->paused && config_get_nes_mouse()) {
        static uint8_t prev_buttons = 0;
        nespad_read();
        uint32_t pad = nespad_state;
        int16_t dx = 0, dy = 0;
        uint8_t buttons = 0;
        // D-pad -> mouse movement (2 pixels per poll)
        // Note: ps2_mouse_event negates dy, so positive = screen up
        if (pad & DPAD_LEFT)  dx = -1;
        if (pad & DPAD_RIGHT) dx =  1;
        if (pad & DPAD_UP)    dy = -1;
        if (pad & DPAD_DOWN)  dy =  1;
        // B = left button, A = right button
        if (pad & DPAD_B) buttons |= 0x01;  // left
        if (pad & DPAD_A) buttons |= 0x02;  // right
        // Send event if there's movement, button press, or button release
        if (dx || dy || buttons || prev_buttons) {
            ps2_mouse_event(pc->mouse, dx, dy, 0, buttons);
        }
        prev_buttons = buttons;
    }
#endif
}

//=============================================================================
// Platform Poll Callback
//=============================================================================

static void platform_poll(void *opaque) {
    (void)opaque;
    poll_keyboard();
    // VGA update is handled by Core 1, don't call here to avoid contention
}

//=============================================================================
// Configuration Loading
//=============================================================================

static void load_default_config(void) {
    memset(&config, 0, sizeof(config));

    // Default memory configuration
#if EMULATE_LTEMS
    config.mem_size = (EMU_MEM_SIZE_MB - 2) * 1024 * 1024;
#else
    config.mem_size = EMU_MEM_SIZE_MB * 1024 * 1024;
#endif
    config.vga_mem_size = EMU_VGA_MEM_SIZE_KB * 1024;

    // CPU configuration
    config.cpu_gen = EMU_CPU_GEN;
    config.fpu = 0;  // Disabled for initial port

    // Display configuration
    config.width = 640;
    config.height = 400;

    // BIOS files (relative to 386 directory on SD card)
    config.bios = "bios.bin";
    config.vga_bios = "vgabios.bin";

    // No disks by default (set via INI file)
    for (int i = 0; i < 4; i++) {
        config.ata[i] = NULL;
        config.iscd[i] = 0;
    }
    config.fdd[0] = NULL;
    config.fdd[1] = NULL;

    config.redirector = 1;
    config.enable_serial = 0;
    config.vga_force_8dm = 0;
}

static int load_config_from_sd(const char *filename) {
    FIL fp;
    FRESULT res;
    DIR dir;
    FILINFO fno;

    // Debug: List 386 directory contents
    DBG_PRINT("Checking SD card contents...\n");
    res = f_opendir(&dir, "386");
    if (res == FR_OK) {
        DBG_PRINT("  386/ directory found, contents:\n");
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
            DBG_PRINT("    %s%s (%lu bytes)\n",
                   fno.fname,
                   (fno.fattrib & AM_DIR) ? "/" : "",
                   (unsigned long)fno.fsize);
        }
        f_closedir(&dir);
    } else {
        DBG_PRINT("  386/ directory not found (error %d)\n", res);
        // Try root directory
        res = f_opendir(&dir, "");
        if (res == FR_OK) {
            DBG_PRINT("  Root directory contents:\n");
            while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
                DBG_PRINT("    %s%s\n", fno.fname, (fno.fattrib & AM_DIR) ? "/" : "");
            }
            f_closedir(&dir);
        }
    }

    char path[256];
    snprintf(path, sizeof(path), "386/%s", filename);

    res = f_open(&fp, path, FA_READ);
    if (res != FR_OK) {
        DBG_PRINT("Config file not found: %s (error %d)\n", path, res);
        return -1;
    }

    DBG_PRINT("Loading config: %s\n", path);

    // Read entire file
    FSIZE_t size = f_size(&fp);
    char *content = malloc(size + 1);
    if (!content) {
        f_close(&fp);
        return -1;
    }

    UINT bytes_read;
    res = f_read(&fp, content, size, &bytes_read);
    f_close(&fp);

    if (res != FR_OK) {
        free(content);
        return -1;
    }

    content[size] = '\0';

    // Parse INI content
    if (ini_parse_string(content, parse_conf_ini, &config) != 0) {
        printf("Failed to parse config\n");
        free(content);
        return -1;
    }

    // Also parse frank-386-specific settings
    ini_parse_string(content, parse_frank_386_ini, NULL);

    free(content);
    return 0;
}

//=============================================================================
// Clock Configuration
//=============================================================================

/*
 * Re-derive the console UART's baud divisors from the current clock.
 *
 * uart_init() computes them once, from whatever clk_peri happened to be
 * at the time — 150 MHz, straight out of the SDK's runtime init. Every
 * later set_sys_clock_khz() drags clk_peri with it and leaves those
 * divisors stale, so the console turns to line noise at exactly the
 * moment the firmware starts overclocking. On C2 the UART is the only
 * debug surface there is, and losing it one line into the boot makes the
 * board very hard to bring up.
 *
 * uart_set_baudrate() recomputes against the live clock, so calling it
 * after each clock change is all it takes. Cheap enough to be
 * unconditional.
 */
void console_reclock(void) {
#ifdef LIB_PICO_STDIO_UART
    uart_set_baudrate(uart_default, PICO_DEFAULT_UART_BAUD_RATE);
#endif
}

// Flash timing configuration for overclocking
void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz, int cfg_flash) {
    /*
     * Raise the floor on the XIP clock, independently of config.ini.
     *
     * Measured on this board with PC_SAMPLE aimed at the flash window while
     * Doom was rendering: 28.7% of core-0 samples were in code executing from
     * flash, almost all of it OPL synthesis - 23.5 KB of slot_render
     * instantiations that are far too large to move into SRAM. flash_freq=66
     * gives CLKDIV 8, i.e. a 63 MHz QSPI clock at 504 MHz sys, so every one of
     * those fetches is charged at half the rate the part is rated for.
     */
#ifdef FLASH_FREQ_FLOOR_MHZ
    if (cfg_flash < FLASH_FREQ_FLOOR_MHZ) cfg_flash = FLASH_FREQ_FLOOR_MHZ;
#endif
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = cfg_flash * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) {
        divisor = 2;
    }

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) {
        rxdelay += 1;
    }

#ifdef FLASH_RXDELAY_OVERRIDE
    /*
     * The working 66 MHz configuration on this board reads back as
     * M0_TIMING = 0x60007008: CLKDIV 8 with RXDELAY *0*.  Borrowing the PSRAM
     * driver's 5.25 ns floor gave RXDELAY 6 at CLKDIV 4 and 5, and both hung
     * at g_diag_stage 0 with POWMAN_CHIP_RESET showing a clean power-on.  If
     * the board is happy sampling immediately at 63 MHz, six half-cycles is
     * more likely to be too late than too early, so this makes the delay
     * directly settable instead of derived.
     */
    rxdelay = FLASH_RXDELAY_OVERRIDE;
    if (rxdelay > 7) rxdelay = 7;
#endif

    qmi_hw->m[0].timing = 0x60007000 |
                        rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                        divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

static void configure_clocks(void) {
#if CPU_CLOCK_MHZ > 252
    // Overclock: disable voltage limit and set higher voltage
    DBG_PRINT("Configuring overclock: %d MHz @ %s\n", CPU_CLOCK_MHZ,
           CPU_CLOCK_MHZ >= 504 ? "1.65V" :
           CPU_CLOCK_MHZ >= 378 ? "1.60V" : "1.50V");

    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    sleep_ms(100);  // Stabilization delay

#if NO_BOD
    /*
     * Diagnostic build only — -DNO_BOD=ON.
     *
     * This board reboots with POWMAN_CHIP_RESET reporting HAD_BOR while the
     * regulator reads back correctly (VREG 1.65 V, VOUT_OK set, threshold at
     * 1.10 V) and the die sits at ~40 C. Disabling the detector does NOT fix
     * a dipping rail; it only stops the chip reacting to one, which is what
     * separates "the rail genuinely collapses" from "the detector fires
     * spuriously". If the reboots stop, the dip was not real.
     *
     * Do not ship this. Without the detector a genuine undervolt keeps
     * executing on corrupted state instead of resetting, and this firmware
     * writes to an SD card.
     */
    hw_clear_bits(&powman_hw->bod, POWMAN_PASSWORD_BITS | POWMAN_BOD_EN_BITS);
    DBG_PRINT("Brownout detector DISABLED (diagnostic build)\n");
#endif

    // Configure flash timing BEFORE changing clock
    set_flash_timings(CPU_CLOCK_MHZ, FLASH_MAX_FREQ_MHZ);
#endif

    // Set system clock
    set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false);
    console_reclock();

    DBG_PRINT("System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
}

/**
 * Get voltage for CPU frequency
 */
static enum vreg_voltage get_voltage_for_freq(int mhz) {
    int v = config_get_voltage();
    if (v >= 0) return (enum vreg_voltage)v;  /* user override */
    /* auto: safe defaults per frequency */
    if (mhz >= 504) return VREG_VOLTAGE_1_65;
    if (mhz >= 378) return VREG_VOLTAGE_1_60;
    return VREG_VOLTAGE_1_50;
}

/**
 * Reconfigure clocks at runtime based on INI settings.
 * This function MUST run from RAM, not flash.
 */
static void __no_inline_not_in_flash_func(reconfigure_clocks)(int cpu_mhz, int psram_mhz, uint psram_pin, int cfg_flash) {
    int current_mhz = clock_get_hz(clk_sys) / 1000000;

    DBG_PRINT("Reconfiguring clocks: %d MHz -> %d MHz, PSRAM: %d MHz, FLASH: %d\n",
              current_mhz, cpu_mhz, psram_mhz, cfg_flash);

    // Only change system clock if CPU frequency actually differs.
    // Unnecessary PLL reconfiguration disrupts PIO timing (HDMI, audio).
    if (cpu_mhz != current_mhz) {
        bool lowering = (cpu_mhz < current_mhz);
        enum vreg_voltage new_voltage = get_voltage_for_freq(cpu_mhz);

        if (lowering) {
            // LOWERING: clock first, then voltage (safe order)
            set_flash_timings(cpu_mhz, cfg_flash);
            set_sys_clock_khz(cpu_mhz * 1000, false);
            sleep_ms(10);
            vreg_set_voltage(new_voltage);
        } else {
            // RAISING: voltage first, then clock (safe order)
            vreg_disable_voltage_limit();
            vreg_set_voltage(new_voltage);
            sleep_ms(50);  // Stabilization delay
            set_flash_timings(cpu_mhz, cfg_flash);
            // ...and the PSRAM divisor too, for the same reason: the QMI
            // still holds the one computed for the old clock, so without this
            // the part is overclocked from the moment the PLL moves until the
            // psram_init_with_freq() below lands.
            psram_set_timings(cpu_mhz, psram_mhz);
            set_sys_clock_khz(cpu_mhz * 1000, false);
        }
        console_reclock();
    } else {
        /*
         * The clock is not moving, but cfg_flash still has to be applied.
         *
         * set_flash_timings() used to live only inside the branch above, so a
         * config.ini that changed flash_freq alone - the CPU frequency
         * matching what the build baked in - was read, stored, reported, and
         * then silently ignored. PSRAM never had this problem because
         * psram_init_with_freq() below runs unconditionally.
         */
        set_flash_timings(cpu_mhz, cfg_flash);
    }

    // Re-initialize PSRAM with the new frequency
    psram_init_with_freq(psram_pin, psram_mhz);

    // Recalculate VGA PIO clock divider (vga_hw_init ran before this call)
    vga_hw_reclock();

    g_diag_stage = DIAG_CLOCKS;
    DBG_PRINT("Clock reconfiguration complete: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
}

//=============================================================================
// Hardware Initialization
//=============================================================================
static void core1_entry(void);
static bool init_hardware(void) {
    // Configure clocks (including overclock if enabled)
    configure_clocks();

    // Initialize PSRAM first
    g_diag_stage = DIAG_PSRAM_BEGIN;
    DBG_PRINT("Initializing PSRAM...\n");
    uint psram_pin = get_psram_pin();
    DBG_PRINT("  PSRAM CS pin: GPIO%d\n", psram_pin);
    psram_init(psram_pin);

    if (!psram_test()) {
        printf("ERROR: PSRAM test failed!\n");
        // Can't show visual error - VGA not ready yet
        return false;
    }
    g_diag_stage = DIAG_PSRAM_OK;
    DBG_PRINT("  PSRAM test passed (8MB)\n");

    // Initialize VGA early so we can show errors on screen
    multicore_launch_core1(core1_entry);

    while(!vga_initialized) {
        sleep_ms(1);
        __dmb();
    }
    __dmb();

    // Initialize SD card
    g_diag_stage = DIAG_SD_BEGIN;
    DBG_PRINT("Initializing SD card...\n");
    FRESULT res = f_mount(&fatfs, "", 1);
    if (res != FR_OK) {
        char detail[32];
        snprintf(detail, sizeof(detail), "FatFS error code: %d", res);
        show_error_screen(" SD Card Error ", "Failed to mount SD card.", detail);
        // show_error_screen never returns
    }
    g_diag_stage = DIAG_SD_MOUNTED;
    DBG_PRINT("  SD card mounted\n");

    // Check if 386/ directory exists
    DIR dir;
    res = f_opendir(&dir, "386");
    if (res != FR_OK) {
        show_error_screen(" Missing Directory ", "Directory '386/' not found on SD card.", "Create it and add config.ini, bios.bin");
        // show_error_screen never returns
    }
    f_closedir(&dir);
    g_diag_stage = DIAG_386_DIR;
    DBG_PRINT("  386/ directory found\n");

    // Load frank-386-specific hardware settings from INI
    // This allows cpu_freq and psram_freq to be configured
    {
        FIL fp;
        char *content = NULL;

        if (f_open(&fp, "386/config.ini", FA_READ) == FR_OK) {
            FSIZE_t size = f_size(&fp);
            content = malloc(size + 1);
            if (content) {
                UINT bytes_read;
                if (f_read(&fp, content, size, &bytes_read) == FR_OK) {
                    content[bytes_read] = '\0';
                    // Parse just the [frank-386] section
                    ini_parse_string(content, parse_frank_386_ini, NULL);
                }
                free(content);
            }
            f_close(&fp);
            DBG_PRINT("  Loaded config.ini\n");
        } else {
            show_warning_screen(" Warning ", "config.ini not found, using defaults.", 2000);
        }

        // Check if clock reconfiguration is needed
        int cfg_cpu = config_get_cpu_freq();
        int cfg_psram = config_get_psram_freq();
        int cfg_flash = config_get_flash_freq();
        // If HDMI boosted the clock (to 504 MHz for jitter-free TMDS),
        // keep it — only reconfigure PSRAM frequency.
        extern bool SELECT_VGA;
        int cur_mhz = clock_get_hz(clk_sys) / 1000000;
        if (!SELECT_VGA && cur_mhz > cfg_cpu) {
            cfg_cpu = cur_mhz;  // preserve HDMI-boosted clock
        }
#ifdef PIN_CLOCKS
        /* Ignore the SD card's cpu_freq/psram_freq so a clock-scaling
         * A/B actually varies the clock. config.ini raises this board to
         * 504 MHz, which would silently flatten every arm of the test. */
        (void)cfg_cpu; (void)cfg_flash;
        reconfigure_clocks(CPU_CLOCK_MHZ, PSRAM_MAX_FREQ_MHZ, psram_pin, FLASH_MAX_FREQ_MHZ);
        DBG_PRINT("  Clocks pinned to build settings: CPU %d, PSRAM %d\n",
                  CPU_CLOCK_MHZ, PSRAM_MAX_FREQ_MHZ);
#else
        if (cfg_cpu != CPU_CLOCK_MHZ || cfg_psram != PSRAM_MAX_FREQ_MHZ || cfg_flash != FLASH_MAX_FREQ_MHZ) {
            reconfigure_clocks(cfg_cpu, cfg_psram, psram_pin, cfg_flash);
        }
#endif
    }

#ifdef BOARD_HAS_PS2
    // Initialize unified PS/2 driver (keyboard + mouse on shared PIO)
    g_diag_stage = DIAG_INPUT;
    DBG_PRINT("Initializing PS/2 (unified driver)...\n");
    DBG_PRINT("  Keyboard CLK: GPIO%d, DATA: GPIO%d\n", PS2_PIN_CLK, PS2_PIN_DATA);
    DBG_PRINT("  Mouse    CLK: GPIO%d, DATA: GPIO%d\n", PS2_MOUSE_CLK, PS2_MOUSE_DATA);
    if (!ps2_init(pio0, PS2_PIN_CLK, PS2_MOUSE_CLK)) {
        printf("WARNING: PS/2 PIO init failed\n");
    }

    // Initialize PS/2 keyboard wrapper (uses unified driver for PIO)
    ps2kbd_init();

    // Initialize PS/2 mouse device (reset, detect IntelliMouse, enable streaming)
    ps2_mouse_init_device();
#else
    DBG_PRINT("PS/2 not present on this board; USB HID is the only input\n");
#endif

    // Initialize USB HID keyboard (if enabled)
#ifdef USB_HID_ENABLED
    DBG_PRINT("Initializing USB HID keyboard...\n");
    usbkbd_init();
#endif

    // Initialize NES/SNES gamepad (if pins defined for this board)
    //
    // -DNO_NESPAD=1 leaves those GPIOs alone. On Z2 the pad sits on GPIO4/6/7
    // and drives GPIO4 as its clock, which is also the board header's default
    // UART TX pin — so anything else wired there fights it. Disabling the pad
    // is the cheapest way to take that out of the picture when chasing
    // instability; it costs only joystick input.
#if defined(NESPAD_GPIO_CLK) && !defined(NO_NESPAD)
    DBG_PRINT("Initializing NES gamepad...\n");
    DBG_PRINT("  CLK: GPIO%d, DATA: GPIO%d, LATCH: GPIO%d\n",
              NESPAD_GPIO_CLK, NESPAD_GPIO_DATA, NESPAD_GPIO_LATCH);
    if (nespad_begin(clock_get_hz(clk_sys) / 1000,
                     NESPAD_GPIO_CLK, NESPAD_GPIO_DATA, NESPAD_GPIO_LATCH)) {
        DBG_PRINT("  NES gamepad initialized\n");
    } else {
        DBG_PRINT("  NES gamepad init failed (PIO unavailable)\n");
    }
#endif

    return true;
}

//=============================================================================
// Emulator Initialization
//=============================================================================

/*
 * Bytes still obtainable from malloc: what sbrk has not handed out
 * yet, plus what is already free inside the arena.
 */
static size_t heap_free_bytes(void)
{
    extern char __StackLimit;   /* top of the heap region */
    struct mallinfo mi = mallinfo();
    return (size_t)(&__StackLimit - (char *)sbrk(0)) + mi.fordblks;
}

static bool init_emulator(void) {
    // Load configuration
    load_default_config();

    // Try to load config from SD card
    if (load_config_from_sd("config.ini") != 0) {
        DBG_PRINT("Using default configuration\n");
    }

    DBG_PRINT("\nEmulator configuration:\n");
    DBG_PRINT("  Memory: %ld MB\n", config.mem_size / (1024 * 1024));
    DBG_PRINT("  VGA Memory: %ld KB\n", config.vga_mem_size / 1024);
    DBG_PRINT("  CPU: %d86\n", config.cpu_gen);
    DBG_PRINT("  BIOS: %s\n", config.bios ? config.bios : "(none)");
    DBG_PRINT("  VGA BIOS: %s\n", config.vga_bios ? config.vga_bios : "(none)");
    DBG_PRINT("  Floppy A: %s\n", config.fdd[0] ? config.fdd[0] : "(none)");
    DBG_PRINT("  Floppy B: %s\n", config.fdd[1] ? config.fdd[1] : "(none)");

    // Calculate total PSRAM needed
    size_t total_psram = config.mem_size;
    DBG_PRINT("  PSRAM needed: %lu KB (available: %lu KB)\n",
           (unsigned long)(total_psram / 1024),
           (unsigned long)(PSRAM_SIZE_BYTES / 1024));

           if (total_psram > PSRAM_SIZE_BYTES) {
        printf("WARNING: Reducing memory to fit in PSRAM\n");
        config.mem_size = PSRAM_SIZE_BYTES;
        DBG_PRINT("  Adjusted memory: %ld MB\n", config.mem_size / (1024 * 1024));
    }

#if REMOTE_MEM
    /* Claim a window of the slave's SRAM immediately above local RAM.
     * Measured at 89 cycles per access against 182 for the master's own
     * PSRAM, so this region is twice as fast as the memory below it.
     *
     * Done before pc_new() because it extends phys_mem_size, and that is
     * what lets load/store pass these addresses through to the
     * dispatch in pload/pstore. */
    {
        uint32_t rbytes = remote_mem_init((uint32_t)config.mem_size,
                                          REMOTE_MEM_BYTES);
        if (rbytes) {
            g_bench[5] = remote_mem_selftest();
            g_bench[4] = remote_mem_rtt();
            config.mem_size += rbytes;
            DBG_PRINT("  Remote tier: +%lu KB (total %ld MB)\n",
                      (unsigned long)(rbytes / 1024u),
                      config.mem_size / (1024 * 1024));
        } else {
            DBG_PRINT("  Remote tier unavailable; PSRAM only\n");
        }
    }
#endif

    /*
     * Bring up the slave's sector cache.
     *
     * Placed here, not in init_hardware(): the link's PIO divider is
     * derived from clk_sys when the state machines are configured, and
     * config.ini raises this board from 378 to 504 MHz during hardware
     * init. This is also the call site where the remote-memory tier was
     * known to work, which matters because an earlier placement inside
     * init_hardware() hung the boot.
     *
     * Harmless with no slave attached: the cache stays disabled and
     * every access goes to the card as before.
     */
    dc_init();

    /*
     * Heap headroom before the boot's largest allocation burst.
     *
     * pc_new() takes ~35 KB out of the malloc heap in one go
     * (VGAState 17092, TLB 8192, SB16State 4844, PCIBus 1156, ...)
     * and the image runs at ~92% RAM, so it is repeatedly the first
     * thing to run out - see the budget notes in i386.c,
     * bbprofile.h and diskcache.h. The SDK reports that failure as
     * an "Out of memory" panic and nothing else, so print the
     * number that explains it before the allocation happens.
     */
    g_diag_free_heap = (uint32_t)heap_free_bytes();
    g_diag_stage = DIAG_PRE_PC_NEW;
    DBG_PRINT("  Free heap: %u bytes\n", (unsigned)g_diag_free_heap);

    // Create PC instance
    DBG_PRINT("\nCreating PC instance...\n");
    pc = pc_new(vga_redraw, platform_poll, NULL, NULL, &config);
    if (!pc) {
        g_diag_pc_new_failed = 1;
        printf("ERROR: Failed to create PC instance\n");
        return false;
    }
    g_diag_stage = DIAG_PC_NEW_OK;

    // Give ISR direct access to VGA register state.
    // From this point the ISR reads cr[], ar[] at the right moment.
    vga_hw_set_vga_state(pc->vga);

    // Ensure emulator starts unpaused
    pc->paused = 0;

    // Initialize disk UI
    DBG_PRINT("Initializing Disk UI...\n");
    diskui_init();

    // Initialize settings UI
    DBG_PRINT("Initializing Settings UI...\n");
    settingsui_init();

    // Initialize config save module with current values from PCConfig
    // (these override INI values if not present in [frank-386] section)
    config_set_mem_size_mb(config.mem_size / (1024 * 1024));
    config_set_cpu_gen(config.cpu_gen);
    config_set_fpu(config.fpu);
    config_set_redirector(config.redirector);
    // Hardware settings are loaded from [frank-386] section via parse_frank_386_ini
    config_clear_changes();

    // Apply audio/mouse enable settings from config to PC instance
    // This allows disabling devices for performance improvement
    pc->pcspk_enabled = config_get_pcspeaker();
    pc->adlib_enabled = config_get_adlib();
    pc->sb16_enabled = config_get_soundblaster();
    pc->tandy_enabled = config_get_tandy();
    pc->covox_enabled = config_get_covox();
    pc->mpu401_enabled = config_get_mpu401();
    pc->dss_enabled = config_get_dss();
    pc->mouse_enabled = config_get_mouse() || config_get_nes_mouse();
    pc->joystick_enabled = config_get_nes_joystick() || config_get_usb_joystick();
    DBG_PRINT("  Audio: PC Speaker=%d, Adlib=%d, SB16=%d, MPU401=%d, Tandy=%d, Covox=%d, DSS=%d, Mouse=%d\n",
              pc->pcspk_enabled, pc->adlib_enabled, pc->sb16_enabled, pc->mpu401_enabled,
              pc->tandy_enabled, pc->covox_enabled, pc->dss_enabled, pc->mouse_enabled);

    // Check if BIOS file exists before loading
    DBG_PRINT("Loading BIOS...\n");
    if (config.bios && config.bios[0]) {
        char bios_path[256];
        FIL fp;
        snprintf(bios_path, sizeof(bios_path), "386/%s", config.bios);
        if (f_open(&fp, bios_path, FA_READ) != FR_OK) {
            char detail[64];
            snprintf(detail, sizeof(detail), "File: %s", bios_path);
            show_error_screen(" Missing BIOS ", "BIOS file not found.", detail);
            // show_error_screen never returns
        }
        f_close(&fp);
    } else {
        show_error_screen(" Missing BIOS ", "No BIOS file specified in config.", "Add bios=filename to config.ini");
        // show_error_screen never returns
    }

    // Load BIOS and reset CPU
    load_bios_and_reset(pc);

    return true;
}

bool timer_callback(repeating_timer_t *rt);
void vga_hw_process_deferred(void);
static bool __not_in_flash_func(timer_callback0)(repeating_timer_t *rt) {
    timer_callback(rt);
    vga_hw_process_deferred();
    return true;
}
// to call DMA wait not from ISR for timer
bool repeat_me_often(void);
static void __not_in_flash_func(core1_entry)(void) {

    DBG_PRINT("[Core 1] Initializing video...\n");
    DBG_PRINT("  Base pin: GPIO%d\n", VGA_BASE_PIN);
    vga_hw_init();
    sleep_ms(100);
    vga_initialized = true;

    // Initialize audio. Boards without an I2S DAC (Olimex PC) define no
    // I2S pins at all, so the pin report has to follow the audio type
    // rather than being printed unconditionally.
#if FEATURE_AUDIO_I2S
    DBG_PRINT("Initializing I2S Audio...\n");
    DBG_PRINT("  DATA: GPIO%d, CLK: GPIO%d, LRCK: GPIO%d\n",
           I2S_DATA_PIN, I2S_CLOCK_PIN_BASE, I2S_CLOCK_PIN_BASE + 1);
#else
    DBG_PRINT("Initializing PWM Audio...\n");
    DBG_PRINT("  LEFT: GPIO%d, RIGHT: GPIO%d\n", PWM_LEFT_PIN, PWM_RIGHT_PIN);
#endif
    audio_set_enabled(false);
    audio_init();
    audio_set_volume(config_get_volume());
    audio_set_enabled(true);
    config_clear_changes();
    while(!initialized) {
        sleep_ms(1);
        __dmb();
    }
    static repeating_timer_t m_timer = { 0 };
    int hz = 44100;
    add_repeating_timer_us(-1000000 / hz, timer_callback0, pc, &m_timer);
    while(1) {
        repeat_me_often();
#if FRANK_AUDIO_DIAG
        /* The DWT comparator is per core, so core 1 has to arm its own.
         * Video and audio both run here and neither was ever watched. */
        if (FRANK_DIAG->mon_addr && !FRANK_DIAG->mon_c1) {
            FRANK_DIAG->mon_c1 = 1u;
            frank_mon_arm(FRANK_DIAG->mon_addr);
        }
#endif
        sleep_us(1);
    }
    __unreachable();
}

//=============================================================================
// Welcome Screen
//=============================================================================

static void show_welcome_screen(void) {
    // Welcome screen dimensions
    int wx = 14, wy = 6, ww = 51, wh = 14;

    osd_clear();

    // Draw the window content once (static, won't flicker)
    osd_draw_box(wx, wy, ww, wh, OSD_ATTR_BORDER);
    osd_fill(wx + 1, wy + 1, ww - 2, wh - 2, ' ', OSD_ATTR_NORMAL);

    // Title
    osd_print_center(wy + 3, "FRANK 386", OSD_ATTR(OSD_YELLOW, OSD_BLUE));

    // Version
    char version_str[32];
    snprintf(version_str, sizeof(version_str), "Version %d.%02d",
             FRANK_386_VERSION_MAJOR, FRANK_386_VERSION_MINOR);
    osd_print_center(wy + 5, version_str, OSD_ATTR_NORMAL);

    // Author
    osd_print_center(wy + 6, "Port by Mikhail Matveev & DnCraptor", OSD_ATTR_NORMAL);
    osd_print_center(wy + 7, "https://github.com/rh1tech/frank-386", OSD_ATTR_NORMAL);

    // Hardware info
    char hw_str[50];
    snprintf(hw_str, sizeof(hw_str), "RP: %d, PSRAM: %d, Flash: %d",
             config_get_cpu_freq(), config_get_psram_freq(), config_get_flash_freq());
    osd_print_center(wy + 9, hw_str, OSD_ATTR(OSD_LIGHTCYAN, OSD_BLUE));

    // Platform (green text)
#ifdef BOARD_M1
    osd_print_center(wy + 10, "Platform: M1", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#elif defined(BOARD_M2)
    osd_print_center(wy + 10, "Platform: M2", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#elif defined(BOARD_PC)
    osd_print_center(wy + 10, "Platform: Olimex PICO-PC", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#elif defined(BOARD_Z2)
    osd_print_center(wy + 10, "Platform: RP2350-PiZero", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#elif defined(BOARD_C2)
    osd_print_center(wy + 10, "Platform: FRANK Core 2", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#else
    osd_print_center(wy + 10, "Platform: Unknown", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#endif

    osd_show();

    // Animate plasma background for 7 seconds (700 frames at ~10ms each)
    // Window area is skipped by osd_draw_plasma_background, so it won't flicker
    for (int frame = 0; frame < 700; frame++) {
        int is_down = 0, keycode = 0;
#ifdef BOARD_HAS_PS2
        ps2kbd_tick();
        ps2kbd_get_key(&is_down, &keycode);
#endif
#ifdef USB_HID_ENABLED
        if (!is_down) {
            usbkbd_tick();
            usbkbd_get_key(&is_down, &keycode);
        }
#endif
        if (is_down) break;
        osd_draw_plasma_background(frame * 3, wx, wy, ww, wh);
        sleep_ms(10);
    }

    osd_hide();
}

//=============================================================================
// Main Entry Point
//=============================================================================

int main(void) {
    // Initialize stdio (USB Serial or UART depending on USB HID mode)
    stdio_init_all();
    #ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    #endif
    DBG_PRINT("\n\n");
    DBG_PRINT("============================================\n");
    DBG_PRINT("  frank-386 - 386 Emulator for RP2350\n");
    DBG_PRINT("  Version %d.%02d\n", FRANK_386_VERSION_MAJOR, FRANK_386_VERSION_MINOR);
    DBG_PRINT("============================================\n\n");

#ifndef USB_HID_ENABLED
    // Wait for USB Serial connection (with timeout)
    // Only when USB CDC is enabled (USB HID disabled)
    DBG_PRINT("Waiting for USB Serial connection...\n");
    DBG_PRINT("(Press any key or wait %d seconds)\n\n", USB_CONSOLE_DELAY_MS / 1000);

    absolute_time_t deadline = make_timeout_time_ms(USB_CONSOLE_DELAY_MS);
    while (!stdio_usb_connected() && !time_reached(deadline)) {
        sleep_ms(100);
    }

    if (stdio_usb_connected()) {
        DBG_PRINT("USB Serial connected!\n\n");
    } else {
        DBG_PRINT("Timeout - continuing without USB Serial\n\n");
    }
#else
    // USB HID mode: using UART for debug output
    DBG_PRINT("USB HID mode: USB port used for keyboard input\n");
    DBG_PRINT("Debug output via UART\n\n");
#endif

    // Print board configuration
    DBG_PRINT("Board Configuration:\n");
#ifdef BOARD_M1
    DBG_PRINT("  Board: M1\n");
#elif defined(BOARD_M2)
    DBG_PRINT("  Board: M2\n");
#elif defined(BOARD_C2)
    DBG_PRINT("  Board: C2 (FRANK Core 2 master)\n");
#else
    DBG_PRINT("  Board: Unknown\n");
#endif
    DBG_PRINT("  CPU Speed: %d MHz\n", CPU_CLOCK_MHZ);
    DBG_PRINT("  PSRAM Speed: %d MHz\n", PSRAM_MAX_FREQ_MHZ);
    DBG_PRINT("\n");

    // Initialize hardware
    if (!init_hardware()) {
        printf("\nHardware initialization failed!\n");
        while (true) {
            sleep_ms(1000);
        }
    }

    // Initialize emulator
    if (!init_emulator()) {
        printf("\nEmulator initialization failed!\n");
        while (true) {
            sleep_ms(1000);
        }
    }

    // Start the core-0 cycle counter before emulation begins.
    prof_init();
    /*
     * Sampling is NOT started here.  A 10 kHz SysTick handler running from
     * boot buys nothing - Win+F10 zeroes the histogram before every real
     * measurement anyway - and it means the profiler build behaves
     * differently from the shipping build during SD mount, HDMI bring-up and
     * the welcome animation.  ps_init() now runs only from
     * profile_reset_runtime(), i.e. on Win+F10.
     */
    prof_mem_bench();
#if defined(SUBSYS_PROFILE) && defined(BOARD_C2) && !REMOTE_MEM
    /* Skipped when REMOTE_MEM is on: init_emulator() has already brought
     * the link up, and a second linkf_init() would re-claim the PIO. */
    prof_link_bench();
#endif

    initialized = true;

    // HDMI DMA starts at normal priority to avoid starving SD/PSRAM during
    // BIOS loading.  Now that all ROMs are in PSRAM, raise DMA priority for
    // glitch-free video output.
    {
        extern bool SELECT_VGA;
        if (!SELECT_VGA) {
            extern void hdmi_set_dma_high_priority(void);
            hdmi_set_dma_high_priority();
            DBG_PRINT("HDMI DMA priority raised\n");
        }
    }

    // Show welcome screen
    DBG_PRINT("\nAbout to show welcome screen...\n");
    if(*(uint32_t*)(0x20000000 + (512ul << 10) - 32) != 0x1927fa52) // magic to fast reboot
        show_welcome_screen();
    DBG_PRINT("Welcome screen done.\n");

    DBG_PRINT("\nStarting emulation...\n");

#if THROTTLING
    // Frame rate throttling for audio sync
    // Target ~60fps to match audio processing rate (16666us per frame)
    uint64_t frame_start_time = time_us_64();
    const uint64_t target_frame_time_us = 16666; // 60Hz = 16.666ms per frame
    int frame_step_count = 0;
    const int steps_per_frame = 100; // Number of outer loop iterations per frame
#endif
    // Retrace-based frame submission state
    static int last_vga_mode = -1;

    // Main emulation loop (Core 0)
    g_diag_stage = DIAG_MAIN_LOOP;
    while (true) {
        // Skip CPU execution when paused (disk UI or settings UI active)
        if (pc->paused) {
            // Still poll keyboard to handle UI input
            poll_keyboard();

            // Animate plasma background for active UI
            if (diskui_is_open()) {
                diskui_animate();
            } else if (settingsui_is_open()) {
                settingsui_animate();
            }

            sleep_ms(16);  // ~60Hz polling/animation rate
            continue;
        }

        autotype_tick();
        swdkey_tick();
        temp_tick();
#if PC_SAMPLE
        if (g_ps_trigger) {
            uint32_t req = g_ps_trigger;
            g_ps_trigger = 0;
            if (req == 1u) profile_reset_runtime();   /* start a clean window */
            else           ps_stop();                 /* freeze the histogram */
        }
#endif

        // Run CPU steps - batch multiple steps for efficiency
        for (int i = 0; i < 10; i++) {
            pc_step(pc);
            throughput_tick();
#if FRANK_AUDIO_DIAG
            if (FRANK_DIAG->mon_addr && !FRANK_DIAG->mon_armed) {
                FRANK_DIAG->mon_armed = 1u;
                frank_mon_arm(FRANK_DIAG->mon_addr);
            }
#endif
        }

#if FRANK_AUDIO_DIAG
        /*
         * Backstop: read the byte itself now and then.
         *
         * The comparator only sees accesses made by a core.  If the byte
         * turns over while mon_hits is still zero, nothing executing on
         * either core wrote it, and every remaining explanation lies
         * outside the emulator - a bus master, or the PSRAM losing it.
         * That is worth far more than another round of instrumenting store
         * paths, so it deserves its own answer rather than being inferred
         * from the watchpoint's silence.
         *
         * Once per 64 batches is about 300 us of guest time, and one
         * uncached byte read costs less than a single guest instruction.
         */
        if (FRANK_DIAG->mon_addr && FRANK_DIAG->mon_bad && !FRANK_DIAG->ud_hit) {
            static uint32_t mon_poll_div;
            if ((++mon_poll_div & 63u) == 0u) {
                uint32_t a = (FRANK_DIAG->mon_addr & 0x00ffffffu) | 0x15000000u;
                uint32_t v = *(volatile uint8_t *)a;
                FRANK_DIAG->mon_polls = FRANK_DIAG->mon_polls + 1u;
                FRANK_DIAG->mon_seen = v;
                if (v == FRANK_DIAG->mon_bad) {
                    if (!FRANK_DIAG->mon_hits) FRANK_DIAG->mon_nowp = 1u;
                    FRANK_DIAG->ud_reason = 7u;
                    FRANK_DIAG->ud_hit = 1u;
                }
            }
        }
#endif

        // Poll keyboard less frequently (every 20 iterations ~5ms)
        // Keyboard events are buffered, so missing a few cycles is fine
        static int poll_counter = 0;
        if (++poll_counter >= 20) {
            poll_counter = 0;
            poll_keyboard();
        }

        // Check for reset request
        if (pc->reset_request) {
            /* Guest-initiated reset: reloads the BIOS but leaves the RP2350
             * running, so counters survive. Counted, not fatal. */
            watchdog_hw->scratch[3]++;
            pc->reset_request = 0;
            *(uint32_t*)(0x20000000 + (512ul << 10) - 32) = 0x1927fa52; // magic to fast reboot
            load_bios_and_reset(pc);
        }

        // Check for settings UI restart request (requires full RP reset)
        if (settingsui_restart_requested()) {
            settingsui_clear_restart();
            DBG_PRINT("Settings changed - triggering RP reset...\n");
            // Full hardware reset via watchdog
            *(uint32_t*)(0x20000000 + (512ul << 10) - 32) = 0x1927fa52; // magic to fast reboot
            watchdog_reboot(0, 0, 0);
        }

        // Check for shutdown
        if (pc->shutdown_state) {
            /* FRANK_FAULT_BOX: name the exit before it reboots the board.
             * A guest shutdown (a triple fault, typically) is answered here by
             * a full watchdog_reboot(), which looks from the outside exactly
             * like a hardware fault - .bss wiped, console gone, counters back
             * to zero. Recording it distinguishes "the emulated CPU gave up"
             * from "the RP2350 crashed". */
            watchdog_hw->scratch[0] = 0x53485554u;   /* "SHUT" */
            watchdog_hw->scratch[1] = (uint32_t)pc->shutdown_state;
            watchdog_hw->scratch[2] = g_diag_stage;
            break;
        }
#if THROTTLING
        // Frame rate throttling for audio synchronization
        frame_step_count++;
        if (frame_step_count >= steps_per_frame) {
            frame_step_count = 0;
            uint64_t now = time_us_64();
            uint64_t elapsed = now - frame_start_time;

            // If we finished the frame early, wait for the remaining time
            if (elapsed < target_frame_time_us) {
                uint64_t sleep_time = target_frame_time_us - elapsed;
                sleep_us(sleep_time);
            }
            // Reset frame timer for next frame
            frame_start_time = time_us_64();
        }
#endif
    }

    DBG_PRINT("\nEmulation stopped.\n");
    *(uint32_t*)(0x20000000 + (512ul << 10) - 32) = 0x1927fa52; // magic to fast reboot
    watchdog_reboot(0, 0, 0);
    while (true);
    __unreachable();
    return 0;
}
