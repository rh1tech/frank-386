/*
 * USB HID Host Application Callbacks for murm386
 * Implements TinyUSB Host callbacks for keyboard
 *
 * Based on TinyUSB HID host example
 * SPDX-License-Identifier: MIT
 */

#include "tusb.h"
#include "usbhid.h"
#include <stdio.h>
#include <string.h>

// Only compile if USB Host is enabled
#if CFG_TUH_ENABLED

//--------------------------------------------------------------------
// Internal state
//--------------------------------------------------------------------

#define MAX_REPORT 4

// Per-device HID info for generic report parsing
static struct {
    uint8_t report_count;
    tuh_hid_report_info_t report_info[MAX_REPORT];
} hid_info[CFG_TUH_HID];

// Previous keyboard report for detecting key changes
static hid_keyboard_report_t prev_kbd_report = { 0, 0, {0} };

// Device connection state
static volatile int keyboard_connected = 0;

// Key action queue (for detecting press/release)
#define KEY_ACTION_QUEUE_SIZE 32
typedef struct {
    uint8_t keycode;
    int down;
} key_action_t;

static key_action_t key_action_queue[KEY_ACTION_QUEUE_SIZE];
static volatile int key_action_head = 0;
static volatile int key_action_tail = 0;

//--------------------------------------------------------------------
// Internal functions
//--------------------------------------------------------------------

static void queue_key_action(uint8_t keycode, int down) {
    int next_head = (key_action_head + 1) % KEY_ACTION_QUEUE_SIZE;
    if (next_head != key_action_tail) {
        key_action_queue[key_action_head].keycode = keycode;
        key_action_queue[key_action_head].down = down;
        key_action_head = next_head;
    }
}

static int find_keycode_in_report(hid_keyboard_report_t const *report, uint8_t keycode) {
    for (int i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) return 1;
    }
    return 0;
}

//--------------------------------------------------------------------
// Process keyboard report
//--------------------------------------------------------------------

static void process_kbd_report(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report) {
    // Handle modifier changes - queue as pseudo-keycodes 0xE0-0xE7
    uint8_t released_mods = prev_report->modifier & ~(report->modifier);
    uint8_t pressed_mods = report->modifier & ~(prev_report->modifier);

    // Left Control (0xE0)
    if (released_mods & KEYBOARD_MODIFIER_LEFTCTRL) {
        queue_key_action(0xE0, 0);
    }
    if (pressed_mods & KEYBOARD_MODIFIER_LEFTCTRL) {
        queue_key_action(0xE0, 1);
    }

    // Left Shift (0xE1)
    if (released_mods & KEYBOARD_MODIFIER_LEFTSHIFT) {
        queue_key_action(0xE1, 0);
    }
    if (pressed_mods & KEYBOARD_MODIFIER_LEFTSHIFT) {
        queue_key_action(0xE1, 1);
    }

    // Left Alt (0xE2)
    if (released_mods & KEYBOARD_MODIFIER_LEFTALT) {
        queue_key_action(0xE2, 0);
    }
    if (pressed_mods & KEYBOARD_MODIFIER_LEFTALT) {
        queue_key_action(0xE2, 1);
    }

    // Left GUI (0xE3)
    if (released_mods & KEYBOARD_MODIFIER_LEFTGUI) {
        queue_key_action(0xE3, 0);
    }
    if (pressed_mods & KEYBOARD_MODIFIER_LEFTGUI) {
        queue_key_action(0xE3, 1);
    }

    // Right Control (0xE4)
    if (released_mods & KEYBOARD_MODIFIER_RIGHTCTRL) {
        queue_key_action(0xE4, 0);
    }
    if (pressed_mods & KEYBOARD_MODIFIER_RIGHTCTRL) {
        queue_key_action(0xE4, 1);
    }

    // Right Shift (0xE5)
    if (released_mods & KEYBOARD_MODIFIER_RIGHTSHIFT) {
        queue_key_action(0xE5, 0);
    }
    if (pressed_mods & KEYBOARD_MODIFIER_RIGHTSHIFT) {
        queue_key_action(0xE5, 1);
    }

    // Right Alt (0xE6)
    if (released_mods & KEYBOARD_MODIFIER_RIGHTALT) {
        queue_key_action(0xE6, 0);
    }
    if (pressed_mods & KEYBOARD_MODIFIER_RIGHTALT) {
        queue_key_action(0xE6, 1);
    }

    // Right GUI (0xE7)
    if (released_mods & KEYBOARD_MODIFIER_RIGHTGUI) {
        queue_key_action(0xE7, 0);
    }
    if (pressed_mods & KEYBOARD_MODIFIER_RIGHTGUI) {
        queue_key_action(0xE7, 1);
    }

    // Check for released keys
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = prev_report->keycode[i];
        if (keycode && !find_keycode_in_report(report, keycode)) {
            queue_key_action(keycode, 0); // Key released
        }
    }

    // Check for pressed keys
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode && !find_keycode_in_report(prev_report, keycode)) {
            queue_key_action(keycode, 1); // Key pressed
        }
    }
}

