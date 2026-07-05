/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// USB HID Boot Protocol keyboard: get_keycode()
// Reads HID reports from kernel USB HID SHM ring, compares with previous
// report to detect key press/release events, maps HID keycodes to input_key.
#include "usb_hid.h"
#include <stdint.h>
#include <string.h>
#include <xos/shm.h>

// Internal state
static uint8_t last_report[8];
static volatile struct usb_hid_shm_header *hid_hdr;
static volatile uint8_t *hid_shm_base;

// HID keycode → input_key mapping (core keys per HID Usage Table)
// HID Usage 0x04-0x1D: Keyboard A-Z (alphabetical order!)
// HID Usage 0x1E-0x26: Keyboard 1-0 (row, 1=0x1E, 2=0x1F, ... 0=0x27)
// HID Usage 0x28: Enter, 0x29: ESC, 0x2A: Backspace, 0x2B: Tab
// HID Usage 0x2C: Space, 0x2D-0x35: punctuation, 0x36: Comma, 0x37: Dot
// HID Usage 0x3A-0x3D: F1-F4, 0x3E-0x41: F5-F8, 0x42-0x45: F9-F12
// HID Usage 0x4F: Right, 0x50: Left, 0x51: Down, 0x52: Up
// HID Usage 0x49: Insert, 0x4A: Home, 0x4B: PageUp, 0x4C: Delete
// HID Usage 0x4D: End, 0x4E: PageDown
// HID Usage 0xE0-0xE7: modifier keys (left/right Ctrl/Shift/Alt/GUI)

static const uint16_t hid_to_input_key[256] = {
    // 0x00-0x03: reserved
    0, 0, 0, 0,
    // 0x04-0x1D: A-Z (HID Usage alphabetical order)
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K,
    KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V,
    KEY_W, KEY_X, KEY_Y, KEY_Z,
    // 0x1E-0x27: 1-0
    KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
    // 0x28-0x2F
    KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE, KEY_MINUS, KEY_EQUAL,
    KEY_LEFTBRACE,
    // 0x30-0x37
    KEY_RIGHTBRACE, KEY_BACKSLASH, 0, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
    KEY_COMMA, KEY_DOT,
    // 0x38-0x3F
    KEY_SLASH, KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    // 0x40-0x47
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, 0, 0,
    // 0x48-0x4F
    0, KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END, KEY_PAGEDOWN,
    KEY_RIGHT,
    // 0x50-0x57
    KEY_LEFT, KEY_DOWN, 0, 0, 0, 0, 0, 0,
    // 0x58-0x5F
    0, 0, 0, 0, 0, 0, 0, 0,
    // 0x60-0x67
    0, 0, 0, 0, 0, 0, 0, 0,
    // ... rest zeros
};

// Modifier mapping: HID modifier bitmap → key_event.modifiers
// HID byte 0: bit 0=Left Ctrl, 1=Left Shift, 2=Left Alt, 3=Left GUI
//             bit 4=Right Ctrl, 5=Right Shift, 6=Right Alt, 7=Right GUI
static uint8_t hid_modifier_to_mods(uint8_t hid_mods) {
  uint8_t mods = 0;
  if (hid_mods & 0x01)
    mods |= MOD_CTRL; // Left Ctrl
  if (hid_mods & 0x02)
    mods |= MOD_SHIFT; // Left Shift
  if (hid_mods & 0x04)
    mods |= MOD_ALT; // Left Alt
  if (hid_mods & 0x10)
    mods |= MOD_CTRL; // Right Ctrl
  if (hid_mods & 0x20)
    mods |= MOD_SHIFT; // Right Shift
  if (hid_mods & 0x40)
    mods |= MOD_ALT; // Right Alt
  if (hid_mods & 0x08)
    mods |= MOD_CAPS; // Treat Left GUI as Caps (approximation)
  return mods;
}

// Initialize get_keycode with SHM address
void get_keycode_init(void *shm_addr) {
  hid_shm_base = (volatile uint8_t *)shm_addr;
  hid_hdr = (volatile struct usb_hid_shm_header *)shm_addr;
  memset(last_report, 0, 8);
}

