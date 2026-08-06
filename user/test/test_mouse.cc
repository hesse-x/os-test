/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */
/* USB HID boot-mouse report parser tests. */

#include <errno.h>
#include <linux/input.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>
#include <xos/shm.h>

#include "user/include/usb_mouse.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t shm_page[4096];

static void shm_reset(void) {
  memset(shm_page, 0, sizeof(shm_page));
  struct usb_hid_shm_header *hdr = (struct usb_hid_shm_header *)shm_page;
  hdr->magic = USB_HID_SHM_MAGIC;
  hdr->version = USB_HID_SHM_VERSION;
  hdr->rings[1].capacity = HID_SUBRING_CAPACITY;
}

static void inject_slot(uint8_t type, const uint8_t *data, uint8_t len) {
  struct usb_hid_shm_header *hdr = (struct usb_hid_shm_header *)shm_page;
  uint32_t head = hdr->rings[1].head;
  struct usb_hid_slot *slot =
      (struct usb_hid_slot *)(shm_page + HID_SUBRING_MOUSE_OFFSET +
                              head * HID_SLOT_SIZE);
  slot->type = type;
  slot->len = len;
  if (data && len)
    memcpy(slot->data, data, len);
  hdr->rings[1].head = (head + 1) % HID_SUBRING_CAPACITY;
}

static void inject_report(const uint8_t *data, uint8_t len) {
  inject_slot(HID_TYPE_MOUSE, data, len);
}

static size_t read_report(input_event *events) {
  size_t count = 99;
  TEST_ASSERT_EQUAL_INT(
      USB_MOUSE_REPORT_READY,
      get_mouse_report(events, USB_MOUSE_REPORT_MAX_EVENTS, &count));
  return count;
}

void test_mouse_movement_report(void) {
  shm_reset();
  get_mouse_event_init(shm_page);
  const uint8_t report[3] = {0, 5, 7};
  inject_report(report, sizeof(report));

  input_event events[USB_MOUSE_REPORT_MAX_EVENTS];
  TEST_ASSERT_EQUAL_UINT(2, read_report(events));
  TEST_ASSERT_EQUAL_UINT16(EV_REL, events[0].type);
  TEST_ASSERT_EQUAL_UINT16(REL_X, events[0].code);
  TEST_ASSERT_EQUAL_INT32(5, events[0].value);
  TEST_ASSERT_EQUAL_UINT16(EV_REL, events[1].type);
  TEST_ASSERT_EQUAL_UINT16(REL_Y, events[1].code);
  TEST_ASSERT_EQUAL_INT32(7, events[1].value);
}

void test_mouse_combined_report_order(void) {
  shm_reset();
  get_mouse_event_init(shm_page);
  const uint8_t report[4] = {2, 3, (uint8_t)-2, 1};
  inject_report(report, sizeof(report));

  input_event events[USB_MOUSE_REPORT_MAX_EVENTS];
  TEST_ASSERT_EQUAL_UINT(4, read_report(events));
  TEST_ASSERT_EQUAL_UINT16(BTN_RIGHT, events[0].code);
  TEST_ASSERT_EQUAL_INT32(1, events[0].value);
  TEST_ASSERT_EQUAL_UINT16(REL_X, events[1].code);
  TEST_ASSERT_EQUAL_INT32(3, events[1].value);
  TEST_ASSERT_EQUAL_UINT16(REL_Y, events[2].code);
  TEST_ASSERT_EQUAL_INT32(-2, events[2].value);
  TEST_ASSERT_EQUAL_UINT16(REL_WHEEL, events[3].code);
  TEST_ASSERT_EQUAL_INT32(1, events[3].value);
}

void test_mouse_reports_never_merge(void) {
  shm_reset();
  get_mouse_event_init(shm_page);
  const uint8_t first[3] = {0, 1, 0};
  const uint8_t second[3] = {0, 0, 2};
  inject_report(first, sizeof(first));
  inject_report(second, sizeof(second));

  input_event events[USB_MOUSE_REPORT_MAX_EVENTS];
  TEST_ASSERT_EQUAL_UINT(1, read_report(events));
  TEST_ASSERT_EQUAL_UINT16(REL_X, events[0].code);
  TEST_ASSERT_EQUAL_UINT(1, read_report(events));
  TEST_ASSERT_EQUAL_UINT16(REL_Y, events[0].code);
  size_t count = 1;
  TEST_ASSERT_EQUAL_INT(
      USB_MOUSE_REPORT_EMPTY,
      get_mouse_report(events, USB_MOUSE_REPORT_MAX_EVENTS, &count));
  TEST_ASSERT_EQUAL_UINT(0, count);
}

void test_mouse_empty_report_does_not_stop_drain(void) {
  shm_reset();
  get_mouse_event_init(shm_page);
  const uint8_t empty[3] = {0, 0, 0};
  const uint8_t movement[3] = {0, 4, 0};
  inject_report(empty, sizeof(empty));
  inject_report(movement, sizeof(movement));

  input_event events[USB_MOUSE_REPORT_MAX_EVENTS];
  TEST_ASSERT_EQUAL_UINT(0, read_report(events));
  TEST_ASSERT_EQUAL_UINT(1, read_report(events));
  TEST_ASSERT_EQUAL_UINT16(REL_X, events[0].code);
  TEST_ASSERT_EQUAL_INT32(4, events[0].value);
}

