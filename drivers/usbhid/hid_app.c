/*
 * USB HID Host Application Callbacks for murm386
 * Implements TinyUSB Host callbacks for keyboard
 *
 * Based on TinyUSB HID host example
 * SPDX-License-Identifier: MIT
 */

#include "tusb.h"
#include "usbhid.h"
#include "debug.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"  // for time_us_32()

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
static volatile int mouse_connected = 0;

// Mouse state
typedef struct {
    int16_t dx;
    int16_t dy;
    int8_t dz;       // Scroll wheel
    uint8_t buttons; // Button state (bit 0=left, 1=right, 2=middle)
    bool has_event;
} mouse_state_t;

static mouse_state_t mouse_state = {0};

// Key action queue (for detecting press/release)
#define KEY_ACTION_QUEUE_SIZE 32
typedef struct {
    uint8_t keycode;
    int down;
} key_action_t;

static key_action_t key_action_queue[KEY_ACTION_QUEUE_SIZE];
static volatile int key_action_head = 0;
static volatile int key_action_tail = 0;

// Key repeat (typematic) support
// USB keyboards don't send repeat events - host must generate them
#define TYPEMATIC_DELAY_MS     500   // Initial delay before repeat starts
#define TYPEMATIC_RATE_MS      33    // ~30 chars/sec repeat rate

static uint8_t held_keys[6] = {0};       // Currently held keys (from last report)
static uint8_t held_modifiers = 0;       // Currently held modifiers
static uint8_t repeat_key = 0;           // Key currently being repeated (0 = none)
static uint32_t repeat_next_time = 0;    // Time for next repeat event
static bool repeat_initial = true;       // true = waiting for initial delay

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

    // Track the last newly pressed key for repeat
    uint8_t new_repeat_key = 0;

    // Check for released keys
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = prev_report->keycode[i];
        if (keycode && !find_keycode_in_report(report, keycode)) {
            queue_key_action(keycode, 0); // Key released
            // If the released key was being repeated, stop repeat
            if (keycode == repeat_key) {
                repeat_key = 0;
            }
        }
    }

    // Check for pressed keys
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode && !find_keycode_in_report(prev_report, keycode)) {
            queue_key_action(keycode, 1); // Key pressed
            new_repeat_key = keycode;     // This becomes the new repeat candidate
        }
    }

    // Update held keys state
    memcpy(held_keys, report->keycode, 6);
    held_modifiers = report->modifier;

    // Start repeat for the newly pressed key (if any)
    if (new_repeat_key) {
        repeat_key = new_repeat_key;
        repeat_next_time = time_us_32() + (TYPEMATIC_DELAY_MS * 1000);
        repeat_initial = true;
    }
}

// Process key repeat - call this from usbhid_task()
static void process_key_repeat(void) {
    if (repeat_key == 0) return;  // No key to repeat

    uint32_t now = time_us_32();

    // Check if repeat time has elapsed (handle wraparound)
    if ((int32_t)(now - repeat_next_time) >= 0) {
        // Queue a repeat key press event
        queue_key_action(repeat_key, 1);

        // Set next repeat time
        repeat_next_time = now + (TYPEMATIC_RATE_MS * 1000);
        repeat_initial = false;
    }
}

//--------------------------------------------------------------------
// Process mouse report
//--------------------------------------------------------------------

static void process_mouse_report(hid_mouse_report_t const *report) {
    // Accumulate mouse movement (will be consumed by usbhid_get_mouse_event)
    mouse_state.dx += report->x;
    mouse_state.dy += report->y;
    mouse_state.dz += report->wheel;
    mouse_state.buttons = report->buttons;
    mouse_state.has_event = true;
}

//--------------------------------------------------------------------
// Process generic HID report (for non-boot protocol keyboards/mice)
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
        } else if (rpt_info->usage == HID_USAGE_DESKTOP_MOUSE) {
            process_mouse_report((hid_mouse_report_t const *)report);
        }
    }
}

//--------------------------------------------------------------------
// TinyUSB Callbacks
//--------------------------------------------------------------------

// Invoked when HID device is mounted
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    DBG_PRINT("USB HID device mounted: dev=%d inst=%d protocol=%d\n", dev_addr, instance, itf_protocol);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 1;
        DBG_PRINT("  USB Keyboard connected!\n");
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        mouse_connected = 1;
        DBG_PRINT("  USB Mouse connected!\n");
    }

    // Parse generic report descriptor for non-boot protocol devices
    if (itf_protocol == HID_ITF_PROTOCOL_NONE && instance < CFG_TUH_HID) {
        hid_info[instance].report_count = tuh_hid_parse_report_descriptor(
            hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
    }

    // Request to receive reports
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        DBG_PRINT("  Failed to request HID report\n");
    }
}

// Invoked when HID device is unmounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    DBG_PRINT("USB HID device unmounted: dev=%d inst=%d\n", dev_addr, instance);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 0;
        DBG_PRINT("  USB Keyboard disconnected\n");
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        mouse_connected = 0;
        DBG_PRINT("  USB Mouse disconnected\n");
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

        case HID_ITF_PROTOCOL_MOUSE:
            process_mouse_report((hid_mouse_report_t const *)report);
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

    // Clear keyboard state
    memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
    key_action_head = 0;
    key_action_tail = 0;
    keyboard_connected = 0;

    // Clear key repeat state
    memset(held_keys, 0, sizeof(held_keys));
    held_modifiers = 0;
    repeat_key = 0;
    repeat_next_time = 0;
    repeat_initial = true;

    // Clear mouse state
    memset(&mouse_state, 0, sizeof(mouse_state));
    mouse_connected = 0;

    DBG_PRINT("USB HID Host initialized\n");
}

void usbhid_task(void) {
    // Process USB events
    tuh_task();

    // Process key repeat (typematic)
    process_key_repeat();
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

int usbhid_mouse_connected(void) {
    return mouse_connected;
}

int usbhid_get_mouse_event(int16_t *dx, int16_t *dy, int8_t *dz, uint8_t *buttons) {
    if (!mouse_state.has_event) {
        return 0;
    }

    *dx = mouse_state.dx;
    *dy = mouse_state.dy;
    *dz = mouse_state.dz;
    *buttons = mouse_state.buttons;

    // Clear accumulated state
    mouse_state.dx = 0;
    mouse_state.dy = 0;
    mouse_state.dz = 0;
    mouse_state.has_event = false;

    return 1;
}

#endif // CFG_TUH_ENABLED
