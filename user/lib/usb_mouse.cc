/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// USB HID Boot Protocol mouse: get_mouse_event()
// Reads boot-mouse reports from the kernel USB HID SHM mouse sub-ring
// (rings[1]) and produces evdev input_event entries (EV_KEY button
// press/release, EV_REL X/Y/wheel movement). Report length is dynamic
// per-report (3 or 4 bytes, taken from slot->len) so 3-byte reports produce no
// REL_WHEEL and 4-byte reports do — no global "this device is 3 or 4" decision.
// Mirrors usb_kbd.cc (single-threaded evdev consumer, pending-event queue to
// return one event per call while one report yields several events).
#include "user/include/usb_mouse.h"
#include <stdint.h>
#include <string.h>
#include <xos/shm.h>

// Internal state
static volatile struct usb_hid_shm_header *hid_hdr;
static volatile uint8_t *hid_shm_base;
static uint8_t last_buttons; // previous button bitmap (bit0/1/2 = L/R/M)

// Pending event queue: one report may yield several events (button diff +
// movement + wheel); get_mouse_event returns one per call. Single-threaded
// (evdev consumer), no concurrency concerns.
#define MOUSE_PENDING_MAX 16
static input_event pending[MOUSE_PENDING_MAX];
static int pending_head;
static int pending_tail;

// Initialize get_mouse_event with SHM address
void get_mouse_event_init(void *shm_addr) {
  hid_shm_base = (volatile uint8_t *)shm_addr;
  hid_hdr = (volatile struct usb_hid_shm_header *)shm_addr;
  last_buttons = 0;
  pending_head = 0;
  pending_tail = 0;
  // Discard stale mouse slots accumulated before evdev started (mirrors
  // get_keycode_init's rings[0] tail=head flush, on rings[1]).
  __atomic_store_n(&hid_hdr->rings[1].tail,
                   __atomic_load_n(&hid_hdr->rings[1].head, __ATOMIC_ACQUIRE),
                   __ATOMIC_RELEASE);
}

// Enqueue one event into the pending queue (no-op if full — drops the event
// rather than overwriting unread events, matching usb_kbd.cc's guard).
static void enqueue(uint16_t type, uint16_t code, int32_t value) {
  int next_head = (pending_head + 1) % MOUSE_PENDING_MAX;
  if (next_head == pending_tail)
    return; // queue full
  input_event *e = &pending[pending_head];
  memset(e, 0,
         sizeof(*e)); // zero sec/usec; caller (on_mouse_event) stamps time
  e->type = type;
  e->code = code;
  e->value = value;
  pending_head = next_head;
}

int get_mouse_event(input_event *ev) {
  if (!hid_hdr)
    return -1;

  // 1. Pop from pending queue (events from a previously processed report).
  if (pending_head != pending_tail) {
    *ev = pending[pending_tail];
    pending_tail = (pending_tail + 1) % MOUSE_PENDING_MAX;
    return 0;
  }

  // 2. Find next mouse slot in the ring, skipping non-mouse slots. Only one
  //    slot per call — queue empties → return -1, evdev sends EV_SYN, next ISR
  //    wake processes the next slot. Aligns with Linux: one HID report → one
  //    event batch → one EV_SYN.
  volatile struct usb_hid_slot *slot = NULL;
  for (;;) {
    uint32_t head = __atomic_load_n(&hid_hdr->rings[1].head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&hid_hdr->rings[1].tail, __ATOMIC_ACQUIRE);

    if (head == tail)
      return -1; // ring empty

    slot = (volatile struct usb_hid_slot *)(hid_shm_base +
                                            HID_SUBRING_MOUSE_OFFSET +
                                            tail * HID_SLOT_SIZE);

    // Advance tail regardless of slot type (a stray keyboard slot routed here
    // by a future bug should not wedge the consumer).
    __atomic_store_n(&hid_hdr->rings[1].tail, (tail + 1) % HID_SUBRING_CAPACITY,
                     __ATOMIC_RELEASE);

    if (slot->type != HID_TYPE_MOUSE)
      continue;

    // A malformed mouse report is consumed just like an unrelated slot. Keep
    // scanning so one bad entry cannot hide a valid report already queued
    // behind it.
    if (slot->len < 3)
      continue;

    break; // found a complete mouse slot, process it below
  }

  // Boot mouse report layout (HID 1.11 boot protocol mouse):
  //   data[0] bit0/1/2 = left/right/middle button (bit3-7 reserved)
  //   data[1] = X delta (int8), data[2] = Y delta (int8)
  //   data[3] (optional) = wheel delta (int8); only present on 4-byte reports
  // Length is dynamic (slot->len, set by the ISR from the completion event's
  // transfer-length field). Truncated reports (len < 3) were dropped while
  // scanning the ring above.

  uint8_t buttons = slot->data[0] & 0x07; // mask reserved high bits
  int8_t dx = (int8_t)slot->data[1];
  int8_t dy = (int8_t)slot->data[2];

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
      enqueue(EV_KEY, btn_map[i].code, (buttons & btn_map[i].mask) ? 1 : 0);
  }
  last_buttons = buttons;

  // Relative movement. Skip zero deltas so a report with no motion does not
  // produce noise events (libinput still gets the button-only frame).
  if (dx != 0)
    enqueue(EV_REL, REL_X, (int32_t)dx);
  if (dy != 0)
    enqueue(EV_REL, REL_Y, (int32_t)dy);

  // Wheel: only when the 4th byte is present (len >= 4). 3-byte reports never
  // produce REL_WHEEL, naturally tolerating mixed-length streams without a
  // per-device "is 3 or 4" decision.
  if (slot->len >= 4) {
    int8_t wheel = (int8_t)slot->data[3];
    if (wheel != 0)
      enqueue(EV_REL, REL_WHEEL, (int32_t)wheel);
  }

  // Pop first event from queue if any.
  if (pending_head != pending_tail) {
    *ev = pending[pending_tail];
    pending_tail = (pending_tail + 1) % MOUSE_PENDING_MAX;
    return 0;
  }

  // No changes in this slot (no button change, no movement, no wheel).
  return -1;
}
