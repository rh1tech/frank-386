/**
 * murm386 - i386 PC Emulator for RP2350
 *
 * Disk UI - on-screen disk manager for inserting/ejecting disk images
 * at runtime. Triggered by Win+F12 hotkey.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: MIT
 */

#ifndef DISKUI_H
#define DISKUI_H

#include <stdint.h>
#include <stdbool.h>

// Drive types
typedef enum {
    DRIVE_FDD_A = 0,    // Floppy A:
    DRIVE_FDD_B = 1,    // Floppy B:
    DRIVE_HDD_C = 2,    // Hard Disk C:
    DRIVE_HDD_D = 3,    // Hard Disk D:
    DRIVE_CDROM_E = 4,  // CD-ROM E:
    DRIVE_COUNT = 5
} DiskUIDrive;

// Drive info for UI display
typedef struct {
    const char *label;       // "A:", "B:", etc.
    const char *type_name;   // "Floppy", "Hard Disk", "CD-ROM"
    bool is_floppy;
    bool is_cdrom;
} DriveInfo;

// Initialize disk UI system
void diskui_init(void);

// Open disk menu (shows OSD, returns immediately)
void diskui_open(void);

// Close disk menu (hides OSD)
void diskui_close(void);

// Check if disk menu is currently open
bool diskui_is_open(void);

// Handle keyboard input
// keycode: Linux keycode
// is_down: true for key press, false for release
// Returns: true if key was consumed by disk UI
bool diskui_handle_key(int keycode, bool is_down);

// Get info about a drive
const DriveInfo* diskui_get_drive_info(DiskUIDrive drive);

// Animate plasma background (call from main loop when menu is open)
void diskui_animate(void);

// Linux keycodes used by disk UI
#define KEY_UP      103
#define KEY_DOWN    108
#define KEY_LEFT    105
#define KEY_RIGHT   106
#define KEY_ENTER   28
#define KEY_ESC     1
#define KEY_F12     88
#define KEY_LEFTMETA 125  // Left Windows/GUI key
#define KEY_A       30
#define KEY_B       48
#define KEY_C       46
#define KEY_D       32
#define KEY_E       18

#endif // DISKUI_H
