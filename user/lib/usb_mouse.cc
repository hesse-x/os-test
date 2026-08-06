/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// USB HID Boot Protocol mouse: get_mouse_report()
// Reads boot-mouse reports from the kernel USB HID SHM mouse sub-ring
// (rings[1]) and produces evdev input_event entries (EV_KEY button
// press/release, EV_REL X/Y/wheel movement). Report length is dynamic
// per-report (3 or 4 bytes, taken from slot->len) so 3-byte reports produce no
// REL_WHEEL and 4-byte reports do — no global "this device is 3 or 4" decision.
// Each call preserves one HID report boundary for the evdev framing layer.
#include "user/include/usb_mouse.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <xos/shm.h>

// Internal state
static volatile struct usb_hid_shm_header *hid_hdr;
static volatile uint8_t *hid_shm_base;
static uint8_t last_buttons; // previous button bitmap (bit0/1/2 = L/R/M)
static uint64_t malformed_reports_total;

// Initialize get_mouse_report with SHM address.
void get_mouse_event_init(void *shm_addr) {
  hid_shm_base = (volatile uint8_t *)shm_addr;
  hid_hdr = (volatile struct usb_hid_shm_header *)shm_addr;
  last_buttons = 0;
  malformed_reports_total = 0;
  // Discard stale mouse slots accumulated before evdev started (mirrors
  // get_keycode_init's rings[0] tail=head flush, on rings[1]).
  __atomic_store_n(&hid_hdr->rings[1].tail,
                   __atomic_load_n(&hid_hdr->rings[1].head, __ATOMIC_ACQUIRE),
                   __ATOMIC_RELEASE);
}

static void append_event(input_event *events, size_t *count, uint16_t type,
                         uint16_t code, int32_t value) {
  input_event *e = &events[(*count)++];
  memset(e, 0, sizeof(*e));
  e->type = type;
  e->code = code;
  e->value = value;
}

int get_mouse_report(input_event *events, size_t capacity,
                     size_t *event_count) {
  if (!events || !event_count || capacity < USB_MOUSE_REPORT_MAX_EVENTS)
    return -EINVAL;
  *event_count = 0;
  if (!hid_hdr)
    return USB_MOUSE_REPORT_EMPTY;

  // Find the next valid mouse slot, consuming malformed or misrouted slots so
  // they cannot wedge the SPSC ring.
  uint8_t report_len = 0;
  uint8_t report[4];
  for (;;) {
    uint32_t head = __atomic_load_n(&hid_hdr->rings[1].head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&hid_hdr->rings[1].tail, __ATOMIC_ACQUIRE);

    if (head == tail)
      return USB_MOUSE_REPORT_EMPTY;

    volatile struct usb_hid_slot *slot =
        (volatile struct usb_hid_slot *)(hid_shm_base +
                                         HID_SUBRING_MOUSE_OFFSET +
                                         tail * HID_SLOT_SIZE);

    // Copy everything needed before publishing the new tail. Once tail moves,
    // the producer is allowed to reuse this slot immediately.
    uint8_t slot_type = slot->type;
    report_len = slot->len;
    for (size_t i = 0; i < sizeof(report); i++)
      report[i] = slot->data[i];

    // Advance tail regardless of slot type (a stray keyboard slot routed here
    // by a future bug should not wedge the consumer).
    __atomic_store_n(&hid_hdr->rings[1].tail, (tail + 1) % HID_SUBRING_CAPACITY,
                     __ATOMIC_RELEASE);

    if (slot_type != HID_TYPE_MOUSE)
      continue;

    // A malformed mouse report is consumed just like an unrelated slot. Keep
    // scanning so one bad entry cannot hide a valid report already queued
    // behind it.
    if (report_len < 3) {
      malformed_reports_total++;
      continue;
    }

    break; // found a complete mouse slot, process it below
  }

  // Boot mouse report layout (HID 1.11 boot protocol mouse):
  //   data[0] bit0/1/2 = left/right/middle button (bit3-7 reserved)
  //   data[1] = X delta (int8), data[2] = Y delta (int8)
  //   data[3] (optional) = wheel delta (int8); only present on 4-byte reports
  // Length is dynamic (slot->len, set by the ISR from the completion event's
  // transfer-length field). Truncated reports (len < 3) were dropped while
  // scanning the ring above.

  uint8_t buttons = report[0] & 0x07; // mask reserved high bits
  int8_t dx = (int8_t)report[1];
  int8_t dy = (int8_t)report[2];

  // Button diff: each changed bit yields one EV_KEY event (value = current
  // pressed state of that button). Non-mutex with the keyboard path — a pure
  // mouse has no KEY_* caps so udevd won't mis-classify it as a keyboard.
  uint8_t changed = buttons ^ last_buttons;
  static const struct {
    uint16_t code;
    uint8_t mask;
  } btn_map[3] = {
      {BTN_LEFT, 0x01},
      {BTN_RIGHT, 0x02},
      {BTN_MIDDLE, 0x04},
  };
  for (int i = 0; i < 3; i++) {
    if (changed & btn_map[i].mask)
      append_event(events, event_count, EV_KEY, btn_map[i].code,
                   (buttons & btn_map[i].mask) ? 1 : 0);
  }
  last_buttons = buttons;

  // Relative movement. Skip zero deltas so a report with no motion does not
  // produce noise events (libinput still gets the button-only frame).
  if (dx != 0)
    append_event(events, event_count, EV_REL, REL_X, (int32_t)dx);
  if (dy != 0)
    append_event(events, event_count, EV_REL, REL_Y, (int32_t)dy);

  // Wheel: only when the 4th byte is present (len >= 4). 3-byte reports never
  // produce REL_WHEEL, naturally tolerating mixed-length streams without a
  // per-device "is 3 or 4" decision.
  if (report_len >= 4) {
    int8_t wheel = (int8_t)report[3];
    if (wheel != 0)
      append_event(events, event_count, EV_REL, REL_WHEEL, (int32_t)wheel);
  }
  return USB_MOUSE_REPORT_READY;
}

uint64_t get_mouse_malformed_reports_total(void) {
  return malformed_reports_total;
}
