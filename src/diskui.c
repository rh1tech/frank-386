/**
 * frank-386 - i386 PC Emulator for RP2350
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
#ifdef USB_HID_ENABLED
#include "usbmsc_device.h"
#endif
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
    { "SD-CARD", "Via BIOS  [only]" },  // DRIVE_SD_CARD  (On/Off toggle)
    { "  USB",   " mode"         },  // DRIVE_USB_MODE (HOST/DEVICE toggle)
    { " BIOS",   "System"   },  // DRIVE_BIOS
};

// File listing (reduced size to save SRAM)
#define MAX_FILES        24
#define MAX_FILENAME_LEN 32

// Menu state
static MenuState menu_state   = MENU_CLOSED;
static int selected_row       = 0;  // Current row in main menu (0..DRIVE_TOTAL-1)
static int selected_file      = 0;
static int file_scroll_offset = 0;
// selected_row intentionally persists between open/close — preserves position

// Pending changes: track what the user wants for each drive
// Empty string = eject, non-empty = new filename
static char pending_filename[DRIVE_TOTAL][MAX_FILENAME_LEN];
static bool pending_changed[DRIVE_TOTAL];  // true if user modified this drive
static bool reboot_required;               // true if any ATA drive was changed

// Pending values for the two config toggles (SD-CARD raw / USB mode). They are
// only written to the config on "Save and Reboot" so config_get_usb_mode()
// keeps reflecting the *running* mode until then. Initialised in diskui_open().
static int pending_raw_sd;                 // 0/1
static int pending_usb_mode;               // USB_MODE_HOST / USB_MODE_DEVICE
static char file_list[MAX_FILES][MAX_FILENAME_LEN];
static int  file_count   = 0;
static int  plasma_frame = 0;  // Animation frame counter

// Bottom row index: either "Save and Exit" or "Save and Reboot"
#define MAIN_MENU_ROWS  (DRIVE_TOTAL + 1)
#define ROW_ACTION       DRIVE_TOTAL

// UI dimensions — height adapts to number of rows
#define MENU_X      10
#define MENU_Y      4
#define MENU_W      60
#define MENU_H      (8 + MAIN_MENU_ROWS)  // border(2) + rows + blank + notification + blank + action + border(2)

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
static void eject_pending(void);
static void apply_and_close(void);
static void reset_pending(void);
static int first_attached_drive(void);
static void usb_device_exit_to_host(void);
static void toggle_sd_card(void);
static void toggle_usb_mode(void);
static void esc_apply_temp_and_close(void);
static bool row_is_toggle(int row);

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static bool row_is_toggle(int row) {
    return row == DRIVE_SD_CARD || row == DRIVE_USB_MODE;
}

static const char *get_drive_filename(int drive_idx) {
    if (drive_idx == DRIVE_BIOS) {
        return config_get_bios_file();           // NULL = Native BIOS
    } else if (drive_idx < 2) {
        return fdd_get_filename(drive_idx);      // FDD-0 / FDD-1
    } else if (drive_idx >= DRIVE_ATA0_0 && drive_idx <= DRIVE_ATA1_1) {
        return ata_get_filename(drive_idx - 2);  // ATA0-0 .. ATA1-1
    }
    return NULL;                                 // SD-CARD / USB toggles: no file
}

// Get the display filename for a drive (pending or current)
static const char *get_display_filename(int drive_idx) {
    if (pending_changed[drive_idx]) {
        if (pending_filename[drive_idx][0] == '\0') return NULL;  // pending eject
        return pending_filename[drive_idx];
    }
    return get_drive_filename(drive_idx);
}

static bool file_is_iso(const char *filename) {
    if (!filename) return false;
    char *ext = strrchr(filename, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".iso") == 0) return true;
    return false;
}

// Returns true if the extension is valid for the given drive type.
static bool ext_accepted_for_drive(const char *ext, int drive_idx) {
    if (drive_idx == DRIVE_BIOS) {
        if (strcasecmp(ext, ".bin") == 0) return true;
        if (strcasecmp(ext, ".rom") == 0) return true;
        return false;
    }

    if (strcasecmp(ext, ".iso") == 0) return true;
    if (strcasecmp(ext, ".img") == 0) return true;
    if (strcasecmp(ext, ".ima") == 0) return true;
    if (strcasecmp(ext, ".vhd") == 0) return true;
    if (strcasecmp(ext, ".bin") == 0) return true;
    return false;
}

static void reset_pending(void) {
    for (int i = 0; i < DRIVE_TOTAL; i++) {
        pending_changed[i] = false;
        pending_filename[i][0] = '\0';
    }
    reboot_required = false;
}


/*
 * Return the same first attached image that USB DEVICE mode exports:
 * FDD-0, FDD-1, then ATA0-0 .. ATA1-1.
 *
 * usbmsc_device_init() intentionally runs after diskui_open() during DEVICE
 * startup, so determine the row directly from the already opened disk FILs
 * instead of querying USB MSC state.
 */
