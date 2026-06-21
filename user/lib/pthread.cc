#include "pthread.h"
#include "stdlib.h"
#include "string.h"
#include "common/syscall.h"
#include "common/errno.h"
#include "arch/x64/memlayout.h"
#include "sys/mman.h"
#include <limits.h>

// ===================== TLS / TCB =====================
// TCB at TLS page start, %fs:0 points to TCB (via FS_BASE = TLS page addr)

struct tcb {
    void  *self;     // points to itself (%fs:0 returns this)
    pid_t  tid;      // kernel thread ID (used by CLONE_CHILD_CLEARTID)
    pid_t  pid;      // process ID (tgid)
    // pad to 64 bytes for future expansion
    char   __pad[64 - sizeof(void*) - sizeof(pid_t) - sizeof(pid_t)];
};

static_assert(sizeof(tcb) == 64, "tcb must be 64 bytes");

// Per-thread startup info passed via clone stack
struct thread_start {
    void *(*fn)(void *);
    void  *arg;
};

// ===================== Thread registry for join =====================
// Maps tid → clear_tid_addr (tcb.tid pointer) so pthread_join can find it.

#define MAX_THREAD_REGISTRY 64

static struct {
    pid_t    tid;
    int32_t *clear_tid_addr;
} thread_registry[MAX_THREAD_REGISTRY];

static int thread_registry_lock = 0;

static void __pthread_register_tid(pid_t tid, int32_t *addr) {
    while (__atomic_test_and_set(&thread_registry_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    for (int i = 0; i < MAX_THREAD_REGISTRY; i++) {
        if (thread_registry[i].tid == 0) {
            thread_registry[i].tid = tid;
            thread_registry[i].clear_tid_addr = addr;
            break;
        }
    }
    __atomic_clear(&thread_registry_lock, __ATOMIC_RELEASE);
}

static int32_t *__pthread_get_clear_tid_addr(pid_t tid) {
    while (__atomic_test_and_set(&thread_registry_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    int32_t *addr = nullptr;
    for (int i = 0; i < MAX_THREAD_REGISTRY; i++) {
        if (thread_registry[i].tid == tid) {
            addr = thread_registry[i].clear_tid_addr;
            break;
        }
    }
    __atomic_clear(&thread_registry_lock, __ATOMIC_RELEASE);
    return addr;
}

// ===================== Internal: allocate TLS page for main thread =====================

static void __pthread_init_main_tls() {
    // Allocate 1 page for TLS
    void *tls_page = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                          0, -1, 0);
    if (!tls_page) return;

    // Zero and set up TCB
    tcb *t = (tcb *)tls_page;
    memset(t, 0, sizeof(tcb));
    t->self = t;
    t->tid = sys_gettid();
    t->pid = sys_getpid();

    // Set FS_BASE to TLS page
    sys_arch_prctl(ARCH_SET_FS, (uint64_t)t);

    // Set clear_tid_address for join support
    sys_set_tid_address(&t->tid);
}

// Called from _start before main()
extern "C" void __libc_tls_init() {
    __pthread_init_main_tls();
}

// ===================== Thread entry (called from inline asm) =====================

extern "C" void __pthread_start(struct thread_start *info) {
    void *(*fn)(void *) = info->fn;
    void *arg = info->arg;
    free(info);

    void *result = fn(arg);
    pthread_exit(result);
}

// ===================== Thread functions =====================

// clone child path: when clone returns 0 in the child, we're on the new stack
// with no valid frame. The inline asm below handles this by directly
// jumping to __pthread_start after detecting rax==0.
//
// The new stack has [info_ptr] at [RSP] so the child can pop it.
// After popq %rdi: RSP is 16-aligned, ready for the call instruction.

int pthread_create(pthread_t *thread, const void *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)attr;

    // Allocate stack for new thread (64KB)
    const size_t stack_size = 64 * 1024;
    void *stack = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE,
                       0, -1, 0);
    if (!stack) return -ENOMEM;

    // Allocate TLS page for new thread
    void *tls_page = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                          0, -1, 0);
    if (!tls_page) {
        munmap(stack, stack_size);
        return -ENOMEM;
    }

    // Set up TCB in new TLS page
    tcb *new_tcb = (tcb *)tls_page;
    memset(new_tcb, 0, sizeof(tcb));
    new_tcb->self = new_tcb;
    new_tcb->tid = 0;    // cleared by proc_reap on exit → join futex
    new_tcb->pid = sys_getpid();

    // Allocate thread_start info
    thread_start *info = (thread_start *)malloc(sizeof(thread_start));
    if (!info) {
        munmap(tls_page, PAGE_SIZE);
        munmap(stack, stack_size);
        return -ENOMEM;
    }
    info->fn = start_routine;
    info->arg = arg;

    // Set up new thread stack: place info pointer at the top so the child
    // can pop it. The child's RSP will be set to child_stack by clone.
    // Layout: [info_ptr] ← child RSP points here
    //         [padding]  ← for 16-byte alignment before call
    uint64_t stack_top = (uint64_t)stack + stack_size;
    stack_top &= ~0xFULL;  // 16-byte align

    // After child does "popq %rdi" (8 bytes), RSP advances 8 bytes.
    // Then "call" pushes 8 bytes (ret addr), making RSP = stack_top - 16.
    // At __pthread_start entry, RSP must be 16-aligned minus 8 (call convention).
    // So we need: stack_top - 8 (after pop) to be 16-aligned for the call.
    // stack_top is 16-aligned, so stack_top - 8 is NOT 16-aligned.
    // Fix: use stack_top - 8 as the child RSP, so after popq %rdi → stack_top
    // (16-aligned), call → stack_top - 8 (proper alignment).
    uint64_t child_rsp = stack_top - 8;
    ((uint64_t *)(stack_top - 8))[0] = (uint64_t)info;

    int32_t parent_tid = 0;
    uint64_t flags = CLONE_VM | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD
                   | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_SETTLS;

    // Raw syscall + child detection in inline asm.
    // In the child (rax==0), we're on the new stack. We must NOT access
    // any parent stack variables. Instead, pop info from new stack and
    // call __pthread_start directly.
    int64_t tid;
    register uint64_t r10 __asm__("r10") = (uint64_t)(uintptr_t)&new_tcb->tid;
    register uint64_t r8  __asm__("r8")  = (uint64_t)tls_page;

    __asm__ volatile(
        "syscall\n"
        "testq %%rax, %%rax\n"
        "jnz 1f\n"
        // Child path (rax == 0)
        "xorq %%rbp, %%rbp\n"    // clear frame pointer
        "popq %%rdi\n"           // rdi = info (from new stack)
        "call __pthread_start\n" // enter thread
        "movq $55, %%rax\n"      // SYS_EXIT_GROUP (shouldn't return)
        "xorq %%rdi, %%rdi\n"
        "syscall\n"
        "1:\n"
        : "=a"(tid)
        : "0"((uint64_t)SYS_CLONE),
          "D"(flags),
          "S"(child_rsp),
          "d"((uint64_t)(uintptr_t)&parent_tid),
          "r"(r10),
          "r"(r8)
        : "r9", "rcx", "r11", "memory"
    );

    if (tid < 0) {
        free(info);
        munmap(tls_page, PAGE_SIZE);
        munmap(stack, stack_size);
        return (int)-tid;
    }

    // Register child's clear_tid_addr for pthread_join
    __pthread_register_tid((pid_t)tid, &new_tcb->tid);

    if (thread) {
        *thread = (pthread_t)tid;
    }

    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    int32_t *tid_addr = __pthread_get_clear_tid_addr((pid_t)thread);
    if (!tid_addr) return -ESRCH;

    // Wait on futex while tcb->tid != 0
    // When the thread exits, proc_reap clears tcb->tid to 0 and does futex_wake
    while (1) {
        int32_t val = __atomic_load_n(tid_addr, __ATOMIC_ACQUIRE);
        if (val == 0) break;  // thread has exited
        sys_futex((uint32_t *)tid_addr, FUTEX_WAIT, (uint32_t)val,
                  nullptr, nullptr, 0);
    }

    if (retval) *retval = nullptr;
    return 0;
}

