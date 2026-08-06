/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USER_USB_MOUSE_H
#define USER_USB_MOUSE_H

#include <linux/input.h> // input_event
#include <stddef.h>
#include <stdint.h>

#define USB_MOUSE_REPORT_MAX_EVENTS 6

enum usb_mouse_read_result {
  USB_MOUSE_REPORT_EMPTY = 0,
  USB_MOUSE_REPORT_READY = 1,
};

// Initialize the mouse HID parser with the kernel HID SHM base address.
// Discards any stale mouse sub-ring slots accumulated before evdev started
// (mirrors get_keycode_init's tail=head flush).
void get_mouse_event_init(void *shm_addr);

// Read one mouse report from the USB HID SHM mouse sub-ring. Boot-protocol
// report layout (3 or 4 bytes, dynamic per-report from slot->len):
//   data[0] bit0/1/2 = left/right/middle button
//   data[1] = X delta (int8), data[2] = Y delta (int8)
//   data[3] (optional) = wheel delta (int8); absent on 3-byte reports
// One successful call consumes exactly one valid report and preserves its
// boundary. Malformed/non-mouse slots are consumed while looking for the next
// report. READY with event_count == 0 represents a no-change report.
// Returns USB_MOUSE_REPORT_READY, USB_MOUSE_REPORT_EMPTY, or -EINVAL.
int get_mouse_report(input_event *events, size_t capacity, size_t *event_count);

uint64_t get_mouse_malformed_reports_total(void);

#endif // USER_USB_MOUSE_H
