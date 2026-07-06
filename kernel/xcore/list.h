/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_LIST_H
#define KERNEL_LIST_H

#include "kernel/xcore/log.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct list_node {
  struct list_node *prev;
  struct list_node *next;
} list_node;

static inline void list_init(list_node *head) {
  head->prev = head;
  head->next = head;
}

static inline void list_push_back(list_node *head, list_node *node) {
  node->prev = head->prev;
  node->next = head;
  head->prev->next = node;
  head->prev = node;
}

static inline void list_remove(list_node *node) {
  // Self-referencing node = not in any list; remove is no-op but suspicious
  WARN_ON(node->prev == node && node->next == node);
  node->prev->next = node->next;
  node->next->prev = node->prev;
  node->prev = node->next = node;
}

static inline bool list_empty(list_node *head) { return head->next == head; }

static inline list_node *list_front(list_node *head) { return head->next; }

#define LIST_ENTRY(node, type, member)                                         \
  ((type *)((char *)(node) - offsetof(type, member)))

#endif // KERNEL_LIST_H
