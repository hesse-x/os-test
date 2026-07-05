/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USER_USB_HID_H
#define USER_USB_HID_H

#include "input.h"
#include <stdint.h>
#include <xos/shm.h> // usb_hid_slot, usb_hid_shm_header, HID constants

// Read keyboard events from USB HID SHM ring
// Returns 0 if event produced, -1 if ring empty
int get_keycode(key_event *ev);

// Initialize get_keycode with SHM address
void get_keycode_init(void *shm_addr);

#endif // USER_USB_HID_H
