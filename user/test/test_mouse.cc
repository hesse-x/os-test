/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_mouse — USB HID boot-mouse parser Unity tests (mouse.md §3.2/§5.6).
 *
 * usb_mouse.cc reads boot-mouse reports from the kernel HID SHM mouse sub-ring
 * (rings[1]) and produces evdev input_event entries (EV_KEY button
 * press/release, EV_REL X/Y/wheel). These tests build an in-memory mock of the
 * SHM page, inject constructed 3-byte (no wheel) and 4-byte (with wheel)
 * reports, and assert the exact event sequences: button diff (press then
 * release), movement signs, and that REL_WHEEL only appears on 4-byte reports.
 * Compiled as C++ because usb_mouse.cc is C++ (it is a source of the evdev
 * target); Unity is C-linkage compatible (extern "C" guards in unity.h). */

#include <linux/input.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>
#include <xos/shm.h>

#include "user/include/usb_mouse.h"

void setUp(void) {}
void tearDown(void) {}

// A 4KB page standing in for the kernel-allocated HID SHM page. usb_mouse.cc
// addresses it via the SHM header + sub-ring offsets
// (HID_SUBRING_MOUSE_OFFSET), exactly as the kernel ISR lays it out.
static uint8_t shm_page[4096];

// Initialize the mock SHM page: write the header (magic/version + rings[1]
// descriptor with capacity) and zero the mouse sub-ring region. Mirrors what
// the kernel xHCI init writes before evdev mmaps the page.
static void shm_reset(void) {
  memset(shm_page, 0, sizeof(shm_page));
  struct usb_hid_shm_header *hdr = (struct usb_hid_shm_header *)shm_page;
  hdr->magic = USB_HID_SHM_MAGIC;
  hdr->version = USB_HID_SHM_VERSION;
  hdr->rings[1].head = 0;
  hdr->rings[1].tail = 0;
  hdr->rings[1].capacity = HID_SUBRING_CAPACITY;
  hdr->rings[1].reserved = 0;
}

// Inject one boot-mouse report into the mouse sub-ring and advance head. The
// report bytes are copied verbatim into slot->data; slot->len governs whether
// the parser reads the wheel byte (data[3]). This mirrors the kernel ISR path
// (xhci.c mouse branch): it fills type/len/data and atomically advances head.
static void inject_report(const uint8_t *data, uint8_t len) {
  struct usb_hid_shm_header *hdr = (struct usb_hid_shm_header *)shm_page;
  uint32_t head = hdr->rings[1].head;
  struct usb_hid_slot *slot =
      (struct usb_hid_slot *)(shm_page + HID_SUBRING_MOUSE_OFFSET +
                              head * HID_SLOT_SIZE);
  slot->type = HID_TYPE_MOUSE;
  slot->len = len;
  memcpy(slot->data, data, len);
  hdr->rings[1].head = (head + 1) % HID_SUBRING_CAPACITY;
}

// Drain every mouse event currently in the parser into out[], up to max. The
// parser returns one event per call and uses an internal pending queue, so this
// loops until the ring + queue are empty. Returns the count drained.
static int drain_all(input_event *out, int max) {
  int n = 0;
  input_event ev;
  while (n < max && get_mouse_event(&ev) == 0)
    out[n++] = ev;
  return n;
}

// TM-001: a rightward + downward movement with no button change and no wheel
// (3-byte report) yields exactly two EV_REL events (REL_X, REL_Y) and no
// REL_WHEEL.
void test_mouse_movement_3byte_no_wheel(void) {
  shm_reset();
  get_mouse_event_init(shm_page);

  uint8_t report[3] = {0x00, 0x05, 0x07}; // buttons=0, X=+5, Y=+7
  inject_report(report, 3);

  input_event evs[8];
  int n = drain_all(evs, 8);
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_UINT16(EV_REL, evs[0].type);
  TEST_ASSERT_EQUAL_UINT16(REL_X, evs[0].code);
  TEST_ASSERT_EQUAL_INT32(5, evs[0].value);
  TEST_ASSERT_EQUAL_UINT16(EV_REL, evs[1].type);
  TEST_ASSERT_EQUAL_UINT16(REL_Y, evs[1].code);
  TEST_ASSERT_EQUAL_INT32(7, evs[1].value);
}

