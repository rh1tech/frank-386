/**
 * murm386 - i386 PC Emulator for RP2350
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

#include "hardware/structs/qmi.h"

#include "board_config.h"
#include "psram_init.h"
#include "vga_hw.h"
#include "vga.h"
#include "ps2kbd_wrapper.h"
#include "ps2mouse.h"
#ifdef USB_HID_ENABLED
#include "usbkbd_wrapper.h"
#include "usbmouse_wrapper.h"
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

//=============================================================================
// Version Information
//=============================================================================

// Version is defined in CMakeLists.txt from version.txt
#ifndef MURM386_VERSION_MAJOR
#define MURM386_VERSION_MAJOR 1
#endif
#ifndef MURM386_VERSION_MINOR
#define MURM386_VERSION_MINOR 0
#endif

//=============================================================================
// Global State
//=============================================================================

static PC *pc = NULL;
static PCConfig config;
static bool initialized = false;

// Framebuffer for VGA output (in PSRAM)
static uint8_t *framebuffer = NULL;

// FatFS state
static FATFS fatfs;

// Flag to track if VGA is initialized (for error display)
static bool vga_initialized = false;

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
uint32_t get_uticks(void) {
    return time_us_32();
}

/**
 * Allocate memory (uses PSRAM for large allocations).
 */
void *pcmalloc(long size) {
    // For small allocations, use regular malloc
    if (size <= 2 * 4096) {
        return malloc(size);
    }
    // For large allocations, use PSRAM
    return bigmalloc(size);
}

/**
 * Allocate large memory block from PSRAM.
 */
static uint8_t *psram_alloc_ptr = NULL;

void *bigmalloc(size_t size) {
    if (!psram_alloc_ptr) {
        psram_alloc_ptr = (uint8_t *)PSRAM_BASE_ADDR;
    }

    // Align to 4 bytes
    size = (size + 3) & ~3;

    void *ptr = psram_alloc_ptr;
    psram_alloc_ptr += size;

    // Check bounds
    if ((uintptr_t)psram_alloc_ptr > (PSRAM_BASE_ADDR + PSRAM_SIZE_BYTES)) {
        printf("ERROR: PSRAM allocation overflow!\n");
        return NULL;
    }

    return ptr;
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

//=============================================================================
// Keyboard Polling
//=============================================================================

// Track modifier key state for Win+F12 hotkey
static bool win_key_pressed = false;

// Process a single keycode, handling disk UI and settings UI hotkeys
// Returns true if key should be passed to emulator, false if consumed
static bool process_keycode(int is_down, int keycode) {
    // Track Win key state
    if (keycode == KEY_LEFTMETA) {
        win_key_pressed = is_down;
    }

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
    // Poll PS/2 keyboard
    ps2kbd_tick();

    int is_down, keycode;
    while (ps2kbd_get_key(&is_down, &keycode)) {
        if (process_keycode(is_down, keycode)) {
            if (pc && pc->kbd) {
                ps2_put_keycode(pc->kbd, is_down, keycode);
            }
        }
    }

    // Poll PS/2 mouse (only if enabled and not paused)
    if (pc && pc->mouse_enabled && !pc->paused) {
        int16_t dx, dy;
        int8_t dz;
        uint8_t buttons;
        if (ps2mouse_get_state(&dx, &dy, &dz, &buttons)) {
            if (pc->mouse) {
                ps2_mouse_event(pc->mouse, dx, dy, dz, buttons);
            }
        }
    }

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

    // Poll USB mouse (only if enabled and not paused)
    if (pc && pc->mouse_enabled && !pc->paused) {
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
    config.mem_size = EMU_MEM_SIZE_MB * 1024 * 1024;
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
        config.disks[i] = NULL;
        config.iscd[i] = 0;
    }
    config.fdd[0] = NULL;
    config.fdd[1] = NULL;

    config.fill_cmos = 1;
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

    // Also parse murm386-specific settings
    ini_parse_string(content, parse_murm386_ini, NULL);

    free(content);
    return 0;
}

//=============================================================================
// Clock Configuration
//=============================================================================

// Flash timing configuration for overclocking
#define FLASH_MAX_FREQ_MHZ 66

static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = FLASH_MAX_FREQ_MHZ * 1000000;

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
    sleep_ms(100);  // Stabilization delay

    // Configure flash timing BEFORE changing clock
    set_flash_timings(CPU_CLOCK_MHZ);
#endif

    // Set system clock
    set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false);

    DBG_PRINT("System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
}

/**
 * Get voltage for CPU frequency
 */
