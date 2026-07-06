/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USER_INPUT_LIB_H
#define USER_INPUT_LIB_H

#include <stdint.h>
#include <xos/input.h>
#include <xos/ioctl.h>

// ===================== libinput_client public API =====================

// Poll ring: drain all pending events into caller buffer (pure drain, no
// sleeping flag). Returns number of events read (0 if ring empty).
int input_client_poll(volatile void *shm, input_event *events, int max_events);

// Helper: input_event → basic single-byte ASCII (lowercase letters, digits,
// symbols without shift, space/enter/bs/tab/esc). shift/caps/ESC-seq/Ctrl
// combos are caller's responsibility. Returns bytes written to buf (0 = no
// basic ASCII mapping for this event).
int input_event_to_ascii(const input_event *ev, uint8_t *buf, int buf_len);

// ===================== libinput_driver public API =====================

// Init and enter main loop (does not return).
// on_event: each callback fills one event into *ev, returns 1=has event / 0=HID
// empty; library loops calling it, and after each fill writes to all consumer
// rings + unconditional notify. hid_init: optional callback invoked after the
// library mmaps the HID SHM, receives the shm address.
void input_driver_run(uint32_t device_type, const char *dev_name,
                      const char *hid_dev_path,
                      int (*on_event)(input_event *ev),
                      void (*hid_init)(void *hid_shm));

#endif // USER_INPUT_LIB_H