void pthread_exit(void *retval) {
    (void)retval;
    sys_exit((int)(intptr_t)retval);
    __builtin_unreachable();
}

pthread_t pthread_self(void) {
    return (pthread_t)sys_gettid();
}

// ===================== Mutex functions =====================

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    mutex->__lock = 0;
    mutex->__type = attr ? attr->__type : PTHREAD_MUTEX_NORMAL;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    while (1) {
        if (__atomic_exchange_n(&mutex->__lock, 1, __ATOMIC_ACQUIRE) == 0)
            return 0;
        sys_futex((uint32_t *)&mutex->__lock, FUTEX_WAIT, 1,
                  nullptr, nullptr, 0);
    }
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    __atomic_store_n(&mutex->__lock, 0, __ATOMIC_RELEASE);
    sys_futex((uint32_t *)&mutex->__lock, FUTEX_WAKE, 1,
              nullptr, nullptr, 0);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    (void)mutex;
    return 0;
}

// ===================== Condition variable functions =====================

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    (void)attr;
    cond->__seq = 0;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    __atomic_add_fetch(&cond->__seq, 1, __ATOMIC_RELEASE);
    sys_futex((uint32_t *)&cond->__seq, FUTEX_WAKE, 1,
              nullptr, nullptr, 0);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    __atomic_add_fetch(&cond->__seq, 1, __ATOMIC_RELEASE);
    sys_futex((uint32_t *)&cond->__seq, FUTEX_WAKE, INT_MAX,
              nullptr, nullptr, 0);
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    unsigned int old_seq = __atomic_load_n(&cond->__seq, __ATOMIC_ACQUIRE);

    pthread_mutex_unlock(mutex);

    sys_futex((uint32_t *)&cond->__seq, FUTEX_WAIT, old_seq,
              nullptr, nullptr, 0);

    pthread_mutex_lock(mutex);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}
