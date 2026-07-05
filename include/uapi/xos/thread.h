/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_THREAD_H
#define COMMON_THREAD_H

#include <stddef.h>
#include <stdint.h>

// Thread cleanup info passed via clone arg6 (filled by user, read by kernel)
// Does not depend on struct tcb layout, keeps user/kernel layering
struct thread_clone_info {
  int detached;             // 1 = detached thread
  uint64_t tls_page;        // user vaddr of TLS+TCB page (0 if N/A)
  size_t tls_total;         // size of TLS+TCB mapping
  uint64_t user_stack_base; // user vaddr of stack base (incl guard)
  size_t user_stack_size;   // stack+guard total size
};

#endif /* COMMON_THREAD_H */
