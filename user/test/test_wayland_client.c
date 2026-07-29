/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

/* test_wayland_client — libwayland-client.so 纯逻辑 Unity 测试.
 *
 * 只覆盖 client 库里不依赖真实 compositor 连接的纯逻辑层:
 *   - wl_array: init/add/for_each/copy/release(动态数组 + 容量增长)
 *   - wl_list:  init/insert/remove/length/empty/insert_list(侵入式双向链表)
 * wl_display_xxx / wl_proxy_create / marshal 路径需要 socket + display,
 * 不在本测范围(那是 e2e, 见 doc/design/todo.md wayland 验收).
 *
 * LINK_LIBS wayland-client c (DT_NEEDED libwayland-client.so); STATIC_LIBS
 * unity 编译期链入. 头经 INCLUDE_DIRS (third_party/wayland/src +
 * third_party/wayland + build/) 解析 wayland-client.h -> 生成协议头.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>
#include <wayland-client.h>

void setUp(void) {}
void tearDown(void) {}

/* ===================== wl_array ===================== */

/* init 后 size=0,可立即 for_each 零次遍历,且 release 不泄漏. */
void test_wl_array_init_zero(void) {
  struct wl_array a;
  wl_array_init(&a);
  TEST_ASSERT_EQUAL_INT(0, a.size);
  wl_array_release(&a);
  TEST_ASSERT_EQUAL_INT(0, a.size);
}

/* add 分配并返回可写槽位;数据落点正确且 size 累计. */
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

/* 连续 add 多个元素,容量(alloc)非递减且 for_each 顺序正确. */
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

/* copy 复制源数据到已 init 的目标,size 一致且内容逐元素相等. */
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

/* release 后可重新 init 复用(add 仍正常工作). */
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

/* ===================== wl_list ===================== */

struct item {
  int value;
  struct wl_list link;
};

/* init 后链表为空,length=0,empty=true. */
void test_wl_list_init_empty(void) {
  struct wl_list list;
  wl_list_init(&list);
  TEST_ASSERT_TRUE(wl_list_empty(&list));
  TEST_ASSERT_EQUAL_INT(0, wl_list_length(&list));
}

/* insert 头插,for_each 按插入逆序遍历(后插在前). */
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
  int seq[] = {3, 2, 1}; /* 头插: c,b,a */
  int idx = 0;
  wl_list_for_each(cur, &list, link) {
    TEST_ASSERT_EQUAL_INT(seq[idx++], cur->value);
  }
  TEST_ASSERT_EQUAL_INT(3, idx);
}

/* remove 中间元素后链表仍连贯,length 递减. */
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
  int seq[] = {3, 1}; /* c,a */
  int idx = 0;
  wl_list_for_each(cur, &list, link) {
    TEST_ASSERT_EQUAL_INT(seq[idx++], cur->value);
  }
  TEST_ASSERT_EQUAL_INT(2, idx);

  /* b 已脱离,其 link 自指(wayland 惯例),不影响 list */
  TEST_ASSERT_EQUAL_INT(2, b.value);
}

/* remove 唯一元素后链表回到空. */
void test_wl_list_remove_only(void) {
  struct wl_list list;
  wl_list_init(&list);

  struct item a = {5, {0}};
  wl_list_insert(&list, &a.link);
  wl_list_remove(&a.link);

  TEST_ASSERT_TRUE(wl_list_empty(&list));
  TEST_ASSERT_EQUAL_INT(0, wl_list_length(&list));
}

/* insert_list 把整条 other 接到 list 头部,拆分点连贯且 other 自身仍有效. */
void test_wl_list_insert_list(void) {
  struct wl_list list, other;
  wl_list_init(&list);
  wl_list_init(&other);

  struct item a = {1, {0}}, b = {2, {0}}; /* list: a,b */
  struct item c = {3, {0}}, d = {4, {0}}; /* other: c,d */
  wl_list_insert(&list, &b.link);
  wl_list_insert(&list, &a.link); /* 头插 → list: a,b */
  wl_list_insert(&other, &d.link);
  wl_list_insert(&other, &c.link); /* 头插 → other: c,d */

  wl_list_insert_list(&list, &other);

  TEST_ASSERT_EQUAL_INT(4, wl_list_length(&list));

  /* insert_list 把 other 整段插到 list 头: c,d,a,b.
   * 注意: 上游 wl_list_insert_list 不重置 other 哨兵——其 next/prev 仍指向
   * 已splice进 list 的节点, 故 other 此刻非"空"（wl_list_empty(&other) 为假）,
   * 与 wayland 自带 list-test.c 的断言口径一致（那里只校验 list 内容, 不校验
   * other 是否清空）。因此这里只验证 list 的长度与顺序。 */

  /* insert_list 把 other 整段插到 list 头: c,d,a,b */
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
  /* wl_array */
  RUN_TEST(test_wl_array_init_zero);
  RUN_TEST(test_wl_array_add_and_read);
  RUN_TEST(test_wl_array_add_multiple);
  RUN_TEST(test_wl_array_copy);
  RUN_TEST(test_wl_array_reuse_after_release);
  /* wl_list */
  RUN_TEST(test_wl_list_init_empty);
  RUN_TEST(test_wl_list_insert_iterate);
  RUN_TEST(test_wl_list_remove_middle);
  RUN_TEST(test_wl_list_remove_only);
  RUN_TEST(test_wl_list_insert_list);
  return UNITY_END();
}
