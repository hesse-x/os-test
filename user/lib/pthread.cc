/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// user/lib/pthread.cc — pthread library implementation (Phase 4)
#include <sys/tls.h>
#include <syscall.h>
#include <xos/errno.h>
#include <xos/syscall_nums.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h> // IWYU pragma: keep  // sigset_t/SIG_BLOCK etc. used in cancel mask; provided transitively by xos/signal.h but IWYU maps to hosted <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xos/mman.h>
#include <xos/signal.h>
#include <xos/thread.h>

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif

#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#endif
#ifndef SIG_UNBLOCK
#define SIG_UNBLOCK 1
#endif
#ifndef SIG_SETMASK
#define SIG_SETMASK 2
#endif

// ===================== Thread registry =====================
// thread_table[tid] — direct index by tid (MAX_PROC=64). No locks:
// - pthread_create inserts BEFORE clone (parent only).
// - pthread_join reads after join (child already reaped).
// - pthread_detach marks detached.
// tid is reused by kernel after task_reap; entry cleared on join/detach.
#define MAX_PROC 256
enum thread_slot_state { SLOT_FREE, SLOT_CLAIMED, SLOT_LIVE, SLOT_JOINED };

struct thread_entry {
  pid_t tid;
  volatile int32_t *clear_tid_addr;
  int detached;
  void *exit_retval;
  enum thread_slot_state state;
  void *guard_base;
  void *stack_base;
  size_t stack_alloc_size;
};
static struct thread_entry thread_table[MAX_PROC];

static volatile int thread_table_lock = 0;
static inline void thread_table_lock_fn(void) {
  // __atomic_test_and_set returns the old value: old!=0 (already locked) returns true
  // and needs retry; old==0 (unlocked) returns false and acquires the lock. Hence the
  // condition is while(__atomic_test_and_set(...)). Previously miswritten as while(!...),
  // which inverted the logic: when the lock was free it span, and when held it
  // immediately "acquired" (entering the critical section concurrently with the holder).
  // Under single-threading it "worked" by accidentally exiting on the second retry after
  // sched_yield; under multi-threaded stress it manifested as sched_yield never returning
  // (bug.md Bug 2: test_futex_stress's first pthread_create stuck in
  // thread_table_activate).
  while (__atomic_test_and_set(&thread_table_lock, __ATOMIC_ACQUIRE)) {
    sched_yield();
  }
}
static inline void thread_table_unlock_fn(void) {
  __atomic_clear(&thread_table_lock, __ATOMIC_RELEASE);
}

// Claim a free slot and mark it CLAIMED. Called holding thread_table_lock.
static struct thread_entry *thread_table_claim(struct tcb *tcb) {
  thread_table_lock_fn();
  for (int i = 0; i < MAX_PROC; i++) {
    if (thread_table[i].state == SLOT_FREE) {
      thread_table[i].state = SLOT_CLAIMED;
      thread_table[i].detached = 0;
      thread_table[i].exit_retval = NULL;
      thread_table[i].tid = 0;
      thread_table[i].clear_tid_addr = NULL;
      thread_table[i].guard_base = NULL;
      thread_table[i].stack_base = NULL;
      thread_table[i].stack_alloc_size = 0;
      tcb->entry = &thread_table[i];
      thread_table_unlock_fn();
      return &thread_table[i];
    }
  }
  thread_table_unlock_fn();
  return NULL;
}

// Backfill tid after clone and mark LIVE. Called holding thread_table_lock.
static void thread_table_activate(struct thread_entry *e, pid_t tid,
                                  volatile int32_t *clear_tid_addr) {
  thread_table_lock_fn();
  e->tid = tid;
  e->clear_tid_addr = clear_tid_addr;
  e->state = SLOT_LIVE;
  thread_table_unlock_fn();
}

static struct thread_entry *thread_table_find(pid_t tid) {
  // Lock-free read: tid is stable during LIVE, caller holds a reference
  for (int i = 0; i < MAX_PROC; i++) {
    if (thread_table[i].state == SLOT_LIVE && thread_table[i].tid == tid)
      return &thread_table[i];
  }
  return NULL;
}