static enum vreg_voltage get_voltage_for_freq(int mhz) {
    if (mhz >= 504) return VREG_VOLTAGE_1_65;
    if (mhz >= 378) return VREG_VOLTAGE_1_60;
    return VREG_VOLTAGE_1_50;
}

/**
 * Reconfigure clocks at runtime based on INI settings.
 * This function MUST run from RAM, not flash.
 */
static void __no_inline_not_in_flash_func(reconfigure_clocks)(int cpu_mhz, int psram_mhz, uint psram_pin) {
    int current_mhz = clock_get_hz(clk_sys) / 1000000;
    bool lowering = (cpu_mhz < current_mhz);

    DBG_PRINT("Reconfiguring clocks: %d MHz -> %d MHz, PSRAM: %d MHz\n",
              current_mhz, cpu_mhz, psram_mhz);

    enum vreg_voltage new_voltage = get_voltage_for_freq(cpu_mhz);

    if (lowering) {
        // LOWERING: clock first, then voltage (safe order)
        set_flash_timings(cpu_mhz);
        set_sys_clock_khz(cpu_mhz * 1000, false);
        sleep_ms(10);
        vreg_set_voltage(new_voltage);
    } else {
        // RAISING: voltage first, then clock (safe order)
        vreg_disable_voltage_limit();
        vreg_set_voltage(new_voltage);
        sleep_ms(50);  // Stabilization delay
        set_flash_timings(cpu_mhz);
        set_sys_clock_khz(cpu_mhz * 1000, false);
    }

    // Re-initialize PSRAM with the new frequency
    psram_init_with_freq(psram_pin, psram_mhz);

    DBG_PRINT("Clock reconfiguration complete: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
}

//=============================================================================
// Hardware Initialization
//=============================================================================

static bool init_hardware(void) {
    // Configure clocks (including overclock if enabled)
    configure_clocks();

    // Initialize PSRAM first
    DBG_PRINT("Initializing PSRAM...\n");
    uint psram_pin = get_psram_pin();
    DBG_PRINT("  PSRAM CS pin: GPIO%d\n", psram_pin);
    psram_init(psram_pin);

    if (!psram_test()) {
        printf("ERROR: PSRAM test failed!\n");
        // Can't show visual error - VGA not ready yet
        return false;
    }
    DBG_PRINT("  PSRAM test passed (8MB)\n");

    // Initialize VGA early so we can show errors on screen
    DBG_PRINT("Initializing VGA...\n");
    DBG_PRINT("  Base pin: GPIO%d\n", VGA_BASE_PIN);
    vga_hw_init();
    vga_initialized = true;

    // Initialize SD card
    DBG_PRINT("Initializing SD card...\n");
    FRESULT res = f_mount(&fatfs, "", 1);
    if (res != FR_OK) {
        char detail[32];
        snprintf(detail, sizeof(detail), "FatFS error code: %d", res);
        show_error_screen(" SD Card Error ", "Failed to mount SD card.", detail);
        // show_error_screen never returns
    }
    DBG_PRINT("  SD card mounted\n");

    // Check if 386/ directory exists
    DIR dir;
    res = f_opendir(&dir, "386");
    if (res != FR_OK) {
        show_error_screen(" Missing Directory ", "Directory '386/' not found on SD card.", "Create it and add config.ini, bios.bin");
        // show_error_screen never returns
    }
    f_closedir(&dir);
    DBG_PRINT("  386/ directory found\n");

    // Load murm386-specific hardware settings from INI
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
                    // Parse just the [murm386] section
                    ini_parse_string(content, parse_murm386_ini, NULL);
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
        if (cfg_cpu != CPU_CLOCK_MHZ || cfg_psram != PSRAM_MAX_FREQ_MHZ) {
            reconfigure_clocks(cfg_cpu, cfg_psram, psram_pin);
        }
    }

    // Initialize PS/2 keyboard
    DBG_PRINT("Initializing PS/2 keyboard...\n");
    DBG_PRINT("  CLK: GPIO%d, DATA: GPIO%d\n", PS2_PIN_CLK, PS2_PIN_DATA);
    ps2kbd_init(PS2_PIN_CLK);

    // Initialize PS/2 mouse
    DBG_PRINT("Initializing PS/2 mouse...\n");
    DBG_PRINT("  CLK: GPIO%d, DATA: GPIO%d\n", PS2_MOUSE_CLK, PS2_MOUSE_DATA);
    ps2mouse_init();

    // Initialize USB HID keyboard (if enabled)
#ifdef USB_HID_ENABLED
    DBG_PRINT("Initializing USB HID keyboard...\n");
    usbkbd_init();
#endif

    // Initialize I2S Audio
    DBG_PRINT("Initializing I2S Audio...\n");
    DBG_PRINT("  DATA: GPIO%d, CLK: GPIO%d, LRCK: GPIO%d\n",
           I2S_DATA_PIN, I2S_CLOCK_PIN_BASE, I2S_CLOCK_PIN_BASE + 1);
    if (!audio_init()) {
        printf("WARNING: Audio initialization failed\n");
    }

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

    DBG_PRINT("\nEmulator configuration:\n");
    DBG_PRINT("  Memory: %ld MB\n", config.mem_size / (1024 * 1024));
    DBG_PRINT("  VGA Memory: %ld KB\n", config.vga_mem_size / 1024);
    DBG_PRINT("  CPU: %d86\n", config.cpu_gen);
    DBG_PRINT("  BIOS: %s\n", config.bios ? config.bios : "(none)");
    DBG_PRINT("  VGA BIOS: %s\n", config.vga_bios ? config.vga_bios : "(none)");
    DBG_PRINT("  Floppy A: %s\n", config.fdd[0] ? config.fdd[0] : "(none)");
    DBG_PRINT("  Floppy B: %s\n", config.fdd[1] ? config.fdd[1] : "(none)");

    // Calculate total PSRAM needed
    size_t total_psram = config.mem_size + config.vga_mem_size;
    DBG_PRINT("  PSRAM needed: %lu KB (available: %lu KB)\n",
           (unsigned long)(total_psram / 1024),
           (unsigned long)(PSRAM_SIZE_BYTES / 1024));

    if (total_psram > PSRAM_SIZE_BYTES) {
        printf("WARNING: Reducing memory to fit in PSRAM\n");
        config.mem_size = PSRAM_SIZE_BYTES - config.vga_mem_size - (64 * 1024);  // Leave 64KB margin
        DBG_PRINT("  Adjusted memory: %ld MB\n", config.mem_size / (1024 * 1024));
    }

    // Create PC instance
    DBG_PRINT("\nCreating PC instance...\n");
    pc = pc_new(vga_redraw, platform_poll, NULL, NULL, &config);
    if (!pc) {
        printf("ERROR: Failed to create PC instance\n");
        return false;
    }

    // Ensure emulator starts unpaused
    pc->paused = 0;

    // Initialize disk UI
    DBG_PRINT("Initializing Disk UI...\n");
    diskui_init();

    // Initialize settings UI
    DBG_PRINT("Initializing Settings UI...\n");
    settingsui_init();

    // Initialize config save module with current values from PCConfig
    // (these override INI values if not present in [murm386] section)
    config_set_mem_size_mb(config.mem_size / (1024 * 1024));
    config_set_cpu_gen(config.cpu_gen);
    config_set_fpu(config.fpu);
    config_set_fill_cmos(config.fill_cmos);
    // Hardware settings are loaded from [murm386] section via parse_murm386_ini
    config_clear_changes();

    // Apply audio/mouse enable settings from config to PC instance
    // This allows disabling devices for performance improvement
    pc->pcspk_enabled = config_get_pcspeaker();
    pc->adlib_enabled = config_get_adlib();
    pc->sb16_enabled = config_get_soundblaster();
    pc->mouse_enabled = config_get_mouse();
    DBG_PRINT("  Audio: PC Speaker=%d, Adlib=%d, SB16=%d, Mouse=%d\n",
              pc->pcspk_enabled, pc->adlib_enabled, pc->sb16_enabled, pc->mouse_enabled);

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

//=============================================================================
// Core 1 Entry Point (VGA rendering + Audio processing)
//=============================================================================

static void core1_entry(void) {
    DBG_PRINT("[Core 1] VGA rendering + Audio started\n");

    while (true) {
        // VGA driver handles rendering in DMA IRQ
        // Core 1 can do VGA updates from PSRAM
        if (initialized && pc) {
            vga_hw_update();

            // Process audio whenever the driver needs samples (DMA buffer free)
            // This decouples audio generation from CPU time and locks it to
            // the actual playback rate (preventing underruns/clicks).
            if (audio_needs_samples()) {
                // Mix SB16, Adlib, PC Speaker and output to I2S
                audio_process_frame(pc);
            }
        }
        tight_loop_contents();
    }
}

//=============================================================================
// Welcome Screen
//=============================================================================

static void show_welcome_screen(void) {
    // Welcome screen dimensions
    int wx = 15, wy = 7, ww = 50, wh = 11;

    osd_clear();

    // Draw the window content once (static, won't flicker)
    osd_draw_box(wx, wy, ww, wh, OSD_ATTR_BORDER);
    osd_fill(wx + 1, wy + 1, ww - 2, wh - 2, ' ', OSD_ATTR_NORMAL);

    // Title
    osd_print_center(wy + 2, "Murm386", OSD_ATTR(OSD_YELLOW, OSD_BLUE));

    // Version
    char version_str[32];
    snprintf(version_str, sizeof(version_str), "Version %d.%02d",
             MURM386_VERSION_MAJOR, MURM386_VERSION_MINOR);
    osd_print_center(wy + 4, version_str, OSD_ATTR_NORMAL);

    // Author
    osd_print_center(wy + 5, "Port by Mikhail Matveev, rh1.tech", OSD_ATTR_NORMAL);

    // Hardware info
    char hw_str[48];
    snprintf(hw_str, sizeof(hw_str), "RP2350 @ %d MHz / PSRAM @ %d MHz",
             config_get_cpu_freq(), config_get_psram_freq());
    osd_print_center(wy + 7, hw_str, OSD_ATTR(OSD_LIGHTCYAN, OSD_BLUE));

    // Platform (green text)
#ifdef BOARD_M1
    osd_print_center(wy + 8, "Platform: M1", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#elif defined(BOARD_M2)
    osd_print_center(wy + 8, "Platform: M2", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#else
    osd_print_center(wy + 8, "Platform: Unknown", OSD_ATTR(OSD_LIGHTGREEN, OSD_BLUE));
#endif

    osd_show();

    // Animate plasma background for 7 seconds (700 frames at ~10ms each)
    // Window area is skipped by osd_draw_plasma_background, so it won't flicker
    for (int frame = 0; frame < 700; frame++) {
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

    DBG_PRINT("\n\n");
    DBG_PRINT("============================================\n");
    DBG_PRINT("  murm386 - 386 Emulator for RP2350\n");
    DBG_PRINT("  Version %d.%02d\n", MURM386_VERSION_MAJOR, MURM386_VERSION_MINOR);
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

    // Start Core 1 for VGA rendering
    multicore_launch_core1(core1_entry);

    initialized = true;

    // Show welcome screen
    show_welcome_screen();

    DBG_PRINT("\nStarting emulation...\n");

    // VGA update timing
    uint64_t last_vga_update = 0;
    const uint64_t vga_interval_us = 16000; // ~60Hz

    // Frame rate throttling for audio sync
    // Target ~60fps to match audio processing rate (16666us per frame)
    uint64_t frame_start_time = time_us_64();
    const uint64_t target_frame_time_us = 16666; // 60Hz = 16.666ms per frame
    int frame_step_count = 0;
    const int steps_per_frame = 100; // Number of outer loop iterations per frame

    // Retrace-based frame submission state
    static bool was_in_retrace = false;
    static uint16_t latched_start_addr = 0;
    static uint8_t latched_panning = 0;
    static int latched_line_compare = -1;
    static int last_vga_mode = -1;

    // Frame skipping - skip every other frame for better performance
    int frame_skip_counter = 0;
    const int frame_skip_pattern = 2; // Render 1 frame, skip 1 (30fps display)

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

            sleep_ms(16);  // ~60Hz polling/animation rate
            continue;
        }

        // Run CPU steps - batch multiple steps for efficiency
        for (int i = 0; i < 10; i++) {
            pc_step(pc);

            // Check retrace and submit frame (fast path)
            // We must check this frequently to catch the VBLANK edge
            bool in_retrace = vga_in_retrace(pc->vga);

            // Latch values at the END of retrace (falling edge of VBLANK)
            if (was_in_retrace && !in_retrace) {
                uint16_t start_addr = vga_get_start_addr(pc->vga);
                uint8_t panning = vga_get_panning(pc->vga);
                int line_compare = vga_get_line_compare(pc->vga);

                // Text geometry:
                // - visible cols from CRTC 0x01 (40/80)
                // - stride in cells from CRTC 0x13 (offset) * 2
                int cols = vga_get_text_cols(pc->vga);
                vga_hw_set_text_cols(cols);
                int cr13 = vga_get_line_offset(pc->vga);   // CRTC offset register
                int stride_cells = cr13 * 2;
                vga_hw_set_text_stride(stride_cells);

                // Glitch Filter logic - avoid mid-update artifacts during smooth scrolling
                bool is_glitch = false;
                if (start_addr == latched_start_addr + 1 && panning >= latched_panning) is_glitch = true;
                else if (start_addr == latched_start_addr - 1 && panning <= latched_panning) is_glitch = true;
                else if (latched_panning >= 6 && panning <= 1 && start_addr == latched_start_addr) is_glitch = true;
                else if (latched_panning <= 1 && panning >= 6 && start_addr == latched_start_addr) is_glitch = true;

                // Persistence check: If a glitch persists for more than 2 frames, assume it's real
                static int glitch_counter = 0;
                if (is_glitch) {
                    glitch_counter++;
                    if (glitch_counter > 2) {
                        is_glitch = false;
                        glitch_counter = 0;
                    }
                } else {
                    glitch_counter = 0;
                }

                if (!is_glitch) {
                    latched_start_addr = start_addr;
                    latched_panning = panning;
                    latched_line_compare = line_compare;

                    // Frame skipping: only submit frame if not skipped
                    frame_skip_counter++;
                    if (frame_skip_counter < frame_skip_pattern) {
                        vga_hw_submit_frame(latched_start_addr, latched_panning, latched_line_compare);
                    } else {
                        frame_skip_counter = 0; // Reset counter, skip this frame
                    }
                }
            }

            was_in_retrace = in_retrace;
        }

        // Poll keyboard less frequently (every 20 iterations ~5ms)
        // Keyboard events are buffered, so missing a few cycles is fine
        static int poll_counter = 0;
        if (++poll_counter >= 20) {
            poll_counter = 0;
            poll_keyboard();
        }

        // Update heavy VGA state periodically (~60Hz)
        // Note: Audio processing is handled by Core 1 for better performance
        uint64_t now = time_us_64();
        if (now - last_vga_update >= vga_interval_us) {
            last_vga_update = now;

            // Update cursor
            int cx, cy, cs, ce, cv;
            vga_get_cursor_info(pc->vga, &cx, &cy, &cs, &ce, &cv);
            int char_height = vga_get_char_height(pc->vga);
            if (cv) {
                vga_hw_set_cursor(cx, cy, cs, ce, char_height);
                // Sync cursor blink phase with emulator
                vga_hw_set_cursor_blink(vga_get_cursor_blink_phase(pc->vga));
            } else {
                vga_hw_set_cursor(-1, -1, 0, 0, 16);  // Hide cursor
            }

            // Update VGA mode
            int vga_mode = vga_get_mode(pc->vga);
            if (vga_mode != last_vga_mode) {
                printf("[VGA_HW] Mode change: %d -> %d\n", last_vga_mode, vga_mode);
                vga_hw_set_mode(vga_mode);
                last_vga_mode = vga_mode;
            }

            // Update palette and graphics submode for graphics modes
            if (vga_mode == 2) {
                // Only update palette when it actually changed
                if (vga_is_palette_dirty(pc->vga)) {
                    vga_hw_set_palette(vga_get_palette(pc->vga));
                }

                int gfx_w, gfx_h;
                int gfx_submode = vga_get_graphics_mode(pc->vga, &gfx_w, &gfx_h);
                int line_offset = vga_get_line_offset(pc->vga);
                static int last_submode = -1;
                if (gfx_submode != last_submode) {
                    printf("[VGA_HW] Graphics submode=%d %dx%d offset=%d\n",
                           gfx_submode, gfx_w, gfx_h, line_offset);
                    last_submode = gfx_submode;
                }
                vga_hw_set_gfx_mode(gfx_submode, gfx_w, gfx_h, line_offset);

                // For EGA mode, also update the 16-color palette
                if (gfx_submode == 2) {
                    uint8_t ega_pal[48];
                    vga_get_palette16(pc->vga, ega_pal);
                    vga_hw_set_palette16(ega_pal);
                }
            }

            // For text mode, submit frame with current offset
            if (vga_mode == 1) {
                vga_hw_set_vram_offset(vga_get_start_addr(pc->vga));
            }
        }

        // Check for reset request
        if (pc->reset_request) {
            pc->reset_request = 0;
            load_bios_and_reset(pc);
        }

        // Check for settings UI restart request (requires full RP reset)
        if (settingsui_restart_requested()) {
            settingsui_clear_restart();
            DBG_PRINT("Settings changed - triggering RP reset...\n");
            // Full hardware reset via watchdog
            watchdog_reboot(0, 0, 0);
        }

        // Check for shutdown
        if (pc->shutdown_state) {
            break;
        }

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
    }

    DBG_PRINT("\nEmulation stopped.\n");

    while (true) {
        sleep_ms(1000);
    }

    return 0;
}