void test_mouse_malformed_report_is_skipped(void) {
  shm_reset();
  get_mouse_event_init(shm_page);
  const uint8_t malformed[2] = {1, 2};
  const uint8_t movement[3] = {0, 4, 0};
  inject_report(malformed, sizeof(malformed));
  inject_report(movement, sizeof(movement));

  input_event events[USB_MOUSE_REPORT_MAX_EVENTS];
  TEST_ASSERT_EQUAL_UINT(1, read_report(events));
  TEST_ASSERT_EQUAL_UINT16(REL_X, events[0].code);
  TEST_ASSERT_EQUAL_INT32(4, events[0].value);
  TEST_ASSERT_EQUAL_UINT64(1, get_mouse_malformed_reports_total());
}

void test_mouse_negative_deltas_and_three_byte_wheel_guard(void) {
  shm_reset();
  get_mouse_event_init(shm_page);
  uint8_t report[4] = {0, (uint8_t)-5, (uint8_t)-7, 0xff};
  inject_report(report, 3);

  input_event events[USB_MOUSE_REPORT_MAX_EVENTS];
  TEST_ASSERT_EQUAL_UINT(2, read_report(events));
  TEST_ASSERT_EQUAL_INT32(-5, events[0].value);
  TEST_ASSERT_EQUAL_INT32(-7, events[1].value);
}

void test_mouse_three_buttons_fit_max_capacity(void) {
  shm_reset();
  get_mouse_event_init(shm_page);
  const uint8_t report[4] = {7, 1, 1, 1};
  inject_report(report, sizeof(report));

  input_event events[USB_MOUSE_REPORT_MAX_EVENTS];
  TEST_ASSERT_EQUAL_UINT(USB_MOUSE_REPORT_MAX_EVENTS, read_report(events));
  TEST_ASSERT_EQUAL_UINT16(BTN_LEFT, events[0].code);
  TEST_ASSERT_EQUAL_UINT16(BTN_RIGHT, events[1].code);
  TEST_ASSERT_EQUAL_UINT16(BTN_MIDDLE, events[2].code);
  TEST_ASSERT_EQUAL_UINT16(REL_X, events[3].code);
  TEST_ASSERT_EQUAL_UINT16(REL_Y, events[4].code);
  TEST_ASSERT_EQUAL_UINT16(REL_WHEEL, events[5].code);
}

void test_mouse_invalid_arguments(void) {
  shm_reset();
  get_mouse_event_init(shm_page);
  input_event events[USB_MOUSE_REPORT_MAX_EVENTS];
  size_t count = 0;
  TEST_ASSERT_EQUAL_INT(
      -EINVAL, get_mouse_report(NULL, USB_MOUSE_REPORT_MAX_EVENTS, &count));
  TEST_ASSERT_EQUAL_INT(
      -EINVAL, get_mouse_report(events, USB_MOUSE_REPORT_MAX_EVENTS, NULL));
  TEST_ASSERT_EQUAL_INT(
      -EINVAL,
      get_mouse_report(events, USB_MOUSE_REPORT_MAX_EVENTS - 1, &count));
}

void test_mouse_drains_99_reports_with_displacement_conservation(void) {
  shm_reset();
  get_mouse_event_init(shm_page);
  const uint8_t report[3] = {0, 1, (uint8_t)-1};
  for (int i = 0; i < 99; i++)
    inject_report(report, sizeof(report));

  input_event events[USB_MOUSE_REPORT_MAX_EVENTS];
  int reports = 0;
  int dx = 0;
  int dy = 0;
  for (;;) {
    size_t count = 0;
    int rc = get_mouse_report(events, USB_MOUSE_REPORT_MAX_EVENTS, &count);
    if (rc == USB_MOUSE_REPORT_EMPTY)
      break;
    TEST_ASSERT_EQUAL_INT(USB_MOUSE_REPORT_READY, rc);
    reports++;
    for (size_t i = 0; i < count; i++) {
      if (events[i].code == REL_X)
        dx += events[i].value;
      if (events[i].code == REL_Y)
        dy += events[i].value;
    }
  }
  TEST_ASSERT_EQUAL_INT(99, reports);
  TEST_ASSERT_EQUAL_INT(99, dx);
  TEST_ASSERT_EQUAL_INT(-99, dy);
}

extern "C" int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_mouse_movement_report);
  RUN_TEST(test_mouse_combined_report_order);
  RUN_TEST(test_mouse_reports_never_merge);
  RUN_TEST(test_mouse_empty_report_does_not_stop_drain);
  RUN_TEST(test_mouse_malformed_report_is_skipped);
  RUN_TEST(test_mouse_negative_deltas_and_three_byte_wheel_guard);
  RUN_TEST(test_mouse_three_buttons_fit_max_capacity);
  RUN_TEST(test_mouse_invalid_arguments);
  RUN_TEST(test_mouse_drains_99_reports_with_displacement_conservation);
  return UNITY_END();
}
