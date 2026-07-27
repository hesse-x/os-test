/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_ROBUST_LIST_H
#define COMMON_ROBUST_LIST_H

#include <stddef.h>
#include <stdint.h>

// Robust-futex list ABI (mirrors Linux include/uapi/linux/robust-list.h).
// musl's pthread robust-mutex support registers a per-thread linked list of
// held robust locks via set_robust_list(2); on thread exit the kernel walks
// the list, marks any still-held futex word with FUTEX_OWNER_DIED, and wakes
// one waiter so the next owner can detect the dead owner and recover.
//
// The futex word lives at a signed byte offset from each list node, so the
// same struct can describe locks embedded anywhere in user structures.

struct robust_list {
  struct robust_list *next; // NULL-terminated
};

struct robust_list_head {
  // The head's own .next starts the chain of held locks. The head node
  // itself is never a lock — it is just the anchor.
  struct robust_list list;
  // Signed byte offset from a robust_list node pointer to the futex word
  // inside the enclosing lock object. May be negative.
  long futex_offset;
  // A lock being acquired/released at the moment of exit (so the kernel can
  // finalize it even though it never made it onto/removed from .next).
  struct robust_list *list_op_pending;
};

// Bit set in a robust futex word by the kernel when its owning thread dies
// while still holding it. The next acquirer observes it and must fixup the
// lock state (recover / mark it unusable).
#define FUTEX_OWNER_DIED 0x40000000

#endif /* COMMON_ROBUST_LIST_H */