// TM-002: a 4-byte report with a nonzero wheel byte yields REL_WHEEL. This is
// the acceptance criterion for the "wheel" item — a 3-byte device never
// produces REL_WHEEL, a 4-byte device does.
void test_mouse_wheel_4byte(void) {
  shm_reset();
  get_mouse_event_init(shm_page);

  uint8_t report[4] = {0x00, 0x00, 0x00, 0x03}; // no buttons, no move, wheel=+3
  inject_report(report, 4);

  input_event evs[8];
  int n = drain_all(evs, 8);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_UINT16(EV_REL, evs[0].type);
  TEST_ASSERT_EQUAL_UINT16(REL_WHEEL, evs[0].code);
  TEST_ASSERT_EQUAL_INT32(3, evs[0].value);
}

// TM-003: a 3-byte report (len==3) must NOT produce REL_WHEEL even when
// data[3] holds a nonzero value. This is the core invariant guarding the
// "phantom REL_WHEEL" hazard (mouse.md §3.1(e)): if the parser read data[3]
// unconditionally, a 3-byte device would surface the 4th byte's residual DMA
// garbage as a wheel event. The parser gates the wheel on slot->len >= 4, so
// only the declared length matters — not what data[3] happens to contain.
// Here we deliberately set data[3]=0xFF to prove the byte is never consulted.
void test_mouse_3byte_ignores_stale_wheel_byte(void) {
  shm_reset();
  get_mouse_event_init(shm_page);

  // Inject with len=3 but data[3] populated (simulating a buggy/over-copying
  // ISR, or a reused slot). The parser must ignore data[3] entirely.
  uint8_t report[4] = {0x00, 0x02, 0x00, 0xFF}; // X=+2, Y=0, "wheel"=0xFF
  inject_report(report, 3);

  input_event evs[8];
  int n = drain_all(evs, 8);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_UINT16(EV_REL, evs[0].type);
  TEST_ASSERT_EQUAL_UINT16(REL_X, evs[0].code);
  TEST_ASSERT_EQUAL_INT32(2, evs[0].value);
  // No further event — and critically no REL_WHEEL from the 0xFF in data[3].
}

// TM-004: pressing the left button (bit0 0→1) yields BTN_LEFT press (value=1);
// a subsequent report releasing it (bit0 1→0) yields BTN_LEFT release
// (value=0). Validates the button-diff path and last_buttons tracking across
// reports.
void test_mouse_button_press_release(void) {
  shm_reset();
  get_mouse_event_init(shm_page);

  uint8_t press[3] = {0x01, 0x00, 0x00}; // left button down
  inject_report(press, 3);
  input_event evs[8];
  int n = drain_all(evs, 8);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_UINT16(EV_KEY, evs[0].type);
  TEST_ASSERT_EQUAL_UINT16(BTN_LEFT, evs[0].code);
  TEST_ASSERT_EQUAL_INT32(1, evs[0].value);

  uint8_t release[3] = {0x00, 0x00, 0x00}; // left button up
  inject_report(release, 3);
  n = drain_all(evs, 8);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_UINT16(EV_KEY, evs[0].type);
  TEST_ASSERT_EQUAL_UINT16(BTN_LEFT, evs[0].code);
  TEST_ASSERT_EQUAL_INT32(0, evs[0].value);
}

