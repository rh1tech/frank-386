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
#include <hardware/watchdog.h>

// Menu states
typedef enum {
    MENU_CLOSED,
    MENU_MAIN,          // Drive selection
    MENU_FILE_BROWSER   // File selection for a drive
} MenuState;

// Drive table — matches DiskUIDrive enum order from diskui.h
// NOTE: update diskui.h (DriveInfo, DiskUIDrive, DRIVE_TOTAL) to match
static const DriveInfo drive_table[DRIVE_TOTAL] = {
    { "FDD-0",  "Floppy"   },  // DRIVE_FDD0
    { "FDD-1",  "Floppy"   },  // DRIVE_FDD1
    { "ATA0-0", "ATA Disk" },  // DRIVE_ATA0_0
    { "ATA0-1", "ATA Disk" },  // DRIVE_ATA0_1
    { "ATA1-0", "ATA Disk" },  // DRIVE_ATA1_0
    { "ATA1-1", "ATA Disk" },  // DRIVE_ATA1_1
};

// Menu state
static MenuState menu_state   = MENU_CLOSED;
static int selected_row       = 0;  // Current row in main menu (0..DRIVE_TOTAL = "Save and Reset")
static int selected_file      = 0;
static int file_scroll_offset = 0;
// selected_row intentionally persists between open/close — preserves position

// File listing (reduced size to save SRAM)
#define MAX_FILES        24
#define MAX_FILENAME_LEN 32
static char file_list[MAX_FILES][MAX_FILENAME_LEN];
static int  file_count   = 0;
static int  plasma_frame = 0;  // Animation frame counter

// Total rows in main menu: drive rows + "Save and Reset"
#define MAIN_MENU_ROWS  (DRIVE_TOTAL + 1)
#define ROW_SAVE_RESET   DRIVE_TOTAL   // Index of the "Save and Reset" entry

// UI dimensions — height adapts to number of rows
#define MENU_X      10
#define MENU_Y      5
#define MENU_W      60
#define MENU_H      (5 + MAIN_MENU_ROWS)  // top border(2) + rows + bottom border(2)

#define FILE_X      12
#define FILE_Y      7
#define FILE_W      56
#define FILE_H      14
#define FILE_VISIBLE (FILE_H - 4)

// Forward declarations
static void draw_main_menu(void);
static void draw_file_browser(void);
static void scan_disk_images(int drive_idx);
static void select_file(void);
static void eject_disk(void);

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static const char *get_drive_filename(int drive_idx) {
    if (drive_idx < 2) {
        return fdd_get_filename(drive_idx);      // FDD-0 / FDD-1
    } else {
        return ata_get_filename(drive_idx - 2);  // ATA0-0 .. ATA1-1
    }
}

static bool file_is_iso(const char *filename) {
    if (!filename) return false;
    char *ext = strrchr(filename, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".iso") == 0) return true;
    return false;
}

