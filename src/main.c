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

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/regs/powman.h"
#include "hardware/structs/powman.h"

#include "hardware/structs/qmi.h"

#ifdef LIB_PICO_STDIO_UART
#include "hardware/uart.h"
#endif

#include "board_config.h"
#include "psram_init.h"
#include "core0_stack.h"
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
#include "ega128_paging.h"
#endif
#include "ems.h"
#include "vga_hw.h"
#include "vga.h"
#include "ps2.h"
#include "ps2kbd_wrapper.h"
#ifdef USB_HID_ENABLED
#include "usbkbd_wrapper.h"
#include "usbmouse_wrapper.h"
#include "usbgamepad.h"
#include "usbmsc_device.h"
#endif
#ifdef NESPAD_GPIO_CLK
#include "nespad.h"
#endif
#include "sdcard.h"
#include "fdos/psram_layout.h"
#include "ff.h"
#include "audio.h"

#include "pc.h"
#include "tsr_callback.h"
#include "mem.h"
#include "bulk_bounce.h"
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
bool ega128_paging_flush(void);
#endif
#include "i8259.h"
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
static volatile bool audio_timer_ready = false;
static volatile bool early_psram_missing = false;

volatile uint16_t current_vreg_mv = 1100;

static uint16_t vreg_to_mv(enum vreg_voltage v) {
    switch ((int)v) {
    case 15: return 1300;
    case 16: return 1350;
    case 17: return 1400;
    case 18: return 1500;
    case 19: return 1600;
    case 20: return 1650;
    default: return 1100;
    }
}

/*
 * True only for a real RP2350 power-on reset.
 *
 * POWMAN reset-cause bits can coexist/stay latched across later reset paths,
 * so HAD_POR alone is not sufficient.  Reject every other explicit reset
 * cause as well as watchdog resets.  This makes the welcome screen a cold
 * power-on feature rather than a "not our SRAM cookie" heuristic.
 */
static bool is_power_on_boot(void)
{
    const uint32_t reset = powman_hw->chip_reset;

    if (watchdog_caused_reboot())
        return false;

    if (!(reset & POWMAN_CHIP_RESET_HAD_POR_BITS))
        return false;

    return !(reset & (
        POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS |
        POWMAN_CHIP_RESET_HAD_BOR_BITS |
        POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS |
        POWMAN_CHIP_RESET_HAD_RESCUE_BITS |
        POWMAN_CHIP_RESET_HAD_HZD_SYS_RESET_REQ_BITS |
        POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS |
        POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS));
}

#ifdef I386_MODE
/*
 * Interpreter throughput, only available with the i386 core.
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
#endif /* I386_MODE */

// Framebuffer for VGA output (in PSRAM)
static uint8_t *framebuffer = NULL;

// FatFS state
static FATFS fatfs;

// Flag to track if VGA is initialized (for error display)
static volatile bool vga_initialized = false;

/* Timed, non-blocking CPU feature warning. */
static uint64_t cpu_feature_warning_until_us = 0;

/*
 * Native BIOS initializes both PIC IMRs to 00h (all IRQs enabled).  A guest
 * that is forcibly terminated during a protected-mode transition cannot run
 * its own cleanup and may leave both PICs masked.  Restore the native BIOS
 * interrupt-mask state when dismissing this specific warning.
 */
static void cpu_feature_warning_recover_pic(void)
{
    if (!pc || !pc->pic)
        return;

    i8259_ioport_write(pc->pic, 0x21, 0x00);
    i8259_ioport_write(pc->pic, 0xA1, 0x00);
}

void emulator_unsupported_cpu_feature(const char *mnemonic,
                                      uint16_t cs, uint16_t ip)
{
    char where[48];

    printf("WARNING: unsupported 80286 feature at %04X:%04X: %s\n",
           cs, ip, mnemonic ? mnemonic : "unknown");

    if (!vga_initialized)
        return;

    osd_init();
    osd_clear();

    const int box_w = 66;
    const int box_h = 10;
    const int box_x = (OSD_COLS - box_w) / 2;
    const int box_y = (OSD_ROWS - box_h) / 2;
    const uint8_t attr = OSD_ATTR(OSD_BLACK, OSD_YELLOW);

    osd_fill(box_x, box_y, box_w, box_h, ' ', attr);
    osd_draw_box_titled(box_x, box_y, box_w, box_h,
                        " Unsupported 80286 feature ", attr);
    osd_print(box_x + 3, box_y + 2,
              mnemonic ? mnemonic : "Unsupported instruction", attr);
    snprintf(where, sizeof(where), "Guest instruction at %04X:%04X", cs, ip);
    osd_print(box_x + 3, box_y + 4, where, attr);
    osd_print(box_x + 3, box_y + 6,
              "Program terminated.", attr);
    osd_print(box_x + 3, box_y + 7,
              "Emulator continues, if state is not corrupted.", attr);
    osd_show();

    cpu_feature_warning_until_us = time_us_64() + 4000000ull;
}

