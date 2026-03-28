/**
 * PS/2 Keyboard wrapper for frank-386
 * Converts HID keycodes to Linux input keycodes for ps2_put_keycode()
 */

#include "ps2kbd_wrapper.h"
#include "ps2kbd_mrmltr.h"
#include <queue>

struct KeyEvent {
    int is_down;
    int keycode;  // Linux input keycode
};

static std::queue<KeyEvent> event_queue;
static Ps2Kbd_Mrmltr* kbd = nullptr;

// HID keycode to Linux input keycode mapping
// Linux keycodes are essentially evdev codes (same as AT Set 1 for most keys)
// Reference: /usr/include/linux/input-event-codes.h
static int hid_to_linux_keycode(uint8_t hid_code) {
    // Main keyboard keys
    switch (hid_code) {
        // Letters A-Z (HID 0x04-0x1D) -> Linux 30-53 (not sequential)
        case 0x04: return 30;  // A
        case 0x05: return 48;  // B
        case 0x06: return 46;  // C
        case 0x07: return 32;  // D
        case 0x08: return 18;  // E
        case 0x09: return 33;  // F
        case 0x0A: return 34;  // G
        case 0x0B: return 35;  // H
        case 0x0C: return 23;  // I
        case 0x0D: return 36;  // J
        case 0x0E: return 37;  // K
        case 0x0F: return 38;  // L
        case 0x10: return 50;  // M
        case 0x11: return 49;  // N
        case 0x12: return 24;  // O
        case 0x13: return 25;  // P
        case 0x14: return 16;  // Q
        case 0x15: return 19;  // R
        case 0x16: return 31;  // S
        case 0x17: return 20;  // T
        case 0x18: return 22;  // U
        case 0x19: return 47;  // V
        case 0x1A: return 17;  // W
        case 0x1B: return 45;  // X
        case 0x1C: return 21;  // Y
        case 0x1D: return 44;  // Z

        // Numbers 1-9, 0 (HID 0x1E-0x27)
        case 0x1E: return 2;   // 1
        case 0x1F: return 3;   // 2
        case 0x20: return 4;   // 3
        case 0x21: return 5;   // 4
        case 0x22: return 6;   // 5
        case 0x23: return 7;   // 6
        case 0x24: return 8;   // 7
        case 0x25: return 9;   // 8
        case 0x26: return 10;  // 9
        case 0x27: return 11;  // 0

        // Special keys
        case 0x28: return 28;  // Enter
        case 0x29: return 1;   // Escape
        case 0x2A: return 14;  // Backspace
        case 0x2B: return 15;  // Tab
        case 0x2C: return 57;  // Space
        case 0x2D: return 12;  // Minus
        case 0x2E: return 13;  // Equal
        case 0x2F: return 26;  // Left Bracket
        case 0x30: return 27;  // Right Bracket
        case 0x31: return 43;  // Backslash
        case 0x33: return 39;  // Semicolon
        case 0x34: return 40;  // Apostrophe
        case 0x35: return 41;  // Grave (backtick)
        case 0x36: return 51;  // Comma
        case 0x37: return 52;  // Period
        case 0x38: return 53;  // Slash
        case 0x39: return 58;  // Caps Lock

        // Function keys F1-F12
        case 0x3A: return 59;  // F1
        case 0x3B: return 60;  // F2
        case 0x3C: return 61;  // F3
        case 0x3D: return 62;  // F4
        case 0x3E: return 63;  // F5
        case 0x3F: return 64;  // F6
        case 0x40: return 65;  // F7
        case 0x41: return 66;  // F8
        case 0x42: return 67;  // F9
        case 0x43: return 68;  // F10
        case 0x44: return 87;  // F11
        case 0x45: return 88;  // F12

        // Print Screen, Scroll Lock, Pause
        case 0x46: return 99;  // Print Screen (extended)
        case 0x47: return 70;  // Scroll Lock
        case 0x48: return 119; // Pause (extended)

        // Navigation cluster (extended keys, need 0xe0 prefix)
        case 0x49: return 110; // Insert
        case 0x4A: return 102; // Home
        case 0x4B: return 104; // Page Up
        case 0x4C: return 111; // Delete
        case 0x4D: return 107; // End
        case 0x4E: return 109; // Page Down
        case 0x4F: return 106; // Right Arrow
        case 0x50: return 105; // Left Arrow
        case 0x51: return 108; // Down Arrow
        case 0x52: return 103; // Up Arrow

        // Numpad
        case 0x53: return 69;  // Num Lock
        case 0x54: return 98;  // Keypad /
        case 0x55: return 55;  // Keypad *
        case 0x56: return 74;  // Keypad -
        case 0x57: return 78;  // Keypad +
        case 0x58: return 96;  // Keypad Enter
        case 0x59: return 79;  // Keypad 1
        case 0x5A: return 80;  // Keypad 2
        case 0x5B: return 81;  // Keypad 3
        case 0x5C: return 75;  // Keypad 4
        case 0x5D: return 76;  // Keypad 5
        case 0x5E: return 77;  // Keypad 6
        case 0x5F: return 71;  // Keypad 7
        case 0x60: return 72;  // Keypad 8
        case 0x61: return 73;  // Keypad 9
        case 0x62: return 82;  // Keypad 0
        case 0x63: return 83;  // Keypad .

        // Modifiers (handled separately but included for completeness)
        case 0xE0: return 29;  // Left Control
        case 0xE1: return 42;  // Left Shift
        case 0xE2: return 56;  // Left Alt
        case 0xE3: return 125; // Left GUI (Windows key)
        case 0xE4: return 97;  // Right Control
        case 0xE5: return 54;  // Right Shift
        case 0xE6: return 100; // Right Alt
        case 0xE7: return 126; // Right GUI

        default: return 0;     // Unknown key
    }
}