// ===================== __libc_clone_thread (asm trampoline)
// ===================== Cannot use a pure C wrapper: after the clone child returns
// from the syscall it is still in the parent's stack frame (rbp points at the parent
// stack), and __syscall5's epilogue (leave; ret) would switch rsp back to the parent
// stack, causing the child to execute on the parent stack.
//
// We issue the syscall directly via inline asm; after the child returns 0 the asm:
//   1. Resets rsp to child_stack_top (matching the kernel trapframe)
//   2. Clears rbp (child stack is empty, no frame)
//   3. Reads start_routine / arg from the TCB (%fs:0)
//   4. Calls start_routine, then pthread_exit
// The parent thread returns normally in rax (child tid or negative errno).
extern "C" int64_t __libc_clone_thread(uint64_t flags, uint64_t stack_top,
                                       uint64_t parent_tid, uint64_t child_tid,
                                       uint64_t tls, uint64_t clone_info) {
  int64_t r;
  register uint64_t r10 __asm__("r10") = child_tid;
  register uint64_t r8 __asm__("r8") = tls;
  register uint64_t r9 __asm__("r9") = clone_info;
  // Lock TCB field offsets used by the asm trampoline below.
  static_assert(offsetof(struct tcb, start_routine) == 0x438,
                "tcb.start_routine offset changed — update asm offsets");
  static_assert(offsetof(struct tcb, arg) == 0x440,
                "tcb.arg offset changed — update asm offsets");
  __asm__ volatile(
      "syscall\n"
      "testq %%rax, %%rax\n"
      "jnz 1f\n" // parent: rax != 0, jump to 1:
      // child: rax == 0
      "movq %%rsi, %%rsp\n"        // rsp = stack_top (child stack)
      "xorq %%rbp, %%rbp\n"        // clear rbp (no frame)
      "movq %%fs:0, %%rdi\n"       // rdi = tcb = %fs:0
      "movq 0x438(%%rdi), %%rax\n" // rax = tcb->start_routine
      "movq 0x440(%%rdi), %%rdi\n" // rdi = tcb->arg (also overwrites tcb, becoming
                                   // the first argument to start_routine)
      "callq *%%rax\n"             // start_routine(arg)
      "movq %%rax, %%rdi\n"        // pthread_exit(retval)
      "callq pthread_exit\n"
      "1:\n"
      : "=a"(r)
      : "a"((int64_t)SYS_CLONE), "D"((int64_t)flags), "S"((int64_t)stack_top),
        "d"((int64_t)parent_tid), "r"(r10), "r"(r8), "r"(r9)
      : "rcx", "r11", "memory");
  return r;
}