int get_keycode(key_event *ev) {
  if (!hid_hdr)
    return -1;

  uint32_t head = __atomic_load_n(&hid_hdr->rings[0].head, __ATOMIC_ACQUIRE);
  uint32_t tail = __atomic_load_n(&hid_hdr->rings[0].tail, __ATOMIC_ACQUIRE);

  if (head == tail)
    return -1; // ring empty

  // Read slot at tail position
  volatile struct usb_hid_slot *slot =
      (volatile struct usb_hid_slot *)(hid_shm_base + HID_SUBRING_KBD_OFFSET +
                                       tail * HID_SLOT_SIZE);

  // Only process keyboard type
  if (slot->type != HID_TYPE_KEYBOARD) {
    // Skip non-keyboard slot
    __atomic_store_n(&hid_hdr->rings[0].tail, (tail + 1) % HID_SUBRING_CAPACITY,
                     __ATOMIC_RELEASE);
    return -1; // let caller loop to try next slot
  }

  // Parse 8-byte HID Boot Protocol report
  // data[0] = modifier bitmap, data[1] = reserved(0), data[2..7] = keycodes
  uint8_t current_mods = slot->data[0];
  uint8_t current_keys[6];
  for (int i = 0; i < 6; i++)
    current_keys[i] = slot->data[2 + i];

  uint8_t last_mods = last_report[0];
  uint8_t last_keys[6];
  for (int i = 0; i < 6; i++)
    last_keys[i] = last_report[2 + i];

  // Advance tail
  __atomic_store_n(&hid_hdr->rings[0].tail, (tail + 1) % HID_SUBRING_CAPACITY,
                   __ATOMIC_RELEASE);

  // Update last_report
  for (int i = 0; i < 8; i++)
    last_report[i] = slot->data[i];

  // Detect events:
  // 1. Modifier changes — if modifier bitmap changed, report modifier
  // press/release
  uint8_t mod_diff = current_mods ^ last_mods;
  if (mod_diff) {
    // Find which modifier bit changed (only first one for simplicity)
    // Check for new modifier pressed
    for (int bit = 0; bit < 8; bit++) {
      if ((mod_diff >> bit) & 1) {
        uint8_t pressed = (current_mods >> bit) & 1;
        // Map modifier bit to a key via HID Usage 0xE0+bit
        ev->key = hid_to_input_key[0xE0 + bit];
        // Override with specific modifier key constants
        switch (bit) {
        case 0:
          ev->key = KEY_LEFTCTRL;
          break;
        case 1:
          ev->key = KEY_LEFTSHIFT;
          break;
        case 2:
          ev->key = KEY_LEFTALT;
          break;
        case 5:
          ev->key = KEY_RIGHTSHIFT;
          break;
        case 6:
          ev->key = KEY_RIGHTALT;
          break;
        case 4:
          ev->key = KEY_RIGHTCTRL;
          break;
        }
        ev->pressed = pressed;
        ev->modifiers = hid_modifier_to_mods(current_mods);
        return 0;
      }
    }
  }

  // 2. Find newly pressed keycodes (in current but not in last)
  for (int i = 0; i < 6; i++) {
    if (current_keys[i] == 0)
      continue;
    int found_in_last = 0;
    for (int j = 0; j < 6; j++) {
      if (current_keys[i] == last_keys[j]) {
        found_in_last = 1;
        break;
      }
    }
    if (!found_in_last) {
      ev->key = hid_to_input_key[current_keys[i]];
      ev->pressed = 1;
      ev->modifiers = hid_modifier_to_mods(current_mods);
      if (ev->key == 0)
        continue; // unmapped key, try next
      return 0;
    }
  }

  // 3. Find released keycodes (in last but not in current)
  for (int i = 0; i < 6; i++) {
    if (last_keys[i] == 0)
      continue;
    int found_in_current = 0;
    for (int j = 0; j < 6; j++) {
      if (last_keys[i] == current_keys[j]) {
        found_in_current = 1;
        break;
      }
    }
    if (!found_in_current) {
      ev->key = hid_to_input_key[last_keys[i]];
      ev->pressed = 0;
      ev->modifiers = hid_modifier_to_mods(current_mods);
      if (ev->key == 0)
        continue;
      return 0;
    }
  }

  // No event found in this report — try consuming next slot
  return -1;
}