static void cpu_feature_warning_tick(void)
{
    if (!cpu_feature_warning_until_us)
        return;
    if (time_us_64() < cpu_feature_warning_until_us)
        return;

    cpu_feature_warning_until_us = 0;
    cpu_feature_warning_recover_pic();

    /*
     * Do not hide an OSD subsequently taken over by a normal UI.
     * Otherwise this warning expires independently of emulation.
     */
    if (!diskui_is_open() && !settingsui_is_open())
        osd_hide();
}

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
    snprintf(path, sizeof(path), SD_DATA_DIR_SLASH "%s", file);

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

#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (ega128_paging_active()) {
        uint32_t guest_addr = backward ? (uint32_t)(addr - size) : (uint32_t)addr;
        FSIZE_t left = size;
        while (left) {
            UINT chunk = left > GUEST_BULK_BUF_SIZE ? GUEST_BULK_BUF_SIZE : (UINT)left;
            bytes_read = 0;
            res = f_read(&fp, guest_bulk_buf, chunk, &bytes_read);
            if (res != FR_OK || bytes_read != chunk) {
                f_close(&fp);
                printf("ERROR: Failed to read ROM: %s (error %d, read %u of %u)\n",
                       file, res, bytes_read, chunk);
                return -1;
            }
            for (UINT i = 0; i < chunk; ++i)
                pstore8(guest_addr + i, guest_bulk_buf[i]);
            guest_addr += chunk;
            left -= chunk;
        }
    } else
#endif
    {
        res = f_read(&fp, dest, size, &bytes_read);
        if (res != FR_OK || bytes_read != size) {
            f_close(&fp);
            printf("ERROR: Failed to read ROM: %s (error %d, read %u of %lu)\n",
                   file, res, bytes_read, (unsigned long)size);
            return -1;
        }
    }

    f_close(&fp);

    // Debug: verify data was written to memory
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (ega128_paging_active()) {
        uint32_t first = backward ? (uint32_t)(addr - size) : (uint32_t)addr;
        uint32_t last = first + (uint32_t)size - 8u;
        DBG_PRINT("  First bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                  pload8(first + 0), pload8(first + 1), pload8(first + 2), pload8(first + 3),
                  pload8(first + 4), pload8(first + 5), pload8(first + 6), pload8(first + 7));
        DBG_PRINT("  Last bytes:  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                  pload8(last + 0), pload8(last + 1), pload8(last + 2), pload8(last + 3),
                  pload8(last + 4), pload8(last + 5), pload8(last + 6), pload8(last + 7));
    } else
#endif
    {
        DBG_PRINT("  First bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
               dest[0], dest[1], dest[2], dest[3],
               dest[4], dest[5], dest[6], dest[7]);
        DBG_PRINT("  Last bytes:  %02x %02x %02x %02x %02x %02x %02x %02x\n",
               dest[size-8], dest[size-7], dest[size-6], dest[size-5],
               dest[size-4], dest[size-3], dest[size-2], dest[size-1]);
    }

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

//=============================================================================
// Keyboard Polling
//=============================================================================

// Host-side modifier state. These hotkeys must remain available even when
// the guest CPU, PIC, 8042, or interrupt state is no longer usable.
static bool win_key_pressed = false;
static bool ctrl_key_pressed = false;
static bool alt_key_pressed = false;

/*
 * process_keycode() receives Linux/evdev keycodes from both keyboard
 * wrappers.  usbkbd_wrapper.c maps these explicitly; ps2kbd_wrapper uses
 * the same keycode space.  Keep the few host-hotkey codes local instead of
 * relying on KEY_* names that are not exported by this build's headers.
 */
enum {
    HOST_KEY_LEFTCTRL  = 29,
    HOST_KEY_LEFTALT   = 56,
    HOST_KEY_RIGHTCTRL = 97,
    HOST_KEY_RIGHTALT  = 100,
    HOST_KEY_DELETE    = 111
};

static __attribute__((noreturn)) void hard_reboot(void)
{
    /* Full RP2350 reboot, not a guest CPU/BIOS reset. */
    watchdog_reboot(0, 0, 0);

    while (true)
        tight_loop_contents();
}

#if CONTROL_STACK
/* Refuse to open an OSD dialog (Win+F11/F12) when the core0 stack has grown so
   deep that rendering it would descend into TEXT_BUFFER and corrupt
   text_buffer_sram (the OSD text buffer). __stack_ext_area__ is the top of
   TEXT_BUFFER; OSD_STACK_HEADROOM reserves room for the OSD render call chain
   above it. Companion to the DosExec() guard in task.c (same CONTROL_STACK
   toggle, same "keep a reserve" idea). */
extern uint8_t __stack_ext_area__[];
#ifndef OSD_STACK_HEADROOM
#define OSD_STACK_HEADROOM 2048u
#endif
static inline bool osd_stack_ok(void) {
    uint32_t sp;
    __asm volatile ("mov %0, sp" : "=r" (sp));
    return sp >= (uint32_t)(uintptr_t)&__stack_ext_area__ + OSD_STACK_HEADROOM;
}
#endif

