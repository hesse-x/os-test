/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef USER_SYS_TLS_H
#define USER_SYS_TLS_H

#include <stddef.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

struct thread_entry;

// TLS template info (variant II layout)
// Phase one trimmed to minimum: pthread_create only needs to copy the merged
// template, no per-object offset. Future dlopen + __tls_get_addr will add
// per-object info.
struct tls_info {
  void *tdata_template; // TLS template (merged tdata copy source)
  size_t tdata_size;    // total tdata size
  size_t tbss_size;     // total tbss size
  size_t alignment;     // maximum alignment
  size_t size; // total size (tdata + tbss + padding, variant II block size)
};

// Global singleton, read by pthread_create
extern struct tls_info g_tls_info;

// Initialization function (static path: read linker symbols to fill g_tls_info)
void __libc_tls_init(void);

#include <pthread.h>

// TCB structure — placed at FS_BASE, %fs:0 returns &TCB.self
// variant II layout: [tls_block (.tdata+.tbss)] [TCB]
// FS_BASE points to TCB. TLS vars accessed via %fs:(-offset).
struct tcb {
  void *self;                  // points to this tcb
  pid_t tid;                   // thread ID (kernel writes via clear_tid)
  void *clear_tid_addr;        // for CLONE_CHILD_CLEARTID + futex_wake on exit
  int cancel_state;            // PTHREAD_CANCEL_ENABLE/DISABLE
  int cancel_type;             // PTHREAD_CANCEL_DEFERRED/ASYNCHRONOUS
  void *tsd[PTHREAD_KEYS_MAX]; // TSD values (pthread_key)
  __pthread_cleanup_handler_t *cleanup_head;
  int detached;
  struct thread_entry *entry; // back-pointer to slot (NULL for main thread)
  // === child thread start info (set by pthread_create, read by
  // __pthread_start) ===
  void *(*start_routine)(void *);
  void *arg;
  void *tls_page;   // for munmap on exit (NULL for main thread)
  size_t tls_total; // for munmap on exit
  int errno_val;    // per-thread errno (__errno_location returns &this field)
};

LIBC_EXPORT struct tcb *__pthread_current_tcb(void);

// Allocate TLS block + TCB for a thread (main or child).
// Returns TCB pointer (= FS_BASE). tls_page_out/tls_total_out for later munmap.
// Exported for pthread_create (pthread.cc) to call for child threads.
LIBC_EXPORT struct tcb *alloc_tls_block(void **tls_page_out,
                                        size_t *tls_total_out);

// Called by __libc_tls_init (static path) and __libc_start_main (dynamic path)
// after g_tls_info is filled. Allocates main thread TCB + sets FS_BASE +
// set_tid_address + registers cancel handler.
LIBC_EXPORT void __libc_tls_init_rest(void);

// Cancel check handler — registered via sys_pthread_set_cancel_handler.
// Called by kernel deliver_signal on SIGCANCEL. Reads TCB cancel_state:
//   ENABLE  → exit (phase 2: sys_exit; phase 3: pthread_exit(PTHREAD_CANCELED))
//   DISABLE → returns (sigreturn restores context)
LIBC_EXPORT void __pthread_cancel_check(int sig);

#ifdef __cplusplus
}
#endif

#endif // USER_SYS_TLS_H
