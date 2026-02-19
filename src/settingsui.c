/**
 * murm386 - i386 PC Emulator for RP2350
 *
 * Settings UI - on-screen settings manager for changing emulator
 * configuration at runtime. Triggered by Win+F11 hotkey.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#include "settingsui.h"
#include "diskui.h"
#include "config_save.h"
#include "../drivers/vga/vga_osd.h"
#include <string.h>
#include <stdio.h>

// Menu states
typedef enum {
    SETTINGS_CLOSED,
    SETTINGS_MAIN,
    SETTINGS_CONFIRM
} SettingsState;

// Setting items
typedef enum {
    SETTING_MEM = 0,
    SETTING_CPU,
    SETTING_FPU,
    SETTING_FILL_CMOS,
    SETTING_PCSPEAKER,
    SETTING_ADLIB,
    SETTING_SOUNDBLASTER,
    SETTING_TANDY,
    SETTING_MOUSE,
    SETTING_CPU_FREQ,
    SETTING_PSRAM_FREQ,
    SETTING_COUNT
} SettingItem;

// Option values
static const int mem_options[] = { 1, 2, 4 };
static const int mem_option_count = 3;

static const int cpu_options[] = { 3, 4, 5 };
static const int cpu_option_count = 3;

static const int cpu_freq_options[] = { 504, 378 };
static const int cpu_freq_option_count = 2;

static const int psram_freq_options[] = { 166, 133 };
static const int psram_freq_option_count = 2;

// State
static SettingsState settings_state = SETTINGS_CLOSED;
static int selected_item = 0;
static int scroll_offset = 0;
static bool restart_requested = false;
static int plasma_frame = 0;  // Animation frame counter

// Original values (to detect changes)
static int orig_mem, orig_cpu, orig_fpu, orig_fill_cmos;
static int orig_pcspeaker, orig_adlib, orig_soundblaster, orig_tandy, orig_mouse;
static int orig_cpu_freq, orig_psram_freq;

// UI dimensions
#define MENU_X      10
#define MENU_Y      3
#define MENU_W      60
#define MENU_H      18
#define VISIBLE_ITEMS 11

// Forward declarations
static void draw_settings_menu(void);
static void draw_confirm_dialog(void);
static int find_option_index(const int *options, int count, int value);
static void cycle_option(int direction);

void settingsui_init(void) {
    settings_state = SETTINGS_CLOSED;
    restart_requested = false;
}

void settingsui_open(void) {
    if (settings_state != SETTINGS_CLOSED) return;

    // Store original values
    orig_mem = config_get_mem_size_mb();
    orig_cpu = config_get_cpu_gen();
    orig_fpu = config_get_fpu();
    orig_fill_cmos = config_get_fill_cmos();
    orig_pcspeaker = config_get_pcspeaker();
    orig_adlib = config_get_adlib();
    orig_soundblaster = config_get_soundblaster();
    orig_tandy = config_get_tandy();
    orig_mouse = config_get_mouse();
    orig_cpu_freq = config_get_cpu_freq();
    orig_psram_freq = config_get_psram_freq();

    settings_state = SETTINGS_MAIN;
    selected_item = 0;
    scroll_offset = 0;
    osd_clear();
    osd_show();
    draw_settings_menu();
}

void settingsui_close(void) {
    // Restore original values if not confirmed
    if (settings_state == SETTINGS_MAIN && config_has_changes()) {
        config_set_mem_size_mb(orig_mem);
        config_set_cpu_gen(orig_cpu);
        config_set_fpu(orig_fpu);
        config_set_fill_cmos(orig_fill_cmos);
        config_set_pcspeaker(orig_pcspeaker);
        config_set_adlib(orig_adlib);
        config_set_soundblaster(orig_soundblaster);
        config_set_tandy(orig_tandy);
        config_set_mouse(orig_mouse);
        config_set_cpu_freq(orig_cpu_freq);
        config_set_psram_freq(orig_psram_freq);
        config_clear_changes();
    }
    settings_state = SETTINGS_CLOSED;
    osd_hide();
}

bool settingsui_is_open(void) {
    return settings_state != SETTINGS_CLOSED;
}

bool settingsui_restart_requested(void) {
    return restart_requested;
}

void settingsui_clear_restart(void) {
    restart_requested = false;
}

static int find_option_index(const int *options, int count, int value) {
    for (int i = 0; i < count; i++) {
        if (options[i] == value) return i;
    }
    return 0;
}

static void cycle_option(int direction) {
    int idx, count;
    const int *options;

    switch (selected_item) {
        case SETTING_MEM:
            options = mem_options;
            count = mem_option_count;
            idx = find_option_index(options, count, config_get_mem_size_mb());
            idx = (idx + direction + count) % count;
            config_set_mem_size_mb(options[idx]);
            break;

        case SETTING_CPU:
            options = cpu_options;
            count = cpu_option_count;
            idx = find_option_index(options, count, config_get_cpu_gen());
            idx = (idx + direction + count) % count;
            config_set_cpu_gen(options[idx]);
            break;

        case SETTING_FPU:
            config_set_fpu(config_get_fpu() ? 0 : 1);
            break;

        case SETTING_FILL_CMOS:
            config_set_fill_cmos(config_get_fill_cmos() ? 0 : 1);
            break;

        case SETTING_PCSPEAKER:
            config_set_pcspeaker(config_get_pcspeaker() ? 0 : 1);
            break;

        case SETTING_ADLIB:
            config_set_adlib(config_get_adlib() ? 0 : 1);
            break;

        case SETTING_SOUNDBLASTER:
            config_set_soundblaster(config_get_soundblaster() ? 0 : 1);
            break;

        case SETTING_TANDY:
            config_set_tandy(config_get_tandy() ? 0 : 1);
            break;

        case SETTING_MOUSE:
            config_set_mouse(config_get_mouse() ? 0 : 1);
            break;

        case SETTING_CPU_FREQ:
            options = cpu_freq_options;
            count = cpu_freq_option_count;
            idx = find_option_index(options, count, config_get_cpu_freq());
            idx = (idx + direction + count) % count;
            config_set_cpu_freq(options[idx]);
            break;

        case SETTING_PSRAM_FREQ:
            options = psram_freq_options;
            count = psram_freq_option_count;
            idx = find_option_index(options, count, config_get_psram_freq());
            idx = (idx + direction + count) % count;
            config_set_psram_freq(options[idx]);
            break;
    }
}

static void draw_settings_menu(void) {
    // Draw plasma background (animated) - covers everything outside window
    osd_draw_plasma_background(plasma_frame * 3, MENU_X, MENU_Y, MENU_W, MENU_H);

    // Draw box with title on border
    osd_draw_box(MENU_X, MENU_Y, MENU_W, MENU_H, OSD_ATTR_BORDER);
    osd_fill(MENU_X + 1, MENU_Y + 1, MENU_W - 2, MENU_H - 2, ' ', OSD_ATTR_NORMAL);
    osd_print_center(MENU_Y, " Settings ", OSD_ATTR(OSD_YELLOW, OSD_BLUE));

    // Settings items
    const char *labels[] = {
        "RAM Size:",
        "CPU Type:",
        "FPU (387):",
        "Fill CMOS:",
        "PC Speaker:",
        "AdLib:",
        "SoundBlaster:",
        "Tandy Sound:",
        "Mouse:",
        "RP2350 Freq:",
        "PSRAM Freq:"
    };
    char value[24];

    for (int i = 0; i < SETTING_COUNT && i < VISIBLE_ITEMS; i++) {
        int setting_idx = i + scroll_offset;
        if (setting_idx >= SETTING_COUNT) break;

        int y = MENU_Y + 2 + i;
        uint8_t attr = (setting_idx == selected_item) ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL;

        osd_fill(MENU_X + 2, y, MENU_W - 4, 1, ' ', attr);
        osd_print(MENU_X + 3, y, labels[setting_idx], attr);

        // Format value
        switch (setting_idx) {
            case SETTING_MEM:
                snprintf(value, sizeof(value), "< %d MB >", config_get_mem_size_mb());
                break;
            case SETTING_CPU:
                snprintf(value, sizeof(value), "< 80%d86 >", config_get_cpu_gen());
                break;
            case SETTING_FPU:
                snprintf(value, sizeof(value), "< %s >", config_get_fpu() ? "Enabled" : "Disabled");
                break;
            case SETTING_FILL_CMOS:
                snprintf(value, sizeof(value), "< %s >", config_get_fill_cmos() ? "Enabled" : "Disabled");
                break;
            case SETTING_PCSPEAKER:
                snprintf(value, sizeof(value), "< %s >", config_get_pcspeaker() ? "Enabled" : "Disabled");
                break;
            case SETTING_ADLIB:
                snprintf(value, sizeof(value), "< %s >", config_get_adlib() ? "Enabled" : "Disabled");
                break;
            case SETTING_SOUNDBLASTER:
                snprintf(value, sizeof(value), "< %s >", config_get_soundblaster() ? "Enabled" : "Disabled");
                break;
            case SETTING_TANDY:
                snprintf(value, sizeof(value), "< %s >", config_get_tandy() ? "Enabled" : "Disabled");
                break;
            case SETTING_MOUSE:
                snprintf(value, sizeof(value), "< %s >", config_get_mouse() ? "Enabled" : "Disabled");
                break;
            case SETTING_CPU_FREQ:
                snprintf(value, sizeof(value), "< %d MHz >", config_get_cpu_freq());
                break;
            case SETTING_PSRAM_FREQ:
                snprintf(value, sizeof(value), "< %d MHz >", config_get_psram_freq());
                break;
        }
        osd_print(MENU_X + 25, y, value, attr);
    }

    // Show if changes pending
    if (config_has_changes()) {
        osd_print(MENU_X + 3, MENU_Y + MENU_H - 4, "* Changes pending - Enter to apply", OSD_ATTR_HIGHLIGHT);
    }

    // Help
    osd_print(MENU_X + 2, MENU_Y + MENU_H - 2, "\x18\x19:Select  \x1b\x1a:Change  Enter:Apply  Esc:Cancel", OSD_ATTR_HIGHLIGHT);
}

static void draw_confirm_dialog(void) {
    int dx = 20, dy = 10, dw = 40, dh = 5;

    // Draw dialog box with red background (no shadow)
    uint8_t dialog_attr = OSD_ATTR(OSD_WHITE, OSD_RED);
    osd_draw_box(dx, dy, dw, dh, dialog_attr);
    osd_fill(dx + 1, dy + 1, dw - 2, dh - 2, ' ', dialog_attr);
    osd_print(dx + 3, dy + 2, "Save settings and restart? (Y/N)", dialog_attr);
}

bool settingsui_handle_key(int keycode, bool is_down) {
    if (!is_down) return true;

    switch (settings_state) {
        case SETTINGS_MAIN:
            switch (keycode) {
                case KEY_UP:
                    if (selected_item > 0) {
                        selected_item--;
                    } else {
                        // Wrap to last item
                        selected_item = SETTING_COUNT - 1;
                        scroll_offset = SETTING_COUNT - VISIBLE_ITEMS;
                        if (scroll_offset < 0) scroll_offset = 0;
                    }
                    // Adjust scroll if needed
                    if (selected_item < scroll_offset) {
                        scroll_offset = selected_item;
                    }
                    draw_settings_menu();
                    break;

                case KEY_DOWN:
                    if (selected_item < SETTING_COUNT - 1) {
                        selected_item++;
                    } else {
                        // Wrap to first item
                        selected_item = 0;
                        scroll_offset = 0;
                    }
                    // Adjust scroll if needed
                    if (selected_item >= scroll_offset + VISIBLE_ITEMS) {
                        scroll_offset = selected_item - VISIBLE_ITEMS + 1;
                    }
                    draw_settings_menu();
                    break;

                case KEY_LEFT:
                    cycle_option(-1);
                    draw_settings_menu();
                    break;

                case KEY_RIGHT:
                    cycle_option(1);
                    draw_settings_menu();
                    break;

                case KEY_ENTER:
                    if (config_has_changes()) {
                        settings_state = SETTINGS_CONFIRM;
                        draw_confirm_dialog();
                    } else {
                        settingsui_close();
                    }
                    break;

                case KEY_ESC:
                    settingsui_close();
                    break;
            }
            break;

        case SETTINGS_CONFIRM:
            // Y = 21, N = 49
            if (keycode == 21) {  // Y
                config_save_all();
                restart_requested = true;
                settings_state = SETTINGS_CLOSED;
                osd_hide();
            } else if (keycode == 49 || keycode == KEY_ESC) {  // N or Escape
                settings_state = SETTINGS_MAIN;
                draw_settings_menu();
            }
            break;

        default:
            break;
    }

    return true;
}

void settingsui_animate(void) {
    if (settings_state == SETTINGS_CLOSED) return;

    plasma_frame++;

    // Only update plasma background, not the window content
    osd_draw_plasma_background(plasma_frame * 3, MENU_X, MENU_Y, MENU_W, MENU_H);
}