static int first_attached_drive(void) {
    for (int i = 0; i < 2; ++i) {
        FIL *fp = fdd_get_file((uint8_t)i);
        if (fp && fp->obj.fs)
            return DRIVE_FDD0 + i;
    }

    for (int i = 0; i < 4; ++i) {
        FIL *fp = ata_get_file((uint8_t)i);
        if (fp && fp->obj.fs)
            return DRIVE_ATA0_0 + i;
    }

    return -1;
}

/*
 * Emergency/convenience exit from USB DEVICE mode.
 *
 * Save HOST to the persistent configuration first.  If saving fails, keep the
 * current DEVICE session alive and leave the menu open instead of rebooting
 * back into DEVICE with the user thinking the change was committed.
 *
 * Once the config is safely on SD, disconnect TinyUSB, sync/close the exported
 * image, then perform the normal full-board reboot.
 */
static void usb_device_exit_to_host(void) {
    if (config_get_usb_mode() != USB_MODE_DEVICE)
        return;

    config_set_usb_mode(USB_MODE_HOST);
    if (!config_save_all()) {
        config_set_usb_mode(USB_MODE_DEVICE);
        draw_main_menu();
        return;
    }

#ifdef USB_HID_ENABLED
    usbmsc_device_shutdown();
#endif

    diskui_close();

    watchdog_reboot(0, 0, 0);
    while (true);
    __unreachable();
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

void diskui_usb_device_disconnected(void) {
    /* The USB host dropped the exported MSC device. Perform the same work as
       Esc in the DEVICE-mode Disk Manager, unconditionally and regardless of
       the current menu state: save config back to HOST mode, shut down
       TinyUSB, reboot. Self-guards on USB_MODE_DEVICE, so it is a no-op in
       other modes. */
    usb_device_exit_to_host();
}

void diskui_init(void) {
    osd_init();
    menu_state    = MENU_CLOSED;
    selected_row  = 0;
    selected_file = 0;
    file_count    = 0;
    reset_pending();
}

void diskui_open(void) {
    if (menu_state != MENU_CLOSED) return;

    reset_pending();

    if (config_get_usb_mode() == USB_MODE_DEVICE) {
        int drive = first_attached_drive();
        if (drive >= 0) {
            selected_row = drive;
        } else {
            /* No image attached: auto-enable the raw SD card so USB DEVICE
               still exports a drive, and move the highlight onto that row.
               Enable it live (disk_set_raw_sd_hdd) because usbmsc_device_init()
               binds the exported medium right after this call. */
            if (!config_get_raw_sd_hdd()) {
                config_set_raw_sd_hdd(1);
                disk_set_raw_sd_hdd(1);
            }
            selected_row = disk_raw_sd_hdd_enabled() ? DRIVE_SD_CARD : DRIVE_FDD0;
        }
    }

    /* Toggle rows start from the current (running) config. */
    pending_raw_sd   = config_get_raw_sd_hdd();
    pending_usb_mode = config_get_usb_mode();

    menu_state = MENU_MAIN;
    osd_clear();
    osd_show();
    draw_main_menu();
}

void diskui_close(void) {
    menu_state = MENU_CLOSED;
    reset_pending();
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

        // Config toggle rows: right-aligned < value >, no file/[Select]/[Eject]
        if (row_is_toggle(i)) {
            const char *val;
            if (i == DRIVE_SD_CARD)
                val = pending_raw_sd ? "< On >" : "< Off >";
            else
                val = (pending_usb_mode == USB_MODE_DEVICE) ? "< DEVICE >" : "< HOST >";
            osd_print(MENU_X + MENU_W - 4 - (int)strlen(val), y, val, attr);
            continue;
        }

        const char *filename = get_display_filename(i);
        if (filename) {
            char truncated[24];
            strncpy(truncated, filename, 23);
            truncated[23] = '\0';
            osd_print(MENU_X + 22, y, truncated, attr);
        } else if (i == DRIVE_BIOS) {
            osd_print(MENU_X + 22, y, "[native]", OSD_ATTR(OSD_LIGHTGRAY, OSD_BLUE));
        } else {
            osd_print(MENU_X + 22, y, "[empty]", OSD_ATTR(OSD_LIGHTGRAY, OSD_BLUE));
        }

        if (i == DRIVE_BIOS) {
            osd_print(MENU_X + MENU_W - 12, y, "[Select]", attr);
        } else if (filename) {
            osd_print(MENU_X + MENU_W - 12, y, "[Eject] ", attr);
        } else {
            osd_print(MENU_X + MENU_W - 12, y, "[Select]", attr);
        }
    }

    // Blank line + reboot notification + blank line
    int notify_y = MENU_Y + 3 + DRIVE_TOTAL;
    if (reboot_required) {
        osd_print_center(notify_y, "! Reboot required for HDD/BIOS changes !", OSD_ATTR(OSD_WHITE, OSD_RED));
    }

    // Action row
    int action_y = MENU_Y + 5 + DRIVE_TOTAL;
    {
        uint8_t attr = (selected_row == ROW_ACTION) ? OSD_ATTR_SELECTED : OSD_ATTR_HIGHLIGHT;
        osd_fill(MENU_X + 2, action_y, MENU_W - 4, 1, ' ', attr);
        if (reboot_required) {
            osd_print_center(action_y, "[ Save and Reboot ]", attr);
        } else {
            osd_print_center(action_y, "[ Save and Exit ]", attr);
        }
    }

    int help_y = MENU_Y + MENU_H - 2;
    if (config_get_usb_mode() == USB_MODE_DEVICE) {
        osd_print_center(help_y,
                         "\x18/\x19: Navigate  Enter: Select/Eject  Esc: USB HOST + Reboot",
                         OSD_ATTR_HIGHLIGHT);
    } else {
        osd_print_center(help_y,
                         "\x18/\x19: Navigate   Enter: Select/Eject   Esc: Cancel",
                         OSD_ATTR_HIGHLIGHT);
    }
}

