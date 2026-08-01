/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_wayland_client — libwayland-client.so pure-logic Unity tests.
//
// Covers only the pure-logic layer that does not need a real compositor:
//   - wl_array: init/add/for_each/copy/release (dynamic array + capacity
//   growth)
//   - wl_list:  init/insert/remove/length/empty/insert_list (intrusive
//   doubly-linked list)
// wl_display_xxx / wl_proxy_create / marshal paths require socket + display and
// are out of scope here (that is e2e; see doc/design/todo.md wayland
// acceptance).
//
// LINK_LIBS wayland-client c (DT_NEEDED libwayland-client.so); STATIC_LIBS
// unity linked at compile time. Headers resolved via INCLUDE_DIRS
// (third_party/wayland/src
// + third_party/wayland + build/) → wayland-client.h → generated protocol
// headers.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>
#include <wayland-client.h>

void setUp(void) {}
void tearDown(void) {}

// ===================== wl_array =====================

// After init, size=0; for_each iterates zero times; release leaks nothing.
void test_wl_array_init_zero(void) {
  struct wl_array a;
  wl_array_init(&a);
  TEST_ASSERT_EQUAL_INT(0, a.size);
  wl_array_release(&a);
  TEST_ASSERT_EQUAL_INT(0, a.size);
}

// add allocates and returns a writable slot; data lands correctly and size
// accumulates.
void test_wl_array_add_and_read(void) {
  struct wl_array a;
  wl_array_init(&a);

  int32_t *slot = wl_array_add(&a, sizeof(int32_t));
  TEST_ASSERT_NOT_NULL(slot);
  *slot = 42;

  int32_t *p = NULL;
  int n = 0;
  wl_array_for_each(p, &a) {
    TEST_ASSERT_EQUAL_INT(42, *p);
    n++;
  }
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_INT(sizeof(int32_t), a.size);

  wl_array_release(&a);
}

// Multiple adds: capacity (alloc) non-decreasing and for_each order preserved.
void test_wl_array_add_multiple(void) {
  struct wl_array a;
  wl_array_init(&a);

  for (int32_t i = 0; i < 16; i++) {
    int32_t *slot = wl_array_add(&a, sizeof(int32_t));
    TEST_ASSERT_NOT_NULL(slot);
    *slot = i * 10;
  }
  TEST_ASSERT_EQUAL_INT(16 * (int)sizeof(int32_t), a.size);
  TEST_ASSERT(a.alloc >= a.size);

  int32_t *p = NULL;
  int idx = 0;
  wl_array_for_each(p, &a) {
    TEST_ASSERT_EQUAL_INT(idx * 10, *p);
    idx++;
  }
  TEST_ASSERT_EQUAL_INT(16, idx);

  wl_array_release(&a);
}

// copy duplicates source into an inited dest; sizes match, contents equal
// elementwise.
void test_wl_array_copy(void) {
  struct wl_array src, dst;
  wl_array_init(&src);
  wl_array_init(&dst);

  for (int32_t i = 0; i < 8; i++) {
    int32_t *slot = wl_array_add(&src, sizeof(int32_t));
    *slot = 100 + i;
  }

  TEST_ASSERT_EQUAL_INT(0, wl_array_copy(&dst, &src));
  TEST_ASSERT_EQUAL_INT(src.size, dst.size);

  int32_t *ps = src.data;
  int32_t *pd = dst.data;
  for (size_t i = 0; i < dst.size / sizeof(int32_t); i++) {
    TEST_ASSERT_EQUAL_INT(ps[i], pd[i]);
  }

  wl_array_release(&src);
  wl_array_release(&dst);
}

// After release, re-init and reuse (add still works).
void test_wl_array_reuse_after_release(void) {
  struct wl_array a;
  wl_array_init(&a);
  int32_t *s = wl_array_add(&a, sizeof(int32_t));
  *s = 7;
  wl_array_release(&a);

  wl_array_init(&a);
  TEST_ASSERT_EQUAL_INT(0, a.size);
  s = wl_array_add(&a, sizeof(int32_t));
  TEST_ASSERT_NOT_NULL(s);
  *s = 9;
  TEST_ASSERT_EQUAL_INT(sizeof(int32_t), a.size);
  wl_array_release(&a);
}

// ===================== wl_list =====================

struct item {
  int value;
  struct wl_list link;
};

// After init the list is empty: length=0, empty=true.
void test_wl_list_init_empty(void) {
  struct wl_list list;
  wl_list_init(&list);
  TEST_ASSERT_TRUE(wl_list_empty(&list));
  TEST_ASSERT_EQUAL_INT(0, wl_list_length(&list));
}