//--------------------------------------------------------------------
// Process generic HID report (for non-boot protocol keyboards)
//--------------------------------------------------------------------

static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)dev_addr;

    uint8_t const rpt_count = hid_info[instance].report_count;
    tuh_hid_report_info_t *rpt_info_arr = hid_info[instance].report_info;
    tuh_hid_report_info_t *rpt_info = NULL;

    if (rpt_count == 1 && rpt_info_arr[0].report_id == 0) {
        // Simple report without report ID
        rpt_info = &rpt_info_arr[0];
    } else {
        // Composite report, first byte is report ID
        uint8_t const rpt_id = report[0];
        for (uint8_t i = 0; i < rpt_count; i++) {
            if (rpt_id == rpt_info_arr[i].report_id) {
                rpt_info = &rpt_info_arr[i];
                break;
            }
        }
        report++;
        len--;
    }

    if (!rpt_info) {
        return;
    }

    if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP) {
        if (rpt_info->usage == HID_USAGE_DESKTOP_KEYBOARD) {
            process_kbd_report((hid_keyboard_report_t const *)report, &prev_kbd_report);
            prev_kbd_report = *(hid_keyboard_report_t const *)report;
        }
    }
}

//--------------------------------------------------------------------
// TinyUSB Callbacks
//--------------------------------------------------------------------

// Invoked when HID device is mounted
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    printf("USB HID device mounted: dev=%d inst=%d protocol=%d\n", dev_addr, instance, itf_protocol);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 1;
        printf("  USB Keyboard connected!\n");
    }

    // Parse generic report descriptor for non-boot protocol devices
    if (itf_protocol == HID_ITF_PROTOCOL_NONE && instance < CFG_TUH_HID) {
        hid_info[instance].report_count = tuh_hid_parse_report_descriptor(
            hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
    }

    // Request to receive reports
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("  Failed to request HID report\n");
    }
}

// Invoked when HID device is unmounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    printf("USB HID device unmounted: dev=%d inst=%d\n", dev_addr, instance);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 0;
        printf("  USB Keyboard disconnected\n");
    }
}

// Invoked when report is received
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            process_kbd_report((hid_keyboard_report_t const *)report, &prev_kbd_report);
            prev_kbd_report = *(hid_keyboard_report_t const *)report;
            break;

        default:
            process_generic_report(dev_addr, instance, report, len);
            break;
    }

    // Continue receiving reports
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        // Failed to request next report
    }
}

//--------------------------------------------------------------------
// Public API
//--------------------------------------------------------------------

void usbhid_init(void) {
    // Initialize TinyUSB Host
    tuh_init(BOARD_TUH_RHPORT);

    // Clear state
    memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
    key_action_head = 0;
    key_action_tail = 0;
    keyboard_connected = 0;

    printf("USB HID Host initialized\n");
}

void usbhid_task(void) {
    // Process USB events
    tuh_task();
}

int usbhid_keyboard_connected(void) {
    return keyboard_connected;
}

int usbhid_get_key_action(uint8_t *keycode, int *down) {
    if (key_action_head == key_action_tail) {
        return 0; // No actions queued
    }

    *keycode = key_action_queue[key_action_tail].keycode;
    *down = key_action_queue[key_action_tail].down;
    key_action_tail = (key_action_tail + 1) % KEY_ACTION_QUEUE_SIZE;
    return 1;
}

#endif // CFG_TUH_ENABLED