// Key handler called by PS/2 driver when HID report changes
static void key_handler(hid_keyboard_report_t *curr, hid_keyboard_report_t *prev) {
    // Check modifier changes
    uint8_t changed_mods = curr->modifier ^ prev->modifier;

    if (changed_mods & KEYBOARD_MODIFIER_LEFTCTRL) {
        int is_down = (curr->modifier & KEYBOARD_MODIFIER_LEFTCTRL) != 0;
        event_queue.push({is_down, 29});  // Left Ctrl
    }
    if (changed_mods & KEYBOARD_MODIFIER_LEFTSHIFT) {
        int is_down = (curr->modifier & KEYBOARD_MODIFIER_LEFTSHIFT) != 0;
        event_queue.push({is_down, 42});  // Left Shift
    }
    if (changed_mods & KEYBOARD_MODIFIER_LEFTALT) {
        int is_down = (curr->modifier & KEYBOARD_MODIFIER_LEFTALT) != 0;
        event_queue.push({is_down, 56});  // Left Alt
    }
    if (changed_mods & KEYBOARD_MODIFIER_LEFTGUI) {
        int is_down = (curr->modifier & KEYBOARD_MODIFIER_LEFTGUI) != 0;
        event_queue.push({is_down, 125}); // Left GUI
    }
    if (changed_mods & KEYBOARD_MODIFIER_RIGHTCTRL) {
        int is_down = (curr->modifier & KEYBOARD_MODIFIER_RIGHTCTRL) != 0;
        event_queue.push({is_down, 97});  // Right Ctrl
    }
    if (changed_mods & KEYBOARD_MODIFIER_RIGHTSHIFT) {
        int is_down = (curr->modifier & KEYBOARD_MODIFIER_RIGHTSHIFT) != 0;
        event_queue.push({is_down, 54});  // Right Shift
    }
    if (changed_mods & KEYBOARD_MODIFIER_RIGHTALT) {
        int is_down = (curr->modifier & KEYBOARD_MODIFIER_RIGHTALT) != 0;
        event_queue.push({is_down, 100}); // Right Alt
    }
    if (changed_mods & KEYBOARD_MODIFIER_RIGHTGUI) {
        int is_down = (curr->modifier & KEYBOARD_MODIFIER_RIGHTGUI) != 0;
        event_queue.push({is_down, 126}); // Right GUI
    }

    // Check for newly pressed keys
    for (int i = 0; i < 6; i++) {
        if (curr->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (prev->keycode[j] == curr->keycode[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                int linux_code = hid_to_linux_keycode(curr->keycode[i]);
                if (linux_code != 0) {
                    event_queue.push({1, linux_code});
                }
            }
        }
    }

    // Check for released keys
    for (int i = 0; i < 6; i++) {
        if (prev->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (curr->keycode[j] == prev->keycode[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                int linux_code = hid_to_linux_keycode(prev->keycode[i]);
                if (linux_code != 0) {
                    event_queue.push({0, linux_code});
                }
            }
        }
    }
}

extern "C" void ps2kbd_init(void) {
    kbd = new Ps2Kbd_Mrmltr(key_handler);
}

extern "C" void ps2kbd_tick(void) {
    if (kbd) {
        kbd->tick();
    }
}

extern "C" int ps2kbd_get_key(int *is_down, int *keycode) {
    if (event_queue.empty()) {
        return 0;
    }
    KeyEvent e = event_queue.front();
    event_queue.pop();
    *is_down = e.is_down;
    *keycode = e.keycode;
    return 1;
}
