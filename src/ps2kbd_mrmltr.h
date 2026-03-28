// You may use, distribute and modify this code under the
// terms of the GPLv2 license, which unfortunately won't be
// written for another century.
//
// SPDX-License-Identifier: GPL-2.0-or-later
//
#pragma once

#ifndef _PS2KBD_H
#define _PS2KBD_H

#include "hid.h"
#include <functional>


typedef struct {
  uint8_t code;
  bool release;
  uint8_t page;
} Ps2KbdAction_;


class Ps2Kbd_Mrmltr {
private:
  Ps2KbdAction_ _actions[2];
  unsigned int _action;
  bool _double;
  hid_keyboard_report_t _report;   // HID report structure

  std::function<void(hid_keyboard_report_t *curr, hid_keyboard_report_t *prev)> _keyHandler;

  inline void clearActions() {
    _actions[0].page = 0;
    _actions[0].release = false;
    _actions[0].code = 0;
    _actions[1].page = 0;
    _actions[1].release = false;
    _actions[1].code = 0;
    _action = 0;
  }

  void handleHidKeyPress(uint8_t hidKeyCode);
  void handleHidKeyRelease(uint8_t hidKeyCode);

  void handleActions();
  uint8_t hidCodePage0(uint8_t ps2code);
  uint8_t hidCodePage1(uint8_t ps2code);
  void clearHidKeys();

public:

  Ps2Kbd_Mrmltr(
    std::function<void(hid_keyboard_report_t *curr, hid_keyboard_report_t *prev)> keyHandler);

  void tick();
};

#endif