// ===================== pthread_create =====================
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
  pthread_attr_t default_attr;
  if (!attr) {
    pthread_attr_init(&default_attr);
    attr = &default_attr;
  }

  size_t stacksize = attr->stacksize ? attr->stacksize : PTHREAD_STACK_DEFAULT;
  size_t guardsize = attr->guardsize;
  void *user_stack = attr->stack;

  void *stack_base;
  size_t stack_alloc_size = 0;
  void *guard_base = NULL;
  int stack_allocated = 0;
  if (user_stack) {
    stack_base = user_stack;
  } else {
    if (guardsize > 0) {
      void *guard = mmap(NULL, guardsize, 0 /* PROT_NONE */,
                         MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
      if (guard == MAP_FAILED)
        return ENOMEM;
      stack_base = mmap(NULL, stacksize, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
      if (stack_base == MAP_FAILED) {
        munmap(guard, guardsize);
        return ENOMEM;
      }
      assert(stack_base == (char *)guard + guardsize);
      guard_base = guard;
      stack_alloc_size = guardsize + stacksize;
    } else {
      stack_alloc_size = stacksize;
      stack_base = mmap(NULL, stacksize, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
      if (stack_base == MAP_FAILED)
        return ENOMEM;
    }
    stack_allocated = 1;
  }
  void *stack_top = (char *)stack_base + stacksize;

  // Allocate TLS block + TCB for child (calls exported alloc_tls_block from
  // tls.cc)
  void *tls_page = NULL;
  size_t tls_total = 0;
  struct tcb *new_tcb = alloc_tls_block(&tls_page, &tls_total);
  if (!new_tcb) {
    if (stack_allocated)
      munmap(stack_base, stack_alloc_size);
    return ENOMEM;
  }
  // Override per-thread fields not set by alloc_tls_block (which sets defaults
  // for main thread: detached=0, start_routine=NULL, arg=NULL).
  new_tcb->detached = (attr->detachstate == PTHREAD_CREATE_DETACHED) ? 1 : 0;
  // Pass start info via TCB (pure C, no asm trampoline)
  new_tcb->start_routine = start_routine;
  new_tcb->arg = arg;

  struct thread_entry *slot = thread_table_claim(new_tcb);
  if (!slot) {
    munmap(tls_page, tls_total);
    if (stack_allocated)
      munmap(stack_base, stack_alloc_size);
    return EAGAIN;
  }
  slot->guard_base = guard_base;
  slot->stack_base = stack_base;
  slot->stack_alloc_size = stack_alloc_size;

  struct thread_clone_info clone_info;
  clone_info.detached = new_tcb->detached;
  clone_info.tls_page = (uint64_t)tls_page;
  clone_info.tls_total = tls_total;
  clone_info.user_stack_base = (uint64_t)(guard_base ? guard_base : stack_base);
  clone_info.user_stack_size = stack_alloc_size;

  uint64_t flags =
      0x100 /*CLONE_VM*/ | 0x400 /*CLONE_FILES*/ | 0x800 /*CLONE_SIGHAND*/ |
      0x10000 /*CLONE_THREAD*/ | 0x80000 /*CLONE_SETTLS*/ |
      0x00200000 /*CLONE_CHILD_CLEARTID*/ | 0x00100000 /*CLONE_PARENT_SETTID*/ |
      0x01000000 /*CLONE_CHILD_SETTID*/;
  pid_t parent_tid_buf = 0;
  int64_t r = __libc_clone_thread(
      flags, (uint64_t)stack_top, (uint64_t)&parent_tid_buf,
      (uint64_t)&new_tcb->tid, (uint64_t)new_tcb, (uint64_t)&clone_info);
  if (r < 0) {
    munmap(tls_page, tls_total);
    if (stack_allocated) {
      void *unmap_base = guard_base ? guard_base : stack_base;
      munmap(unmap_base, stack_alloc_size);
    }
    return (int)-r;
  }

  pid_t tid = (pid_t)r;
  // new_tcb->tid was written by the kernel via CLONE_CHILD_SETTID; do not overwrite it
  // here, otherwise if the child exits first (writing 0) the parent could overwrite it
  // with a non-zero value and join would never see 0.
  // Invariant: after clone returns, tid can only be tid (the kernel-written value) or 0
  // (child already exited and cleared via clear_tid_addr). A third value means someone
  // wrote it an extra time (one of the root causes of bug.md Bug 2) or
  // CLONE_CHILD_SETTID wrote the wrong value.
  // Pure check, does not change semantics.
  pid_t observed = __atomic_load_n(&new_tcb->tid, __ATOMIC_ACQUIRE);
  assert(observed == tid || observed == 0);
  thread_table_activate(slot, tid, &new_tcb->tid);
  if (new_tcb->detached) {
    struct thread_entry *e = thread_table_find(tid);
    if (e)
      e->detached = 1;
  }

  *thread = (pthread_t)tid;
  return 0;
}

// ===================== pthread_exit =====================
void pthread_exit(void *retval) {
  struct tcb *tcb = __pthread_current_tcb();

  // Run TSD destructors (up to 4 rounds)
  extern int __pthread_key_used[];
  extern void (*__pthread_key_destructor[])(void *);
  for (int iter = 0; iter < PTHREAD_DESTRUCTOR_ITERATIONS; iter++) {
    int any = 0;
    for (int k = 0; k < PTHREAD_KEYS_MAX; k++) {
      if (tcb->tsd[k] && __pthread_key_used[k] && __pthread_key_destructor[k]) {
        void *val = tcb->tsd[k];
        tcb->tsd[k] = NULL;
        __pthread_key_destructor[k](val);
        any = 1;
      }
    }
    if (!any)
      break;
  }
  // Run cleanup handlers (reverse order)
  while (tcb->cleanup_head) {
    __pthread_cleanup_handler_t *h = tcb->cleanup_head;
    tcb->cleanup_head = h->prev;
    h->routine(h->arg);
    free(h);
  }
  if (tcb->entry) {
    tcb->entry->exit_retval = retval;
    // Detached thread: recycle slot immediately (no joiner will ever claim it).
    // Must do BEFORE sys_exit — after that the kernel unmaps TLS/stack,
    // so tcb and clear_tid_addr become inaccessible (UAF/page fault).
    if (tcb->entry->detached) {
      thread_table_lock_fn();
      tcb->entry->state = SLOT_FREE;
      tcb->entry->tid = 0;
      tcb->entry->clear_tid_addr = NULL;
      tcb->entry->detached = 0;
      thread_table_unlock_fn();
    }
  }

  // tid == tgid → main thread → exit_group
  // tid != tgid → child thread → sys_exit (kernel clears *clear_tid_addr +
  // futex_wake)
  pid_t tid = tcb->tid;
  pid_t tgid = (pid_t)sys_getpid();
  if (tid == tgid) {
    sys_exit_group((int)(intptr_t)retval);
  } else {
    sys_exit((int)(intptr_t)retval);
  }
  __builtin_unreachable();
}

// ===================== pthread_join =====================
int pthread_join(pthread_t thread, void **retval) {
  struct thread_entry *e = thread_table_find((pid_t)thread);
  if (!e || !e->clear_tid_addr)
    return ESRCH;
  if (e->detached)
    return EINVAL;

  volatile int32_t *tid_addr = e->clear_tid_addr;
  while (1) {
    int32_t val = __atomic_load_n((int32_t *)tid_addr, __ATOMIC_ACQUIRE);
    if (val == 0)
      break;
    sys_futex((uint32_t *)tid_addr, FUTEX_WAIT, (uint32_t)val, NULL, NULL, 0);
  }
  if (retval)
    *retval = e->exit_retval;
  e->state = SLOT_JOINED;
  if (!e->detached && e->stack_alloc_size > 0) {
    void *unmap_base = e->guard_base ? e->guard_base : e->stack_base;
    munmap(unmap_base, e->stack_alloc_size);
  }
  e->tid = 0;
  e->clear_tid_addr = NULL;
  e->detached = 0;
  return 0;
}

// ===================== pthread_detach =====================
int pthread_detach(pthread_t thread) {
  struct thread_entry *e = thread_table_find((pid_t)thread);
  if (!e)
    return ESRCH;
  e->detached = 1;
  return 0;
}

// ===================== pthread_self / equal =====================
pthread_t pthread_self(void) { return (pthread_t)__pthread_current_tcb()->tid; }

int pthread_equal(pthread_t t1, pthread_t t2) { return t1 == t2; }

// ===================== Attributes =====================
int pthread_attr_init(pthread_attr_t *attr) {
  memset(attr, 0, sizeof(*attr));
  attr->detachstate = PTHREAD_CREATE_JOINABLE;
  attr->stacksize = PTHREAD_STACK_DEFAULT;
  attr->guardsize = 4096;
  return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
  (void)attr;
  return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate) {
  *detachstate = attr->detachstate;
  return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
  if (detachstate != PTHREAD_CREATE_JOINABLE &&
      detachstate != PTHREAD_CREATE_DETACHED)
    return EINVAL;
  attr->detachstate = detachstate;
  return 0;
}

int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr,
                          size_t *stacksize) {
  *stackaddr = attr->stack;
  *stacksize = attr->stacksize;
  return 0;
}

int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr,
                          size_t stacksize) {
  if (stacksize < PTHREAD_STACK_MIN)
    return EINVAL;
  attr->stack = stackaddr;
  attr->stacksize = stacksize;
  attr->guardsize = 0;
  return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize) {
  *stacksize = attr->stacksize;
  return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize) {
  if (stacksize < PTHREAD_STACK_MIN)
    return EINVAL;
  attr->stacksize = stacksize;
  return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *guardsize) {
  *guardsize = attr->guardsize;
  return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize) {
  attr->guardsize = guardsize;
  return 0;
}