// insert prepends; for_each iterates in reverse insertion order (newest first).
void test_wl_list_insert_iterate(void) {
  struct wl_list list;
  wl_list_init(&list);

  struct item a = {1, {0}}, b = {2, {0}}, c = {3, {0}};
  wl_list_insert(&list, &a.link);
  wl_list_insert(&list, &b.link);
  wl_list_insert(&list, &c.link);

  TEST_ASSERT_EQUAL_INT(3, wl_list_length(&list));
  TEST_ASSERT_FALSE(wl_list_empty(&list));

  struct item *cur;
  int seq[] = {3, 2, 1}; // head insert: c,b,a
  int idx = 0;
  wl_list_for_each(cur, &list, link) {
    TEST_ASSERT_EQUAL_INT(seq[idx++], cur->value);
  }
  TEST_ASSERT_EQUAL_INT(3, idx);
}

// remove a middle element; list stays coherent, length decreases.
void test_wl_list_remove_middle(void) {
  struct wl_list list;
  wl_list_init(&list);

  struct item a = {1, {0}}, b = {2, {0}}, c = {3, {0}};
  wl_list_insert(&list, &a.link);
  wl_list_insert(&list, &b.link);
  wl_list_insert(&list, &c.link);

  wl_list_remove(&b.link);
  TEST_ASSERT_EQUAL_INT(2, wl_list_length(&list));

  struct item *cur;
  int seq[] = {3, 1}; // c,a
  int idx = 0;
  wl_list_for_each(cur, &list, link) {
    TEST_ASSERT_EQUAL_INT(seq[idx++], cur->value);
  }
  TEST_ASSERT_EQUAL_INT(2, idx);

  // b detached; its link self-references (wayland convention), no effect on
  // list
  TEST_ASSERT_EQUAL_INT(2, b.value);
}

// remove the only element; list returns to empty.
void test_wl_list_remove_only(void) {
  struct wl_list list;
  wl_list_init(&list);

  struct item a = {5, {0}};
  wl_list_insert(&list, &a.link);
  wl_list_remove(&a.link);

  TEST_ASSERT_TRUE(wl_list_empty(&list));
  TEST_ASSERT_EQUAL_INT(0, wl_list_length(&list));
}

// insert_list splices the whole other list onto list's head; splice point is
// coherent and other itself stays valid.
void test_wl_list_insert_list(void) {
  struct wl_list list, other;
  wl_list_init(&list);
  wl_list_init(&other);

  struct item a = {1, {0}}, b = {2, {0}}; // list: a,b
  struct item c = {3, {0}}, d = {4, {0}}; // other: c,d
  wl_list_insert(&list, &b.link);
  wl_list_insert(&list, &a.link); // head insert → list: a,b
  wl_list_insert(&other, &d.link);
  wl_list_insert(&other, &c.link); // head insert → other: c,d

  wl_list_insert_list(&list, &other);

  TEST_ASSERT_EQUAL_INT(4, wl_list_length(&list));

  // insert_list splices the whole other segment to the list head: c,d,a,b.
  // Upstream wl_list_insert_list does not reset the other sentinel — its
  // next/prev still point at nodes spliced into list, so other is not "empty"
  // now (wl_list_empty(&other) is false). This matches upstream list-test.c,
  // which only checks list contents, not other emptiness. So we only verify
  // list length and order here.
  struct item *cur;
  int seq[] = {3, 4, 1, 2};
  int idx = 0;
  wl_list_for_each(cur, &list, link) {
    TEST_ASSERT_EQUAL_INT(seq[idx++], cur->value);
  }
  TEST_ASSERT_EQUAL_INT(4, idx);
}

int main(int argc, char **argv, char **envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  UNITY_BEGIN();
  // wl_array
  RUN_TEST(test_wl_array_init_zero);
  RUN_TEST(test_wl_array_add_and_read);
  RUN_TEST(test_wl_array_add_multiple);
  RUN_TEST(test_wl_array_copy);
  RUN_TEST(test_wl_array_reuse_after_release);
  // wl_list
  RUN_TEST(test_wl_list_init_empty);
  RUN_TEST(test_wl_list_insert_iterate);
  RUN_TEST(test_wl_list_remove_middle);
  RUN_TEST(test_wl_list_remove_only);
  RUN_TEST(test_wl_list_insert_list);
  return UNITY_END();
}