// Process a single keycode, handling host and UI hotkeys
// Returns true if key should be passed to emulator, false if consumed
static bool process_keycode(int is_down, int keycode) {
    /*
     * Track Ctrl/Alt before any modal OSD handling.  Ctrl+Alt+Del is a
     * host-side emergency reset and must work even while the guest is wedged.
     */
    if (keycode == HOST_KEY_LEFTCTRL || keycode == HOST_KEY_RIGHTCTRL)
        ctrl_key_pressed = is_down;
    if (keycode == HOST_KEY_LEFTALT || keycode == HOST_KEY_RIGHTALT)
        alt_key_pressed = is_down;

    if (is_down && keycode == HOST_KEY_DELETE &&
        ctrl_key_pressed && alt_key_pressed) {
        hard_reboot();
    }

    /*
     * Treat the unsupported-CPU warning as a modal OSD for keyboard input:
     * dismiss it on the first key press and consume that press so it does not
     * leak through to the guest application underneath the OSD.
     *
     * Ignore key releases.  Otherwise release of a key that was already held
     * before the warning appeared could close the dialog immediately.
     */
    if (cpu_feature_warning_until_us && is_down) {
        cpu_feature_warning_until_us = 0;
        cpu_feature_warning_recover_pic();
        if (!diskui_is_open() && !settingsui_is_open())
            osd_hide();
        return false;
    }

    // Track Win key state
    if (keycode == KEY_LEFTMETA) {
        win_key_pressed = is_down;
    }

    // Win+F12: enter Disk Manager, or switch to it from Settings.
    if (is_down && keycode == KEY_F12 && win_key_pressed) {
#if CONTROL_STACK
        if (!osd_stack_ok()) {
            DBG_PRINT("OSD (Win+F12) refused: native stack low\n");
            return false;
        }
#endif
        if (settingsui_is_open()) {
            settingsui_close();
            diskui_open();
        } else if (!diskui_is_open()) {
            diskui_open();
            if (pc) {
                pc->paused = 1;
                audio_set_enabled(false);
            }
        }
        return false;  // Don't pass to emulator
    }

    // Win+F11: enter Settings, or switch to it from Disk Manager.
    if (is_down && keycode == KEY_F11 && win_key_pressed) {
#if CONTROL_STACK
        if (!osd_stack_ok()) {
            DBG_PRINT("OSD (Win+F11) refused: native stack low\n");
            return false;
        }
#endif
        if (diskui_is_open()) {
            diskui_close();
            settingsui_open();
        } else if (!settingsui_is_open()) {
            settingsui_open();
            if (pc) {
                pc->paused = 1;
                audio_set_enabled(false);
            }
        }
        return false;  // Don't pass to emulator
    }

    // When disk UI is open, route all keys to it
    if (diskui_is_open()) {
        diskui_handle_key(keycode, is_down);

        // Leaving the OSD session in DEVICE mode always detaches the
        // exported image and reboots. F11/F12 page switches do not exit OSD.
        if (!diskui_is_open() && pc && pc->paused) {
#ifdef USB_HID_ENABLED
            if (config_get_usb_mode() == USB_MODE_DEVICE) {
                usbmsc_device_shutdown();
                hard_reboot();
            }
#endif
            pc->paused = 0;
            audio_set_enabled(true);
        }
        return false;  // Don't pass to emulator
    }

    // When settings UI is open, route all keys to it
    if (settingsui_is_open()) {
        settingsui_handle_key(keycode, is_down);

        // Same OSD-session exit rule regardless of the page used last.
        if (!settingsui_is_open() && pc && pc->paused) {
#ifdef USB_HID_ENABLED
            if (config_get_usb_mode() == USB_MODE_DEVICE) {
                usbmsc_device_shutdown();
                hard_reboot();
            }
#endif
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
    // Drain PS/2 events. Raw PS/2 input is serviced by the core0 timer.
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

    /*
     * platform_poll() is called from pc_step(), including the nested pc_step()
     * loops used by native FDOS/FCOM.  A native command processor can own the
     * C stack indefinitely, so main()'s outer loop is not guaranteed to run
     * while an OSD warning is visible.
     */
    cpu_feature_warning_tick();
    poll_keyboard();
    // VGA update is handled by Core 1, don't call here to avoid contention
    if (pc && pc->reset_request) {
        pc->reset_request = 0;
rst:
        watchdog_reboot(0, 0, 0);
        while (true);
    }
    /* Same reachability problem as above: the settings-UI restart request
       was also only checked in main()'s outer loop. */
    if (settingsui_restart_requested()) {
        settingsui_clear_restart();
        goto rst;
    }
}

//=============================================================================
// Configuration Loading
//=============================================================================

static void load_default_config(void) {
    memset(&config, 0, sizeof(config));

    // Guest RAM follows the physically detected PSRAM.  The optional
    // Lo-tech EMS board reserves its fixed 2 MiB backing store at the top.
    size_t detected_psram = psram_detected_size();
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (guest_ram_base == ram_pages)
        detected_psram = ega128_paging_active() ? EGA128_VIRTUAL_RAM_SIZE : RAM_PAGES_SIZE;
#endif
#if EMULATE_LTEMS
    if (detected_psram >= (4u << 20)) {
        config.mem_size = detected_psram - (2u << 20);
        ems_base_ptr = PC_RAM + config.mem_size;
    } else {
        config.mem_size = detected_psram;
        ems_base_ptr = NULL;
    }
#else
    config.mem_size = detected_psram;
#endif
    config.vga_mem_size = EMU_VGA_MEM_SIZE_KB * 1024;

    // CPU configuration
    config.cpu_gen = EMU_CPU_GEN;
    config.fpu = 0;  // Disabled for initial port

    // Display configuration
    config.width = 640;
    config.height = 400;

    // BIOS files (relative to 386 directory on SD card)
    // NULL main BIOS means Native BIOS generated by bios_post().
    config.bios = NULL;
    config.vga_bios = "vgabios.bin";

    // No disks by default (set via INI file)
    for (int i = 0; i < 4; i++) {
        config.ata[i] = NULL;
        config.iscd[i] = 0;
    }
    config.raw_sd_hdd = 0;
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
    res = f_opendir(&dir, SD_DATA_DIR);
    if (res == FR_OK) {
        DBG_PRINT("  " SD_DATA_DIR_SLASH " directory found, contents:\n");
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
            DBG_PRINT("    %s%s (%lu bytes)\n",
                   fno.fname,
                   (fno.fattrib & AM_DIR) ? "/" : "",
                   (unsigned long)fno.fsize);
        }
        f_closedir(&dir);
    } else {
        DBG_PRINT("  " SD_DATA_DIR_SLASH " directory not found (error %d)\n", res);
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
    snprintf(path, sizeof(path), SD_DATA_DIR_SLASH "%s", filename);

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
    current_vreg_mv = vreg_to_mv(CPU_VOLTAGE);
    sleep_ms(100);  // Stabilization delay

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
    if (mhz > 504) return VREG_VOLTAGE_1_65;
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
            current_vreg_mv = vreg_to_mv(new_voltage);
        } else {
            // RAISING: voltage first, then clock (safe order)
            vreg_disable_voltage_limit();
            vreg_set_voltage(new_voltage);
            current_vreg_mv = vreg_to_mv(new_voltage);
            sleep_ms(50);  // Stabilization delay
            set_flash_timings(cpu_mhz, cfg_flash);
            set_sys_clock_khz(cpu_mhz * 1000, false);
        }
        console_reclock();
    }

    // Re-initialize PSRAM with the new frequency
    psram_init_with_freq(psram_pin, psram_mhz);

    // Recalculate VGA PIO clock divider (vga_hw_init ran before this call)
    vga_hw_reclock();

    DBG_PRINT("Clock reconfiguration complete: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
}

//=============================================================================
// Hardware Initialization
//=============================================================================
#ifdef DIAG_ENABLED
#include "diag.h"        /* on-device fault/hang catcher, gated by CMake */
#endif

static void core1_entry(void);
static bool init_hardware(void) {
    // Configure clocks (including overclock if enabled)
    configure_clocks();

    // Initialize PSRAM first
    DBG_PRINT("Initializing PSRAM...\n");
    uint psram_pin = get_psram_pin();
    DBG_PRINT("  PSRAM CS pin: GPIO%d\n", psram_pin);
    psram_init(psram_pin);

    /* Probe PSRAM before video as before, but do not stop here: the error OSD
     * can only be shown after the video core has completed initialization.
     * Full/cached capacity handling remains below, after clock configuration. */
    bool psram_missing = psram_detect_size() < (1u << 20);
    early_psram_missing = psram_missing;
    __dmb();

#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (psram_missing) {
        guest_ram_base = ram_pages;
        printf("PSRAM not detected; using %u KiB SRAM guest-RAM fallback\n", (unsigned)(RAM_PAGES_SIZE >> 10));
    } else {
        guest_ram_base = (uint8_t *)PSRAM_BASE_ADDR;
        ega128_select_direct_backend();
    }
#else
    if (psram_missing)
        printf("ERROR: PSRAM not detected or smaller than 1 MiB!\n");
#endif
    // Initialize VGA early so we can show errors on screen
    multicore_launch_core1(core1_entry);

    while(!vga_initialized) {
        sleep_ms(1);
        __dmb();
    }
    __dmb();

#if !defined(EGA128) && !defined(VGA128) && !defined(MCGA)
    if (psram_missing) {
        show_error_screen(" PSRAM Error ",
                          "QSPI PSRAM not detected (>= 4 MB is required).",
                          "Try a reduced-VRAM (EGA128/VGA128/MCGA) build.");
        __unreachable();
    }
#endif

    // Initialize SD card
    DBG_PRINT("Initializing SD card...\n");
    FRESULT res = f_mount(&fatfs, "", 1);
    if (res != FR_OK) {
        char detail[32];

        if (res == FR_NOT_READY) {
            show_error_screen(" SD Card Error ",
                              "SD card not found or not ready.",
                              "Insert a card and restart.");
            __unreachable();
        }
        if (res == FR_NO_FILESYSTEM) {
            show_error_screen(" SD Card Error ",
                              "SD card has no supported filesystem.",
                              "Format it as FAT and restart.");
            __unreachable();
        }

        snprintf(detail, sizeof(detail), "FatFS error code: %d", res);
        show_error_screen(" SD Card Error ", "Failed to mount SD card.", detail);
        __unreachable();
    }
    DBG_PRINT("  SD card mounted\n");

    // A fresh card is valid: create the per-CPU data directory on demand.
    if (!config_ensure_data_dir()) {
        show_error_screen(" Directory Error ",
                          "Failed to create directory '" SD_DATA_DIR_SLASH "'.",
                          "Check SD card write protection/filesystem.");
        __unreachable();
    }
    DBG_PRINT("  " SD_DATA_DIR_SLASH " directory ready\n");

    // Load frank-386-specific hardware settings from INI
    // This allows cpu_freq and psram_freq to be configured
    {
        FIL fp;
        char *content = NULL;

        if (f_open(&fp, SD_DATA_DIR_SLASH "config.ini", FA_READ) == FR_OK) {
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

    /* A real power-on follows the same reset-cause test as the welcome screen:
     * always redetect and fully POST-test PSRAM.  Warm/watchdog reboots may
     * reuse the previously tested capacity. */
    if (is_power_on_boot())
        config_invalidate_psram_test_cache_runtime();

    /* PSRAM capacity is stable hardware configuration.  Reuse the capacity
     * saved after a successful full POST test when it was tested at the
     * current PSRAM clock; otherwise detect it through the uncached XIP alias. */
    {
        int cached_mb = config_get_psram_size_mb();
        int cfg_psram = config_get_psram_freq();
        int test_freq = config_get_psram_test_freq();
        size_t detected_psram;

        if (!psram_missing &&
            (cached_mb == 1 || cached_mb == 2 || cached_mb == 4 ||
             cached_mb == 8 || cached_mb == 16) &&
            test_freq == cfg_psram) {
            detected_psram = (size_t)cached_mb << 20;
            psram_set_detected_size(detected_psram);
            DBG_PRINT("  PSRAM cached: %d MiB (tested at %d MHz)\n",
                      cached_mb, test_freq);
        } else {
            detected_psram = psram_missing ? 0 : psram_detect_size();
            if (detected_psram < (1u << 20)) {
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
                if (!ega128_paging_init()) {
                    printf("ERROR: guest-RAM paging backing store initialization failed!\n");
                    return false;
                }
                guest_ram_base = ram_pages;
                detected_psram = 0;
#else
                printf("ERROR: PSRAM not detected or smaller than 1 MiB!\n");
                return false;
#endif
            }
            DBG_PRINT("  PSRAM detected: %lu MiB\n",
                      (unsigned long)(detected_psram >> 20));
        }
    }

#ifdef BOARD_HAS_PS2
    // Initialize unified PS/2 driver (keyboard + mouse on shared PIO)
    DBG_PRINT("Initializing PS/2 (unified driver)...\n");
    DBG_PRINT("  Keyboard CLK: GPIO%d, DATA: GPIO%d\n", PS2_PIN_CLK, PS2_PIN_DATA);
    DBG_PRINT("  Mouse    CLK: GPIO%d, DATA: GPIO%d\n", PS2_MOUSE_CLK, PS2_MOUSE_DATA);
    if (!ps2_init(PIO_PS2KBD, PS2_PIN_CLK, PS2_MOUSE_CLK)) {
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
    if (config_get_usb_mode() == USB_MODE_HOST) {
        DBG_PRINT("Initializing USB HID host...\n");
        usbkbd_init();
    }
#endif

    // Initialize NES/SNES gamepad (if pins defined for this board)
#ifdef NESPAD_GPIO_CLK
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

static bool init_emulator(void) {
    // Load configuration
    load_default_config();

    // Try to load config from SD card
    if (load_config_from_sd("config.ini") != 0) {
        DBG_PRINT("Using default configuration\n");
    }

    /* init_hardware() already invalidates the PSRAM test cache on a real
     * power-on, but load_config_from_sd() above parses [frank-386] again and
     * restores psram_size/psram_test_freq from disk.  Drop only the runtime
     * copy again so the upcoming BIOS POST performs the full memory test.
     * Warm/watchdog reboots keep the saved cache and skip the test. */
    if (is_power_on_boot())
        config_invalidate_psram_test_cache_runtime();

    DBG_PRINT("\nEmulator configuration:\n");
    DBG_PRINT("  Memory: %ld MB\n", config.mem_size / (1024 * 1024));
    DBG_PRINT("  VGA Memory: %ld KB\n", config.vga_mem_size / 1024);
    DBG_PRINT("  CPU: %d86\n", config.cpu_gen);
    DBG_PRINT("  BIOS: %s\n", config.bios ? config.bios : "(none)");
    DBG_PRINT("  VGA BIOS: %s\n", config.vga_bios ? config.vga_bios : "(none)");
    DBG_PRINT("  Floppy A: %s\n", config.fdd[0] ? config.fdd[0] : "(none)");
    DBG_PRINT("  Floppy B: %s\n", config.fdd[1] ? config.fdd[1] : "(none)");

    size_t detected_psram = psram_detected_size();
    DBG_PRINT("  PSRAM detected: %lu KB; guest RAM: %lu KB\n",
              (unsigned long)(detected_psram / 1024),
              (unsigned long)(config.mem_size / 1024));

    /*
     * Direct-QSPI builds can use the currently unowned extended-memory tail as
     * an L2 FatFs cache.  config.mem_size already excludes the fixed LTEMS
     * backing store when EMM is enabled.  Keep the cache below both that guest
     * boundary and the fixed native-stack arena; XMS/native owners will raise
     * the reclaim floor dynamically as they consume PSRAM.
     */
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    const bool direct_qspi_guest = !ega128_paging_active();
#else
    const bool direct_qspi_guest = true;
#endif
    if (direct_qspi_guest &&
        detected_psram > FDOS_XMS_EMB_BASE_PHYS +
                             ARM_ELF_NATIVE_STACK_ARENA_SIZE) {
        uintptr_t low = (uintptr_t)PSRAM_BASE_ADDR + FDOS_XMS_EMB_BASE_PHYS;
        uintptr_t guest_end = (uintptr_t)PSRAM_BASE_ADDR + config.mem_size;
        uintptr_t stack_begin = (uintptr_t)PSRAM_BASE_ADDR + detected_psram -
                                ARM_ELF_NATIVE_STACK_ARENA_SIZE;
        uintptr_t high = guest_end < stack_begin ? guest_end : stack_begin;
        if (high > low)
            sdcard_enable_ff_qspi_cache((void *)low, (void *)high);
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

    // Create PC instance
    DBG_PRINT("\nCreating PC instance...\n");
    pc = pc_new(vga_redraw, platform_poll, NULL, NULL, &config);
    if (!pc) {
        printf("ERROR: Failed to create PC instance\n");
        return false;
    }

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
    config_set_cpu_gen(config.cpu_gen);
    config_set_fpu(config.fpu);
    config_set_redirector(config.redirector);
    config_set_bios_file(config.bios);
    config_set_raw_sd_hdd(config.raw_sd_hdd);
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

    // Check external BIOS file exists before loading.
    // NULL/empty BIOS is the Native mode and is handled by bios_post().
    DBG_PRINT("Loading BIOS...\n");
    if (config.bios && config.bios[0]) {
        char bios_path[256];
        FIL fp;
        snprintf(bios_path, sizeof(bios_path), SD_DATA_DIR_SLASH "%s", config.bios);
        if (f_open(&fp, bios_path, FA_READ) != FR_OK) {
            char detail[64];
            snprintf(detail, sizeof(detail), "File: %s", bios_path);
            show_error_screen(" Missing BIOS ", "BIOS file not found.", detail);
            __unreachable();
        }
        f_close(&fp);
    }

    // Select cold/warm native POST from the actual RP2350 reset cause.
    // bios_post() consumes the cold state after the first native POST.
    pc_set_cold_post_pending(is_power_on_boot());

    // Load BIOS and reset CPU
    load_bios_and_reset(pc);

    return true;
}

bool timer_callback(repeating_timer_t *rt);
void vga_hw_process_deferred(void);

static void __not_in_flash_func(default_tsr0)(void)
{
    i8254_update_irq(pc->pit);
    cmos_update_irq(pc->cmos);
}

static void __not_in_flash_func(default_tsr1)(void)
{
}

static tsr_callback_t tsr0_callback = default_tsr0;
static tsr_callback_t tsr1_callback = default_tsr1;

tsr_callback_t __not_in_flash_func(set_tsr0_callback)(tsr_callback_t cb)
{
    return __atomic_exchange_n(&tsr0_callback, cb, __ATOMIC_ACQ_REL);
}

tsr_callback_t __not_in_flash_func(set_tsr1_callback)(tsr_callback_t cb)
{
    return __atomic_exchange_n(&tsr1_callback, cb, __ATOMIC_ACQ_REL);
}

#ifdef BOARD_HAS_PS2
#define PS2_TIMER_POLL_DIV 46u  /* 46 * 22 us ~= 1.0 ms */
static uint8_t ps2_timer_poll_div;
#endif

static bool __not_in_flash_func(timer_callback_core0)(repeating_timer_t *rt)
{
    (void)rt;
    tsr_callback_t cb = __atomic_load_n(&tsr0_callback, __ATOMIC_ACQUIRE);
    if (cb)
        cb();
    return true;
}

static bool __not_in_flash_func(timer_callback0)(repeating_timer_t *rt) {
    timer_callback(rt);
#ifdef BOARD_HAS_PS2
    if (++ps2_timer_poll_div >= PS2_TIMER_POLL_DIV) {
        ps2_timer_poll_div = 0;
        ps2kbd_tick();
    }
#endif
    return true;
}

void __not_in_flash_func(tsr1_dispatch)(void)
{
    tsr_callback_t cb = __atomic_load_n(&tsr1_callback, __ATOMIC_ACQUIRE);
    if (cb)
        cb();
}
// to call DMA wait not from ISR for timer
bool repeat_me_often(void);
static void __not_in_flash_func(core1_entry)(void) {

    // Audio comes first so fatal PSRAM errors can signal before video starts.
    // Boards without an I2S DAC (Olimex PC) define no I2S pins at all, so the
    // pin report has to follow the audio type rather than being unconditional.
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

    __dmb();
#if !defined(EGA128) && !defined(VGA128) && !defined(MCGA)
    if (early_psram_missing)
        audio_play_tone(300u, 500u);
#endif
    DBG_PRINT("[Core 1] Initializing video...\n");
    DBG_PRINT("  Base pin: GPIO%d\n", VGA_BASE_PIN);
    vga_hw_init();
    sleep_ms(100);
    vga_initialized = true;

    while(!initialized) {
        sleep_ms(1);
        __dmb();
    }
    static repeating_timer_t m_timer = { 0 };
    int hz = 44100;
	add_repeating_timer_us(-1000000 / hz, timer_callback0, pc, &m_timer);

    __dmb();
    audio_timer_ready = true;
    while(1) {
        vga_hw_process_deferred();
        repeat_me_often();
#ifdef DIAG_ENABLED
        diag_core1_poll();  /* reports if core0's heartbeat stopped */
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
#ifndef I386_MODE
    osd_print_center(wy + 3, "FRANK 286", OSD_ATTR(OSD_YELLOW, OSD_BLUE));
#else
    osd_print_center(wy + 3, "FRANK 386", OSD_ATTR(OSD_YELLOW, OSD_BLUE));
#endif

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

uintptr_t core0_stack_floor_runtime;
uintptr_t core0_stack_top_runtime;
bool core0_stack_uses_gfx_buffer;

static void __attribute__((noinline, noreturn)) main_after_hardware(void);

#if defined(EGA128) || defined(VGA128) || defined(MCGA)
static void __attribute__((noinline, used)) core0_enable_relocated_stack_services(void)
{
    extern uint8_t __Core0StackRegionStart;
    extern uint8_t __Core0StackRegionEnd;
    uintptr_t bottom = (uintptr_t)&__Core0StackRegionStart;
    uintptr_t top = (uintptr_t)&__Core0StackRegionEnd;

    /* SP has already moved into GFX_BUFFER. CORE0_STACK is now permanently
       free and becomes an 8-sector FatFs write-through cache. */
    sdcard_enable_ff_stack_cache((void *)bottom, (size_t)(top - bottom));
}

static void __attribute__((naked, noreturn)) core0_stack_switch_and_continue(uintptr_t new_sp)
{
    __asm volatile (
        "mov sp, r0\n"
        "ldr r1, =core0_stack_floor_runtime\n"
        "ldr r1, [r1]\n"
        "msr msplim, r1\n"
        /* CORE0_STACK becomes reusable only after SP has changed. */
        "bl core0_enable_relocated_stack_services\n"
        "ldr r0, =main_after_hardware\n"
        "bx r0\n"
    );
}
#endif

int main(void) {
    extern uint8_t __text_buffer_area__[];
    extern uint8_t __StackTop;
    core0_stack_floor_runtime = (uintptr_t)&__text_buffer_area__;
    core0_stack_top_runtime = (uintptr_t)&__StackTop;
    core0_stack_uses_gfx_buffer = false;

    /* Initialize .core0_stack_ext (CORE0_STACK_EXT) from its FLASH image before
       anything uses it. Nothing in the SDK crt0 copies this region (unlike
       .data / scratch), so without this any `= {0}` or initialized variable
       parked there would power up as garbage. Must run before init_hardware()
       (video/text buffer) and FDOS. Stack is shallow here, so copying into the
       stack-extension area is safe. */
    {
        extern uint8_t __stack_ext_area__[], __stack_ext_area_end__[];
        extern uint8_t __stack_ext_area_source__[];
        memcpy(__stack_ext_area__, __stack_ext_area_source__,
               (size_t)(__stack_ext_area_end__ - __stack_ext_area__));
        extern uint8_t __text_buffer_area__[], __text_buffer_area_end__[];
        extern uint8_t __text_buffer_area_source__[];
        memcpy(__text_buffer_area__, __text_buffer_area_source__,
               (size_t)(__text_buffer_area_end__ - __text_buffer_area__));
    }

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

#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    /* With direct QSPI guest RAM the ram_pages tail of GFX_BUFFER is unused.
       Reuse that SRAM as a large core0 stack without adding checks to the
       guest-memory hot paths. Once SP has moved, the old CORE0_STACK becomes
       an 8-sector FatFs cache. Paging/fallback builds keep the original stack
       and leave this cache disabled. */
    if (guest_ram_base == (uint8_t *)PSRAM_BASE_ADDR && !ega128_paging_active()) {
        extern uint8_t __gfx_video_end__;
        extern uint8_t __gfx_buffer_end__;
        core0_stack_floor_runtime = (uintptr_t)&__gfx_video_end__;
        core0_stack_top_runtime = (uintptr_t)&__gfx_buffer_end__;
        core0_stack_uses_gfx_buffer = true;
        core0_stack_switch_and_continue(core0_stack_top_runtime);
    }
#endif

    main_after_hardware();
}

void core0_expand_relocated_stack_services(void)
{
#if defined(EGA128) || defined(VGA128) || defined(MCGA)
    if (core0_stack_uses_gfx_buffer) {
        extern uint8_t __Core0StackExtRegionStart;
        extern uint8_t __Core0StackRegionEnd;
        uintptr_t bottom = (uintptr_t)&__Core0StackExtRegionStart;
        uintptr_t top = (uintptr_t)&__Core0StackRegionEnd;

        /* init_kernel() has returned: MenuStruct is dead, while the runtime
           INSTALL= queue lives in ordinary SRAM. Rebuild the same cache over
           the full contiguous 8 KiB old-stack range (16 sectors). */
        sdcard_enable_ff_stack_cache((void *)bottom, (size_t)(top - bottom));
    }
#endif
}

static void __attribute__((noinline, noreturn)) main_after_hardware(void)
{
#ifdef DIAG_ENABLED
    /* Arm MSPLIM, fault handlers and the stall alarm on core0. Must be after
       init_hardware() (it needs the clocks/timer up) and before the pc_step
       loop below (whose heartbeat it watches). */
    diag_init();
#endif

    // Initialize emulator
    if (!init_emulator()) {
        printf("\nEmulator initialization failed!\n");
        while (true) {
            sleep_ms(1000);
        }
    }
    // Start the core-0 cycle counter before emulation begins.
    prof_init();
    ps_init(clock_get_hz(clk_sys), 10000u);   /* 10 kHz PC sampling */
    prof_mem_bench();
#if defined(SUBSYS_PROFILE) && defined(BOARD_C2) && !REMOTE_MEM
    /* Skipped when REMOTE_MEM is on: init_emulator() has already brought
     * the link up, and a second linkf_init() would re-claim the PIO. */
    prof_link_bench();
#endif

    initialized = true;

    /* TSR0 is an OS/native-API source and must execute on core0.  Keep its
       44.1 kHz source rate independent from the core1 audio timer; clients may
       cheaply chain the saved callback on ticks they do not need. */
    static repeating_timer_t tsr0_timer = { 0 };
    add_repeating_timer_us(-1000000 / 44100, timer_callback_core0, pc, &tsr0_timer);

    /* Native POST ran before core1 could start its audio timer.  Wait until
     * the mixer is actually servicing samples, then emit the queued POST tone. */
    while (!audio_timer_ready)
        tight_loop_contents();
    pc_play_pending_post_beep(pc);

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
#if WELCOME_SCREEN
    // Show welcome screen only after a real power-on reset.
    DBG_PRINT("\nAbout to show welcome screen...\n");
    if (is_power_on_boot())
        show_welcome_screen();
    DBG_PRINT("Welcome screen done.\n");

    DBG_PRINT("\nStarting emulation...\n");
#endif
#ifdef USB_HID_ENABLED
    /* Start the device stack only when we are ready to service it continuously.
     * TinyUSB enumeration requires tud_task() to run promptly after tud_init();
     * starting it before the profiler/welcome screen leaves EP0 requests
     * unattended for seconds and the host can reject enumeration. */
    if (config_get_usb_mode() == USB_MODE_DEVICE) {
        /*
         * USB DEVICE bring-up diagnostic mode: prepare the normal Win+F12
         * Disk Manager before starting TinyUSB. diskui_open() may touch/scan
         * the SD card, so it must not run in the interval after tud_init()
         * and before the first tud_task().
         */
        pc->paused = 1;
        audio_set_enabled(false);
        diskui_open();

        DBG_PRINT("Initializing USB MSC device...\n");
        usbmsc_device_init();
        usbmsc_device_task();
    }
#endif

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

#ifdef USB_HID_ENABLED
            if (config_get_usb_mode() == USB_MODE_DEVICE) {
                /*
                 * With the guest paused there is no pc_step() and therefore no
                 * platform_poll() heartbeat.  Keep TinyUSB responsive while
                 * retaining the ~60 Hz OSD cadence.
                 */
                for (int i = 0; i < 16; ++i) {
                    usbmsc_device_task();
                    sleep_ms(1);
                }
                /*
                 * Host closed the USB link (eject / bus reset / cable pull).
                 * Run the same cleanup as Esc in the Win+F12 Disk Manager,
                 * unconditionally: save config back to HOST mode, shut down
                 * TinyUSB, reboot. (No-op if we somehow are not in DEVICE mode.)
                 */
                if (usbmsc_device_host_disconnected()) {
                    diskui_usb_device_disconnected();
                }
            } else
#endif
            {
                sleep_ms(16);  // ~60Hz polling/animation rate
            }
            continue;
        }

        autotype_tick();

        // Run CPU steps - batch multiple steps for efficiency
        for (int i = 0; i < 10; i++) {
            pc_step(pc, 4096);
#ifdef I386_MODE
            throughput_tick();
#endif
        }
#ifdef DIAG_ENABLED
        diag_heartbeat();   /* core0 alive; the stall alarm checks this */
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
            pc->reset_request = 0;
                watchdog_reboot(0, 0, 0);
            while (true);
            __unreachable();
        }

        // Check for settings UI restart request (requires full RP reset)
        if (settingsui_restart_requested()) {
            settingsui_clear_restart();
            DBG_PRINT("Settings changed - triggering RP reset...\n");
#ifdef I386_PROFILE
            i386_profile_dump_sd_and_reset("watchdog_reboot_settings");
#endif
            // Full hardware reset via watchdog
                watchdog_reboot(0, 0, 0);
        }

        // Check for shutdown
        if (pc->shutdown_state) {
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
#ifdef I386_PROFILE
    i386_profile_dump_sd_and_reset("watchdog_reboot_shutdown");
#endif
    watchdog_reboot(0, 0, 0);
    while (true);
    __unreachable();
}