// ===================== Mutex =====================
int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attr) {
  mutex->state = 0;
  mutex->type = attr ? attr->type : PTHREAD_MUTEX_NORMAL;
  mutex->owner = 0;
  mutex->count = 0;
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
  (void)mutex;
  return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
  pid_t tid = (pid_t)sys_gettid();
  if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->owner == tid) {
    mutex->count++;
    return 0;
  }
  if (mutex->type == PTHREAD_MUTEX_ERRORCHECK && mutex->owner == tid) {
    return EDEADLK;
  }
  uint32_t expected = 0;
  while (!__atomic_compare_exchange_n(&mutex->state, &expected, 1, 0,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    if (expected == 1) {
      // Try to mark contention: state 1→2. If it returns 0, the holder unlocked during
      // this window (state was reset to 0); we cannot futex_wait (val=2≠0 would EAGAIN
      // with no waker), nor leave state=2 (later CAS would fail forever → infinite
      // spin). Restore state=0 and skip futex_wait to retry the fast-path (avoiding a
      // syscall guaranteed to EAGAIN).
      uint32_t prev = __atomic_exchange_n(&mutex->state, 2, __ATOMIC_ACQUIRE);
      if (prev == 0) {
        __atomic_store_n(&mutex->state, 0, __ATOMIC_RELEASE);
        expected = 0;
        continue;
      } else if (prev != 1) {
        // prev==2: someone else already marked contention, just wait
      }
      sys_futex(&mutex->state, FUTEX_WAIT, 2, NULL, NULL, 0);
    }
    expected = 0;
  }
  mutex->owner = tid;
  mutex->count = 1;
  return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  pid_t tid = (pid_t)sys_gettid();
  if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->owner == tid) {
    mutex->count++;
    return 0;
  }
  uint32_t expected = 0;
  if (__atomic_compare_exchange_n(&mutex->state, &expected, 1, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    mutex->owner = tid;
    mutex->count = 1;
    return 0;
  }
  return EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  pid_t tid = (pid_t)sys_gettid();
  if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->owner == tid &&
      mutex->count > 1) {
    mutex->count--;
    return 0;
  }
  if (mutex->owner != tid)
    return EPERM;
  mutex->owner = 0;
  mutex->count = 0;
  uint32_t prev = __atomic_exchange_n(&mutex->state, 0, __ATOMIC_RELEASE);
  if (prev == 2) {
    sys_futex(&mutex->state, FUTEX_WAKE, 1, NULL, NULL, 0);
  }
  return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex,
                            const struct timespec *abstime) {
  pid_t tid = (pid_t)sys_gettid();
  if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->owner == tid) {
    mutex->count++;
    return 0;
  }
  if (mutex->type == PTHREAD_MUTEX_ERRORCHECK && mutex->owner == tid) {
    return EDEADLK;
  }
  uint32_t expected = 0;
  while (!__atomic_compare_exchange_n(&mutex->state, &expected, 1, 0,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    if (expected == 1) {
      uint32_t prev = __atomic_exchange_n(&mutex->state, 2, __ATOMIC_ACQUIRE);
      if (prev == 0) {
        __atomic_store_n(&mutex->state, 0, __ATOMIC_RELEASE);
        expected = 0;
        continue;
      } else if (prev != 1) {
        // prev==2: someone else already marked contention
      }
      int64_t r = sys_futex((uint32_t *)&mutex->state, FUTEX_WAIT, 2,
                            (const void *)abstime, NULL, 0);
      // sys_futex wrapper: negative kernel return → errno = -r, return -1
      if (r < 0 && errno == ETIMEDOUT) {
        // Kernel already removed the waiter; userspace CAS 2→0 + wake
        uint32_t exp = 2;
        if (__atomic_compare_exchange_n(&mutex->state, &exp, 0, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
          sys_futex((uint32_t *)&mutex->state, FUTEX_WAKE, 1, NULL, NULL, 0);
        }
        return ETIMEDOUT;
      }
      // r == 0 (wake) or r == -EINTR (signal): retry, do not return EINTR
    }
    expected = 0;
  }
  mutex->owner = tid;
  mutex->count = 1;
  return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
  attr->type = PTHREAD_MUTEX_NORMAL;
  return 0;
}
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
  (void)attr;
  return 0;
}
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
  *type = attr->type;
  return 0;
}
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
  if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_ERRORCHECK &&
      type != PTHREAD_MUTEX_RECURSIVE)
    return EINVAL;
  attr->type = type;
  return 0;
}