static void draw_file_browser(void) {
    osd_draw_plasma_background(plasma_frame * 3, FILE_X, FILE_Y, FILE_W, FILE_H);

    osd_draw_box(FILE_X, FILE_Y, FILE_W, FILE_H, OSD_ATTR_BORDER);
    osd_fill(FILE_X + 1, FILE_Y + 1, FILE_W - 2, FILE_H - 2, ' ', OSD_ATTR_NORMAL);

    char title[48];
    snprintf(title, sizeof(title),
             (selected_row == DRIVE_BIOS) ? " Select BIOS " : " Select Image for %s ",
             drive_table[selected_row].label);
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
        osd_print_center(FILE_Y + FILE_H / 2,
                         (selected_row == DRIVE_BIOS) ? "No BIOS files found in " SD_DATA_DIR_SLASH : "No disk images found in " SD_DATA_DIR_SLASH,
                         OSD_ATTR_DISABLED);
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

    if (drive_idx == DRIVE_BIOS) {
        strncpy(file_list[file_count++], "[native]", MAX_FILENAME_LEN - 1);
    }

    res = f_opendir(&dir, SD_DATA_DIR);
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

    // Sort alphabetically, keeping the BIOS Native item first.
    int sort_start = (drive_idx == DRIVE_BIOS) ? 1 : 0;
    for (int i = sort_start; i < file_count - 1; i++) {
        for (int j = sort_start; j < file_count - i + sort_start - 1; j++) {
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

    int drive_idx = selected_row;
    if (drive_idx == DRIVE_BIOS && strcasecmp(file_list[selected_file], "[native]") == 0) {
        pending_filename[drive_idx][0] = '\0';
    } else {
        strncpy(pending_filename[drive_idx], file_list[selected_file], MAX_FILENAME_LEN - 1);
        pending_filename[drive_idx][MAX_FILENAME_LEN - 1] = '\0';
    }
    pending_changed[drive_idx] = true;

    if (drive_idx >= 2) reboot_required = true;

    menu_state = MENU_MAIN;
    draw_main_menu();
}

static void eject_pending(void) {
    int drive_idx = selected_row;
    pending_filename[drive_idx][0] = '\0';
    pending_changed[drive_idx] = true;

    if (drive_idx >= 2) reboot_required = true;

    draw_main_menu();
}

// SD-CARD / USB toggles: only change the pending value here. They need a
// reboot to take effect, so mark reboot_required; the config is written in
// apply_and_close(). This keeps config_get_usb_mode() = the running mode.
static void toggle_sd_card(void) {
    pending_raw_sd = pending_raw_sd ? 0 : 1;
    reboot_required = true;
    draw_main_menu();
}

static void toggle_usb_mode(void) {
    pending_usb_mode = (pending_usb_mode == USB_MODE_DEVICE)
                       ? USB_MODE_HOST : USB_MODE_DEVICE;
    reboot_required = true;
    draw_main_menu();
}

// Apply only pending FLOPPY changes to the live system, without persisting to
// config, then close. This is the Esc action in HOST mode: a quick temporary
// floppy insert/eject. Reboot-requiring pending changes (ATA/BIOS and the SD /
// USB toggles) are discarded.
static void esc_apply_temp_and_close(void) {
    for (int i = DRIVE_FDD0; i <= DRIVE_FDD1; i++) {
        if (!pending_changed[i]) continue;
        if (pending_filename[i][0] == '\0')
            ejectdisk(i, true);
        else
            insertdisk(i, true, false, pending_filename[i]);
    }
    diskui_close();
}

static void apply_and_close(void) {
    // Apply all pending disk changes
    for (int i = 0; i < DRIVE_TOTAL; i++) {
        if (!pending_changed[i]) continue;

        if (pending_filename[i][0] == '\0') {
            // Eject
            if (i == DRIVE_BIOS) {
                config_set_bios_file(NULL);
            } else if (i < 2) {
                ejectdisk(i, true);
            } else if (i >= DRIVE_ATA0_0 && i <= DRIVE_ATA1_1) {
                ejectdisk(i - 2, false);
            }
        } else {
            // Insert
            if (i == DRIVE_BIOS) {
                config_set_bios_file(pending_filename[i]);
            } else if (i < 2) {
                insertdisk(i, true, false, pending_filename[i]);
            } else if (i >= DRIVE_ATA0_0 && i <= DRIVE_ATA1_1) {
                int ata_index = i - 2;
                bool is_cdrom = file_is_iso(pending_filename[i]);
                insertdisk(ata_index, false, is_cdrom, pending_filename[i]);
            }
        }
    }

    // Config toggles (SD raw / USB mode) take effect on the reboot below.
    if (pending_raw_sd != config_get_raw_sd_hdd())
        config_set_raw_sd_hdd(pending_raw_sd);
    if (pending_usb_mode != config_get_usb_mode())
        config_set_usb_mode(pending_usb_mode);

    if (reboot_required)
        config_save_all();     // persist disks + SD/USB/BIOS before reboot
    else
        config_save_disks();

    if (reboot_required) {
        watchdog_reboot(0, 0, 0);
        while (true);
        __unreachable();
    }

    diskui_close();
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
                    if (selected_row == ROW_ACTION) {
                        apply_and_close();
                        break;
                    }
                    if (selected_row == DRIVE_SD_CARD) { toggle_sd_card(); break; }
                    if (selected_row == DRIVE_USB_MODE) { toggle_usb_mode(); break; }
                    const char *filename = get_display_filename(selected_row);
                    if (selected_row == DRIVE_BIOS || !filename) {
                        scan_disk_images(selected_row);
                        menu_state = MENU_FILE_BROWSER;
                        draw_file_browser();
                    } else {
                        eject_pending();
                    }
                    break;
                }

                case KEY_LEFT:
                case KEY_RIGHT:
                    if (selected_row == DRIVE_SD_CARD) toggle_sd_card();
                    else if (selected_row == DRIVE_USB_MODE) toggle_usb_mode();
                    break;

                case KEY_ESC:
                    // In DEVICE mode Esc exits to HOST + reboots. config isn't
                    // changed by the toggles (pending only), so this still
                    // reflects the running mode.
                    if (config_get_usb_mode() == USB_MODE_DEVICE)
                        usb_device_exit_to_host();
                    else
                        esc_apply_temp_and_close();
                    break;

                // Quick selection by drive number
                case KEY_A: selected_row = DRIVE_FDD0;   draw_main_menu(); break;
                case KEY_B: selected_row = DRIVE_FDD1;   draw_main_menu(); break;
                case KEY_C: selected_row = DRIVE_ATA0_0; draw_main_menu(); break;
                case KEY_D: selected_row = DRIVE_ATA0_1; draw_main_menu(); break;
                case KEY_E: selected_row = DRIVE_ATA1_0; draw_main_menu(); break;
                case KEY_F: selected_row = DRIVE_ATA1_1; draw_main_menu(); break;
                case KEY_G: selected_row = DRIVE_BIOS;   draw_main_menu(); break;
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
