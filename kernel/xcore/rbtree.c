/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include "kernel/xcore/rbtree.h"

#define RB_RED 0
#define RB_BLACK 1
#define rb_color(n) ((n)->rb_parent_color & 1)
#define rb_is_red(n) (!rb_color(n))
#define rb_is_black(n) (rb_color(n))
#define rb_parent(n) ((rb_node *)((n)->rb_parent_color & ~(uintptr_t)1))
#define rb_set_red(n) ((n)->rb_parent_color &= ~(uintptr_t)1)
#define rb_set_black(n) ((n)->rb_parent_color |= 1)
#define rb_set_parent(n, p)                                                    \
  ((n)->rb_parent_color = ((n)->rb_parent_color & 1) | (uintptr_t)(p))
#define rb_set_color(n, c)                                                     \
  ((n)->rb_parent_color = ((n)->rb_parent_color & ~(uintptr_t)1) | (c))

static void rb_rotate_left(rb_root *root, rb_node *node) {
  rb_node *right = node->rb_right;
  node->rb_right = right->rb_left;
  if (right->rb_left)
    rb_set_parent(right->rb_left, node);
  rb_set_parent(right, rb_parent(node));
  if (!rb_parent(node))
    root->rb_node = right;
  else if (rb_parent(node)->rb_left == node)
    rb_parent(node)->rb_left = right;
  else
    rb_parent(node)->rb_right = right;
  right->rb_left = node;
  rb_set_parent(node, right);
}

static void rb_rotate_right(rb_root *root, rb_node *node) {
  rb_node *left = node->rb_left;
  node->rb_left = left->rb_right;
  if (left->rb_right)
    rb_set_parent(left->rb_right, node);
  rb_set_parent(left, rb_parent(node));
  if (!rb_parent(node))
    root->rb_node = left;
  else if (rb_parent(node)->rb_right == node)
    rb_parent(node)->rb_right = left;
  else
    rb_parent(node)->rb_left = left;
  left->rb_right = node;
  rb_set_parent(node, left);
}

static void rb_insert_fixup(rb_root *root, rb_node *node) {
  while (rb_parent(node) && rb_is_red(rb_parent(node))) {
    rb_node *parent = rb_parent(node);
    rb_node *gparent = rb_parent(parent);
    if (gparent->rb_left == parent) {
      rb_node *uncle = gparent->rb_right;
      if (uncle && rb_is_red(uncle)) {
        rb_set_black(uncle);
        rb_set_black(parent);
        rb_set_red(gparent);
        node = gparent;
      } else {
        if (parent->rb_right == node) {
          rb_rotate_left(root, parent);
          node = parent;
          parent = rb_parent(node);
        }
        rb_set_black(parent);
        rb_set_red(gparent);
        rb_rotate_right(root, gparent);
      }
    } else {
      rb_node *uncle = gparent->rb_left;
      if (uncle && rb_is_red(uncle)) {
        rb_set_black(uncle);
        rb_set_black(parent);
        rb_set_red(gparent);
        node = gparent;
      } else {
        if (parent->rb_left == node) {
          rb_rotate_right(root, parent);
          node = parent;
          parent = rb_parent(node);
        }
        rb_set_black(parent);
        rb_set_red(gparent);
        rb_rotate_left(root, gparent);
      }
    }
  }
  rb_set_black(root->rb_node);
}

void rb_insert(rb_root *root, rb_node *node,
               int (*cmp)(rb_node *a, rb_node *b)) {
  rb_node **link = &root->rb_node;
  rb_node *parent = NULL;
  while (*link) {
    parent = *link;
    if (cmp(node, parent) < 0)
      link = &parent->rb_left;
    else
      link = &parent->rb_right;
  }
  rb_set_parent(node, parent);
  node->rb_left = node->rb_right = NULL;
  rb_set_red(node);
  *link = node;
  rb_insert_fixup(root, node);
}

rb_node *rb_search(rb_root *root, rb_node *key,
                   int (*cmp)(rb_node *a, rb_node *b)) {
  rb_node *n = root->rb_node;
  while (n) {
    int c = cmp(key, n);
    if (c < 0)
      n = n->rb_left;
    else if (c > 0)
      n = n->rb_right;
    else
      return n;
  }
  return NULL;
}

static void rb_erase_fixup(rb_root *root, rb_node *node, rb_node *parent) {
  while ((!node || rb_is_black(node)) && node != root->rb_node) {
    if (parent && parent->rb_left == node) {
      rb_node *sibling = parent->rb_right;
      if (sibling && rb_is_red(sibling)) {
        rb_set_black(sibling);
        rb_set_red(parent);
        rb_rotate_left(root, parent);
        sibling = parent->rb_right;
      }
      if ((!sibling || !sibling->rb_left || rb_is_black(sibling->rb_left)) &&
          (!sibling || !sibling->rb_right || rb_is_black(sibling->rb_right))) {
        if (sibling)
          rb_set_red(sibling);
        node = parent;
        parent = rb_parent(node);
      } else if (sibling) {
        if (!sibling->rb_right || rb_is_black(sibling->rb_right)) {
          rb_set_black(sibling->rb_left);
          rb_set_red(sibling);
          rb_rotate_right(root, sibling);
          sibling = parent->rb_right;
        }
        rb_set_color(sibling, rb_color(parent));
        rb_set_black(parent);
        if (sibling->rb_right)
          rb_set_black(sibling->rb_right);
        rb_rotate_left(root, parent);
        node = root->rb_node;
        break;
      }
    } else if (parent) {
      rb_node *sibling = parent->rb_left;
      if (sibling && rb_is_red(sibling)) {
        rb_set_black(sibling);
        rb_set_red(parent);
        rb_rotate_right(root, parent);
        sibling = parent->rb_left;
      }
      if ((!sibling || !sibling->rb_right || rb_is_black(sibling->rb_right)) &&
          (!sibling || !sibling->rb_left || rb_is_black(sibling->rb_left))) {
        if (sibling)
          rb_set_red(sibling);
        node = parent;
        parent = rb_parent(node);
      } else if (sibling) {
        if (!sibling->rb_left || rb_is_black(sibling->rb_left)) {
          rb_set_black(sibling->rb_right);
          rb_set_red(sibling);
          rb_rotate_left(root, sibling);
          sibling = parent->rb_left;
        }
        rb_set_color(sibling, rb_color(parent));
        rb_set_black(parent);
        if (sibling->rb_left)
          rb_set_black(sibling->rb_left);
        rb_rotate_right(root, parent);
        node = root->rb_node;
        break;
      }
    }
  }
  if (node)
    rb_set_black(node);
}

void rb_erase(rb_root *root, rb_node *node) {
  rb_node *child = node->rb_left ? node->rb_left : node->rb_right;
  rb_node *parent = rb_parent(node);
  int color = rb_color(node);
  if (child)
    rb_set_parent(child, parent);
  if (!parent)
    root->rb_node = child;
  else if (parent->rb_left == node)
    parent->rb_left = child;
  else
    parent->rb_right = child;
  if (color == RB_BLACK)
    rb_erase_fixup(root, child, parent);
}

void rb_replace(rb_root *root, rb_node *old, rb_node *new) {
  rb_node *parent = rb_parent(old);
  if (!parent)
    root->rb_node = new;
  else if (parent->rb_left == old)
    parent->rb_left = new;
  else
    parent->rb_right = new;
  new->rb_left = old->rb_left;
  new->rb_right = old->rb_right;
  new->rb_parent_color = old->rb_parent_color;
}