// ===================== Condition variable =====================
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
  (void)attr;
  cond->seq = 0;
  cond->waiters = 0;
  return 0;
}
int pthread_cond_destroy(pthread_cond_t *cond) {
  (void)cond;
  return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  uint32_t cur_seq = __atomic_load_n(&cond->seq, __ATOMIC_ACQUIRE);
  __atomic_add_fetch(&cond->waiters, 1, __ATOMIC_ACQUIRE);
  pthread_mutex_unlock(mutex);
  sys_futex(&cond->seq, FUTEX_WAIT, cur_seq, NULL, NULL, 0);
  __atomic_sub_fetch(&cond->waiters, 1, __ATOMIC_ACQUIRE);
  pthread_mutex_lock(mutex);
  return 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
  __atomic_add_fetch(&cond->seq, 1, __ATOMIC_RELEASE);
  if (__atomic_load_n(&cond->waiters, __ATOMIC_ACQUIRE) > 0) {
    sys_futex(&cond->seq, FUTEX_WAKE, 1, NULL, NULL, 0);
  }
  return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
  __atomic_add_fetch(&cond->seq, 1, __ATOMIC_RELEASE);
  if (__atomic_load_n(&cond->waiters, __ATOMIC_ACQUIRE) > 0) {
    sys_futex(&cond->seq, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
  }
  return 0;
}

// ===================== Read-write lock =====================
int pthread_rwlock_init(pthread_rwlock_t *rwlock,
                        const pthread_rwlockattr_t *attr) {
  (void)attr;
  rwlock->readers = 0;
  rwlock->writer = 0;
  rwlock->wwaiters = 0;
  rwlock->rwaiters = 0;
  return 0;
}
int pthread_rwlock_destroy(pthread_rwlock_t *rwlock) {
  (void)rwlock;
  return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock) {
  while (1) {
    while (__atomic_load_n(&rwlock->writer, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&rwlock->wwaiters, __ATOMIC_ACQUIRE)) {
      __atomic_add_fetch(&rwlock->rwaiters, 1, __ATOMIC_ACQUIRE);
      sys_futex(&rwlock->writer, FUTEX_WAIT, 1, NULL, NULL, 0);
      __atomic_sub_fetch(&rwlock->rwaiters, 1, __ATOMIC_ACQUIRE);
    }
    if (__atomic_add_fetch(&rwlock->readers, 1, __ATOMIC_ACQUIRE) == 1 &&
        __atomic_load_n(&rwlock->writer, __ATOMIC_ACQUIRE)) {
      __atomic_sub_fetch(&rwlock->readers, 1, __ATOMIC_RELEASE);
      continue;
    }
    return 0;
  }
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock) {
  __atomic_add_fetch(&rwlock->wwaiters, 1, __ATOMIC_ACQUIRE);
  while (1) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&rwlock->writer, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      while (__atomic_load_n(&rwlock->readers, __ATOMIC_ACQUIRE) > 0) {
        sys_futex(&rwlock->readers, FUTEX_WAIT,
                  __atomic_load_n(&rwlock->readers, __ATOMIC_ACQUIRE), NULL,
                  NULL, 0);
      }
      __atomic_sub_fetch(&rwlock->wwaiters, 1, __ATOMIC_RELEASE);
      return 0;
    }
    sys_futex(&rwlock->writer, FUTEX_WAIT, 1, NULL, NULL, 0);
  }
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock) {
  if (__atomic_load_n(&rwlock->writer, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&rwlock->writer, 0, __ATOMIC_RELEASE);
    sys_futex(&rwlock->writer, FUTEX_WAKE, 1, NULL, NULL, 0);
    if (__atomic_load_n(&rwlock->rwaiters, __ATOMIC_ACQUIRE) > 0) {
      sys_futex(&rwlock->readers, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
    }
  } else {
    __atomic_sub_fetch(&rwlock->readers, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&rwlock->readers, __ATOMIC_ACQUIRE) == 0 &&
        __atomic_load_n(&rwlock->wwaiters, __ATOMIC_ACQUIRE) > 0) {
      sys_futex(&rwlock->readers, FUTEX_WAKE, 1, NULL, NULL, 0);
    }
  }
  return 0;
}