// Returns true if the extension is valid for the given drive type.
// CD-ROM drives only accept .iso; other drives accept .img/.ima/.vhd/.bin.
static bool ext_accepted_for_drive(const char *ext, int drive_idx) {
    if (strcasecmp(ext, ".iso") == 0) return true;
    if (strcasecmp(ext, ".img") == 0) return true;
    if (strcasecmp(ext, ".ima") == 0) return true;
    if (strcasecmp(ext, ".vhd") == 0) return true;
    if (strcasecmp(ext, ".bin") == 0) return true;
    return false;
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

void diskui_init(void) {
    osd_init();
    menu_state    = MENU_CLOSED;
    selected_row  = 0;
    selected_file = 0;
    file_count    = 0;
}

void diskui_open(void) {
    if (menu_state != MENU_CLOSED) return;

    menu_state = MENU_MAIN;
    // selected_row is NOT reset — position is preserved between openings
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
    if (drive < 0 || drive >= DRIVE_TOTAL) return NULL;
    return &drive_table[drive];
}

// --------------------------------------------------------------------------
// Drawing
// --------------------------------------------------------------------------

static void draw_main_menu(void) {
    osd_draw_plasma_background(plasma_frame * 3, MENU_X, MENU_Y, MENU_W, MENU_H);

    osd_draw_box(MENU_X, MENU_Y, MENU_W, MENU_H, OSD_ATTR_BORDER);
    osd_fill(MENU_X + 1, MENU_Y + 1, MENU_W - 2, MENU_H - 2, ' ', OSD_ATTR_NORMAL);
    osd_print_center(MENU_Y, " Disk Manager ", OSD_ATTR(OSD_YELLOW, OSD_BLUE));

    // Drive rows
    for (int i = 0; i < DRIVE_TOTAL; i++) {
        int y = MENU_Y + 2 + i;
        uint8_t attr = (i == selected_row) ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL;

        osd_fill(MENU_X + 2, y, MENU_W - 4, 1, ' ', attr);

        char line[64];
        snprintf(line, sizeof(line), "[%-7s] %-10s", drive_table[i].label, drive_table[i].type_name);
        osd_print(MENU_X + 2, y, line, attr);

        const char *filename = get_drive_filename(i);
        if (filename) {
            char truncated[24];
            strncpy(truncated, filename, 23);
            truncated[23] = '\0';
            osd_print(MENU_X + 22, y, truncated, attr);
        } else {
            osd_print(MENU_X + 22, y, "[empty]", OSD_ATTR(OSD_LIGHTGRAY, OSD_BLUE));
        }

        if (filename) {
            osd_print(MENU_X + MENU_W - 12, y, "[Eject] ", attr);
        } else {
            osd_print(MENU_X + MENU_W - 12, y, "[Select]", attr);
        }
    }

    // "Save and Reset" row
    {
        int y = MENU_Y + 2 + ROW_SAVE_RESET;
        uint8_t attr = (selected_row == ROW_SAVE_RESET) ? OSD_ATTR_SELECTED : OSD_ATTR_HIGHLIGHT;
        osd_fill(MENU_X + 2, y, MENU_W - 4, 1, ' ', attr);
        osd_print_center(y, "[ Save and Reset ]", attr);
    }

    int help_y = MENU_Y + MENU_H - 2;
    osd_print(MENU_X + 2, help_y, "\x18/\x19: Navigate   Enter: Select/Eject   Esc: Close", OSD_ATTR_HIGHLIGHT);
}

static void draw_file_browser(void) {
    osd_draw_plasma_background(plasma_frame * 3, FILE_X, FILE_Y, FILE_W, FILE_H);

    osd_draw_box(FILE_X, FILE_Y, FILE_W, FILE_H, OSD_ATTR_BORDER);
    osd_fill(FILE_X + 1, FILE_Y + 1, FILE_W - 2, FILE_H - 2, ' ', OSD_ATTR_NORMAL);

    char title[48];
    snprintf(title, sizeof(title), " Select Image for %s ", drive_table[selected_row].label);
    osd_print_center(FILE_Y, title, OSD_ATTR(OSD_YELLOW, OSD_BLUE));

    int visible_files = FILE_VISIBLE;
    for (int i = 0; i < visible_files && (file_scroll_offset + i) < file_count; i++) {
        int file_idx = file_scroll_offset + i;
        int y = FILE_Y + 1 + i;
        uint8_t attr = (file_idx == selected_file) ? OSD_ATTR_SELECTED : OSD_ATTR_NORMAL;

        osd_fill(FILE_X + 2, y, FILE_W - 4, 1, ' ', attr);

        if (file_idx == selected_file) {
            osd_print(FILE_X + 2, y, ">", attr);
        }
        osd_print(FILE_X + 4, y, file_list[file_idx], attr);
    }

    if (file_scroll_offset > 0) {
        osd_putchar(FILE_X + FILE_W - 3, FILE_Y + 1, '\x1e', OSD_ATTR_HIGHLIGHT);
    }
    if (file_scroll_offset + visible_files < file_count) {
        osd_putchar(FILE_X + FILE_W - 3, FILE_Y + FILE_H - 2, '\x1f', OSD_ATTR_HIGHLIGHT);
    }

    if (file_count == 0) {
        osd_print_center(FILE_Y + FILE_H / 2, "No disk images found in 386/", OSD_ATTR_DISABLED);
    }

    int help_y = FILE_Y + FILE_H - 2;
    osd_fill(FILE_X + 1, help_y, FILE_W - 2, 1, ' ', OSD_ATTR_NORMAL);
    osd_print(FILE_X + 2, help_y, "\x18/\x19: Navigate   Enter: Select   Esc: Cancel", OSD_ATTR_HIGHLIGHT);
}

// --------------------------------------------------------------------------
// File scanning
// --------------------------------------------------------------------------

static void scan_disk_images(int drive_idx) {
    DIR dir;
    FILINFO fno;
    FRESULT res;

    file_count = 0;
    memset(file_list, 0, sizeof(file_list));

    res = f_opendir(&dir, "386");
    if (res != FR_OK) return;

    while (file_count < MAX_FILES) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;

        if (fno.fattrib & AM_DIR) continue;

        char *ext = strrchr(fno.fname, '.');
        if (!ext) continue;

        if (ext_accepted_for_drive(ext, drive_idx)) {
            strncpy(file_list[file_count], fno.fname, MAX_FILENAME_LEN - 1);
            file_list[file_count][MAX_FILENAME_LEN - 1] = '\0';
            file_count++;
        }
    }

    f_closedir(&dir);

    // Sort alphabetically (bubble sort)
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

// --------------------------------------------------------------------------
// Actions
// --------------------------------------------------------------------------

static void select_file(void) {
    if (file_count == 0 || selected_file >= file_count) return;

    const char *filename = file_list[selected_file];
    int drive_idx = selected_row;

    if (drive_idx < 2) {
        // FDD-0 / FDD-1
        if (insertdisk(drive_idx, true, false, filename)) {
            config_save_disks();
        }
    } else {
        // ATA drives: ata_index is offset by 2
        int ata_index = drive_idx - 2;
        bool is_cdrom = file_is_iso(filename);
        if (insertdisk(ata_index, false, is_cdrom, filename)) {
            config_save_disks();
        }
    }

    menu_state = MENU_MAIN;
    draw_main_menu();
}

static void eject_disk(void) {
    int drive_idx = selected_row;
    if (drive_idx < 2) {
        ejectdisk(drive_idx, true);
    } else {
        ejectdisk(drive_idx - 2, false);
    }
    config_save_disks();
    draw_main_menu();
}

// --------------------------------------------------------------------------
// Input handling
// --------------------------------------------------------------------------

bool diskui_handle_key(int keycode, bool is_down) {
    if (!is_down) return true;

    switch (menu_state) {
        case MENU_MAIN:
            switch (keycode) {
                case KEY_UP:
                    selected_row = (selected_row > 0) ? selected_row - 1 : MAIN_MENU_ROWS - 1;
                    draw_main_menu();
                    break;

                case KEY_DOWN:
                    selected_row = (selected_row < MAIN_MENU_ROWS - 1) ? selected_row + 1 : 0;
                    draw_main_menu();
                    break;

                case KEY_ENTER: {
                    if (selected_row == ROW_SAVE_RESET) {
                        config_save_disks();
                        *(uint32_t*)(0x20000000 + (512ul << 10) - 32) = 0x1927fa52; // magic to fast reboot
                        watchdog_reboot(0, 0, 0);
                        while (true);
                        __unreachable();
                    }
                    const char *filename = get_drive_filename(selected_row);
                    if (filename) {
                        eject_disk();
                    } else {
                        scan_disk_images(selected_row);
                        menu_state = MENU_FILE_BROWSER;
                        draw_file_browser();
                    }
                    break;
                }

                case KEY_ESC:
                    diskui_close();
                    break;

                // Quick selection by drive number
                case KEY_A: selected_row = DRIVE_FDD0;   draw_main_menu(); break;
                case KEY_B: selected_row = DRIVE_FDD1;   draw_main_menu(); break;
                case KEY_C: selected_row = DRIVE_ATA0_0; draw_main_menu(); break;
                case KEY_D: selected_row = DRIVE_ATA0_1; draw_main_menu(); break;
                case KEY_E: selected_row = DRIVE_ATA1_0; draw_main_menu(); break;
                case KEY_F: selected_row = DRIVE_ATA1_1; draw_main_menu(); break;
            }
            break;

        case MENU_FILE_BROWSER:
            switch (keycode) {
                case KEY_UP:
                    if (file_count == 0) break;
                    if (selected_file > 0) {
                        selected_file--;
                    } else {
                        selected_file = file_count - 1;
                        file_scroll_offset = file_count - FILE_VISIBLE;
                        if (file_scroll_offset < 0) file_scroll_offset = 0;
                    }
                    if (selected_file < file_scroll_offset) {
                        file_scroll_offset = selected_file;
                    }
                    draw_file_browser();
                    break;

                case KEY_DOWN:
                    if (file_count == 0) break;
                    if (selected_file < file_count - 1) {
                        selected_file++;
                    } else {
                        selected_file = 0;
                        file_scroll_offset = 0;
                    }
                    if (selected_file >= file_scroll_offset + FILE_VISIBLE) {
                        file_scroll_offset = selected_file - FILE_VISIBLE + 1;
                    }
                    draw_file_browser();
                    break;

                case KEY_ENTER:
                    select_file();
                    break;

                case KEY_ESC:
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

    if (menu_state == MENU_MAIN) {
        osd_draw_plasma_background(plasma_frame * 3, MENU_X, MENU_Y, MENU_W, MENU_H);
    } else if (menu_state == MENU_FILE_BROWSER) {
        osd_draw_plasma_background(plasma_frame * 3, FILE_X, FILE_Y, FILE_W, FILE_H);
    }
}