// TM-005: one report with button press + movement + wheel yields all three
// event types in one batch (the pending queue buffers them so get_mouse_event
// returns one per call). libinput commits the batch on the following EV_SYN.
void test_mouse_combined_report_multiple_events(void) {
  shm_reset();
  get_mouse_event_init(shm_page);

  uint8_t report[4] = {0x02, 0x03, (uint8_t)-2, 0x01};
  // right button down, X=+3, Y=-2, wheel=+1
  inject_report(report, 4);

  input_event evs[8];
  int n = drain_all(evs, 8);
  TEST_ASSERT_EQUAL_INT(4, n);
  // Button diff first: right button 0→1.
  TEST_ASSERT_EQUAL_UINT16(EV_KEY, evs[0].type);
  TEST_ASSERT_EQUAL_UINT16(BTN_RIGHT, evs[0].code);
  TEST_ASSERT_EQUAL_INT32(1, evs[0].value);
  // Then the two relative axes.
  TEST_ASSERT_EQUAL_UINT16(EV_REL, evs[1].type);
  TEST_ASSERT_EQUAL_UINT16(REL_X, evs[1].code);
  TEST_ASSERT_EQUAL_INT32(3, evs[1].value);
  TEST_ASSERT_EQUAL_UINT16(EV_REL, evs[2].type);
  TEST_ASSERT_EQUAL_UINT16(REL_Y, evs[2].code);
  TEST_ASSERT_EQUAL_INT32(-2, evs[2].value);
  // Then the wheel.
  TEST_ASSERT_EQUAL_UINT16(EV_REL, evs[3].type);
  TEST_ASSERT_EQUAL_UINT16(REL_WHEEL, evs[3].code);
  TEST_ASSERT_EQUAL_INT32(1, evs[3].value);
}

// TM-006: a report with no button change, no movement, and no wheel produces no
// events (get_mouse_event returns -1). Guards against noise frames that would
// make libinput emit empty pointer updates.
void test_mouse_no_change_no_events(void) {
  shm_reset();
  get_mouse_event_init(shm_page);

  uint8_t report[3] = {0x00, 0x00, 0x00};
  inject_report(report, 3);

  input_event ev;
  TEST_ASSERT_EQUAL_INT(-1, get_mouse_event(&ev));
}

// TM-007: a truncated report (len < 3) is dropped — get_mouse_event returns -1
// and the ring's tail advances past it so the next valid report is still
// reachable. Mirrors the parser's len>=3 guard (mouse.md §3.2/§5.2).
void test_mouse_truncated_report_dropped(void) {
  shm_reset();
  get_mouse_event_init(shm_page);

  uint8_t trunc[2] = {0x01, 0x02};
  inject_report(trunc, 2);
  uint8_t good[3] = {0x00, 0x04, 0x00};
  inject_report(good, 3);

  input_event ev;
  // First call skips the truncated slot and returns the movement event.
  TEST_ASSERT_EQUAL_INT(0, get_mouse_event(&ev));
  TEST_ASSERT_EQUAL_UINT16(EV_REL, ev.type);
  TEST_ASSERT_EQUAL_UINT16(REL_X, ev.code);
  TEST_ASSERT_EQUAL_INT32(4, ev.value);
  // No further events.
  TEST_ASSERT_EQUAL_INT(-1, get_mouse_event(&ev));
}

// TM-008: negative X/Y deltas carry their int8 sign through to the int32 event
// value (left = negative X, up = negative Y). Guards against an unsigned-cast
// bug that would turn a leftward move into a huge positive jump.
void test_mouse_negative_deltas_sign_extend(void) {
  shm_reset();
  get_mouse_event_init(shm_page);

  uint8_t report[3] = {0x00, (uint8_t)-5, (uint8_t)-7}; // X=-5, Y=-7
  inject_report(report, 3);

  input_event evs[8];
  int n = drain_all(evs, 8);
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_INT32(-5, evs[0].value);
  TEST_ASSERT_EQUAL_INT32(-7, evs[1].value);
}

// extern "C": clang under -ffreestanding mangles a C++ `main`, breaking the
// crt0.o `main` reference (same as evdev.cc / shell.cc); gcc leaves `main`
// unmangled regardless.
extern "C" int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_mouse_movement_3byte_no_wheel);
  RUN_TEST(test_mouse_wheel_4byte);
  RUN_TEST(test_mouse_3byte_ignores_stale_wheel_byte);
  RUN_TEST(test_mouse_button_press_release);
  RUN_TEST(test_mouse_combined_report_multiple_events);
  RUN_TEST(test_mouse_no_change_no_events);
  RUN_TEST(test_mouse_truncated_report_dropped);
  RUN_TEST(test_mouse_negative_deltas_sign_extend);
  return UNITY_END();
}