// ===================== Barrier =====================
int pthread_barrier_init(pthread_barrier_t *barrier,
                         const pthread_barrierattr_t *attr, unsigned count) {
  (void)attr;
  barrier->count = count;
  barrier->waiting = 0;
  barrier->generation = 0;
  return 0;
}
int pthread_barrier_destroy(pthread_barrier_t *barrier) {
  (void)barrier;
  return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier) {
  uint32_t gen = __atomic_load_n(&barrier->generation, __ATOMIC_ACQUIRE);
  uint32_t n = __atomic_add_fetch(&barrier->waiting, 1, __ATOMIC_ACQUIRE);
  if (n >= barrier->count) {
    __atomic_store_n(&barrier->waiting, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&barrier->generation, 1, __ATOMIC_RELEASE);
    sys_futex(&barrier->generation, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
    return PTHREAD_BARRIER_SERIAL_THREAD;
  }
  while (__atomic_load_n(&barrier->generation, __ATOMIC_ACQUIRE) == gen) {
    sys_futex(&barrier->generation, FUTEX_WAIT, gen, NULL, NULL, 0);
  }
  return 0;
}

// ===================== Once =====================
int pthread_once(pthread_once_t *once, void (*init)(void)) {
  if (__atomic_load_n(&once->done, __ATOMIC_ACQUIRE) == 0) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&once->done, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      init();
      __atomic_store_n(&once->done, 2, __ATOMIC_RELEASE);
    } else {
      while (__atomic_load_n(&once->done, __ATOMIC_ACQUIRE) != 2) {
        sched_yield();
      }
    }
  }
  return 0;
}

