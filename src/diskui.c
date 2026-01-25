/**
 * murm386 - i386 PC Emulator for RP2350
 *
 * Disk UI - on-screen disk manager for inserting/ejecting disk images
 * at runtime. Triggered by Win+F12 hotkey.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#include "diskui.h"
#include "vga_osd.h"
#include "disk.h"
#include "config_save.h"
#include "ff.h"
#include <string.h>
#include <strings.h>  // For strcasecmp
#include <stdio.h>

// Menu states
typedef enum {
    MENU_CLOSED,
    MENU_MAIN,          // Drive selection
    MENU_FILE_BROWSER   // File selection for a drive
} MenuState;

// Drive information
static const DriveInfo drive_info[DRIVE_COUNT] = {
    { "A:", "Floppy",    true,  false },
    { "B:", "Floppy",    true,  false },
    { "C:", "Hard Disk", false, false },
    { "D:", "Hard Disk", false, false },
    { "E:", "CD-ROM",    false, true  },
};

// Menu state
static MenuState menu_state = MENU_CLOSED;
static int selected_drive = 0;
static int selected_file = 0;
static int file_scroll_offset = 0;

// File listing (reduced size to save SRAM)
#define MAX_FILES 24
#define MAX_FILENAME_LEN 32
static char file_list[MAX_FILES][MAX_FILENAME_LEN];
static int file_count = 0;
static int plasma_frame = 0;  // Animation frame counter

// UI dimensions
#define MENU_X      10
#define MENU_Y      5
#define MENU_W      60
#define MENU_H      15

#define FILE_X      12
#define FILE_Y      7
#define FILE_W      56
#define FILE_H      11
#define FILE_VISIBLE (FILE_H - 2)

// Forward declarations
static void draw_main_menu(void);
static void draw_file_browser(void);
static void scan_disk_images(void);
static void select_file(void);
static void eject_disk(void);

void diskui_init(void) {
    osd_init();
    menu_state = MENU_CLOSED;
    selected_drive = 0;
    selected_file = 0;
    file_count = 0;
}

void diskui_open(void) {
    if (menu_state != MENU_CLOSED) return;

    menu_state = MENU_MAIN;
    selected_drive = 0;
    osd_clear();
    osd_show();
    draw_main_menu();
}

void diskui_close(void) {
    menu_state = MENU_CLOSED;
    osd_hide();
}

bool diskui_is_open(void) {
    return menu_state != MENU_CLOSED;
}

const DriveInfo* diskui_get_drive_info(DiskUIDrive drive) {
    if (drive < 0 || drive >= DRIVE_COUNT) return NULL;
    return &drive_info[drive];
}

static void draw_main_menu(void) {
    // Draw plasma background (animated) - covers everything outside window
    osd_draw_plasma_background(plasma_frame * 3, MENU_X, MENU_Y, MENU_W, MENU_H);

    // Draw main box with title on border
    osd_draw_box(MENU_X, MENU_Y, MENU_W, MENU_H, OSD_ATTR_BORDER);
    osd_fill(MENU_X + 1, MENU_Y + 1, MENU_W - 2, MENU_H - 2, ' ', OSD_ATTR_NORMAL);
    osd_print_center(MENU_Y, " Disk Manager ", OSD_ATTR(OSD_YELLOW, OSD_BLUE));

    // Draw drive list
    for (int i = 0; i < DRIVE_COUNT; i++) {
        int y = MENU_Y + 2 + i;
        uint8_t attr = (i == selected_drive) ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL;

        // Clear line
        osd_fill(MENU_X + 2, y, MENU_W - 4, 1, ' ', attr);

        // Drive letter and type
        char line[64];
        snprintf(line, sizeof(line), "[%s] %-10s", drive_info[i].label, drive_info[i].type_name);
        osd_print(MENU_X + 2, y, line, attr);

        // Current disk name or [empty]
        const char *filename = disk_get_filename(i);
        if (disk_is_inserted(i) && filename[0]) {
            // Truncate long filenames
            char truncated[24];
            strncpy(truncated, filename, 23);
            truncated[23] = '\0';
            osd_print(MENU_X + 20, y, truncated, attr);
        } else {
            osd_print(MENU_X + 20, y, "[empty]", OSD_ATTR(OSD_LIGHTGRAY, OSD_BLUE));
        }

        // Action hint
        if (disk_is_inserted(i)) {
            osd_print(MENU_X + MENU_W - 12, y, "[Eject]", attr);
        } else {
            osd_print(MENU_X + MENU_W - 12, y, "[Select]", attr);
        }
    }

    // Help text
    int help_y = MENU_Y + MENU_H - 2;
    osd_print(MENU_X + 2, help_y, "\x18/\x19: Navigate   Enter: Select/Eject   Esc: Close", OSD_ATTR_HIGHLIGHT);
}

static void draw_file_browser(void) {
    // Draw plasma background (animated) - covers everything outside window
    osd_draw_plasma_background(plasma_frame * 3, FILE_X, FILE_Y, FILE_W, FILE_H);

    // Draw box with title on border
    osd_draw_box(FILE_X, FILE_Y, FILE_W, FILE_H, OSD_ATTR_BORDER);
    osd_fill(FILE_X + 1, FILE_Y + 1, FILE_W - 2, FILE_H - 2, ' ', OSD_ATTR_NORMAL);

    // Title on border
    char title[48];
    snprintf(title, sizeof(title), " Select Image for %s ", drive_info[selected_drive].label);
    osd_print_center(FILE_Y, title, OSD_ATTR(OSD_YELLOW, OSD_BLUE));

    // Draw file list
    int visible_files = FILE_VISIBLE;
    for (int i = 0; i < visible_files && (file_scroll_offset + i) < file_count; i++) {
        int file_idx = file_scroll_offset + i;
        int y = FILE_Y + 1 + i;
        uint8_t attr = (file_idx == selected_file) ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL;

        // Clear line
        osd_fill(FILE_X + 2, y, FILE_W - 4, 1, ' ', attr);

        // Selection indicator
        if (file_idx == selected_file) {
            osd_print(FILE_X + 2, y, ">", attr);
        }

        // Filename
        osd_print(FILE_X + 4, y, file_list[file_idx], attr);
    }

    // Scroll indicators
    if (file_scroll_offset > 0) {
        osd_putchar(FILE_X + FILE_W - 3, FILE_Y + 1, '\x1e', OSD_ATTR_HIGHLIGHT);  // Up arrow
    }
    if (file_scroll_offset + visible_files < file_count) {
        osd_putchar(FILE_X + FILE_W - 3, FILE_Y + FILE_H - 2, '\x1f', OSD_ATTR_HIGHLIGHT);  // Down arrow
    }

    // No files message
    if (file_count == 0) {
        osd_print_center(FILE_Y + FILE_H / 2, "No disk images found in 386/", OSD_ATTR_DISABLED);
    }

    // Help text
    int help_y = FILE_Y + FILE_H - 2;
    osd_fill(FILE_X + 1, help_y, FILE_W - 2, 1, ' ', OSD_ATTR_NORMAL);
    osd_print(FILE_X + 2, help_y, "\x18/\x19: Navigate   Enter: Select   Esc: Cancel", OSD_ATTR_HIGHLIGHT);
}

static void scan_disk_images(void) {
    DIR dir;
    FILINFO fno;
    FRESULT res;

    file_count = 0;
    memset(file_list, 0, sizeof(file_list));

    res = f_opendir(&dir, "386");
    if (res != FR_OK) {
        return;
    }

    while (file_count < MAX_FILES) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;

        // Skip directories
        if (fno.fattrib & AM_DIR) continue;

        // Check file extension
        char *ext = strrchr(fno.fname, '.');
        if (!ext) continue;

        // Accept .img, .ima, .bin, .iso files
        bool valid = false;
        if (strcasecmp(ext, ".img") == 0) valid = true;
        else if (strcasecmp(ext, ".ima") == 0) valid = true;
        else if (strcasecmp(ext, ".bin") == 0) valid = true;
        else if (strcasecmp(ext, ".iso") == 0) valid = true;

        if (valid) {
            strncpy(file_list[file_count], fno.fname, MAX_FILENAME_LEN - 1);
            file_list[file_count][MAX_FILENAME_LEN - 1] = '\0';
            file_count++;
        }
    }

    f_closedir(&dir);

    // Sort files alphabetically (simple bubble sort)
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = 0; j < file_count - i - 1; j++) {
            if (strcasecmp(file_list[j], file_list[j + 1]) > 0) {
                char temp[MAX_FILENAME_LEN];
                strcpy(temp, file_list[j]);
                strcpy(file_list[j], file_list[j + 1]);
                strcpy(file_list[j + 1], temp);
            }
        }
    }

    selected_file = 0;
    file_scroll_offset = 0;
}

static void select_file(void) {
    if (file_count == 0 || selected_file >= file_count) return;

    const char *filename = file_list[selected_file];

    // Insert the disk
    if (disk_insert(selected_drive, filename)) {
        // Set CD-ROM flag if applicable
        if (drive_info[selected_drive].is_cdrom) {
            disk_set_cdrom(selected_drive, 1);
        }
        // Save disk configuration to INI file
        config_save_disks();
    }

    // Return to main menu
    menu_state = MENU_MAIN;
    draw_main_menu();
}

static void eject_disk(void) {
    disk_eject(selected_drive);
    // Save disk configuration to INI file
    config_save_disks();
    draw_main_menu();
}

bool diskui_handle_key(int keycode, bool is_down) {
    if (!is_down) return true;  // Only handle key presses

    switch (menu_state) {
        case MENU_MAIN:
            switch (keycode) {
                case KEY_UP:
                    if (selected_drive > 0) {
                        selected_drive--;
                    } else {
                        // Wrap to last drive
                        selected_drive = DRIVE_COUNT - 1;
                    }
                    draw_main_menu();
                    break;

                case KEY_DOWN:
                    if (selected_drive < DRIVE_COUNT - 1) {
                        selected_drive++;
                    } else {
                        // Wrap to first drive
                        selected_drive = 0;
                    }
                    draw_main_menu();
                    break;

                case KEY_ENTER:
                    if (disk_is_inserted(selected_drive)) {
                        eject_disk();
                    } else {
                        // Open file browser
                        scan_disk_images();
                        menu_state = MENU_FILE_BROWSER;
                        draw_file_browser();
                    }
                    break;

                case KEY_ESC:
                    diskui_close();
                    break;

                // Quick drive selection
                case KEY_A:
                    selected_drive = DRIVE_FDD_A;
                    draw_main_menu();
                    break;
                case KEY_B:
                    selected_drive = DRIVE_FDD_B;
                    draw_main_menu();
                    break;
                case KEY_C:
                    selected_drive = DRIVE_HDD_C;
                    draw_main_menu();
                    break;
                case KEY_D:
                    selected_drive = DRIVE_HDD_D;
                    draw_main_menu();
                    break;
                case KEY_E:
                    selected_drive = DRIVE_CDROM_E;
                    draw_main_menu();
                    break;
            }
            break;

        case MENU_FILE_BROWSER:
            switch (keycode) {
                case KEY_UP:
                    if (file_count == 0) break;  // No files to navigate
                    if (selected_file > 0) {
                        selected_file--;
                    } else {
                        // Wrap to last file
                        selected_file = file_count - 1;
                        file_scroll_offset = file_count - FILE_VISIBLE;
                        if (file_scroll_offset < 0) file_scroll_offset = 0;
                    }
                    // Scroll up if needed
                    if (selected_file < file_scroll_offset) {
                        file_scroll_offset = selected_file;
                    }
                    draw_file_browser();
                    break;

                case KEY_DOWN:
                    if (file_count == 0) break;  // No files to navigate
                    if (selected_file < file_count - 1) {
                        selected_file++;
                    } else {
                        // Wrap to first file
                        selected_file = 0;
                        file_scroll_offset = 0;
                    }
                    // Scroll down if needed
                    if (selected_file >= file_scroll_offset + FILE_VISIBLE) {
                        file_scroll_offset = selected_file - FILE_VISIBLE + 1;
                    }
                    draw_file_browser();
                    break;

                case KEY_ENTER:
                    select_file();
                    break;

                case KEY_ESC:
                    // Back to main menu
                    menu_state = MENU_MAIN;
                    draw_main_menu();
                    break;
            }
            break;

        default:
            break;
    }

    return true;
}

void diskui_animate(void) {
    if (menu_state == MENU_CLOSED) return;

    plasma_frame++;

    // Update plasma background based on current menu state
    if (menu_state == MENU_MAIN) {
        osd_draw_plasma_background(plasma_frame * 3, MENU_X, MENU_Y, MENU_W, MENU_H);
    } else if (menu_state == MENU_FILE_BROWSER) {
        osd_draw_plasma_background(plasma_frame * 3, FILE_X, FILE_Y, FILE_W, FILE_H);
    }
}
