/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USER_USB_MOUSE_H
#define USER_USB_MOUSE_H

#include <linux/input.h> // input_event
#include <stdint.h>

// Initialize the mouse HID parser with the kernel HID SHM base address.
// Discards any stale mouse sub-ring slots accumulated before evdev started
// (mirrors get_keycode_init's tail=head flush).
void get_mouse_event_init(void *shm_addr);

// Read one mouse event from the USB HID SHM mouse sub-ring. Boot-protocol
// report layout (3 or 4 bytes, dynamic per-report from slot->len):
//   data[0] bit0/1/2 = left/right/middle button
//   data[1] = X delta (int8), data[2] = Y delta (int8)
//   data[3] (optional) = wheel delta (int8); absent on 3-byte reports
// One report may produce multiple events (button diff + movement + wheel),
// buffered in a pending queue. Returns 0 if an event was produced, -1 if the
// ring is empty.
int get_mouse_event(input_event *ev);

#endif // USER_USB_MOUSE_H