// ===================== Cancel =====================
int pthread_cancel(pthread_t thread) {
  pid_t tgid = (pid_t)sys_getpid();
  return sys_tgkill(tgid, (pid_t)thread, SIGCANCEL);
}

int pthread_setcancelstate(int state, int *oldstate) {
  struct tcb *tcb = __pthread_current_tcb();
  if (oldstate)
    *oldstate = tcb->cancel_state;
  if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
    return EINVAL;
  tcb->cancel_state = state;
  // Map to kernel sig_blocked: DISABLE → block SIGCANCEL, ENABLE → unblock
  sigset_t block_set = (1ULL << SIGCANCEL);
  if (state == PTHREAD_CANCEL_DISABLE) {
    sys_sigprocmask(SIG_BLOCK, &block_set, NULL);
  } else {
    sys_sigprocmask(SIG_UNBLOCK, &block_set, NULL);
  }
  return 0;
}

int pthread_setcanceltype(int type, int *oldtype) {
  struct tcb *tcb = __pthread_current_tcb();
  if (oldtype)
    *oldtype = tcb->cancel_type;
  if (type != PTHREAD_CANCEL_DEFERRED && type != PTHREAD_CANCEL_ASYNCHRONOUS)
    return EINVAL;
  tcb->cancel_type =
      type; // ASYNCHRONOUS accepted but treated as DEFERRED (todo)
  return 0;
}

void pthread_testcancel(void) {
  // SIGCANCEL delivery at cancellation points is handled by kernel signal path.
  // This is a cancellation point per POSIX; pending SIGCANCEL delivered on
  // syscall return.
  (void)0;
}

// ===================== Cleanup =====================
void pthread_cleanup_push(void (*routine)(void *), void *arg) {
  struct tcb *tcb = __pthread_current_tcb();
  __pthread_cleanup_handler_t *h = (__pthread_cleanup_handler_t *)malloc(
      sizeof(__pthread_cleanup_handler_t));
  if (!h)
    return;
  h->routine = routine;
  h->arg = arg;
  h->prev = tcb->cleanup_head;
  tcb->cleanup_head = h;
}

void pthread_cleanup_pop(int execute) {
  struct tcb *tcb = __pthread_current_tcb();
  __pthread_cleanup_handler_t *h = tcb->cleanup_head;
  if (!h)
    return;
  tcb->cleanup_head = h->prev;
  if (execute)
    h->routine(h->arg);
  free(h);
}

// ===================== TSD =====================
int __pthread_key_used[PTHREAD_KEYS_MAX];
void (*__pthread_key_destructor[PTHREAD_KEYS_MAX])(void *);

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
  for (int k = 0; k < PTHREAD_KEYS_MAX; k++) {
    if (!__pthread_key_used[k]) {
      __pthread_key_used[k] = 1;
      __pthread_key_destructor[k] = destructor;
      *key = (pthread_key_t)k;
      return 0;
    }
  }
  return EAGAIN;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
  if (key < 0 || key >= PTHREAD_KEYS_MAX)
    return EINVAL;
  __pthread_current_tcb()->tsd[key] = (void *)value;
  return 0;
}

void *pthread_getspecific(pthread_key_t key) {
  if (key < 0 || key >= PTHREAD_KEYS_MAX)
    return NULL;
  return __pthread_current_tcb()->tsd[key];
}

// ===================== Signal =====================
int pthread_kill(pthread_t thread, int sig) {
  pid_t tgid = (pid_t)sys_getpid();
  return sys_tgkill(tgid, (pid_t)thread, sig);
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  return sys_sigprocmask(how, set, oldset);
}

// ===================== Name =====================
int pthread_setname_np(pthread_t thread, const char *name) {
  // No /proc/self/comm; no-op (todo)
  (void)thread;
  (void)name;
  return 0;
}
