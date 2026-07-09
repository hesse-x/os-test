/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef KERNEL_RBTREE_H
#define KERNEL_RBTREE_H

#include <stddef.h>
#include <stdint.h>

typedef struct rb_node {
  uintptr_t rb_parent_color; // low bit = color, rest = parent pointer
  struct rb_node *rb_right;
  struct rb_node *rb_left;
} rb_node;

typedef struct rb_root {
  rb_node *rb_node;
} rb_root;

#define RB_ROOT ((rb_root){NULL})
#define rb_entry(ptr, type, member)                                            \
  ((type *)((char *)(ptr) - offsetof(type, member)))

void rb_insert(rb_root *root, rb_node *node,
               int (*cmp)(rb_node *a, rb_node *b));
rb_node *rb_search(rb_root *root, rb_node *key,
                   int (*cmp)(rb_node *a, rb_node *b));
void rb_erase(rb_root *root, rb_node *node);
void rb_replace(rb_root *root, rb_node *old, rb_node *new);

// rb_first: leftmost node (in-order minimum). Returns NULL if empty.
static inline rb_node *rb_first(rb_root *root) {
  rb_node *n = root->rb_node;
  if (!n)
    return NULL;
  while (n->rb_left)
    n = n->rb_left;
  return n;
}

#endif // KERNEL_RBTREE_H
